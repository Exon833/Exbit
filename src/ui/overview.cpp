#include "model/routing.h"
#include "ui/overview.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/clip_painter.h"
#include "ui/colors.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/style.h"

using clip_painter::pixels;
using overview::EDGE;
using ui::style::box;

int overview::standardHeight() { return theme::metric("--overview-height", config::OVERVIEW_H); }
int overview::maxHeight() { return theme::metric("--overview-max-height", config::OVERVIEW_MAX_H); }

Overview::Overview(Project& project_, audio::Engine& engine_, Timeline* timeline_, ui::ScrollArea* scroll_, int height)
    : project(project_), engine(engine_), timeline(timeline_), scroll(scroll_) {
    setStyleType("Overview");
    setHeight(height);
    hPolicy = ui::SizePolicy::Expanding;
    setMouseTracking(true);  // (the cursor shows where the bottom edge can be dragged)
    setCursor(ui::Cursor::Hand);
    setToolTip("Project overview: click or drag to move the view. Drag its bottom edge to make it "
               "taller or smaller; double-click the edge for the standard height");
    // (overview.py meant to refresh when the editor's scroll bars move too, but that code is never
    // reached: the main window's refresh() every frame does it)
}

int Overview::setHeight(int height) {
    height = std::max(overview::standardHeight(), std::min(std::max(overview::standardHeight(), overview::maxHeight()), height));
    setFixedHeight(height);
    return height;
}

int Overview::padTop() const { return ui::style::px(computedStyle(), "padding-top", overview::PAD); }
int Overview::padBottom() const { return ui::style::px(computedStyle(), "padding-bottom", overview::PAD); }

bool Overview::onEdge(double y) const { return y >= this->height() - EDGE; }

// --- what to show -------------------------------------------------------------------------------

double Overview::span() const {
    // seconds shown across the bar: the whole project, the playhead, and the editor's view
    double sr = engine.sr;
    double viewEnd = (scroll->horizontalScrollBar()->value() + scroll->viewport()->width()) / timeline->pps;
    return std::max({project.endFrame() / sr, engine.pos.load() / sr, timeline->cursor / sr, viewEnd,
                     config::OVERVIEW_MIN_S});
}

std::vector<Lane> Overview::lanes() const {
    auto ls = lanes::layout(project);
    ls.pop_back();  // (the master lane)
    return ls;
}

double Overview::rowScale() const {
    int total = 0;
    for (auto& lane : lanes()) total += lane.height;
    return total ? (double)(height() - padTop() - padBottom()) / total : 0;
}

std::vector<double> Overview::contentKey(double span) const {
    // everything the tracks-and-clips image depends on
    std::vector<double> key{(double)width(), (double)height(), std::nearbyint(span * 1000) / 1000,
                            (double)theme::styles().generation()};
    auto ptr = [](const void* p) { return (double)(uintptr_t)p; };
    auto text = [](const std::string& s) { return (double)(std::hash<std::string>()(s) & 0xffffffffffffULL); };
    for (auto& lane : lanes()) {
        key.push_back(lane.height);
        key.push_back(lane.track ? ptr(lane.track.get()) : lane.group ? ptr(lane.group.get()) : lane.automation ? ptr(lane.automation.get()) : ptr(lane.take.get()));
        key.push_back(lane.group ? text(lane.group->color + "/" + lane.group->tone) : 0);
    }
    std::map<const Track*, int> rows;
    int i = 0;
    for (auto& t : project.tracks) {
        key.push_back(project.audible(*t) ? 1 : 0);
        rows[t.get()] = i++;
    }
    for (auto& clip : project.clips) {
        std::string hex, tone;
        colors::clipColor(*clip, hex, tone);
        auto r = rows.find(clip->track.get());
        key.insert(key.end(), {ptr(clip.get()), (double)clip->start, (double)clip->frames(),
                               (double)(r != rows.end() ? r->second : -1), text(hex), clip->disabled ? 1.0 : 0.0});
    }
    return key;
}

