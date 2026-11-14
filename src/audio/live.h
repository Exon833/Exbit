// Notes played live into a track (the MIDI editor's keys and notes, a MIDI keyboard), heard through
// the track itself: its VST instrument or the built-in synth or sampler, then its effects, volume and
// outputs (live.py).
//
// The GUI thread calls noteOn / noteOff; the audio thread (mixer) takes what arrived once per
// buffer. A lock-free single-producer/single-consumer ring hands them over, so no lock is needed.
// For the built-in synth and sampler the GUI thread renders the note (synth::renderNote, or the
// track's NoteRender: sampler::noteRenderer) and the audio thread only copies it in, fading it out
// quickly when the note is let go. A note held down (no length given) is rendered HOLD_S long and its
// last LOOP_S repeat seamlessly until it is let go (loopable()); a sampler that doesn't loop plays the
// sample to its end instead, and a one-shot sampler note plays to its end whether the key is let go or
// not. A finished note's audio is handed back to the GUI thread to be freed (collect(), also done by
// noteOn/noteOff).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "audio/plugin_audio.h"

namespace live {

constexpr double HOLD_S = 4.0;       // a built-in note held down is rendered this long; its last LOOP_S then repeat
constexpr double LOOP_S = 1.0;       // (about: a whole number of its wave's periods, when known)
constexpr double LOOP_FADE_S = 0.05; // the loop's end crossfades into what comes before its start: no click
constexpr double WHOLE_S = 600.0;    // a non-looping sample held down plays to its end (at most this long)

using NoteAudio = std::shared_ptr<const std::vector<float>>;  // interleaved, 1 (synth) or 2 (sampler) channels

// How a built-in note is rendered (sampler::noteRenderer; empty: the built-in synth, mono).
struct NoteRender {
    // (pitch, velocity, duration s, sr) -> the note with its release, `channels` interleaved
    std::function<std::vector<float>(int, int, double, int)> render;
    int channels = 1;
    bool oneShot = false;   // plays to its end, letting go of the key or not
    bool sustains = true;   // a held note loops (else: it plays to its end, at most WHOLE_S)
    std::function<double(int, int)> period;  // (pitch, sr) -> frames its sound repeats after (< 0: none); null: the synth's
};

// (audio, loop start or -1) of a held note: `audio` up to `hold` frames (its release left out: the voice
// fades itself out), its last part made to repeat seamlessly from the loop start. period (> 1): frames
// after which the sound repeats, so the loop is a whole number of them.
std::vector<float> loopable(const std::vector<float>& audio, int channels, int64_t hold, int sr, double period,
                            int64_t& loopStart);
// (audio, loop start or -1) of a built-in note played live: lengthS long (>= 0, with its release), a
// one-shot sample to its end, or held until let go (looped)
std::vector<float> builtInNote(const NoteRender& render, int pitch, int velocity, double lengthS, int sr,
                               int64_t& loopStart);

// A built-in synth or sampler note being played live (audio thread).
struct LiveVoice {
    int pitch = 0;
    NoteAudio data;
    int channels = 1;
    int64_t pos = 0;
    int64_t releasedAt = -1;  // position where it was let go: it fades out from there (-1: held)
    int64_t loopStart = -1;   // a held note: from its end it goes on from here, until let go
    int64_t length() const { return data ? (int64_t)data->size() / std::max(1, channels) : 0; }
    // add the next n frames into `channels` planar channels; false once it has finished
    bool addTo(float* const signal[2], int signalChannels, int n, int fade);
};

// Live notes for one track. GUI thread: noteOn, noteOff, collect. Audio thread: take, addVoices,
// active, holding, clear.
class LiveNotes {
public:
    explicit LiveNotes(size_t capacity = 1024);

    // --- GUI thread ---
    // Start a note; it stops at noteOff, or after lengthS (>= 0) if given. builtin: the track plays
    // the built-in synth or sampler (else its VST instrument), rendered by `render` (null: the one
    // setRenderer() gave; Engine::liveNotes sets the track's). False if the hand-over ring is full.
    bool noteOn(int pitch, int velocity, int sr, bool builtin, double lengthS = -1, const NoteRender* render = nullptr);
    bool noteOff(int pitch);  // (a one-shot sampler note goes on to its end: no note-off for it)
    void setRenderer(NoteRender render) { renderer_ = std::move(render); }
    void collect();  // free the audio of notes that have finished

    // --- audio thread ---
    bool active() const;   // something to play or arriving
    bool holding() const;  // is a note being played live into the instrument (held, or to stop by itself)?
    void clear();
    // What arrived since the last buffer: MIDI messages for a VST instrument, appended to `out`
    // (built-in synth notes become voices instead). Notes that stop by themselves stop within this buffer.
    void take(int frames, int sr, plugin_audio::Messages& out);
    // mix the built-in synth's live notes into signal (`channels` planar channels)
    void addVoices(float* const signal[2], int channels, int frames, int sr);

private:
    struct Item {
        bool on = false;
        int pitch = 0, velocity = 0;
        NoteAudio audio;
        int channels = 1;
        int64_t loopStart = -1;
        int64_t length = -1;  // frames; -1 = until noteOff
    };
    plugin_audio::MidiMsg release(int pitch, int64_t atFrame);
    void retire(NoteAudio&& audio);  // hand back to the GUI thread

    std::vector<Item> inbox_;
    std::atomic<uint64_t> inHead_{0}, inTail_{0};
    std::vector<NoteAudio> done_;  // finished notes' audio, for the GUI thread to free
    NoteRender renderer_;          // GUI thread
    std::map<int, int> oneShots_;  // GUI thread: pitch -> one-shot sampler notes of it playing to their end
    std::atomic<uint64_t> doneHead_{0}, doneTail_{0};
    // audio thread only
    std::vector<LiveVoice> voices_;
    std::vector<std::pair<int64_t, int>> timedOffs_;  // [frames left, pitch]: notes that stop by themselves
    int held_[128] = {};  // pitch -> notes of it sent to a VST instrument and not let go yet
    int heldTotal_ = 0;
};

using LiveMap = std::vector<std::pair<const Track*, std::shared_ptr<LiveNotes>>>;
using LiveMapPtr = std::shared_ptr<const LiveMap>;
// the LiveNotes of a track in a map, or null
LiveNotes* find(const LiveMap* map, const Track* track);

}  // namespace live
