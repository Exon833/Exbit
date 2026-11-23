// The built-in synth's patch (a MIDI track's Track::synth): a simple subtractive synth on one page.
//
// TWO VOICES. A track without a patch (Track::synth null, every project made before it existed) plays
// the CLASSIC voice (synth.h: three sine harmonics, untouched, bit for bit). A track with a patch plays
// this synth: two oscillators (sine, triangle, saw, pulse with width; octave, semitones, cents, level)
// and noise, unison (1…8 voices, detuned by `spread`, panned across), one filter (low-, high- or
// band-pass: cutoff, resonance, drive, key tracking, Env 2's amount), Env 1 (the level) and Env 2 (for
// modulation), an LFO (five shapes; free in Hz or synced to the song's beats; retriggered by each note or
// free-running), glide, mono or poly, drift (slow random pitch and cutoff movement per voice) and an
// output level. MODULATION: any of Env 1, Env 2 and the LFO moves any modulatable knob (ParamInfo::mod)
// by `depth` (-1…1 of the knob's whole range), as many routings as wanted.
//
// SOUND (synth_voice.cpp). Band-limited oscillators (PolyBLEP for the saw's and pulse's steps,
// PolyBLAMP for the triangle's corners: Välimäki et al.), a zero-delay-feedback state-variable filter
// (Simper's trapezoidal SVF: stable at any cutoff and resonance; drive saturates its input, so it stays
// stable), modulation worked out every CONTROL frames. Notes are rendered ahead of time, as the
// classic voice's are: a clip's on the worker pool (one job per voice, summed in note order: the same,
// bit for bit, however many threads), a live note on the GUI thread (noteRenderer) — nothing is ever
// worked out or allocated on the audio thread. Everything random (unison phases, drift, noise, S&H) is
// seeded by the note, so a clip renders the same every time.
//
// Patches are replaced whole on every change (never changed in place), as SamplerSettings is: undo and
// the audio thread see whole ones. Saved in projects (project_io: "synth", only when set) and as preset
// files: JSON {"format": "tinydaw-synth", "name", "category", "params": {key: value}, "mods": [{"source",
// "target", "depth"}], and a Drawn synth's "drawn": [WAVE_POINTS numbers]}; keys missing take their defaults. The presets shipped are in the program's data
// (data/presets/synth/<category>/), the user's own in <data>/presets/synth/<category>/ (paths.h).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "audio/synth.h"
#include "core/json.h"

struct Track;
namespace live { struct NoteRender; }

