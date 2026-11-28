// The audio threads' setup (audio/realtime_priority.h, audio/denormals.h): denormals flushed (FTZ+DAZ) on a new audio
// thread and not elsewhere; the outcomes as the Audio properties dialog says them. Linux: the order of the ways to
// real-time priority with a stand-in system and rtkit (SCHED_FIFO, else rtkit's real time after RLIMIT_RTTIME is
// lowered, else rtkit's nice level, else nothing), RLIMIT_RTTIME lowered on another process, and on this machine's own
// rtkit (when it grants it): a thread made real time, and dropped back instead of killed when it runs too long without
// a pause (SIGXCPU). Windows: MMCSS, the thread's power throttling and the process's timer resolution, or their
// fallbacks. Linux and Windows.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include "audio/power_throttling_win.h"
#else
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio/realtime_promotion.h"
#endif

#include "audio/denormals.h"
#include "audio/realtime_priority.h"
#include "check.h"

namespace audio::detail {
void raiseThreadPriority();  // (audio/devices.cpp, audio/devices_win.cpp)
}

namespace {

#ifndef _WIN32
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
#endif

// a new audio thread has FTZ and DAZ, and flushes a denormal to zero; the thread that made it doesn't
void testDenormalsOnANewAudioThread() {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t audioControl = 0;
    float audioResult = 1;
    std::thread audioThread([&] {
        audio::detail::raiseThreadPriority();
        audioControl = audio::readFloatingPointControl();
        volatile float tiny = 1e-30f;
        audioResult = tiny * 1e-10f;  // (1e-40: a denormal, unless flushed)
    });
    audioThread.join();
    printf("   audio thread's MXCSR %#x\n", audioControl);
    CHECK((audioControl & 0x8040) == 0x8040);
    CHECK(audioResult == 0.0f);
    CHECK((audio::readFloatingPointControl() & 0x8040) == 0);  // (only the audio thread's own register)
    volatile float tiny = 1e-30f;
    CHECK(tiny * 1e-10f != 0.0f);
#else
    CHECK(!audio::flushDenormalsOnThisThread());  // (compiled out off x86)
#endif
}

// the dialog's words for each outcome
void testOutcomeTexts() {
    realtime::Outcome granted;
    granted.realtime = true;
    granted.how = "rtkit";
    granted.level = 20;
    CHECK(realtime::describeOutcome(granted) == "granted (rtkit, 20)");
    realtime::Outcome mmcss;
    mmcss.realtime = true;
    mmcss.how = "MMCSS Pro Audio";
    CHECK(realtime::describeOutcome(mmcss) == "granted (MMCSS Pro Audio)");
    realtime::Outcome nice;
    nice.highNice = true;
    nice.how = "rtkit";
    nice.level = -15;
    nice.whyNot = "rtkit refused";
    CHECK(realtime::describeOutcome(nice) == "high priority only (rtkit, nice -15): rtkit refused");
    realtime::Outcome refused;
    refused.whyNot = "no rtkit";
    CHECK(realtime::describeOutcome(refused) == "not granted: no rtkit");
    realtime::forgetOutcomes();
    CHECK(realtime::statusText() == "Not asked for yet (no audio thread is running)");
}

#ifndef _WIN32

long thisThreadId() { return (long)syscall(SYS_gettid); }

// a stand-in system and rtkit: each step answers as told, and says it was taken
struct FakeSystem {
    bool direct = false, rtkitThere = true, timeLimited = true, realtimeGranted = true, niceGranted = true;
    realtime::RtkitLimits limits{20, -15, 200000};
    std::vector<std::string> steps;

