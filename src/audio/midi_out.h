// What the audio thread sends out as MIDI, timed by its own clock (the half of audio/midi_ports.h that runs in
// Engine::callback): MIDI clock, Start / Continue / Stop and Song Position Pointer to the outputs with Clock out,
// and the notes of External synth tracks.
//
// TIMING. Every message is given the monotonic moment it belongs to, from the buffer being made: the callback's
// start (Buffer::now) plus its frame offset in the buffer. Clock and transport messages are to be HEARD with the
// audio, so they come the device's output delay and the plugins' delay later (outLatency, latency), plus each
// port's own offset. An External synth's notes are sent `delay` EARLY (Track::ext.delayMs, the round trip found by
// Measure): a note at timeline frame T goes out as the buffer for T − delay is made, so its sound comes back on the
// device's inputs in the very buffer that plays T, and is heard through the track's effects in time (the mixer:
// mixer.h EXTERNAL SYNTHS). Messages go into a lock-free ring (no allocation, no locks, no system calls but one
// eventfd write per buffer to wake the sender); the Hub's output thread sends each at its moment.
//
// CLOCK. 24 pulses a quarter note on the tempo map's beats (TempoMap::secondsAt: through every tempo change and
// ramp, exactly, not stepped per buffer). Playback starting at the song's start sends Start; elsewhere a Song
// Position Pointer (the next 16th) and Continue, the pulses starting at that 16th (Always send Start: Start
// instead of Continue, for synths that ignore Continue). Stop when playback stops; a jump (a locate, the loop
// going round) sends Stop, the new Song Position and Continue.
//
// EXTERNAL SYNTHS. A track's clips' notes ahead by its delay (through the loop's jump: notes after the loop's end
// come from its start), on its port and channel; when playback stops or jumps, its notes still on get their
// note-offs and an All Notes Off. Starting playback, the notes in its first `delay` are sent at once (late:
// unavoidable). Measure (the input as monitoring hears it, Buffer::in): listens 0.15 s to the input for its noise, sends middle C at full velocity and finds the
// first sample well above the noise: the round trip, in frames.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/plugin_audio.h"
#include "core/spsc.h"
#include "model/model.h"

namespace midi_out {

constexpr int PPQN = 24;
constexpr int MAX_EXTERNAL = 64;         // External synth tracks played at once at most
constexpr double MEASURE_NOISE_S = 0.15;  // Measure: the input listened to for its noise first
constexpr double MEASURE_TIMEOUT_S = 2.0; // …and for the note's sound after
constexpr int MEASURE_NOTE = 60;

// raw MIDI bytes
enum : uint8_t {
    NOTE_OFF = 0x80, NOTE_ON = 0x90, CONTROL = 0xB0, SONG_POSITION = 0xF2, CLOCK = 0xF8, START = 0xFA,
    CONTINUE = 0xFB, STOP = 0xFC
};

struct Event {
    double when = 0;    // monotonic seconds (plat::now()'s clock)
    int16_t slot = -1;  // the output (midi_ports::Hub's slot)
    uint8_t n = 0;      // bytes used
    uint8_t b[3] = {0, 0, 0};
};

struct ClockPort {
    int slot = -1;
    bool alwaysStart = false;
    double offsetS = 0;
};
struct ExternalTrack {
    const Track* track = nullptr;  // (only played while it is in the project: Buffer::tracks)
    int slot = -1;
    int channel = 0;               // 0-15
};
// what the GUI thread publishes (setClocks / setTracks); replaced whole
struct Config {
    std::vector<ClockPort> clocks;
    std::vector<ExternalTrack> tracks;
};

// one buffer, as Engine::callback makes it
struct Buffer {
    double now = 0;           // when the callback began (monotonic s)
    double outLatency = 0;    // the device's output delay (s)
    int latency = 0;          // frames the plugins hold the sound back
    int frames = 0, sr = 48000;
    const float* in = nullptr;  // the device's inputs (interleaved; null: none)
    int inChannels = 0;
    const TempoMap* map = nullptr;
    const std::vector<TrackPtr>* tracks = nullptr;  // the project's tracks (snapshot)
    const std::vector<ClipPtr>* clips = nullptr;    // the project's clips (snapshot)
    bool loops = false;       // playback jumps from loopEnd back to loopStart
    int64_t loopStart = 0, loopEnd = 0;
};
struct Part {
    int offset = 0, frames = 0;
    bool moving = false;      // the transport moves (false: stopped, or counting in)
    int64_t start = 0;        // the timeline frame at its first frame (moving)
};

class Out {
public:
    Out();
    ~Out();
    // --- GUI thread ---
    void setClocks(std::vector<ClockPort> clocks);
    void setTracks(std::vector<ExternalTrack> tracks);   // (cheap when unchanged)
    // Measure an External synth: middle C to `slot`/`channel`, listened for on input channel `input` (0-based)
    void measure(int slot, int channel, int input);
    enum class Measuring { Idle, Listening, Waiting, Done, Failed };
    Measuring measuring() const { return state_.load(); }
    int64_t measuredFrames() const { return measured_.load(); }  // (Done) the round trip
    void cancelMeasure() { state_ = Measuring::Idle; }

    // --- audio thread ---
    void process(const Buffer& b, const Part* parts, int count);

    // --- the output thread ---
    bool take(Event& e) { return ring_.pop(e); }
    int wakeFd() const { return wake_; }  // readable when something was pushed (an eventfd)
    int dropped() const { return ring_.dropped(); }

private:
    struct TrackState {
        const Track* track = nullptr;
        int64_t next = 0;         // the (continuous) timeline frame its notes were sent up to
        int slot = -1, channel = 0;
        bool held[128] = {};
        bool used = false;
    };
    Spsc<Event> ring_;
    int wake_ = -1;
    bool pushed_ = false;
    std::shared_ptr<const Config> config_;  // (atomic_load / atomic_store)
    std::vector<std::shared_ptr<const Config>> kept_;  // (GUI: old ones never freed by the audio thread)
    std::vector<ClockPort> clocks_;          // (GUI: as last set)
    std::vector<ExternalTrack> tracksSet_;
    // audio thread
    bool wasMoving_ = false;
    int64_t nextPos_ = 0;
    int64_t pulse_ = 0;                      // the next clock pulse (in 1/24 beats)
    TrackState states_[MAX_EXTERNAL];
    std::vector<const Clip*> clips_;
    plugin_audio::Messages messages_;
    std::vector<plugin_audio::NoteEvent> events_;
    // measuring
    std::atomic<Measuring> state_{Measuring::Idle};
    std::atomic<int64_t> measured_{0};
    std::atomic<int> mSlot_{-1}, mChannel_{0}, mInput_{0};  // (set by the GUI thread)
    std::atomic<int> request_{0};
    int seen_ = 0;
    int64_t mFrames_ = 0, mSent_ = 0;        // stream frames counted since Listening began; the note's frame
    float mNoise_ = 0;
    int64_t stream_ = 0;                     // frames of every buffer so far

    void publish();
    void push(double when, int slot, uint8_t a, int n = 1, uint8_t b = 0, uint8_t c = 0);
    void part(const Buffer& b, const Part& p, const Config& c);
    void external(const Buffer& b, const Part& p, TrackState& st, bool restart, bool loopJump);
    void notesOff(TrackState& st, double when);
    void measureBuffer(const Buffer& b);
};

}  // namespace midi_out
