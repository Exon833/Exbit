// The dynamics effects (effects/compressor.py, multiband_compressor.py, gate.py, limiter.py, and
// their twins in native/effects.cpp, which is what plays).
#include <algorithm>
#include <cstdio>

#include "effects/builtin.h"

namespace effects {
namespace {

std::string dbText(double reduction) {  // f"{-reduction:.1f} dB"
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f dB", -reduction);
    return buf;
}

// --- Compressor -------------------------------------------------------------------------------------
// Turns the sound down when it goes over the threshold, by the ratio (4:1: 4 dB over comes out 1 dB
// over), easing in over the knee, at the attack's and release's speed; makeup gain brings it back
// up. Mix below 100 % blends in the uncompressed sound (parallel compression). With Sidechain on,
// what is wired into its SC jack decides how much it compresses (ducking under a kick).

class Compressor : public Effect {
public:
    Compressor()
        : Effect("compressor", "Compressor",
                 {param("threshold", "Threshold", -60, 0, -18, "dB", Scale::Linear, 1),
                  param("ratio", "Ratio", 1, 20, 4, ":1", Scale::Log, 1),
                  param("attack", "Attack", 0.1, 100, 10, "ms", Scale::Log, 1),
                  param("release", "Release", 10, 1000, 120, "ms", Scale::Log),
                  param("knee", "Knee", 0, 18, 6, "dB", Scale::Linear, 1),
                  param("makeup", "Makeup gain", 0, 24, 0, "dB", Scale::Linear, 1),
                  choice("detector", "Detector", {"Peak", "RMS"}),
                  param("mix", "Mix", 0, 100, 100, "%"),
                  toggle("sidechain", "Sidechain")},
                 true) {
        curveYGrid = {{-1 + 2 * 40.0 / 60, "−20 dB"}, {-1 + 2 * 20.0 / 60, "−40 dB"}};
        curveXGrid = {{40.0 / 60, "−20"}, {20.0 / 60, "−40"}};
    }
    int threshold = key("threshold"), ratio = key("ratio"), attack = key("attack"), release = key("release"),
        knee = key("knee"), makeup = key("makeup"), detector = key("detector"), mix = key("mix"),
        sideOn = key("sidechain");
    Envelope envelope;
    double lastDb = 0;
    std::atomic<double> reduction{0};  // for the meter (dB, 0 or more)
    std::vector<double> level, change;
    std::vector<float> curveFrames;

