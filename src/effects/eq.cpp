// EQ (effects/eq.py, its twin in native/effects.cpp): a low cut, a low shelf, a mid peak, a high
// shelf and a high cut, each a second-order filter (RBJ's cookbook). The window draws the response.
// Low cut at its lowest (10 Hz) and high cut at its highest (22 kHz) are off.
//
// Behind the response the window shows what the EQ hears (its analyzer): the spectrum of the sound
// going in ("Pre") or coming out ("Post"), on the same frequency scale, rising at once and falling
// back slowly. The audio thread only copies the sound for it (effects/tap.h) while the window looks;
// the FFT (audio/scope.h's Spectrum, as the master scope's) runs on the window's thread as it repaints.
#include <algorithm>
#include <atomic>

#include "audio/scope.h"
#include "effects/builtin.h"
#include "effects/tap.h"

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

    // --- the analyzer ---
    enum Listen { OFF, PRE, POST };
    std::atomic<int> listen{POST};
    SoundTap tap;
    // (the window's thread)
    scope::Spectrum spectrum;
    std::vector<float> heard;

    // its FFT: about 85 ms of sound (4096 points at 48 kHz)
    static size_t fftSize(double rate) {
        size_t n = 1024;
        while ((double)n < rate / 12) n <<= 1;
        return n;
    }
    void allocate(double rate, int) override { tap.allocate(fftSize(rate), rate); }

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
        int l = listen.load(std::memory_order_relaxed);
        if (l == PRE) tap.write(x, n);
        filters.run(x, y, n);
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) y[c][i] = (float)(y[c][i] * gain);
        if (l == POST) tap.write(y, n);
    }

    std::vector<std::string> analyzerChoices() const override { return {"Off", "Pre", "Post"}; }
    int analyzerChoice() const override { return listen.load(std::memory_order_relaxed); }
    void setAnalyzerChoice(int choice) override {
        listen.store(std::clamp(choice, (int)OFF, (int)POST), std::memory_order_relaxed);
        spectrum.reset();
    }
    // the levels -90…+12 dB as y -1…1 (room above 0 dB for the tilt), tilted 4.5 dB per octave
    // about 1 kHz (music then looks about flat), at bands spread as the curve's x (EQ_LOW…EQ_HIGH,
    // by frequency's logarithm)
    bool analyzer(int count, double dt, std::vector<CurvePoint>& out) override {
        out.clear();
        if (listen.load(std::memory_order_relaxed) == OFF || count <= 0 || !tap.size()) return false;
        heard.resize(tap.size());
        tap.read(heard.data(), heard.size());
        scope::SpectrumSettings settings;
        settings.fftSize = (int)tap.size();
        settings.fall = ANALYZER_FALL;
        settings.slope = 4.5;
        settings.floorDb = ANALYZER_FLOOR;
        spectrum.analyse(heard.data(), heard.size(), (int)sr, settings, count, dt);
        const auto& levels = spectrum.levels();
        auto power = [&](int b) {
            return std::pow(10.0, levels[(size_t)std::clamp(b, 0, (int)levels.size() - 1)] / 10);
        };
        for (int b = 0; b < (int)levels.size(); b++) {
            // (smoothed: a band's narrow dips filled from its neighbours', its peaks kept)
            double around = 10 * std::log10(0.25 * power(b - 1) + 0.5 * power(b) + 0.25 * power(b + 1));
            double level = std::max((double)levels[(size_t)b], around);
            double y = -1 + 2 * (level - ANALYZER_FLOOR) / (ANALYZER_TOP - ANALYZER_FLOOR);
            out.push_back({eqX(spectrum.bandHz(b)), std::clamp(y, -1.0, 1.0)});
        }
        return !out.empty();
    }
    static constexpr double ANALYZER_FLOOR = -90, ANALYZER_TOP = 12, ANALYZER_FALL = 24;  // dB, dB, dB per second

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
