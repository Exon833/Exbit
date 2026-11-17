#include "ui/master_lane.h"

#include "ui/timeline.h"
#include "ui/toolkit/style.h"

MasterLaneView::MasterLaneView(Timeline* tl) : timeline(tl) {
    setStyleType("MasterLane");
    setAcceptDrops(true);
}

int MasterLaneView::offset() const { return timeline->scroll->horizontalScrollBar()->value(); }

void MasterLaneView::paint(Painter& p) {
    // its box (MasterLane: background, the divider above it as border-top), then the timeline's grid and
    // cursors over it, and the divider again over the grid
    const css::Style& st = computedStyle();
    ui::style::box(p, rect(), st);
    int dx = offset();
    p.translate((float)-dx, 0);  // draw in timeline coordinates
    paint::Line divider{ui::style::color(st, "border-top-color"), ui::style::px(st, "border-top-width")};
    timeline->paintMaster(p, p.clip(), 0, height(), divider);
}

void MasterLaneView::updateLine(int x) {
    const TimelineLook& lk = timeline->look();
    int line = std::max(lk.cursor.width, lk.playhead.width) / 2 + 1;
    update(Rect(x - offset() - line, 0, 2 * line + 1, height()));
}

double MasterLaneView::timelineX(const Point& pos) const { return pos.x + offset(); }

bool MasterLaneView::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return true;
    Timeline* tl = timeline;
    tl->setCursor(tl->snappedFrameAt(timelineX(e.pos), e.mods));
    if (!e.ctrl()) tl->clearSelection();
    tl->setFocus();
    return true;
}

bool MasterLaneView::contextMenu(ui::MouseEvent& e) {
    timeline->showMenu(e.globalPos);  // (not over a track lane)
    return true;
}

bool MasterLaneView::wheel(ui::WheelEvent& e) {
    timeline->wheel(e.dy * 120.0, e.mods, timelineX(e.pos));
    return true;
}

void MasterLaneView::dragEnter(ui::DragEvent& e) {
    if (e.data && !e.data->files.empty()) e.accepted = true;
}

void MasterLaneView::drop(ui::DragEvent& e) {
    if (!e.data) return;
    std::vector<std::string> paths = e.data->files;
    timeline->dropPaths(paths, timeline->snappedFrameAt(timelineX(e.pos), e.mods), std::nullopt);
    e.accepted = true;
}
