// The audio-thread half of plugins.py: how the mixer, the patch and renders use plugins
// (plug::Plugin, plugins/plugin.h). Loading plugins, their library and their windows are elsewhere.
//
//   - Effects get a stereo input, plus their sidechain input when they have one (hasSidechain();
//     audio/patch feeds it from its cables). Instruments get the notes of each buffer, timed to the
//     frame; hanging notes are ended with note-offs for every note still held (notesOff()).
//   - A plugin with look-ahead reports how late it is (latency(), in frames); its output is that
//     much behind its input, and the mixer delays every other track to match.
//   - A plugin that throws while processing is switched off: slot.failed (and slot.error) are set,
//     and it is skipped from then on, never the whole audio stream.
//
// Threads: the audio thread reads `slot.instance` with std::atomic_load, so the GUI should replace
// it with std::atomic_store (or only while the audio thread leaves plugins alone).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model/model.h"
#include "plugins/plugin.h"

// an atomic snapshot of a shared_ptr field another thread may replace
template <class T>
inline std::shared_ptr<T> aload(const std::shared_ptr<T>& p) { return std::atomic_load(&p); }

namespace plugin_audio {

constexpr const char* INSTRUMENT = "instrument";
constexpr const char* EFFECT = "effect";

// A MIDI message for an instrument, timed in frames from the buffer's start (the Python program's
// mido messages timed in seconds). AllOff stands for its ALL_NOTES_OFF control changes: every note
// held ends. PitchBend and Control come from a MIDI keyboard played live (live.h): the bend wheel
// (value -8192..8191) and a control change (pitch: the control number, value 0..127).
struct MidiMsg {
    enum Type { NoteOn, NoteOff, AllOff, PitchBend, Control };
    Type type = NoteOn;
    int64_t frame = 0;
    int pitch = 0;
    int velocity = 0;
    int channel = 0;
    int value = 0;  // PitchBend, Control
};
using Messages = std::vector<MidiMsg>;

// what plugins are told about the transport before each buffer (plug::Plugin::setTransport)
struct Transport {
    double tempo = 120;
    int numerator = 4, denominator = 4;
    bool playing = false;
    int64_t position = 0;
    double beats = -1;  // the song's beats at `position` (through tempo changes and ramps; < 0: none)
    // the tempo and time signature where timeline frame `start` is (the tempo map: inside a ramp, the tempo at
    // that frame), playing from there
    static Transport at(const TempoMap& map, bool playing, int64_t start, int sr);
};

// One of a plugin's parameters, as automation and the GUI use it: its value 0…1 (rawValue; set from
// either thread, read on the main thread), the plugin's text for a value, and its steps (a switch or
// a choice: only so many values; VST2 plugins don't say, so theirs take any value).
struct Parameter {
    std::shared_ptr<plug::Plugin> plugin;
    uint32_t id = 0;
    std::string name;
    int steps = 0;  // 0: any value; n: n + 1 values (k / n), e.g. 1: Off / On
    Parameter() = default;
    Parameter(std::shared_ptr<plug::Plugin> p, const plug::ParamInfo& info)
        : plugin(std::move(p)), id(info.id), name(info.name), steps(std::max(0, info.stepCount)) {}
    double rawValue() const { return plugin->getParam(id); }
    void setRawValue(double v) const { plugin->setParam(id, v); }
    std::string stringValue() const { return plugin->paramText(id, rawValue()); }
    std::string textFor(double v) const { return plugin->paramText(id, v); }
};

// Have the plugin take in the latest changes made in its window before its state is saved: they
// reach its processor with the next buffer it processes. The audio thread processes plugins all the
// time while a device is open; without one, this processes one silent buffer (main thread only,
// never while the audio thread uses the plugin).
void syncState(PluginSlot& slot, int sr, int frames = 512);

// --- audio thread -----------------------------------------------------------------------------

bool usable(const PluginSlot* slot);  // loaded and not failed
inline bool usable(const SlotPtr& slot) { return usable(slot.get()); }
bool active(const PluginSlot* slot);  // usable and not bypassed
int latency(const PluginSlot& slot);  // frames a plugin holds the sound back, as it reports it
// get the plugin ready for sample rate sr if it isn't (the device changed): it starts afresh
void ensureRate(plug::Plugin& plugin, int sr);
// mark a slot failed (set error before failed, so whoever sees `failed` can read it)
void fail(PluginSlot& slot, const char* what);
// why it failed, to say after its name ("<name> stopped: …"): its error without the name it may begin with
std::string whyStopped(const PluginSlot& slot);

// An effect's output for its input (2 channels) and sidechain (or null). False if it failed
// (slot.failed is set; out is left as it is).
bool processEffect(PluginSlot& slot, const float* const in[2], const float* const side[2], float* const out[2],
                   int frames, int sr, const Transport& transport);

// tracks whose latency is being worked out already (a stack list: a loop counts once)
struct Seen {
    const Track* track;
    const Seen* up;
    bool contains(const Track* t) const {
        for (const Seen* s = this; s; s = s->up)
            if (s->track == t) return true;
        return false;
    }
};
using TrackList = std::vector<TrackPtr>;
bool inList(const Track* t, const TrackList& tracks);

// Frames a track's plugins hold its sound back (0 while frozen: its rendering is compensated): its
// instrument, then the slowest path through its effects' cables (other tracks into sidechains
// included: a track listening to a late one is late too). `tracks`: the project's tracks (snapshot);
// cables from tracks not among them count as 0 late.
int trackLatency(const Track& track, const TrackList& tracks, const Seen* seen = nullptr);
// Frames late the sound reaching one of the track's effects is (its instrument and the effects before it): what
// its automation is held back by, to stay in time with that sound. An instrument (or anything else): 0.
int inputLatency(const Track& track, const PluginSlot* slot, const TrackList& tracks);

// Does a MIDI track play instrument plugins (its own, or its instrument patch panel's: even none
// wired in) rather than the built-in synth?
bool playsPlugins(const Track& track);
// Is this one of the instrument plugins of a track that are heard: its one instrument, or those of
// its instrument patch panel wired to OUT and not muted (bypass)? Only loaded ones that haven't failed.
bool isHeard(const Track& track, const PluginSlot* slot, const std::vector<Cable>& rackCables);
// the heard instruments (allocates: GUI thread and renders)
std::vector<SlotPtr> heardInstruments(const Track& track);
// Is it one of the instruments of a track's instrument patch panel that get its notes (wired from IN)?
bool isFed(const PluginSlot* slot, const std::vector<Cable>& rackCables);
// Frames its instruments hold the sound back: the latest heard one (the others are held back to match).
int instrumentLatency(const Track& track);

// scratch for playInstrument (keeps its capacity: no allocation once warmed up)
struct NoteScratch {
    std::vector<plug::NoteMessage> notes;
    std::vector<float> audio;
};
// Render one buffer of an instrument plugin and add it into out (`channels` of the 2 planar
// pointers). Any AllOff message ends every note held. False if it failed (slot.failed set).
bool playInstrument(PluginSlot& slot, const MidiMsg* messages, size_t count, float* const out[2], int channels,
                    int frames, int sr, NoteScratch& scratch, const Transport& transport);

// MIDI messages for the notes of `clips` that start or end in timeline frames [start, start+frames),
// timed from the buffer start, into `out` (cleared first). Notes are cut at the end of each clip's
// window. hasStart=false (transport stopped): no notes, only "all notes off" on a restart.
// `events` is scratch (no allocation once warmed up).
struct NoteEvent {
    int64_t t;
    int on;
    int pitch, velocity;
};
// held (128 counts, or null; kept by the caller between buffers while playing): the notes the clips turned
// on and haven't turned off yet. One that no clip plays on through this buffer's start any more (its clip
// was deleted, trimmed, moved or disabled while it sounded) is let go at once, so it doesn't hang on.
void noteMessages(const Clip* const* clips, size_t count, bool hasStart, int64_t start, int frames, bool restart,
                  Messages& out, std::vector<NoteEvent>& events, uint16_t* held = nullptr);
// does the clip play a note of `pitch` started before timeline frame `at` and not ended before it?
bool soundsOn(const Clip& clip, int pitch, int64_t at);
// the same, allocating (GUI thread, tests)
Messages noteMessages(const std::vector<ClipPtr>& clips, bool hasStart, int64_t start, int frames, bool restart);

}  // namespace plugin_audio
