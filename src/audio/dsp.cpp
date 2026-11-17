#include "audio/dsp.h"

#include <algorithm>

#include "audio/fft.h"

namespace dsp {

std::shared_ptr<AudioBuffer> toStereo(const float* in, size_t frames, int channels) {
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize(frames * 2);
    float* o = out->samples.data();
    if (channels <= 0) {
        std::fill(out->samples.begin(), out->samples.end(), 0.0f);
    } else if (channels == 1) {
        for (size_t i = 0; i < frames; i++) o[2 * i] = o[2 * i + 1] = in[i];
    } else {
        for (size_t i = 0; i < frames; i++) {
            o[2 * i] = in[i * channels];
            o[2 * i + 1] = in[i * channels + 1];
        }
    }
    return out;
}

AudioPtr resample(const AudioPtr& data, int srcSr, int dstSr) {
    if (!data || srcSr == dstSr || srcSr <= 0 || dstSr <= 0) return data;
    int64_t len = data->frames();
    int64_t n = (int64_t)std::nearbyint((double)len * dstSr / srcSr);
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize((size_t)std::max<int64_t>(0, n) * 2);
    if (len == 0 || n <= 0) return out;
    // np.linspace(0, len - 1, n) and np.interp
    double step = n > 1 ? (double)(len - 1) / (double)(n - 1) : 0.0;
    const float* s = data->samples.data();
    float* o = out->samples.data();
    for (int64_t i = 0; i < n; i++) {
        double x = i * step;
        int64_t j = (int64_t)x;
        if (j >= len - 1) {
            o[2 * i] = s[2 * (len - 1)];
            o[2 * i + 1] = s[2 * (len - 1) + 1];
            continue;
        }
        float t = (float)(x - (double)j);
        o[2 * i] = s[2 * j] + (s[2 * j + 2] - s[2 * j]) * t;
        o[2 * i + 1] = s[2 * j + 1] + (s[2 * j + 3] - s[2 * j + 1]) * t;
    }
    return out;
}

PeaksPtr waveformPeaks(const AudioBuffer& data, int block) {
    auto p = std::make_shared<Peaks>();
    int64_t len = data.frames();
    int64_t blocks = (len + block - 1) / block;
    p->mins.resize((size_t)blocks);
    p->maxs.resize((size_t)blocks);
    const float* s = data.samples.data();
    for (int64_t b = 0; b < blocks; b++) {
        float lo = 1e9f, hi = -1e9f;
        int64_t a = b * block, e = std::min(len, a + block);
        for (int64_t i = a; i < e; i++) {
            float m = (s[2 * i] + s[2 * i + 1]) * 0.5f;
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        if (e - a < block) {  // np.pad with zeros
            lo = std::min(lo, 0.0f);
            hi = std::max(hi, 0.0f);
        }
        p->mins[(size_t)b] = std::clamp(lo, -1.0f, 1.0f);
        p->maxs[(size_t)b] = std::clamp(hi, -1.0f, 1.0f);
    }
    return p;
}

}  // namespace dsp

// --- onsets ---
namespace dsp {

Onsets::Onsets(const float* stereo, int64_t frames, int sr) : sr_(std::max(1, sr)) {
    mono_.resize((size_t)std::max<int64_t>(0, frames));
    for (int64_t i = 0; i < frames; i++) mono_[(size_t)i] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
    if (frames <= 0) return;
    const int n = ONSET_N, half = n / 2, bins = n / 2;
    std::vector<double> window((size_t)n);
    for (int k = 0; k < n; k++) window[(size_t)k] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * k / n);
    // the bands each bin belongs to (octaves from ONSET_BAND_HZ up): a kick's rise counts in its own band as
    // much as a hi-hat's in its own, though the hi-hat spreads over many more bins
    std::vector<int> bandOf((size_t)bins + 1, 0);
    int bands = 1;
    for (int b = 1; b <= bins; b++) {
        double hz = (double)b * sr_ / n;
        int band = hz <= ONSET_BAND_HZ ? 0 : std::min(ONSET_BANDS - 1, 1 + (int)std::log2(hz / ONSET_BAND_HZ));
        bandOf[(size_t)b] = band;
        bands = std::max(bands, band + 1);
    }
    fft::Plan plan(n);
    std::vector<fft::cd> x((size_t)n);
    // each bin's level in the last ONSET_LAG windows: a rise counts against the loudest of them, so a low
    // tone's level wobbling from one short window to the next (its phase in them) is no rise
    std::vector<std::vector<double>> past((size_t)ONSET_LAG, std::vector<double>((size_t)bins + 1, 0.0));
    std::vector<double> now((size_t)bins + 1);
    int64_t hops = frames / ONSET_HOP + 1;
    std::vector<std::vector<double>> rises((size_t)bands, std::vector<double>((size_t)hops, 0.0));
    level_.assign((size_t)hops, 0.0);
    for (int64_t h = 0; h < hops; h++) {
        int64_t centre = h * ONSET_HOP;  // (each window centred on its hop)
        float loud = 0;
        for (int k = 0; k < n; k++) {
            int64_t i = centre - half + k;
            float v = i >= 0 && i < frames ? mono_[(size_t)i] : 0.0f;
            loud = std::max(loud, std::fabs(v));
            x[(size_t)k] = fft::cd(v * window[(size_t)k], 0.0);
        }
        level_[(size_t)h] = loud;
        plan.forward(x.data());
        for (int b = 1; b <= bins; b++) {
            now[(size_t)b] = std::log1p(100.0 * std::abs(x[(size_t)b]));
            double before = 0;
            for (auto& p : past) before = std::max(before, p[(size_t)b]);
            rises[(size_t)bandOf[(size_t)b]][(size_t)h] += std::max(0.0, now[(size_t)b] - before);
        }
        std::swap(past[(size_t)(h % ONSET_LAG)], now);
    }
    // each band's flux against its own largest (bands that hardly ever rise left out), the largest of them
    std::vector<double> tops((size_t)bands, 0.0);
    double topmost = 0;
    for (int band = 0; band < bands; band++) {
        for (double r : rises[(size_t)band]) tops[(size_t)band] = std::max(tops[(size_t)band], r);
        topmost = std::max(topmost, tops[(size_t)band]);
    }
    flux_.assign((size_t)hops, 0.0);
    for (int band = 0; band < bands; band++) {
        double top = tops[(size_t)band];
        if (top <= 0 || top < 0.02 * topmost) continue;
        for (int64_t h = 0; h < hops; h++)
            flux_[(size_t)h] = std::max(flux_[(size_t)h], rises[(size_t)band][(size_t)h] / top);
    }
}

int64_t Onsets::attack(int64_t from, int64_t to) const {
    const int64_t total = (int64_t)mono_.size();
    from = std::max<int64_t>(0, from), to = std::min(total, to);
    if (to - from < 2) return std::max<int64_t>(0, std::min(from, total - 1));
    auto peakIn = [&](int64_t a, int64_t b) {  // the loudest sample of [a, b)
        float m = 0;
        for (int64_t i = std::max<int64_t>(0, a); i < std::min(total, b); i++) m = std::max(m, std::fabs(mono_[(size_t)i]));
        return m;
    };
    // the sharpest rise: a short stretch much louder than the stretch just before it
    const int64_t block = std::max<int64_t>(8, sr_ / 1500), behind = 3 * block, step = std::max<int64_t>(1, block / 4);
    double best = -1;
    int64_t at = from;
    for (int64_t i = from; i < to; i += step) {
        double ahead = peakIn(i, i + block), past = peakIn(i - behind, i);
        double rise = ahead / (past + 1e-4 * (ahead + 1e-9));
        if (ahead > 1e-5 && rise > best + 1e-12) best = rise, at = i;
    }
    // its first sample clearly above what came before…
    float past = peakIn(at - behind, at), m = peakIn(at, at + 4 * block);
    float threshold = std::max(2 * past, 0.05f * m);
    int64_t j = at;
    for (int64_t i = std::max<int64_t>(0, at - step); i < std::min(total, at + 4 * block); i++)
        if (std::fabs(mono_[(size_t)i]) >= threshold && std::fabs(mono_[(size_t)i]) > 0) {
            j = i;
            break;
        }
    // …then back to where it began rising (a sine starting at nought, a fade-in of a few samples)
    int64_t earliest = std::max<int64_t>(0, std::max(at - step, j - sr_ / 500));
    while (j > earliest && std::fabs(mono_[(size_t)j - 1]) < std::fabs(mono_[(size_t)j]) &&
           std::fabs(mono_[(size_t)j - 1]) > past)
        j--;
    if (j > earliest && std::fabs(mono_[(size_t)j - 1]) <= past && std::fabs(mono_[(size_t)j - 1]) < std::fabs(mono_[(size_t)j]))
        j--;  // (the sample where it left the quiet: a sine's zero)
    return j;
}

std::vector<int64_t> Onsets::find(double sensitivity) const {
    std::vector<int64_t> hits;
    const int64_t hops = (int64_t)flux_.size();
    if (hops == 0 || mono_.empty()) return hits;
    double s = std::min(1.0, std::max(0.0, sensitivity));
    double delta = 0.02 + 0.6 * (1 - s) * (1 - s);  // how far above the flux around it a peak must stand
    double loudest = 0;
    for (double l : level_) loudest = std::max(loudest, l);
    if (loudest <= 0) return hits;
    // the flux weighed by how loud the sound is there: a quiet hit stands out less than a loud one
    std::vector<double> d((size_t)hops);
    for (int64_t h = 0; h < hops; h++) d[(size_t)h] = flux_[(size_t)h] * (0.3 + 0.7 * level_[(size_t)h] / loudest);
    const int64_t near = 3, around = std::max<int64_t>(4, (int64_t)(0.08 * sr_ / ONSET_HOP));
    const int64_t gap = std::max<int64_t>(1, (int64_t)(ONSET_GAP_S * sr_));
    std::vector<std::pair<int64_t, double>> peaks;  // (hop, weighed flux)
    for (int64_t h = 0; h < hops; h++) {
        double f = d[(size_t)h];
        if (f <= 0 || level_[(size_t)h] < 1e-3 * loudest) continue;  // (silence's noise is no hit)
        bool peak = true;
        for (int64_t k = std::max<int64_t>(0, h - near); k <= std::min(hops - 1, h + near) && peak; k++)
            if (k != h && (d[(size_t)k] > f || (k < h && d[(size_t)k] == f))) peak = false;
        if (!peak) continue;
        double sum = 0;
        int64_t count = 0;
        for (int64_t k = std::max<int64_t>(0, h - around); k <= std::min(hops - 1, h + around); k++)
            sum += d[(size_t)k], count++;
        if (f >= sum / std::max<int64_t>(1, count) + delta) peaks.push_back({h, f});
    }
    int64_t lastStart = -1;
    double lastFlux = 0;
    for (auto& pk : peaks) {
        int64_t centre = pk.first * ONSET_HOP;
        int64_t from = std::max(centre - ONSET_N / 2 - ONSET_HOP, lastStart >= 0 ? lastStart + gap : 0);
        if (from >= centre + ONSET_N / 2 - 1) continue;  // (its window lies within the hit before: the same one)
        int64_t start = attack(from, centre + ONSET_N / 2);
        if (lastStart >= 0 && start - lastStart < gap) {  // (too close: the stronger one stays)
            if (pk.second > lastFlux) hits.back() = start, lastStart = start, lastFlux = pk.second;
            continue;
        }
        hits.push_back(start);
        lastStart = start;
        lastFlux = pk.second;
    }
    return hits;
}

}  // namespace dsp
