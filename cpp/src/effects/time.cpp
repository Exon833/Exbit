// The time effects (effects/chorus.py, delay.py, reverb.py, and their twins in native/effects.cpp,
// which is what plays).
#include <algorithm>

#include "effects/builtin.h"

namespace effects {

const double DELAY_BEATS[8] = {0.25, 1.0 / 3, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0};

namespace {

// --- Chorus -----------------------------------------------------------------------------------------
// Copies of the sound, each delayed a little and the delay swaying slowly (a sine), mixed back with
// it: a fuller, wider, slightly detuned sound. The right side sways out of step with the left by
// the stereo width (100 %: half a cycle apart).

constexpr double CHORUS_LONGEST_MS = 40.0;

class Chorus : public Effect {
public:
    Chorus()
        : Effect("chorus", "Chorus",
                 {param("rate", "Rate", 0.05, 5, 0.8, "Hz", Scale::Log, 2),
                  param("depth", "Depth", 0, 8, 3, "ms", Scale::Linear, 1),
                  param("delay", "Delay", 2, 25, 7, "ms", Scale::Linear, 1),
                  param("voices", "Voices", 1, 3, 2, "", Scale::Int),
                  param("width", "Stereo width", 0, 100, 100, "%"),
                  param("mix", "Mix", 0, 100, 50, "%")}) {}
    int rate = key("rate"), depth = key("depth"), delay = key("delay"), voices = key("voices"), width = key("width"),
        mix = key("mix");
    FractionalDelay line;
    double phase = 0;  // of the sway, in cycles

    void allocate(double rate_, int) override { line.allocate(CHORUS_LONGEST_MS * 0.001 * rate_); }
    void start() override {
        line.clear();
        phase = 0;
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int count = std::max(1, (int)v(voices));
        double spread = v(width) / 100 * 0.5, m = v(mix) / 100;
        for (int j = 0; j < n; j++) {
            double cycles = phase + j * v(rate) / sr;
            for (int c = 0; c < 2; c++) {
                float sum = 0;
                for (int k = 0; k < count; k++) {
                    double ph = cycles + (double)k / count + (c == 1 ? spread : 0.0);
                    double d = (v(delay) + v(depth) * (0.5 + 0.5 * std::sin(2 * PI * ph))) * 0.001 * sr;
                    sum += line.read(j, c, d, x, n);
                }
                float wet = sum / count;
                y[c][j] = (float)(x[c][j] * (1 - m) + wet * m);
            }
        }
        line.write(x, n);
        phase = std::fmod(phase + n * v(rate) / sr, 1.0);
    }
};

// --- Delay ------------------------------------------------------------------------------------------
// Echoes of the sound, each one quieter (feedback), on each side at its own time; ping-pong sends
// each echo to the other side. A filter in the echoes' loop makes each one a little darker, like
// tape. Sync sets the times in beats (at the project's tempo) instead of milliseconds.

constexpr double DELAY_LONGEST_S = 4.0;

class Delay : public Effect {
public:
    Delay()
        : Effect("delay", "Delay",
                 {param("left", "Left time", 1, 2000, 250, "ms", Scale::Log),
                  param("right", "Right time", 1, 2000, 375, "ms", Scale::Log),
                  toggle("sync", "Sync to the tempo"),
                  choice("left_beats", "Left (synced)",
                         {"1/16", "1/8 triplet", "1/8", "dotted 1/8", "1/4", "dotted 1/4", "1/2", "1 bar (4/4)"}, 2.0),
                  choice("right_beats", "Right (synced)",
                         {"1/16", "1/8 triplet", "1/8", "dotted 1/8", "1/4", "dotted 1/4", "1/2", "1 bar (4/4)"}, 3.0),
                  param("feedback", "Feedback", 0, 95, 40, "%"),
                  toggle("pingpong", "Ping-pong"),
                  param("high_cut", "Echoes' high cut", 500, 20000, 6000, "Hz", Scale::Log),
                  param("mix", "Mix", 0, 100, 25, "%")}) {}
    int left = key("left"), right = key("right"), sync = key("sync"), leftBeats = key("left_beats"),
        rightBeats = key("right_beats"), feedback = key("feedback"), pingpong = key("pingpong"),
        highCut = key("high_cut"), mix = key("mix");
    int64_t size = 1, at = 0;
    std::vector<double> ring[2];
    double damped[2] = {0, 0};  // the echoes' low-pass state

    void allocate(double rate, int) override {
        size = (int64_t)(DELAY_LONGEST_S * rate) + 1;
        for (auto& r : ring) r.assign(size, 0.0);
    }
    void start() override {
        for (auto& r : ring) std::fill(r.begin(), r.end(), 0.0);
        at = 0;
        damped[0] = damped[1] = 0;
    }

