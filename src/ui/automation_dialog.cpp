// Choosing a parameter to automate (automation_dialog.py).
#include "ui/automation_dialog.h"

#include <algorithm>

#include "audio/automation.h"
#include "core/util.h"
#include "ui/toolkit/layout.h"

ParameterDialog::ParameterDialog(ui::Window* parent, const TrackPtr& track_, const PluginSlot* modulating)
    : ui::Dialog(parent, (modulating ? "Modulate a knob with " + modulating->name + ": " : "Automate a parameter: ") +
                             track_->name,
                 420, 520),
      track(track_) {
    if (!modulating)
        for (auto& a : track->automation) taken_.insert({a->slot.get(), a->param});
    auto* col = ui::vbox(this, 10, 6);
    auto formLayout = std::make_unique<ui::FormLayout>();
    ui::FormLayout* form = formLayout.get();
    col->addLayout(std::move(formLayout));
    plugins = add<ui::ComboBox>();
    std::vector<SlotPtr> instruments = track->instruments();
    std::vector<SlotPtr> owners = automation::pluginsOf(*track);
    // the track's own volume and pan first (automation.plugins_of lists its controls first)
    if (track->controls && std::find(owners.begin(), owners.end(), track->controls) == owners.end())
        owners.insert(owners.begin(), track->controls);
    if (modulating)  // (its own knobs, and the track's volume and pan, aren't moved by modulation)
        owners.erase(std::remove_if(owners.begin(), owners.end(),
                                    [&](const SlotPtr& s) { return s.get() == modulating || s == track->controls; }),
                     owners.end());
    for (auto& slot : owners) {
        if (slot == track->controls) {
            plugins->addItem("Track: volume and pan", (int)slots.size());
            slots.push_back(slot);
            continue;
        }
        bool isInstrument = std::find(instruments.begin(), instruments.end(), slot) != instruments.end();
        plugins->addItem(std::string(isInstrument ? "Instrument" : "Effect") + ": " + slot->name +
                             (aload(slot->instance) ? "" : "  (not loaded)"),
                         (int)slots.size());
        slots.push_back(slot);
    }
    plugins->currentIndexChanged.connect([this](int) { fill(); });
    search = add<ui::LineEdit>();
    search->setPlaceholder("Type part of a parameter's name…");
    search->textChanged.connect([this](const std::string&) { fill(); });
    form->addRow("Plugin", plugins);
    form->addRow("Find", search);
    list = add<ui::ListWidget>();
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    list->itemActivated.connect([this](int) { accept(); });
    col->addWidget(list, 1);
    count = add<ui::Label>();
    count->setProperty("hint", "true");
    col->addWidget(count);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    col->addWidget(buttons);
    fill();
    search->setFocus();
}

SlotPtr ParameterDialog::currentSlot() const {
    int i = plugins->currentIndex();
    return i >= 0 && i < (int)slots.size() ? slots[i] : nullptr;
}

void ParameterDialog::fill() {
    SlotPtr slot = currentSlot();
    list->clear();
    if (!slot) {
        count->setText("This track has no plugins to automate: add an instrument or effects first.");
        return;
    }
    if (!params_.count(slot.get())) params_[slot.get()] = automation::parameters(*slot);
    const auto& all = params_[slot.get()];
    if (slot->isControls() && all.empty()) {  // (Volume and Pan by name, when automation lists none for them)
        std::vector<std::string> words = util::splitWords(util::lower(search->text()));
        int shown = 0;
        for (const char* name : {"Volume", "Pan"}) {
            std::string low = util::lower(name);
            bool ok = true;
            for (auto& w : words) ok = ok && low.find(w) != std::string::npos;
            if (!ok) continue;
            ui::ListWidget::Item item;
            item.text = name;
            item.data = name;
            if (taken_.count({slot.get(), name})) {
                item.enabled = false;
                item.text += "   — automated already";
            }
            list->addItem(item);
            ++shown;
        }
        count->setText(util::format("%d of 2 parameters", shown));
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i).enabled) {
                list->setCurrentRow(i);
                break;
            }
        return;
    }
    std::vector<std::string> words = util::splitWords(util::lower(search->text()));
    auto matches = [&](const std::string& name) {
        std::string low = util::lower(name);
        for (auto& w : words)
            if (low.find(w) == std::string::npos) return false;
        return true;
    };
    std::vector<const plugin_audio::Parameter*> recent, found;
    for (auto& name : slot->recent) {
        if (!matches(name)) continue;
        for (auto& p : all)
            if (p.name == name) {
                recent.push_back(&p);
                break;
            }
    }
    for (auto& p : all)
        if (matches(p.name)) found.push_back(&p);
    if (!recent.empty()) {  // the ones tweaked last in its window first, under headings
        addHeading("Recently tweaked");
        addItems(slot, recent);
        addHeading("All parameters");
    }
    addItems(slot, found);
    count->setText(aload(slot->instance) || slot->isControls()
                       ? util::format("%d of %d parameters", (int)found.size(), (int)all.size())
                       : slot->name + " isn't loaded, so its parameters can't be listed.");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i).enabled) {
            list->setCurrentRow(i);
            break;
        }
}

void ParameterDialog::addHeading(const std::string& title) {
    ui::ListWidget::Item heading;
    heading.text = title;
    heading.enabled = false;
    heading.selectable = false;
    heading.bold = true;
    list->addItem(heading);
}

void ParameterDialog::addItems(const SlotPtr& slot, const std::vector<const plugin_audio::Parameter*>& found) {
    for (const plugin_audio::Parameter* param : found) {
        std::string value;
        try {
            value = util::trim(param->stringValue());
        } catch (...) {
            value.clear();
        }
        ui::ListWidget::Item item;
        item.text = value.empty() ? param->name : param->name + "   (" + value + ")";
        item.data = param->name;
        if (taken_.count({slot.get(), param->name})) {
            item.enabled = false;
            item.text += "   — automated already";
        }
        list->addItem(item);
    }
}

void ParameterDialog::done(int result) {
    if (result == Accepted) {
        int row = list->currentRow();
        if (row < 0 || row >= list->count() || !list->item(row).enabled) return;
        chosenSlot = currentSlot();
        chosenParam = list->item(row).data;
    }
    ui::Dialog::done(result);
}