    void allocate(double, int maxBlock) override {
        level.assign(blocksFor(maxBlock), 0.0);
        change.assign(blocksFor(maxBlock), 0.0);
        curveFrames.assign(maxBlock, 0.0f);
    }
    void start() override {
        envelope = Envelope();
        lastDb = 0;
        reduction = 0;
    }
    void block(const float* const* x, const float* const* side, float* const* y, int n) override {
        const float* const* detect = v(sideOn) >= 0.5 && side ? side : x;
        int blocks = 0;
        double size = envelope.follow(detect, n, v(attack), v(release), sr, v(detector) >= 0.5, level.data(), &blocks);
        double lowest = 0;
        for (int i = 0; i < blocks; i++) {
            change[i] = compression(level[i], v(threshold), v(ratio), v(knee));
            lowest = i == 0 ? change[i] : std::min(lowest, change[i]);
        }
        perFrame(change.data(), blocks, size, n, lastDb, curveFrames.data());
        lastDb = change[blocks - 1];
        reduction = -lowest;
        double m = v(mix) / 100;
        for (int i = 0; i < n; i++) {
            float gain = (float)dbToGain((double)curveFrames[i] + v(makeup));
            float factor = (float)(gain * m + (1 - m));
            for (int c = 0; c < 2; c++) y[c][i] = x[c][i] * factor;
        }
    }
    std::vector<Meter> meters() const override {
        double r = reduction.load();
        return {{"Gain reduction", r / 24, dbText(r)}};
    }
    bool hasCurve() const override { return true; }
    std::vector<CurvePoint> curve() const override {  // out against in, from −60 to 0 dB
        std::vector<CurvePoint> points;
        for (int i = 0; i <= 120; i++) {
            double lv = -60 + 60.0 * i / 120;
            double out = lv + compression(lv, v(threshold), v(ratio), v(knee)) + v(makeup);
            points.push_back({(lv + 60) / 60, (out + 60) / 60 * 2 - 1});
        }
        return points;
    }
};

// --- Multi-band compressor --------------------------------------------------------------------------
// The sound split at two crossover frequencies into low, mid and high bands (Linkwitz-Riley
// crossovers, which add back up flat), each compressed on its own (its threshold, ratio and gain)
// and added together again: the bass held in check without dulling the highs, and so on.

std::vector<Param> multibandParams() {
    std::vector<Param> p = {param("low_cross", "Low / mid", 40, 1000, 200, "Hz", Scale::Log),
                            param("high_cross", "Mid / high", 1000, 12000, 3000, "Hz", Scale::Log)};
    const char* bands[3][2] = {{"low", "Low"}, {"mid", "Mid"}, {"high", "High"}};
    double thresholds[3] = {-20, -18, -16};
    for (int b = 0; b < 3; b++) {
        std::string k = bands[b][0], name = bands[b][1];
        p.push_back(param(k + "_threshold", name + " threshold", -60, 0, thresholds[b], "dB", Scale::Linear, 1));
        p.push_back(param(k + "_ratio", name + " ratio", 1, 20, 3, ":1", Scale::Log, 1));
        p.push_back(param(k + "_gain", name + " gain", -24, 24, 0, "dB", Scale::Linear, 1));
    }
    p.push_back(param("attack", "Attack", 0.1, 100, 8, "ms", Scale::Log, 1));
    p.push_back(param("release", "Release", 10, 1000, 150, "ms", Scale::Log));
    p.push_back(param("output", "Output", -24, 24, 0, "dB", Scale::Linear, 1));
    return p;
}

class MultibandCompressor : public Effect {
public:
    MultibandCompressor() : Effect("multiband_compressor", "Multi-band compressor", multibandParams()) {}
    int lowCross = key("low_cross"), highCross = key("high_cross"), attack = key("attack"), release = key("release"),
        output = key("output");
    int thresholds[3] = {key("low_threshold"), key("mid_threshold"), key("high_threshold")};
    int ratios[3] = {key("low_ratio"), key("mid_ratio"), key("high_ratio")};
    int bandGains[3] = {key("low_gain"), key("mid_gain"), key("high_gain")};
    Filters lowLp, lowHp, highLp, highHp, lowApLp, lowApHp;
    Envelope envelopes[3];
    double lastDb[3] = {0, 0, 0};
    std::atomic<double> reduction[3];
    Rows low, rest, band[3], apl, aph;
    std::vector<double> level, change;
    std::vector<float> curveFrames;

