// The bare reference of the plugin speed benchmark (tests/test_plugin_speed.cpp): the case's plugins loaded with
// Exbit's own VST3 host and called directly, buffer after buffer, with no mixer and no bridge between. Built into
// the test (Linux plugins, in process) and into plugin-speed-reference.exe (Windows plugins, under Wine or on
// Windows: tests/plugin_speed/reference_main.cpp).
#pragma once
#include <string>
#include <vector>

namespace workers {
class Pool;
}

namespace plugin_speed {

// per-buffer times (seconds) of a case ("one", "chain" or "eight") called directly on the calling thread (an audio
// thread) and `pool` (the engine's: as many workers as it would have), one list per FRAME_SIZES entry; empty, with
// `error`, if a plugin doesn't load
std::vector<std::vector<double>> timeDirectCalls(const std::string& speedCase, const std::string& instrumentPath,
                                                 const std::string& effectPath, workers::Pool& pool, std::string& error);

}  // namespace plugin_speed
