// The sound through a patch space: a track's (or the master's) effects, wired with cables
// (patch.py; cables are model.h's Cable).
//
// The sound comes in at IN and goes wherever the cables take it. An effect's input adds up
// everything wired into it (nothing wired in: silence), and its output can feed several inputs; what
// reaches OUT goes on to the fader (nothing: the track is silent). An effect with a sidechain
// input gets what is wired into its Sidechain the same way (nothing: silence); besides IN and
// effects, other tracks can be wired into a sidechain (TrackSource: their sound after their effects
// and fader, handed over by the mixer, which processes them first). Only effects whose output
// reaches OUT are processed (and modulators with something wired in: audio/modulation.h); a
// bypassed effect, or one that isn't loaded or has stopped after an
// error, passes its input on unchanged.
//
// An effect's Mix (PluginSlot::mix, below 1) blends its input back in: input × (1 − mix) + output × mix,
// the input held back by the effect's own look-ahead so the two are in time (effects::DryWet, one per
// node).
//
// Effects with look-ahead hold their sound back (plugin_audio::latency). Where paths of different
// delays meet (an input's cables, or an effect's input and sidechain), the quicker ones are held
// back to match (a DelayLine per cable), so parallel paths stay in time, and patchLatency() is how
// late OUT is: the track's share of the delay compensation. Everything is counted from the
// timeline: IN may already be late (an instrument with look-ahead; for the master, the latest
// track), and another track's sound is as late as that track (plugin_audio::trackLatency).
// Runs on the audio thread (and in renders); the plan of a set of cables is worked out once.
//
// A MIDI track's instrument patch panel is simpler: its notes go from IN to each instrument wired
// from it, and the sound of every instrument wired to OUT is added up, each at its own volume and
// held back to line up with the latest of them (playInstruments()).
//
// STATES. Each track keeps its patch's working state in Track::patchState (a PatchState) and its
// instrument panel's in Track::rackState (a RackState); the master's is Project::masterPatchState.
// The GUI should call ensureStates(project) (or ensureState(track)) after it adds tracks or changes
// cables, effects or a rack: that makes the plan and the buffers on the GUI thread. The audio
// thread makes a missing or outdated one itself (a few small allocations, once per change).
// An effect slot named by a cable is only used while it is in the track's `effects` (a removed
// one is never touched), a track named by a TrackSource only while it is in project.tracks.
#pragma once
#include <memory>
#include <utility>
#include <vector>

#include "audio/plugin_audio.h"
#include "effects/mix.h"
#include "model/model.h"

