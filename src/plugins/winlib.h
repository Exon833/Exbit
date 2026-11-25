// Loading a Windows DLL by a UTF-8 path, for the plugin hosts built for Windows: a real Windows
// build, or Exbit's Wine host (EXBIT_WINE_HOST: Linux code inside Wine, given Unix paths,
// which Wine turns into its DOS names). The DLL's own folder is searched for the DLLs it needs.
#pragma once
#ifdef _WIN32
#include <windows.h>

#include <string>

namespace plug {

inline HMODULE loadWindowsLibrary(const std::string& path, std::string& error) {
    HMODULE h = nullptr;
#ifdef EXBIT_WINE_HOST
    if (LPWSTR dos = wine_get_dos_file_name(path.c_str())) {
        h = LoadLibraryExW(dos, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HeapFree(GetProcessHeap(), 0, dos);
    }
#else
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    for (auto& c : w)  // (the altered search path needs '\\': the program's paths have '/')
        if (c == L'/')
            c = L'\\';
    h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#endif
    if (!h)
        error = "Cannot load " + path + " (error " + std::to_string((unsigned)GetLastError()) + ")";
    return h;
}

}  // namespace plug
#endif
