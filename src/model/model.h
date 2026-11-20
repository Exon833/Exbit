// The project data: tracks and the clips placed on them (model.py). No audio-device or GUI code.
//
// THREADS. The audio thread reads the project while the GUI thread edits it, without locks: every
// list the audio thread reads is a List<T> (the Python program's tuples), an immutable snapshot that
// is REPLACED, never changed in place. The GUI thread (the only writer) reads it directly with
// `*list` / range-for; the audio thread takes a snapshot with `list.get()` and keeps it for the
// buffer. Objects are shared (std::shared_ptr) and compared by identity, as Python's eq=False
// dataclasses are. Scalar fields (muted, volume_db…) are plain fields, changed in place.
//
// There are two kinds of track, AUDIO and MIDI, and each only holds clips of its own kind. Both
// clip kinds have `data`: stereo audio at the engine rate that the mixer plays. A MIDI clip gets
// its data by rendering its notes with the built-in synth; on a MIDI track with a VST instrument
// the notes (`events`) are played live through the plugin instead.
//
// Any track (and the master) can hold effects, placed in its patch space (the FX window) and wired
// by hand with cables: (from, to), from = IN (the track's sound before its effects), an effect, or
// another track (TrackSource: its sound after its effects and fader, only ever into a Sidechain);
// to = an effect, an effect's Sidechain input, or OUT (on to the track's fader). A new track has IN
// wired straight to OUT. A MIDI track can play several instruments at once from its instrument
// patch panel (`usesRack`): `rack` holds them, `rackCables` wires them: only IN → instrument and
// instrument → OUT.
//
// Return tracks (kind RETURN, always below the other tracks) hold effects shared by many tracks (a
// reverb, a delay): every other track can send its sound to each of them (`sends`, after its fader
// and pan, or before them: Send::preFader), and a return plays what is sent to it through its effects, fader and pan. They have no
// clips.
//
// A track's own fader and pan can be automated: such a lane's slot is the track's `controls`, a
// PluginSlot of kind "controls" (never loaded, not in pluginSlots()), with param "Volume" or "Pan".
//
// Shared pointers the audio thread reads (PluginSlot::instance, Track::instrument, Track::frozen,
// Track::sampler, Track::padKit, Clip::track) are replaced with std::atomic_store and read with std::atomic_load.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio/dsp.h"
#include "audio/synth.h"
#include "audio/warp.h"
#include "config.h"
#include "model/timing.h"

namespace plug { class Plugin; }

// --- an immutable list, replaced as a whole (see THREADS above) ---
template <class T>
class List {
public:
    using Vec = std::vector<T>;
    using Ptr = std::shared_ptr<const Vec>;
    List() : p_(std::make_shared<const Vec>()) {}
    List(Vec v) : p_(std::make_shared<const Vec>(std::move(v))) {}
    List(std::initializer_list<T> v) : p_(std::make_shared<const Vec>(v)) {}
    List(const List& o) : p_(o.get()) {}
    List& operator=(const List& o) { set(o.get()); return *this; }
    List& operator=(Vec v) { set(std::move(v)); return *this; }
    // audio thread: a snapshot to keep while using it
    Ptr get() const { return std::atomic_load(&p_); }
    // GUI thread (the writer): the current list
    const Vec& operator*() const { return *p_; }
    const Vec* operator->() const { return p_.get(); }
    typename Vec::const_iterator begin() const { return p_->begin(); }
    typename Vec::const_iterator end() const { return p_->end(); }
    size_t size() const { return p_->size(); }
    bool empty() const { return p_->empty(); }
    const T& operator[](size_t i) const { return (*p_)[i]; }
    Vec copy() const { return *p_; }
    void set(Vec v) { std::atomic_store(&p_, Ptr(std::make_shared<const Vec>(std::move(v)))); }
    void set(Ptr p) { std::atomic_store(&p_, std::move(p)); }
    void append(const T& item) { Vec v = *p_; v.push_back(item); set(std::move(v)); }
    bool contains(const T& item) const { for (auto& x : *p_) if (x == item) return true; return false; }
    int indexOf(const T& item) const { for (size_t i = 0; i < p_->size(); i++) if ((*p_)[i] == item) return (int)i; return -1; }
private:
    Ptr p_;
};

// a list's == (Python's tuple ==): the same snapshot, or the same items
template <class T>
bool operator==(const List<T>& a, const List<T>& b) {
    return a.get() == b.get() || *a == *b;
}
template <class T>
bool operator!=(const List<T>& a, const List<T>& b) { return !(a == b); }

// AUDIO and MIDI tracks and clips; RETURN tracks (see the top; they have no clips)
enum class Kind { Audio, Midi, Return };
inline const char* kindName(Kind k) { return k == Kind::Midi ? "midi" : k == Kind::Return ? "return" : "audio"; }
inline Kind kindFromName(const std::string& s) {
    return s == "midi" ? Kind::Midi : s == "return" ? Kind::Return : Kind::Audio;
}

struct Track;
struct Group;
struct SamplerSettings;
struct PadKit;
struct PluginSlot;
struct Clip;
struct Automation;
struct ClipLink;
using TrackPtr = std::shared_ptr<Track>;
using GroupPtr = std::shared_ptr<Group>;
using SlotPtr = std::shared_ptr<PluginSlot>;
using ClipPtr = std::shared_ptr<Clip>;
using AutomationPtr = std::shared_ptr<Automation>;
using SamplerPtr = std::shared_ptr<const SamplerSettings>;
using PadKitPtr = std::shared_ptr<const PadKit>;

