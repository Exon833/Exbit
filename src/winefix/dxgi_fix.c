// Exbit's dxgi.dll for Wine prefixes (see wine_own_dll.h): Wine's own dxgi.dll, with a WaitForVBlank.
//
// Wine's IDXGIOutput::WaitForVBlank is a stub (E_NOTIMPL at once). JUCE 8 draws after a thread of its
// own that waits for each vertical blank, and that thread looks whether it should end only after a
// wait that succeeded: with Wine's it never ends, and whatever waits for it to end (a window closing,
// a program's window being made again) waits forever. Quanta 2's installer (JUCE 8) hung so before
// its window ever showed. (Exbit's plugin host does the same for plugins: winehost/d2dfix.cpp.)
//
// Here every DXGI factory made through this DLL first gives Wine's outputs a WaitForVBlank that waits
// for the next refresh of a 60 Hz screen (vblank_wait.c). EXBIT_WINE_DXGI_AS_IS=1: Wine's alone.
#include <windows.h>

#include "winefix/export_thunks.h"
#include "winefix/vblank_wait.h"
#include "winefix/wine_own_dll.h"

enum {
#define EXPORT(index, ordinal, name) DXGI_INDEX_##name = index,
#include "winefix/dxgi_exports.h"
#undef EXPORT
    DXGI_EXPORT_COUNT
};

#define EXPORT(index, ordinal, name) EXPORT_THUNK(index)
#include "winefix/dxgi_exports.h"
#undef EXPORT

void* exbitRealFunctions[DXGI_EXPORT_COUNT];
static void* wineFunctions[DXGI_EXPORT_COUNT];  // (Wine's own, for those replaced here)
static HMODULE wineDxgi;
static int missingExports = -1;
static volatile LONG outputsFixed;

static const char* const exportNames[] = {
#define EXPORT(index, ordinal, name) #name,
#include "winefix/dxgi_exports.h"
#undef EXPORT
};
static const WORD exportOrdinals[] = {
#define EXPORT(index, ordinal, name) ordinal,
#include "winefix/dxgi_exports.h"
#undef EXPORT
};

// gives Wine's outputs their WaitForVBlank through this factory, the first time one is made
static void fixOutputsThrough(void* factory) {
    if (outputsFixed || !factory || GetEnvironmentVariableW(L"EXBIT_WINE_DXGI_AS_IS", NULL, 0))
        return;
    if (giveOutputsWaitForVBlank(factory, wineDxgi))
        InterlockedExchange(&outputsFixed, 1);
}

typedef LONG(WINAPI* CreateFactoryFunction)(const GUID* interfaceId, void** factory);
typedef LONG(WINAPI* CreateFactory2Function)(UINT flags, const GUID* interfaceId, void** factory);
typedef LONG(WINAPI* CreateDeviceFunction)(HMODULE d3d10core, void* factory, void* adapter, UINT flags,
                                           const void* featureLevels, UINT levelCount, void** device);

// CreateDXGIFactory: Wine's, then the outputs' WaitForVBlank
static LONG WINAPI createFactory(const GUID* interfaceId, void** factory) {
    LONG result = ((CreateFactoryFunction)wineFunctions[DXGI_INDEX_CreateDXGIFactory])(interfaceId, factory);
    if (result >= 0)
        fixOutputsThrough(*factory);
    return result;
}

// CreateDXGIFactory1: likewise
static LONG WINAPI createFactory1(const GUID* interfaceId, void** factory) {
    LONG result = ((CreateFactoryFunction)wineFunctions[DXGI_INDEX_CreateDXGIFactory1])(interfaceId, factory);
    if (result >= 0)
        fixOutputsThrough(*factory);
    return result;
}

// CreateDXGIFactory2: likewise
static LONG WINAPI createFactory2(UINT flags, const GUID* interfaceId, void** factory) {
    LONG result = ((CreateFactory2Function)wineFunctions[DXGI_INDEX_CreateDXGIFactory2])(flags, interfaceId, factory);
    if (result >= 0)
        fixOutputsThrough(*factory);
    return result;
}

// DXGID3D10CreateDevice (how Wine's d3d10 and d3d11 make a device, with a factory of their own): the
// outputs' WaitForVBlank through that factory, then Wine's
static LONG WINAPI createDevice(HMODULE d3d10core, void* factory, void* adapter, UINT flags, const void* featureLevels,
                                UINT levelCount, void** device) {
    fixOutputsThrough(factory);
    return ((CreateDeviceFunction)wineFunctions[DXGI_INDEX_DXGID3D10CreateDevice])(d3d10core, factory, adapter, flags,
                                                                                  featureLevels, levelCount, device);
}

// (exported as ExbitWineFixReady, for Exbit's check: wine-fixes/exbit-wine-fix-check.exe) 1 when Wine's
// dxgi.dll is behind this one with all its exports, 0 if not
int exbitWineFixReady(void) { return missingExports == 0; }

// finds Wine's own dxgi.dll and every export of it; the factories and devices are made through the above
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    wineDxgi = loadWineOwnDll(L"dxgi.dll");
    if (!wineDxgi)
        return FALSE;  // (Wine says it couldn't load dxgi.dll; Exbit's check takes the stand-in away)
    missingExports = resolveExports(wineDxgi, exportNames, exportOrdinals, exbitRealFunctions, DXGI_EXPORT_COUNT);
    for (int index = 0; index < DXGI_EXPORT_COUNT; index++)
        wineFunctions[index] = exbitRealFunctions[index];
    if (missingExports)
        return TRUE;  // (passed on as they are)
    exbitRealFunctions[DXGI_INDEX_CreateDXGIFactory] = (void*)createFactory;
    exbitRealFunctions[DXGI_INDEX_CreateDXGIFactory1] = (void*)createFactory1;
    exbitRealFunctions[DXGI_INDEX_CreateDXGIFactory2] = (void*)createFactory2;
    exbitRealFunctions[DXGI_INDEX_DXGID3D10CreateDevice] = (void*)createDevice;
    return TRUE;
}
