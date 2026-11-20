// The moving effects' filter and LFO (effects/motion.cpp: Filter Motion, Phaser / Flanger, Tremolo / Pan), shared
// with the built-in sampler's voices (audio/sampler.h). No allocation, any thread.
//
// Svf: a state-variable filter in its "zero-delay feedback" form (the analog circuit's integrators turned digital
// by the trapezoidal rule, Zavalishin's "The Art of VA Filter Design"): its cutoff can sweep fast and far without
// clicks or blowing up. 24 dB an octave: two of them, a Butterworth's two sections, the second made resonant.
//
// Lfo: a slow wave (sine, triangle, saw up or down, square, a random line gliding from one random value to the
// next, or random steps), stepped by cycles; its synced lengths in beats (SYNC_BEATS).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace effects {

inline const std::vector<std::string> LFO_SHAPES = {"Sine",   "Triangle", "Saw up",         "Saw down",
                                                    "Square", "Random (smooth)", "Sample and hold"};
enum Shape { SINE, TRIANGLE, SAW_UP, SAW_DOWN, SQUARE, RANDOM_SMOOTH, SAMPLE_HOLD };

// the synced lengths of one cycle, in beats (a bar: 4 beats)
inline const std::vector<std::string> SYNC_NAMES = {"1/16", "1/8 triplet", "1/8",          "dotted 1/8",    "1/4",
                                                    "dotted 1/4", "1/2",   "1 bar (4/4)", "2 bars (4/4)", "4 bars (4/4)"};
constexpr double SYNC_BEATS[] = {0.25, 1.0 / 3, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0};
constexpr int SYNC_COUNT = (int)(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

// a value too small to matter (states fall to it, not on into denormals, which are slow)
inline double flush(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }

// A random value (-1…1) for each cycle: the cycle's number scrambled (splitmix64), so the same
// cycle always has the same value, and the right side, running a part of a cycle off the left,
// meets the same values a little earlier.
inline double cycleRandom(int64_t cycle) {
    uint64_t z = (uint64_t)cycle + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;  // (53 bits over 2^53, to -1…1)
}

// The LFO: where it is (whole cycles gone by, and how far into this one), and its wave there.
struct Lfo {
    int64_t cycle = 0;
    double phase = 0;  // 0…1

    void reset() {
        cycle = 0;
        phase = 0;
    }
    void advance(double cycles) {
        phase += cycles;
        if (phase >= 1.0) {
            double whole = std::floor(phase);
            cycle += (int64_t)whole;
            phase -= whole;
        }
    }
    // the wave (-1…1), `offset` cycles (0…1) ahead
    double at(int shape, double offset) const {
        const double pi = 3.14159265358979323846;
        double p = phase + offset;
        int64_t c = cycle;
        if (p >= 1.0) {
            p -= 1.0;
            c++;
        }
        switch (shape) {
            case TRIANGLE: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
            case SAW_UP: return 2 * p - 1;
            case SAW_DOWN: return 1 - 2 * p;
            case SQUARE: return p < 0.5 ? 1.0 : -1.0;
            case RANDOM_SMOOTH: {  // (a half cosine from this cycle's value to the next's)
                double a = cycleRandom(c), b = cycleRandom(c + 1);
                return a + (b - a) * (0.5 - 0.5 * std::cos(pi * p));
            }
            case SAMPLE_HOLD: return cycleRandom(c);
            default: return std::sin(2 * pi * p);
        }
    }
};

enum FilterType { LOWPASS, HIGHPASS, BANDPASS, NOTCH };
constexpr double MOST_Q = 25.0;  // resonance at 100 % (rings, but stays a filter)
// 24 dB: two filters, the first a Butterworth's lower section, the second its upper one, made resonant
constexpr double Q_12 = 0.7071067811865476, Q_24_FIRST = 0.5411961001461970, Q_24_SECOND = 1.3065629648763766;

// the two filters' damping (1/Q) for the type, slope (steep: 24 dB) and resonance (0…1)
inline void svfDampings(int type, bool steep, double resonance, double& first, double& second) {
    double res = std::min(1.0, std::max(0.0, resonance));
    bool poles = steep && (type == LOWPASS || type == HIGHPASS);
    double q0 = poles ? Q_24_SECOND : Q_12;
    double q = q0 * std::pow(MOST_Q / q0, res);
    second = 1 / q;
    first = poles ? 1 / Q_24_FIRST : 1 / q;
}

// One state-variable filter (topology-preserving transform: trapezoidal integrators, solved
// without a delay in the loop): the step for an input x at g = tan(π fc / sr) and damping k = 1/Q;
// lp, bp, hp its low-, band- and high-pass outputs.
struct Svf {
    double ic1[2] = {0, 0}, ic2[2] = {0, 0};  // the integrators' states, per side

    void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
    double step(int c, double x, double g, double k, int type) {
        double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        double v3 = x - ic2[c];
        double v1 = a1 * ic1[c] + a2 * v3;
        double v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
        ic1[c] = flush(2 * v1 - ic1[c]);
        ic2[c] = flush(2 * v2 - ic2[c]);
        if (!(std::fabs(ic1[c]) < 1e6 && std::fabs(ic2[c]) < 1e6)) ic1[c] = ic2[c] = 0;  // (never: a guard)
        switch (type) {
            case HIGHPASS: return x - k * v1 - v2;
            case BANDPASS: return k * v1;  // (its peak at 0 dB, however narrow)
            case NOTCH: return x - k * v1;
            default: return v2;
        }
    }
};

}  // namespace effects
