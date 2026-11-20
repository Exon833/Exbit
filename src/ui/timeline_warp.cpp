// timeline.py's successor, warping (model/warping.h, audio/warp.h): Warp… and Warp from here (the clip menu), and a
// warped clip's warp markers on the timeline (clip_painter.h paintWarp: in a lane taller than a strip): drag a
// marker's handle along (snapping to the grid; the audio follows at once for a short clip or Beats, else when let
// go), double-click a transient's tick to make it a marker, right-click a marker to remove it.
#include <algorithm>
#include <cmath>
#include <set>

#include "core/util.h"
#include "model/edits.h"
#include "model/warping.h"
#include "ui/clip_painter.h"
#include "ui/timeline.h"
#include "ui/warp_dialog.h"

namespace {
constexpr double LIVE_DRAG_S = 30;  // a clip's audio at most this long is warped again at every move of a drag
constexpr float EDGE_PX = 6;        // markers this close to a clip's ends are left to its fade handles and ends
}

void Timeline::warpSelected(int64_t at) {
    std::vector<ClipPtr> clips;  // (in the project's order: the first one's settings are shown)
    for (auto& c : project.clips)
        if (c->kind == Kind::Audio && selection.contains(c) && !c->track->frozen) clips.push_back(c);
    if (clips.empty()) return;
    int sr = engine.sr;
    int64_t here = at >= 0 ? at : cursor;
    WarpDialog dialog(dialogParent(), *clips[0], project.timing.map(), (int)clips.size(), sr, here, gridStepAt(here, true));
    if (!dialog.run()) return;
    std::map<ClipPtr, ClipPtr> made;
    std::vector<ClipPtr> news;
    for (auto& c : clips) {
        ClipPtr n;
        if (dialog.warp) {
            // the markers edited in the dialog (one clip's); else its own, while its tempo stays (else straight at it)
            auto markers = clips.size() == 1 ? dialog.markers : nullptr;
            if (!markers && c->warp && c->sourceBpm == dialog.sourceBpm) markers = c->warpMarkers;
            n = warping::rewarped(c, true, dialog.mode, dialog.preserve, dialog.sourceBpm, markers, dialog.pitch, sr);
        } else {
            n = c->warp ? warping::rewarped(c, false, c->warpMode, c->warpPreserve, c->sourceBpm, c->warpMarkers, c->pitch, sr) : c;
            n = edits::restretched(n, dialog.stretch, dialog.pitch, sr);
            n->sourceBpm = dialog.sourceBpm;
        }
        made[c] = n;
        news.push_back(n);
    }
    std::vector<ClipPtr> all;
    for (auto& c : project.clips) all.push_back(made.count(c) ? made[c] : c);
    project.clips = all;  // (all at once: the audio thread)
    edits::clearUnder(project, news);
    std::set<ClipPtr> kept;
    for (auto& c : selection.clips) {
        ClipPtr n = made.count(c) ? made[c] : c;
        if (project.clips.contains(n)) kept.insert(n);
    }
    selection.clips = kept;
    util::debugf("timeline: Warp… on %zu clip(s): %s\n", clips.size(),
                 dialog.warp ? warp::modeName(dialog.mode) : "not warped");
    relayout();
}

void Timeline::warpFromHere(const ClipPtr& clip, int64_t frame) {
    if (!clip || !clip->warp || clip->track->frozen) return;
    warping::setMarkers(clip, warping::fromHere(*clip, frame, *project.timing.map(), engine.sr), engine.sr);
    if (status) status("Warp from here: a marker on the nearest bar", 3000);
    relayout();
}

ClipPtr Timeline::warpClipAt(PointF pos) const {
    auto [clip, zone] = hit(pos);  // (never a frozen track's)
    if (!clip || zone != ClipZone::Body || clip->disabled) return nullptr;
    auto body = envelopeBody(*clip);
    return body && clip_painter::warpShown(*clip, *body) ? clip : nullptr;
}

int Timeline::warpMarkerAt(const ClipPtr& clip, PointF pos) const {
    auto body = envelopeBody(*clip);
    if (!body || pos.y > body->y + clip_painter::WARP_HANDLE + 3) return -1;
    int best = -1;
    double nearest = clip_painter::WARP_HANDLE / 2 + 2;
    for (auto& [i, x] : clip_painter::warpMarkerXs(*clip, *body, pps, engine.sr)) {
        if (x < body->x + EDGE_PX || x > body->right() - EDGE_PX) continue;  // (there the clip's fades and ends)
        if (std::fabs(pos.x - x) <= nearest) nearest = std::fabs(pos.x - x), best = i;
    }
    return best;
}

