#include "audio/realtime_promotion.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/util.h"

namespace realtime {

namespace {

constexpr int DIRECT_PRIORITY = 70;      // (the plugin hosts ask for the same: winehost/host.cpp)
constexpr int DIRECT_PRIORITY_LOW = 10;  // (RLIMIT_RTPRIO may allow less)

// step 1 for the calling thread: SCHED_FIFO, at 70 or else 10
bool askCallingThreadDirectly(int& priority) {
    const int lowest = sched_get_priority_min(SCHED_FIFO), highest = sched_get_priority_max(SCHED_FIFO);
    for (int wanted : {DIRECT_PRIORITY, DIRECT_PRIORITY_LOW}) {
        sched_param parameters{};
        parameters.sched_priority = std::max(lowest, std::min(highest, wanted));
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0) {
            priority = parameters.sched_priority;
            return true;
        }
    }
    return false;
}

// the real-time priority thread `threadId` already runs at; false if it isn't real time
bool currentRealtimePriority(long threadId, int& priority) {
    int policy = sched_getscheduler((pid_t)threadId);
    if (policy < 0) return false;
    policy &= ~SCHED_RESET_ON_FORK;  // (rtkit adds it)
    sched_param parameters{};
    if ((policy != SCHED_FIFO && policy != SCHED_RR) || sched_getparam((pid_t)threadId, &parameters) != 0) return false;
    priority = parameters.sched_priority;
    return true;
}

// the calling thread's Linux thread id
long callingThreadId() { return (long)syscall(SYS_gettid); }

// thread `threadId` of this process back to normal priority (SCHED_OTHER, nice 0: rtkit then no longer counts it)
bool dropToNormalPriority(long threadId) {
    sched_param parameters{};  // (SCHED_RESET_ON_FORK kept: rtkit set it, and only a privileged thread may clear it)
    return sched_setscheduler((pid_t)threadId, SCHED_OTHER | SCHED_RESET_ON_FORK, &parameters) == 0 &&
           setpriority(PRIO_PROCESS, (id_t)threadId, 0) == 0;
}

// rtkit answers at most 25 requests per user in 20 s (more audio threads than that, or a burst of plugin hosts, are
// refused as "busy"): the threads refused are asked for again, a window later, a few times
constexpr auto RTKIT_BURST_WINDOW = std::chrono::seconds(21);
constexpr int RETRIES = 3;

struct RefusedThread {
    int processId;
    long threadId;
    ThreadRole role;
    int triesLeft;
};
std::mutex refusedLock;
std::vector<RefusedThread> refusedThreads;
bool retrying = false;  // (a thread runs retryRefusedThreads)

// asks rtkit again for each refused thread still running, a burst window apart, until none is left
void retryRefusedThreads() {
    std::unique_lock<std::mutex> hold(refusedLock);
    while (!refusedThreads.empty()) {
        hold.unlock();
        std::this_thread::sleep_for(RTKIT_BURST_WINDOW);
        hold.lock();
        std::vector<RefusedThread> batch, again;
        batch.swap(refusedThreads);
        hold.unlock();
        for (RefusedThread& thread : batch) {
            if (!util::isDir("/proc/" + std::to_string(thread.processId) + "/task/" + std::to_string(thread.threadId)))
                continue;  // (ended meanwhile)
            const long threadId = thread.threadId;
            Outcome outcome = promoteThread(thread.processId, threadId, systemSteps([threadId](int& priority) {
                                                return currentRealtimePriority(threadId, priority);
                                            }));
            if (outcome.realtime && thread.processId == getpid()) demoteOnOverrun(threadId);
            recordOutcome(thread.role, thread.processId, threadId, outcome);
            if (!outcome.realtime && --thread.triesLeft > 0) again.push_back(thread);
        }
        hold.lock();
        refusedThreads.insert(refusedThreads.end(), again.begin(), again.end());
    }
    retrying = false;
}

// a refused thread asked for again later (retryRefusedThreads, started when not running)
void askAgainLater(int processId, long threadId, ThreadRole role) {
    std::lock_guard<std::mutex> hold(refusedLock);
    refusedThreads.push_back({processId, threadId, role, RETRIES});
    if (retrying) return;
    try {
        std::thread(retryRefusedThreads).detach();
        retrying = true;
    } catch (...) {  // (out of threads: not asked again)
        refusedThreads.clear();
    }
}

}  // namespace