    void crossovers(double rate) {
        double lo = v(lowCross), hi = std::max(v(highCross), v(lowCross) * 1.5);
        Sos s[2];
        linkwitzRiley(Kind::Lowpass, lo, rate, s);
        lowLp.set(s, 2);
        linkwitzRiley(Kind::Highpass, lo, rate, s);
        lowHp.set(s, 2);
        linkwitzRiley(Kind::Lowpass, hi, rate, s);
        highLp.set(s, 2);
        lowApLp.set(s, 2);  // (the low band goes through the upper crossover's two halves as well: in phase)
        linkwitzRiley(Kind::Highpass, hi, rate, s);
        highHp.set(s, 2);
        lowApHp.set(s, 2);
    }
    void allocate(double, int maxBlock) override {
        for (Rows* r : {&low, &rest, &band[0], &band[1], &band[2], &apl, &aph}) r->allocate(maxBlock);
        level.assign(blocksFor(maxBlock), 0.0);
        change.assign(blocksFor(maxBlock), 0.0);
        curveFrames.assign(maxBlock, 0.0f);
    }
    void start() override {
        for (Filters* f : {&lowLp, &lowHp, &highLp, &highHp, &lowApLp, &lowApHp}) *f = Filters();
        crossovers(sr);
        for (int b = 0; b < 3; b++) {
            envelopes[b] = Envelope();
            lastDb[b] = 0;
            reduction[b] = 0;
        }
    }
    void changed() override { crossovers(sr); }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        float* const* l = low.rows();
        float* const* r = rest.rows();
        float* const* a = apl.rows();
        float* const* b2 = aph.rows();
        lowLp.run(x, l, n);
        lowApLp.run(l, a, n);
        lowApHp.run(l, b2, n);
        float* const* bands[3] = {band[0].rows(), band[1].rows(), band[2].rows()};
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) bands[0][c][i] = a[c][i] + b2[c][i];
        lowHp.run(x, r, n);
        highLp.run(r, bands[1], n);
        highHp.run(r, bands[2], n);
        for (int c = 0; c < 2; c++) std::fill(y[c], y[c] + n, 0.0f);
        for (int k = 0; k < 3; k++) {
            int blocks = 0;
            double size = envelopes[k].follow(bands[k], n, v(attack), v(release), sr, false, level.data(), &blocks);
            double lowest = 0;
            for (int i = 0; i < blocks; i++) {
                change[i] = compression(level[i], v(thresholds[k]), v(ratios[k]), 6.0);
                lowest = i == 0 ? change[i] : std::min(lowest, change[i]);
            }
            perFrame(change.data(), blocks, size, n, lastDb[k], curveFrames.data());
            lastDb[k] = change[blocks - 1];
            reduction[k] = -lowest;
            for (int i = 0; i < n; i++) {
                float gain = (float)dbToGain((double)curveFrames[i] + v(bandGains[k]));
                for (int c = 0; c < 2; c++) y[c][i] += bands[k][c][i] * gain;
            }
        }
        float outGain = (float)dbToGain(v(output));
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) y[c][i] *= outGain;
    }
    std::vector<Meter> meters() const override {
        const char* names[3] = {"Low reduction", "Mid reduction", "High reduction"};
        std::vector<Meter> m;
        for (int b = 0; b < 3; b++) {
            double r = reduction[b].load();
            m.push_back({names[b], r / 24, dbText(r)});
        }
        return m;
    }
};

// --- Gate -------------------------------------------------------------------------------------------
// Lets the sound through while it is louder than the threshold, and turns it down by the range
// (80 dB: silence) while it is quieter, after holding open for the hold time: cuts the hiss and
// spill between notes and hits. It closes only once the level is the hysteresis below the
// threshold, so a level hovering there doesn't chatter. With Sidechain on, what is wired into its
// SC jack opens it.

class Gate : public Effect {
public:
    Gate()
        : Effect("gate", "Gate",
                 {param("threshold", "Threshold", -80, 0, -40, "dB", Scale::Linear, 1),
                  param("range", "Range", 0, 80, 80, "dB"),
                  param("attack", "Attack", 0.05, 50, 0.5, "ms", Scale::Log, 2),
                  param("hold", "Hold", 0, 500, 30, "ms"),
                  param("release", "Release", 5, 2000, 100, "ms", Scale::Log),
                  param("hysteresis", "Hysteresis", 0, 12, 3, "dB", Scale::Linear, 1),
                  toggle("sidechain", "Sidechain")},
                 true) {}
    int threshold = key("threshold"), range = key("range"), attack = key("attack"), hold = key("hold"),
        release = key("release"), hysteresis = key("hysteresis"), sideOn = key("sidechain");
    double gain = 0, held = 0;  // gain 0 (shut, down by the range) … 1 (open); seconds it stays open
    bool open = false;
    std::atomic<double> meterGain{0};
    std::atomic<bool> meterOpen{false};
    std::vector<double> gains;
    std::vector<float> curveFrames;

