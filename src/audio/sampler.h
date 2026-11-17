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

std::string noteName(int pitch);                // 60 -> "C4"
int rootFromName(const std::string& name);      // "Piano C4.wav" -> 60, "flute_A#3" -> 58; else 60

// the sample at rate sr (stereo), kept for the samples in use; null without one
AudioPtr sampleAt(const SamplerSettings& settings, int sr);
// the level over n frames of a note held for `held` frames: attack, decay to the sustain level, release
std::vector<float> envelope(int64_t n, int64_t held, const SamplerSettings& s, int sr);
// One note, including its release (or to the sample's end, one-shot), stereo interleaved. tailS >= 0: the
// most it may go on after the note (< 0: as long as it sounds). cents: its fine tuning (Note::fine).
std::vector<float> renderNote(const SamplerSettings& s, int pitch, double velocity, double durationS, int sr,
                              double tailS = -1, double cents = 0);
// Stereo audio of all notes, as long as the clip plus the releases (at most MAX_TAIL_S), as synth::render is:
// each note panned and fine-tuned by its own properties, and a sliding note gliding from the previous note's
// pitch (taking its voice over while it still sounds: the sample plays on, its speed gliding).
AudioPtr render(const SamplerSettings& s, const std::vector<Note>& notes, int sr, double lengthS);
// frames after which a looping sample's note repeats (its loop at the note's pitch); < 0: none
double loopPeriod(const SamplerSettings& s, int pitch, int sr);

// A MIDI clip's audio: through its track's sampler (Track::playsSampler), or the built-in synth.
AudioPtr renderNotes(const Track& track, const std::vector<Note>& notes, int sr, double lengthS);
// how a live note on the track is rendered: its sampler's, or (empty) the built-in synth
live::NoteRender noteRenderer(const Track& track);

// set clip_hooks::renderSampler (once, at start-up; Engine's constructor does it)
void install();

}  // namespace sampler
