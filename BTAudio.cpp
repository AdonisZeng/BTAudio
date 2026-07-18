#include "pch.h"
#include "BTAudio.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void SetupSettingsFlyout();
winrt::fire_and_forget ConnectDevice(DeviceInformation device, int retryCount = MANUAL_RETRY_COUNT, int attempt = 0, bool isAutoReconnect = false, bool notifyNextOnComplete = false, ULONGLONG autoReconnectDeadline = 0);
winrt::fire_and_forget ConnectDevice(std::wstring deviceId, int retryCount = MANUAL_RETRY_COUNT, int attempt = 0, bool isAutoReconnect = false, bool notifyNextOnComplete = false, ULONGLONG autoReconnectDeadline = 0);
void SetupDeviceWatcher();
void SetupSvgIcon();
void UpdateNotifyIcon();
void ApplyTheme();
void SetupMainWindow();
void ShowMainWindow();
void HideMainWindow();
void RefreshDeviceList();
void ShowUpdateDialog();
void SetupRenameFlyout();
std::wstring GetDeviceDisplayName(const std::wstring& deviceId, std::wstring_view defaultName);
int GetDeviceBatteryLevel(const std::wstring& deviceId);
VOID CALLBACK StartupDelayTimerProc(HWND, UINT, UINT_PTR, DWORD);

// ---------------------------------------------------------------------------
// Auto-start via HKCU\...\Run — read / write the registry value "BTAudio".
// ---------------------------------------------------------------------------
bool IsAutoStartEnabled()
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
		0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		wchar_t path[MAX_PATH] = {};
		DWORD size = sizeof(path);
		if (RegQueryValueExW(hKey, L"BTAudio", nullptr, nullptr,
			reinterpret_cast<LPBYTE>(path), &size) == ERROR_SUCCESS)
		{
			RegCloseKey(hKey);
			auto exePath = GetModuleFsPath(g_hInst).wstring();
			return _wcsicmp(path, exePath.c_str()) == 0;
		}
		RegCloseKey(hKey);
	}
	return false;
}

void SetAutoStart(bool enable)
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
		0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
	{
		if (enable)
		{
			auto exePath = GetModuleFsPath(g_hInst).wstring();
			RegSetValueExW(hKey, L"BTAudio", 0, REG_SZ,
				reinterpret_cast<const BYTE*>(exePath.c_str()),
				static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t)));
		}
		else
		{
			RegDeleteValueW(hKey, L"BTAudio");
		}
		RegCloseKey(hKey);
	}
}

// ---------------------------------------------------------------------------
// Get the display name for a device — returns the user-defined alias if one
// is set, otherwise returns the device's real name.
// ---------------------------------------------------------------------------
std::wstring GetDeviceDisplayName(const std::wstring& deviceId, std::wstring_view defaultName)
{
	auto it = g_deviceAliases.find(deviceId);
	if (it != g_deviceAliases.end() && !it->second.empty())
		return it->second;
	return std::wstring(defaultName);
}

