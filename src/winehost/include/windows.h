// The few parts of the Win32 API that Tiny DAW's Wine host (src/winehost) and its plugin hosts
// (plugins/vst3host.cpp, vst2host.cpp, built for Windows) use: Tiny DAW's own declaration of them,
// from the API's public documentation, so the host builds with winegcc alone (Wine's own headers
// aren't needed). Only for that build: it is on the include path of src/winehost and nothing else.
//
// The host is Linux code (built with g++, linked by winegcc against Wine's DLLs), so every Win32
// function and callback is declared with the Windows x64 calling convention (WINAPI = ms_abi), and
// wide strings are UTF-16 (WCHAR = char16_t; wchar_t stays the Linux 32-bit one).
#pragma once
#include <cstddef>
#include <cstdint>

#define WINAPI __attribute__((ms_abi))
#define CALLBACK WINAPI
#define APIENTRY WINAPI

extern "C" {

typedef int BOOL;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int32_t LONG;
typedef uint64_t UINT_PTR, ULONG_PTR, WPARAM, SIZE_T;
typedef int64_t LONG_PTR, LPARAM, LRESULT;
typedef uint16_t ATOM;
typedef char16_t WCHAR;
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef const char* LPCSTR;
typedef void* LPVOID;
typedef void* HANDLE;
typedef struct HWND__* HWND;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
typedef struct HICON__* HICON;
typedef HICON HCURSOR;
typedef struct HBRUSH__* HBRUSH;
typedef struct HMENU__* HMENU;
typedef struct HDC__* HDC;

#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu
#define CP_UTF8 65001u
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008u
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(ULONG_PTR)(WORD)(i))

typedef struct tagPOINT {
    LONG x, y;
} POINT;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT;

typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
    DWORD lPrivate;
} MSG;

typedef LRESULT(WINAPI* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void(WINAPI* TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);

typedef struct tagWNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXW;

// --- kernel32 ---
WINAPI HMODULE LoadLibraryW(LPCWSTR name);
WINAPI HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
WINAPI void* GetProcAddress(HMODULE module, LPCSTR name);
WINAPI BOOL FreeLibrary(HMODULE module);
WINAPI HMODULE GetModuleHandleW(LPCWSTR name);
WINAPI DWORD GetLastError(void);
WINAPI void ExitProcess(UINT code) __attribute__((noreturn));
WINAPI BOOL TerminateProcess(HANDLE process, UINT code);
WINAPI HANDLE GetCurrentProcess(void);
WINAPI int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR text, int size, LPWSTR out, int outSize);
WINAPI HANDLE CreateThread(void* security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                           DWORD* id);
WINAPI DWORD WaitForSingleObject(HANDLE handle, DWORD ms);
WINAPI BOOL CloseHandle(HANDLE handle);
WINAPI void Sleep(DWORD ms);
WINAPI HANDLE GetCurrentThread(void);
WINAPI DWORD GetCurrentThreadId(void);
WINAPI BOOL SetThreadPriority(HANDLE thread, int priority);
WINAPI HANDLE GetProcessHeap(void);
WINAPI BOOL HeapFree(HANDLE heap, DWORD flags, void* memory);
// (a critical section, as an opaque block of its real size: 40 bytes on 64-bit Windows)
typedef struct {
    void* debug;
    LONG lockCount, recursionCount;
    HANDLE owner, semaphore;
    ULONG_PTR spin;
} CRITICAL_SECTION_;
WINAPI void InitializeCriticalSection(CRITICAL_SECTION_* section);
WINAPI void EnterCriticalSection(CRITICAL_SECTION_* section);
WINAPI void LeaveCriticalSection(CRITICAL_SECTION_* section);
#define InitializeCriticalSection_ InitializeCriticalSection
#define EnterCriticalSection_ EnterCriticalSection
#define LeaveCriticalSection_ LeaveCriticalSection
WINAPI BOOL VirtualProtect(void* address, SIZE_T size, DWORD protect, DWORD* old);
#define PAGE_READWRITE 0x04u
#define THREAD_PRIORITY_TIME_CRITICAL 15

