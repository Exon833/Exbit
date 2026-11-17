// Time FX: the sound slowed, stopped, repeated or played at half speed, in time with the project.
// Trigger (a switch, so automation can play it: Off normal, On engaged) sets it going; Mode says
// what it does:
//
//   - Tape stop: the sound slows down to a standstill over the stop length (1/8 … 4 bars), as a tape
//     machine switched off, pitch and speed falling together; Curve shapes the fall (0: steadily,
//     +: the speed held up longer, then dropping at the end, −: dropping at once, then dragging).
//     It fades out over the last bit of speed, so it ends in silence. Trigger Off again spins it
//     back up (tape start) over the spin-up length, then it is back on the live sound.
//   - Stutter: the last slice (1/32 … 1 bar) before the trigger, played again and again until it is
//     turned off; each repeat can be pitched down (semitones per repeat) or faded (dB per repeat),
//     and Gate cuts each repeat short (100 %: the whole slice).
//   - Half speed: the sound an octave down at half speed. Played slower it falls behind, so every
//     1, 2 or 4 bars (on the project's bar lines) it starts again from the sound of that moment.
//
// How: everything coming in goes into a ring of the last 8 seconds, and what comes out is read from
// it by a read head at the speed wanted (between samples by cubic Hermite interpolation). Normally the
// head is on the newest frame, reading the input as it is (and then the input is passed through
// untouched, bit for bit). Whenever the head has to jump (a slice starting over, a re-sync, back to
// the live sound) the old head goes on reading where it was for 8 ms, fading out as the new one
// fades in: no clicks. A stutter's slice is copied out of the ring as it plays the first time, so
// it can repeat for as long as the trigger stays on. Lengths are in beats at the project's tempo
// (a bar from its time signature), the bar lines from the transport's position; with the
// transport stopped the sound passes through. It adds no latency.
#include <algorithm>
#include <cstdio>

#include "effects/builtin.h"

namespace effects {
namespace {

constexpr double RING_S = 8.0;           // the input kept
constexpr double LONGEST_SLICE_S = 4.0;  // a stutter's slice at most (1 bar at 60 bpm)
constexpr double XFADE_MS = 8.0;         // every jump crossfaded over this
constexpr double GATE_RAMP_MS = 3.0;     // the gate opening and closing
constexpr double SILENT_SPEED = 0.05;    // a stopping tape fades out below this speed,
constexpr double MOTOR_FADE = 0.1;       // and over the last tenth of its stop
constexpr int TAILS = 3;                 // old heads fading out at once at most

enum Mode { TAPE, STUTTER, HALF };
enum class State { Live, Tape, Stutter, Half };

const std::vector<std::string> LENGTHS = {"1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"};
const std::vector<std::string> SLICES = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
const std::vector<std::string> RESYNCS = {"1 bar", "2 bars", "4 bars"};

// a length's beats (quarter notes), a bar being `bar` beats
double lengthBeats(int i, double bar) {
    const double notes[] = {0.5, 1.0, 2.0};
    i = std::min(5, std::max(0, i));
    return i < 3 ? notes[i] : bar * (1 << (i - 3));
}
double sliceBeats(int i, double bar) {
    const double notes[] = {0.125, 0.25, 0.5, 1.0, 2.0};
    i = std::min(5, std::max(0, i));
    return i < 5 ? notes[i] : bar;
}

// where a read head is: a frame in the ring (absolute), or in the stutter's slice; how fast it moves
struct Head {
    double pos = 0, rate = 1, gain = 1;
    bool slice = false;
};
struct Tail {  // an old head fading out
    Head head;
    double weight = 0;  // at the start of its fade
    int left = 0;       // frames still to go
};

class TimeFx : public Effect {
public:
    TimeFx()
        : Effect("time_fx", "Time FX",
                 {choice("mode", "Mode", {"Tape stop", "Stutter", "Half speed"}),
                  toggle("trigger", "Trigger"),
                  choice("stop_length", "Stop length", LENGTHS, 3),
                  choice("start_length", "Spin-up length", LENGTHS, 1),
                  param("curve", "Stop curve", -100, 100, 0, "%"),
                  choice("slice", "Slice", SLICES, 1),
                  param("repeat_pitch", "Pitch per repeat", -12, 0, 0, "st", Scale::Linear, 1),
                  param("repeat_fade", "Fade per repeat", 0, 24, 0, "dB", Scale::Linear, 1),
                  param("gate", "Gate", 5, 100, 100, "%", Scale::Linear, 0),
                  choice("resync", "Re-sync every", RESYNCS)}) {}
    int mode = key("mode"), trigger = key("trigger"), stopLength = key("stop_length"),
        startLength = key("start_length"), curve = key("curve"), slice = key("slice"),
        repeatPitch = key("repeat_pitch"), repeatFade = key("repeat_fade"), gate = key("gate"),
        resync = key("resync");

