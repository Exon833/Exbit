// timeline.py's successor, gain envelopes (model/clip_envelope.h): an audio clip's volume curve, drawn over its
// waveform while shown (right-click → Gain envelope) and edited there like an automation lane: click to add a
// point (dragged at once), drag points, Ctrl+drag a segment to bend it, right-click or double-click a point to
// remove it; Gain envelope points… lists them (gain_envelope_dialog.h).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/util.h"
#include "model/clip_envelope.h"
#include "ui/gain_envelope_dialog.h"
#include "ui/timeline.h"

void Timeline::showEnvelopes(const std::vector<ClipPtr>& clips, bool on) {
    for (auto& c : clips)
        if (c->kind == Kind::Audio) c->envelopeShown = on;
    util::debugf("gain envelope: %s on %zu clip(s)\n", on ? "shown" : "hidden", clips.size());
    update();
}

void Timeline::removeEnvelopes(const std::vector<ClipPtr>& clips) {
    for (auto& c : clips) {
        std::atomic_store(&c->gainEnvelope, clip_envelope::Points());
        c->envelopeShown = false;
    }
    util::debugf("gain envelope: removed from %zu clip(s)\n", clips.size());
    update();
}

void Timeline::setEnvelope(const ClipPtr& clip, std::vector<AutoPoint> points) {
    std::atomic_store(&clip->gainEnvelope, clip_envelope::make(std::move(points)));  // (the audio thread reads it)
    update();
}

void Timeline::editEnvelopePoints(const ClipPtr& clip) {
    if (!clip || clip->kind != Kind::Audio) return;
    GainEnvelopeDialog dialog(dialogParent(), *clip, engine.sr);
    if (!dialog.run()) return;
    setEnvelope(clip, dialog.points);
    clip->envelopeShown = clip->gainEnvelope != nullptr;
    util::debugf("gain envelope: %zu point(s) on \"%s\"\n", dialog.points.size(), clip->name.c_str());
}

std::optional<RectF> Timeline::envelopeBody(const Clip& clip) const {
    auto rect = clipRect(clip, trackTops());
    if (!rect) return std::nullopt;
    return rect->adjusted(0, (float)clip_painter::headerHeight(), 0, 0);
}

ClipPtr Timeline::envelopeClipAt(PointF pos) const {
    auto [clip, zone] = hit(pos);  // (never a frozen track's)
    return clip && zone == ClipZone::Body && clip->kind == Kind::Audio && clip->envelopeShown && !clip->disabled ? clip
                                                                                                                 : nullptr;
}

int Timeline::envelopePointAt(const ClipPtr& clip, PointF pos, int64_t* frame) const {
    auto body = envelopeBody(*clip);
    auto envelope = clip->gainEnvelope;
    if (!body || !envelope) return -1;
    const double grab = clip_painter::ENVELOPE_DOT;
    auto frameAt = [&](double x) { return clip->offset + (int64_t)std::nearbyint((x - body->x) / pps * engine.sr); };
    for (size_t i = 0; i < envelope->size(); ++i) {
        const AutoPoint& p = (*envelope)[i];
        if (std::fabs(clip_painter::envelopeY(*body, p.v) - pos.y) > grab) continue;
        for (int64_t f : clip_envelope::framesOf(*clip, p.t, frameAt(pos.x - grab), frameAt(pos.x + grab) + 1)) {
            if (frame) *frame = f;
            return (int)i;
        }
    }
    return -1;
}

int Timeline::envelopeSegmentAt(const ClipPtr& clip, double x) const {
    auto body = envelopeBody(*clip);
    auto envelope = clip->gainEnvelope;
    if (!body || !envelope) return -1;
    double t = (double)clip_envelope::audioFrame(*clip, clip->offset + (int64_t)std::nearbyint((x - body->x) / pps * engine.sr));
    for (size_t i = 0; i + 1 < envelope->size(); ++i)
        if ((*envelope)[i].t <= t && t < (*envelope)[i + 1].t) return (int)i;
    return -1;
}

