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
// and pan), and a return plays what is sent to it through its effects, fader and pan. They have no
// clips.
//
// A track's own fader and pan can be automated: such a lane's slot is the track's `controls`, a
// PluginSlot of kind "controls" (never loaded, not in pluginSlots()), with param "Volume" or "Pan".
//
// Shared pointers the audio thread reads (PluginSlot::instance, Track::instrument, Track::frozen,
// Track::sampler, Clip::track) are replaced with std::atomic_store and read with std::atomic_load.
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
struct PluginSlot;
struct Clip;
struct Automation;
using TrackPtr = std::shared_ptr<Track>;
using GroupPtr = std::shared_ptr<Group>;
using SlotPtr = std::shared_ptr<PluginSlot>;
using ClipPtr = std::shared_ptr<Clip>;
using AutomationPtr = std::shared_ptr<Automation>;
using SamplerPtr = std::shared_ptr<const SamplerSettings>;

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
    std::shared_ptr<plug::Plugin> instance;  // the loaded plugin; null while not loaded (or missing)
    std::atomic<bool> failed{false};         // set by the audio thread if the plugin failed; it is then skipped
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
struct Group {
    std::string name;
    bool folded = false;  // member lanes hidden; the header lane shows their clips in miniature
    bool muted = false;
    bool soloed = false;
    std::string color;    // "#rrggbb" for its header row; "" = the standard look
    std::string tone;     // as Track::tone
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
    bool operator==(const AutoPoint& o) const { return t == o.t && v == o.v; }
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
    // replace the curve: kept in time order, values within 0…1
    void setPoints(std::vector<AutoPoint> pts);
};

// --- a MIDI effect on a MIDI track (audio/midi_effects.py): its kind and its values, by key ---
// Never changed in place: with() makes a changed copy, and the track's list is replaced.
struct MidiEffect {
    std::string kind;                     // "Arpeggiator", "Chord", "Scale", "Note Length", "Velocity", "Random", "Pitch"
    std::map<std::string, double> values;  // every parameter's value (MidiEffect::make fills the defaults)
    double get(const std::string& key) const;  // its value (the default if not set)
    MidiEffect with(const std::string& key, double value) const;
    bool operator==(const MidiEffect& o) const { return kind == o.kind && values == o.values; }
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

// --- a send from a track to a return track: its sound after fader and pan, at `db` ---
struct Send {
    TrackPtr track;  // the return track
    double db = 0;
    bool operator==(const Send& o) const { return track == o.track && db == o.db; }
    bool operator!=(const Send& o) const { return !(*this == o); }
};

// --- a track ---
struct Track {
    std::string name;
    Kind kind = Kind::Audio;
    bool muted = false;
    bool soloed = false;
    int output = 0;           // first device output (0-based) of the pair it plays on: 0 = Out 1/2, 2 = Out 3/4
    double volumeDb = 0;      // its fader, after its effects (MIN_DB = silent); not part of freezes and bounces
    double delayMs = 0;       // plays this much later (negative: earlier), as delay compensation lines it up; not frozen
    double pan = 0;           // its balance after the fader: -1 left … 0 centre … 1 right; not part of freezes and bounces
    List<Send> sends;         // to return tracks (see the top)
    int height = config::TRACK_H;
    SlotPtr instrument;       // MIDI tracks: a VST instrument; null = the built-in synth (or sampler)
    List<MidiEffect> midiEffects;  // MIDI tracks: MIDI effects changing its notes, in order
    SamplerPtr sampler;       // MIDI tracks: the built-in sampler, played when there is no instrument plugin;
                              // null = the built-in synth
    bool needsInstrument = false;  // a new MIDI track: silent until an instrument is chosen
    List<SlotPtr> effects;    // in its patch space
    Cables cables{straight()};
    std::string color;        // "#rrggbb" for its row; "" = the standard look
    std::string tone;         // "light" / "dark": fixed text colour on its row; "" = automatic
    GroupPtr group;
    std::shared_ptr<Clip> frozen;  // the track rendered, played instead of its clips and plugins while frozen
                                   // (its `track` stays null: a back pointer would be a shared_ptr cycle)
    List<AutomationPtr> automation;
    bool showAutomation = false;
    List<ClipPtr> takes;      // every recorded take (clips, not placed: copies of them as recorded), for comping
                              // (their `track` stays null: a back pointer would be a shared_ptr cycle)
    bool showTakes = false;   // its take lanes are shown under it
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
    // does it play its built-in sampler (it has one, and no instrument plugin or patch panel)?
    bool playsSampler() const { return sampler && !instrument && !usesRack; }
    Track(std::string n = "", Kind k = Kind::Audio);
};

// --- a clip: a window onto `data` ---
// frames [offset, offset + frames()) of data play from timeline frame `start`.
// Splitting makes copies (copyClip) that share data with different windows.
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

