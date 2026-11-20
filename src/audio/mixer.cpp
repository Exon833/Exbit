#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <sched.h>
#include <stdexcept>

#include "audio/automation.h"
#include "audio/modulation.h"
#include "model/routing.h"
#include "model/clip_envelope.h"
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
        for (auto& f : preBuf) f.assign((size_t)frames * 2, 0.0f);
        for (auto& f : sendBuf) f.assign((size_t)frames * 2, 0.0f);
        for (auto& f : padBuf) f.assign((size_t)frames * 2, 0.0f);
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
        preDelays.reserve(tracks);
        preLines.reserve(tracks);
        heldOf.reserve(tracks);
        position.reserve(tracks);
        jobs.reserve(tracks);
        all.reserve(tracks);
        for (auto* v : {&dest, &level, &firstOut, &own, &needOut, &inLate, &sendExtra, &pathLat}) v->reserve(tracks);
        for (auto* v : {&plain, &hasInput, &reachesOnly}) v->reserve(tracks);
        sendDelays.reserve(tracks);
        sendLines.reserve(tracks);
        while ((int)feedBuf.size() < tracks) feedBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)sentBuf.size() < tracks) sentBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)trackBuf.size() < tracks) trackBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)preBuf.size() < tracks) preBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)sendBuf.size() < tracks) sendBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)padBuf.size() < tracks) padBuf.emplace_back((size_t)capacity * 2, 0.0f);
        padDest.reserve((size_t)tracks * PadKit::MAX_PADS);
        for (auto* v : {&padFrom}) v->reserve(tracks);
        for (auto* v : {&padsOut, &padFilled}) v->reserve(tracks);
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
    for (auto& s : scratch) {
        s->feeds.reserve(std::max(trackCapacity, 1));
        s->pieceFeeds.reserve(std::max(trackCapacity, 1));
    }
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
    for (size_t j = 0; j < n; j++)  // (in their own order: what it listens to, and what plays into it)
        if ((st.listens[i * n + j] || st.dest[j] == (int)i) && !st.visit[j]) visitTrack(st, n, j);
    st.visit[i] = 2;
    st.order.push_back((int)i);
}

// which tracks listen to which (st.listens) and which some sidechain listens to (st.feeder); true if any listens
// (st.cables: the tracks' cables, snapshots taken for this buffer)
static bool listening(const TrackList& tracks, const std::vector<Cable>& masterCables, MixState& st) {
    size_t n = tracks.size();
    st.listens.assign(n * n, 0);
    st.feeder.assign(n, 0);
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
    // PADS ON THEIR OWN TRACKS: a pad track listens to its kit (one kit per track: the first claiming it)
    const size_t P = PadKit::MAX_PADS;
    st.padDest.assign(n * P, -1);
    st.padFrom.assign(n, -1);
    st.padsOut.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        auto outs = std::atomic_load(&tracks[i]->padOut);
        if (!outs || !tracks[i]->playsPadKit()) continue;
        for (size_t pad = 0; pad < outs->size() && pad < P; pad++) {
            const Track* to = (*outs)[pad].get();
            if (!to) continue;
            for (size_t j = 0; j < n; j++) {
                if (tracks[j].get() != to || j == i || to->kind == Kind::Return || to->busOf) continue;
                if (st.padFrom[j] >= 0 && st.padFrom[j] != (int)i) break;
                st.padFrom[j] = (int)i;
                st.padDest[i * P + pad] = (int)j;
                st.padsOut[i] = 1;
                st.listens[j * n + i] = 1;
                st.feeder[i] = 1;  // (worked out even unheard; never put in the last wave after its pad tracks)
                any = true;
            }
        }
    }
    return any;
}

