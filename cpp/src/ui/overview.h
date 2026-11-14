// The project overview (overview.py): a thin bar above the editor showing the whole project at once.
//
// Every lane is a thin row (as tall, relative to the others, as in the editor) and every clip a
// block in its own colour, across the project's whole length; a group's header row shows all its
// tracks' clips in miniature (also while it is folded). The part the editor shows is outlined and
// the playback cursor is a red line, the selection cursor a blue one. Click or drag on the bar to
// move the editor there; drag the outline to slide the view. Drag its bottom edge down to make it
// taller (more room for each lane), up to make it smaller again; double-click the edge for the
// standard height.
//
// The tracks and clips are drawn into an image only when they change: the main window calls
// refresh() every frame, which compares a cheap summary of the project, so playback and scrolling
// only redraw the outline and the playhead over that image.
#pragma once
#include <vector>

#include "audio/engine.h"
#include "model/model.h"
#include "ui/lanes.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

class Timeline;

namespace overview {
constexpr int PAD = 3;   // px above and below the rows
constexpr int EDGE = 5;  // px along the bottom that resize the bar
}  // namespace overview

class Overview : public ui::Widget {
public:
    Overview(Project& project, audio::Engine& engine, Timeline* timeline, ui::ScrollArea* scroll,
             int height = config::OVERVIEW_H);
    // Python's `resized` signal (renamed: Widget::resized() is the toolkit's resize hook): its new
    // height, after dragging its bottom edge
    ui::Signal<int> resizedTo;

    Project& project;
    audio::Engine& engine;
    Timeline* timeline;
    ui::ScrollArea* scroll;

    int setHeight(int height);           // clamped to OVERVIEW_H…OVERVIEW_MAX_H; returns it
    bool onEdge(double y) const;
    double span() const;                 // seconds shown across the bar
    std::vector<Lane> lanes() const;     // the editor's lanes without the master lane, from y = 0
    double rowScale() const;             // overview pixels per pixel of lane height
    RectF viewRect(double span) const;   // the editor's visible area, in overview pixels
    void refresh();                      // redraw only what changed (call every frame)
    void forgetImage();                  // after a skin change: the cached image used the old colours
    Image renderContent(double span) const;
    void moveView(PointF pos, double span);

    void paint(Painter& p) override;
    void resized() override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;

private:
    std::vector<double> contentKey(double span) const;  // everything the image depends on
    std::vector<double> key_, marks_;
    Image image_;
    bool hasImage_ = false;
    bool grabbing_ = false, sizing_ = false;
    double grabX_ = 0, grabY_ = 0;       // while dragging: where in the view outline it was grabbed
    double sizeY_ = 0;                   // while dragging the bottom edge: global mouse y and height at the start
    int sizeH_ = 0;
};
