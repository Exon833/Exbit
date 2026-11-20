// Edits made to many clips at once: slip, crop, fades (clip_edits.h).
#include "model/clip_edits.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "audio/workers.h"
#include "core/util.h"
#include "model/edits.h"

namespace clip_edits {

namespace {

using NotesPtr = std::shared_ptr<const std::vector<Note>>;

int64_t floorMod(int64_t a, int64_t n) { return ((a % n) + n) % n; }

bool frozen(const Clip& c) {
    TrackPtr t = std::atomic_load(&c.track);
    return t && t->frozen;
}

// notes moved `by` seconds (a looping clip's wrapped into its pass, 0…pass), kept in time order
NotesPtr shifted(const NotesPtr& ns, double by, double pass) {
    if (!ns || ns->empty()) return ns;
    auto out = std::make_shared<std::vector<Note>>(*ns);
    for (Note& n : *out) {
        double len = n.end - n.start;
        n.start += by;
        if (pass > 0) {
            n.start -= std::floor(n.start / pass) * pass;
            if (!(n.start < pass)) n.start = 0;  // (rounding)
        }
        n.end = n.start + len;
    }
    std::stable_sort(out->begin(), out->end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

}  // namespace

// clips prepared again (each to `reach` frames; -1 its window) on copies, on every core when there are many,
// then swapped in (the audio thread atomic_loads them)
void prepareAll(const std::vector<std::pair<ClipPtr, int64_t>>& todo, int sr) {
    if (todo.empty()) return;
    struct Work {
        const std::vector<std::pair<ClipPtr, int64_t>>* todo;
        std::vector<Clip> ready;
        int sr;
    } work{&todo, {}, sr};
    work.ready.reserve(todo.size());
    for (auto& [c, reach] : todo) {
        work.ready.push_back(*c);
        work.ready.back().track = std::atomic_load(&c->track);
    }
    auto job = [](void* ctx, int i) {
        Work& w = *static_cast<Work*>(ctx);
        w.ready[(size_t)i].prepare(w.sr, (*w.todo)[(size_t)i].second);
    };
    int threads = todo.size() >= 4 ? workers::workersFor(workers::chosenCores()) : 0;
    {
        workers::Pool pool(threads, false);  // (as a render's: never outranking playback)
        pool.run((int)todo.size(), job, &work);
    }
    for (size_t i = 0; i < todo.size(); i++) {
        Clip& r = work.ready[i];
        const ClipPtr& c = todo[i].first;
        c->baseFrames = r.baseFrames;
        c->loopFrom = r.loopFrom;
        c->loopFor = r.loopFor;
        std::atomic_store(&c->peaks, r.peaks);
        std::atomic_store(&c->played, r.played);
        std::atomic_store(&c->events, r.events);
        std::atomic_store(&c->data, r.data);
    }
    if (todo.size() >= 4) util::debugf("clip_edits: %zu clips prepared again on %d worker threads (+1)\n", todo.size(), threads);
}

// --- slip ---

std::vector<SlipStart> slipStart(const Project& p, const std::vector<ClipPtr>& clips) {
    std::vector<SlipStart> out;
    std::set<const ClipLink*> links;
    std::set<const Clip*> seen;
    for (const ClipPtr& c : clips) {
        if (!c || frozen(*c) || !seen.insert(c.get()).second) continue;
        if (c->kind == Kind::Midi && c->link && links::linked(p, *c) && !links.insert(c->link.get()).second) continue;
        SlipStart s;
        s.clip = c;
        s.offset = c->offset;
        s.length = std::max<int64_t>(1, c->frames());
        s.notes = c->notes;
        s.mutedNotes = c->mutedNotes;
        out.push_back(s);
    }
    return out;
}

void slipRoom(const SlipStart& s, int64_t& lo, int64_t& hi) {
    const Clip& c = *s.clip;
    if (c.kind == Kind::Midi || (c.loopingAudio() && c.loopFor > 0)) {
        lo = std::numeric_limits<int64_t>::min() / 4;
        hi = std::numeric_limits<int64_t>::max() / 4;
        return;
    }
    AudioPtr data = std::atomic_load(&c.data);
    int64_t have = data ? data->frames() : 0;
    hi = std::max<int64_t>(0, s.offset);                         // (the window's start at the audio's)
    lo = std::min<int64_t>(0, s.offset - (have - s.length));     // (its end at the audio's)
}

void slipFrom(Project& p, const std::vector<SlipStart>& from, int64_t frames, int sr) {
    std::vector<std::pair<ClipPtr, int64_t>> todo;  // to prepare again (reach)
    std::vector<std::pair<ClipPtr, int64_t>> offsets;  // looping audio built further: its window moved after
    for (const SlipStart& s : from) {
        Clip& c = *s.clip;
        if (c.kind == Kind::Midi) {
            double pass = c.loop && c.lengthS > 0 ? c.lengthS : 0;
            NotesPtr notes = shifted(s.notes, (double)frames / sr, pass);
            NotesPtr muted = shifted(s.mutedNotes, (double)frames / sr, pass);
            std::vector<ClipPtr> all{s.clip};
            if (c.link && links::linked(p, c)) all = links::partners(p, c);
            for (const ClipPtr& o : all) {
                if (o->length < 0) o->length = std::max<int64_t>(1, o->frames());  // (its window as it is now)
                if (o->notes == notes && o->mutedNotes == muted) continue;
                o->notes = notes;  // (the same immutable vectors for every linked clip)
                o->mutedNotes = muted;
                todo.push_back({o, -1});
            }
            continue;
        }
        int64_t lo, hi;
        slipRoom(s, lo, hi);
        int64_t f = std::clamp(frames, lo, hi);
        int64_t offset = s.offset - f;
        if (c.loopingAudio() && c.loopFor > 0) {  // the loop's phase: wrapped into the loop
            int64_t a = c.loopFrom, n = c.loopFor;
            if (f != 0) offset = a + floorMod(offset - a, n);
            AudioPtr data = std::atomic_load(&c.data);
            if (offset + s.length > (data ? data->frames() : 0)) {  // (its looped audio first, as far as it needs)
                todo.push_back({s.clip, offset + s.length + (int64_t)std::nearbyint(config::LOOP_DRAG_AHEAD_S * sr)});
                offsets.push_back({s.clip, offset});
                continue;
            }
        }
        if (c.length < 0) c.length = s.length;  // (the same window, written out; then where it starts)
        c.offset = offset;
    }
    prepareAll(todo, sr);
    for (auto& [c, offset] : offsets) {
        if (c->length < 0) c->length = std::max<int64_t>(1, c->frames());
        c->offset = offset;
    }
}

void slip(Project& p, const std::vector<ClipPtr>& clips, int64_t frames, int sr) {
    auto from = slipStart(p, clips);
    slipFrom(p, from, frames, sr);
    util::debugf("clip_edits::slip: %zu clips' contents moved %lld frames\n", from.size(), (long long)frames);
}

// --- crop ---

std::map<ClipPtr, ClipPtr> crop(Project& p, const std::vector<ClipPtr>& clips, int64_t start, int64_t end) {
    std::map<ClipPtr, ClipPtr> made;
    if (end <= start) return made;
    for (const ClipPtr& c : clips) {
        if (!c || made.count(c) || frozen(*c) || !p.clips.contains(c)) continue;
        if (c->start >= start && c->end() <= end) continue;  // (inside already)
        if (ClipPtr piece = edits::pieceWithin(c, start, end)) made[c] = piece;
    }
    if (made.empty()) return made;
    std::vector<ClipPtr> all;
    for (const ClipPtr& c : p.clips) {
        auto it = made.find(c);
        all.push_back(it != made.end() ? it->second : c);
    }
    p.clips = all;  // (all at once: the audio thread)
    util::debugf("clip_edits::crop: %zu clips cut to frames %lld…%lld\n", made.size(), (long long)start, (long long)end);
    return made;
}

// --- fades ---

int setFades(const std::vector<ClipPtr>& clips, const Fades& f) {
    int n = 0;
    for (const ClipPtr& c : clips) {
        if (!c || c->kind != Kind::Audio || frozen(*c)) continue;
        int64_t frames = c->frames();
        // (plain fields the mixer reads as it goes: changed in place, as dragging a fade handle does)
        c->fadeIn = std::clamp<int64_t>(f.in, 0, frames);
        c->fadeOut = std::clamp<int64_t>(f.out, 0, frames);
        c->fadeLead = c->fadeTail = 0;
        c->fadeInShape = f.inShape;
        c->fadeOutShape = f.outShape;
        n++;
    }
    util::debugf("clip_edits::setFades: %d audio clips, in %lld, out %lld frames\n", n, (long long)f.in, (long long)f.out);
    return n;
}

}  // namespace clip_edits
