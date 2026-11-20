// WARPING: an audio clip's audio follows the song's tempo, its changes too (Live's warping). Its WARP MARKERS
// pair frames of its audio (the source, at its own rate) with beats of the clip's own beat time (beat 0: where
// the clip's data starts, its "origin" on the timeline); each marker's frame plays at the song time of that
// beat through the tempo map (TempoMap::secondsAt), the audio between two markers stretched evenly in frames per
// beat (before the first and after the last: as the nearest two do; with fewer than two: at the clip's own
// tempo). So a clip's beats land on the song's beats however the tempo goes.
//
// Rendered when the clip is prepared (Clip::prepare), to its data at the engine's rate, and kept (rendered(): the
// last CACHE renderings), like stretch.h's. Three ways (MODES), each clip its own:
//   BEATS    the audio is cut at its transients (or every 1/4, 1/8 or 1/16 of its beats: `preserve`) and at its
//            markers, and each piece is placed at its warped time, as it was (not stretched): an attack stays an
//            attack. Short crossfades join them; a gap left by slowing down is filled by a loop of the piece's tail
//            fading out, and a piece running into the next one (speeding up) is cut there. Drums, beats.
//   TONES    the phase vocoder of stretch.h (identity phase locking), its speed changing from frame to frame as
//            the warping wants (each frame's own instantaneous frequencies, measured against a frame a little
//            before it, so any speed works, even a standstill). Single instruments, voices, pads.
//   COMPLEX  for full mixes: TONES with its transients kept (phase reset at onsets, as in Röbel's and Rubber
//            Band's transient handling): around each onset the audio plays at its own speed (the speeding up or
//            slowing down taken up between onsets instead), every frame reads only the audio between the onsets
//            around it (no pre-echo of the next attack), and the frames just after an onset start from its own
//            phases (the attack as it was). Its bass is worked out with a window twice as long (better low
//            notes), the rest with the usual one; the two meet at about 150-300 Hz.
// Pitch (semitones) works in every mode: the audio is warped that much longer, then played that much faster.
//
// The work runs on every processor core (workers::Pool): the vocoder in batches of frames (their spectra in
// parallel, the phases carried on in order, the frames put back together in parallel), BEATS piece by piece; a
// long file never needs more than its own audio and its result in memory (the vocoder's sums are kept for a
// batch at a time). Thread-safe.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "model/timing.h"

namespace warp {

enum class Mode { Beats = 0, Tones = 1, Complex = 2 };
const char* modeName(Mode m);  // "Beats", "Tones", "Complex"
Mode modeFromName(const std::string& name);  // (unknown: Beats)
// what BEATS cuts at: its transients, or every quarter, eighth or sixteenth of its beats
enum Preserve { TRANSIENTS = 0, QUARTERS = 1, EIGHTHS = 2, SIXTEENTHS = 3 };
const char* preserveName(int preserve);  // "transients", "1/4", "1/8", "1/16"
int preserveFromName(const std::string& name);

constexpr double DEFAULT_BPM = 120;  // a clip's tempo when it has none (sourceBpm 0)
constexpr int CACHE = 16;            // renderings kept

struct Marker {
    double frame = 0;  // a frame of the source (its own rate)
    double beat = 0;   // the clip's beat it plays on
    bool operator==(const Marker& o) const { return frame == o.frame && beat == o.beat; }
    bool operator!=(const Marker& o) const { return !(*this == o); }
};
using Markers = std::vector<Marker>;

// markers in order of frame, frames and beats both rising (ones breaking that order left out)
Markers ordered(Markers markers);
// a clip's first and last frame: frame 0 on beat 0, the end as many beats later as `bpm` makes it
Markers straight(int64_t frames, int sourceSr, double bpm);

// beats of a clip's beat time and frames of its source, through its markers
class BeatMap {
public:
    BeatMap(Markers markers, double bpm, int sourceSr);  // (markers ordered here)
    double frameAt(double beat) const;
    double beatAt(double frame) const;
    const Markers& markers() const { return m_; }
private:
    Markers m_;
    double fpb_;  // frames per beat at the clip's own tempo (fewer than two markers)
};

// do two tempo maps say the same?
bool sameTempo(const TempoMap& a, const TempoMap& b);

// what to render: the source, how it is warped, and where it plays
struct Job {
    AudioPtr source;
    int sourceSr = 0;
    Markers markers;          // (none: straight at `bpm` from frame 0)
    double bpm = DEFAULT_BPM; // the clip's own tempo (beyond / without markers)
    Mode mode = Mode::Beats;
    int preserve = TRANSIENTS;
    double semitones = 0;
    bool reverse = false;     // the result played backwards
    TempoMapPtr map;          // the song's tempo (null: 120 BPM)
    double origin = 0;        // song seconds where the data starts (its beat 0)
    int sr = 0;               // the engine's rate
};

// The data of a warped clip at the engine's rate: frame 0 at `origin`, as long as its audio reaches. Not kept.
AudioPtr render(const Job& job);
// the same, kept (the last CACHE renderings) for the same audio and settings
AudioPtr rendered(const Job& job);
// how long render() makes it, in seconds (without rendering)
double seconds(const Job& job);

// where the source's transients are (frames of it, in order: dsp::Onsets), kept for the last CACHE sources
std::shared_ptr<const std::vector<int64_t>> transients(const AudioPtr& source, int sourceSr);

}  // namespace warp
