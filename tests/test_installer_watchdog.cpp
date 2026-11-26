// Is an installer stuck (plugins/installer_watchdog.h)? Judged on made-up looks at a prefix: its windows
// and the processor time its programs used.
#include <vector>

#include "check.h"
#include "plugins/installer_watchdog.h"

namespace {

using watchdog::Sample;

// a look every `step` seconds for `span` seconds: `windows` windows, the processor used at `share` of one
std::vector<Sample> steady(double span, double step, int windows, double share, double start = 0) {
    std::vector<Sample> samples;
    for (double t = 0; t <= span + 1e-9; t += step)
        samples.push_back({start + t, (start + t) * share, windows});
    return samples;
}

void testHung() {
    // no window, next to no work, for long enough: hung (Quanta 2's installer under Wine: about 1%)
    CHECK(watchdog::looksHung(steady(200, 10, 0, 0.01)));
    CHECK(watchdog::looksHung(steady(180, 30, 0, 0.0)));
    // not long enough yet
    CHECK(!watchdog::looksHung(steady(170, 10, 0, 0.0)));
    CHECK(!watchdog::looksHung({}));
    CHECK(!watchdog::looksHung({{0, 0, 0}}));
    // a window shown: waiting for the person (a licence, a question)
    CHECK(!watchdog::looksHung(steady(600, 10, 1, 0.0)));
    // working (unpacking, copying): not hung, with or without a window
    CHECK(!watchdog::looksHung(steady(600, 10, 0, 0.5)));
    CHECK(!watchdog::looksHung(steady(600, 10, 0, 0.05)));
    // the windows couldn't be told: never taken as hung
    CHECK(!watchdog::looksHung(steady(600, 10, -1, 0.0)));
}

// only the last HUNG_AFTER_SECONDS count
void testRecentOnly() {
    // busy, with a window, then idle without one for 3.5 minutes: hung
    std::vector<Sample> samples = steady(300, 10, 1, 0.8);
    double used = samples.back().processorSeconds;
    for (double t = 310; t <= 520; t += 10) samples.push_back({t, used + (t - 300) * 0.005, 0});
    CHECK(watchdog::looksHung(samples));
    // a window shown a minute ago: not yet
    samples = steady(400, 10, 0, 0.0);
    samples[samples.size() - 7].visibleWindows = 1;
    CHECK(!watchdog::looksHung(samples));
    // one burst of work within the span: not hung
    samples = steady(400, 10, 0, 0.0);
    for (size_t i = samples.size() - 5; i < samples.size(); i++) samples[i].processorSeconds += 20;
    CHECK(!watchdog::looksHung(samples));
    // processor time going down (a program ended, its time gone from the sum): something happened
    samples = steady(400, 10, 0, 0.0, 0);
    for (auto& s : samples) s.processorSeconds = 50;
    samples.back().processorSeconds = 10;
    CHECK(!watchdog::looksHung(samples));
    // a different threshold and span
    CHECK(watchdog::looksHung(steady(60, 5, 0, 0.0), 60));
    CHECK(!watchdog::looksHung(steady(200, 10, 0, 0.02), 180, 0.01));
}

void testTrim() {
    std::vector<Sample> samples = steady(1000, 10, 0, 0.0);
    watchdog::trim(samples, 360);
    CHECK(!samples.empty() && samples.back().seconds == 1000);
    CHECK(samples.front().seconds <= 640 && samples.front().seconds >= 630);
    CHECK(watchdog::looksHung(samples));
    std::vector<Sample> none;
    watchdog::trim(none);
    CHECK(none.empty());
}

}  // namespace

int main() {
    testHung();
    testRecentOnly();
    testTrim();
    TEST_MAIN_END();
}
