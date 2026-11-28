// Running plugin-speed-reference.exe (tests/plugin_speed/reference_main.cpp) for the plugin speed benchmark, under
// Wine or on Windows, and reading back its buffer times
#pragma once
#include <string>
#include <vector>

namespace plugin_speed {

// runs these words as one command (each quoted for the system's shell: environment settings first through `env` on
// Linux, then wine, the program and its arguments) and reads its per-buffer times (seconds), one list per
// FRAME_SIZES entry; empty, with `error`, if it fails
std::vector<std::vector<double>> runReferenceProgram(const std::vector<std::string>& commandWords, std::string& error);

}  // namespace plugin_speed
