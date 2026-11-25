// exbit-unix32.dll: the 32-bit Wine host's (the plain Windows build, unixside.h) way to its
// Linux side, unixside.c. Wine lets only a builtin DLL call into a Unix library of its own, so this
// is built with winegcc (Wine's headers, -include wine/unixlib.h) and marked builtin.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE self, DWORD why, void* reserved) {
    return why != DLL_PROCESS_ATTACH || __wine_init_unix_call() == 0;
}

NTSTATUS __cdecl exbit_unix_call(void* args) { return WINE_UNIX_CALL(0, args); }
