// Small audio helpers (dsp.py) and the shared audio buffer type. Audio is float32 interleaved stereo.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// Stereo audio, interleaved (L R L R …): what clips play from. Shared between split clips.
struct AudioBuffer {
    std::vector<float> samples;  // frames * 2
    // a Pad Kit clip's rendering (model/pad_tracks.h): the sound of each pad playing on a track of its own, kept
    // apart from `samples` (the rest of the kit), each as many frames; (pad, its sound), by pad; empty otherwise
    std::vector<std::pair<int, std::shared_ptr<const AudioBuffer>>> pads;
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

// ONSETS: where the hits of a sound start (slicing a drum loop into a Pad Kit: audio/padkit.h). The spectral
// flux of its mono mix (how much louder each frequency became in each ONSET_N-frame window, ONSET_HOP frames
// apart, than in the loudest of the ONSET_LAG windows before it, on a log scale; per octave band, each
// against its own largest, the largest band counting) is worked out once; find() picks its peaks (weighed by
// how loud the sound is there) standing above the flux around them (sensitivity 0…1: higher finds quieter
// hits too), at least ONSET_GAP_S apart, and puts each on the frame where its sound starts rising (the
// first sample of the hit, not the window's centre).
constexpr int ONSET_N = 1024;
constexpr int ONSET_HOP = 256;
constexpr int ONSET_LAG = 3;
constexpr double ONSET_GAP_S = 0.03;
constexpr int ONSET_BANDS = 8;          // the flux is weighed per octave band (a kick's low thump counts as much
constexpr double ONSET_BAND_HZ = 150;   // as a hi-hat's hiss): the lowest band up to this, then octaves
class Onsets {
public:
    Onsets(const float* stereo, int64_t frames, int sr);  // interleaved stereo
    std::vector<int64_t> find(double sensitivity) const;  // frames, in order
    int64_t frames() const { return (int64_t)mono_.size(); }
private:
    std::vector<float> mono_;
    std::vector<double> flux_;  // per hop: the largest band's rise, against that band's largest (0…1)
    std::vector<double> level_; // per hop: the loudest sample of its window
    int sr_;
    int64_t attack(int64_t from, int64_t to) const;  // where the hit found between these frames starts
};
}  // namespace dsp
