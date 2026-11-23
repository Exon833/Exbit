// The timeline's time-signature lane (timeline.h, "the time-signature lane"): a labelled block per time signature
// under the tempo lane, from its bar to the next one's; added, moved by bars, typed and removed with the mouse. Its
// edits go through the tempo map (Timing::setSignature / removeSignature / moveSignature) as the tempo lane's do.
#include <algorithm>
#include <cmath>

#include "core/util.h"
#include "ui/lanes.h"
#include "ui/tempo_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/style.h"

using ui::style::box;

namespace {
// bar 1's signature, then each change at a bar's start that changes it (a tempo change at a bar in the same
// signature isn't a signature of its own)
std::vector<Meter> shown(const TempoMap& m) {
    std::vector<Meter> out;
    for (const Meter& me : m.meters())
        if (out.empty() || me.sig() != out.back().sig()) out.push_back(me);
    return out;
}

bool hasOwn(const TempoMap& m, int bar) {  // (1-based)
    for (const Meter& me : shown(m))
        if (me.bar + 1 == bar) return true;
    return false;
}
}  // namespace

bool Timeline::inSignatureLane(double y) const {
    int top = lanes::signatureLaneTop();
    return lanes::signatureLaneShown() && y >= top && y < top + lanes::signatureLaneHeight();
}

std::vector<Meter> Timeline::signatures() const { return shown(*project.timing.map()); }

int Timeline::signatureBarAt(double x) const { return project.timing.barAt(std::max(0.0, x / pps)) + 1; }

std::optional<int> Timeline::signatureAt(PointF pos) const {
    if (!inSignatureLane(pos.y)) return std::nullopt;
    TempoMapPtr m = project.timing.map();
    std::optional<int> found;
    double best = 1e18;
    for (const Meter& me : shown(*m)) {  // (its start: its first bar, or a few pixels around its line when narrow)
        double x0 = m->barSeconds(me.bar) * pps, x1 = std::max(m->barSeconds(me.bar + 1) * pps, x0 + config::AUTOMATION_GRAB_PX);
        if (pos.x < x0 - config::AUTOMATION_GRAB_PX || pos.x >= x1) continue;
        double d = std::fabs(pos.x - x0);
        if (d < best) best = d, found = me.bar + 1;
    }
    return found;
}

// --- painting ---------------------------------------------------------------------------------------

void Timeline::paintSignatureLane(Painter& p, const Rect& r, bool contents) {
    // (the tempo lane's look: an automation lane's box; each signature a TimeSignature box, labelled while it fits)
    if (!lanes::signatureLaneShown()) return;
    int top = lanes::signatureLaneTop(), h = lanes::signatureLaneHeight();
    Rect area = r.intersected(Rect(0, top, width(), h));
    if (area.empty()) return;
    const LaneLook& lk = look().lanes->get(LaneKind::Automation);
    if (!contents) {
        box(p, Rect(0, top, width(), h), lk.box);
        return;
    }
    const TimelineLook& tl = look();
    p.save();
    p.setClip(area);
    if (lk.topEdge.width > 0) p.fillRect(Rect(area.left(), top, area.w, lk.topEdge.width), lk.topEdge.color);
    TempoMapPtr m = project.timing.map();
    std::vector<Meter> sigs = shown(*m);
    const auto* d = std::get_if<SignatureDrag>(&drag);
    int dragged = d && !d->add ? d->to : -1;
    int viewLeft = scroll ? scroll->horizontalScrollBar()->value() : 0;
    int bt = top + tl.signatureMarginTop, bh = std::max(1, h - tl.signatureMarginTop - tl.signatureMarginBottom);
    const Font& font = tl.signatureFont;
    for (size_t i = 0; i < sigs.size(); i++) {
        double x0 = m->barSeconds(sigs[i].bar) * pps;
        double x1 = i + 1 < sigs.size() ? m->barSeconds(sigs[i + 1].bar) * pps : std::max<double>(width(), area.right() + 1);
        if (x1 < area.left() || x0 > area.right()) continue;
        int on = sigs[i].bar + 1 == dragged ? 1 : 0;
        // (kept to the view and a little more: a block can be millions of pixels wide zoomed in)
        int a = (int)std::floor(std::max(x0, area.left() - 16.0)), b = (int)std::ceil(std::min(x1, area.right() + 16.0));
        box(p, Rect(a, bt, std::max(1, b - a), bh), tl.signature[on]);
        // its label at its start, or at the view's left while its start is scrolled past (as long as it fits)
        std::string text = sigs[i].sig().text();  // ("7/8")
        float tw = font.width(text) + 2, lx = (float)std::max(x0, (double)viewLeft) + tl.signaturePadLeft;
        if (lx + tw > x1 - 2) lx = (float)x0 + tl.signaturePadLeft;
        if (lx + tw <= x1 - 2 || i + 1 == sigs.size())
            p.drawText(RectF(lx, (float)bt, tw, (float)bh), AlignLeft | AlignVCenter, text, font, tl.signatureText[on]);
    }
    p.restore();
}

