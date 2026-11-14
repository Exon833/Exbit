#include "audio/render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "audio/automation.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "config.h"

using namespace plugin_audio;

namespace render {

namespace {
int64_t roundFrames(double seconds, int sr) { return (int64_t)std::nearbyint(seconds * sr); }

// frames [pos, pos + n) of a sound starting at frame `at`, planar into dst[2], silence outside it
void window(const Rendered& sound, int64_t pos, int n, float* const dst[2]) {
    std::memset(dst[0], 0, sizeof(float) * n);
    std::memset(dst[1], 0, sizeof(float) * n);
    int64_t lo = std::max(pos, sound.start), hi = std::min(pos + n, sound.start + sound.frames());
    for (int64_t f = lo; f < hi; f++) {
        const float* s = &sound.audio[(size_t)(f - sound.start) * 2];
        dst[0][f - pos] = s[0];
        dst[1][f - pos] = s[1];
    }
}

// the frame after the last one louder than RENDER_SILENCE in planar [from, size), relative to `from` (0: none)
int64_t loudEnd(const float* l, const float* r, int64_t from, int64_t size) {
    for (int64_t i = size - 1; i >= from; i--)
        if (std::fabs(l[i]) > config::RENDER_SILENCE || std::fabs(r[i]) > config::RENDER_SILENCE) return i - from + 1;
    return 0;
}
}  // namespace

std::vector<std::pair<const Track*, Rendered>> sidechainSources(Project& project, const Track& track, int sr,
                                                                const Seen* seen) {
    std::vector<std::pair<const Track*, Rendered>> found;
    auto cables = track.cables.get();
    auto tracks = project.tracks.get();
    for (auto& other : *tracks) {
        if (!mixer::listensTo(*cables, other.get()) || (seen && seen->contains(other.get()))) continue;
        std::optional<Rendered> rendered;
        if (ClipPtr f = aload(other->frozen)) {
            Rendered r;
            r.start = f->start;
            int64_t n = std::max<int64_t>(0, f->frames());
            std::vector<float> l((size_t)n, 0.0f), rr((size_t)n, 0.0f);
            float* buf[2] = {l.data(), rr.data()};
            mixer::addClip(buf, 2, n, *f, f->start);
            r.audio.resize((size_t)n * 2);
            for (int64_t i = 0; i < n; i++) r.audio[i * 2] = l[i], r.audio[i * 2 + 1] = rr[i];
            rendered = std::move(r);
        } else {
            Seen more{other.get(), seen};
            rendered = renderTrack(project, other, sr, nullptr, &more);
        }
        if (rendered) {
            if (other->volumeDb != 0) {  // (its fader, as it plays)
                float g = (float)dsp::dbToGain(other->volumeDb, config::MIN_DB);
                for (auto& v : rendered->audio) v *= g;
            }
            found.emplace_back(other.get(), std::move(*rendered));
        }
    }
    return found;
}

std::optional<Rendered> renderTrack(Project& project, const TrackPtr& track, int sr, const Progress& progress,
                                    const Seen* seen) {
    std::vector<ClipPtr> clips;
    for (auto& c : project.clipsOn(track))
        if (!c->disabled) clips.push_back(c);
    if (clips.empty()) return std::nullopt;
    bool instrument = playsPlugins(*track);  // its instrument plugins play the notes (else the built-in synth)
    std::vector<SlotPtr> instruments, plugins;
    if (instrument)
        for (auto& s : track->instruments())
            if (usable(s)) instruments.push_back(s);
    plugins = instruments;
    for (auto& s : track->effects)
        if (active(s.get())) plugins.push_back(s);
    // EDGE_FADE_FRAMES of silence on both sides: a clip made of the rendering fades in and out over
    // them (mixer::addClip), so it plays exactly like the track did (unless it starts at 0 s)
    const int64_t pad = config::EDGE_FADE_FRAMES;
    int64_t first = clips[0]->start, lastNote = clips[0]->end();
    for (auto& c : clips) first = std::min(first, c->start), lastNote = std::max(lastNote, c->end());
    int64_t start = std::max<int64_t>(0, first - pad);
    TrackList tracks = project.tracks.copy();
    int late = trackLatency(*track, tracks);  // its plugins hold the sound back up to this long: render that much more, drop it
    int64_t end = lastNote + (plugins.empty() ? 0 : roundFrames(config::RENDER_TAIL_S, sr)) + pad + late;
    const int block = config::RENDER_BLOCK;
    for (auto& slot : plugins) aload(slot->instance)->reset();
    Seen me{track.get(), nullptr};
    auto sources = sidechainSources(project, *track, sr, seen ? seen : &me);
    int inLate = instrumentLatency(*track);
    int64_t total = end - start;
    std::vector<float> L((size_t)total, 0.0f), R((size_t)total, 0.0f);
    patch::PatchState state;  // (its own: the audio thread keeps the track's)
    patch::RackState rackState;
    state.reserve(block);
    rackState.reserve(block);
    std::vector<std::vector<float>> windows(sources.size(), std::vector<float>((size_t)block * 2));
    std::vector<patch::Feed> feeds(sources.size());
    Messages messages;
    std::vector<NoteEvent> events;
    std::vector<const Clip*> clipPtrs;
    for (auto& c : clips) clipPtrs.push_back(c.get());
    auto cables = track->cables.get();
    auto effects = track->effects.get();
    struct Restore {  // (the audio thread starts afresh with its cables; nor does playing hear the tails)
        Track& t;
        std::vector<SlotPtr>& plugins;
        ~Restore() {
            patch::freshState(t, true);
            for (auto& slot : plugins)
                if (auto inst = aload(slot->instance)) inst->reset();
        }
    } restore{*track, plugins};
    TempoMapPtr tempoMap = project.timing.audioMap();
    int i = 0;
    for (int64_t pos = start; pos < end; pos += block, i++) {
        int n = (int)std::min<int64_t>(block, end - pos);
        float* buf[2] = {L.data() + (pos - start), R.data() + (pos - start)};
        // (tempo-synced plugins follow the song's tempo, as they do playing)
        Transport transport = Transport::at(*tempoMap, true, pos, sr);
        automation::apply(*track, pos, sr);
        if (instrument) {
            noteMessages(clipPtrs.data(), clipPtrs.size(), true, pos, n, pos == start, messages, events);
            patch::playInstruments(*track, messages.data(), messages.size(), buf, 2, n, sr, rackState, transport);
            for (auto& slot : instruments)
                if (slot->failed) throw std::runtime_error(slot->name + " stopped: " + slot->error);
        } else {
            for (const Clip* c : clipPtrs) mixer::addClip(buf, 2, n, *c, pos);
        }
        for (size_t k = 0; k < sources.size(); k++) {  // (in time: 0 late)
            float* w[2] = {windows[k].data(), windows[k].data() + block};
            window(sources[k].second, pos, n, w);
            feeds[k] = {sources[k].first, {w[0], w[1]}, 0};
        }
        late = patch::runPatch(cables, *effects, buf, 2, n, sr, state, feeds.data(), feeds.size(), inLate, transport);
        if (progress && i % 32 == 0 && !progress((double)(pos - start) / (double)(end - start))) return std::nullopt;
    }
    // (as late as its cables said) in time with the clips, as delay compensation plays it
    int64_t from = std::min<int64_t>(late, total);
    int64_t size = total - from;
    // cut the tail where it has died away (keeping the silent pad)
    int64_t keepTo = lastNote - start;
    int64_t loud = keepTo < size ? loudEnd(L.data() + from, R.data() + from, keepTo, size) : 0;
    int64_t length = std::min(size, keepTo + loud + pad);
    Rendered out;
    out.start = start;
    out.audio.resize((size_t)std::max<int64_t>(0, length) * 2);
    for (int64_t k = 0; k < length; k++) out.audio[k * 2] = L[from + k], out.audio[k * 2 + 1] = R[from + k];
    return out;
}

void clearPatchStates(Project& project) {
    for (auto& t : project.tracks) patch::freshState(*t, false);
    patch::freshMasterState(project);
}

std::optional<std::vector<float>> renderMix(Project& project, int64_t start, int64_t end, int sr, double masterDb,
                                            bool tail, const Progress& progress, const Track* only) {
    std::vector<SlotPtr> slots;
    for (auto& slot : project.pluginSlots())
        if (usable(slot)) slots.push_back(slot);
    for (auto& slot : slots) aload(slot->instance)->reset();  // never start with what they were playing live
    clearPatchStates(project);
    struct Clear {
        Project& p;
        std::vector<SlotPtr>& slots;
        ~Clear() {
            clearPatchStates(p);
            for (auto& slot : slots)  // (nor does playing hear the rendering's reverb and delay tails)
                if (auto inst = aload(slot->instance)) inst->reset();
        }
    } clear{project, slots};
    int64_t first = std::max<int64_t>(0, start - roundFrames(config::RENDER_PREROLL_S, sr));
    int64_t extra = tail ? roundFrames(config::RENDER_TAIL_S, sr) : 0;
    const int block = config::RENDER_BLOCK;
    int latency = 0;
    int64_t total = std::max<int64_t>(0, end - first + extra);
    std::vector<float> out((size_t)total * 2, 0.0f);
    mixer::MixState st;
    st.compensate = true;
    int64_t pos = first;
    // until the end, plus the time plugins with look-ahead hold the sound back (known as it plays)
    while (pos - first < total + latency) {
        int n = block;
        if ((size_t)(pos - first + n) * 2 > out.size()) out.resize(out.size() + (size_t)n * 2, 0.0f);
        latency = mixer::mixClips(project, out.data() + (pos - first) * 2, 2, n, true, pos, st, nullptr, sr,
                                  pos == first, true, nullptr, only);
        pos += n;
        if (progress && (pos - first) / n % 32 == 0 &&
            !progress(std::min(1.0, (double)(pos - first) / (double)(total + latency))))
            return std::nullopt;
    }
    int64_t frames = (int64_t)out.size() / 2;
    int64_t from = std::min(frames, start - first + latency);  // what is heard at `start` comes out `latency` later
    int64_t length = std::min(frames - from, end - start + extra);
    if (tail) {  // cut where it has died away
        int64_t keep = end - start, loud = 0;
        for (int64_t k = length - 1; k >= keep; k--) {
            const float* f = &out[(size_t)(from + k) * 2];
            if (std::fabs(f[0]) > config::RENDER_SILENCE || std::fabs(f[1]) > config::RENDER_SILENCE) {
                loud = k - keep + 1;
                break;
            }
        }
        length = std::min(length, keep + loud);
    }
    length = std::max<int64_t>(0, length);
    std::vector<float> result(out.begin() + from * 2, out.begin() + (from + length) * 2);
    if (masterDb != 0) {
        float g = (float)dsp::dbToGain(masterDb, config::MIN_DB);
        for (auto& v : result) v *= g;
    }
    return result;
}

void keepMix(const Track& from, Track& to) {
    to.volumeDb = from.volumeDb, to.pan = from.pan, to.output = from.output;
    to.delayMs = from.delayMs, to.sends = from.sends;
    std::vector<AutomationPtr> lanes;
    for (auto& a : from.automation)
        if (a->slot == from.controls) {
            auto lane = std::make_shared<Automation>();
            lane->slot = to.controls;
            lane->param = a->param;
            lane->points = a->points;
            lane->height = a->height;
            lanes.push_back(lane);
        }
    to.automation = lanes;
    to.showAutomation = from.showAutomation && !lanes.empty();
}

}  // namespace render