// --- patch cables ---
// One end of a cable: IN, OUT, an effect (or instrument), an effect's sidechain input, or another track.
struct End {
    enum Type { In, Out, Slot, Sidechain, TrackSource };
    Type type = In;
    PluginSlot* slot = nullptr;  // Slot / Sidechain (the slot is kept alive by its track's list)
    Track* track = nullptr;      // TrackSource
    static End in() { return {In, nullptr, nullptr}; }
    static End out() { return {Out, nullptr, nullptr}; }
    static End of(PluginSlot* s) { return {Slot, s, nullptr}; }
    static End sidechain(PluginSlot* s) { return {Sidechain, s, nullptr}; }
    static End source(Track* t) { return {TrackSource, nullptr, t}; }
    bool operator==(const End& o) const { return type == o.type && slot == o.slot && track == o.track; }
    bool operator!=(const End& o) const { return !(*this == o); }
    bool operator<(const End& o) const {  // (for maps and sets)
        if (type != o.type) return type < o.type;
        if (slot != o.slot) return slot < o.slot;
        return track < o.track;
    }
};
using Cable = std::pair<End, End>;
using Cables = List<Cable>;
inline std::vector<Cable> straight() { return {Cable(End::in(), End::out())}; }  // no effects in the way
// cables wiring `slots` one after the other: IN → first → … → last → OUT
std::vector<Cable> chain(const std::vector<SlotPtr>& slots);

// --- a VST3 or VST2 plugin on a track or the master ---
struct PluginSlot {
    std::string path;          // the plugin file
    std::string name;
    std::string kind;          // "instrument" or "effect"
    std::string pluginName;    // which plugin, for files that hold several ("" = none)
    bool bypass = false;
    std::string state;         // its settings as last saved or loaded (packed: plugin.h packState); "" = none
    mutable std::string lastRead;  // (main thread) its settings as currentState() last read them from the plugin
    std::shared_ptr<plug::Plugin> instance;  // the loaded plugin; null while not loaded (or missing)
    std::atomic<bool> failed{false};         // set by the audio thread if the plugin failed; it is then skipped
    // (runtime) left out while recording in low-latency monitoring (ui/recording.h): passes its input on, as bypassed
    std::atomic<bool> lowLatencyBypass{false};
    std::string error;                       // why (set together with `failed`)
    std::vector<std::string> recent;         // names of the parameters tweaked last in its window, latest first
    bool placed = false;       // `at` is set: its unit's place in the patch space
    double atX = 0, atY = 0;
    double volumeDb = 0;       // instruments in an instrument patch panel: their own volume (MIN_DB = silent)
    double mix = 1.0;          // effects: how much of its sound is heard (1) against its input passing by (0): dry / wet
    // the loaded plugin's settings now (or the saved ones when it isn't loaded)
    std::string currentState() const;
    // a track's own fader and pan as automation sees them (Track::controls), not a plugin
    bool isControls() const { return kind == CONTROLS; }
    static constexpr const char* CONTROLS = "controls";
    PluginSlot() = default;
    PluginSlot(const PluginSlot& o);  // copies everything but `failed`/`instance` sharing rules: instance is shared
};

// --- a group of tracks under a header lane ---
// GROUP BUSES (model/routing.h). A group is a bus: its tracks (and the groups inside it) play into its `bus`, a
// track of its own (never a lane: reached through the group's header row) with a fader, pan, effects, sends,
// automation and a meter; the bus plays into its parent group's bus, or the master. "Fold only" (foldOnly) turns
// the bus off: the group then only folds, mutes and solos its tracks, which play on as if it weren't there (into
// the next bus up, or the master). Muting or soloing a group mutes or solos everything inside it, nested groups
// too. A bus that changes nothing (routing::isPlain) is skipped by the mixer: its tracks go straight on.
struct Group {
    std::string name;
    bool folded = false;  // member lanes hidden; the header lane shows their clips in miniature
    bool muted = false;
    bool soloed = false;
    std::string color;    // "#rrggbb" for its header row; "" = the standard look
    std::string tone;     // as Track::tone
    bool foldOnly = false;  // no bus: it only folds, mutes and solos its tracks
    GroupPtr parent;        // the group it is in (null: none); never a loop (routing::setParent); std::atomic_store
    const TrackPtr bus;     // its bus (Track::busOf is this group); always the same one
    Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
};

// --- a named place in the song (Intro, Chorus…), shown on the ruler ---
struct Marker {
    double seconds = 0;
    std::string name;
    bool operator==(const Marker& o) const { return seconds == o.seconds && name == o.name; }
    bool operator!=(const Marker& o) const { return !(*this == o); }
};