// ---------------------------------------------------------------------------
// Get the battery level (0-100) for a device from the DeviceWatcher's
// additional properties. Returns -1 if the battery level is unknown.
// Property key: DEVPKEY_Device_BatteryLevel {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2
// ---------------------------------------------------------------------------
int GetDeviceBatteryLevel(const std::wstring& deviceId)
{
	auto it = g_availableDevices.find(deviceId);
	if (it == g_availableDevices.end())
		return -1;

	try
	{
		auto props = it->second.Properties();
		auto val = props.Lookup(L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2");
		if (!val)
			return -1;
		// The property may be boxed as different integer types.
		try { return static_cast<int>(winrt::unbox_value<uint8_t>(val)); } catch (...) {}
		try { return static_cast<int>(winrt::unbox_value<uint32_t>(val)); } catch (...) {}
		try { return winrt::unbox_value<int32_t>(val); } catch (...) {}
	}
	catch (...) {}
	return -1;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	// Single-instance check — prevent multiple instances from running
	// simultaneously, which would create duplicate tray icons and conflict
	// over the same settings file / Bluetooth connections.
	HANDLE hSingleInstance = CreateMutexW(nullptr, TRUE, L"BTAudio_SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (hSingleInstance)
			CloseHandle(hSingleInstance);
		return EXIT_SUCCESS;
	}

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"BTAudio is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_BTAUDIO)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"BTAudio",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"BTAudio", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	// Immediately sync the auto-start setting with the registry so that the
	// Run key is always consistent with the persisted preference.
	SetAutoStart(g_autoStart);
	SetupFlyout();
	SetupMenu();
	SetupSettingsFlyout();
	SetupRenameFlyout();
	SetupDeviceWatcher();
	SetupSvgIcon();
	SetupMainWindow();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"BTAudio"));
	UpdateNotifyIcon();
	ApplyTheme();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	// Delay the initial reconnect to give Bluetooth services time to
	// initialize. This is especially important when auto-starting with
	// Windows, where the Bluetooth radio may not be ready at logon. Kept short:
	// devices not yet enumerated are deferred to g_pendingConnectOnAppear and
	// connected lazily via WM_DEVICEAPPEARED, so we no longer need a long fixed
	// delay just to wait for the radio to enumerate.
	SetTimer(g_hWnd, IDT_STARTUP_DELAY, STARTUP_DELAY_MS, StartupDelayTimerProc);

	// Silently check for updates on startup.
	CheckForUpdate(true);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		// Clean up all icon handles
		if (g_hIconLight)               DestroyIcon(g_hIconLight);
		if (g_hIconDark)                DestroyIcon(g_hIconDark);
		if (g_hIconLightConnecting)     DestroyIcon(g_hIconLightConnecting);
		if (g_hIconDarkConnecting)      DestroyIcon(g_hIconDarkConnecting);
		if (g_hIconLightConnected)      DestroyIcon(g_hIconLightConnected);
		if (g_hIconDarkConnected)       DestroyIcon(g_hIconDarkConnected);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
			ApplyTheme();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
			if (g_mainWindowVisible)
				HideMainWindow();
			else
				ShowMainWindow();
			break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			g_mainWindowVisible = false;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			// Partition the remembered devices: those already enumerated by the
			// watcher go straight into the serial connect queue; the rest are
			// recorded as pending and connected lazily when they appear
			// (WM_DEVICEAPPEARED). This avoids burning the manual retry budget on
			// devices that are currently switched off / out of range.
			for (const auto& i : g_lastDevices)
			{
				if (g_availableDevices.find(i) != g_availableDevices.end())
					g_connectQueue.push_back(i);
				else
					g_pendingConnectOnAppear.insert(i);
			}
			g_lastDevices.clear();
			// Start a timeout for devices pending on appear so we don't wait
			// forever for a device that never comes back in range.
			if (!g_pendingConnectOnAppear.empty())
				SetTimer(g_hWnd, IDT_PENDING_APPEAR_TIMEOUT, PENDING_APPEAR_TIMEOUT_MS, PendingAppearTimeoutTimerProc);
			// Start the serial pump. WM_CONNECTNEXT will issue one ConnectDevice
			// at a time and chain to the next when each finishes.
			PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
		}
		break;
	case WM_CONNECTNEXT:
		// Pump the serial connect queue, but only if no connect is currently
		// in flight — a connect in flight will re-post WM_CONNECTNEXT itself
		// when its attempt chain ends, so we never start two concurrently.
		if (!g_connectInProgress && !g_connectQueue.empty())
		{
			std::wstring deviceId = std::move(g_connectQueue.front());
			g_connectQueue.pop_front();
			g_connectInProgress = true;
			// notifyNextOnComplete => chain to the next queued device once this
			// connect (including its retries) settles, and clear the in-flight
			// flag so the pump can resume.
			ConnectDevice(std::move(deviceId), MANUAL_RETRY_COUNT, 0, false, true);
		}
		break;
	case WM_DEVICEAPPEARED:
		// A device was reported by the watcher; see if any of the pending
		// (out-of-range at startup) ids have now appeared and queue them.
		HandleDeviceAppeared();
		break;
	case WM_REFRESHDEVICELIST:
		RefreshDeviceList();
		break;
	case WM_DEVICECLOSED:
	{
		// StateChanged(Closed) was posted from a (possibly non-UI) thread. The
		// pointer is heap-allocated by the sender; take ownership and free it.
		auto* pDeviceId = reinterpret_cast<std::wstring*>(wParam);
		std::unique_ptr<std::wstring> guard(pDeviceId);
		if (pDeviceId)
			HandleDeviceClosed(*pDeviceId);
	}
	break;
	case WM_UPDATEAVAILABLE:
		ShowUpdateDialog();
		break;
	case WM_UPTODATE:
		TaskDialog(g_hWnd, nullptr, L"BTAudio", _(L"You are using the latest version."), nullptr, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
		break;
	case WM_UPDATEFAILED:
		TaskDialog(g_hWnd, nullptr, L"BTAudio", _(L"Failed to check for updates. Please check your network connection and try again later."), nullptr, TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupSettingsFlyout()
{
	using namespace winrt::Windows::UI::Text;

	TextBlock title;
	title.Text(_(L"Settings"));
	title.FontSize(16);
	title.FontWeight(FontWeights::SemiBold());
	title.Margin({ 0, 0, 0, 12 });

	g_settingsReconnectCheckbox = CheckBox();
	g_settingsReconnectCheckbox.IsChecked(g_reconnect);
	g_settingsReconnectCheckbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	g_settingsReconnectCheckbox.Margin({ 0, 0, 0, 12 });
	g_settingsReconnectCheckbox.Click([](const auto&, const auto&) {
		g_reconnect = g_settingsReconnectCheckbox.IsChecked().Value();
		SaveSettings();
	});

	g_settingsAutoStartCheckbox = CheckBox();
	g_settingsAutoStartCheckbox.IsChecked(g_autoStart);
	g_settingsAutoStartCheckbox.Content(winrt::box_value(_(L"Start with Windows")));
	g_settingsAutoStartCheckbox.Margin({ 0, 0, 0, 12 });
	g_settingsAutoStartCheckbox.Click([](const auto&, const auto&) {
		g_autoStart = g_settingsAutoStartCheckbox.IsChecked().Value();
		SetAutoStart(g_autoStart);
		SaveSettings();
	});

	Button closeButton;
	closeButton.Content(winrt::box_value(_(L"Done")));
	closeButton.HorizontalAlignment(HorizontalAlignment::Right);
	closeButton.Click([](const auto&, const auto&) {
		g_settingsFlyout.Hide();
	});

	StackPanel panel;
	panel.Margin({ 16, 16, 16, 16 });
	panel.Width(280);
	panel.Children().Append(title);
	panel.Children().Append(g_settingsReconnectCheckbox);
	panel.Children().Append(g_settingsAutoStartCheckbox);
	panel.Children().Append(closeButton);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(panel);

	g_settingsFlyout = flyout;
}

// ---------------------------------------------------------------------------
// Rename flyout — lets the user set a local alias for a device. The alias is
// shown in the UI everywhere the device name would appear, but does not
// affect the system Bluetooth pairing name.
// ---------------------------------------------------------------------------
void SetupRenameFlyout()
{
	using namespace winrt::Windows::UI::Text;

	TextBlock title;
	title.Text(_(L"Set Device Alias"));
	title.FontSize(14);
	title.FontWeight(FontWeights::SemiBold());
	title.Margin({ 0, 0, 0, 8 });

	g_renameTextBox = TextBox();
	g_renameTextBox.PlaceholderText(_(L"Enter custom name (leave empty to reset)"));
	g_renameTextBox.Margin({ 0, 0, 0, 8 });

	Button saveButton;
	saveButton.Content(winrt::box_value(_(L"Save")));
	saveButton.HorizontalAlignment(HorizontalAlignment::Right);
	saveButton.Click([](const auto&, const auto&) {
		auto text = g_renameTextBox.Text();
		if (text.empty())
			g_deviceAliases.erase(g_renamingDeviceId);
		else
			g_deviceAliases[g_renamingDeviceId] = std::wstring(text);
		SaveSettings();
		g_renameFlyout.Hide();
		PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
		UpdateNotifyIcon();
	});

	StackPanel panel;
	panel.Margin({ 12, 12, 12, 12 });
	panel.Width(260);
	panel.Children().Append(title);
	panel.Children().Append(g_renameTextBox);
	panel.Children().Append(saveButton);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(panel);

	g_renameFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon updateIcon;
	updateIcon.Glyph(L"\xE946"); // Refresh / Update icon

	MenuFlyoutItem checkUpdateItem;
	checkUpdateItem.Text(_(L"Check for Updates"));
	checkUpdateItem.Icon(updateIcon);
	checkUpdateItem.Click([](const auto&, const auto&) {
		CheckForUpdate(false);
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			g_mainWindowVisible = false;
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		g_mainWindowVisible = false;

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(checkUpdateItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.template as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

// One-shot timer that fires after a short delay to initiate the initial
// reconnect. This gives Bluetooth services time to initialize when the
// app is auto-started with Windows.
VOID CALLBACK StartupDelayTimerProc(HWND hwnd, UINT, UINT_PTR idEvent, DWORD)
{
	KillTimer(hwnd, idEvent);
	PostMessageW(hwnd, WM_CONNECTDEVICE, 0, 0);
}

// One-shot timer callback used to schedule a reconnect attempt on the UI thread
// after a backoff. SetTimer callbacks fire inside the message loop, so all
// accesses to the connection maps stay single-threaded.
VOID CALLBACK ReconnectTimerProc(HWND hwnd, UINT, UINT_PTR idEvent, DWORD)
{
	KillTimer(hwnd, idEvent);
	auto it = g_pendingReconnects.find(idEvent);
	if (it != g_pendingReconnects.end())
	{
		PendingReconnect pr = std::move(it->second);
		g_pendingReconnects.erase(it);
		// deviceId is moved into the coroutine frame (passed by value), so it is
		// safe across the suspension points inside ConnectDevice even though this
		// stack frame returns immediately afterwards.
		ConnectDevice(std::move(pr.deviceId), pr.retryCount, pr.attempt, pr.isAutoReconnect, pr.notifyNextOnComplete, pr.autoReconnectDeadline);
	}
}

// Compute the backoff delay (ms) before the next reconnect attempt.
// attempt is the 0-based index of the upcoming attempt. Auto-reconnect uses a
// gentler exponential curve with a higher cap than a manual connect retry.
UINT ComputeReconnectDelay(int attempt, bool isAutoReconnect)
{
	// Guard against negative / absurd inputs.
	const int idx = attempt < 0 ? 0 : attempt;

	if (isAutoReconnect)
	{
		// Link-drop auto-reconnect: 1s, 2s, 4s, 8s, 16s, 30s, 30s, 30s ...
		constexpr UINT baseDelay = 1000;
		constexpr UINT maxDelay = 30000;
		UINT delay = baseDelay;
		for (int i = 0; i < idx; ++i)
		{
			delay = (delay > maxDelay / 2) ? maxDelay : delay * 2;
		}
		return (std::min)(delay, maxDelay);
	}
	else
	{
		// Manual connect retry: 500ms, 1s, 2s, 4s ...
		constexpr UINT baseDelay = 500;
		constexpr UINT maxDelay = 4000;
		UINT delay = baseDelay;
		for (int i = 0; i < idx; ++i)
		{
			delay = (delay > maxDelay / 2) ? maxDelay : delay * 2;
		}
		return (std::min)(delay, maxDelay);
	}
}

winrt::fire_and_forget ConnectDevice(DeviceInformation device, int retryCount, int attempt, bool isAutoReconnect, bool notifyNextOnComplete, ULONGLONG autoReconnectDeadline)
{
	const std::wstring deviceId(device.Id());
	const std::wstring deviceName(device.Name());
	const std::wstring displayName(GetDeviceDisplayName(deviceId, deviceName));

	// Mark the device as "connecting" so the available list can show progress.
	// Store the real name; the UI applies the alias at display time.
	g_connectingDevices.emplace(deviceId, deviceName);
	PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);

	bool success = false;
	bool shouldRetry = false; // true for transient failures (timeout / unknown)
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			// Avoid duplicate connections to the same device. emplace won't overwrite
			// an existing key, which would create multiple AudioPlaybackConnection
			// objects competing for the same Bluetooth link and cause audio stutter.
			auto existing = g_audioPlaybackConnections.find(deviceId);
			if (existing != g_audioPlaybackConnections.end())
			{
				// Remove from the map BEFORE Close() so the StateChanged callback
				// (posted to the UI thread) observes the device is already gone and
				// treats this as a user-initiated teardown rather than a link drop.
				auto oldConnection = existing->second.second;
				g_audioPlaybackConnections.erase(existing);
				oldConnection.Close();
			}
			g_audioPlaybackConnections.emplace(deviceId, std::pair(device, connection));

			// The StateChanged event may be raised on an arbitrary thread. Do NOT
			// touch the global connection maps here — just hand the device id off
			// to the UI thread via WM_DEVICECLOSED, which performs all bookkeeping
			// (erase + optional auto-reconnect) safely.
			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto* pDeviceId = new std::wstring(sender.DeviceId());
					if (!PostMessageW(g_hWnd, WM_DEVICECLOSED,
						reinterpret_cast<WPARAM>(pDeviceId), 0))
					{
						delete pDeviceId; // queue full / window gone — free locally
					}
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				shouldRetry = true;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				// Treat as transient: format the extended HRESULT for diagnostics
				// without throwing, so the unified retry path below can handle it.
				success = false;
				shouldRetry = true;
				{
					auto hr = result.ExtendedError();
					errorMessage.resize(64);
					auto n = swprintf(errorMessage.data(), errorMessage.size(),
						L"%ls (0x%08X)", _(L"Unknown failure"),
						static_cast<uint32_t>(hr));
					if (n > 0)
						errorMessage.resize(n);
					else
						errorMessage = _(L"Unknown failure");
				}
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	// Connecting phase is over regardless of outcome.
	g_connectingDevices.erase(deviceId);

	if (!success)
	{
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			// Remove BEFORE Close() so the resulting StateChanged(Closed) does not
			// get mistaken for a link drop and trigger an unwanted auto-reconnect.
			auto conn = it->second.second;
			g_audioPlaybackConnections.erase(it);
			conn.Close();
		}

		// Retry transient failures (timeout / unknown) with exponential backoff
		// so the user does not have to manually toggle Bluetooth. The retry runs
		// back on the UI thread via a one-shot SetTimer, keeping map access
		// single-threaded. Auto-reconnect uses a gentler curve than manual.
		if (shouldRetry && retryCount > 0)
		{
			// Check if auto-reconnect was cancelled by the user while we were
			// awaiting the async operation.
			if (isAutoReconnect && g_autoReconnectingDevices.find(deviceId) == g_autoReconnectingDevices.end())
			{
				if (notifyNextOnComplete)
				{
					g_connectInProgress = false;
					PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
				}
				PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
				UpdateNotifyIcon();
				co_return;
			}
			// Check if the auto-reconnect wall-clock timeout has been exceeded.
			if (isAutoReconnect && autoReconnectDeadline > 0 && GetTickCount64() >= autoReconnectDeadline)
			{
				g_autoReconnectingDevices.erase(deviceId);
				if (notifyNextOnComplete)
				{
					g_connectInProgress = false;
					PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
				}
				PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
				UpdateNotifyIcon();
				co_return;
			}
			const int nextAttempt = attempt + 1;
			const UINT delay = ComputeReconnectDelay(nextAttempt, isAutoReconnect);
			// Keep showing the "Connecting" state while we wait out the backoff.
			g_connectingDevices.emplace(deviceId, deviceName);
			if (UINT_PTR timerId = SetTimer(g_hWnd, 0, delay, ReconnectTimerProc))
				g_pendingReconnects[timerId] = { deviceId, retryCount - 1, nextAttempt, isAutoReconnect, notifyNextOnComplete, autoReconnectDeadline };
			PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
			UpdateNotifyIcon();
			co_return;
		}
		// Auto-reconnect chain ended without success (exhausted retries or
		// non-retryable failure). Clear the tracking so the UI drops the
		// "Cancel" button.
		if (isAutoReconnect)
			g_autoReconnectingDevices.erase(deviceId);
	}
	else
	{
		// Connection succeeded — persist the updated device list immediately
		// (P1: incremental save) so a crash/forced-exit does not lose the
		// freshly-established connection.
		if (isAutoReconnect)
			g_autoReconnectingDevices.erase(deviceId);
		SaveSettings();
	}
	PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
	UpdateNotifyIcon();
	// Chain to the next queued startup connect (serial reconnect, P3). The
	// retry branch above returns early without reaching here, so this only
	// fires once the whole attempt chain (including retries) has settled.
	if (notifyNextOnComplete)
	{
		g_connectInProgress = false;
		PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
	}
}

winrt::fire_and_forget ConnectDevice(std::wstring deviceId, int retryCount, int attempt, bool isAutoReconnect, bool notifyNextOnComplete, ULONGLONG autoReconnectDeadline)
{
	// deviceId is owned by the coroutine frame (passed by value), so it remains
	// valid across the suspension point below.
	try
	{
		auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
		ConnectDevice(device, retryCount, attempt, isAutoReconnect, notifyNextOnComplete, autoReconnectDeadline);
	}
	catch (winrt::hresult_error const&)
	{
		// The device may have left range between scheduling the retry and firing
		// it; silently drop the retry and refresh the UI.
		LOG_CAUGHT_EXCEPTION();
		g_connectingDevices.erase(deviceId);
		if (isAutoReconnect)
			g_autoReconnectingDevices.erase(deviceId);
		PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
		UpdateNotifyIcon();
		// This attempt chain ends here; chain to the next queued connect so the
		// serial pump keeps moving even when a device vanishes mid-retry.
		if (notifyNextOnComplete)
		{
			g_connectInProgress = false;
			PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
		}
	}
}

// Process a Closed notification posted from the AudioPlaybackConnection
// StateChanged callback. Runs on the UI thread.
//
// Decision logic relies on map membership: every user-initiated teardown
// (disconnect button, reconnect button, duplicate-connection replacement,
// failure cleanup, app exit) removes the entry from g_audioPlaybackConnections
// BEFORE calling Close(). Therefore:
//   * entry still present  -> unexpected link drop -> auto-reconnect.
//   * entry already gone  -> user/system initiated -> ignore.
void HandleDeviceClosed(const std::wstring& deviceId)
{
	auto it = g_audioPlaybackConnections.find(deviceId);
	if (it == g_audioPlaybackConnections.end())
		return; // already handled by a user-initiated teardown path

	std::wstring deviceName(GetDeviceDisplayName(deviceId, it->second.first.Name()));
	// Erase the stale connection object first; the AutoReconnect below will
	// create a fresh one if it succeeds.
	g_audioPlaybackConnections.erase(it);
	g_connectingDevices.erase(deviceId);
	PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
	UpdateNotifyIcon();

	if (!deviceName.empty())
	{
		// Kick off an automatic reconnect with a larger retry budget and gentler
		// exponential backoff so a briefly-out-of-range / sleeping device gets a
		// chance to come back without user intervention. The wall-clock deadline
		// caps the entire chain so a permanently-unreachable device doesn't keep
		// the tray icon amber indefinitely.
		g_autoReconnectingDevices.insert(deviceId);
		ConnectDevice(deviceId, AUTO_RECONNECT_RETRY_COUNT, 0, true, false,
			GetTickCount64() + AUTO_RECONNECT_TIMEOUT_MS);
	}
}

// Reconcile g_pendingConnectOnAppear against the devices the watcher has now
// found. Anything that has appeared is promoted into the serial connect queue.
// Runs on the UI thread (posted as WM_DEVICEAPPEARED).
void HandleDeviceAppeared()
{
	if (g_pendingConnectOnAppear.empty())
		return;
	// Collect ids that have appeared, then remove them from the pending set.
	std::vector<std::wstring> ready;
	for (auto it = g_pendingConnectOnAppear.begin(); it != g_pendingConnectOnAppear.end(); )
	{
		if (g_availableDevices.find(*it) != g_availableDevices.end())
		{
			ready.push_back(*it);
			it = g_pendingConnectOnAppear.erase(it);
		}
		else
		{
			++it;
		}
	}
	if (ready.empty())
		return;
	for (auto& id : ready)
		g_connectQueue.push_back(std::move(id));
	// Only (re)start the pump if no connect is in flight; if one is, it will
	// post WM_CONNECTNEXT itself when it settles, which will then pick up the
	// newly-queued devices. This preserves strict serial ordering.
	if (!g_connectInProgress)
		PostMessageW(g_hWnd, WM_CONNECTNEXT, 0, 0);
}

// ---------------------------------------------------------------------------
// Cancel an in-progress auto-reconnect for a device. Kills any pending retry
// timer, clears the connecting / auto-reconnect tracking, and refreshes the
// UI so the device reappears in the available list with a Connect button.
//
// Note: if a ConnectDevice coroutine is currently awaiting StartAsync/OpenAsync,
// the cancellation is detected when that async call returns — the coroutine
// checks g_autoReconnectingDevices before scheduling the next retry.
// ---------------------------------------------------------------------------
void CancelReconnect(const std::wstring& deviceId)
{
	// Kill any pending reconnect timer for this device.
	for (auto it = g_pendingReconnects.begin(); it != g_pendingReconnects.end(); )
	{
		if (it->second.deviceId == deviceId)
		{
			KillTimer(g_hWnd, it->first);
			it = g_pendingReconnects.erase(it);
		}
		else
		{
			++it;
		}
	}
	g_connectingDevices.erase(deviceId);
	g_autoReconnectingDevices.erase(deviceId);
	PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
	UpdateNotifyIcon();
}

// One-shot timer that fires after PENDING_APPEAR_TIMEOUT_MS to give up waiting
// for devices that were out-of-range at startup. Clears g_pendingConnectOnAppear
// so those devices are no longer auto-connected when they eventually appear.
VOID CALLBACK PendingAppearTimeoutTimerProc(HWND hwnd, UINT, UINT_PTR idEvent, DWORD)
{
	KillTimer(hwnd, idEvent);
	if (!g_pendingConnectOnAppear.empty())
	{
		g_pendingConnectOnAppear.clear();
		PostMessageW(hwnd, WM_REFRESHDEVICELIST, 0, 0);
	}
}

void SetupDeviceWatcher()
{
	// Use the A2DP sink device selector so the watcher surfaces exactly the
	// devices that AudioPlaybackConnection can use.
	auto selector = AudioPlaybackConnection::GetDeviceSelector();

	// Request the battery-level property so we can display it in the UI.
	// DEVPKEY_Device_BatteryLevel = {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2
	auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>({
		L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2"
	});
	g_deviceWatcher = DeviceInformation::CreateWatcher(selector, additionalProperties);

	g_deviceWatcher.Added([](const auto&, const DeviceInformation& device) {
		std::wstring id(device.Id());
		g_availableDevices.emplace(id, device);
		PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
		// If this device was pending (out-of-range at startup), let the UI
		// thread promote it into the serial connect queue.
		if (!g_pendingConnectOnAppear.empty() && g_pendingConnectOnAppear.count(id))
			PostMessageW(g_hWnd, WM_DEVICEAPPEARED, 0, 0);
	});
	g_deviceWatcher.Updated([](const auto&, const DeviceInformationUpdate& update) {
		auto it = g_availableDevices.find(std::wstring(update.Id()));
		if (it != g_availableDevices.end())
		{
			it->second.Update(update);
			PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
		}
	});
	g_deviceWatcher.Removed([](const auto&, const DeviceInformationUpdate& update) {
		std::wstring id(update.Id());
		if (g_availableDevices.erase(id) > 0)
			PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
		// Stop waiting for a device that has left range entirely.
		g_pendingConnectOnAppear.erase(id);
	});

	g_deviceWatcher.Start();
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	// Normal state – black for light theme, white for dark
	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });

	// Connecting state – amber / yellow-orange
	g_hIconLightConnecting = SvgTohIcon(svg, width, height, { 0.85f, 0.55f, 0.0f, 1 });
	g_hIconDarkConnecting  = SvgTohIcon(svg, width, height, { 1.0f, 0.7f, 0.1f, 1 });

	// Connected state – green
	g_hIconLightConnected = SvgTohIcon(svg, width, height, { 0.1f, 0.65f, 0.2f, 1 });
	g_hIconDarkConnected  = SvgTohIcon(svg, width, height, { 0.3f, 0.85f, 0.3f, 1 });
}

bool IsSystemLightTheme()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	return value != 0;
}

void ApplyTheme()
{
	auto theme = IsSystemLightTheme() ? ElementTheme::Light : ElementTheme::Dark;

	g_xamlCanvas.RequestedTheme(theme);

	if (g_xamlFlyout)
	{
		if (auto content = g_xamlFlyout.Content().try_as<FrameworkElement>())
			content.RequestedTheme(theme);
	}

	if (g_settingsFlyout)
	{
		if (auto content = g_settingsFlyout.Content().try_as<FrameworkElement>())
			content.RequestedTheme(theme);
	}

	if (g_xamlMenu)
	{
		for (auto const& item : g_xamlMenu.Items())
		{
			if (auto fe = item.try_as<FrameworkElement>())
				fe.RequestedTheme(theme);
		}
	}

	if (g_mainWindowRoot)
		g_mainWindowRoot.RequestedTheme(theme);
}

void UpdateNotifyIcon()
{
	const bool isLight = IsSystemLightTheme();
	const bool hasConnection = !g_audioPlaybackConnections.empty();
	const bool isConnecting = !g_connectingDevices.empty();

	// Choose icon by priority: connected > connecting > normal
	if (hasConnection)
		g_nid.hIcon = isLight ? g_hIconLightConnected : g_hIconDarkConnected;
	else if (isConnecting)
		g_nid.hIcon = isLight ? g_hIconLightConnecting : g_hIconDarkConnecting;
	else
		g_nid.hIcon = isLight ? g_hIconLight : g_hIconDark;

	// Update tooltip with the first connected / connecting device name
	if (hasConnection)
	{
		auto connIt = g_audioPlaybackConnections.begin();
		auto name = GetDeviceDisplayName(connIt->first, connIt->second.first.Name());
		const size_t numDevices = g_audioPlaybackConnections.size();
		if (numDevices == 1)
			swprintf_s(g_nid.szTip, L"BTAudio - %s", name.c_str());
		else
			swprintf_s(g_nid.szTip, L"BTAudio - %s +%zu", name.c_str(), numDevices - 1);
	}
	else if (isConnecting)
	{
		auto connIt = g_connectingDevices.begin();
		auto name = GetDeviceDisplayName(connIt->first, connIt->second);
		swprintf_s(g_nid.szTip, L"BTAudio - %s", name.c_str());
	}
	else
	{
		wcscpy_s(g_nid.szTip, L"BTAudio");
	}

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

void SetupMainWindow()
{
	using namespace winrt::Windows::UI;
	using namespace winrt::Windows::UI::Text;
	using namespace winrt::Windows::UI::Xaml::Media;

	// Title bar
	TextBlock titleText;
	titleText.Text(L"BTAudio " BTAUDIO_VERSION_WSTR);
	titleText.FontSize(18);
	titleText.FontWeight(FontWeights::SemiBold());
	titleText.VerticalAlignment(VerticalAlignment::Center);
	titleText.Margin({ 12, 0, 0, 0 });

	Button closeButton;
	closeButton.Content(winrt::box_value(L"\xE8BB"));
	closeButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
	closeButton.FontSize(12);
	closeButton.Width(40);
	closeButton.Height(32);
	closeButton.Background(SolidColorBrush(Colors::Transparent()));
	closeButton.Foreground(SolidColorBrush(Colors::Gray()));
	closeButton.HorizontalAlignment(HorizontalAlignment::Right);
	closeButton.Click([](const auto&, const auto&) {
		HideMainWindow();
	});

	Grid titleBar;
	titleBar.Height(36);
	ColumnDefinition titleBarCol0;
	titleBarCol0.Width(GridLength(1, GridUnitType::Star));
	ColumnDefinition titleBarCol1;
	titleBarCol1.Width(GridLength(40, GridUnitType::Pixel));
	titleBar.ColumnDefinitions().Append(titleBarCol0);
	titleBar.ColumnDefinitions().Append(titleBarCol1);
	titleBar.SetValue(Canvas::ZIndexProperty(), winrt::box_value(1));

	titleBar.Children().Append(titleText);
	titleBar.Children().Append(closeButton);
	Grid::SetColumn(titleText, 0);
	Grid::SetColumn(closeButton, 1);

	// Separator
	Border titleSeparator;
	titleSeparator.Height(1);
	titleSeparator.Background(SolidColorBrush(Colors::LightGray()));

	// Device section header
	TextBlock deviceHeader;
	deviceHeader.Text(_(L"Connected Devices"));
	deviceHeader.FontSize(14);
	deviceHeader.FontWeight(FontWeights::SemiBold());
	deviceHeader.Margin({ 12, 12, 12, 8 });

	// Device list panel (inside ScrollViewer)
	g_mainDeviceListPanel = StackPanel();

	g_mainNoDeviceText = TextBlock();
	g_mainNoDeviceText.Text(_(L"No devices connected"));
	g_mainNoDeviceText.Foreground(SolidColorBrush(Colors::Gray()));
	g_mainNoDeviceText.HorizontalAlignment(HorizontalAlignment::Center);
	g_mainNoDeviceText.Margin({ 0, 20, 0, 20 });

	ScrollViewer scrollViewer;
	scrollViewer.Content(g_mainDeviceListPanel);
	scrollViewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);

	// Middle separator between connected and available sections
	Border middleSeparator;
	middleSeparator.Height(1);
	middleSeparator.Background(SolidColorBrush(Colors::LightGray()));
	middleSeparator.Margin({ 0, 8, 0, 0 });

	// Available device section header
	TextBlock availableHeader;
	availableHeader.Text(_(L"Available Devices"));
	availableHeader.FontSize(14);
	availableHeader.FontWeight(FontWeights::SemiBold());
	availableHeader.Margin({ 12, 8, 12, 8 });

	// Available device list panel (inside its own ScrollViewer)
	g_mainAvailableListPanel = StackPanel();

	g_mainNoAvailableText = TextBlock();
	g_mainNoAvailableText.Text(_(L"No devices available"));
	g_mainNoAvailableText.Foreground(SolidColorBrush(Colors::Gray()));
	g_mainNoAvailableText.HorizontalAlignment(HorizontalAlignment::Center);
	g_mainNoAvailableText.Margin({ 0, 12, 0, 12 });

	ScrollViewer availableScrollViewer;
	availableScrollViewer.Content(g_mainAvailableListPanel);
	availableScrollViewer.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);

	// Separator
	Border bottomSeparator;
	bottomSeparator.Height(1);
	bottomSeparator.Background(SolidColorBrush(Colors::LightGray()));
	bottomSeparator.Margin({ 0, 8, 0, 4 });

	// Bottom button bar: [Settings] [Bluetooth Settings] [Exit] in one row
	Button appSettingsButton;
	appSettingsButton.Content(winrt::box_value(L"\xE713"));
	appSettingsButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
	appSettingsButton.FontSize(14);
	appSettingsButton.Width(40);
	appSettingsButton.Height(32);
	appSettingsButton.Margin({ 0, 0, 0, 0 });
	appSettingsButton.HorizontalContentAlignment(HorizontalAlignment::Center);
	appSettingsButton.VerticalContentAlignment(VerticalAlignment::Center);
	appSettingsButton.Click([](const auto& sender, const auto&) {
		// Sync checkbox with current value before showing
		if (g_settingsReconnectCheckbox)
			g_settingsReconnectCheckbox.IsChecked(g_reconnect);
		if (g_settingsAutoStartCheckbox)
			g_settingsAutoStartCheckbox.IsChecked(g_autoStart);
		g_settingsFlyout.ShowAt(sender.template as<FrameworkElement>());
	});

	Button settingsButton;
	settingsButton.Content(winrt::box_value(_(L"Bluetooth Settings")));
	settingsButton.Height(32);
	settingsButton.Margin({ 0, 0, 0, 0 });
	settingsButton.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	// Button: Exit
	Button exitButton;
	exitButton.Content(winrt::box_value(_(L"Exit")));
	exitButton.MinWidth(72);
	exitButton.Height(32);
	exitButton.Margin({ 0, 0, 0, 0 });
	exitButton.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			HideMainWindow();
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		HideMainWindow();

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	// Bottom bar: evenly distributed horizontal buttons
	StackPanel bottomBar;
	bottomBar.Orientation(Orientation::Horizontal);
	bottomBar.HorizontalAlignment(HorizontalAlignment::Center);
	bottomBar.Padding({ 12, 4, 12, 8 });
	bottomBar.Spacing(8);
	bottomBar.Children().Append(appSettingsButton);
	bottomBar.Children().Append(settingsButton);
	bottomBar.Children().Append(exitButton);

	// Main layout (Grid so both device lists share the remaining vertical space)
	g_mainWindowRoot = Grid();
	RowDefinition rowTitle;
	rowTitle.Height(GridLength(36, GridUnitType::Pixel));
	RowDefinition rowSep1;
	rowSep1.Height(GridLength(1, GridUnitType::Pixel));
	RowDefinition rowConnHeader;
	rowConnHeader.Height(GridLength(1, GridUnitType::Auto));
	RowDefinition rowConnList;
	rowConnList.Height(GridLength(1, GridUnitType::Star));
	RowDefinition rowMidSep;
	rowMidSep.Height(GridLength(1, GridUnitType::Pixel));
	RowDefinition rowAvailHeader;
	rowAvailHeader.Height(GridLength(1, GridUnitType::Auto));
	RowDefinition rowAvailList;
	rowAvailList.Height(GridLength(1, GridUnitType::Star));
	RowDefinition rowBotSep;
	rowBotSep.Height(GridLength(1, GridUnitType::Pixel));
	RowDefinition rowBottomBar;
	rowBottomBar.Height(GridLength(1, GridUnitType::Auto));
	g_mainWindowRoot.RowDefinitions().Append(rowTitle);
	g_mainWindowRoot.RowDefinitions().Append(rowSep1);
	g_mainWindowRoot.RowDefinitions().Append(rowConnHeader);
	g_mainWindowRoot.RowDefinitions().Append(rowConnList);
	g_mainWindowRoot.RowDefinitions().Append(rowMidSep);
	g_mainWindowRoot.RowDefinitions().Append(rowAvailHeader);
	g_mainWindowRoot.RowDefinitions().Append(rowAvailList);
	g_mainWindowRoot.RowDefinitions().Append(rowBotSep);
	g_mainWindowRoot.RowDefinitions().Append(rowBottomBar);

	g_mainWindowRoot.Children().Append(titleBar);
	g_mainWindowRoot.Children().Append(titleSeparator);
	g_mainWindowRoot.Children().Append(deviceHeader);
	g_mainWindowRoot.Children().Append(scrollViewer);
	g_mainWindowRoot.Children().Append(middleSeparator);
	g_mainWindowRoot.Children().Append(availableHeader);
	g_mainWindowRoot.Children().Append(availableScrollViewer);
	g_mainWindowRoot.Children().Append(bottomSeparator);
	g_mainWindowRoot.Children().Append(bottomBar);

	Grid::SetRow(titleBar, 0);
	Grid::SetRow(titleSeparator, 1);
	Grid::SetRow(deviceHeader, 2);
	Grid::SetRow(scrollViewer, 3);
	Grid::SetRow(middleSeparator, 4);
	Grid::SetRow(availableHeader, 5);
	Grid::SetRow(availableScrollViewer, 6);
	Grid::SetRow(bottomSeparator, 7);
	Grid::SetRow(bottomBar, 8);

	g_xamlCanvas.Children().Append(g_mainWindowRoot);
}

