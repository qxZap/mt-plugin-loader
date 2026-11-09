// C++ conveniences over mt_plugin_api.h for plugins built with this repo.
// Header-only; everything goes through the loader's api, nothing is rediscovered.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include "mt_plugin_api.h"

namespace mt {
inline const MtApi* api;

inline bool loggingOn();
// With this plugin's log off, the arguments are not even evaluated: no name lookups, no strings.
#define MT_LOG(...) (::mt::loggingOn() ? ::mt::api->log(::mt::api, __VA_ARGS__) : (void)0)

template <class T> T get(uintptr_t p, T def = T{}) { T v; return api->read(p, &v, sizeof v) ? v : def; }
inline bool rd(uintptr_t p, void* out, size_t n) { return api->read(p, out, n) != 0; }
inline bool wr(uintptr_t p, const void* in, size_t n) { return api->write(p, in, n) != 0; }

inline std::string name(uint32_t i) { char b[1100]; int n = api->name(i, b, sizeof b); return { b, (size_t)n }; }
inline std::string fname(uintptr_t at) { char b[1100]; int n = api->fname(at, b, sizeof b); return { b, (size_t)n }; }

inline const MtLayout& L() { return *api->layout; }
inline uintptr_t classOf(uintptr_t o) { return get<uintptr_t>(o + L().obj_class); }
inline uintptr_t outerOf(uintptr_t o) { return get<uintptr_t>(o + L().obj_outer); }
inline uintptr_t superOf(uintptr_t c) { return get<uintptr_t>(c + L().struct_super); }
inline bool isCDO(uintptr_t o) { return (get<uint32_t>(o + L().obj_flags) & 0x10) != 0; }  // RF_ClassDefaultObject
inline std::string objName(uintptr_t o) { return name(get<uint32_t>(o + L().obj_name)); }
inline int off(uintptr_t cls, const char* n) { return api->field_offset(cls, n); }

// A component a plugin makes, never culled by distance: the maps' cull-distance volumes drop small meshes a little way
// off (2026-11-09: brakes, flames and plates vanished at a distance). Call before FinishAddComponent, which registers
// it. Only a PrimitiveComponent has the flag; anything else (a light) is left alone.
inline void neverCull(uintptr_t comp) {
    static uintptr_t cPrim; static int oFlag = -2, oCached;
    if (oFlag == -2) {
        cPrim = api->find_object("Class", "PrimitiveComponent", "/Script/Engine");
        oFlag = cPrim ? off(cPrim, "bNeverDistanceCull") : -1;
        oCached = cPrim ? off(cPrim, "CachedMaxDrawDistance") : -1;
    }
    if (!comp || oFlag < 0 || !api->is_a(classOf(comp), cPrim)) return;
    uint8_t b = get<uint8_t>(comp + oFlag) | 0x08;  // bNeverDistanceCull's bit in UE 5.5 (jmap: field_mask 8)
    wr(comp + oFlag, &b, 1);
    float zero = 0;
    if (oCached >= 0) wr(comp + oCached, &zero, 4);
}

// The game exe's folder (Binaries\Win64). Settings live in its plugins folder: see iniFile.
inline std::wstring gameDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    *(wcsrchr(p, L'\\') + 1) = 0;
    return p;
}

// Every match of an IDA-style pattern ("8B 48 78 ?? 0F") in the game exe's .text.
// Patch sites are found this way, not by address, so a game update that leaves the
// code alone keeps the plugin working -- and one that changes it makes it refuse.
inline std::vector<uintptr_t> find_sig(const char* pattern) {
    std::vector<int> pat;  // -1 = wildcard
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { pat.push_back(-1); while (*p == '?') p++; continue; }
        pat.push_back((int)strtoul(std::string(p, 2).c_str(), nullptr, 16)); p += 2;
    }
    std::vector<uintptr_t> out;
    auto base = (const uint8_t*)api->module_base();
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        const uint8_t* lo = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize, m = pat.size();
        for (size_t i = 0; i + m <= n; i++) {
            size_t j = 0;
            while (j < m && (pat[j] < 0 || lo[i + j] == pat[j])) j++;
            if (j == m) out.push_back((uintptr_t)(lo + i));
        }
    }
    return out;
}

// PluginInit boilerplate: refuse a loader older than the api this was built for.
inline bool attach(const MtApi* a) {
    if (!a || a->version < MT_API_VERSION || a->size < sizeof(MtApi)) return false;
    api = a;
    return true;
}