namespace subsynth {

enum Param : int {
    O1_WAVE, O1_OCT, O1_SEMI, O1_FINE, O1_LEVEL, O1_WIDTH,
    O2_WAVE, O2_OCT, O2_SEMI, O2_FINE, O2_LEVEL, O2_WIDTH,
    NOISE, UNISON, SPREAD,
    F_TYPE, CUTOFF, RESO, DRIVE, KEYTRACK, F_ENV,
    E1_A, E1_D, E1_S, E1_R,
    E2_A, E2_D, E2_S, E2_R,
    LFO_SHAPE, LFO_RATE, LFO_SYNC, LFO_RETRIG,
    GLIDE, MONO, DRIFT, OUTPUT,
    PARAMS
};
enum Wave : int { SINE, TRIANGLE, SAW, PULSE };
enum FilterType : int { LOWPASS, HIGHPASS, BANDPASS };
enum Source : int { ENV1, ENV2, LFO, SOURCES };
constexpr int MAX_UNISON = 8;
constexpr int CONTROL = 16;            // frames between modulation updates
constexpr double MAX_TAIL_S = 30.0;    // a release never rings longer
constexpr double OUT_GAIN = 0.2;       // a note's level at velocity 127, one oscillator at full level, 0 dB
constexpr const char* FORMAT = "tinydaw-synth";
constexpr int WAVE_POINTS = 256;       // a drawn wave's points across its one cycle (Patch::drawn)

struct ParamInfo {
    enum Kind { Number, Log, Int, Choice, Toggle };
    const char* key;      // in files
    const char* name;     // shown
    const char* section;  // "Osc 1", "Filter"…
    Kind kind;
    double lo, hi, def;
    const char* unit;     // after the value ("Hz", "s"…)
    std::vector<const char*> choices;  // Choice
    bool mod;             // a modulation target
};
const ParamInfo& info(int p);
int paramOf(const std::string& key);  // -1: none
const char* sourceName(int s);        // "Env 1", "Env 2", "LFO"
const char* sourceKey(int s);         // "env1", "env2", "lfo"
int sourceOf(const std::string& key); // -1: none
// a value as 0…1 across its range (logarithmic ones by their logarithm) and back (clamped, whole for steps)
double toNorm(int p, double v);
double fromNorm(int p, double n);
double clampValue(int p, double v);
std::string valueText(int p, double v);   // "1.20 kHz", "Saw", "On"
extern const double SYNC_BEATS[];          // LFO_SYNC's choices in beats (0: free)

struct Mod {
    int source = ENV2, target = CUTOFF;
    double depth = 0;  // -1…1 of the target's range
    bool operator==(const Mod& o) const { return source == o.source && target == o.target && depth == o.depth; }
};

struct Patch {
    double v[PARAMS];
    std::vector<Mod> mods;
    std::string name = "Init", category;
    // THE DRAWN SYNTH (Instrument ▾ → Drawn synth…; its window is a canvas, ui/wave_canvas.h): one cycle of a wave
    // drawn by hand, WAVE_POINTS values -1…1, joined by straight lines. Osc 1 plays it in place of its wave,
    // band-limited for each note's pitch and with its DC taken away (synth_voice.cpp); the rest of the patch
    // (Init's: Env 1, an open filter) is as ever. Replaced whole, as the patch is. Null: an ordinary patch.
    std::shared_ptr<const std::vector<float>> drawn;
    Patch();  // every parameter at its default ("Init": one saw through an open low-pass filter)
    double operator[](int p) const { return v[p]; }
    bool operator==(const Patch& o) const;
    bool operator!=(const Patch& o) const { return !(*this == o); }
    // the routing from source to target (null: none)
    const Mod* modOf(int source, int target) const;
    // set (depth 0: remove) the routing from source to target
    void setMod(int source, int target, double depth);
};
using PatchPtr = std::shared_ptr<const Patch>;
// a Drawn synth's patch: Init's, named "Drawn", its wave a sine to start drawing over
Patch drawnPatch();
// `wave` (any length ≥ 2, read as one cycle) as WAVE_POINTS values, each finite and within -1…1
std::vector<float> fitWave(const std::vector<float>& wave);

// --- files ---
Json toJson(const Patch& p);
// keys missing (or unknown) keep their defaults; values are clamped to their ranges
Patch fromJson(const Json& j);

// --- sound (synth_voice.cpp) ---
// Stereo audio of all notes as synth::render gives (lengthS plus the longest release, at most MAX_TAIL_S).
// beatS: the song's seconds per beat (a synced LFO). Each note's pan and fine tuning are its own; a sliding
// note (note_props) glides into its pitch, carrying the voice it took over on.
AudioPtr render(const Patch& patch, const std::vector<Note>& notes, int sr, double lengthS, double beatS = 0.5);
// one note with its release, stereo interleaved (live notes)
std::vector<float> renderNote(const Patch& patch, int pitch, int velocity, double durationS, int sr, double beatS = 0.5);
// how long a note's release rings (Env 1's release, at most MAX_TAIL_S)
double tailS(const Patch& patch);

// --- a track ---
bool plays(const Track& track);  // does it play its patch (it has one, and nothing else plays)?
// a live note's renderer: the track's patch when it plays one, else its sampler's / Pad Kit's (sampler.h)
live::NoteRender noteRenderer(const Track& track);

// --- presets ---
struct Preset {
    std::string name, category, path;
    bool user = false;  // the user's own (in the data folder), not shipped with the program
};
extern const std::vector<const char*> CATEGORIES;  // "Bass", "Lead", "Pad", "Keys", "Pluck", "FX"
std::string shippedFolder();  // <the program's data>/presets/synth
std::string userFolder();     // <data>/presets/synth
void setShippedFolder(const std::string& dir);  // tests ("" = back to the usual one)
// every preset: the shipped ones, then the user's, each by category and name
std::vector<Preset> presets();
bool load(const std::string& path, Patch& out, std::string& error);
// save as the user's preset `name` in `category` (one of that name is replaced); `path`: where
bool save(const Patch& patch, const std::string& name, const std::string& category, std::string& path,
          std::string& error);

}  // namespace subsynth
