// Bars and beats: the project's tempo (and its changes), and the grid the ruler shows and clicks snap
// to (timing.py).
//
// Bar 1 starts at 0 s. Beats are quarter notes; a bar has `beatsPerBar` of them (the time signature
// is beatsPerBar / 4). The tempo and time signature can change at the start of any bar (`changes`:
// (bar number, bpm, beats per bar), from bar 2 on; `bpm` and `beatsPerBar` are bar 1's): the song is
// then a row of segments (segments()), each with a steady tempo, and beats are counted on through
// them (beatAt / secondsAt). Clips and notes stay where they are in seconds; the grid, the ruler,
// snapping, the metronome, MIDI effects and the tempo plugins are given follow the changes.
//
// The grid is dynamic: its lines are always at least GRID_MIN_PX apart, so zooming in makes it
// finer (bars → beats → 8ths → 16ths → 32nds) and zooming out coarser (2, 4, 8… bars). In thirds
// view (triplets) the steps within a bar divide beats in three instead: half-note, quarter, 8th,
// 16th and 32nd triplets. Grid positions are counted in beats as exact fractions, so thirds stay
// exact; only steps that fit a whole number of times in a bar are used, so the grid always lines up
// with the bars (and each segment starts on a bar).
//
// A fixed grid (the Grid button, `grid`) keeps its step at any zoom. Its "1 bar" is a bar of the
// time signature where it is (3 beats in 3/4); a step that doesn't fit a whole number of times in a
// bar (1/2 in 3/4) is counted from each bar line, so every bar line stays a grid line (the bar's
// last step is shorter).
//
// THREADS. The tempo map (TempoMap: the segments) is an immutable object. The GUI thread uses
// Timing's own functions (each works on map(), rebuilt when bpm / beatsPerBar / changes changed).
// The audio thread (metronome, MIDI effects, the transport given to plugins) reads
// `timing.audioMap()`: the map as last published, an atomic snapshot. It is published whenever the
// GUI thread calls map() (any of Timing's tempo or grid functions) or publish(): the engine's idle()
// calls project.timing.publish() every frame, so a change of tempo is heard at once.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// an exact fraction (beats)
struct Frac {
    int64_t num = 0, den = 1;
    Frac() = default;
    Frac(int64_t n, int64_t d = 1);
    // the nearest fraction with a denominator at most maxDen (Python's Fraction(x).limit_denominator)
    static Frac fromDouble(double x, int64_t maxDen = 96);
    double value() const { return (double)num / (double)den; }
    int64_t floor() const;  // the whole number at or below it
    Frac operator+(const Frac& o) const { return Frac(num * o.den + o.num * den, den * o.den); }
    Frac operator-(const Frac& o) const { return Frac(num * o.den - o.num * den, den * o.den); }
    Frac operator*(const Frac& o) const { return Frac(num * o.num, den * o.den); }
    Frac operator/(const Frac& o) const { return Frac(num * o.den, den * o.num); }
    bool operator<(const Frac& o) const { return num * o.den < o.num * den; }
    bool operator<=(const Frac& o) const { return num * o.den <= o.num * den; }
    bool operator>(const Frac& o) const { return o < *this; }
    bool operator>=(const Frac& o) const { return o <= *this; }
    bool operator==(const Frac& o) const { return num == o.num && den == o.den; }
    bool operator!=(const Frac& o) const { return !(*this == o); }
};

namespace timing {
enum Strength { FINE = 0, BEAT = 1, BAR = 2 };
constexpr double EPSILON = 1e-4;  // beats; a position rounded to whole frames still counts as on its beat
extern const std::vector<Frac> STRAIGHT_STEPS;  // 1/8, 1/4, 1/2, 1, 2 beats
extern const std::vector<Frac> THIRDS_STEPS;    // 1/12, 1/6, 1/3, 2/3, 4/3
// a fixed grid's choices (thirds or not): STRAIGHT_STEPS + 4 ("1 bar"), or THIRDS_STEPS
const std::vector<Frac>& fixedSteps(bool thirds);
const Frac WHOLE_BAR(4);  // the fixed "1 bar" choice: a bar of the time signature where it is (gridStep)
}  // namespace timing

