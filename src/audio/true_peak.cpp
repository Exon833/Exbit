#include "audio/true_peak.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio {

const std::array<std::array<float, TruePeak::TAPS>, TruePeak::FACTOR>& TruePeak::coefficients() {
    static const auto table = [] {
        const double PI = 3.14159265358979323846, half = TAPS / 2.0;
        std::array<std::array<float, TAPS>, FACTOR> c{};
        for (int p = 0; p < FACTOR; p++) {
            double d = (double)p / FACTOR, sum = 0;
            double h[TAPS];
            for (int j = 0; j < TAPS; j++) {
                double x = j + 1 - half - d;  // (the tap's distance from the point interpolated, in samples)
                double sinc = x == 0 ? 1.0 : std::sin(PI * x) / (PI * x);
                double window = 0.42 + 0.5 * std::cos(PI * x / half) + 0.08 * std::cos(2 * PI * x / half);  // Blackman
                h[j] = sinc * window;
                sum += h[j];
            }
            for (int j = 0; j < TAPS; j++) c[p][j] = (float)(h[j] / sum);  // (unity gain at DC)
        }
        return c;
    }();
    return table;
}

TruePeak::TruePeak(int channels) : channels_(std::max(1, channels)) { reset(); }

void TruePeak::reset() {
    history_.assign((size_t)channels_ * TAPS * 2, 0.0f);
    pos_ = 0;
    peak_ = 0;
}

void TruePeak::add(const float* x, int frames, int width) {
    const auto& co = coefficients();
    int used = std::min(width, channels_);
    float top = (float)peak_;
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < channels_; c++) {
            float* h = history_.data() + (size_t)c * TAPS * 2;
            float v = c < used ? x[(size_t)i * width + c] : 0.0f;
            h[pos_] = h[pos_ + TAPS] = v;  // (written twice: the last TAPS samples always lie in one piece)
        }
        pos_ = (pos_ + 1) % TAPS;
        for (int c = 0; c < used; c++) {
            const float* w = history_.data() + (size_t)c * TAPS * 2 + pos_;  // oldest first
            for (int p = 0; p < FACTOR; p++) {
                float acc = 0;
                for (int j = 0; j < TAPS; j++) acc += co[p][j] * w[j];
                top = std::max(top, std::fabs(acc));
            }
        }
    }
    peak_ = top;
}

void TruePeak::addSilence(int frames) {
    // TAPS zeros flush the history; the ones before them still ring out through the interpolator
    int n = std::min(frames, TAPS);
    std::vector<float> zeros((size_t)channels_ * n, 0.0f);
    add(zeros.data(), n, channels_);
}

double TruePeak::peakDb() const {
    return peak_ > 0 ? 20 * std::log10(peak_) : -std::numeric_limits<double>::infinity();
}

}  // namespace audio
