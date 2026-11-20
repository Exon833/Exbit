// Warped audio clips in the project (audio/warp.h renders them): turning warping on and off, their markers, and
// keeping them on the song's tempo.
//
// A warped clip's data is rendered for the tempo map and the place it was prepared at (Clip::warpKey: the map,
// and its ORIGIN, the timeline frame of its data's frame 0 = start - offset; beat 0 of the clip's beat time).
// When the song's tempo changes, or the clip is moved or slipped while the tempo has changes, retime() renders it
// again: the clip keeps its start, and its window (offset, length, fades, gain envelope, loop) keeps the same
// part of its audio, now as long as the new tempo makes it (a tempo halved: twice as long). Under one steady
// tempo, moving a clip changes nothing in its audio (only its key is brought along).
//
// The main window calls retime() after each tempo change (MainWindow::retimeWarped: setTiming, the ruler's
// tempo changes) and before each undo step is noted (a clip moved under tempo changes), so the change and the
// clips' new audio are one step. Undo puts back each clip's data with its key, so nothing renders again then.
// GUI thread; the clips' new audio is swapped in atomically (the audio thread may be playing them), rendered on
// every core (one clip per worker; a single long clip uses them all itself).
#pragma once
#include <cstdint>
#include <vector>

#include "audio/warp.h"
#include "model/model.h"

namespace warping {

// the markers a clip warps by: its own, or straight at its tempo (sourceBpm) from its first frame
warp::Markers markersOf(const Clip& c);
double bpmOf(const Clip& c);  // its own tempo (sourceBpm, or warp::DEFAULT_BPM)

// How a clip's data frames (engine rate, before looping) and its source's frames meet, as it is prepared now
struct Frames {
    bool warped = false, reverse = false;
    int sr = 0;
    double k = 1;          // unwarped: source frames per data frame
    double n = 0;          // source frames
    double length = 0;     // data frames before looping
    TempoMapPtr map;       // warped: what it was rendered for
    double origin = 0, b0 = 0;  // …its data's start: song seconds and beat
    warp::BeatMap beats{{}, warp::DEFAULT_BPM, 1};
    double toSource(double dataFrame) const;
    double toData(double sourceFrame) const;
    double beatAt(double dataFrame) const;    // warped: the clip's beat there
    double dataAt(double beat) const;         // …and back
};
Frames framesOf(const Clip& c, int sr);

// --- changes (GUI thread) ---
// a copy of `clip` warped as said (on: with `mode`, `preserve`, its tempo `bpm` and `markers`, null: straight at
// bpm), pitched `pitch` semitones, prepared: its start stays, its window keeps the same part of its audio
ClipPtr rewarped(const ClipPtr& clip, bool on, warp::Mode mode, int preserve, double bpm,
                 std::shared_ptr<const warp::Markers> markers, double pitch, int sr);
// new markers on a placed clip (dragged, added, removed): rendered and swapped in now (it may be playing). Its
// window stays where it is in its data (the audio moves under it); one reaching its audio's end keeps reaching it.
void setMarkers(const ClipPtr& clip, warp::Markers markers, int sr);
// "Warp from here": a marker at the clicked timeline frame on the nearest bar of the song, the markers after it
// replaced by one at the end, straight at the clip's tempo from there (Live's "Warp From Here (Straight)")
warp::Markers fromHere(const Clip& clip, int64_t frame, const TempoMap& map, int sr);
// warped clips whose tempo map or place changed rendered again (see the top); returns how many changed
int retime(Project& p, int sr);

// --- where markers and transients are on the timeline (drawing, the mouse) ---
// the timeline frame where clip beat `beat` plays (its first pass)
int64_t frameOfBeat(const Clip& c, double beat, int sr);
// the clip beat at timeline frame `frame`
double beatAtFrame(const Clip& c, int64_t frame, int sr);
// the timeline frame where source frame `f` plays
int64_t frameOfSource(const Clip& c, double f, int sr);

}  // namespace warping