// The game's Effects quality (sg.EffectsQuality: 0 Low .. 3 Epic, 4 Cinematic), -1 if it cannot
// be read. Game thread only. Plugins draw less on Low, so a weak PC still runs them.
inline int effectsQuality() {
    static uintptr_t cdo = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    static uintptr_t fn = api->find_object("Function", "GetConsoleVariableIntValue", "KismetSystemLibrary");
    if (!cdo || !fn) return -1;
    const wchar_t* key = L"sg.EffectsQuality";
    struct { const wchar_t* data; int num, max; } in = {key, (int)wcslen(key) + 1, (int)wcslen(key) + 1};
    unsigned char p[0x40] = {0};
    int at = off(fn, "VariableName"), ret = off(fn, "ReturnValue");
    if (at < 0 || ret < 0) return -1;
    memcpy(p + at, &in, sizeof in);
    api->process_event(cdo, fn, p);
    int out = -1;
    memcpy(&out, p + ret, 4);
    return out;
}

// ---- ini files ----
// The loader owns them, so two plugins that write the same file do not
// overwrite each other: every key is written on its own, under one lock, and
// the rest of the file is left alone.
//
// iniSet declares a value. force = false: a default -- a value the user
// already put in the file wins over it. force = true: write it through now
// and always win (live state that must not wait).
// iniGet reads the merged value: what was declared for the key, or what the
// file holds. Empty if there is none.
// [General] Logging in mt_loader.ini, and [Logging] <Plugin> for this plugin alone (EcuTuning for
// mt_plugin_EcuTuning.dll) -- the same rule the loader opens the log file by.
inline bool loggingOn() {
    static int on = [] {
        std::wstring ini = gameDir() + L"mt_loader.ini";
        if (!GetPrivateProfileIntW(L"General", L"Logging", 0, ini.c_str())) return 0;
        HMODULE self = nullptr;
        wchar_t p[MAX_PATH] = L"";
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&loggingOn, &self);
        GetModuleFileNameW(self, p, MAX_PATH);
        std::wstring who = wcsrchr(p, L'\\') ? wcsrchr(p, L'\\') + 1 : p;
        if (who.size() > 4) who.resize(who.size() - 4);
        if (_wcsnicmp(who.c_str(), L"mt_plugin_", 10) == 0) who = who.substr(10);
        return (int)GetPrivateProfileIntW(L"Logging", who.c_str(), 1, ini.c_str());
    }();
    return on != 0;
}

// A settings file's path relative to the game exe, under plugins\. One still next to the exe,
// where these used to live, moves over the first time it is asked for.
inline std::wstring iniFile(const wchar_t* file) {
    static std::set<std::wstring> moved;
    std::wstring to = std::wstring(L"plugins\\") + file;
    if (moved.insert(file).second) MoveFileW((gameDir() + file).c_str(), (gameDir() + to).c_str());
    return to;
}
inline std::wstring iniPath(const wchar_t* file) { return gameDir() + iniFile(file); }
inline bool iniSet(const wchar_t* file, const wchar_t* section, const wchar_t* key, const wchar_t* value, bool force = false) {
    return api->ini_set(api, iniFile(file).c_str(), section, key, value, force ? 1 : 0) != 0;
}
inline std::wstring iniGet(const wchar_t* file, const wchar_t* section, const wchar_t* key) {
    wchar_t b[1024];
    int n = api->ini_get(iniFile(file).c_str(), section, key, b, sizeof b / sizeof *b);
    return n > 0 ? std::wstring(b, (size_t)n) : std::wstring();
}

// A number from one of the loader's ini files. The value is declared as a
// default: the file's own value wins over it, and a value that is not a
// number is ignored in favour of `def`. An int below zero counts as no
// setting at all.
inline double iniNum(const wchar_t* file, const wchar_t* section, const wchar_t* key, double def) {
    wchar_t b[64]; swprintf(b, 64, L"%g", def);
    iniSet(file, section, key, b);
    std::wstring v = iniGet(file, section, key);
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    double d = wcstod(v.c_str(), &end);
    return (end && !*end) ? d : def;
}
inline int iniInt(const wchar_t* file, const wchar_t* section, const wchar_t* key, int def) {
    wchar_t b[32]; swprintf(b, 32, L"%d", def);
    iniSet(file, section, key, b);
    std::wstring v = iniGet(file, section, key);
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    if (!end || *end || n < 0) return def;
    return (int)n;
}

// [General] Disabled=1 in a plugin's own file: it starts nothing, so the game runs as if it
// were not installed. Asked before PluginInit starts the plugin's thread.
inline bool iniDisabled(const wchar_t* file) { return iniInt(file, L"General", L"Disabled", 0) != 0; }
}  // namespace mt
