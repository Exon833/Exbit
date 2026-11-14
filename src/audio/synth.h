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
};

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
// stereo audio of all notes
AudioPtr render(const std::vector<Note>& notes, int sr, double lengthS);
}  // namespace synth
