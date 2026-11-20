// Playing MIDI keyboards into Tiny DAW (the GUI half of daw/ui/midi_input.py; the ports are audio/midi_ports.h,
// the sustain pedal audio/midi_in.h): Options → MIDI ports… chooses the inputs listened to, several at once
// (`hub`: remembered by their stable names in the preferences, Preferences::midiPorts).
//
// ROUTING. A MIDI track's MIDI input (Track::midiIn; its menu's "MIDI input ▸") is "All" (the default) or one port,
// optionally one channel. What is played goes to the selected track (the lane or row clicked last) when its input
// is All, and to every MIDI track whose input is that very port (and channel), selected or not; each when it has an
// instrument and isn't frozen, heard through it as the MIDI editor's keys are (audio/live.h): its MIDI effects
// (those that work live), its VST instrument, the built-in synth or sampler, then its effects. An External synth
// track gets it as MIDI on its port and channel at once (bends and controls too). The computer keyboard plays the
// selected track. A note is let go on the tracks it started on, even if another track was selected meanwhile; a
// port unplugged lets its notes go.
//
// The bend wheel bends what the selected track plays live (a VST instrument by its own range; the
// built-in synth and sampler by the track's bend range, Track::bendRange: live.h), and the mod wheel
// (control 1) goes on to the track's instrument plugins. Every control change and bend is also noted in
// midi_cc's table (audio/midi_cc.h), which the MIDI Control modulator follows. When another track is
// selected, the one bent before is set straight again.
//
// The sustain pedal (control 64) holds notes (midi_in::Sustain): while it is down, a key let go keeps
// sounding until the pedal is let go (or the key is struck again). It is applied before anything else
// sees the notes, so it works the same for every instrument, and ● Rec and Capture keep the notes as
// long as they sounded (one pedal per port). The computer keyboard (computer_keyboard.h) plays through handle() too.
//
// Every note is also noted down in `log` (when, and where the playhead was heard then), recording or
// not, so Capture (capture.h) can turn what was just played into a clip.
//
// THE MAIN WINDOW:
//   midiInput = std::make_unique<midi_input::MidiInput>(engine, project,
//       [this] { return timeline->currentTrack; }, [this](const std::string& m, int ms) { status(m, ms); });
//   midiInput->hub.load(…the preferences…); midiInput->hub.start();
//   every frame: midiInput->poll();
//   Options → "MIDI ports…": midi_ports_dialog (ui/midi_ports_dialog.h);
//   the clock-in port's Start / Stop / clock (Sync: MIDI clock): midiInput->realtime = [](const midi_in::Event&) {…};
//   recording MIDI (ui/recording): midiInput->recorder = [](const midi_in::Event&) {…}; null when done
//   closing: midiInput->close().
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio/engine.h"
#include "audio/midi_in.h"
#include "audio/midi_ports.h"
#include "model/model.h"

namespace ui { class Window; }

namespace midi_input {

constexpr size_t LOG_NOTES = 20000;  // note messages kept for Capture at most (the oldest go first)

// A note played live into `track` (the MIDI editor's keys, a MIDI keyboard): through the track's MIDI
// effects that work live (midi_effects.live), then its instrument (VST, built-in synth or sampler).
// lengthS >= 0: it stops by itself after that long. Returns the pitches it sounds (let go with
// engine.liveNotes(track)->noteOff(pitch) each).
std::vector<int> playLive(audio::Engine& engine, const TrackPtr& track, int pitch, int velocity, double lengthS = -1);
// [(pitch, velocity)] a note played live into the track becomes (its MIDI effects that work live)
std::vector<std::pair<int, int>> throughEffects(const Track& track, int pitch, int velocity);

// what was played, kept for Capture
struct LogEntry {
    double clock = 0;                 // plat::now() seconds
    std::optional<int64_t> heard;     // where the playhead was heard then (playing), in frames
    bool on = false;
    int pitch = 0, velocity = 0;
};

// the port the program before MIDI ports remembered ("options/midi_input"; "" none): on from then on
std::string savedName();

class MidiInput {
public:
    MidiInput(audio::Engine& engine, Project& project, std::function<TrackPtr()> currentTrack,
              std::function<void(const std::string&, int)> status);
    ~MidiInput();

    // listen to the input called `name` too (its stable name; "" for none); false if it isn't there now
    bool open(const std::string& name);
    void close();  // every note let go, every port closed
    const std::string& name() const { return name_; }  // the port open() was asked for last
    std::string portName(int slot) const;  // an event's port's name ("" the computer keyboard)

    // the track notes go to: the selected one, if it is a MIDI track that can play them (else null)
    TrackPtr target() const;
    // the tracks an event from port `slot` (channel 0-15) plays: the selected one if its input is All, and every
    // MIDI track listening to that port (and channel); for the computer keyboard (slot < 0) the selected one
    std::vector<TrackPtr> targets(int slot, int channel) const;
    // an event from the MIDI keyboard or the computer keyboard: the sustain pedal first, then played
    void handle(const midi_in::Event& e);
    // GUI thread, every frame: what arrived from the port, handled; a port unplugged is closed
    void poll();
    // every note stops (all notes off, another input): each gets its note-off, so the log and a
    // recording see it end too
    void releaseAll();

    // recording MIDI: called with each note on / off as played (the pedal taken into account)
    std::function<void(const midi_in::Event&)> recorder;
    // control changes are offered here first (GUI thread; MIDI learn: midi_learn.h): true = used, not played
    std::function<bool(const midi_in::Event&)> controlHook;
    // MIDI clock and Start / Continue / Stop / Song Position from an input with Clock in (midi_sync.h)
    std::function<void(const midi_in::Event&)> realtime;
    std::deque<LogEntry> log;              // (clock, heard frame, on, pitch, velocity), for Capture
    double capturedAt = -1.0;              // clock time of the last Capture: notes before it were captured
    double now() const;                    // the log's clock (plat::now(); tests may set `clock`)
    std::function<double()> clock;

    audio::Engine& engine;
    Project& project;
    std::function<TrackPtr()> currentTrack;
    std::function<void(const std::string&, int)> status;
    midi_ports::Hub hub;
    midi_in::Sustain sustain;  // (the computer keyboard's; each port has its own)

private:
    void play(const midi_in::Event& e);    // as played (the pedal already taken into account)
    void noteDown(const midi_in::Event& e);
    void letGo(int key);                         // (key: heldKey)
    static int heldKey(const midi_in::Event& e) { return (e.port + 1) * 128 + e.data1; }
    void releasePort(int slot);                  // its notes let go (closed, unplugged)
    midi_in::Sustain& sustainOf(int slot);
    void sendExternal(const TrackPtr& track, uint8_t status, int d1, int d2);  // to its External synth now
    // --- the bend wheel and the mod wheel (live) ---
    void controllers(const midi_in::Event& e);  // (as played) a bend or the mod wheel to the selected track
    void unbend();                               // the track bent last set straight again
    TrackPtr bent_;                              // the track the wheel bent last (null: none bent)
    std::string name_;
    // heldKey -> (each track it went to, the pitches it sounds there)
    std::map<int, std::vector<std::pair<TrackPtr, std::vector<int>>>> held_;
    std::map<int, midi_in::Sustain> sustains_;   // each port's pedal
    std::vector<midi_in::Event> events_, out_;
    std::vector<int> lost_;
};

}  // namespace midi_input
