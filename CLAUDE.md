# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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
| `resource.h` | Version macros (current 1.1.5), icon resource ID |
| `BTAudio.rc` | Windows resources: icon, version info, SVG, i18n YMO |
| `BTAudio.svg` | Tray icon SVG source (recolored at runtime for state/theme) |
| `translate/generated/` | Compiled translation data files |

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
         ├── WM_DEVICECLOSED  → auto-reconnect on unexpected link drop
         ├── WM_NOTIFYICON    → tray click (show menu / toggle window)
         ├── WM_REFRESHDEVICELIST → rebuild connected/available device lists
         ├── WM_UPDATE*       → update notification dialogs
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

StateChanged(Closed) fires on an arbitrary thread. It posts `WM_DEVICECLOSED` to the UI thread, which calls `HandleDeviceClosed`. The decision to auto-reconnect is based on map membership: if the entry is still in `g_audioPlaybackConnections`, it's an unexpected link drop; if already removed, it was user-initiated.

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
- **Thread safety**: all global state mutations happen on the UI thread (message loop); the only cross-thread path is `StateChanged` → posts `WM_DEVICECLOSED` with a heap-allocated device id pointer
- **Map ordering**: `g_audioPlaybackConnections` is a `std::map<std::wstring, ...>` (sorted), so the tray tooltip and UI iterate in deterministic order; `g_availableDevices` shares the same key type for O(log n) lookups

### Global State (in BTAudio.h)

The app uses module-level globals, not a class. Connection maps are always manipulated from the UI thread:
- `g_audioPlaybackConnections` — active connections (deviceId → {DeviceInformation, AudioPlaybackConnection})
- `g_availableDevices` — discovered Bluetooth audio devices
- `g_connectingDevices` — devices in the process of connecting
- `g_pendingReconnects` — scheduled retry timers
- `g_connectQueue` / `g_connectInProgress` — serial startup reconnect state
- `g_pendingConnectOnAppear` — devices deferred until watcher reports them
- `g_deviceAliases` — user-defined display name overrides
