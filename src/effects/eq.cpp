// EQ (effects/eq.py, its twin in native/effects.cpp): a low cut, a low shelf, a mid peak, a high
// shelf and a high cut, each a second-order filter (RBJ's cookbook). The window draws the response.
// Low cut at its lowest (10 Hz) and high cut at its highest (22 kHz) are off.
#include <algorithm>

#include "effects/builtin.h"

namespace effects {

double eqX(double freq) { return std::log(freq / EQ_LOW) / std::log(EQ_HIGH / EQ_LOW); }

namespace {

class EQ : public Effect {
public:
    EQ()
        : Effect("eq", "EQ",
                 {param("low_cut", "Low cut", 10, 1000, 10, "Hz", Scale::Log),
                  param("low_freq", "Low shelf", 30, 500, 100, "Hz", Scale::Log),
                  param("low_gain", "Low shelf gain", -18, 18, 0, "dB", Scale::Linear, 1),
                  param("mid_freq", "Mid", 100, 10000, 1000, "Hz", Scale::Log),
                  param("mid_gain", "Mid gain", -18, 18, 0, "dB", Scale::Linear, 1),
                  param("mid_q", "Mid width (Q)", 0.3, 8, 0.9, "", Scale::Log, 2),
                  param("high_freq", "High shelf", 1000, 16000, 6000, "Hz", Scale::Log),
                  param("high_gain", "High shelf gain", -18, 18, 0, "dB", Scale::Linear, 1),
                  param("high_cut", "High cut", 1000, 22000, 22000, "Hz", Scale::Log),
                  param("output", "Output", -24, 24, 0, "dB", Scale::Linear, 1)}) {
        curveYGrid = {{0.0, "0 dB"}, {0.5, "+12"}, {-0.5, "−12"}};
        curveXGrid = {{eqX(100), "100"}, {eqX(1000), "1k"}, {eqX(10000), "10k"}};
    }

    int lowCut = key("low_cut"), lowFreq = key("low_freq"), lowGain = key("low_gain"), midFreq = key("mid_freq"),
        midGain = key("mid_gain"), midQ = key("mid_q"), highFreq = key("high_freq"), highGain = key("high_gain"),
        highCut = key("high_cut"), output = key("output");
    Filters filters;
    double gain = 1.0;

    void sections(double rate, Sos out[5]) const {
        Sos flat = biquad(Kind::Flat, 0, rate);
        out[0] = v(lowCut) > 10.5 ? biquad(Kind::Highpass, v(lowCut), rate) : flat;
        out[1] = v(lowGain) != 0 ? biquad(Kind::Lowshelf, v(lowFreq), rate, 0.707, v(lowGain)) : flat;
        out[2] = v(midGain) != 0 ? biquad(Kind::Peak, v(midFreq), rate, v(midQ), v(midGain)) : flat;
        out[3] = v(highGain) != 0 ? biquad(Kind::Highshelf, v(highFreq), rate, 0.707, v(highGain)) : flat;
        out[4] = v(highCut) < std::min(21500.0, rate * 0.45) ? biquad(Kind::Lowpass, v(highCut), rate) : flat;
    }
    void start() override {
        Sos s[5];
        sections(sr, s);
        filters = Filters();
        filters.set(s, 5);
        gain = 1.0;
    }
    void changed() override {
        Sos s[5];
        sections(sr, s);
        filters.set(s, 5);  // (always five sections: the filters keep their state)
        gain = dbToGain(v(output));
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        filters.run(x, y, n);
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) y[c][i] = (float)(y[c][i] * gain);
    }

    bool hasCurve() const override { return true; }
    std::vector<CurvePoint> curve() const override {  // 200 points, 20 Hz…20 kHz; ±24 dB fills the height
        Sos s[5];
        sections(sr, s);
        std::vector<CurvePoint> points;
        for (int i = 0; i < 200; i++) {
            double x = i / 199.0;
            double f = std::min(EQ_LOW * std::pow(EQ_HIGH / EQ_LOW, x), sr * 0.499);
            double db = 20 * std::log10(std::max(magnitude(s, 5, f, sr), 1e-6)) + v(output);
            points.push_back({x, db / 24});
        }
        return points;
    }
};

}  // namespace

std::unique_ptr<Effect> makeEq() { return std::make_unique<EQ>(); }

}  // namespace effects
