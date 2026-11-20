#include "audio/sampler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

#include "audio/padkit.h"
#include "audio/synth.h"

namespace sampler {

const char* const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int pitch) {
    int octave = (pitch >= 0 ? pitch / 12 : (pitch - 11) / 12) - 1;
    return std::string(NOTE_NAMES[((pitch % 12) + 12) % 12]) + std::to_string(octave);
}

int rootFromName(const std::string& name) {
    // the last match of (?<![A-Za-z])([A-G])(#|b)?(-?\d)(?!\d)
    auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    bool found = false;
    int pitch = 60;
    size_t n = name.size();
    for (size_t i = 0; i < n; i++) {
        char letter = name[i];
        if (letter < 'A' || letter > 'G' || (i > 0 && alpha(name[i - 1]))) continue;
        static const int index[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
        auto tryAt = [&](size_t j, int accidental) -> bool {
            int sign = 1;
            if (j < n && name[j] == '-' && j + 1 < n && digit(name[j + 1])) sign = -1, j++;
            if (j >= n || !digit(name[j])) return false;
            if (j + 1 < n && digit(name[j + 1])) return false;
            int octave = sign * (name[j] - '0');
            pitch = std::min(127, std::max(0, index[letter - 'A'] + accidental + (octave + 1) * 12));
            found = true;
            return true;
        };
        size_t j = i + 1;
        if (j < n && (name[j] == '#' || name[j] == 'b') && tryAt(j + 1, name[j] == '#' ? 1 : -1)) continue;
        tryAt(j, 0);
    }
    return found ? pitch : 60;
}

namespace {
std::mutex cacheLock;
std::map<std::pair<const AudioBuffer*, int>, std::pair<AudioPtr, AudioPtr>> cache;  // (source, rate) -> (source, at rate)
}  // namespace

AudioPtr sampleAt(const SamplerSettings& s, int sr) {
    if (!s.sample || !s.sample->source) return nullptr;
    const ClipPtr& clip = s.sample;
    std::lock_guard<std::mutex> hold(cacheLock);
    auto key = std::make_pair(clip->source.get(), sr);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == clip->source) return it->second.second;
    if (cache.size() > 32) cache.clear();
    AudioPtr data = dsp::resample(clip->source, clip->sourceSr > 0 ? clip->sourceSr : sr, sr);
    cache[key] = {clip->source, data};
    return data;
}

std::vector<float> envelope(int64_t n, int64_t held, const SamplerSettings& s, int sr) {
    std::vector<double> level((size_t)std::max<int64_t>(0, n));
    for (int64_t k = 0; k < n; k++) {
        double t = (double)k / sr;
        double v = s.attack > 0 ? std::min(1.0, t / s.attack) : 1.0;
        double after = t - s.attack;
        if (s.decay > 0) {
            if (after > 0) v = v * (1 - (1 - s.sustain) * std::min(1.0, std::max(0.0, after / s.decay)));
        } else if (after > 0) {
            v = s.sustain;
        }
        level[(size_t)k] = v;
    }
    if (!s.oneShot && held < n) {  // let go: down to nothing over the release
        double at = level[(size_t)std::max<int64_t>(0, held - 1)];
        double release = std::max(s.release, 1e-4);
        for (int64_t k = held; k < n; k++)
            level[(size_t)k] = at * std::min(1.0, std::max(0.0, 1 - ((double)(k - held) / sr) / release));
    }
    return std::vector<float>(level.begin(), level.end());
}

