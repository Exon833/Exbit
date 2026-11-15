// Time-stretching and pitch-shifting audio clips (audio/stretch.py): longer or shorter without
// changing their pitch, and higher or lower without changing their length.
//
// A phase vocoder with identity phase locking (Laroche & Dolson): the audio is cut into overlapping
// windowed frames (N_FFT long), read HOP / factor apart and written HOP apart; each frequency's phase
// is carried on at the speed it was turning, and the bins around each spectral peak keep their phase
// relation to it, which keeps notes clear rather than phasey. Stereo is worked out on the sum of the
// channels, each channel keeping its own phase relation to it, so the stereo image holds. Pitch is
// shifted by stretching by the pitch ratio too and then playing back that much faster.
//
// Clips are stretched when they are prepared (AudioClip::prepare) and the result is kept
// (stretched()), so split pieces and copies of a clip share it, and only a new setting or sample rate
// renders again. The audio's sample rate doesn't matter: a clip is stretched at its own rate and
// resampled to the engine's afterwards (model.py: resample(stretched(source, ...), source_sr, sr)).
#pragma once
#include <cstdint>
#include <vector>

#include "audio/dsp.h"

namespace stretch {

constexpr int N_FFT = 2048;
constexpr int HOP = 512;   // frames between synthesis frames (a quarter of N_FFT)
constexpr int CACHE = 16;  // stretched renderings kept

// --- on interleaved float audio of any channel count (frames * channels samples)
// `in` read evenly into `length` frames (linear interpolation); unchanged if length == frames or frames < 2
std::vector<float> resampleTo(const float* in, int64_t frames, int channels, int64_t length);
// `factor` times as long (2: half speed), at the same pitch; unchanged if factor is 1 or there's no audio
std::vector<float> timeStretch(const float* in, int64_t frames, int channels, double factor);
// `factor` times as long, `semitones` higher (or lower; cents are fractions), each without changing the other
std::vector<float> stretch(const float* in, int64_t frames, int channels, double factor, double semitones);

// --- on clips' audio (AudioBuffer: stereo)
// stretch() of `src` (and played backwards if `reverse`); `src` itself when nothing changes. Not kept.
AudioPtr process(const AudioPtr& src, double factor, double semitones, bool reverse = false);
// the same, kept (the last CACHE renderings) for the same audio and settings: split pieces of a clip
// share their rendering. Thread-safe.
AudioPtr stretched(const AudioPtr& src, double factor, double semitones, bool reverse = false);

}  // namespace stretch
