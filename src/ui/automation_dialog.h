// Choosing a plugin parameter to automate (automation_dialog.py). CONTRACT written by the lead;
// implemented in automation_dialog.cpp.
//
// The track's own volume and pan ("Track: volume and pan", listed first: its `controls` slot), or one
// of the track's plugins (its instrument or an effect), then one of that plugin's parameters,
// found by typing part of its name (big synths have hundreds). The ones tweaked last in the
// plugin's window come first (automation::TweakWatcher), so turning a knob there and then choosing
// it here takes no searching.
#pragma once
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio/plugin_audio.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class ParameterDialog : public ui::Dialog {
public:
    // modulating: choosing a knob for that modulator to move (audio/modulation.h) instead of a parameter to
    // automate: the track's units but the modulator itself (not its volume and pan), automated ones too
    ParameterDialog(ui::Window* parent, const TrackPtr& track, const PluginSlot* modulating = nullptr);
    // after exec() returned Accepted: the plugin and its parameter's name
    SlotPtr chosenSlot;
    std::string chosenParam;

    // (added by the plugins-UI port)
    TrackPtr track;
    std::vector<SlotPtr> slots;   // the plugins listed, in the combo box's order
    ui::ComboBox* plugins;
    ui::LineEdit* search;
    ui::ListWidget* list;
    ui::Label* count;
    void fill();                  // the list for the plugin chosen and the words typed
    SlotPtr currentSlot() const;
    void done(int result) override;  // Accepted only with an enabled parameter chosen

private:
    std::set<std::pair<const PluginSlot*, std::string>> taken_;  // automated already
    std::map<const PluginSlot*, std::vector<plugin_audio::Parameter>> params_;  // listed once
    void addHeading(const std::string& title);
    void addItems(const SlotPtr& slot, const std::vector<const plugin_audio::Parameter*>& found);
};
