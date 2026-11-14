// Playing a MIDI keyboard into Tiny DAW (the low half of daw/ui/midi_input.py): the ALSA sequencer's
// input ports, one of them listened to on a thread of our own, and the sustain pedal.
//
// How the successor routes what is played (and what the GUI port, ui/midi_input, does with this):
//   - Options → MIDI input… lists ports() by stableName() ("None" first); the choice is remembered
//     in the settings by stable name and opened with Input::open(name) at start (false: "MIDI input
//     “…” isn't connected now").
//   - Every frame (or when notified, see setNotify) the GUI thread calls Input::poll(events) and
//     hands each event to its handle(): events go through ONE Sustain (the computer keyboard's notes
//     too, so the pedal holds them as well), and what Sustain lets out is "played": given to the
//     MIDI recorder while recording (recording.py's midi_event: heard frame of each note on/off),
//     kept in the Capture log (clock time = Event::time, plat::now()'s clock, and the heard frame),
//     and heard: a note-on goes to the selected MIDI track's engine.liveNotes(track) (through its
//     MIDI effects; a key let go stops on the track it started on), CC 120/123 let every note go.
//   - Close (or open another port): Sustain::releaseAll, so recording and Capture see every note end.
// The engine itself takes live notes only through LiveNotes (live.h), from the GUI thread; an
// Input::setCallback receiver runs on the MIDI thread instead, for a consumer that wants events
// at once (it must be quick and thread-safe; the queue is filled as well unless setQueue(false)).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace midi_in {

constexpr int SUSTAIN = 64;          // the sustain (damper) pedal's control number
constexpr int ALL_SOUND_OFF = 120;
constexpr int ALL_NOTES_OFF = 123;

struct Event {
    enum Type : uint8_t { NoteOn, NoteOff, Control, PitchBend };
    Type type = NoteOn;
    uint8_t channel = 0;   // 0-15
    uint8_t data1 = 0;     // note / control number
    uint8_t data2 = 0;     // velocity (NoteOn: 1-127; a note-on of velocity 0 arrives as NoteOff) / control value
    int16_t bend = 0;      // PitchBend: -8192..8191
    double time = 0;       // when it arrived: monotonic seconds, plat::now()'s clock (CLOCK_MONOTONIC)

    static Event noteOn(int note, int velocity, int channel = 0, double time = 0);
    static Event noteOff(int note, int channel = 0, double time = 0);
    static Event control(int number, int value, int channel = 0, double time = 0);
    bool isNote() const { return type == NoteOn || type == NoteOff; }
};

struct PortInfo {
    std::string name;    // as ALSA (and the Python program's mido) shows it: "Client:Port 20:0"
    std::string stable;  // stableName(name): "Client:Port", what is remembered
    int client = 0, port = 0;
};

// The MIDI input ports there are now: readable, subscribable MIDI ports of the other sequencer
// clients ([] without an ALSA sequencer). Our own client and "System" (timer, announce) are left out.
std::vector<PortInfo> ports();
// a port's name without the numbers ALSA puts at the end ("… 20:0"), which can change between sessions
std::string stableName(const std::string& port);

// One input port listened to (an ALSA sequencer client "Tiny DAW" with a port "MIDI in" connected
// from it). Notes, controls and pitch bend arrive on a thread of its own.
class Input {
public:
    Input();
    ~Input();  // closes
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Listen to the port called `name` (its stable name, or the full one); "" listens to none (true).
    // False if it isn't there now (or the sequencer can't be opened): `name()` is still set to it.
    bool open(const std::string& name);
    void close();
    const std::string& name() const { return name_; }  // the port asked for ("" none)
    bool isOpen() const { return connected_; }         // a port is connected (midi_input.py: port is not None)
    // the port went away (keyboard unplugged): no more events until open() again
    bool lost() const { return lost_.load(); }

    // GUI thread: take what arrived since the last call (appended, oldest first). Returns how many.
    size_t poll(std::vector<Event>& out);
    // (MIDI thread) called with each event as it arrives (before it is queued); set before open()
    void setCallback(std::function<void(const Event&)> cb) { callback_ = std::move(cb); }
    // (MIDI thread) called after events were queued, e.g. [] { plat::postToMain(drain) }; set before open()
    void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }
    void setQueue(bool on) { queue_ = on; }  // keep events for poll() (default on)
    int dropped() const { return dropped_.load(); }  // events lost because poll() wasn't called (queue full)

private:
    struct Seq;
    std::unique_ptr<Seq> seq_;
    std::string name_;
    bool connected_ = false;
    std::atomic<bool> lost_{false}, running_{false};
    std::thread thread_;
    std::function<void(const Event&)> callback_;
    std::function<void()> notify_;
    bool queue_ = true;
    // single-producer (MIDI thread) / single-consumer (poll) ring
    static constexpr size_t RING = 4096;
    std::vector<Event> ring_;
    std::atomic<uint64_t> head_{0}, tail_{0};
    std::atomic<int> dropped_{0};

    void loop();
    void deliver(const Event& e);
};

// The sustain pedal (control 64), applied before anything else sees the notes, so it works the same
// for every instrument and recording / Capture keep the notes as long as they sounded: while it is
// down, a key let go keeps sounding until the pedal is let go, or the key is struck again (its note
// is let go first, then struck anew). All notes off / all sound off (123 / 120) let every note go,
// the pedal too. handle() appends what is "played" to `out`: notes on/off as they should sound, the
// other events unchanged (the pedal's own control changes are swallowed; 120/123 come after the
// note-offs they cause). Channels are not told apart (midi_input.py doesn't either).
class Sustain {
public:
    void handle(const Event& e, std::vector<Event>& out);
    // every note sounding stops now (closing the input, another input): a note-off each, pedal up
    void releaseAll(std::vector<Event>& out, double time = 0);
    bool pedal() const { return pedal_; }
    bool sounding(int note) const { return sounding_.count(note) > 0; }  // struck, not let go (pedal or not)

private:
    void pedalUp(std::vector<Event>& out, double time);
    bool pedal_ = false;
    std::set<int> sounding_;   // keys whose note-on was played and whose note-off wasn't yet
    std::set<int> sustained_;  // of those, keys let go while the pedal is down
    uint8_t channel_[128] = {};  // the channel each sounding key was struck on (for the note-offs we make)
};

}  // namespace midi_in