// --- one parameter of one of a track's plugins (or its own volume or pan) following a curve over time ---
struct AutoPoint {
    double t, v;  // seconds, value 0…1
    // the shape of the segment from this point to the next (automation::valueAt): 0 straight; > 0 bows
    // it up (above the straight line), < 0 down, -1…1; it stays monotonic. Stepped parameters ignore it.
    double bend = 0;
    bool operator==(const AutoPoint& o) const { return t == o.t && v == o.v && bend == o.bend; }
    bool operator!=(const AutoPoint& o) const { return !(*this == o); }
};
struct Automation {
    SlotPtr slot;          // the track's instrument or one of its effects, or its `controls`
    std::string param;     // the parameter's name in the plugin ("Volume" / "Pan" for the controls)
    List<AutoPoint> points;  // by time
    int height = config::AUTOMATION_H;
    // looked up by automation::resolve(): the plugin instance and its parameter id (audio thread reads)
    std::shared_ptr<plug::Plugin> targetPlugin;
    uint32_t targetParam = 0;
    int targetSteps = 0;
    bool hasTarget = false;
    double last = -1;      // the value last given to the plugin (audio thread); -1 = none
    // replace the curve: kept in time order, values within 0…1 (bends within -1…1)
    void setPoints(std::vector<AutoPoint> pts);
    // --- writing automation by moving the control (automation::Recorder) ---
    bool armed = false;               // "Write": moving the control while playing writes the curve (GUI thread; not saved)
    std::atomic<bool> touched{false}; // being written now: playback leaves the control to the user's hand
    std::atomic<double> touchValue{0};  // …and its value meanwhile (the mixer plays a Volume / Pan lane at it)
};

// --- a MIDI effect on a MIDI track (audio/midi_effects.py): its kind and its values, by key ---
// Never changed in place: with() makes a changed copy, and the track's list is replaced.
struct MidiEffect {
    std::string kind;                     // "Arpeggiator", "Chord", "Scale", "Note Length", "Velocity", "Random", "Pitch", "Groove"
    std::map<std::string, double> values;  // every parameter's value (MidiEffect::make fills the defaults)
    bool bypass = false;                  // switched off: kept in the chain, changing nothing (Commit to notes sets it)
    double get(const std::string& key) const;  // its value (the default if not set)
    MidiEffect with(const std::string& key, double value) const;
    bool operator==(const MidiEffect& o) const { return kind == o.kind && values == o.values && bypass == o.bypass; }
    bool operator!=(const MidiEffect& o) const { return !(*this == o); }
    // an effect of that kind with its default values, updated from `values` (unknown keys left out);
    // false if the kind is unknown
    static bool make(const std::string& kind, const std::map<std::string, double>& values, MidiEffect& out);
    struct Param {
        const char* key;
        double defaultValue;
    };
    // the kinds (in midi_effects.py's KINDS order) and their parameters' keys and defaults
    static const std::vector<std::pair<std::string, std::vector<Param>>>& kinds();
};

// --- the built-in sampler of a MIDI track (audio/sampler.py's SamplerSettings) ---
// Replaced on every change (never changed in place), so undo and the audio thread see whole ones.
struct SamplerSettings {
    ClipPtr sample;          // an audio clip holding the sample (source, sourceSr, sourcePath: saved with the project)
    int root = 60;           // the key that plays the sample as it is
    double transpose = 0;    // semitones (fractions: fine tuning)
    double gainDb = 0;
    double velocity = 0.5;   // how much velocity changes the level: 0 (not at all) … 1 (fully)
    double attack = 0.002;   // seconds
    double decay = 0.3;
    double sustain = 1.0;    // level held while the note is (0…1)
    double release = 0.15;
    bool oneShot = false;    // play to the end of the sample, whatever the note's length
    bool loop = false;       // (not one-shot) repeat start…end while the note is held
    double start = 0.0;      // part of the sample played: fractions, start < end
    double end = 1.0;
    bool operator==(const SamplerSettings& o) const;
    bool operator!=(const SamplerSettings& o) const { return !(*this == o); }
};

// --- a Pad Kit: a MIDI track's built-in drum instrument (audio/padkit.h) ---
// PAD KIT. MAX_PADS pads, each a sample on a key of its own (FIRST_NOTE upward, chromatic, to start with),
// played with the built-in sampler's settings (`sound`: its tuning, volume, velocity, envelope, one-shot or
// held, the part played; its `root` is not used: a pad's key plays the sample at its own pitch, `transpose`
// tunes it). A pad in a CHOKE GROUP (1…CHOKE_GROUPS; 0: none) cuts off every pad of its group still
// sounding when it starts (an open hi-hat cut off by the closed one). Pads sharing a key play together.
// Replaced as a whole on every change (never changed in place), as SamplerSettings is.
struct Pad {
    SamplerSettings sound;   // sound.sample null: an empty pad (silent)
    int note = 36;           // the key that plays it
    int choke = 0;           // its choke group: 0 none, 1…PadKit::CHOKE_GROUPS
    bool empty() const;      // no sample (or no audio in it)
    bool operator==(const Pad& o) const { return sound == o.sound && note == o.note && choke == o.choke; }
    bool operator!=(const Pad& o) const { return !(*this == o); }
};
struct PadKit {
    static constexpr int MAX_PADS = 32;     // two banks of 16, shown 4 × 4
    static constexpr int BANK = 16;
    static constexpr int FIRST_NOTE = 36;   // pad 1's key (C2 here: 60 = C4); the others chromatically upward
    static constexpr int CHOKE_GROUPS = 8;
    std::vector<Pad> pads;   // always MAX_PADS
    PadKit();                // every pad empty, one-shot, on FIRST_NOTE + its index
    bool operator==(const PadKit& o) const { return pads == o.pads; }
    bool operator!=(const PadKit& o) const { return !(*this == o); }
    int used() const;        // pads with a sample
};

