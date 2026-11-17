// Modulators: units of the patch space that move other units' knobs instead of changing the sound
// (Effect::isModulator; the cables that carry their values are audio/modulation.h's). Four kinds:
//
//   LFO            a wave (sine, triangle, saw up or down, square), in time with the song (1/32 to
//                  8 bars) or at a rate in Hz, with a phase; two-way (-1…1) or one-way (0…1).
//   Random         a new random value every step (in time with the song, or in Hz), from one to the
//                  next in a jump or a glide (Smooth); the values come from a seed, so the same song
//                  moves the same way every time it plays or is rendered.
//   Level Follower how loud what is wired into it is (its input, or its sidechain: another track's
//                  sound, a kick ducking a pad), followed with an attack and a release, × a gain; 0…1.
//   Macro          one knob (0…100 %): wire it to several knobs and turn them together.
//
// While the song plays, the LFO's and Random's place comes from the song's position (seconds, or
// beats on the tempo map), not from a counter of their own: playing from anywhere, looping, and
// rendering all move them exactly as playing straight through does. Stopped, they run on by
// themselves (a counter advanced by each piece's length).
//
// They let no sound through (their output is silence), so wiring one to OUT changes nothing. A Level
// Follower is worked out like any effect, as its input flows in (audio/patch.cpp works out modulators
// that have something wired in even though their output reaches nowhere); the others need no sound.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "effects/builtin.h"

namespace effects {
namespace {

// the musical lengths a synced LFO or Random step can take, and their names (in beats; bars: × the
// bar's length, so negative here: -1 = one bar)
const std::vector<std::string> LENGTH_NAMES = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"};
const double LENGTH_BEATS[] = {0.125, 0.25, 0.5, 1, 2, -1, -2, -4, -8};

double lengthInBeats(int index, double beatsPerBar) {
    index = std::max(0, std::min((int)LENGTH_NAMES.size() - 1, index));
    double b = LENGTH_BEATS[index];
    return b > 0 ? b : -b * beatsPerBar;
}

// how far along (in cycles) a wave is: from the song's position while playing, else from its own count
struct Cycles {
    double free = 0;  // (stopped: its own count of cycles)
    double at(const ModClock& c, bool synced, double rateHz, int lengthIndex) {
        double cycles;
        if (synced) {
            double beats = lengthInBeats(lengthIndex, c.beatsPerBar);
            if (c.playing) return c.beats / beats;
            cycles = free;
            free += c.frames / c.sr * (c.tempo / 60.0) / beats;
        } else {
            if (c.playing) return c.seconds * rateHz;
            cycles = free;
            free += c.frames / c.sr * rateHz;
        }
        if (free > 1e6) free -= std::floor(free);  // (keep precision over days)
        return cycles;
    }
};

std::vector<Meter> nowMeter(double value, bool twoWay) {
    double fraction = twoWay ? (value + 1) / 2 : value;
    char text[32];
    std::snprintf(text, sizeof text, "%+.2f", value);
    return {{"Now", std::max(0.0, std::min(1.0, fraction)), text}};
}

class Modulator : public Effect {
public:
    using Effect::Effect;
    bool isModulator() const override { return true; }
    void start() override {}
    void block(const float* const*, const float* const*, float* const* y, int n) override {
        std::memset(y[0], 0, sizeof(float) * n);
        std::memset(y[1], 0, sizeof(float) * n);
    }
};

class Lfo : public Modulator {
public:
    Lfo()
        : Modulator("mod_lfo", "LFO",
                    {choice("shape", "Shape", {"Sine", "Triangle", "Saw up", "Saw down", "Square"}),
                     toggle("sync", "In time", true),
                     choice("length", "Length", LENGTH_NAMES, 5),
                     param("rate", "Rate", 0.01, 40, 1, "Hz", Scale::Log),
                     param("phase", "Phase", 0, 360, 0, "°"),
                     choice("direction", "Direction", {"Both ways", "Up only"})}) {}
    int shape = key("shape"), sync = key("sync"), length = key("length"), rate = key("rate"), phase = key("phase"),
        direction = key("direction");
    Cycles cycles;

