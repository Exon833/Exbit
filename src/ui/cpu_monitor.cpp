// Audio CPU monitor in the top-right corner (cpu_monitor.py).
#include "ui/cpu_monitor.h"

#include <algorithm>
#include <cstdio>

#include "config.h"
#include "ui/theme.h"

namespace cpu_monitor {
Color loadColor(double load) {
    return theme::c(load >= 0.8 ? "meter_hot" : load >= 0.5 ? "meter_warn" : "meter_ok");
}
}  // namespace cpu_monitor

namespace {
std::string pct(const char* prefix, double fraction) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%4.1f %%", prefix, fraction * 100);
    return buf;
}
std::string g(double v) {  // Python's :g
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
}  // namespace

CpuMonitor::CpuMonitor() {
    setFixedSize(config::PANEL_W, config::RULER_H);
    showLoad(std::nullopt);
}

void CpuMonitor::showLoad(std::optional<std::pair<double, double>> load, int late) {
    load_ = load;
    late_ = late;
    tip();
    update();
}

void CpuMonitor::showTracks(std::vector<std::pair<std::string, double>> tracks) {
    if (tracks == tracks_) return;
    tracks_ = std::move(tracks);
    tip();
}

void CpuMonitor::tip() {
    std::string t = "Audio CPU: share of each buffer's time spent computing audio.\n"
                    "Current = last " + std::to_string(config::CPU_REFRESH_MS) + " ms, avg = rolling ~" +
                    g(config::CPU_AVERAGE_SECONDS) + " s.\n"
                    "Near 100 % you will hear dropouts.\n"
                    "⚠ = late audio buffers since Play (likely clicks): " + std::to_string(late_);
    if (!tracks_.empty()) {
        t += "\n\nThe costliest tracks (share of one core; tracks run on several cores at once):";
        for (auto& tr : tracks_) t += "\n" + pct("  ", tr.second) + "  " + tr.first;
        t += "\n(Options → CPU per track shows it on every row)";
    }
    setToolTip(t);
}

void CpuMonitor::paint(Painter& p) {
    p.fillRect(rect(), theme::c("ruler_bg"));
    p.fillRect(Rect(0, 0, 1, height()), theme::c("meter_bg"));  // separator from the ruler
    RectF textRect(rect().adjusted(8, 0, -8, -3));
    Font f = theme::uiFont();
    Color text = theme::c("ruler_text");
    if (!load_) {
        p.drawText(textRect, AlignVCenter | AlignLeft, "Audio CPU  –", f, text);
        return;
    }
    double current = load_->first, average = load_->second;
    p.drawText(textRect, AlignVCenter | AlignLeft, pct("Audio CPU ", current), f, text);
    if (late_) {
        p.drawText(textRect.adjusted(0, 0, -50, 0), AlignVCenter | AlignRight, pct("avg ", average), f, text);
        p.drawText(textRect, AlignVCenter | AlignRight, "⚠ " + std::to_string(late_), f, theme::c("meter_hot"));
    } else {
        p.drawText(textRect, AlignVCenter | AlignRight, pct("avg ", average), f, text);
    }
    // thin bar along the bottom: current load, full width = 100 %
    int barW = (int)((width() - 1) * std::min(1.0, current));
    p.fillRect(Rect(1, height() - 3, width() - 1, 3), theme::c("meter_bg"));
    p.fillRect(Rect(1, height() - 3, barW, 3), cpu_monitor::loadColor(current));
}
