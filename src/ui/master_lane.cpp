#include "ui/master_lane.h"

#include "ui/timeline.h"

MasterLaneView::MasterLaneView(Timeline* tl) : timeline(tl) { setAcceptDrops(true); }

int MasterLaneView::offset() const { return timeline->scroll->horizontalScrollBar()->value(); }

void MasterLaneView::paint(Painter& p) {
    int dx = offset();
    p.translate((float)-dx, 0);  // draw in timeline coordinates
    timeline->paintMaster(p, p.clip(), 0, height());
}

void MasterLaneView::updateLine(int x) { update(Rect(x - offset() - 1, 0, 3, height())); }

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
