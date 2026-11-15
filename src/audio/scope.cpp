// What the master scope shows (see scope.h).
#include "audio/scope.h"

#include <algorithm>
#include <cmath>

#include "audio/fft.h"

namespace scope {

namespace {

constexpr double PI = 3.14159265358979323846;

int powerOfTwoAtLeast(size_t n) {
    int p = 1;
    while ((size_t)p < n) p <<= 1;
    return p;
}

// one plan per size, kept (a plan's tables are the expensive part)
const fft::Plan& planFor(std::map<int, std::unique_ptr<fft::Plan>>& plans, int n) {
    auto& p = plans[n];
    if (!p) p = std::make_unique<fft::Plan>(n);
    return *p;
}

}  // namespace

const char* windowName(Window w) {
    switch (w) {
        case Window::Hann: return "Hann";
        case Window::Hamming: return "Hamming";
        case Window::BlackmanHarris: return "Blackman-Harris";
        case Window::Rectangular: return "Rectangular";
    }
    return "Hann";
}

std::vector<double> windowShape(Window w, int n) {
    std::vector<double> out((size_t)std::max(n, 0), 1.0);
    if (n < 2) return out;
    for (int i = 0; i < n; i++) {
        double x = 2 * PI * i / (n - 1);
        switch (w) {
            case Window::Hann: out[(size_t)i] = 0.5 - 0.5 * std::cos(x); break;
            case Window::Hamming: out[(size_t)i] = 0.54 - 0.46 * std::cos(x); break;
            case Window::BlackmanHarris:
                out[(size_t)i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
                break;
            case Window::Rectangular: break;
        }
    }
    return out;
}

Spectrum::Spectrum() = default;
Spectrum::~Spectrum() = default;

double Spectrum::bandHz(int band) const {
    int count = std::max(1, (int)levels_.size());
    return lowHz_ * std::pow(highHz_ / lowHz_, (band + 0.5) / count);
}

void Spectrum::analyse(const float* samples, size_t n, int sr, const SpectrumSettings& s, int count, double dt) {
    if (sr <= 0 || count <= 0) return;
    int N = std::clamp(powerOfTwoAtLeast((size_t)std::max(s.fftSize, 256)), 256, 32768);
    std::vector<double> w = windowShape(s.window, N);
    double sumW = 0;
    for (double v : w) sumW += v;
    std::vector<fft::cd> x((size_t)N, fft::cd(0, 0));
    std::vector<double> windowed((size_t)N, 0.0);  // (kept for the bands narrower than a bin)
    size_t take = std::min(n, (size_t)N);
    for (size_t i = 0; i < take; i++) {  // (the newest last; padded with silence before)
        size_t at = (size_t)N - take + i;
        windowed[at] = samples[n - take + i] * w[at];
        x[at] = fft::cd(windowed[at], 0);
    }
    planFor(plans_, N).forward(x.data());
    int half = N / 2;
    std::vector<double> mag((size_t)half + 1);
    for (int k = 0; k <= half; k++)  // (a sine of amplitude 1: 1 at its bin, for any window)
        mag[(size_t)k] = std::abs(x[(size_t)k]) * (k == 0 || k == half ? 1.0 : 2.0) / std::max(sumW, 1e-12);
    lowHz_ = 20;
    highHz_ = std::min(20000.0, sr * 0.5 * 0.999);
    double binHz = (double)sr / N;
    // the loudest frequency (parabolic, on dB)
    int best = 1;
    for (int k = 2; k < half; k++)
        if (mag[(size_t)k] > mag[(size_t)best]) best = k;
    auto db = [](double m) { return 20 * std::log10(std::max(m, 1e-12)); };
    double a = db(mag[(size_t)best - 1]), b = db(mag[(size_t)best]), c = db(mag[(size_t)std::min(best + 1, half)]);
    double shift = (a - 2 * b + c) != 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0;
    peakHz_ = (best + std::clamp(shift, -0.5, 0.5)) * binHz;
    // the bands: the loudest bin in each (one narrower than a bin: between the two around its middle)
    std::vector<float> now((size_t)count);
    for (int band = 0; band < count; band++) {
        double f0 = lowHz_ * std::pow(highHz_ / lowHz_, (double)band / count);
        double f1 = lowHz_ * std::pow(highHz_ / lowHz_, (double)(band + 1) / count);
        double fc = std::sqrt(f0 * f1);
        int k0 = (int)std::ceil(f0 / binHz), k1 = (int)std::floor(f1 / binHz);
        double m = 0;
        if (k1 >= k0 && k1 >= 1) {
            for (int k = std::max(k0, 1); k <= std::min(k1, half); k++) m = std::max(m, mag[(size_t)k]);
        } else {
            // (narrower than a bin: the spectrum at the band's own frequency, Goertzel's sum; between
            // two bins a peak reads true there, where reading it off the bins reads it low)
            double coeff = 2 * std::cos(2 * PI * fc / sr), s1 = 0, s2 = 0;
            for (int i = 0; i < N; i++) {
                double s0 = windowed[(size_t)i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            m = std::sqrt(std::max(power, 0.0)) * 2.0 / std::max(sumW, 1e-12);
        }
        double level = db(m) + s.slope * std::log2(fc / 1000.0);
        now[(size_t)band] = (float)std::max(level, s.floorDb);
    }
    if (levels_.size() != now.size() || s.fall <= 0) {
        levels_ = now;
        return;
    }
    float drop = (float)(s.fall * std::max(dt, 0.0));
    for (size_t i = 0; i < now.size(); i++)
        levels_[i] = std::max({now[i], levels_[i] - drop, (float)s.floorDb});
}

void columns(const float* samples, size_t n, int count, std::vector<Column>& out) {
    out.assign((size_t)std::max(count, 0), Column{});
    if (!samples || n == 0 || count <= 0) return;
    for (int c = 0; c < count; c++) {
        size_t from = (size_t)((double)c * n / count), to = (size_t)((double)(c + 1) * n / count);
        to = std::max(to, from + 1);
        to = std::min(to, n);
        from = std::min(from, n - 1);
        float lo = samples[from], hi = samples[from];
        for (size_t i = from + 1; i < to; i++) lo = std::min(lo, samples[i]), hi = std::max(hi, samples[i]);
        out[(size_t)c] = {lo, hi};
    }
}

Pitch detectPitch(const float* samples, size_t n, int sr, double minHz, double maxHz) {
    Pitch none;
    if (!samples || sr <= 0 || minHz <= 0 || maxHz <= minHz) return none;
    size_t tauMin = (size_t)std::max(2.0, std::floor(sr / maxHz));
    size_t tauMax = (size_t)std::ceil(sr / minHz);
    size_t W = std::max(tauMax, (size_t)1024);
    if (n < W + tauMax + 2) {  // (too little heard: the longest periods left out)
        if (n < 2 * tauMin + 4) return none;
        tauMax = (n - 2) / 2;
        W = tauMax;
        if (tauMax <= tauMin + 2) return none;
    }
    const float* x = samples + (n - (W + tauMax + 1));
    size_t len = W + tauMax + 1;
    double energy = 0;
    for (size_t i = 0; i < len; i++) energy += (double)x[i] * x[i];
    if (energy / len < 1e-8) return none;  // (silence: -80 dB)
    // r(t) = sum_{j<W} x_j x_{j+t}, through an FFT (correlation of the first W with all of it)
    int M = powerOfTwoAtLeast(len + W);
    std::vector<fft::cd> A((size_t)M, fft::cd(0, 0)), B((size_t)M, fft::cd(0, 0));
    for (size_t i = 0; i < W; i++) A[i] = fft::cd(x[i], 0);
    for (size_t i = 0; i < len; i++) B[i] = fft::cd(x[i], 0);
    static thread_local std::map<int, std::unique_ptr<fft::Plan>> plans;
    const fft::Plan& plan = planFor(plans, M);
    plan.forward(A.data());
    plan.forward(B.data());
    for (int i = 0; i < M; i++) A[(size_t)i] = std::conj(A[(size_t)i]) * B[(size_t)i];
    plan.inverse(A.data());
    std::vector<double> squares(len + 1, 0.0);  // (running sums of x², for the energies)
    for (size_t i = 0; i < len; i++) squares[i + 1] = squares[i] + (double)x[i] * x[i];
    std::vector<double> d(tauMax + 1, 0.0), dn(tauMax + 1, 1.0);
    double running = 0;
    for (size_t t = 1; t <= tauMax; t++) {
        double r = A[t].real() / M;
        double e0 = squares[W], et = squares[t + W] - squares[t];
        d[t] = std::max(0.0, e0 + et - 2 * r);
        running += d[t];
        dn[t] = running > 0 ? d[t] * t / running : 1.0;
    }
    // the first dip under the threshold (then down to its bottom); else the deepest
    constexpr double THRESHOLD = 0.15, UNSURE = 0.35;
    size_t tau = 0;
    for (size_t t = tauMin; t + 1 <= tauMax; t++)
        if (dn[t] < THRESHOLD) {
            while (t + 1 <= tauMax && dn[t + 1] < dn[t]) t++;
            tau = t;
            break;
        }
    if (!tau) {
        tau = tauMin;
        for (size_t t = tauMin; t <= tauMax; t++)
            if (dn[t] < dn[tau]) tau = t;
    }
    if (dn[tau] > UNSURE || tau <= tauMin || tau >= tauMax) return none;
    double a = dn[tau - 1], b = dn[tau], c = dn[tau + 1];
    double shift = (a - 2 * b + c) != 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0;
    Pitch p;
    p.period = tau + std::clamp(shift, -0.5, 0.5);
    p.hz = sr / p.period;
    p.clarity = std::clamp(1 - b, 0.0, 1.0);
    return p;
}

std::string noteName(double hz) {
    if (!(hz > 0)) return "";
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int midi = (int)std::lround(69 + 12 * std::log2(hz / 440.0));
    int octave = (int)std::floor(midi / 12.0) - 1;
    return std::string(names[((midi % 12) + 12) % 12]) + std::to_string(octave);
}

size_t followStart(const float* samples, size_t n, size_t length, double period, const std::vector<float>& previous) {
    if (!samples || n <= length) return 0;
    size_t last = n - length;
    if (!(period > 1)) return last;
    size_t span = (size_t)std::ceil(period);
    size_t from = last > span ? last - span : 1;
    std::vector<size_t> crossings;
    for (size_t z = std::max<size_t>(from, 1); z <= last; z++)
        if (samples[z - 1] < 0 && samples[z] >= 0) crossings.push_back(z);
    if (crossings.empty()) return last;
    if (previous.empty()) return crossings.back();
    size_t compare = std::min({span, previous.size()});
    size_t best = crossings.back();
    double bestScore = -1;
    for (size_t z : crossings) {
        size_t m = std::min(compare, n - z);
        double diff = 0;
        for (size_t i = 0; i < m; i++) {
            double e = (double)samples[z + i] - previous[i];
            diff += e * e;
        }
        diff /= std::max<size_t>(m, 1);
        if (bestScore < 0 || diff < bestScore) bestScore = diff, best = z;
    }
    return best;
}

}  // namespace scope
