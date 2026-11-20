// True peak, as ITU-R BS.1770-4 Annex 2 measures it: the signal 4× oversampled, so a peak falling between two
// samples (which a DAC's reconstruction, or a lossy encoder, will make) is seen, not only the samples' own peaks.
//
// The oversampling is a polyphase FIR interpolator: phase p (0…3) is a windowed-sinc fractional delay of p/4 of a
// sample (TAPS taps, Blackman window, each phase normalised to unity gain at DC); phase 0 is the sample itself. The
// loudest absolute value of all phases of all channels since reset() is kept.
#pragma once
#include <array>
#include <vector>

namespace audio {

class TruePeak {
public:
    static constexpr int FACTOR = 4;  // BS.1770-4: 4× at 48 kHz
    static constexpr int TAPS = 16;   // per phase (input samples each interpolated value looks at)
    explicit TruePeak(int channels = 2);
    void reset();  // forget the peak and the samples held
    // `channels` floats per frame (a narrower stream: its channels only)
    void add(const float* interleaved, int frames, int width);
    void addSilence(int frames);  // (silence moves the history on)
    double peak() const { return peak_; }  // linear (1.0 = 0 dBTP); 0: nothing loud yet
    double peakDb() const;                 // dBTP; -inf: silent so far
    // the interpolator's coefficients: phase p's TAPS taps (for sample n - TAPS/2 + 1 + j, oldest first)
    static const std::array<std::array<float, TAPS>, FACTOR>& coefficients();

private:
    int channels_;
    std::vector<float> history_;  // per channel, a ring of the last TAPS samples
    int pos_ = 0;                 // where the next sample goes
    double peak_ = 0;
};

}  // namespace audio