// --- a send from a track to a return track: its sound after fader and pan, at `db` ---
// PRE-FADER SENDS. With `preFader` the sound is taken after the track's effects but before its fader and
// pan: the send stays the same however the fader is set (a headphone mix, a reverb that keeps going while
// the dry track is faded out). Still in time with the others (its own share of the delay compensation).
struct Send {
    TrackPtr track;  // the return track
    double db = 0;
    bool preFader = false;  // before the track's fader and pan (after its effects); kept even at −inf
    bool operator==(const Send& o) const { return track == o.track && db == o.db && preFader == o.preFader; }
    bool operator!=(const Send& o) const { return !(*this == o); }
};

// --- a modulation cable (audio/modulation.h): a modulator unit of a track (an LFO, a Level Follower…:
// effects/modulators.cpp) moving a parameter of another of the track's units (an effect, another
// modulator, or its instrument) by `depth` ---
// Its working values (the audio thread's) are in a ModState shared by every copy of the cable, so
// replacing the track's list (an edit, undo) keeps them.
struct ModState {
    std::shared_ptr<plug::Plugin> target;  // the parameter's plugin (std::atomic_load / store), set by
    std::atomic<uint32_t> paramId{0};      // modulation::resolve on the GUI thread; used only while
    std::atomic<bool> hasTarget{false};    // `target` is the slot's instance
    std::atomic<double> base{-1};      // the parameter's own value (0…1) the modulation moves around; -1: none yet
    std::atomic<double> applied{-1};   // the value last given it; -1: none (another value read: the user's: new base)
};
struct ModCable {
    PluginSlot* from = nullptr;  // a modulator among the track's effects
    PluginSlot* to = nullptr;    // one of the track's effects (or modulators), or an instrument it plays
    std::string param;           // the parameter's name (as automation names it)
    double depth = 0.5;          // -1…1: how far the modulator's full swing moves it, in its 0…1 range
    std::shared_ptr<ModState> state = std::make_shared<ModState>();
    bool operator==(const ModCable& o) const {
        return from == o.from && to == o.to && param == o.param && depth == o.depth && state == o.state;
    }
    bool operator!=(const ModCable& o) const { return !(*this == o); }
};

// --- a track ---
// EXTERNAL SYNTH (audio/midi_out.h, ui/midi_ports_dialog.h): a MIDI track played by a hardware synth: its notes go
// out to a MIDI port and channel, `delayMs` early (the round trip Measure finds), and its sound comes back on the
// audio device's inputs, played through the track's effects in time (mixer.h EXTERNAL SYNTHS).
struct ExternalSynth {
    bool on = false;      // the track plays it (rather than its instrument, sampler or Pad Kit, which it keeps)
    std::string port;     // the MIDI output's stable name (midi_ports.h)
    int channel = 1;      // 1-16
    int input = -1;       // the first audio input (0-based) its sound comes back on; -1: none (heard from the synth)
    bool stereo = true;   // …and the one after it (a stereo pair)
    double delayMs = 0;   // its notes go out this much early
    bool operator==(const ExternalSynth& o) const {
        return on == o.on && port == o.port && channel == o.channel && input == o.input && stereo == o.stereo &&
               delayMs == o.delayMs;
    }
    bool operator!=(const ExternalSynth& o) const { return !(*this == o); }
};

struct Track {
    std::string name;
    Kind kind = Kind::Audio;
    bool muted = false;
    bool soloed = false;
    bool soloSafe = false;    // SOLO SAFE: stays heard while other tracks are soloed (as a return does); mute still mutes
    int output = 0;           // first device output (0-based) of the pair it plays on: 0 = Out 1/2, 2 = Out 3/4
    double volumeDb = 0;      // its fader, after its effects (MIN_DB = silent); not part of freezes and bounces
    double delayMs = 0;       // plays this much later (negative: earlier), as delay compensation lines it up; not frozen
    double pan = 0;           // its balance after the fader: -1 left … 0 centre … 1 right; not part of freezes and bounces
    List<Send> sends;         // to return tracks (see the top)
    int height = config::TRACK_H;
    SlotPtr instrument;       // MIDI tracks: a VST instrument; null = the built-in synth (or sampler)
    List<MidiEffect> midiEffects;  // MIDI tracks: MIDI effects changing its notes, in order
    SamplerPtr sampler;       // MIDI tracks: the built-in sampler, played when there is no instrument plugin
                              // (nor Pad Kit); null = the built-in synth
    PadKitPtr padKit;         // MIDI tracks: a Pad Kit (PAD KIT above), played when there is no instrument plugin
    // PADS ON THEIR OWN TRACKS (model/pad_tracks.h): a Pad Kit's track: the track each pad plays on (PadKit::MAX_PADS
    // of them; null: in the kit's own sound); null: every pad in the kit. Replaced whole (std::atomic_store)
    std::shared_ptr<const std::vector<TrackPtr>> padOut;
    bool needsInstrument = false;  // a new MIDI track: silent until an instrument is chosen
    int bendRange = 2;        // MIDI tracks: semitones the bend wheel bends its built-in synth or sampler played live,
                              // all the way up or down (1…24; right-click its row → Pitch bend range…)
    double swing = -1;        // MIDI tracks: its own swing (0…100 %, midi_effects.h SWING); -1 = the song's
    // MIDI tracks: the MIDI input it plays live (ui/midi_input.h): "" all of them, while it is the selected track;
    // else that port's stable name (midi_ports.h), selected or not, on `midiInChannel` (1-16; 0 any)
    std::string midiIn;
    int midiInChannel = 0;
    ExternalSynth ext;        // MIDI tracks: an External synth plays it (above)
    List<SlotPtr> effects;    // in its patch space
    Cables cables{straight()};
    List<ModCable> modCables; // modulators moving its units' knobs (see ModCable)
    std::string color;        // "#rrggbb" for its row; "" = the standard look
    std::string tone;         // "light" / "dark": fixed text colour on its row; "" = automatic
    GroupPtr group;           // its group (the innermost one, when groups are nested)
    TrackPtr into;            // OUTPUT ROUTING (model/routing.h): the track or group bus its sound goes into, before that
                              // one's effects; null: its group's bus (or the master); never a loop; std::atomic_store
    Group* busOf = nullptr;   // a group's bus: that group (it then has no clips and isn't in Project::tracks)
    std::shared_ptr<Clip> frozen;  // the track rendered, played instead of its clips and plugins while frozen
                                   // (its `track` stays null: a back pointer would be a shared_ptr cycle)
    List<AutomationPtr> automation;
    bool showAutomation = false;
    List<ClipPtr> takes;      // every recorded take (clips, not placed: copies of them as recorded), for comping
                              // (their `track` stays null: a back pointer would be a shared_ptr cycle)
    bool showTakes = false;   // its take lanes are shown under it
    // RECORDING SEVERAL TRACKS (ui/recording.h): armed (●) to be recorded onto by ● Rec (saved; not undone: it is
    // how the project is worked, not what it is); hearing its input through its effects, fader and pan (monitor:
    // Auto while armed, In always, Off never; audio tracks); the device input an audio track records from and hears:
    // inputCh (0-based; -1: none) and, with inputStereo, the one after it as a pair. Plain fields the mixer reads.
    enum class Monitor : int { Auto, In, Off };
    bool armed = false;
    Monitor monitor = Monitor::Auto;
    int inputCh = 0;
    bool inputStereo = false;
    std::shared_ptr<void> patchState;  // the audio thread's working state for its cables (audio/patch)
    bool usesRack = false;
    List<SlotPtr> rack;       // instruments of its instrument patch panel
    Cables rackCables;
    std::shared_ptr<void> rackState;   // the audio thread's delay lines lining up its instruments
    const SlotPtr controls;   // its fader and pan as automation sees them (always the same one; see the top)

