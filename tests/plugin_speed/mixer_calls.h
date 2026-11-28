// Exbit's side of the plugin speed benchmark (tests/test_plugin_speed.cpp): the case built as a project and mixed
// the way the engine does it, so every plugin goes the way the program sends it (in process, or bridged).
#pragma once
#include <string>
#include <vector>

class Project;

namespace plugin_speed {

// fills an empty project with the case ("one", "chain" or "eight"): MIDI tracks, each with the instrument (and for the
// chain, its effects) playing the sustained chord; false, with `error`, if a plugin doesn't load
bool buildSpeedProject(Project& project, const std::string& speedCase, const std::string& instrumentPath,
                       const std::string& effectPath, std::string& error);

// per-buffer times (seconds) of the project mixed by mixer::mixClips on the engine's worker pool, on an audio thread
// of its own, one list per FRAME_SIZES entry
std::vector<std::vector<double>> timeMixerCalls(Project& project);

}  // namespace plugin_speed
