// Timing buffers as a sound device asks for them: one every frames / rate seconds (tests/test_plugin_speed.cpp)
#pragma once
#include <functional>
#include <vector>

namespace plugin_speed {

// calls processBuffer(index) once per buffer for secondsOfSound of sound, at real-time pace; returns how long each
// call took (seconds), the first tenth (the plugins settling) left out
std::vector<double> timePacedBuffers(int frames, double secondsOfSound, const std::function<void(int index)>& processBuffer);

}  // namespace plugin_speed