    // (the Python program slept a second after the last sound, losing an echo due later than that)
    double sleepAfter() const override {
        int64_t d[2];
        times(d);
        return SLEEP_AFTER_S * sr + std::max(d[0], d[1]);
    }

public:
    // (left, right) delays in frames
    void times(int64_t out[2]) const {
        double seconds[2];
        if (v(sync) >= 0.5) {
            double beatS = 60.0 / std::max(tempo(), 1.0);
            seconds[0] = DELAY_BEATS[std::min(7, std::max(0, (int)v(leftBeats)))] * beatS;
            seconds[1] = DELAY_BEATS[std::min(7, std::max(0, (int)v(rightBeats)))] * beatS;
        } else {
            seconds[0] = v(left) / 1000;
            seconds[1] = v(right) / 1000;
        }
        for (int c = 0; c < 2; c++) out[c] = std::min(size - 1, std::max<int64_t>(1, (int64_t)roundEven(seconds[c] * sr)));
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int64_t delays[2];
        times(delays);
        double fb = v(feedback) / 100, cut = std::min(v(highCut), sr * 0.45);
        double damp = std::exp(-2 * PI * cut / sr), m = v(mix) / 100;
        bool pp = v(pingpong) >= 0.5;
        for (int j = 0; j < n; j++) {
            double echo[2];
            for (int c = 0; c < 2; c++) {
                double heard = ring[c][((at - delays[c]) % size + size) % size];
                // (the echoes' one-pole low-pass, its state kept as scipy's lfilter keeps it: damp × the
                // last output, so a changed high cut takes over the way the script's does)
                echo[c] = (1 - damp) * heard + damped[c];
                damped[c] = damp * echo[c];
            }
            for (int c = 0; c < 2; c++) {
                double dry = pp ? (c == 0 ? ((double)x[0][j] + (double)x[1][j]) / 2 : 0.0) : (double)x[c][j];
                ring[c][at] = dry + fb * (pp ? echo[1 - c] : echo[c]);
                y[c][j] = (float)((double)(float)(x[c][j] * (1 - m)) + echo[c] * m);
            }
            at = (at + 1) % size;
        }
    }
};

// --- Reverb (Freeverb) ------------------------------------------------------------------------------
// A room's echoes (Freeverb's design, by Jezar at Dreampoint): eight comb filters in parallel and
// four allpass filters after them, for each side, the right side's slightly longer (stereo). Size
// sets how long the echoes ring, damping how fast their highs fade, width how wide they are;
// pre-delay leaves a gap before them, low cut keeps the bass out of them.
//
// Its echoes come out about as loud as what goes in, whatever the size and damping: the combs' gain
// over the low mids (where most of music's energy is), mean(1 / (1 - g² |L|²)), is worked out when
// they change, and the echoes are divided by its square root.

const int COMBS[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};  // frames at 44.1 kHz
const int ALLPASSES[] = {556, 441, 341, 225};
const int SPREAD = 23;  // the right side's filters are this much longer
const double INPUT_GAIN = 0.015, WET_GAIN = 3.0, ALLPASS_FEEDBACK = 0.5;
constexpr double LONGEST_PREDELAY_MS = 100.0;

class Reverb : public Effect {
public:
    Reverb()
        : Effect("reverb", "Reverb",
                 {param("size", "Size", 0, 100, 60, "%"),
                  param("damping", "Damping", 0, 100, 50, "%"),
                  param("width", "Width", 0, 100, 100, "%"),
                  param("predelay", "Pre-delay", 0, LONGEST_PREDELAY_MS, 10, "ms"),
                  param("low_cut", "Low cut", 20, 1000, 150, "Hz", Scale::Log),
                  param("mix", "Mix", 0, 100, 25, "%")}) {}
    int size = key("size"), damping = key("damping"), width = key("width"), predelay = key("predelay"),
        lowCut = key("low_cut"), mix = key("mix");
    std::vector<std::vector<double>> combs, allpasses;  // rings: left 8 then right 8; stage j's left, right
    std::vector<size_t> combAt, allpassAt;
    std::vector<double> combLp;
    FractionalDelay pre;
    Filters cut;
    double level = 1.0;
    Rows delayed, wet;

