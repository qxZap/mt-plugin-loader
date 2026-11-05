// Motor Town native plugin loader -- version.dll.
//
// The game exe imports version.dll and Windows looks beside the exe first, so
// this file forwards every export to the real System32 DLL and, on load:
//
//   1. loads Binaries\Win64\plugins\mt_plugin_*.dll in alphabetical order and
//      calls each one's PluginInit(api), one log per plugin beside it;
//   2. finds the engine once -- name pool, object table, reflection layout --
//      by shape, not signatures, and shares it through the api;
//   3. owns the single ProcessEvent hook, which runs queued game-thread work;
  //   4. keeps the .usmap export ([Mappings] Export=1 in mt_loader.ini);
  //   5. exports the game's console variables to Binaries\Win64\mt_loader_cvars.txt
  //      ([Mappings] ExportCvars=1 in mt_loader.ini).
//
// Engine discovery, the layout detection, the .usmap writer and the hook are
// lifted from capitalist_economy\src\dllmain.cpp, where they were proven on
// 0.7.20 (ProcessEvent is vtable entry 79 there; it is found, not assumed).

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../include/mt_plugin_api.h"
#include "ini_merge.h"

// ---- version.dll forwarding: every export goes to the real system DLL ----
#define FWD(n) __pragma(comment(linker, "/export:" #n "=C:\\Windows\\System32\\version." #n))
FWD(GetFileVersionInfoA) FWD(GetFileVersionInfoByHandle) FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW) FWD(GetFileVersionInfoSizeA) FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW) FWD(GetFileVersionInfoSizeW) FWD(GetFileVersionInfoW)
FWD(VerFindFileA) FWD(VerFindFileW) FWD(VerInstallFileA) FWD(VerInstallFileW)
FWD(VerLanguageNameA) FWD(VerLanguageNameW) FWD(VerQueryValueA) FWD(VerQueryValueW)

static HMODULE g_self;
static DWORD g_gameThread;
static FILE* g_log;

static void vlog(FILE* f, const char* fmt, va_list a) {
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    vfprintf(f, fmt, a);
    fputc('\n', f); fflush(f);
}
// [General] Logging=1 in mt_loader.ini. Off, no log file is even created.
static bool logsOn() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    wcscpy_s(wcsrchr(p, L'\\') + 1, MAX_PATH - (wcsrchr(p, L'\\') + 1 - p), L"mt_loader.ini");
    return GetPrivateProfileIntW(L"General", L"Logging", 0, p) != 0;
}
static void logf(const char* fmt, ...) { va_list a; va_start(a, fmt); vlog(g_log, fmt, a); va_end(a); }

static std::wstring exeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH);
    *(wcsrchr(p, L'\\') + 1) = 0;
    return p;
}

// UTF-8 in, UTF-16 out (the ini files are wide, like the profile API).
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// UTF-16 out, UTF-8 in.
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- ini files: the loader owns them ----
// Every plugin writes its settings through here, so two plugins that set
// different keys of the same file cannot overwrite each other, and a plugin's
// write cannot land in the middle of the game's own config.
static ini::Store g_ini;
static void iniLog(const wchar_t* m) {
    char b[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, m, -1, b, sizeof b, nullptr, nullptr);
    logf("%s", n > 0 ? b : "?");
}

