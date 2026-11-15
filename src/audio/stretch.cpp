// The phase vocoder of audio/stretch.py (see stretch.h). The numbers follow the Python exactly: the
// same framing (hop_in = round(HOP / factor), frames written round(f * hop_in * factor) apart), the
// same periodic Hann window, padding, weighting and trimming; Python's round() (half to even) is
// std::nearbyint.
#include "audio/stretch.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <memory>
#include <mutex>

#include "audio/fft.h"

namespace stretch {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;

inline double pyRound(double x) { return std::nearbyint(x); }

}  // namespace

std::vector<float> resampleTo(const float* in, int64_t frames, int channels, int64_t length) {
    if (length == frames || frames < 2) return std::vector<float>(in, in + frames * channels);
    int64_t n = std::max<int64_t>(1, length);
    std::vector<float> out((size_t)(n * channels));
    // np.linspace(0, frames - 1, n) and np.interp
    double step = n > 1 ? (double)(frames - 1) / (double)(n - 1) : 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double x = i == n - 1 && n > 1 ? (double)(frames - 1) : (double)i * step;
        int64_t j = (int64_t)x;
        if (j >= frames - 1) {
            for (int c = 0; c < channels; ++c) out[(size_t)(i * channels + c)] = in[(frames - 1) * channels + c];
            continue;
        }
        double t = x - (double)j;
        for (int c = 0; c < channels; ++c) {
            double a = in[j * channels + c], b = in[(j + 1) * channels + c];
            out[(size_t)(i * channels + c)] = (float)(a + (b - a) * t);
        }
    }
    return out;
}

std::vector<float> timeStretch(const float* in, int64_t n, int channels, double factor) {
    if (std::fabs(factor - 1) < 1e-6 || n == 0 || channels <= 0) return std::vector<float>(in, in + n * channels);
    const int N = N_FFT, half = N / 2, nb = half + 1;
    const int hopIn = std::max(1, (int)pyRound(HOP / factor));
    const double hopOut = hopIn * factor;  // (exactly `factor` times hop_in; frames go where they round to)
    std::vector<float> window(N);
    for (int i = 0; i < N; ++i) window[i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / N));  // (periodic Hann)
    const int64_t xlen = n + 2 * (int64_t)N;  // the audio padded with N zeros at both ends
    const int64_t frames = 1 + (xlen - N) / hopIn;
    const int64_t outLen = (int64_t)std::ceil((double)(frames - 1) * hopOut) + N + 1;
    std::vector<float> out((size_t)(outLen * channels), 0.0f);
    std::vector<float> weight((size_t)outLen, 0.0f);
    std::vector<double> expected(nb);  // how far each bin's phase turns in hop_in, at its own frequency
    for (int b = 0; b < nb; ++b) expected[b] = kTwoPi * b * hopIn / N;

    fft::Plan plan(N);
    std::vector<fft::cd> work;
    const int pairs = (channels + 1) / 2;
    std::vector<std::vector<fft::cd>> spec((size_t)channels, std::vector<fft::cd>(nb));
    std::vector<double> bufA(N), bufB(N), outA(N), outB(N);
    std::vector<double> magnitude(nb), phase(nb), lastPhase(nb), synth(nb), advance(nb);
    std::vector<int> peaks, owner(nb);
    std::vector<double> peakSynth;
    std::vector<fft::cd> turn(nb), tmpA(nb), tmpB(nb);
    bool first = true;

    auto sample = [&](int64_t i, int c) -> double {  // the padded signal x
        int64_t j = i - N;
        return j >= 0 && j < n ? (double)in[j * channels + c] : 0.0;
    };

    for (int64_t f = 0; f < frames; ++f) {
        const int64_t s0 = f * hopIn;
        for (int p = 0; p < pairs; ++p) {
            int c0 = 2 * p, c1 = 2 * p + 1;
            for (int i = 0; i < N; ++i) {
                float w = window[i];
                bufA[i] = (double)((float)sample(s0 + i, c0) * w);
                bufB[i] = c1 < channels ? (double)((float)sample(s0 + i, c1) * w) : 0.0;
            }
            fft::realPair(plan, bufA.data(), bufB.data(), spec[c0].data(), c1 < channels ? spec[c1].data() : nullptr,
                          work);
        }
        for (int b = 0; b < nb; ++b) {
            fft::cd mono(0, 0);
            for (int c = 0; c < channels; ++c) mono += spec[c][b];
            magnitude[b] = std::abs(mono);
            phase[b] = std::atan2(mono.imag(), mono.real());
        }
        if (first) {
            synth = phase;
            first = false;
        } else {
            for (int b = 0; b < nb; ++b) {
                double turned = phase[b] - lastPhase[b] - expected[b];
                turned -= kTwoPi * std::nearbyint(turned / kTwoPi);
                advance[b] = (expected[b] + turned) * factor;
            }
            peaks.clear();
            for (int k = 1; k < nb - 1; ++k)
                if (magnitude[k] > magnitude[k - 1] && magnitude[k] >= magnitude[k + 1]) peaks.push_back(k);
            if (!peaks.empty()) {  // each bin follows the peak it lies under (identity phase locking)
                // owner = searchsorted((peaks[1:] + peaks[:-1]) // 2, bins, side="right")
                size_t o = 0;
                for (int b = 0; b < nb; ++b) {
                    while (o + 1 < peaks.size() && (peaks[o] + peaks[o + 1]) / 2 <= b) ++o;
                    owner[b] = (int)o;
                }
                peakSynth.resize(peaks.size());
                for (size_t q = 0; q < peaks.size(); ++q) peakSynth[q] = synth[peaks[q]] + advance[peaks[q]];
                for (int b = 0; b < nb; ++b) {
                    int pk = peaks[owner[b]];
                    synth[b] = peakSynth[owner[b]] + phase[b] - phase[pk];
                }
            } else {
                for (int b = 0; b < nb; ++b) synth[b] += advance[b];
            }
        }
        lastPhase = phase;
        for (int b = 0; b < nb; ++b) {  // (every channel keeps its phase relation to the sum)
            double d = synth[b] - phase[b];
            turn[b] = fft::cd(std::cos(d), std::sin(d));
        }
        const int64_t at = (int64_t)pyRound((double)f * hopOut);
        for (int p = 0; p < pairs; ++p) {
            int c0 = 2 * p, c1 = 2 * p + 1;
            for (int b = 0; b < nb; ++b) {
                tmpA[b] = spec[c0][b] * turn[b];
                if (c1 < channels) tmpB[b] = spec[c1][b] * turn[b];
            }
            fft::realPairInverse(plan, tmpA.data(), c1 < channels ? tmpB.data() : nullptr, outA.data(), outB.data(),
                                 work);
            for (int i = 0; i < N && at + i < outLen; ++i) {
                out[(size_t)((at + i) * channels + c0)] += (float)outA[i] * window[i];
                if (c1 < channels) out[(size_t)((at + i) * channels + c1)] += (float)outB[i] * window[i];
            }
        }
        for (int i = 0; i < N && at + i < outLen; ++i) weight[(size_t)(at + i)] += window[i] * window[i];
    }
    for (int64_t i = 0; i < outLen; ++i) {
        float w = std::max(weight[(size_t)i], 1e-3f);
        for (int c = 0; c < channels; ++c) out[(size_t)(i * channels + c)] /= w;
    }
    const int64_t start = (int64_t)pyRound(N * factor);  // (the padding before the audio, stretched)
    const int64_t want = (int64_t)pyRound((double)n * factor);
    int64_t a = std::min(start, outLen), e = std::min(start + want, outLen);
    return std::vector<float>(out.begin() + a * channels, out.begin() + e * channels);
}