bool Timeline::envelopePress(ui::MouseEvent& e, const ClipPtr& clip) {
    // grab the point there, or bend the segment there (Ctrl), or add a point there (and grab it)
    auto body = envelopeBody(*clip);
    if (!body || clip->track->frozen) return false;
    if (!selection.contains(clip)) selection.selectOnly(clip);
    PointF pos(e.pos);
    int64_t frame = 0;
    int index = envelopePointAt(clip, pos, &frame);
    if (index < 0 && (e.mods & plat::ModCtrl)) {
        int segment = envelopeSegmentAt(clip, pos.x);
        if (segment >= 0) {
            drag = EnvelopeBendDrag{clip, segment, pos.y, (*clip->gainEnvelope)[(size_t)segment].bend};
            return true;
        }
    }
    if (index < 0) {
        frame = clip->offset + snappedFrameAt(pos.x, e.mods) - clip->start;
        frame = std::max(clip->offset, std::min(clip->offset + clip->frames() - 1, frame));
        double t = (double)clip_envelope::audioFrame(*clip, frame), v = clip_painter::envelopeValue(*body, pos.y);
        std::vector<AutoPoint> points = clip->gainEnvelope ? *clip->gainEnvelope : std::vector<AutoPoint>{};
        auto at = std::upper_bound(points.begin(), points.end(), t, [](double s, const AutoPoint& p) { return s < p.t; });
        index = (int)(at - points.begin());
        points.insert(at, AutoPoint{t, v, 0.0});
        setEnvelope(clip, points);
        util::debugf("gain envelope: a point at frame %.0f, %s\n", t, clip_envelope::dbText(v).c_str());
    }
    drag = EnvelopeDrag{clip, index, frame, (*clip->gainEnvelope)[(size_t)index].t};
    ui::App::get().showToolTip(e.globalPos, "Gain: " + clip_envelope::dbText((*clip->gainEnvelope)[(size_t)index].v), this);
    return true;
}

void Timeline::dragEnvelope(PointF pos, Point globalPos, int mods) {
    // the grabbed point: in time by as much as the mouse moved (between its neighbours), and in gain
    auto& d = std::get<EnvelopeDrag>(drag);
    auto body = envelopeBody(*d.clip);
    if (!body || !d.clip->gainEnvelope) return;
    std::vector<AutoPoint> points = *d.clip->gainEnvelope;
    if (d.index < 0 || d.index >= (int)points.size()) return;
    int64_t frame = d.clip->offset + snappedFrameAt(pos.x, mods) - d.clip->start;
    double t = std::max(0.0, d.t + (double)(frame - d.frame));
    if (d.index > 0) t = std::max(t, points[(size_t)d.index - 1].t);
    if (d.index + 1 < (int)points.size()) t = std::min(t, points[(size_t)d.index + 1].t);
    points[(size_t)d.index].t = t;
    points[(size_t)d.index].v = clip_painter::envelopeValue(*body, pos.y);
    setEnvelope(d.clip, points);  // (still in order: it stays between its neighbours)
    ui::App::get().showToolTip(globalPos, "Gain: " + clip_envelope::dbText(points[(size_t)d.index].v), this);
}

void Timeline::dragEnvelopeBend(PointF pos, Point globalPos) {
    // up bows the segment up, down bows it down: half the body's height for a full bend
    auto& d = std::get<EnvelopeBendDrag>(drag);
    auto body = envelopeBody(*d.clip);
    if (!body || !d.clip->gainEnvelope) return;
    std::vector<AutoPoint> points = *d.clip->gainEnvelope;
    if (d.index < 0 || d.index + 1 >= (int)points.size()) return;
    double bend = std::max(-1.0, std::min(1.0, d.bend - 2 * (pos.y - d.y) / std::max(1.0f, body->h)));
    points[(size_t)d.index].bend = bend;
    setEnvelope(d.clip, points);
    char text[48];
    std::snprintf(text, sizeof text, "Curve: %+.0f %%", bend * 100);
    ui::App::get().showToolTip(globalPos, text, this);
}