    void allocate(double, int maxBlock) override {
        gains.assign(blocksFor(maxBlock), 0.0);
        curveFrames.assign(maxBlock, 0.0f);
    }
    void start() override {
        gain = held = 0;
        open = false;
        meterGain = 0;
        meterOpen = false;
    }
    void block(const float* const* x, const float* const* side, float* const* y, int n) override {
        const float* const* detect = v(sideOn) >= 0.5 && side ? side : x;
        int blocks = std::max(1, n / ENVELOPE_BLOCK);
        int per = n >= ENVELOPE_BLOCK ? ENVELOPE_BLOCK : n;
        double size = (double)n / blocks;
        double a = timeCoeff(v(attack), sr, size), r = timeCoeff(v(release), sr, size), blockS = size / sr;
        double last = gain;
        for (int b = 0; b < blocks; b++) {
            double peak = 0;
            for (int i = b * per; i < b * per + per; i++)
                peak = std::max({peak, (double)std::fabs(detect[0][i]), (double)std::fabs(detect[1][i])});
            double level = gainToDb(peak);
            if (level > v(threshold)) {
                open = true;
                held = v(hold) * 0.001;
            } else if (open && level < v(threshold) - v(hysteresis)) {
                held -= blockS;
                if (held <= 0) open = false;
            }
            double target = open ? 1.0 : 0.0;
            gain = target + (target > gain ? a : r) * (gain - target);
            gains[b] = gain;
        }
        double floor = v(range) >= 79.9 ? 0.0 : std::pow(10.0, -v(range) / 20);
        perFrame(gains.data(), blocks, size, n, last, curveFrames.data());
        for (int i = 0; i < n; i++) {
            float factor = (float)(floor + (1 - floor) * curveFrames[i]);
            for (int c = 0; c < 2; c++) y[c][i] = x[c][i] * factor;
        }
        meterGain = gain;
        meterOpen = open;
    }
    std::vector<Meter> meters() const override {
        double g = meterGain.load();
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f %%", g * 100);
        return {{meterOpen.load() ? "Open" : "Shut", g, buf}};
    }
};

// --- Limiter ----------------------------------------------------------------------------------------
// Keeps the sound under the ceiling, however loud it comes in (gain pushes it up first). It looks
// LOOKAHEAD_MS ahead (the track is that much late; delay compensation makes up for it) and turns
// down smoothly before each peak arrives, rather than clipping it, then back up at the release's
// speed. The gain needed for each frame is the lowest needed anywhere in the lookahead; that,
// averaged over the lookahead, falls smoothly and is still low enough when the peak comes. A last
// clip at the ceiling catches what is left (a release made faster mid-peak).

constexpr double LOOKAHEAD_MS = 2.0;

class Limiter : public Effect {
public:
    Limiter()
        : Effect("limiter", "Limiter",
                 {param("gain", "Gain", 0, 24, 0, "dB", Scale::Linear, 1),
                  param("ceiling", "Ceiling", -12, 0, -0.3, "dB", Scale::Linear, 1),
                  param("release", "Release", 1, 1000, 60, "ms", Scale::Log)}) {}
    int gainDb = key("gain"), ceilingDb = key("ceiling"), release = key("release");
    int ahead = 1;
    std::vector<std::array<float, 2>> audio;  // the frames still held back (ahead)
    std::vector<float> needed, lowest;        // the last frames' gains needed (ahead), the lookahead's lowest (ahead - 1)
    double released = 1.0;
    std::atomic<double> reduction{0};
    std::vector<float> neededAll, lowestAll, smooth, curveFrames;
    std::vector<double> curveBlocks;
    std::vector<std::array<float, 2>> boosted, tail;

