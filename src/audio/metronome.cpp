#include "audio/metronome.h"

#include <algorithm>
#include <cmath>

namespace metronome {

const char* const WAVEFORMS[4] = {"sine", "triangle", "square", "noise"};

namespace {
const double PI = 3.14159265358979323846;

// The same noise every time (the Python program uses numpy's default_rng(0); any fixed noise does).
struct Noise {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    double uniform(double lo, double hi) {  // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return lo + (hi - lo) * ((double)(z >> 11) * (1.0 / 9007199254740992.0));
    }
};

int64_t msFrames(double ms, int sr) { return std::max<int64_t>(0, (int64_t)std::nearbyint(ms / 1000 * sr)); }
}  // namespace

std::vector<float> renderClick(const Click& click, int sr) {
    int64_t a = msFrames(click.attackMs, sr), s = msFrames(click.sustainMs, sr), r = msFrames(click.releaseMs, sr);
    int64_t n = std::max<int64_t>(1, a + s + r);
    if (a + s + r == 0) return {};  // (numpy: a 1-frame wave times an empty envelope)
    std::vector<float> out((size_t)n);
    double gain = std::pow(10.0, click.levelDb / 20);
    Noise noise;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / sr;
        double phase = std::fmod(t * click.pitch, 1.0);
        if (phase < 0) phase += 1.0;
        double wave;
        if (click.waveform == "triangle")
            wave = 1.0 - 4.0 * std::fabs(phase - 0.5);
        else if (click.waveform == "square")
            wave = (phase < 0.5 ? 1.0 : -1.0) * 0.6;  // a square sounds louder than a sine at the same peak
        else if (click.waveform == "noise")
            wave = noise.uniform(-1.0, 1.0);
        else
            wave = std::sin(2 * PI * phase);
        double env;
        if (i < a)
            env = (double)i / a;  // linspace(0, 1, a, endpoint=False)
        else if (i < a + s)
            env = 1.0;
        else
            env = 1.0 - (double)(i - a - s + 1) / r;  // linspace(1, 0, r + 1)[1:]
        out[(size_t)i] = (float)(wave * env * gain);
    }
    return out;
}

Clicks renderClicks(const MetronomeSettings& settings, int sr) {
    return {renderClick(settings.bar, sr), renderClick(settings.beat, sr)};
}

void addClicks(float* out, int channels, int frames, int64_t start, const Clicks& clicks, const TempoMap& map,
               int sr, double gain, int64_t earliest, std::vector<BeatTime>& beats) {
    int64_t longest = (int64_t)std::max(clicks.bar.size(), clicks.beat.size());
    int width = std::min(2, channels);
    // (tempo and time signature changes: the map's beats)
    map.beatTimes((double)std::max<int64_t>(0, start - longest) / sr, (double)(start + frames - 1) / sr, beats);
    for (const BeatTime& b : beats) {
        int64_t at = (int64_t)std::nearbyint(b.seconds * sr);
        if (at < earliest) continue;
        const std::vector<float>& click = b.barStart ? clicks.bar : clicks.beat;
        int64_t a = std::max(start, at), e = std::min(start + frames, at + (int64_t)click.size());
        for (int64_t f = a; f < e; f++) {
            float v = (float)(click[(size_t)(f - at)] * gain);
            float* o = out + (f - start) * channels;
            for (int c = 0; c < width; c++) o[c] += v;
        }
    }
}

void addClicks(float* out, int channels, int frames, int64_t start, const Clicks& clicks, const Timing& timing,
               int sr, double gain, int64_t earliest) {
    thread_local std::vector<BeatTime> beats;
    if (beats.capacity() < 256) beats.reserve(256);
    TempoMapPtr map = timing.map();
    addClicks(out, channels, frames, start, clicks, *map, sr, gain, earliest, beats);
}

AudioPtr countIn(const MetronomeSettings& settings, const Timing& timing, int bars, int sr, double at,
                 int64_t& frames) {
    frames = 0;
    if (bars <= 0) return nullptr;
    TempoMapPtr map = timing.map();
    int perBar = map->beatsPerBarAt(at);
    int beats = bars * perBar;
    double beatFrames = map->beatSAt(at) * sr;
    frames = (int64_t)std::nearbyint(beats * beatFrames);
    std::vector<float> mono((size_t)std::max<int64_t>(0, frames), 0.0f);
    int meterBar, meterPerBar;
    double meterBeat;
    map->meterAt(map->beatAt(at), meterBar, meterBeat, meterPerBar);
    double now = map->beatAt(at) - meterBeat;  // (beats since the time signature's first bar line)
    double begin = now - beats;  // (where the count-in starts, in those beats)
    Clicks c = renderClicks(settings, sr);
    for (int64_t beat = (int64_t)std::ceil(begin - 1e-6); beat < (int64_t)std::ceil(now - 1e-6); beat++) {
        const std::vector<float>& click = ((beat % perBar) + perBar) % perBar == 0 ? c.bar : c.beat;
        int64_t where = std::max<int64_t>(0, (int64_t)std::nearbyint((beat - begin) * beatFrames));
        int64_t n = std::min<int64_t>((int64_t)click.size(), frames - where);
        for (int64_t k = 0; k < n; k++) mono[(size_t)(where + k)] += click[(size_t)k];
    }
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize(mono.size() * 2);
    for (size_t k = 0; k < mono.size(); k++) out->samples[k * 2] = out->samples[k * 2 + 1] = mono[k];
    return out;
}

}  // namespace metronome
