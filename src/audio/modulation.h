// Modulation cables: a modulator unit (an LFO, Random, a Level Follower, a Macro:
// effects/modulators.cpp) moving a knob of another of the track's units (model.h ModCable).
//
// THE VALUE. Each modulated parameter has its own value, its BASE (0…1: where the user, or an
// automation lane, put it), and the modulation moves it around that: base + the sum of depth × the
// modulator's value over every cable to it, kept within 0…1. A two-way modulator (-1…1) moves it both
// ways, a one-way one (0…1) only up (a negative depth: only down).
//
// WHO KNOWS THE BASE. The audio thread never reads a plugin's parameters (a Windows plugin's are kept
// for the GUI thread, and asking may wait for its host), so the base is kept in the cable's ModState:
//   - made when the cable is (resolve(): the parameter's value then; a saved project keeps it);
//   - an automation lane on a modulated parameter moves its base instead of the parameter
//     (takeAutomation(), from automation::apply);
//   - turning the knob in the plugin's (or built-in effect's) own window moves its base: the plugin
//     says which knobs were turned (automation::TweakWatcher), and userEdited() takes the new value
//     unless it is just the modulation's own last one.
// When a cable goes (release()), the parameter goes back to its base.
//
// WHEN. The audio thread works the modulation out every BLOCK frames, counted from the timeline's
// frame 0 while playing (so a render moves the knobs at the same moments as playback, whatever the
// buffers' size): mixer.cpp splits a track's effects' processing at those frames when the track has
// modulation cables. An instrument's parameters follow once per buffer (it plays before its effects).
#pragma once
#include <cstdint>
#include <vector>

#include "effects/effect.h"
#include "model/model.h"

class Project;
namespace plugin_audio { struct Transport; }

namespace modulation {

constexpr int BLOCK = 128;  // frames between steps (2.7 ms at 48 kHz)

// its loaded plugin is a modulator (null: not loaded, or not one)
effects::Effect* modulatorOf(const PluginSlot* slot);
// the slot is a modulator unit (by its plugin path, loaded or not)
bool isModulatorPath(const std::string& path);
bool isModulator(const PluginSlot* slot);

// --- GUI thread ---
// Find each cable's parameter in its plugin (newly loaded or reloaded ones), and take its base where it
// has none yet. Every frame (main window), as automation::resolve.
void resolve(Project& project);
void resolve(Track& track);
// The cables whose ends are still the track's units (an effect removed takes its cables with it).
std::vector<ModCable> kept(const Track& track);
// The track's units a cable may go to from `from`: its effects and modulators but `from`, and the
// instruments it plays.
std::vector<SlotPtr> targetsFor(const Track& track, const PluginSlot* from);
// The parameter back at its base (its cable is gone, or the modulator was removed).
void release(const ModCable& cable);
// The plugin of `slot` says these parameters were turned in its window: their cables' base follows.
void userEdited(Project& project, const PluginSlot* slot, const std::vector<uint32_t>& ids);

// --- audio thread ---
// Where the song is at timeline frame `frame` (playing) for a piece of `frames` frames.
effects::ModClock clockAt(const TempoMap& map, bool playing, int64_t frame, int frames, int sr);
// Every cable's target set from its modulators' values now (one step). Cables to an instrument too
// (`instruments`: false leaves them for the next step).
void apply(const std::vector<ModCable>& cables, const effects::ModClock& clock, bool instruments = true);
// An automation lane's value for this parameter: true if it is modulated (its base takes the value, and
// the modulation applies it), false if the lane should set it itself.
bool takeAutomation(const Track& track, const PluginSlot* slot, uint32_t paramId, double value);
// frames from `frame` to the next step (BLOCK frames counted from 0), at most `left`
int toNextStep(bool playing, int64_t frame, int done, int left);

}  // namespace modulation
