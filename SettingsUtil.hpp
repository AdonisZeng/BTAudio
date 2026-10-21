#pragma once

constexpr auto CONFIG_NAME = L"BTAudio.json";
constexpr auto BUFFER_SIZE = 4096;

// ---------------------------------------------------------------------------
// Data directory resolution.
//
// Portable-first: the executable's own directory is used when it is writable,
// exactly like every previous version, so portable users are unaffected. When
// BTAudio lives in a read-only location (e.g. Program Files) writing the config
// next to the exe silently failed, so settings were lost on every restart —
// in that case %LOCALAPPDATA%\BTAudio is used instead.
//
// An existing config file always pins the location (exe dir first, then
// AppData), so an install that already has state in one place never splits it
// across both.
// ---------------------------------------------------------------------------
inline bool IsDirectoryWritable(const fs::path& dir)
{
	if (dir.empty())
		return false;
	const auto probe = dir / L"BTAudio.writetest";
	wil::unique_hfile hFile(CreateFileW(probe.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
	return static_cast<bool>(hFile);
}

inline fs::path GetAppDataDir()
{
	std::wstring base;
	wchar_t buffer[MAX_PATH];

	DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
	if (length > 0 && length < ARRAYSIZE(buffer))
		base.assign(buffer, length);

	if (base.empty())
	{
		length = GetEnvironmentVariableW(L"USERPROFILE", buffer, ARRAYSIZE(buffer));
		if (length > 0 && length < ARRAYSIZE(buffer))
		{
			base.assign(buffer, length);
			base += L"\\AppData\\Local";
		}
	}
	if (base.empty())
		return {};

	fs::path dir = fs::path(base) / L"BTAudio";
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec)
		return {};
	return dir;
}

inline void ResolveDataDir()
{
	std::error_code ec;
	const auto exeDir = GetModuleFsPath(g_hInst).remove_filename();

	// 1) Existing state pins the location.
	if (fs::exists(exeDir / CONFIG_NAME, ec))
	{
		g_dataDir = exeDir;
		return;
	}

	auto appDataDir = GetAppDataDir();
	if (!appDataDir.empty() && fs::exists(appDataDir / CONFIG_NAME, ec))
	{
		g_dataDir = appDataDir;
		return;
	}

	// 2) No state yet: prefer portable mode when the install is writable.
	if (IsDirectoryWritable(exeDir))
	{
		g_dataDir = exeDir;
		return;
	}

	// 3) Read-only install — persist under LOCALAPPDATA. If even that is not
	//    available, stay next to the exe (writes will fail, but consistently).
	g_dataDir = appDataDir.empty() ? exeDir : appDataDir;
}

inline fs::path GetConfigPath()
{
	const auto dir = g_dataDir.empty() ? GetModuleFsPath(g_hInst).remove_filename() : g_dataDir;
	return dir / CONFIG_NAME;
}

inline fs::path GetLogPath()
{
	const auto dir = g_dataDir.empty() ? GetModuleFsPath(g_hInst).remove_filename() : g_dataDir;
	return dir / L"BTAudio.log";
}

inline void DefaultSettings()
{
	g_reconnect = false;
	g_autoStart = false;
	g_language = 0;
	g_debugLogging = false;
	g_lastDevices.clear();
	g_deviceAliases.clear();
	g_deviceVolumes.clear();
	g_lowBatteryNotified.clear();
}

inline void LoadSettings()
{
	try
	{
		DefaultSettings();

		const auto configPath = GetConfigPath();
		wil::unique_hfile hFile(CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string string;
		while (1)
		{
			size_t size = string.size();
			string.resize(size + BUFFER_SIZE);
			DWORD read = 0;
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + size, BUFFER_SIZE, &read, nullptr));
			string.resize(size + read);
			if (read == 0)
				break;
		}

		std::wstring utf16 = Utf8ToUtf16(string);
		auto jsonObj = JsonObject::Parse(utf16);
		// Guard every key with HasKey so a legacy/corrupt file degrades
		// gracefully instead of aborting the whole load.
		if (jsonObj.HasKey(L"reconnect"))
			g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();

		// Read auto-start preference; if the key is missing (upgrade from an
		// older settings file), detect the current state from the registry.
		if (jsonObj.HasKey(L"autoStart"))
			g_autoStart = jsonObj.Lookup(L"autoStart").GetBoolean();
		else
			g_autoStart = IsAutoStartEnabled();

		if (jsonObj.HasKey(L"language"))
		{
			int lang = static_cast<int>(jsonObj.Lookup(L"language").GetNumber());
			if (lang < 0 || lang > 3)
				lang = 0; // clamp corrupt values
			g_language = lang;
		}
		if (jsonObj.HasKey(L"debugLogging"))
			g_debugLogging = jsonObj.Lookup(L"debugLogging").GetBoolean();

		if (jsonObj.HasKey(L"lastDevices"))
		{
			auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
			g_lastDevices.reserve(lastDevices.Size());
			for (const auto& i : lastDevices)
			{
				if (i.ValueType() == JsonValueType::String)
					g_lastDevices.push_back(std::wstring(i.GetString()));
			}
		}

		// Load device aliases (deviceId -> custom display name).
		// Older settings files won't have this key; absence is fine.
		if (jsonObj.HasKey(L"aliases"))
		{
			auto aliases = jsonObj.Lookup(L"aliases").GetObject();
			for (const auto& pair : aliases)
			{
				if (pair.Value().ValueType() == JsonValueType::String)
					g_deviceAliases[std::wstring(pair.Key())] = std::wstring(pair.Value().GetString());
			}
		}

		// Load remembered per-device volumes (deviceId -> 0..1). Out-of-range
		// values (hand-edited or corrupt file, NaN) are dropped instead of
		// being handed to SetMasterVolume, which would reject them silently.
		if (jsonObj.HasKey(L"volumes"))
		{
			auto volumes = jsonObj.Lookup(L"volumes").GetObject();
			for (const auto& pair : volumes)
			{
				if (pair.Value().ValueType() != JsonValueType::Number)
					continue;
				const double level = pair.Value().GetNumber();
				if (!(level >= 0.0 && level <= 1.0))
					continue;
				g_deviceVolumes[std::wstring(pair.Key())] = static_cast<float>(level);
			}
		}
	}
	catch (...)
	{
		// A corrupt/legacy settings file must not silently disable auto-start:
		// fall back to the current registry state instead of the default false
		// (which would delete the user's Run key on the next SetAutoStart).
		g_autoStart = IsAutoStartEnabled();
		LOG_CAUGHT_EXCEPTION();
	}
}

