// Small audio helpers (dsp.py) and the shared audio buffer type. Audio is float32 interleaved stereo.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Stereo audio, interleaved (L R L R …): what clips play from. Shared between split clips.
struct AudioBuffer {
    std::vector<float> samples;  // frames * 2
    int64_t frames() const { return (int64_t)(samples.size() / 2); }
    float* frame(int64_t i) { return samples.data() + i * 2; }
    const float* frame(int64_t i) const { return samples.data() + i * 2; }
};
using AudioPtr = std::shared_ptr<const AudioBuffer>;

// waveform min/max of the mono mix per PEAK_BLOCK samples, for drawing
struct Peaks {
    std::vector<float> mins, maxs;
    size_t size() const { return mins.size(); }
};
using PeaksPtr = std::shared_ptr<const Peaks>;

namespace dsp {
// duplicate mono to two channels, or keep the first two of a multichannel file
std::shared_ptr<AudioBuffer> toStereo(const float* interleaved, size_t frames, int channels);
// linear-interpolation resampling (as the Python program does)
AudioPtr resample(const AudioPtr& data, int srcSr, int dstSr);
PeaksPtr waveformPeaks(const AudioBuffer& data, int block);
inline double dbToGain(double db, double minDb) { return db <= minDb ? 0.0 : std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) { return 20.0 * std::log10(std::max(gain, 1e-6)); }
}  // namespace dsp
