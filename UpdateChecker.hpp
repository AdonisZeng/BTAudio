#pragma once

#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

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
	std::wstring digest;      // asset digest ("sha256:<hex>", empty if not published)
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
	if (swscanf_s(tag.c_str(), L"%d.%d.%d", &rMajor, &rMinor, &rPatch) < 3)
		return false; // malformed tag — treat as not newer

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
// Architecture detection — returns the same suffix the build system appends
// to the exe name (TargetName = $(ProjectName)$(PlatformArchitecture)):
//   x64   -> "64"    (BTAudio64.exe)
//   Win32 -> "32"    (BTAudio32.exe)
//   ARM64 -> "ARM64" (BTAudioARM64.exe)
//   ARM   -> "ARM"   (BTAudioARM.exe)
// The update downloader matches release assets against this suffix, so asset
// naming must stay in sync with the build output.
// ---------------------------------------------------------------------------
inline std::wstring GetCurrentArch()
{
#if defined(_M_X64) || defined(__x86_64__)
	return L"64";
#elif defined(_M_IX86) || defined(__i386__)
	return L"32";
#elif defined(_M_ARM64) || defined(__aarch64__)
	return L"ARM64";
#elif defined(_M_ARM) || defined(__arm__)
	return L"ARM";
#else
	return L"";
#endif
}

// Match a lowercased asset name against the build-suffix arch. Order matters:
// "arm64" is a substring of "arm64" itself and must be tested before "arm",
// and "64" must NOT match an "arm64" asset.
inline bool ArchMatchesAsset(const std::wstring& lowerName, const std::wstring& arch)
{
	if (arch == L"64")
		return lowerName.find(L"arm64") == std::wstring::npos &&
			lowerName.find(L"arm") == std::wstring::npos &&
			lowerName.find(L"64") != std::wstring::npos;
	if (arch == L"32")
		return lowerName.find(L"32") != std::wstring::npos &&
			lowerName.find(L"64") == std::wstring::npos;
	if (arch == L"arm64")
		return lowerName.find(L"arm64") != std::wstring::npos;
	if (arch == L"arm")
		return lowerName.find(L"arm") != std::wstring::npos &&
			lowerName.find(L"arm64") == std::wstring::npos;
	return false;
}

// Verify a downloaded buffer against the release asset digest
// ("sha256:<hex>"). Returns true when the digest is absent or in an unknown
// format (nothing to verify against) or when it matches.
inline bool VerifyDownloadDigest(const std::wstring& digest, const winrt::Windows::Storage::Streams::IBuffer& buffer)
{
	using namespace winrt::Windows::Security::Cryptography;
	using namespace winrt::Windows::Security::Cryptography::Core;

	if (digest.empty())
		return true;
	constexpr wchar_t prefix[] = L"sha256:";
	if (digest.size() <= std::size(prefix) - 1 ||
		_wcsnicmp(digest.c_str(), prefix, std::size(prefix) - 1) != 0)
	{
		return true; // unknown digest format — don't block the update
	}
	auto expected = digest.substr(std::size(prefix) - 1);
	auto hasher = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
	auto hash = hasher.HashData(buffer);
	auto actual = CryptographicBuffer::EncodeToHexString(hash);
	return _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}

