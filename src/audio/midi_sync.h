// Following another transport (the transport bar's "Sync ▾": Internal, MIDI clock or JACK).
//
// MIDI CLOCK (ClockFollower): the clock-in port's real-time messages (an input with Clock in: midi_ports.h). The
// tempo is the slope of a least-squares line through the times of the last 2 beats of pulses (24 a beat), so a
// pulse arriving a little early or late hardly moves it; Start plays from the song's start, Continue from the Song
// Position Pointer received while stopped (in 16ths; none: on from where it is), Stop stops, and a Song Position
// while running moves the playhead.
//
// JACK (JackFollower over a JackTransport: the real one is JACK MIDI's client, midi_jack.cpp; tests use a fake).
// Both ways, looked at every frame: what Tiny DAW did since (played, stopped, moved its playhead) is done to JACK's
// transport (start, stop, locate), and what JACK's did (another app started it, or located) is done here; while
// both roll, Tiny DAW's playhead follows JACK's when they part by more than JACK_TOLERANCE_S. When another app is
// JACK's timebase master its tempo (BBT) is followed; otherwise Tiny DAW is (conditional) timebase master and tells
// the others its tempo map as bars, beats and ticks (fillBBT).
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "audio/jack_api.h"
#include "audio/midi_in.h"
#include "model/timing.h"

namespace midi_sync {

enum class Mode { Internal, MidiClock, Jack };
const char* modeName(Mode m);              // "Internal", "MIDI clock", "JACK" (shown; saved)
Mode modeFrom(const std::string& name);    // (unknown: Internal)
constexpr double JACK_TOLERANCE_S = 0.1;   // the playheads may part by this much before Tiny DAW's is moved
constexpr double TICKS_PER_BEAT = 1920.0;  // BBT's ticks

// what the transport is to do
struct Action {
    enum Kind { Play, Stop, Locate } kind = Play;
    double beat = 0;  // Locate: where (beats since the song's start)
};

class ClockFollower {
public:
    // a message from the clock-in port (Clock, Start, Continue, Stop, SongPos; others ignored): what to do, appended
    void event(const midi_in::Event& e, std::vector<Action>& out);
    double bpm() const;  // the tempo the last pulses give (0: not enough of them yet)
    bool running() const { return running_; }
    void reset();

private:
    std::deque<double> pulses_;  // the times of the last pulses
    bool running_ = false;
    double songPos_ = 0;         // beats, from the last Song Position Pointer
    bool located_ = false;       // …received while stopped (Continue goes there)
};

struct JackState {
    bool rolling = false;
    int64_t frame = 0;
    int rate = 48000;
    bool bbt = false;   // another app tells its tempo
    double bpm = 0;
};

// what Tiny DAW needs of JACK's transport
class JackTransport {
public:
    virtual ~JackTransport() = default;
    virtual bool query(JackState& out) = 0;   // false: no JACK
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void locate(int64_t frame) = 0;
    // be JACK's timebase master (if no other app is), telling `timing`'s tempo map (null: let go of it)
    virtual bool setMaster(const Timing* timing) = 0;
    virtual bool master() const = 0;
};

class JackFollower {
public:
    struct Host {
        std::function<bool()> playing;
        std::function<int64_t()> pos;   // the playhead (frames)
        int sr = 48000;
        std::function<void()> play, stop;
        std::function<void(int64_t)> seek;
        std::function<void(double)> tempo;  // JACK's timebase master's tempo
    };
    // GUI thread, every frame (now: monotonic seconds)
    void tick(JackTransport& t, const Host& h, double now);
    void reset() { known_ = false; }

private:
    bool known_ = false, rolling_ = false, playing_ = false;
    int64_t jack_ = 0, pos_ = 0;
    double at_ = 0;
};

// JACK's bar, beat and tick at `frame` (at `rate`) on `map`, as a timebase master tells them
void fillBBT(const TempoMap& map, uint32_t frame, uint32_t rate, jack::Position& pos);

}  // namespace midi_sync
