// Exbit's Wine fixes (src/winefix): stand-ins for a few of Wine's own DLLs, put in a prefix's
// system32 (64-bit) and syswow64 (32-bit) by Exbit (plugins/wine_prefix_fixes.h), with Wine told to
// prefer them (DllOverrides "native,builtin"). Each loads Wine's own DLL of the same name and passes
// every call on to it, but for the few it corrects:
//   - msi.dll: MsiDatabaseCommit (msi_fix.c): Wine's commit could leave a database unreadable;
//   - dxgi.dll: IDXGIOutput::WaitForVBlank (dxgi_fix.c): Wine has none, and JUCE 8 programs hang.
// Built with MinGW (Makefile: wine-fixes/), for Wine only.
#pragma once
#include <windows.h>

// Wine's own (builtin) DLL of this name ("msi.dll"), loaded from Wine's DLL folder ($WINEDLLDIR0…
// /x86_64-windows or /i386-windows), not the stand-in the name would give; NULL if it can't be found
HMODULE loadWineOwnDll(const wchar_t* name);

// the addresses of `count` exports of `module`, by name (else by ordinal) into `addresses`;
// returns how many couldn't be found (0: all were)
int resolveExports(HMODULE module, const char* const* names, const WORD* ordinals, void** addresses, int count);
