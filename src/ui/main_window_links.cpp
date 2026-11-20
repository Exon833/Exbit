// The main window (main_window.py): what it does with the newer screens' modules — the clips'
// clipboard, insert / delete time, Capture, MIDI input and the computer keyboard, MIDI export, stems,
// the recording offset, the Ableton Live import. See main_window.h.
#include <algorithm>

#include "audio/automation_write.h"
#include "audio/midi_effects.h"
#include "core/util.h"
#include "ui/ableton_import.h"
#include "ui/capture.h"
#include "ui/clipboard.h"
#include "ui/computer_keyboard.h"
#include "ui/main_window.h"
#include "ui/midi_editor.h"
#include "ui/midi_effects_dialog.h"
#include "ui/midi_export.h"
#include "ui/midi_input.h"
#include "ui/mix_export.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_host.h"
#include "ui/record_offset_dialog.h"
#include "ui/recording.h"
#include "ui/sampler_dialog.h"
#include "ui/song_scale.h"
#include "ui/synth_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/transport_bar.h"

using ui::App;

// --- Edit menu ---------------------------------------------------------------------------------

void MainWindow::useClipboard(const std::string& what) {
    // cut, copy or paste clips (clipboard.h), saying what happened in the status bar
    showStatus(clipboard::use(*timeline, what), 4000);
}

bool MainWindow::dragging() {
    if (!std::holds_alternative<std::monostate>(timeline->drag)) return true;
    for (auto& [clip, editor] : midiEditors) {
        (void)clip;
        if (editor->drag.kind != MidiEditor::Drag::None) return true;
    }
    return false;
}

void MainWindow::reloadEditors() {
    for (auto& [clip, editor] : midiEditors) {
        (void)clip;
        editor->reload();
    }
    midi_effects_dialog::reloadAll();  // (they show the track's MIDI effects or sampler as they are now)
    sampler_dialog::reloadAll();
    synth_dialog::reloadAll();
    pad_kit_dialog::reloadAll();
}

std::string MainWindow::insertTime() { return timeline->insertTime(); }

std::string MainWindow::deleteTime() { return timeline->deleteTime(); }

void MainWindow::addCaptureAction(ui::Menu* menu) {
    // Capture (capture.h): Ctrl+Alt+R from every window (registered with the transport's shortcuts)
    // (the transport's action owns the shortcut, application-wide: shown here only)
    menu->addAction("Ca&pture\tCtrl+Alt+R", [this] { capture(); });
}

void MainWindow::capture() {
    if (!midiInput) return;
    capture::Host h;
    h.input = midiInput.get();
    h.cursor = [this] { return timeline->cursor; };
    h.placeClip = [this](const ClipPtr& c, int64_t f, const TrackPtr& t) { timeline->placeClip(c, f, t); };
    h.nextTake = [this] { return ++recording->takes; };
    h.status = [this](const std::string& t, int ms) { showStatus(t, ms); };
    capture::capture(h);
}

// --- File menu additions -----------------------------------------------------------------------

void MainWindow::importAbleton() {
    ableton_import::Hooks hooks;
    hooks.confirmDiscard = [this] { return confirmDiscard(); };
    hooks.stop = [this] { stop(); };
    hooks.openProjectFile = [this](const std::string& path) { return openProjectFile(path); };
    ableton_import::run(this, hooks);
}

void MainWindow::exportStems() { exporter->exportStems(); }

void MainWindow::exportMidi(const std::vector<ClipPtr>& clips) {
    midi_export::exportMidi(this, clips, projectPath, project, engine.sr,
                            [this](const std::string& t, int ms) { showStatus(t, ms); });
}

// --- Options -----------------------------------------------------------------------------------

void MainWindow::addMidiOptions(ui::Menu* menu) {
    menu->addAction("Recording &offset…", [this] { showRecordOffset(); });
    menu->addAction("&MIDI ports…", [this] { chooseMidiInput(); })->tooltip =
        "Every MIDI input and output: on or off, MIDI clock in and out (several at once)";
    // the computer keyboard as a piano (computer_keyboard.h)
    keysAction = menu->addCheckable("Computer &keyboard", computer_keyboard::savedOn(), [this](bool on) {
        transport->keysAction->setChecked(on);  // (its toggled turns the keyboard on or off)
    });
    keysAction->tooltip = computer_keyboard::LABEL_TIP;
}

void MainWindow::showRecordOffset() {
    RecordOffsetDialog d(this, prefs, *recording, project, engine.sr);
    d.exec();
}

void MainWindow::chooseMidiInput() { showMidiPorts(); }

