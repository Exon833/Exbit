#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <sched.h>
#include <stdexcept>

#include "audio/automation.h"
#include "config.h"

using namespace plugin_audio;

namespace mixer {

void MixState::reserve(int frames, int tracks) {
    if (frames > capacity) {
        capacity = frames;
        master.assign((size_t)frames * 2, 0.0f);
        for (auto& f : feedBuf) f.assign((size_t)frames * 2, 0.0f);
        for (auto& f : sentBuf) f.assign((size_t)frames * 2, 0.0f);
        for (auto& f : trackBuf) f.assign((size_t)frames * 2, 0.0f);
    }
    if (tracks > trackCapacity) {
        trackCapacity = tracks;
        latency.reserve(tracks);
        order.reserve(tracks);
        feeder.reserve(tracks);
        visit.reserve(tracks);
        listens.reserve((size_t)tracks * tracks);
        cables.reserve(tracks);
        feeds.reserve(tracks);
        delays.reserve(tracks);
        held.reserve(tracks);
        mixOrder.reserve(tracks);
        sentUsed.reserve(tracks);
        results.reserve(tracks);
        clipFrom.reserve((size_t)tracks + 1);
        lines.reserve(tracks);
        heldOf.reserve(tracks);
        position.reserve(tracks);
        jobs.reserve(tracks);
        while ((int)feedBuf.size() < tracks) feedBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)sentBuf.size() < tracks) sentBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)trackBuf.size() < tracks) trackBuf.emplace_back((size_t)capacity * 2, 0.0f);
        if (finishedSize < tracks) {
            finished.reset(new std::atomic<char>[(size_t)tracks]);
            finishedSize = tracks;
        }
    }
    // one scratch for the calling thread and one for each worker
    size_t threads = 1 + (pool ? (size_t)pool->threads() : 0);
    while (scratch.size() < threads) {
        auto s = std::make_unique<Scratch>();
        s->messages.reserve(2048);
        s->events.reserve(2048);
        scratch.push_back(std::move(s));
    }
    for (auto& s : scratch) s->feeds.reserve(std::max(trackCapacity, 1));
    if (active.capacity() < 1024) active.reserve(1024);
    if (trackClips.capacity() < 1024) trackClips.reserve(1024);
}

bool listensTo(const std::vector<Cable>& cables, const Track* track) {
    for (auto& c : cables)
        if (c.first.type == End::TrackSource && c.first.track == track) return true;
    return false;
}

static void visitTrack(MixState& st, size_t n, size_t i) {
    st.visit[i] = 1;
    for (size_t j = 0; j < n; j++)  // (in their own order)
        if (st.listens[i * n + j] && !st.visit[j]) visitTrack(st, n, j);
    st.visit[i] = 2;
    st.order.push_back((int)i);
}

// (st.cables: the tracks' cables, snapshots taken for this buffer)
static void orderTracks(const TrackList& tracks, const std::vector<Cable>& masterCables, MixState& st) {
    size_t n = tracks.size();
    st.listens.assign(n * n, 0);
    st.feeder.assign(n, 0);
    st.order.clear();
    bool any = false;
    for (size_t i = 0; i < n; i++)
        for (auto& c : *st.cables[i]) {
            if (c.first.type != End::TrackSource) continue;
            for (size_t j = 0; j < n; j++)
                if (tracks[j].get() == c.first.track && tracks[j]->kind != Kind::Return) {  // (returns are last)
                    st.listens[i * n + j] = 1;
                    st.feeder[j] = 1;
                    any = true;
                }
        }
    for (auto& c : masterCables)
        if (c.first.type == End::TrackSource)
            for (size_t j = 0; j < n; j++)
                if (tracks[j].get() == c.first.track) st.feeder[j] = 1;
    if (!any) {
        for (size_t i = 0; i < n; i++) st.order.push_back((int)i);
    } else {
        st.visit.assign(n, 0);
        for (size_t i = 0; i < n; i++)
            if (!st.visit[i]) visitTrack(st, n, i);
    }
    st.mixOrder.clear();  // (the returns last)
    for (int i : st.order)
        if (tracks[i]->kind != Kind::Return) st.mixOrder.push_back(i);
    for (int i : st.order)
        if (tracks[i]->kind == Kind::Return) st.mixOrder.push_back(i);
    st.order.swap(st.mixOrder);
}

