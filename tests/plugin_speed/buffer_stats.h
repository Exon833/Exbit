// What the plugin speed benchmark reports of a side's buffer times (tests/test_plugin_speed.cpp)
#pragma once
#include <vector>

namespace plugin_speed {

struct BufferStats {
    double medianSeconds = 0;       // the median of the runs' medians (a run's noise moves it least)
    double percentile99Seconds = 0;  // over every buffer of every run
    int overDeadline = 0;            // buffers that took longer than their own length (frames / rate)
    int buffers = 0;
};

// the stats of several runs' per-buffer times (seconds) at this buffer size
BufferStats summarizeRuns(const std::vector<std::vector<double>>& runs, int frames);

}  // namespace plugin_speed
