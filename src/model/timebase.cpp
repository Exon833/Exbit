// The song's timebase: what follows a change of the tempo map (timebase.h).
#include "model/timebase.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/util.h"
#include "model/clip_edits.h"

namespace timebase {

std::function<void(Project& project, const std::vector<ClipPtr>& moved, int sr)> retimed;

Before remember(const Project& p) {
    Before b;
    if (!p.timing.timebaseBeats) return b;
    b.map = p.timing.map();
    for (auto& c : p.clips) b.clips.push_back({c, c->start, c->offset, c->length, c->lengthS, c->notes, c->mutedNotes});
    for (auto& t : p.allTracks()) {
        Before::TakeAt at{t, {}};
        for (auto& k : t->takes) at.starts.push_back(k->start);
        at.starts.push_back(t->frozen ? t->frozen->start : -1);
        b.takes.push_back(std::move(at));
        for (auto& a : t->automation) b.automation.push_back({a, a->points.copy()});
    }
    b.markers = p.markers.copy();
    b.loopStart = p.timing.loopStart;
    b.loopEnd = p.timing.loopEnd;
    return b;
}

namespace {
bool sameMap(const TempoMap& a, const TempoMap& b) {
    return a.bpm == b.bpm && a.beatsPerBar == b.beatsPerBar && a.beatUnit == b.beatUnit && a.ramp == b.ramp &&
           a.changes == b.changes;
}
}  // namespace

std::vector<ClipPtr> follow(Project& p, const Before& before, int sr) {
    std::vector<ClipPtr> moved;
    if (before.empty() || !p.timing.timebaseBeats || sr <= 0) return moved;
    TempoMapPtr now = p.timing.map();
    const TempoMap& was = *before.map;
    if (sameMap(was, *now)) return moved;
    // (seconds on the old map → the same beat on the new one)
    auto at = [&](double s) { return now->secondsAt(was.beatAt(s)); };
    auto frame = [&](int64_t f) { return (int64_t)std::llround(at((double)f / sr) * sr); };
    using NotesPtr = std::shared_ptr<const std::vector<Note>>;
    // linked clips share their notes: worked out once per link, from the first clip of it (the same beats
    // after its origin for every one of them, as they share their notes in seconds now)
    std::map<std::pair<const void*, const void*>, NotesPtr> done;
    std::vector<std::pair<ClipPtr, int64_t>> prepare;
    size_t notes = 0;
    for (auto& c : before.clips) {
        Clip& k = *c.clip;
        if (!p.clips.contains(c.clip)) continue;
        int64_t start = frame(c.start);
        bool changed = start != k.start;
        if (k.kind == Kind::Midi) {
            double origin = (double)(c.start - c.offset) / sr;
            // (the notes from the new origin as frames place it: rounding the clip's start doesn't move them)
            int64_t offset = std::max<int64_t>(0, start - (int64_t)std::llround(at(origin) * sr));
            double originNow = (double)(start - offset) / sr;
            auto convert = [&](const NotesPtr& ns) -> NotesPtr {
                if (!ns || ns->empty()) return ns;
                // (copies that aren't linked share their notes too, each at its own place: worked out apart)
                auto key = std::make_pair((const void*)ns.get(), (const void*)c.clip->link.get());
                auto found = c.clip->link ? done.find(key) : done.end();
                if (found != done.end()) return found->second;
                auto out = std::make_shared<std::vector<Note>>(*ns);
                for (Note& n : *out) {
                    n.start = at(origin + n.start) - originNow;
                    n.end = std::max(n.start, at(origin + n.end) - originNow);
                }
                notes += out->size();
                done[key] = out;
                return out;
            };
            NotesPtr ns = convert(c.notes), muted = convert(c.mutedNotes);
            double lengthS = c.lengthS > 0 ? std::max(0.0, at(origin + c.lengthS) - originNow) : c.lengthS;
            int64_t length = c.length;
            if (c.length >= 0) length = std::max<int64_t>(1, frame(c.start + c.length) - start);
            changed = changed || ns != k.notes || muted != k.mutedNotes || lengthS != k.lengthS ||
                      offset != k.offset || length != k.length;
            k.notes = ns;
            k.mutedNotes = muted;
            k.lengthS = lengthS;
            k.offset = offset;
            k.length = length;
            k.start = start;
            if (changed) prepare.push_back({c.clip, -1});
        } else {
            k.start = start;  // (audio plays at its own speed: only where it starts follows the beat)
        }
        if (changed) moved.push_back(c.clip);
    }
    clip_edits::prepareAll(prepare, sr);
    for (auto& t : before.takes) {
        std::vector<ClipPtr> takes;
        size_t i = 0;
        for (auto& k : t.track->takes) {
            ClipPtr m = k;
            if (i < t.starts.size() - 1) {  // (takes are replaced, not changed: the history keeps them shared)
                m = copyClip(k);
                m->start = frame(t.starts[i]);
            }
            takes.push_back(m);
            i++;
        }
        if (!takes.empty()) t.track->takes = takes;
        if (t.track->frozen && t.starts.back() >= 0) t.track->frozen->start = frame(t.starts.back());
    }
    size_t points = 0;
    for (auto& [lane, pts] : before.automation) {
        std::vector<AutoPoint> out = pts;
        for (auto& pt : out) pt.t = std::max(0.0, at(pt.t));
        points += out.size();
        lane->setPoints(std::move(out));
    }
    std::vector<Marker> markers = before.markers;
    for (auto& m : markers) m.seconds = std::max(0.0, at(m.seconds));
    p.setMarkers(std::move(markers));
    if (before.loopEnd > before.loopStart) {
        p.timing.loopStart = std::max(0.0, at(before.loopStart));
        p.timing.loopEnd = std::max(p.timing.loopStart, at(before.loopEnd));
    }
    util::debugf("timebase: the song follows the tempo map in beats: %zu clips moved (%zu MIDI prepared again, "
                 "%zu notes), %zu automation points, %zu markers\n",
                 moved.size(), prepare.size(), notes, points, before.markers.size());
    if (retimed && !moved.empty()) retimed(p, moved, sr);
    return moved;
}

}  // namespace timebase