RectF Overview::viewRect(double span) const {
    // the editor's visible area, in overview pixels
    ui::ScrollBar* hbar = scroll->horizontalScrollBar();
    ui::ScrollBar* vbar = scroll->verticalScrollBar();
    ui::Widget* vp = scroll->viewport();
    double xScale = width() / span / timeline->pps;
    double scale = rowScale();
    int lanesH = 0;
    for (auto& lane : lanes()) lanesH += lane.height;
    int top = std::min(std::max(vbar->value() - lanes::lanesTop(), 0), lanesH);
    int bottom = std::min(std::max(vbar->value() + vp->height() - lanes::lanesTop(), 0), lanesH);
    const int pad = padTop();
    double y0 = pad, y1 = height() - padBottom();
    if (scale) {
        y0 = pad + top * scale;
        y1 = pad + bottom * scale;
    }
    return RectF((float)(hbar->value() * xScale), (float)y0, (float)(vp->width() * xScale), (float)std::max(y1 - y0, 2.0));
}

void Overview::refresh() {
    // redraw only what changed (called every frame and when the editor scrolls)
    double s = span();
    auto key = contentKey(s);
    if (!hasImage_ || key != key_) {
        key_ = key;
        image_ = renderContent(s);
        hasImage_ = true;
        marks_.clear();
    }
    auto xOf = [&](int64_t frame) { return (int)(frame / (double)engine.sr / s * width()); };
    RectF v = viewRect(s);
    std::vector<double> marks{v.x, v.y, v.w, v.h, (double)xOf(engine.heardPos()), (double)xOf(timeline->cursor)};
    if (marks != marks_) {
        marks_ = marks;
        update();
    }
}

void Overview::forgetImage() {
    hasImage_ = false;
    int was = height();
    if (setHeight(was) != was) resizedTo.emit(height());  // (the skin's limits changed)
    refresh();
}

// --- painting -----------------------------------------------------------------------------------

Image Overview::renderContent(double span) const {
    // the bar (Overview: a box), the rows (Lane boxes) and the clip blocks, as an image the size of the bar
    if (lookStamp_.stale(this)) laneLooks_.clear();
    Image image(std::max(1, width()), std::max(1, height()), Color(0, 0, 0, 0));
    Painter p(image);
    box(p, Rect(0, 0, image.width(), image.height()), computedStyle());
    double scale = rowScale(), xScale = width() / span / engine.sr;
    const int pad = padTop();
    // track -> (y, height) of its own row / of its row in its group's header
    std::map<const Track*, std::pair<double, double>> rows, inHeader;
    int row = 0;
    for (auto& lane : lanes()) {
        double y = pad + lane.top * scale, h = lane.height * scale;
        RectF area(0, (float)y, (float)width(), (float)h);
        if (lane.automation || lane.take) {
            box(p, pixels(area), laneLooks_.get(lane.automation ? LaneKind::Automation : LaneKind::Take).box);
        } else if (lane.track) {
            box(p, pixels(area), laneLooks_.get(LaneKind::Track, row % 2 != 0).box);
            rows[lane.track.get()] = {y, h};
            ++row;
        } else {  // a group's header: its tracks' clips, each track a thinner row
            const LaneLook& g = laneLooks_.get(LaneKind::Group);
            Color own;
            box(p, pixels(area), paint::groupColor(*lane.group, own) ? g.recolored.get(g.box, own) : g.box);
            auto members = routing::inside(project, lane.group.get());
            for (size_t i = 0; i < members.size(); ++i)
                inHeader[members[i].get()] = {y + i * h / members.size(), h / members.size()};
        }
    }
    for (auto& clip : project.clips) {
        std::string hex, tone;
        colors::clipColor(*clip, hex, tone);
        Color color = colors::clipColors(hex, project.audible(*clip->track)).fill;
        for (auto* places : {&rows, &inHeader}) {
            auto it = places->find(clip->track.get());
            if (it == places->end()) continue;
            double y = it->second.first, h = it->second.second;
            RectF block((float)(clip->start * xScale), (float)(y + std::min(1.0, h / 4)),
                        (float)std::max(1.0, clip->frames() * xScale), (float)std::max(1.0, h - std::min(2.0, h / 2)));
            if (clip->disabled) p.drawRect(pixels(block), color);  // outline only, as on the timeline
            else p.fillRect(pixels(block), color);
        }
    }
    return image;
}

