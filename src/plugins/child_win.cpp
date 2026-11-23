// Windows' child processes and pipes for the plugin code (see child_win.h)
#include "plugins/child_win.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <map>
#include <mutex>

namespace child {

namespace {

std::mutex lock;
std::map<int, HANDLE> children;  // (under lock) started here and not yet known to have ended

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// one argument as CommandLineToArgvW / the C runtime read it back: quoted when it must be, the
// backslashes before a quote (or the closing one) doubled
void quote(std::wstring& out, const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += a;
        return;
    }
    out += L'"';
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') i++, slashes++;
        if (i == a.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        out += a[i];
    }
    out += L'"';
}

// an inheritable copy of a CRT fd's handle (nullptr: none)
HANDLE inheritable(int fd) {
    HANDLE h = fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
    HANDLE copy = nullptr;
    if (h == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE,
                                                      DUPLICATE_SAME_ACCESS))
        return nullptr;
    return copy;
}

}  // namespace

std::wstring commandLine(const std::string& exe, const std::vector<std::string>& args) {
    std::wstring line, w = wide(exe);
    for (auto& c : w)  // (Windows' own way of writing it: the program's paths have '/')
        if (c == L'/') c = L'\\';
    quote(line, w);
    for (const auto& a : args) {
        line += L' ';
        quote(line, wide(a));
    }
    return line;
}

int spawn(const std::string& exe, const std::vector<std::string>& args, int inFd, int outFd, std::string& why) {
    // (what isn't given: NUL, so that the child's reads end and its writes go nowhere)
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                             nullptr);
    HANDLE in = inheritable(inFd), out = inheritable(outFd), err = nullptr;
    HANDLE ownErr = GetStdHandle(STD_ERROR_HANDLE);
    if (ownErr && ownErr != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), ownErr, GetCurrentProcess(), &err, 0, TRUE, DUPLICATE_SAME_ACCESS);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in ? in : nul;
    si.StartupInfo.hStdOutput = out ? out : nul;
    si.StartupInfo.hStdError = err ? err : nul;
    // only these handles go to the child (not the other children's pipes, which would then never end)
    HANDLE list[3] = {si.StartupInfo.hStdInput, si.StartupInfo.hStdOutput, si.StartupInfo.hStdError};
    int count = 0;
    HANDLE unique[3];
    for (HANDLE h : list) {
        bool seen = false;
        for (int i = 0; i < count; i++) seen = seen || unique[i] == h;
        if (h && h != INVALID_HANDLE_VALUE && !seen) unique[count++] = h;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string attrs(size, '\0');
    auto* attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)&attrs[0];
    bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
                  UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, unique, count * sizeof(HANDLE),
                                            nullptr, nullptr);
    si.lpAttributeList = listed ? attributes : nullptr;
    std::wstring line = commandLine(exe, args), app = wide(exe);
    for (auto& c : app)
        if (c == L'/') c = L'\\';
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(app.c_str(), &line[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD error = GetLastError();
    if (listed) DeleteProcThreadAttributeList(attributes);
    for (HANDLE h : {in, out, err, nul})
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        why = "Cannot start " + exe + " (error " + std::to_string((unsigned)error) + ")";
        return -1;
    }
    CloseHandle(pi.hThread);
    std::lock_guard<std::mutex> hold(lock);
    children[(int)pi.dwProcessId] = pi.hProcess;
    return (int)pi.dwProcessId;
}

bool finished(int pid, int& code) {
    std::lock_guard<std::mutex> hold(lock);
    auto it = children.find(pid);
    if (it == children.end()) {
        code = -1;
        return true;  // (not ours, or already known to have ended)
    }
    if (WaitForSingleObject(it->second, 0) != WAIT_OBJECT_0) return false;
    DWORD exit = 0;
    GetExitCodeProcess(it->second, &exit);
    code = (int)exit;
    CloseHandle(it->second);
    children.erase(it);
    return true;
}

void kill(int pid) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> hold(lock);
        auto it = children.find(pid);
        if (it != children.end()) {
            h = it->second;
            children.erase(it);
        }
    }
    if (!h) h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return;
    TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
}

bool pipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) == 0; }

int readable(int fd, int ms) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    // (an anonymous pipe can't be waited on: looked at every few ms)
    DWORD start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return -1;  // (broken: its writers have gone)
        if (avail > 0) return 1;
        if ((int)(GetTickCount() - start) >= ms) return 0;
        Sleep(ms > 0 ? 2 : 0);
    }
}

}  // namespace child
