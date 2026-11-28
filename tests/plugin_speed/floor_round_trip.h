// The floor of a bridge crossing: the bare wake-up and wait each way that the bridge is built on (bridge.h's wake and
// waitChange; on Windows the events bound as bridge_win.h binds them) between two processes, nothing else
// (tests/test_bridge_round_trip.cpp, tests/test_plugin_speed.cpp)
#pragma once

namespace plugin_speed {

// the median seconds of one round trip at 64 frames' pace on a real-time thread; 0 if it couldn't be measured
double floorRoundTrip();

// (Windows) the floor's other process is this program again: true if this run is it (it answers, then main returns)
bool answerFloorRoundTrips(int argc, char** argv);

}  // namespace plugin_speed