// Drop remembered aliases/volumes for devices that are neither connected nor
// currently enumerated, once the tables grow large enough to matter. Without
// this a long-lived install accumulates every device it has ever seen.
//
// Deliberately conservative: a device that is merely switched off / out of
// range is absent from both maps, so "not currently visible" must not be read
// as "gone for good". The limit is high enough that a normal install never
// reaches it, and the devices remembered for startup reconnect count as known.
inline void PruneDevicePreferences()
{
	constexpr size_t kMaxRememberedDevices = 256;
	if (g_deviceAliases.size() <= kMaxRememberedDevices &&
		g_deviceVolumes.size() <= kMaxRememberedDevices)
	{
		return;
	}

	auto isKnown = [](const std::wstring& id) {
		return g_audioPlaybackConnections.find(id) != g_audioPlaybackConnections.end() ||
			g_availableDevices.find(id) != g_availableDevices.end() ||
			std::find(g_lastDevices.begin(), g_lastDevices.end(), id) != g_lastDevices.end();
	};

	for (auto it = g_deviceAliases.begin(); it != g_deviceAliases.end(); )
	{
		if (isKnown(it->first))
			++it;
		else
			it = g_deviceAliases.erase(it);
	}
	for (auto it = g_deviceVolumes.begin(); it != g_deviceVolumes.end(); )
	{
		if (isKnown(it->first))
			++it;
		else
			it = g_deviceVolumes.erase(it);
	}
}

inline void SaveSettings()
{
	try
	{
		PruneDevicePreferences();

		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		jsonObj.Insert(L"autoStart", JsonValue::CreateBooleanValue(g_autoStart));
		jsonObj.Insert(L"language", JsonValue::CreateNumberValue(g_language));
		jsonObj.Insert(L"debugLogging", JsonValue::CreateBooleanValue(g_debugLogging.load()));

		JsonArray lastDevices;
		for (const auto& i : g_audioPlaybackConnections)
		{
			lastDevices.Append(JsonValue::CreateStringValue(i.first));
		}
		jsonObj.Insert(L"lastDevices", lastDevices);

		JsonObject aliases;
		for (const auto& [id, alias] : g_deviceAliases)
		{
			aliases.Insert(id, JsonValue::CreateStringValue(alias));
		}
		jsonObj.Insert(L"aliases", aliases);

		JsonObject volumes;
		for (const auto& [id, level] : g_deviceVolumes)
		{
			volumes.Insert(id, JsonValue::CreateNumberValue(level));
		}
		jsonObj.Insert(L"volumes", volumes);

		// Write to a temp file first, then atomically replace the config so a
		// crash/power loss never leaves a half-written BTAudio.json behind.
		auto configPath = GetConfigPath();
		auto tempPath = configPath;
		tempPath += L".tmp";
		{
			wil::unique_hfile hFile(CreateFileW(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			THROW_LAST_ERROR_IF(!hFile);

			std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
			DWORD written = 0;
			THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
			THROW_HR_IF(E_FAIL, written != utf8.size());
		}
		THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(tempPath.c_str(), configPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
	}
	CATCH_LOG();
}
