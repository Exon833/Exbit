// timeline.py, part 2: painting (lanes, ruler and grid, automation lanes, group miniatures, clips,
// the range, the loop band, the take being recorded, both cursors) and the automation helpers.
#include <algorithm>
#include <cmath>

#include "audio/automation.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "ui/clip_painter.h"
#include "ui/colors.h"
#include "ui/formatting.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/style.h"

using clip_painter::pixels;
using ui::style::box;

namespace {
// the lane's parameter, when its plugin is there and has it (Python: a.target and a.target[1])
std::shared_ptr<plug::Plugin> targetOf(const Automation& a) {
    auto plugin = aload(a.targetPlugin);
    return plugin && a.hasTarget ? plugin : nullptr;
}

// a dotted horizontal line (Qt::DotLine at 1 px: 1 on, 2 off), its pattern anchored at x = 0 so a
// strip painted alone matches the whole
void dottedHLine(Painter& p, int x0, int x1, int y, Color col) {
    for (int x = x0 - (((x0 % 3) + 3) % 3); x < x1; x += 3)
        if (x >= x0) p.drawHLine(x, x + 1, y, col);
}
}  // namespace

void Timeline::paint(Painter& p) {
    Rect r = p.clip();  // the part to draw (often a thin strip: the playhead, a name strip)
    if (r.empty()) return;
    refreshLook();  // (its states, hover or focus, may have changed what applies)
    paintArea(p, r);
}

bool Timeline::automationLocked(const Lane& lane) const {
    // (a track's own Volume and Pan are always there, frozen or not; a plugin's parameter is locked while its
    // track is frozen, and can't be moved while its plugin or parameter isn't there)
    const Automation& a = *lane.automation;
    if (a.slot && a.slot->isControls()) return false;
    return lane.owner->frozen || !targetOf(a);
}

void Timeline::refreshLook() const {
    if (lookStamp_.stale(this)) {
        look_.lanes = &laneLooks_;
        readTimelineLook(look_, this);
        clipLooks_.refresh();
    }
}

const TimelineLook& Timeline::look() const {
    // (the stylesheets' generation only: its own states are looked at once per paint, refreshLook)
    if (lookStamp_.generation != theme::styles().generation()) refreshLook();
    return look_;
}