std::vector<float> stretch(const float* in, int64_t frames, int channels, double factor, double semitones) {
    if (std::fabs(factor - 1) < 1e-6 && std::fabs(semitones) < 1e-6) return std::vector<float>(in, in + frames * channels);
    double ratio = std::pow(2.0, semitones / 12);
    int64_t target = (int64_t)pyRound((double)frames * factor);
    // (longer by the ratio too, then played faster)
    std::vector<float> s = timeStretch(in, frames, channels, factor * ratio);
    return resampleTo(s.data(), (int64_t)(s.size() / std::max(1, channels)), channels, target);
}

AudioPtr process(const AudioPtr& src, double factor, double semitones, bool reverse) {
    if (!src) return src;
    bool same = std::fabs(factor - 1) < 1e-6 && std::fabs(semitones) < 1e-6;
    if (same && !reverse) return src;
    auto out = std::make_shared<AudioBuffer>();
    if (same) out->samples = src->samples;
    else out->samples = stretch(src->samples.data(), src->frames(), 2, factor, semitones);
    if (reverse) {
        int64_t n = out->frames();
        float* s = out->samples.data();
        for (int64_t i = 0, j = n - 1; i < j; ++i, --j) {
            std::swap(s[2 * i], s[2 * j]);
            std::swap(s[2 * i + 1], s[2 * j + 1]);
        }
    }
    return out;
}

namespace {

struct Entry {
    const AudioBuffer* key;
    double factor, semitones;
    bool reverse;
    std::weak_ptr<const AudioBuffer> source;  // (a freed source's address may come back: then it's no hit)
    AudioPtr result;
};

std::mutex g_mutex;
std::deque<Entry> g_cache;  // oldest first

}  // namespace

AudioPtr stretched(const AudioPtr& src, double factor, double semitones, bool reverse) {
    if (!src) return src;
    if (std::fabs(factor - 1) < 1e-6 && std::fabs(semitones) < 1e-6 && !reverse) return src;
    double f = std::nearbyint(factor * 1e6) / 1e6, s = std::nearbyint(semitones * 1e4) / 1e4;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
            if (it->key == src.get() && it->factor == f && it->semitones == s && it->reverse == reverse) {
                if (it->source.lock() == src) return it->result;
                g_cache.erase(it);
                break;
            }
        }
    }
    AudioPtr result = process(src, factor, semitones, reverse);
    std::lock_guard<std::mutex> lock(g_mutex);
    while ((int)g_cache.size() >= CACHE) g_cache.pop_front();
    g_cache.push_back(Entry{src.get(), f, s, reverse, src, result});
    return result;
}

}  // namespace stretch
