#pragma once
#include "FnvHash.hpp"

inline std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

inline void LoadTranslateData()
{
	// Language preference overrides the thread UI language (0 = follow
	// system, 3 = English which has no resource and falls back to source
	// strings). English is also the fallback when a language resource is
	// missing.
	WORD langId = 0;
	switch (g_language)
	{
	case 1:
		langId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
		break;
	case 2:
		langId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
		break;
	case 3:
		langId = 0; // English — use source strings
		break;
	default:
		langId = GetThreadUILanguage();
		break;
	}

	// Fallback chain: exact LANGID -> neutral sublanguage of that language ->
	// Simplified Chinese for any other Chinese locale -> English (no resource,
	// source strings). Before this, region-specific Chinese UI languages such
	// as zh-HK / zh-SG / zh-MO (0x0C04 / 0x1004 / 0x1404) matched neither
	// resource, so the entire UI silently fell back to English.
	const WORD candidates[] = {
		langId,
		langId ? static_cast<WORD>(MAKELANGID(PRIMARYLANGID(langId), SUBLANG_NEUTRAL)) : static_cast<WORD>(0),
		(langId && PRIMARYLANGID(langId) == LANG_CHINESE)
			? static_cast<WORD>(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
			: static_cast<WORD>(0),
	};

	HRSRC hRes = nullptr;
	for (const WORD candidate : candidates)
	{
		if (candidate == 0)
			continue;
		hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), candidate);
		if (hRes)
			break;
	}
	if (!hRes)
		return;

	const DWORD resourceSize = SizeofResource(g_hInst, hRes);
	if (resourceSize == 0)
		return;

	auto hResData = LoadResource(g_hInst, hRes);
	if (!hResData)
		return;

	// Use the locked pointer for BOTH the header and the string data (the
	// offsets are relative to the start of the resource).
	const auto* base = reinterpret_cast<const uint8_t*>(LockResource(hResData));
	if (!base)
		return;

	auto ymo = reinterpret_cast<const YMOData*>(base);
	// Validate the header before walking the table: a truncated or mismatched
	// data file must not be able to make us read past the end of the resource.
	const size_t headerSize = offsetof(YMOData, table) +
		static_cast<size_t>(ymo->len) * sizeof(ymo->table[0]);
	if (ymo->len == 0 || headerSize > resourceSize)
		return;

	hashToStrMap.reserve(ymo->len);
	for (uint16_t i = 0; i < ymo->len; ++i)
	{
		const uint32_t hash = ymo->table[i].hash;
		const size_t offset = ymo->table[i].offset;
		if (offset < headerSize || offset + sizeof(wchar_t) > resourceSize)
			continue;

		auto str = reinterpret_cast<const wchar_t*>(base + offset);
		// The string must be NUL-terminated inside the resource.
		const size_t maxChars = (resourceSize - offset) / sizeof(wchar_t);
		if (wcsnlen(str, maxChars) == maxChars)
			continue;

		hashToStrMap.emplace(hash, str);
	}
}

inline const wchar_t* Translate(const wchar_t* str)
{
	// thread_local, NOT plain static: Translate runs both on the UI thread
	// and inside ConnectDevice coroutine continuations on thread-pool
	// threads (the app is an MTA, so co_await resumes on the pool). A shared
	// map written lazily from several threads would race (concurrent
	// find/emplace -> heap corruption, potentially crashing the whole app
	// and dropping every Bluetooth connection). Per-thread caches need no
	// lock; keys/values are string literals with static storage duration.
	static thread_local std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

inline const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)
