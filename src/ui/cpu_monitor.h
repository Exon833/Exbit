// Audio CPU monitor in the top-right corner (cpu_monitor.py): current and average callback load,
// and a red warning with the number of late audio buffers (likely clicks) since Play was pressed.
// As tall as the timeline's ruler, so the panel's rows line up with the lanes.
#pragma once
#include <optional>
#include <utility>

#include "ui/toolkit/widget.h"

namespace cpu_monitor {
Color loadColor(double load);  // meter_hot from 80 %, meter_warn from 50 %, else meter_ok
}

class CpuMonitor : public ui::Widget {
public:
    CpuMonitor();
    // (current, average) as fractions, or nullopt when no audio is running; late buffers since Play
    void showLoad(std::optional<std::pair<double, double>> load, int late = 0);
    std::optional<std::pair<double, double>> load() const { return load_; }
    int late() const { return late_; }
    void paint(Painter& p) override;

private:
    std::optional<std::pair<double, double>> load_;
    int late_ = 0;
};
