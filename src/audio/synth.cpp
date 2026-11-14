#include "audio/synth.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <tuple>

namespace synth {

static const double HARMONICS[][2] = {{1, 1.0}, {2, 0.35}, {3, 0.15}};

double noteFrequency(double pitch) { return 440.0 * std::pow(2.0, (pitch - 69) / 12.0); }

namespace {
// each pitch's wave (its harmonics added up, before the envelope), least recently used first
struct Waves {
    std::mutex lock;
    std::list<std::pair<std::pair<double, int>, std::shared_ptr<const std::vector<double>>>> kept;
    size_t bytes = 0;
};
Waves& waves() {
    static Waves w;
    return w;
}

std::shared_ptr<const std::vector<double>> makeWave(double pitch, int64_t n, int sr) {
    auto wave = std::make_shared<std::vector<double>>((size_t)n);
    double f = noteFrequency(pitch);
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / sr, v = 0;
        for (auto& h : HARMONICS)
            if (f * h[0] < sr / 2.0) v += h[1] * std::sin(2 * M_PI * f * h[0] * t);
        (*wave)[(size_t)i] = v;
    }
    return wave;
}
}  // namespace

std::shared_ptr<const std::vector<double>> waveOf(double pitch, int64_t n, int sr) {
    Waves& w = waves();
    std::lock_guard<std::mutex> hold(w.lock);
    auto key = std::make_pair(pitch, sr);
    std::shared_ptr<const std::vector<double>> wave;
    for (auto it = w.kept.begin(); it != w.kept.end(); ++it)
        if (it->first == key) {
            wave = it->second;
            w.kept.erase(it);
            break;
        }
    if (!wave || (int64_t)wave->size() < n) {
        if (n > KEPT_LONGEST_S * sr) {  // (longer notes: their wave is worked out each time)
            if (wave) w.kept.emplace_back(key, wave);
            return makeWave(pitch, n, sr);
        }
        if (wave) w.bytes -= wave->size() * sizeof(double);
        wave = makeWave(pitch, std::max<int64_t>(n, sr), sr);
        w.bytes += wave->size() * sizeof(double);
        while (!w.kept.empty() && w.bytes > KEPT_BYTES) {
            w.bytes -= w.kept.front().second->size() * sizeof(double);
            w.kept.pop_front();
        }
    }
    w.kept.emplace_back(key, wave);  // (now the newest)
    return wave;
}

std::vector<float> renderNote(double pitch, double velocity, double durationS, int sr) {
    int64_t hold = std::max<int64_t>(1, (int64_t)(durationS * sr));
    int64_t n = hold + (int64_t)(RELEASE_S * sr);
    std::vector<float> out((size_t)n);
    auto wave = waveOf(pitch, n, sr);
    double gain = NOTE_GAIN * velocity / 127.0;
    int64_t tail = n - hold;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / sr;
        double env = std::min(1.0, t / ATTACK_S);
        if (i >= hold) {  // np.linspace(1, 0, n - hold)
            int64_t k = i - hold;
            env *= tail > 1 ? 1.0 - (double)k / (double)(tail - 1) : 1.0;
        }
        out[(size_t)i] = (float)(gain * (*wave)[(size_t)i] * env);
    }
    return out;
}

AudioPtr render(const std::vector<Note>& notes, int sr, double lengthS) {
    int64_t len = (int64_t)((lengthS + RELEASE_S) * sr) + 1;
    std::vector<float> mono((size_t)len, 0.0f);
    // a note played again (same pitch, velocity and length: a part's repeats, a drum pattern) is rendered
    // once and added again
    std::map<std::tuple<double, double, int64_t>, std::vector<float>> rendered;
    for (auto& note : notes) {
        int64_t a = (int64_t)(note.start * sr);
        if (a >= len || a < 0) continue;
        auto key = std::make_tuple(note.pitch, note.velocity, std::max<int64_t>(1, (int64_t)((note.end - note.start) * sr)));
        auto it = rendered.find(key);
        if (it == rendered.end())
            it = rendered.emplace(key, renderNote(note.pitch, note.velocity, note.end - note.start, sr)).first;
        const std::vector<float>& wave = it->second;
        int64_t m = std::min<int64_t>((int64_t)wave.size(), len - a);
        for (int64_t i = 0; i < m; i++) mono[(size_t)(a + i)] += wave[(size_t)i];
    }
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize((size_t)len * 2);
    for (int64_t i = 0; i < len; i++) out->samples[2 * i] = out->samples[2 * i + 1] = mono[(size_t)i];
    return out;
}

}  // namespace synth
