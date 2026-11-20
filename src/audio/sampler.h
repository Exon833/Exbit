// The built-in sampler: a MIDI track playing one sample across the keyboard (Ableton's Simpler),
// audio/sampler.py. No GUI.
//
// A track has one when its instrument is "Built-in sampler" (Track::sampler, a SamplerSettings, replaced
// on every change so undo and the audio thread always see a whole one). Like the built-in synth, it
// renders a MIDI clip's notes ahead of time (Clip::prepare → clip_hooks::renderSampler = renderNotes())
// and each live note when it is played (noteRenderer(), live.h), so playing it costs what an audio clip
// does; freezes and bounces hear it too.
//
// A note plays the sample faster or slower so that `root` sounds at its own pitch (+ `transpose`
// semitones), from `start` to `end` (fractions of the sample), with an attack / decay / sustain /
// release envelope and velocity sensitivity. "One-shot" plays to the end of the sample however long the
// note is (drums); otherwise the note's length decides, and with `loop` the part between start and end
// repeats while the note is held.
//
// SIMPLER'S LEVEL (all off to start with: old settings sound exactly as they did, bit for bit):
//   MODES     Classic (keys repitch it; with `loop` it loops from loopStart to end while held), One-shot (plays to
//             the end whatever the note's length, no loop) and Slice: the part played cut at its transients
//             (dsp::Onsets, at `sensitivity`) or on a 1/4…1/32 grid at its tempo (`bpm`; padkit::sliceStarts /
//             gridStarts), slice i on key root + i, each played to its end at the sample's own pitch (+ transpose).
//   REVERSE and NORMALISE change the sound played (soundAt): start, end and the loop mark parts of it as played.
//   FILTER    low-pass, high-pass (12 or 24 dB), band-pass or notch: effects/svf.h's state-variable filter, per voice,
//             its cutoff worked out every 16 frames (gliding in between, so it never clicks) from the cutoff, key
//             tracking (octaves per octave from root), its own ADSR times its amount in octaves, and the LFO.
//   LFO       effects/svf.h's shapes, a rate in Hz or a length in beats at the song's tempo, restarting with each
//             note; moving the pitch (±12 semitones at full depth), the filter (±4 octaves), the volume or the pan.
//   GLIDE     each note slides from the pitch of the one before it over `glide` seconds; MONO: a note starting while
//             another sounds takes its voice over (the sample and the envelope go on), live: a new key lets go of
//             the others. Played live, a note glides from the last key played on the track (live.h glideFrom).
//   WARP      the sample, at its own tempo `bpm`, is rendered at the song's (warp.h, on every core: a rendering per
//             pitch, kept), so a key changes its pitch, not its length.
// Notes are rendered as they always were, ahead of time (a clip's, on the worker that prepares it; a live note's
// on the GUI thread): the audio thread only copies them, so it never allocates. The song's tempo is its first
// bar's (clip_hooks::currentTempoMap); shortcut: tempo changes later in the song don't move the LFO or warp.
//
// GUI thread (sampleAt keeps the resampled samples in use; it locks, so any thread may call it).
#pragma once
#include <string>
#include <vector>

#include "audio/live.h"
#include "model/model.h"

namespace sampler {

extern const char* const NOTE_NAMES[12];
constexpr double MAX_TAIL_S = 2.0;  // a clip's rendering goes on this long at most after its end (a release, a one-shot's end)
constexpr double EDGE_S = 0.003;    // fade at a note's very end (one-shot samples, cut tails), so nothing clicks
constexpr int MAX_SLICES = 64;      // Slice mode: at most this many slices (the rest stay in the last)

std::string noteName(int pitch);                // 60 -> "C4"
int rootFromName(const std::string& name);      // "Piano C4.wav" -> 60, "flute_A#3" -> 58; else 60

// the sample at rate sr (stereo), kept for the samples in use; null without one
AudioPtr sampleAt(const SamplerSettings& settings, int sr);
// the sound a note plays (stereo, at rate sr): the sample, reversed, normalised, warped to the song's tempo and
// pitched `semitones` (warp only) as the settings say; kept. null without a sample
AudioPtr soundAt(const SamplerSettings& s, int sr, double semitones = 0);
double songBpm();  // the song's tempo (its first bar's), for the LFO's sync and warping
// the level over n frames of a note held for `held` frames: attack, decay to the sustain level, release (not
// one-shot)
std::vector<float> adsr(int64_t n, int64_t held, double attack, double decay, double sustain, double release,
                        bool oneShot, int sr);
std::vector<float> envelope(int64_t n, int64_t held, const SamplerSettings& s, int sr);  // (the amp's)
double lfoHz(const SamplerSettings& s);  // the LFO's rate now (synced: at the song's tempo)
// Slice mode: the frames of soundAt(s, sr, s.transpose) where the slices start, in order (kept)
std::vector<int64_t> sliceStarts(const SamplerSettings& s, int sr);
// does its sound change with the song's tempo (warp, a synced LFO, a slice grid)? Its clips are rendered again then
bool followsTempo(const SamplerSettings& s);
// One note, including its release (or to the sample's end, one-shot), stereo interleaved. tailS >= 0: the
// most it may go on after the note (< 0: as long as it sounds). cents: its fine tuning (Note::fine). fromPitch
// >= 0: it glides from that pitch over the settings' glide.
std::vector<float> renderNote(const SamplerSettings& s, int pitch, double velocity, double durationS, int sr,
                              double tailS = -1, double cents = 0, double fromPitch = -1);
// Stereo audio of all notes, as long as the clip plus the releases (at most MAX_TAIL_S), as synth::render is:
// each note panned and fine-tuned by its own properties, and a sliding note gliding from the previous note's
// pitch (taking its voice over while it still sounds: the sample plays on, its speed gliding).
AudioPtr render(const SamplerSettings& s, const std::vector<Note>& notes, int sr, double lengthS);
// frames after which a looping sample's note repeats (its loop at the note's pitch); < 0: none
double loopPeriod(const SamplerSettings& s, int pitch, int sr);

// A MIDI clip's audio: through its track's Pad Kit (Track::playsPadKit: audio/padkit.h) or sampler
// (Track::playsSampler), or the built-in synth.
AudioPtr renderNotes(const Track& track, const std::vector<Note>& notes, int sr, double lengthS);
// how a live note on the track is rendered: its Pad Kit's or sampler's, or (empty) the built-in synth
live::NoteRender noteRenderer(const Track& track);

// set clip_hooks::renderSampler (once, at start-up; Engine's constructor does it): the Pad Kit or sampler
void install();

}  // namespace sampler
