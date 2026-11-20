// Playing a MIDI keyboard into Tiny DAW (the low half of daw/ui/midi_input.py): what a MIDI port sends
// (Event), the ALSA sequencer's ports, and the sustain pedal. The ports themselves are listened to and
// written to by audio/midi_ports.h (Options → MIDI ports…: several at once).
//
// How the successor routes what is played (and what the GUI port, ui/midi_input, does with this):
//   - Every frame the GUI thread takes what the ports sent (midi_ports::Hub::poll) and hands each event
//     to its handle(): events go through a Sustain per port (the computer keyboard's notes
//     too, so the pedal holds them as well), and what Sustain lets out is "played": given to the
//     MIDI recorder while recording (recording.py's midi_event: heard frame of each note on/off),
//     kept in the Capture log (clock time = Event::time, plat::now()'s clock, and the heard frame),
//     and heard: a note-on goes to the selected MIDI track's engine.liveNotes(track) (through its
//     MIDI effects; a key let go stops on the track it started on), CC 120/123 let every note go.
//   - A port closed or unplugged: Sustain::releaseAll, so recording and Capture see every note end.
// The engine itself takes live notes only through LiveNotes (live.h), from the GUI thread.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace midi_in {

constexpr int SUSTAIN = 64;          // the sustain (damper) pedal's control number
constexpr int ALL_SOUND_OFF = 120;
constexpr int ALL_NOTES_OFF = 123;

struct Event {
    // (Clock … SongPos: the system real-time messages a MIDI clock sends: midi_sync.h)
    enum Type : uint8_t { NoteOn, NoteOff, Control, PitchBend, Clock, Start, Continue, Stop, SongPos };
    Type type = NoteOn;
    uint8_t channel = 0;   // 0-15
    uint8_t data1 = 0;     // note / control number
    uint8_t data2 = 0;     // velocity (NoteOn: 1-127; a note-on of velocity 0 arrives as NoteOff) / control value
    int16_t bend = 0;      // PitchBend: -8192..8191; SongPos: the song position in 16ths (0..16383)
    int16_t port = -1;     // the port it came from (midi_ports::Hub's slot); -1: the computer keyboard, a test
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
// clients ([] without an ALSA sequencer). Tiny DAW's own clients and "System" (timer, announce) are left out.
std::vector<PortInfo> ports();
std::vector<PortInfo> outputs();  // the same for the ports MIDI can be sent to (synths, interfaces' outs)
// a port's name without the numbers ALSA puts at the end ("… 20:0"), which can change between sessions
std::string stableName(const std::string& port);

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
