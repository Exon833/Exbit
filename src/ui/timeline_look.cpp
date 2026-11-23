#include "ui/timeline_look.h"

#include <cmath>

#include "config.h"
#include "ui/lanes.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"

using ui::style::background;
using ui::style::color;
using ui::style::px;

namespace {
// a style as style::box reads it (its properties; the variables, which every node has, aren't copied)
css::Style boxOf(const css::Style& s) { return paint::withBackground(s, background(s)); }

int baselineOf(const css::Style& s, int def, int boxH) { return paint::lengthPx(s, "baseline", def, boxH); }
}  // namespace

const LaneLook& LaneLooks::get(LaneKind kind, bool alternate, bool armed, bool locked) {
    int k = (int)kind | (alternate ? 8 : 0) | (armed ? 16 : 0) | (locked ? 32 : 0);
    auto it = looks_.find(k);
    if (it != looks_.end()) return it->second;
    static const char* const kinds[] = {"track", "group", "automation", "take", "below"};
    std::vector<std::string> states;
    if (alternate) states.push_back("alternate");
    if (armed) states.push_back("armed");
    if (locked) states.push_back("locked");
    auto st = paint::nodeStyles(owner_, "Lane", {kinds[(int)kind]}, states,
                                {"", "overlay", "curve", "level", "point", "range", "range-edge", "guide", "label",
                                 "value", "hint"});
    const css::Style& lane = st[0];
    LaneLook l;
    l.box = boxOf(lane);
    l.overlay = boxOf(st[1]);
    l.overlayShown = paint::shows(l.overlay);
    l.topEdge = paint::Line{color(lane, "border-top-color"), px(lane, "border-top-width")};
    l.padTop = px(lane, "padding-top");
    l.padBottom = px(lane, "padding-bottom");
    Color line = theme::c("automation_line");
    l.curve = paint::line(st[2], line, 1);
    l.curveWidth = std::max(0.f, paint::length(st[2], "stroke-width", 1.5f));
    l.level = paint::line(st[3], line, 1);
    l.pointColor = color(st[4], "color", line);
    l.pointW = px(st[4], "width", 4);
    l.pointH = px(st[4], "height", 4);
    l.range = boxOf(st[5]);
    l.rangeEdge = paint::line(st[6], line.withAlpha(70), 1);
    l.guide = paint::line(st[7], line.withAlpha(40), 1);
    l.labelColor = color(st[8], "color", line.withAlpha(170));
    l.labelFont = ui::style::font(st[8], theme::uiFont());
    l.labelPadTop = px(st[8], "padding-top", 2);
    l.labelPadLeft = px(st[8], "padding-left", 6);
    l.labelPadBottom = px(st[8], "padding-bottom", 1);
    l.labelW = px(st[8], "width", 220);
    l.labelH = px(st[8], "height", 14);
    l.valueBox = boxOf(st[9]);
    l.valueColor = color(st[9], "color", l.labelColor);
    l.valueFont = ui::style::font(st[9], l.labelFont);
    l.valuePadLeft = px(st[9], "padding-left", 2);
    l.valuePadRight = px(st[9], "padding-right", 4);
    l.valueMarginLeft = px(st[9], "margin-left", 4);
    l.hintColor = color(st[10], "color", theme::c("ruler_text"));
    l.hintFont = ui::style::font(st[10], theme::uiFont());
    l.hintPadLeft = px(st[10], "padding-left", 8);
    l.hint = st[10];
    return looks_.emplace(k, std::move(l)).first->second;
}

