# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Reasonix, etc.) when working with code in this repository.

## Project Overview

BTAudio is a Windows system-tray application that provides Bluetooth A2DP Sink audio playback connectivity for Windows 10 2004+. It uses C++/WinRT to wrap the Windows Runtime `AudioPlaybackConnection` API, with a Win32 message-loop core and XAML Islands for the UI. It runs silently in the notification area and surfaces a popup window to discover, connect, and manage Bluetooth audio devices.

## Build & Run

- **Solution**: `BTAudio.sln` — Visual Studio 2022, PlatformToolset v145, C++20 (`stdcpplatest`)
- **Configurations**: Debug/Release × x64, Win32, ARM, ARM64
- **Build**: Open in VS2022 and build, or use MSBuild:
  ```
  msbuild BTAudio.sln /p:Configuration=Release /p:Platform=x64
  ```
- **NuGet packages** (restored automatically on build):
  - `Microsoft.Windows.CppWinRT 3.0.260520.1`
  - `Microsoft.Windows.ImplementationLibrary 1.0.260126.7` (wil)
- **Precompiled header**: `pch.h` / `pch.cpp`
- **Output**: `BTAudio64.exe` (x64), with platform suffix for other architectures
- **Version**: Defined in `resource.h` (`BTAUDIO_VERSION_MAJOR`/`MINOR`/`PATCH`/`BUILD`)
- **Translations**: `translate/generated/` (gitignored) is regenerated automatically by a `PreBuildEvent` running `translate/gen_rc.ps1` (requires python3; incremental — no-ops when the ymo files are fresh). Regenerate manually with `powershell -File translate/gen_rc.ps1` after editing `translate/source/*.po`.

## Code Architecture

### Source Files

| File | Purpose |
|------|---------|
| `BTAudio.cpp` | Main app — `wWinMain`, `WndProc` (exception boundary) + `HandleMessage`, tray icon, device watcher, connection logic, main window UI, retry/reconnect machinery (~2900 lines) |
| `BTAudio.h` | Global declarations — all `g_*` state variables (`ConnectingEntry`, `chainId`), forward declarations, WM_APP message IDs, retry constants |
| `pch.h` | Precompiled header — Win32 API, C++/WinRT projections, wil headers |

### Utility Headers (single-file `.hpp`, all inline)

| File | Purpose |
|------|---------|
| `Util.hpp` | `Utf8ToUtf16` / `Utf16ToUtf8` conversion, `GetModuleFsPath` |
| `I18n.hpp` | Lightweight i18n via FNV-1a hash lookup in embedded YMO-format resources |
| `FnvHash.hpp` | FNV-1a 32-bit hash implementation |
| `SettingsUtil.hpp` | JSON settings persistence (`BTAudio.json`): reconnect flag, auto-start, last devices, device aliases |
| `UpdateChecker.hpp` | GitHub API release check, version comparison, `DownloadAndInstall` with batch-based self-replacement |
| `Direct2DSvg.hpp` | SVG → HICON rendering via Direct2D 1.3 SVG Document API |

### Resource / Config

| File | Purpose |
|------|---------|
| `resource.h` | Version macros (current 1.1.12), icon resource ID |
| `BTAudio.rc` | Windows resources: icon, version info, SVG |
| `BTAudio.svg` | Tray icon SVG source (recolored at runtime for state/theme) |
| `translate/generated/` | Compiled translation data files (`translate.rc` + `zh_CN.ymo` / `zh_TW.ymo`, gitignored, auto-generated at build) |

### Architecture Overview (Message Flow)

```
 WinMain
   ├── Init: create hidden window + XAML Islands, load settings, setup watcher/UI
   ├── SetTimer(IDT_STARTUP_DELAY, 1500ms)
   ├── CheckForUpdate(true) [silent]
   └── Message Loop
         ├── WM_CONNECTDEVICE → queue remembered devices for serial connect
         ├── WM_CONNECTNEXT   → dequeue & connect one device at a time
         ├── WM_DEVICEAPPEARED → promote out-of-range devices into queue
         ├── WM_DEVICEADDED / WM_DEVICEUPDATED / WM_DEVICEREMOVED
         │                    → DeviceWatcher callbacks (any thread) post
         │                      payloads; UI thread updates g_availableDevices
         ├── WM_CONNECTRESULT → apply one attempt's outcome (all map
         │                      bookkeeping, retries, notifications)
         ├── WM_DEVICECLOSED  → auto-reconnect on unexpected link drop
         ├── WM_NOTIFYICON    → tray click (show menu / toggle window)
         ├── WM_REFRESHDEVICELIST → rebuild connected/available device lists
         ├── WM_UPDATEAVAILABLE / WM_UPDATEINSTALLED → update dialogs
         ├── WM_UPTODATE / WM_UPDATEFAILED → update notification dialogs
         ├── WM_SHOWMAINWINDOW → second instance asks for the main window
         └── WM_SETTINGCHANGE → theme change → update icons + XAML theme
```

