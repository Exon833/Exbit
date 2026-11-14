#include "ui/formatting.h"

#include <cmath>
#include <cstdio>

#include "config.h"

namespace fmt {

std::string time(double seconds, int decimals) {
    double m = std::floor(seconds / 60.0);
    double s = seconds - m * 60.0;
    int width = decimals ? 3 + decimals : 2;
    char buf[64];
    snprintf(buf, sizeof buf, "%d:%0*.*f", (int)m, width, decimals, s);
    return buf;
}

std::string bars(double seconds, const Timing& timing) {
    int bar, beat, part;
    timing.position(seconds, bar, beat, part);
    return std::to_string(bar) + "." + std::to_string(beat) + "." + std::to_string(part);
}

double rulerStep(double pps) {
    for (double s : config::RULER_STEPS)
        if (s * pps >= config::RULER_MIN_TICK_PX) return s;
    return config::RULER_STEPS[sizeof(config::RULER_STEPS) / sizeof(double) - 1];
}

int stepDecimals(double step) {
    if (step >= 1) return 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%g", step);
    std::string s = buf;
    size_t dot = s.find('.');
    return dot == std::string::npos ? 0 : (int)(s.size() - dot - 1);
}

std::string db(double value, double minDb) {
    if (value <= minDb) return "-inf dB";
    char buf[32];
    // Python's f"{db:+d}" needs an int: the callers pass whole numbers (sliders); round to be safe
    snprintf(buf, sizeof buf, "%+d dB", (int)std::lround(value));
    return buf;
}

std::string pan(double value) {
    long amount = std::lround(std::fabs(value) * 100);  // (Python's round: half to even; .5 % never comes up)
    if (amount == 0) return "C";
    return std::to_string(amount) + (value < 0 ? " L" : " R");
}

}  // namespace fmt