    // the instrument plugins it plays: its rack's, its one instrument, or none (built-in synth; audio tracks)
    std::vector<SlotPtr> instruments() const;
    // does it play instrument plugins (its own, or its instrument patch panel's) rather than the built-in
    // synth or sampler? (plugins.py's plays_plugins)
    bool playsPlugins() const;
    // does it play its built-in sampler (it has one, and no instrument plugin, patch panel or Pad Kit)?
    bool playsSampler() const { return sampler && !padKit && !instrument && !usesRack && !ext.on; }
    // does it play its Pad Kit (it has one, and no instrument plugin or patch panel)?
    bool playsPadKit() const { return padKit && !instrument && !usesRack && !ext.on; }
    // what a key plays, for the MIDI editor's keys: the name of its pad when it plays a Pad Kit; "" otherwise
    std::string keyLabel(int pitch) const;
    Track(std::string n = "", Kind k = Kind::Audio);
};

// --- fade shapes (an audio clip's fade-in and fade-out) ---
// FADE SHAPES. How a fade's level goes between silence and full (Clip::fadeInShape / fadeOutShape):
//   Linear       a straight line (every fade before shapes existed; files without a shape)
//   Smooth       an S-curve: gentle at both ends
//   Fast         most of the change early: a fade-in is loud soon, a fade-out quiet soon
//   Slow         most of the change late
//   Equal power  a quarter of a sine (fade-in) and of a cosine (fade-out): two clips crossfading with it
//                keep the same loudness all through (sin² + cos² = 1); crossfades made by overlapping
//                clips (edits::crossfaded) start with it
// fadeCurve() is what the mixer runs for every sample of a fade: polynomials only (no pow() or sin()),
// the same in playback and renders.
enum class FadeShape : uint8_t { Linear, Smooth, Fast, Slow, EqualPower };
constexpr FadeShape FADE_SHAPES[] = {FadeShape::Linear, FadeShape::Smooth, FadeShape::Fast, FadeShape::Slow,
                                     FadeShape::EqualPower};
const char* fadeShapeName(FadeShape s);              // in project files: "linear", "smooth", "fast", "slow", "equal_power"
FadeShape fadeShapeFromName(const std::string& s);   // (anything else: Linear)
const char* fadeShapeLabel(FadeShape s);             // in menus: "Linear", "Smooth (S-curve)"…
// a fade's shape as the other kind of fade heard backwards (a reversed clip's fade-in becomes its fade-out)
inline FadeShape mirroredFade(FadeShape s) {
    return s == FadeShape::Fast ? FadeShape::Slow : s == FadeShape::Slow ? FadeShape::Fast : s;
}
// The level (0…1) of a fade `x` (0…1) of the way from its silent end to its full end: a fade-in's x grows
// with time, a fade-out's (`out`) shrinks. Linear gives x itself, exactly.
inline double fadeCurve(FadeShape s, double x, bool out) {
    if (s == FadeShape::Linear) return x;
    if (out && (s == FadeShape::Fast || s == FadeShape::Slow)) s = mirroredFade(s);  // (fast: quiet soon)
    switch (s) {
    case FadeShape::Smooth:
        return x * x * (3 - 2 * x);
    case FadeShape::Fast: {
        double y = 1 - x;
        return 1 - y * y * y;
    }
    case FadeShape::Slow:
        return x * x * x;
    case FadeShape::EqualPower: {  // sin(x·π/2): its series to t¹¹ (off by under 4e-8 on 0…π/2)
        double t = x * 1.5707963267948966, t2 = t * t;
        return t * (1 + t2 * (-1.0 / 6 + t2 * (1.0 / 120 + t2 * (-1.0 / 5040 + t2 * (1.0 / 362880 - t2 / 39916800)))));
    }
    default:
        return x;
    }
}

