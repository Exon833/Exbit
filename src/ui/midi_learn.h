// MIDI learn: a knob (or fader, button…) of a MIDI controller moving a parameter of the program, as
// if it were turned in its window.
//
// LEARNING. Right-click a control → "Link to a MIDI controller…" (a built-in effect's knobs in its
// window; a track's fader and pan, and any parameter of its plugins, from the track's right-click menu;
// a plugin's parameter from its unit's menu in the effects window), then turn a knob on the controller:
// the first control change that arrives links it (the status bar says so meanwhile; Escape cancels).
// A parameter has one link (learning again replaces it); a knob may move several parameters.
//
// MOVING. A linked control change (its number and channel) sets the parameter to value / 127 (0…1 of
// its range). Everything happens on the GUI thread: the MIDI input's thread only queues the events
// (audio/midi_in.cpp), MidiInput::poll() hands them to handle() every frame (MidiInput::controlHook), and
// the parameter is set the way a turn of its knob in its window sets it: Plugin::windowEdit, so the
// plugin's edited() lists it (automation's TweakWatcher, and modulation's base: modulation::userEdited;
// with no window open to ask, the edit is handed to modulation::userEdited here). A track's Volume and
// Pan set its fader and pan knob (Track::volumeDb, pan) as dragging them does. Nothing here runs on, waits
// for, or allocates on the audio thread.
//
// THE LINKS are the project's (Project::midiLinks), saved with it (project_io: by track index, plugin
// and parameter name, only when there are any). Options → "MIDI controller links…" lists them (the
// controller, its control number and channel, what it moves) with Remove.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio/midi_in.h"
#include "model/model.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
namespace plug { class Plugin; }

namespace midi_learn {

// channel mode messages (all sound off, reset, all notes off…): never learnt nor linked
constexpr int FIRST_MODE_CC = 120;

// what a link moves, in words: "Bass › EQ › Gain" ("Master › …", "Bass › Volume")
std::string targetName(const Project& project, const TrackPtr& track, const SlotPtr& slot, const std::string& param);
std::string targetName(const Project& project, const MidiLink& link);
// the controller's knob in words: "CC 74, channel 1 (Keystation 49)"
std::string controlName(const MidiLink& link);
// the track holding a slot (its instrument, rack, effects or controls); null for the master's effects or none
TrackPtr ownerOf(const Project& project, const PluginSlot* slot);
// does the link still name a parameter of the project (its track there, the plugin on it)?
bool present(const Project& project, const MidiLink& link);
// the parameters a link can name for a slot: Volume and Pan for a track's controls, else the automatable
// parameters of its loaded plugin (by name)
std::vector<std::string> parameterNames(const PluginSlot& slot);

class Learner {
public:
    explicit Learner(Project& project);
    Project& project;

    // --- the GUI's hooks (all optional) ---
    std::function<void(const std::string& text, int ms)> status;     // the status bar
    std::function<bool(const PluginSlot* slot)> windowOpen;          // its window is open (its TweakWatcher asks it)
    std::function<void(const TrackPtr& track)> mixerMoved;           // a track's fader or pan moved: show it
    std::function<void(const SlotPtr& slot)> pluginMoved;            // a plugin's parameter moved (unsaved settings)
    std::function<void()> changed;                                   // links added or removed
    std::function<bool()> inputReady;                                // a MIDI input is chosen (else learning says how)

    // --- learning ---
    // wait for the first control change: it will move this parameter (track null: a master effect's)
    void learn(const TrackPtr& track, const SlotPtr& slot, const std::string& param);
    void cancel();  // (Escape) stop waiting
    bool waiting() const { return waiting_.has_value(); }
    std::string waitingFor() const;  // what it waits to link, in words ("" none)

    // --- MIDI (GUI thread) ---
    // an event from the MIDI input: true if it was a control change used here (learnt, or moving links)
    bool handle(const midi_in::Event& e, const std::string& controller = "");
    // set a link's parameter to `value` (0…1) as if turned in its window; false if it isn't there now
    bool apply(const MidiLink& link, double value);

    // --- the links ---
    // the links moving this parameter (indices in project.midiLinks)
    std::vector<size_t> linksOf(const PluginSlot* slot, const std::string& param) const;
    void remove(size_t index);
    void removeLinksOf(const PluginSlot* slot, const std::string& param);
    void add(const MidiLink& link);  // (replacing any link to the same parameter)

private:
    struct Waiting {
        TrackPtr track;
        SlotPtr slot;
        std::string param;
    };
    std::optional<Waiting> waiting_;
    struct Found {
        std::weak_ptr<plug::Plugin> plugin;
        uint32_t id = 0;
    };
    std::map<std::pair<const PluginSlot*, std::string>, Found> ids_;  // parameters found by name (per plugin)
    bool findParam(const SlotPtr& slot, const std::shared_ptr<plug::Plugin>& inst, const std::string& param,
                   uint32_t& id);
    void say(const std::string& text, int ms);
};

// the program's learner (set by the main window; null: MIDI learn isn't offered)
Learner* active();
void setActive(Learner* learner);

// Menu entries for one parameter: "Link to a MIDI controller…" (and, when linked, which control moves it
// and "Remove the MIDI controller link"); nothing without an active learner. `label`: the first entry's
// text instead of "Link to a MIDI controller…"
void fillParamMenu(ui::Menu& menu, const TrackPtr& track, const SlotPtr& slot, const std::string& param,
                   const std::string& label = "");
// "Link a knob to a MIDI controller…": choose one of the slot's parameters (a list), then learn it
void chooseAndLearn(ui::Window* parent, const TrackPtr& track, const SlotPtr& slot);

// Options → MIDI controller links…: the links (the controller's knob, what it moves), each removable
class LinksDialog : public ui::Dialog {
public:
    LinksDialog(ui::Window* parent, Learner& learner);
    Learner& learner;
    ui::ListWidget* list;
    ui::Button* removeButton;
    ui::Label* empty;
    void fill();             // the list as the project's links are now
    void removeSelected();   // the selected links go
};
void showLinks(ui::Window* parent, Learner& learner);

}  // namespace midi_learn
