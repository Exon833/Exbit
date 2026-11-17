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

using clip_painter::pixels;
using theme::c;

namespace {
// the lane's parameter, when its plugin is there and has it (Python: a.target and a.target[1])
std::shared_ptr<plug::Plugin> targetOf(const Automation& a) {
    auto plugin = aload(a.targetPlugin);
    return plugin && a.hasTarget ? plugin : nullptr;
}

const Color& gridColor(int strength) {
    static const char* names[] = {"grid_fine", "grid", "grid_bar"};
    return c(names[std::min(2, std::max(0, strength))]);
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
    paintArea(p, r);
}

void Timeline::paintArea(Painter& p, const Rect& r) {
    auto ls = lanes();
    std::map<const Track*, int> tops;
    for (auto& lane : ls)
        if (lane.track) tops[lane.track.get()] = lane.top;
    paintLanes(p, r, ls);
    paintRuler(p, r);  // (the grid: over the lanes, under what is in them)
    for (auto& lane : ls) {
        if (lane.bottom() < r.top() || lane.top > r.bottom() - 1) continue;
        if (lane.group) paintGroupContents(p, r, lane);
        else if (lane.automation) paintAutomation(p, r, lane, false);
        else if (lane.take) paintTake(p, r, lane, false);
    }
    int left = scroll->horizontalScrollBar()->value();
    std::map<const ClipLink*, int> linkCounts = links::counts(project);  // (linked clips: their chain marks)
    for (auto& clip : project.clips) {
        auto rect = clipRect(*clip, tops);
        if (!rect) continue;
        if (rect->right() + 2 >= r.left() && rect->x - 2 <= r.right() - 1  // +2: antialiased border
            && rect->bottom() + 2 >= r.top() && rect->y - 2 <= r.bottom() - 1) {
            auto linked = clip->link ? linkCounts.find(clip->link.get()) : linkCounts.end();
            clip_painter::paintClip(p, *clip, *rect, r, left, pps, engine.sr, selection.contains(clip),
                                    project.audible(*clip->track), clip->track->frozen != nullptr, true,
                                    linked != linkCounts.end() ? linked->second : 0);
        }
    }
    if (selection.range) {
        std::vector<std::pair<int, int>> spans;
        for (auto& t : selection.range->tracks) {
            auto it = tops.find(t.get());
            if (it != tops.end()) spans.push_back({it->second, t->height});
        }
        if (!spans.empty())
            clip_painter::paintRange(p, *selection.range, spans, [this](int64_t f) { return xOf(f); }, r);
    }
    paintLoop(p);
    paintMarkers(p, r);
    if (recording) paintRecording(p, tops);
    paint::cursor(p, cursorX, c("cursor"), height());
    paint::cursor(p, playheadX, c("playhead"), height());  // on top where they meet
}

void Timeline::paintLanes(Painter& p, const Rect& r, const std::vector<Lane>& ls) {
    int row = 0;  // track lanes alternate in shade
    for (auto& lane : ls) {
        if (lane.bottom() <= r.top() || lane.top >= r.bottom()) {
            if (lane.track) ++row;
            if (!lane.isMaster()) continue;
        }
        Rect area(r.left(), lane.top, r.w, lane.height);
        if (lane.track) {
            p.fillRect(area, c(row % 2 ? "lane_alt" : "lane"));
            ++row;
        } else if (lane.group) {
            p.fillRect(area, paint::groupFill(*lane.group));
            p.fillRect(Rect(r.left(), lane.top, r.w, 1), c("ruler_bg"));
        } else if (lane.automation || lane.take) {  // (their contents: after the grid, paintArea)
            p.fillRect(area, c("automation_lane"));
        } else {  // the master lane's place: room to scroll the last track above the pinned master lane
            p.fillRect(Rect(r.left(), lane.top, r.w, height() - lane.top), c("below_lanes"));
        }
    }
}

// --- automation lanes ---------------------------------------------------------------------------

double Timeline::automationY(const Lane& lane, double value) const {
    const int pad = config::AUTOMATION_PAD;
    return lane.top + pad + (1 - value) * (lane.height - 2 * pad);
}

double Timeline::automationValue(const Lane& lane, double y) const {
    const int pad = config::AUTOMATION_PAD;
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

void Timeline::paintAutomation(Painter& p, const Rect& r, const Lane& lane, bool background) {
    // An automation lane: its background, with the curve's range (from 1 at the top to 0 at the
    // bottom) as a tinted band between two lines, a dotted line halfway and the parameter's values
    // at both ends (at the left of the view), and a line above it, so stacked lanes stay apart; then
    // the curve through its points (flat before the first and after the last), and the points.
    // Without points, a dashed line at the parameter's value now.
    const Automation& a = *lane.automation;
    if (background) p.fillRect(Rect(r.left(), lane.top, r.w, lane.height), c("automation_lane"));
    Color line = c("automation_line");
    Color color = line;
    auto target = targetOf(a);
    // a track's own Volume and Pan are always there (no plugin to load), and they work on frozen tracks too
    bool controls = a.slot && a.slot->isControls();
    if (!controls && (lane.owner->frozen || !target)) color.a = 110;  // locked (frozen), or its plugin or parameter isn't there now
    int top = (int)std::nearbyint(automationY(lane, 1.0)), bottom = (int)std::nearbyint(automationY(lane, 0.0));
    p.fillRect(Rect(r.left(), top, r.w, bottom - top + 1), line.withAlpha(14));
    Color guide = line.withAlpha(70);
    p.drawHLine(r.left(), r.right(), top, guide);
    p.drawHLine(r.left(), r.right(), bottom, guide);
    guide.a = 40;
    int steps = automation::stepsOf(a);
    std::vector<double> between;  // (a stepped parameter: a line at each value it can take; else halfway)
    if (steps > 1 && steps <= timeline::STEP_LINES_MAX)
        for (int k = 1; k < steps; ++k) between.push_back((double)k / steps);
    else if (!steps)
        between.push_back(0.5);
    for (double value : between) dottedHLine(p, r.left(), r.right(), (int)std::nearbyint(automationY(lane, value)), guide);
    p.fillRect(Rect(r.left(), lane.top, r.w, 1), gridColor(2));  // (apart from the lane above)
    int left = scroll->horizontalScrollBar()->value();
    if (bottom - top >= 30) {
        Font ui = theme::uiFont();
        Font font = ui.withPointSize(std::max(6.5f, ui.pointSize() - 1.5f));
        Color label = line.withAlpha(170);
        auto labels = rangeLabels(a);
        p.drawText(RectF((float)left + 6, (float)top + 2, 220, 14), AlignLeft | AlignTop, labels.first, font, label);
        p.drawText(RectF((float)left + 6, (float)bottom - 15, 220, 14), AlignLeft | AlignBottom, labels.second, font,
                   label);
        if (!between.empty() && steps && (double)(bottom - top) / steps >= timeline::STEP_LABEL_MIN_PX) {
            // a stepped parameter, with room: the name of each value in between, on its line
            int fh = (int)std::ceil(font.height());
            for (double value : between) {
                int y = (int)std::nearbyint(automationY(lane, value));
                std::string text = valueText(a, value);
                Rect box(left + 4, y - fh / 2, (int)std::ceil(font.width(text)) + 6, fh);
                p.fillRect(box, c("automation_lane"));
                p.drawText(RectF(box.adjusted(2, 0, 0, 0)), AlignLeft | AlignVCenter, text, font, label);
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
        }
        if (i1 == n - 1) curve.push_back(PointF(std::max((float)r.right() - 1, xs[n - 1]) + 1, ys[n - 1]));
        p.drawPolyline(curve, color, 1.5f);
        float half = config::AUTOMATION_GRAB_PX / 2.0f;
        for (size_t i = 0; i < n; ++i)
            if (r.left() - half <= xs[i] && xs[i] <= r.right() - 1 + half)
                p.fillRect(RectF(xs[i] - half + 1, ys[i] - half + 1, 2 * half - 2, 2 * half - 2), color);
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
                p.drawDashedLine(PointF(x0, y), PointF((float)r.right(), y), color, 1, 4, 2);
            }
        }
        p.drawText(PointF((float)left + 8, (float)(lane.top + lane.height / 2 + 4)), "Click to add points to the curve",
                   theme::uiFont(), c("ruler_text"));
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

void Timeline::paintMaster(Painter& p, const Rect& r, int top, int bottom) {
    p.fillRect(Rect(r.left(), top, r.w, bottom - top), c("master_lane"));
    paintRuler(p, r, false, top, bottom);
    p.fillRect(Rect(r.left(), top, r.w, 1), c("master_divider"));
    p.drawVLine(cursorX, top, bottom + 1, c("cursor"));
    p.drawVLine(playheadX, top, bottom + 1, c("playhead"));
}

void Timeline::paintGroupContents(Painter& p, const Rect& r, const Lane& lane) {
    // a group's header lane: every clip of its tracks in miniature, one thin row per track
    auto members = project.members(lane.group);
    if (members.empty()) return;
    double rowH = (double)(lane.height - 2 * timeline::GROUP_PAD) / members.size();
    std::map<const Track*, double> rows;
    for (size_t i = 0; i < members.size(); ++i) rows[members[i].get()] = lane.top + timeline::GROUP_PAD + i * rowH;
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
    int x0, x1;
    if (!loopXs(x0, x1)) return;
    Color color = c(project.timing.looping ? "loop" : "loop_off");
    int top = config::RULER_H / 2;
    p.fillRect(Rect(x0, top, x1 - x0, config::RULER_H - top), color);
    Color edge = color.withAlpha(255);
    for (int x : {x0, x1}) p.fillRect(Rect(x - 1, top, 2, config::RULER_H - top), edge);
}

void Timeline::paintRecording(Painter& p, const std::map<const Track*, int>& tops) {
    // the take being recorded, growing on its track's lane up to the playback cursor
    const TrackPtr& track = recording->first;
    auto it = tops.find(track.get());
    if (it == tops.end()) return;
    int x0 = (int)xOf(recording->second);
    p.fillRect(Rect(x0, it->second, std::max(0, playheadX - x0), track->height), c("record").withAlpha(70));
}

void Timeline::paintRuler(Painter& p, const Rect& r, bool ruler, int line0, int line1) {
    // The ruler (unless ruler=false) and the grid lines below it, from y line0 to line1 (default:
    // from the ruler down the whole timeline).
    if (line0 < 0) {
        line0 = config::RULER_H;
        line1 = height();
    }
    if (ruler) p.fillRect(Rect(r.left(), 0, r.w, config::RULER_H), c("ruler_bg"));
    if (project.timing.beats) {
        paintBars(p, r, ruler, line0, line1);
        if (ruler) paintTempoChanges(p, r);
        return;
    }
    double step = fmt::rulerStep(pps);
    int decimals = fmt::stepDecimals(step);
    Font font = theme::uiFont();
    Color grid = c("grid");
    Color text = c("ruler_text");
    int i0 = (int)(r.left() / pps / step), i1 = (int)((r.right() - 1) / pps / step) + 2;
    for (int i = i0; i < i1; ++i) {
        int x = (int)(i * step * pps);
        p.drawVLine(x, line0, line1 + 1, grid);
        if (!ruler) continue;
        p.drawVLine(x, config::RULER_H - 6, config::RULER_H + 1, text);
        p.drawText(PointF((float)x + 4, 15), fmt::time(i * step, decimals), font, text);
    }
}

void Timeline::paintBars(Painter& p, const Rect& r, bool ruler, int line0, int line1) {
    // Bars-and-beats ruler and grid: the closer the zoom, the finer the grid. Labelled lines cross
    // the whole ruler; other bars, beats and finer lines get shorter ticks.
    const int ticks[3] = {3, 6, 10};  // FINE, BEAT, BAR
    const int labelRoom = 80;         // px: a label whose line is left of r can still reach into r
    Font font = theme::uiFont();
    Color text = c("ruler_text");
    for (auto& gl : project.timing.lines(std::max(0, r.left() - labelRoom) / pps, r.right() / pps, pps)) {
        int x = (int)(gl.seconds * pps);
        p.drawVLine(x, line0, line1 + 1, gridColor(gl.strength));
        if (!ruler) continue;
        int strength = std::min(2, std::max(0, gl.strength));
        p.drawVLine(x, gl.label.empty() ? config::RULER_H - ticks[strength] : 0, config::RULER_H + 1, text);
        if (!gl.label.empty()) p.drawText(PointF((float)x + 4, 15), gl.label, font, text);
    }
}
