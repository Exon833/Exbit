#include "audio/cpu.h"

#include <algorithm>

namespace audio {

void CpuMeter::reset() {
    {
        std::lock_guard<std::mutex> g(lock_);
        busy_ = budget_ = 0;
    }
    average = -1;
}

void CpuMeter::add(double busy, double budget) {
    std::lock_guard<std::mutex> g(lock_);
    busy_ += busy;
    budget_ += budget;
}

bool CpuMeter::read(double averageSeconds, double& current, double& averageOut) {
    double busy, budget;
    {
        std::lock_guard<std::mutex> g(lock_);
        busy = busy_;
        budget = budget_;
        busy_ = budget_ = 0;
    }
    if (budget == 0) return false;
    current = busy / budget;
    double weight = std::min(1.0, budget / averageSeconds);
    average = average < 0 ? current : average + weight * (current - average);
    averageOut = average;
    return true;
}

}  // namespace audio
