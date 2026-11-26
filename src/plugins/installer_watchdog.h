// Is an installer stuck? While Exbit waits for an install (Options → Install Windows plugin…), it looks
// every so often at the prefix's Windows programs: how much processor time they have used, and how many
// windows they show. An installer that shows no window and whose prefix has done (next to) nothing for
// several minutes is taken as hung (Quanta 2's installer under Wine sat so, its window never made);
// Exbit then says so and offers to end the prefix's programs (wineserver -k, that prefix only).
// The judging is here, on samples (tests give it made-up ones); the looking is installer_activity.h.
#pragma once
#include <vector>

namespace watchdog {

// one look at the prefix
struct Sample {
    double seconds = 0;         // when (any clock that goes forward, in seconds)
    double processorSeconds = 0;  // processor time used so far by all the prefix's programs together
    int visibleWindows = 0;     // windows the prefix's programs show (-1: couldn't be told)
};

constexpr double HUNG_AFTER_SECONDS = 180;  // how long without a window and without work
constexpr double IDLE_SHARE = 0.03;         // "without work": under 3% of one processor on average

// hung: the samples of the last HUNG_AFTER_SECONDS (at least that long a span of them) all show no
// window, and the processor time used over that span is under IDLE_SHARE of it. A sample that couldn't
// tell the windows (-1) isn't taken as "no window"
bool looksHung(const std::vector<Sample>& samples, double hungAfter = HUNG_AFTER_SECONDS, double idleShare = IDLE_SHARE);

// keeps only the samples the judging needs (those of the last `keepSeconds`, and the one before them)
void trim(std::vector<Sample>& samples, double keepSeconds = HUNG_AFTER_SECONDS * 2);

}  // namespace watchdog