void MainWindow::setUpMidi() {
    // MIDI input (Options → MIDI input…), the computer keyboard (Ctrl+K), Capture, the MIDI windows' host
    midiInput = std::make_unique<midi_input::MidiInput>(
        engine, project, [this] { return timeline->currentTrack; },
        [this](const std::string& m, int ms) { showStatus(m, ms); });
    setUpMidiPorts();  // (Options → MIDI ports…: main_window_midi.cpp)
    // while recording MIDI, every note played (the pedal taken into account) goes to the take
    midiInput->recorder = [this](const midi_in::Event& e) {
        if (!recording->recordingMidi()) return;
        // (each armed track records what its MIDI input lets through: All, or that port and channel)
        std::string port = midiInput->portName(e.port);
        auto to = [&](const Track& t) {
            return e.port < 0 || t.midiIn.empty() ||
                   (t.midiIn == port && (t.midiInChannel == 0 || t.midiInChannel == e.channel + 1));
        };
        if (e.type == midi_in::Event::NoteOn) recording->midiNote(e.data1, e.data2, to);
        else if (e.type == midi_in::Event::NoteOff) recording->midiNote(e.data1, 0, to);
    };
    keyboard = std::make_unique<computer_keyboard::ComputerKeyboard>(
        *midiInput, [this](const std::string& m, int ms) { showStatus(m, ms); });
    keyboard->showState = [this](bool visible, const std::string& text) {
        status->setPermanentText(visible ? text : "", computer_keyboard::LABEL_TIP);
    };
    bool on = computer_keyboard::savedOn();
    keyboard->setOn(on, false);
    if (keysAction) keysAction->checked = on;
    transport->keysAction->setCheckedSilently(on);
    // every window's keys go through it first (the toolkit's application-wide filter)
    App::get().keyFilter = [this](ui::Window* w, const ui::KeyEvent& ke, bool press) {
        if (!keyboard || closing) return false;
        plat::Event e;
        e.type = press ? plat::Event::KeyDown : plat::Event::KeyUp;
        e.key = ke.key;
        e.mods = ke.mods;
        e.text = ke.text;
        e.autoRepeat = ke.autoRepeat;
        return keyboard->filter(w, e);
    };
    App::get().deactivated.connect([this] {  // (its key releases would go to another program: no note may stick)
        if (keyboard) keyboard->releaseAll();
    });
    sampler_dialog::Host sh;
    sh.window = this;
    sh.project = &project;
    sh.engine = &engine;
    sh.relayout = [this] { timeline->relayout(); };
    sh.tracksChanged = [this] {
        timeline->relayout();
        timeline->tracksChanged.emit();
    };
    sh.status = [this](const std::string& text) { showStatus(text, 5000); };
    sampler_dialog::setHost(sh);
    midi_effects::install(&project.timing);
    midi_effects_dialog::Hooks fx;  // (Commit to notes)
    fx.selectedClips = [this] { return timeline->selection.clipList(); };
    fx.loadPlugins = [this](const std::vector<SlotPtr>& slots) { return plugins->restore(slots); };
    fx.committed = [this](const midi_commit::Result& r, const std::string& text) {
        if (r.track) timeline->setCurrentTrack(r.track);
        timeline->relayout();
        timeline->tracksChanged.emit();  // (the rows: a new track)
        if (r.clips && song_scale::notesCommitted) song_scale::notesCommitted();  // (open MIDI editors: the notes)
        showStatus(text, 6000);
    };
    midi_effects_dialog::setHooks(fx);
}

// --- the project replaced, the sample rate, closing ---------------------------------------------

void MainWindow::projectReplaced() {
    automation::recorder().releaseAll();  // (curves being written belonged to the project replaced)
    midi_effects::install(&project.timing);  // (their beats follow the project's timing)
    midi_effects_dialog::closeGone(project);
    sampler_dialog::closeGone(project);
    synth_dialog::closeGone(project);
    pad_kit_dialog::closeGone(project);
}

void MainWindow::sampleRateChanged(int oldSr, int newSr) { timeline->changeSampleRate(oldSr, newSr); }

void MainWindow::beforeClose() {
    App::get().keyFilter = nullptr;
    keyboard.reset();  // (lets every key go)
    if (midiInput) midiInput->close();
}

// --- the grid (the transport's Grid ▾ button, Ctrl+1/2/4/5) -------------------------------------

void MainWindow::fillGridMenu(ui::Menu* menu) {
    // following the zoom, the fixed steps, snapping
    menu->clear();
    Timing& timing = project.timing;
    ui::Action* zoom = menu->addCheckable("Follows the zoom\tCtrl+5", !timing.fixedStep());
    zoom->group = 1;
    zoom->triggered = [this] { changeGrid(0.0); };
    double saved = timing.grid;
    std::optional<Frac> current = timing.fixedStep();  // (the step in use, thirds counted)
    for (const Frac& step : timing::fixedSteps(timing.thirds)) {
        timing.grid = step.value();
        std::optional<Frac> mine = timing.fixedStep();
        ui::Action* a = menu->addCheckable(timing.gridName(), current && mine && *mine == *current);
        a->group = 1;
        double value = step.value();
        a->triggered = [this, value] { changeGrid(value); };
    }
    timing.grid = saved;
    menu->addSeparator();
    ui::Action* snap = menu->addCheckable("Snap to the grid\tCtrl+4", timing.snapOn);
    snap->triggered = [this] { changeGrid({}, !project.timing.snapOn); };
    menu->addAction("Finer\tCtrl+1", [this] { changeGrid({}, {}, true); });
    menu->addAction("Coarser\tCtrl+2", [this] { changeGrid({}, {}, false); });
}

void MainWindow::changeGrid(std::optional<double> grid, std::optional<bool> snap, std::optional<bool> finer) {
    // a fixed grid (beats; 0: follows the zoom), snapping on or off, or a finer / coarser fixed grid
    Timing& timing = project.timing;
    if (!timing.beats) {
        showStatus("The grid is for bars and beats: switch ♩ Beats on (B)", 3000);
        return;
    }
    if (finer)  // (from the grid shown where the view is, when it follows the zoom)
        timing.stepGrid(*finer, timeline->pps, scroll->horizontalScrollBar()->value() / timeline->pps);
    if (grid) timing.grid = *grid;
    if (snap) timing.snapOn = *snap;
    transport->showTiming(timing);
    timeline->update();
    for (auto& [clip, editor] : midiEditors) {
        (void)clip;
        editor->refresh();
    }
    showStatus("Grid: " + timing.gridName() + "; snapping " + (timing.snapOn ? "on" : "off"), 2500);
}