void addClip(float* const buf[2], int channels, int64_t bufFrames, const Clip& clip, int64_t start) {
    AudioPtr data = aload(clip.data);
    if (!data) return;
    int64_t cs = clip.start, frames = clip.frames();
    int64_t a = std::max(start, cs), b = std::min(start + bufFrames, cs + frames);
    if (a >= b) return;
    int64_t i0 = a - cs, i1 = b - cs;  // positions inside the clip window
    i1 = std::min(i1, data->frames() - clip.offset);  // (never past its data)
    if (i1 <= i0) return;
    int ch = std::min(channels, 2);
    const int fade = config::EDGE_FADE_FRAMES;
    const float* src = data->frame(clip.offset);
    bool edges = i0 < fade || i1 > frames - fade;  // short fade at the clip's edges, so cuts don't click
    bool levels = clip.hasLevels();  // its gain and fades (before the track's effects)
    int64_t fi = 0, fo = 0;
    if (levels) clip.fades(fi, fo);
    float gain = levels ? (float)std::pow(10.0, clip.gainDb / 20) : 1.0f;
    int64_t lead = std::max<int64_t>(0, clip.fadeLead), tail = std::max<int64_t>(0, clip.fadeTail);
    float* o0 = buf[0] + (a - start);
    float* o1 = ch > 1 ? buf[1] + (a - start) : nullptr;
    if (!edges && !levels) {
        for (int64_t pos = i0; pos < i1; pos++) {
            const float* f = src + pos * 2;
            o0[pos - i0] += f[0];
            if (o1) o1[pos - i0] += f[1];
        }
        return;
    }
    for (int64_t pos = i0; pos < i1; pos++) {
        float level = 1.0f;
        if (levels) {
            level = gain;
            if (fi) level *= (float)std::min(1.0, std::max(0.0, (double)(pos + lead + 1) / fi));
            if (fo) level *= (float)std::min(1.0, std::max(0.0, (double)(frames + tail - pos) / fo));
        }
        if (pos < fade || pos > frames - fade)
            level *= (float)std::min(1.0, (double)std::min(pos + 1, frames - pos) / fade);
        const float* f = src + pos * 2;
        o0[pos - i0] += f[0] * level;
        if (o1) o1[pos - i0] += f[1] * level;
    }
}

int64_t delayFrames(const Track& track, int sr) { return (int64_t)std::nearbyint(track.delayMs * sr / 1000.0); }

void panGains(double pan, double& left, double& right) {
    const double PI = 3.14159265358979323846;
    left = pan > 0 ? std::cos(pan * PI / 2) : 1.0;
    right = pan < 0 ? std::cos(pan * PI / 2) : 1.0;
}

void applyFader(float* const signal[2], int channels, int frames, const Track& track, bool hasStart, int64_t start,
                int sr) {
    double v0, p0, v1, p1;
    automation::mixerValues(track, hasStart, start, sr, v0, p0);
    automation::mixerValues(track, hasStart, start + frames, sr, v1, p1);
    auto gainOf = [](double db) { return dsp::dbToGain(db, config::MIN_DB); };
    if (v0 == v1 && p0 == p1) {  // (as usual: nothing moving)
        if (v0 != 0) {
            float k = (float)gainOf(v0);
            for (int c = 0; c < channels; c++)
                for (int i = 0; i < frames; i++) signal[c][i] *= k;
        }
        if (p0 != 0 && channels == 2) {
            double l, r;
            panGains(p0, l, r);
            for (int i = 0; i < frames; i++) {
                signal[0][i] = (float)(signal[0][i] * l);
                signal[1][i] = (float)(signal[1][i] * r);
            }
        }
        return;
    }
    double g0 = gainOf(v0), g1 = gainOf(v1), l0, r0, l1, r1;
    panGains(p0, l0, r0);
    panGains(p1, l1, r1);
    for (int i = 0; i < frames; i++) {
        float k = (float)(g0 + (g1 - g0) * i / frames);
        if (channels == 2) {
            signal[0][i] *= k * (float)(l0 + (l1 - l0) * i / frames);
            signal[1][i] *= k * (float)(r0 + (r1 - r0) * i / frames);
        } else {
            signal[0][i] *= k;
        }
    }
}

