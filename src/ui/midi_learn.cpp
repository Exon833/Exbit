// MIDI learn: a MIDI controller's knobs moving the program's parameters (midi_learn.h).
#include "ui/midi_learn.h"

#include <algorithm>
#include <cmath>

#include "audio/automation.h"
#include "audio/automation_write.h"
#include "audio/modulation.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "plugins/plugin.h"
#include "ui/toolkit/layout.h"

namespace midi_learn {

namespace {
Learner* gActive = nullptr;
const char* ARROW = " \xe2\x80\xba ";  // " › "
}  // namespace

Learner* active() { return gActive; }
void setActive(Learner* learner) { gActive = learner; }

// --- names ---------------------------------------------------------------------------------------

TrackPtr ownerOf(const Project& project, const PluginSlot* slot) {
    if (!slot) return nullptr;
    for (auto& t : project.tracks) {
        if (t->controls.get() == slot || t->instrument.get() == slot) return t;
        for (auto& s : t->rack)
            if (s.get() == slot) return t;
        for (auto& s : t->effects)
            if (s.get() == slot) return t;
    }
    return nullptr;
}

std::string targetName(const Project& project, const TrackPtr& track, const SlotPtr& slot, const std::string& param) {
    (void)project;
    std::string where = track ? track->name : std::string("Master");
    if (slot && slot->isControls()) return where + ARROW + param;
    return where + ARROW + (slot ? slot->name : std::string("?")) + ARROW + param;
}

std::string targetName(const Project& project, const MidiLink& link) {
    return targetName(project, link.track, link.slot, link.param);
}

std::string controlName(const MidiLink& link) {
    std::string s = "CC " + std::to_string(link.cc) + ", channel " + std::to_string(link.channel + 1);
    if (!link.controller.empty()) s += " (" + link.controller + ")";
    return s;
}

bool present(const Project& project, const MidiLink& link) {
    if (!link.slot) return false;
    if (!link.track) return project.masterEffects.contains(link.slot);
    if (!project.tracks.contains(link.track)) return false;
    const Track& t = *link.track;
    return link.slot == t.controls || (t.instrument && link.slot == t.instrument) || t.rack.contains(link.slot) ||
           t.effects.contains(link.slot);
}

std::vector<std::string> parameterNames(const PluginSlot& slot) {
    if (slot.isControls()) return {automation::VOLUME, automation::PAN};
    std::vector<std::string> out;
    for (auto& p : automation::parameters(slot)) out.push_back(p.name);
    return out;
}

// --- the learner ---------------------------------------------------------------------------------

Learner::Learner(Project& p) : project(p) {}

void Learner::say(const std::string& text, int ms) {
    if (status) status(text, ms);
}

void Learner::learn(const TrackPtr& track, const SlotPtr& slot, const std::string& param) {
    if (!slot || param.empty()) return;
    waiting_ = Waiting{track, slot, param};
    std::string text = "Turn a knob on your MIDI controller to link it to " + targetName(project, track, slot, param) +
                       " (Esc cancels)";
    if (inputReady && !inputReady()) text += ". No MIDI input is chosen yet: Options \xe2\x86\x92 MIDI input\xe2\x80\xa6";
    say(text, 0);
}

void Learner::cancel() {
    if (!waiting_) return;
    waiting_.reset();
    say("Linking to a MIDI controller cancelled", 2500);
}

std::string Learner::waitingFor() const {
    return waiting_ ? targetName(project, waiting_->track, waiting_->slot, waiting_->param) : std::string();
}

bool Learner::handle(const midi_in::Event& e, const std::string& controller) {
    if (e.type != midi_in::Event::Control || e.data1 >= FIRST_MODE_CC) return false;
    if (waiting_) {
        Waiting w = *waiting_;
        waiting_.reset();
        MidiLink link;
        link.controller = controller;
        link.channel = e.channel & 15;
        link.cc = e.data1 & 127;
        link.track = w.track;
        link.slot = w.slot;
        link.param = w.param;
        if (!present(project, link)) {
            say("Not linked: " + targetName(project, link) + " is gone", 4000);
            return true;
        }
        add(link);
        say(controlName(link) + " now moves " + targetName(project, link), 5000);
        return true;
    }
    bool used = false;
    double value = std::max(0, std::min(127, (int)e.data2)) / 127.0;
    std::vector<MidiLink> links = project.midiLinks;  // (a copy: applying may call back into the GUI)
    for (const MidiLink& link : links)
        if (link.cc == e.data1 && link.channel == (e.channel & 15)) used = apply(link, value) || used;
    return used;
}

bool Learner::findParam(const SlotPtr& slot, const std::shared_ptr<plug::Plugin>& inst, const std::string& param,
                        uint32_t& id) {
    auto key = std::make_pair(slot.get(), param);
    auto it = ids_.find(key);
    if (it != ids_.end() && it->second.plugin.lock() == inst) {
        id = it->second.id;
        return true;
    }
    for (auto& p : automation::parameters(*slot))
        if (p.name == param) {
            ids_[key] = Found{inst, p.id};
            id = p.id;
            return true;
        }
    return false;
}

bool Learner::apply(const MidiLink& link, double value) {
    if (!present(project, link)) return false;
    value = std::max(0.0, std::min(1.0, value));
    if (link.slot->isControls()) {  // a track's fader and pan, as dragging them sets them
        if (!link.track) return false;
        if (link.param == automation::VOLUME) {
            link.track->volumeDb = std::round(automation::volumeOf(value) * 10) / 10;  // (0.1 dB steps)
        } else if (link.param == automation::PAN) {
            link.track->pan = std::round(automation::panOf(value) * 100) / 100;  // (as the knob rounds it)
        } else {
            return false;
        }
        if (mixerMoved) mixerMoved(link.track);
        // (an armed lane of it writes the movement: automation_write.h; a turn of the knob is a touch ending when
        // the turning stops)
        if (AutomationPtr lane = automation::armedControlLane(*link.track, link.param))
            automation::recorder().move(lane, value);
        return true;
    }
    if (link.track && std::atomic_load(&link.track->frozen)) return false;  // (its plugins are locked)
    auto inst = aload(link.slot->instance);
    if (!inst || !plugin_audio::usable(link.slot)) return false;
    uint32_t id = 0;
    if (!findParam(link.slot, inst, link.param, id)) return false;
    inst->windowEdit(id, value);  // (as a turn of its knob in its window: edited() lists it)
    if (!windowOpen || !windowOpen(link.slot.get())) {
        // no window open, so no TweakWatcher asks the plugin: modulation's base follows here
        std::vector<uint32_t> edited = inst->edited();
        modulation::userEdited(project, link.slot.get(), edited);
        // …and an armed lane of it writes the movement (with the window open, its TweakWatcher does)
        if (AutomationPtr lane = automation::armedLane(project, link.slot.get(), link.param))
            automation::recorder().move(lane, value);
    }
    if (pluginMoved) pluginMoved(link.slot);
    return true;
}

std::vector<size_t> Learner::linksOf(const PluginSlot* slot, const std::string& param) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < project.midiLinks.size(); i++)
        if (project.midiLinks[i].slot.get() == slot && project.midiLinks[i].param == param) out.push_back(i);
    return out;
}

