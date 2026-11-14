// Utility (effects/utility.py, its twin in native/effects.cpp): the small fixes every mix needs.
// Gain; width (0 % mono, 100 % as it is, up to 200 % wider: the difference between the sides made
// louder or quieter); Mono (the same as 0 % width); a polarity flip of either side; Bass mono:
// below the frequency the sound is mono (its lows can't be heard from one side only, nor cancel out
// on a mono speaker), the rest keeps its width.
//
// Mid and side: mid = (L + R) / 2 and side = (L − R) / 2, so L = mid + side and R = mid − side.
// Width scales the side; Bass mono takes the side's lows away with a Linkwitz-Riley high-pass. That
// filter turns the phase of the side, so the mid goes through the same crossover's all-pass (its
// low-pass plus its high-pass, which add up flat): mid and side keep in step and the stereo image
// above the frequency stays as it was. Switching Bass mono on or off crossfades over one block (the
// filters always run), so it doesn't click.
#include <algorithm>

#include "effects/builtin.h"

namespace effects {
namespace {

class Utility : public Effect {
public:
    Utility()
        : Effect("utility", "Utility",
                 {param("gain", "Gain", -36, 36, 0, "dB", Scale::Linear, 1),
                  param("width", "Width", 0, 200, 100, "%"),
                  toggle("mono", "Mono"),
                  toggle("invert_left", "Invert left"),
                  toggle("invert_right", "Invert right"),
                  toggle("bass_mono", "Bass mono"),
                  param("bass_freq", "Bass mono below", 40, 500, 120, "Hz", Scale::Log)}) {}
    int gain = key("gain"), width = key("width"), mono = key("mono"), invertLeft = key("invert_left"),
        invertRight = key("invert_right"), bassMono = key("bass_mono"), bassFreq = key("bass_freq");
    Filters highpass, lowpass;  // (rows: mid, side)
    Rows rows, high, low;
    bool wasOn = false;

    void setFilters() {
        Sos s[2];
        linkwitzRiley(Kind::Highpass, v(bassFreq), sr, s);
        highpass.set(s, 2);
        linkwitzRiley(Kind::Lowpass, v(bassFreq), sr, s);
        lowpass.set(s, 2);
    }
    void allocate(double, int maxBlock) override {
        rows.allocate(maxBlock);
        high.allocate(maxBlock);
        low.allocate(maxBlock);
    }
    void start() override {
        setFilters();
        highpass.reset();
        lowpass.reset();
        wasOn = v(bassMono) >= 0.5;
    }
    void changed() override { setFilters(); }
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        double flipLeft = v(invertLeft) >= 0.5 ? -1.0 : 1.0, flipRight = v(invertRight) >= 0.5 ? -1.0 : 1.0;
        float* const* r = rows.rows();
        for (int j = 0; j < n; j++) {
            double left = x[0][j] * flipLeft, right = x[1][j] * flipRight;
            r[0][j] = (float)((left + right) / 2);
            r[1][j] = (float)((left - right) / 2);
        }
        float* const* hi = high.rows();
        float* const* lo = low.rows();
        highpass.run(r, hi, n);  // (always: switching Bass mono is seamless)
        lowpass.run(r, lo, n);
        bool on = v(bassMono) >= 0.5;
        double w = v(mono) >= 0.5 ? 0.0 : v(width) / 100, g = dbToGain(v(gain));
        for (int j = 0; j < n; j++) {
            double left = x[0][j] * flipLeft, right = x[1][j] * flipRight;
            double mid = (left + right) / 2, side = (left - right) / 2;
            if (on || wasOn) {
                double midOn = (double)lo[0][j] + hi[0][j], sideOn = hi[1][j];
                if (on != wasOn) {  // (switched: crossfade over this block)
                    double fade = (j + 1.0) / n;
                    fade = on ? fade : 1 - fade;
                    mid += (midOn - mid) * fade;
                    side += (sideOn - side) * fade;
                } else {
                    mid = midOn;
                    side = sideOn;
                }
            }
            double s = side * w;
            y[0][j] = (float)((mid + s) * g);
            y[1][j] = (float)((mid - s) * g);
        }
        wasOn = on;
    }
};

}  // namespace

std::unique_ptr<Effect> makeUtility() { return std::make_unique<Utility>(); }

}  // namespace effects