    std::vector<float> ring[2], loop[2];  // the input; a stutter's slice (and a crossfade's worth after it)
    int64_t size = 1, loopSize = 1;
    int64_t now = 0;  // frames written into the ring since start(): the next one's index
    int xfade = 1, gateRamp = 1;
    State state = State::Live;
    Head head;
    Tail tails[TAILS];
    double motor = 1;  // the tape's motor: 1 running, 0 stopped (its speed is a curve of this)
    // the stutter
    int64_t loopStart = 0, sliceFrames = 1, phase = 0, copied = 0, elapsed = 0;
    int repeat = 0;
    double gateEnv = 1;
    // half speed
    int64_t group = 0;
    std::atomic<double> shownSpeed{1};

    void allocate(double rate, int maxBlock) override {
        xfade = std::max(1, (int)roundEven(XFADE_MS * 0.001 * rate));
        gateRamp = std::max(1, (int)roundEven(GATE_RAMP_MS * 0.001 * rate));
        size = (int64_t)(RING_S * rate) + maxBlock + 16;
        loopSize = (int64_t)(LONGEST_SLICE_S * rate) + xfade + 8;
        for (int c = 0; c < 2; c++) {
            ring[c].assign((size_t)size, 0.0f);
            loop[c].assign((size_t)loopSize, 0.0f);
        }
    }
    // (nothing read is older than what start() let in: the rings need no clearing)
    void start() override {
        now = 0;
        state = State::Live;
        head = Head();
        for (auto& t : tails) t = Tail();
        motor = 1;
        loopStart = phase = copied = elapsed = 0;
        sliceFrames = 1;
        repeat = 0;
        gateEnv = 1;
        group = 0;
        shownSpeed = 1;
    }
    // (engaged, it may hold sound for as long as the ring is: a stutter's slice with a quiet stretch)
    double sleepAfter() const override { return SLEEP_AFTER_S * sr + (state != State::Live ? (double)size : 0.0); }

    // --- reading ---