    realtime::PromotionSteps table() {
        realtime::PromotionSteps table;
        table.askDirectly = [this](int& priority) {
            steps.push_back("direct");
            priority = 70;
            return direct;
        };
        table.readRtkitLimits = [this](realtime::RtkitLimits& got, std::string& error) {
            steps.push_back("limits");
            got = limits;
            if (!rtkitThere) error = "rtkit can't be reached";
            return rtkitThere;
        };
        table.limitRealtimeTime = [this](int processId, int64_t maxMicroseconds, std::string& error) {
            steps.push_back("rttime " + std::to_string(processId) + " " + std::to_string(maxMicroseconds));
            if (!timeLimited) error = "RLIMIT_RTTIME can't be lowered";
            return timeLimited;
        };
        table.rtkitRealtime = [this](int processId, long threadId, int priority, std::string& error) {
            steps.push_back("realtime " + std::to_string(processId) + " " + std::to_string(threadId) + " " +
                            std::to_string(priority));
            if (!realtimeGranted) error = "rtkit refused";
            return realtimeGranted;
        };
        table.rtkitHighPriority = [this](int processId, long threadId, int niceLevel, std::string& error) {
            steps.push_back("nice " + std::to_string(processId) + " " + std::to_string(threadId) + " " +
                            std::to_string(niceLevel));
            if (!niceGranted) error = "rtkit refused that too";
            return niceGranted;
        };
        return table;
    }
    std::string taken() const {
        std::string all;
        for (const auto& step : steps) all += (all.empty() ? "" : ", ") + step;
        return all;
    }
};

// the order of the ways, and what each outcome says
void testPromotionOrder() {
    {  // 1. SCHED_FIFO directly: nothing else asked
        FakeSystem system;
        system.direct = true;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct");
        CHECK(realtime::describeOutcome(outcome) == "granted (direct, 70)");
    }
    {  // 2. rtkit: RLIMIT_RTTIME lowered to its maximum first, at its highest priority
        FakeSystem system;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits, rttime 100 200000, realtime 100 101 20");
        CHECK(realtime::describeOutcome(outcome) == "granted (rtkit, 20)");
    }
    {  // rtkit allows less: no higher than its MaxRealtimePriority
        FakeSystem system;
        system.limits.maxRealtimePriority = 5;
        realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits, rttime 100 200000, realtime 100 101 5");
    }
    {  // 3. real time refused: rtkit's lowest nice level
        FakeSystem system;
        system.realtimeGranted = false;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits, rttime 100 200000, realtime 100 101 20, nice 100 101 -15");
        CHECK(realtime::describeOutcome(outcome) == "high priority only (rtkit, nice -15): rtkit refused");
    }
    {  // RLIMIT_RTTIME can't be lowered: rtkit would refuse, so not asked for real time
        FakeSystem system;
        system.timeLimited = false;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits, rttime 100 200000, nice 100 101 -15");
        CHECK(outcome.highNice && contains(outcome.whyNot, "RLIMIT_RTTIME"));
    }
    {  // rtkit allows no real time at all
        FakeSystem system;
        system.limits.maxRealtimePriority = 0;
        realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits, nice 100 101 -15");
    }
    {  // 4. nothing granted: both reasons said
        FakeSystem system;
        system.realtimeGranted = system.niceGranted = false;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(!outcome.realtime && !outcome.highNice);
        CHECK(realtime::describeOutcome(outcome) ==
              "not granted: rtkit refused; nor a higher nice level: rtkit refused that too");
    }
    {  // no rtkit (or no libdbus): nothing more asked
        FakeSystem system;
        system.rtkitThere = false;
        realtime::Outcome outcome = realtime::promoteThread(100, 101, system.table());
        CHECK(system.taken() == "direct, limits");
        CHECK(realtime::describeOutcome(outcome) ==
              "not granted: the system allows none (ulimit -r is 0), and rtkit can't be reached");
    }
}

// RLIMIT_RTTIME of another process lowered to rtkit's maximum, its soft limit a quarter below; never raised
void testRealtimeTimeLimit() {
    pid_t child = fork();
    if (child == 0) {
        pause();  // (until killed)
        _exit(0);
    }
    CHECK(child > 0);
    std::string error;
    CHECK(realtime::limitRealtimeTime(child, 200000, error));
    rlimit limits{};
    CHECK(prlimit(child, RLIMIT_RTTIME, nullptr, &limits) == 0);
    CHECK(limits.rlim_max == 200000 && limits.rlim_cur == 150000);
    CHECK(realtime::limitRealtimeTime(child, 400000, error));  // (a larger maximum leaves it as it is)
    CHECK(prlimit(child, RLIMIT_RTTIME, nullptr, &limits) == 0);
    CHECK(limits.rlim_max == 200000 && limits.rlim_cur == 150000);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    CHECK(!realtime::limitRealtimeTime(child, 200000, error) && !error.empty());  // (no such process now)
}

bool runsRealtime(long threadId) {
    int policy = sched_getscheduler((pid_t)threadId) & ~SCHED_RESET_ON_FORK;
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

// this machine's own way: a new audio thread as the sound device makes it; with rtkit's real time, one that then runs
// past RLIMIT_RTTIME's soft limit is dropped back to normal priority, and the program goes on
void testOnThisMachine() {
    realtime::forgetOutcomes();
    std::atomic<bool> release{false};
    std::atomic<long> audioThreadId{0};
    realtime::Outcome outcome;
    std::thread audioThread([&] {
        audio::detail::raiseThreadPriority();
        audioThreadId = thisThreadId();
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    while (audioThreadId.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::string status = realtime::statusText();
    printf("   on this machine:\n%s\n", status.c_str());
    CHECK(contains(status, "Audio thread: "));
    CHECK(contains(status, "Audio thread: granted") == runsRealtime(audioThreadId.load()));  // (as /proc says)
    release = true;
    audioThread.join();
    CHECK(realtime::statusText() == "Not asked for yet (no audio thread is running)");  // (an ended thread is left out)

    // a thread recorded as real time that isn't (any more) is shown as dropped
    realtime::Outcome claimed;
    claimed.realtime = true;
    claimed.how = "rtkit";
    claimed.level = 20;
    realtime::recordOutcome(realtime::ThreadRole::Worker, getpid(), thisThreadId(), claimed);
    CHECK(contains(realtime::statusText(), "Workers: 0 of 1 not granted: dropped"));
    realtime::forgetOutcomes();

    // the overrun: only where rtkit granted real time (the limit is then 200 ms)
    std::atomic<bool> grantedByRtkit{false}, demoted{false};
    std::thread busyThread([&] {
        grantedByRtkit = realtime::raiseCallingThread(realtime::ThreadRole::DeviceThread).how == "rtkit" &&
                         runsRealtime(thisThreadId());
        if (!grantedByRtkit) return;
        // (on purpose: runs without a pause past the soft limit, 150 ms, short of the hard one, 200 ms)
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(195);
        while (std::chrono::steady_clock::now() < until && runsRealtime(thisThreadId())) {
        }
        demoted = !runsRealtime(thisThreadId());
        printf("   ran %.0f ms without a pause before it was dropped back\n", 195 - std::chrono::duration<double, std::milli>(until - std::chrono::steady_clock::now()).count());
    });
    busyThread.join();
    if (grantedByRtkit) {
        CHECK(demoted.load());  // (and this test still runs: SIGXCPU didn't end it)
        printf("   a thread that ran past RLIMIT_RTTIME's soft limit was dropped back, the program went on\n");
    } else {
        printf("   (rtkit didn't grant real time here: the overrun isn't tried)\n");
    }
}

#else

// MMCSS on each audio thread (or the fallback), power throttling off for it, the process's timer resolution kept
void testWindowsCalls() {
    realtime::forgetOutcomes();
    bool fullSpeed = false;
    std::string status;
    std::thread audioThread([&] {
        audio::detail::raiseThreadPriority();
        status = realtime::statusText();
        fullSpeed = audio::keepThreadAtFullSpeed();  // (asked again, to see the answer: Windows can't be asked back
                                                     // on Windows 10, GetThreadInformation refuses this class)
    });
    audioThread.join();
    printf("   %s\n", status.c_str());
    CHECK(status == "Audio thread: granted (MMCSS Pro Audio)" ||
          status.rfind("Audio thread: not granted: MMCSS isn't running", 0) == 0);  // (MMCSS, or its fallback)
    OSVERSIONINFOW version{sizeof version};
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);  // (ntdll's: GetVersionEx says 6.2 to programs without a manifest)
    auto getVersion = reinterpret_cast<GetVersion>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    const bool hasPowerThrottling = getVersion && getVersion(&version) == 0 && version.dwBuildNumber >= 16299;
    printf("   Windows build %lu: the audio thread's power throttling off: %s\n", version.dwBuildNumber,
           fullSpeed ? "yes" : "no (the fallback: Windows chooses)");
    CHECK(fullSpeed == hasPowerThrottling);  // (Windows 10 1709, build 16299, and later have it)
    printf("   the process's timer resolution kept: %s\n",
           audio::keepProcessTimerResolution() ? "yes" : "no (this Windows hasn't the setting: the fallback)");
}

#endif

}  // namespace

int main() {
    printf("== denormals on a new audio thread\n");
    testDenormalsOnANewAudioThread();
    printf("== the outcomes' texts\n");
    testOutcomeTexts();
#ifndef _WIN32
    printf("== the order of the ways to real-time priority\n");
    testPromotionOrder();
    printf("== RLIMIT_RTTIME lowered\n");
    testRealtimeTimeLimit();
    printf("== on this machine\n");
    testOnThisMachine();
#else
    printf("== Windows' calls\n");
    testWindowsCalls();
#endif
    TEST_MAIN_END();
}
