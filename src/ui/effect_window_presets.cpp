// A built-in effect's window: its presets, comparing (A/B) and MIDI learn (effect_window.h; presets_ui.h,
// midi_learn.h). Kept apart from effect_window.cpp, which draws and follows the effect.
#include <algorithm>

#include "model/presets.h"
#include "ui/effect_window.h"
#include "ui/midi_learn.h"
#include "ui/presets_ui.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/widgets.h"

void EffectWindow::addPresetsButton(ui::HBox* row) {
    if (!effect || !row) return;
    // each parameter's name label (made by the constructor, in the parameters' order)
    const auto& params = effect->params();
    nameLabels.assign(params.size(), nullptr);
    std::vector<ui::Widget*> used;
    for (size_t i = 0; i < params.size(); i++)
        for (auto& child : children()) {
            auto* label = dynamic_cast<ui::Label*>(child.get());
            if (!label || label->text() != params[i].name) continue;
            if (std::find(used.begin(), used.end(), label) != used.end()) continue;
            used.push_back(label);
            nameLabels[i] = label;
            break;
        }
    presetsButton = add<ui::Button>("Presets \xe2\x96\xbe");
    presetsButton->setToolTip("Save these settings as a preset, load one, or compare two settings (Keep this, then "
                              "switch between Now and Kept)");
    ui::Menu* menu = widgets::ownedMenu(presetsButton);
    menu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        presets_ui::fillEffectMenu(*m, this, slot);
    };
    presetsButton->setMenu(menu);
    row->addWidget(presetsButton);
}

void EffectWindow::showDifferences() {
    if (!effect || !slot) return;
    std::vector<int> now = presets::differing(*slot);
    if (now == marked) return;
    marked = now;
    for (size_t i = 0; i < nameLabels.size(); i++) {
        ui::Label* label = nameLabels[i];
        if (!label) continue;
        bool differs = std::find(marked.begin(), marked.end(), (int)i) != marked.end();
        if (differs) {
            label->setColor(theme::c("accent"));
            presets::Compare* c = presets::compareOf(slot.get());
            label->setToolTip(std::string("Differs from the ") + (c && c->onKept ? "settings of Now" : "kept settings"));
        } else {
            label->clearColor();
            label->setToolTip("");
        }
    }
}

int EffectWindow::controlAt(Point pos) {
    for (ui::Widget* w = childAt(pos); w && w != this; w = w->parent()) {
        for (const auto& c : controls)
            if (c.widget == w || c.text == w) return c.index;
        for (size_t i = 0; i < nameLabels.size(); i++)
            if (nameLabels[i] == w) return (int)i;
    }
    return -1;
}

bool EffectWindow::contextMenu(ui::MouseEvent& e) {
    if (!effect || !slot || !midi_learn::active()) return false;
    int index = controlAt(e.pos);
    if (index < 0 || index >= (int)effect->params().size()) return false;
    ui::Menu menu;
    midi_learn::Learner* learner = midi_learn::active();
    TrackPtr track = midi_learn::ownerOf(learner->project, slot.get());
    midi_learn::fillParamMenu(menu, track, slot, effect->params()[index].name);
    if (menu.isEmpty()) return false;
    menu.exec(e.globalPos, this);
    return true;
}