// --- a clip: a window onto `data` ---
// frames [offset, offset + frames()) of data play from timeline frame `start`.
// Splitting makes copies (copyClip) that share data with different windows.
// where a warped clip's data was rendered for (Clip::warpKey)
struct WarpKey {
    TempoMapPtr map;
    int64_t origin = 0;  // timeline frame of its data's frame 0
};

struct Clip {
    Kind kind = Kind::Audio;
    std::string name;
    TrackPtr track;
    int64_t start = 0;    // timeline position in engine frames
    int64_t offset = 0;   // first frame of data that plays
    int64_t length = -1;  // frames that play; -1 = to the end of data
    std::string color;    // "#rrggbb"; "" = the default for its kind
    std::string tone;     // "light" / "dark" / "" (automatic)
    bool disabled = false;  // stays on the timeline, drawn as an outline, but doesn't play (nor render)
    double gainDb = 0;    // audio clips: their own volume, before the track's effects
    int64_t fadeIn = 0;   // audio clips: frames of fade-in from their start…
    int64_t fadeOut = 0;  // …and of fade-out to their end (shortened to fit when the clip is: fades())
    int64_t fadeLead = 0; // a piece cut from a faded clip: frames before its start where the fade-in began…
    int64_t fadeTail = 0; // …and after its end where the fade-out ends (so its pieces play as the whole clip did)
    FadeShape fadeInShape = FadeShape::Linear;   // audio clips: how the fade-in rises (FADE SHAPES above)…
    FadeShape fadeOutShape = FadeShape::Linear;  // …and how the fade-out falls
    bool loop = false;    // audio: made longer than its audio, its loop repeats (looped()); MIDI: made longer
                          // than lengthS, its notes repeat every lengthS
    AudioPtr data;        // stereo at the engine rate (set by prepare)

    // audio clips
    AudioPtr source;      // original stereo audio
    int sourceSr = 0;
    std::string sourcePath;  // WAV in a saved project's audio folder holding exactly `source`, if any
    PeaksPtr peaks;       // waveform min/max for drawing
    double stretch = 1.0; // how many times as long as recorded it plays (2: half speed), at the same pitch
    double pitch = 0.0;   // semitones higher (or lower) than recorded, at the same speed
    double sourceBpm = 0; // the tempo it was recorded at, once matched to the project's; 0 = none (JSON null)
    bool reverse = false; // plays backwards
    double loopStart = 0; // where the loop starts, s into its audio as it plays (stretched, reversed)
    double loopLength = 0;  // …and how long it is; 0: not looping
    int64_t baseFrames = 0; // frames of its audio before any looping (set by prepare)
    int64_t loopFrom = 0, loopFor = 0;  // a looping one's loop in frames of `data` (set by prepare with it)
    // its gain envelope (model/clip_envelope.h): a volume curve over its audio, with its gain and fades; null = none.
    // Points at frames of its audio as it plays (before looping), values 0…1: silence … +24 dB. Replaced whole.
    std::shared_ptr<const std::vector<AutoPoint>> gainEnvelope;
    bool envelopeShown = false;  // (view, not saved) its envelope's line is drawn over it and edited by the mouse
    // WARPING (audio/warp.h, model/warping.h): warped, its audio follows the song's tempo (its `stretch` is then
    // not used). Beat 0 of its beat time is where its data starts on the timeline (start - offset).
    bool warp = false;
    warp::Mode warpMode = warp::Mode::Beats;
    int warpPreserve = warp::TRANSIENTS;  // what Beats cuts at (warp::Preserve)
    // its warp markers (replaced whole); null: straight at sourceBpm (or warp::DEFAULT_BPM) from its first frame
    std::shared_ptr<const warp::Markers> warpMarkers;
    // (runtime) the tempo map and origin (start - offset, frames) its data was rendered for: model/warping.h
    // renders it again when the song's tempo or its place changes; kept with `data` by undo
    std::shared_ptr<const WarpKey> warpKey;

    // MIDI clips
    std::shared_ptr<const std::vector<Note>> notes;  // times relative to the clip start
    double lengthS = 0;   // clip length without the synth's release tail (looping: the length of one pass)
    std::shared_ptr<const std::vector<Note>> mutedNotes;  // switched off: kept, never played (null = none)
    std::shared_ptr<const std::vector<Note>> played;      // the notes as they play: `notes` repeated as far as
                                                          // the clip reaches when looping, after the MIDI effects
    struct Event { int64_t on, off; int pitch, velocity; };
    std::shared_ptr<const std::vector<Event>> events;  // for VST instruments (frames, relative to the clip start)
    // MIDI clips linked to each other share it (see "linked MIDI clips" at the end); null = its notes are its own
    std::shared_ptr<ClipLink> link;
    // its track's MIDI effects and swing are already written into its notes (Commit to notes, ui/midi_commit.h):
    // prepared without them
    bool effectsCommitted = false;
    int key = -1;         // MIDI clips: its own key (0 = C … 11 = B) and scale (SCALES name, "" = none) for the
    std::string scale;    // MIDI editor, instead of the song's (Timing::key / scale); -1 = the song's

