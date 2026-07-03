#include "pch.h"
#include "BTAudio.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
void ApplyTheme();
void SetupMainWindow();
void ShowMainWindow();
void HideMainWindow();
void RefreshDeviceList();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

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
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();
	SetupMainWindow();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"BTAudio"));
	UpdateNotifyIcon();
	ApplyTheme();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

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
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
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
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	case WM_REFRESHDEVICELIST:
		RefreshDeviceList();
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

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
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

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			// Avoid duplicate connections to the same device. emplace won't overwrite
			// an existing key, which would create multiple AudioPlaybackConnection
			// objects competing for the same Bluetooth link and cause audio stutter.
			auto existing = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (existing != g_audioPlaybackConnections.end())
			{
				existing->second.second.Close();
				g_audioPlaybackConnections.erase(existing);
			}
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto it = g_audioPlaybackConnections.find(std::wstring(sender.DeviceId()));
					if (it != g_audioPlaybackConnections.end())
					{
						g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						g_audioPlaybackConnections.erase(it);
					}
					PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
					// Do not call sender.Close() here. The connection is already in the
					// Closed state and the map entry has been erased, so calling Close()
					// again is redundant and risks reentrancy within the event callback.
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
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
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

	if (success)
	{
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
	PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		if (g_mainWindowVisible)
			ShowMainWindow();
		else
			SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
		PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
	});
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

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
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
	g_nid.hIcon = IsSystemLightTheme() ? g_hIconLight : g_hIconDark;

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
	titleText.Text(L"BTAudio");
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

	// Button: Connect New Device
	Button connectButton;
	connectButton.Content(winrt::box_value(_(L"Connect New Device")));
	connectButton.Margin({ 12, 8, 12, 4 });
	connectButton.HorizontalAlignment(HorizontalAlignment::Stretch);
	connectButton.Click([](const auto&, const auto&) {
		using namespace winrt::Windows::UI::Popups;

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);
		Rect rect = {
			static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
			static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
		};

		// Hide window without changing g_mainWindowVisible so it re-shows after picker dismisses
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
		SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA);
		SetForegroundWindow(g_hWnd);
		g_devicePicker.Show(rect, Placement::Above);
	});

	// Separator
	Border bottomSeparator;
	bottomSeparator.Height(1);
	bottomSeparator.Background(SolidColorBrush(Colors::LightGray()));
	bottomSeparator.Margin({ 0, 8, 0, 4 });

	// Button: Bluetooth Settings
	Button settingsButton;
	settingsButton.Content(winrt::box_value(_(L"Bluetooth Settings")));
	settingsButton.Margin({ 12, 4, 12, 2 });
	settingsButton.HorizontalAlignment(HorizontalAlignment::Stretch);
	settingsButton.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	// Button: Exit
	Button exitButton;
	exitButton.Content(winrt::box_value(_(L"Exit")));
	exitButton.Margin({ 12, 2, 12, 8 });
	exitButton.HorizontalAlignment(HorizontalAlignment::Stretch);
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

	// Main layout (Grid so the device list fills remaining space)
	g_mainWindowRoot = Grid();
	RowDefinition rowTitle;
	rowTitle.Height(GridLength(36, GridUnitType::Pixel));
	RowDefinition rowSep1;
	rowSep1.Height(GridLength(1, GridUnitType::Pixel));
	RowDefinition rowHeader;
	rowHeader.Height(GridLength(1, GridUnitType::Auto));
	RowDefinition rowList;
	rowList.Height(GridLength(1, GridUnitType::Star));
	RowDefinition rowConnect;
	rowConnect.Height(GridLength(1, GridUnitType::Auto));
	RowDefinition rowSep2;
	rowSep2.Height(GridLength(1, GridUnitType::Pixel));
	RowDefinition rowSettings;
	rowSettings.Height(GridLength(1, GridUnitType::Auto));
	RowDefinition rowExit;
	rowExit.Height(GridLength(1, GridUnitType::Auto));
	g_mainWindowRoot.RowDefinitions().Append(rowTitle);
	g_mainWindowRoot.RowDefinitions().Append(rowSep1);
	g_mainWindowRoot.RowDefinitions().Append(rowHeader);
	g_mainWindowRoot.RowDefinitions().Append(rowList);
	g_mainWindowRoot.RowDefinitions().Append(rowConnect);
	g_mainWindowRoot.RowDefinitions().Append(rowSep2);
	g_mainWindowRoot.RowDefinitions().Append(rowSettings);
	g_mainWindowRoot.RowDefinitions().Append(rowExit);

	g_mainWindowRoot.Children().Append(titleBar);
	g_mainWindowRoot.Children().Append(titleSeparator);
	g_mainWindowRoot.Children().Append(deviceHeader);
	g_mainWindowRoot.Children().Append(scrollViewer);
	g_mainWindowRoot.Children().Append(connectButton);
	g_mainWindowRoot.Children().Append(bottomSeparator);
	g_mainWindowRoot.Children().Append(settingsButton);
	g_mainWindowRoot.Children().Append(exitButton);

	Grid::SetRow(titleBar, 0);
	Grid::SetRow(titleSeparator, 1);
	Grid::SetRow(deviceHeader, 2);
	Grid::SetRow(scrollViewer, 3);
	Grid::SetRow(connectButton, 4);
	Grid::SetRow(bottomSeparator, 5);
	Grid::SetRow(settingsButton, 6);
	Grid::SetRow(exitButton, 7);

	g_xamlCanvas.Children().Append(g_mainWindowRoot);
}

