// The master lane (master_lane.py): always at the bottom of the editor's view, drawn over the
// tracks, which scroll underneath it (the main window pins it there, and the master's row in the
// panel next to it).
//
// It shows the grid and both cursors, scrolling sideways with the timeline. Clicking it moves the
// selection cursor; files dropped on it go onto new tracks, as on the master lane before.
#pragma once
#include "ui/toolkit/widget.h"

class Timeline;

class MasterLaneView : public ui::Widget {
public:
    explicit MasterLaneView(Timeline* timeline);
    Timeline* timeline;
    int offset() const;                      // timeline x at this widget's left edge
    void updateLine(int x);                  // repaint the strip where a cursor line at timeline x is (or was)
    double timelineX(const Point& pos) const;
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override { return mousePress(e); }
    bool contextMenu(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override;
    void dragEnter(ui::DragEvent& e) override;
    void drop(ui::DragEvent& e) override;
};