void ShowMainWindow()
{
	const float windowWidth = 420.0f;
	const float windowHeight = 480.0f;
	auto dpi = GetDpiForWindow(g_hWnd);
	auto scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
	auto scaledWidth = static_cast<int>(windowWidth * scale);
	auto scaledHeight = static_cast<int>(windowHeight * scale);

	// Center on the primary work area
	RECT workArea;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
	int x = (workArea.left + workArea.right - scaledWidth) / 2;
	int y = (workArea.top + workArea.bottom - scaledHeight) / 2;

	// Ensure window is on screen
	if (x < workArea.left)
		x = workArea.left;
	if (y < workArea.top)
		y = workArea.top;
	if (x + scaledWidth > workArea.right)
		x = workArea.right - scaledWidth;
	if (y + scaledHeight > workArea.bottom)
		y = workArea.bottom - scaledHeight;

	RefreshDeviceList();

	g_xamlCanvas.Width(windowWidth);
	g_xamlCanvas.Height(windowHeight);
	if (g_mainWindowRoot)
	{
		g_mainWindowRoot.Width(windowWidth);
		g_mainWindowRoot.Height(windowHeight);
	}

	SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);
	SetWindowPos(g_hWnd, HWND_TOPMOST, x, y, scaledWidth, scaledHeight, SWP_SHOWWINDOW);
	SetWindowPos(g_hWndXaml, nullptr, 0, 0, scaledWidth, scaledHeight, SWP_NOZORDER | SWP_SHOWWINDOW);
	SetForegroundWindow(g_hWnd);

	g_mainWindowVisible = true;
}