std::vector<float> renderNote(const SamplerSettings& s, int pitch, double velocity, double durationS, int sr,
                              double tailS, double cents) {
    AudioPtr data = sampleAt(s, sr);
    int64_t len = data ? data->frames() : 0;
    if (len < 3) return {};  // (nothing to play)
    // the part played: at least a frame long, whatever start and end say (a short sample, start at 99.9 %)
    int64_t first = std::min((int64_t)(s.start * len), len - 2);
    int64_t last = std::min(std::max((int64_t)(s.end * len), first + 2), len - 1);
    double ratio = std::pow(2.0, (pitch - s.root + s.transpose + cents / 100) / 12.0);
    int64_t held = std::max<int64_t>(1, (int64_t)(durationS * sr));
    int64_t toEnd = (int64_t)((double)(last - first) / ratio);
    int64_t n;
    if (s.oneShot)
        n = toEnd;
    else if (s.loop)
        n = held + (int64_t)(s.release * sr);
    else
        n = std::min(toEnd, held + (int64_t)(s.release * sr));
    if (tailS >= 0) n = std::min(n, held + (int64_t)(tailS * sr));
    if (n <= 0) return {};
    std::vector<float> level = envelope(n, held, s, sr);
    float gain = (float)(std::pow(10.0, s.gainDb / 20) * (1 - s.velocity + s.velocity * velocity / 127));
    for (auto& v : level) v *= gain;
    int64_t edge = std::min(n, (int64_t)(EDGE_S * sr));
    for (int64_t k = 0; k < edge; k++)  // np.linspace(1, 0, edge)
        level[(size_t)(n - edge + k)] *= (float)(edge > 1 ? 1.0 - (double)k / (double)(edge - 1) : 1.0);
    std::vector<float> out((size_t)n * 2);
    bool loops = s.loop && !s.oneShot;
    double span = (double)std::max<int64_t>(1, last - first);
    const float* d = data->samples.data();
    for (int64_t k = 0; k < n; k++) {
        double pos = (double)first + (double)k * ratio;
        if (loops && pos >= (double)last) pos = (double)first + std::fmod(pos - (double)first, span);
        pos = std::min(pos, (double)(last - 1));
        int64_t i = (int64_t)pos;
        float frac = (float)(pos - (double)i);
        for (int c = 0; c < 2; c++) {
            float a = d[i * 2 + c], b = d[(i + 1) * 2 + c];
            float sound = a * (1.0f - frac) + b * frac;
            out[(size_t)k * 2 + c] = sound * level[(size_t)k];
        }
    }
    return out;
}

