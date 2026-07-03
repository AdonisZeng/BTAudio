#pragma once

#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>

// ---------------------------------------------------------------------------
// Release info returned by GitHub API
// ---------------------------------------------------------------------------
struct ReleaseInfo
{
	bool valid = false;
	std::wstring tagName;
	std::wstring name;
	std::wstring body;        // release notes (markdown)
	std::wstring htmlUrl;     // web page URL (fallback when no matching asset)
	std::wstring downloadUrl; // direct download URL for matched asset (may be empty)
	std::wstring assetName;
	uint64_t assetSize = 0;
};

// Defined in BTAudio.h (after this header is included).
extern ReleaseInfo g_pendingRelease;

// ---------------------------------------------------------------------------
// Version comparison — returns true if remoteTag is newer than the built-in version
// ---------------------------------------------------------------------------
inline bool IsNewerVersion(const std::wstring& remoteTag)
{
	std::wstring tag = remoteTag;
	if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V'))
		tag.erase(0, 1);

	int rMajor = 0, rMinor = 0, rPatch = 0;
	swscanf_s(tag.c_str(), L"%d.%d.%d", &rMajor, &rMinor, &rPatch);

	if (rMajor != BTAUDIO_VERSION_MAJOR) return rMajor > BTAUDIO_VERSION_MAJOR;
	if (rMinor != BTAUDIO_VERSION_MINOR) return rMinor > BTAUDIO_VERSION_MINOR;
	return rPatch > BTAUDIO_VERSION_PATCH;
}

// Strip leading 'v'/'V' from a tag for display: "v1.2.0" -> "1.2.0"
inline std::wstring NormalizeVersionTag(const std::wstring& tag)
{
	if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V'))
		return tag.substr(1);
	return tag;
}

// ---------------------------------------------------------------------------
// Architecture detection — used to pick the correct asset from a multi-arch release
// ---------------------------------------------------------------------------
inline std::wstring GetCurrentArch()
{
#if defined(_M_X64) || defined(__x86_64__)
	return L"x64";
#elif defined(_M_IX86) || defined(__i386__)
	return L"x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
	return L"ARM64";
#elif defined(_M_ARM) || defined(__arm__)
	return L"ARM";
#else
	return L"";
#endif
}

inline bool ArchMatchesAsset(const std::wstring& assetName, const std::wstring& arch)
{
	auto toLower = [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); };

	std::wstring lowerName(assetName);
	std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), toLower);

	std::wstring lowerArch(arch);
	std::transform(lowerArch.begin(), lowerArch.end(), lowerArch.begin(), toLower);

	return lowerName.find(lowerArch) != std::wstring::npos;
}

// Pick the asset that matches the current architecture; fall back to first .exe.
inline bool SelectAsset(const JsonArray& assets, std::wstring& outUrl, std::wstring& outName, uint64_t& outSize)
{
	auto arch = GetCurrentArch();

	if (!arch.empty())
	{
		for (const auto& a : assets)
		{
			auto obj = a.GetObject();
			auto name = std::wstring(obj.GetNamedString(L"name"));
			if (ArchMatchesAsset(name, arch))
			{
				outUrl = std::wstring(obj.GetNamedString(L"browser_download_url"));
				outName = name;
				outSize = static_cast<uint64_t>(obj.GetNamedNumber(L"size"));
				return true;
			}
		}
	}

	for (const auto& a : assets)
	{
		auto obj = a.GetObject();
		auto name = std::wstring(obj.GetNamedString(L"name"));
		if (name.size() >= 4)
		{
			auto ext = name.substr(name.size() - 4);
			auto toLower = [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); };
			std::transform(ext.begin(), ext.end(), ext.begin(), toLower);
			if (ext == L".exe")
			{
				outUrl = std::wstring(obj.GetNamedString(L"browser_download_url"));
				outName = name;
				outSize = static_cast<uint64_t>(obj.GetNamedNumber(L"size"));
				return true;
			}
		}
	}

	return false;
}