// GROUP BUSES AND ROUTING (mixer.h): where each track's sound goes (plain buses skipped), and the output pair it
// reaches; true if any track plays into another
static bool routeTracks(const TrackList& tracks, MixState& st, const Track* only) {
    size_t n = tracks.size();
    auto indexOf = [&](const Track* t) -> int {
        for (size_t j = 0; j < n; j++)
            if (tracks[j].get() == t) return (int)j;
        return -1;
    };
    st.plain.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        const Track& t = *tracks[i];
        st.plain[i] = t.busOf && routing::isPlain(t) && !st.feeder[i] && &t != only && !Project::mutedIn(t);
    }
    st.dest.assign(n, -1);
    st.firstOut.assign(n, 0);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        const Track& t = *tracks[i];
        int pair = t.output, d = -1;
        const Track* to = routing::target(t);
        for (size_t steps = 0; to && steps <= n; steps++) {  // (past plain buses: to where they go)
            d = indexOf(to);
            if (d < 0 || !st.plain[d]) break;
            pair = to->output;
            to = routing::target(*to);
            d = -1;
        }
        if (st.plain[i] || d == (int)i) d = -1;  // (a plain bus itself gets nothing)
        st.dest[i] = d;
        st.firstOut[i] = d >= 0 ? 0 : pair;
        any = any || d >= 0;
    }
    return any;
}

// a sidechain listening to a bus (or track) its own sound plays into would be a loop: it hears nothing instead, and
// its sound still goes where it goes
static void dropFeedback(MixState& st, size_t n) {
    for (size_t i = 0; i < n; i++)
        for (int d = st.dest[i], steps = 0; d >= 0 && steps <= (int)n; d = st.dest[d], steps++) st.listens[i * n + d] = 0;
}

// The feeding order (st.order): each track after the ones its sidechains listen to and the ones playing into it,
// in waves (st.level); the returns last.
static void orderTracks(const TrackList& tracks, bool listens, bool routes, MixState& st) {
    size_t n = tracks.size();
    st.order.clear();
    if (!listens && !routes) {
        for (size_t i = 0; i < n; i++) st.order.push_back((int)i);
    } else {
        st.visit.assign(n, 0);
        for (size_t i = 0; i < n; i++)
            if (!st.visit[i]) visitTrack(st, n, i);
    }
    st.level.assign(n, 0);
    int deepest = 0;
    if (routes) {
        st.position.assign(n, 0);
        for (size_t k = 0; k < st.order.size(); k++) st.position[st.order[k]] = (int)k;
        for (size_t j = 0; j < n; j++)  // (a loop being edited: what would come back plays to the outputs)
            if (st.dest[j] >= 0 && st.position[st.dest[j]] <= st.position[j]) st.dest[j] = -1;
        // a track's wave: after every wave a track playing into it was in; with what it listens to, or after
        for (int i : st.order) {
            int lv = 0;
            for (size_t j = 0; j < n; j++) {
                if (st.position[j] >= st.position[i]) continue;
                if (st.dest[j] == i) lv = std::max(lv, st.level[j] + 1);
                if (listens && st.listens[(size_t)i * n + j]) lv = std::max(lv, st.level[j]);
            }
            st.level[i] = lv;
            deepest = std::max(deepest, lv);
        }
        // what nothing waits for (playing to the outputs, no sidechain listening to it) goes in the last wave: the
        // cores work on it alongside the buses there rather than all at once before them
        for (size_t i = 0; i < n; i++)
            if (st.dest[i] < 0 && !st.feeder[i] && tracks[i]->kind != Kind::Return) st.level[i] = deepest;
    }
    st.mixOrder.clear();  // (wave by wave; the returns last)
    for (int lv = 0; lv <= deepest; lv++)
        for (int i : st.order)
            if (tracks[i]->kind != Kind::Return && st.level[i] == lv) st.mixOrder.push_back(i);
    for (int i : st.order)
        if (tracks[i]->kind == Kind::Return) st.mixOrder.push_back(i);
    st.order.swap(st.mixOrder);
}

void addClip(float* const buf[2], int channels, int64_t bufFrames, const Clip& clip, int64_t start,
             const AudioBuffer* instead) {
    AudioPtr own = instead ? nullptr : aload(clip.data);
    const AudioBuffer* data = instead ? instead : own.get();
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
    const FadeShape inShape = clip.fadeInShape, outShape = clip.fadeOutShape;
    auto envelope = levels ? std::atomic_load(&clip.gainEnvelope) : nullptr;  // (its gain envelope: clip_envelope.h)
    static const std::vector<AutoPoint> none;
    clip_envelope::Walk walk{clip, envelope ? *envelope : none};
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
            // (each fade of its shape: fadeCurve, a polynomial; Linear is the straight line exactly)
            if (fi) level *= (float)fadeCurve(inShape, std::min(1.0, std::max(0.0, (double)(pos + lead + 1) / fi)), false);
            if (fo) level *= (float)fadeCurve(outShape, std::min(1.0, std::max(0.0, (double)(frames + tail - pos) / fo)), true);
            if (envelope) level *= walk.gain(clip.offset + pos);
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
    bool listens = listening(tracks, masterCables, st);
    bool routes = routeTracks(tracks, st, nullptr);
    if (routes) dropFeedback(st, tracks.size());
    orderTracks(tracks, listens, routes, st);
}

