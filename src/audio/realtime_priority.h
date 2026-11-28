// Real-time priority of the audio threads: what each one got, kept so Options → Audio properties can say it
// (realtime_priority.cpp). How a thread gets it: on Linux realtime_promotion.h (SCHED_FIFO, else rtkit), on Windows
// MMCSS (audio/devices_win.cpp).
#pragma once
#include <string>

namespace realtime {

// which kind of audio thread an outcome is about
enum class ThreadRole { DeviceThread, Worker, PluginHost };

// what one thread asked for and got
struct Outcome {
    bool realtime = false;    // a real-time policy (SCHED_FIFO / SCHED_RR), or MMCSS on Windows
    bool highNice = false;    // not real time, but a raised nice level
    std::string how;          // "direct", "rtkit", "MMCSS Pro Audio", "the driver's"
    int level = 0;            // the real-time priority, or with highNice the nice level (0: none to say)
    std::string whyNot;       // why real time wasn't granted ("" when it was)
};

// an outcome as the dialog says it: "granted (rtkit, 20)", "high priority only (rtkit, nice -15): …", "not granted: …"
std::string describeOutcome(const Outcome& outcome);

// keeps the outcome of thread `threadId` of process `processId` (Linux ids; Windows: the thread id, process 0), and
// logs it (debug output)
void recordOutcome(ThreadRole role, int processId, long threadId, const Outcome& outcome);

// the outcomes of the audio threads still running, one line per role: "Audio thread: granted (rtkit, 20)",
// "Workers: 15 of 15 granted (rtkit, 20)", "Plugin hosts: 2 of 2 granted (rtkit, 20)"
std::string statusText();

// (tests) forgets every outcome
void forgetOutcomes();

}  // namespace realtime