### Connection Model

```
ConnectDevice(deviceId/DeviceInformation)
  → TryCreateFromId → register StateChanged → StartAsync → OpenAsync
  → On Success: add to g_audioPlaybackConnections, persist, notify
  → On Failure: Close the failed connection object (deterministic transport
    teardown), retry transient failures with exponential backoff
  → A failed chain NEVER touches g_audioPlaybackConnections — an entry there
    belongs to a healthy connection from another chain; link drops arrive via
    WM_DEVICECLOSED, duplicates are replaced on the success path
```

StateChanged(Closed) fires on an arbitrary thread. It posts `WM_DEVICECLOSED` to the UI thread, which calls `HandleDeviceClosed`. The decision to auto-reconnect is based on map membership PLUS a generation check: the message carries the ABI pointer of the connection that raised the event; if the connection currently in `g_audioPlaybackConnections` is a different instance (replaced by a reconnect / duplicate-connection teardown), the event is a stale `Close()` and is ignored. Entry still present with the same instance → unexpected link drop → auto-reconnect; entry already gone → user/system initiated → ignore.

### Retry/Reconnect Strategy

- **Manual retries** (user clicks Connect): `MANUAL_RETRY_COUNT = 2` → 3 attempts total, backoff 1s → 2s (capped at 4s)
- **Auto-reconnect** (unexpected link drop): `AUTO_RECONNECT_RETRY_COUNT = 12` → 13 attempts total, backoff 2s → 4s → 8s → 16s → 30s (capped), i.e. ~4.5 minutes; `AUTO_RECONNECT_TIMEOUT_MS = 300000` (5 min) is the hard wall-clock cap, so the two limits agree (a 10-minute cap used to be unreachable)
- **Startup reconnect**: `STARTUP_RETRY_COUNT = 6` → 7 attempts, ~19s of backoff; devices connected serially (one at a time) from a FIFO queue, pumped by `WM_CONNECTNEXT`
- **Deferred connection**: devices out-of-range at startup go into `g_pendingConnectOnAppear` and connect lazily when the DeviceWatcher reports them

### Key Design Decisions