void Overview::resized() { refresh(); }

void Overview::paint(Painter& p) {
    if (!hasImage_ || marks_.size() < 6) refresh();
    if (!hasImage_ || marks_.size() < 6) return;
    p.drawImage(Point(0, 0), image_);  // (with its box: background and borders)
    // the editor's view: a box (Overview::view) and its outline (outline-offset -1px: on its outermost pixels)
    RectF view((float)marks_[0], (float)marks_[1], (float)marks_[2], (float)marks_[3]);
    const css::Style& vs = computedStyle("view");
    Rect viewBox = pixels(view);
    box(p, viewBox, vs);
    int outline = ui::style::px(vs, "outline-width", 0);
    if (outline > 0 && vs.keyword("outline-style", "solid") != "none") {
        int grow = outline + ui::style::px(vs, "outline-offset", 0);
        p.drawRect(viewBox.adjusted(-grow, -grow, grow, grow), ui::style::color(vs, "outline-color"), outline);
    }
    // the cursors, down to its bottom border
    int playheadX = (int)marks_[4], cursorX = (int)marks_[5];
    int bottom = height() - ui::style::insets(computedStyle(), "border-width").bottom;
    paint::vline(p, cursorX, 0, bottom, paint::line(computedStyle("cursor"), theme::c("cursor")));
    paint::vline(p, playheadX, 0, bottom, paint::line(computedStyle("playhead"), theme::c("playhead")));
}

// --- mouse: move the editor's view --------------------------------------------------------------

bool Overview::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return true;
    if (onEdge(e.pos.y)) {
        sizing_ = true;
        sizeY_ = e.globalPos.y;
        sizeH_ = height();
        return true;
    }
    double s = span();
    PointF pos(e.pos);
    RectF view = viewRect(s);
    // grabbing the outline keeps the grabbed spot under the mouse; elsewhere the view centres there
    if (view.x <= pos.x && pos.x <= view.right()) {
        grabX_ = pos.x - view.x;
        grabY_ = pos.y - view.y;
    } else {
        grabX_ = view.w / 2;
        grabY_ = view.h / 2;
    }
    grabbing_ = true;
    moveView(pos, s);
    return true;
}

bool Overview::mouseMove(ui::MouseEvent& e) {
    if (sizing_) {
        resizedTo.emit(setHeight((int)(sizeH_ + e.globalPos.y - sizeY_)));
    } else if (grabbing_) {
        moveView(PointF(e.pos), span());
    } else {
        setCursor(onEdge(e.pos.y) ? ui::Cursor::SizeVer : ui::Cursor::Hand);
    }
    return true;
}

bool Overview::mouseRelease(ui::MouseEvent& e) {
    grabbing_ = sizing_ = false;
    return true;
}

bool Overview::mouseDoubleClick(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && onEdge(e.pos.y)) resizedTo.emit(setHeight(overview::standardHeight()));
    return true;
}

void Overview::moveView(PointF pos, double span) {
    ui::ScrollBar* hbar = scroll->horizontalScrollBar();
    ui::ScrollBar* vbar = scroll->verticalScrollBar();
    hbar->setValue((int)std::nearbyint((pos.x - grabX_) * span / width() * timeline->pps));
    double scale = rowScale();
    if (scale) vbar->setValue((int)std::nearbyint((pos.y - grabY_ - padTop()) / scale + lanes::lanesTop()));
}
