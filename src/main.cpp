// FileTraceSKSE - Asynchronous file open tracer for Skyrim SKSE.

#include "PCH.h"

#include <MinHook.h>
#define SI_CONVERT_WIN32
#include <SimpleIni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace
{
    constexpr std::wstring_view kMainIniSection = L"FileTraceSKSE";
    constexpr std::wstring_view kFallbackIniSection = L"General";
    constexpr std::wstring_view kDefaultExtensions =
        L".nif|.dds|.hkx|.hkb|.tri|.esp|.esm|.esl|.bsa|.pex|.psc|.wav|.xwm|.lip";

    using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

    HMODULE g_moduleHandle = nullptr;
    CreateFileW_t g_originalCreateFileW = ::CreateFileW;
    CreateFileA_t g_originalCreateFileA = ::CreateFileA;
    LPVOID g_createFileWTarget = nullptr;
    LPVOID g_createFileATarget = nullptr;

    std::atomic<bool> g_runtimeEnabled{ false };
    std::atomic<bool> g_hooksEnabled{ false };
    std::atomic<bool> g_hookEngineInitialized{ false };
    std::atomic<std::uint32_t> g_activeHookCalls{ 0 };
    std::mutex g_lifecycleMutex;
    thread_local bool g_inHook = false;

    struct ConfigSnapshot
    {
        bool enabled = true;
        std::filesystem::path dataRoot;
        std::vector<std::filesystem::path> extraRoots;
        bool segmentFallbackEnabled = true;
        bool includeAllExtensions = false;
        std::unordered_set<std::wstring> includeExtensions;
        std::size_t queueCapacity = 8192;
        std::uint32_t flushIntervalMs = 100;
        std::uint32_t minDurationMs = 1;
        bool logFailedOpen = true;
        std::filesystem::path logDir;
        std::filesystem::path iniPath;
    };

    struct RawEvent
    {
        std::chrono::system_clock::time_point timestamp;
        std::uint32_t threadId = 0;
        bool success = false;
        std::uint32_t durationMs = 0;
        DWORD desiredAccess = 0;
        DWORD shareMode = 0;
        DWORD creationDisposition = 0;
        DWORD lastError = 0;
        std::wstring path;
    };

    ConfigSnapshot g_config{};

    std::wstring Trim(std::wstring_view value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::iswspace(value[begin]) != 0) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::iswspace(value[end - 1]) != 0) {
            --end;
        }
        return std::wstring(value.substr(begin, end - begin));
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool StartsWith(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    std::vector<std::wstring> Split(std::wstring_view value, wchar_t delimiter)
    {
        std::vector<std::wstring> out;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t pos = value.find(delimiter, start);
            const std::size_t end = pos == std::wstring_view::npos ? value.size() : pos;
            out.emplace_back(value.substr(start, end - start));
            if (pos == std::wstring_view::npos) {
                break;
            }
            start = pos + 1;
        }
        return out;
    }

    std::string WideToUtf8(std::wstring_view value)
    {
        if (value.empty()) {
            return {};
        }
        const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return {};
        }
        std::string out(static_cast<std::size_t>(required), '\0');
        const int converted = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), required, nullptr, nullptr);
        if (converted <= 0) {
            return {};
        }
        return out;
    }

    std::wstring AnsiToWide(std::string_view value)
    {
        if (value.empty()) {
            return {};
        }
        const int required = ::MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0) {
            return {};
        }
        std::wstring out(static_cast<std::size_t>(required), L'\0');
        const int converted = ::MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), out.data(), required);
        if (converted <= 0) {
            return {};
        }
        return out;
    }

    std::wstring NormalizePath(const std::wstring& input)
    {
        if (input.empty()) {
            return {};
        }

        std::wstring path = input;
        std::replace(path.begin(), path.end(), L'/', L'\\');

        if (!StartsWith(path, L"\\\\?\\")) {
            const DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
            if (needed > 0) {
                std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
                const DWORD copied = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
                if (copied > 0 && copied < buffer.size()) {
                    buffer.resize(copied);
                    path = std::move(buffer);
                }
            }
        }

        if (StartsWith(path, L"\\\\?\\UNC\\")) {
            path = L"\\" + path.substr(7);
        } else if (StartsWith(path, L"\\\\?\\")) {
            path = path.substr(4);
        }
        return path;
    }

    bool IsAutoKeyword(std::wstring_view value)
    {
        return ToLower(Trim(value)) == L"auto";
    }

    bool ParseBool(std::wstring_view raw, bool defaultValue)
    {
        const std::wstring value = ToLower(Trim(raw));
        if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") {
            return true;
        }
        if (value == L"0" || value == L"false" || value == L"no" || value == L"off") {
            return false;
        }
        return defaultValue;
    }

    std::optional<std::int64_t> ParseInt64(std::wstring_view raw)
    {
        const std::wstring value = Trim(raw);
        if (value.empty()) {
            return std::nullopt;
        }
        try {
            std::size_t consumed = 0;
            const auto parsed = std::stoll(value, &consumed, 10);
            if (consumed != value.size()) {
                return std::nullopt;
            }
            return parsed;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::unordered_set<std::wstring> ParseExtensions(std::wstring_view raw)
    {
        std::unordered_set<std::wstring> out;
        for (auto ext : Split(raw, L'|')) {
            ext = Trim(ext);
            if (ext.empty()) {
                continue;
            }
            if (ext.front() != L'.') {
                ext.insert(ext.begin(), L'.');
            }
            out.insert(ToLower(std::move(ext)));
        }
        return out;
    }

    const wchar_t* GetIniValue(const CSimpleIniW& ini, const wchar_t* key)
    {
        if (const auto* value = ini.GetValue(kMainIniSection.data(), key, nullptr)) {
            return value;
        }
        if (const auto* value = ini.GetValue(kFallbackIniSection.data(), key, nullptr)) {
            return value;
        }
        return ini.GetValue(nullptr, key, nullptr);
    }

    std::filesystem::path GetModulePath()
    {
        if (g_moduleHandle == nullptr) {
            return {};
        }
        std::wstring buffer(32768, L'\0');
        const DWORD copied = ::GetModuleFileNameW(g_moduleHandle, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0 || copied >= buffer.size()) {
            return {};
        }
        buffer.resize(copied);
        return std::filesystem::path(buffer);
    }

    std::filesystem::path GetDefaultUserSkseLogDir(const std::filesystem::path& dataDirFallback)
    {
        wchar_t* userProfile = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&userProfile, &length, L"USERPROFILE") == 0 &&
            userProfile != nullptr &&
            userProfile[0] != L'\0') {
            const auto resolvedPath =
                std::filesystem::path(userProfile) /
                L"Documents" /
                L"My Games" /
                L"Skyrim Special Edition" /
                L"SKSE";
            std::free(userProfile);
            return resolvedPath;
        }
        std::free(userProfile);

        return dataDirFallback / L"SKSE" / L"Plugins";
    }

    ConfigSnapshot LoadConfig()
    {
        ConfigSnapshot cfg{};
        cfg.includeExtensions = ParseExtensions(kDefaultExtensions);

        const auto modulePath = GetModulePath();
        const auto pluginDir = modulePath.empty() ? std::filesystem::current_path() : modulePath.parent_path();
        auto dataDir = pluginDir;
        if (!pluginDir.empty()) {
            const auto skseDir = pluginDir.parent_path();
            if (!skseDir.empty() && !skseDir.parent_path().empty()) {
                dataDir = skseDir.parent_path();
            }
        }

        cfg.dataRoot = dataDir;
        cfg.logDir = GetDefaultUserSkseLogDir(dataDir);
        cfg.iniPath = pluginDir / L"FileTraceSKSE.ini";

        CSimpleIniW ini;
        ini.SetUnicode(true);
        if (ini.LoadFile(cfg.iniPath.c_str()) < 0) {
            cfg.dataRoot = NormalizePath(cfg.dataRoot.wstring());
            cfg.logDir = NormalizePath(cfg.logDir.wstring());
            return cfg;
        }

        if (const auto* v = GetIniValue(ini, L"Enabled")) {
            cfg.enabled = ParseBool(v, cfg.enabled);
        }
        if (const auto* v = GetIniValue(ini, L"SegmentFallbackEnabled")) {
            cfg.segmentFallbackEnabled = ParseBool(v, cfg.segmentFallbackEnabled);
        }
        if (const auto* v = GetIniValue(ini, L"LogFailedOpen")) {
            cfg.logFailedOpen = ParseBool(v, cfg.logFailedOpen);
        }
        if (const auto* v = GetIniValue(ini, L"IncludeAllExtensions")) {
            cfg.includeAllExtensions = ParseBool(v, cfg.includeAllExtensions);
        }
        if (const auto* v = GetIniValue(ini, L"DataRoot")) {
            if (!IsAutoKeyword(v)) {
                cfg.dataRoot = Trim(v);
            }
        }
        if (const auto* v = GetIniValue(ini, L"LogDir")) {
            if (!IsAutoKeyword(v)) {
                cfg.logDir = Trim(v);
            }
        }
        if (const auto* v = GetIniValue(ini, L"ExtraRoots")) {
            cfg.extraRoots.clear();
            for (auto part : Split(v, L';')) {
                part = Trim(part);
                if (!part.empty()) {
                    cfg.extraRoots.emplace_back(part);
                }
            }
        }
        if (const auto* v = GetIniValue(ini, L"IncludeExtensions")) {
            const auto parsed = ParseExtensions(v);
            if (!parsed.empty()) {
                cfg.includeExtensions = parsed;
            }
        }
        if (const auto* v = GetIniValue(ini, L"QueueCapacity")) {
            if (const auto parsed = ParseInt64(v)) {
                cfg.queueCapacity = static_cast<std::size_t>(std::clamp<std::int64_t>(*parsed, 64, 262144));
            }
        }
        if (const auto* v = GetIniValue(ini, L"FlushIntervalMs")) {
            if (const auto parsed = ParseInt64(v)) {
                cfg.flushIntervalMs = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*parsed, 10, 5000));
            }
        }
        if (const auto* v = GetIniValue(ini, L"MinDurationMs")) {
            if (const auto parsed = ParseInt64(v)) {
                cfg.minDurationMs = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*parsed, 0, 30000));
            }
        }

        cfg.dataRoot = NormalizePath(cfg.dataRoot.wstring());
        cfg.logDir = NormalizePath(cfg.logDir.wstring());
        for (auto& root : cfg.extraRoots) {
            root = NormalizePath(root.wstring());
        }

        return cfg;
    }

    std::string FormatTimestamp(const std::chrono::system_clock::time_point& timestamp)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch());
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(ms);
        std::time_t wallTime = sec.count();
        std::tm localTime{};
        localtime_s(&localTime, &wallTime);

        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
               << '.'
               << std::setw(3)
               << std::setfill('0')
               << static_cast<int>(ms.count() % 1000);
        return stream.str();
    }

    std::string AccessFlagsToText(DWORD desiredAccess)
    {
        const bool read = (desiredAccess & (GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES)) != 0;
        const bool write = (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
        const bool del = (desiredAccess & DELETE) != 0;
        if (read && write && del) {
            return "RW+DEL";
        }
        if (read && write) {
            return "RW";
        }
        if (read && del) {
            return "R+DEL";
        }
        if (write && del) {
            return "W+DEL";
        }
        if (read) {
            return "R";
        }
        if (write) {
            return "W";
        }
        if (del) {
            return "DEL";
        }
        return "-";
    }

    std::string CreationDispositionToText(DWORD creationDisposition)
    {
        switch (creationDisposition) {
            case CREATE_NEW: return "CREATE_NEW";
            case CREATE_ALWAYS: return "CREATE_ALWAYS";
            case OPEN_EXISTING: return "OPEN_EXISTING";
            case OPEN_ALWAYS: return "OPEN_ALWAYS";
            case TRUNCATE_EXISTING: return "TRUNCATE_EXISTING";
            default: return "UNKNOWN";
        }
    }

    std::wstring GetLowerExtension(std::wstring_view normalizedPath)
    {
        const auto slash = normalizedPath.find_last_of(L'\\');
        const auto dot = normalizedPath.find_last_of(L'.');
        if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) {
            return {};
        }
        return std::wstring(normalizedPath.substr(dot));
    }

    class PathFilter
    {
    public:
        explicit PathFilter(const ConfigSnapshot& config) :
            _segmentFallbackEnabled(config.segmentFallbackEnabled),
            _includeAllExtensions(config.includeAllExtensions),
            _extensions(config.includeExtensions)
        {
            const auto addRoot = [this](const std::filesystem::path& rawPath) {
                const auto normalized = ToLower(NormalizePath(rawPath.wstring()));
                if (normalized.empty()) {
                    return;
                }
                std::wstring withSlash = normalized;
                if (withSlash.back() != L'\\') {
                    withSlash.push_back(L'\\');
                }
                _roots.push_back(std::move(withSlash));
            };

            addRoot(config.dataRoot);
            for (const auto& root : config.extraRoots) {
                addRoot(root);
            }
        }

        bool ShouldInclude(const std::wstring& rawPath, std::wstring& normalizedPathOut) const
        {
            normalizedPathOut = NormalizePath(rawPath);
            if (normalizedPathOut.empty()) {
                return false;
            }

            const std::wstring lowered = ToLower(normalizedPathOut);
            bool rootMatched = false;
            for (const auto& root : _roots) {
                if (StartsWith(lowered, root)) {
                    rootMatched = true;
                    break;
                }
            }

            bool segmentMatched = false;
            if (!rootMatched && _segmentFallbackEnabled) {
                static constexpr std::array<std::wstring_view, 4> segments{
                    L"\\meshes\\",
                    L"\\textures\\",
                    L"\\scripts\\",
                    L"\\sound\\"
                };
                for (const auto segment : segments) {
                    if (lowered.find(segment) != std::wstring::npos) {
                        segmentMatched = true;
                        break;
                    }
                }
            }

            if (!rootMatched && !segmentMatched) {
                return false;
            }

            if (_includeAllExtensions) {
                return true;
            }

            const auto ext = GetLowerExtension(lowered);
            return !ext.empty() && _extensions.contains(ext);
        }

    private:
        std::vector<std::wstring> _roots;
        bool _segmentFallbackEnabled = true;
        bool _includeAllExtensions = false;
        std::unordered_set<std::wstring> _extensions;
    };

    class AsyncLogger
    {
    public:
        AsyncLogger() = default;
        ~AsyncLogger();
        bool Start(const ConfigSnapshot& config);
        void Stop();
        bool TryEnqueue(RawEvent&& event);
        const std::filesystem::path& GetLogPath() const;

    private:
        bool WriteRaw(std::string_view content);
        void WriteHeader();
        void WriteDroppedWarningIfNeeded();
        std::string BuildEventLine(const RawEvent& event, const std::wstring& normalizedPath) const;
        void ProcessBatch(const std::vector<RawEvent>& batch);
        void WorkerLoop();

        ConfigSnapshot _config{};
        std::unique_ptr<PathFilter> _filter;
        std::mutex _queueMutex;
        std::condition_variable _queueCv;
        std::deque<RawEvent> _queue;
        std::thread _worker;
        std::atomic<bool> _running{ false };
        bool _stopRequested = false;
        std::atomic<std::uint64_t> _dropped{ 0 };
        HANDLE _logHandle = INVALID_HANDLE_VALUE;
        std::filesystem::path _logPath;
    };

    std::unique_ptr<AsyncLogger> g_logger;

    AsyncLogger::~AsyncLogger()
    {
        Stop();
    }

    bool AsyncLogger::Start(const ConfigSnapshot& config)
    {
        if (_running.load(std::memory_order_acquire)) {
            return true;
        }

        _config = config;
        _filter = std::make_unique<PathFilter>(_config);

        std::error_code ec;
        std::filesystem::create_directories(_config.logDir, ec);
        if (ec) {
            return false;
        }

        const auto now = std::chrono::system_clock::now();
        const auto wall = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
        localtime_s(&localTime, &wall);

        std::wostringstream fileName;
        fileName << L"FileTraceSKSE_"
                 << std::setw(4) << std::setfill(L'0') << localTime.tm_year + 1900
                 << std::setw(2) << std::setfill(L'0') << localTime.tm_mon + 1
                 << std::setw(2) << std::setfill(L'0') << localTime.tm_mday
                 << L"_"
                 << std::setw(2) << std::setfill(L'0') << localTime.tm_hour
                 << std::setw(2) << std::setfill(L'0') << localTime.tm_min
                 << std::setw(2) << std::setfill(L'0') << localTime.tm_sec
                 << L"_pid" << ::GetCurrentProcessId()
                 << L".log";
        _logPath = _config.logDir / fileName.str();

        _logHandle = ::CreateFileW(
            _logPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (_logHandle == INVALID_HANDLE_VALUE) {
            return false;
        }

        _stopRequested = false;
        _running.store(true, std::memory_order_release);
        _worker = std::thread(&AsyncLogger::WorkerLoop, this);
        return true;
    }

    void AsyncLogger::Stop()
    {
        if (!_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::lock_guard lock(_queueMutex);
            _stopRequested = true;
        }
        _queueCv.notify_all();

        if (_worker.joinable()) {
            _worker.join();
        }

        if (_logHandle != INVALID_HANDLE_VALUE) {
            ::FlushFileBuffers(_logHandle);
            ::CloseHandle(_logHandle);
            _logHandle = INVALID_HANDLE_VALUE;
        }

        _filter.reset();
    }

    bool AsyncLogger::TryEnqueue(RawEvent&& event)
    {
        if (!_running.load(std::memory_order_acquire)) {
            return false;
        }

        {
            std::lock_guard lock(_queueMutex);
            if (_queue.size() >= _config.queueCapacity) {
                _dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            _queue.push_back(std::move(event));
        }

        _queueCv.notify_one();
        return true;
    }

    const std::filesystem::path& AsyncLogger::GetLogPath() const
    {
        return _logPath;
    }

    bool AsyncLogger::WriteRaw(std::string_view content)
    {
        if (_logHandle == INVALID_HANDLE_VALUE || content.empty()) {
            return false;
        }

        const char* cursor = content.data();
        std::size_t remaining = content.size();

        while (remaining > 0) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!::WriteFile(_logHandle, cursor, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            remaining -= written;
            cursor += written;
        }

        return true;
    }

    void AsyncLogger::WriteHeader()
    {
        std::ostringstream line;
        line << "# FileTraceSKSE version=" << PLUGIN_VERSION
             << " pid=" << ::GetCurrentProcessId()
             << " data_root=" << WideToUtf8(_config.dataRoot.wstring())
             << " queue_capacity=" << _config.queueCapacity
             << " flush_interval_ms=" << _config.flushIntervalMs
             << "\r\n";
        line << "# timestamp | tid | success | duration_ms | access_text | share_mode_hex | creation_disp | last_error | normalized_path\r\n";
        WriteRaw(line.str());
    }

    void AsyncLogger::WriteDroppedWarningIfNeeded()
    {
        const std::uint64_t dropped = _dropped.exchange(0, std::memory_order_acq_rel);
        if (dropped == 0) {
            return;
        }

        std::ostringstream line;
        line << "[WARN] dropped=" << dropped
             << " reason=queue_full capacity=" << _config.queueCapacity
             << "\r\n";
        WriteRaw(line.str());
    }

    std::string AsyncLogger::BuildEventLine(const RawEvent& event, const std::wstring& normalizedPath) const
    {
        std::ostringstream line;
        line << FormatTimestamp(event.timestamp)
             << " | " << event.threadId
             << " | " << (event.success ? "1" : "0")
             << " | " << event.durationMs
             << " | " << AccessFlagsToText(event.desiredAccess)
             << " | 0x" << std::hex << std::uppercase << event.shareMode << std::dec
             << " | " << CreationDispositionToText(event.creationDisposition)
             << " | " << event.lastError
             << " | " << WideToUtf8(normalizedPath)
             << "\r\n";
        return line.str();
    }

    void AsyncLogger::ProcessBatch(const std::vector<RawEvent>& batch)
    {
        if (_filter == nullptr) {
            return;
        }

        std::string output;
        output.reserve(batch.size() * 140);

        for (const auto& event : batch) {
            if (!event.success && !_config.logFailedOpen) {
                continue;
            }
            if (event.durationMs < _config.minDurationMs) {
                continue;
            }

            std::wstring normalizedPath;
            if (!_filter->ShouldInclude(event.path, normalizedPath)) {
                continue;
            }

            output.append(BuildEventLine(event, normalizedPath));
        }

        if (!output.empty()) {
            WriteRaw(output);
        }
    }

    void AsyncLogger::WorkerLoop()
    {
        WriteHeader();
        const auto waitInterval = std::chrono::milliseconds(_config.flushIntervalMs);

        for (;;) {
            std::vector<RawEvent> batch;
            batch.reserve(512);

            {
                std::unique_lock lock(_queueMutex);
                _queueCv.wait_for(lock, waitInterval, [this] {
                    return _stopRequested || !_queue.empty();
                });
                while (!_queue.empty() && batch.size() < 512) {
                    batch.push_back(std::move(_queue.front()));
                    _queue.pop_front();
                }
                if (_stopRequested && _queue.empty() && batch.empty()) {
                    break;
                }
            }

            if (!batch.empty()) {
                ProcessBatch(batch);
            }
            WriteDroppedWarningIfNeeded();
        }

        std::vector<RawEvent> rest;
        {
            std::lock_guard lock(_queueMutex);
            rest.reserve(_queue.size());
            while (!_queue.empty()) {
                rest.push_back(std::move(_queue.front()));
                _queue.pop_front();
            }
        }
        if (!rest.empty()) {
            ProcessBatch(rest);
        }
        WriteDroppedWarningIfNeeded();
        if (_logHandle != INVALID_HANDLE_VALUE) {
            ::FlushFileBuffers(_logHandle);
        }
    }

    struct HookReentryGuard
    {
        HookReentryGuard()
        {
            if (!g_inHook) {
                g_inHook = true;
                active = true;
            }
        }
        ~HookReentryGuard()
        {
            if (active) {
                g_inHook = false;
            }
        }
        bool active = false;
    };

    struct ActiveHookCallGuard
    {
        ActiveHookCallGuard()
        {
            g_activeHookCalls.fetch_add(1, std::memory_order_acq_rel);
        }
        ~ActiveHookCallGuard()
        {
            g_activeHookCalls.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    HANDLE WINAPI HookedCreateFileW(
        LPCWSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile);

    HANDLE WINAPI HookedCreateFileA(
        LPCSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile);

    FARPROC ResolveCreateFileProcAddress(const char* procName)
    {
        static constexpr std::array<const wchar_t*, 2> modules{
            L"KernelBase.dll",
            L"Kernel32.dll"
        };
        for (const auto* moduleName : modules) {
            if (const auto module = ::GetModuleHandleW(moduleName)) {
                if (const auto proc = ::GetProcAddress(module, procName)) {
                    return proc;
                }
            }
        }
        return nullptr;
    }

    bool EnsureHookEngineInitialized()
    {
        if (g_hookEngineInitialized.load(std::memory_order_acquire)) {
            return true;
        }
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            return false;
        }
        g_hookEngineInitialized.store(true, std::memory_order_release);
        return true;
    }

    bool InstallHooks()
    {
        if (!EnsureHookEngineInitialized()) {
            return false;
        }

        g_createFileWTarget = reinterpret_cast<LPVOID>(ResolveCreateFileProcAddress("CreateFileW"));
        g_createFileATarget = reinterpret_cast<LPVOID>(ResolveCreateFileProcAddress("CreateFileA"));
        if (g_createFileWTarget == nullptr || g_createFileATarget == nullptr) {
            return false;
        }

        const auto statusW = MH_CreateHook(
            g_createFileWTarget,
            reinterpret_cast<LPVOID>(&HookedCreateFileW),
            reinterpret_cast<LPVOID*>(&g_originalCreateFileW));
        if (statusW != MH_OK && statusW != MH_ERROR_ALREADY_CREATED) {
            return false;
        }

        const auto statusA = MH_CreateHook(
            g_createFileATarget,
            reinterpret_cast<LPVOID>(&HookedCreateFileA),
            reinterpret_cast<LPVOID*>(&g_originalCreateFileA));
        if (statusA != MH_OK && statusA != MH_ERROR_ALREADY_CREATED) {
            MH_RemoveHook(g_createFileWTarget);
            return false;
        }

        const auto enableW = MH_EnableHook(g_createFileWTarget);
        const auto enableA = MH_EnableHook(g_createFileATarget);
        if ((enableW != MH_OK && enableW != MH_ERROR_ENABLED) ||
            (enableA != MH_OK && enableA != MH_ERROR_ENABLED)) {
            MH_DisableHook(g_createFileWTarget);
            MH_DisableHook(g_createFileATarget);
            MH_RemoveHook(g_createFileWTarget);
            MH_RemoveHook(g_createFileATarget);
            return false;
        }

        g_hooksEnabled.store(true, std::memory_order_release);
        return true;
    }

    void DisableHooks()
    {
        if (!g_hooksEnabled.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (g_createFileWTarget != nullptr) {
            MH_DisableHook(g_createFileWTarget);
        }
        if (g_createFileATarget != nullptr) {
            MH_DisableHook(g_createFileATarget);
        }
    }

    void UninitializeHookEngine()
    {
        if (!g_hookEngineInitialized.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (g_createFileWTarget != nullptr) {
            MH_RemoveHook(g_createFileWTarget);
            g_createFileWTarget = nullptr;
        }
        if (g_createFileATarget != nullptr) {
            MH_RemoveHook(g_createFileATarget);
            g_createFileATarget = nullptr;
        }
        MH_Uninitialize();
    }

    void WaitForInFlightHooks()
    {
        constexpr std::uint32_t maxWaitMs = 5000;
        std::uint32_t waited = 0;
        while (g_activeHookCalls.load(std::memory_order_acquire) != 0 && waited < maxWaitMs) {
            ::Sleep(1);
            ++waited;
        }
    }

    template <class CallOriginal>
    HANDLE TraceCreateFileCommon(
        const wchar_t* pathForLog,
        DWORD desiredAccess,
        DWORD shareMode,
        DWORD creationDisposition,
        CallOriginal&& callOriginal)
    {
        if (g_inHook) {
            return callOriginal();
        }

        HookReentryGuard reentry;
        if (!reentry.active) {
            return callOriginal();
        }

        ActiveHookCallGuard inFlight;
        if (!g_runtimeEnabled.load(std::memory_order_acquire) || g_logger == nullptr) {
            return callOriginal();
        }

        const auto startTick = std::chrono::steady_clock::now();
        const auto startWall = std::chrono::system_clock::now();
        const HANDLE handle = callOriginal();
        const DWORD lastError = ::GetLastError();
        const bool success = handle != INVALID_HANDLE_VALUE && handle != nullptr;
        const auto durationMs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTick).count());

        if (pathForLog != nullptr &&
            pathForLog[0] != L'\0' &&
            (success || g_config.logFailedOpen) &&
            durationMs >= g_config.minDurationMs) {
            RawEvent event{};
            event.timestamp = startWall;
            event.threadId = ::GetCurrentThreadId();
            event.success = success;
            event.durationMs = durationMs;
            event.desiredAccess = desiredAccess;
            event.shareMode = shareMode;
            event.creationDisposition = creationDisposition;
            event.lastError = success ? ERROR_SUCCESS : lastError;
            event.path = pathForLog;
            g_logger->TryEnqueue(std::move(event));
        }

        ::SetLastError(lastError);
        return handle;
    }

    HANDLE WINAPI HookedCreateFileW(
        LPCWSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile)
    {
        const auto original = g_originalCreateFileW != nullptr ? g_originalCreateFileW : ::CreateFileW;
        return TraceCreateFileCommon(
            fileName,
            desiredAccess,
            shareMode,
            creationDisposition,
            [&]() {
                return original(
                    fileName,
                    desiredAccess,
                    shareMode,
                    securityAttributes,
                    creationDisposition,
                    flagsAndAttributes,
                    templateFile);
            });
    }

    HANDLE WINAPI HookedCreateFileA(
        LPCSTR fileName,
        DWORD desiredAccess,
        DWORD shareMode,
        LPSECURITY_ATTRIBUTES securityAttributes,
        DWORD creationDisposition,
        DWORD flagsAndAttributes,
        HANDLE templateFile)
    {
        const std::wstring widePath = fileName != nullptr ? AnsiToWide(fileName) : std::wstring{};
        const auto original = g_originalCreateFileA != nullptr ? g_originalCreateFileA : ::CreateFileA;
        return TraceCreateFileCommon(
            widePath.c_str(),
            desiredAccess,
            shareMode,
            creationDisposition,
            [&]() {
                return original(
                    fileName,
                    desiredAccess,
                    shareMode,
                    securityAttributes,
                    creationDisposition,
                    flagsAndAttributes,
                    templateFile);
            });
    }

    bool StartTracing()
    {
        std::lock_guard lock(g_lifecycleMutex);
        if (g_runtimeEnabled.load(std::memory_order_acquire)) {
            return true;
        }

        g_config = LoadConfig();
        if (!g_config.enabled) {
            SKSE::log::info("FileTraceSKSE disabled by ini: {}", WideToUtf8(g_config.iniPath.wstring()));
            return true;
        }

        g_logger = std::make_unique<AsyncLogger>();
        if (!g_logger->Start(g_config)) {
            g_logger.reset();
            SKSE::log::error("FileTraceSKSE failed to start logger");
            return false;
        }

        if (!InstallHooks()) {
            g_logger->Stop();
            g_logger.reset();
            SKSE::log::error("FileTraceSKSE failed to install hooks");
            return false;
        }

        g_runtimeEnabled.store(true, std::memory_order_release);
        SKSE::log::info(
            "FileTraceSKSE enabled. ini={} log={}",
            WideToUtf8(g_config.iniPath.wstring()),
            WideToUtf8(g_logger->GetLogPath().wstring()));
        return true;
    }

    void StopTracing()
    {
        std::lock_guard lock(g_lifecycleMutex);
        g_runtimeEnabled.store(false, std::memory_order_release);
        DisableHooks();
        WaitForInFlightHooks();
        UninitializeHookEngine();
        if (g_logger != nullptr) {
            g_logger->Stop();
            g_logger.reset();
        }
    }

    struct RuntimeFinalizer
    {
        ~RuntimeFinalizer()
        {
            StopTracing();
        }
    };

    RuntimeFinalizer g_finalizer;
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info)
{
    info->infoVersion = SKSE::PluginInfo::kVersion;
    info->name = PLUGIN_NAME;
    info->version = 1;
    return !skse->IsEditor();
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData data;
    data.PluginVersion(REL::Version{ 1, 0, 0 });
    data.PluginName(PLUGIN_NAME);
    data.AuthorName(PLUGIN_AUTHOR);
    data.UsesAddressLibrary();
    data.CompatibleVersions({
        SKSE::RUNTIME_SSE_LATEST_AE,
        SKSE::RUNTIME_SSE_LATEST_SE
    });
    return data;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    SKSE::log::info("========================================");
    SKSE::log::info("{} v{}", PLUGIN_NAME, PLUGIN_VERSION);
    SKSE::log::info("Author: {}", PLUGIN_AUTHOR);
    SKSE::log::info("========================================");

    if (!StartTracing()) {
        SKSE::log::error("FileTraceSKSE startup failed");
        return false;
    }

    SKSE::log::info("FileTraceSKSE loaded successfully");
    return true;
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_moduleHandle = module;
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