namespace patch {

// Holds a sound back by `frames` (a track's share of the delay compensation, or a cable's).
class DelayLine {
public:
    DelayLine() = default;
    DelayLine(int frames, int channels) { init(frames, channels); }
    void init(int frames, int channels);  // (allocates)
    int frames = 0, channels = 0;
    bool sounding = false;  // sound still held, to come out in the next buffers
    // delay `channels` planar channels of n frames in place
    void process(float* const* signal, int n);
    void clear();

private:
    std::vector<float> held_;  // planar: [channel][frames], a ring
    int pos_ = 0;
    int64_t quiet_ = 0;        // frames written since the last one that wasn't silent
};

bool isStraight(const std::vector<Cable>& cables);  // IN → OUT only: no effects in the way

// The plan of a set of cables (immutable once made, shared between threads): the effects that are
// heard (their output reaches OUT), and modulators with something wired in (audio/modulation.h), each
// after all that feed it, and what is wired into each one's input and sidechain and into OUT. A cable
// that would close a loop is left out.
// Sources are coded: -1 = IN, k >= 0 = order[k], -2 - j = leaves[j] (another track).
struct Plan {
    Cables::Ptr cables;
    struct Node {
        PluginSlot* slot;
        std::vector<int> in, side;
    };
    std::vector<Node> order;
    std::vector<int> out;
    std::vector<Track*> leaves;
};
std::shared_ptr<const Plan> makePlan(const Cables::Ptr& cables);

// A patch's working state.
struct PatchState {
    std::shared_ptr<const Plan> plan;  // the latest plan (atomic access: any thread may cache one here)
    // the owner's (audio thread's, or a render's) working state, for `planned`:
    std::shared_ptr<const Plan> planned;
    struct Work {
        std::vector<float> in, side, out;  // planar 2 x capacity
        std::vector<DelayLine> inLines, sideLines;
        effects::DryWet dry;  // its input, for its Mix
        const float* sound[2] = {nullptr, nullptr};
        int late = 0;
    };
    std::vector<Work> nodes;
    std::vector<DelayLine> outLines;
    std::vector<float> inSound, outSound, temp, zeros;
    std::vector<int> leafLate;
    std::vector<const float*> leafSound;  // 2 per leaf
    int capacity = 0;
    void reset(const std::shared_ptr<const Plan>& plan);  // forget everything held, for this plan
    void reserve(int frames);                              // buffers for this many frames
};

// another track's sound handed to the sidechains listening to it (mixer.py heard_by_sidechains)
struct Feed {
    const Track* track;
    const float* ch[2];
    int late;
};

// The instrument patch panel's state: delay lines lining up its instruments, and scratch.
struct RackState {
    std::vector<std::pair<const PluginSlot*, DelayLine>> lines;
    std::vector<float> sound;
    plugin_audio::Messages resets;
    plugin_audio::NoteScratch scratch;
    void reserve(int frames);
};

// buffer size states are made for (Engine::open sets it to the device's); bigger buffers grow them
void setBlockHint(int frames);
int blockHint();

// the state in a Track::patchState / Project::masterPatchState field, made (and stored) if missing
std::shared_ptr<PatchState> stateOf(std::shared_ptr<void>& field);
std::shared_ptr<RackState> rackStateOf(std::shared_ptr<void>& field);
// (GUI thread) make the track's states ready for its current cables; see STATES above
void ensureState(Track& track);
void ensureMasterState(Project& project);
void ensureStates(Project& project);
// (GUI thread) fresh states for the track (or master): what the cables (and with `rack`, its
// instrument panel) held back is forgotten
void freshState(Track& track, bool rack = true);
void freshMasterState(Project& project);

// the plan for `cables`, cached in the state field (made if the cached one is for other cables)
std::shared_ptr<const Plan> planFor(const Cables::Ptr& cables, const std::shared_ptr<void>& stateField);

// Frames OUT is late, IN being `inLate` frames late (the slowest path to OUT). seen: tracks whose
// latency is being worked out already. effects: the patch's effects (track.effects / masterEffects).
int patchLatency(const Cables::Ptr& cables, const std::vector<SlotPtr>& effects, int inLate,
                 const plugin_audio::TrackList& tracks, const plugin_audio::Seen* seen,
                 const std::shared_ptr<void>& stateField);
// inputOf: how late what reaches that effect's input is instead (0 if it isn't heard)
int planLatency(const Plan& plan, const std::vector<SlotPtr>& effects, int inLate,
                const plugin_audio::TrackList& tracks, const plugin_audio::Seen* seen,
                const PluginSlot* inputOf = nullptr);

// Process signal (`channels` planar channels: 1 or 2) through the cables' effects, in place.
// feeds: the other tracks wired in (missing: silence). inLate: frames the signal is late already.
// Returns how late the result is.
int runPatch(const Cables::Ptr& cables, const std::vector<SlotPtr>& effects, float* const signal[2], int channels,
             int frames, int sr, PatchState& state, const Feed* feeds, size_t feedCount, int inLate,
             const plugin_audio::Transport& transport);

// Render one buffer of the instruments a track plays and add them into out: its one instrument, or
// each heard instrument of its instrument patch panel (at its own volume, held back by a DelayLine in
// `state` to line up with the latest). Instruments of the panel that don't get the notes any more, or
// aren't heard, let go of the notes they hold; all get "all notes off".
void playInstruments(Track& track, const plugin_audio::MidiMsg* messages, size_t count, float* const out[2],
                     int channels, int frames, int sr, RackState& state, const plugin_audio::Transport& transport);

}  // namespace patch
