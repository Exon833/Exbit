#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/automation.h"
#include "config.h"

using namespace plugin_audio;

namespace mixer {

void MixState::reserve(int frames, int tracks) {
    if (frames > capacity) {
        capacity = frames;
        sig.assign((size_t)frames * 2, 0.0f);
        master.assign((size_t)frames * 2, 0.0f);
        for (auto& f : feedBuf) f.assign((size_t)frames * 2, 0.0f);
        for (auto& f : sentBuf) f.assign((size_t)frames * 2, 0.0f);
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
        while ((int)feedBuf.size() < tracks) feedBuf.emplace_back((size_t)capacity * 2, 0.0f);
        while ((int)sentBuf.size() < tracks) sentBuf.emplace_back((size_t)capacity * 2, 0.0f);
    }
    if (active.capacity() < 1024) active.reserve(1024);
    if (clips.capacity() < 256) clips.reserve(256);
    if (messages.capacity() < 2048) messages.reserve(2048);
    if (events.capacity() < 2048) events.reserve(2048);
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
    float* sig[2] = {st.sig.data(), st.sig.data() + cap};
    for (int ti : st.order) {
        Track& track = *tracks[ti];
        bool isReturn = track.kind == Kind::Return;
        bool into = isReturn && st.sentUsed[ti];  // (a return's input: what the tracks send it)
        st.clips.clear();
        for (auto& a : st.active)
            if (a.track == &track) st.clips.push_back(a.clip);
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
        if (frozen) {  // frozen: its rendering plays instead of its clips and plugins
            st.clips.clear();
            if (hasStart && frozen->start < start + frames && frozen->end() > start) st.clips.push_back(frozen.get());
            vstInstrument = instrument = hasPlugins = false;
        }
        int first = track.output < outChannels ? track.output : 0;  // device lacks that output: use 1/2
        int channels = std::min(2, outChannels - first);
        patch::DelayLine* line = nullptr;
        if (st.compensate) {  // hold it back to line up with the latest track (a return: by its own delay)
            int wanted = (isReturn ? (int)std::max<int64_t>(0, delayFrames(track, sr)) : latest - st.latency[ti]) +
                         (first >= 2 ? master : 0);
            for (auto& d : st.delays)
                if (d.first == &track) line = &d.second;
            if (!line) {
                st.delays.emplace_back(&track, patch::DelayLine());
                line = &st.delays.back().second;
                line->init(wanted, channels);
            } else if (line->frames != wanted || line->channels != channels) {
                line->init(wanted, channels);  // (rare: a latency or delay changed)
            }
            if (restart) line->clear();  // what it held belongs to where playback was
        }
        bool audible = only ? !track.needsInstrument && (&track == only || [&] {  // (a stem: it, and what sends to it)
            auto sends = track.sends.get();
            for (auto& s : *sends)
                if (s.track.get() == only) return true;
            return false;
        }())
                            : Project::audibleIn(track, tracks);
        bool feeds = st.feeder[ti] && plugins;  // (a sidechain listens to it: worked out even if not heard)
        if (!audible && line) line->clear();
        live::LiveNotes* notes = live::find(live, &track);
        if (notes && (!audible || frozen || (vstInstrument && !plugins))) notes->clear();  // nothing to hear them through now
        bool playingLive = notes && notes->active();
        if (!(audible || feeds) ||
            !(!st.clips.empty() || hasPlugins || playingLive || into || (line && line->sounding)) ||
            (vstInstrument && !plugins)) {
            if (tap) tap->silent(&track, frames);
            continue;
        }
        std::memset(sig[0], 0, sizeof(float) * frames);
        std::memset(sig[1], 0, sizeof(float) * frames);
        if (instrument) {  // keeps rendering even without notes, so releases ring out
            uint16_t* held = nullptr;
            for (auto& h : st.held)
                if (h.first == &track) held = h.second.data();
            if (!held) {
                st.held.emplace_back(&track, std::array<uint16_t, 128>{});
                held = st.held.back().second.data();
            }
            noteMessages(st.clips.data(), st.clips.size(), hasStart, start, frames, restart, st.messages, st.events,
                         held);
            if (notes) notes->take(frames, sr, st.messages);
            auto rack = patch::rackStateOf(track.rackState);
            patch::playInstruments(track, st.messages.data(), st.messages.size(), sig, channels, frames, sr, *rack,
                                   transport);
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
            for (const Clip* clip : st.clips) addClip(sig, channels, frames, *clip, start);
            if (into) {
                const float* in = st.sentBuf[ti].data();
                for (int c = 0; c < channels; c++)
                    for (int i = 0; i < frames; i++) sig[c][i] += in[(size_t)c * cap + i];
            }
            if (notes) {
                st.messages.clear();
                notes->take(frames, sr, st.messages);
                notes->addVoices(sig, channels, frames, sr);
            }
        }
        if (plugins && !frozen) {
            auto state = patch::stateOf(track.patchState);
            patch::runPatch(st.cables[ti], *effects, sig, channels, frames, sr, *state, st.feeds.data(), st.feeds.size(),
                            instrumentLatency(track), transport);
        }
        applyFader(sig, channels, frames, track, hasStart, start, sr);  // its fader and pan, after its effects
        if (feeds) {
            std::vector<float>& fb = st.feedBuf[ti];
            if ((int)fb.size() < cap * 2) fb.assign((size_t)cap * 2, 0.0f);  // (rare)
            std::memcpy(fb.data(), sig[0], sizeof(float) * frames);
            std::memcpy(fb.data() + cap, channels == 2 ? sig[1] : sig[0], sizeof(float) * frames);
            st.feeds.push_back({&track, {fb.data(), fb.data() + cap}, st.latency[ti]});
        }
        if (!audible) {
            if (tap) tap->silent(&track, frames);
            continue;
        }
        if (line) line->process(sig, frames);
        auto sends = track.sends.get();
        for (auto& send : *sends) {  // (to returns: after the fader and pan, in time with the others)
            if (!(send.db > config::MIN_DB) || !send.track || send.track->kind != Kind::Return) continue;
            int target = -1;
            for (size_t j = 0; j < n; j++)
                if (tracks[j] == send.track) target = (int)j;
            if (target < 0) continue;
            std::vector<float>& buf = st.sentBuf[target];
            if ((int)buf.size() < cap * 2) buf.assign((size_t)cap * 2, 0.0f);  // (rare)
            if (!st.sentUsed[target]) {
                std::memset(buf.data(), 0, sizeof(float) * frames);
                std::memset(buf.data() + cap, 0, sizeof(float) * frames);
                st.sentUsed[target] = 1;
            }
            float g = (float)dsp::dbToGain(send.db, config::MIN_DB);
            for (int c = 0; c < channels; c++) {
                float* b = buf.data() + (size_t)c * cap;
                for (int i = 0; i < frames; i++) b[i] += sig[c][i] * g;
            }
        }
        if (tap) tap->sound(&track, sig, channels, first, frames);
        if (only && &track != only) continue;
        for (int c = 0; c < channels; c++) {
            float* o = out + first + c;
            const float* s = sig[c];
            for (int i = 0; i < frames; i++) o[(size_t)i * outChannels] += s[i];
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