namespace {

// does the track send to a return before its fader (mixer.h PRE-FADER SENDS)? (a send that is up)
bool hasPreFaderSend(const Track& track) {
    auto sends = track.sends.get();
    for (auto& s : *sends)
        if (s.preFader && s.db > config::MIN_DB && s.track && s.track->kind == Kind::Return) return true;
    return false;
}
// …or after it
bool hasPostFaderSend(const Track& track) {
    auto sends = track.sends.get();
    for (auto& s : *sends)
        if (!s.preFader && s.db > config::MIN_DB && s.track && s.track->kind == Kind::Return) return true;
    return false;
}

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
    TempoMapPtr tempoMap;
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
    double waited = 0;  // (seconds spent waiting for the tracks its sidechains listen to)
    MixState::Result& res = st.results[ti];
    res = MixState::Result();
    Track& track = *tracks[ti];
    bool isReturn = track.kind == Kind::Return;
    // (its input: what the tracks send it, a return's; what plays into it, a bus's or a routed-into track's)
    bool into = st.sentUsed[ti];
    const Clip* const* clips = st.trackClips.data() + st.clipFrom[ti];
    size_t clipCount = (size_t)(st.clipFrom[ti + 1] - st.clipFrom[ti]);
    ClipPtr frozen = aload(track.frozen);
    // its automated parameters, for this buffer (in time: automation.h; a bus's as late as its input comes)
    if (plugins && hasStart && !frozen && !track.automation.empty())
        automation::apply(track, start - st.inLate[ti], sr, &tracks);
    bool vstInstrument = playsPlugins(track);
    bool instrument = vstInstrument && plugins;  // its instrument plugins play the notes
    // an External synth: its sound from the device's inputs (EXTERNAL SYNTHS)
    const bool external = track.kind == Kind::Midi && track.ext.on && !track.needsInstrument && st.input &&
                          track.ext.input >= 0 && track.ext.input < st.inputChannels;
    auto effects = track.effects.get();
    bool hasPlugins = false;
    if (plugins) {
        hasPlugins = instrument;
        for (auto& s : *effects)
            if (active(s.get())) hasPlugins = true;
    }
    const Clip* const* ownClips = clips;  // (a kit's pads on their own tracks: from its clips, even frozen)
    const size_t ownCount = clipCount;
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
    // MONITORING (mixer.h): recorded onto, its clips are muted; its input heard through it
    if (track.kind == Kind::Audio && !frozen)
        for (int r = 0; r < st.recordingCount; r++)
            if (st.recording[r] == &track) clipCount = 0;
    const float* heard = st.input && !frozen && hearsInput(track) && track.inputCh < st.inputChannels ? st.input : nullptr;
    // (the output pair it reaches: its own, or the outermost bus's it passes; device lacks that output: use 1/2)
    int first = st.firstOut[ti] < b.outChannels ? st.firstOut[ti] : 0;
    int channels = std::min(2, b.outChannels - first);
    patch::DelayLine* line = st.lines[ti];
    if (line && b.restart) line->clear();  // what it held belongs to where playback was
    patch::DelayLine* preLine = st.preLines[ti];  // (pre-fader sends: mixer.h)
    if (preLine && b.restart) preLine->clear();
    patch::DelayLine* sendLine = st.sendLines[ti];  // (its sends held back further: GROUP BUSES AND ROUTING)
    if (sendLine && b.restart) sendLine->clear();
    const Track* only = b.only;
    // a stem: it, what sends to it and what plays into it. Otherwise as mute and solo say; a bus (or a track played
    // into) also while a heard track plays into it (soloing a track keeps its bus heard), unless muted
    bool audible = only ? !track.needsInstrument && (&track == only || st.reachesOnly[ti] || [&] {
        auto sends = track.sends.get();
        for (auto& s : *sends)
            if (s.track.get() == only) return true;
        return false;
    }())
                        : Project::audibleIn(track, tracks) || (into && !Project::mutedIn(track) && !track.needsInstrument);
    bool feeds = st.feeder[ti] && plugins;  // (a sidechain listens to it: worked out even if not heard)
    // PADS ON THEIR OWN TRACKS (see the top): a pad track's input from its kit, processed before it (waited for);
    // a kit hands its pads on even when not heard
    const bool padsOut = st.padsOut[ti];
    bool padIn = false;
    if (int kit = st.padFrom[ti]; kit >= 0 && !frozen && st.position[kit] < st.position[ti]) {
        if (!st.finished[kit].load(std::memory_order_acquire)) {
            auto w0 = std::chrono::steady_clock::now();
            spinUntil(st.finished[kit]);
            waited += secondsSince(w0);
        }
        padIn = st.padFilled[ti];
    }
    if (!audible && line) line->clear();
    if (!audible && preLine) preLine->clear();
    if (!audible && sendLine) sendLine->clear();
    live::LiveNotes* notes = live::find(b.live, &track);
    if (notes && (!(audible || padsOut) || frozen || (vstInstrument && !plugins))) notes->clear();  // nothing to hear them through now
    bool playingLive = notes && notes->active();
    // (the song's beats for the MIDI effects that time notes played live: live_fx)
    live::Clock liveClock = playingLive ? live::clockAt(*b.tempoMap, hasStart, start, sr) : live::Clock();
    // (an instrument chosen this very moment, after the buffer's preparation: it starts with the next one, so
    // no note is sent it without its held notes being kept: instrument && !heldOf)
    if (!(audible || feeds || padsOut) || !(clipCount || (padsOut && ownCount) || hasPlugins || playingLive || into || padIn || heard ||
                                            (external && !frozen) ||
                                            (line && line->sounding) || (sendLine && sendLine->sounding)) ||
        (vstInstrument && !plugins) || (instrument && !st.heldOf[ti])) {
        res.seconds = secondsSince(t0);
        return;  // Skipped
    }
    // modulation cables (audio/modulation.h): the knobs set for the first piece now, before the instruments
    // play; the effects are then worked out piece by piece, the knobs moved between pieces
    auto mods = plugins && !frozen ? track.modCables.get() : nullptr;
    bool modulated = mods && !mods->empty();
    int firstPiece = frames;
    if (modulated) {
        firstPiece = modulation::toNextStep(hasStart, start, 0, frames);
        // (a step only on its frame, or where playback starts or jumps: between steps the knobs keep the last
        // step's values, so buffers of any size move them at the same frames)
        if (!hasStart || b.restart || start % modulation::BLOCK == 0)
            modulation::apply(*mods, modulation::clockAt(*b.tempoMap, hasStart, start, firstPiece, sr), true);
    }
    std::vector<float>& buf = st.trackBuf[ti];
    float* sig[2] = {buf.data(), buf.data() + cap};
    std::memset(sig[0], 0, sizeof(float) * frames);
    std::memset(sig[1], 0, sizeof(float) * frames);
    auto addInput = [&] {  // (its input: before its effects)
        if (into) {
            const float* in = st.sentBuf[ti].data();
            for (int c = 0; c < channels; c++)
                for (int i = 0; i < frames; i++) sig[c][i] += in[(size_t)c * cap + i];
        }
        if (padIn) {  // (a pad track: its pad's sound)
            const float* in = st.padBuf[ti].data();
            for (int c = 0; c < channels; c++)
                for (int i = 0; i < frames; i++) sig[c][i] += in[(size_t)c * cap + i];
        }
    };
    if (instrument) {  // keeps rendering even without notes, so releases ring out
        noteMessages(clips, clipCount, hasStart, start, frames, b.restart, scratch.messages, scratch.events,
                     st.heldOf[ti]);
        if (notes) notes->take(frames, sr, scratch.messages, &liveClock);
        auto rack = patch::rackStateOf(track.rackState);
        patch::playInstruments(track, scratch.messages.data(), scratch.messages.size(), sig, channels, frames, sr,
                               *rack, b.transport);
        addInput();
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
    } else if (external && !frozen) {  // (EXTERNAL SYNTHS: what its synth played, on the inputs it names)
        const float* in = st.input;
        int ic = st.inputChannels, left = track.ext.input;
        int right = track.ext.stereo && left + 1 < ic ? left + 1 : left;
        for (int i = 0; i < frames; i++) {
            sig[0][i] = in[(size_t)i * ic + left];
            if (channels > 1) sig[1][i] = in[(size_t)i * ic + right];
        }
        addInput();
        if (notes) notes->clear();  // (its notes played live went out as MIDI)
    } else {
        for (size_t k = 0; k < clipCount; k++) addClip(sig, channels, frames, *clips[k], start);
        // a kit's pads kept apart: into their tracks' input (one whose track isn't there: into its own sound)
        const int* dest = padsOut ? st.padDest.data() + (size_t)ti * PadKit::MAX_PADS : nullptr;
        float* padSig[PadKit::MAX_PADS][2] = {};
        if (dest)
            for (int pad = 0; pad < PadKit::MAX_PADS; pad++) {
                int d = dest[pad];
                if (d < 0) continue;
                padSig[pad][0] = st.padBuf[d].data();
                padSig[pad][1] = channels > 1 ? st.padBuf[d].data() + cap : st.padBuf[d].data();
                if (!st.padFilled[d]) {
                    std::memset(padSig[pad][0], 0, sizeof(float) * frames);
                    std::memset(st.padBuf[d].data() + cap, 0, sizeof(float) * frames);
                    st.padFilled[d] = 1;
                }
            }
        for (size_t k = 0; k < ownCount; k++) {
            const Clip& clip = *ownClips[k];
            if (clip.kind != Kind::Midi) continue;
            AudioPtr data = aload(clip.data);
            if (!data) continue;
            for (auto& [pad, sound] : data->pads) {
                bool routed = dest && pad >= 0 && pad < PadKit::MAX_PADS && dest[pad] >= 0;
                if (routed) addClip(padSig[pad], channels, frames, clip, start, sound.get());
                else if (!frozen) addClip(sig, channels, frames, clip, start, sound.get());  // (frozen: in its rendering)
            }
        }
        addInput();
        if (heard) {  // (its device input: a pair, or a mono one on both sides)
            const int w = st.inputChannels, l = track.inputCh, r = track.inputStereo && l + 1 < w ? l + 1 : l;
            for (int i = 0; i < frames; i++) {
                sig[0][i] += heard[(size_t)i * w + l];
                if (channels > 1) sig[1][i] += heard[(size_t)i * w + r];
            }
        }
        if (notes) {
            scratch.messages.clear();
            notes->take(frames, sr, scratch.messages, &liveClock);
            float* const* byPitch[128] = {};
            PadKitPtr kit = dest ? aload(track.padKit) : nullptr;
            if (kit)  // (each key the pad it plays: padkit::padFor)
                for (int pad = PadKit::MAX_PADS; pad-- > 0;)
                    if (!kit->pads[(size_t)pad].empty()) {
                        int key = kit->pads[(size_t)pad].note;
                        if (key >= 0 && key < 128) byPitch[key] = dest[pad] >= 0 ? padSig[pad] : nullptr;
                    }
            notes->addVoices(sig, channels, frames, sr, kit ? byPitch : nullptr);
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
                if (!st.finished[j].load(std::memory_order_acquire)) {  // (waiting isn't its work: not counted)
                    auto w0 = std::chrono::steady_clock::now();
                    spinUntil(st.finished[j]);
                    waited += secondsSince(w0);
                }
                if (st.results[j].fed) scratch.feeds.push_back(feedOf(b, (int)j));
            }
            feedList = scratch.feeds.data();
            feedCount = scratch.feeds.size();
        }
        auto state = patch::stateOf(track.patchState);
        if (!modulated) {
            patch::runPatch(st.cables[ti], *effects, sig, channels, frames, sr, *state, feedList, feedCount,
                            instrumentLatency(track), b.transport);
        } else {
            for (int done = 0; done < frames;) {
                int n = done == 0 ? firstPiece : modulation::toNextStep(hasStart, start + done, done, frames - done);
                if (done > 0)
                    modulation::apply(*mods, modulation::clockAt(*b.tempoMap, hasStart, start + done, n, sr), false);
                float* piece[2] = {sig[0] + done, sig[1] + done};
                scratch.pieceFeeds.clear();  // (the sidechains' sound from the same place)
                for (size_t k = 0; k < feedCount; k++) {
                    patch::Feed f = feedList[k];
                    f.ch[0] += done;
                    f.ch[1] += done;
                    scratch.pieceFeeds.push_back(f);
                }
                Transport tr = Transport::at(*b.tempoMap, hasStart, start + done, sr);
                patch::runPatch(st.cables[ti], *effects, piece, channels, n, sr, *state, scratch.pieceFeeds.data(),
                                scratch.pieceFeeds.size(), instrumentLatency(track), tr);
                done += n;
            }
        }
    }
    // --- pre-fader sends (mixer.h): its sound as it is now, before the fader, kept for them ---
    // (only when heard, as sends are; with delay compensation, only once its line is made: mixClips)
    float* pre[2] = {nullptr, nullptr};
    if (audible && (!st.compensate || preLine) && hasPreFaderSend(track)) {
        std::vector<float>& pb = st.preBuf[ti];
        pre[0] = pb.data();
        pre[1] = pb.data() + cap;
        for (int c = 0; c < channels; c++) std::memcpy(pre[c], sig[c], sizeof(float) * frames);
        res.pre = true;
    }
    // its fader and pan, after its effects: their curves as late as the effects made the sound (automation.h)
    int64_t late = plugins ? st.latency[ti] + (isReturn ? 0 : delayFrames(track, sr)) : 0;
    applyFader(sig, channels, frames, track, hasStart, start - late, sr);
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
        if (res.pre && preLine) preLine->process(pre, frames);  // (held back just as much, and its sends' extra)
        if (sendLine) {  // (playing into a bus: its post-fader sends reach the returns with everything else)
            std::vector<float>& sb = st.sendBuf[ti];
            float* sent[2] = {sb.data(), sb.data() + cap};
            for (int c = 0; c < channels; c++) std::memcpy(sent[c], sig[c], sizeof(float) * frames);
            sendLine->process(sent, frames);
            res.sent = true;
        }
        res.kind = MixState::Result::Heard;
    } else {
        res.kind = MixState::Result::Unheard;
    }
    res.seconds = std::max(0.0, secondsSince(t0) - waited);
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
    std::vector<float>& pb = st.preBuf[ti];
    float* pre[2] = {pb.data(), pb.data() + cap};  // (pre-fader sends: its sound before the fader, if kept)
    std::vector<float>& sb = st.sendBuf[ti];
    float* sent[2] = {sb.data(), sb.data() + cap};  // (post-fader sends of a bus's track: held back further)
    float* const* post = res.sent ? sent : sig;
    auto sends = track.sends.get();
    for (auto& send : *sends) {  // (to returns: after the fader and pan, or before them; in time with the others)
        if (!(send.db > config::MIN_DB) || !send.track || send.track->kind != Kind::Return) continue;
        if (send.preFader && !res.pre) continue;  // (made one a moment ago: from the next buffer)
        float* const* from = send.preFader ? pre : post;
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
            for (int i = 0; i < frames; i++) d[i] += from[c][i] * g;
        }
    }
    if (b.tap) b.tap->sound(&track, sig, channels, res.first, frames);
    // into the bus or track it plays into (GROUP BUSES AND ROUTING: processed in a later wave), or the outputs
    int d = st.dest[ti];
    if (b.only ? &track != b.only : d >= 0) {
        if (d < 0 || (b.only && !st.reachesOnly[ti])) return;
        std::vector<float>& buf = st.sentBuf[d];
        if (!st.sentUsed[d]) {
            std::memset(buf.data(), 0, sizeof(float) * frames);
            std::memset(buf.data() + cap, 0, sizeof(float) * frames);
            st.sentUsed[d] = 1;
        }
        for (int c = 0; c < channels; c++) {
            float* to = buf.data() + (size_t)c * cap;
            for (int i = 0; i < frames; i++) to[i] += sig[c][i];
        }
        return;
    }
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
    // the tracks mixed: the project's, then the bus of every group they are in that has one (GROUP BUSES AND ROUTING)
    st.all.clear();
    for (auto& t : *tracksPtr) st.all.push_back(t);
    const size_t projectTracks = st.all.size();
    for (size_t i = 0; i < projectTracks; i++) {
        if (st.all[i]->kind == Kind::Return) continue;
        int depth = 0;  // (its groups, innermost first; parents never loop, but a guard all the same)
        for (Group* g = st.all[i]->group.get(); g && depth < 4096; g = aload(g->parent).get(), depth++) {
            if (!routing::busOn(*g)) continue;
            bool known = false;
            for (size_t j = projectTracks; j < st.all.size() && !known; j++) known = st.all[j] == g->bus;
            if (!known) st.all.push_back(g->bus);
        }
    }
    const TrackList& tracks = st.all;
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
    if (plugins) {
        // (every patch keeps its plan in its state: made once here if the GUI didn't, see patch.h STATES)
        for (auto& t : tracks) patch::stateOf(t->patchState);
        patch::stateOf(project.masterPatchState);
    }
    bool listens = listening(tracks, *masterCables, st);
    bool routes = routeTracks(tracks, st, only);
    if (routes) dropFeedback(st, n);
    orderTracks(tracks, listens, routes, st);
    st.own.assign(n, 0);
    for (size_t i = 0; i < n; i++) {  // (a track's delay: to play later, it counts as less late)
        const Track& t = *tracks[i];
        st.own[i] = (plugins ? trackLatency(t, tracks) : 0) - (t.kind != Kind::Return ? (int)delayFrames(t, sr) : 0);
    }
    // delay compensation along the paths (GROUP BUSES AND ROUTING): how late each one's sound comes out (pathLat:
    // a target's input lined up with the latest of its sources), the latest of those reaching the outputs, then how
    // late each must reach where it goes (needOut) to line up there
    st.hasInput.assign(n, 0);
    for (size_t j = 0; j < n; j++)
        if (st.dest[j] >= 0) st.hasInput[st.dest[j]] = 1;
    st.pathLat.assign(n, 0);
    for (int i : st.order) {
        int base = 0;
        if (st.hasInput[i]) {
            bool firstSource = true;
            for (size_t j = 0; j < n; j++)
                if (st.dest[j] == i) {
                    base = firstSource ? st.pathLat[j] : std::max(base, st.pathLat[j]);
                    firstSource = false;
                }
            if (!tracks[i]->busOf) base = std::max(base, 0);  // (a track's own clips come in time)
        }
        st.pathLat[i] = base + st.own[i];
    }
    int latest = 0;
    for (size_t i = 0; i < n; i++)
        if (st.dest[i] < 0 || tracks[i].get() == only) latest = std::max(latest, st.pathLat[i]);
    st.needOut.assign(n, 0);
    st.latency.assign(n, 0);
    st.inLate.assign(n, 0);
    st.sendExtra.assign(n, 0);
    for (size_t k = st.order.size(); k-- > 0;) {
        int i = st.order[k], d = st.dest[i];
        st.needOut[i] = d < 0 || tracks[i].get() == only ? latest : st.needOut[d] - st.own[d];
        st.inLate[i] = st.hasInput[i] ? st.needOut[i] - st.own[i] : 0;
        st.latency[i] = st.hasInput[i] ? st.needOut[i] : st.own[i];  // (its sound's lateness, before its line)
        st.sendExtra[i] = std::max(0, latest - st.needOut[i]);
    }
    st.reachesOnly.assign(n, 0);
    if (only)
        for (size_t i = 0; i < n; i++)
            for (int d = st.dest[i], steps = 0; d >= 0 && steps <= (int)n; d = st.dest[d], steps++)
                if (tracks[d].get() == only) st.reachesOnly[i] = 1;
    int master = plugins && !only ? patch::patchLatency(masterCables, *masterEffects, latest, tracks, nullptr,
                                                         project.masterPatchState) - latest
                                  : 0;
    st.feeds.clear();
    st.sentUsed.assign(n, 0);
    if (st.compensate) {
        for (size_t i = 0; i < st.delays.size();) {
            if (!inList(st.delays[i].first, tracks))
                st.delays.erase(st.delays.begin() + i);
            else
                i++;
        }
        for (size_t i = 0; i < st.preDelays.size();) {  // (pre-fader sends: tracks gone, or without one now)
            const Track* t = st.preDelays[i].first;
            if (!inList(t, tracks) || !hasPreFaderSend(*t))
                st.preDelays.erase(st.preDelays.begin() + i);
            else
                i++;
        }
        for (size_t i = 0; i < st.sendDelays.size();) {  // (sends held back further: gone, or not now)
            const Track* t = st.sendDelays[i].first;
            int at = -1;
            for (size_t j = 0; j < n; j++)
                if (tracks[j].get() == t) at = (int)j;
            if (at < 0 || !st.sendExtra[at] || !hasPostFaderSend(*t))
                st.sendDelays.erase(st.sendDelays.begin() + i);
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
    st.preLines.assign(n, nullptr);
    st.sendLines.assign(n, nullptr);
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
            int first = st.firstOut[ti] < outChannels ? st.firstOut[ti] : 0;
            int channels = std::min(2, outChannels - first);
            int wanted = (track.kind == Kind::Return ? (int)std::max<int64_t>(0, delayFrames(track, sr))
                                                     : std::max(0, st.needOut[ti] - st.latency[ti])) +
                         (first >= 2 && st.dest[ti] < 0 ? master : 0);
            patch::DelayLine* line = nullptr;
            for (auto& d : st.delays)
                if (d.first == &track) line = &d.second;
            if (line->frames != wanted || line->channels != channels) line->init(wanted, channels);
            st.lines[ti] = line;
        }
        // pre-fader sends (mixer.h): a second line for the tracks that have one, as long as their main one
        for (size_t ti = 0; ti < n; ti++) {
            const Track& track = *tracks[ti];
            if (!hasPreFaderSend(track)) continue;
            bool known = false;
            for (auto& d : st.preDelays)
                if (d.first == &track) known = true;
            if (!known) st.preDelays.emplace_back(&track, patch::DelayLine());
        }
        for (size_t ti = 0; ti < n; ti++) {  // (pointers taken once `preDelays` stops growing)
            const patch::DelayLine* main = st.lines[ti];
            int length = main->frames + st.sendExtra[ti];  // (a bus's track: its sends later still)
            for (auto& d : st.preDelays)
                if (d.first == tracks[ti].get()) {
                    if (d.second.frames != length || d.second.channels != main->channels)
                        d.second.init(length, main->channels);
                    st.preLines[ti] = &d.second;
                }
        }
        // post-fader sends of a track playing into a bus that makes it later: a line of their own (GROUP BUSES)
        for (size_t ti = 0; ti < n; ti++) {
            const Track& track = *tracks[ti];
            if (!st.sendExtra[ti] || !hasPostFaderSend(track)) continue;
            bool known = false;
            for (auto& d : st.sendDelays)
                if (d.first == &track) known = true;
            if (!known) st.sendDelays.emplace_back(&track, patch::DelayLine());
        }
        for (size_t ti = 0; ti < n; ti++) {  // (pointers taken once `sendDelays` stops growing)
            if (!st.sendExtra[ti]) continue;
            const patch::DelayLine* main = st.lines[ti];
            for (auto& d : st.sendDelays)
                if (d.first == tracks[ti].get()) {
                    if (d.second.frames != st.sendExtra[ti] || d.second.channels != main->channels)
                        d.second.init(st.sendExtra[ti], main->channels);
                    st.sendLines[ti] = &d.second;
                }
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
    st.padFilled.assign(n, 0);

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
    b.tempoMap = tempoMap;

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
        st.jobs.clear();  // every track but the returns, at once: a wave at a time (GROUP BUSES AND ROUTING)
        for (int ti : st.order) {
            if (tracks[ti]->kind == Kind::Return) continue;
            if (!st.jobs.empty() && st.level[ti] != st.level[st.jobs.back()]) {
                runParallel(b);
                st.jobs.clear();
            }
            st.jobs.push_back(ti);
        }
        if (!st.jobs.empty()) runParallel(b);
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
