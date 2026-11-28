// RLIMIT_RTTIME, which rtkit requires (no process gets real time from it with a hard limit above rtkit's 200 ms):
// a real-time thread that runs that long without a pause is sent SIGXCPU at the soft limit, and killed at the hard
// one. Exbit catches SIGXCPU and drops its real-time threads back to normal priority, so a stuck audio thread costs
// the real-time priority rather than the whole program. (The plugin hosts catch it themselves: winehost/host.cpp.)
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "audio/realtime_promotion.h"

namespace realtime {

namespace {

constexpr int WATCHED_THREAD_SLOTS = 256;  // (more than any machine's audio threads; the oldest are reused)
std::atomic<long> watchedThreads[WATCHED_THREAD_SLOTS];
std::atomic<int> nextWatchedSlot{0};
rlimit ownLimits{};  // (this process's RLIMIT_RTTIME as lowered; 0: not lowered)

// SIGXCPU: every watched thread still in this process back to SCHED_OTHER (async-signal-safe calls only)
void demoteWatchedThreads(int) {
    const int savedErrno = errno;
    const long processId = getpid();
    sched_param normal{};
    for (auto& slot : watchedThreads) {
        const long threadId = slot.load(std::memory_order_relaxed);
        // (tgkill with no signal: only a thread of this process; an id an ended thread left may be another's now.
        // SCHED_RESET_ON_FORK kept: rtkit sets it, and a user may not clear it)
        if (threadId > 0 && syscall(SYS_tgkill, processId, threadId, 0) == 0)
            syscall(SYS_sched_setscheduler, threadId, SCHED_OTHER | SCHED_RESET_ON_FORK, &normal);
    }
    // (Linux raised the soft limit by a second with the signal, past the hard one: back, or the next thread to run
    // too long would be killed at the hard limit with no SIGXCPU first)
    if (ownLimits.rlim_max) syscall(SYS_prlimit64, 0, RLIMIT_RTTIME, &ownLimits, nullptr);
    static const char message[] = "exbit: an audio thread ran too long without a pause: back to normal priority\n";
    ssize_t written = write(2, message, sizeof message - 1);
    (void)written;
    errno = savedErrno;
}

// SIGXCPU caught from now on
void catchOverrunOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_handler = demoteWatchedThreads;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGXCPU, &action, nullptr);
    });
}

}  // namespace

bool limitRealtimeTime(int processId, int64_t maxMicroseconds, std::string& error) {
    if (maxMicroseconds <= 0) {
        error = "rtkit gave no time limit";
        return false;
    }
    if (processId == getpid()) catchOverrunOnce();  // (before the limit: never a SIGXCPU that ends the program)
    rlimit current{};
    if (prlimit(processId, RLIMIT_RTTIME, nullptr, &current) != 0) {
        error = std::string("RLIMIT_RTTIME can't be read: ") + strerror(errno);
        return false;
    }
    rlimit lowered = current;
    lowered.rlim_max = std::min<rlim_t>(current.rlim_max, (rlim_t)maxMicroseconds);
    lowered.rlim_cur = std::min<rlim_t>(current.rlim_cur, lowered.rlim_max - lowered.rlim_max / 4);
    if (prlimit(processId, RLIMIT_RTTIME, &lowered, nullptr) != 0) {
        error = std::string("RLIMIT_RTTIME can't be lowered: ") + strerror(errno);
        return false;
    }
    if (processId == getpid()) ownLimits = lowered;
    return true;
}

void demoteOnOverrun(long threadId) {
    catchOverrunOnce();
    watchedThreads[nextWatchedSlot.fetch_add(1) % WATCHED_THREAD_SLOTS].store(threadId);
}

}  // namespace realtime