int64_t Timeline::transientAt(const ClipPtr& clip, PointF pos) const {
    auto body = envelopeBody(*clip);
    if (!body || pos.y > body->y + std::max(clip_painter::WARP_TICK, clip_painter::WARP_HANDLE) + 3) return -1;
    int64_t best = -1;
    double nearest = 4;
    for (auto& [f, x] : clip_painter::transientXs(*clip, *body, pps, engine.sr))
        if (std::fabs(pos.x - x) <= nearest) nearest = std::fabs(pos.x - x), best = f;
    return best;
}

bool Timeline::warpPress(ui::MouseEvent& e) {
    PointF pos(e.pos);
    if (e.button != plat::LeftButton) return false;
    ClipPtr clip = warpClipAt(pos);
    int index = clip ? warpMarkerAt(clip, pos) : -1;
    if (index < 0) return false;
    if (!selection.contains(clip)) selection.selectOnly(clip);
    WarpDrag d{clip, index, warping::markersOf(*clip), warping::beatAtFrame(*clip, frameAt(pos.x), engine.sr), 0};
    d.beat = d.markers[(size_t)index].beat;
    drag = d;
    ui::App::get().showToolTip(e.globalPos, util::format("Warp marker: beat %.3f", d.beat), this);
    return true;
}

void Timeline::dragWarpMarker(PointF pos, int mods) {
    auto& d = std::get<WarpDrag>(drag);
    if (d.index < 0 || d.index >= (int)d.markers.size()) return;
    // where the mouse is (snapped to the grid), as a beat of the clip; between its neighbours
    double beat = warping::beatAtFrame(*d.clip, snappedFrameAt(pos.x, mods), engine.sr);
    const double gap = 1e-3;
    if (d.index > 0) beat = std::max(beat, d.markers[(size_t)d.index - 1].beat + gap);
    if (d.index + 1 < (int)d.markers.size()) beat = std::min(beat, d.markers[(size_t)d.index + 1].beat - gap);
    if (beat == d.markers[(size_t)d.index].beat) return;
    d.markers[(size_t)d.index].beat = beat;
    double seconds = d.clip->source ? (double)d.clip->source->frames() / std::max(1, d.clip->sourceSr) : 0;
    if (seconds <= LIVE_DRAG_S || d.clip->warpMode == warp::Mode::Beats) warping::setMarkers(d.clip, d.markers, engine.sr);
    if (status) status(util::format("Warp marker: beat %.3f", beat), 0);
    update();
}

void Timeline::releaseWarpMarker() {
    auto& d = std::get<WarpDrag>(drag);
    if (warping::markersOf(*d.clip) != d.markers) warping::setMarkers(d.clip, d.markers, engine.sr);
    util::debugf("timeline: warp marker %d of \"%s\" let go on beat %.3f\n", d.index, d.clip->name.c_str(),
                 d.markers[(size_t)d.index].beat);
    if (status) status("", 0);
    relayout();
}

bool Timeline::warpDoubleClick(PointF pos) {
    ClipPtr clip = warpClipAt(pos);
    int64_t f = clip ? transientAt(clip, pos) : -1;
    if (f < 0 || warpMarkerAt(clip, pos) >= 0) return false;
    warp::Markers m = warping::markersOf(*clip);
    warp::BeatMap beats(m, warping::bpmOf(*clip), clip->sourceSr);
    m.push_back({(double)f, beats.beatAt((double)f)});  // (where it plays now: nothing moves until it is dragged)
    warping::setMarkers(clip, m, engine.sr);
    util::debugf("timeline: a warp marker at the transient at source frame %lld\n", (long long)f);
    update();
    return true;
}

bool Timeline::warpRightClick(PointF pos) {
    ClipPtr clip = warpClipAt(pos);
    int index = clip ? warpMarkerAt(clip, pos) : -1;
    if (index < 0) return false;
    warp::Markers m = warping::markersOf(*clip);
    if (m.size() <= 1) return true;
    m.erase(m.begin() + index);
    warping::setMarkers(clip, m, engine.sr);
    util::debugf("timeline: warp marker %d removed\n", index);
    relayout();
    return true;
}
