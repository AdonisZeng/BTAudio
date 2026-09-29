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

	auto hRes = langId ? FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), langId) : nullptr;
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				hashToStrMap.reserve(ymo->len);

				for (int i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(hResData) + offset);
					hashToStrMap.emplace(hash, str);
				}
			}
		}
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
