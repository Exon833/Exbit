// Is an installer stuck? (see installer_watchdog.h)
#include "plugins/installer_watchdog.h"

namespace watchdog {

bool looksHung(const std::vector<Sample>& samples, double hungAfter, double idleShare) {
    if (samples.size() < 2)
        return false;
    const Sample& last = samples.back();
    // the earliest sample still inside the span, walking back while every one shows no window
    std::size_t first = samples.size() - 1;
    while (first > 0 && last.seconds - samples[first].seconds < hungAfter) {
        if (samples[first].visibleWindows != 0)
            return false;
        first--;
    }
    if (samples[first].visibleWindows != 0 || last.seconds - samples[first].seconds < hungAfter)
        return false;
    double span = last.seconds - samples[first].seconds;
    double used = last.processorSeconds - samples[first].processorSeconds;
    return used >= 0 && used < idleShare * span;  // (less used: a program ended, something happened)
}

void trim(std::vector<Sample>& samples, double keepSeconds) {
    if (samples.empty())
        return;
    double since = samples.back().seconds - keepSeconds;
    std::size_t firstKept = 0;
    while (firstKept + 1 < samples.size() && samples[firstKept + 1].seconds <= since)
        firstKept++;
    samples.erase(samples.begin(), samples.begin() + firstKept);
}

}  // namespace watchdog
