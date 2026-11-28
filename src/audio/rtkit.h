// Linux: rtkit (RealtimeKit, org.freedesktop.RealtimeKit1 on the system D-Bus), the desktop's service that gives a
// user's threads real-time priority where the user's own limits (ulimit -r) don't allow it. Reached through
// libdbus-1.so.3, loaded when first needed (rtkit itself is built on it, so it is there wherever rtkit runs); without
// it, or without rtkit, every call fails with a reason. One connection, kept, for the whole program.
#pragma once
#include <cstdint>
#include <string>

namespace realtime {

// what rtkit allows: the highest real-time priority, the lowest nice level, and the longest a real-time thread may
// run without a pause (rtkit refuses a process whose hard RLIMIT_RTTIME is above it)
struct RtkitLimits {
    int maxRealtimePriority = 0;
    int minNiceLevel = 0;
    int64_t realtimeTimeMaxMicroseconds = 0;
};

// reads rtkit's limits; false, with `error`, when rtkit can't be reached
bool readRtkitLimits(RtkitLimits& limits, std::string& error);
// rtkit's MakeThreadRealtimeWithPID: thread `threadId` (a Linux thread id) of process `processId` to SCHED_RR
bool rtkitMakeRealtime(int processId, long threadId, int priority, std::string& error);
// rtkit's MakeThreadHighPriorityWithPID: the thread to this nice level
bool rtkitMakeHighPriority(int processId, long threadId, int niceLevel, std::string& error);

}  // namespace realtime
