// The Pad Kit (model.h PAD KIT): a MIDI track's built-in drum instrument, a sample on each of up to 32
// keys, and slicing an audio clip's hits into one. No GUI (its window: ui/pad_kit_dialog.h).
//
// Each pad plays through the built-in sampler's own code (audio/sampler.h): a clip's notes are split by
// pad and each pad's notes rendered by sampler::render with the pad's settings, the key changed to the
// pad's root (so the sample sounds at its own pitch, tuned by `transpose`); a live note by
// sampler::renderNote. So a pad sounds exactly as the sampler would with the same settings. A key plays
// the first pad on it that has a sample (pads sharing a key: the first). Like the sampler's, a clip's
// rendering is made ahead of time (Clip::prepare → clip_hooks::renderSampler → render()), so freezing,
// bouncing and exporting hear what playback plays.
//
// CHOKE GROUPS. A note of a pad in a choke group (1…8) cuts off every note of its group that started
// before it and still sounds: from its start, that note fades out over live::CHOKE_S (and is gone). Notes
// starting at the same moment don't cut each other. Played live, a key's note cuts off the group's notes
// sounding when it arrives (live.h), with the same fade.
//
// SLICING. slice() cuts an audio clip's sound (as it plays: its window, stretched, reversed or looped,
// with its gain and fades: clipSound()) at the hits dsp::Onsets found (or any frames given): a kit with
// one slice per pad, one-shot (at most MAX_SLICES: the rest stay in the last slice), and the notes playing
// each slice at the time it had, so the new clip replays the loop where it was (its hits can then be
// moved, rearranged, swapped, choked…). WAV cue markers aren't read (the sound file reader doesn't keep
// them): the hits are found in the sound.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "audio/live.h"
#include "model/model.h"

namespace padkit {

constexpr int MAX_SLICES = PadKit::MAX_PADS;
constexpr double DEFAULT_SENSITIVITY = 0.5;  // slicing's sensitivity to start with (0…1)
constexpr int SLICE_VELOCITY = 127;           // the notes a slicing makes play their slices at full level

// the name of a pad: its sample's file name without the extension ("" when empty)
std::string padName(const Pad& pad);
// the first pad with a sample on key `pitch`, or -1
int padFor(const PadKit& kit, int pitch);
// a pad playing `sample` on key `note`, as drums want it: one-shot, no attack (choke group none)
Pad newPad(const ClipPtr& sample, int note);
// a copy of the kit with pad `index` replaced
PadKitPtr withPad(const PadKit& kit, int index, const Pad& pad);
// an audio clip holding a sound file (named by its file name), or null and `error`
ClipPtr loadSample(const std::string& path, std::string& error);
// the kit with `paths` loaded onto consecutive pads from `first` (keeping each pad's key and choke group;
// files past the last pad are left out; those that can't be loaded are skipped, their messages appended to
// `errors`); null if none was loaded
PadKitPtr withFiles(const PadKit& kit, int first, const std::vector<std::string>& paths,
                    std::vector<std::string>& errors);

// Stereo audio of the notes through the kit, as long as the clip plus the longest tail its pads may have
// (sampler::MAX_TAIL_S at most), as sampler::render's is: notes on keys without a pad are silent.
// apart: pads (bit i: pad i) playing on tracks of their own (model/pad_tracks.h): their sound kept apart, in the
// result's `pads`, each as long as the rest; choke groups still cut across them. Every pad is rendered on its own
// (on every core when there are several) and the sum made in pad order, so the kit's sound is the same, bit for
// bit, as its pads' tracks summed in that order.
AudioPtr render(const PadKit& kit, const std::vector<Note>& notes, int sr, double lengthS, uint32_t apart = 0);
// how live notes play the kit: each key its pad (one-shot or held, its choke group)
live::NoteRender noteRenderer(const PadKitPtr& kit);

// --- slicing (see the top) ---
struct Sliced {
    PadKitPtr kit;                // a slice per pad, from pad 1 (key PadKit::FIRST_NOTE) on
    std::vector<Note> notes;      // a note per slice, at its time in the sound (seconds)
    double lengthS = 0;           // the sound's length
    std::vector<int64_t> starts;  // the frames the slices start at (the first may be 0: sound before the first hit)
};
// The sound (interleaved stereo at sr) cut at `hits` (frames; sorted here, duplicates and those outside
// left out). The sound before the first hit becomes a slice of its own unless it is silent. Slices are
// named "<name> slice N.wav".
Sliced slice(const float* stereo, int64_t frames, int sr, std::vector<int64_t> hits, const std::string& name);
// where slice() would start the slices (Sliced::starts), without making them; `most` of them (the slice window
// shows them all: MAX_SLICES become pads); top: the sound's loudest sample if known (< 0: found here)
std::vector<int64_t> sliceStarts(const float* stereo, int64_t frames, std::vector<int64_t> hits,
                                 int most = MAX_SLICES, float top = -1);
// The slice window's Grid: where a grid of 1/`division` notes (4: quarters … 32: 32nds) falls in an audio clip's
// window of `frames` frames (frames into it). At the clip's own tempo when it has one (sourceBpm, as stretched),
// counted from the start of its audio; else on the song's grid where the clip is (its tempo changes and time
// signatures: timing.h), sr: the engine's.
std::vector<int64_t> gridStarts(const Clip& clip, int64_t frames, const Timing& timing, int sr, int division);
// the hit (sorted frames) nearest `frame` within `reach` frames, else `frame` (a dragged line snapping to a hit)
int64_t snapToHit(int64_t frame, const std::vector<int64_t>& hits, int64_t reach);
// the sound of an audio clip as it plays (its window of its prepared audio, with its gain and fades):
// interleaved stereo at the rate it was prepared for
std::vector<float> clipSound(const Clip& clip);

}  // namespace padkit
