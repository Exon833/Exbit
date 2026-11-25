// Windows (a native build: Exbit and its plugin host, exbit-plugin-host.exe, both Windows
// programs): bridge.h's socket, shared memory and wake-up calls.
//
//   - the socket is a Unix one still (AF_UNIX, Windows 10 1803 and later), a file in the user's
//     temporary folder; where Windows hasn't them, TCP on the loopback instead (the name given to the
//     host is then "tcp:<port>"). Sockets are held as ints, as on Linux: Windows' handles have 32
//     significant bits in either build (and are sign-extended back);
//   - the shared memory is a named file mapping ("Local\exbit-…": this session's), gone when both
//     sides have let go of it;
//   - a futex can't be shared between processes on Windows (WaitOnAddress is one process's), so each
//     futex word is bound to a named event of its own (bindWake: Exbit and the host each bind both
//     words of the AudioBlock): wake() sets the event, waitChange() waits on it while the word holds
//     its value. A word that couldn't be bound is polled (a failsafe: slower, never stuck).
#pragma once
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // (Windows sends no SIGPIPE)
#endif

namespace bridge {

inline SOCKET socketOf(int fd) { return (SOCKET)(intptr_t)fd; }

inline bool socketsReady() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

inline void closeSocket(int fd) {
    if (fd >= 0) closesocket(socketOf(fd));
}

inline std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// all of n bytes (false: the other side is gone)
inline bool sendAll(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        int k = ::send(socketOf(fd), c, (int)(n < (1u << 30) ? n : (1u << 30)), 0);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

inline bool recvAll(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        int k = ::recv(socketOf(fd), c, (int)(n < (1u << 30) ? n : (1u << 30)), 0);
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

// as on Linux (bridge.h): giving up when the other side takes nothing for `seconds` (errno ETIMEDOUT).
// Windows has no per-call MSG_DONTWAIT: the socket is non-blocking for the while (only Exbit's main
// thread uses its end)
inline bool sendAllWithin(int fd, const void* p, size_t n, double seconds, size_t& sent) {
    const char* c = (const char*)p;
    sent = 0;
    u_long on = 1, off = 0;
    ioctlsocket(socketOf(fd), FIONBIO, &on);
    bool ok = true;
    while (n) {
        int k = ::send(socketOf(fd), c, (int)(n < (1u << 30) ? n : (1u << 30)), 0);
        if (k < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            WSAPOLLFD q{socketOf(fd), POLLWRNORM, 0};
            int r = WSAPoll(&q, 1, (int)(seconds * 1000));
            if (r == 0) errno = ETIMEDOUT;
            else if (r < 0 || (q.revents & (POLLERR | POLLHUP))) errno = EPIPE;
            if (r <= 0 || (q.revents & (POLLERR | POLLHUP))) {
                ok = false;
                break;
            }
            continue;
        }
        if (k <= 0) {
            errno = EPIPE;
            ok = false;
            break;
        }
        c += k;
        n -= (size_t)k;
        sent += (size_t)k;
    }
    ioctlsocket(socketOf(fd), FIONBIO, &off);
    return ok;
}

// is a message waiting (or the other side gone)? waits up to ms. Meanwhile Windows' messages sent to
// this thread's windows are answered (in slices): the plugin's window, a child of Exbit's in the host's
// process, sends Exbit's some and waits for them; a main thread waiting here for the host would hold both
inline bool readable(int fd, int ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        WSAPOLLFD p{socketOf(fd), POLLRDNORM, 0};
        int r = WSAPoll(&p, 1, left <= 0 ? 0 : left < 5 ? left : 5);
        if (r != 0) return r > 0;
        if (left <= 0) return false;
    }
}

// --- the socket's two ends ---

// (Exbit) a socket for one host to connect to: a Unix socket at `path`, else TCP on the loopback;
// `name` gets what to tell the host. -1 and why if neither
inline int listenFor(const std::string& path, std::string& name, std::string& why) {
    if (!socketsReady()) {
        why = "Windows' sockets can't be used (WSAStartup failed)";
        return -1;
    }
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() < sizeof(addr.sun_path)) {
            std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());  // (UTF-8, as Windows takes it)
            DeleteFileW(wide(path).c_str());
            if (bind(s, (sockaddr*)&addr, sizeof addr) == 0 && listen(s, 1) == 0) {
                name = path;
                return (int)(intptr_t)s;
            }
        }
        closesocket(s);
    }
    // (no Unix sockets: Windows before 1803) the loopback, a port of the system's choosing
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof in;
    if (s != INVALID_SOCKET && bind(s, (sockaddr*)&in, sizeof in) == 0 && listen(s, 1) == 0 &&
        getsockname(s, (sockaddr*)&in, &len) == 0) {
        name = "tcp:" + std::to_string(ntohs(in.sin_port));
        return (int)(intptr_t)s;
    }
    if (s != INVALID_SOCKET) closesocket(s);
    why = "Cannot make the socket for a plugin host (error " + std::to_string(WSAGetLastError()) + ")";
    return -1;
}

