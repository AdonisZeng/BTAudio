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
| `BTAudio.cpp` | Main app — `wWinMain`, `WndProc`, tray icon, device watcher, connection logic, main window UI, retry/reconnect machinery (~1750 lines) |
| `BTAudio.h` | Global declarations — all `g_*` state variables, forward declarations, WM_APP message IDs, retry constants |
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
| `resource.h` | Version macros (current 1.1.8), icon resource ID |
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
         ├── WM_DEVICECLOSED  → auto-reconnect on unexpected link drop
         ├── WM_CONNECTFAILED → TaskDialog for failed manual connects
         ├── WM_NOTIFYICON    → tray click (show menu / toggle window)
         ├── WM_REFRESHDEVICELIST → rebuild connected/available device lists
         ├── WM_UPDATEAVAILABLE (carries ReleaseInfo) → update dialog
         ├── WM_UPTODATE / WM_UPDATEFAILED → update notification dialogs
         └── WM_SETTINGCHANGE → theme change → update icons + XAML theme
```

### Connection Model

```
ConnectDevice(deviceId/DeviceInformation)
  → TryCreateFromId → register StateChanged → StartAsync → OpenAsync
  → On Success: add to g_audioPlaybackConnections, persist, notify
  → On Transient Failure: retry with exponential backoff
  → On Permanent Failure: notify error, erase connection
```

StateChanged(Closed) fires on an arbitrary thread. It posts `WM_DEVICECLOSED` to the UI thread, which calls `HandleDeviceClosed`. The decision to auto-reconnect is based on map membership PLUS a generation check: the message carries the ABI pointer of the connection that raised the event; if the connection currently in `g_audioPlaybackConnections` is a different instance (replaced by a reconnect / duplicate-connection teardown), the event is a stale `Close()` and is ignored. Entry still present with the same instance → unexpected link drop → auto-reconnect; entry already gone → user/system initiated → ignore.

### Retry/Reconnect Strategy

- **Manual retries** (user clicks Connect): 2 attempts, 500ms → 1s → 2s → 4s exponential backoff
- **Auto-reconnect** (unexpected link drop): 8 attempts, 1s → 2s → 4s → 8s → 16s → 30s (capped) exponential backoff
- **Startup reconnect**: devices connected serially (one at a time) from a FIFO queue, pumped by `WM_CONNECTNEXT`
- **Deferred connection**: devices out-of-range at startup go into `g_pendingConnectOnAppear` and connect lazily when the DeviceWatcher reports them

### Key Design Decisions

- **Single-instance**: enforced via named mutex at `wWinMain` entry
- **Tray icons**: rendered from SVG at runtime with state-specific colors (normal, connecting amber, connected green), with light/dark theme variants
- **Settings**: saved as `BTAudio.json` next to the executable, incremental save on each connect/disconnect
- **I18n**: custom FNV-1a hash-based system — strings are hashed at runtime and looked up in an embedded resource block per thread UI language, with `_("string")` / `C_(context, string)` macros
- **Update mechanism**: downloads new EXE from GitHub, spawns a batch script that waits for the current process to exit, replaces the file, and relaunches
- **Thread safety**: all global state mutations happen on the UI thread (message loop). Cross-thread paths are limited to code that ONLY forwards heap-allocated payloads via `PostMessage`: `StateChanged` → `WM_DEVICECLOSED` (device id + connection ABI pointer), DeviceWatcher callbacks → `WM_DEVICEADDED/UPDATED/REMOVED` (AddRef'd `IUnknown*`), update check → `WM_UPDATEAVAILABLE` (ReleaseInfo), and the `ConnectDevice` coroutine continuations → `WM_CONNECTRESULT` (ConnectResultInfo). The UI thread takes ownership and does all map bookkeeping.
- **Map ordering**: `g_audioPlaybackConnections` is a `std::unordered_map` (NOT sorted) — do not rely on iteration order; the tray tooltip simply shows the first entry. `g_availableDevices` is a `std::map` (sorted by device id).

### Known Issue — Coroutine Threading Model (FIXED)

**Fixed** in the "coroutine thread model refactor": `ConnectDevice` coroutines no longer touch any global state. The app runs in an MTA (`winrt::init_apartment()`), so a `fire_and_forget` coroutine resumes on a thread-pool thread after `co_await`. Both `ConnectDevice` overloads now only perform the async operations (`CreateFromIdAsync` / `TryCreateFromId` / `StartAsync` / `OpenAsync`), collect the outcome into a heap-allocated `ConnectResultInfo`, and post it as `WM_CONNECTRESULT`. All map bookkeeping (`g_audioPlaybackConnections`, `g_connectingDevices`, `g_autoReconnectingDevices`, `g_pendingReconnects`, `g_connectInProgress`), `SaveSettings()`, `UpdateNotifyIcon()`, retry scheduling and dialogs/notifications happen in `HandleConnectResult` (or `StartConnect` for the "connecting" entry state) on the UI thread.

Rules that keep it that way:
- Start a connect chain ONLY via `StartConnect` (UI thread) — it sets the connecting state and launches the coroutine. Never call `ConnectDevice` directly from a coroutine continuation.
- Never add global-state access to coroutine continuations. If a new async path needs to mutate state, post a message carrying a heap-allocated payload (as `WM_CONNECTRESULT`/`WM_DEVICECLOSED`/`WM_DEVICEADDED` do) and do the mutation on the UI thread.
- `HandleConnectResult` performs the duplicate-connection replacement (erase + `Close()` before replacing) that the coroutine used to do, and the failure-path cleanup; the `StateChanged` callback still only forwards `WM_DEVICECLOSED`.

### Global State (in BTAudio.h)

The app uses module-level globals, not a class. Connection maps are always manipulated from the UI thread:
- `g_audioPlaybackConnections` — active connections (deviceId → {DeviceInformation, AudioPlaybackConnection})
- `g_availableDevices` — discovered Bluetooth audio devices
- `g_connectingDevices` — devices in the process of connecting
- `g_pendingReconnects` — scheduled retry timers
- `g_connectQueue` / `g_connectInProgress` — serial startup reconnect state
- `g_pendingConnectOnAppear` — devices deferred until watcher reports them
- `g_deviceAliases` — user-defined display name overrides