void Timeline::paintArea(Painter& p, const Rect& r) {
    const TimelineLook& lk = look();
    p.save();
    p.setClip(r);  // (the lanes' boxes are as wide as the timeline)
    auto ls = lanes();
    std::map<const Track*, int> tops;
    for (auto& lane : ls)
        if (lane.track) tops[lane.track.get()] = lane.top;
    paintLanes(p, r, ls);
    paintRuler(p, r);  // (the grid: over the lanes, under what is in them)
    for (auto& lane : ls) {
        if (lane.bottom() < r.top() || lane.top > r.bottom() - 1) continue;
        if (lane.group) paintGroupContents(p, r, lane);
        else if (lane.automation) paintAutomation(p, r, lane);
        else if (lane.take) paintTake(p, r, lane);
    }
    int left = scroll->horizontalScrollBar()->value();
    std::map<const ClipLink*, int> linkCounts = links::counts(project);  // (linked clips: their chain marks)
    for (auto& clip : project.clips) {
        auto rect = clipRect(*clip, tops);
        if (!rect) continue;
        if (rect->right() + 2 >= r.left() && rect->x - 2 <= r.right() - 1  // +2: antialiased border
            && rect->bottom() + 2 >= r.top() && rect->y - 2 <= r.bottom() - 1) {
            auto linked = clip->link ? linkCounts.find(clip->link.get()) : linkCounts.end();
            bool selected = selection.contains(clip), audible = project.audible(*clip->track);
            bool frozen = clip->track->frozen != nullptr;
            int linkedWith = linked != linkCounts.end() ? linked->second : 0;
            clip_painter::paintClip(p, clipLooks_.get(*clip, selected, audible, frozen, linkedWith), *clip, *rect, r,
                                    left, pps, engine.sr, selected, audible, frozen, true, linkedWith);
        }
    }
    if (slicePreview && project.clips.contains(slicePreview->clip)) {  // (the slice window: where slices start)
        const Clip& clip = *slicePreview->clip;
        auto rect = clipRect(clip, tops);
        if (rect) {
            int top = (int)rect->y + clip_painter::headerHeight(), bottom = (int)rect->bottom();
            int reach = std::max(1, std::max(lk.sliceW, lk.slice.width) / 2);
            for (int64_t f : slicePreview->frames) {
                int x = (int)std::lround(xOf(clip.start + f));
                if (x >= r.left() - reach && x <= r.right() - 1 + reach && x >= rect->x && x < rect->right()) {
                    paint::vline(p, x, top, bottom, lk.slice);
                    p.fillRect(Rect(x - lk.sliceW / 2, top, lk.sliceW, lk.sliceH), lk.slice.color);
                }
            }
        }
    }
    if (selection.range) {
        std::vector<std::pair<int, int>> spans;
        for (auto& t : selection.range->tracks) {
            auto it = tops.find(t.get());
            if (it != tops.end()) spans.push_back({it->second, t->height});
        }
        if (!spans.empty())
            clip_painter::paintRange(p, *selection.range, spans, [this](int64_t f) { return xOf(f); }, r, lk.selection,
                                     lk.selectionEdge.color, lk.selectionEdge.width);
    }
    paintLoop(p);
    paintMarkers(p, r);
    if (recording) paintRecording(p, tops);
    paint::cursor(p, cursorX, lk.cursor.color, height(), lk.cursor.width);
    paint::cursor(p, playheadX, lk.playhead.color, height(), lk.playhead.width);  // on top where they meet
    p.restore();
}

void Timeline::paintLanes(Painter& p, const Rect& r, const std::vector<Lane>& ls) {
    // each lane a box as wide as the timeline (the painter is clipped to r): Lane .track (:alternate every
    // other), .group (in its own colour, if it has one), .automation (:armed, :locked) and .take, then its
    // ::overlay; and .below, under the last lane
    LaneLooks& looks = *look().lanes;
    int row = 0;  // track lanes alternate in shade
    for (auto& lane : ls) {
        if (lane.bottom() <= r.top() || lane.top >= r.bottom()) {
            if (lane.track) ++row;
            if (!lane.isMaster()) continue;
        }
        Rect area(0, lane.top, width(), lane.height);
        const LaneLook* l;
        if (lane.track) {
            l = &looks.get(LaneKind::Track, row % 2 != 0);
            ++row;
            box(p, area, l->box);
        } else if (lane.group) {
            l = &looks.get(LaneKind::Group);
            Color own;
            box(p, area, paint::groupColor(*lane.group, own) ? l->recolored.get(l->box, own) : l->box);
        } else if (lane.automation || lane.take) {  // (their contents: after the grid, paintArea)
            l = lane.automation ? &looks.get(LaneKind::Automation, false, lane.automation->armed, automationLocked(lane))
                                : &looks.get(LaneKind::Take);
            box(p, area, l->box);
        } else {  // the master lane's place: room to scroll the last track above the pinned master lane
            l = &looks.get(LaneKind::Below);
            area.h = height() - lane.top;
            box(p, area, l->box);
        }
        if (l->overlayShown) box(p, area, l->overlay);  // (an automation lane armed for writing, Write on its row: red)
    }
}

// --- automation lanes ---------------------------------------------------------------------------

double Timeline::automationY(const Lane& lane, double value) const {
    const int pad = timeline::automationPad();
    return lane.top + pad + (1 - value) * (lane.height - 2 * pad);
}

double Timeline::automationValue(const Lane& lane, double y) const {
    const int pad = timeline::automationPad();
    double value = std::min(1.0, std::max(0.0, 1 - (y - lane.top - pad) / std::max(1, lane.height - 2 * pad)));
    return automation::snap(value, automation::stepsOf(*lane.automation));
}

