// The few parts of the Win32 API that Exbit's Wine host (src/winehost) and its plugin hosts
// (plugins/vst3host.cpp, vst2host.cpp, built for Windows) use: Exbit's own declaration of them,
// from the API's public documentation, so the host builds with winegcc alone (Wine's own headers
// aren't needed). Only for that build: it is on the include path of src/winehost and nothing else.
//
// The host is Linux code (built with g++, linked by winegcc against Wine's DLLs), so every Win32
// function and callback is declared with Windows' calling convention: x64's (WINAPI = ms_abi) for the
// 64-bit host, __stdcall for the 32-bit one (exbit-wine-host-32.exe); the pointer-sized types follow
// the build. Wide strings are UTF-16 (WCHAR = char16_t; wchar_t stays the Linux 32-bit one). Where Wine
// has no 32-bit Unix part, the 32-bit host is built with MinGW instead (EXBIT_WINE_PE), on the same
// declarations.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(__i386__)
#define WINAPI __attribute__((stdcall))
#else
#define WINAPI __attribute__((ms_abi))
#endif
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
typedef uintptr_t UINT_PTR, ULONG_PTR, WPARAM, SIZE_T;  // (64 bits in the 64-bit host, 32 in the 32-bit one)
typedef intptr_t LONG_PTR, LPARAM, LRESULT;
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
WINAPI void ExitThread(DWORD code) __attribute__((noreturn));

// exceptions (only what the host's watch over them needs: host.cpp)
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    void* ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS {
    EXCEPTION_RECORD* ExceptionRecord;
    void* ContextRecord;
} EXCEPTION_POINTERS;
typedef LONG(WINAPI* PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS* info);
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4
WINAPI void* AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
WINAPI void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high);
WINAPI BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module);
WINAPI DWORD GetModuleFileNameW(HMODULE module, WCHAR* name, DWORD size);
WINAPI BOOL TerminateProcess(HANDLE process, UINT code);
WINAPI HANDLE GetCurrentProcess(void);
WINAPI int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR text, int size, LPWSTR out, int outSize);
WINAPI HANDLE CreateThread(void* security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                           DWORD* id);
WINAPI DWORD WaitForSingleObject(HANDLE handle, DWORD ms);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
WINAPI BOOL CloseHandle(HANDLE handle);
// (a file's pages shared: the 32-bit host built as a Windows program maps Exbit's /dev/shm block)
WINAPI HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, void* security, DWORD how, DWORD flags,
                          HANDLE templateFile);
WINAPI HANDLE CreateFileMappingW(HANDLE file, void* security, DWORD protect, DWORD sizeHigh, DWORD sizeLow,
                                 LPCWSTR name);
WINAPI void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytes);
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define OPEN_EXISTING 3u
#define FILE_MAP_ALL_ACCESS 0xf001fu
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
WINAPI void Sleep(DWORD ms);
WINAPI HANDLE GetCurrentThread(void);
WINAPI DWORD GetCurrentThreadId(void);
WINAPI BOOL SetThreadPriority(HANDLE thread, int priority);
WINAPI HANDLE GetProcessHeap(void);
WINAPI BOOL HeapFree(HANDLE heap, DWORD flags, void* memory);
// (a critical section, as an opaque block of its real size: 40 bytes on 64-bit Windows, 24 on 32-bit)
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
WINAPI BOOL K32EnumProcessModules(HANDLE process, HMODULE* modules, DWORD size, DWORD* needed);
#define THREAD_PRIORITY_TIME_CRITICAL 15

// Wine's own (kernel32): a Unix path's DOS name ("Z:\…", "C:\…"), allocated on the process heap
#ifdef EXBIT_WINE_PE  // (MinGW's kernel32 has no Wine exports: taken from Wine's at run time)
inline LPWSTR wine_get_dos_file_name(LPCSTR unixName) {
    static auto fn = (LPWSTR(WINAPI*)(LPCSTR))GetProcAddress(GetModuleHandleW(u"kernel32.dll"), "wine_get_dos_file_name");
    return fn ? fn(unixName) : nullptr;
}
#else
WINAPI LPWSTR wine_get_dos_file_name(LPCSTR unixName);
#endif

// --- user32 ---
WINAPI ATOM RegisterClassExW(const WNDCLASSEXW* wc);
WINAPI HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                            HWND parent, HMENU menu, HINSTANCE instance, LPVOID param);
WINAPI BOOL DestroyWindow(HWND hwnd);
WINAPI LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
WINAPI BOOL ShowWindow(HWND hwnd, int how);
WINAPI BOOL RedrawWindow(HWND hwnd, const RECT* update, void* region, UINT flags);
#define RDW_INVALIDATE 0x0001u
#define RDW_ERASE 0x0004u
#define RDW_ALLCHILDREN 0x0080u
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
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define MK_LBUTTON 0x0001u
#define CWP_SKIPINVISIBLE 0x0001u
WINAPI HWND ChildWindowFromPointEx(HWND parent, POINT point, UINT flags);
WINAPI BOOL ClientToScreen(HWND hwnd, POINT* point);
WINAPI int MapWindowPoints(HWND from, HWND to, POINT* points, UINT count);
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

