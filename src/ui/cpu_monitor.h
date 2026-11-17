// Audio CPU monitor in the top-right corner (cpu_monitor.py): current and average callback load,
// and a red warning with the number of late audio buffers (likely clicks) since Play was pressed.
// As tall as the timeline's ruler, so the panel's rows line up with the lanes.
//
// Styled as CpuMonitor (default.css): its background-color, color, font, the left border (the line between it
// and the ruler: border-width and border-color), the padding its text keeps; ::late, the late buffers' warning
// (color; min-width: the room it keeps right of the average); ::track, the bar along its bottom (its height and
// background-color), and ::bar:ok / :warn / :hot, the load on it.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include "ui/panel_style.h"
#include "ui/toolkit/widget.h"

namespace cpu_monitor {
const char* loadState(double load);  // "hot" from 80 %, "warn" from 50 %, else "ok"
// the colour of a load bar (CpuMonitor::bar in loadState's state, as the stylesheets give it)
Color loadColor(double load);
}

class CpuMonitor : public ui::Widget {
public:
    CpuMonitor();
    // (current, average) as fractions, or nullopt when no audio is running; late buffers since Play
    void showLoad(std::optional<std::pair<double, double>> load, int late = 0);
    // the costliest tracks now, for its tooltip: (name, share of a core), costliest first
    void showTracks(std::vector<std::pair<std::string, double>> tracks);
    std::optional<std::pair<double, double>> load() const { return load_; }
    int late() const { return late_; }
    void paint(Painter& p) override;

private:
    std::optional<std::pair<double, double>> load_;
    int late_ = 0;
    std::vector<std::pair<std::string, double>> tracks_;
    void tip();
    // what it is drawn with (kept: it repaints several times a second)
    struct Look {
        const css::Style* self = nullptr;  // (its own style: background and border)
        const css::Style* track = nullptr;  // (the bar's background: a box)
        Color text, late, bar[3];
        ui::style::Insets padding, border;
        int lateW = 50, barH = 3;
        Font font, lateFont;
    };
    const Look& look();
    panel_style::Parts parts_{*this};
    Look look_;
};