std::pair<std::string, std::string> Timeline::rangeLabels(const Automation& a) const {
    // (top, bottom): how the plugin shows the parameter at 1 and at 0, e.g. ("+30.0 dB", "-30.0 dB");
    // "max" / "min" if it can't say. Worked out once per plugin. A track's own Volume and Pan have no
    // plugin: they say it themselves ("+6.0 dB" / "-inf dB", "100 R" / "100 L"), as automation.py's
    // TrackParameter does.
    if (a.slot && a.slot->isControls()) return {automation::controlText(a.param, 1.0), automation::controlText(a.param, 0.0)};
    auto plugin = aload(a.targetPlugin);
    const void* key = plugin.get();
    auto cached = rangeText_.find(&a);
    if (cached != rangeText_.end() && cached->second.first == key) return cached->second.second;
    std::pair<std::string, std::string> labels{"max", "min"};
    if (plugin && a.hasTarget) {
        try {
            std::string top = util::trim(plugin->paramText(a.targetParam, 1.0));
            std::string bottom = util::trim(plugin->paramText(a.targetParam, 0.0));
            if (!top.empty() && !bottom.empty() && top != bottom) labels = {top, bottom};
        } catch (...) {
        }
    }
    rangeText_[&a] = {key, labels};
    return labels;
}

std::string Timeline::valueText(const Automation& a, double value) const {
    if (a.slot && a.slot->isControls()) return automation::controlText(a.param, value);  // (a track's own Volume / Pan)
    char buf[32];
    snprintf(buf, sizeof buf, "%.0f %%", value * 100);
    std::string text = buf;
    if (auto plugin = targetOf(a)) {
        try {
            std::string t = util::trim(plugin->paramText(a.targetParam, value));
            if (!t.empty()) text = t;
        } catch (...) {
        }
    }
    return text;
}

