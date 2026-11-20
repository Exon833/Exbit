// A MIDI track's MIDI effects window (midi_effects_dialog.py).
#include "ui/midi_effects_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/midi_effects.h"
#include "core/util.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"
#include "ui/widgets.h"

namespace midi_effects_dialog {

namespace {
std::map<const Track*, MidiEffectsDialog*> gWindows;
Hooks gHooks;

std::string itemText(const MidiEffect& e) { return e.kind + (e.bypass ? "  (off)" : ""); }

}  // namespace

double Param::toNormal(double value) const {
    if (!choices.empty()) return std::min(1.0, std::max(0.0, value / std::max<double>(1, (double)choices.size() - 1)));
    value = std::min(high, std::max(low, value));
    if (scale == "log") return std::log(value / low) / std::log(high / low);
    return (value - low) / (high - low);
}

double Param::fromNormal(double normal) const {
    normal = std::min(1.0, std::max(0.0, normal));
    if (!choices.empty()) return std::nearbyint(normal * ((double)choices.size() - 1));
    if (scale == "log") return low * std::pow(high / low, normal);
    double value = low + normal * (high - low);
    return scale == "int" ? std::nearbyint(value) : value;
}

std::string Param::text(double value) const {
    if (!choices.empty()) return choices[std::min(choices.size() - 1, (size_t)std::max(0L, std::lround(value)))];
    int d = decimals;
    if (d < 0) {
        double span = scale == "log" ? std::fabs(value) : high - low;
        d = scale == "int" || span >= 100 ? 0 : span >= 10 ? 1 : 2;
    }
    char buf[64];
    if (unit == "Hz" && value >= 1000) {
        std::snprintf(buf, sizeof buf, "%.2f kHz", value / 1000);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.*f", d, value);
    return std::string(buf) + (unit.empty() || unit == ":1" ? unit : " " + unit);
}

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = [] {  // (audio/midi_effects.h's, with the window's text and scales)
        std::vector<Kind> v;
        for (const std::string& name : midi_effects::kinds()) {
            Kind kind;
            kind.name = name;
            kind.live = midi_effects::isLive(name);
            for (const midi_effects::ParamInfo& info : midi_effects::params(name)) {
                Param p;
                p.key = info.key;
                p.name = info.name;
                p.low = info.low;
                p.high = info.high;
                p.def = info.defaultValue;
                p.unit = info.unit;
                p.scale = info.scale.empty() ? "linear" : info.scale;
                p.choices = info.choices;
                p.decimals = info.decimals;
                kind.params.push_back(p);
            }
            v.push_back(kind);
        }
        return v;
    }();
    return k;
}

const Kind* kindOf(const std::string& name) {
    for (const Kind& k : kinds())
        if (k.name == name) return &k;
    return nullptr;
}

// =================================================================================================

MidiEffectsDialog::MidiEffectsDialog(ui::Window* window, TrackPtr track_)
    : ui::Dialog(window, "MIDI effects \xe2\x80\x94 " + track_->name, 640, 360), track(std::move(track_)) {
    deleteOnClose = true;
    setStyleType("MidiEffectsDialog");
    ui::HBox* row = ui::styleRoom(ui::hbox(this, 9, 9), computedStyle());
    auto left = std::make_unique<ui::VBox>();
    left->addWidget(add<ui::Label>("They change the notes in this order, before the instrument:"));
    list = add<ui::ListWidget>();
    list->currentRowChanged.connect([this](int r) {
        if (!filling_) showSettings(r);
    });
    left->addWidget(list, 1);
    auto buttons = std::make_unique<ui::HBox>();
    addButton = ui::toolButton(this, "Add");  // (its menu gives it the ▾)
    ui::Menu* menu = widgets::ownedMenu(addButton);
    for (const Kind& k : kinds()) {
        std::string name = k.name;
        ui::Action* item = menu->addAction(name + (k.live ? "" : "  (timing)"), [this, name] { addEffect(name); });
        item->tooltip = midi_effects::describe(name);
    }
    addButton->setMenu(menu);
    buttons->addWidget(addButton);
    struct B { const char* text; const char* tip; std::function<void()> act; };
    for (const B& b : {B{"Remove", "Remove the chosen effect", [this] { remove(); }},
                       B{"\xe2\x86\x91", "Earlier", [this] { move(-1); }}, B{"\xe2\x86\x93", "Later", [this] { move(1); }}}) {
        ui::Button* button = ui::pushButton(this, b.text, b.act);
        button->setToolTip(b.tip);
        buttons->addWidget(button);
    }
    left->addLayout(std::move(buttons));
    commitButton = ui::toolButton(this, "Commit to notes");  // (its menu gives it the ▾)
    commitButton->setToolTip("Write the notes the effects make (with the song's and the track's swing) into notes: "
                             "the clip sounds the same, and the effects it holds now are switched off. One undo step");
    ui::Menu* commitMenu = widgets::ownedMenu(commitButton);
    commitMenu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        bool selected = false;
        if (gHooks.selectedClips)
            for (auto& c : gHooks.selectedClips()) selected = selected || (c->track == track && c->kind == ::Kind::Midi);
        struct C { const char* text; bool every, fresh; const char* tip; };
        for (const C& c : {C{"The selected clip, into its notes", false, false, "The clips of this track selected on the timeline"},
                           C{"The selected clip, onto a new track", false, true, "A new clip on a new MIDI track below; the original is kept, switched off"},
                           C{"Every clip of the track, into their notes", true, false, "The effects are then switched off"},
                           C{"Every clip of the track, onto a new track", true, true, "New clips on a new MIDI track below; the originals are kept, switched off"}}) {
            bool every = c.every, fresh = c.fresh;
            ui::Action* a = m->addAction(c.text, [this, every, fresh] { commit(every, fresh); });
            a->tooltip = c.tip;
            a->enabled = every || selected;
        }
    };
    commitButton->setMenu(commitMenu);
    left->addWidget(commitButton);
    ui::Label* hint = add<ui::Label>("They also change notes played live (the MIDI editor's keys, a MIDI keyboard); "
                                     "Arpeggiator, Note Length and Groove time them on the song's beats.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    left->addWidget(hint);
    row->addLayout(std::move(left), 1);
    settingsPane = add<ui::Widget>();
    settingsPane->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    row->addWidget(settingsPane, 1);
    fillList();
}

MidiEffectsDialog::~MidiEffectsDialog() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
}