// --- drag and drop (winehost/dragdrop.cpp: plugins' drags out, files dropped in) ---
typedef LONG HRESULT;
typedef void* HGLOBAL;
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t Data4[8];
} GUID;
typedef struct tagFORMATETC {
    WORD cfFormat;
    void* ptd;
    DWORD dwAspect;
    LONG lindex;
    DWORD tymed;
} FORMATETC;
typedef struct tagSTGMEDIUM {
    DWORD tymed;
    HGLOBAL hGlobal;  // (a union; only the HGLOBAL case is used)
    void* pUnkForRelease;
} STGMEDIUM;
typedef struct _POINTL {
    LONG x, y;
} POINTL;
// the start of a CF_HDROP block (then the file names, each ended by a 0, the list by another)
typedef struct _DROPFILES {
    DWORD pFiles;  // (where the names start)
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES;
// IDataObject's and IDropTarget's tables of methods, in their order (COM: `this` first)
typedef struct IDataObjectVtbl {
    HRESULT(WINAPI* QueryInterface)(void* self, const GUID* iid, void** object);
    ULONG(WINAPI* AddRef)(void* self);
    ULONG(WINAPI* Release)(void* self);
    HRESULT(WINAPI* GetData)(void* self, FORMATETC* format, STGMEDIUM* medium);
    HRESULT(WINAPI* GetDataHere)(void* self, FORMATETC* format, STGMEDIUM* medium);
    HRESULT(WINAPI* QueryGetData)(void* self, FORMATETC* format);
    HRESULT(WINAPI* GetCanonicalFormatEtc)(void* self, FORMATETC* in, FORMATETC* out);
    HRESULT(WINAPI* SetData)(void* self, FORMATETC* format, STGMEDIUM* medium, BOOL release);
    HRESULT(WINAPI* EnumFormatEtc)(void* self, DWORD direction, void** enumerator);
    HRESULT(WINAPI* DAdvise)(void* self, FORMATETC* format, DWORD flags, void* sink, DWORD* connection);
    HRESULT(WINAPI* DUnadvise)(void* self, DWORD connection);
    HRESULT(WINAPI* EnumDAdvise)(void* self, void** enumerator);
} IDataObjectVtbl;
typedef struct IDropTargetVtbl {
    HRESULT(WINAPI* QueryInterface)(void* self, const GUID* iid, void** object);
    ULONG(WINAPI* AddRef)(void* self);
    ULONG(WINAPI* Release)(void* self);
    HRESULT(WINAPI* DragEnter)(void* self, void* data, DWORD keys, POINTL point, DWORD* effect);
    HRESULT(WINAPI* DragOver)(void* self, DWORD keys, POINTL point, DWORD* effect);
    HRESULT(WINAPI* DragLeave)(void* self);
    HRESULT(WINAPI* Drop)(void* self, void* data, DWORD keys, POINTL point, DWORD* effect);
} IDropTargetVtbl;
#define CF_HDROP 15
#define DVASPECT_CONTENT 1u
#define TYMED_HGLOBAL 1u
#define DROPEFFECT_NONE 0u
#define DROPEFFECT_COPY 1u
#define DRAGDROP_S_DROP 0x00040100L
#define S_OK 0L
#define E_NOTIMPL ((HRESULT)0x80004001u)
#define E_NOINTERFACE ((HRESULT)0x80004002u)
#define DV_E_FORMATETC ((HRESULT)0x80040064u)
#define OLE_E_ADVISENOTSUPPORTED ((HRESULT)0x80040003u)
#define GMEM_MOVEABLE 0x0002u
#define GMEM_ZEROINIT 0x0040u
#define GWL_EXSTYLE (-20)
#define WS_EX_ACCEPTFILES 0x00000010u
#define WM_DROPFILES 0x0233u
WINAPI HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes);
WINAPI void* GlobalLock(HGLOBAL memory);
WINAPI BOOL GlobalUnlock(HGLOBAL memory);
WINAPI HGLOBAL GlobalFree(HGLOBAL memory);
WINAPI SIZE_T GlobalSize(HGLOBAL memory);
WINAPI void ReleaseStgMedium(STGMEDIUM* medium);
WINAPI HANDLE GetPropW(HWND hwnd, LPCWSTR name);
WINAPI HWND WindowFromPoint(POINT point);
WINAPI LONG GetWindowLongW(HWND hwnd, int index);
WINAPI BOOL ScreenToClient(HWND hwnd, POINT* point);
// Wine's own (kernel32): a DOS path's Unix name, allocated on the process heap
#ifdef EXBIT_WINE_PE
inline char* wine_get_unix_file_name(LPCWSTR dosName) {
    static auto fn = (char*(WINAPI*)(LPCWSTR))GetProcAddress(GetModuleHandleW(u"kernel32.dll"), "wine_get_unix_file_name");
    return fn ? fn(dosName) : nullptr;
}
#else
WINAPI char* wine_get_unix_file_name(LPCWSTR dosName);
#endif

}  // extern "C"