    static double wave(int shape, double f) {  // f: 0…1 through a cycle
        switch (shape) {
        case 1: return f < 0.25 ? 4 * f : f < 0.75 ? 2 - 4 * f : 4 * f - 4;  // triangle (from 0, up first)
        case 2: return 2 * f - 1;                                            // saw up
        case 3: return 1 - 2 * f;                                            // saw down
        case 4: return f < 0.5 ? 1.0 : -1.0;                                 // square
        default: return std::sin(2 * PI * f);                                // sine
        }
    }
    double modulation(const ModClock& clock) override {
        double c = cycles.at(clock, v(sync) >= 0.5, v(rate), (int)v(length)) + v(phase) / 360.0;
        double f = c - std::floor(c);
        double value = wave((int)v(shape), f);
        if ((int)v(direction) == 1) value = (value + 1) / 2;
        lastModulation_.store(value, std::memory_order_relaxed);
        return value;
    }
    void start() override { cycles.free = 0; }
    std::vector<Meter> meters() const override { return nowMeter(lastModulation(), (int)v(direction) == 0); }
};

class Random : public Modulator {
public:
    Random()
        : Modulator("mod_random", "Random",
                    {toggle("sync", "In time", true),
                     choice("length", "Step", LENGTH_NAMES, 3),
                     param("rate", "Rate", 0.01, 40, 4, "Hz", Scale::Log),
                     param("smooth", "Smooth", 0, 100, 0, "%"),
                     param("seed", "Seed", 1, 999, 1, "", Scale::Int),
                     choice("direction", "Direction", {"Both ways", "Up only"})}) {}
    int sync = key("sync"), length = key("length"), rate = key("rate"), smooth = key("smooth"), seed = key("seed"),
        direction = key("direction");
    Cycles cycles;

    // the value of step k for this seed: -1…1 (a hash: the same for the same step, every time)
    static double stepValue(int64_t k, uint32_t seed) {
        uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
        x ^= x >> 31;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 33;
        return (double)(x >> 11) / (double)(1ull << 53) * 2 - 1;
    }
    double modulation(const ModClock& clock) override {
        double c = cycles.at(clock, v(sync) >= 0.5, v(rate), (int)v(length));
        double k = std::floor(c), f = c - k;
        uint32_t s = (uint32_t)v(seed);
        double now = stepValue((int64_t)k, s), before = stepValue((int64_t)k - 1, s);
        double glide = v(smooth) / 100;  // the first part of the step glides from the last value
        double value = now;
        if (glide > 0 && f < glide) value = before + (now - before) * (1 - std::cos(PI * f / glide)) / 2;
        if ((int)v(direction) == 1) value = (value + 1) / 2;
        lastModulation_.store(value, std::memory_order_relaxed);
        return value;
    }
    void start() override { cycles.free = 0; }
    std::vector<Meter> meters() const override { return nowMeter(lastModulation(), (int)v(direction) == 0); }
};

class Follower : public Modulator {
public:
    Follower()
        : Modulator("mod_follower", "Level Follower",
                    {param("attack", "Attack", 0.1, 1000, 10, "ms", Scale::Log),
                     param("release", "Release", 1, 5000, 200, "ms", Scale::Log),
                     param("gain", "Gain", -24, 24, 0, "dB", Scale::Linear, 1),
                     choice("listen", "Listen to", {"Its input", "Its sidechain"})},
                    true) {}
    int attack = key("attack"), release = key("release"), gain = key("gain"), listen = key("listen");
    double env = 0, up = 0, down = 0;

    void start() override { env = 0; }
    void changed() override {
        up = 1 - std::exp(-1.0 / (v(attack) / 1000 * sr));
        down = 1 - std::exp(-1.0 / (v(release) / 1000 * sr));
    }
    void block(const float* const* x, const float* const* side, float* const* y, int n) override {
        const float* const* in = (int)v(listen) == 1 && side ? side : x;
        for (int i = 0; i < n; i++) {
            double level = std::max(std::fabs(in[0][i]), std::fabs(in[1][i]));
            env += (level > env ? up : down) * (level - env);
        }
        if (env < 1e-9) env = 0;  // (no denormals)
        Modulator::block(x, side, y, n);
    }
    // it falls back to 0 before it sleeps (start() clears it then)
    double sleepAfter() const override { return (SLEEP_AFTER_S + 5 * v(release) / 1000) * sr; }
    double modulation(const ModClock&) override {
        double value = std::min(1.0, env * dbToGain(v(gain)));
        lastModulation_.store(value, std::memory_order_relaxed);
        return value;
    }
    std::vector<Meter> meters() const override { return nowMeter(lastModulation(), false); }
};

class Macro : public Modulator {
public:
    Macro() : Modulator("mod_macro", "Macro", {param("value", "Value", 0, 100, 0, "%")}) {}
    int value = key("value");
    double modulation(const ModClock&) override {
        double m = v(value) / 100;
        lastModulation_.store(m, std::memory_order_relaxed);
        return m;
    }
    std::vector<Meter> meters() const override { return nowMeter(v(value) / 100, false); }
};

}  // namespace

std::unique_ptr<Effect> makeLfo() { return std::make_unique<Lfo>(); }
std::unique_ptr<Effect> makeRandomModulator() { return std::make_unique<Random>(); }
std::unique_ptr<Effect> makeLevelFollower() { return std::make_unique<Follower>(); }
std::unique_ptr<Effect> makeMacro() { return std::make_unique<Macro>(); }

}  // namespace effects
