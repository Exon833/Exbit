// The main window (main_window.h): presets (presets_ui.h: the effects', instruments' and tracks' menus,
// the Explorer's Presets place, dropping a track preset) and MIDI learn (midi_learn.h: MIDI controller
// links, Options → MIDI controller links…).
#include <cmath>

#include "plugins/plugin.h"
#include "ui/effect_window.h"
#include "ui/explorer.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/midi_input.h"
#include "ui/midi_learn.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_host.h"
#include "ui/presets_ui.h"
#include "ui/timeline.h"
#include "ui/widgets.h"

using ui::App;

void MainWindow::setUpPresets() {
    std::weak_ptr<bool> alive = alive_;
    // --- presets ---
    presets_ui::Host h;
    h.window = this;
    h.project = &project;
    h.plugins = plugins.get();
    h.sampleRate = [this] { return engine.sr; };
    h.status = [this](const std::string& t, int ms) { showStatus(t, ms); };
    h.nameStep = [this](const std::string& label) {
        noteChanges();  // (what changed before is a step of its own)
        if (history) history->nameNext(label);
    };
    h.trackChanged = [this](const TrackPtr& track) {
        timeline->setCurrentTrack(track);
        checkFxWindows();  // (the effects windows show the new chain; patch states)
        panel->rebuild();
        timeline->relayout();
        reloadEditors();   // (the MIDI effects and sampler windows show the track as it is now)
        noteChanges();     // (one undo step: "Load track preset")
    };
    h.newTrack = [this](Kind kind, const std::string& name) {
        TrackPtr t = timeline->addTrack(kind == Kind::Return ? Kind::Audio : kind);
        if (t && !name.empty()) t->name = name;
        return t;
    };
    h.presetsChanged = [this, alive] {
        if (!alive.expired() && explorer) explorer->presetsChanged();
    };
    h.settingsChanged = [this](const SlotPtr& slot) {
        if (EffectWindow* w = plugins->effectWindowOf(slot)) w->follow();
    };
    presets_ui::setHost(h);
    panel->fillPresetMenu = [this](ui::Menu& menu, const TrackPtr& track) {
        presets_ui::fillTrackMenu(menu, this, track);
    };
    // the Explorer's Presets place: dragged onto a track (or below them: a new track), or double-clicked
    timeline->dropPreset = [this, alive](const std::string& path, const TrackPtr& track) {
        std::string file = path;
        TrackPtr onto = track;
        App::get().singleShot(0, [this, alive, file, onto] {  // (once the drop is over: it may ask first)
            if (!alive.expired()) presets_ui::loadTrack(this, onto, file);
        });
    };
    explorer->openPreset = [this](const std::string& path) {
        TrackPtr track = timeline->currentTrack;
        if (track && !project.tracks.contains(track)) track = nullptr;
        presets_ui::loadTrack(this, track, path);
    };

    // --- MIDI learn ---
    learner = std::make_unique<midi_learn::Learner>(project);
    learner->status = [this](const std::string& t, int ms) { showStatus(t, ms); };
    learner->windowOpen = [this](const PluginSlot* slot) { return plugins->editors.count(const_cast<PluginSlot*>(slot)) > 0; };
    learner->mixerMoved = [this](const TrackPtr& track) { showMixerOf(track); };
    learner->pluginMoved = [this](const SlotPtr&) { plugins->edited = true; };  // (its settings may differ from the saved)
    learner->inputReady = [this] { return midiInput && midiInput->hub.anyInputOpen(); };
    midi_learn::setActive(learner.get());
    if (midiInput)
        midiInput->controlHook = [this](const midi_in::Event& e) {
            return learner && learner->handle(e, midiInput ? midiInput->portName(e.port) : std::string());
        };
    // Escape stops waiting for a controller's knob (before anything else sees the key)
    auto keys = App::get().keyFilter;
    App::get().keyFilter = [this, keys](ui::Window* w, const ui::KeyEvent& ke, bool press) {
        if (press && ke.key == plat::Key_Escape && learner && learner->waiting() && !closing) {
            learner->cancel();
            return true;
        }
        return keys ? keys(w, ke, press) : false;
    };
}

void MainWindow::addPresetOptions(ui::Menu* menu) {
    menu->addAction("MIDI c&ontroller links\xe2\x80\xa6", [this] { showMidiLinks(); })->tooltip =
        "The knobs of your MIDI controller linked to knobs of the program (right-click a knob or a track's row to "
        "link one), each removable";
}

void MainWindow::showMidiLinks() {
    if (learner) midi_learn::showLinks(this, *learner);
}

void MainWindow::showMixerOf(const TrackPtr& track) {
    auto it = panel->mixers.find(track);
    if (it == panel->mixers.end()) return;
    int db = (int)std::nearbyint(track->volumeDb);
    it->second.slider->setValueSilently(db);
    it->second.reading->showValue(db);
    it->second.pan->showValue(track->pan);
}
