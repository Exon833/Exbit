// MIDI effects on notes played live, on the audio thread: the whole chain of a track's MIDI effects in
// order, including those that change timing (Arpeggiator, Note Length, Groove), timed by the audio
// callback's own clock, sample-accurately.
//
// THE GUI THREAD compiles the track's chain (compile(): every effect's settings as plain numbers and
// tables, an immutable Program) and hands it to the track's LiveNotes (live.h) with the keys played; the
// built-in synth and sampler get the audio of every pitch the key can sound rendered first (reach()).
// Chains without such effects keep the plain path (midi_effects::live on the GUI thread).
//
// THE AUDIO THREAD (Runner, one per track's LiveNotes): keys go in (key()), and run() works out, for one
// buffer, every note-on and note-off the effects make in it, at their frames, in time order. Each effect
// is a stage; a stage passes a note on at once (Pitch, Chord, Scale, Velocity, Random) or later (a Groove
// delays the off-beats, a Note Length ends notes when it says, the Arpeggiator plays the notes held on its
// grid while they are held). The beats come from the song: synced to the transport while it plays (the
// tempo map at the playhead), free-running at the song's tempo when stopped. Everything is preallocated:
// no allocation, lock or system call on the audio thread; queues and tables have fixed sizes (a note
// that doesn't fit is left out: never a note-on whose note-off could be lost).
//
// TIMING. A key reaches the audio thread at the start of the buffer after it was played (as every live
// note does); from there on everything is exact to the frame: the same notes, at the same frames (within
// one frame of rounding), as the clip with those keys as notes would play (test_live_fx), the song's and the
// track's swing included (applied first, as apply() does). Groove's humanizing is random each time (a clip's is
// seeded by its notes), and can only make notes later, never earlier than now. A chain without a timing effect
// plays keys as they come (the swing then isn't applied to them).
//
// NOTES are numbered (Ev::id): a note-off ends the very note its note-on started, even when the same pitch
// sounds twice (a long Note Length over an arpeggio). When the chain changes (another Program), every note
// sounding stops and the keys held are forgotten (pressed again, they play through the new chain).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/midi_effects.h"
#include "model/model.h"