void HideMainWindow()
{
	g_mainWindowVisible = false;
	SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	SetWindowPos(g_hWndXaml, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA);
	g_xamlCanvas.Width(0);
	g_xamlCanvas.Height(0);
	if (g_mainWindowRoot)
	{
		g_mainWindowRoot.Width(0);
		g_mainWindowRoot.Height(0);
	}
}

void RefreshDeviceList()
{
	using namespace winrt::Windows::UI;
	using namespace winrt::Windows::UI::Xaml::Media;

	// ---- Connected devices ----
	if (g_mainDeviceListPanel)
	{
		g_mainDeviceListPanel.Children().Clear();

		if (g_audioPlaybackConnections.empty())
		{
			g_mainDeviceListPanel.Children().Append(g_mainNoDeviceText);
		}
		else
		{
			for (const auto& [id, pair] : g_audioPlaybackConnections)
			{
				const auto& device = pair.first;

				TextBlock nameText;
				nameText.Text(GetDeviceDisplayName(id, device.Name()));
				nameText.FontSize(13);
				nameText.VerticalAlignment(VerticalAlignment::Center);
				nameText.TextTrimming(TextTrimming::CharacterEllipsis);

				// Build status text, appending battery level if available.
				std::wstring statusStr = _(L"Connected");
				int battery = GetDeviceBatteryLevel(id);
				if (battery >= 0)
					statusStr += L" | " + std::to_wstring(battery) + L"%";

				TextBlock statusText;
				statusText.Text(statusStr);
				statusText.FontSize(11);
				statusText.Foreground(SolidColorBrush(Colors::Green()));
				statusText.VerticalAlignment(VerticalAlignment::Center);
				statusText.Margin({ 8, 0, 0, 0 });

		Button renameButton;
		renameButton.Content(winrt::box_value(L"\xE70F")); // Edit icon
		renameButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
		renameButton.FontSize(10);
		renameButton.Padding(ThicknessHelper::FromUniformLength(0));
		renameButton.MinWidth(28);
		renameButton.MinHeight(28);
		renameButton.Width(28);
		renameButton.Height(28);
		renameButton.Margin({ 4, 0, 0, 0 });
		renameButton.HorizontalContentAlignment(HorizontalAlignment::Center);
		renameButton.VerticalContentAlignment(VerticalAlignment::Center);
		renameButton.Background(SolidColorBrush(Colors::Transparent()));
		renameButton.Foreground(SolidColorBrush(Colors::Gray()));
		renameButton.VerticalAlignment(VerticalAlignment::Center);
		ToolTipService::SetToolTip(renameButton, winrt::box_value(_(L"Rename")));

		Button reconnectButton;
		reconnectButton.Content(winrt::box_value(L"\xE72C")); // Refresh icon
			reconnectButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
			reconnectButton.FontSize(10);
			reconnectButton.Padding(ThicknessHelper::FromUniformLength(0));
			reconnectButton.MinWidth(28);
			reconnectButton.MinHeight(28);
			reconnectButton.Width(28);
			reconnectButton.Height(28);
			reconnectButton.Margin({ 4, 0, 0, 0 });
			reconnectButton.HorizontalContentAlignment(HorizontalAlignment::Center);
			reconnectButton.VerticalContentAlignment(VerticalAlignment::Center);
			reconnectButton.Background(SolidColorBrush(Colors::Transparent()));
			reconnectButton.Foreground(SolidColorBrush(Colors::Gray()));
			reconnectButton.VerticalAlignment(VerticalAlignment::Center);
			ToolTipService::SetToolTip(reconnectButton, winrt::box_value(_(L"Reconnect")));

			Button disconnectButton;
			disconnectButton.Content(winrt::box_value(L"\xE8BB"));
			disconnectButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
			disconnectButton.FontSize(10);
			disconnectButton.Padding(ThicknessHelper::FromUniformLength(0));
			disconnectButton.MinWidth(28);
			disconnectButton.MinHeight(28);
			disconnectButton.Width(28);
			disconnectButton.Height(28);
			disconnectButton.Margin({ 4, 0, 0, 0 });
			disconnectButton.HorizontalContentAlignment(HorizontalAlignment::Center);
			disconnectButton.VerticalContentAlignment(VerticalAlignment::Center);
			disconnectButton.Background(SolidColorBrush(Colors::Transparent()));
			disconnectButton.Foreground(SolidColorBrush(Colors::Gray()));
			disconnectButton.VerticalAlignment(VerticalAlignment::Center);
			ToolTipService::SetToolTip(disconnectButton, winrt::box_value(_(L"Disconnect")));

		std::wstring deviceId(id);
		renameButton.Click([deviceId](const auto& sender, const auto&) {
			g_renamingDeviceId = deviceId;
			auto aliasIt = g_deviceAliases.find(deviceId);
			if (aliasIt != g_deviceAliases.end())
				g_renameTextBox.Text(aliasIt->second);
			else
				g_renameTextBox.Text(L"");
			g_renameFlyout.ShowAt(sender.template as<FrameworkElement>());
		});
	reconnectButton.Click([deviceId](const auto&, const auto&) {
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it != g_audioPlaybackConnections.end())
			{
				// Capture the DeviceInformation before tearing down the current
				// connection, then re-initiate with a fresh retry budget. This
				// forces the Bluetooth stack to re-negotiate AVDTP/L2CAP, which
				// is the most reliable way to break a "connected-but-silent"
				// deadlock without asking the user to toggle Bluetooth.
				DeviceInformation device = it->second.first;
				auto connection = it->second.second;
				// Remove from the map BEFORE Close() so the StateChanged callback
				// does not treat this as a link drop and start an auto-reconnect.
				g_audioPlaybackConnections.erase(it);
				connection.Close();
				PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
				UpdateNotifyIcon();
				ConnectDevice(device, MANUAL_RETRY_COUNT);
			}
		});
		disconnectButton.Click([deviceId](const auto&, const auto&) {
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it != g_audioPlaybackConnections.end())
			{
			std::wstring deviceName(GetDeviceDisplayName(deviceId, it->second.first.Name()));
			auto connection = it->second.second;
			// Remove from the map BEFORE Close() so this user-initiated
			// disconnect is not mistaken for a link drop and auto-reconnected.
			g_audioPlaybackConnections.erase(it);
			connection.Close();
			PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
				UpdateNotifyIcon();
			// Persist the reduced device list so a crashed/forced exit does not
			// reconnect a device the user explicitly disconnected.
			SaveSettings();
			}
		});

		Grid deviceGrid;
		ColumnDefinition deviceCol0;
		deviceCol0.Width(GridLength(1, GridUnitType::Star));
		ColumnDefinition deviceCol1;
		deviceCol1.Width(GridLength(1, GridUnitType::Auto));
		ColumnDefinition deviceCol2;
		deviceCol2.Width(GridLength(1, GridUnitType::Auto));
		ColumnDefinition deviceCol3;
		deviceCol3.Width(GridLength(1, GridUnitType::Auto));
		ColumnDefinition deviceCol4;
		deviceCol4.Width(GridLength(1, GridUnitType::Auto));
		deviceGrid.ColumnDefinitions().Append(deviceCol0);
		deviceGrid.ColumnDefinitions().Append(deviceCol1);
		deviceGrid.ColumnDefinitions().Append(deviceCol2);
		deviceGrid.ColumnDefinitions().Append(deviceCol3);
		deviceGrid.ColumnDefinitions().Append(deviceCol4);
		deviceGrid.Children().Append(nameText);
		deviceGrid.Children().Append(statusText);
		deviceGrid.Children().Append(renameButton);
		deviceGrid.Children().Append(reconnectButton);
		deviceGrid.Children().Append(disconnectButton);
		Grid::SetColumn(nameText, 0);
		Grid::SetColumn(statusText, 1);
		Grid::SetColumn(renameButton, 2);
		Grid::SetColumn(reconnectButton, 3);
		Grid::SetColumn(disconnectButton, 4);

				Border deviceCard;
				deviceCard.Child(deviceGrid);
				deviceCard.Margin({ 12, 2, 12, 2 });
				deviceCard.Padding({ 8, 8, 8, 8 });
				deviceCard.CornerRadius(CornerRadius(4));
				deviceCard.BorderThickness(Thickness(1));
				deviceCard.BorderBrush(SolidColorBrush(Colors::LightGray()));

				g_mainDeviceListPanel.Children().Append(deviceCard);
			}
		}
	}

	// ---- Available devices ----
	if (g_mainAvailableListPanel)
	{
		g_mainAvailableListPanel.Children().Clear();

		// Only show devices that are neither already connected nor currently connecting.
		bool any = false;
		for (const auto& [id, device] : g_availableDevices)
		{
			if (g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end())
				continue;
			if (g_connectingDevices.find(id) != g_connectingDevices.end())
				continue;

			any = true;

			TextBlock nameText;
			nameText.Text(GetDeviceDisplayName(id, device.Name()));
			nameText.FontSize(13);
			nameText.VerticalAlignment(VerticalAlignment::Center);
			nameText.TextTrimming(TextTrimming::CharacterEllipsis);

			Button renameButton;
			renameButton.Content(winrt::box_value(L"\xE70F")); // Edit icon
			renameButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
			renameButton.FontSize(10);
			renameButton.Padding(ThicknessHelper::FromUniformLength(0));
			renameButton.MinWidth(28);
			renameButton.MinHeight(28);
			renameButton.Width(28);
			renameButton.Height(28);
			renameButton.Margin({ 4, 0, 0, 0 });
			renameButton.HorizontalContentAlignment(HorizontalAlignment::Center);
			renameButton.VerticalContentAlignment(VerticalAlignment::Center);
			renameButton.Background(SolidColorBrush(Colors::Transparent()));
			renameButton.Foreground(SolidColorBrush(Colors::Gray()));
			renameButton.VerticalAlignment(VerticalAlignment::Center);
			ToolTipService::SetToolTip(renameButton, winrt::box_value(_(L"Rename")));

			Button connectButton;
			connectButton.Content(winrt::box_value(_(L"Connect")));
			connectButton.FontSize(11);
			connectButton.Width(72);
			connectButton.Height(28);
			connectButton.HorizontalAlignment(HorizontalAlignment::Right);
			connectButton.VerticalAlignment(VerticalAlignment::Center);

			std::wstring deviceId(id);
			renameButton.Click([deviceId](const auto& sender, const auto&) {
				g_renamingDeviceId = deviceId;
				auto aliasIt = g_deviceAliases.find(deviceId);
				if (aliasIt != g_deviceAliases.end())
					g_renameTextBox.Text(aliasIt->second);
				else
					g_renameTextBox.Text(L"");
				g_renameFlyout.ShowAt(sender.template as<FrameworkElement>());
			});
			connectButton.Click([deviceId](const auto&, const auto&) {
				ConnectDevice(deviceId);
			});

			Grid availGrid;
			ColumnDefinition availCol0;
			availCol0.Width(GridLength(1, GridUnitType::Star));
			ColumnDefinition availCol1;
			availCol1.Width(GridLength(1, GridUnitType::Auto));
			ColumnDefinition availCol2;
			availCol2.Width(GridLength(1, GridUnitType::Auto));
			availGrid.ColumnDefinitions().Append(availCol0);
			availGrid.ColumnDefinitions().Append(availCol1);
			availGrid.ColumnDefinitions().Append(availCol2);
			availGrid.Children().Append(nameText);
			availGrid.Children().Append(renameButton);
			availGrid.Children().Append(connectButton);
			Grid::SetColumn(nameText, 0);
			Grid::SetColumn(renameButton, 1);
			Grid::SetColumn(connectButton, 2);

			Border availCard;
			availCard.Child(availGrid);
			availCard.Margin({ 12, 2, 12, 2 });
			availCard.Padding({ 8, 8, 8, 8 });
			availCard.CornerRadius(CornerRadius(4));
			availCard.BorderThickness(Thickness(1));
			availCard.BorderBrush(SolidColorBrush(Colors::LightGray()));

			g_mainAvailableListPanel.Children().Append(availCard);
		}

		// Devices currently connecting are shown with a "Connecting" status.
		// Auto-reconnecting devices also get a "Cancel" button so the user can
		// abort the retry chain and manually reconnect.
		for (const auto& [id, name] : g_connectingDevices)
		{
			any = true;
			const bool isAutoReconnect = g_autoReconnectingDevices.find(id) != g_autoReconnectingDevices.end();

			TextBlock nameText;
			nameText.Text(GetDeviceDisplayName(id, name));
			nameText.FontSize(13);
			nameText.VerticalAlignment(VerticalAlignment::Center);
			nameText.TextTrimming(TextTrimming::CharacterEllipsis);

			TextBlock statusText;
			statusText.Text(_(L"Connecting"));
			statusText.FontSize(11);
			statusText.Foreground(SolidColorBrush(Colors::Gray()));
			statusText.VerticalAlignment(VerticalAlignment::Center);
			statusText.Margin({ 8, 0, 0, 0 });

			Grid availGrid;
			ColumnDefinition availCol0;
			availCol0.Width(GridLength(1, GridUnitType::Star));
			ColumnDefinition availCol1;
			availCol1.Width(GridLength(1, GridUnitType::Auto));
			ColumnDefinition availCol2;
			availCol2.Width(GridLength(1, GridUnitType::Auto));
			availGrid.ColumnDefinitions().Append(availCol0);
			availGrid.ColumnDefinitions().Append(availCol1);
			availGrid.ColumnDefinitions().Append(availCol2);
			availGrid.Children().Append(nameText);
			availGrid.Children().Append(statusText);
			Grid::SetColumn(nameText, 0);
			Grid::SetColumn(statusText, 1);

			if (isAutoReconnect)
			{
				Button cancelButton;
				cancelButton.Content(winrt::box_value(_(L"Cancel")));
				cancelButton.FontSize(11);
				cancelButton.Width(72);
				cancelButton.Height(28);
				cancelButton.HorizontalAlignment(HorizontalAlignment::Right);
				cancelButton.VerticalAlignment(VerticalAlignment::Center);

				std::wstring deviceId(id);
				cancelButton.Click([deviceId](const auto&, const auto&) {
					CancelReconnect(deviceId);
				});

				availGrid.Children().Append(cancelButton);
				Grid::SetColumn(cancelButton, 2);
			}
			else
			{
				ProgressRing ring;
				ring.IsActive(true);
				ring.Width(20);
				ring.Height(20);
				ring.HorizontalAlignment(HorizontalAlignment::Right);
				ring.VerticalAlignment(VerticalAlignment::Center);

				availGrid.Children().Append(ring);
				Grid::SetColumn(ring, 2);
			}

			Border availCard;
			availCard.Child(availGrid);
			availCard.Margin({ 12, 2, 12, 2 });
			availCard.Padding({ 8, 8, 8, 8 });
			availCard.CornerRadius(CornerRadius(4));
			availCard.BorderThickness(Thickness(1));
			availCard.BorderBrush(SolidColorBrush(Colors::LightGray()));

			g_mainAvailableListPanel.Children().Append(availCard);
		}

		if (!any)
		{
			g_mainAvailableListPanel.Children().Append(g_mainNoAvailableText);
		}
	}

	// Keep the tray icon in sync with the current connection state.
	UpdateNotifyIcon();
}

