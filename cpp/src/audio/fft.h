// A small complex FFT (radix 2, double precision) for the time-stretcher and the encoders' analysis
// (stretch.cpp, mp3_encoder.cpp, vorbis_encoder.cpp). Header-only.
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace fft {

using cd = std::complex<double>;

// in-place transform of n = 2^k points: forward X[k] = sum x[j] e^(-2 pi i jk/n); inverse unscaled (+i)
class Plan {
public:
    explicit Plan(int n) : n_(n), tw_((size_t)n / 2), rev_((size_t)n) {
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < n / 2; ++k) tw_[(size_t)k] = std::polar(1.0, -2.0 * pi * k / n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[(size_t)i] = r;
        }
    }
    int size() const { return n_; }
    void forward(cd* x) const { run(x, false); }
    void inverse(cd* x) const { run(x, true); }

private:
    void run(cd* x, bool inv) const {
        const int n = n_;
        for (int i = 0; i < n; ++i) {
            int r = rev_[(size_t)i];
            if (r > i) std::swap(x[i], x[r]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len) {
                cd* a = x + i;
                cd* b = x + i + half;
                for (int k = 0; k < half; ++k) {
                    cd w = tw_[(size_t)(k * step)];
                    if (inv) w = std::conj(w);
                    // (written out: std::complex's operator* checks for NaN/inf and is slow)
                    double br = b[k].real() * w.real() - b[k].imag() * w.imag();
                    double bi = b[k].real() * w.imag() + b[k].imag() * w.real();
                    double ar = a[k].real(), ai = a[k].imag();
                    a[k] = cd(ar + br, ai + bi);
                    b[k] = cd(ar - br, ai - bi);
                }
            }
        }
    }
    int n_;
    std::vector<cd> tw_;
    std::vector<int> rev_;
};

// The spectra (bins 0..n/2) of two real signals a and b of n points, by one complex FFT of a + ib.
// `work` holds n points.
inline void realPair(const Plan& p, const double* a, const double* b, cd* A, cd* B, std::vector<cd>& work) {
    const int n = p.size();
    work.resize((size_t)n);
    for (int i = 0; i < n; ++i) work[(size_t)i] = cd(a[i], b ? b[i] : 0.0);
    p.forward(work.data());
    for (int k = 0; k <= n / 2; ++k) {
        cd z = work[(size_t)k], zc = std::conj(work[(size_t)((n - k) & (n - 1))]);
        A[k] = 0.5 * (z + zc);
        if (B) B[k] = cd(0, -0.5) * (z - zc);
    }
}

// The inverse: two real signals (scaled by 1/n, as numpy's irfft) from their half spectra; the
// imaginary parts of bins 0 and n/2 are ignored (as irfft does).
inline void realPairInverse(const Plan& p, const cd* A, const cd* B, double* a, double* b, std::vector<cd>& work) {
    const int n = p.size();
    work.resize((size_t)n);
    cd i1(0, 1);
    for (int k = 0; k <= n / 2; ++k) {
        cd x = A[k], y = B ? B[k] : cd(0, 0);
        if (k == 0 || k == n / 2) {
            x = cd(x.real(), 0);
            y = cd(y.real(), 0);
        }
        work[(size_t)k] = x + i1 * y;
        if (k > 0 && k < n / 2) work[(size_t)(n - k)] = std::conj(x) + i1 * std::conj(y);
    }
    p.inverse(work.data());
    double s = 1.0 / n;
    for (int i = 0; i < n; ++i) {
        a[i] = work[(size_t)i].real() * s;
        if (b) b[i] = work[(size_t)i].imag() * s;
    }
}

}  // namespace fft
