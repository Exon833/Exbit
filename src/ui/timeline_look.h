// What the stylesheets give the timeline's drawing (docs/skins.md, default.css's Timeline section), read once
// per stylesheet change (and change of the widget's own states), never while painting.
//
// NODES. The Timeline widget (its parts: ::grid-fine ::grid ::grid-bar ::cursor ::playhead ::selection
// ::selection-edge ::recording ::slice), and under it:
//   Ruler                 the ruler: box (background, borders), color (its labels and ticks); :looping while the
//                         loop is on. Parts ::label (padding-left, baseline, font) ::tick (seconds) ::tick-fine
//                         ::tick-beat ::tick-bar (height, color, stroke-width) ::loop (box, height) ::loop-edge
//                         (width, background-color; unset: the band's colour, opaque) ::marker (box, color, font,
//                         padding, height) ::marker-line (box, width) ::tempo (color, font, margin-left, baseline)
//   Lane                  a lane: .track (:alternate every other), .group, .automation (:armed, :locked), .take,
//                         .below (under the last one, the master lane's place). Box; padding (a group's
//                         miniatures); ::overlay (a box over it: an armed lane's red). An automation lane's
//                         color (its curve and points) and parts ::curve ::level (stroke-width) ::point (width,
//                         height) ::range (box) ::range-edge ::guide (color, stroke-width) ::label (color, font,
//                         padding, width, height) ::value (box, color, font, padding, margin-left) ::hint (color,
//                         font, padding-left, baseline)
//   Clip                  clips: clip_painter.h
// Lengths for lines are `stroke-width`; `baseline` is the y of a text's baseline in its box (% of the box).
#pragma once
#include <map>
#include <string>

#include "core/css.h"
#include "gfx/font.h"
#include "ui/paint_helpers.h"

namespace ui { class Widget; }

// one lane's look (Lane, its classes and states, and its parts)
struct LaneLook {
    css::Style box;               // Lane: its background and borders (style::box)
    paint::Recolored recolored;   // …in a group's own colour
    css::Style overlay;           // Lane::overlay: a box over the whole lane (an armed automation lane's red)
    bool overlayShown = false;    // (whether it draws anything: most lanes have none)
    paint::Line topEdge;          // its border-top, drawn again over the grid (automation and take lanes)
    int padTop = 0, padBottom = 0;  // padding (a group's miniatures inside it)
    // an automation lane
    paint::Line curve, level;     // ::curve, ::level (the line at the value, without points); `color`: the lane's
    float curveWidth = 1.5f;      // (the curve's stroke-width, antialiased)
    Color pointColor;
    int pointW = 4, pointH = 4;   // ::point
    css::Style range;             // ::range: the band between 1 and 0 (a box)
    paint::Line rangeEdge, guide; // ::range-edge (its top and bottom), ::guide (dotted: halfway, or each step)
    Color labelColor;             // ::label: the values at both ends
    Font labelFont;
    int labelPadTop = 2, labelPadLeft = 6, labelPadBottom = 1, labelW = 220, labelH = 14;
    css::Style valueBox;          // ::value: a stepped parameter's values on their lines
    Color valueColor;
    Font valueFont;
    int valuePadLeft = 2, valuePadRight = 4, valueMarginLeft = 4;
    Color hintColor;              // ::hint: "Click to add points…"
    Font hintFont;
    int hintPadLeft = 8;
    css::Style hint;              // (its baseline: % of the lane's height)
};

enum class LaneKind { Track, Group, Automation, Take, Below };

// lanes' looks under one widget (the timeline's, the overview's), kept until the stylesheets change
class LaneLooks {
public:
    explicit LaneLooks(const ui::Widget* owner) : owner_(owner) {}
    void clear() { looks_.clear(); }
    const LaneLook& get(LaneKind kind, bool alternate = false, bool armed = false, bool locked = false);

private:
    const ui::Widget* owner_;
    std::map<int, LaneLook> looks_;
};

struct TimelineLook {
    // the Timeline's parts
    paint::Line grid[3];          // ::grid-fine, ::grid (beats, seconds), ::grid-bar
    paint::Line cursor, playhead; // the lines (their arrows: paint::cursor)
    css::Style selection;         // a box per lane of the selected range
    paint::Line selectionEdge;
    css::Style recording;         // the take growing while recording (a box)
    paint::Line slice;            // where slices start (the slice window): a line, and a mark at its top
    int sliceW = 5, sliceH = 3;
    // the ruler
    int rulerHeight = 24;         // (--ruler-height)
    css::Style ruler;
    Color rulerText;
    Font labelFont;
    int labelPadLeft = 4, labelBaseline = 15;
    struct Tick {
        paint::Line line;
        int height = 6;
    } tick, ticks[3];             // ::tick (seconds); ::tick-fine, ::tick-beat, ::tick-bar
    css::Style loop[2];           // ::loop: looping off, on (Ruler:looping)
    int loopHeight[2] = {12, 12};
    int loopEdgeW[2] = {2, 2};
    bool loopEdgeSet[2] = {false, false};
    Color loopEdge[2];
    css::Style marker;            // ::marker: a marker's flag
    Color markerText;
    Font markerFont;
    int markerPadLeft = 5, markerPadRight = 2, markerHeight = 12;
    css::Style markerLine;        // ::marker-line: its place, across the ruler
    int markerLineW = 2;
    Color tempoColor;             // ::tempo: a tempo change after its bar's number
    Font tempoFont;
    int tempoMargin = 6, tempoBaseline = 15;
    LaneLooks* lanes = nullptr;   // (the timeline's)
};

// read what the stylesheets give the timeline's drawing (`owner`: the Timeline)
void readTimelineLook(TimelineLook& look, const ui::Widget* owner);