void MidiEffectsDialog::fillList(int current) {
    filling_ = true;
    list->clear();
    for (const MidiEffect& e : track->midiEffects) list->addItem(itemText(e));
    filling_ = false;
    list->setCurrentRow(std::min(current, list->count() - 1));
    showSettings(list->currentRow());
}

void MidiEffectsDialog::reload() { fillList(std::max(0, list->currentRow())); }

void MidiEffectsDialog::changed(std::vector<MidiEffect> effects, int current) {
    track->midiEffects = std::move(effects);  // (replaced whole: undo, and the clips, see a whole chain)
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    renderTimer_ = ui::App::get().singleShot(RENDER_DELAY_MS, [this] {
        renderTimer_ = -1;
        sampler_dialog::renderTrackClips(track);
    });
    if (current >= 0) fillList(current);
}

void MidiEffectsDialog::addEffect(const std::string& kind) {
    MidiEffect e;
    if (!MidiEffect::make(kind, {}, e)) return;
    std::vector<MidiEffect> effects = track->midiEffects.copy();
    effects.push_back(e);
    changed(effects, (int)effects.size() - 1);
}

void MidiEffectsDialog::remove() {
    int row = list->currentRow();
    if (row < 0) return;
    std::vector<MidiEffect> effects;
    for (int i = 0; i < (int)track->midiEffects.size(); ++i)
        if (i != row) effects.push_back(track->midiEffects[i]);
    changed(effects, std::max(0, row - 1));
}

void MidiEffectsDialog::move(int step) {
    int row = list->currentRow();
    std::vector<MidiEffect> effects = track->midiEffects.copy();
    if (row >= 0 && row < (int)effects.size() && row + step >= 0 && row + step < (int)effects.size()) {
        std::swap(effects[row], effects[row + step]);
        changed(effects, row + step);
    }
}

void MidiEffectsDialog::setValue(int row, const std::string& key, double value) {
    std::vector<MidiEffect> effects = track->midiEffects.copy();
    if (row >= 0 && row < (int)effects.size()) {
        effects[row] = effects[row].with(key, value);
        changed(effects);
    }
}

void MidiEffectsDialog::setBypass(int row, bool bypass) {
    std::vector<MidiEffect> effects = track->midiEffects.copy();
    if (row < 0 || row >= (int)effects.size() || effects[row].bypass == bypass) return;
    effects[row].bypass = bypass;
    util::debugf("midi effects: \"%s\": %s %s\n", track->name.c_str(), effects[row].kind.c_str(),
                 bypass ? "switched off (bypassed)" : "switched on");
    if (row < list->count()) {  // (the list's text; the settings stay as they are: this box is among them)
        list->item(row).text = itemText(effects[row]);
        list->update();
    }
    changed(effects);
}

