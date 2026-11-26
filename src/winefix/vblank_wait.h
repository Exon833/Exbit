// A WaitForVBlank for Wine's DXGI (used by dxgi_fix.c; see there).
#pragma once
#include <windows.h>

// IDXGIOutput::WaitForVBlank done here: returns at the next refresh of a 60 Hz screen (S_OK)
LONG WINAPI waitForNextRefresh(void* output);

// gives Wine's IDXGIOutput (all of them: they share one table of methods) waitForNextRefresh as
// WaitForVBlank, found through this factory's first adapter and output; only where Wine's own is
// the stub (it answers E_NOTIMPL at once). `wineDxgi`: Wine's dxgi.dll. 1 if it was given
int giveOutputsWaitForVBlank(void* factory, HMODULE wineDxgi);