    void allocate(double rate, int maxBlock) override {
        ahead = std::max(1, (int)roundEven(LOOKAHEAD_MS * 0.001 * rate));
        audio.assign(ahead, {0.0f, 0.0f});
        tail.assign(ahead, {0.0f, 0.0f});
        needed.assign(ahead, 1.0f);
        lowest.assign(ahead - 1, 1.0f);
        neededAll.assign(ahead + maxBlock, 1.0f);
        lowestAll.assign(ahead - 1 + maxBlock, 1.0f);
        smooth.assign(maxBlock, 1.0f);
        curveFrames.assign(maxBlock, 1.0f);
        curveBlocks.assign(blocksFor(maxBlock), 1.0);
        boosted.assign(maxBlock, {0.0f, 0.0f});
    }
    void start() override {
        std::fill(audio.begin(), audio.end(), std::array<float, 2>{0.0f, 0.0f});
        std::fill(needed.begin(), needed.end(), 1.0f);
        std::fill(lowest.begin(), lowest.end(), 1.0f);
        released = 1.0;
        reduction = 0;
    }
    int effectLatency() const override { return ahead; }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int L = ahead;
        float ceiling = (float)dbToGain(v(ceilingDb));
        float boost = (float)dbToGain(v(gainDb));
        std::copy(needed.begin(), needed.end(), neededAll.begin());
        for (int i = 0; i < n; i++) {
            boosted[i] = {x[0][i] * boost, x[1][i] * boost};
            float peak = std::max(std::fabs(boosted[i][0]), std::fabs(boosted[i][1]));
            neededAll[L + i] = std::min(1.0f, ceiling / std::max(peak, 1e-9f));
        }
        std::copy(lowest.begin(), lowest.end(), lowestAll.begin());
        for (int i = 0; i < n; i++) {  // for the frame L back: the lowest of what is coming
            float m = neededAll[i];
            for (int k = 1; k <= L; k++) m = std::min(m, neededAll[i + k]);
            lowestAll[L - 1 + i] = m;
        }
        for (int i = 0; i < n; i++) {  // falls over the lookahead
            double sum = 0;
            for (int k = 0; k < L; k++) sum += lowestAll[i + k];
            smooth[i] = (float)(sum / L);
        }
        std::copy(neededAll.begin() + n, neededAll.begin() + n + L, needed.begin());
        if (L > 1) std::copy(lowestAll.begin() + n, lowestAll.begin() + n + L - 1, lowest.begin());
        int blocks = std::max(1, n / ENVELOPE_BLOCK);
        int per = n >= ENVELOPE_BLOCK ? ENVELOPE_BLOCK : n;
        double size = (double)n / blocks;
        double r = timeCoeff(v(release), sr, size);
        double g = released;
        for (int b = 0; b < blocks; b++) {
            double low = smooth[b * per];
            for (int i = b * per; i < b * per + per; i++) low = std::min(low, (double)smooth[i]);
            g = std::min(low, 1.0 + r * (g - 1.0));  // down at once (it was eased already), up at the release's speed
            curveBlocks[b] = g;
        }
        perFrame(curveBlocks.data(), blocks, size, n, released, curveFrames.data());
        released = curveBlocks[blocks - 1];
        float least = 1e30f;
        for (int i = 0; i < n; i++) {
            float gain = std::min(smooth[i], curveFrames[i]);
            least = std::min(least, gain);
            std::array<float, 2> sample = i < L ? audio[i] : boosted[i - L];
            for (int c = 0; c < 2; c++) y[c][i] = std::min(ceiling, std::max(-ceiling, sample[c] * gain));
        }
        for (int k = 0; k < L; k++) {  // the last L frames of (held back, then this block) for the next block
            int index = n + k;
            tail[k] = index < L ? audio[index] : boosted[index - L];
        }
        audio.swap(tail);
        reduction = -gainToDb(least);
    }
    std::vector<Meter> meters() const override {
        double r = reduction.load();
        return {{"Gain reduction", r / 24, dbText(r)}};
    }
};

}  // namespace

std::unique_ptr<Effect> makeCompressor() { return std::make_unique<Compressor>(); }
std::unique_ptr<Effect> makeMultibandCompressor() { return std::make_unique<MultibandCompressor>(); }
std::unique_ptr<Effect> makeGate() { return std::make_unique<Gate>(); }
std::unique_ptr<Effect> makeLimiter() { return std::make_unique<Limiter>(); }

}  // namespace effects
