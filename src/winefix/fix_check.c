// exbit-wine-fix-check.exe (and -32.exe): run by Exbit in a prefix it has given its fixes (see
// wine_own_dll.h, plugins/wine_prefix_fixes.h), to see that the stand-ins load there and have Wine's
// own DLLs behind them. Prints a line per DLL:
//   msi.dll: fixed            Exbit's, with all of Wine's exports behind it
//   msi.dll: not ready        Exbit's, but Wine's own (or some of its exports) wasn't found
//   msi.dll: Wine's own       not Exbit's (the prefix isn't set to prefer it)
//   msi.dll: not loaded       it couldn't be loaded at all
// and ends with 0 only if every one is "fixed". With "windows": the windows shown (see main).
#include <windows.h>

#include <stdio.h>
#include <string.h>

// one DLL's line (see the top); 1 if it is fixed
static int checkOne(const wchar_t* name) {
    HMODULE module = LoadLibraryW(name);
    const char* state;
    int fixed = 0;
    if (!module) {
        state = "not loaded";
    } else {
        int (*ready)(void) = (int (*)(void))(void*)GetProcAddress(module, "ExbitWineFixReady");
        if (!ready)
            state = "Wine's own";
        else if (ready())
            state = "fixed", fixed = 1;
        else
            state = "not ready";
    }
    printf("%ls: %s\n", name, state);
    return fixed;
}

static int windowsShown;

// counts a top-level window that is shown, but not the desktop's own (its taskbar, Wine's helpers)
static BOOL CALLBACK countShown(HWND window, LPARAM unused) {
    char className[64] = "";
    GetClassNameA(window, className, sizeof className);
    if (IsWindowVisible(window) && strcmp(className, "Shell_TrayWnd") && strcmp(className, "Progman") &&
        strcmp(className, "#32769"))
        windowsShown++;
    return TRUE;
}

// "windows": prints "windows: <n>", the windows the prefix's programs show (for Exbit's watchdog of
// installers, plugins/installer_activity.h); else the check above
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "windows")) {
        EnumWindows(countShown, 0);
        printf("windows: %d\n", windowsShown);
        fflush(stdout);
        return 0;
    }
    int fixed = checkOne(L"msi.dll");
    fixed &= checkOne(L"dxgi.dll");
    fflush(stdout);
    return fixed ? 0 : 1;
}