inline int acceptOne(int listener) {
    SOCKET s = accept(socketOf(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET) return -1;
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);  // (TCP: no delay for small messages)
    return (int)(intptr_t)s;
}

// (the host) connects to what listenFor() named
inline int connectTo(const std::string& name) {
    if (!socketsReady()) return -1;
    SOCKET s;
    int r;
    if (name.compare(0, 4, "tcp:") == 0) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        in.sin_port = htons((u_short)atoi(name.c_str() + 4));
        r = s == INVALID_SOCKET ? -1 : connect(s, (sockaddr*)&in, sizeof in);
        BOOL on = TRUE;
        if (r == 0) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
    } else {
        s = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", name.c_str());
        r = s == INVALID_SOCKET ? -1 : connect(s, (sockaddr*)&addr, sizeof addr);
    }
    if (r != 0) {
        errno = ECONNREFUSED;
        if (s != INVALID_SOCKET) closesocket(s);
        return -1;
    }
    return (int)(intptr_t)s;
}

// --- shared memory ---

// (the mappings made here, by name: a name lasts while a handle to it does, so it is held until the
// other side has opened it, unlinkShared(), as shm_unlink on Linux)
inline std::mutex sharedLock;
inline std::map<std::string, HANDLE> sharedNames;

// a named mapping of `size` bytes: made (create) or opened; nullptr if not
inline void* mapShared(const std::string& name, size_t size, bool create) {
    std::wstring w = wide(name);
    HANDLE m = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                           (DWORD)size, w.c_str())
                      : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
    if (!m) return nullptr;
    if (create && GetLastError() == ERROR_ALREADY_EXISTS) {  // (someone else's: not shared with them)
        CloseHandle(m);
        return nullptr;
    }
    void* p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (create && p) {
        std::lock_guard<std::mutex> hold(sharedLock);
        sharedNames[name] = m;  // (its name kept for the other side)
    } else {
        CloseHandle(m);  // (the view keeps the mapping)
    }
    return p;
}

// the name of a mapping made here let go of (the mapping lasts while it is mapped)
inline void unlinkShared(const std::string& name) {
    std::lock_guard<std::mutex> hold(sharedLock);
    auto it = sharedNames.find(name);
    if (it == sharedNames.end()) return;
    CloseHandle(it->second);
    sharedNames.erase(it);
}

inline void unmapShared(void* p) {
    if (p) UnmapViewOfFile(p);
}

// --- futex words, woken through events ---

struct WakeSlot {
    std::atomic<void*> word{nullptr};
    std::atomic<HANDLE> event{nullptr};
};
constexpr int WAKE_SLOTS = 1024;  // (two a plugin)
inline WakeSlot wakeSlots[WAKE_SLOTS];

inline HANDLE wakeEvent(const void* word) {
    for (auto& s : wakeSlots)
        if (s.word.load(std::memory_order_acquire) == word) return s.event.load(std::memory_order_acquire);
    return nullptr;
}

// the word woken by the named event (made, or opened: the other side made it); false: it is polled instead
inline bool bindWake(std::atomic<uint32_t>* word, const std::string& eventName, bool create) {
    std::wstring w = wide(eventName);
    HANDLE e = create ? CreateEventW(nullptr, FALSE, FALSE, w.c_str())
                      : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, w.c_str());
    if (!e) return false;
    for (auto& s : wakeSlots) {
        void* none = nullptr;
        if (s.word.load() == nullptr && s.event.load() == nullptr) {
            HANDLE noEvent = nullptr;
            if (!s.event.compare_exchange_strong(noEvent, e)) continue;
            if (!s.word.compare_exchange_strong(none, (void*)word)) {
                s.event = nullptr;
                continue;
            }
            return true;
        }
    }
    CloseHandle(e);
    return false;
}

inline void unbindWake(const void* word) {
    for (auto& s : wakeSlots)
        if (s.word.load() == word) {
            HANDLE e = s.event.load();
            s.word = nullptr;
            s.event = nullptr;
            if (e) CloseHandle(e);
        }
}

inline void wake(std::atomic<uint32_t>* word) {
    if (HANDLE e = wakeEvent(word)) SetEvent(e);
}

// waits while *word == value, up to `seconds` (< 0: forever); true if it changed
inline bool waitChange(std::atomic<uint32_t>* word, uint32_t value, double seconds) {
    using clock = std::chrono::steady_clock;
    auto end = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds < 0 ? 0 : seconds));
    HANDLE e = wakeEvent(word);
    while (word->load(std::memory_order_acquire) == value) {
        DWORD ms = INFINITE;
        if (seconds >= 0) {
            auto left = end - clock::now();
            if (left <= clock::duration::zero()) return false;
            // (rounded up: a wait of 0 ms would spin)
            ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(left + std::chrono::microseconds(999)).count();
        }
        if (e) WaitForSingleObject(e, ms);
        else std::this_thread::sleep_for(std::chrono::microseconds(200));  // (not bound: polled)
    }
    return true;
}

}  // namespace bridge