// Wine's own (kernel32): a Unix path's DOS name ("Z:\…", "C:\…"), allocated on the process heap
WINAPI LPWSTR wine_get_dos_file_name(LPCSTR unixName);

// --- user32 ---
WINAPI ATOM RegisterClassExW(const WNDCLASSEXW* wc);
WINAPI HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                            HWND parent, HMENU menu, HINSTANCE instance, LPVOID param);
WINAPI BOOL DestroyWindow(HWND hwnd);
WINAPI LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
WINAPI BOOL ShowWindow(HWND hwnd, int how);
WINAPI BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags);
WINAPI BOOL GetWindowRect(HWND hwnd, RECT* rect);
WINAPI BOOL GetClientRect(HWND hwnd, RECT* rect);
WINAPI BOOL IsWindow(HWND hwnd);
WINAPI HANDLE GetPropA(HWND hwnd, LPCSTR name);
WINAPI BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
WINAPI BOOL GetMessageW(MSG* msg, HWND hwnd, UINT first, UINT last);
WINAPI BOOL PeekMessageW(MSG* msg, HWND hwnd, UINT first, UINT last, UINT remove);
WINAPI BOOL TranslateMessage(const MSG* msg);
WINAPI LRESULT DispatchMessageW(const MSG* msg);
WINAPI void PostQuitMessage(int code);
WINAPI UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, TIMERPROC proc);
WINAPI BOOL KillTimer(HWND hwnd, UINT_PTR id);
WINAPI HCURSOR LoadCursorW(HINSTANCE instance, LPCWSTR name);
WINAPI HWND SetFocus(HWND hwnd);
WINAPI HWND GetWindow(HWND hwnd, UINT cmd);
WINAPI BOOL SetProcessDPIAware(void);
typedef struct HMONITOR__* HMONITOR;
WINAPI HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
#define MONITOR_DEFAULTTONULL 0u
WINAPI HWND GetFocus(void);
WINAPI HWND GetForegroundWindow(void);
WINAPI HWND GetParent(HWND hwnd);
WINAPI BOOL IsChild(HWND parent, HWND hwnd);
WINAPI int GetClassNameA(HWND hwnd, char* name, int size);
typedef struct HHOOK__* HHOOK;
typedef LRESULT(WINAPI* HOOKPROC)(int code, WPARAM wp, LPARAM lp);
WINAPI HHOOK SetWindowsHookExW(int id, HOOKPROC proc, HINSTANCE module, DWORD thread);
WINAPI LRESULT CallNextHookEx(HHOOK hook, int code, WPARAM wp, LPARAM lp);
#define WH_GETMESSAGE 3

#define HWND_MESSAGE ((HWND)(LONG_PTR)-3)
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_NOACTIVATE 0x08000000u
#define SW_HIDE 0
#define SW_SHOWNA 8
#define SW_SHOW 5
#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOACTIVATE 0x0010u
#define SWP_NOOWNERZORDER 0x0200u
#define PM_REMOVE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_CLOSE 0x0010u
#define WM_TIMER 0x0113u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ACTIVATE 0x0006u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_LBUTTONDOWN 0x0201u
#define MA_ACTIVATE 1
#define WM_USER 0x0400u
#define WM_APP 0x8000u
#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define GW_CHILD 5

// --- advapi32 (the registry) ---
typedef struct HKEY__* HKEY;
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001)
#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE 0x0002u
#define REG_SZ 1u
#define ERROR_SUCCESS 0L
WINAPI LONG RegCreateKeyExW(HKEY key, LPCWSTR subKey, DWORD reserved, LPWSTR cls, DWORD options, DWORD access,
                            void* security, HKEY* result, DWORD* disposition);
WINAPI LONG RegQueryValueExW(HKEY key, LPCWSTR name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size);
WINAPI LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size);
WINAPI LONG RegCloseKey(HKEY key);

// --- ole32 ---
WINAPI LONG OleInitialize(LPVOID reserved);
WINAPI void OleUninitialize(void);

}  // extern "C"
