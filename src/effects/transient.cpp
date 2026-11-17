// The transient shaper: a drum's hit made harder or softer, its ring longer or shorter, whatever
// its level.
//
// How: the sound's level (its louder side, or with Sidechain on what is wired into its SC jack) is
// held at its peak over the last 20 ms, so a steady tone reads as one steady level, without the
// ripple of its waveform, and is followed in dB by envelope followers, a fast one and a slow one
// for each of the two jobs:
//   - the envelope: up at once (0.5 ms), falling over the release;
//   - for the attack, a slow follower of the envelope: up over the attack time, down with it. Where a
//     sound starts, the envelope is up before it: the envelope over it (dB) is how much of an
//     attack it is, and Attack turns that much up (+) or down (−);
//   - for the tail, a fast follower: up at once, falling in 20 ms. Where a sound dies away, the
//     envelope (slow to fall) lags above it: that difference is how much of a tail it is, and
//     Sustain turns that up or down.
// A steady tone has every follower on the same level: it passes untouched, loud or quiet, and so
// does any level, since only the followers' differences count (their ratios, not their level). At
// ±100 % an attack is changed by up to 15 dB, a tail by up to 24 dB.
//
// Output gain comes after, and Soft clip rounds off what would go over full scale (a tanh knee from
// half of it), for the hits Attack makes louder. The meter shows the gain change it applies.
#include <algorithm>
#include <cstdio>

#include "effects/builtin.h"

namespace effects {
namespace {

constexpr double HOLD_MS = 20.0;          // the level held at its peak this long
constexpr double FAST_ATTACK_MS = 0.5, FAST_RELEASE_MS = 20.0;
constexpr double FLOOR_DB = -100.0;       // the followers' lowest level
constexpr double ATTACK_RANGE_DB = 15.0;  // the most an attack is changed (at ±100 %)
constexpr double SUSTAIN_RANGE_DB = 24.0; // the most a tail is changed
constexpr double METER_DB = 24.0;         // the meter's full scale
constexpr double CLIP_KNEE = 0.5;         // soft clip: untouched below this

// the largest of the last `window` values pushed (a monotonic queue: no ripple from a steady
// tone's waveform, and O(1) a value)
struct RunningMax {
    std::vector<float> value;
    std::vector<int64_t> time;
    size_t capacity = 1, front = 0, count = 0;
    int64_t window = 1, now = 0;

    void allocate(int64_t frames) {  // (main thread)
        window = std::max<int64_t>(1, frames);
        capacity = (size_t)window + 1;
        value.assign(capacity, 0.0f);
        time.assign(capacity, 0);
        clear();
    }
    void clear() {
        front = count = 0;
        now = 0;
    }
    float push(float x) {
        while (count && value[(front + count - 1) % capacity] <= x) count--;  // (never the largest again)
        size_t back = (front + count) % capacity;
        value[back] = x;
        time[back] = now;
        count++;
        while (time[front] <= now - window) {  // (too old; the newest stays: count ≥ 1)
            front = (front + 1) % capacity;
            count--;
        }
        now++;
        return value[front];
    }
};

// the soft clip: straight up to CLIP_KNEE, then a tanh curve that never reaches 1
float softClip(double u) {
    double a = std::fabs(u);
    if (a <= CLIP_KNEE) return (float)u;
    double out = CLIP_KNEE + (1 - CLIP_KNEE) * std::tanh((a - CLIP_KNEE) / (1 - CLIP_KNEE));
    return (float)(u < 0 ? -out : out);
}

class TransientShaper : public Effect {
public:
    TransientShaper()
        : Effect("transient_shaper", "Transient Shaper",
                 {param("attack", "Attack", -100, 100, 0, "%"),
                  param("sustain", "Sustain", -100, 100, 0, "%"),
                  param("attack_time", "Attack time", 1, 100, 20, "ms", Scale::Log, 1),
                  param("release", "Release", 20, 2000, 200, "ms", Scale::Log),
                  param("output", "Output", -24, 24, 0, "dB", Scale::Linear, 1),
                  toggle("soft_clip", "Soft clip"),
                  toggle("sidechain", "Sidechain")},
                 true) {}
    int attack = key("attack"), sustain = key("sustain"), attackTime = key("attack_time"), release = key("release"),
        output = key("output"), clip = key("soft_clip"), sideOn = key("sidechain");
    RunningMax held;
    float lastHeld = -1.0f;
    double heldDb = FLOOR_DB, envelope = FLOOR_DB, slow = FLOOR_DB, fast = FLOOR_DB;  // (dB)
    double quick = 1, fastDown = 1, slowUp = 1, releaseDown = 1;  // the followers' coefficients
    std::atomic<double> shown{0};                                // the meter's gain change (dB)

    void allocate(double rate, int) override { held.allocate((int64_t)roundEven(HOLD_MS * 0.001 * rate)); }
    void start() override {
        held.clear();
        lastHeld = -1.0f;
        heldDb = envelope = slow = fast = FLOOR_DB;
        shown = 0;
    }
    void changed() override {  // (the share of the way each follower moves a frame)
        quick = 1 - timeCoeff(FAST_ATTACK_MS, sr);
        fastDown = 1 - timeCoeff(FAST_RELEASE_MS, sr);
        slowUp = 1 - timeCoeff(v(attackTime), sr);
        releaseDown = 1 - timeCoeff(v(release), sr);
    }
    void block(const float* const* x, const float* const* side, float* const* y, int n) override {
        const float* const* detect = v(sideOn) >= 0.5 && side ? side : x;
        double a = v(attack) / 100, s = v(sustain) / 100, out = v(output);
        bool soft = v(clip) >= 0.5;
        double widest = 0;  // (the block's largest change, for the meter)
        for (int i = 0; i < n; i++) {
            float level = held.push(std::max(std::fabs(detect[0][i]), std::fabs(detect[1][i])));
            if (level != lastHeld) {  // (the held level changes seldom: its dB worked out only then)
                lastHeld = level;
                heldDb = std::max(FLOOR_DB, 20.0 * std::log10(std::max((double)level, 1e-12)));
            }
            envelope += (heldDb - envelope) * (heldDb > envelope ? quick : releaseDown);
            // (the slow one rises behind the envelope, and falls with it: in a tail it is no attack)
            slow = envelope > slow ? slow + (envelope - slow) * slowUp : envelope;
            fast += (heldDb - fast) * (heldDb > fast ? quick : fastDown);
            double attackDb = std::min(ATTACK_RANGE_DB, std::max(0.0, envelope - slow));
            double tailDb = std::min(SUSTAIN_RANGE_DB, std::max(0.0, envelope - fast));
            double change = a * attackDb + s * tailDb;
            if (std::fabs(change) > std::fabs(widest)) widest = change;
            double gain = dbToGain(change + out);
            for (int c = 0; c < 2; c++) {
                double u = x[c][i] * gain;
                y[c][i] = soft ? softClip(u) : (float)u;
            }
        }
        shown = widest;
    }
    std::vector<Meter> meters() const override {
        double g = shown.load();
        char text[32];
        snprintf(text, sizeof text, "%+.1f dB", g);
        return {{"Gain change", std::min(1.0, std::fabs(g) / METER_DB), text}};
    }
};

}  // namespace

std::unique_ptr<Effect> makeTransientShaper() { return std::make_unique<TransientShaper>(); }

}  // namespace effects
