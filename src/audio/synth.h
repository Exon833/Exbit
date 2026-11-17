// A tiny built-in instrument so MIDI lanes make sound (synth.py): notes are rendered to audio
// ahead of time, so playing them costs the same as an audio clip.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/dsp.h"

struct Note {
    double start = 0, end = 0;  // seconds, relative to the clip's start
    double pitch = 60;
    double velocity = 100;
    // --- the note's own properties (the MIDI editor's lane chooser, and Slide) ---
    // Saved only when they aren't their defaults (project_io), so an unchanged note is saved as it always was.
    double chance = 100;  // % of the times it plays (decided the same way every time: note_props::sounds)
    int repeats = 1;      // played as so many equal hits, evenly spaced across its length (a roll), 1…8
    bool slide = false;   // its pitch glides from the previous note's (note_props::slides)
    double pan = 0;       // -1 left … 1 right (the built-in synth and sampler only)
    double fine = 0;      // cents, -100…100 (the built-in synth and sampler only)
    bool plain() const { return chance >= 100 && repeats <= 1 && !slide && pan == 0 && fine == 0; }
};

// What a note's own properties do when it is played.
//
// A clip's notes go through perform() when they become the notes heard (Clip::performed): after a loop's
// passes are laid out and BEFORE the track's MIDI effects, so an arpeggiator or a chord works on what is
// actually played, and a switched-off note (Clip::mutedNotes) is never even considered. The instruments
// do the rest: the built-in synth and sampler glide (slides()), pan and fine-tune each note; a VST
// instrument gets a sliding note's note-on before the previous note's note-off (legato, so a mono synth
// with portamento glides on its own), and no pan or fine tuning (there is no per-note expression here).
namespace note_props {
constexpr int MAX_REPEATS = 8;
constexpr double GLIDE_S = 0.06;    // how long a slide takes to reach its note's pitch
constexpr double LEGATO_S = 1e-4;   // a note ending this close before a sliding note still hands its voice over
// Does the note play in loop pass `pass`? Its chance is decided by a hash of the note (its start and
// pitch) and the pass: the same every time the clip is prepared (a render sounds as playback does),
// different from one pass of a loop to the next. Seeded by the note itself rather than by its place in
// the list, so adding a note elsewhere doesn't change whether the others play.
bool sounds(const Note& n, int64_t pass);
// the note as played: nothing (its chance said no), or its hits (repeats), appended to `out`. Only the
// first hit slides.
void perform(const Note& n, int64_t pass, std::vector<Note>& out);
// For each note: the note it slides from (-1: none, or it doesn't slide) — the one starting last before
// it (the higher of two starting together) — and whether it takes that note's voice over (legato: that
// note still sounds when it starts, and no other sliding note took it already).
struct Slide {
    int from = -1;
    bool legato = false;
};
std::vector<Slide> slides(const std::vector<Note>& notes);
// a balance control's (left, right) factors, as the track's pan (mixer::panGains): 1 and 1 in the middle
void panGains(double pan, double& left, double& right);
}  // namespace note_props

namespace synth {
constexpr double ATTACK_S = 0.005;
constexpr double RELEASE_S = 0.08;
constexpr double NOTE_GAIN = 0.15;
constexpr size_t KEPT_BYTES = size_t(64) << 20;  // each pitch's wave is kept for the next notes (an edit is quick)
constexpr double KEPT_LONGEST_S = 20.0;          // (longer notes: their wave is worked out each time)
double noteFrequency(double pitch);
// the note's wave (its harmonics added up, before the envelope), at least n frames: worked out once per
// pitch and kept (KEPT_BYTES). Any thread.
std::shared_ptr<const std::vector<double>> waveOf(double pitch, int64_t n, int sr);
// one note including its release tail, mono
std::vector<float> renderNote(double pitch, double velocity, double durationS, int sr);
// Stereo audio of all notes, each panned and fine-tuned by its own properties. A sliding note glides from
// the previous note's pitch over note_props::GLIDE_S; when that note still sounds, the sliding note carries
// its voice on (one voice: no new attack, the wave's phase unbroken, a mono / legato glide).
AudioPtr render(const std::vector<Note>& notes, int sr, double lengthS);
}  // namespace synth
