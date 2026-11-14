// Summing clips into an output buffer (mixer.py). Runs on the audio thread, so it doesn't allocate
// once its MixState has grown to the buffer size and the number of tracks.
//
// Plugin delay compensation: plugins with look-ahead (limiters, linear-phase EQs) make their track
// come out late. Every track is delayed so that all come out together with the latest one
// (DelayLine); outputs 3 and up are also delayed by the master effects' latency, which only
// outputs 1/2 go through. mixClips returns how far the output lags behind the playhead.
//
// Sidechains (audio/patch.h) can listen to other tracks: a track some sidechain listens to is mixed
// before the tracks (and the master) that listen to it, and its sound after its effects and fader is
// handed to them, as late as it is (before its delay compensation). It is worked out even while it
// is muted or not soloed (then only for the sidechains, not heard): a muted "ghost" kick can still
// make a compressor duck.
//
// A track's delay (Track::delayMs, −500…+500 ms) works through the same compensation: a track to play
// later counts as less late than it is, one to play earlier as later, and the others are held back to
// match.
//
// Return tracks are mixed last: each track's sound after its fader and pan (and its delay compensation)
// is added, at each send's level, into the input of the returns it sends to, which then play it through
// their effects, fader and pan. A muted (or not soloed) track sends nothing; soloing leaves the returns
// playing. A return's own effects' look-ahead isn't made up for (a return is held back only by its own
// positive delay).
//
// Each track's fader and pan (applyFader) come after its effects; automated (automation::mixerValues),
// they go in a straight line from their values at the buffer's start to those at its end. Pan is a
// balance: centre leaves both sides alone; towards one side the other goes down (cos), to silence at ±1.
//
// An audio clip's own gain and fades (Clip::level) apply before the track's effects (addClip).
//
// This is the one mixer: playback (Engine::callback) and renders (render.h) both run it, so a render
// is what playback sounds like, sample for sample, when buffers fall the same way.
#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "audio/live.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "model/model.h"

namespace mixer {

// told every track's sound in each buffer, so the engine can meter it
struct TrackTap {
    virtual ~TrackTap() = default;
    // silent in this buffer (no clips and no plugins that could still ring, muted, or not soloed)
    virtual void silent(const Track* track, int frames) = 0;
    // its sound: `channels` planar channels, on device outputs [first, first + channels)
    virtual void sound(const Track* track, const float* const ch[2], int channels, int first, int frames) = 0;
};

// What the caller keeps between buffers (the delay compensation's lines) and scratch.
struct MixState {
    bool compensate = true;  // plugin delay compensation (the Python program's delays dict; false = None)
    std::vector<std::pair<const Track*, patch::DelayLine>> delays;
    void reserve(int frames, int tracks);  // make room ahead (the engine does when the device opens)

    // scratch (no need to touch)
    int capacity = 0, trackCapacity = 0;
    std::vector<int> latency, order;
    std::vector<char> feeder, visit, listens;
    std::vector<Cables::Ptr> cables;
    struct Active {
        const Clip* clip;
        const Track* track;
    };
    std::vector<Active> active;
    std::vector<const Clip*> clips;
    std::vector<float> sig, master;
    std::vector<std::vector<float>> feedBuf, sentBuf;
    std::vector<char> sentUsed;
    std::vector<int> mixOrder;
    std::vector<std::pair<const Track*, std::array<uint16_t, 128>>> held;  // the clips' notes each track's instruments hold
    std::vector<patch::Feed> feeds;
    plugin_audio::Messages messages;
    std::vector<plugin_audio::NoteEvent> events;
};

// is a TrackSource of `track` among these cables (another track's sound into a sidechain)?
bool listensTo(const std::vector<Cable>& cables, const Track* track);
// The tracks (as indices into `tracks`), each after the ones its sidechains listen to (else in their
// own order); and which tracks some sidechain (or the master's) listens to. A loop of tracks
// listening to each other keeps its own order.
void feedingOrder(const plugin_audio::TrackList& tracks, const std::vector<Cable>& masterCables, MixState& st);

// frames a track plays later (its delay), at rate sr
int64_t delayFrames(const Track& track, int sr);
// (left, right) factors of a balance control (dsp.py's pan_gains)
void panGains(double pan, double& left, double& right);
// The track's volume and pan on `channels` planar channels at timeline frame `start` (hasStart false:
// stopped), gliding from their values at the buffer's start to those at its end while automated.
void applyFader(float* const signal[2], int channels, int frames, const Track& track, bool hasStart, int64_t start,
                int sr);

// Add every audible track's sound for frames [start, start + frames) into out (interleaved,
// outChannels per frame).
//
// Each track is summed on its own output pair (track.output): its clips' audio, or, on a MIDI
// track with a VST instrument, the instrument playing the clips' notes; then its effects, then its
// volume. A frozen track plays its rendering (track.frozen) instead, without calling its plugins.
// tap (may be null) is told each track's sound. hasStart=false: the transport is stopped, so only the
// plugins run (no clips). `restart`: playback just started, stopped or jumped; instruments get "all
// notes off" first. plugins=false: don't call any plugin (one is being processed elsewhere): tracks
// with a VST instrument are silent and effects are skipped. live: notes played live into tracks,
// heard through each track's instrument and effects, playing or not. Returns the frames the output
// lags behind `start` (0 without compensation).
// only: a track whose own sound alone goes into out (a stem: render::renderMix): the others still play into
// the returns and sidechains they feed, but aren't heard; mute and solo, and the master effects, are left out.
int mixClips(Project& project, float* out, int outChannels, int frames, bool hasStart, int64_t start, MixState& st,
             TrackTap* tap, int sr, bool restart = false, bool plugins = true, const live::LiveMap* live = nullptr,
             const Track* only = nullptr);

// Add the part of clip that falls in buf (`channels` planar channels of bufFrames frames, starting at
// timeline frame `start`), at its gain and fades (Clip::level), with the short fade at the clip's edges so
// cuts don't click.
void addClip(float* const buf[2], int channels, int64_t bufFrames, const Clip& clip, int64_t start);

}  // namespace mixer