void feedingOrder(const TrackList& tracks, const std::vector<Cable>& masterCables, MixState& st) {
    st.cables.clear();
    for (auto& t : tracks) st.cables.push_back(t->cables.get());
    orderTracks(tracks, masterCables, st);
}

namespace {

// One buffer's mixing, shared by the threads working on it (see the top of mixer.h).
struct Buffer {
    const TrackList* tracks;
    MixState* st;
    float* out;
    int outChannels, frames, sr, latest;
    int64_t start;
    bool hasStart, restart, plugins;
    const live::LiveMap* live;
    const Track* only;
    TrackTap* tap;
    Transport transport;
    // the first error a worker met (rethrown on the calling thread once the run is over)
    std::atomic<bool> failed{false};
    char error[256] = {0};
};

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void spinUntil(const std::atomic<char>& flag) {
    for (int spins = 0; !flag.load(std::memory_order_acquire); spins++) {
        if (spins < 4096) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        } else {
            sched_yield();
        }
    }
}

patch::Feed feedOf(const Buffer& b, int ti) {
    MixState& st = *b.st;
    std::vector<float>& fb = st.feedBuf[ti];
    return {(*b.tracks)[ti].get(), {fb.data(), fb.data() + st.capacity}, st.latency[ti]};
}

// Processing a track (see the top of mixer.h): into st.trackBuf[ti], what happened into st.results[ti].
void processTrack(Buffer& b, int ti, MixState::Scratch& scratch) {
    MixState& st = *b.st;
    const TrackList& tracks = *b.tracks;
    const size_t n = tracks.size();
    const int frames = b.frames, sr = b.sr, cap = st.capacity;
    const bool hasStart = b.hasStart, plugins = b.plugins;
    const int64_t start = b.start;
    auto t0 = std::chrono::steady_clock::now();
    MixState::Result& res = st.results[ti];
    res = MixState::Result();
    Track& track = *tracks[ti];
    bool isReturn = track.kind == Kind::Return;
    bool into = isReturn && st.sentUsed[ti];  // (a return's input: what the tracks send it)
    const Clip* const* clips = st.trackClips.data() + st.clipFrom[ti];
    size_t clipCount = (size_t)(st.clipFrom[ti + 1] - st.clipFrom[ti]);
    ClipPtr frozen = aload(track.frozen);
    if (plugins && hasStart && !frozen && !track.automation.empty())
        automation::apply(track, start, sr);  // its automated parameters, for this buffer
    bool vstInstrument = playsPlugins(track);
    bool instrument = vstInstrument && plugins;  // its instrument plugins play the notes
    auto effects = track.effects.get();
    bool hasPlugins = false;
    if (plugins) {
        hasPlugins = instrument;
        for (auto& s : *effects)
            if (active(s.get())) hasPlugins = true;
    }
    const Clip* frozenClip[1] = {nullptr};
    if (frozen) {  // frozen: its rendering plays instead of its clips and plugins
        clipCount = 0;
        if (hasStart && frozen->start < start + frames && frozen->end() > start) {
            frozenClip[0] = frozen.get();
            clips = frozenClip;
            clipCount = 1;
        }
        vstInstrument = instrument = hasPlugins = false;
    }
    int first = track.output < b.outChannels ? track.output : 0;  // device lacks that output: use 1/2
    int channels = std::min(2, b.outChannels - first);
    patch::DelayLine* line = st.lines[ti];
    if (line && b.restart) line->clear();  // what it held belongs to where playback was
    const Track* only = b.only;
    bool audible = only ? !track.needsInstrument && (&track == only || [&] {  // (a stem: it, and what sends to it)
        auto sends = track.sends.get();
        for (auto& s : *sends)
            if (s.track.get() == only) return true;
        return false;
    }())
                        : Project::audibleIn(track, tracks);
    bool feeds = st.feeder[ti] && plugins;  // (a sidechain listens to it: worked out even if not heard)
    if (!audible && line) line->clear();
    live::LiveNotes* notes = live::find(b.live, &track);
    if (notes && (!audible || frozen || (vstInstrument && !plugins))) notes->clear();  // nothing to hear them through now
    bool playingLive = notes && notes->active();
    if (!(audible || feeds) || !(clipCount || hasPlugins || playingLive || into || (line && line->sounding)) ||
        (vstInstrument && !plugins)) {
        res.seconds = secondsSince(t0);
        return;  // Skipped
    }
    std::vector<float>& buf = st.trackBuf[ti];
    float* sig[2] = {buf.data(), buf.data() + cap};
    std::memset(sig[0], 0, sizeof(float) * frames);
    std::memset(sig[1], 0, sizeof(float) * frames);
    if (instrument) {  // keeps rendering even without notes, so releases ring out
        noteMessages(clips, clipCount, hasStart, start, frames, b.restart, scratch.messages, scratch.events,
                     st.heldOf[ti]);
        if (notes) notes->take(frames, sr, scratch.messages);
        auto rack = patch::rackStateOf(track.rackState);
        patch::playInstruments(track, scratch.messages.data(), scratch.messages.size(), sig, channels, frames, sr,
                               *rack, b.transport);
        if (!hasStart && !(notes && notes->holding())) {
            // stopped, nothing played live, yet a note is on: let it go
            auto letGo = [](const SlotPtr& slot) {
                if (!usable(slot)) return;
                auto inst = aload(slot->instance);
                if (inst && inst->holding()) inst->notesOff();
            };
            if (track.usesRack) {
                auto r = track.rack.get();
                for (auto& s : *r) letGo(s);
            } else {
                letGo(aload(track.instrument));
            }
        }
    } else {
        for (size_t k = 0; k < clipCount; k++) addClip(sig, channels, frames, *clips[k], start);
        if (into) {
            const float* in = st.sentBuf[ti].data();
            for (int c = 0; c < channels; c++)
                for (int i = 0; i < frames; i++) sig[c][i] += in[(size_t)c * cap + i];
        }
        if (notes) {
            scratch.messages.clear();
            notes->take(frames, sr, scratch.messages);
            notes->addVoices(sig, channels, frames, sr);
        }
    }
    if (plugins && !frozen) {
        // the sidechains' sources: a return's, every track committed before it (all of them but the returns
        // after it); another track's, the tracks it listens to, processed before it (and waited for)
        const patch::Feed* feedList = st.feeds.data();
        size_t feedCount = st.feeds.size();
        if (!isReturn) {
            scratch.feeds.clear();
            for (size_t j = 0; j < n; j++) {
                if (!st.listens[(size_t)ti * n + j] || st.position[j] >= st.position[ti]) continue;
                spinUntil(st.finished[j]);
                if (st.results[j].fed) scratch.feeds.push_back(feedOf(b, (int)j));
            }
            feedList = scratch.feeds.data();
            feedCount = scratch.feeds.size();
        }
        auto state = patch::stateOf(track.patchState);
        patch::runPatch(st.cables[ti], *effects, sig, channels, frames, sr, *state, feedList, feedCount,
                        instrumentLatency(track), b.transport);
    }
    applyFader(sig, channels, frames, track, hasStart, start, sr);  // its fader and pan, after its effects
    if (feeds) {
        std::vector<float>& fb = st.feedBuf[ti];
        std::memcpy(fb.data(), sig[0], sizeof(float) * frames);
        std::memcpy(fb.data() + cap, channels == 2 ? sig[1] : sig[0], sizeof(float) * frames);
        res.fed = true;
    }
    res.channels = channels;
    res.first = first;
    if (audible) {
        if (line) line->process(sig, frames);
        res.kind = MixState::Result::Heard;
    } else {
        res.kind = MixState::Result::Unheard;
    }
    res.seconds = secondsSince(t0);
}

