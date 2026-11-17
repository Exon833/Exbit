// Tuner: lets the sound through untouched and shows, in its window, the note being played, how
// many cents it is off that note, and its frequency. The reference pitch (the A above middle C,
// 440 Hz unless set otherwise: 415 for baroque pitch, 432, 442…) sets where the notes are.
//
// The audio thread only copies the sound for the window (effects/tap.h), and only while the window
// is open; the pitch is found on the window's thread as it repaints (every 50 ms), by the master
// scope's detector (audio/scope.h's detectPitch: YIN, the difference function from an FFT), over
// the newest 70 ms or so — long enough for a bass's low E. Its meters() carry what the window shows:
// the note's name, the cents off (−50…+50) and the frequency; the window draws them large.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "audio/scope.h"
#include "effects/builtin.h"
#include "effects/tap.h"

namespace effects {

TunerReading tunerReading(double hz, double reference) {
    TunerReading r;
    if (!(hz > 0) || !(reference > 0)) return r;
    double semis = 12 * std::log2(hz / reference);  // (from the A above middle C)
    double nearest = std::round(semis);
    r.hz = hz;
    r.cents = 100 * (semis - nearest);
    // (the name of the note at the same place on a 440 Hz scale: scope::noteName's C#4 style)
    r.note = scope::noteName(440.0 * std::pow(2.0, nearest / 12));
    return r;
}

namespace {

constexpr double LOWEST_HZ = 30, HIGHEST_HZ = 2000;  // a bass's low B (31 Hz) to a whistle
constexpr double HOLD_S = 0.6;  // a note stays shown this long after the pitch is lost

class Tuner : public Effect {
public:
    Tuner() : Effect("tuner", "Tuner", {param("reference", "Reference pitch (A)", 415, 466, 440, "Hz", Scale::Linear, 1)}) {}
    int reference = key("reference");
    mutable SoundTap tap;  // (written by the audio thread, read by meters())
    // (the window's thread)
    mutable std::vector<float> heard;
    mutable TunerReading shown;
    mutable std::chrono::steady_clock::time_point found;  // when `shown` was last heard

    // samples the detector needs for the lowest note: its longest period twice, and some
    static size_t window(double rate) { return (size_t)std::ceil(2 * rate / LOWEST_HZ) + 64; }

    void allocate(double rate, int) override { tap.allocate(window(rate), rate); }
    void start() override {}
    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        for (int c = 0; c < 2; c++) std::copy(x[c], x[c] + n, y[c]);
        tap.write(x, n);
    }

    // [note (its name; the fraction: none), cents off (−50…+50 as 0…1), frequency] — the pitch is
    // found here, on the caller's thread
    std::vector<Meter> meters() const override {
        auto now = std::chrono::steady_clock::now();
        size_t n = std::min(window(sr), tap.size());
        if (n) {
            heard.resize(n);
            if (tap.read(heard.data(), n)) {  // (something new: else the effect sleeps, or no sound came)
                scope::Pitch p = scope::detectPitch(heard.data(), n, (int)sr, LOWEST_HZ, HIGHEST_HZ);
                if (p.hz > 0) {
                    shown = tunerReading(p.hz, v(reference));
                    found = now;
                }
            }
        }
        if (shown.hz > 0 && std::chrono::duration<double>(now - found).count() > HOLD_S) shown = TunerReading();
        if (!(shown.hz > 0)) return {{"Note", 0, "—"}, {"Cents", 0.5, "—"}, {"Frequency", 0, "—"}};
        char cents[32], hz[32];
        std::snprintf(cents, sizeof cents, "%+.1f ct", shown.cents);
        std::snprintf(hz, sizeof hz, shown.hz < 1000 ? "%.2f Hz" : "%.1f Hz", shown.hz);
        double where = std::log(shown.hz / LOWEST_HZ) / std::log(HIGHEST_HZ / LOWEST_HZ);
        return {{"Note", 1, shown.note},
                {"Cents", std::clamp(0.5 + shown.cents / 100, 0.0, 1.0), cents},
                {"Frequency", std::clamp(where, 0.0, 1.0), hz}};
    }
};

}  // namespace

std::unique_ptr<Effect> makeTuner() { return std::make_unique<Tuner>(); }

}  // namespace effects
