// The moving effects: Filter Motion, Phaser / Flanger and Tremolo / Pan. Each one is moved by a slow
// wave, its LFO (low-frequency oscillator), which they share: a sine, a triangle, a saw going up or
// down, a square, a random line gliding from one random value to the next, or random steps (sample
// and hold), each cycle new. Its speed is a rate in Hz, or, synced, one cycle per length in beats at
// the project's tempo (1/16 … 4 bars); stopped, it keeps going at the last tempo it was given. The
// right side's wave can run a part of a cycle (the stereo phase, 0…180°) ahead of the left's.
//
// Filter Motion: a resonant filter (low-pass, high-pass, band-pass or notch; 12 or 24 dB an octave),
// a state-variable filter worked out sample by sample in its "zero-delay feedback" form (the analog
// circuit's integrators turned digital by the trapezoidal rule, Zavalishin's "The Art of VA Filter
// Design"): its cutoff can sweep fast and far without clicks or blowing up. The LFO moves the
// cutoff up and down by its depth in octaves, and an envelope follower (how loud the sound is, or
// what comes into its sidechain jack) by its amount in octaves: louder, higher (or lower). Drive
// warms the sound before the filter (a soft clip). The window draws the filter's response.
//
// Phaser / Flanger: the sound mixed with a copy of itself that is shifted in time by different
// amounts at different pitches (a phaser: 4, 8 or 12 first-order allpass filters, their corner swept
// around the centre frequency) or delayed a few milliseconds (a flanger: 0.1 to 10 ms, swept by the
// LFO), which cancels some pitches and doubles others: moving notches, the whoosh and the jet plane.
// Feedback sends the result back in (negative: the other notches stand out).
//
// Tremolo / Pan: the volume moved up and down by the LFO, by its depth. With the right side half a
// cycle (180°) behind the left, one side rises as the other falls: the sound pans from side to
// side. Smoothing softens the wave's corners (a square's edges, the saws' jumps, the random steps),
// so they don't click.
//
// Every setting glides to its new value over a few milliseconds, so turning a knob or automating it
// doesn't step (no zipper noise). Nothing is allocated while playing.
#include <algorithm>
#include <complex>
#include <cstdio>

#include "effects/builtin.h"

namespace effects {
namespace {

// --- what they share ----------------------------------------------------------------------------------

const std::vector<std::string> LFO_SHAPES = {"Sine",   "Triangle", "Saw up",         "Saw down",
                                             "Square", "Random (smooth)", "Sample and hold"};
enum Shape { SINE, TRIANGLE, SAW_UP, SAW_DOWN, SQUARE, RANDOM_SMOOTH, SAMPLE_HOLD };

// the synced lengths of one cycle, in beats (a bar: 4 beats)
const std::vector<std::string> SYNC_NAMES = {"1/16", "1/8 triplet", "1/8",          "dotted 1/8",    "1/4",
                                             "dotted 1/4", "1/2",   "1 bar (4/4)", "2 bars (4/4)", "4 bars (4/4)"};
const double SYNC_BEATS[] = {0.25, 1.0 / 3, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0};
constexpr int SYNC_COUNT = (int)(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

// how long settings take to glide to a new value, and the cutoff's moves (a square's jumps)
constexpr double GLIDE_MS = 20.0, CUTOFF_GLIDE_MS = 1.0;

// a value too small to matter (states fall to it, not on into denormals, which are slow)
inline double flush(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }

// A random value (-1…1) for each cycle: the cycle's number scrambled (splitmix64), so the same
// cycle always has the same value, and the right side, running a part of a cycle off the left,
// meets the same values a little earlier.
double cycleRandom(int64_t cycle) {
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
                return a + (b - a) * (0.5 - 0.5 * std::cos(PI * p));
            }
            case SAMPLE_HOLD: return cycleRandom(c);
            default: return std::sin(2 * PI * p);
        }
    }
};

// A setting gliding to its target: a one-pole low-pass, stepped once a frame.
struct Glide {
    double value = 0, coeff = 0;
    void time(double ms, double rate) { coeff = timeCoeff(ms, rate); }
    double to(double target) { return value = target + coeff * (value - target); }
};

std::vector<Param> lfoParams(double lowHz, double highHz, double defHz, int defBeats, int defShape) {
    return {choice("lfo_shape", "LFO shape", LFO_SHAPES, defShape),
            toggle("lfo_sync", "Sync to the tempo"),
            param("lfo_rate", "LFO rate", lowHz, highHz, defHz, "Hz", Scale::Log, 2),
            choice("lfo_beats", "LFO rate (synced)", SYNC_NAMES, defBeats)};
}

