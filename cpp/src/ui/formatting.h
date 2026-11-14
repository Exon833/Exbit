// Turning numbers into display text (formatting.py).
#pragma once
#include <string>

#include "model/timing.h"

namespace fmt {
std::string time(double seconds, int decimals = 3);         // 75.5 -> "1:15.500"
std::string bars(double seconds, const Timing& timing);      // "5.2.3"
double rulerStep(double pps);                                // seconds between ruler ticks
int stepDecimals(double step);                               // 0.25 -> 2, 0.5 -> 1, 2 -> 0
std::string db(double db, double minDb);                     // "-inf dB" / "+3 dB"
std::string pan(double pan);                                 // -0.3 -> "30 L", 0 -> "C", 1 -> "100 R"
}  // namespace fmt