// Every read of game memory goes through here: a stale pointer is a skipped
// value, never a crash.
static bool rd(uintptr_t p, void* out, size_t n) {
    if (p < 0x10000) return false;
    __try { memcpy(out, (const void*)p, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool wr(uintptr_t p, const void* in, size_t n) {
    if (p < 0x10000) return false;
    __try { memcpy((void*)p, in, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> static T get(uintptr_t p, T def = T{}) { T v; return rd(p, &v, sizeof v) ? v : def; }

// ---- sections of the game exe ----
struct Range { uintptr_t lo, hi; };
static Range section(const char* want) {
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char*)s->Name, want, 8))
            return { base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize };
    return { 0, 0 };
}

// ---- FName pool: Blocks[0] begins with the entries "None", "ByteProperty" ----
static uintptr_t g_blocks;

static bool isNoneBlock(uintptr_t b) {
    char e[24];
    if (!rd(b, e, sizeof e)) return false;
    uint16_t h0 = *(uint16_t*)e, h1 = *(uint16_t*)(e + 6);
    return (h0 >> 6) == 4 && !(h0 & 1) && !memcmp(e + 2, "None", 4)
        && (h1 >> 6) == 12 && !memcmp(e + 8, "ByteProperty", 12);
}
static bool findNamePool() {
    Range d = section(".data");
    for (uintptr_t a = d.lo; a + 8 <= d.hi; a += 8)
        if (isNoneBlock(get<uintptr_t>(a))) { g_blocks = a; return true; }
    return false;
}
static std::string name(uint32_t idx) {
    uintptr_t blk = get<uintptr_t>(g_blocks + 8ull * (idx >> 16));
    uintptr_t e = blk + 2ull * (idx & 0xFFFF);
    uint16_t h = get<uint16_t>(e);
    int len = h >> 6;
    if (!blk || len <= 0 || len > 1024) return {};
    std::string s(len, '\0');
    if (h & 1) {  // wide entry: narrow it
        for (int i = 0; i < len; i++) s[i] = (char)get<uint16_t>(e + 2 + 2ull * i);
    } else if (!rd(e + 2, s.data(), len)) return {};
    return s;
}
static std::string fname(uintptr_t at) {
    std::string s = name(get<uint32_t>(at));
    int num = get<int>(at + 4);
    if (num > 0) s += "_" + std::to_string(num - 1);
    return s;
}

// The reverse of name(): walk the pool and find the index of a name string.
// Pool entries are 2-byte aligned, so an entry's index is (block << 16) | (offset / 2).
static uint32_t nameIndexOf(const char* want) {
    if (!want) return 0;
    for (uintptr_t b = 0; b < 4096; b++) {
        uintptr_t blk = get<uintptr_t>(g_blocks + 8ull * b);
        if (!blk) break;
        for (uintptr_t off = 0; off < 0x20000; ) {
            uint16_t h = get<uint16_t>(blk + off);
            int len = h >> 6;
            bool wide = (h & 1) != 0;
            if (len > 1024) break;                 // garbage: end of block
            if (len == 0) { off += 2; continue; }  // an empty entry: skip it
            std::string s;
            if (wide) {
                for (int i = 0; i < len; i++) s += (char)get<uint16_t>(blk + off + 2 + 2ull * i);
            } else {
                s.resize(len);
                if (!rd(blk + off + 2, s.data(), len)) break;
            }
            if (s == want) return (uint32_t)((b << 16) | (off / 2));
            uintptr_t adv = 2 + (wide ? 2ull * len : (uint64_t)len);
            if (adv & 1) adv++;
            off += adv;
        }
    }
    return 0;
}

  // Walk the name pool: every name the game registered, one at a time. This is
  // where the engine's console variables live -- they are registered by FName,
  // so their names are pool entries and not strings in the binary.
  static int poolNames(int (*fn)(const char*, void*), void* ctx) {
      if (!g_blocks || !fn) return 0;
      int count = 0;
      for (uintptr_t b = 0; b < 4096; b++) {
          uintptr_t blk = get<uintptr_t>(g_blocks + 8ull * b);
          if (!blk) break;
          for (uintptr_t off = 0; off < 0x20000; ) {
              uint16_t h = get<uint16_t>(blk + off);
              int len = h >> 6;
              bool wide = (h & 1) != 0;
              if (len > 1024) break;                 // garbage: end of block
              if (len == 0) { off += 2; continue; }  // an empty entry: skip it
              std::string s;
              bool ok = true;
              if (wide) {
                  for (int i = 0; i < len; i++) s += (char)get<uint16_t>(blk + off + 2 + 2ull * i);
              } else {
                  s.resize(len);
                  if (!rd(blk + off + 2, s.data(), len)) break;
              }
              uintptr_t adv = 2 + (wide ? 2ull * len : (uint64_t)len);
              if (adv & 1) adv++;
              off += adv;
              if (!ok) continue;
              count++;
              if (fn(s.c_str(), ctx)) return count;
          }
      }
      return count;
  }

  // ---- GUObjectArray: found by its own invariants ----
static uintptr_t g_chunks; static int g_stride;
static uintptr_t g_numAddr;
constexpr int kChunk = 64 * 1024;
constexpr int oFlags = 0x08, oIndex = 0x0C, oClass = 0x10, oName = 0x18, oOuter = 0x20;

static bool findObjectArray() {
    Range d = section(".data");
    for (uintptr_t a = d.lo; a + 0x20 <= d.hi; a += 8) {
        int mx = get<int>(a + 0x10), num = get<int>(a + 0x14), mc = get<int>(a + 0x18), nc = get<int>(a + 0x1C);
        if (mx < 100000 || num < 1000 || num > mx || mc != (mx + kChunk - 1) / kChunk || nc < 1 || nc > mc) continue;
        uintptr_t chunks = get<uintptr_t>(a), c0 = get<uintptr_t>(chunks);
        if (!c0) continue;
        for (int stride : { 24, 16, 32 }) {  // FUObjectItem size varies by engine version
            int ok = 0;
            for (int i = 1; i < 64; i++) {
                uintptr_t o = get<uintptr_t>(c0 + (uintptr_t)i * stride);
                if (o && get<int>(o + oIndex, -1) == i) ok++;
            }
            if (ok > 50) { g_chunks = chunks; g_stride = stride; g_numAddr = a + 0x14; return true; }
        }
    }
    return false;
}
static int numObjects() { return get<int>(g_numAddr); }
static uintptr_t object(int i) {
    uintptr_t c = get<uintptr_t>(g_chunks + 8ull * (i / kChunk));
    return c ? get<uintptr_t>(c + (uintptr_t)(i % kChunk) * g_stride) : 0;
}
static std::string objName(uintptr_t o) { return name(get<uint32_t>(o + oName)); }
static uintptr_t findObject(const char* cls, const char* nm, const char* outer) {
    for (int i = 0, n = numObjects(); i < n; i++) {
        uintptr_t o = object(i);
        if (o && objName(o) == nm && objName(get<uintptr_t>(o + oClass)) == cls
            && objName(get<uintptr_t>(o + oOuter)) == outer) return o;
    }
    return 0;
}

// ---- reflection: UStruct / FField offsets detected, not assumed ----
// MTCargo is the reference class: any of these names proves a pointer is one of
// its FFields. A set, not "the first property", because order is not guaranteed.
static int oSuper = -1, oChildProps = -1, fNext = -1, fName = -1, fOffset = -1;

static bool cargoField(const std::string& s) {
    static const char* k[] = { "bCanPickup", "bCanAutoload", "Mesh", "Net_CargoKey", "Net_Weight", "Net_Damage",
        "Net_Payment", "Net_SingleCargoPayment", "Net_DeliveryId", "Net_DestinationLocation",
        "Net_SenderAbsoluteLocation", "EmptyContainerMass", "Marker", "DroppedMarker" };
    for (auto n : k) if (s == n) return true;
    return false;
}
static void hexdump(const char* what, uintptr_t p, int n) {
    for (int i = 0; i < n; i += 8) {
        uintptr_t v = get<uintptr_t>(p + i);
        logf("  %s+%02x = %016llx  %s", what, i, v, name(get<uint32_t>(p + i)).c_str());
    }
}
static bool learnLayout(uintptr_t cargoCls, uintptr_t actorCls, bool verbose) {
    for (int s = 0x28; s <= 0x80 && oSuper < 0; s += 8)
        if (actorCls && get<uintptr_t>(cargoCls + s) == actorCls) oSuper = s;
    for (int c = 0x28; c <= 0xC0 && oChildProps < 0; c += 8) {
        uintptr_t f = get<uintptr_t>(cargoCls + c);
        for (int n = 0x08; n <= 0x48 && f; n += 4)
            if (cargoField(name(get<uint32_t>(f + n)))) { oChildProps = c; fName = n; break; }
    }
    if (oSuper < 0 || oChildProps < 0) {
        if (verbose) hexdump("MTCargo", cargoCls, 0xC8);
        return false;
    }
    uintptr_t first = get<uintptr_t>(cargoCls + oChildProps);
    int best = 0;
    for (int x = 0x08; x <= 0x40; x += 8) {
        int hits = 0;
        for (uintptr_t f = first; f && hits < 64; f = get<uintptr_t>(f + x)) {
            if (!cargoField(name(get<uint32_t>(f + fName))) && hits == 0 && f != first) break;
            hits++;
        }
        if (hits > best) { best = hits; fNext = x; }
    }
    logf("layout: super+%x childprops+%x name+%x next+%x walks %d fields", oSuper, oChildProps, fName, fNext, best);
    if (best < 10 && verbose) hexdump("FField", first, 0x60);
    return best >= 10;
}
static std::unordered_map<std::string, uintptr_t> fields(uintptr_t cls) {
    std::unordered_map<std::string, uintptr_t> m;
    for (uintptr_t f = get<uintptr_t>(cls + oChildProps); f && m.size() < 512; f = get<uintptr_t>(f + fNext))
        m[name(get<uint32_t>(f + fName))] = f;
    return m;
}
// Offset_Internal: the int32 at which Net_Payment sits exactly 16 bytes past
// Net_SingleCargoPayment (two adjacent MTShadowedInt64) -- unique in practice.
static bool learnOffset(std::unordered_map<std::string, uintptr_t>& m) {
    uintptr_t a = m["Net_SingleCargoPayment"], b = m["Net_Payment"], w = m["Net_Weight"], d = m["Net_Damage"];
    if (!a || !b || !w || !d) return false;
    for (int x = 0x20; x <= 0x80; x += 4) {
        int pa = get<int>(a + x), pb = get<int>(b + x), pw = get<int>(w + x), pd = get<int>(d + x);
        if (pa > 0x200 && pb == pa + 16 && pd == pw + 4) { fOffset = x; return true; }
    }
    hexdump("Net_Payment", b, 0x80);
    return false;
}
static bool isA(uintptr_t cls, uintptr_t target) {
    for (int i = 0; cls && i < 32; i++, cls = get<uintptr_t>(cls + oSuper))
        if (cls == target) return true;
    return false;
}
static uintptr_t fieldOf(uintptr_t cls, const char* n) {
    for (int i = 0; cls && i < 32; i++, cls = get<uintptr_t>(cls + oSuper)) {  // inherited ones live on the super
        auto fm = fields(cls);
        auto it = fm.find(n);
        if (it != fm.end()) return it->second;
    }
    return 0;
}

// ---- .usmap export (version 4, uncompressed: what UAssetAPI and our tools read) ----
struct Usmap {
    std::vector<std::string> names; std::unordered_map<std::string, int> idx;
    std::string body;
    int D = -1, enumNames = -1;            // FProperty subclass data start, UEnum::Names
    uintptr_t enumCls = 0, structCls = 0, funcCls = 0;
    std::unordered_map<std::string, int> unknownTypes;
    int n(const std::string& s) {
        auto it = idx.find(s); if (it != idx.end()) return it->second;
        idx[s] = (int)names.size(); names.push_back(s); return (int)names.size() - 1;
    }
    template <class T> void put(T v) { body.append((const char*)&v, sizeof v); }
    static std::string fieldClass(uintptr_t f) { return name(get<uint32_t>(get<uintptr_t>(f + 8))); }
    void type(uintptr_t f, int depth = 0) {
        static const std::unordered_map<std::string, uint8_t> ids = {
            {"ByteProperty",0},{"BoolProperty",1},{"IntProperty",2},{"FloatProperty",3},{"ObjectProperty",4},
            {"ObjectPtrProperty",4},{"ClassProperty",4},{"ClassPtrProperty",4},{"NameProperty",5},{"DelegateProperty",6},
            {"DoubleProperty",7},{"ArrayProperty",8},{"StructProperty",9},{"StrProperty",10},{"TextProperty",11},
            {"InterfaceProperty",12},{"MulticastDelegateProperty",13},{"MulticastInlineDelegateProperty",13},
            {"MulticastSparseDelegateProperty",13},{"WeakObjectProperty",14},{"LazyObjectProperty",15},
            {"AssetObjectProperty",16},{"SoftObjectProperty",17},{"SoftClassProperty",17},{"UInt64Property",18},
            {"UInt32Property",19},{"UInt16Property",20},{"Int64Property",21},{"Int16Property",22},{"Int8Property",23},
            {"MapProperty",24},{"SetProperty",25},{"EnumProperty",26},{"FieldPathProperty",27},{"OptionalProperty",28},
            {"Utf8StrProperty",29},{"AnsiStrProperty",30} };
        std::string c = f && depth < 8 ? fieldClass(f) : "";
        auto it = ids.find(c);
        if (it == ids.end()) { unknownTypes[c]++; put<uint8_t>(0xFF); return; }
        uint8_t id = it->second;
        if (id == 0) {  // an enum-typed byte is written as EnumProperty(ByteProperty, Enum)
            uintptr_t e = get<uintptr_t>(f + D);
            if (e && isA(get<uintptr_t>(e + oClass), enumCls)) { put<uint8_t>(26); put<uint8_t>(0); put<int>(n(fname(e + oName))); }
            else put<uint8_t>(0);
            return;
        }
        put<uint8_t>(id);
        switch (id) {
        case 9: put<int>(n(fname(get<uintptr_t>(f + D) + oName))); break;
        case 8: type(get<uintptr_t>(f + D + 8), depth + 1); break;  // FArrayProperty: ArrayFlags, then Inner
        case 25: case 28: type(get<uintptr_t>(f + D), depth + 1); break;
        case 24: type(get<uintptr_t>(f + D), depth + 1); type(get<uintptr_t>(f + D + 8), depth + 1); break;
        case 26: type(get<uintptr_t>(f + D), depth + 1); put<int>(n(fname(get<uintptr_t>(f + D + 8) + oName))); break;
        }
    }
    bool write(const wchar_t* path, int& nEnums, int& nStructs) {
        std::vector<uintptr_t> enums, structs;
        for (int i = 0, c = numObjects(); i < c; i++) {
            uintptr_t o = object(i);
            if (!o || (get<uint32_t>(o + oFlags) & 0x10)) continue;
            uintptr_t k = get<uintptr_t>(o + oClass);
            if (isA(k, enumCls)) enums.push_back(o);
            else if (isA(k, structCls) && !isA(k, funcCls)) structs.push_back(o);
        }
        put<int>((int)enums.size());
        for (uintptr_t e : enums) {
            put<int>(n(fname(e + oName)));
            uintptr_t data = get<uintptr_t>(e + enumNames); int cnt = get<int>(e + enumNames + 8);
            if (cnt < 0 || cnt > 32767) cnt = 0;
            put<int16_t>((int16_t)cnt);
            for (int j = 0; j < cnt; j++) {
                std::string v = fname(data + 16ull * j);
                auto sep = v.rfind("::"); if (sep != std::string::npos) v = v.substr(sep + 2);
                put<int64_t>(get<int64_t>(data + 16ull * j + 8)); put<int>(n(v));
            }
        }
        put<int>((int)structs.size());
        for (uintptr_t st : structs) {
            put<int>(n(fname(st + oName)));
            uintptr_t sup = get<uintptr_t>(st + oSuper);
            put<int>(sup ? n(fname(sup + oName)) : -1);
            std::vector<uintptr_t> props;
            for (uintptr_t f = get<uintptr_t>(st + oChildProps); f && props.size() < 4096; f = get<uintptr_t>(f + fNext)) props.push_back(f);
            int total = 0;
            for (uintptr_t f : props) { int d = get<int>(f + 0x30); total += d > 0 && d < 256 ? d : 1; }
            put<uint16_t>((uint16_t)total); put<uint16_t>((uint16_t)props.size());
            int at = 0;
            for (uintptr_t f : props) {
                int d = get<int>(f + 0x30); if (d <= 0 || d >= 256) d = 1;
                put<uint16_t>((uint16_t)at); put<uint8_t>((uint8_t)d); put<int>(n(fname(f + fName)));
                type(f); at += d;
            }
        }
        nEnums = (int)enums.size(); nStructs = (int)structs.size();
        std::string out, nm;
        uint32_t count = (uint32_t)names.size(); nm.append((const char*)&count, 4);
        for (auto& s2 : names) { int16_t l = (int16_t)s2.size(); nm.append((const char*)&l, 2); nm += s2; }
        std::string payload = nm + body;
        uint32_t sz = (uint32_t)payload.size();
        out += "\xC4\x30"; out += (char)4;                // magic, version ExplicitEnumValues
        int32_t noVer = 0; out.append((const char*)&noVer, 4);
        out += (char)0;                                   // no compression
        out.append((const char*)&sz, 4); out.append((const char*)&sz, 4);
        out += payload;
        FILE* fp = _wfopen(path, L"wb");
        if (!fp) return false;
        fwrite(out.data(), 1, out.size(), fp); fclose(fp);
        return true;
    }
};

static void dumpMappings(std::unordered_map<std::string, uintptr_t>& cargoFields) {
    Usmap u;
    u.enumCls = findObject("Class", "Enum", "/Script/CoreUObject");
    u.structCls = findObject("Class", "Struct", "/Script/CoreUObject");
    u.funcCls = findObject("Class", "Function", "/Script/CoreUObject");
    uintptr_t np = cargoFields["Net_Payment"];
    for (int x = 0x48; x <= 0xA0 && u.D < 0; x += 8)
        if (fname(get<uintptr_t>(np + x) + oName) == "MTShadowedInt64") u.D = x;
    uintptr_t eSpace = findObject("Enum", "EMTCargoSpaceType", "/Script/MotorTown");
    for (int x = 0x30; x <= 0x80 && u.enumNames < 0 && eSpace; x += 8) {
        uintptr_t data = get<uintptr_t>(eSpace + x); int cnt = get<int>(eSpace + x + 8);
        for (int j = 0; j < cnt && j < 64; j++)
            if (fname(data + 16ull * j).find("Flatbed") != std::string::npos) { u.enumNames = x; break; }
    }
    logf("mappings: enum %p struct %p function %p | property data+%x, enum names+%x",
         (void*)u.enumCls, (void*)u.structCls, (void*)u.funcCls, u.D, u.enumNames);
    if (!u.enumCls || !u.structCls || !u.funcCls || u.D < 0 || u.enumNames < 0) { logf("mappings: FAILED, layout not recognised"); return; }
    std::wstring path = exeDir() + L"Mappings.usmap";
    int ne = 0, ns = 0;
    bool ok = u.write(path.c_str(), ne, ns);
    logf("mappings: %s Mappings.usmap: %d enums, %d structs, %zu names", ok ? "wrote" : "FAILED to write", ne, ns, u.names.size());
    for (auto& kv : u.unknownTypes) logf("mappings: unknown property type '%s' x%d", kv.first.c_str(), kv.second);
}

// ---- the one ProcessEvent hook, and the game-thread queue it drains ----
typedef void(__fastcall* ProcessEventFn)(uintptr_t self, uintptr_t func, void* parms, uintptr_t r9);
static ProcessEventFn g_pe;
static int g_peIndex = -1;
static CRITICAL_SECTION g_queueLock;
static std::vector<std::pair<MtGameThreadFn, void*>> g_queue;
static volatile LONG g_queued;

static bool inText(uintptr_t a) { Range t = section(".text"); return a >= t.lo && a < t.hi; }

// ProcessEvent is the vtable entry whose code tests UFunction::FunctionFlags for both
// FUNC_Native (0x400) and FUNC_HasOutParms (0x400000) at the same offset.
static int findProcessEvent(uintptr_t anyObject) {
    uintptr_t vt = get<uintptr_t>(anyObject);
    for (int i = 0; i < 0x180; i++) {
        uintptr_t fn = get<uintptr_t>(vt + 8ull * i);
        if (!inText(fn)) break;
        for (int j = 0; j < 2 && get<uint8_t>(fn) == 0xE9; j++) fn = fn + 5 + get<int32_t>(fn + 1);
        uint8_t code[0x400];
        if (!rd(fn, code, sizeof code)) continue;
        int dNative = -1, dOut = -2;
        for (int k = 0; k + 10 <= (int)sizeof code; k++) {
            if (code[k] != 0xF7) continue;
            uint8_t m = code[k + 1];
            if ((m >> 6) != 2 || ((m >> 3) & 7) != 0 || (m & 7) == 4) continue;  // test [reg+disp32], imm32
            int32_t disp = *(int32_t*)(code + k + 2); uint32_t imm = *(uint32_t*)(code + k + 6);
            if (imm == 0x400) dNative = disp;
            if (imm == 0x400000) dOut = disp;
        }
        if (dNative == dOut && dNative > 0x40 && dNative < 0x200) return i;
    }
    return -1;
}

// ---- function watchers: a plugin gets called back when a UFunction fires ----
// A fixed table, published by count: the game thread reads it without a lock.
// Plugins register from their own threads, so appends take g_watchLock. Cheap
// enough for the hot path -- one compare per watcher, and only when a watcher
// is armed.
static CRITICAL_SECTION g_watchLock;
static MtPeHookFn g_watchFn[4096];
static void* g_watchCtx[4096];
static uint32_t g_watchIdx[4096];
static uint32_t g_watchNum[4096];
static volatile LONG g_watchCount = 0;

// The FName of a UFunction, found through the object table: a UFunction's class
// is /Script/CoreUObject.Function, and the function's own name is the pool
// index the hook compares against.
static bool funcFName(const char* nm, uint32_t* idx, uint32_t* num) {
    uintptr_t fc = findObject("Class", "Function", "/Script/CoreUObject");
    if (!fc) return false;
    for (int i = 0, n = numObjects(); i < n; i++) {
        uintptr_t o = object(i);
        if (!o || get<uintptr_t>(o + oClass) != fc) continue;
        uint32_t ix = get<uint32_t>(o + oName);
        if (name(ix) != nm) continue;
        *idx = ix; *num = get<uint32_t>(o + oName + 4);
        return true;
    }
    return false;
}

static void __fastcall hookPE(uintptr_t self, uintptr_t func, void* parms, uintptr_t r9) {
    if (g_queued && GetCurrentThreadId() == g_gameThread && InterlockedExchange(&g_queued, 0)) {
        std::vector<std::pair<MtGameThreadFn, void*>> work;
        EnterCriticalSection(&g_queueLock); work.swap(g_queue); LeaveCriticalSection(&g_queueLock);
        for (auto& w : work) w.first(w.second);
    }
    LONG n = g_watchCount;
    if (n > 0) {
        uint32_t idx = get<uint32_t>(func + oName), num = get<uint32_t>(func + oName + 4);
        for (LONG i = 0; i < n; i++)
            if (g_watchIdx[i] == idx && g_watchNum[i] == num) {
                // Where the call came from: the return address of THIS function is
                // the address the caller will resume at, i.e. inside the native
                // code that called ProcessEvent. That is the address a detour
                // would have to target.
                logf("watch fire: %s from %p", name(idx).c_str(), (void*)_ReturnAddress());
                if (g_watchFn[i](self, func, parms, g_watchCtx[i]))
                    return;  // a watcher blocked the call: do not run the original
            }
    }
    g_pe(self, func, parms, r9);
}

// Swap every vtable slot in .rdata that points at ProcessEvent: classes that do not
// override it all share the one function, so the game thread hits the hook each frame.
static int installHook(uintptr_t anyObject) {
    g_peIndex = findProcessEvent(anyObject);
    if (g_peIndex < 0) return 0;
    ProcessEventFn pe = (ProcessEventFn)get<uintptr_t>(get<uintptr_t>(anyObject) + 8ull * g_peIndex);
    if (pe == (ProcessEventFn)&hookPE) return 0;
    g_pe = pe;
    Range r = section(".rdata");
    int n = 0;
    for (uintptr_t a = r.lo; a + 8 <= r.hi; a += 8) {
        if (get<uintptr_t>(a) != (uintptr_t)g_pe) continue;
        DWORD old;
        if (!VirtualProtect((void*)a, 8, PAGE_READWRITE, &old)) continue;
        *(uintptr_t*)a = (uintptr_t)&hookPE;
        VirtualProtect((void*)a, 8, old, &old);
        n++;
    }
    return n;
}

  // ---- console variables, exported next to the mappings ----
  // The game writes the console variables it runs with into its own config,
  // and MTNet (as a plugin) writes the catalogue the server sent into
  // mt_net_cvars.txt when it sees the config. Both land in one file here, so
  // a modeller has a single place to look and to change them.
  static std::wstring projectName() {
      std::wstring d = exeDir();                      // ...MotorTown, Binaries, Win64
      size_t cut = d.find(L"\\Binaries\\");
      if (cut == std::wstring::npos) return L"MotorTown";
      d = d.substr(0, cut);                          // ...MotorTown
      size_t slash = d.find_last_of(L'\\');
      return slash == std::wstring::npos ? d : d.substr(slash + 1);
  }

  static std::wstring userConfigDir() {
      const char* local = getenv("LOCALAPPDATA");
      if (!local) return L"";
      return widen(local) + L"\\" + projectName() + L"\\Saved\\Config\\Windows";
  }

  static int dumpCvars() {
      std::wstring out = exeDir() + L"mt_loader_cvars.txt";
      FILE* f = _wfopen(out.c_str(), L"w");
      if (!f) return 0;
      std::set<std::string> seen;
      int n = 0;
      fprintf(f, "; the console variables, exported by the plugin loader\n");
      // The catalogue the server sent (MTNet writes it), then what the game
      // itself wrote -- the live value wins over the catalogue's.
      {
          std::wstring path = exeDir() + L"mt_net_cvars.txt";
          FILE* m = _wfopen(path.c_str(), L"r");
          if (m) {
              char line[512];
              while (fgets(line, sizeof line, m)) {
                  if (line[0] == ';' || !strchr(line, '=')) continue;
                  std::string key(line, strchr(line, '='));
                  if (!seen.insert(key).second) continue;
                  fprintf(f, "%s", line);
                  n++;
              }
              fclose(m);
          }
      }
      {
          std::wstring dir = userConfigDir();
          if (!dir.empty()) {
              for (std::wstring& line : ini::sectionLines(dir + L"\\Engine.ini", L"ConsoleVariables")) {
                  size_t eq = line.find(L'=');
                  if (eq == std::wstring::npos) continue;
                  if (!seen.insert(narrow(line.substr(0, eq))).second) continue;
                  fprintf(f, "%ls\n", line.c_str());
                  n++;
              }
          }
      }
      fclose(f);
      return n;
  }

  // ---- engine discovery, once ----
static HANDLE g_engineReady;  // manual-reset event
static MtLayout g_layout;

static int iniInt(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring ini = exeDir() + L"mt_loader.ini";
    wchar_t b[64];
    if (!GetPrivateProfileStringW(sec, key, L"", b, 64, ini.c_str())) {
        swprintf(b, 64, L"%d", def); WritePrivateProfileStringW(sec, key, b, ini.c_str()); return def;
    }
    return _wtoi(b);
}

static DWORD WINAPI discover(LPVOID) {
    while (!findNamePool()) Sleep(1000);
    logf("name pool at +0x%llx", g_blocks - (uintptr_t)GetModuleHandleW(nullptr));
    while (!findObjectArray()) Sleep(1000);
    logf("object array found, stride %d, %d objects", g_stride, numObjects());

    uintptr_t cargo = 0;
    while (!(cargo = findObject("Class", "MTCargo", "/Script/MotorTown"))) Sleep(2000);
    uintptr_t actor = findObject("Class", "Actor", "/Script/Engine");
    // The class exists before its fields are linked; wait for them (up to ~5 min).
    for (int tries = 0; !learnLayout(cargo, actor, tries == 150); tries++) {
        if (tries == 150) { logf("FAILED: UStruct/FField layout not recognised -- plugins get no engine"); return 0; }
        oSuper = oChildProps = fNext = fName = -1;
        Sleep(2000);
    }
    auto m = fields(cargo);
    if (!learnOffset(m)) { logf("FAILED: property Offset_Internal not found -- plugins get no engine"); return 0; }
    g_layout = { oFlags, oIndex, oClass, oName, oOuter, oSuper, oChildProps, fNext, fName, fOffset, g_stride };
      logf("engine ready: super+%x props+%x next+%x name+%x offset+%x", oSuper, oChildProps, fNext, fName, fOffset);
      SetEvent(g_engineReady);

      // The hook goes in NOW, not after the world is up. ProcessEvent is the
      // only way onto the game thread, and a plugin that wants to touch the
      // game before the first delivery has arrived (a key that opens a panel,
      // say) would otherwise wait for a world that is not coming.
      {
          int slots = installHook(cargo);
          logf("ProcessEvent: vtable index %d, %d slots hooked -- game-thread work %s",
               g_peIndex, slots, slots ? "runs" : "DISABLED");
      }

    // The world is up once MTDeliverySystem exists: the classes it uses are loaded.
    uintptr_t sysCls = findObject("Class", "MTDeliverySystem", "/Script/MotorTown");
    for (bool up = false; !up; ) {
        Sleep(1000);
        for (int i = 0, n = numObjects(); i < n && !up; i++) {
            uintptr_t o = object(i);
            up = o && !(get<uint32_t>(o + oFlags) & 0x10) && get<uintptr_t>(o + oClass) == sysCls;
        }
    }
    Sleep(10000);
    // Capitalist Economy owned this switch before the loader did; inherit its value.
    // Through the store, so the read cannot land in the middle of a plugin's write.
    std::wstring mb;
    g_ini.get(L"capitalist_economy.ini", L"Mappings", L"Export", mb);
      int was = _wtoi(mb.c_str());
      if (iniInt(L"Mappings", L"Export", was)) dumpMappings(m);
      // One file next to the mappings, for a modeller to look at and change.
      if (iniInt(L"Mappings", L"ExportCvars", 0)) logf("cvars: %d exported to mt_loader_cvars.txt", dumpCvars());
      return 0;
  }

  // ---- the api ----
  struct Plugin { std::string name; FILE* log; int order; MtApi api; };

  static void apiLog(const MtApi* self, const char* fmt, ...) {
      va_list a; va_start(a, fmt); vlog(((Plugin*)self->plugin)->log, fmt, a); va_end(a);
  }
static int apiWait(uint32_t ms) { return WaitForSingleObject(g_engineReady, ms) == WAIT_OBJECT_0; }
static int apiRead(uintptr_t p, void* out, size_t n) { return rd(p, out, n); }
static int apiWrite(uintptr_t p, const void* in, size_t n) { return wr(p, in, n); }
static int copyOut(const std::string& s, char* buf, int cap) {
    if (!buf || cap <= 0) return 0;
    int n = (int)std::min<size_t>(s.size(), (size_t)cap - 1);
    memcpy(buf, s.data(), n); buf[n] = 0;
    return n;
}
static int apiName(uint32_t i, char* buf, int cap) { return copyOut(name(i), buf, cap); }
static int apiFname(uintptr_t at, char* buf, int cap) { return copyOut(fname(at), buf, cap); }
  static int apiNameIndex(const char* nm) { return (int)nameIndexOf(nm); }
  static int apiPoolNames(int (*fn)(const char*, void*), void* ctx) { return poolNames(fn, ctx); }

// Overwrite bytes in the game's code. The page is made writable for the write
// and restored afterwards, and the instruction cache is flushed so the change
// takes effect immediately.
static int apiPatch(uintptr_t addr, const void* bytes, int len) {
    if (!bytes || len <= 0 || addr < 0x10000) return 0;
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)addr, bytes, (size_t)len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, (SIZE_T)len);
    DWORD restore = 0;
    VirtualProtect((void*)addr, (SIZE_T)len, old, &restore);
    return 1;
}

  static uintptr_t apiModuleBase() { return (uintptr_t)GetModuleHandleW(nullptr); }
  static int apiNum() { return numObjects(); }
  static uintptr_t apiObject(int i) { return object(i); }
  static uintptr_t apiFind(const char* c, const char* n, const char* o) { return findObject(c, n, o); }
  static int apiIsA(uintptr_t c, uintptr_t t) { return isA(c, t); }
  static uintptr_t apiField(uintptr_t c, const char* n) { return fieldOf(c, n); }
  static int apiFieldOffset(uintptr_t c, const char* n) { uintptr_t f = fieldOf(c, n); return f ? get<int>(f + fOffset, -1) : -1; }

  // Declare a value for a key in one of the loader's ini files. The file's
  // own value wins over a default (the user edits these by hand), and the
  // plugin that loaded first wins over a later one -- the conflict is logged,
  // with both values.
  static int apiIniSet(const MtApi* self, const wchar_t* file, const wchar_t* section, const wchar_t* key,
                       const wchar_t* value, int force) {
      if (!file || !section || !key || !value) return 0;
      Plugin* p = self ? (Plugin*)self->plugin : nullptr;
      std::wstring owner = p ? widen(p->name) : L"?";
      int order = p ? p->order : 0;
      std::wstring conflict;
      ini::Store::Result r = g_ini.declare(file, section, key, value, force != 0, owner, order, &conflict);
      if (!conflict.empty()) iniLog(conflict.c_str());
      return r == ini::Store::APPLIED;
  }
  static int apiIniGet(const wchar_t* file, const wchar_t* section, const wchar_t* key, wchar_t* buf, int cap) {
      if (!file || !section || !key || !buf || cap <= 0) return 0;
      std::wstring v;
      if (!g_ini.get(file, section, key, v)) { buf[0] = 0; return 0; }
      int n = (int)std::min<size_t>(v.size(), (size_t)cap - 1);
      memcpy(buf, v.data(), n * sizeof(wchar_t)); buf[n] = 0;
      return n;
  }
static int apiRun(MtGameThreadFn fn, void* ctx) {
    if (!fn) return 0;
    EnterCriticalSection(&g_queueLock); g_queue.emplace_back(fn, ctx); LeaveCriticalSection(&g_queueLock);
    InterlockedExchange(&g_queued, 1);
    return 1;
}
static void apiPE(uintptr_t obj, uintptr_t func, void* parms) { if (g_pe) g_pe(obj, func, parms, 0); }
static int apiHooked() { return g_pe != nullptr; }

static int apiWatch(const char* const* names, int count, MtPeHookFn fn, void* ctx) {
    if (!names || count <= 0 || !fn) return 0;
    int added = 0;
    for (int i = 0; i < count; i++) {
        uint32_t idx = 0, num = 0;
        if (!names[i] || !funcFName(names[i], &idx, &num)) {
            logf("watch: '%s' is not loaded -- not registered", names[i] ? names[i] : "?");
            continue;
        }
        EnterCriticalSection(&g_watchLock);
        LONG n = g_watchCount;
        bool dup = false;
        // The same callback again is a no-op, so a plugin can retry a whole list.
        // Another callback on the same function is a separate watcher.
        for (LONG k = 0; k < n; k++)
            if (g_watchIdx[k] == idx && g_watchNum[k] == num && g_watchFn[k] == fn && g_watchCtx[k] == ctx) { dup = true; break; }
        bool full = !dup && n >= 4096;
        if (!dup && !full) {
            g_watchIdx[n] = idx; g_watchNum[n] = num; g_watchFn[n] = fn; g_watchCtx[n] = ctx;
            InterlockedExchange(&g_watchCount, n + 1);   // publishes the entry
        }
        LeaveCriticalSection(&g_watchLock);
        if (full) { logf("watch: table full -- '%s' not registered", names[i]); break; }
        if (!dup) logf("watch: '%s' -> FName %u:%u", names[i], idx, num);
        added++;
    }
    return added ? 1 : 0;
}

// ---- plugins: plugins\mt_plugin_*.dll, alphabetical ----
static DWORD WINAPI loadPlugins(LPVOID) {
    std::wstring dir = exeDir() + L"plugins\\";
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"mt_plugin_*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    logf("%zu plugin(s) in plugins\\ (mt_plugin_*.dll, alphabetical)", files.size());
    int order = 0;
    for (auto& f : files) {
        std::wstring path = dir + f;
        std::string nm;
        for (wchar_t c : f) nm += (char)c;
        // Look for PluginInit before running anything: mapped this way, the DLL's
        // DllMain does not run. A DLL that does its work from DllMain must never be
        // loaded and then freed -- its threads would run in unmapped code, which
        // is how a DllMain-style probe once took the game down.
        HMODULE peek = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        bool conforms = peek && GetProcAddress(peek, "PluginInit");
        if (peek) FreeLibrary(peek);
        if (!conforms) { logf("plugin %s: no PluginInit export -- skipped, not run", nm.c_str()); continue; }
        HMODULE mod = LoadLibraryW(path.c_str());
        if (!mod) { logf("plugin %s: LoadLibrary failed (%lu)", nm.c_str(), GetLastError()); continue; }
        auto init = (MtPluginInitFn)GetProcAddress(mod, "PluginInit");
        if (!init) { logf("plugin %s: PluginInit vanished on load", nm.c_str()); continue; }
        auto* p = new Plugin{ nm, nullptr, order++, {} };  // lives for the process
        std::wstring logPath = path.substr(0, path.size() - 4) + L".log";
        p->log = logsOn() ? _wfopen(logPath.c_str(), L"w") : nullptr;
        p->api = { MT_API_VERSION, sizeof(MtApi), apiLog, apiWait, &g_layout, apiRead, apiWrite, apiName, apiFname,
                     apiNum, apiObject, apiFind, apiIsA, apiField, apiFieldOffset, apiRun, apiPE, apiHooked, p, apiWatch, apiNameIndex, apiPatch, apiModuleBase, apiIniSet, apiIniGet, apiPoolNames };
        int rc = init(&p->api);
        logf("plugin %s: PluginInit -> %d", nm.c_str(), rc);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        g_gameThread = GetCurrentThreadId();
        InitializeCriticalSection(&g_queueLock);
        InitializeCriticalSection(&g_watchLock);
        g_engineReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_log = logsOn() ? _wfopen((exeDir() + L"mt_loader.log").c_str(), L"w") : nullptr;
        // The loader owns the ini files the plugins write, so two plugins that
        // set different keys of the same file cannot overwrite each other.
        g_ini.init(exeDir(), iniLog);
        logf("Motor Town plugin loader, api v%d", MT_API_VERSION);
        // Both run once DllMain returns and the loader lock is free.
        CloseHandle(CreateThread(nullptr, 0, discover, nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, loadPlugins, nullptr, 0, nullptr));
    }
    return TRUE;
}