std::vector<Param> joined(std::vector<Param> a, const std::vector<Param>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// An effect moved by the LFO: its rate and sync settings, and the tempo it last heard.
class LfoEffect : public Effect {
public:
    LfoEffect(std::string id, std::string name, std::vector<Param> params, bool sidechain = false)
        : Effect(std::move(id), std::move(name), std::move(params), sidechain) {}
    int lfoShape = key("lfo_shape"), lfoSync = key("lfo_sync"), lfoRate = key("lfo_rate"), lfoBeats = key("lfo_beats");
    Lfo lfo;
    double heldTempo = 120.0;  // (the last real tempo: stopped, or given none, the LFO keeps to it)

    int shape() const { return std::min((int)LFO_SHAPES.size() - 1, std::max(0, (int)v(lfoShape))); }
    // the LFO's speed: cycles a second
    double cyclesPerSecond() {
        double t = tempo();
        if (std::isfinite(t) && t >= 1.0) heldTempo = std::min(t, 1000.0);
        if (v(lfoSync) < 0.5) return v(lfoRate);
        int i = std::min(SYNC_COUNT - 1, std::max(0, (int)v(lfoBeats)));
        return heldTempo / 60.0 / SYNC_BEATS[i];
    }
};

std::string hzText(double hz) {
    char buf[32];
    if (hz >= 1000)
        snprintf(buf, sizeof buf, "%.2f kHz", hz / 1000);
    else
        snprintf(buf, sizeof buf, "%.0f Hz", hz);
    return buf;
}

// --- Filter Motion --------------------------------------------------------------------------------

enum FilterType { LOWPASS, HIGHPASS, BANDPASS, NOTCH };
constexpr double LOWEST_CUTOFF = 20.0, HIGHEST_CUTOFF = 20000.0;
constexpr double MOST_Q = 25.0;  // resonance at 100 % (rings, but stays a filter)
// 24 dB: two filters, the first a Butterworth's lower section, the second its upper one, made resonant
constexpr double Q_12 = 0.7071067811865476, Q_24_FIRST = 0.5411961001461970, Q_24_SECOND = 1.3065629648763766;
// the follower's range: from -60 dB (nothing) to 0 dB (all of the amount)
constexpr double FOLLOW_RANGE_DB = 60.0;

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

// |H| of the analog state-variable filter at w (the frequency over the cutoff), as the digital one
// is at the frequency that the trapezoidal rule's warping maps there
double svfMagnitude(int type, double w, double k) {
    std::complex<double> s(0, w);
    std::complex<double> d = s * s + k * s + 1.0, h;
    switch (type) {
        case HIGHPASS: h = s * s / d; break;
        case BANDPASS: h = k * s / d; break;
        case NOTCH: h = (s * s + 1.0) / d; break;
        default: h = 1.0 / d;
    }
    return std::abs(h);
}

class FilterMotion : public LfoEffect {
public:
    FilterMotion()
        : LfoEffect("filter_motion", "Filter Motion",
                    joined({choice("type", "Filter", {"Low-pass", "High-pass", "Band-pass", "Notch"}),
                            choice("slope", "Slope", {"12 dB/oct", "24 dB/oct"}),
                            param("cutoff", "Cutoff", LOWEST_CUTOFF, HIGHEST_CUTOFF, 1000, "Hz", Scale::Log),
                            param("resonance", "Resonance", 0, 100, 20, "%"),
                            param("drive", "Drive", 0, 24, 0, "dB", Scale::Linear, 1)},
                           joined(lfoParams(0.01, 20, 1, 4, SINE),
                                  {param("lfo_depth", "LFO depth", -4, 4, 0, "oct", Scale::Linear, 2),
                                   param("lfo_phase", "LFO stereo phase", 0, 180, 0, "°", Scale::Linear, 0),
                                   param("env_amount", "Envelope amount", -4, 4, 0, "oct", Scale::Linear, 2),
                                   param("env_attack", "Envelope attack", 0.1, 100, 5, "ms", Scale::Log, 1),
                                   param("env_release", "Envelope release", 5, 2000, 150, "ms", Scale::Log),
                                   toggle("sidechain", "Envelope follows the sidechain")})),
                    true) {
        curveYGrid = {{0.0, "0 dB"}, {0.5, "+12"}, {-0.5, "−12"}};
        curveXGrid = {{eqX(100), "100"}, {eqX(1000), "1k"}, {eqX(10000), "10k"}};
    }
    int type = key("type"), slope = key("slope"), cutoff = key("cutoff"), resonance = key("resonance"),
        drive = key("drive"), lfoDepth = key("lfo_depth"), lfoPhase = key("lfo_phase"), envAmount = key("env_amount"),
        envAttack = key("env_attack"), envRelease = key("env_release"), sideOn = key("sidechain");
    Svf stages[2];
    Glide base, depth, envOct, kFirst, kSecond, driveGain;  // (base: the cutoff, in octaves above 1 Hz)
    Glide moved[2];                                         // the cutoff with its moves, per side
    double env = 0;
    int lastSlope = -1;
    bool fresh = true;  // (the first block since start(): the settings are where they are, no gliding)
    std::atomic<double> shown{1000.0};  // the cutoff now (the left's), for the meter

    void allocate(double rate, int) override {
        for (Glide* g : {&base, &depth, &envOct, &kFirst, &kSecond, &driveGain}) g->time(GLIDE_MS, rate);
        for (Glide& g : moved) g.time(CUTOFF_GLIDE_MS, rate);
    }
    void start() override {
        for (Svf& s : stages) s.reset();
        lfo.reset();
        env = 0;
        fresh = true;
        shown = v(cutoff);
    }
    // the two filters' damping (1/Q) for the type, slope and resonance
    void dampings(double& first, double& second) const {
        double res = std::min(1.0, std::max(0.0, v(resonance) / 100));
        bool steep = v(slope) >= 0.5, poles = (int)v(type) == LOWPASS || (int)v(type) == HIGHPASS;
        double q0 = steep && poles ? Q_24_SECOND : Q_12;
        double q = q0 * std::pow(MOST_Q / q0, res);
        second = 1 / q;
        first = steep && poles ? 1 / Q_24_FIRST : 1 / q;
    }
    void block(const float* const* x, const float* const* side, float* const* y, int n) override {
        int t = std::min(3, std::max(0, (int)v(type)));
        int steep = v(slope) >= 0.5 ? 1 : 0;
        if (steep != lastSlope) {  // (the second filter starts from rest when it comes in)
            stages[1].reset();
            lastSlope = steep;
        }
        double k1, k2;
        dampings(k1, k2);
        double baseTarget = std::log2(v(cutoff)), depthTarget = v(lfoDepth), envTarget = v(envAmount);
        double gainTarget = dbToGain(v(drive));
        if (fresh) {
            base.value = baseTarget;
            depth.value = depthTarget;
            envOct.value = envTarget;
            kFirst.value = k1;
            kSecond.value = k2;
            driveGain.value = gainTarget;
            for (Glide& g : moved) g.value = baseTarget;
            fresh = false;
        }
        double step = cyclesPerSecond() / sr, offset = v(lfoPhase) / 360;
        int sh = shape();
        const float* const* follow = v(sideOn) >= 0.5 && side ? side : x;
        double attack = timeCoeff(v(envAttack), sr), release = timeCoeff(v(envRelease), sr);
        double lowest = std::log2(LOWEST_CUTOFF / 2), highest = std::log2(std::min(HIGHEST_CUTOFF, sr * 0.45));
        for (int j = 0; j < n; j++) {
            double level = std::max(std::fabs((double)follow[0][j]), std::fabs((double)follow[1][j]));
            env = flush(level + (level > env ? attack : release) * (env - level));
            double loud = env > 1e-9 ? std::min(1.0, std::max(0.0, 1 + gainToDb(env) / FOLLOW_RANGE_DB)) : 0.0;
            double b = base.to(baseTarget), d = depth.to(depthTarget), e = envOct.to(envTarget);
            double ka = kFirst.to(k1), kb = kSecond.to(k2), dg = driveGain.to(gainTarget);
            double makeup = dg > 1.0 ? 1 / std::sqrt(dg) : 1.0;  // (half the drive's gain taken back)
            for (int c = 0; c < 2; c++) {
                double oct = b + d * lfo.at(sh, c ? offset : 0.0) + e * loud;
                oct = moved[c].to(std::min(highest, std::max(lowest, oct)));
                double g = std::tan(PI * std::exp2(oct) / sr);
                double in = x[c][j];
                if (dg > 1.0) in = std::tanh(in * dg) * makeup;
                double out = stages[0].step(c, in, g, steep ? ka : kb, t);
                if (steep) out = stages[1].step(c, out, g, kb, t);
                y[c][j] = (float)out;
            }
            lfo.advance(step);
        }
        shown = std::exp2(moved[0].value);
    }

    std::vector<Meter> meters() const override {
        double hz = shown.load();
        return {{"Cutoff now", eqX(std::min(EQ_HIGH, std::max(EQ_LOW, hz))), hzText(hz)}};
    }
    bool hasCurve() const override { return true; }
    std::vector<CurvePoint> curve() const override {  // the response at the cutoff set (without its moves)
        double k1, k2;
        dampings(k1, k2);
        int t = std::min(3, std::max(0, (int)v(type)));
        bool steep = v(slope) >= 0.5;
        double fc = std::min(v(cutoff), sr * 0.45), warped = std::tan(PI * fc / sr);
        std::vector<CurvePoint> points;
        for (int i = 0; i < 200; i++) {
            double x = i / 199.0;
            double f = std::min(EQ_LOW * std::pow(EQ_HIGH / EQ_LOW, x), sr * 0.499);
            double w = std::tan(PI * f / sr) / warped;
            double m = svfMagnitude(t, w, steep ? k1 : k2);
            if (steep) m *= svfMagnitude(t, w, k2);
            double db = 20 * std::log10(std::max(m, 1e-6));
            points.push_back({x, std::max(-1.0, std::min(1.0, db / 24))});
        }
        return points;
    }
};

// --- Phaser / Flanger ------------------------------------------------------------------------------

enum Mode { PHASER, FLANGER };
const int PHASER_STAGES[] = {4, 8, 12};
constexpr int MOST_STAGES = 12;
constexpr double PHASER_OCTAVES = 3.0;  // the sweep at 100 % depth: 3 octaves either side of the centre
constexpr double LONGEST_FLANGE_MS = 10.0 * 2;  // (10 ms, swept up to nearly twice that)
constexpr double SHORTEST_FLANGE_MS = 0.05;

class PhaserFlanger : public LfoEffect {
public:
    PhaserFlanger()
        : LfoEffect("phaser_flanger", "Phaser / Flanger",
                    joined({choice("mode", "Mode", {"Phaser", "Flanger"}),
                            choice("stages", "Phaser stages", {"4", "8", "12"}, 1)},
                           joined(lfoParams(0.01, 10, 0.5, 7, SINE),
                                  {param("depth", "Depth", 0, 100, 70, "%"),
                                   param("feedback", "Feedback", -90, 90, 30, "%"),
                                   param("centre", "Phaser centre", 100, 8000, 700, "Hz", Scale::Log),
                                   param("delay", "Flanger delay", 0.1, 10, 2.5, "ms", Scale::Log, 2),
                                   param("spread", "Stereo spread", 0, 180, 90, "°", Scale::Linear, 0)}))) {}
    int mode = key("mode"), stages = key("stages"), depth = key("depth"), feedback = key("feedback"),
        centre = key("centre"), delay = key("delay"), spread = key("spread");
    double allpass[2][MOST_STAGES] = {};  // the phaser's allpasses' states, per side
    double last[2] = {0, 0};              // what came out of the phaser's allpasses or the flanger's delay
    std::vector<double> ring[2];          // the flanger's delay line
    size_t mask = 0, at = 0;
    Glide depthG, feedbackG, centreG, delayG;
    int lastMode = -1, lastStages = -1;
    bool fresh = true;

    void allocate(double rate, int) override {
        size_t size = 4;
        while (size < (size_t)(LONGEST_FLANGE_MS * 0.001 * rate) + 4) size *= 2;
        for (auto& r : ring) r.assign(size, 0.0);
        mask = size - 1;
        for (Glide* g : {&depthG, &feedbackG, &centreG, &delayG}) g->time(GLIDE_MS, rate);
    }
    void clear() {
        for (auto& side : allpass) std::fill(side, side + MOST_STAGES, 0.0);
        last[0] = last[1] = 0;
        for (auto& r : ring) std::fill(r.begin(), r.end(), 0.0);
        at = 0;
    }
    void start() override {
        clear();
        lfo.reset();
        fresh = true;
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int m = v(mode) >= 0.5 ? FLANGER : PHASER;
        int count = PHASER_STAGES[std::min(2, std::max(0, (int)v(stages)))];
        if (m != lastMode || (m == PHASER && count != lastStages)) {  // (another sound: from rest)
            clear();
            lastMode = m;
            lastStages = count;
        }
        double depthT = v(depth) / 100, feedbackT = v(feedback) / 100, centreT = std::log2(v(centre)),
               delayT = v(delay);
        if (fresh) {
            depthG.value = depthT;
            feedbackG.value = feedbackT;
            centreG.value = centreT;
            delayG.value = delayT;
            fresh = false;
        }
        double step = cyclesPerSecond() / sr, offset = v(spread) / 360;
        int sh = shape();
        double highest = std::min(20000.0, sr * 0.45), longest = (double)(mask - 2);
        for (int j = 0; j < n; j++) {
            double d = depthG.to(depthT), fb = feedbackG.to(feedbackT), ct = centreG.to(centreT), dl = delayG.to(delayT);
            for (int c = 0; c < 2; c++) {
                double l = lfo.at(sh, c ? offset : 0.0), dry = x[c][j], wet;
                if (m == PHASER) {
                    double f = std::min(highest, std::max(20.0, std::exp2(ct + d * PHASER_OCTAVES * l)));
                    double t = std::tan(PI * f / sr), a = (t - 1) / (t + 1);
                    double in = dry + fb * last[c];
                    for (int s = 0; s < count; s++) {  // (first-order allpasses: y = a·x + s, s = x − a·y)
                        double out = a * in + allpass[c][s];
                        allpass[c][s] = flush(in - a * out);
                        in = out;
                    }
                    wet = last[c] = flush(in);
                } else {
                    double ms = std::max(SHORTEST_FLANGE_MS, dl * (1 + 0.95 * d * l));
                    double frames = std::min(longest, std::max(1.0, ms * 0.001 * sr));
                    double back = std::floor(frames), frac = frames - back;
                    size_t i = (at - (size_t)back) & mask;
                    const std::vector<double>& r = ring[c];
                    wet = r[i] + (r[(i - 1) & mask] - r[i]) * frac;  // (linear between two frames)
                    ring[c][at] = flush(dry + fb * wet);
                    last[c] = wet;
                }
                y[c][j] = (float)(0.5 * (dry + wet));
            }
            at = (at + 1) & mask;
            lfo.advance(step);
        }
    }
};

// --- Tremolo / Pan ----------------------------------------------------------------------------------

class TremoloPan : public LfoEffect {
public:
    TremoloPan()
        : LfoEffect("tremolo_pan", "Tremolo / Pan",
                    joined(lfoParams(0.05, 30, 4, 4, SINE),
                           {param("depth", "Depth", 0, 100, 50, "%"),
                            param("phase", "Stereo phase", 0, 180, 0, "°", Scale::Linear, 0),
                            param("smoothing", "Smoothing", 0, 100, 10, "%")})) {}
    int depth = key("depth"), phase = key("phase"), smoothing = key("smoothing");
    Glide depthG;
    double wave[2] = {0, 0};  // the LFO smoothed, per side
    bool fresh = true;
    std::atomic<double> gains[2] = {{1.0}, {1.0}};  // for the meters

    void allocate(double rate, int) override { depthG.time(GLIDE_MS, rate); }
    void start() override {
        lfo.reset();
        fresh = true;
        gains[0] = gains[1] = 1.0;
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        double hz = cyclesPerSecond(), step = hz / sr, offset = v(phase) / 360, depthT = v(depth) / 100;
        int sh = shape();
        // smoothing: a one-pole low-pass on the wave, its time up to a tenth of a cycle
        double seconds = v(smoothing) / 100 * 0.1 / std::max(hz, 1e-3);
        double soften = seconds > 0 ? std::exp(-1 / (seconds * sr)) : 0.0;
        if (fresh) {
            depthG.value = depthT;
            for (int c = 0; c < 2; c++) wave[c] = lfo.at(sh, c ? offset : 0.0);
            fresh = false;
        }
        double g[2] = {1, 1};
        for (int j = 0; j < n; j++) {
            double d = depthG.to(depthT);
            for (int c = 0; c < 2; c++) {
                double target = lfo.at(sh, c ? offset : 0.0);
                wave[c] = target + soften * (wave[c] - target);
                g[c] = 1 - d * (0.5 - 0.5 * wave[c]);
                y[c][j] = (float)(x[c][j] * g[c]);
            }
            lfo.advance(step);
        }
        gains[0] = g[0];
        gains[1] = g[1];
    }
    std::vector<Meter> meters() const override {
        std::vector<Meter> m;
        const char* labels[2] = {"Left", "Right"};
        for (int c = 0; c < 2; c++) {
            double gain = gains[c].load();
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f %%", gain * 100);
            m.push_back({labels[c], gain, buf});
        }
        return m;
    }
};

}  // namespace

std::unique_ptr<Effect> makeFilterMotion() { return std::make_unique<FilterMotion>(); }
std::unique_ptr<Effect> makePhaserFlanger() { return std::make_unique<PhaserFlanger>(); }
std::unique_ptr<Effect> makeTremoloPan() { return std::make_unique<TremoloPan>(); }

}  // namespace effects
