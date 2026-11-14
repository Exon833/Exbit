// The distortion effects (effects/saturation.py, bitcrusher.py, and their twins in
// native/effects.cpp, which is what plays).
#include <algorithm>

#include "effects/builtin.h"

namespace effects {
namespace {

// --- Saturation -------------------------------------------------------------------------------------
// Warmth and grit by bending the waveform. Drive pushes it into one of four curves — Soft (tanh,
// tape-like), Hard (clipping), Tube (lopsided, so even harmonics too) and Fold (the peaks fold back
// over, bright and metallic) — at twice the sample rate, so the new harmonics don't fold back down
// as aliasing. Tone darkens the result; the level is evened out a little as drive rises, and
// Output trims it.

class Saturation : public Effect {
public:
    Saturation()
        : Effect("saturation", "Saturation",
                 {param("drive", "Drive", 0, 36, 12, "dB", Scale::Linear, 1),
                  choice("shape", "Shape", {"Soft", "Hard", "Tube", "Fold"}),
                  param("tone", "Tone", 1000, 20000, 20000, "Hz", Scale::Log),
                  param("mix", "Mix", 0, 100, 100, "%"),
                  param("output", "Output", -24, 12, 0, "dB", Scale::Linear, 1)}) {}
    int drive = key("drive"), shape = key("shape"), toneFreq = key("tone"), mix = key("mix"), output = key("output");
    Filters up, down, tone;
    Rows wide, shaped, back, wet;

    void allocate(double, int maxBlock) override {
        wide.allocate(2 * maxBlock);
        shaped.allocate(2 * maxBlock);
        back.allocate(2 * maxBlock);
        wet.allocate(maxBlock);
    }
    void start() override {
        up = Filters();
        down = Filters();
        up.set(OVERSAMPLING_SOS, 4);
        down.set(OVERSAMPLING_SOS, 4);
        tone = Filters();
        tone.set({biquad(Kind::Lowpass, v(toneFreq), sr), biquad(Kind::Highpass, 10, sr)});
    }
    void changed() override {
        Sos low = v(toneFreq) < 19500 ? biquad(Kind::Lowpass, v(toneFreq), sr) : biquad(Kind::Flat, 0, sr);
        tone.set({low, biquad(Kind::Highpass, 10, sr)});  // (and no DC: Tube is lopsided)
    }

public:
    static float curve(int kind, float x) {
        if (kind == 0) return std::tanh(x);
        if (kind == 1) return std::min(1.0f, std::max(-1.0f, x));
        if (kind == 2) return x >= 0 ? 1 - std::exp(-x) : -(1 - std::exp(1.25f * x)) * 0.8f;
        return std::sin((float)PI / 2 * x);  // (folds back past ±1)
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        float gain = (float)dbToGain(v(drive));
        int kind = (int)v(shape);
        float* const* w = wide.rows();
        float* const* s = shaped.rows();
        float* const* b = back.rows();
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) {
                w[c][2 * i] = x[c][i] * 2;  // (×2: the zeros halve the level)
                w[c][2 * i + 1] = 0;
            }
        up.run(w, s, 2 * n);
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 2 * n; i++) s[c][i] = curve(kind, s[c][i] * gain);
        down.run(s, b, 2 * n);
        float* const* t = wet.rows();
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) t[c][i] = b[c][2 * i];
        tone.run(t, t, n);
        double outGain = dbToGain(v(output)), m = v(mix) / 100, level = std::sqrt((double)gain);
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) {
                float wetValue = (float)(t[c][i] / level * outGain);  // (louder drive, not much louder sound)
                y[c][i] = (float)(x[c][i] * (1 - m) + wetValue * m);
            }
    }
};

// --- Bit-crusher ------------------------------------------------------------------------------------
// The lo-fi sound of old samplers and games. Bits rounds every value to fewer steps (grainy noise);
// downsample holds each value for that many frames (a lower sample rate: metallic, aliased highs).

class Bitcrusher : public Effect {
public:
    Bitcrusher()
        : Effect("bitcrusher", "Bit-crusher",
                 {param("bits", "Bits", 1, 16, 8, "", Scale::Int),
                  param("downsample", "Downsample", 1, 64, 4, "×", Scale::Int),
                  param("mix", "Mix", 0, 100, 100, "%"),
                  param("output", "Output", -24, 12, 0, "dB", Scale::Linear, 1)}) {}
    int bits = key("bits"), downsample = key("downsample"), mix = key("mix"), output = key("output");
    float held[2] = {0, 0};  // the value being held when a block ended
    int64_t count = 0;       // frames of it held so far

    void start() override {
        held[0] = held[1] = 0;
        count = 0;
    }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int factor = std::max(1, (int)v(downsample));
        float steps = (float)std::pow(2.0, (int)v(bits) - 1);
        double m = v(mix) / 100, outGain = dbToGain(v(output));
        float last[2] = {held[0], held[1]};
        for (int j = 0; j < n; j++) {
            int64_t startFrame = j - (count + j) % factor;  // where its held value was taken (< 0: an earlier block)
            for (int c = 0; c < 2; c++) {
                float h = startFrame >= 0 ? x[c][startFrame] : held[c];
                last[c] = h;
                float crushed = (float)roundEven(h * steps) / steps;
                y[c][j] = (float)((x[c][j] * (1 - m) + crushed * m) * outGain);
            }
        }
        held[0] = last[0];
        held[1] = last[1];
        count = (count + n) % factor;
    }
};

}  // namespace

std::unique_ptr<Effect> makeSaturation() { return std::make_unique<Saturation>(); }
std::unique_ptr<Effect> makeBitcrusher() { return std::make_unique<Bitcrusher>(); }

}  // namespace effects