    float ringAt(int c, int64_t i, int64_t newest) const {
        i = std::min(newest, std::max(std::max<int64_t>(0, newest - size + maxBlock() + 1), i));
        return ring[c][(size_t)(i % size)];
    }
    float loopAt(int c, int64_t i) const {
        i = std::min(sliceFrames + xfade - 1, std::max<int64_t>(0, i));
        return loop[c][(size_t)i];
    }
    static double hermite(float y0, float y1, float y2, float y3, double f) {
        double c1 = 0.5 * ((double)y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3,
               c3 = 0.5 * ((double)y3 - y0) + 1.5 * ((double)y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    // what a head reads, frame `cur` being the newest in the ring
    double read(int c, const Head& h, int64_t cur) const {
        double fl = std::floor(h.pos);
        int64_t i = (int64_t)fl;
        double f = h.pos - fl;
        if (h.slice) {
            if (f == 0) return loopAt(c, i);
            return hermite(loopAt(c, i - 1), loopAt(c, i), loopAt(c, i + 1), loopAt(c, i + 2), f);
        }
        if (f == 0) return ringAt(c, i, cur);
        return hermite(ringAt(c, i - 1, cur), ringAt(c, i, cur), ringAt(c, i + 1, cur), ringAt(c, i + 2, cur), f);
    }

    // --- jumps ---

    double tailWeight(const Tail& t) const {
        if (t.left <= 0) return 0;
        return t.weight * 0.5 * (1 + std::cos(PI * (double)(xfade - t.left) / xfade));
    }
    double tailsWeight() const {
        double w = 0;
        for (const auto& t : tails) w += tailWeight(t);
        return std::min(1.0, w);
    }
    bool fading() const {
        for (const auto& t : tails)
            if (t.left > 0) return true;
        return false;
    }
    // the head moved to `to`: the old one fades out as this one fades in
    void jump(const Head& to) {
        double w = 1 - tailsWeight();  // (the old head's weight now: it carries on from there)
        Tail* slot = &tails[0];
        for (auto& t : tails) {
            if (t.left <= 0) {
                slot = &t;
                break;
            }
            if (tailWeight(t) < tailWeight(*slot)) slot = &t;  // (all busy: the faintest one goes,
        }
        double dropped = tailWeight(*slot);                    //  its weight handed on: the weights add up)
        slot->head = head;
        slot->weight = w + dropped;
        slot->left = xfade;
        head = to;
    }
    void goLive(int64_t cur) {
        if (state != State::Live) jump(Head{(double)cur, 1, 1, false});
        state = State::Live;
        motor = 1;
    }

    // --- the modes ---

    double beatFrames() const { return 60.0 / std::max(1.0, tempo()) * sr; }
    double speedOf(double m) const { return std::pow(std::min(1.0, std::max(0.0, m)), std::pow(2.0, -2.0 * v(curve) / 100)); }
    static double smooth(double t) {
        t = std::min(1.0, std::max(0.0, t));
        return t * t * (3 - 2 * t);
    }
    // a stopping (or starting) tape's level: silent at a standstill, whatever the curve
    static double tapeGain(double speed, double motor) { return smooth(speed / SILENT_SPEED) * smooth(motor / MOTOR_FADE); }
    double repeatRate(int k) const { return std::pow(2.0, std::max(-48.0, v(repeatPitch) * k) / 12); }
    double repeatGain(int k) const {
        double db = v(repeatFade) * k;
        return db >= 140 ? 0.0 : dbToGain(-db);
    }
    int64_t groupAt(int j) const {
        double bars = (double)(1 << std::min(2, std::max(0, (int)v(resync))));
        double beats = (double)(position() + j) / beatFrames();
        return (int64_t)std::floor(beats / (bars * beatsPerBar()) + 1e-9);
    }
    int64_t maxLag() const { return size - maxBlock() - 16; }

    void engage(Mode m, int64_t cur, int j) {
        if (m == TAPE) {
            if (state != State::Live) jump(Head{(double)cur, 1, 1, false});
            state = State::Tape;
            motor = 1;
        } else if (m == STUTTER) {
            sliceFrames = std::min<int64_t>(loopSize - xfade - 8,
                                            std::max<int64_t>(2 * xfade, (int64_t)roundEven(sliceBeats((int)v(slice), beatsPerBar()) * beatFrames())));
            // the slice before now, or (not that much heard yet since it started) the one from now on
            loopStart = cur >= sliceFrames ? cur - sliceFrames : cur;
            phase = copied = elapsed = 0;
            repeat = 0;
            gateEnv = 1;
            jump(Head{(double)loopStart, 1, 1, false});
            state = State::Stutter;
        } else {
            jump(Head{(double)cur, 0.5, 1, false});
            group = groupAt(j);
            state = State::Half;
        }
    }

    void block(const float* const* x, const float* const*, float* const* y, int n) override {
        int64_t base = now;
        for (int c = 0; c < 2; c++)
            for (int j = 0; j < n; j++) ring[c][(size_t)((base + j) % size)] = x[c][j];
        now += n;

        bool playing_ = playing();
        bool on = playing_ && v(trigger) >= 0.5;
        Mode m = (Mode)std::min(2, std::max(0, (int)v(mode)));
        State wanted = m == TAPE ? State::Tape : m == STUTTER ? State::Stutter : State::Half;
        if (!playing_)
            goLive(base);
        else if (on && state != wanted)
            engage(m, base, 0);
        else if (!on && (state == State::Stutter || state == State::Half))
            goLive(base);
        double stopStep = 1 / std::max(1.0, lengthBeats((int)v(stopLength), beatsPerBar()) * beatFrames());
        double startStep = 1 / std::max(1.0, lengthBeats((int)v(startLength), beatsPerBar()) * beatFrames());
        int64_t gateFrames = (int64_t)roundEven(v(gate) / 100 * (double)sliceFrames);

        for (int j = 0; j < n; j++) {
            int64_t cur = base + j;
            switch (state) {
            case State::Live:
                head = Head{(double)cur, 1, 1, false};
                break;
            case State::Tape: {
                motor = std::min(1.0, std::max(0.0, motor + (on ? -stopStep : startStep)));
                if (!on && motor >= 1) {
                    goLive(cur);
                    head = Head{(double)cur, 1, 1, false};
                    break;
                }
                if (motor <= 0) {  // (standing still: silent, the head kept on the newest frame for the spin-up)
                    head = Head{(double)cur, 0, 0, false};
                    break;
                }
                double s = speedOf(motor);
                head.rate = s;
                head.gain = tapeGain(s, motor);
                if ((double)cur - head.pos > (double)maxLag()) jump(Head{(double)cur, s, head.gain, false});
                break;
            }
            case State::Stutter: {
                if (phase >= sliceFrames) {  // (the slice again, from its start)
                    phase = 0;
                    repeat = std::min(repeat + 1, 1 << 20);
                    jump(Head{0, repeatRate(repeat), repeatGain(repeat) * gateEnv, true});
                }
                // the slice copied out of the ring a frame at a time as it plays the first time (read from
                // the ring then), and a crossfade's worth after it, for the repeats
                for (; copied < sliceFrames + xfade && copied <= elapsed + 3 && loopStart + copied <= cur; copied++)
                    for (int c = 0; c < 2; c++) loop[c][(size_t)copied] = ringAt(c, loopStart + copied, cur);
                elapsed++;
                double target = phase < gateFrames ? 1.0 : 0.0;
                gateEnv += std::max(-1.0 / gateRamp, std::min(1.0 / gateRamp, target - gateEnv));
                double rate = repeatRate(repeat);
                head.rate = rate;
                head.pos = repeat == 0 ? (double)(loopStart + phase) : phase * rate;
                head.slice = repeat > 0;
                head.gain = repeatGain(repeat) * gateEnv;
                phase++;
                break;
            }
            case State::Half: {
                int64_t g = groupAt(j);
                if (g != group || (double)cur - head.pos > (double)maxLag()) {
                    group = g;
                    jump(Head{(double)cur, 0.5, 1, false});
                }
                break;
            }
            }

            if (state == State::Live && !fading()) {
                y[0][j] = x[0][j];
                y[1][j] = x[1][j];
            } else {
                double hw = 1 - tailsWeight();
                for (int c = 0; c < 2; c++) {
                    double o = head.gain == 0 ? 0.0 : hw * head.gain * read(c, head, cur);
                    for (const auto& t : tails)
                        if (t.left > 0 && t.head.gain != 0) o += tailWeight(t) * t.head.gain * read(c, t.head, cur);
                    y[c][j] = (float)o;
                }
            }

            // on to the next frame
            head.pos += head.rate;
            for (auto& t : tails)
                if (t.left > 0) {
                    t.head.pos += t.head.rate;
                    t.left--;
                }
        }
        shownSpeed = state == State::Live ? 1.0 : head.rate;
    }

    std::vector<Meter> meters() const override {
        double s = shownSpeed.load();
        char text[32];
        snprintf(text, sizeof text, "%.0f %%", s * 100);
        return {{"Speed", std::min(1.0, std::max(0.0, s)), text}};
    }
};

}  // namespace

std::unique_ptr<Effect> makeTimeFx() { return std::make_unique<TimeFx>(); }

}  // namespace effects
