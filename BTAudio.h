#pragma once

#include "resource.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_REFRESHDEVICELIST = WM_APP + 3;
constexpr UINT WM_UPDATEAVAILABLE = WM_APP + 4;
constexpr UINT WM_UPTODATE = WM_APP + 5;
constexpr UINT WM_UPDATEFAILED = WM_APP + 6;
// Posted by the AudioPlaybackConnection.StateChanged callback to signal that a
// connection transitioned to Closed. The callback may fire on an arbitrary
// thread, so it only forwards a heap-allocated DeviceClosedInfo here and all
// map bookkeeping is performed on the UI thread. WPARAM owns a
// DeviceClosedInfo*.
constexpr UINT WM_DEVICECLOSED = WM_APP + 7;
// Posted by the DeviceWatcher.Added callback so the UI thread can reconcile
// the set of devices that were out-of-range at startup (g_pendingConnectOnAppear)
// against what the watcher has now found, and queue any newly-available ones
// for a serial startup reconnect.
constexpr UINT WM_DEVICEAPPEARED = WM_APP + 8;
// Posted to pump the next item out of g_connectQueue so startup reconnects are
// issued one at a time (serial) rather than firing concurrently and competing
// for the Bluetooth radio.
constexpr UINT WM_CONNECTNEXT = WM_APP + 9;
// Posted by the DeviceWatcher callbacks (which may fire on an arbitrary thread
// in a desktop MTA) so that ALL mutations of g_availableDevices /
// g_pendingConnectOnAppear happen on the UI thread. WPARAM owns a
// heap-allocated payload:
//   WM_DEVICEADDED   -> DeviceInformation*
//   WM_DEVICEUPDATED -> DeviceInformationUpdate*
//   WM_DEVICEREMOVED -> std::wstring* (device id)
constexpr UINT WM_DEVICEADDED = WM_APP + 10;
constexpr UINT WM_DEVICEUPDATED = WM_APP + 11;
constexpr UINT WM_DEVICEREMOVED = WM_APP + 12;
// Posted by a second instance (single-instance mutex already held) to ask the
// running instance to show its main window.
constexpr UINT WM_SHOWMAINWINDOW = WM_APP + 14;
// Posted by DownloadAndInstall once the update has been downloaded and the
// helper batch is waiting for this process to exit.
constexpr UINT WM_UPDATEINSTALLED = WM_APP + 16;
// Posted by the ConnectDevice coroutines (which run on a thread-pool thread
// after co_await in the MTA) to hand the outcome of a connect attempt chain
// back to the UI thread. The coroutine never touches global state — ALL map
// bookkeeping, retry scheduling, notifications and settings persistence
// happen in HandleConnectResult on the UI thread. WPARAM owns a
// heap-allocated ConnectResultInfo*.
constexpr UINT WM_CONNECTRESULT = WM_APP + 17;

// Initial delay before the startup reconnect. Kept short: devices that are not
// yet enumerated by the DeviceWatcher are recorded in g_pendingConnectOnAppear
// and connected lazily when WM_DEVICEAPPEARED reports them, so a fixed long
// delay is no longer needed just to "wait for the radio".
constexpr UINT_PTR IDT_STARTUP_DELAY = 1001;
constexpr UINT STARTUP_DELAY_MS = 1500;

// Retry budgets. Manual connections retry a couple of times with short
// backoff; automatic (link-drop) reconnects retry more aggressively over a
// longer window with exponential backoff so a briefly-out-of-range device has
// time to come back.
constexpr int MANUAL_RETRY_COUNT = 2;
constexpr int AUTO_RECONNECT_RETRY_COUNT = 8;

// Wall-clock timeout for an auto-reconnect cycle (unexpected link drop). Caps
// the entire retry chain so a permanently-unreachable device doesn't keep the
// tray icon in the amber "connecting" state indefinitely.
constexpr UINT AUTO_RECONNECT_TIMEOUT_MS = 120000; // 2 minutes

// Wall-clock timeout for devices that were out-of-range at startup. After this
// elapses, g_pendingConnectOnAppear is cleared and those devices are no longer
// auto-connected when they eventually appear.
constexpr UINT PENDING_APPEAR_TIMEOUT_MS = 300000; // 5 minutes
constexpr UINT_PTR IDT_PENDING_APPEAR_TIMEOUT = 1002;

// Battery level (percent) at or below which the user gets a tray notification.
constexpr int LOW_BATTERY_THRESHOLD = 20;

