// Linux: how an audio thread gets real-time priority, in this order:
//   1. SCHED_FIFO asked for directly (works when the user's limits allow it: ulimit -r, the realtime group);
//   2. else rtkit's MakeThreadRealtimeWithPID (audio/rtkit.h) at a priority no higher than rtkit allows, after
//      RLIMIT_RTTIME is lowered to rtkit's maximum, with SIGXCPU then dropping the thread back to normal priority
//      instead of ending the program (realtime_overrun_posix.cpp);
//   3. else rtkit's MakeThreadHighPriorityWithPID at rtkit's lowest nice level;
//   4. else nothing: the thread runs as before.
// A thread refused real time is asked for again a few times, 21 s apart (rtkit answers 25 requests per 20 s per user).
// The steps are a table so the tests can stand in for the system and rtkit. Every outcome is recorded
// (audio/realtime_priority.h).
#pragma once
#include <functional>
#include <string>

#include "audio/realtime_priority.h"
#include "audio/rtkit.h"

namespace realtime {

// the steps of the order above, each false (with `error`) when refused
struct PromotionSteps {
    std::function<bool(int& priority)> askDirectly;  // step 1; true with the priority got
    std::function<bool(RtkitLimits& limits, std::string& error)> readRtkitLimits;
    std::function<bool(int processId, int64_t maxMicroseconds, std::string& error)> limitRealtimeTime;
    std::function<bool(int processId, long threadId, int priority, std::string& error)> rtkitRealtime;
    std::function<bool(int processId, long threadId, int niceLevel, std::string& error)> rtkitHighPriority;
};

// runs the order for thread `threadId` (a Linux thread id) of process `processId`
Outcome promoteThread(int processId, long threadId, const PromotionSteps& steps);

// a plugin host's audio thread refused real time takes the place of a worker that has it: rtkit allows only so many
// real-time threads per user (25 by default), and a host left at normal priority loses its core to the real-time
// workers while the buffer waits for it. dropWorker(workerThreadId) puts the worker back to normal priority (false: it
// can't, and nothing more is asked), the host is asked again, and if it is refused still (rtkit's burst limit, not its
// thread limit) the worker asks for its place back. Returns the host's outcome; workerOutcome is the worker's if dropped
Outcome promoteHostOverWorker(int hostProcessId, long hostThreadId, int workerProcessId, long workerThreadId,
                              const PromotionSteps& hostSteps, const PromotionSteps& workerSteps,
                              const std::function<bool(long workerThreadId)>& dropWorker, Outcome& workerOutcome);

// the system's own steps (rtkit, RLIMIT_RTTIME), with this as step 1
PromotionSteps systemSteps(std::function<bool(int& priority)> askDirectly);

// the calling thread made an audio thread of this role, and recorded
Outcome raiseCallingThread(ThreadRole role);

// a plugin host's audio thread (its Linux process and thread ids) made real time, and recorded
Outcome raisePluginHostThread(int processId, long threadId);

// records a thread its driver set up (JACK's process thread) as it is
void recordCallingThreadAsItIs(ThreadRole role, const std::string& how);

// RLIMIT_RTTIME of process `processId` lowered to at most `maxMicroseconds` (the soft limit a quarter below it), as
// rtkit requires; for this process, SIGXCPU is caught first (realtime_overrun_posix.cpp)
bool limitRealtimeTime(int processId, int64_t maxMicroseconds, std::string& error);

// this process's thread `threadId` dropped back to normal priority if it ever runs past the soft limit
void demoteOnOverrun(long threadId);

}  // namespace realtime
