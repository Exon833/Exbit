#include "audio/realtime_priority.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/util.h"

namespace realtime {

namespace {

struct RecordedThread {
    ThreadRole role;
    Outcome outcome;
};

std::mutex recordsLock;
std::map<std::pair<int, long>, RecordedThread> recordedThreads;  // by (process id, thread id)

// whether the thread is still running (an ended one's outcome no longer counts)
bool threadStillRunning(int processId, long threadId) {
#ifdef _WIN32
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)threadId);
    if (!thread) return false;
    DWORD exitCode = 0;
    bool running = GetExitCodeThread(thread, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(thread);
    return running;
#else
    return util::isDir("/proc/" + std::to_string(processId) + "/task/" + std::to_string(threadId));
#endif
}

// a recorded outcome as it is now: on Linux, a thread recorded as real time may have been dropped back since (SIGXCPU,
// audio/realtime_promotion.h)
Outcome outcomeNow(long threadId, Outcome outcome) {
#ifndef _WIN32
    int policy = sched_getscheduler((pid_t)threadId);
    policy &= ~SCHED_RESET_ON_FORK;
    if (outcome.realtime && policy != SCHED_FIFO && policy != SCHED_RR) {
        outcome.realtime = false;
        outcome.whyNot = "dropped, as it ran too long without a pause (RLIMIT_RTTIME)";
    }
#endif
    return outcome;
}

// "Workers: 15 of 15 granted (rtkit, 20)": a role's threads, the first one's outcome standing for the rest that
// got the same
std::string roleLine(const char* roleName, const std::vector<Outcome>& outcomes, bool counted) {
    if (outcomes.empty()) return "";
    int granted = 0;
    const Outcome* firstGranted = nullptr;
    const Outcome* firstRefused = nullptr;
    for (const Outcome& outcome : outcomes) {
        if (outcome.realtime) {
            granted++;
            if (!firstGranted) firstGranted = &outcome;
        } else if (!firstRefused) {
            firstRefused = &outcome;
        }
    }
    std::string line = std::string(roleName) + ": ";
    if (!counted) return line + describeOutcome(outcomes.back()) + "\n";
    line += std::to_string(granted) + " of " + std::to_string(outcomes.size());
    if (firstGranted) line += " " + describeOutcome(*firstGranted);
    if (firstRefused) line += std::string(firstGranted ? "; the rest " : " ") + describeOutcome(*firstRefused);
    return line + "\n";
}

}  // namespace

std::string describeOutcome(const Outcome& outcome) {
    if (outcome.realtime)
        return "granted (" + outcome.how + (outcome.level ? ", " + std::to_string(outcome.level) : "") + ")";
    if (outcome.highNice)
        return "high priority only (" + outcome.how + ", nice " + std::to_string(outcome.level) + ")" +
               (outcome.whyNot.empty() ? "" : ": " + outcome.whyNot);
    return "not granted: " + (outcome.whyNot.empty() ? std::string("unknown") : outcome.whyNot);
}

void recordOutcome(ThreadRole role, int processId, long threadId, const Outcome& outcome) {
    static const char* roleNames[] = {"audio thread", "worker", "plugin host's audio thread"};
    util::debugf("audio: real-time priority of the %s %d/%ld: %s\n", roleNames[(int)role], processId, threadId,
                 describeOutcome(outcome).c_str());
    std::lock_guard<std::mutex> hold(recordsLock);
    recordedThreads[{processId, threadId}] = {role, outcome};
}

std::string statusText() {
    std::vector<Outcome> deviceThreads, workerThreads, hostThreads;
    {
        std::lock_guard<std::mutex> hold(recordsLock);
        for (auto it = recordedThreads.begin(); it != recordedThreads.end();) {
            if (!threadStillRunning(it->first.first, it->first.second)) {
                it = recordedThreads.erase(it);
                continue;
            }
            const RecordedThread& thread = it->second;
            (thread.role == ThreadRole::DeviceThread ? deviceThreads
             : thread.role == ThreadRole::Worker     ? workerThreads
                                                     : hostThreads)
                .push_back(outcomeNow(it->first.second, thread.outcome));
            ++it;
        }
    }
    std::string text = roleLine("Audio thread", deviceThreads, false) + roleLine("Workers", workerThreads, true) +
                       roleLine("Plugin hosts", hostThreads, true);
    if (text.empty()) return "Not asked for yet (no audio thread is running)";
    text.pop_back();  // (the last line's end)
    return text;
}

void forgetOutcomes() {
    std::lock_guard<std::mutex> hold(recordsLock);
    recordedThreads.clear();
}

}  // namespace realtime
