// A WaitForVBlank for Wine's DXGI (see vblank_wait.h, dxgi_fix.c).
#include "winefix/vblank_wait.h"

#include <math.h>

enum { UNKNOWN_RELEASE = 2, FACTORY_ENUM_ADAPTERS = 7, ADAPTER_ENUM_OUTPUTS = 7, OUTPUT_WAIT_FOR_VBLANK = 10 };
static const double REFRESH_HZ = 60;  // (Wine can't tell the screen's own rate here)

// the method at `index` of a COM object's table
static void* methodOf(void* object, int index) { return (*(void***)object)[index]; }

// lets go of a COM object (NULL: nothing)
static void releaseObject(void* object) {
    if (object)
        ((ULONG(WINAPI*)(void*))methodOf(object, UNKNOWN_RELEASE))(object);
}

// is this address inside the module's image?
static int insideModule(HMODULE module, void* address) {
    const unsigned char* base = (const unsigned char*)module;
    DWORD headerOffset = *(const DWORD*)(base + 0x3C);
    DWORD imageSize = *(const DWORD*)(base + headerOffset + 24 + 56);  // (OptionalHeader.SizeOfImage)
    return (const unsigned char*)address >= base && (const unsigned char*)address < base + imageSize;
}

LONG WINAPI waitForNextRefresh(void* output) {
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    double seconds = (double)now.QuadPart / frequency.QuadPart, period = 1 / REFRESH_HZ;
    double nextRefresh = (floor(seconds / period) + 1) * period;
    for (;;) {
        QueryPerformanceCounter(&now);
        double left = nextRefresh - (double)now.QuadPart / frequency.QuadPart;
        if (left <= 0)
            return S_OK;
        Sleep(left > 0.002 ? (DWORD)((left - 0.001) * 1000) : 0);  // (the last millisecond: Sleep(0))
    }
}

int giveOutputsWaitForVBlank(void* factory, HMODULE wineDxgi) {
    void *adapter = NULL, *output = NULL;
    int given = 0;
    if (((LONG(WINAPI*)(void*, UINT, void**))methodOf(factory, FACTORY_ENUM_ADAPTERS))(factory, 0, &adapter) >= 0 &&
        adapter &&
        ((LONG(WINAPI*)(void*, UINT, void**))methodOf(adapter, ADAPTER_ENUM_OUTPUTS))(adapter, 0, &output) >= 0 &&
        output) {
        void** table = *(void***)output;
        void* wines = table[OUTPUT_WAIT_FOR_VBLANK];
        if (wines == (void*)waitForNextRefresh) {
            given = 1;
        } else if (insideModule(wineDxgi, wines) && ((LONG(WINAPI*)(void*))wines)(output) < 0) {
            DWORD old = 0;
            if (VirtualProtect(&table[OUTPUT_WAIT_FOR_VBLANK], sizeof(void*), PAGE_READWRITE, &old)) {
                table[OUTPUT_WAIT_FOR_VBLANK] = (void*)waitForNextRefresh;
                VirtualProtect(&table[OUTPUT_WAIT_FOR_VBLANK], sizeof(void*), old, &old);
                given = 1;
            }
        }
    }
    releaseObject(output);
    releaseObject(adapter);
    return given;
}