// --- editing ----------------------------------------------------------------------------------------

bool Timeline::setSignature(int bar, TimeSig sig) {
    Timing& t = project.timing;
    bar = std::max(1, bar);
    sig = TimeSig{timing::clampedBeats(sig.num), timing::checkedUnit(sig.den, "the time-signature lane")};
    TempoMapPtr m = t.map();
    bool own = bar == 1 || hasOwn(*m, bar);
    if (bar > 1 && t.sigAtBar(bar - 1) == sig) {  // (the one in force there already)
        if (own) return removeSignature(bar);
        if (status) status("Bar " + std::to_string(bar) + " is in " + sig.text() + " already", 3000);
        return false;
    }
    if (own && t.sigAtBar(bar) == sig) return false;
    tempoEdit([&] { t.setSignature(bar, sig); });
    util::debugf("timeline: time signature %s from bar %d\n", sig.text().c_str(), bar);
    return true;
}

bool Timeline::removeSignature(int bar) {
    Timing& t = project.timing;
    if (bar <= 1 || !hasOwn(*t.map(), bar)) return false;
    tempoEdit([&] { t.removeSignature(bar); });
    util::debugf("timeline: the time signature at bar %d removed\n", bar);
    return true;
}

void Timeline::editSignature(int bar) {
    const Timing& t = project.timing;
    bar = std::max(1, bar);
    bool own = bar == 1 || hasOwn(*t.map(), bar);
    SignatureDialog dialog(dialogParent(), bar, t.sigAtBar(bar), own && bar > 1, t.timebaseBeats);
    if (!dialog.run()) return;
    if (dialog.removed) removeSignature(bar);
    else setSignature(bar, dialog.sig);
}

void Timeline::fillSignatureMenu(ui::Menu& menu, int bar) {
    const Timing& t = project.timing;
    TempoMapPtr m = t.map();
    int from = 1;  // the block there: the signature in force at `bar`
    for (const Meter& me : shown(*m))
        if (me.bar + 1 <= bar) from = me.bar + 1;
    std::string sig = t.sigAtBar(from).text();
    menu.addAction("Edit the time signature at bar " + std::to_string(from) + " (" + sig + ")…",
                   [this, from] { editSignature(from); });
    ui::Action* del = menu.addAction("Remove the time signature at bar " + std::to_string(from),
                                     [this, from] { removeSignature(from); });
    del->setEnabled(from > 1);
    if (from == 1) del->tooltip = "Bar 1's is the song's own: edit it instead";
    if (bar != from) menu.addAction("Add a time signature at bar " + std::to_string(bar) + "…", [this, bar] { editSignature(bar); });
}

// --- the mouse --------------------------------------------------------------------------------------

bool Timeline::signaturePress(ui::MouseEvent& e) {
    PointF pos(e.pos);
    Timing& t = project.timing;
    SignatureDrag d;
    d.changes = t.changes;
    d.map = t.map();
    d.before = std::make_shared<timebase::Before>(timebase::remember(project));
    if (std::optional<int> bar = signatureAt(pos)) {  // a block's start: dragged along the bars
        d.from = d.to = *bar;
    } else {  // an empty bar: a signature typed for it once let go
        d.add = true;
        d.from = d.to = signatureBarAt(pos.x);
    }
    drag = d;
    update();
    return true;
}

void Timeline::dragSignature(PointF pos) {
    auto* d = std::get_if<SignatureDrag>(&drag);
    if (!d) return;
    if (d->add || d->from < 2) {  // (an empty bar dragged across: nothing added; bar 1's: it stays)
        d->moved = d->moved || d->add;
        return;
    }
    const TempoMap& m = *d->map;  // (the bars as the drag began: the bar under the mouse doesn't jump as it moves)
    double beat = m.beatAt(std::max(0.0, pos.x / pps));
    int bar = m.barAtBeat(beat);
    if (beat - m.barBeat(bar) > (m.barBeat(bar + 1) - m.barBeat(bar)) / 2) bar++;  // (the nearest bar line)
    int to = std::max(2, bar + 1);
    if (to != d->from && hasOwn(m, to)) to = d->to;  // (another signature there: it stays where it is)
    if (to == d->to) return;
    Timing& t = project.timing;
    t.changes = d->changes;  // (from the changes as the drag began, each time)
    if (to != d->from) t.moveSignature(d->from, to);
    d->to = to;
    d->moved = true;
    timebase::follow(project, *d->before, engine.sr);
    if (status) status("Time signature " + t.sigAtBar(to).text() + " from bar " + std::to_string(to), 3000);
    relayout();
    update();
}

void Timeline::endSignatureDrag() {
    auto* d = std::get_if<SignatureDrag>(&drag);
    if (!d) return;
    SignatureDrag done = *d;
    drag = std::monostate();
    update();
    if (done.add) {
        if (!done.moved) editSignature(done.from);  // (a click: the dialog, with the one in force there)
        return;
    }
    if (!done.moved) return;
    util::debugf("timeline: the time signature at bar %d dragged to bar %d\n", done.from, done.to);
    timingChanged.emit();
}
