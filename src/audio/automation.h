// Automation: a plugin parameter, or a track's own volume or pan, following a curve over time
// (model.h's Automation, one lane each under its track), automation.py. No GUI.
//
// A track's volume and pan are automated like a plugin's parameters: their lanes' slot is the track's
// `controls` (model.h), with the parameters "Volume" (0: silence … 1: MAX_DB, the fader's range) and
// "Pan" (0 left, ½ centre, 1 right). The mixer reads them where it applies the fader (mixerValues()),
// gliding across each buffer, so they work on frozen tracks too and, like the fader, aren't part of
// freezes and bounces; while a lane has points it decides, and the fader and knob on the track's row
// show its value as it plays (mixerValues(track, true, engine.heardPos(), sr, …) every frame).
//
// A track's automation lanes belong to its instrument and effects. Parameters are found by name in
// the plugin's parameter list (plugin_audio::Parameter), which is quick even for big synths.
//
// While a plugin's window is open, TweakWatcher notes which parameters you change there (the
// plugin tells the host about each change made with the mouse; the parameter chooser lists the last
// ones first). Several changing at once is a preset being loaded, not a tweak; parameters that
// automation is moving don't count either.
//
// While playing (and when rendering), apply() sets each automated parameter to the curve's value
// at the start of every buffer, before the plugin processes it.
//
// A parameter with a few values only (a switch: On / Off; a choice of 3 styles…; its `steps`, from
// the plugin) has a stepped curve: its points take one of those values (snap()), and it holds each
// point's value until the next point, jumping there, rather than sloping through values it can't
// take. A lane without points leaves its parameter alone. While stopped, nothing is set, so a
// plugin's own controls can still be moved.
//
// The GUI calls resolve(project) every frame (cheap when nothing is new): it looks up each lane's
// parameter id in its plugin (Automation::targetPlugin / targetParam / targetSteps / hasTarget).
#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio/plugin_audio.h"
#include "model/model.h"

namespace automation {

constexpr int RECENT = 10;        // parameters remembered as recently tweaked, per plugin
constexpr int PRESET_CHANGE = 4;  // more parameters than this changing between two looks: a preset was loaded

bool isPlaceholder(const std::string& name);  // placeholders some plugins expose by the thousand

// --- a track's own Volume and Pan (Track::controls) ---
constexpr const char* VOLUME = "Volume";
constexpr const char* PAN = "Pan";
double volumeOf(double value);                         // a Volume lane's value (0…1) in dB: MIN_DB … MAX_DB
double panOf(double value);                            // a Pan lane's value (0…1): -1 … 1
double controlValue(const Track& track, const std::string& name);  // the fader's / knob's value now, as 0…1
std::string controlText(const std::string& name, double value);    // "-inf dB", "+1.5 dB"; "C", "30 L", "12 R"
// (volume dB, pan) the track plays with at timeline frame `start` (hasStart false: stopped): its fader and
// pan, or their lanes' values while those have points. Any thread, no allocation.
void mixerValues(const Track& track, bool hasStart, int64_t start, int sr, double& volumeDb, double& pan);
// is the track's Volume (or Pan) automated (a lane with points)?
bool controlAutomated(const Track& track, const std::string& name);

// what a track's automation can belong to: its own volume and pan (`controls`), its instrument (or those
// of its panel), then its effects
std::vector<SlotPtr> pluginsOf(const Track& track);
// the parameters a plugin can automate, in its own order (GUI thread); Parameter::name is the name. A
// `controls` slot has none here: its parameters are VOLUME and PAN.
std::vector<plugin_audio::Parameter> parameters(const PluginSlot& slot);

// Asks a plugin now and then (poll(), GUI thread) which parameters were changed in its window, and
// keeps their names in slot.recent, latest first.
class TweakWatcher {
public:
    TweakWatcher(SlotPtr slot, Project& project);
    std::set<std::string> automated() const;  // names of its parameters that automation moves (curves with points)
    void poll();
    SlotPtr slot;

private:
    Project& project_;
    std::vector<std::pair<uint32_t, std::string>> names_;
};

// Drop the lanes of plugins the track no longer has (its volume and pan lanes, those of an instrument
// patch panel it doesn't play now, or of the instrument it played before, stay). True if any were dropped.
bool prune(Track& track);
// Look up the parameter of every lane whose plugin is (newly) loaded (GUI thread, every frame).
void resolve(Project& project);
int stepsOf(const Automation& lane);  // 0: any value; 1: two values, e.g. Off / On
double snap(double value, int steps);  // at the nearest of the parameter's values: k / steps
// The curve's value (0…1) at `seconds`: straight lines between points, flat before the first and
// after the last. A stepped parameter (steps > 0) holds each point's value until the next.
double valueAt(const std::vector<AutoPoint>& points, double seconds, int steps = 0);
// Set the track's automated plugin parameters to their values at timeline frame `start` (audio thread,
// or the main thread when rendering). Volume and pan lanes are left to the mixer (mixerValues()).
void apply(Track& track, int64_t start, int sr);

}  // namespace automation
