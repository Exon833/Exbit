// (internal) the built-in effects' makers, one per effect (effects.cpp lists them)
#pragma once
#include <memory>

#include "effects/effect.h"

namespace effects {

std::unique_ptr<Effect> makeEq();                   // eq.cpp
std::unique_ptr<Effect> makeCompressor();           // dynamics.cpp
std::unique_ptr<Effect> makeMultibandCompressor();  // dynamics.cpp
std::unique_ptr<Effect> makeGate();                 // dynamics.cpp
std::unique_ptr<Effect> makeLimiter();              // dynamics.cpp
std::unique_ptr<Effect> makeChorus();               // time.cpp
std::unique_ptr<Effect> makeDelay();                // time.cpp
std::unique_ptr<Effect> makeReverb();               // time.cpp
std::unique_ptr<Effect> makeSaturation();           // distortion.cpp
std::unique_ptr<Effect> makeBitcrusher();           // distortion.cpp
std::unique_ptr<Effect> makeUtility();              // utility.cpp
std::unique_ptr<Effect> makeFilterMotion();         // motion.cpp
std::unique_ptr<Effect> makePhaserFlanger();        // motion.cpp
std::unique_ptr<Effect> makeTremoloPan();           // motion.cpp
// (transient shaping and time manipulation)
std::unique_ptr<Effect> makeTransientShaper();      // transient.cpp
std::unique_ptr<Effect> makeTimeFx();               // timefx.cpp

// effects/delay.py's synced lengths (beats) and their names
extern const double DELAY_BEATS[8];

// effects/eq.py: the response's frequency range in the window, and x (0…1) of a frequency there
constexpr double EQ_LOW = 20.0, EQ_HIGH = 20000.0;
double eqX(double freq);

}  // namespace effects
