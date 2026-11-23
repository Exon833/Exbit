// core/posix_win.h's functions (Windows)
#include "core/posix_win.h"

#include <sys/utime.h>
#include <windows.h>

#include <cerrno>
#include <climits>
#include <cstdint>

static_assert(sizeof(off_t) == 8, "-D_FILE_OFFSET_BITS=64 is needed (files over 2 GB)");

namespace {
HANDLE osHandle(int fd) { return (HANDLE)_get_osfhandle(fd); }

ssize_t positioned(int fd, void* rbuf, const void* wbuf, size_t n, off_t offset) {
    HANDLE h = osHandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD done = 0, want = (DWORD)(n > 0x40000000u ? 0x40000000u : n);
    BOOL ok = rbuf ? ReadFile(h, rbuf, want, &done, &ov) : WriteFile(h, wbuf, want, &done, &ov);
    if (!ok) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t)done;
}
}  // namespace

ssize_t pread(int fd, void* buf, size_t n, off_t offset) { return positioned(fd, buf, nullptr, n, offset); }
ssize_t pwrite(int fd, const void* buf, size_t n, off_t offset) { return positioned(fd, nullptr, buf, n, offset); }

int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
    if (dirfd != AT_FDCWD || flags != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!times) return _utime64(path, nullptr);
    __utimbuf64 t{times[0].tv_sec, times[1].tv_sec};
    return _utime64(path, &t);
}

int flock(int fd, int op) {
    HANDLE h = osHandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    if (op & LOCK_UN) return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
    DWORD flags = (op & LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (op & LOCK_NB ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) return 0;
    errno = EWOULDBLOCK;
    return -1;
}

int kill(pid_t pid, int sig) {
    HANDLE p = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!p) {
        errno = ESRCH;
        return -1;
    }
    BOOL ok = TRUE;
    if (sig != 0) ok = TerminateProcess(p, 128 + (UINT)sig);  // (every signal but 0 ends it: Windows has no others)
    CloseHandle(p);
    if (!ok) errno = EPERM;
    return ok ? 0 : -1;
}
