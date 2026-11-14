#include "audio/sampler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

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
                              double tailS) {
    AudioPtr data = sampleAt(s, sr);
    int64_t len = data ? data->frames() : 0;
    if (len < 3) return {};  // (nothing to play)
    // the part played: at least a frame long, whatever start and end say (a short sample, start at 99.9 %)
    int64_t first = std::min((int64_t)(s.start * len), len - 2);
    int64_t last = std::min(std::max((int64_t)(s.end * len), first + 2), len - 1);
    double ratio = std::pow(2.0, (pitch - s.root + s.transpose) / 12.0);
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

AudioPtr render(const SamplerSettings& s, const std::vector<Note>& notes, int sr, double lengthS) {
    // (one-shot: every note plays to the sample's end, however short it is: only the clip's tail limits it)
    double tail = s.oneShot ? MAX_TAIL_S : std::min(MAX_TAIL_S, std::max(synth::RELEASE_S, s.release));
    int64_t total = (int64_t)((lengthS + tail) * sr) + 1;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.assign((size_t)std::max<int64_t>(0, total) * 2, 0.0f);
    std::map<std::tuple<int, double, int64_t>, std::vector<float>> rendered;  // (a note played again is rendered once)
    for (auto& note : notes) {
        int64_t a = (int64_t)(note.start * sr);
        if (a >= total) continue;
        auto key = std::make_tuple((int)note.pitch, note.velocity,
                                   std::max<int64_t>(1, (int64_t)((note.end - note.start) * sr)));
        auto it = rendered.find(key);
        if (it == rendered.end())
            it = rendered
                     .emplace(key, renderNote(s, (int)note.pitch, note.velocity, note.end - note.start, sr,
                                              s.oneShot ? -1 : tail))
                     .first;
        const std::vector<float>& sound = it->second;
        int64_t frames = std::min<int64_t>((int64_t)sound.size() / 2, total - a);
        if (a < 0) continue;  // (a note before the clip's start: never made by the model)
        float* o = out->samples.data() + a * 2;
        for (int64_t k = 0; k < frames * 2; k++) o[k] += sound[(size_t)k];
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
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (s && track.playsSampler()) return render(*s, notes, sr, lengthS);
    return synth::render(notes, sr, lengthS);
}

live::NoteRender noteRenderer(const Track& track) {
    live::NoteRender r;
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (!s || !track.playsSampler()) return r;  // (the built-in synth)
    r.render = [s](int pitch, int velocity, double durationS, int sr) {
        return renderNote(*s, pitch, velocity, durationS, sr);
    };
    r.channels = 2;
    r.oneShot = s->oneShot;  // (such a note plays to its end, letting go of the key or not)
    r.sustains = s->loop;    // (a held note loops; else it plays the sample to its end)
    r.period = [s](int pitch, int sr) { return loopPeriod(*s, pitch, sr); };
    return r;
}

void install() {
    clip_hooks::renderSampler = [](const Track& track, const std::vector<Note>& notes, int sr, double lengthS) {
        SamplerPtr s = std::atomic_load(&track.sampler);
        return s ? render(*s, notes, sr, lengthS) : synth::render(notes, sr, lengthS);
    };
}

}  // namespace sampler
