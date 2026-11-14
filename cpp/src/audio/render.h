// Rendering offline, on the GUI thread (render.py): a track, for freezing and bouncing it
// (renderTrack), and the whole mix, for File → Render selection / Render project (renderMix).
//
// A track renders the way it plays: its clips, through its VST instrument on a MIDI track (else
// the built-in synth's audio), then its effects, with their automation. Not included: mute, solo,
// its group, the master volume and master effects. The rendering runs from the track's first clip
// to its last clip's end, plus up to RENDER_TAIL_S while plugins may still ring out (cut once the
// sound has died away).
//
// Plugins with look-ahead make the track come out late (plugin_audio::latency): the rendering is
// moved back by that much, so it plays in time without them.
//
// Its sidechains hear the other tracks they listen to as they play (their effects and fader; a
// frozen one its rendering), even muted: those are rendered first (sidechainSources).
//
// A track's fader, pan (and their automation), output, delay and sends are not part of its rendering:
// a frozen track still plays through them, and a bounce takes them over (keepMix).
//
// Renders call plugins from this (the main) thread, so the audio thread must leave them alone
// meanwhile: hold an Engine::PluginsPaused for the whole render. They are reset first, so a
// rendering never starts with what they were playing live, and afterwards, so playback never hears a
// rendering's reverb and delay tails. Tempo-synced plugins follow the song's tempo map, as they do
// playing. Afterwards the patch states the audio thread uses are fresh (clearPatchStates), so neither
// hears the other's sound.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "audio/plugin_audio.h"
#include "model/model.h"

namespace render {

// called now and then with the fraction done; return false to stop the rendering
using Progress = std::function<bool(double)>;

struct Rendered {
    int64_t start = 0;         // timeline frame
    std::vector<float> audio;  // stereo, interleaved
    int64_t frames() const { return (int64_t)(audio.size() / 2); }
};

// The track rendered: (start frame, stereo audio), or nothing if it has no clips that play (disabled
// clips are left out) or progress said stop. Throws std::runtime_error if its instrument fails
// ("<name> stopped: <error>"). seen: tracks being rendered already (a sidechain loop counts once).
std::optional<Rendered> renderTrack(Project& project, const TrackPtr& track, int sr, const Progress& progress = nullptr,
                                    const plugin_audio::Seen* seen = nullptr);

// {track: (start frame, its sound)} for the other tracks the track's sidechains listen to,
// rendered as they play, in time (their own delay already taken out).
std::vector<std::pair<const Track*, Rendered>> sidechainSources(Project& project, const Track& track, int sr,
                                                                const plugin_audio::Seen* seen);

// Stereo audio (interleaved) of the whole mix from frame `start` to `end`, the way it plays: every
// audible track (mute and solo count) through its instrument, effects and volume, with plugin delay
// compensation, then the master effects and the master volume (masterDb). Tracks playing on other
// outputs than 1/2 are mixed in too. Not included: the metronome and the Explorer's preview.
// It starts RENDER_PREROLL_S before `start`, so effects such as reverb and delay are already ringing
// there. tail=true renders past `end` while plugins still ring out (up to RENDER_TAIL_S, cut where
// the sound has died away). Nothing if progress said stop.
// only: one track's own sound (a stem; mixer::mixClips' `only`), without the master effects (masterDb still
// applies: pass 0 for none).
std::optional<std::vector<float>> renderMix(Project& project, int64_t start, int64_t end, int sr, double masterDb = 0.0,
                                            bool tail = false, const Progress& progress = nullptr,
                                            const Track* only = nullptr);

// A bounce's new track keeps the original's mix: fader, pan, output, track delay, sends, and its
// volume / pan automation (re-pointed to `to`'s controls). track_render.py.
void keepMix(const Track& from, Track& to);

// Forget what the effects' cables held back (fresh patch states for every track and the master).
void clearPatchStates(Project& project);

}  // namespace render
