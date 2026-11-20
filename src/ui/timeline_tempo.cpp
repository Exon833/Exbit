// The timeline's tempo lane (timeline.h, "the tempo lane"): the tempo map's points, steps and ramps under the
// ruler, edited with the mouse; and the menus and dialog the ruler and the terminal share with it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/util.h"
#include "ui/lanes.h"
#include "ui/paint_helpers.h"
#include "ui/tempo_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/style.h"

using ui::style::box;

namespace {
std::string g(double v) {  // Python's :g
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// a point's place from its beat on the map: (bar, beats into it), tiny leftovers of rounding on the bar line
TempoRef placeAt(const TempoMap& m, double beat) {
    int bar = m.barAtBeat(beat);
    double in = beat - m.barBeat(bar);
    double grid = std::nearbyint(in * 3840) / 3840;  // (rounding left by seconds → beats: on the grid's fraction)
    if (std::fabs(grid - in) < 1e-6) in = grid;
    double next = m.barBeat(bar + 1) - m.barBeat(bar);
    if (in > next - 1e-6) bar++, in = 0;
    return TempoRef{bar + 1, std::max(0.0, in)};
}

TempoRef refOf(const TempoMap& m, const TempoPoint& p) {
    if (p.change < 0) return TempoRef{};
    const TempoChange& c = m.changes[(size_t)p.change];
    return TempoRef{c.bar, c.beat};
}

// the time signature in force just before `beat` (what a point there that isn't a time signature keeps)
int meterBefore(const TempoMap& m, double beat) {
    int bar, perBar;
    double at;
    m.meterAt(std::max(0.0, beat - 1e-6), bar, at, perBar);
    return perBar;
}

// does the tempo ruling at `beat` ramp (a new point there keeps the ramp going through it)?
bool rampingAt(const TempoMap& m, double beat) {
    bool ramp = false;
    for (auto& p : m.points())
        if (p.beat <= beat + 1e-9) ramp = p.ramp;
    return ramp;
}

std::string placeText(const TempoRef& at) {
    return "bar " + std::to_string(at.bar) + (at.beat > 0 ? ", beat " + g(at.beat + 1) : "");
}
}  // namespace

bool Timeline::inTempoLane(double y) const {
    int top = lanes::rulerHeight();
    return lanes::tempoLaneShown() && y >= top && y < top + lanes::tempoLaneHeight();
}

void Timeline::tempoRange(double& lo, double& hi) const {
    if (auto* d = std::get_if<TempoDrag>(&drag)) {  // (kept while dragging: the point stays under the mouse)
        lo = d->lo, hi = d->hi;
        return;
    }
    TempoMapPtr m = project.timing.map();
    lo = hi = m->bpm;
    for (auto& p : m->points()) lo = std::min(lo, p.bpm), hi = std::max(hi, p.bpm);
    double mid = (lo + hi) / 2, span = std::max(40.0, (hi - lo) * 1.3);  // (some room above and below, in tens)
    lo = std::max(config::MIN_BPM, std::floor((mid - span / 2) / 10) * 10);
    hi = std::min(config::MAX_BPM, std::ceil((mid + span / 2) / 10) * 10);
    if (hi <= lo) hi = lo + 10;
}

double Timeline::tempoY(double bpm) const {
    double lo, hi;
    tempoRange(lo, hi);
    int pad = timeline::automationPad(), top = lanes::rulerHeight(), h = lanes::tempoLaneHeight();
    return top + pad + (hi - bpm) / (hi - lo) * (h - 2 * pad);
}

double Timeline::tempoBpmAt(double y) const {
    double lo, hi;
    tempoRange(lo, hi);
    int pad = timeline::automationPad(), top = lanes::rulerHeight(), h = lanes::tempoLaneHeight();
    double bpm = hi - (y - top - pad) / std::max(1, h - 2 * pad) * (hi - lo);
    return std::min(config::MAX_BPM, std::max(config::MIN_BPM, std::nearbyint(bpm * 10) / 10));  // (to 0.1 BPM)
}

std::optional<TempoRef> Timeline::tempoPointAt(PointF pos) const {
    if (!inTempoLane(pos.y)) return std::nullopt;
    TempoMapPtr m = project.timing.map();
    std::optional<TempoRef> found;
    double best = 1e18;
    for (auto& p : m->points()) {
        double dx = p.seconds * pps - pos.x, dy = tempoY(p.bpm) - pos.y;
        if (std::fabs(dx) > config::AUTOMATION_GRAB_PX || std::fabs(dy) > config::AUTOMATION_GRAB_PX) continue;
        if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, found = refOf(*m, p);
    }
    return found;
}

std::optional<TempoRef> Timeline::tempoPointNear(double seconds, double px) const {
    TempoMapPtr m = project.timing.map();
    std::optional<TempoRef> found;
    double best = px + 1e-9;
    for (auto& p : m->points()) {
        double d = std::fabs(p.seconds - seconds) * pps;
        if (d <= best) best = d, found = refOf(*m, p);
    }
    return found;
}

double Timeline::tempoSeconds(const TempoRef& at) const {
    if (at.song()) return 0;
    TempoMapPtr m = project.timing.map();
    return m->secondsAt(m->beatOf(TempoChange{at.bar, 120, 4, at.beat}));
}

// --- painting ---------------------------------------------------------------------------------------

void Timeline::paintTempoLane(Painter& p, const Rect& r, bool contents) {
    // (an automation lane's look: its box and range band, the line of its curve, its points and labels)
    if (!lanes::tempoLaneShown()) return;
    int top = lanes::rulerHeight(), h = lanes::tempoLaneHeight();
    Rect area = r.intersected(Rect(0, top, width(), h));
    if (area.empty()) return;
    const LaneLook& lk = look().lanes->get(LaneKind::Automation);
    TempoMapPtr m = project.timing.map();
    double lo, hi;
    tempoRange(lo, hi);
    if (!contents) {
        box(p, Rect(0, top, width(), h), lk.box);
        return;
    }
    p.save();
    p.setClip(area);
    int yTop = (int)std::nearbyint(tempoY(hi)), yBottom = (int)std::nearbyint(tempoY(lo));
    box(p, Rect(0, yTop, width(), yBottom - yTop + 1), lk.range);
    paint::hline(p, area.left(), area.right(), yTop, lk.rangeEdge);
    paint::hline(p, area.left(), area.right(), yBottom, lk.rangeEdge);
    if (lk.topEdge.width > 0) p.fillRect(Rect(area.left(), top, area.w, lk.topEdge.width), lk.topEdge.color);
    // the line: flat for a steady tempo, sloped through a ramp (linear in beats: drawn in pieces), jumping at a step
    const std::vector<Segment>& segs = m->segments();
    double t0 = std::max(0.0, (area.left() - 2) / pps), t1 = (area.right() + 2) / pps;
    std::vector<PointF> line;
    for (size_t i = 0; i < segs.size(); i++) {
        const Segment& s = segs[i];
        double end = i + 1 < segs.size() ? segs[i + 1].seconds : t1;
        if (end < t0 || s.seconds > t1) continue;
        double a = std::max(t0, s.seconds), b = std::min(t1, std::max(end, a));
        int pieces = s.k == 0 ? 1 : std::max(2, std::min(4000, (int)((b - a) * pps / 3)));
        for (int k = 0; k <= pieces; k++) {
            double at = a + (b - a) * k / pieces;
            // (on this segment's own tempo, up to its very end: a step then jumps straight up or down)
            line.push_back(PointF((float)(at * pps), (float)tempoY(s.bpmAfter(std::max(0.0, s.beatsFor(at - s.seconds))))));
        }
    }
    if (line.size() >= 2 && lk.curveWidth > 0) p.drawPolyline(line, lk.curve.color, lk.curveWidth);
    // the points, each with its tempo when there is room; the time signature at each bar where it changes
    const Font& font = lk.labelFont;
    const std::vector<TempoPoint>& points = m->points();
    std::optional<TempoRef> dragged;
    if (auto* d = std::get_if<TempoDrag>(&drag)) dragged = d->at;
    for (size_t i = 0; i < points.size(); i++) {
        float x = (float)(points[i].seconds * pps), y = (float)tempoY(points[i].bpm);
        if (x < area.left() - 80 || x > area.right() + 4) continue;
        float grow = dragged && *dragged == refOf(*m, points[i]) ? 2.0f : 0.0f;
        p.fillRect(RectF(x - lk.pointW / 2.0f - grow, y - lk.pointH / 2.0f - grow, lk.pointW + 2 * grow, lk.pointH + 2 * grow),
                   lk.pointColor);
        double room = i + 1 < points.size() ? (points[i + 1].seconds - points[i].seconds) * pps : 1e9;
        std::string text = g(std::nearbyint(points[i].bpm * 100) / 100) + (points[i].ramp ? " ↗" : "");
        if (room >= font.width(text) + 12) {
            float ty = y - lk.pointH / 2.0f - 2 - (float)font.height() < top ? y + 2 : y - lk.pointH / 2.0f - 2 - (float)font.height();
            p.drawText(RectF(x + 4, ty, (float)font.width(text) + 2, (float)font.height()), AlignLeft | AlignTop, text, font,
                       lk.labelColor);
        }
    }
    for (size_t i = 0; i < segs.size(); i++) {
        const Segment& s = segs[i];
        if (s.beat != s.barBeat || (i > 0 && segs[i - 1].beatsPerBar == s.beatsPerBar)) continue;
        float x = (float)(s.seconds * pps);
        if (x < area.left() - 60 || x > area.right()) continue;
        std::string sig = std::to_string(s.beatsPerBar) + "/4";
        p.drawText(RectF(x + 4, (float)(top + h) - (float)font.height() - 2, (float)font.width(sig) + 2, (float)font.height()),
                   AlignLeft | AlignTop, sig, font, lk.labelColor);
    }
    // the range at the view's right (its left has bar 1's point and time signature)
    float right = (float)(scroll->horizontalScrollBar()->value() + scroll->viewport()->width() - lk.labelPadLeft);
    std::string high = g(hi) + " BPM", low = g(lo) + " BPM";
    p.drawText(RectF(right - font.width(high) - 2, (float)yTop + 1, font.width(high) + 2, (float)font.height()),
               AlignLeft | AlignTop, high, font, lk.labelColor);
    p.drawText(RectF(right - font.width(low) - 2, (float)yBottom - 1 - (float)font.height(), font.width(low) + 2,
                     (float)font.height()),
               AlignLeft | AlignTop, low, font, lk.labelColor);
    p.restore();
}

// --- editing ----------------------------------------------------------------------------------------

void Timeline::tempoEdit(const std::function<void()>& edit) {
    timebase::Before before = timebase::remember(project);
    edit();
    std::vector<ClipPtr> moved = timebase::follow(project, before, engine.sr);
    const Timing& t = project.timing;
    util::debugf("timeline: tempo edit: %zu changes, bar 1 at %g BPM%s; timebase %s, %zu clips moved\n", t.changes.size(),
                 t.bpm, t.ramp ? " (ramping)" : "", t.timebaseBeats ? "Beats" : "Time", moved.size());
    timingChanged.emit();
    relayout();
    update();
}

void Timeline::editTempoPoint(const TempoRef& at) {
    Timing& t = project.timing;
    TempoMapPtr m = t.map();
    const TempoChange* c = at.song() ? nullptr : t.changeAt(at.bar, at.beat);
    double beat = at.song() ? 0 : m->beatOf(TempoChange{at.bar, 120, 4, at.beat});
    double bpm = c ? c->bpm : at.song() ? t.bpm : m->bpmAtBeat(beat);
    int before = at.song() ? t.beatsPerBar : meterBefore(*m, beat);
    int perBar = c ? c->beatsPerBar : before;
    bool ramp = c ? c->ramp : at.song() && t.ramp;  // (a new change: a step)
    bool canRamp = !m->points().empty() && m->points().back().beat > beat + 1e-9;
    TempoDialog dialog(dialogParent(), at.bar, bpm, perBar, c != nullptr, at.beat, ramp, canRamp, t.timebaseBeats);
    if (!dialog.run()) return;
    tempoEdit([&] {
        if (at.song()) {
            t.bpm = dialog.bpm;
            t.beatsPerBar = dialog.beatsPerBar;
            t.ramp = dialog.ramp;
        } else if (dialog.removed) {
            t.removeChange(at.bar, at.beat);
        } else {
            t.setChange(TempoChange{at.bar, dialog.bpm, at.beat == 0 ? dialog.beatsPerBar : before, at.beat, dialog.ramp});
        }
    });
}

void Timeline::addTempoPoint(double seconds) {
    const Timing& t = project.timing;
    TempoMapPtr m = t.map();
    double at = t.snaps() ? t.snap(seconds, pps) : seconds;
    editTempoPoint(placeAt(*m, m->beatAt(std::max(0.0, at))));  // (the dialog: a new change there, or the one there)
}

void Timeline::removeTempoPoint(const TempoRef& at) {
    if (at.song() || !project.timing.changeAt(at.bar, at.beat)) return;
    tempoEdit([&] { project.timing.removeChange(at.bar, at.beat); });
}

void Timeline::setTempoRamp(const TempoRef& at, bool on) {
    Timing& t = project.timing;
    tempoEdit([&] {
        if (at.song()) {
            t.ramp = on;
        } else if (const TempoChange* c = t.changeAt(at.bar, at.beat)) {
            TempoChange changed = *c;
            changed.ramp = on;
            t.setChange(changed);
        }
    });
}

void Timeline::setTimebase(bool beats) {
    project.timing.timebaseBeats = beats;
    util::debugf("timeline: timebase %s\n", beats ? "Beats (the song keeps its bars)" : "Time (it keeps its seconds)");
    if (status)
        status(beats ? "Timebase: Beats. A tempo change keeps clips, notes, automation and markers on their beats"
                     : "Timebase: Time. A tempo change keeps everything where it is in seconds",
               5000);
    update();
}

void Timeline::fillTempoMenu(ui::Menu& menu, double seconds, std::optional<TempoRef> point) {
    const Timing& t = project.timing;
    TempoMapPtr m = t.map();
    if (point) {
        TempoRef at = *point;
        const TempoChange* c = at.song() ? nullptr : t.changeAt(at.bar, at.beat);
        double bpm = c ? c->bpm : t.bpm;
        bool ramp = c ? c->ramp : t.ramp;
        double beat = at.song() ? 0 : m->beatOf(TempoChange{at.bar, 120, 4, at.beat});
        bool canRamp = !m->points().empty() && m->points().back().beat > beat + 1e-9;
        menu.addAction("Edit the tempo at " + placeText(at) + " (" + g(bpm) + " BPM)…", [this, at] { editTempoPoint(at); });
        ui::Action* r = menu.addCheckable("Ramp to the next change", ramp && canRamp,
                                          [this, at](bool on) { setTempoRamp(at, on); });
        r->tooltip = "On: the tempo goes smoothly from here to the next change's. Off: a step";
        r->setEnabled(canRamp);
        ui::Action* del = menu.addAction("Remove the tempo change at " + placeText(at), [this, at] { removeTempoPoint(at); });
        del->setEnabled(!at.song());
    } else {
        menu.addAction("Add a tempo change here…", [this, seconds] { addTempoPoint(seconds); });
    }
    ui::Menu* base = menu.addMenu("Timebase");
    ui::Action* beats = base->addCheckable("Beats: the song keeps its bars when the tempo changes", t.timebaseBeats,
                                           [this](bool on) {
                                               if (on) setTimebase(true);
                                           });
    beats->group = 1;
    beats->tooltip = "Clips, notes, automation and markers keep their beats; audio plays at its own speed";
    ui::Action* time = base->addCheckable("Time: everything keeps its place in seconds", !t.timebaseBeats,
                                          [this](bool on) {
                                              if (on) setTimebase(false);
                                          });
    time->group = 1;
}

// --- the mouse --------------------------------------------------------------------------------------

bool Timeline::tempoPress(ui::MouseEvent& e) {
    PointF pos(e.pos);
    Timing& t = project.timing;
    TempoDrag d;
    tempoRange(d.lo, d.hi);
    d.before = std::make_shared<timebase::Before>(timebase::remember(project));
    std::optional<TempoRef> point = tempoPointAt(pos);
    TempoMapPtr m = t.map();
    if (!point) {  // a new point at the grid line there (at the tempo the height says), dragged at once
        double seconds = std::max(0.0, pos.x / pps);
        if (t.snaps(e.alt())) seconds = t.snap(seconds, pps);
        double beat = m->beatAt(seconds);
        TempoRef at = placeAt(*m, beat);
        if (!at.song() && !t.changeAt(at.bar, at.beat)) {
            t.setChange(TempoChange{at.bar, tempoBpmAt(pos.y), meterBefore(*m, beat), at.beat, rampingAt(*m, beat)});
            timebase::follow(project, *d.before, engine.sr);
            d.moved = true;
            util::debugf("timeline: tempo point added at %s, %g BPM\n", placeText(at).c_str(), tempoBpmAt(pos.y));
        }
        point = at;
        m = t.map();
    }
    d.at = *point;
    if (const TempoChange* c = d.at.song() ? nullptr : t.changeAt(d.at.bar, d.at.beat))  // (a time signature: by bars)
        d.bars = c->atBar() && c->beatsPerBar != meterBefore(*m, m->beatOf(*c));
    drag = d;
    update();
    return true;
}

void Timeline::dragTempo(PointF pos, int mods) {
    auto* d = std::get_if<TempoDrag>(&drag);
    if (!d) return;
    Timing& t = project.timing;
    const TempoChange* c = d->at.song() ? nullptr : t.changeAt(d->at.bar, d->at.beat);
    if (!d->at.song() && !c) return;  // (gone meanwhile)
    double bpm = tempoBpmAt(pos.y);
    if (d->at.song()) {  // (bar 1's: only its tempo)
        t.bpm = bpm;
    } else {
        TempoMapPtr m = t.map();
        double seconds = std::max(0.0, pos.x / pps);
        if (t.snaps((mods & plat::ModAlt) != 0)) seconds = t.snap(seconds, pps);
        double beat = m->beatAt(seconds);
        if (d->bars) {  // (to the nearest bar line)
            int bar = m->barAtBeat(beat);
            if (beat - m->barBeat(bar) > (m->barBeat(bar + 1) - m->barBeat(bar)) / 2) bar++;
            beat = m->barBeat(bar);
        }
        TempoRef to = placeAt(*m, beat);
        if (to.song() || (!(to == d->at) && t.changeAt(to.bar, to.beat))) to = d->at;  // (another point there: it stays)
        TempoChange moved = *c;
        t.removeChange(d->at.bar, d->at.beat);
        moved.bar = to.bar;
        moved.beat = to.beat;
        moved.bpm = bpm;
        if (!d->bars && to.beat == 0) moved.beatsPerBar = meterBefore(*m, m->beatOf(moved));  // (not a time signature)
        t.setChange(moved);
        d->at = to;
    }
    d->moved = true;
    timebase::follow(project, *d->before, engine.sr);  // (always from where the drag began: nothing drifts)
    if (status) status("Tempo at " + placeText(d->at) + ": " + g(bpm) + " BPM", 3000);
    relayout();
    update();
}

void Timeline::endTempoDrag() {
    auto* d = std::get_if<TempoDrag>(&drag);
    if (!d || !d->moved) return;
    util::debugf("timeline: tempo point dragged to %s\n", placeText(d->at).c_str());
    timingChanged.emit();
    update();
}
