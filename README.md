# FileTraceSKSE

用于调试 Skyrim / SKSE 的文件加载行为。  
插件会异步记录文件访问事件，减少对游戏线程的干扰。

## 主要功能

- Hook `CreateFileW` / `CreateFileA`，记录实际文件打开。
- 新增 `BSResourceNiBinaryStream::ctor` Hook，可追踪 BSA 封装包中的资源条目请求。
- 异步队列写日志，避免在 Hook 内做重 I/O。
- 支持扩展名白名单，或通过 `IncludeAllExtensions=true` 监听所有后缀。
- 支持路径过滤、失败事件记录、最小时延过滤和队列丢弃告警。

## 构建

```cmd
cmake -B build -S .
cmake --build build --config Release
```

输出：

`build/Release/FileTraceSKSE.dll`

## 安装

1. 复制 `FileTraceSKSE.dll` 到：
   - `<Skyrim>/Data/SKSE/Plugins/`
2. 复制 `FileTraceSKSE.ini` 到：
   - `<Skyrim>/Data/SKSE/Plugins/FileTraceSKSE.ini`

## 默认日志目录

`C:\Users\<用户名>\Documents\My Games\Skyrim Special Edition\SKSE`

日志文件名示例：

`FileTraceSKSE_yyyyMMdd_HHmmss_pid<id>.log`

## 配置文件

`FileTraceSKSE.ini`（UTF-8）：

```ini
[FileTraceSKSE]
Enabled=true
TraceBsaEntries=true
DataRoot=auto
ExtraRoots=
SegmentFallbackEnabled=true
IncludeAllExtensions=false
IncludeExtensions=.nif|.dds|.hkx|.hkb|.tri|.esp|.esm|.esl|.bsa|.pex|.psc|.wav|.xwm|.lip
QueueCapacity=8192
FlushIntervalMs=100
MinDurationMs=1
LogFailedOpen=true
LogDir=auto
```

字段说明：

- `Enabled`：总开关。
- `TraceBsaEntries`：是否追踪 BSA 内部资源条目（通过资源系统层 Hook）。
- `DataRoot`：`auto` 时自动推导为 `<GameRoot>/Data`。
- `ExtraRoots`：额外根目录，使用 `;` 分隔。
- `SegmentFallbackEnabled`：根目录未命中时，是否按常见资源目录段兜底。
- `IncludeAllExtensions`：开启后忽略 `IncludeExtensions` 白名单。
- `IncludeExtensions`：扩展名白名单（`|` 分隔）。
- `QueueCapacity`：异步队列容量。
- `FlushIntervalMs`：后台批量刷新间隔。
- `MinDurationMs`：最小时延过滤阈值。
- `LogFailedOpen`：是否记录失败事件。
- `LogDir`：日志目录（`auto` 使用默认文档目录）。

## 日志格式

日志头后每行字段：

`timestamp | tid | source | success | duration_ms | access_text | share_mode_hex | creation_disp | last_error | normalized_path | context`

- `source=win32_open`：来自 `CreateFile`。
- `source=bsa_entry`：来自资源系统条目请求（可覆盖 BSA 资源读取场景）。

队列丢弃告警示例：

`[WARN] dropped=<N> reason=queue_full capacity=<C>`