void Timeline::paintAutomation(Painter& p, const Rect& r, const Lane& lane) {
    // An automation lane (its background: paintLanes): the curve's range (from 1 at the top to 0 at the
    // bottom) as a tinted band between two lines, a dotted line halfway and the parameter's values
    // at both ends (at the left of the view), and a line above it, so stacked lanes stay apart; then
    // the curve through its points (flat before the first and after the last), and the points.
    // Without points, a dashed line at the parameter's value now. Locked (frozen, or its plugin or
    // parameter isn't there now): the lane is :locked (its curve fainter).
    const Automation& a = *lane.automation;
    auto target = targetOf(a);
    // a track's own Volume and Pan are always there (no plugin to load), and they work on frozen tracks too
    bool controls = a.slot && a.slot->isControls();
    const LaneLook& lk = look().lanes->get(LaneKind::Automation, false, a.armed, automationLocked(lane));
    int top = (int)std::nearbyint(automationY(lane, 1.0)), bottom = (int)std::nearbyint(automationY(lane, 0.0));
    box(p, Rect(0, top, width(), bottom - top + 1), lk.range);
    paint::hline(p, r.left(), r.right(), top, lk.rangeEdge);
    paint::hline(p, r.left(), r.right(), bottom, lk.rangeEdge);
    int steps = automation::stepsOf(a);
    std::vector<double> between;  // (a stepped parameter: a line at each value it can take; else halfway)
    if (steps > 1 && steps <= timeline::STEP_LINES_MAX)
        for (int k = 1; k < steps; ++k) between.push_back((double)k / steps);
    else if (!steps)
        between.push_back(0.5);
    for (double value : between)
        dottedHLine(p, r.left(), r.right(), (int)std::nearbyint(automationY(lane, value)), lk.guide.color);
    // (its top edge again, over the grid: apart from the lane above)
    if (lk.topEdge.width > 0) p.fillRect(Rect(r.left(), lane.top, r.w, lk.topEdge.width), lk.topEdge.color);
    int left = scroll->horizontalScrollBar()->value();
    if (bottom - top >= 30) {
        const Font& font = lk.labelFont;
        auto labels = rangeLabels(a);
        p.drawText(RectF((float)left + lk.labelPadLeft, (float)top + lk.labelPadTop, (float)lk.labelW, (float)lk.labelH),
                   AlignLeft | AlignTop, labels.first, font, lk.labelColor);
        p.drawText(RectF((float)left + lk.labelPadLeft, (float)bottom - lk.labelPadBottom - lk.labelH, (float)lk.labelW,
                         (float)lk.labelH),
                   AlignLeft | AlignBottom, labels.second, font, lk.labelColor);
        if (!between.empty() && steps && (double)(bottom - top) / steps >= timeline::STEP_LABEL_MIN_PX) {
            // a stepped parameter, with room: the name of each value in between, on its line
            const Font& vf = lk.valueFont;
            int fh = (int)std::ceil(vf.height());
            for (double value : between) {
                int y = (int)std::nearbyint(automationY(lane, value));
                std::string text = valueText(a, value);
                Rect b(left + lk.valueMarginLeft, y - fh / 2,
                       (int)std::ceil(vf.width(text)) + lk.valuePadLeft + lk.valuePadRight, fh);
                box(p, b, lk.valueBox);
                p.drawText(RectF(b.adjusted(lk.valuePadLeft, 0, 0, 0)), AlignLeft | AlignVCenter, text, vf, lk.valueColor);
            }
        }
    }
    auto& points = *a.points;
    if (!points.empty()) {
        const size_t n = points.size();
        std::vector<float> xs(n), ys(n);
        for (size_t i = 0; i < n; ++i) {
            xs[i] = (float)std::nearbyint(points[i].t * pps);
            ys[i] = (float)automationY(lane, automation::snap(points[i].v, steps));
        }
        // only the part of the curve that can reach into r (the same pixels as drawing all of it)
        size_t i0 = 0, i1 = n - 1;
        while (i0 + 1 < n && xs[i0 + 1] < r.left() - 2) ++i0;
        while (i1 > 0 && xs[i1 - 1] > r.right() + 2) --i1;
        std::vector<PointF> curve;
        if (i0 == 0) curve.push_back(PointF(std::min((float)r.left(), xs[0]) - 1, ys[0]));
        for (size_t i = i0; i <= i1; ++i) {
            if (steps && i) curve.push_back(PointF(xs[i], ys[i - 1]));  // (stepped: holds the value until the point, then jumps)
            curve.push_back(PointF(xs[i], ys[i]));
            const AutoPoint& from = points[i];
            if (!steps && i < i1 && from.bend != 0 && points[i + 1].t > from.t) {  // (a curved segment: along its shape)
                int pieces = std::max(2, std::min(64, (int)((xs[i + 1] - xs[i]) / 3)));
                double rise = points[i + 1].v - from.v;
                for (int k = 1; k < pieces; ++k) {
                    double x = (double)k / pieces;
                    double v = from.v + rise * automation::bendShape(x, from.bend, rise);
                    curve.push_back(PointF(xs[i] + (xs[i + 1] - xs[i]) * (float)x, (float)automationY(lane, v)));
                }
            }
        }
        if (i1 == n - 1) curve.push_back(PointF(std::max((float)r.right() - 1, xs[n - 1]) + 1, ys[n - 1]));
        if (lk.curveWidth > 0) p.drawPolyline(curve, lk.curve.color, lk.curveWidth);
        // (a point reaches into r from as far as half its size, and its antialiased edge)
        float half = std::max(config::AUTOMATION_GRAB_PX / 2.0f, std::max(lk.pointW, lk.pointH) / 2.0f + 1);
        for (size_t i = 0; i < n; ++i)
            if (r.left() - half <= xs[i] && xs[i] <= r.right() - 1 + half)
                p.fillRect(RectF(xs[i] - lk.pointW / 2.0f, ys[i] - lk.pointH / 2.0f, (float)lk.pointW, (float)lk.pointH),
                           lk.pointColor);
    } else {
        if (target || controls) {
            double value = -1;
            if (controls) {
                value = automation::controlValue(*lane.owner, a.param);  // (the fader's / knob's setting now)
            } else {
                try {
                    value = target->getParam(a.targetParam);
                } catch (...) {
                }
            }
            if (value >= 0) {
                float y = (float)automationY(lane, value);
                float x0 = (float)(r.left() - r.left() % 6);  // (the dashes anchored at x = 0)
                if (lk.level.width > 0)
                    p.drawDashedLine(PointF(x0, y), PointF((float)r.right(), y), lk.level.color, (float)lk.level.width, 4, 2);
            }
        }
        int baseline = paint::lengthPx(lk.hint, "baseline", lane.height / 2 + 4, lane.height);
        p.drawText(PointF((float)left + lk.hintPadLeft, (float)(lane.top + baseline)),
                   a.armed ? "Write is on: play the song and move the control to write the curve"
                           : "Click to add points to the curve",
                   lk.hintFont, lk.hintColor);
    }
}

