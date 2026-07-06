#pragma once

constexpr auto CONFIG_NAME = L"BTAudio.json";
constexpr auto BUFFER_SIZE = 4096;

void DefaultSettings()
{
	g_reconnect = false;
	g_autoStart = false;
	g_lastDevices.clear();
	g_deviceAliases.clear();
}

void LoadSettings()
{
	try
	{
		DefaultSettings();

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
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
		g_reconnect = jsonObj.Lookup(L"reconnect").GetBoolean();

		// Read auto-start preference; if the key is missing (upgrade from an
		// older settings file), detect the current state from the registry.
		if (jsonObj.HasKey(L"autoStart"))
			g_autoStart = jsonObj.Lookup(L"autoStart").GetBoolean();
		else
			g_autoStart = IsAutoStartEnabled();

		auto lastDevices = jsonObj.Lookup(L"lastDevices").GetArray();
		g_lastDevices.reserve(lastDevices.Size());
		for (const auto& i : lastDevices)
		{
			if (i.ValueType() == JsonValueType::String)
				g_lastDevices.push_back(std::wstring(i.GetString()));
		}

		// Load device aliases (deviceId -> custom display name).
		// Older settings files won't have this key; absence is fine.
		if (jsonObj.HasKey(L"aliases"))
		{
			auto aliases = jsonObj.Lookup(L"aliases").GetObject();
			for (const auto& pair : aliases)
			{
				g_deviceAliases[std::wstring(pair.Key())] = std::wstring(pair.Value().GetString());
			}
		}
	}
	CATCH_LOG();
}

void SaveSettings()
{
	try
	{
		JsonObject jsonObj;
		jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_reconnect));
		jsonObj.Insert(L"autoStart", JsonValue::CreateBooleanValue(g_autoStart));

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

		wil::unique_hfile hFile(CreateFileW((GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!hFile);

		std::string utf8 = Utf16ToUtf8(jsonObj.Stringify());
		DWORD written = 0;
		THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr));
		THROW_HR_IF(E_FAIL, written != utf8.size());
	}
	CATCH_LOG();
}