    int64_t frames() const { return length >= 0 ? length : (data ? data->frames() - offset : 0); }
    int64_t end() const { return start + frames(); }
    bool loopingAudio() const { return kind == Kind::Audio && loop && loopLength > 0; }
    // (re)build data (and peaks / events) for the engine's sample rate; a looping audio clip's as far as its
    // window reaches (or `reach` frames: further, while its end is being dragged)
    void prepare(int sr, int64_t reach = -1);
    // a warped clip's rendering for `map`, its data starting at timeline frame `origin` (audio/warp.h)
    warp::Job warpJob(const TempoMapPtr& map, int64_t origin, int sr) const;
    // an audio clip's window kept inside its audio (after it was prepared at another rate, rounded)
    void fitWindow();
    // (fade-in, fade-out) frames as they play: shortened in proportion when together they are longer than
    // the clip (it was trimmed); a piece cut from a faded clip counts the whole clip's length
    void fades(int64_t& fadeInOut, int64_t& fadeOutOut) const;
    bool hasLevels() const { return gainDb != 0 || fadeIn || fadeOut || std::atomic_load(&gainEnvelope); }  // (else all 1)
    // its gain, fades (each of its shape) and gain envelope as a factor at `pos` frames into its window
    float level(int64_t pos) const;
    // (fade-in, fade-out) frames inside its window, and how far (0…1, fadeCurve's x) its fades are at its start
    // and end: where its fade curves are drawn and its handles sit
    void fadeParts(int64_t& partIn, int64_t& partOut, double& levelStart, double& levelEnd) const;
    // MIDI: the notes as far as the clip reaches, its notes again every lengthS while it loops
    std::vector<Note> repeated(int sr) const;
    // MIDI: the notes as the clip plays them (before its track's MIDI effects): loops repeated, cut to its
    // window, switched-off ones left out; times in seconds from the clip's start
    std::vector<Note> heardNotes(int sr) const;
    // MIDI: repeated(), each note then played as its own properties say (note_props::perform): left out when
    // its chance says so (decided by the note and its loop pass, the same every time), split into its
    // repeats. What prepare() hands the track's MIDI effects and instrument.
    std::vector<Note> performed(int sr) const;
};
ClipPtr copyClip(const ClipPtr& c);  // dataclasses.replace(): a new clip sharing the audio
ClipPtr makeAudioClip(const std::string& name, AudioPtr source, int sourceSr, int sr);
ClipPtr makeMidiClip(const std::string& name, std::vector<Note> notes, double lengthS, int sr);

// `audio` up to the end of its loop (frames start…start + frames), then the loop again and again until
// `reach` frames: each pass begins with a `seam`-frame crossfade from what followed the loop in the audio
// (if anything did), so a loop not cut at a zero crossing doesn't click
AudioPtr looped(const AudioPtr& audio, int64_t start, int64_t frames, int64_t reach, int64_t seam);

// What preparing clips needs from modules the model doesn't own; set by them at start-up (main.cpp or
// the module's own init). Unset: the fallbacks noted. GUI thread.
namespace clip_hooks {
// audio/midi_effects.apply: a clip's notes after the track's MIDI effects (origin: the timeline's seconds at
// note time 0), sorted by start then pitch. Unset: the notes as they are.
extern std::function<std::vector<Note>(const Track& track, const std::vector<Note>& notes, double origin)> midiEffects;
// the song's tempo map, for warped audio clips (the main window: its project's timing.audioMap()). Unset: 120 BPM.
extern std::function<TempoMapPtr()> tempoMap;
// the tempo map warped clips are prepared with on this thread now: the one a UseTempoMap gave, else tempoMap()
TempoMapPtr currentTempoMap();
// while it lives, clips prepared on this thread use `map` (a project being opened: its own tempo)
struct UseTempoMap {
    explicit UseTempoMap(TempoMapPtr map);
    ~UseTempoMap();
    TempoMapPtr before;
};
// audio/sampler.render_notes: a MIDI clip's audio through its track's sampler (track->playsSampler()) or
// Pad Kit (track->playsPadKit()). Unset: the built-in synth.
extern std::function<AudioPtr(const Track& track, const std::vector<Note>& notes, int sr, double lengthS)> renderSampler;
}  // namespace clip_hooks

// --- a MIDI controller link (ui/midi_learn.h): a knob of a MIDI controller (a control change number) moving
// a parameter, as if turned in its window ---
// GUI thread only (the audio thread never reads them). Saved with the project by track index, plugin and
// parameter name (project_io), only when there are any.
struct MidiLink {
    std::string controller;  // the MIDI input it was learned from (shown in the list; any input moves it)
    int channel = 0;         // 0-15
    int cc = 0;              // the control change number, 0-127
    TrackPtr track;          // the track holding the parameter; null: the master's effects
    SlotPtr slot;            // one of its plugins (instrument, rack instrument, effect), or its `controls` (Volume / Pan)
    std::string param;       // the parameter's name (as automation names it)
};

// --- the project ---
class Project {
public:
    Project() { clear(); }
    void clear();