// Debounced timer that persists g_deviceVolumes after slider drags stop.
constexpr UINT_PTR IDT_VOLUME_SAVE = 1003;
constexpr UINT VOLUME_SAVE_DELAY_MS = 800;

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DeviceWatcher g_deviceWatcher = nullptr;
// Available (pairable) bluetooth audio devices discovered by DeviceWatcher.
// Keyed by device id so add/update/remove from the watcher is O(1).
std::map<std::wstring, DeviceInformation> g_availableDevices;
// Devices currently being connected. Key = device id, value = device name.
// Used to render a "Connecting..." status in the available list.
std::unordered_map<std::wstring, std::wstring> g_connectingDevices;
// id -> (DeviceInformation, AudioPlaybackConnection)
std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
// Pending reconnect attempts scheduled via SetTimer, keyed by timer id.
// The timer callback fires inside the message loop, so all map accesses stay
// on the UI thread (same thread that owns the connection maps above).
struct PendingReconnect
{
	std::wstring deviceId;
	int retryCount;              // remaining attempts after the scheduled one
	int attempt;                 // index of the scheduled attempt (0-based) — drives backoff
	bool isAutoReconnect;        // true => link-drop auto-reconnect (slower backoff, more tries)
	bool notifyNextOnComplete;   // true => PostMessage(WM_CONNECTNEXT) when this attempt chain ends
	ULONGLONG autoReconnectDeadline; // 0 = not an auto-reconnect (no deadline check)
};
std::map<UINT_PTR, PendingReconnect> g_pendingReconnects;
// Device ids from g_lastDevices that were out-of-range when the startup
// reconnect ran. When the DeviceWatcher later reports them (Added), they are
// promoted into g_connectQueue. This implements P2(六): don't waste the manual
// retry budget on devices that aren't currently reachable.
std::set<std::wstring> g_pendingConnectOnAppear;
// Device ids currently in an auto-reconnect cycle (unexpected link drop).
// Tracked separately so the UI can offer a "Cancel" button and the
// ConnectDevice coroutine can detect user-initiated cancellation.
std::set<std::wstring> g_autoReconnectingDevices;
// FIFO of device ids awaiting a serial startup connect. Pumped by
// WM_CONNECTNEXT; ConnectDevice(notifyNextOnComplete=true) posts WM_CONNECTNEXT
// once its attempt chain finishes (success or final failure), so connects are
// issued one at a time.
std::deque<std::wstring> g_connectQueue;
// True while a serial (startup) connect is in flight. Guards against
// WM_CONNECTNEXT / HandleDeviceAppeared starting a second concurrent connect
// when the queue was momentarily empty after a pop.
bool g_connectInProgress = false;
// Tray icons for different connection states. Each pair has a light-theme and
// dark-theme variant, rendered from the SVG resource with a state-specific colour.
HICON g_hIconLight = nullptr;          // normal — no device connected
HICON g_hIconDark = nullptr;
HICON g_hIconLightConnecting = nullptr; // connecting — amber
HICON g_hIconDarkConnecting = nullptr;
HICON g_hIconLightConnected = nullptr;  // connected  — green
HICON g_hIconDarkConnected = nullptr;

NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_reconnect = false;
bool g_autoStart = false;
// UI language preference: 0 = follow system, 1 = Simplified Chinese,
// 2 = Traditional Chinese, 3 = English. Applied on the next start (the
// translation table is loaded once at startup).
int g_language = 0;
// When enabled, wil diagnostics are appended to BTAudio.log next to the exe.
bool g_debugLogging = false;
std::vector<std::wstring> g_lastDevices;
// User-defined display aliases for devices (deviceId -> custom name).
// Does NOT affect system Bluetooth pairing names — only the BTAudio UI.
std::map<std::wstring, std::wstring> g_deviceAliases;
// Remembered per-device master volume (0..1), restored when the device
// connects. Keyed by device id.
std::map<std::wstring, float> g_deviceVolumes;
// While true, programmatic Slider updates must not re-trigger ValueChanged
// volume writes.
bool g_updatingVolume = false;
// Device ids for which the low-battery notification was already shown.
// Reset when the reported level climbs back above the threshold so the user
// can be reminded again on the next discharge cycle.
std::set<std::wstring> g_lowBatteryNotified;

// Main window
bool g_mainWindowVisible = false;
StackPanel g_mainDeviceListPanel = nullptr;       // connected devices
TextBlock g_mainNoDeviceText = nullptr;
StackPanel g_mainAvailableListPanel = nullptr;    // available devices
TextBlock g_mainNoAvailableText = nullptr;
Grid g_mainWindowRoot = nullptr;
// Cached references to the controls of one device card, so RefreshDeviceList
// can update text in place instead of rebuilding the whole list (preserves
// scroll position and avoids XAML churn).
struct DeviceCardRefs
{
	winrt::Windows::UI::Xaml::Controls::Border card;      // used for removal
	winrt::Windows::UI::Xaml::Controls::TextBlock name;   // device name
	winrt::Windows::UI::Xaml::Controls::TextBlock status; // status / battery (may be null)
	winrt::Windows::UI::Xaml::Controls::Slider volume;    // volume slider (connected cards only)
};
// id -> card refs for the connected list
std::unordered_map<std::wstring, DeviceCardRefs> g_connectedCards;
// id -> card refs for the available/connecting list
std::unordered_map<std::wstring, DeviceCardRefs> g_availableCards;

// Update checker
Flyout g_updateFlyout = nullptr;
// "Update downloaded" confirmation (restart now / defer to next exit)
Flyout g_updateInstalledFlyout = nullptr;

