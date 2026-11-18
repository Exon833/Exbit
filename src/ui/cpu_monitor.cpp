// Audio CPU monitor in the top-right corner (cpu_monitor.py).
#include "ui/cpu_monitor.h"

#include <algorithm>
#include <cstdio>

#include "config.h"
#include "ui/theme.h"

namespace cpu_monitor {
const char* loadState(double load) { return load >= 0.8 ? "hot" : load >= 0.5 ? "warn" : "ok"; }

Color loadColor(double load) {
    static int generation = -1;
    static Color colors[3];
    const css::Engine& engine = theme::styles();
    const char* states[] = {"ok", "warn", "hot"};
    if (engine.generation() != generation) {  // (computed again only when the stylesheets change)
        css::SimpleNode monitor("CpuMonitor");
        for (int i = 0; i < 3; ++i) {
            monitor.states = {states[i]};
            colors[i] = ui::style::background(engine.compute(monitor, nullptr, "bar"));
        }
        generation = engine.generation();
    }
    std::string state = loadState(load);
    return colors[state == "hot" ? 2 : state == "warn" ? 1 : 0];
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
    setStyleType("CpuMonitor");
    fit();
    showLoad(std::nullopt);
}

void CpuMonitor::fit() {
    Point size(theme::metric("panel-width", config::PANEL_W), theme::metric("ruler-height", config::RULER_H));
    if (minimumSize() != size || maximumSize() != size) setFixedSize(size.x, size.y);
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

const CpuMonitor::Look& CpuMonitor::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    l.self = &parts_.get("");
    l.text = ui::style::foreground(*l.self);
    l.font = ui::style::font(*l.self, theme::uiFont());
    l.padding = ui::style::insets(*l.self, "padding");
    l.border = ui::style::insets(*l.self, "border-width");
    const css::Style& late = parts_.get("late");
    l.late = ui::style::foreground(late);
    l.lateFont = ui::style::font(late, l.font);
    l.lateW = panel_style::px(late, "min-width", 50);
    const css::Style& track = parts_.get("track");
    l.track = &track;
    l.trackInset = ui::style::contentInsets(track);
    l.barH = std::max(0, panel_style::px(track, "height", 3));
    const char* states[] = {"ok", "warn", "hot"};
    for (int i = 0; i < 3; ++i) {
        l.barStyle[i] = &parts_.get("bar", states[i]);
        l.bar[i] = panel_style::bg(*l.barStyle[i]);
    }
    look_ = l;
    return look_;
}

void CpuMonitor::paint(Painter& p) {
    const Look& look = this->look();
    ui::style::box(p, rect(), *look.self);  // (its left border: the line between it and the ruler)
    RectF textRect(ui::style::inside(rect(), look.padding));
    const Font& f = look.font;
    Color text = look.text;
    if (!load_) {
        p.drawText(textRect, AlignVCenter | AlignLeft, "Audio CPU  –", f, text);
        return;
    }
    double current = load_->first, average = load_->second;
    p.drawText(textRect, AlignVCenter | AlignLeft, pct("Audio CPU ", current), f, text);
    if (late_) {
        p.drawText(textRect.adjusted(0, 0, -(float)look.lateW, 0), AlignVCenter | AlignRight, pct("avg ", average), f, text);
        p.drawText(textRect, AlignVCenter | AlignRight, "⚠ " + std::to_string(late_), look.lateFont, look.late);
    } else {
        p.drawText(textRect, AlignVCenter | AlignRight, pct("avg ", average), f, text);
    }
    // thin bar along the bottom (inside the borders): current load, full width = 100 %
    Rect bar = ui::style::inside(rect(), look.border);
    bar = Rect(bar.x, bar.bottom() - look.barH, bar.w, look.barH);
    const char* state = cpu_monitor::loadState(current);
    int range = state[0] == 'h' ? 2 : state[0] == 'w' ? 1 : 0;
    ui::style::box(p, bar, *look.track);
    Rect in = ui::style::inside(bar, look.trackInset);  // (the bar inside the track's border and padding)
    int barW = (int)(std::max(0, in.w) * std::min(1.0, current));
    if (barW > 0 && in.h > 0)
        panel_style::fillPart(p, RectF(Rect(in.x, in.y, barW, in.h)), look.barStyle[range], look.bar[range]);
}
