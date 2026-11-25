// Windows (MinGW-w64 on the UCRT): the small POSIX calls the program makes that MinGW lacks, so the files that
// use them build unchanged. Force-included into every file of the Windows build (the Makefile: -include
// core/posix_win.h); never included on Linux. Bigger differences (libraries, mapped files, processes, wake-ups)
// are core/os.h's.
//
// Paths are UTF-8 with '/' everywhere: the program's manifest (tools/windows/exbit.manifest) makes UTF-8 the
// process's code page, so the C library's narrow file functions (fopen, open, stat, opendir…) take UTF-8 names.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX  // (windows.h's min/max macros would break std::min/std::max)
#endif
#ifdef __cplusplus
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// every file opened by open() binary (Windows' default text mode would change "\r\n"), not inherited by children
#ifndef O_CLOEXEC
#define O_CLOEXEC (_O_NOINHERIT | _O_BINARY)
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

// reads n bytes at `offset` (the file position is not used; it may move)
ssize_t pread(int fd, void* buf, size_t n, off_t offset);
// writes n bytes at `offset`
ssize_t pwrite(int fd, const void* buf, size_t n, off_t offset);

inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
inline struct tm* gmtime_r(const time_t* t, struct tm* out) { return gmtime_s(out, t) == 0 ? out : nullptr; }
inline time_t timegm(struct tm* t) { return _mkgmtime(t); }
inline int fsync(int fd) { return _commit(fd); }
inline int mkdir(const char* path, int /*mode*/) { return _mkdir(path); }
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
inline int unsetenv(const char* name) { return _putenv_s(name, "") == 0 ? 0 : -1; }
// (no symbolic links are followed specially: Windows' links and junctions read as what they point at)
inline int lstat(const char* path, struct stat* st) { return stat(path, st); }
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif

// utimensat(AT_FDCWD, path, times, 0): a file's access and modification times (whole seconds)
#define AT_FDCWD (-100)
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);

// flock(): a whole-file lock (LockFileEx), held until LOCK_UN or the file is closed
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int op);

// kill(pid, SIGKILL): ends the process (TerminateProcess); other signals: -1
#ifndef SIGKILL
#define SIGKILL 9
#endif
int kill(pid_t pid, int sig);
#endif  // __cplusplus