- **Single-instance**: enforced via named mutex at `wWinMain` entry; `ERROR_ALREADY_EXISTS` and `ERROR_ACCESS_DENIED` both count as "already running" (the second instance retries `FindWindowW` briefly, then posts `WM_SHOWMAINWINDOW`)
- **Tray icons**: rendered from SVG at runtime with state-specific colors (normal, connecting amber, connected green), with light/dark theme variants; falls back to the embedded `.ico` if SVG rendering failed
- **Settings**: `BTAudio.json` in the *data directory* — portable-first (`ResolveDataDir()`): the exe directory when an existing config lives there or when it is writable, otherwise `%LOCALAPPDATA%\BTAudio` (a read-only install such as Program Files used to lose every setting silently). `BTAudio.log` follows the same directory. Incremental save on each connect/disconnect/cancel
- **I18n**: custom FNV-1a hash-based system — strings are hashed at runtime and looked up in an embedded resource block per thread UI language, with `_("string")` / `C_(context, string)` macros; language lookup falls back exact LANGID → neutral sublanguage → Simplified Chinese (for any Chinese locale) → English
- **Update mechanism**: downloads new EXE from GitHub, spawns a batch script that waits for the current process to exit, replaces the file, and relaunches. The asset must be an `.exe` whose name matches the build architecture, is stored under a fixed per-process temp name (never the remote asset name), and is validated against the declared size and the published sha256 digest before being applied. If the helper batch cannot replace the exe it writes a marker in `%TEMP%`; the next start consumes it (`ConsumeFailedUpdateMarker()`) and shows `WM_UPDATEFAILED` once
- **Thread safety**: all global state mutations happen on the UI thread (message loop). Cross-thread paths are limited to code that ONLY forwards heap-allocated payloads via `PostMessage`: `StateChanged` → `WM_DEVICECLOSED` (device id + connection ABI pointer), DeviceWatcher callbacks → `WM_DEVICEADDED/UPDATED/REMOVED` (AddRef'd `IUnknown*`), update check → `WM_UPDATEAVAILABLE` (ReleaseInfo), and the `ConnectDevice` coroutine continuations → `WM_CONNECTRESULT` (ConnectResultInfo). The UI thread takes ownership and does all map bookkeeping.
- **Map ordering**: `g_audioPlaybackConnections` is a `std::unordered_map` (NOT sorted) — do not rely on iteration order; the tray tooltip simply shows the first entry. `g_availableDevices` is a `std::map` (sorted by device id).

### Known Issue — Coroutine Threading Model (FIXED)

**Fixed** in the "coroutine thread model refactor": `ConnectDevice` coroutines no longer touch any global state. The app runs in an MTA (`winrt::init_apartment()`), so a `fire_and_forget` coroutine resumes on a thread-pool thread after `co_await`. Both `ConnectDevice` overloads now only perform the async operations (`CreateFromIdAsync` / `TryCreateFromId` / `StartAsync` / `OpenAsync`), collect the outcome into a heap-allocated `ConnectResultInfo`, and post it as `WM_CONNECTRESULT`. All map bookkeeping (`g_audioPlaybackConnections`, `g_connectingDevices`, `g_autoReconnectingDevices`, `g_pendingReconnects`, `g_connectInProgress`), `SaveSettings()`, `UpdateNotifyIcon()`, retry scheduling and dialogs/notifications happen in `HandleConnectResult` (or `StartConnect` for the "connecting" entry state) on the UI thread.

Rules that keep it that way:
- Start a connect chain ONLY via `StartConnect` (UI thread) — it sets the connecting state, guards against a second concurrent chain for the same device (each entry carries a `chainId`; `chainId == 0` starts a new chain and is refused while an entry exists, a non-zero id continues that chain and is refused once the entry is gone), and launches the coroutine. Never call `ConnectDevice` directly from a coroutine continuation.
- Never add global-state access to coroutine continuations. If a new async path needs to mutate state, post a message carrying a heap-allocated payload (as `WM_CONNECTRESULT`/`WM_DEVICECLOSED`/`WM_DEVICEADDED` do) and do the mutation on the UI thread.
- `HandleConnectResult` performs the duplicate-connection replacement (erase + `Close()` before replacing) that the coroutine used to do; the `StateChanged` callback still only forwards `WM_DEVICECLOSED`. A FAILED chain never touches `g_audioPlaybackConnections` (its entry, if any, belongs to a healthy connection from a concurrent chain) — the coroutine `Close()`es the failed connection object itself for deterministic transport teardown.
- `WM_DEVICEREMOVED` cancels any pending reconnect for the removed device (the id can no longer resolve); an in-flight attempt is cleaned up via the cancellation check in `HandleConnectResult`.

### Global State (in BTAudio.h)

The app uses module-level globals, not a class. Connection maps are always manipulated from the UI thread:
- `g_audioPlaybackConnections` — active connections (deviceId → {DeviceInformation, AudioPlaybackConnection})
- `g_availableDevices` — discovered Bluetooth audio devices
- `g_connectingDevices` — devices in the process of connecting (`ConnectingEntry{ name, chainId, isAutoReconnect }`)
- `g_pendingReconnects` — scheduled retry timers (each carries its `chainId`)
- `g_connectQueue` / `g_connectInProgress` — serial startup reconnect state; every terminal path must go through `EndSerialConnectChain()`
- `g_pendingConnectOnAppear` — devices deferred until watcher reports them
- `g_deviceAliases` / `g_deviceVolumes` — per-device display name and restored volume (pruned only above 256 entries, and only for ids absent from the connection map, the watcher list and `g_lastDevices` — a device that is merely out of range must keep its alias)
- `g_dataDir` — resolved settings/log directory (portable-first, AppData fallback)

## 重要约定 / 踩坑记录

- **`WndProc` 只是异常边界**：真正的消息处理在 `HandleMessage`。任何新增 case 都不需要自己包 try/catch（WinRT 调用在蓝牙服务重启/睡眠唤醒后会抛 `RPC_E_DISCONNECTED`，逃出窗口过程会直接结束进程）。`WM_DESTROY` 例外：包装层在捕获异常后仍会 `PostQuitMessage`，避免留下不可见的进程。
- **连接链必须带 `chainId`**：`g_connectingDevices` 的条目归属于某一条链。重入保护、重试续链、取消都必须校验 id；`CancelReconnect`/设备移除可以强制擦除（`erase(key)`），但**不能**在别的链正在跑时按 key 无条件擦除（曾导致双链并发、`g_autoReconnectingDevices` 残留）。
- **串行泵只有 `EndSerialConnectChain()` 一个出口**：凡是杀掉 `notifyNextOnComplete` 的待重试定时器、或终结该链（成功/失败/被取消/设备移除）都必须调用它，否则 `g_connectInProgress` 永久为 true，后续队列与 `WM_DEVICEAPPEARED` 新设备再也不会连接。
- **`HandleDeviceClosed` 不再自己开新链**：如果该设备已有链在跑，只摘除失效连接并落盘，让在跑的链接管——否则两条链会争抢同一个射频，且失败链可能把刚建立好的连接拆掉（v1.1.9 修复的"幽灵断开"）。
- **弹窗锚点统一走 `PrepareFlyoutAnchor()`**：它同时把 `g_mainWindowVisible` 置 false 并把窗口缩到托盘图标处。更新弹窗曾直接 `SetWindowPos(...,0,0,SWP_HIDEWINDOW)` 而不清标志，导致托盘左键要点两次才能重开主窗口。
- **窗口隐藏时 `RefreshDeviceList()` 直接返回**（只更新托盘图标）；`ShowMainWindow()` 传 `force=true` 一次性同步。否则每个电量/属性更新都会在后台重建整棵 XAML 树。
- **音频端点对象有 2 秒缓存**：增删设备、连接成功、链路掉线后都要 `InvalidateAudioEndpointCache()`，否则会继续操作旧的 `IAudioEndpointVolume`（例如蓝牙耳机切换后音量为 0 或读不到）。
- **更新器不接受远端提供的路径/文件名**：`assetName` 只用于日志，落盘名固定为 `BTAudio_update_<pid>.exe`；资产必须是 `.exe`，架构比较前统一转小写（`GetCurrentArch()` 返回的是大写 `ARM64`/`ARM`，曾导致 ARM 机器下载到 x64 包）；批处理里双引号内只转义 `%`（`^` 在引号内不是转义符）。
- **配置目录是"便携优先 + AppData 回退"**：`ResolveDataDir()` 必须早于 `LoadSettings()` 调用；不要把读写路径硬编码成 `GetModuleFsPath(...).remove_filename()`（装到 Program Files 时会静默失败）。
- **`g_debugLogging` 是 `std::atomic<bool>`**：wil 回调在任意线程触发，启动时无条件安装回调，开关只控制是否落盘。

## 变更记录

- **2026-10-21 — v1.1.11 全量缺陷修复**
  - 更新器：ARM/ARM64 架构匹配大小写归一化；资产必须为 `.exe` 且优先产品名；下载文件名净化（PID 隔离，杜绝路径穿越）；校验声明大小与 sha256；`%ComSpec%` 解析 + 创建进程失败上报；批处理 `%` 转义与 `move` 重试，替换成功后才启动新版本。
  - 连接状态机：引入 `chainId` 连接链身份；`HandleConnectResult` 用链归属判定结果（过期结果直接丢弃并关闭已建立的连接）；`CancelReconnect`/设备移除结束串行链并落盘；`HandleDeviceClosed` 不再另起并发链；通知文案设备名四级兜底。
  - 稳定性：`WndProc` 拆出 `HandleMessage` 并统一异常边界；DeviceWatcher 重建失败后由 30s 轮询重试；`WM_DPICHANGED` 的 SVG 重绘失败降级到内置 `.ico`；`WM_CLOSE` 落盘音量；单实例识别 `ERROR_ACCESS_DENIED`；消息循环处理 `GetMessageW() == -1`。
  - 存储：`ResolveDataDir()`（便携优先 + `%LOCALAPPDATA%` 回退），日志同目录；Run 键写入带引号路径；音量值读取裁剪到 `[0,1]`；设备别名/音量超过 64 条时清理未知设备。
  - 体验/性能：`IAudioEndpointVolume` 缓存（2s TTL + 失败退避）；主窗口隐藏时跳过 XAML 重建；每设备每次刷新只读一次电量；音量滑块就地更新时重新评估启用状态；自动重连中转为可取消卡片；托盘提示 `StringCchPrintfW` 截断；I18n 语言回退链与 YMO 资源边界校验；`swprintf` 错误信息长度上限。
  - 版本 1.1.11，`.rc` 中 `InternalName`/`OriginalFilename` 改为平台中性 `BTAudio`。
  - 审查补漏：单实例按真实错误码分流（`CreateMutexW` 的其它失败不再被当成"已运行"而静默退出）；更新失败标记在下次启动时消费并提示；WIL natvis 的手写条目全部删除——`Microsoft.Windows.ImplementationLibrary.targets` 已经自动 `<Natvis Include=...>`，再手写一份会让 VS 报"筛选器下已存在项目"；设备偏好剪枝阈值提高到 256 并把 `g_lastDevices` 计入已知设备。
