// A standalone test of the loader's ini merge (src\ini_merge.h).
//
// It drives the store the way two plugins would, in a temporary folder, and
// checks the things that matter:
//
//   1. two plugins setting different keys of the same file: both survive;
//   2. two plugins setting the same key: the one that loaded first wins, and
//      the conflict is reported;
//   3. a value the user put in the file by hand is kept, not overwritten;
//   4. the rest of the file -- comments, other sections -- survives a write;
//   5. a forced value is written through at once;
//   6. a file name that is not a plain name is refused.
//
// Build and run: build.bat runs it (tools\ini_test.cpp -> out\ini_test.exe).

#include <windows.h>
#include <cstdio>
#include <string>

#include "../src/ini_merge.h"

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_fail++;
    printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
}

static void logf_(const wchar_t* m) { printf("    [log] %ls\n", m); }

// The value of a key as it sits in the file, straight from the disk.
static std::wstring onDisk(const std::wstring& dir, const wchar_t* file, const wchar_t* section, const wchar_t* key) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\%ls", dir.c_str(), file);
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, L"", buf, 1024, path);
    return buf;
}

static std::string wholeFile(const std::wstring& dir, const wchar_t* file) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\%ls", dir.c_str(), file);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"mt_ini_test";
    CreateDirectoryW(dir.c_str(), nullptr);

    // A file the user has already edited by hand. Written as plain bytes: the
    // profile API reads ANSI, and a UTF-16 file only if it has a BOM.
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\settings.ini", dir.c_str());
    {
        const char text[] = "; my own notes\r\n"
                            "[CompanySlots]\r\n"
                            "MaxBoughtSlots=42\r\n"
                            "\r\n"
                            "[WheelScale]\r\n"
                            "Min=0.25\r\n";
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n;
        WriteFile(h, text, (DWORD)(sizeof text - 1), &n, nullptr);
        CloseHandle(h);
    }

    ini::Store store;
    store.init(dir, logf_);

    printf("merge test in %ls\n\n", dir.c_str());

    // 1. two plugins, same file, different keys: both survive.
    {
        std::wstring conflict;
        auto r1 = store.declare(L"settings.ini", L"CompanySlots", L"MaxSpawnedPerCompany", L"100", false, L"CompanySlots.dll", 0, &conflict);
        auto r2 = store.declare(L"settings.ini", L"CompanySlots", L"MaxSpawnedPerCompany2", L"200", false, L"WheelScale.dll", 1, &conflict);
        check(r1 == ini::Store::APPLIED, "plugin 1's key is applied");
        check(r2 == ini::Store::APPLIED, "plugin 2's key is applied");
        check(onDisk(dir, L"settings.ini", L"CompanySlots", L"MaxSpawnedPerCompany") == L"100", "plugin 1's key is in the file");
        check(onDisk(dir, L"settings.ini", L"CompanySlots", L"MaxSpawnedPerCompany2") == L"200", "plugin 2's key is in the file");
        check(onDisk(dir, L"settings.ini", L"CompanySlots", L"MaxBoughtSlots") == L"42", "the user's own value is untouched");
    }

    // 2. two plugins, same key, different values: the one that loaded first wins.
    {
        std::wstring conflict;
        auto first = store.declare(L"settings.ini", L"Shared", L"Rate", L"0.5", false, L"First.dll", 0, &conflict);
        check(first == ini::Store::APPLIED, "the first plugin's key is applied");
        auto r = store.declare(L"settings.ini", L"Shared", L"Rate", L"0.9", false, L"Second.dll", 1, &conflict);
        check(r == ini::Store::CONFLICT, "the second plugin's value is refused");
        check(!conflict.empty() && conflict.find(L"Second.dll") != std::wstring::npos, "the conflict names the plugin that lost");
        check(onDisk(dir, L"settings.ini", L"Shared", L"Rate") == L"0.5", "the first plugin's value is the one in the file");
        std::wstring v;
        store.get(L"settings.ini", L"Shared", L"Rate", v);
        check(v == L"0.5", "a read sees the merged value");
    }

    // 3. a value already in the file beats a plugin's default.
    {
        std::wstring conflict;
        auto r = store.declare(L"settings.ini", L"WheelScale", L"Min", L"0.1", false, L"WheelScale.dll", 1, &conflict);
        check(r == ini::Store::ON_DISK, "the file's value wins over the plugin's default");
        check(onDisk(dir, L"settings.ini", L"WheelScale", L"Min") == L"0.25", "the user's value is still in the file");
        std::wstring v;
        store.get(L"settings.ini", L"WheelScale", L"Min", v);
        check(v == L"0.25", "a read sees the user's value, not the default");
    }

    // 4. a forced value is written through and wins.
    {
        std::wstring conflict;
        auto r = store.declare(L"settings.ini", L"Shared", L"Rate", L"0.9", true, L"First.dll", 0, &conflict);
        check(r == ini::Store::APPLIED, "a forced value is applied");
        check(onDisk(dir, L"settings.ini", L"Shared", L"Rate") == L"0.9", "a forced value is in the file at once");
    }

    // 5. the rest of the file survives.
    {
        std::string all = wholeFile(dir, L"settings.ini");
        check(all.find("MaxBoughtSlots=42") != std::string::npos, "the user's key survives");
        check(all.find("[WheelScale]") != std::string::npos, "the other section survives");
    }

    // 6. a file name that is not a plain name is refused.
    {
        std::wstring conflict;
        auto r = store.declare(L"../escape.ini", L"X", L"Y", L"1", false, L"Evil.dll", 0, &conflict);
        check(r == ini::Store::REJECTED, "a path out of the game folder is refused");
    }

    // 7. a value in a file that does not exist yet is written.
    {
        std::wstring conflict;
        auto r = store.declare(L"fresh.ini", L"New", L"Key", L"7", false, L"Fresh.dll", 0, &conflict);
        check(r == ini::Store::APPLIED, "a key in a new file is applied");
        check(onDisk(dir, L"fresh.ini", L"New", L"Key") == L"7", "the new file has the key");
    }

    // 8. plugins declare on their own threads, so the order they arrive in is
    //    not the order they loaded in. The one that loaded first must win
  //    either way.
    {
        std::wstring conflict;
        store.declare(L"order.ini", L"Sec", L"Key", L"late", false, L"Second.dll", 1, &conflict);
        auto r = store.declare(L"order.ini", L"Sec", L"Key", L"early", false, L"First.dll", 0, &conflict);
        check(r == ini::Store::APPLIED, "the plugin that loaded first wins, whichever declares first");
        check(onDisk(dir, L"order.ini", L"Sec", L"Key") == L"early", "its value is the one in the file");
        std::wstring v;
        store.get(L"order.ini", L"Sec", L"Key", v);
        check(v == L"early", "a read sees the winner's value");
    }

    // 9. a section read back out of a file: the profile API hands it over as
    //    one NUL-separated string, not one line per key.
    {
        wchar_t ini[MAX_PATH];
        swprintf(ini, MAX_PATH, L"%s\\section.ini", dir.c_str());
        WritePrivateProfileStringW(L"CVars", L"mh.one", L"1", ini);
        WritePrivateProfileStringW(L"CVars", L"mh.two", L"2.5", ini);
        WritePrivateProfileStringW(L"CVars", L"mh.three", L"three", ini);
        auto lines = ini::sectionLines(ini, L"CVars");
        check(lines.size() == 3, "a section of three keys reads back as three lines");
        bool ok = lines.size() == 3;
        for (auto& l : lines) if (l.find(L'=') == std::wstring::npos) ok = false;
        check(ok, "every line of it is a key=value pair");
        bool names = lines.size() == 3 && lines[0].find(L"mh.one") == 0 &&
                     lines[1].find(L"mh.two") == 0 && lines[2].find(L"mh.three") == 0;
        check(names, "the keys are the ones that were written");
        DeleteFileW(ini);
    }

    // 5. a file with a UTF-8 BOM (what Notepad and many editors write): the
    // profile API cannot see the first section at all, so a value the user set
    // there reads as absent -- and the loader's own write appends a second copy
    // of the section. 2026-11-16: a plugin's [General] Disabled=1 was
    // invisible, the plugin ran anyway, and it crashed the game three times.
    {
        wchar_t bom[MAX_PATH];
        swprintf(bom, MAX_PATH, L"%s\\bom.ini", dir.c_str());
        {
            const unsigned char body[] = {0xEF, 0xBB, 0xBF, '[', 'G', 'e', 'n', 'e', 'r', 'a', 'l', ']',
                                          '\r', '\n', 'D', 'i', 's', 'a', 'b', 'l', 'e', 'd', '=', '1',
                                          '\r', '\n', '[', 'O', 't', 'h', 'e', 'r', ']', '\r', '\n',
                                          'K', 'e', 'y', '=', 'k', 'e', 'e', 'p', '\r', '\n', 0};
            HANDLE h = CreateFileW(bom, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD n = 0;
            WriteFile(h, body, (DWORD)(sizeof body - 1), &n, nullptr);
            CloseHandle(h);
        }
        std::wstring v;
        bool saw = store.get(L"bom.ini", L"General", L"Disabled", v);
        check(saw && v == L"1", "a value in the first section of a BOM'd file is read");
        store.declare(L"bom.ini", L"General", L"Disabled", L"0", false, L"Test.dll", 0, nullptr);
        check(onDisk(dir, L"bom.ini", L"General", L"Disabled") == L"1", "the user's value in a BOM'd file is not overwritten");
        check(onDisk(dir, L"bom.ini", L"Other", L"Key") == L"keep", "the other section of a BOM'd file survives");
        DeleteFileW(bom);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    // Leave no litter behind.
    for (const wchar_t* f : {L"settings.ini", L"fresh.ini", L"order.ini", L"bom.ini"}) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\%ls", dir.c_str(), f);
        DeleteFileW(p);
    }
    RemoveDirectoryW(dir.c_str());
    return g_fail ? 1 : 0;
}
