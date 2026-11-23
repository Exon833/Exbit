// The 32-bit Wine host built as a plain Windows program (TINYDAW_WINE_PE: with MinGW, where Wine is
// built only for WoW64 and has no 32-bit Unix part for winegcc -m32): its Linux calls.
//
// Windows code can't make a Linux syscall under Wine (Wine's syscall dispatch takes one for an NT
// call), so the host's few Linux calls (the socket to Tiny DAW, the audio thread's futex, its
// realtime priority, its Linux process id) go to a Linux library of its own, unixside.c
// (wine32/x86_64-unix/tinydaw-unix32.so), through Wine's way for a builtin DLL into its Unix part:
// unix32.c (wine32/i386-windows/tinydaw-unix32.dll). Wine finds both through WINEDLLPATH=<the host's
// folder>/wine32 (plugins/wine.cpp sets it), and the host loads the DLL by its path. The fds are
// Linux ones, held as ints; the calls keep their Linux meaning (bridge.h is the same code).
//
// This header is C for unixside.c, and C++ for the host (the wrappers below).
#pragma once
#include <stdint.h>

enum LinuxFn {
    LINUX_CONNECT = 1,  // ptr: a Unix socket's path -> ret: the fd
    LINUX_SEND,         // a: fd, ptr, b: size, c: flags
    LINUX_RECV,         // a: fd, ptr, b: size, c: flags
    LINUX_POLL,         // ptr: pollfds, a: how many, b: ms
    LINUX_READ,         // a: fd, ptr, b: size
    LINUX_IS_FIFO,      // a: fd -> ret: 1 if a pipe
    LINUX_FUTEX,        // ptr: the word, a: op, b: value, ns: timeout (< 0: none)
    LINUX_PID,          // -> ret: the Linux process id
    LINUX_REALTIME,     // the calling thread to SCHED_FIFO (as host.cpp's realtime())
};

// (32-bit fields for pointers: the same layout on both sides, so Wine's WoW64 needs no conversion)
struct LinuxCall {
    uint32_t fn, ptr;
    int32_t a, b, c, pad;
    int64_t ns;
    int64_t ret;  // < 0: -errno (Linux's numbers)
};
#ifdef __cplusplus
static_assert(sizeof(LinuxCall) == 40, "LinuxCall");
#else
_Static_assert(sizeof(struct LinuxCall) == 40, "LinuxCall");
#endif

#if defined(__cplusplus) && defined(TINYDAW_WINE_PE)
#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>

#include <sys/types.h>

// Linux's values (the .so passes them on as they are)
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define POLLIN 0x1
#define POLLOUT 0x4
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
struct pollfd {
    int fd;
    short events, revents;
};

namespace unixside {

inline int64_t call(LinuxCall c) {
    using Fn = long(__cdecl*)(void*);
    static Fn fn = [] {
        std::u16string dll(1024, u'\0');
        dll.resize(GetModuleFileNameW(nullptr, &dll[0], (DWORD)dll.size()));
        dll = dll.substr(0, dll.find_last_of(u'\\') + 1) + u"wine32\\i386-windows\\tinydaw-unix32.dll";
        HMODULE m = LoadLibraryW(dll.c_str());
        Fn f = m ? (Fn)GetProcAddress(m, "tinydaw_unix_call") : nullptr;
        if (!f)
            fprintf(stderr, "tinydaw-wine-host-32: its Linux side (wine32/ next to it, through WINEDLLPATH) "
                            "can't be loaded\n");
        return f;
    }();
    if (!fn || fn(&c) != 0)
        c.ret = -38;  // (ENOSYS)
    // (errno gets Linux's number: the ones the callers look at, EINTR and EAGAIN, are the CRT's too)
    if (c.ret < 0)
        errno = (int)-c.ret;
    return c.ret;
}

inline uint32_t ptr(const void* p) { return (uint32_t)(uintptr_t)p; }

inline int connectUnix(const char* path) { return (int)call({LINUX_CONNECT, ptr(path), 0, 0, 0, 0, 0, 0}); }
inline int futex(void* word, int op, uint32_t value, const timespec* timeout) {
    int64_t ns = timeout ? (int64_t)timeout->tv_sec * 1000000000 + timeout->tv_nsec : -1;
    return (int)call({LINUX_FUTEX, ptr(word), op, (int32_t)value, 0, 0, ns, 0});
}
inline ssize_t read(int fd, void* p, size_t n) { return (ssize_t)call({LINUX_READ, ptr(p), fd, (int32_t)n, 0, 0, 0, 0}); }
inline bool isFifo(int fd) { return call({LINUX_IS_FIFO, 0, fd, 0, 0, 0, 0, 0}) == 1; }
inline int linuxPid() { return (int)call({LINUX_PID, 0, 0, 0, 0, 0, 0, 0}); }
inline void realtime() { call({LINUX_REALTIME, 0, 0, 0, 0, 0, 0, 0}); }

}  // namespace unixside

// the socket calls bridge.h makes, on Linux fds
inline ssize_t send(int fd, const void* p, size_t n, int flags) {
    return (ssize_t)unixside::call({LINUX_SEND, unixside::ptr(p), fd, (int32_t)n, flags, 0, 0, 0});
}
inline ssize_t recv(int fd, void* p, size_t n, int flags) {
    return (ssize_t)unixside::call({LINUX_RECV, unixside::ptr(p), fd, (int32_t)n, flags, 0, 0, 0});
}
inline int poll(pollfd* fds, unsigned long n, int ms) {
    return (int)unixside::call({LINUX_POLL, unixside::ptr(fds), (int32_t)n, ms, 0, 0, 0, 0});
}
#endif