    static int scaled(double d, double rate) { return std::max(1, (int)roundEven(d * rate / 44100)); }
    void allocate(double rate, int maxBlock) override {
        combs.clear();
        for (int side = 0; side < 2; side++)
            for (int d : COMBS) combs.emplace_back(scaled(d + side * SPREAD, rate), 0.0);
        allpasses.clear();
        for (int d : ALLPASSES)
            for (int side = 0; side < 2; side++) allpasses.emplace_back(scaled(d + side * SPREAD, rate), 0.0);
        combAt.assign(combs.size(), 0);
        allpassAt.assign(allpasses.size(), 0);
        combLp.assign(combs.size(), 0.0);
        pre.allocate(LONGEST_PREDELAY_MS * 0.001 * rate);
        delayed.allocate(maxBlock);
        wet.allocate(maxBlock);
    }
    void start() override {
        for (auto& r : combs) std::fill(r.begin(), r.end(), 0.0);
        for (auto& r : allpasses) std::fill(r.begin(), r.end(), 0.0);
        std::fill(combAt.begin(), combAt.end(), 0);
        std::fill(allpassAt.begin(), allpassAt.end(), 0);
        std::fill(combLp.begin(), combLp.end(), 0.0);
        pre.clear();
        cut = Filters();
        cut.set({biquad(Kind::Highpass, v(lowCut), sr)});
    }
    // the combs' feedback and their low-pass's coefficient for the size and damping (the coefficient
    // is per sample: p at 44.1 kHz is p^(44100/sr) at sr, so the highs die as fast at any rate)
    void loop(double& feedback, double& damp) const {
        feedback = v(size) / 100 * 0.28 + 0.7;
        damp = std::pow(v(damping) / 100 * 0.4, 44100.0 / sr);
    }
    // the combs' mean power gain over 100-1000 Hz: 1 / (1 - g² |L|²), L their loop's one-pole low-pass
    static double combPower(double feedback, double damp, double rate) {
        double sum = 0;
        for (int k = 0; k < 64; k++) {
            double w = 2 * PI * (100.0 + (1000.0 - 100.0) * k / 63) / rate;
            double l = (1 - damp) * (1 - damp) / (1 - 2 * damp * std::cos(w) + damp * damp);
            sum += 1 / (1 - feedback * feedback * l);
        }
        return sum / 64;
    }
    void changed() override {
        cut.set({biquad(Kind::Highpass, v(lowCut), sr)});
        double feedback, damp;
        loop(feedback, damp);
        level = 1 / std::sqrt(combPower(feedback, damp, sr));  // (the echoes as loud as the input)
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        double feedback, damp;
        loop(feedback, damp);
        double wd = v(width) / 100;
        double wet1 = WET_GAIN * level * (wd / 2 + 0.5), wet2 = WET_GAIN * level * ((1 - wd) / 2);
        float* const* d = delayed.rows();
        double pd = v(predelay) * 0.001 * sr;
        for (int j = 0; j < n; j++)
            for (int c = 0; c < 2; c++) d[c][j] = pre.read(j, c, pd, x, n);
        pre.write(x, n);
        float* const* w = wet.rows();
        size_t half = combs.size() / 2;
        for (int j = 0; j < n; j++) {
            double mono = ((double)d[0][j] + (double)d[1][j]) * INPUT_GAIN;
            double sides[2] = {0, 0};
            for (size_t k = 0; k < combs.size(); k++) {
                size_t a = combAt[k];
                double heard = combs[k][a];
                combLp[k] = heard * (1.0 - damp) + combLp[k] * damp;
                combs[k][a] = mono + feedback * combLp[k];
                combAt[k] = a + 1 >= combs[k].size() ? 0 : a + 1;
                sides[k < half ? 0 : 1] += heard;
            }
            for (size_t stage = 0; stage < allpasses.size() / 2; stage++)
                for (int c = 0; c < 2; c++) {
                    size_t row = 2 * stage + c, a = allpassAt[row];
                    double held = allpasses[row][a];
                    allpasses[row][a] = sides[c] + held * ALLPASS_FEEDBACK;
                    allpassAt[row] = a + 1 >= allpasses[row].size() ? 0 : a + 1;
                    sides[c] = held - sides[c];
                }
            w[0][j] = (float)(sides[0] * wet1 + sides[1] * wet2);
            w[1][j] = (float)(sides[1] * wet1 + sides[0] * wet2);
        }
        cut.run(w, w, n);
        double m = v(mix) / 100;
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) y[c][i] = (float)(x[c][i] * (1 - m) + w[c][i] * m);
    }
};

}  // namespace

std::unique_ptr<Effect> makeChorus() { return std::make_unique<Chorus>(); }
std::unique_ptr<Effect> makeDelay() { return std::make_unique<Delay>(); }
std::unique_ptr<Effect> makeReverb() { return std::make_unique<Reverb>(); }

}  // namespace effects