    List<TrackPtr> tracks;          // in display order
    List<ClipPtr> clips;
    List<SlotPtr> masterEffects;    // on the master (outputs 1/2)
    Cables masterCables{straight()};
    std::shared_ptr<void> masterPatchState;
    std::map<std::string, int> tracksMade;  // "audio" / "midi" / "return" / "group": numbering for default names
    Timing timing;
    List<Marker> markers;           // by time
    std::vector<MidiLink> midiLinks;  // MIDI controller links (ui/midi_learn.h; GUI thread only)

    void forgetGoneTracks();        // unplug sidechain cables from tracks that are gone (and sends to returns)
    std::vector<SlotPtr> pluginSlots() const;  // instruments, rack instruments, track effects, master effects

    // tracks
    // a new track at the bottom (above the returns), or right below `after`, or at the end of `group`;
    // a return goes at the very bottom
    TrackPtr addTrack(Kind kind = Kind::Audio, GroupPtr group = nullptr, TrackPtr after = nullptr);
    std::vector<TrackPtr> returns() const;
    std::vector<GroupPtr> groups() const;            // in use (and the groups they are in), top to bottom, outer first
    // every track the mixer may play: `tracks`, then every group's bus (groups() order); GUI thread
    std::vector<TrackPtr> allTracks() const;
    std::vector<TrackPtr> members(const GroupPtr& g) const;
    GroupPtr groupTracks(const std::vector<TrackPtr>& tracks);
    void ungroup(const GroupPtr& g);
    void gatherGroups();
    void removeTrack(const TrackPtr& t);
    int indexOf(const TrackPtr& t) const { return tracks.indexOf(t); }
    TrackPtr firstEmptyTrack(Kind kind) const;
    bool audible(const Track& t) const;               // GUI thread (uses `tracks` directly)
    static bool audibleIn(const Track& t, const std::vector<TrackPtr>& tracks);  // audio thread, on a snapshot
    static bool mutedIn(const Track& t);  // muted: itself, a group it is in, or (a bus) its group or one around it
    // exclusive solo (Alt-click on S): `t` soloed, every other track and group unsoloed
    void soloOnly(const TrackPtr& t);

    // clips
    void addClip(const ClipPtr& c) { clips.append(c); }
    void removeClips(const std::vector<ClipPtr>& cs);
    std::vector<ClipPtr> clipsOn(const TrackPtr& t) const;
    std::vector<ClipPtr> clipsIn(const std::vector<TrackPtr>& ts, int64_t start, int64_t end) const;
    void setMarkers(std::vector<Marker> ms);  // kept by time
    int64_t endFrame() const;
    // every clip (placed, frozen renderings, recorded takes) in frames of the new rate: its start and end are
    // converted, not its length, so clips that met still meet
    void changeSampleRate(int oldSr, int newSr);

    // plugins whose slot was removed: kept alive a moment so the audio thread never destroys one
    void retire(std::shared_ptr<plug::Plugin> p);
    void purgeRetired(double now);  // GUI thread, now and then
private:
    std::vector<std::pair<double, std::shared_ptr<plug::Plugin>>> retired_;
};

// --- linked MIDI clips ---
// Copies of a MIDI clip can be LINKED (the timeline's Alt+Shift-drag or "Duplicate as linked"): they hold
// the same ClipLink and always the same notes, switched-off notes, lengthS (a loop's pass) and loop switch,
// the very same immutable note vectors. After editing one (the MIDI editor, quantize, the loop switch, a
// length), links::notesChanged gives every other one the same and prepares it again (its audio and events,
// for its own track and place). Their start, track, name, colour, window (offset, length) and being
// disabled stay their own. A plain copy (Ctrl-drag, Ctrl+D, paste) is independent; the pieces of an
// explicit split, and joined clips, are independent too (the timeline makes them unique); a clip trimmed
// by one placed over it or by cutting a range out stays linked (the same notes in a smaller window).
// A linked clip on a frozen track takes the notes too; the track plays its frozen sound (as it was frozen)
// until it is unfrozen. Freezing and bouncing render each clip as it is prepared, linked or not.
// Undo restores every clip's fields, so linked clips come back together. Saved as a "link" number per
// clip (project_io). GUI thread: the audio thread only reads what notesChanged prepares, swapped in
// atomically.
struct ClipLink {};
namespace links {
// every clip of the project sharing c's link, c included, in the project's order; empty: c isn't linked
std::vector<ClipPtr> partners(const Project& p, const Clip& c);
// how many clips of the project share c's link (0: none)
int count(const Project& p, const Clip& c);
// is c linked to at least one other clip of the project?
inline bool linked(const Project& p, const Clip& c) { return count(p, c) >= 2; }
// how many clips of the project share each link (every clip's badge, worked out in one pass)
std::map<const ClipLink*, int> counts(const Project& p);
// a copy of a MIDI clip linked to it (it gets a link first if it had none); an audio clip: a plain copy
ClipPtr linkedCopy(const ClipPtr& c);
// a copy of a clip that is independent of it (no link)
ClipPtr plainCopy(const ClipPtr& c);
// after `from`'s notes (or switched-off notes, lengthS, loop switch) changed: every other clip linked to
// it gets them and is prepared again at `sr`; one playing to the end of its notes (length -1) keeps the
// window it had. Returns the clips changed.
std::vector<ClipPtr> notesChanged(Project& p, const ClipPtr& from, int sr);
// these clips leave their links, keeping their notes; a link left with a single clip is undone too
void makeUnique(Project& p, const std::vector<ClipPtr>& clips);
}  // namespace links