// ---------------------------------------------------------------------------
// Check for update — posts a message to the main window when done.
//   silent=true  : only notify if a newer version exists
//   silent=false : always notify (up-to-date / failed / available)
// ---------------------------------------------------------------------------
inline winrt::fire_and_forget CheckForUpdate(bool silent)
{
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Headers;

	ReleaseInfo info{};

	try
	{
		HttpClient client;
		client.DefaultRequestHeaders().UserAgent().Append(
			HttpProductInfoHeaderValue(L"BTAudio", BTAUDIO_VERSION_STR));

		auto response = co_await client.GetAsync(
			Uri(L"https://api.github.com/repos/AdonisZeng/BTAudio/releases/latest"));
		if (response.StatusCode() == HttpStatusCode::Ok)
		{
			auto json = co_await response.Content().ReadAsStringAsync();
			auto obj = JsonObject::Parse(json);

			info.tagName = std::wstring(obj.GetNamedString(L"tag_name"));
			info.name = std::wstring(obj.GetNamedString(L"name", L""));
			info.body = std::wstring(obj.GetNamedString(L"body", L""));
			info.htmlUrl = std::wstring(obj.GetNamedString(L"html_url", L""));

			auto assets = obj.GetNamedArray(L"assets", nullptr);
			if (assets)
			{
				SelectAsset(assets, info.downloadUrl, info.assetName, info.assetSize);
			}

			info.valid = true;
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}

	if (!info.valid)
	{
		if (!silent)
			PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
		co_return;
	}

	if (IsNewerVersion(info.tagName))
	{
		g_pendingRelease = info;
		PostMessageW(g_hWnd, WM_UPDATEAVAILABLE, 0, 0);
	}
	else
	{
		if (!silent)
			PostMessageW(g_hWnd, WM_UPTODATE, 0, 0);
	}
}

// ---------------------------------------------------------------------------
// Download the new exe and launch a helper batch to replace & restart.
// ---------------------------------------------------------------------------
inline winrt::fire_and_forget DownloadAndInstall(ReleaseInfo info)
{
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Headers;

	try
	{
		HttpClient client;
		client.DefaultRequestHeaders().UserAgent().Append(
			HttpProductInfoHeaderValue(L"BTAudio", BTAUDIO_VERSION_STR));

		auto response = co_await client.GetAsync(Uri(info.downloadUrl));
		if (response.StatusCode() != HttpStatusCode::Ok)
			co_return;

		auto buffer = co_await response.Content().ReadAsBufferAsync();

		// Save to temp directory
		auto tempDir = fs::temp_directory_path();
		auto downloadPath = tempDir / info.assetName;

		{
			wil::unique_hfile hFile(CreateFileW(downloadPath.c_str(), GENERIC_WRITE, 0,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			THROW_LAST_ERROR_IF(!hFile);
			DWORD written = 0;
			THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), buffer.data(),
				static_cast<DWORD>(buffer.Length()), &written, nullptr));
		}

		// Build a batch script that waits for this process to exit, then
		// replaces the exe and relaunches it.
		auto exePath = GetModuleFsPath(g_hInst);

		// Use short paths to avoid spaces / unicode issues inside the batch.
		std::wstring shortDownload(MAX_PATH, L'\0');
		std::wstring shortExe(MAX_PATH, L'\0');
		GetShortPathNameW(downloadPath.c_str(), shortDownload.data(),
			static_cast<DWORD>(shortDownload.size()));
		GetShortPathNameW(exePath.wstring().c_str(), shortExe.data(),
			static_cast<DWORD>(shortExe.size()));
		shortDownload.resize(wcslen(shortDownload.data()));
		shortExe.resize(wcslen(shortExe.data()));

		// Narrow to ASCII (short paths are ASCII-safe)
		std::string sDownload = Utf16ToUtf8(shortDownload);
		std::string sExe = Utf16ToUtf8(shortExe);

		auto batPath = tempDir / L"BTAudio_update.bat";
		std::ostringstream bat;
		bat << "@echo off\r\n";
		bat << ":wait\r\n";
		bat << "tasklist /FI \"PID eq " << GetCurrentProcessId()
			<< "\" 2>NUL | find \"" << GetCurrentProcessId() << "\" >NUL\r\n";
		bat << "if not errorlevel 1 (\r\n";
		bat << "  timeout /t 1 /nobreak >NUL\r\n";
		bat << "  goto wait\r\n";
		bat << ")\r\n";
		bat << "move /y \"" << sDownload << "\" \"" << sExe << "\"\r\n";
		bat << "start \"\" \"" << sExe << "\"\r\n";
		bat << "del \"%~f0\"\r\n";

		{
			wil::unique_hfile hBat(CreateFileW(batPath.c_str(), GENERIC_WRITE, 0,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			THROW_LAST_ERROR_IF(!hBat);
			auto batStr = bat.str();
			DWORD written = 0;
			THROW_IF_WIN32_BOOL_FALSE(WriteFile(hBat.get(), batStr.data(),
				static_cast<DWORD>(batStr.size()), &written, nullptr));
		}

		// Launch the batch (hidden) and tell the main window to quit.
		STARTUPINFOW si = { sizeof(si) };
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi = {};
		std::wstring cmd = L"/c \"" + batPath.wstring() + L"\"";
		if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmd.data(),
			nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}
