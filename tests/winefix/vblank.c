// winefix_vblank.exe: does DXGI's IDXGIOutput::WaitForVBlank work, as JUCE 8 programs need it to
// (tests/test_wine_fixes.cpp runs it in a prefix; src/winefix/dxgi_fix.c)? 20 waits on the first output
// of the first adapter: each must succeed, and they must take a screen's refresh each (on average 4 to
// 40 ms: 25 to 250 Hz). Prints "OK" and ends with 0, else "BROKEN" and 1; 2 when there is no output
// to wait on (no screen: nothing to tell).
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>

int main(void) {
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    IDXGIOutput* output = NULL;
    if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory) < 0 ||
        factory->lpVtbl->EnumAdapters1(factory, 0, &adapter) < 0 ||
        adapter->lpVtbl->EnumOutputs(adapter, 0, &output) < 0) {
        printf("no DXGI output here\n");
        return 2;
    }
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const int WAITS = 20;
    HRESULT failed = S_OK;
    for (int index = 0; index < WAITS; index++) {
        HRESULT result = output->lpVtbl->WaitForVBlank(output);
        if (result < 0) failed = result;
    }
    QueryPerformanceCounter(&end);
    double averageMs = 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart / WAITS;
    int works = failed == S_OK && averageMs >= 4 && averageMs <= 40;
    printf("WaitForVBlank: %s, %.2f ms each\n%s\n", failed == S_OK ? "S_OK" : "failed", averageMs, works ? "OK" : "BROKEN");
    output->lpVtbl->Release(output);
    adapter->lpVtbl->Release(adapter);
    factory->lpVtbl->Release(factory);
    return works ? 0 : 1;
}
