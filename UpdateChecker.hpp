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
//
// NOTE: GetCurrentArch() returns the MSBuild $(PlatformArchitecture) suffix in
// upper case ("ARM64"/"ARM"), so `arch` is normalised to lower case here. An
// un-normalised value silently fell through to the "first .exe in the release"
// fallback and could hand an ARM64 user the x64 build.
inline bool ArchMatchesAsset(const std::wstring& lowerName, std::wstring arch)
{
	std::transform(arch.begin(), arch.end(), arch.begin(),
		[](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });

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

// A release asset is a usable update payload only when it is an .exe. Without
// this check the architecture pass could select e.g. "BTAudio64.pdb" (or a
// "BTAudio64.zip"), which was then moved over the running executable.
inline bool IsExeAssetName(const std::wstring& lowerName)
{
	constexpr wchar_t ext[] = L".exe";
	constexpr size_t extLen = sizeof(ext) / sizeof(ext[0]) - 1;
	return lowerName.size() > extLen &&
		lowerName.compare(lowerName.size() - extLen, extLen, ext) == 0;
}

inline std::wstring ToLowerWideCopy(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		[](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
	return s;
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

// Pick the asset that matches the current architecture; fall back to an .exe
// that at least carries our product name, then to the first .exe in the list.
// Non-executable assets (.pdb, .zip, checksums, ...) are never eligible: they
// used to be selected by the architecture pass and then moved over the exe.
inline bool SelectAsset(const JsonArray& assets, std::wstring& outUrl, std::wstring& outName, uint64_t& outSize, std::wstring& outDigest)
{
	struct AssetCandidate
	{
		std::wstring name;      // original (diagnostics / log only)
		std::wstring lowerName; // lower-cased copy for matching
		std::wstring url;
		std::wstring digest;
		uint64_t size = 0;
	};

	std::vector<AssetCandidate> exeAssets;
	exeAssets.reserve(assets.Size());

	for (const auto& a : assets)
	{
		auto obj = a.GetObject();
		AssetCandidate candidate;
		candidate.name = std::wstring(obj.GetNamedString(L"name"));
		candidate.lowerName = ToLowerWideCopy(candidate.name);
		if (!IsExeAssetName(candidate.lowerName))
			continue;
		candidate.url = std::wstring(obj.GetNamedString(L"browser_download_url"));
		candidate.size = static_cast<uint64_t>(obj.GetNamedNumber(L"size"));
		candidate.digest = std::wstring(obj.GetNamedString(L"digest", L""));
		exeAssets.push_back(std::move(candidate));
	}

	auto select = [&](const AssetCandidate& candidate) {
		outUrl = candidate.url;
		outName = candidate.name;
		outSize = candidate.size;
		outDigest = candidate.digest;
	};

	// 1) Exact architecture match (e.g. BTAudio64.exe on x64).
	const auto arch = GetCurrentArch();
	if (!arch.empty())
	{
		for (const auto& candidate : exeAssets)
		{
			if (ArchMatchesAsset(candidate.lowerName, arch))
			{
				select(candidate);
				return true;
			}
		}
	}

	// 2) Fallback: prefer an asset named after the product so a helper exe
	//    shipped in the same release is not mistaken for the application.
	for (const auto& candidate : exeAssets)
	{
		if (candidate.lowerName.find(L"btaudio") != std::wstring::npos)
		{
			select(candidate);
			return true;
		}
	}

	// 3) Last resort: the first executable asset.
	if (!exeAssets.empty())
	{
		select(exeAssets.front());
		return true;
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
// Resolve an 8.3 short path when available. GetShortPathNameW reports the
// required buffer size (including the terminator) when the caller's buffer is
// too small, so the returned length must be validated before it is used as a
// character count — trusting it read past the buffer for long paths.
// ---------------------------------------------------------------------------
inline std::wstring TryGetShortPath(const std::wstring& path)
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD length = GetShortPathNameW(path.c_str(), buffer.data(),
		static_cast<DWORD>(buffer.size()));
	if (length == 0)
		return path;

	if (length >= buffer.size())
	{
		buffer.resize(static_cast<size_t>(length) + 1);
		length = GetShortPathNameW(path.c_str(), buffer.data(),
			static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			return path;
	}

	buffer.resize(length);
	return buffer;
}

// Escape a path for use INSIDE double quotes in a batch file. cmd.exe does not
// treat '^' as an escape character there, so escaping '&'/'^' corrupted paths
// containing them; only '%' still expands inside quotes and must be doubled.
inline std::wstring EscapeQuotedBatchPath(std::wstring path)
{
	std::wstring out;
	out.reserve(path.size() + 8);
	for (wchar_t c : path)
	{
		if (c == L'%')
			out += L'%';
		out += c;
	}
	return out;
}

// The helper batch writes this marker when it could not replace the executable
// even after its retries. The next start consumes it and tells the user —
// otherwise a failed update silently keeps running the old version.
inline fs::path GetUpdateFailedMarkerPath()
{
	return fs::temp_directory_path() / L"BTAudio_update_failed.txt";
}

// Returns true exactly once for a failed update (the marker is deleted).
inline bool ConsumeFailedUpdateMarker()
{
	try
	{
		const auto marker = GetUpdateFailedMarkerPath();
		std::error_code ec;
		if (!fs::exists(marker, ec))
			return false;
		fs::remove(marker, ec);
		return true;
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		return false;
	}
}

// ---------------------------------------------------------------------------
// Download the new exe and launch a helper batch to replace & restart.
// ---------------------------------------------------------------------------
inline winrt::fire_and_forget DownloadAndInstall(ReleaseInfo info)
{
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Headers;

	// Declared outside the try so the failure paths below can delete a
	// half-downloaded file even when an exception unwinds past the write.
	fs::path downloadPath;

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

		// Everything below derives from remote release metadata, so validate it
		// before touching disk: a truncated/oversized body is not an update.
		if (info.assetSize != 0 && buffer.Length() != info.assetSize)
		{
			LOG_HR_MSG(E_UNEXPECTED, "Update asset size mismatch (expected %llu, got %u)",
				static_cast<unsigned long long>(info.assetSize), buffer.Length());
			PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
			co_return;
		}

		// Verify the download against the published asset digest (if any).
		// A mismatch means a corrupt or tampered file — discard it.
		if (!VerifyDownloadDigest(info.digest, buffer))
		{
			LOG_HR_MSG(E_UNEXPECTED, "Update asset digest mismatch");
			PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
			co_return;
		}

		// Never let the remote asset name participate in path construction: a
		// crafted name ("..\\..\\foo" or an absolute path) could write outside
		// the temp directory. Use a fixed per-process name instead; assetName is
		// only used for diagnostics.
		auto tempDir = fs::temp_directory_path();
		downloadPath = tempDir / (L"BTAudio_update_" +
			std::to_wstring(GetCurrentProcessId()) + L".exe");

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
		// `chcp 65001`.
		std::wstring shortDownload = EscapeQuotedBatchPath(TryGetShortPath(downloadPath.wstring()));
		std::wstring shortExe = EscapeQuotedBatchPath(TryGetShortPath(exePath.wstring()));

		// Narrow to UTF-8 — the batch runs `chcp 65001` before using these.
		std::string sDownload = Utf16ToUtf8(shortDownload);
		std::string sExe = Utf16ToUtf8(shortExe);

		std::string sFailedMarker = Utf16ToUtf8(
			EscapeQuotedBatchPath(TryGetShortPath(GetUpdateFailedMarkerPath().wstring())));

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
		// The old exe may still be held by AV / a scanner for a moment after
		// the process exits, so retry the replacement instead of silently
		// leaving the update unapplied (and never start the new exe unless the
		// move actually succeeded).
		bat << "set /a tries=0\r\n";
		bat << ":replace\r\n";
		bat << "move /y \"" << sDownload << "\" \"" << sExe << "\" >NUL 2>&1\r\n";
		bat << "if not errorlevel 1 goto replaced\r\n";
		bat << "set /a tries+=1\r\n";
		bat << "if %tries% GEQ 10 goto failed\r\n";
		bat << "timeout /t 1 /nobreak >NUL\r\n";
		bat << "goto replace\r\n";
		bat << ":replaced\r\n";
		bat << "start \"\" \"" << sExe << "\"\r\n";
		bat << "goto done\r\n";
		bat << ":failed\r\n";
		bat << "echo update failed> \"" << sFailedMarker << "\"\r\n";
		bat << ":done\r\n";
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

		// Resolve the command interpreter through %ComSpec% instead of
		// hard-coding C:\Windows\System32\cmd.exe: Windows may be installed on
		// another drive, and a 32-bit image would be redirected anyway.
		std::wstring comSpec;
		{
			wchar_t comSpecBuffer[MAX_PATH];
			DWORD comSpecLength = GetEnvironmentVariableW(L"ComSpec", comSpecBuffer, ARRAYSIZE(comSpecBuffer));
			if (comSpecLength > 0 && comSpecLength < ARRAYSIZE(comSpecBuffer))
				comSpec.assign(comSpecBuffer, comSpecLength);
		}
		if (comSpec.empty())
		{
			wchar_t systemDir[MAX_PATH];
			UINT length = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
			if (length > 0 && length < ARRAYSIZE(systemDir))
			{
				comSpec.assign(systemDir, length);
				comSpec += L"\\cmd.exe";
			}
		}

		if (!comSpec.empty())
		{
			// Launch the batch (hidden) and tell the main window it is waiting.
			STARTUPINFOW si = { sizeof(si) };
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi = {};
			std::wstring cmd = L"/c \"" + batPath.wstring() + L"\"";
			if (CreateProcessW(comSpec.c_str(), cmd.data(),
				nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
			{
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
				// The batch is now waiting for this process to exit. Ask the
				// user whether to restart immediately or defer to next exit.
				PostMessageW(g_hWnd, WM_UPDATEINSTALLED, 0, 0);
				co_return;
			}
			LOG_LAST_ERROR();
		}
		else
		{
			LOG_HR_MSG(E_UNEXPECTED, "Failed to locate the command interpreter");
		}

		// No usable command interpreter / process creation failed — report it
		// to the user instead of silently doing nothing.
		DeleteFileW(downloadPath.c_str());
		downloadPath.clear();
		PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		if (!downloadPath.empty())
			DeleteFileW(downloadPath.c_str());
		PostMessageW(g_hWnd, WM_UPDATEFAILED, 0, 0);
	}
}