    // MIDI clips
    std::shared_ptr<const std::vector<Note>> notes;  // times relative to the clip start
    double lengthS = 0;   // clip length without the synth's release tail (looping: the length of one pass)
    std::shared_ptr<const std::vector<Note>> mutedNotes;  // switched off: kept, never played (null = none)
    std::shared_ptr<const std::vector<Note>> played;      // the notes as they play: `notes` repeated as far as
                                                          // the clip reaches when looping, after the MIDI effects
    struct Event { int64_t on, off; int pitch, velocity; };
    std::shared_ptr<const std::vector<Event>> events;  // for VST instruments (frames, relative to the clip start)

    int64_t frames() const { return length >= 0 ? length : (data ? data->frames() - offset : 0); }
    int64_t end() const { return start + frames(); }
    bool loopingAudio() const { return kind == Kind::Audio && loop && loopLength > 0; }
    // (re)build data (and peaks / events) for the engine's sample rate; a looping audio clip's as far as its
    // window reaches (or `reach` frames: further, while its end is being dragged)
    void prepare(int sr, int64_t reach = -1);
    // an audio clip's window kept inside its audio (after it was prepared at another rate, rounded)
    void fitWindow();
    // (fade-in, fade-out) frames as they play: shortened in proportion when together they are longer than
    // the clip (it was trimmed); a piece cut from a faded clip counts the whole clip's length
    void fades(int64_t& fadeInOut, int64_t& fadeOutOut) const;
    bool hasLevels() const { return gainDb != 0 || fadeIn || fadeOut; }  // (else levels are all 1)
    // its gain and fades (straight lines) as a factor at `pos` frames into its window
    float level(int64_t pos) const;
    // (fade-in, fade-out) frames inside its window, and the levels (0…1) at its start and end: where its
    // fade lines are drawn and its handles sit
    void fadeParts(int64_t& partIn, int64_t& partOut, double& levelStart, double& levelEnd) const;
    // MIDI: the notes as far as the clip reaches, its notes again every lengthS while it loops
    std::vector<Note> repeated(int sr) const;
    // MIDI: the notes as the clip plays them (before its track's MIDI effects): loops repeated, cut to its
    // window, switched-off ones left out; times in seconds from the clip's start
    std::vector<Note> heardNotes(int sr) const;
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
// audio/sampler.render_notes: a MIDI clip's audio through its track's sampler (track->playsSampler()).
// Unset: the built-in synth.
extern std::function<AudioPtr(const Track& track, const std::vector<Note>& notes, int sr, double lengthS)> renderSampler;
}  // namespace clip_hooks

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

    void forgetGoneTracks();        // unplug sidechain cables from tracks that are gone (and sends to returns)
    std::vector<SlotPtr> pluginSlots() const;  // instruments, rack instruments, track effects, master effects

    // tracks
    // a new track at the bottom (above the returns), or right below `after`, or at the end of `group`;
    // a return goes at the very bottom
    TrackPtr addTrack(Kind kind = Kind::Audio, GroupPtr group = nullptr, TrackPtr after = nullptr);
    std::vector<TrackPtr> returns() const;
    std::vector<GroupPtr> groups() const;            // in use, top to bottom
    std::vector<TrackPtr> members(const GroupPtr& g) const;
    GroupPtr groupTracks(const std::vector<TrackPtr>& tracks);
    void ungroup(const GroupPtr& g);
    void gatherGroups();
    void removeTrack(const TrackPtr& t);
    int indexOf(const TrackPtr& t) const { return tracks.indexOf(t); }
    TrackPtr firstEmptyTrack(Kind kind) const;
    bool audible(const Track& t) const;               // GUI thread (uses `tracks` directly)
    static bool audibleIn(const Track& t, const std::vector<TrackPtr>& tracks);  // audio thread, on a snapshot

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
