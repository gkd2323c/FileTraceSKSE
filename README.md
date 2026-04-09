# FileTraceSKSE

用于调试 Skyrim 文件加载行为的 SKSE 插件。  
V1 目标是追踪 `CreateFileW/CreateFileA`，并将事件异步写入可实时跟读的日志文件。

## 功能

- Hook `CreateFileW` 与 `CreateFileA`（优先 `KernelBase`，失败回退 `Kernel32`）
- Hook 线程只做轻量采样与入队，不做同步落盘
- 后台线程异步批量写日志（`CreateFileW + WriteFile`，`FILE_SHARE_READ`）
- UTF-8 INI 配置（`SimpleIniW`）
- 路径过滤：`DataRoot/ExtraRoots` 优先，支持目录段兜底，最终按扩展名白名单过滤
- 队列打满时丢弃并输出 `[WARN] dropped=...` 可观测告警

## 构建要求

- CMake 3.24+
- MSVC Build Tools（x64 C++）
- 首次构建需要联网下载依赖：
  - CommonLibSSE-NG
  - MinHook
  - SimpleIni

## 构建

```cmd
cmake -B build -S .
cmake --build build --config Release
```

输出 DLL：

`build/Release/FileTraceSKSE.dll`

## 安装

1. 复制 `FileTraceSKSE.dll` 到：
   - `<Skyrim>/Data/SKSE/Plugins/`
2. 复制 `FileTraceSKSE.ini` 到同目录：
   - `<Skyrim>/Data/SKSE/Plugins/FileTraceSKSE.ini`
3. 用 SKSE 启动游戏

## 日志位置

默认日志目录（可配置）：

`C:\Users\<用户名>\Documents\My Games\Skyrim Special Edition\SKSE`

日志文件名格式：

`FileTraceSKSE_yyyyMMdd_HHmmss_pid<id>.log`

可用 PowerShell 实时跟读：

```powershell
Get-Content "C:\\Users\\<用户名>\\Documents\\My Games\\Skyrim Special Edition\\SKSE\\<logfile>.log" -Wait
```

## 配置

配置文件：`FileTraceSKSE.ini`（UTF-8）

```ini
[FileTraceSKSE]
Enabled=true
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

- `Enabled`：总开关
- `DataRoot`：`auto` 时自动推导 `<GameRoot>/Data`
- `ExtraRoots`：额外根目录，使用 `;` 分隔
- `SegmentFallbackEnabled`：根目录未命中时是否启用目录段兜底
- `IncludeAllExtensions`：是否监听所有后缀文件（开启后忽略 `IncludeExtensions` 白名单）
- `IncludeExtensions`：扩展名白名单（`|` 分隔）
- `QueueCapacity`：有界队列容量（满则丢弃并计数）
- `FlushIntervalMs`：后台线程写盘周期
- `MinDurationMs`：最小记录阈值（句柄打开耗时）
- `LogFailedOpen`：是否记录打开失败事件
- `LogDir`：日志目录（`auto` 默认到 `C:\Users\<用户名>\Documents\My Games\Skyrim Special Edition\SKSE`）

## 日志格式

Header 后每行字段顺序固定：

`timestamp | tid | success | duration_ms | access_text | share_mode_hex | creation_disp | last_error | normalized_path`

队列丢弃统计行：

`[WARN] dropped=<N> reason=queue_full capacity=<C>`

## V1 边界

- 不追踪 `NtCreateFile/NtOpenFile`
- 不追踪 BSA 内部条目路径
- INI 启动时读取一次，运行期不热重载
