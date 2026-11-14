// Psychoacoustic helpers shared by the encoders (mp3_encoder.cpp, vorbis_encoder.cpp): the Bark
// scale, the threshold in quiet, and partitions of an FFT's bins about a third of a Bark wide with the
// spreading of masking between them (Schroeder's spreading function). Header-only.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace psy {

inline double bark(double f) { return 13 * std::atan(0.00076 * f) + 3.5 * std::atan((f / 7500) * (f / 7500)); }

// threshold in quiet (Terhardt), dB SPL, made safer at both ends: no higher than 15 dB SPL in the
// bass (the formula's steep rise would leave bass notes coded coarsely: audible when played loud),
// and above 13 kHz it rises gently instead of steeply (else everything above ~16 kHz would go)
inline double athDb(double f) {
    auto terhardt = [](double k) {
        return 3.64 * std::pow(k, -0.8) - 6.5 * std::exp(-0.6 * (k - 3.3) * (k - 3.3)) + 1e-3 * k * k * k * k;
    };
    double k = std::max(f, 20.0) / 1000.0;
    if (k <= 13) return std::min(terhardt(k), 15.0);
    return std::min(terhardt(k), terhardt(13) + 2.5 * (k - 13));
}

struct Partitions {
    int n = 0, bins = 0;
    std::vector<int> first, last;                // bins [first, last)
    std::vector<double> z, athE, rnorm, spread;  // spread[q * n + p]: masker p on maskee q
    std::vector<int> spreadLo, spreadHi;         // the maskers that matter for maskee q: [lo, hi)
    std::vector<int> partOf;                     // bin -> partition

    // for an N-point FFT with a Hann window at rate sr; a full-scale sine is taken as 96 dB SPL
    void build(int N, int sr) {
        bins = N / 2 + 1;
        const double fullScale = 3.0 * N * N / 32.0;  // its energy in the half spectrum (amplitude 1)
        first.clear();
        last.clear();
        int j = 0;
        while (j < bins) {
            int a = j;
            double za = bark((double)a * sr / N);
            ++j;
            while (j < bins && bark((double)j * sr / N) - za < 0.34) ++j;
            first.push_back(a);
            last.push_back(j);
        }
        n = (int)first.size();
        z.assign(n, 0);
        athE.assign(n, 0);
        partOf.assign(bins, 0);
        for (int p = 0; p < n; ++p) {
            double fa = (double)first[p] * sr / N, fb = (double)(last[p] - 1) * sr / N;
            z[p] = bark(0.5 * (fa + fb));
            double m = 1e9;
            for (int k = first[p]; k < last[p]; ++k) {
                m = std::min(m, athDb(std::max(1.0, (double)k) * sr / N));
                partOf[k] = p;
            }
            athE[p] = fullScale * std::pow(10.0, (m - 96.0) / 10.0) / 3.0;  // (a third: three partitions a Bark)
        }
        spread.assign((size_t)n * n, 0.0);
        rnorm.assign(n, 0);
        spreadLo.assign(n, n);
        spreadHi.assign(n, 0);
        for (int q = 0; q < n; ++q) {
            double sum = 0;
            for (int p = 0; p < n; ++p) {
                double dz = z[q] - z[p] + 0.474;
                double db = 15.811389 + 7.5 * dz - 17.5 * std::sqrt(1.0 + dz * dz);
                double v = db < -60 ? 0.0 : std::pow(10.0, db / 10.0);
                spread[(size_t)q * n + p] = v;
                sum += v;
                if (v > 0) {
                    spreadLo[q] = std::min(spreadLo[q], p);
                    spreadHi[q] = std::max(spreadHi[q], p + 1);
                }
            }
            rnorm[q] = 1.0 / sum;
        }
    }
};

}  // namespace psy
