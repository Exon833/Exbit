// What the plugin speed benchmark (tests/test_plugin_speed.cpp) plays and how: shared by its two sides, Exbit's
// mixer and the bare reference that calls the plugins directly (also built as plugin-speed-reference.exe, for
// Wine and Windows), so both do exactly the same work.
#pragma once

namespace plugin_speed {

constexpr int SAMPLE_RATE = 48000;
constexpr int FRAME_SIZES[] = {64, 256, 1024};
constexpr double SECONDS_PER_RUN = 1.5;  // of sound, played at real-time pace (a device's)
constexpr int REPEATS = 3;               // runs of each side, taken in turn (their medians compared)
constexpr int CHAIN_EFFECTS = 3;         // the chain: the instrument and three effects after it, on one track
constexpr int EIGHT_TRACKS = 8;
constexpr double TEMPO = 120;
// a sustained chord (MIDI pitches), held through every run, so the synth does real work
constexpr int CHORD_PITCHES[] = {48, 52, 55, 59, 62, 64, 67, 71};
constexpr int CHORD_VELOCITY = 100;

}  // namespace plugin_speed