void Learner::remove(size_t index) {
    if (index >= project.midiLinks.size()) return;
    project.midiLinks.erase(project.midiLinks.begin() + (long)index);
    if (changed) changed();
}

void Learner::removeLinksOf(const PluginSlot* slot, const std::string& param) {
    auto& links = project.midiLinks;
    size_t before = links.size();
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const MidiLink& l) { return l.slot.get() == slot && l.param == param; }),
                links.end());
    if (links.size() != before && changed) changed();
}

void Learner::add(const MidiLink& link) {
    auto& links = project.midiLinks;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const MidiLink& l) { return l.slot == link.slot && l.param == link.param; }),
                links.end());
    links.push_back(link);
    if (changed) changed();
}

// --- menus ---------------------------------------------------------------------------------------

void fillParamMenu(ui::Menu& menu, const TrackPtr& track, const SlotPtr& slot, const std::string& param,
                   const std::string& label) {
    Learner* learner = active();
    if (!learner || !slot || param.empty()) return;
    std::string target = targetName(learner->project, track, slot, param);
    ui::Action* a = menu.addAction(label.empty() ? "Link to a MIDI controller\xe2\x80\xa6" : label, [track, slot, param] {
        if (Learner* l = active()) l->learn(track, slot, param);
    });
    a->tooltip = "Then turn a knob on your MIDI controller: it moves " + target + " from then on (Esc cancels)";
    std::vector<size_t> links = learner->linksOf(slot.get(), param);
    if (!links.empty()) {
        const MidiLink& link = learner->project.midiLinks[links[0]];
        ui::Action* r = menu.addAction("Remove the MIDI controller link (" + controlName(link) + ")", [slot, param] {
            if (Learner* l = active()) l->removeLinksOf(slot.get(), param);
        });
        r->tooltip = "The controller's knob no longer moves " + target;
    }
}

