// Windows: the icon and the version details built into the programs (src/platform/tinydaw_win.rc). Tests are linked
// with the same resources as tinydaw.exe: this one looks at its own, then at tinydaw.exe's and the plugin host's.
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "check.h"

static int iconsIn(const char* exe) {
    int n = (int)ExtractIconExA(exe, -1, nullptr, nullptr, 0);  // (-1: how many icons the file holds)
    fprintf(stderr, "%s: %d icon(s)\n", exe, n);
    return n;
}

// how many different opaque colours an icon's image of `side` px holds (a flat placeholder: one)
static int distinctColours(HICON icon, int side) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return 0;
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof format.bmiHeader;
    format.bmiHeader.biWidth = side;
    format.bmiHeader.biHeight = -side;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> pixels((size_t)side * side);
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, info.hbmColor, 0, side, pixels.data(), &format, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    std::set<uint32_t> colours;
    for (uint32_t pixel : pixels)
        if ((pixel >> 24) == 0xff) colours.insert(pixel & 0xffffff);
    return (int)colours.size();
}

// the widths in px of the images icon group 1 holds (the resource's directory: a 6-byte header, then 14 bytes
// per image, whose first byte is its width, 0 meaning 256)
static std::set<int> sizesInIconGroup(HMODULE module) {
    std::set<int> sizes;
    HRSRC group = FindResourceW(module, MAKEINTRESOURCEW(1), (LPCWSTR)RT_GROUP_ICON);
    if (!group) return sizes;
    const unsigned char* data = (const unsigned char*)LockResource(LoadResource(module, group));
    DWORD length = SizeofResource(module, group);
    if (!data || length < 6) return sizes;
    int count = data[4] | (data[5] << 8);
    for (int image = 0; image < count && 6 + 14 * (DWORD)(image + 1) <= length; image++) {
        int width = data[6 + 14 * image];
        sizes.insert(width == 0 ? 256 : width);
    }
    return sizes;
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
    // the program's logo, not a placeholder of one flat colour: its 32 px image holds many colours (the
    // logo's gradients) and both larger sizes for high-DPI screens are there
    HICON medium = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    CHECK(medium != nullptr);
    if (medium) CHECK(distinctColours(medium, 32) >= 20);
    std::set<int> sizes = sizesInIconGroup(self);
    for (int size : {16, 32, 48, 256}) CHECK(sizes.count(size) == 1);  // (an image of its own at each size)
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
