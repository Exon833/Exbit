// Small drawing helpers several screens share (timeline.py's group_fill and paint_cursor).
#pragma once
#include "gfx/painter.h"
#include "model/model.h"

namespace paint {
// background of a group's header lane and panel row
Color groupFill(const Group& group);
// a cursor: a line down from y=0 to `height` at x, with a small arrow on the ruler pointing down at it
void cursor(Painter& p, int x, Color color, int height);
}  // namespace paint