namespace live_fx {

constexpr int MAX_STAGES = 16;   // effects in a chain (more: the rest are left out live)
constexpr int QUEUE = 1024;      // notes waiting in each stage
constexpr int TABLE = 512;       // notes each stage keeps track of
constexpr int SOUNDING = 2048;   // notes the chain has on at once
constexpr int MAX_OUTS = 4;      // a Chord's note and up to 3 more

// one effect, compiled (GUI thread); the song's, the track's and the rows' swing (midi_effects.h SWING) come first,
// as a Groove of their own, as apply() swings a clip's notes before its effects
struct Stage {
    enum Type { Pitch, Chord, Scale, Velocity, Random, Arp, Length, Groove };
    Type type = Pitch;
    int semitones = 0;                         // Pitch
    int intervals[MAX_OUTS - 1] = {}, count = 0;  // Chord: added notes (semitones)…
    double chordVelocity = 85;                 // …and their velocity (%)
    int16_t scaleMap[128] = {};                // Scale: pitch → the nearest in the scale
    std::vector<uint8_t> velocityMap;          // Velocity: [pitch * 128 + velocity] → velocity
    double chance = 0, range = 0;              // Random
    double stepBeats = 0.25, gate = 80;        // Arpeggiator: its grid step, gate (%)…
    int octaves = 1, style = 0;                // …octaves, style (midi_effects::STYLES)
    bool share = false;                        // Note Length: a share of each note's own length (else beats)
    double beats = 0.25, sharePct = 100;
    double grid = 0.25, swing = 0.6, accent = 0, humanizeS = 0, humanizeVelocity = 0;  // Groove
    bool rowOwn[128] = {};                     // Groove: rows with a swing of their own (not swung by it)
    bool perPitch = false;                     // Groove as the song's / track's / rows' swing: each pitch its own…
    double pitchSwing[128] = {};               // …share of the pair (0.5: straight)
};

struct Program {
    uint64_t serial = 0;  // (each compiled one its own: the Runner sees a new chain by it)
    std::vector<Stage> stages;
};

// does the track's chain change the timing of notes (an Arpeggiator, Note Length or Groove switched on)?
bool timed(const Track& track);
// the track's MIDI effects switched on, compiled (GUI thread)
std::shared_ptr<const Program> compile(const Track& track);
// every pitch a key of `pitch` can sound through the program (out[p] set)
void reach(const Program& program, int pitch, bool out[128]);

// a note-on or note-off: at frame `at` (the Runner's clock: frames since it started)
struct Ev {
    int64_t at = 0;
    uint32_t id = 0;
    int16_t pitch = 0, velocity = 0;
    bool on = false;
};

// what run() makes is handed to emit(ctx, event), in time order (GUI-free, no allocation)
using Emit = void (*)(void* ctx, const Ev& e);

class Runner {
public:
    Runner();
    // (audio thread) the chain to use: another one than before stops every note sounding (offs at `now`)
    void use(const Program& program, int64_t now, Emit emit, void* ctx);
    // (audio thread) a key: on (velocity 1…127) or off, at frame `at`; offAt >= 0: it is let go then by itself
    void key(bool on, int pitch, int velocity, int64_t at, int64_t offAt = -1);
    // (audio thread) the notes of frames [now, now + frames), beats beat0 + (frame - now) * beatsPerFrame, at
    // sample rate sr
    void run(int64_t now, int frames, double beat0, double beatsPerFrame, int sr, Emit emit, void* ctx);
    // every note sounding stops (offs at `at`) and everything is forgotten
    void flush(int64_t at, Emit emit, void* ctx);
    void reset();       // everything forgotten, without note-offs
    bool busy() const;  // keys held, notes waiting or sounding, an arpeggio running
    int sounding() const { return soundingCount_; }

private:
    struct Entry {
        uint32_t id = 0;
        int16_t pitch = 0, velocity = 0;
        int64_t at = 0, delay = 0;
        int n = 0;
        uint32_t outId[MAX_OUTS] = {};
        int16_t outPitch[MAX_OUTS] = {};
    };
    struct Slot {  // a stage's state
        std::vector<Entry> table;  // (TABLE, in order of arrival)
        int tableCount = 0;
        std::vector<Ev> queue;     // notes waiting to go INTO this stage (QUEUE, in order of scheduling)
        int queueCount = 0;
        bool running = false;      // an Arpeggiator stepping
        int64_t lastStep = INT64_MIN, index = 0;
    };
    void deliver(int stage, const Ev& e);
    void out(const Ev& e);
    void schedule(int stage, const Ev& e);
    int find(Slot& s, uint32_t id) const;
    void removeAt(Slot& s, int i);
    bool addEntry(Slot& s, const Entry& e);
    int64_t nextStep(int stage) const;
    void step(int stage, int64_t at);
    double beatAt(int64_t frame) const { return beat0_ + (double)(frame - now_) * bpf_; }
    int64_t framesFor(double beats) const;
    uint32_t newId() { return ++lastId_ ? lastId_ : ++lastId_; }

    const Program* program_ = nullptr;
    uint64_t serial_ = 0;
    std::vector<Slot> slots_;  // MAX_STAGES + 1 (the last: what goes out late)
    uint32_t keyId_[128] = {};
    std::vector<uint32_t> soundingId_;
    std::vector<int16_t> soundingPitch_;
    int soundingCount_ = 0;
    std::vector<std::pair<int16_t, int16_t>> pool_;  // (an arpeggio's notes: pitch, velocity)
    std::vector<Entry> held_;
    midi_effects::NumpyRandom rng_, arpRng_, arpRngStart_;
    uint32_t lastId_ = 0;
    // during run()
    Emit emit_ = nullptr;
    void* ctx_ = nullptr;
    int64_t now_ = 0, cursor_ = 0;
    double beat0_ = 0, bpf_ = 0;
    int sr_ = 48000;
};

}  // namespace live_fx
