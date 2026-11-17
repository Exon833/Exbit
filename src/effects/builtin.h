// (internal) the built-in effects' makers, one per effect (effects.cpp lists them)
#pragma once
#include <memory>
#include <string>
#include <vector>

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

// --- Space (space.cpp) and Tuner (tuner.cpp) ---
std::unique_ptr<Effect> makeSpace();
std::unique_ptr<Effect> makeTuner();
// the tuner's reading of a frequency: the nearest note ("A4", on the reference's scale) and the
// cents off it (−50…+50)
struct TunerReading {
    double hz = 0;  // 0: no pitch
    double cents = 0;
    std::string note;
};
TunerReading tunerReading(double hz, double reference = 440.0);
// Space's impulse response, set directly (tests; a sound file's, later): the left and right
// responses as they are (no level change; pre-delay, cuts and length still apply), ready before
// this returns. (any thread but the audio thread)
void setSpaceImpulse(Effect& space, std::vector<float> left, std::vector<float> right);
// the impulse response Space plays now (after its pre-delay, cuts and length), each side
std::vector<float> spaceImpulse(Effect& space, int side);
// wait until Space has built the response for its current settings (tests); false: not in `seconds`
bool spaceSettle(Effect& space, double seconds = 5.0);
// the pieces of 256 frames Space's response has after its first (each costs a complex multiply-add
// per bin and side every 256 frames: tests, measuring)
int spacePartitions(Effect& space);

// effects/delay.py's synced lengths (beats) and their names
extern const double DELAY_BEATS[8];

// effects/eq.py: the response's frequency range in the window, and x (0…1) of a frequency there
constexpr double EQ_LOW = 20.0, EQ_HIGH = 20000.0;
double eqX(double freq);

}  // namespace effects
