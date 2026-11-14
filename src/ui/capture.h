// Capture (the ⤓ button next to Rec, or Ctrl+Alt+R): what was just played, kept as a clip after all
// (capture.py).
//
// Every note played on the MIDI keyboard or the computer keyboard is noted down (midi_input.h: `log`),
// recording or not. Capture turns the last phrase of it (the notes since the last pause of more than
// GAP_S with nothing held) into a MIDI clip on the selected MIDI track:
//
// - played along with playback: where it was heard (as ● Rec would have placed it), the clip starting
//   at the bar the first note is in;
// - played while stopped (or across a jump of the playhead): at the selection cursor, starting on the
//   first note.
//
// Either way the clip lasts to the end of the bar its last note ends in (counted from its start), it
// replaces what it covers on the track, and it is one undo step. A phrase is captured once: Capture
// again right away finds nothing new.
//
// `phrase` and `placement` do the work on their own (tested); `capture` is the action.
//
// THE MAIN WINDOW: the transport's Capture action (Ctrl+Alt+R, application-wide) calls
//   capture::capture({midiInput.get(), [this] { return timeline->cursor; },
//                     [this](ClipPtr c, int64_t f, TrackPtr t) { timeline->placeClip(c, f, t); },
//                     [this] { return ++recording->takes; }, statusFn});
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/midi_input.h"

namespace capture {

constexpr double GAP_S = 4.0;        // a pause this long with no note held ends a phrase
constexpr double KEPT_S = 30 * 60;   // notes older than this are forgotten
constexpr double STEADY_S = 0.25;    // how far the playhead may drift from the clock and still count as one stretch
constexpr double MIN_NOTE_S = 0.01;

struct PlayedNote {
    double on = 0, off = 0;            // clock seconds
    int pitch = 0, velocity = 0;
    std::optional<int64_t> heard;      // the heard frame at the note-on (playing), or none
};

// the notes in `log` (by time), a note still held ending `now`, a note played again before it was let
// go ending there; sorted
std::vector<PlayedNote> notesOf(const std::vector<midi_input::LogEntry>& log, double now);
// the last phrase of notesOf(log, now): those after the last pause of more than gapS (empty if none)
std::vector<PlayedNote> phrase(const std::vector<midi_input::LogEntry>& log, double now, double gapS = GAP_S);

struct Placement {
    double start = 0;            // timeline seconds
    std::vector<Note> notes;     // on, off seconds from the start; pitch, velocity
    double lengthS = 0;
};
// where a clip for `notes` (from phrase()) goes, and its notes and length (see the top)
Placement placement(const std::vector<PlayedNote>& notes, int sr, const Timing& timing, double cursorS);

struct Host {
    midi_input::MidiInput* input = nullptr;
    std::function<int64_t()> cursor;                                 // the timeline's selection cursor (frames)
    std::function<void(const ClipPtr&, int64_t, const TrackPtr&)> placeClip;  // timeline.place_clip
    std::function<int()> nextTake;                                   // ++recording.takes
    std::function<void(const std::string&, int)> status;
};
// the Capture action: the last phrase played, as a clip on the selected MIDI track (null if nothing)
ClipPtr capture(const Host& host);

}  // namespace capture
