// core/os.h on Windows
#include "core/os.h"

#include <windows.h>

#include <cerrno>
#include <clocale>

namespace os {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const wchar_t* w, int len) {
    if (!w) return std::string();
    if (len < 0) len = (int)wcslen(w);
    if (len == 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}

std::string nativePath(const std::string& path) {
    std::string p = path;
    for (char& c : p)
        if (c == '/') c = '\\';
    return p;
}

std::string portablePath(const std::string& path) {
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

namespace {
std::string lastError() {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   e, 0, (wchar_t*)&msg, 0, nullptr);
    std::string s = msg ? narrow(msg) : "error " + std::to_string((unsigned long)e);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}
}  // namespace

void* openLibrary(const std::string& path, std::string* error) {
    // (a full path: the DLLs it needs are looked for in its own folder first, as a plugin's are)
    bool full = path.find('/') != std::string::npos || path.find('\\') != std::string::npos;
    HMODULE h = LoadLibraryExW(widen(nativePath(path)).c_str(), nullptr, full ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (!h && error) *error = path + ": " + lastError();
    return (void*)h;
}

void* librarySymbol(void* lib, const char* name) {
    return lib ? (void*)GetProcAddress((HMODULE)lib, name) : nullptr;
}

void closeLibrary(void* lib) {
    if (lib) FreeLibrary((HMODULE)lib);
}

bool MappedFile::map(const std::string& path) {
    unmap();
    HANDLE f = CreateFileW(widen(nativePath(path)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    HANDLE m = nullptr;
    if (GetFileType(f) == FILE_TYPE_DISK && GetFileSizeEx(f, &sz) && sz.QuadPart > 0)
        m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);  // (the mapping keeps the file)
    if (!m) return false;
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) {
        CloseHandle(m);
        return false;
    }
    data = (const uint8_t*)v;
    size = (size_t)sz.QuadPart;
    handle = m;
    return true;
}

void MappedFile::unmap() {
    if (data) UnmapViewOfFile(data);
    if (handle) CloseHandle((HANDLE)handle);
    data = nullptr;
    size = 0;
    handle = nullptr;
}

bool makePipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) == 0; }

namespace {
// one argument as CommandLineToArgvW and the C runtime read it back
void quoteArg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') i++, slashes++;
        if (i == a.size()) {
            cmd.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
        } else {
            cmd.append(slashes, L'\\');
        }
        cmd += a[i];
    }
    cmd += L'"';
}

bool start(const std::vector<std::string>& args, int outFd, bool detached, Process& p, int* err) {
    if (args.empty()) {
        if (err) *err = EINVAL;
        return false;
    }
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); i++) quoteArg(cmd, widen(i == 0 ? nativePath(args[i]) : args[i]));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE out = nul;
    if (outFd >= 0) {
        HANDLE h = (HANDLE)_get_osfhandle(outFd);
        if (h == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
            if (err) *err = EBADF;
            return false;
        }
    }
    // only these handles are inherited (not another thread's pipe, which would then never see its end)
    HANDLE inherit[2] = {nul, out};
    DWORD count = out == nul ? 1 : 2;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::string attrBuf(attrSize, '\0');
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)&attrBuf[0];
    bool listOk = nul != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, count * sizeof(HANDLE),
                                            nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = nul;
    if (listOk) si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | (listOk ? EXTENDED_STARTUPINFO_PRESENT : 0) |
                  (detached ? CREATE_NEW_PROCESS_GROUP : 0);
    BOOL ok = listOk && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                       &si.StartupInfo, &pi);
    DWORD e = GetLastError();
    if (listOk) DeleteProcThreadAttributeList(attrs);
    if (out != nul) CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (err) *err = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EIO;
        return false;
    }
    CloseHandle(pi.hThread);
    p.pid = (long long)pi.dwProcessId;
    p.handle = pi.hProcess;
    return true;
}
}  // namespace

bool spawn(const std::vector<std::string>& args, int outFd, Process& p, int* err) {
    return start(args, outFd, false, p, err);
}

int waitProcess(Process& p, bool block) {
    if (!p.handle) return -1;
    if (WaitForSingleObject((HANDLE)p.handle, block ? INFINITE : 0) == WAIT_TIMEOUT) return -2;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess((HANDLE)p.handle, &code);
    CloseHandle((HANDLE)p.handle);
    p.handle = nullptr;
    p.pid = -1;
    // (killed by killProcess: 128 + 9, as a shell reports it; crashes: NTSTATUS codes, 0xC0000005…)
    if (!ok || code > 255) return -1;
    return (int)code;
}

void killProcess(const Process& p) {
    if (p.handle) TerminateProcess((HANDLE)p.handle, 128 + 9);
}

bool spawnDetached(const std::vector<std::string>& args) {
    Process p;
    if (!start(args, -1, true, p, nullptr)) return false;
    CloseHandle((HANDLE)p.handle);
    return true;
}

std::string executablePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    return n ? portablePath(narrow(buf.c_str(), (int)n)) : std::string();
}

WakeEvent::WakeEvent() { native_ = (intptr_t)CreateEventW(nullptr, FALSE, FALSE, nullptr); }

WakeEvent::~WakeEvent() {
    if (native_) CloseHandle((HANDLE)native_);
}

void WakeEvent::signal() { SetEvent((HANDLE)native_); }

void WakeEvent::clear() { ResetEvent((HANDLE)native_); }

bool WakeEvent::wait(int timeoutMs) {
    return WaitForSingleObject((HANDLE)native_, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs) == WAIT_OBJECT_0;
}

double monotonicSeconds() {
    static const double tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / (double)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * tick;
}

void sleepMs(int ms) { Sleep(ms < 0 ? 0 : (DWORD)ms); }

bool setUtf8Locale() { return setlocale(LC_CTYPE, ".UTF-8") != nullptr; }

}  // namespace os
