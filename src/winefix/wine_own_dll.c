// Loading Wine's own DLLs behind Exbit's stand-ins (see wine_own_dll.h).
#include "winefix/wine_own_dll.h"

#include <stdio.h>
#include <wchar.h>

// Wine names its DLL folders in the environment of every Windows program: WINEDLLDIR0, WINEDLLDIR1…
// ("\??\Z:\usr\lib\wine"); its own DLLs are in the subfolder for the program's machine
HMODULE loadWineOwnDll(const wchar_t* name) {
    const wchar_t* machineFolder = sizeof(void*) == 8 ? L"x86_64-windows" : L"i386-windows";
    for (int index = 0; index < 10; index++) {
        wchar_t variable[32], folder[MAX_PATH], path[MAX_PATH];
        swprintf(variable, 32, L"WINEDLLDIR%d", index);
        DWORD length = GetEnvironmentVariableW(variable, folder, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            continue;
        const wchar_t* start = folder;
        if (!wcsncmp(start, L"\\??\\", 4))
            start += 4;
        swprintf(path, MAX_PATH, L"%ls\\%ls\\%ls", start, machineFolder, name);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        HMODULE module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module)
            return module;
    }
    return NULL;
}

int resolveExports(HMODULE module, const char* const* names, const WORD* ordinals, void** addresses, int count) {
    int missing = 0;
    for (int index = 0; index < count; index++) {
        FARPROC address = GetProcAddress(module, names[index]);
        if (!address)
            address = GetProcAddress(module, (LPCSTR)(ULONG_PTR)ordinals[index]);
        addresses[index] = (void*)address;
        if (!address)
            missing++;
    }
    return missing;
}