void ShowUpdateDialog()
{
	using namespace winrt::Windows::UI;
	using namespace winrt::Windows::UI::Text;
	using namespace winrt::Windows::UI::Xaml::Media;

	StackPanel panel;
	panel.Margin({ 16, 16, 16, 16 });
	panel.MaxWidth(340);
	panel.Spacing(8);

	TextBlock title;
	title.Text(_(L"A new version is available"));
	title.FontSize(16);
	title.FontWeight(FontWeights::SemiBold());
	panel.Children().Append(title);

	// Version line: "Version 1.2.0  (Current: 1.1)"
	TextBlock versionLine;
	versionLine.Text(std::wstring(_(L"Version")) + L" " + NormalizeVersionTag(g_pendingRelease.tagName) +
		L"  (" + _(L"Current") + L": " + BTAUDIO_VERSION_WSTR + L")");
	versionLine.FontSize(12);
	versionLine.Foreground(SolidColorBrush(Colors::Gray()));
	panel.Children().Append(versionLine);

	// Release notes
	if (!g_pendingRelease.body.empty())
	{
		TextBlock notesLabel;
		notesLabel.Text(_(L"What's new:"));
		notesLabel.FontSize(12);
		notesLabel.FontWeight(FontWeights::SemiBold());
		notesLabel.Margin({ 0, 8, 0, 0 });
		panel.Children().Append(notesLabel);

		TextBlock notes;
		notes.Text(g_pendingRelease.body);
		notes.FontSize(11);
		notes.TextWrapping(TextWrapping::Wrap);
		notes.MaxHeight(160);
		panel.Children().Append(notes);
	}

	// Buttons
	StackPanel buttonRow;
	buttonRow.Orientation(Orientation::Horizontal);
	buttonRow.HorizontalAlignment(HorizontalAlignment::Right);
	buttonRow.Spacing(8);
	buttonRow.Margin({ 0, 12, 0, 0 });

	Button laterBtn;
	laterBtn.Content(winrt::box_value(_(L"Later")));
	laterBtn.Click([](const auto&, const auto&) {
		g_updateFlyout.Hide();
	});
	buttonRow.Children().Append(laterBtn);

	if (!g_pendingRelease.downloadUrl.empty())
	{
		Button updateBtn;
		updateBtn.Content(winrt::box_value(_(L"Update Now")));
		updateBtn.Click([](const auto&, const auto&) {
			g_updateFlyout.Hide();
			DownloadAndInstall(g_pendingRelease);
		});
		buttonRow.Children().Append(updateBtn);
	}
	else
	{
		// No matching architecture asset — fall back to browser.
		Button openBtn;
		openBtn.Content(winrt::box_value(_(L"Open Download Page")));
		auto htmlUrl = g_pendingRelease.htmlUrl;
		openBtn.Click([htmlUrl](const auto&, const auto&) {
			winrt::Windows::System::Launcher::LaunchUriAsync(Uri(htmlUrl));
			g_updateFlyout.Hide();
		});
		buttonRow.Children().Append(openBtn);
	}

	panel.Children().Append(buttonRow);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(panel);
	g_updateFlyout = flyout;

	// Show near the tray icon (same pattern as the exit-confirmation flyout).
	RECT iconRect;
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr))
	{
		LOG_HR(hr);
		return;
	}

	auto dpi = GetDpiForWindow(g_hWnd);
	g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
	g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

	SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
	g_updateFlyout.ShowAt(g_xamlCanvas);
}