struct GridLine {
    double seconds;
    int strength;       // timing::BAR / BEAT / FINE
    std::string label;  // "5" for bar 5, "5.3" for its third beat, or "" (none)
};

// a change of tempo and time signature at the start of bar `bar` (1-based, ≥ 2)
struct TempoChange {
    int bar = 2;
    double bpm = 120;
    int beatsPerBar = 4;
    bool operator==(const TempoChange& o) const { return bar == o.bar && bpm == o.bpm && beatsPerBar == o.beatsPerBar; }
    bool operator!=(const TempoChange& o) const { return !(*this == o); }
    bool operator<(const TempoChange& o) const {  // (Python's tuple order)
        if (bar != o.bar) return bar < o.bar;
        if (bpm != o.bpm) return bpm < o.bpm;
        return beatsPerBar < o.beatsPerBar;
    }
};

// a stretch of steady tempo: from `seconds` (bar `bar`, 0-based; `beat` beats since 0 s) to the next one
struct Segment {
    double seconds = 0;
    double beat = 0;
    int bar = 0;
    double bpm = 120;
    int beatsPerBar = 4;
    double beatS() const { return 60.0 / bpm; }
};

struct BeatTime {
    double seconds;
    bool barStart;  // the first beat of a bar
};

// The segments of a tempo map (immutable; see THREADS above). Any thread.
class TempoMap {
public:
    TempoMap(double bpm = 120, int beatsPerBar = 4, std::vector<TempoChange> changes = {});
    const double bpm;       // bar 1's
    const int beatsPerBar;  // bar 1's
    const std::vector<TempoChange> changes;  // sorted by bar
    const std::vector<Segment>& segments() const { return segs_; }
    const Segment& segmentAt(double seconds) const;
    const Segment* following(const Segment& s) const;  // the next segment, or null
    double beatAt(double seconds) const;  // beats since 0 s (through every segment) at `seconds`
    double secondsAt(double beat) const;
    double barSeconds(int bar) const;     // where bar `bar` (0-based) starts, in seconds
    int barAt(double seconds) const;      // the bar (0-based) `seconds` falls in
    double bpmAt(double seconds) const { return segmentAt(seconds).bpm; }
    double beatSAt(double seconds) const { return segmentAt(seconds).beatS(); }
    int beatsPerBarAt(double seconds) const { return segmentAt(seconds).beatsPerBar; }
    // every beat from t0 to t1 seconds (the metronome); the audio thread passes a vector with room
    // (cleared first; it only allocates when it has to grow)
    void beatTimes(double t0, double t1, std::vector<BeatTime>& out) const;
    std::vector<BeatTime> beatTimes(double t0, double t1) const;
    bool hasChanges() const { return segs_.size() > 1; }

private:
    std::vector<Segment> segs_;
};
using TempoMapPtr = std::shared_ptr<const TempoMap>;

struct Timing {
    double bpm;
    int beatsPerBar;
    bool beats = true;     // edit in bars and beats (ruler, grid, snapping); false = minutes and seconds
    bool thirds = false;   // the grid divides beats in thirds (triplets)
    double loopStart = 0;  // the loop range, in seconds (end <= start: none chosen yet)
    double loopEnd = 0;
    bool looping = false;  // playback jumps from the loop's end back to its start
    std::vector<TempoChange> changes;  // by bar: the tempo and time signature from there (GUI thread)
    double grid = 0;       // a fixed grid step in beats (STRAIGHT_STEPS / THIRDS_STEPS / 4); 0 = it follows the zoom
    bool snapOn = true;    // clicks and drags snap to the grid (Ctrl+4); Alt does the opposite for one drag
    int key = 0;           // the song's key (0 = C … 11 = B), with `scale`: the MIDI editor's scale mode
    std::string scale;     // a scale of the MIDI effects' SCALES ("Major", "Minor"…); "" = none
    double swing = 0;      // the song's swing, 0 % (straight) … 100 %: off-beats later (midi_effects.h SWING)
    int swingGrid = 1;     // what swings: 0 the 8ths, 1 the 16ths