// Committing a processed track (the calling thread, in the feeding order): its sound for sidechains, into
// the returns it sends to, the tap, and the outputs.
void commitTrack(Buffer& b, int ti) {
    MixState& st = *b.st;
    const TrackList& tracks = *b.tracks;
    const size_t n = tracks.size();
    const int frames = b.frames, cap = st.capacity;
    const MixState::Result& res = st.results[ti];
    const Track& track = *tracks[ti];
    if (b.tap) b.tap->work(&track, res.seconds);
    if (res.fed) st.feeds.push_back(feedOf(b, ti));
    if (res.kind != MixState::Result::Heard) {
        if (b.tap) b.tap->silent(&track, frames);
        return;
    }
    int channels = res.channels;
    std::vector<float>& tb = st.trackBuf[ti];
    float* sig[2] = {tb.data(), tb.data() + cap};
    auto sends = track.sends.get();
    for (auto& send : *sends) {  // (to returns: after the fader and pan, in time with the others)
        if (!(send.db > config::MIN_DB) || !send.track || send.track->kind != Kind::Return) continue;
        int target = -1;
        for (size_t j = 0; j < n; j++)
            if (tracks[j] == send.track) target = (int)j;
        if (target < 0) continue;
        std::vector<float>& buf = st.sentBuf[target];
        if (!st.sentUsed[target]) {
            std::memset(buf.data(), 0, sizeof(float) * frames);
            std::memset(buf.data() + cap, 0, sizeof(float) * frames);
            st.sentUsed[target] = 1;
        }
        float g = (float)dsp::dbToGain(send.db, config::MIN_DB);
        for (int c = 0; c < channels; c++) {
            float* d = buf.data() + (size_t)c * cap;
            for (int i = 0; i < frames; i++) d[i] += sig[c][i] * g;
        }
    }
    if (b.tap) b.tap->sound(&track, sig, channels, res.first, frames);
    if (b.only && &track != b.only) return;
    for (int c = 0; c < channels; c++) {
        float* o = b.out + res.first + c;
        const float* s = sig[c];
        for (int i = 0; i < frames; i++) o[(size_t)i * b.outChannels] += s[i];
    }
}

