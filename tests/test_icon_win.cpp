// Windows: the icon and the version details built into the programs (src/platform/tinydaw_win.rc). Tests are linked
// with the same resources as tinydaw.exe: this one looks at its own, then at tinydaw.exe's and the plugin host's.
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <string>

#include "check.h"

static int iconsIn(const char* exe) {
    int n = (int)ExtractIconExA(exe, -1, nullptr, nullptr, 0);  // (-1: how many icons the file holds)
    fprintf(stderr, "%s: %d icon(s)\n", exe, n);
    return n;
}

int main() {
    HMODULE self = GetModuleHandleW(nullptr);
    // the icon, at the sizes the window class asks for
    CHECK(FindResourceW(self, MAKEINTRESOURCEW(1), (LPCWSTR)RT_GROUP_ICON) != nullptr);
    HICON big = LoadIconW(self, MAKEINTRESOURCEW(1));
    HICON small = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    CHECK(big && small);
    ICONINFO info{};
    if (small && GetIconInfo(small, &info)) {
        BITMAP bm{};
        GetObjectW(info.hbmColor, sizeof bm, &bm);
        CHECK(bm.bmWidth == 16 && bm.bmHeight == 16);  // (its own 16 px image, not a scaled 256)
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
    }
    // the version details: the product's name
    HRSRC v = FindResourceW(self, MAKEINTRESOURCEW(1), (LPCWSTR)RT_VERSION);
    CHECK(v != nullptr);
    if (v) {
        const char* data = (const char*)LockResource(LoadResource(self, v));
        DWORD size = SizeofResource(self, v);
        std::wstring block((const wchar_t*)data, size / sizeof(wchar_t));
        CHECK(block.find(L"ProductName") != std::wstring::npos && block.find(L"Tiny DAW") != std::wstring::npos);
    }
    // the programs Explorer shows (where they're built: make tests-win builds them first)
    if (GetFileAttributesA("tinydaw.exe") != INVALID_FILE_ATTRIBUTES) CHECK(iconsIn("tinydaw.exe") >= 1);
    if (GetFileAttributesA("tinydaw-plugin-host.exe") != INVALID_FILE_ATTRIBUTES) CHECK(iconsIn("tinydaw-plugin-host.exe") >= 1);
    TEST_MAIN_END();
}
