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
	int retryCount;
};
std::map<UINT_PTR, PendingReconnect> g_pendingReconnects;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
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
std::vector<std::wstring> g_lastDevices;

// Main window
bool g_mainWindowVisible = false;
StackPanel g_mainDeviceListPanel = nullptr;       // connected devices
TextBlock g_mainNoDeviceText = nullptr;
StackPanel g_mainAvailableListPanel = nullptr;    // available devices
TextBlock g_mainNoAvailableText = nullptr;
Grid g_mainWindowRoot = nullptr;

// Update checker
Flyout g_updateFlyout = nullptr;

// Settings flyout (BTAudio program settings)
Flyout g_settingsFlyout = nullptr;
CheckBox g_settingsReconnectCheckbox = nullptr;

#include "Util.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "UpdateChecker.hpp"

// Defined after UpdateChecker.hpp so ReleaseInfo is visible.
ReleaseInfo g_pendingRelease;

#include "Direct2DSvg.hpp"