void readTimelineLook(TimelineLook& look, const ui::Widget* owner) {
    TimelineLook l;
    l.lanes = look.lanes;
    // the Timeline's parts
    {
        const css::Engine& engine = theme::styles();
        const css::Style& self = owner->computedStyle();
        auto part = [&](const char* name) { return engine.compute(*owner, &self, name); };
        static const char* const grids[] = {"grid-fine", "grid", "grid-bar"};
        static const char* const gridColors[] = {"grid_fine", "grid", "grid_bar"};
        for (int i = 0; i < 3; i++) l.grid[i] = paint::line(part(grids[i]), theme::c(gridColors[i]));
        l.cursor = paint::line(part("cursor"), theme::c("cursor"));
        l.playhead = paint::line(part("playhead"), theme::c("playhead"));
        l.selection = boxOf(part("selection"));
        l.selectionEdge = paint::line(part("selection-edge"), theme::c("selection_edge"));
        l.recording = boxOf(part("recording"));
        css::Style slice = part("slice");
        l.slice = paint::line(slice, theme::c("selected_note"));
        l.sliceW = px(slice, "width", 5);
        l.sliceH = px(slice, "height", 3);
    }
    // the ruler
    const int rulerH = l.rulerHeight = lanes::rulerHeight();
    auto ruler = paint::nodeStyles(owner, "Ruler", {}, {},
                                   {"", "label", "tick", "tick-fine", "tick-beat", "tick-bar", "loop", "loop-edge",
                                    "marker", "marker-line", "tempo"});
    auto looping = paint::nodeStyles(owner, "Ruler", {}, {"looping"}, {"loop", "loop-edge"});
    l.ruler = boxOf(ruler[0]);
    l.rulerText = color(ruler[1], "color", color(ruler[0], "color", theme::c("ruler_text")));  // (::label)
    l.labelFont = ui::style::font(ruler[1], theme::uiFont());
    l.labelPadLeft = px(ruler[1], "padding-left", 4);
    l.labelBaseline = baselineOf(ruler[1], 15, rulerH);
    static const int tickDefaults[] = {6, 3, 6, 10};
    for (int i = 0; i < 4; i++) {
        TimelineLook::Tick& t = i == 0 ? l.tick : l.ticks[i - 1];
        t.line = paint::line(ruler[2 + i], l.rulerText);
        t.height = paint::lengthPx(ruler[2 + i], "height", tickDefaults[i], rulerH);
    }
    for (int on = 0; on < 2; on++) {
        const css::Style& band = on ? looping[0] : ruler[6];
        const css::Style& edge = on ? looping[1] : ruler[7];
        l.loop[on] = boxOf(band);
        l.loopHeight[on] = paint::lengthPx(band, "height", rulerH - rulerH / 2, rulerH);
        l.loopEdgeW[on] = std::max(0, px(edge, "width", 2));
        l.loopEdgeSet[on] = edge.has("background-color");
        l.loopEdge[on] = l.loopEdgeSet[on] ? background(edge) : background(band).withAlpha(255);
    }
    l.marker = boxOf(ruler[8]);
    l.markerText = color(ruler[8], "color", theme::c("accent_text"));
    l.markerFont = ui::style::font(ruler[8], theme::uiFont());
    l.markerPadLeft = px(ruler[8], "padding-left", 5);
    l.markerPadRight = px(ruler[8], "padding-right", 2);
    l.markerHeight = paint::lengthPx(ruler[8], "height", rulerH - rulerH / 2, rulerH);
    l.markerLine = boxOf(ruler[9]);
    l.markerLineW = std::max(0, px(ruler[9], "width", 2));
    l.tempoColor = color(ruler[10], "color", theme::c("accent"));
    l.tempoFont = ui::style::font(ruler[10], theme::uiFont());
    l.tempoMargin = px(ruler[10], "margin-left", 6);
    l.tempoBaseline = baselineOf(ruler[10], 15, rulerH);
    for (int dragged = 0; dragged < 2; dragged++) {  // a time signature's block
        css::Style sig = paint::nodeStyle(owner, "TimeSignature", {}, dragged ? std::vector<std::string>{"dragged"}
                                                                              : std::vector<std::string>{});
        l.signature[dragged] = boxOf(sig);
        l.signatureText[dragged] = color(sig, "color", theme::c("window_text"));
        if (!dragged) {
            l.signatureFont = ui::style::font(sig, theme::uiFont());
            l.signaturePadLeft = px(sig, "padding-left", 5);
            l.signatureMarginTop = px(sig, "margin-top", 3);
            l.signatureMarginBottom = px(sig, "margin-bottom", 3);
        }
    }
    if (l.lanes) l.lanes->clear();
    look = std::move(l);
}
