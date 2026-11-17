#include "ui/paint_helpers.h"

#include <cmath>

#include "config.h"
#include "ui/colors.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"

namespace paint {

bool groupColor(const Group& group, Color& out) {
    if (group.color.empty()) return false;
    Color text;
    colors::trackColors(group.color, group.tone, out, text);
    return true;
}

Color groupFill(const Group& group) {
    Color bg;
    return groupColor(group, bg) ? bg : theme::c("group_lane");
}

int cursorArrowWidth() { return theme::metric("--cursor-arrow-width", config::CURSOR_ARROW_W); }
int cursorArrowHeight() { return theme::metric("--cursor-arrow-height", config::CURSOR_ARROW_H); }

void cursor(Painter& p, int x, Color color, int height, int width) {
    vline(p, x, 0, height + 1, Line{color, width});
    float half = cursorArrowWidth() / 2.0f;
    p.fillPolygon({PointF(x + 0.5f - half, 0), PointF(x + 0.5f + half, 0), PointF(x + 0.5f, (float)cursorArrowHeight())},
                  color);
}

// --- what the stylesheets give drawn things ---

std::vector<css::Style> nodeStyles(const ui::Widget* owner, const std::string& type,
                                   const std::vector<std::string>& classes, const std::vector<std::string>& states,
                                   const std::vector<std::string>& parts) {
    const css::Engine& engine = theme::styles();
    css::SimpleNode node(type, owner);
    node.classes = classes;
    node.states = states;
    css::Style self = engine.compute(node, owner ? &owner->computedStyle() : nullptr);
    std::vector<css::Style> out;
    out.reserve(parts.size());
    for (const std::string& part : parts) out.push_back(part.empty() ? self : engine.compute(node, &self, part));
    return out;
}

css::Style nodeStyle(const ui::Widget* owner, const std::string& type, const std::vector<std::string>& classes,
                     const std::vector<std::string>& states) {
    return nodeStyles(owner, type, classes, states, {""})[0];
}

bool Stamp::stale(const ui::Widget* owner) {
    int gen = theme::styles().generation();
    std::string now;
    if (owner) {
        std::vector<std::string> st;
        owner->styleStates(st);
        for (auto& s : st) now += s + " ";
    }
    if (gen == generation && now == states) return false;
    generation = gen;
    states = std::move(now);
    return true;
}

namespace {
bool mentionsCurrentColor(const std::string& v) {
    std::string low;
    for (char ch : v) low += (char)std::tolower((unsigned char)ch);
    return low.find("currentcolor") != std::string::npos;
}
}  // namespace

css::Style withBackground(const css::Style& s, Color c) {
    // Only what a box is drawn from: no variables, no text properties; its colours resolved to #hex now
    // (currentColor too), so drawing it parses no more than it must (it is drawn every frame).
    css::Style out;
    out.emPx_ = s.emPx_;
    bool current = false;
    for (auto& [key, value] : s.props_) {
        if (key == "color" || key.compare(0, 5, "font-") == 0 || key == "letter-spacing") continue;
        out.props_.emplace(key, value);
        current = current || mentionsCurrentColor(value);
    }
    for (const char* side : {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"})
        out.props_[side] = css::hex(ui::style::toRgba(ui::style::color(s, side, ui::style::foreground(s))));
    out.props_["background-color"] = css::hex(ui::style::toRgba(c));
    if (current && s.has("color")) out.props_["color"] = s.raw("color");  // (what else uses it)
    return out;
}

bool shows(const css::Style& s) {
    if (ui::style::background(s).a) return true;
    ui::style::Insets b = ui::style::insets(s, "border-width");
    if (b.w() || b.h()) return true;
    for (auto& [key, value] : s.props_)  // (what boxes may draw besides: images, shadows)
        if ((key.compare(0, 11, "background-") == 0 && key != "background-color") || key == "box-shadow") return true;
    return false;
}

const css::Style& Recolored::get(const css::Style& base, Color c) const {
    uint32_t key = (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.a;
    auto it = kept_.find(key);
    if (it == kept_.end()) {
        if (kept_.size() >= 256) kept_.clear();  // (ever more colours: it never grows without end)
        it = kept_.emplace(key, withBackground(base, c)).first;
    }
    return it->second;
}

Line line(const css::Style& s, Color def, int defWidth) {
    Line l;
    l.color = ui::style::color(s, "color", def);
    l.width = std::max(0, ui::style::px(s, "stroke-width", defWidth));
    return l;
}

void vline(Painter& p, int x, int y0, int y1, const Line& l) {
    if (l.width == 1) p.drawVLine(x, y0, y1, l.color);
    else if (l.width > 1 && y1 > y0) p.fillRect(Rect(x - (l.width - 1) / 2, y0, l.width, y1 - y0), l.color);
}

void hline(Painter& p, int x0, int x1, int y, const Line& l) {
    if (l.width == 1) p.drawHLine(x0, x1, y, l.color);
    else if (l.width > 1 && x1 > x0) p.fillRect(Rect(x0, y - (l.width - 1) / 2, x1 - x0, l.width), l.color);
}

float length(const css::Style& s, const char* property, float def, float percentOf) {
    return (float)s.length(property, def, percentOf);
}

int lengthPx(const css::Style& s, const char* property, int def, int percentOf) {
    double v = s.length(property, std::nan(""), percentOf);
    if (std::isnan(v)) return def;
    std::string raw = s.raw(property);
    bool percent = raw.find('%') != std::string::npos;
    return percent ? (int)std::floor(v + 1e-9) : (int)std::lround(v);
}

}  // namespace paint