MixState::Scratch& borrowScratch(MixState& st) {
    for (;;)
        for (auto& s : st.scratch) {
            bool expected = false;
            if (!s->busy.load(std::memory_order_relaxed) &&
                s->busy.compare_exchange_strong(expected, true, std::memory_order_acquire))
                return *s;
        }
}

// a parallel run's job: st.jobs[index]'s track (errors are kept to be rethrown on the calling thread)
void runJob(void* ctx, int index) {
    Buffer& b = *static_cast<Buffer*>(ctx);
    MixState& st = *b.st;
    int ti = st.jobs[index];
    MixState::Scratch& scratch = borrowScratch(st);
    try {
        processTrack(b, ti, scratch);
    } catch (const std::exception& e) {
        st.results[ti] = MixState::Result();
        bool expected = false;
        if (b.failed.compare_exchange_strong(expected, true)) {
            std::strncpy(b.error, e.what(), sizeof(b.error) - 1);
        }
    } catch (...) {
        st.results[ti] = MixState::Result();
        bool expected = false;
        if (b.failed.compare_exchange_strong(expected, true)) std::strncpy(b.error, "unknown error", sizeof(b.error) - 1);
    }
    scratch.busy.store(false, std::memory_order_release);
    st.finished[ti].store(1, std::memory_order_release);
}

// st.jobs processed (on the pool), then committed in their order
void runParallel(Buffer& b) {
    MixState& st = *b.st;
    st.pool->run((int)st.jobs.size(), runJob, &b);
    if (b.failed.load()) throw std::runtime_error(b.error);
    for (int ti : st.jobs) commitTrack(b, ti);
}

}  // namespace

