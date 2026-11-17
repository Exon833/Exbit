// Playing a MIDI keyboard into Tiny DAW (the GUI half of daw/ui/midi_input.py; the ALSA side and the
// sustain pedal are audio/midi_in.h): Options → MIDI input… chooses the port (remembered by its stable
// name in "options/midi_input").
//
// Notes go to the selected track (the lane or row clicked last) when it is a MIDI track that has an
// instrument and isn't frozen, heard through it as the MIDI editor's keys are (audio/live.h): its
// MIDI effects (those that work live), its VST instrument, the built-in synth or sampler, then its
// effects. A note is let go on the track it started on, even if another track was selected meanwhile.
//
// The sustain pedal (control 64) holds notes (midi_in::Sustain): while it is down, a key let go keeps
// sounding until the pedal is let go (or the key is struck again). It is applied before anything else
// sees the notes, so it works the same for every instrument, and ● Rec and Capture keep the notes as
// long as they sounded. The computer keyboard (computer_keyboard.h) plays through handle() too.
//
// Every note is also noted down in `log` (when, and where the playhead was heard then), recording or
// not, so Capture (capture.h) can turn what was just played into a clip.
//
// THE MAIN WINDOW:
//   midiInput = std::make_unique<midi_input::MidiInput>(engine, project,
//       [this] { return timeline->currentTrack; }, [this](const std::string& m, int ms) { status(m, ms); });
//   if (!midiInput->open(midi_input::savedName()))
//       status("MIDI input “" + name + "” isn't connected now (Options → MIDI input…)", 5000);
//   every frame: midiInput->poll();
//   Options → "MIDI input…": midi_input::chooseInput(this, *midiInput);
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

std::string savedName();                   // the port remembered ("" none)
void saveName(const std::string& name);

class MidiInput {
public:
    MidiInput(audio::Engine& engine, Project& project, std::function<TrackPtr()> currentTrack,
              std::function<void(const std::string&, int)> status);
    ~MidiInput();

    // listen to the port called `name` (its stable name; "" for none); false if it isn't there now
    bool open(const std::string& name);
    void close();
    const std::string& name() const { return name_; }

    // the track notes go to: the selected one, if it is a MIDI track that can play them (else null)
    TrackPtr target() const;
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
    std::deque<LogEntry> log;              // (clock, heard frame, on, pitch, velocity), for Capture
    double capturedAt = -1.0;              // clock time of the last Capture: notes before it were captured
    double now() const;                    // the log's clock (plat::now(); tests may set `clock`)
    std::function<double()> clock;

    audio::Engine& engine;
    Project& project;
    std::function<TrackPtr()> currentTrack;
    std::function<void(const std::string&, int)> status;
    midi_in::Input input;
    midi_in::Sustain sustain;

private:
    void play(const midi_in::Event& e);    // as played (the pedal already taken into account)
    void noteDown(const midi_in::Event& e);
    void letGo(int note);
    std::string name_;
    std::map<int, std::pair<TrackPtr, std::vector<int>>> held_;  // key -> (the track it went to, its pitches)
    std::vector<midi_in::Event> events_, out_;
    bool lostReported_ = false;
};

// Options → MIDI input…: the keyboard played into the selected MIDI track (remembered)
void chooseInput(ui::Window* parent, MidiInput& input);

}  // namespace midi_input
