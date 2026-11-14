#include "ui/paint_helpers.h"

#include "config.h"
#include "ui/colors.h"
#include "ui/theme.h"

namespace paint {

Color groupFill(const Group& group) {
    if (group.color.empty()) return theme::c("group_lane");
    Color bg, text;
    colors::trackColors(group.color, group.tone, bg, text);
    return bg;
}

void cursor(Painter& p, int x, Color color, int height) {
    p.drawVLine(x, 0, height + 1, color);
    float half = config::CURSOR_ARROW_W / 2.0f;
    p.fillPolygon({PointF(x + 0.5f - half, 0), PointF(x + 0.5f + half, 0), PointF(x + 0.5f, (float)config::CURSOR_ARROW_H)},
                  color);
}

}  // namespace paint