namespace {
double heardPitch(const Note& n) { return (int)n.pitch + n.fine / 100; }  // (its fine tuning in cents)

// One voice of the sampler playing `voice` (indexes into notes, in time order: a note, then the sliding notes
// that took its voice over), starting at `fromPitch` (the pitch the first note slides from; its own: none):
// the sample plays on from where it is while its speed glides to each next note's pitch (synth's renderVoice,
// for the sampler), its level and pan gliding too. Stereo interleaved, at most `most` frames; tailS as in
// renderNote.
std::vector<float> renderVoice(const SamplerSettings& s, const std::vector<Note>& notes, const std::vector<int>& voice,
                               double fromPitch, int sr, double tailS, int64_t most) {
    AudioPtr data = sampleAt(s, sr);
    int64_t len = data ? data->frames() : 0;
    if (len < 3 || most <= 0) return {};
    int64_t first = std::min((int64_t)(s.start * len), len - 2);
    int64_t last = std::min(std::max((int64_t)(s.end * len), first + 2), len - 1);
    const Note& head = notes[(size_t)voice.front()];
    int64_t a0 = (int64_t)(head.start * sr);
    int64_t held = std::max<int64_t>(1, (int64_t)((notes[(size_t)voice.back()].end - head.start) * sr));
    int64_t limit = s.oneShot ? most : held + (int64_t)(s.release * sr);
    if (tailS >= 0) limit = std::min(limit, held + (int64_t)(tailS * sr));
    limit = std::min(limit, most);
    struct Part { int64_t at; double pitch, gain, pan; };
    std::vector<Part> parts;
    double level = std::pow(10.0, s.gainDb / 20);
    for (int v : voice) {
        const Note& note = notes[(size_t)v];
        parts.push_back({std::max<int64_t>(0, (int64_t)(note.start * sr) - a0), heardPitch(note),
                         level * (1 - s.velocity + s.velocity * note.velocity / 127), note.pan});
    }
    // where in the sample each frame plays, and its gain and pan, until the sample's end (or the limit)
    bool loops = s.loop && !s.oneShot;
    double span = (double)std::max<int64_t>(1, last - first);
    double glide = std::max(1.0, note_props::GLIDE_S * sr);
    std::vector<double> at, gains, pans;
    size_t part = 0;
    double pitchFrom = fromPitch, gainFrom = parts[0].gain, panFrom = parts[0].pan;
    double pitch = fromPitch, gain = parts[0].gain, pan = parts[0].pan;
    double pos = (double)first, lastPitch = -1e9, ratio = 1;
    for (int64_t k = 0; k < limit; k++) {
        if (loops && pos >= (double)last) pos = (double)first + std::fmod(pos - (double)first, span);
        if (!loops && pos >= (double)last) break;  // (the sample's end)
        while (part + 1 < parts.size() && k >= parts[part + 1].at) {
            part++;
            pitchFrom = pitch, gainFrom = gain, panFrom = pan;
        }
        const Part& p = parts[part];
        double along = std::min(1.0, (double)(k - p.at) / glide);
        pitch = pitchFrom + (p.pitch - pitchFrom) * along;
        gain = gainFrom + (p.gain - gainFrom) * along;
        pan = panFrom + (p.pan - panFrom) * along;
        if (pitch != lastPitch) ratio = std::pow(2.0, ((lastPitch = pitch) - s.root + s.transpose) / 12.0);
        at.push_back(pos);
        gains.push_back(gain);
        pans.push_back(pan);
        pos += ratio;
    }
    int64_t n = (int64_t)at.size();
    if (n <= 0) return {};
    std::vector<float> env = envelope(n, held, s, sr);
    int64_t edge = std::min(n, (int64_t)(EDGE_S * sr));
    for (int64_t k = 0; k < edge; k++)
        env[(size_t)(n - edge + k)] *= (float)(edge > 1 ? 1.0 - (double)k / (double)(edge - 1) : 1.0);
    std::vector<float> out((size_t)n * 2);
    const float* d = data->samples.data();
    double l = 1, r = 1, lastPan = -1e9;
    for (int64_t k = 0; k < n; k++) {
        double q = std::min(at[(size_t)k], (double)(last - 1));
        int64_t i = (int64_t)q;
        float frac = (float)(q - (double)i);
        if (pans[(size_t)k] != lastPan) note_props::panGains(lastPan = pans[(size_t)k], l, r);
        float g = (float)(env[(size_t)k] * gains[(size_t)k]);
        for (int c = 0; c < 2; c++) {
            float sound = d[i * 2 + c] * (1.0f - frac) + d[(i + 1) * 2 + c] * frac;
            out[(size_t)k * 2 + c] = (float)(sound * g * (c == 0 ? l : r));
        }
    }
    return out;
}
}  // namespace