void chooseAndLearn(ui::Window* parent, const TrackPtr& track, const SlotPtr& slot) {
    Learner* learner = active();
    if (!learner || !slot) return;
    std::vector<std::string> names = parameterNames(*slot);
    if (names.empty()) {
        ui::msg::information(parent, "Link to a MIDI controller",
                             slot->name + " has no parameters to link (is it loaded?).");
        return;
    }
    std::vector<std::string> shown = names;
    for (auto& n : shown)
        if (!learner->linksOf(slot.get(), n).empty()) n += "   (linked)";
    int index = 0;
    std::string where = slot->isControls() ? (track ? track->name : std::string()) : slot->name;
    if (!ui::getItem(parent, "Link to a MIDI controller",
                     "The parameter of " + where + " to move with a knob of your MIDI controller:", shown, index))
        return;
    if (index < 0 || index >= (int)names.size()) return;
    if (Learner* l = active()) l->learn(track, slot, names[index]);
}

// --- Options → MIDI controller links… -------------------------------------------------------------

LinksDialog::LinksDialog(ui::Window* parent, Learner& l)
    : ui::Dialog(parent, "MIDI controller links", 560, 360), learner(l) {
    ui::VBox* col = ui::vbox(this, 12, 8);
    auto* intro = add<ui::Label>(
        "Knobs of your MIDI controller and what each moves. To link one, right-click a knob (an effect's, in "
        "its window) or a track's row (its fader, pan or a plugin's parameter) and choose \xe2\x80\x9cLink to a MIDI "
        "controller\xe2\x80\xa6\xe2\x80\x9d, then turn the knob.");
    intro->setWordWrap(true);
    col->addWidget(intro);
    list = add<ui::ListWidget>();
    list->setMultiSelection(true);
    list->setToolTip("Each link: the controller's knob (its control number and channel), then what it moves");
    col->addWidget(list, 1);
    empty = add<ui::Label>("No links yet.");
    empty->setProperty("hint", "true");  // (the skin's hint colour: Label.hint)
    col->addWidget(empty);
    auto row = std::make_unique<ui::HBox>();
    removeButton = add<ui::Button>("Remove");
    removeButton->setToolTip("The selected links go: their knobs no longer move anything");
    removeButton->clicked.connect([this] { removeSelected(); });
    row->addWidget(removeButton);
    row->addStretch(1);
    auto* close = add<ui::Button>("Close");
    close->setToolTip("Close this window (the links stay)");
    close->clicked.connect([this] { accept(); });
    row->addWidget(close);
    col->addLayout(std::move(row));
    list->currentRowChanged.connect([this](int) { removeButton->setEnabled(!list->selectedRows().empty()); });
    list->itemClicked.connect([this](int) { removeButton->setEnabled(!list->selectedRows().empty()); });
    fill();
}

void LinksDialog::fill() {
    list->clear();
    for (size_t i = 0; i < learner.project.midiLinks.size(); i++) {
        const MidiLink& link = learner.project.midiLinks[i];
        ui::ListWidget::Item item;
        bool there = present(learner.project, link);
        item.text = controlName(link) + "  \xe2\x86\x92  " + targetName(learner.project, link) +
                    (there ? std::string() : "   (gone)");
        item.data = std::to_string(i);
        item.tooltip = there ? "Turning this knob moves " + targetName(learner.project, link)
                             : "Its track or plugin was removed (undo may bring it back); it moves nothing now";
        list->addItem(item);
    }
    empty->setVisible(list->count() == 0);
    removeButton->setEnabled(!list->selectedRows().empty());
}

void LinksDialog::removeSelected() {
    std::vector<int> rows = list->selectedRows();
    if (rows.empty() && list->currentRow() >= 0) rows.push_back(list->currentRow());
    std::sort(rows.rbegin(), rows.rend());  // (from the end: the indices before stay valid)
    for (int r : rows)
        if (r >= 0 && r < (int)learner.project.midiLinks.size()) learner.remove((size_t)r);
    fill();
}

void showLinks(ui::Window* parent, Learner& learner) {
    LinksDialog d(parent, learner);
    d.exec();
}

}  // namespace midi_learn