void ShowMainWindow()
{
	const float windowWidth = 380.0f;
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
	if (!g_mainDeviceListPanel)
		return;

	g_mainDeviceListPanel.Children().Clear();

	if (g_audioPlaybackConnections.empty())
	{
		g_mainDeviceListPanel.Children().Append(g_mainNoDeviceText);
		return;
	}

	using namespace winrt::Windows::UI;
	using namespace winrt::Windows::UI::Xaml::Media;

	for (const auto& [id, pair] : g_audioPlaybackConnections)
	{
		const auto& device = pair.first;

		TextBlock nameText;
		nameText.Text(device.Name());
		nameText.FontSize(13);
		nameText.VerticalAlignment(VerticalAlignment::Center);
		nameText.TextTrimming(TextTrimming::CharacterEllipsis);

		TextBlock statusText;
		statusText.Text(_(L"Connected"));
		statusText.FontSize(11);
		statusText.Foreground(SolidColorBrush(Colors::Green()));
		statusText.VerticalAlignment(VerticalAlignment::Center);
		statusText.Margin({ 8, 0, 0, 0 });

		Button disconnectButton;
		disconnectButton.Content(winrt::box_value(L"\xE8BB"));
		disconnectButton.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
		disconnectButton.FontSize(10);
		disconnectButton.Width(28);
		disconnectButton.Height(28);
		disconnectButton.Background(SolidColorBrush(Colors::Transparent()));
		disconnectButton.Foreground(SolidColorBrush(Colors::Gray()));
		disconnectButton.HorizontalAlignment(HorizontalAlignment::Right);
		disconnectButton.VerticalAlignment(VerticalAlignment::Center);

		std::wstring deviceId(id);
		disconnectButton.Click([deviceId](const auto&, const auto&) {
			auto it = g_audioPlaybackConnections.find(deviceId);
			if (it != g_audioPlaybackConnections.end())
			{
				g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
				it->second.second.Close();
				g_audioPlaybackConnections.erase(it);
				PostMessageW(g_hWnd, WM_REFRESHDEVICELIST, 0, 0);
			}
		});

		Grid deviceGrid;
		ColumnDefinition deviceCol0;
		deviceCol0.Width(GridLength(1, GridUnitType::Star));
		ColumnDefinition deviceCol1;
		deviceCol1.Width(GridLength(1, GridUnitType::Auto));
		ColumnDefinition deviceCol2;
		deviceCol2.Width(GridLength(1, GridUnitType::Auto));
		deviceGrid.ColumnDefinitions().Append(deviceCol0);
		deviceGrid.ColumnDefinitions().Append(deviceCol1);
		deviceGrid.ColumnDefinitions().Append(deviceCol2);
		deviceGrid.Children().Append(nameText);
		deviceGrid.Children().Append(statusText);
		deviceGrid.Children().Append(disconnectButton);
		Grid::SetColumn(nameText, 0);
		Grid::SetColumn(statusText, 1);
		Grid::SetColumn(disconnectButton, 2);

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
