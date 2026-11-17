// Audio CPU load: how much of each buffer's time budget the callback uses (cpu.py).
//
// 100 % means the callback took as long as the audio it produced lasts; above that the driver
// runs dry and you hear dropouts.
#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

struct Track;

namespace audio {

class CpuMeter {
public:
    CpuMeter() { reset(); }
    std::atomic<int> late{0};  // late callbacks (likely dropouts) since playback started; counted by the engine
    double average = -1;       // the rolling average (GUI thread); -1 = none yet

    void reset();
    // called by the audio thread after every callback (the lock is held for a few instructions)
    void add(double busy, double budget);
    // (GUI thread) the load as fractions; false if no audio ran since the last read.
    // current: load over the time since the previous read(); average: exponential average with a
    // time constant of about averageSeconds.
    bool read(double averageSeconds, double& current, double& averageOut);

private:
    std::mutex lock_;
    double busy_ = 0;    // seconds spent in the callback since the last read()
    double budget_ = 0;  // seconds of audio produced in that time
};

// How much processor time each track takes (the mixer measures it: mixer::TrackTap::work), as a share
// of one core: 0.25 = a quarter of the time the audio it made lasts. With several cores (mixer.h,
// SEVERAL CORES) the tracks' shares can add up to more than the overall load.
//
// The audio thread adds to a fixed open-addressing table keyed by the Track* (no allocation, no lock);
// the GUI thread takes and clears the totals now and then. A track that is gone is forgotten when the
// GUI reads (its row may be taken by a new track later). More than SLOTS tracks at once: the rest aren't
// measured.
class TrackCpu {
public:
    static constexpr size_t SLOTS = 1024;
    // audio thread
    void add(const Track* track, double seconds);
    void addBudget(double seconds);  // seconds of audio made in a callback
    // GUI thread: (track, share of a core) since the last read, the costliest first; only `known` tracks
    // (the rest are forgotten). Empty if no audio was made since.
    std::vector<std::pair<const Track*, double>> read(const std::vector<const Track*>& known);

private:
    struct Slot {
        std::atomic<const Track*> key{nullptr};
        std::atomic<double> busy{0};
    };
    Slot slots_[SLOTS];
    std::atomic<double> budget_{0};
};

}  // namespace audio