AudioPtr render(const SamplerSettings& s, const std::vector<Note>& notes, int sr, double lengthS) {
    // (one-shot: every note plays to the sample's end, however short it is: only the clip's tail limits it)
    double tail = s.oneShot ? MAX_TAIL_S : std::min(MAX_TAIL_S, std::max(synth::RELEASE_S, s.release));
    int64_t total = (int64_t)((lengthS + tail) * sr) + 1;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.assign((size_t)std::max<int64_t>(0, total) * 2, 0.0f);
    // the voices sliding notes carry on (note_props::slides): a note taken over is played by its voice
    std::vector<note_props::Slide> slid = note_props::slides(notes);
    std::vector<int> takenBy(notes.size(), -1);
    for (size_t i = 0; i < notes.size(); i++)
        if (slid[i].legato) takenBy[(size_t)slid[i].from] = (int)i;
    // (a note played again is rendered once)
    std::map<std::tuple<int, double, int64_t, double>, std::vector<float>> rendered;
    for (size_t j = 0; j < notes.size(); j++) {
        const Note& note = notes[j];
        if (slid[j].legato) continue;  // (played by the voice it took over)
        int64_t a = (int64_t)(note.start * sr);
        if (a >= total) continue;
        if (takenBy[j] >= 0 || slid[j].from >= 0) {  // a voice gliding from note to note
            if (a < 0) continue;
            std::vector<int> voice{(int)j};
            while (takenBy[(size_t)voice.back()] >= 0 && voice.size() <= notes.size())
                voice.push_back(takenBy[(size_t)voice.back()]);
            double from = slid[j].from >= 0 ? heardPitch(notes[(size_t)slid[j].from]) : heardPitch(note);
            std::vector<float> sound = renderVoice(s, notes, voice, from, sr, s.oneShot ? -1 : tail, total - a);
            float* o = out->samples.data() + a * 2;
            for (size_t k = 0; k < sound.size(); k++) o[k] += sound[k];
            continue;
        }
        auto key = std::make_tuple((int)note.pitch, note.velocity,
                                   std::max<int64_t>(1, (int64_t)((note.end - note.start) * sr)), note.fine);
        auto it = rendered.find(key);
        if (it == rendered.end())
            it = rendered
                     .emplace(key, renderNote(s, (int)note.pitch, note.velocity, note.end - note.start, sr,
                                              s.oneShot ? -1 : tail, note.fine))
                     .first;
        const std::vector<float>& sound = it->second;
        int64_t frames = std::min<int64_t>((int64_t)sound.size() / 2, total - a);
        if (a < 0) continue;  // (a note before the clip's start: never made by the model)
        float* o = out->samples.data() + a * 2;
        if (note.pan == 0) {
            for (int64_t k = 0; k < frames * 2; k++) o[k] += sound[(size_t)k];
        } else {  // (its balance: the sample's left and right, each turned down as the track's pan does)
            double l, r;
            note_props::panGains(note.pan, l, r);
            for (int64_t k = 0; k < frames; k++) {
                o[2 * k] += (float)(sound[(size_t)(2 * k)] * l);
                o[2 * k + 1] += (float)(sound[(size_t)(2 * k + 1)] * r);
            }
        }
    }
    return out;
}

double loopPeriod(const SamplerSettings& s, int pitch, int sr) {
    AudioPtr data = sampleAt(s, sr);
    int64_t len = data ? data->frames() : 0;
    if (len < 3) return -1;
    int64_t first = std::min((int64_t)(s.start * len), len - 2);
    int64_t last = std::min(std::max((int64_t)(s.end * len), first + 2), len - 1);
    return (double)std::max<int64_t>(1, last - first) / std::pow(2.0, (pitch - s.root + s.transpose) / 12.0);
}

AudioPtr renderNotes(const Track& track, const std::vector<Note>& notes, int sr, double lengthS) {
    PadKitPtr kit = std::atomic_load(&track.padKit);
    if (kit && track.playsPadKit()) return padkit::render(*kit, notes, sr, lengthS);
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (s && track.playsSampler()) return render(*s, notes, sr, lengthS);
    return synth::render(notes, sr, lengthS);
}

live::NoteRender noteRenderer(const Track& track) {
    live::NoteRender r;
    PadKitPtr kit = std::atomic_load(&track.padKit);
    if (kit && track.playsPadKit()) return padkit::noteRenderer(kit);  // (each key its pad)
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (!s || !track.playsSampler()) return r;  // (the built-in synth)
    r.render = [s](int pitch, int velocity, double durationS, int sr) {
        return renderNote(*s, pitch, velocity, durationS, sr);
    };
    r.channels = 2;
    r.oneShot = s->oneShot;  // (such a note plays to its end, letting go of the key or not)
    r.sustains = s->loop;    // (a held note loops; else it plays the sample to its end)
    r.velocityFloor = 1 - s->velocity;  // (renderNote's level: 1 - velocity + velocity * v / 127)
    r.period = [s](int pitch, int sr) { return loopPeriod(*s, pitch, sr); };
    return r;
}

void install() {
    clip_hooks::renderSampler = [](const Track& track, const std::vector<Note>& notes, int sr, double lengthS) {
        PadKitPtr kit = std::atomic_load(&track.padKit);
        if (kit && track.playsPadKit()) return padkit::render(*kit, notes, sr, lengthS);
        SamplerPtr s = std::atomic_load(&track.sampler);
        return s ? render(*s, notes, sr, lengthS) : synth::render(notes, sr, lengthS);
    };
}

}  // namespace sampler
