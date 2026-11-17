// Small drawing helpers several screens share (timeline.py's group_fill and paint_cursor), and what the
// stylesheets give the things drawn that aren't widgets (lanes, clips, the ruler: css::SimpleNode under the
// widget that draws them).
//
// PERFORMANCE. Computing a style takes microseconds; the timeline repaints at frame rate with many clips and
// lines. So nothing computes a style while painting: each painter keeps what it read (its "look", one per
// distinct node / part / state) and reads it again only when a Stamp says the stylesheets (theme::styles()'s
// generation) or the owner widget's own states changed.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/css.h"
#include "gfx/painter.h"
#include "model/model.h"

namespace ui { class Widget; }

namespace paint {
// the group's own colour (its header lane and row), if it has one
bool groupColor(const Group& group, Color& out);
// background of a group's header lane and panel row: its own colour, else the skin's --group-lane
Color groupFill(const Group& group);
// a cursor: a line down from y=0 to `height` at x, with a small arrow on the ruler pointing down at it
// (--cursor-arrow-width x --cursor-arrow-height); `width`: the line's (centred on x)
void cursor(Painter& p, int x, Color color, int height, int width = 1);
int cursorArrowWidth();   // --cursor-arrow-width (config::CURSOR_ARROW_W)
int cursorArrowHeight();  // --cursor-arrow-height

// --- what the stylesheets give drawn things ---
// The style of a node of `type` (css::SimpleNode) with these classes and states, under `owner` (null: under the
// root), and of its parts: parts[i] -> out[i] (each inheriting from the node, as a widget's parts do). Computed
// on every call: callers keep what they read.
css::Style nodeStyle(const ui::Widget* owner, const std::string& type, const std::vector<std::string>& classes,
                     const std::vector<std::string>& states);
std::vector<css::Style> nodeStyles(const ui::Widget* owner, const std::string& type,
                                   const std::vector<std::string>& classes, const std::vector<std::string>& states,
                                   const std::vector<std::string>& parts);
// Whether what was read from the stylesheets for drawing in `owner` is out of date: the stylesheets changed, or
// the owner's own states (hover, focus…). Updates itself (stale() is true once per change).
struct Stamp {
    int generation = -1;
    std::string states;
    bool stale(const ui::Widget* owner);
};
// a copy of `s` (what style::box reads of it) with this background colour: a box in a colour of the data's own
// (a group's colour), drawn through style::box all the same
css::Style withBackground(const css::Style& s, Color c);
// whether style::box would draw anything of it (a background that shows, a border, an image or shadow)
bool shows(const css::Style& s);
// withBackground of one style in the colours asked for, kept (there are few: the groups' or clips' colours,
// drawn every frame)
class Recolored {
public:
    const css::Style& get(const css::Style& base, Color c) const;

private:
    mutable std::map<uint32_t, css::Style> kept_;
};

// A line as the stylesheets give it: `color` and `stroke-width` (whole pixels, at least 0)
struct Line {
    Color color;
    int width = 1;
};
Line line(const css::Style& s, Color def = Color(0, 0, 0, 0), int defWidth = 1);
// a vertical / horizontal line of that width centred on x / y (1 px: exactly drawVLine / drawHLine), from y0 to
// y1 (x0 to x1) exclusive
void vline(Painter& p, int x, int y0, int y1, const Line& l);
void hline(Painter& p, int x0, int x1, int y, const Line& l);
// a length property in px (not rounded); `def` when unset; % of `percentOf`
float length(const css::Style& s, const char* property, float def, float percentOf = 0);
// a length in whole px, % rounded down (as integer division did: 50% of 25 is 12)
int lengthPx(const css::Style& s, const char* property, int def, int percentOf = 0);

}  // namespace paint