// Pick the asset that matches the current architecture; fall back to first .exe.
inline bool SelectAsset(const JsonArray& assets, std::wstring& outUrl, std::wstring& outName, uint64_t& outSize, std::wstring& outDigest)
{
	auto arch = GetCurrentArch();

	if (!arch.empty())
	{
		for (const auto& a : assets)
		{
			auto obj = a.GetObject();
			auto name = std::wstring(obj.GetNamedString(L"name"));
			std::wstring lowerName(name);
			std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
				[](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
			if (ArchMatchesAsset(lowerName, arch))
			{
				outUrl = std::wstring(obj.GetNamedString(L"browser_download_url"));
				outName = name;
				outSize = static_cast<uint64_t>(obj.GetNamedNumber(L"size"));
				outDigest = std::wstring(obj.GetNamedString(L"digest", L""));
				return true;
			}		}
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
				outDigest = std::wstring(obj.GetNamedString(L"digest", L""));
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
				SelectAsset(assets, info.downloadUrl, info.assetName, info.assetSize, info.digest);
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
		// Hand the release info to the UI thread inside the message itself so
		// g_pendingRelease is only ever written on the UI thread.
		auto* pInfo = new ReleaseInfo(std::move(info));
		if (!PostMessageW(g_hWnd, WM_UPDATEAVAILABLE,
			reinterpret_cast<WPARAM>(pInfo), 0))
		{
			delete pInfo; // queue full / window gone — free locally
		}
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
		{
			PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
			co_return;
		}

		auto buffer = co_await response.Content().ReadAsBufferAsync();

		// Save to temp directory
		auto tempDir = fs::temp_directory_path();
		auto downloadPath = tempDir / info.assetName;

		// Verify the download against the published asset digest (if any).
		// A mismatch means a corrupt or tampered file — discard it.
		if (!VerifyDownloadDigest(info.digest, buffer))
		{
			DeleteFileW(downloadPath.c_str());
			PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
			co_return;
		}

		{
			wil::unique_hfile hFile(CreateFileW(downloadPath.c_str(), GENERIC_WRITE, 0,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			THROW_LAST_ERROR_IF(!hFile);
			DWORD written = 0;
			THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), buffer.data(),
				static_cast<DWORD>(buffer.Length()), &written, nullptr));
			THROW_HR_IF(E_FAIL, written != buffer.Length());
		}

		// Build a batch script that waits for this process to exit, then
		// replaces the exe and relaunches it.
		auto exePath = GetModuleFsPath(g_hInst);

		// Prefer short paths (ASCII-safe) to avoid spaces / unicode issues
		// inside the batch. If 8.3 short names are disabled (GetShortPathNameW
		// fails), fall back to the long path: the batch switches to UTF-8 via
		// `chcp 65001` and '&' is escaped so quoted long paths survive.
		std::wstring shortDownload = downloadPath.wstring();
		std::wstring shortExe = exePath.wstring();
		std::wstring shortBuf(MAX_PATH, L'\0');
		if (DWORD n = GetShortPathNameW(shortDownload.c_str(), shortBuf.data(),
			static_cast<DWORD>(shortBuf.size())))
			shortDownload.assign(shortBuf.data(), n);
		if (DWORD n = GetShortPathNameW(shortExe.c_str(), shortBuf.data(),
			static_cast<DWORD>(shortBuf.size())))
			shortExe.assign(shortBuf.data(), n);

		// Escape '&' and '^' (cmd.exe metacharacters) inside the batch file.
		auto escapeBatch = [](std::wstring s) {
			std::wstring out;
			out.reserve(s.size());
			for (wchar_t c : s)
			{
				if (c == L'&' || c == L'^')
					out += L'^';
				out += c;
			}
			return out;
		};
		shortDownload = escapeBatch(shortDownload);
		shortExe = escapeBatch(shortExe);

		// Narrow to UTF-8 — the batch runs `chcp 65001` before using these.
		std::string sDownload = Utf16ToUtf8(shortDownload);
		std::string sExe = Utf16ToUtf8(shortExe);

		auto batPath = tempDir / L"BTAudio_update.bat";
		std::ostringstream bat;
		bat << "@echo off\r\n";
		// Switch cmd to UTF-8 so long non-ASCII paths (8.3 fallback) work.
		bat << "chcp 65001 >NUL\r\n";
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
			// The batch is now waiting for this process to exit. Ask the user
			// whether to restart immediately or defer to the next exit.
			PostMessageW(g_hWnd, WM_UPDATEINSTALLED, 0, 0);
		}
	}
	catch (winrt::hresult_error const&)
	{
		LOG_CAUGHT_EXCEPTION();
		PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
	}
}
