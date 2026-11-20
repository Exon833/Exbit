// The Pad Kit: rendering it, playing it live, slicing a sound into one (padkit.h).
#include "audio/padkit.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "audio/file_io.h"
#include "audio/sampler.h"
#include "audio/synth.h"
#include "audio/workers.h"
#include "core/util.h"

namespace padkit {

std::string padName(const Pad& pad) {
    if (pad.empty()) return "";
    const std::string& name = pad.sound.sample->name;
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

int padFor(const PadKit& kit, int pitch) {
    for (size_t i = 0; i < kit.pads.size(); i++)
        if (kit.pads[i].note == pitch && !kit.pads[i].empty()) return (int)i;
    return -1;
}

Pad newPad(const ClipPtr& sample, int note) {
    Pad pad;
    pad.sound.sample = sample;
    pad.sound.oneShot = true;
    pad.sound.attack = 0;  // (a drum's hit as recorded: its very first sample at full level)
    pad.note = std::min(127, std::max(0, note));
    return pad;
}

PadKitPtr withPad(const PadKit& kit, int index, const Pad& pad) {
    auto out = std::make_shared<PadKit>(kit);
    if (index >= 0 && index < (int)out->pads.size()) out->pads[(size_t)index] = pad;
    return out;
}

ClipPtr loadSample(const std::string& path, std::string& error) {
    AudioPtr data;
    int sr = 0;
    if (!file_io::loadAudio(path, data, sr, error)) return nullptr;
    auto clip = std::make_shared<Clip>();
    clip->kind = Kind::Audio;
    clip->name = util::baseName(path);
    clip->source = data;
    clip->sourceSr = sr;
    return clip;
}

PadKitPtr withFiles(const PadKit& kit, int first, const std::vector<std::string>& paths,
                    std::vector<std::string>& errors) {
    auto out = std::make_shared<PadKit>(kit);
    int at = std::max(0, first), loaded = 0;
    for (const std::string& path : paths) {
        if (at >= (int)out->pads.size()) break;
        if (path.empty() || util::isDir(path)) continue;
        std::string error;
        ClipPtr sample = loadSample(path, error);
        if (!sample) {
            errors.push_back(util::baseName(path) + ": " + error);
            continue;
        }
        Pad& pad = out->pads[(size_t)at];
        Pad made = newPad(sample, pad.note);
        made.choke = pad.choke;  // (the pad's key and choke group stay)
        pad = made;
        at++, loaded++;
    }
    return loaded ? out : nullptr;
}

namespace {
// how long after the clip a pad's notes may sound (sampler::render's own tail)
double tailOf(const SamplerSettings& s) {
    return s.oneShot ? sampler::MAX_TAIL_S : std::min(sampler::MAX_TAIL_S, std::max(synth::RELEASE_S, s.release));
}

void addInto(std::vector<float>& out, const AudioBuffer& sound, int64_t at, int64_t frames) {
    int64_t n = std::min<int64_t>(frames, std::min(sound.frames(), (int64_t)out.size() / 2 - at));
    if (at < 0 || n <= 0) return;
    float* o = out.data() + at * 2;
    const float* d = sound.samples.data();
    for (int64_t k = 0; k < n * 2; k++) o[k] += d[k];
}
}  // namespace

AudioPtr render(const PadKit& kit, const std::vector<Note>& notes, int sr, double lengthS, uint32_t apart) {
    // the notes of each pad, on its root (the sample at its own pitch, `transpose` tuning it)
    std::map<int, std::vector<Note>> byPad;
    for (const Note& n : notes) {
        int pad = padFor(kit, (int)n.pitch);
        if (pad < 0) continue;
        Note m = n;
        m.pitch = kit.pads[(size_t)pad].sound.root;
        byPad[pad].push_back(m);
    }
    double tail = synth::RELEASE_S;
    for (auto& kv : byPad) tail = std::max(tail, tailOf(kit.pads[(size_t)kv.first].sound));
    int64_t total = (int64_t)((lengthS + tail) * sr) + 1;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.assign((size_t)std::max<int64_t>(0, total) * 2, 0.0f);
    // choke groups: when each group's notes start (seconds, sorted; every pad's, routed apart or not)
    std::map<int, std::vector<double>> groupStarts;
    for (auto& kv : byPad) {
        int group = kit.pads[(size_t)kv.first].choke;
        if (group <= 0) continue;
        for (const Note& n : kv.second) groupStarts[group].push_back(n.start);
    }
    for (auto& kv : groupStarts) std::sort(kv.second.begin(), kv.second.end());
    const int64_t fade = std::max<int64_t>(1, (int64_t)(live::CHOKE_S * sr));
    // one pad's sound on its own, as long as the kit's
    auto padSound = [&](int index, const std::vector<Note>& padNotes) {
        auto own = std::make_shared<AudioBuffer>();
        own->samples.assign(out->samples.size(), 0.0f);
        const Pad& pad = kit.pads[(size_t)index];
        std::vector<Note> whole;  // (notes no other note of their choke group cuts off: rendered together)
        for (const Note& n : padNotes) {
            double cut = -1;  // when a later note of its group starts
            if (pad.choke > 0) {
                const std::vector<double>& starts = groupStarts.at(pad.choke);
                auto later = std::upper_bound(starts.begin(), starts.end(), n.start);
                if (later != starts.end()) cut = *later;
            }
            if (cut < 0) {
                whole.push_back(n);
                continue;
            }
            // cut off: rendered alone (as the sampler places it), faded out from the later note's start
            if (n.start < 0 || n.start >= lengthS + tail) continue;
            int64_t at = (int64_t)(n.start * sr);
            if (at >= total) continue;
            Note alone = n;
            alone.start = 0;
            alone.end = n.end - n.start;
            AudioPtr sound = sampler::render(pad.sound, {alone}, sr, lengthS - n.start);
            int64_t stop = (int64_t)(cut * sr) - at;  // (frames into the note, where it is let go)
            int64_t n2 = std::min<int64_t>(std::min(sound->frames(), stop + fade), total - at);
            float* o = own->samples.data() + at * 2;
            const float* d = sound->samples.data();
            for (int64_t k = 0; k < n2; k++) {
                float gain = k < stop ? 1.0f : (float)std::max(0.0, 1.0 - (double)(k - stop) / (double)fade);
                o[2 * k] += d[2 * k] * gain;
                o[2 * k + 1] += d[2 * k + 1] * gain;
            }
        }
        if (!whole.empty()) {
            AudioPtr sound = sampler::render(pad.sound, whole, sr, lengthS);
            addInto(own->samples, *sound, 0, sound->frames());
        }
        return own;
    };
    // Each pad rendered on its own, on every core when there are several (a batch at a time: at most one sound per
    // thread held at once), then the kit's sound summed in pad order: the same, bit for bit, however many threads,
    // and the same as its pads' tracks summed in that order. The pads in `apart` are kept apart (out->pads).
    std::vector<std::pair<int, const std::vector<Note>*>> used;
    for (auto& kv : byPad) used.emplace_back(kv.first, &kv.second);
    int threads = used.size() >= 4 && total > sr && !workers::inJob() ? workers::workersFor(workers::chosenCores()) : 0;
    threads = std::min(threads, (int)used.size() - 1);
    struct Work {
        decltype(padSound)* make;
        const std::vector<std::pair<int, const std::vector<Note>*>>* used;
        size_t from;
        std::vector<std::shared_ptr<AudioBuffer>> made;
    } work{&padSound, &used, 0, {}};
    std::unique_ptr<workers::Pool> pool;
    if (threads > 0) pool = std::make_unique<workers::Pool>(threads, false);  // (as a render's: never outranking playback)
    size_t batch = (size_t)threads + 1;
    for (size_t from = 0; from < used.size(); from += batch) {
        size_t count = std::min(batch, used.size() - from);
        work.from = from;
        work.made.assign(count, nullptr);
        auto job = [](void* ctx, int i) {
            Work& w = *static_cast<Work*>(ctx);
            auto& [index, padNotes] = (*w.used)[w.from + (size_t)i];
            w.made[(size_t)i] = (*w.make)(index, *padNotes);
        };
        if (pool) pool->run((int)count, job, &work);
        else
            for (size_t i = 0; i < count; i++) job(&work, (int)i);
        for (size_t i = 0; i < count; i++) {
            int index = used[from + i].first;
            if (index < 32 && (apart >> index & 1u)) out->pads.emplace_back(index, std::move(work.made[i]));
            else addInto(out->samples, *work.made[i], 0, work.made[i]->frames());
        }
    }
    if (apart) util::debugf("padkit: %zu pads rendered (%d worker threads +1), %zu of them apart for their own tracks\n",
                            used.size(), threads, out->pads.size());
    return out;
}

live::NoteRender noteRenderer(const PadKitPtr& kit) {
    live::NoteRender r;
    if (!kit) return r;
    r.channels = 2;
    r.oneShot = true;  // (every key is played as its pad says: perKey)
    r.render = [kit](int pitch, int velocity, double durationS, int sr) {
        int pad = padFor(*kit, pitch);
        if (pad < 0) return std::vector<float>();
        const SamplerSettings& s = kit->pads[(size_t)pad].sound;
        return sampler::renderNote(s, s.root, velocity, durationS, sr);
    };
    r.perKey = [kit, render = r.render](int pitch, live::NoteRender& out) {
        out = live::NoteRender();
        out.channels = 2;
        out.render = render;
        int pad = padFor(*kit, pitch);
        if (pad < 0) {  // (no pad on that key: nothing plays, and nothing waits for its note-off)
            out.oneShot = true;
            return;
        }
        const Pad& p = kit->pads[(size_t)pad];
        out.oneShot = p.sound.oneShot;
        out.sustains = p.sound.loop;
        out.choke = p.choke;
        out.velocityFloor = 1 - p.sound.velocity;
        out.period = [kit, pad](int, int sr) {
            const SamplerSettings& s = kit->pads[(size_t)pad].sound;
            return sampler::loopPeriod(s, s.root, sr);
        };
    };
    return r;
}

// --- slicing ---

std::vector<float> clipSound(const Clip& clip) {
    std::vector<float> out;
    AudioPtr data = std::atomic_load(&clip.data);
    if (!data || clip.kind != Kind::Audio) return out;
    int64_t from = std::max<int64_t>(0, std::min(clip.offset, data->frames()));
    int64_t n = std::max<int64_t>(0, std::min(clip.frames(), data->frames() - from));
    out.assign(data->samples.begin() + from * 2, data->samples.begin() + (from + n) * 2);
    if (clip.hasLevels())
        for (int64_t k = 0; k < n; k++) {
            float g = clip.level(k);
            out[(size_t)k * 2] *= g;
            out[(size_t)k * 2 + 1] *= g;
        }
    return out;
}

std::vector<int64_t> sliceStarts(const float* stereo, int64_t frames, std::vector<int64_t> hits, int most, float top) {
    if (frames <= 0) return {};
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    std::vector<int64_t> starts;
    for (int64_t h : hits)
        if (h >= 0 && h < frames) starts.push_back(h);
    float before = 0;
    if (top < 0) {
        top = 0;
        for (int64_t i = 0; i < frames * 2; i++) top = std::max(top, std::fabs(stereo[i]));
    }
    for (int64_t i = 0; i < (starts.empty() ? 0 : starts[0]) * 2; i++) before = std::max(before, std::fabs(stereo[i]));
    if (starts.empty() || (starts[0] > 0 && before > 0.01f * top))  // (sound before the first hit: a slice too)
        starts.insert(starts.begin(), 0);
    if ((int)starts.size() > std::max(1, most)) starts.resize((size_t)std::max(1, most));  // (the rest stay in the last slice)
    return starts;
}

std::vector<int64_t> gridStarts(const Clip& clip, int64_t frames, const Timing& timing, int sr, int division) {
    std::vector<int64_t> out;
    if (frames <= 0 || sr <= 0 || division <= 0) return out;
    double steps = 4.0 / division;  // beats (quarter notes) per step
    if (clip.sourceBpm > 0) {       // the clip's own tempo, from its audio's start (its window may start later)
        double step = 60.0 / clip.sourceBpm * std::max(1e-6, clip.stretch) * sr * steps;
        for (int64_t k = (int64_t)std::ceil(clip.offset / step - 1e-9);; k++) {
            int64_t f = std::llround(k * step) - clip.offset;
            if (f >= frames) break;
            if (f >= 0) out.push_back(f);
        }
    } else {  // the song's grid (beats counted on through its tempo changes, so every bar line is a grid line)
        TempoMapPtr map = timing.map();
        double s0 = (double)clip.start / sr, s1 = s0 + (double)frames / sr;
        for (int64_t k = (int64_t)std::ceil(map->beatAt(s0) / steps - 1e-9);; k++) {
            double s = map->secondsAt(k * steps);
            if (s >= s1) break;
            int64_t f = std::llround((s - s0) * sr);
            if (f >= 0 && f < frames && (out.empty() || f > out.back())) out.push_back(f);
        }
    }
    util::debugf("slicing: a 1/%d grid (%s) gives %zu slices", division,
                 clip.sourceBpm > 0 ? "the clip's tempo" : "the song's", out.size());
    return out;
}

int64_t snapToHit(int64_t frame, const std::vector<int64_t>& hits, int64_t reach) {
    auto it = std::lower_bound(hits.begin(), hits.end(), frame);
    int64_t best = frame, gap = reach + 1;
    if (it != hits.end() && *it - frame < gap) best = *it, gap = *it - frame;
    if (it != hits.begin() && frame - *(it - 1) < gap) best = *(it - 1);
    return best;
}

Sliced slice(const float* stereo, int64_t frames, int sr, std::vector<int64_t> hits, const std::string& name) {
    Sliced made;
    made.lengthS = (double)std::max<int64_t>(0, frames) / std::max(1, sr);
    auto kit = std::make_shared<PadKit>();
    made.kit = kit;
    if (frames <= 0 || sr <= 0) return made;
    std::vector<int64_t> starts = sliceStarts(stereo, frames, std::move(hits));
    made.starts = starts;
    for (size_t i = 0; i < starts.size(); i++) {
        int64_t a = starts[i], b = i + 1 < starts.size() ? starts[i + 1] : frames;
        auto audio = std::make_shared<AudioBuffer>();
        audio->samples.assign(stereo + a * 2, stereo + b * 2);
        auto sample = std::make_shared<Clip>();
        sample->kind = Kind::Audio;
        sample->name = name + " slice " + std::to_string(i + 1) + ".wav";
        sample->source = audio;
        sample->sourceSr = sr;
        int note = kit->pads[i].note;
        kit->pads[i] = newPad(sample, note);
        kit->pads[i].sound.velocity = 0;  // (each slice as loud as it was in the sound)
        Note n;
        n.start = (double)a / sr;
        n.end = (double)b / sr;
        n.pitch = note;
        n.velocity = SLICE_VELOCITY;
        made.notes.push_back(n);
    }
    return made;
}

}  // namespace padkit
