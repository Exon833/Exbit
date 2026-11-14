// Audio CPU load: how much of each buffer's time budget the callback uses (cpu.py).
//
// 100 % means the callback took as long as the audio it produced lasts; above that the driver
// runs dry and you hear dropouts.
#pragma once
#include <atomic>
#include <mutex>

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

}  // namespace audio