midi_commit::Result MidiEffectsDialog::commit(bool everyClip, bool newTrack) {
    midi_commit::Result r;
    const sampler_dialog::Host& h = sampler_dialog::host();
    if (!h.project || !h.engine) return r;
    if (renderTimer_ >= 0) {  // (settings just changed: the clips as they will sound)
        ui::App::get().cancelTimer(renderTimer_);
        renderTimer_ = -1;
    }
    std::vector<ClipPtr> clips;
    if (!everyClip && gHooks.selectedClips)
        for (auto& c : gHooks.selectedClips())
            if (c->track == track && c->kind == ::Kind::Midi) clips.push_back(c);
    if (!everyClip && clips.empty()) {
        r.error = "Select a MIDI clip of " + track->name + " on the timeline first";
    } else {
        r = midi_commit::commit(*h.project, h.engine->sr, track, clips, newTrack, gHooks.loadPlugins);
    }
    std::string text;
    if (!r.error.empty()) {
        text = "Commit to notes: " + r.error;
    } else {
        text = "Committed " + std::to_string(r.clips) + (r.clips == 1 ? " clip" : " clips") + " to notes";
        if (r.track) text += " on the new track " + r.track->name + " (the originals are kept, switched off)";
        if (r.bypassed) text += "; " + std::to_string(r.bypassed) + " effects switched off";
    }
    util::debugf("midi effects: %s\n", text.c_str());
    if (r.error.empty() && !r.track) sampler_dialog::renderTrackClips(track);  // (its takes too)
    fillList(std::max(0, list->currentRow()));
    if (gHooks.committed) gHooks.committed(r, text);
    return r;
}

void MidiEffectsDialog::showSettings(int row) {
    settingsPane->clearChildren();
    ui::FormLayout* form = ui::styleRoom(ui::form(settingsPane, 0, 6), computedStyle("settings"));
    if (row < 0 || row >= (int)track->midiEffects.size()) {
        form->addRow(settingsPane->add<ui::Label>(track->midiEffects.empty() ? "Add an effect with Add \xe2\x96\xbe." : ""));
        return;
    }
    const MidiEffect& effect = track->midiEffects[row];
    ui::Label* title = settingsPane->add<ui::Label>(effect.kind);
    title->addStyleClass("heading");  // (default.css: Label.heading, bold)
    form->addRow(title);
    ui::CheckBox* on = settingsPane->add<ui::CheckBox>("On");
    on->setChecked(!effect.bypass);
    on->setToolTip("Off: the effect stays in the chain but changes nothing (bypassed)");
    on->toggled.connect([this, row](bool v) { setBypass(row, !v); });
    form->addRow(on);
    const Kind* kind = kindOf(effect.kind);
    if (!kind) return;
    for (const Param& param : kind->params) {
        double value = effect.get(param.key);
        std::string key = param.key;
        if (!param.choices.empty()) {
            ui::ComboBox* box = settingsPane->add<ui::ComboBox>();
            for (const std::string& c : param.choices) box->addItem(c);
            box->setCurrentIndexSilently(std::min((int)param.choices.size() - 1, std::max(0, (int)std::lround(value))));
            box->currentIndexChanged.connect([this, row, key](int i) { setValue(row, key, (double)i); });
            form->addRow(param.name, box);
        } else {
            ui::Slider* slider = settingsPane->add<ui::Slider>(ui::Orientation::Horizontal);
            slider->setRange(0, STEPS);
            slider->setValueSilently((int)std::lround(param.toNormal(value) * STEPS));
            slider->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
            ui::Label* text = settingsPane->add<ui::Label>(param.text(value));
            text->setMinimumWidth(std::max(0, ui::style::px(computedStyle("value"), "min-width", 90)));
            Param p = param;
            slider->valueChanged.connect([this, row, p, text](int v) {
                double to = p.fromNormal((double)v / STEPS);
                text->setText(p.text(to));
                setValue(row, p.key, to);
            });
            auto line = std::make_unique<ui::HBox>();
            line->addWidget(slider, 1);
            line->addWidget(text);
            form->addRow(param.name, std::move(line));
        }
    }
    if (layout()) {  // (as Qt would: the window grows to fit the settings)
        Point need = layout()->minimumSize();
        if (need.x > width() || need.y > height()) resizeWindow(std::max(width(), need.x), std::max(height(), need.y));
    }
}

void MidiEffectsDialog::done(int result) {
    if (renderTimer_ >= 0) {
        ui::App::get().cancelTimer(renderTimer_);
        renderTimer_ = -1;
        sampler_dialog::renderTrackClips(track);
    }
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
    ui::Dialog::done(result);
}

MidiEffectsDialog* openFor(const Track* track) {
    auto it = gWindows.find(track);
    return it == gWindows.end() ? nullptr : it->second;
}

MidiEffectsDialog* openMidiEffects(const TrackPtr& track) {
    if (!track) return nullptr;
    MidiEffectsDialog* dialog = openFor(track.get());
    if (!dialog) {
        dialog = new MidiEffectsDialog(sampler_dialog::host().window, track);
        gWindows[track.get()] = dialog;
    }
    dialog->open();
    dialog->raiseWindow();
    return dialog;
}

void setHooks(Hooks hooks) { gHooks = std::move(hooks); }

void reloadAll() {
    for (auto& kv : gWindows) kv.second->reload();
}

void closeGone(const Project& project) {
    std::vector<MidiEffectsDialog*> gone;
    for (auto& kv : gWindows)
        if (!project.tracks.contains(kv.second->track)) gone.push_back(kv.second);
    for (MidiEffectsDialog* d : gone) d->close();
}

}  // namespace midi_effects_dialog