Outcome promoteThread(int processId, long threadId, const PromotionSteps& steps) {
    Outcome outcome;
    int priority = 0;
    if (steps.askDirectly && steps.askDirectly(priority)) {
        outcome.realtime = true;
        outcome.how = "direct";
        outcome.level = priority;
        return outcome;
    }
    std::string rtkitError;
    RtkitLimits limits;
    if (!steps.readRtkitLimits(limits, rtkitError)) {
        outcome.whyNot = "the system allows none (ulimit -r is 0), and " + rtkitError;
        return outcome;
    }
    std::string realtimeError;
    if (limits.maxRealtimePriority < 1) {
        realtimeError = "rtkit allows no real-time priority";
    } else if (steps.limitRealtimeTime(processId, limits.realtimeTimeMaxMicroseconds, realtimeError) &&
               steps.rtkitRealtime(processId, threadId, limits.maxRealtimePriority, realtimeError)) {
        outcome.realtime = true;
        outcome.how = "rtkit";
        outcome.level = limits.maxRealtimePriority;
        return outcome;
    }
    std::string niceError;
    if (steps.rtkitHighPriority(processId, threadId, limits.minNiceLevel, niceError)) {
        outcome.highNice = true;
        outcome.how = "rtkit";
        outcome.level = limits.minNiceLevel;
        outcome.whyNot = realtimeError;
        return outcome;
    }
    outcome.whyNot = realtimeError + "; nor a higher nice level: " + niceError;
    return outcome;
}

PromotionSteps systemSteps(std::function<bool(int& priority)> askDirectly) {
    return {std::move(askDirectly), readRtkitLimits, limitRealtimeTime, rtkitMakeRealtime, rtkitMakeHighPriority};
}

Outcome raiseCallingThread(ThreadRole role) {
    const int processId = getpid();
    const long threadId = callingThreadId();
    Outcome outcome = promoteThread(processId, threadId, systemSteps(askCallingThreadDirectly));
    if (outcome.realtime) demoteOnOverrun(threadId);
    recordOutcome(role, processId, threadId, outcome);
    if (!outcome.realtime) askAgainLater(processId, threadId, role);
    return outcome;
}

Outcome raisePluginHostThread(int processId, long threadId) {
    // (step 1: the host asked for SCHED_FIFO itself; it may have got it)
    const PromotionSteps hostSteps =
        systemSteps([threadId](int& priority) { return currentRealtimePriority(threadId, priority); });
    const long worker = realtimeWorkerOf(getpid());
    Outcome outcome;
    if (worker == 0) {
        outcome = promoteThread(processId, threadId, hostSteps);
    } else {  // (refused, it takes that worker's place: realtime_promotion.h)
        bool dropped = false;
        Outcome workerOutcome;
        outcome = promoteHostOverWorker(
            processId, threadId, getpid(), worker, hostSteps,
            systemSteps([worker](int& priority) { return currentRealtimePriority(worker, priority); }),
            [&dropped](long workerThreadId) { return dropped = dropToNormalPriority(workerThreadId); }, workerOutcome);
        if (dropped) {
            recordOutcome(ThreadRole::Worker, getpid(), worker, workerOutcome);
            if (workerOutcome.realtime) demoteOnOverrun(worker);
            else if (!outcome.realtime) askAgainLater(getpid(), worker, ThreadRole::Worker);  // (both refused: a burst)
        }
    }
    recordOutcome(ThreadRole::PluginHost, processId, threadId, outcome);
    if (!outcome.realtime) askAgainLater(processId, threadId, ThreadRole::PluginHost);
    return outcome;
}

void recordCallingThreadAsItIs(ThreadRole role, const std::string& how) {
    const long threadId = callingThreadId();
    Outcome outcome;
    outcome.how = how;
    outcome.realtime = currentRealtimePriority(threadId, outcome.level);
    if (!outcome.realtime) outcome.whyNot = how + " didn't make it real time";
    recordOutcome(role, getpid(), threadId, outcome);
}

}  // namespace realtime