// Settings flyout (BTAudio program settings)
Flyout g_settingsFlyout = nullptr;
CheckBox g_settingsReconnectCheckbox = nullptr;
CheckBox g_settingsAutoStartCheckbox = nullptr;
ComboBox g_settingsLanguageCombo = nullptr;
CheckBox g_settingsDebugLoggingCheckbox = nullptr;

// Rename flyout (for setting local device aliases)
Flyout g_renameFlyout = nullptr;
TextBox g_renameTextBox = nullptr;
std::wstring g_renamingDeviceId;

// Forward declarations — must appear before the .hpp includes so that
// settings / utility helpers can call these functions.
bool IsAutoStartEnabled();
void SetAutoStart(bool enable);
void SetupRenameFlyout();
std::wstring GetDeviceDisplayName(const std::wstring& deviceId, std::wstring_view defaultName);
int GetDeviceBatteryLevel(const std::wstring& deviceId);
VOID CALLBACK StartupDelayTimerProc(HWND, UINT, UINT_PTR, DWORD);
VOID CALLBACK PendingAppearTimeoutTimerProc(HWND, UINT, UINT_PTR, DWORD);
// Cancel an in-progress auto-reconnect for a device: kills pending retry
// timers, clears the connecting/auto-reconnect tracking, and refreshes the UI.
void CancelReconnect(const std::wstring& deviceId);
// Compute the backoff delay (ms) before the next reconnect attempt.
// attempt is the 0-based index of the upcoming attempt. Auto-reconnect uses a
// gentler exponential curve with a higher cap than a manual connect retry.
UINT ComputeReconnectDelay(int attempt, bool isAutoReconnect);
// Handle a Closed notification that was posted from the StateChanged callback.
// Decides whether the close was a user-initiated teardown (ignored) or an
// unexpected link drop (erases the stale connection and kicks off auto-reconnect).
// connectionAbiPtr must be the ABI pointer of the connection that raised the
// event; if the connection currently in the map is a different instance, the
// notification belongs to a replaced connection and is ignored.
// Payload for WM_DEVICECLOSED. connectionAbiPtr identifies WHICH connection
// object raised the Closed event, so a stale Close() from a replaced
// connection is not mistaken for a link drop of the current one. It is used
// for identity comparison only and is never dereferenced.
struct DeviceClosedInfo
{
    std::wstring deviceId;
    void* connectionAbiPtr; // AudioPlaybackConnection ABI pointer (identity only)
};
void HandleDeviceClosed(const std::wstring& deviceId, void* connectionAbiPtr);
// Reconcile g_pendingConnectOnAppear against the devices the DeviceWatcher has
// now found: move any that have appeared into g_connectQueue and pump it.
void HandleDeviceAppeared();
// Payload for WM_CONNECTRESULT — the complete outcome of one connect attempt
// chain, produced by a ConnectDevice coroutine on a thread-pool thread. All
// global-state effects are applied by HandleConnectResult on the UI thread.
struct ConnectResultInfo
{
    std::wstring deviceId;
    std::wstring deviceName;        // real device name (for the connecting status)
    std::wstring errorMessage;      // localized failure reason; empty = stay silent
    DeviceInformation device = nullptr;       // set on success (owned by the UI thread)
    winrt::Windows::Media::Audio::AudioPlaybackConnection connection = nullptr; // set on success
    bool success = false;
    bool shouldRetry = false;       // transient failure (timeout / unknown)
    int retryCount = 0;             // remaining retries after this attempt
    int attempt = 0;                // 0-based index of the attempt that just ran
    bool isAutoReconnect = false;
    bool notifyNextOnComplete = false;
    ULONGLONG autoReconnectDeadline = 0;
};
// Apply the outcome of a connect attempt chain to the global state. Runs on
// the UI thread only (posted as WM_CONNECTRESULT): updates the connection
// maps, schedules retries via SetTimer/g_pendingReconnects, persists settings,
// refreshes the UI and surfaces notifications/dialogs.
void HandleConnectResult(ConnectResultInfo& info);
// UI-thread entry point that starts a connect attempt chain: marks the device
// as "connecting" (g_connectingDevices) and launches the ConnectDevice
// coroutine, which then only talks back via WM_CONNECTRESULT. Never call the
// coroutine directly from a coroutine continuation. Returns false (starting
// nothing) if a chain is already in flight for this device — callers that
// legitimately restart a chain (ReconnectTimerProc) erase the "connecting"
// entry first.
bool StartConnect(const std::wstring& deviceId, int retryCount = MANUAL_RETRY_COUNT, int attempt = 0, bool isAutoReconnect = false, bool notifyNextOnComplete = false, ULONGLONG autoReconnectDeadline = 0);
bool StartConnect(const DeviceInformation& device, int retryCount = MANUAL_RETRY_COUNT, int attempt = 0, bool isAutoReconnect = false, bool notifyNextOnComplete = false, ULONGLONG autoReconnectDeadline = 0);

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "UpdateChecker.hpp"

// Defined after UpdateChecker.hpp so ReleaseInfo is visible.
ReleaseInfo g_pendingRelease;

#include "Direct2DSvg.hpp"