int Timeline::pointAt(const Lane& lane, PointF pos) const {
    const double grab = config::AUTOMATION_GRAB_PX;
    int best = -1;
    double bestD = 0;
    auto& points = *lane.automation->points;
    for (size_t i = 0; i < points.size(); ++i) {
        double dx = std::fabs(points[i].t * pps - pos.x), dy = std::fabs(automationY(lane, points[i].v) - pos.y);
        if (dx <= grab && dy <= grab && (best < 0 || dx + dy < bestD)) {
            best = (int)i;
            bestD = dx + dy;
        }
    }
    return best;
}

// --- the master lane, groups, loop, recording, ruler --------------------------------------------

void Timeline::paintMaster(Painter& p, const Rect& r, int top, int bottom, const paint::Line& divider) {
    const TimelineLook& lk = look();
    paintRuler(p, r, false, top, bottom);
    if (divider.width > 0) p.fillRect(Rect(r.left(), top, r.w, divider.width), divider.color);  // (over the grid)
    paint::vline(p, cursorX, top, bottom + 1, lk.cursor);
    paint::vline(p, playheadX, top, bottom + 1, lk.playhead);
}

void Timeline::paintGroupContents(Painter& p, const Rect& r, const Lane& lane) {
    // a group's header lane: every clip of its tracks in miniature, one thin row per track
    auto members = project.members(lane.group);
    if (members.empty()) return;
    const LaneLook& lk = look().lanes->get(LaneKind::Group);  // (its padding: above and below them)
    double rowH = (double)(lane.height - lk.padTop - lk.padBottom) / members.size();
    std::map<const Track*, double> rows;
    for (size_t i = 0; i < members.size(); ++i) rows[members[i].get()] = lane.top + lk.padTop + i * rowH;
    for (auto& clip : project.clips) {
        auto it = rows.find(clip->track.get());
        if (it == rows.end()) continue;
        double x0 = xOf(clip->start), x1 = xOf(clip->end());
        if (x1 < r.left() || x0 > r.right()) continue;
        std::string hex, tone;
        colors::clipColor(*clip, hex, tone);
        Color color = colors::clipColors(hex, project.audible(*clip->track)).fill;
        RectF block((float)x0, (float)(it->second + 0.5), (float)std::max(1.0, x1 - x0), (float)std::max(1.0, rowH - 1));
        if (clip->disabled) p.drawRect(pixels(block), color);  // outline only
        else p.fillRect(pixels(block), color);
    }
}

void Timeline::paintLoop(Painter& p) {
    // the loop range: a band across the lower half of the ruler, with its two ends marked
    // (Ruler::loop, Ruler:looping::loop; its ends: ::loop-edge)
    int x0, x1;
    if (!loopXs(x0, x1)) return;
    const TimelineLook& lk = look();
    int on = project.timing.looping ? 1 : 0;
    int h = lk.loopHeight[on], top = lk.rulerHeight - h;
    box(p, Rect(x0, top, x1 - x0, h), lk.loop[on]);
    int w = lk.loopEdgeW[on];
    for (int x : {x0, x1}) p.fillRect(Rect(x - w / 2, top, w, h), lk.loopEdge[on]);
}

