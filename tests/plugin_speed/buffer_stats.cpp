#include "buffer_stats.h"

#include <algorithm>

#include "speed_settings.h"

namespace plugin_speed {

namespace {

// the value a share of the way up the sorted list (0.5: the median)
double sortedQuantile(std::vector<double> values, double share) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, (size_t)(share * (double)values.size()))];
}

}  // namespace

BufferStats summarizeRuns(const std::vector<std::vector<double>>& runs, int frames) {
    BufferStats stats;
    std::vector<double> runMedians, everyBuffer;
    const double deadlineSeconds = (double)frames / SAMPLE_RATE;
    for (const auto& run : runs) {
        runMedians.push_back(sortedQuantile(run, 0.5));
        everyBuffer.insert(everyBuffer.end(), run.begin(), run.end());
    }
    stats.medianSeconds = sortedQuantile(runMedians, 0.5);
    stats.percentile99Seconds = sortedQuantile(everyBuffer, 0.99);
    stats.overDeadline = (int)std::count_if(everyBuffer.begin(), everyBuffer.end(),
                                            [&](double seconds) { return seconds > deadlineSeconds; });
    stats.buffers = (int)everyBuffer.size();
    return stats;
}

}  // namespace plugin_speed
