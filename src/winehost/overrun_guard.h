// The plugin host's audio thread under RLIMIT_RTTIME: Exbit lowers the host's limit to rtkit's maximum (200 ms) to
// have rtkit make that thread real time (audio/realtime_promotion.h). A real-time thread that runs past the soft limit
// without a pause is sent SIGXCPU, which would end the host; caught here, it drops the audio thread back to normal
// priority instead. The handler makes its one system call itself, with no C library: under Wine the thread may be in
// the plugin's Windows code when the signal comes, where the C library's thread data can't be trusted.
// C and C++: for the Winelib host (host.cpp) and the 32-bit Windows-built host's Linux side (unixside.c).
#pragma once
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>

static volatile long exbitGuardedAudioThread = 0;  // (its Linux thread id)

// SIGXCPU: the guarded audio thread back to SCHED_OTHER (SCHED_RESET_ON_FORK kept: rtkit sets it, and a user may not
// clear it)
static void exbitDemoteGuardedAudioThread(int signalNumber) {
    (void)signalNumber;
#if defined(__x86_64__)
    static const struct sched_param normalPriority = {0};
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "0"((long)SYS_sched_setscheduler), "D"(exbitGuardedAudioThread),
                       "S"((long)(SCHED_OTHER | SCHED_RESET_ON_FORK)), "d"(&normalPriority)
                     : "rcx", "r11", "memory");
    (void)result;
#endif
}

// the audio thread `threadId` guarded: SIGXCPU demotes it from now on
static void exbitGuardAudioThreadAgainstOverrun(long threadId) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    exbitGuardedAudioThread = threadId;
#if defined(__x86_64__)
    action.sa_handler = exbitDemoteGuardedAudioThread;
    action.sa_flags = SA_RESTART | SA_ONSTACK;  // (Wine gives each thread a signal stack of its own)
#else
    // shortcut: a 32-bit Winelib host ignores SIGXCPU (its thread is killed only at the hard limit, the soft one
    // passing unseen); a demoting handler would need i386's system call, when 32-bit Wine hosts matter
    action.sa_handler = SIG_IGN;
#endif
    sigemptyset(&action.sa_mask);
    sigaction(SIGXCPU, &action, NULL);
}
