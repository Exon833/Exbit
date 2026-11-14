// Loudness in LUFS, following ITU-R BS.1770 / EBU R128 (loudness.py).
//
// The signal is K-weighted (a high shelf and a high-pass that model how loud we hear
// frequencies), then its power is collected in 100 ms blocks:
//   momentary   = last 400 ms
//   short-term  = last 3 s
//   integrated  = everything since reset, over 400 ms blocks, gated: blocks below -70 LUFS
//                 are ignored, then blocks more than 10 LU below the average of the rest.
// Every channel is weighted 1.0 (BS.1770's weights for left/right/centre).
#pragma once
#include <array>
#include <deque>
#include <mutex>
#include <vector>

namespace loudness {

constexpr double BLOCK_S = 0.1;
constexpr int MOMENTARY_BLOCKS = 4, SHORT_TERM_BLOCKS = 30;
constexpr double ABSOLUTE_GATE_LUFS = -70.0, RELATIVE_GATE_LU = -10.0;

// one second-order section, normalized (a0 = 1): b0 b1 b2 / a1 a2 (scipy's sos row is b0 b1 b2 1 a1 a2)
struct Biquad {
    double b0, b1, b2, a1, a2;
};
// the two sections of the K-weighting filter at sample rate sr: high shelf, then high-pass
std::array<Biquad, 2> kWeighting(double sr);
double toLufs(double power);  // -inf for power <= 0
// gated mean loudness of 400 ms block powers
double integrated(const std::vector<double>& gates);

// Measures one signal. add()/addSilence() and read() may run on different threads.
class LoudnessMeter {
public:
    LoudnessMeter(int sr, int channels);
    const int channels;
    void add(const float* interleaved, int frames);  // `channels` floats per frame
    void addSilence(int frames);
    // (momentary, short-term, integrated) in LUFS; -inf when silent or nothing measured yet
    void read(double& momentary, double& shortTerm, double& integratedOut) const;

private:
    void collect(const double* power, int frames);  // power null: silence
    std::array<Biquad, 2> sos_;
    std::vector<double> zi_;  // filter state carried between buffers: [section][2][channel]
    int hop_;
    double sum_ = 0;          // the 100 ms block being filled
    int count_ = 0;
    std::deque<double> blocks_;   // mean power of recent 100 ms blocks (at most SHORT_TERM_BLOCKS)
    std::vector<double> gates_;   // power of every 400 ms block (75 % overlap) since reset
    std::vector<double> power_;   // per-frame power (scratch)
    mutable std::mutex lock_;
};

}  // namespace loudness