int mixClips(Project& project, float* out, int outChannels, int frames, bool hasStart, int64_t start, MixState& st,
             TrackTap* tap, int sr, bool restart, bool plugins, const live::LiveMap* live, const Track* only) {
    auto tracksPtr = project.tracks.get();
    const TrackList& tracks = *tracksPtr;
    auto clipsPtr = project.clips.get();
    auto masterCables = project.masterCables.get();
    auto masterEffects = project.masterEffects.get();
    TempoMapPtr tempoMap = project.timing.audioMap();
    const size_t n = tracks.size();
    st.reserve(frames, (int)n);
    const int cap = st.capacity;
    // (the tempo and place tempo-synced plugins follow)
    Transport transport = Transport::at(*tempoMap, hasStart, start, sr);

    st.cables.clear();
    for (auto& t : tracks) st.cables.push_back(t->cables.get());
    st.latency.assign(n, 0);
    if (plugins) {
        // (every patch keeps its plan in its state: made once here if the GUI didn't, see patch.h STATES)
        for (auto& t : tracks) patch::stateOf(t->patchState);
        patch::stateOf(project.masterPatchState);
    }
    int latest = 0;
    for (size_t i = 0; i < n; i++) {  // (a track's delay: to play later, it counts as less late)
        const Track& t = *tracks[i];
        st.latency[i] = (plugins ? trackLatency(t, tracks) : 0) - (t.kind != Kind::Return ? (int)delayFrames(t, sr) : 0);
        latest = std::max(latest, st.latency[i]);
    }
    int master = plugins && !only ? patch::patchLatency(masterCables, *masterEffects, latest, tracks, nullptr,
                                                         project.masterPatchState) - latest
                                  : 0;
    orderTracks(tracks, *masterCables, st);
    st.feeds.clear();
    st.sentUsed.assign(n, 0);
    if (st.compensate) {
        for (size_t i = 0; i < st.delays.size();) {
            if (!inList(st.delays[i].first, tracks))
                st.delays.erase(st.delays.begin() + i);
            else
                i++;
        }
    }
    for (size_t i = 0; i < st.held.size();) {
        if (!inList(st.held[i].first, tracks))
            st.held.erase(st.held.begin() + i);
        else
            i++;
    }
    st.active.clear();
    if (hasStart) {
        int64_t end = start + frames;
        for (auto& clip : *clipsPtr)
            // >= : a note-off can fall exactly on a MIDI clip's end
            if (clip->start < end && clip->end() >= start && !clip->disabled)
                st.active.push_back({clip.get(), aload(clip->track).get()});
    }
    // --- before the tracks: what may allocate, or is shared, made here on this thread ---
    st.results.assign(n, MixState::Result());
    st.position.assign(n, 0);
    for (size_t k = 0; k < st.order.size(); k++) st.position[st.order[k]] = (int)k;
    st.clipFrom.assign(n + 1, 0);
    st.trackClips.clear();
    for (size_t ti = 0; ti < n; ti++) {  // each track's clips, in the project's order
        st.clipFrom[ti] = (int)st.trackClips.size();
        for (auto& a : st.active)
            if (a.track == tracks[ti].get()) st.trackClips.push_back(a.clip);
    }
    st.clipFrom[n] = (int)st.trackClips.size();
    st.lines.assign(n, nullptr);
    if (st.compensate) {  // each track held back to line up with the latest one (a return: by its own delay)
        for (size_t ti = 0; ti < n; ti++) {
            const Track& track = *tracks[ti];
            bool known = false;
            for (auto& d : st.delays)
                if (d.first == &track) known = true;
            if (!known) st.delays.emplace_back(&track, patch::DelayLine());
        }
        for (size_t ti = 0; ti < n; ti++) {  // (pointers taken once `delays` stops growing)
            const Track& track = *tracks[ti];
            int first = track.output < outChannels ? track.output : 0;
            int channels = std::min(2, outChannels - first);
            int wanted = (track.kind == Kind::Return ? (int)std::max<int64_t>(0, delayFrames(track, sr))
                                                     : latest - st.latency[ti]) +
                         (first >= 2 ? master : 0);
            patch::DelayLine* line = nullptr;
            for (auto& d : st.delays)
                if (d.first == &track) line = &d.second;
            if (line->frames != wanted || line->channels != channels) line->init(wanted, channels);
            st.lines[ti] = line;
        }
    }
    st.heldOf.assign(n, nullptr);
    if (plugins) {  // the notes each instrument track holds: an entry made for each, then pointers taken
        for (size_t ti = 0; ti < n; ti++) {
            Track& track = *tracks[ti];
            if (!playsPlugins(track)) continue;
            bool known = false;
            for (auto& h : st.held)
                if (h.first == &track) known = true;
            if (!known) st.held.emplace_back(&track, std::array<uint16_t, 128>{});
            patch::rackStateOf(track.rackState);  // (made here if the GUI didn't)
        }
        for (size_t ti = 0; ti < n; ti++)  // (once `held` stops growing)
            for (auto& h : st.held)
                if (h.first == tracks[ti].get()) st.heldOf[ti] = h.second.data();
    }
    for (size_t ti = 0; ti < n; ti++) st.finished[ti].store(0, std::memory_order_relaxed);

    Buffer b;
    b.tracks = &tracks;
    b.st = &st;
    b.out = out;
    b.outChannels = outChannels;
    b.frames = frames;
    b.sr = sr;
    b.latest = latest;
    b.start = start;
    b.hasStart = hasStart;
    b.restart = restart;
    b.plugins = plugins;
    b.live = live;
    b.only = only;
    b.tap = tap;
    b.transport = transport;

    // --- the tracks ---
    bool parallel = st.pool && st.pool->threads() > 0 && n >= 2;
    if (!parallel) {
        MixState::Scratch& scratch = *st.scratch[0];
        for (int ti : st.order) {
            processTrack(b, ti, scratch);
            st.finished[ti].store(1, std::memory_order_relaxed);
            commitTrack(b, ti);
        }
    } else {
        st.jobs.clear();  // every track but the returns, at once
        for (int ti : st.order)
            if (tracks[ti]->kind != Kind::Return) st.jobs.push_back(ti);
        runParallel(b);
        // the returns: at once too, unless one sends to or listens to another (then in order, as on one thread)
        bool linked = false;
        for (int ti : st.order) {
            const Track& t = *tracks[ti];
            if (t.kind != Kind::Return) continue;
            auto sends = t.sends.get();
            for (auto& s : *sends)
                if (s.track && s.track->kind == Kind::Return) linked = true;
            for (auto& c : *st.cables[ti])
                if (c.first.type == End::TrackSource && c.first.track && c.first.track->kind == Kind::Return)
                    linked = true;
        }
        st.jobs.clear();
        for (int ti : st.order)
            if (tracks[ti]->kind == Kind::Return) st.jobs.push_back(ti);
        if (!linked) {
            runParallel(b);
        } else {
            MixState::Scratch& scratch = *st.scratch[0];
            for (int ti : st.jobs) {
                processTrack(b, ti, scratch);
                st.finished[ti].store(1, std::memory_order_relaxed);
                commitTrack(b, ti);
            }
        }
    }
    if (plugins && !only) {
        int w = std::min(2, outChannels);
        float* m[2] = {st.master.data(), st.master.data() + cap};
        if (!patch::isStraight(*masterCables)) {
            for (int c = 0; c < w; c++)
                for (int i = 0; i < frames; i++) m[c][i] = out[(size_t)i * outChannels + c];
            auto state = patch::stateOf(project.masterPatchState);
            patch::runPatch(masterCables, *masterEffects, m, w, frames, sr, *state, st.feeds.data(), st.feeds.size(),
                            latest, transport);
            for (int c = 0; c < w; c++)
                for (int i = 0; i < frames; i++) out[(size_t)i * outChannels + c] = m[c][i];
        }
    }
    return st.compensate ? latest + master : 0;
}

}  // namespace mixer
