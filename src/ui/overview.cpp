#include "ui/overview.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/clip_painter.h"
#include "ui/colors.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/timeline.h"

using clip_painter::pixels;
using overview::EDGE;
using overview::PAD;
using theme::c;

Overview::Overview(Project& project_, audio::Engine& engine_, Timeline* timeline_, ui::ScrollArea* scroll_, int height)
    : project(project_), engine(engine_), timeline(timeline_), scroll(scroll_) {
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
    height = std::max(config::OVERVIEW_H, std::min(config::OVERVIEW_MAX_H, height));
    setFixedHeight(height);
    return height;
}

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
    return total ? (double)(height() - 2 * PAD) / total : 0;
}

std::vector<double> Overview::contentKey(double span) const {
    // everything the tracks-and-clips image depends on
    std::vector<double> key{(double)width(), (double)height(), std::nearbyint(span * 1000) / 1000};
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
    int top = std::min(std::max(vbar->value() - config::RULER_H, 0), lanesH);
    int bottom = std::min(std::max(vbar->value() + vp->height() - config::RULER_H, 0), lanesH);
    double y0 = PAD, y1 = height() - PAD;
    if (scale) {
        y0 = PAD + top * scale;
        y1 = PAD + bottom * scale;
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
    refresh();
}

// --- painting -----------------------------------------------------------------------------------

Image Overview::renderContent(double span) const {
    // the rows and clip blocks, as an image the size of the bar
    Image image(std::max(1, width()), std::max(1, height()), c("overview_bg"));
    Painter p(image);
    double scale = rowScale(), xScale = width() / span / engine.sr;
    // track -> (y, height) of its own row / of its row in its group's header
    std::map<const Track*, std::pair<double, double>> rows, inHeader;
    int row = 0;
    for (auto& lane : lanes()) {
        double y = PAD + lane.top * scale, h = lane.height * scale;
        RectF area(0, (float)y, (float)width(), (float)h);
        if (lane.automation || lane.take) {
            p.fillRect(pixels(area), c("automation_lane"));
        } else if (lane.track) {
            p.fillRect(pixels(area), c(row % 2 ? "lane_alt" : "lane"));
            rows[lane.track.get()] = {y, h};
            ++row;
        } else {  // a group's header: its tracks' clips, each track a thinner row
            p.fillRect(pixels(area), paint::groupFill(*lane.group));
            auto members = project.members(lane.group);
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
    p.drawImage(Point(0, 0), image_);
    RectF view((float)marks_[0], (float)marks_[1], (float)marks_[2], (float)marks_[3]);
    p.fillRect(pixels(view), c("overview_view_fill"));
    p.drawRect(pixels(view), c("overview_view"));
    int playheadX = (int)marks_[4], cursorX = (int)marks_[5];
    p.drawVLine(cursorX, 0, height() + 1, c("cursor"));
    p.drawVLine(playheadX, 0, height() + 1, c("playhead"));
    p.drawHLine(0, width() + 1, height() - 1, c("overview_edge"));
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
    if (e.button == plat::LeftButton && onEdge(e.pos.y)) resizedTo.emit(setHeight(config::OVERVIEW_H));
    return true;
}

void Overview::moveView(PointF pos, double span) {
    ui::ScrollBar* hbar = scroll->horizontalScrollBar();
    ui::ScrollBar* vbar = scroll->verticalScrollBar();
    hbar->setValue((int)std::nearbyint((pos.x - grabX_) * span / width() * timeline->pps));
    double scale = rowScale();
    if (scale) vbar->setValue((int)std::nearbyint((pos.y - grabY_ - PAD) / scale + config::RULER_H));
}
