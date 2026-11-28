// The Linux side of the 32-bit Wine host built as a plain Windows program (unixside.h):
// exbit-unix32.so, a Unix library Wine loads into the host's process for exbit-unix32.dll
// (unix32.c), which hands it a LinuxCall. Linux code in a 64-bit process, given 32-bit pointers.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "winehost/overrun_guard.h"
#include "winehost/unixside.h"

#define P(x) ((void*)(uintptr_t)(x))

static int64_t result(long r) { return r < 0 ? -errno : r; }

static int64_t connectUnix(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

// as host.cpp's realtime(): a little below Exbit's own audio thread, which waits for this one; and SIGXCPU caught,
// for when Exbit has rtkit make it real time instead (winehost/overrun_guard.h)
static int64_t realtime(void) {
    exbitGuardAudioThreadAgainstOverrun(syscall(SYS_gettid));
    int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
    struct sched_param sp = {.sched_priority = lo + 69 < hi ? lo + 69 : hi};
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
        return 0;
    sp.sched_priority = lo + 9;
    return result(sched_setscheduler(0, SCHED_FIFO, &sp));
}

static int call(void* args) {
    struct LinuxCall* c = args;
    struct stat st;
    struct timespec t = {(time_t)(c->ns / 1000000000), (long)(c->ns % 1000000000)};
    switch (c->fn) {
    case LINUX_CONNECT: c->ret = connectUnix(P(c->ptr)); break;
    case LINUX_SEND: c->ret = result(send(c->a, P(c->ptr), (size_t)c->b, c->c)); break;
    case LINUX_RECV: c->ret = result(recv(c->a, P(c->ptr), (size_t)c->b, c->c)); break;
    case LINUX_POLL: c->ret = result(poll(P(c->ptr), (nfds_t)c->a, c->b)); break;
    case LINUX_READ: c->ret = result(read(c->a, P(c->ptr), (size_t)c->b)); break;
    case LINUX_IS_FIFO: c->ret = fstat(c->a, &st) == 0 && S_ISFIFO(st.st_mode); break;
    case LINUX_FUTEX:
        c->ret = result(syscall(SYS_futex, P(c->ptr), c->a, (uint32_t)c->b, c->ns >= 0 ? &t : NULL, NULL, 0));
        break;
    case LINUX_PID: c->ret = getpid(); break;
    case LINUX_REALTIME: c->ret = realtime(); break;
    case LINUX_THREAD_ID: c->ret = syscall(SYS_gettid); break;
    default: c->ret = -ENOSYS;
    }
    return 0;
}

// (Wine's tables: the 32-bit host calls through the WoW64 one; the layout needs no conversion)
__attribute__((visibility("default"))) const void* __wine_unix_call_funcs[] = {call};
__attribute__((visibility("default"))) const void* __wine_unix_call_wow64_funcs[] = {call};
