// MIDI effects: a MIDI track's notes changed before its instrument hears them (Track::midiEffects, in
// order; audio/midi_effects.py). No GUI.
//
// They work on a clip's notes when it is prepared (Clip::prepare → clip_hooks::midiEffects = apply()), so
// the built-in synth and sampler, VST instruments and freezes all hear the result, and on notes played
// live (MIDI editor keys, a MIDI keyboard → live()) for those that need no time to pass: Chord, Scale,
// Pitch, Velocity and Random (Arpeggiator, Note Length and Groove change the notes' timing, so they apply
// to clips only). Each has parameters like an audio effect (params(kind): key, name, range, unit, choices), set in
// the track's MIDI effects window. An effect never changes in place (MidiEffect::with), so undo and the
// audio thread see whole ones.
//
// Beats come from the project's timing (install(&project.timing)), so an arpeggiator's 1/16 follows the
// tempo map; a clip is prepared again when it changes. Without a timing: 120 BPM.
//
// Groove swings a clip's notes: on a 1/8, 1/16 or 1/32 grid of the song's beats, notes on the off-beats
// come later ("swing": 50 % straight … 75 %: the off-beat at three quarters of the pair of steps), notes
// between the grid lines in proportion, notes on the beats not at all; "accent" makes the swung notes
// louder or softer, and "timing" / "velocity" humanize every note by up to so many ms / steps, at random
// but seeded by the note (and "seed"), so a clip comes out the same each time it is prepared.
//
// "Random" amounts are numpy's: the same Generator (default_rng: SeedSequence + PCG64) seeded the same
// way, so a project's notes come out as the Python program makes them.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "model/model.h"

namespace midi_effects {

// --- the GUI's: what each effect's window shows ---
struct ParamInfo {
    std::string key;    // MidiEffect::values' key
    std::string name;   // shown
    double low = 0, high = 1, defaultValue = 0;
    std::string unit;   // "%", "semitones", "beats", ""
    std::string scale;  // "linear", "log", "int"
    std::vector<std::string> choices;  // names: the value is the index of the one chosen
    int decimals = -1;  // shown after the point (-1: from the range)
    int steps() const;  // as automation counts them: choices - 1, or the int range, or 0 (any value)
};
// the kinds, in KINDS order ("Arpeggiator", "Chord", "Scale", "Note Length", "Velocity", "Random", "Pitch",
// "Groove")
const std::vector<std::string>& kinds();
const std::vector<ParamInfo>& params(const std::string& kind);  // empty for an unknown kind
// does it apply to notes played live too (it doesn't change their timing)?
bool isLive(const std::string& kind);
// what it does, in a sentence (its tooltip in the MIDI effects window); "" for an unknown kind
std::string describe(const std::string& kind);

// --- tables (midi_effects.py) ---
extern const std::vector<std::pair<std::string, std::vector<int>>> CHORDS;   // name, intervals (projects keep the index)
extern const std::vector<std::pair<std::string, std::vector<int>>> SCALES;   // name, steps from the root
extern const char* const KEYS[12];                                            // "C" … "B"
extern const std::vector<std::string> RATES;                                  // "1/4" … "1/32"
extern const std::vector<double> RATE_BEATS;
extern const std::vector<std::string> STYLES;                                 // "Up", "Down", …

// the pitch classes (0 = C … 11 = B) of a scale (SCALES name; unknown: none) on `root` (0…11)
std::set<int> scaleSteps(int root, const std::string& scale);
// the note of the scale nearest to `pitch` (the lower one when two are as near), within 0…127
double nearestInScale(double pitch, const std::set<int>& steps);
// the chord of `notes` notes stacked in thirds of the scale on the scale note nearest `root`, lowest first
std::vector<int> inScaleChord(int root, const std::set<int>& steps, int notes = 3);

// --- the project's timing (GUI thread) ---
// Use the project's timing for beats, and set clip_hooks::midiEffects. Call once at start-up (and
// again if the Timing object is another one).
void install(const Timing* timing);
const Timing* timing();
// for a while, another project's timing (loading a project or a template into a new Project)
class TimingScope {
public:
    explicit TimingScope(const Timing* t);
    ~TimingScope();

private:
    const Timing* before_;
};

// --- applying them ---
// one effect on notes (in clip time; origin: the timeline's seconds at note time 0)
std::vector<Note> process(const MidiEffect& effect, const std::vector<Note>& notes, double origin);
// A clip's notes after the track's MIDI effects (in order), sorted by start, then pitch.
std::vector<Note> apply(const Track& track, std::vector<Note> notes, double origin);
// [(pitch, velocity)] a note played live into the track becomes (the effects that work live)
std::vector<std::pair<int, int>> live(const Track& track, int pitch, int velocity);
std::vector<std::pair<double, double>> liveOne(const MidiEffect& effect, double pitch, double velocity);

// --- numpy's random numbers (np.random.default_rng(seed): SeedSequence + PCG64) ---
class NumpyRandom {
public:
    explicit NumpyRandom(uint64_t seed);
    double random();                          // Generator.random(): [0, 1)
    int64_t integers(int64_t low, int64_t high);  // Generator.integers(low, high): [low, high)
    uint64_t next64();
    uint32_t next32();

private:
    unsigned __int128 state_ = 0, inc_ = 0;
    bool hasHalf_ = false;
    uint32_t half_ = 0;
};

}  // namespace midi_effects
