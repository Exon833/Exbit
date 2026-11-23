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
TimeSig meterBefore(const TempoMap& m, double beat) { return m.meterAtBeat(std::max(0.0, beat - 1e-6)).sig(); }
// a change at bar `bar`, `beat` into it, in `sig`
TempoChange changeIn(int bar, double bpm, TimeSig sig, double beat, bool ramp) {
    return TempoChange{bar, bpm, sig.num, beat, ramp, sig.den};
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
    const Timing& t = project.timing;
    if (t.tempoLaneFixed()) {  // (the row's Min and Max: as they are now, even while dragging)
        lo = t.tempoLaneLo, hi = t.tempoLaneHi;
        return;
    }
    if (auto* d = std::get_if<TempoDrag>(&drag)) {  // (Auto: kept while dragging, the point stays under the mouse)
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

void Timeline::setTempoLaneRange(double lo, double hi) {
    Timing& t = project.timing;
    if (auto* d = std::get_if<TempoDrag>(&drag); d && !t.tempoLaneFixed())  // (Auto now, while dragging: from here on)
        tempoRange(d->lo, d->hi);
    t.tempoLaneLo = lo, t.tempoLaneHi = hi;
    bool fixed = t.tempoLaneFixed();
    if (!fixed && (lo != 0 || hi != 0)) {  // (Max not above Min, or outside MIN_BPM…MAX_BPM: Auto)
        util::debugf("timeline: the tempo lane's range %g…%g BPM isn't one: Auto\n", lo, hi);
        if (status) status("The tempo lane's Max must be above its Min (" + g(config::MIN_BPM) + "…" + g(config::MAX_BPM) +
                           " BPM): the range fits the tempo changes (Auto)", 5000);
    }
    if (!fixed) t.tempoLaneLo = t.tempoLaneHi = 0;
    if (auto* d = std::get_if<TempoDrag>(&drag); d && fixed) d->lo = lo, d->hi = hi;
    util::debugf("timeline: the tempo lane's range: %s\n", fixed ? (g(lo) + "…" + g(hi) + " BPM").c_str() : "Auto");
    tempoRangeChanged.emit();
    update();
}

void Timeline::editTempoLaneRange() {
    double lo, hi;
    tempoRange(lo, hi);
    TempoRangeDialog dialog(dialogParent(), lo, hi, !project.timing.tempoLaneFixed());
    if (!dialog.run()) return;
    if (dialog.automatic) setTempoLaneRange(0, 0);
    else setTempoLaneRange(dialog.lo, dialog.hi);
}

double Timeline::tempoPointY(double bpm) const {
    double lo, hi;
    tempoRange(lo, hi);
    return tempoY(std::min(hi, std::max(lo, bpm)));
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
        double dx = p.seconds * pps - pos.x, dy = tempoPointY(p.bpm) - pos.y;  // (outside the range: at its edge)
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
            line.push_back(
                PointF((float)(at * pps), (float)tempoPointY(s.bpmAfter(std::max(0.0, s.beatsFor(at - s.seconds))))));
        }
    }
    if (line.size() >= 2 && lk.curveWidth > 0) p.drawPolyline(line, lk.curve.color, lk.curveWidth);
    // the labels, never over one another: each on a plate (Lane.automation::value's box: the line passes under it),
    // placed in turn, each where it fits or left out: the time signature at each bar where it changes (at the lane's
    // bottom, when the signature lane is hidden), a point's tempo (above it, else below, else past what is there),
    // the range at the view's right (its Min and Max are in the row beside the lane too)
    const Font& font = lk.labelFont;
    const float fh = (float)font.height();
    std::vector<RectF> placed;
    auto apart = [](const RectF& a, const RectF& b) { return a.x >= b.right() + 3 || b.x >= a.right() + 3; };
    auto fits = [&](const RectF& b) {
        if (b.y < top || b.bottom() > top + h) return false;
        for (const RectF& q : placed)
            if (!apart(b, q) && b.y < q.bottom() && q.y < b.bottom()) return false;
        return true;
    };
    auto label = [&](const RectF& b, const std::string& text) {
        placed.push_back(b);
        box(p, b, lk.valueBox);
        p.drawText(b, AlignLeft | AlignTop, text, font, lk.labelColor);
    };
    for (size_t i = 0; i < segs.size() && !lanes::signatureLaneShown(); i++) {  // (the lane under it shows them)
        const Segment& s = segs[i];
        if (s.beat != s.barBeat || (i > 0 && segs[i - 1].sig() == s.sig())) continue;
        float x = (float)(s.seconds * pps);
        if (x < area.left() - 60 || x > area.right()) continue;
        std::string sig = s.sig().text();  // ("7/8")
        RectF b(x + 4, (float)(top + h) - fh - 2, (float)font.width(sig) + 2, fh);
        if (fits(b)) label(b, sig);
    }
    const std::vector<TempoPoint>& points = m->points();
    std::optional<TempoRef> dragged;
    if (auto* d = std::get_if<TempoDrag>(&drag)) dragged = d->at;
    std::vector<std::pair<size_t, float>> shown;  // (the points in view, and their y)
    for (size_t i = 0; i < points.size(); i++) {
        float x = (float)(points[i].seconds * pps), y = (float)tempoPointY(points[i].bpm);
        if (x < area.left() - 80 || x > area.right() + 4) continue;
        float grow = dragged && *dragged == refOf(*m, points[i]) ? 2.0f : 0.0f;
        p.fillRect(RectF(x - lk.pointW / 2.0f - grow, y - lk.pointH / 2.0f - grow, lk.pointW + 2 * grow, lk.pointH + 2 * grow),
                   lk.pointColor);
        if (points[i].bpm > hi || points[i].bpm < lo) {  // outside the range: drawn at its edge, a ▲ / ▼ beside it
            float cx = x - lk.pointW / 2.0f - grow - 6, tip = points[i].bpm > hi ? y - 4 : y + 4, base = 2 * y - tip;
            p.fillPolygon({PointF(cx - 4, base), PointF(cx, tip), PointF(cx + 4, base)}, lk.pointColor);
        }
        shown.push_back({i, y});
    }
    for (auto [i, y] : shown) {
        double room = i + 1 < points.size() ? (points[i + 1].seconds - points[i].seconds) * pps : 1e9;
        std::string text = g(std::nearbyint(points[i].bpm * 100) / 100) + (points[i].ramp ? " ↗" : "");
        float x = (float)(points[i].seconds * pps), w = (float)font.width(text) + 2;
        const float ys[2] = {y - lk.pointH / 2.0f - 2 - fh, y + lk.pointH / 2.0f + 2};
        for (float lx = x + 4; lx + w + 8 <= x + room;) {  // (blocked at both heights: tried again past what blocks it)
            float past = lx;
            bool done = false;
            for (float ly : ys) {
                RectF b(lx, ly, w, fh);
                if (fits(b)) {
                    label(b, text);
                    done = true;
                    break;
                }
                for (const RectF& q : placed)
                    if (!apart(b, q)) past = std::max(past, q.right() + 4);
            }
            if (done || past <= lx) break;
            lx = past;
        }
    }
    float right = (float)(scroll->horizontalScrollBar()->value() + scroll->viewport()->width() - lk.labelPadLeft);
    std::string high = g(hi) + " BPM", low = g(lo) + " BPM";
    RectF hb(right - font.width(high) - 2, (float)yTop + 1, font.width(high) + 2, fh);
    RectF lb(right - font.width(low) - 2, (float)yBottom - 1 - fh, font.width(low) + 2, fh);
    if (fits(hb)) label(hb, high);
    if (fits(lb)) label(lb, low);
    tempoLabelsShown = std::move(placed);
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
    TimeSig before = at.song() ? t.sig() : meterBefore(*m, beat);
    TimeSig sig = c ? c->sig() : before;
    bool ramp = c ? c->ramp : at.song() && t.ramp;  // (a new change: a step)
    bool canRamp = !m->points().empty() && m->points().back().beat > beat + 1e-9;
    TempoDialog dialog(dialogParent(), at.bar, bpm, sig, c != nullptr, at.beat, ramp, canRamp, t.timebaseBeats);
    if (!dialog.run()) return;
    tempoEdit([&] {
        if (at.song()) {
            t.bpm = dialog.bpm;
            t.beatsPerBar = dialog.sig().num;
            t.beatUnit = dialog.sig().den;
            t.ramp = dialog.ramp;
        } else if (dialog.removed) {
            t.removeChange(at.bar, at.beat);
        } else {
            t.setChange(changeIn(at.bar, dialog.bpm, at.beat == 0 ? dialog.sig() : before, at.beat, dialog.ramp));
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
    double lo, hi;
    tempoRange(lo, hi);
    menu.addAction("The lane's range (" + g(lo) + "…" + g(hi) + " BPM" + (t.tempoLaneFixed() ? "" : ", Auto") + ")…",
                   [this] { editTempoLaneRange(); })
        ->tooltip = "The tempo at the lane's bottom and top, or Auto (fitted to the tempo changes)";
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
            t.setChange(changeIn(at.bar, tempoBpmAt(pos.y), meterBefore(*m, beat), at.beat, rampingAt(*m, beat)));
            timebase::follow(project, *d.before, engine.sr);
            d.moved = true;
            util::debugf("timeline: tempo point added at %s, %g BPM\n", placeText(at).c_str(), tempoBpmAt(pos.y));
        }
        point = at;
        m = t.map();
    }
    d.at = *point;
    if (const TempoChange* c = d.at.song() ? nullptr : t.changeAt(d.at.bar, d.at.beat))  // (a time signature: by bars)
        d.bars = c->atBar() && c->sig() != meterBefore(*m, m->beatOf(*c));
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
        if (!d->bars && to.beat == 0) {  // (not a time signature)
            TimeSig before = meterBefore(*m, m->beatOf(moved));
            moved.beatsPerBar = before.num, moved.beatUnit = before.den;
        }
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