void Timeline::paintRecording(Painter& p, const std::map<const Track*, int>& tops) {
    // the take being recorded, growing on its track's lane up to the playback cursor
    const TrackPtr& track = recording->first;
    auto it = tops.find(track.get());
    if (it == tops.end()) return;
    int x0 = (int)xOf(recording->second);
    box(p, Rect(x0, it->second, std::max(0, playheadX - x0), track->height), look().recording);
}

void Timeline::paintRuler(Painter& p, const Rect& r, bool ruler, int line0, int line1) {
    // The ruler (unless ruler=false) and the grid lines below it, from y line0 to line1 (default:
    // from the ruler down the whole timeline).
    const TimelineLook& lk = look();
    const int rulerH = lk.rulerHeight;
    if (line0 < 0) {
        line0 = rulerH;
        line1 = height();
    }
    if (ruler) box(p, Rect(0, 0, width(), rulerH), lk.ruler);
    if (project.timing.beats) {
        paintBars(p, r, ruler, line0, line1);
        if (ruler) paintTempoChanges(p, r);
        return;
    }
    double step = fmt::rulerStep(pps);
    int decimals = fmt::stepDecimals(step);
    const paint::Line& grid = lk.grid[1];
    const TimelineLook::Tick& tick = lk.tick;
    int reach = std::max(grid.width, tick.line.width) / 2;  // (wider lines reach into r from outside it)
    int i0 = (int)((r.left() - reach) / pps / step), i1 = (int)((r.right() - 1 + reach) / pps / step) + 2;
    for (int i = i0; i < i1; ++i) {
        int x = (int)(i * step * pps);
        paint::vline(p, x, line0, line1 + 1, grid);
        if (!ruler) continue;
        paint::vline(p, x, rulerH - tick.height, rulerH + 1, tick.line);
        p.drawText(PointF((float)x + lk.labelPadLeft, (float)lk.labelBaseline), fmt::time(i * step, decimals),
                   lk.labelFont, lk.rulerText);
    }
}

void Timeline::paintBars(Painter& p, const Rect& r, bool ruler, int line0, int line1) {
    // Bars-and-beats ruler and grid: the closer the zoom, the finer the grid. Labelled lines cross
    // the whole ruler; other bars, beats and finer lines get shorter ticks.
    // (Timeline::grid-fine / ::grid / ::grid-bar; Ruler::tick-fine / ::tick-beat / ::tick-bar, ::label)
    const TimelineLook& lk = look();
    const int rulerH = lk.rulerHeight;
    const int labelRoom = 80;         // px: a label whose line is left of r can still reach into r
    int reach = 0;                    // (wider lines reach into r from outside it)
    for (int s = 0; s < 3; ++s) reach = std::max({reach, lk.grid[s].width / 2, lk.ticks[s].line.width / 2});
    double to = r.right() / pps;
    if (reach) to = (r.right() + reach) / pps;
    for (auto& gl : project.timing.lines(std::max(0, r.left() - std::max(labelRoom, reach)) / pps, to, pps)) {
        int x = (int)(gl.seconds * pps);
        int strength = std::min(2, std::max(0, gl.strength));
        paint::vline(p, x, line0, line1 + 1, lk.grid[strength]);
        if (!ruler) continue;
        const TimelineLook::Tick& tick = lk.ticks[strength];
        paint::vline(p, x, gl.label.empty() ? rulerH - tick.height : 0, rulerH + 1, tick.line);
        if (!gl.label.empty())
            p.drawText(PointF((float)x + lk.labelPadLeft, (float)lk.labelBaseline), gl.label, lk.labelFont, lk.rulerText);
    }
}