    Timing();
    Timing(const Timing& o);
    Timing& operator=(const Timing& o);

    // (start, end) of the loop range, if there is one
    bool loopRange(double& start, double& end) const;

    // --- the tempo map ---
    TempoMapPtr map() const;       // GUI thread: the map of the fields as they are now (and publishes it)
    void publish() const { map(); }
    TempoMapPtr audioMap() const;  // any thread: the map as last published
    const std::vector<Segment>& segments() const { return map()->segments(); }  // (valid until the next change)
    Segment segmentAt(double seconds) const { return map()->segmentAt(seconds); }
    // the tempo and time signature from bar `bar` (≥ 2) on
    void setChange(int bar, double bpm, int beatsPerBar);
    void removeChange(int bar);  // no change at bar `bar` any more
    const TempoChange* changeAtBar(int bar) const;  // null if none
    double beatAt(double seconds) const { return map()->beatAt(seconds); }
    double secondsAt(double beat) const { return map()->secondsAt(beat); }
    double barSeconds(int bar) const { return map()->barSeconds(bar); }
    int barAt(double seconds) const { return map()->barAt(seconds); }
    double bpmAt(double seconds) const { return map()->bpmAt(seconds); }
    double beatSAt(double seconds) const { return map()->beatSAt(seconds); }
    int beatsPerBarAt(double seconds) const { return map()->beatsPerBarAt(seconds); }
    std::vector<BeatTime> beatTimes(double t0, double t1) const { return map()->beatTimes(t0, t1); }
    double beatSeconds() const { return 60.0 / bpm; }  // seconds per beat at bar 1 (beatSAt(): anywhere)

    // --- the grid ---
    // do clicks and drags snap to the grid? In bars and beats, when snapping is on (Alt held: the other way)
    bool snaps(bool alt = false) const { return beats && snapOn != alt; }
    std::optional<Frac> fixedStep() const;  // the fixed grid step, or none when the grid follows the zoom
    // beats between grid lines at zoom pps where `seconds` is: the fixed grid's step if there is one
    // (drawing: never closer than GRID_MIN_PX, so a fine grid zoomed out draws coarser lines)
    Frac gridStep(double pps, double seconds = 0, bool drawing = false) const;
    // Ctrl+1 / Ctrl+2: the next finer or coarser fixed grid (from the one shown at zoom pps where
    // `seconds` is, when it follows the zoom; pps <= 0: none shown)
    void stepGrid(bool finer, double pps = 0, double seconds = 0);
    std::string gridName() const;  // "1/16", "1/8 triplets", "1 bar", "follows the zoom"
    Frac adaptiveStep(double pps, double seconds = 0) const;  // the grid that follows the zoom
    // `seconds` moved to the nearest grid line at zoom pps (floor: the one at or before it)
    double snap(double seconds, double pps, bool floor = false) const;
    // every grid line from t0 to t1 seconds; bars labelled every 1, 2, 4… bars and beats once there is room
    std::vector<GridLine> lines(double t0, double t1, double pps) const;
    std::vector<GridLine> linesBetween(double t0, double t1, double pps) const;  // (and a line either side)
    // (bar, beat, part) counted from 1: part is the 16th within the beat, or the third in thirds view
    void position(double seconds, int& bar, int& beat, int& part) const;

private:
    mutable TempoMapPtr map_;  // the last map made (read with atomic_load by the audio thread)
};

// Works out a tempo from taps (the Tap button): the average interval of the last TAP_COUNT taps.
class TapTempo {
public:
    // a tap at `now` seconds; returns the tempo (BPM) once there are two taps, else 0
    double tap(double now);
    int count() const { return (int)taps_.size(); }  // the taps counted (Python's len(taps))
private:
    std::vector<double> taps_;
};
