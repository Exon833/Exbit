// The main window (main_window.py). See main_window.h.
#include "ui/main_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audio/automation.h"
#include "audio/modulation.h"
#include "audio/devices.h"
#include "audio/metering.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "config.h"
#include "core/util.h"
#include "model/project_io.h"
#include "plugins/library.h"
#include "ui/audio_dialog.h"
#include "ui/autosave.h"
#include "ui/colors.h"
#include "ui/cpu_monitor.h"
#include "ui/explorer.h"
#include "ui/instrument_menu.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/master_lane.h"
#include "ui/master_strip.h"
#include "ui/midi_input.h"
#include "ui/computer_keyboard.h"
#include "ui/metronome_dialog.h"
#include "ui/midi_editor.h"
#include "ui/mix_export.h"
#include "ui/overview.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_folders_dialog.h"
#include "ui/wine_prefixes_dialog.h"
#include "ui/wine_setup.h"
#include "ui/lanes.h"
#include "ui/scope_view.h"
#include "plugins/wine.h"
#include "ui/plugin_host.h"
#include "ui/recording.h"
#include "ui/skin_dialog.h"
#include "ui/templates.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"
#include "ui/track_render.h"
#include "ui/transport_bar.h"
#include "ui/update_notice.h"
#include "ui/widgets.h"

using ui::App;

namespace {

plat::WindowOptions mainOptions() {
    plat::WindowOptions o;
    o.title = "Untitled — Tiny DAW";
    o.w = 1280;
    o.h = 800;
    o.acceptsFiles = true;  // files dragged from the desktop onto the lanes
    return o;
}

std::string fmtG(double v) {
    char b[64];
    snprintf(b, sizeof b, "%g", v);
    return b;
}

// "Project overview" beside the overview: same height, its colours, a line under it
class OverviewCaption : public ui::Label {
public:
    OverviewCaption() : ui::Label("Project overview") { setAlignment(AlignCenter); }
    void paint(Painter& p) override {
        p.fillRect(rect(), theme::c("overview_bg"));
        p.fillRect(Rect(0, height() - 1, width(), 1), theme::c("overview_edge"));
        setColor(theme::c("ruler_text"));
        ui::Label::paint(p);
    }
};

// The Explorer's dock (QDockWidget "Explorer"): a title bar with ✕ over the Explorer
class ExplorerDock : public ui::Widget {
public:
    ExplorerDock(Explorer* explorer, std::function<void()> onClose) {
        auto* col = ui::vbox(this, 0, 0);
        auto* bar = add<ui::Widget>();
        bar->setFixedHeight(22);
        auto* row = ui::hbox(bar, 0, 0);
        row->setContentsMargins(6, 0, 2, 0);
        auto* title = bar->add<ui::Label>("Explorer");
        row->addWidget(title, 1);
        auto* x = bar->add<ui::Button>("✕");
        x->setStyle(ui::Button::Style::Tool);
        x->setProperty("autoRaise", "true");
        x->setFixedSize(18, 18);
        x->setToolTip("Close the Explorer (View → Explorer, Ctrl+E shows it again)");
        x->clicked.connect(std::move(onClose));
        row->addWidget(x);
        col->addWidget(bar);
        adopt(std::unique_ptr<ui::Widget>(explorer));
        col->addWidget(explorer, 1);
    }
};

}  // namespace

// ============================================================================================

MainWindowCore::MainWindowCore() : engine(project), prefs(Preferences::load()) {}

MainWindowCore::~MainWindowCore() {
    // the editors and finder first (they use the engine and project), then the rest
    keyboard.reset();  // (lets every key go)
    if (midiInput) midiInput->close();
    midiInput.reset();
    autosaver.reset();
    history.reset();
    recording.reset();
    exporter.reset();
    renderer.reset();
    plugins.reset();
    master.reset();
    engine.close();
}

MainWindow::MainWindow() : ui::Window(mainOptions()) {
    App::get().mainWindow = this;
    skin = skin::find(prefs.skin);  // before anything is built: widgets take its colours
    theme::rounded = prefs.roundedCorners;
    skin::apply(skin);
    metronomeSettings = metronome_dialog::loadSettings();
    engine.blocksize = prefs.bufferSize;
    engine.cores = prefs.processingCores;
    workers::setChosenCores(prefs.processingCores);

    menuBar = add<ui::MenuBar>();
    transport = add<TransportBar>();
    scroll = add<ui::ScrollArea>();
    timeline = scroll->makeContent<Timeline>(project, engine, scroll);
    wine::setPrefixes(prefs.wineList());  // (Windows plugins: the prefixes they're found in)
    wine::setWinePrograms(prefs.wineProgramMap());  // (…and the wine program chosen for some of them)
    plugins = std::make_unique<PluginHost>(this, project, engine, [this] { return prefs.vst2List(); });
    plugins->autoReload = prefs.reloadCrashedPlugins;
    // (hidden, for testing: more VST3 folders, separated by ':')
    if (const char* extra = getenv("TINYDAW_VST3_PATH")) {
        plugins->folders = plug::defaultFolders();
        for (auto& f : wine::vst3Folders(wine::prefixes())) plugins->folders.push_back(f);
        for (auto& f : util::split(extra, ':'))
            if (!f.empty()) plugins->folders.push_back(f);
    }
    // the master scope under the Master row (its options kept: options/scope); its height first
    ScopeSettings scopeSettings = ScopeSettings::fromJson(Json::parse(prefs.scope));
    lanes::setMasterScopeHeight(scopeSettings.shown ? config::SCOPE_H : 0);
    master = std::make_unique<MasterStrip>(engine, [this] { openMasterPlugins(); });
    master->scope->settings = scopeSettings;
    master->setScopeShown(scopeSettings.shown);
    master->scope->settingsChanged = [this] {
        prefs.scope = master->scope->settings.toJson().dump();
        prefs.save();
    };
    master->scope->hideRequested = [this] { showScope(false); };

    TrackRenderHost rh;
    rh.window = this;
    rh.stop = [this] { stop(); };
    rh.showMessage = [this](const std::string& t, int ms) { showStatus(t, ms); };
    rh.tracksRegrouped = [this] { timeline->tracksRegrouped(); };
    rh.placeClip = [this](const ClipPtr& c, int64_t f, const TrackPtr& t) { timeline->placeClip(c, f, t); };
    rh.closeEditors = [this](const std::vector<SlotPtr>& slots) { plugins->closeEditor(slots); };
    renderer = std::make_unique<TrackRenderer>(rh, engine);

    MixExportHost xh;
    xh.window = this;
    xh.stop = [this] { stop(); };
    xh.showMessage = [this](const std::string& t, int ms) { showStatus(t, ms); };
    xh.projectPath = [this] { return projectPath; };
    xh.masterDb = [this] { return (double)master->volume->value(); };
    xh.selection = [this](int64_t& start, int64_t& end) {
        if (!timeline->selection.range) return false;
        start = timeline->selection.range->start;
        end = timeline->selection.range->end;
        return true;
    };
    exporter = std::make_unique<MixExporter>(xh, engine);

    RecordingHost ch;
    ch.window = this;
    ch.prefs = &prefs;
    ch.metronome = &metronomeSettings;
    ch.currentTrack = [this] { return timeline->currentTrack; };
    ch.cursor = [this] { return timeline->cursor; };
    ch.seek = [this](int64_t f) { timeline->seek(f); };
    ch.stop = [this] { stop(); };
    ch.setPlaying = [this](bool on) { transport->setPlaying(on); };
    ch.setRecording = [this](bool on) { transport->setRecording(on); };
    ch.showRecording = [this](const TrackPtr& t, int64_t start) {
        if (t) timeline->recording = std::make_pair(t, start);
        else timeline->recording.reset();
        timeline->update();
    };
    ch.placeClip = [this](const ClipPtr& c, int64_t f, const TrackPtr& t) { timeline->placeClip(c, f, t); };
    ch.showMessage = [this](const std::string& t, int ms) { showStatus(t, ms); };
    ch.resetMeters = [this] { resetMeters(); };
    ch.lanesChanged = [this] { timeline->lanesChanged(); };
    ch.midiInputReady = [this] {  // a MIDI keyboard chosen, or the computer keyboard playing
        return (midiInput && !midiInput->name().empty()) || (keyboard && keyboard->isOn());
    };
    recording = std::make_unique<Recording>(ch, engine);

    panelScroll = add<ui::ScrollArea>();
    {
        auto p = std::make_unique<LanePanel>(
            project, *timeline, *master, engine.channels, [this](const TrackPtr& t) { openTrackPlugins(t); },
            [this](ui::Menu& m, const TrackPtr& t) { renderer->fillMenu(&m, t); },
            LanePanel::Instruments{[this](ui::Menu& m, const TrackPtr& t) { fillInstrumentMenu(m, t); },
                                   [this](const TrackPtr& t) { editInstrument(t); }});
        panel = p.get();
        panelScroll->setContent(std::move(p), false);  // (it sets its own height)
    }
    cpuMonitor = add<CpuMonitor>();
    overview = add<Overview>(project, engine, timeline, scroll, prefs.overviewH);
    overview->resizedTo.connect([this](int h) { overviewResized(h); });
    explorer = new Explorer([this](const std::string& path) { timeline->addFile(path, timeline->cursor); }, engine);
    status = add<ui::StatusBar>();
    plugins->statusBar = status;

    buildLayout();
    buildFileMenu();
    buildEditMenu();
    buildViewMenu();
    buildOptionsMenu();
    menuBar->registerShortcuts(ui::ShortcutScope::Window);
    // undo and redo from any of the program's windows (MIDI editor, FX windows too)
    for (ui::Action* a : {undoAction, redoAction}) a->shortcut = ui::KeySequence();
    extraShortcuts.push_back(std::make_unique<ui::Shortcut>(this, ui::KeySequence::parse("Ctrl+Z"), [this] { undo(); },
                                                            ui::ShortcutScope::ApplicationWide));
    for (const char* keys : {"Ctrl+Shift+Z", "Ctrl+Y"})
        extraShortcuts.push_back(std::make_unique<ui::Shortcut>(this, ui::KeySequence::parse(keys), [this] { redo(); },
                                                                ui::ShortcutScope::ApplicationWide));
    undoAction->shortcut = ui::KeySequence::parse("Ctrl+Z");  // (shown in the menu)
    redoAction->shortcut = ui::KeySequence::parse("Ctrl+Shift+Z");
    // shown only in the menu: the toolbar button owns the shortcut (registered after the others)
    for (auto& m : menuBar->menus())
        for (auto& a : m->actions())
            if (a->text.rfind("Add audio or MIDI", 0) == 0) a->shortcut = ui::KeySequence::parse("Ctrl+I");
    connectControls();
    startTimers();
    applyMeterOptions();
    updates = std::make_unique<UpdateChecker>();  // the notice under the master row: is there a newer version?
    updates->done.connect([this](const update_check::Result& r) { master->showUpdate(r); });
    if (!getenv("TINYDAW_NO_UPDATE_CHECK")) updates->start();  // (hidden: tests leave GitHub alone)
    openFirstDevice();
    setUpMidi();
    history = std::make_unique<history::History>(project, engine.sr);
    {
        std::weak_ptr<bool> alive = alive_;
        historyTimer_ = App::get().every(HISTORY_MS, [this, alive] {
            if (!alive.expired() && !closing) noteChanges();
        });
    }
    autosave::Host ah;  // (and crash recovery: autosave.h)
    ah.window = this;
    ah.hasUnsavedChanges = [this] { return hasUnsavedChanges(); };
    ah.snapshot = [this] { return snapshot(); };
    ah.recordingActive = [this] { return recording->active(); };
    ah.pluginsEdited = [this] { return plugins->edited; };
    ah.projectPath = [this] { return projectPath; };
    ah.describe = [this](const std::string& folder, std::map<std::string, std::string>& written,
                         std::vector<project_io::PendingAudio>& toWrite) {
        return project_io::describeRecovery(project, engine.sr, master->settings(), folder, written, toWrite);
    };
    ah.showMessage = [this](const std::string& t, int ms) { showStatus(t, ms); };
    ah.confirmDiscard = [this] { return confirmDiscard(); };
    ah.openProjectFile = [this](const std::string& path) { return openProjectFile(path); };
    ah.recovered = [this](const std::string& original) { recovered(original); };
    autosaver = std::make_unique<autosave::Autosave>(std::move(ah));
    markSaved();
    std::string start = templates::defaultTemplate(prefs);  // (Options → New projects start from)
    if (!start.empty()) startFromTemplate(start);
    closeRequested = [this] { return requestClose(); };
    timeline->setFocus();  // editing keys (Space, S, Delete…) work straight away
}

MainWindow::~MainWindow() {
    *alive_ = false;
    if (frameId_ >= 0) App::get().removeFrame(frameId_);
    if (cpuTimer_ >= 0) App::get().cancelTimer(cpuTimer_);
    if (historyTimer_ >= 0) App::get().cancelTimer(historyTimer_);
    for (auto& [key, w] : fxWindows) {
        (void)key;
        w->finished.disconnectAll();
    }
    for (auto& [key, e] : midiEditors) {
        (void)key;
        e->closed.disconnectAll();
    }
    fxWindows.clear();
    midiEditors.clear();
    updates.reset();
    masterPins.clear();
    if (plugins) {
        plugins->closeEditor();
        plugins->stopFindingKinds();
        plugins->statusBar = nullptr;
    }
    engine.close();
    if (App::get().mainWindow == this) App::get().mainWindow = nullptr;
}

void MainWindow::showStatus(const std::string& text, int ms) {
    if (status) status->showMessage(text, ms);
}

// --- setup ----------------------------------------------------------------------------------

void MainWindow::buildLayout() {
    // always shown, so the panel's matching bottom padding is always right
    scroll->setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);

    panelScroll->setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOff);
    panelScroll->setVerticalPolicy(ui::ScrollArea::Policy::AlwaysOff);
    panelScroll->setFixedWidth(config::PANEL_W);
    // the master lane and its row stay at the bottom of the screen, over the tracks scrolling beneath
    std::unique_ptr<ui::Widget> lane = timeline->takeMasterView();
    masterPins.push_back(std::make_unique<PinToBottom>(std::move(lane), scroll->viewport(), lanes::masterHeight()));
    masterPins.push_back(std::make_unique<PinToBottom>(master->releaseFrame(), panelScroll->viewport(),
                                                       lanes::masterHeight(), theme::SCROLLBAR_W));
    ui::ScrollBar* timelineBar = scroll->verticalScrollBar();
    ui::ScrollBar* panelBar = panelScroll->verticalScrollBar();
    timelineBar->valueChanged.connect([panelBar](int v) { panelBar->setValue(v); });
    panelBar->valueChanged.connect([timelineBar](int v) { timelineBar->setValue(v); });

    auto* caption = add<OverviewCaption>();
    overviewCaption = caption;
    caption->setFixedHeight(overview->height());  // same height as the overview

    auto* root = ui::vbox(this, 0, 0);
    menuBar->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    root->addWidget(menuBar);
    transport->setFixedHeight(transport->sizeHint().y);
    transport->hPolicy = ui::SizePolicy::Expanding;
    root->addWidget(transport);

    auto* body = add<ui::Widget>();
    auto* row = ui::hbox(body, 0, 0);
    explorerDock = body->add<ExplorerDock>(explorer, [this] { setExplorerVisible(false); });
    explorerHandle = body->add<ui::SplitterHandle>(ui::Orientation::Horizontal);
    setExplorerWidth(config::EXPLORER_W);
    auto start = std::make_shared<int>(0);
    explorerHandle->pressed.connect([this, start] { *start = explorerWidth; });
    explorerHandle->dragged.connect([this, start](int d) { setExplorerWidth(*start + d); });
    row->addWidget(explorerDock);
    row->addWidget(explorerHandle);

    auto* central = body->add<ui::Widget>();
    central->hPolicy = ui::SizePolicy::Expanding;
    auto* line = ui::hbox(central, 0, 0);
    auto left = std::make_unique<ui::VBox>();
    left->spacing = 0;
    // (the overview, scroll, caption, monitor and panel scroll are the window's children so far)
    for (ui::Widget* w : std::initializer_list<ui::Widget*>{overview, scroll, caption, cpuMonitor, panelScroll})
        central->adopt(release(w));
    left->addWidget(overview);
    left->addWidget(scroll, 1);
    auto right = std::make_unique<ui::VBox>();
    right->spacing = 0;
    right->addWidget(caption);
    right->addWidget(cpuMonitor);  // same height as the ruler, so the rows still line up
    right->addWidget(panelScroll, 1);
    line->addLayout(std::move(left), 1);
    line->addLayout(std::move(right));
    row->addWidget(central, 1);
    root->addWidget(body, 1);
    status->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    root->addWidget(status);
}

void MainWindow::setExplorerVisible(bool on) {
    explorerDock->setVisible(on);
    explorerHandle->setVisible(on);
    if (explorerAction) explorerAction->checked = on;
}

void MainWindow::setExplorerWidth(int w) {
    int most = std::max(160, width() - config::PANEL_W - 200);
    explorerWidth = std::max(120, std::min(most, w));
    explorerDock->setFixedWidth(explorerWidth);
}

void MainWindow::buildFileMenu() {
    ui::Menu* menu = menuBar->addMenu("&File");
    struct Entry {
        const char* text;
        const char* keys;
        std::function<void()> slot;
    };
    std::vector<Entry> entries = {
        {"&New project", "Ctrl+N", [this] { newProject(); }},
        {"New from &template", "submenu:templates", nullptr},
        {"&Open project…", "Ctrl+O", [this] { openProject(); }},
        {"Open &recent", "submenu:recent", nullptr},
        {"&Save", "Ctrl+S", [this] { saveProject(); }},
        {"Save &as…", "Ctrl+Shift+S", [this] { saveProjectAs(); }},
        {"Save as t&emplate…", nullptr, [this] { templates::saveAsTemplate(*this); }},
        {"Open a &backup…", nullptr, [this] { openBackup(); }},
        {"&Import Ableton Live set…", nullptr, [this] { importAbleton(); }},
        {nullptr, nullptr, nullptr},
        {"&Render selection…", "Ctrl+Shift+R", [this] { exporter->renderSelection(); }},
        {"Render &project…", "Ctrl+Shift+E", [this] { exporter->renderProject(); }},
        {"Export &stems…", nullptr, [this] { exportStems(); }},
        {"Export selected &MIDI clips…", nullptr, [this] {
             exportMidi(std::vector<ClipPtr>(timeline->selection.clips.begin(), timeline->selection.clips.end()));
         }},
        {nullptr, nullptr, nullptr},
        {"Add audio or MIDI &file…", nullptr, [this] { addFiles(); }},  // (Ctrl+I shown: the toolbar's)
    };
    for (auto& e : entries) {
        if (!e.text) {
            menu->addSeparator();
            continue;
        }
        std::string keys = e.keys ? e.keys : "";
        if (keys == "submenu:templates" || keys == "submenu:recent") {  // (a submenu, filled as it opens)
            ui::Menu* sub = menu->addMenu(e.text);
            if (keys == "submenu:recent") sub->aboutToShow = [this](ui::Menu* m) { templates::fillRecentMenu(m, *this); };
            else sub->aboutToShow = [this](ui::Menu* m) { templates::fillTemplateMenu(m, *this); };
            continue;
        }
        menu->addAction(e.text, e.slot, keys);
    }
}

void MainWindow::buildEditMenu() {
    // undo and redo (history.h): from any of the program's windows (MIDI editor, FX windows too)
    ui::Menu* menu = menuBar->addMenu("&Edit");
    undoAction = menu->addAction("&Undo", [this] { undo(); }, "Ctrl+Z");
    redoAction = menu->addAction("&Redo", [this] { redo(); }, "Ctrl+Shift+Z");
    menu->aboutToShow = [this](ui::Menu*) { showUndoState(); };
    menu->addSeparator();
    // (this window only: the MIDI editor copies notes with the same keys)
    menu->addAction("Cu&t clips", [this] { useClipboard("cut"); }, "Ctrl+X");
    menu->addAction("&Copy clips", [this] { useClipboard("copy"); }, "Ctrl+C");
    menu->addAction("&Paste clips at the cursor", [this] { useClipboard("paste"); }, "Ctrl+V");
    menu->addSeparator();
    // (in the whole song: song_time.h)
    menu->addAction("&Insert time…", [this] { showTimeEdit(insertTime()); }, "Ctrl+Shift+I");
    menu->addAction("&Delete time", [this] { showTimeEdit(deleteTime()); }, "Ctrl+Shift+Del");
    menu->addSeparator();
    addCaptureAction(menu);  // (Capture: capture.h)
}

void MainWindow::showTimeEdit(const std::string& message) {
    if (!message.empty()) showStatus(message, 6000);
}

bool MainWindow::mouseHeld() const {
    if (App::get().headless()) {
        for (ui::Window* w : App::get().windows())
            if (w->grabWidget()) return true;
        return false;
    }
    return plat::heldButtons() != 0;
}

bool MainWindow::rendersPending() {
    for (auto& [key, w] : renderWindows) {
        (void)key;
        if (w.pending && w.pending()) return true;
    }
    return false;
}

void MainWindow::flushRenders() {
    // render now what those windows were about to render (before undo / redo, so their change is a step)
    auto windows = renderWindows;
    for (auto& [key, w] : windows) {
        (void)key;
        if (w.pending && w.pending() && w.flush) w.flush();
    }
}

void MainWindow::noteChanges() {
    // Now and then: a change to the project becomes an undo step, once no mouse button is held (a drag
    // is one step), and once the clips a MIDI effects or sampler window changed are rendered again (the
    // change and how the clips then sound are one step).
    if (history && !mouseHeld() && !rendersPending()) history->check(engine.sr);
}

void MainWindow::showUndoState() {
    noteChanges();
    undoAction->enabled = history->canUndo();
    redoAction->enabled = history->canRedo();
}

void MainWindow::undo() { stepHistory(false); }

void MainWindow::redo() { stepHistory(true); }

void MainWindow::stepHistory(bool forward) {
    if (mouseHeld() || dragging()) {
        // (undoing under a drag would leave it working on what undo took away)
        showStatus("Let go of the mouse first: undo and redo wait until the drag is done", 3000);
        return;
    }
    flushRenders();
    noteChanges();  // (a change not yet noted is a step of its own: undo takes it back first)
    if (!(forward ? history->redo(engine.sr) : history->undo(engine.sr))) {
        showStatus(forward ? "Nothing to redo" : "Nothing to undo", 2000);
        return;
    }
    timeline->selection.clear();  // (what was selected may be gone)
    timeline->relayout();
    timeline->tracksChanged.emit();  // the panel's rows, the FX windows
    master->refreshFx(*project.masterEffects);
    transport->showTiming(project.timing);
    checkMidiEditors();
    reloadEditors();
    auto windows = renderWindows;  // (they show the track's MIDI effects or sampler as they are now)
    for (auto& [key, w] : windows) {
        (void)key;
        if (w.reload) w.reload();
    }
    showStatus(forward ? "Redone" : "Undone", 2000);
}

void MainWindow::buildViewMenu() {
    ui::Menu* menu = menuBar->addMenu("&View");
    explorerAction = menu->addCheckable("&Explorer", true, [this](bool on) { setExplorerVisible(on); }, "Ctrl+E");
    menu->addSeparator();
    menu->addAction("Show all automation", [this] { showAllAutomation(true); });
    menu->addAction("Hide all automation", [this] { showAllAutomation(false); });
}

void MainWindow::showAllAutomation(bool on) {
    // every track with automation lanes shows them, or none does
    for (auto& t : project.tracks) t->showAutomation = on && !t->automation.empty();
    timeline->lanesChanged();
}

void MainWindow::buildOptionsMenu() {
    // audio properties, and options that lower CPU use; all remembered between runs
    ui::Menu* menu = menuBar->addMenu("&Options");
    menu->addAction("&Audio properties…", [this] { showAudioProperties(); });
    addMidiOptions(menu);  // Recording offset…, MIDI input…, Computer keyboard
    ui::Menu* starts = menu->addMenu("&New projects start from");  // (an empty project, or a template: templates.h)
    starts->aboutToShow = [this](ui::Menu* m) { templates::fillDefaultMenu(m, *this); };
    menu->addAction("&VST2 plugin folders…", [this] { chooseVst2Folders(); });
    menu->addAction("&Wine prefixes…", [this] { chooseWinePrefixes(); });
    menu->addAction("&Install Windows plugin…", [this] { installWindowsPlugin(); });
    menu->addCheckable("Reload crashed Windows plugins automaticall&y", prefs.reloadCrashedPlugins, [this](bool on) {
            prefs.reloadCrashedPlugins = plugins->autoReload = on;
            prefs.save();
        })->tooltip = "A Windows plugin whose Wine host crashed starts again by itself, with the settings it last had "
                      "(at most 3 times a minute; then it stays stopped)";
    menu->addSeparator();

    ui::Menu* refresh = menu->addMenu("Screen &refresh");
    for (int fps : prefs::refreshRates()) {
        std::string label = fps ? std::to_string(fps) + " per second"
                                : "As the screen (" + std::to_string((int)std::lround(App::get().displayRate())) + " per second)";
        ui::Action* a = refresh->addCheckable(label, fps == prefs.refreshFps,
                                              [this, fps](bool on) {
                                                  if (on) setRefreshRate(fps);
                                              });
        a->group = 1;
        a->tooltip = "How often the playhead and meters move. Lower: less CPU, less smooth. "
                     "(Drags and resizes always move with every picture the screen shows)";
    }
    menu->addSeparator();
    menu->addCheckable("&Level meters", prefs.levelMeters, [this](bool on) { setMeterOption("level_meters", on); });
    menu->addCheckable("Loudness (&LUFS)", prefs.loudnessMeters,
                       [this](bool on) { setMeterOption("loudness_meters", on); });
    scopeAction = menu->addCheckable("Master &scope", master->scope->settings.shown, [this](bool on) { showScope(on); });
    scopeAction->tooltip = "Under the Master row: the waveform or spectrum of what the master plays (right-click it "
                           "for its options)";
    auto* cpuAction = menu->addCheckable("&CPU per track", prefs.trackCpu, [this](bool on) {
        prefs.trackCpu = on;
        prefs.save();
        if (!on)
            for (auto& kv : panel->frames) kv.second->setLoad(-1);
    });
    cpuAction->tooltip = "A thin bar along the bottom of each track's row: how much of one processor core the track "
                         "takes (its plugins and effects). The Audio CPU monitor's tooltip "
                         "lists the costliest tracks";
    menu->addSeparator();
    ui::Menu* colours = menu->addMenu("&Colours");
    colours->addCheckable("&Random colour for new tracks", prefs.randomTrackColors,
                          [this](bool on) { setColourOption("random_track_colors", on); })
        ->tooltip = "Every new track gets a colour of its own, unlike its neighbours'";
    colours->addCheckable("&Clips take their track's colour", prefs.clipsFollowTrack,
                          [this](bool on) { setColourOption("clips_follow_track", on); })
        ->tooltip = "Clips without a colour of their own are drawn in their track's colour";
    colours->addSeparator();
    colours->addAction("&Default clip and row colours…", [this] { customizeSkin("Tracks and clips"); });
    applyColourOptions();
    skinMenu = menu->addMenu("&Skin");
    skinMenu->aboutToShow = [this](ui::Menu*) { fillSkinMenu(); };
    fillSkinMenu();
    roundedAction = menu->addCheckable("Rounded c&orners", prefs.roundedCorners,
                                       [this](bool on) { setRoundedCorners(on); });
    roundedAction->tooltip = "Off: every corner of buttons, fields, sliders and scroll bars is square";
    // the whole window bigger or smaller: from the next start (main.cpp)
    ui::Menu* sizes = menu->addMenu("Interface si&ze");
    int current = prefs::interfaceScale();
    for (int size : prefs::interfaceSizes()) {
        ui::Action* a = sizes->addCheckable(std::to_string(size) + " %", size == current);
        a->group = 3;
        a->triggered = [this, size] { setInterfaceScale(size); };
        sizeActions[size] = a;
    }
    sizes->aboutToShow = [this](ui::Menu*) { offerInterfaceSizes(); };
}

bool MainWindow::interfaceSizeFits(int size) {
    // Would the window, drawn at `size` %, fit across this screen? (Its narrowest is the same in its own
    // pixels at every size: the top bar's, with every control on it in its short form.)
    Rect screen = App::get().screenAt(App::get().cursorPos());
    if (screen.empty()) return true;
    double across = screen.w * (double)App::get().scale();  // (in screen pixels)
    int narrowest = std::max(minimumSizeHint().x, transport->narrowestWidth());
    return narrowest * size / 100.0 <= across + 1;
}

void MainWindow::offerInterfaceSizes() {
    // Options → Interface size: sizes too wide for this screen can't be chosen
    int current = prefs::interfaceScale();
    for (auto& [size, a] : sizeActions) {
        bool fits = size == current || interfaceSizeFits(size);
        a->enabled = fits;
        a->tooltip = fits ? "" : "Too wide for this screen: the window couldn't fit across it";
    }
}

void MainWindow::setInterfaceScale(int size) {
    // Options → Interface size: remembered, and used from the next start (everything is sized once)
    prefs.interfaceScale = size;
    prefs.save();
    ui::msg::information(this, "Interface size",
                         "Tiny DAW will be drawn at " + std::to_string(size) +
                             " % from the next time it starts: close it and start it again. (For more contrast, "
                             "try Options → Skin → High contrast.)");
}

// --- skins (skin.h) -------------------------------------------------------------------------

void MainWindow::fillSkinMenu() {
    // the skins there are (looked up each time the menu opens), and what to do with them
    ui::Menu* menu = skinMenu;
    menu->clear();
    for (const skin::Skin& s : skin::library()) {
        ui::Action* a = menu->addCheckable(s.builtin ? s.name : s.name + "  (yours)",
                                           s.name == skin.name && s.builtin == skin.builtin);
        a->group = 2;
        a->tooltip = s.path;
        a->triggered = [this, s] { chooseSkin(s); };
    }
    menu->addSeparator();
    menu->addAction("&Customize this skin…", [this] { customizeSkin(); });
    menu->addAction("&Import skin…", [this] { importSkin(); });
    menu->addAction("&Export this skin…", [this] { exportSkin(); });
    menu->addAction("&Open the skins folder", [this] { openSkinsFolder(); });
}

void MainWindow::chooseSkin(const skin::Skin& s) {
    // use it from now on (remembered)
    skin = s;
    prefs.skin = s.name;
    prefs.save();
    showSkin(s);
}

void MainWindow::showSkin(const skin::Skin& s) {
    // put a skin on the whole program at once (also while editing one)
    skin::apply(s);
    restyle();
}

void MainWindow::restyle() {
    // rebuild or repaint what took its colours from the old skin
    panel->rebuild();
    master->restyle();
    overviewCaption->update();
    overview->forgetImage();  // (its cached image used the old colours)
    overview->refresh();
    transport->fit();  // (another font: the top bar's controls may be wider or narrower)
    App::get().repaintAll();
}

void MainWindow::setRoundedCorners(bool on) {
    // Options → Rounded corners: square corners everywhere when off (remembered)
    prefs.roundedCorners = on;
    prefs.save();
    theme::rounded = on;
    showSkin(skin);
}

void MainWindow::overviewResized(int height) {
    // the overview's bottom edge was dragged: the caption beside it follows; remembered
    overviewCaption->setFixedHeight(height);
    prefs.overviewH = height;
    prefs.save();
}

void MainWindow::setColourOption(const std::string& attr, bool on) {
    if (attr == "random_track_colors") prefs.randomTrackColors = on;
    else prefs.clipsFollowTrack = on;
    prefs.save();
    applyColourOptions();
}

void MainWindow::applyColourOptions() {
    // Options → Colours: random colours for new tracks; clips taking their track's colour
    colors::followTrack = prefs.clipsFollowTrack;
    timeline->randomTrackColors = prefs.randomTrackColors;
    restyle();
}

void MainWindow::customizeSkin(const std::string& tab) {
    // the skin editor (Options → Skin → Customize, or Colours → Default colours: `tab`)
    {
        SkinDialog d(this, skin, [this](const skin::Skin& s) { showSkin(s); },
                     [this](const skin::Skin& s) { chooseSkin(s); }, tab);
        d.exec();
    }
    showSkin(skin);  // what was saved, or back to the skin as it was
}

void MainWindow::importSkin() { skin::importSkin(this, [this](const skin::Skin& s) { chooseSkin(s); }); }

void MainWindow::exportSkin() {
    std::string path = skin::exportSkin(this, skin);
    if (!path.empty()) showStatus("Skin saved as " + path, 5000);
}

void MainWindow::openSkinsFolder() { skin::openSkinsFolder(); }

void MainWindow::chooseVst2Folders() {
    // Options → VST2 plugin folders: where VST2 plugins are looked for (remembered)
    std::vector<std::string> chosen;
    {
        PluginFoldersDialog d(this, prefs.vst2List());
        if (!d.exec()) return;
        chosen = d.folders();
    }
    prefs.setVst2List(chosen);
    prefs.save();
    int found = 0;
    for (auto& p : plugins->rescan().plugins)
        if (plug::isVst2Path(p.path)) ++found;
    showStatus(std::to_string(found) + " VST2 plugin" + (found == 1 ? "" : "s") + " found", 5000);
}

void MainWindow::showScope(bool on) {
    // Options → Master scope (and "Hide the scope" in its menu): the Master row and lane that much taller
    lanes::setMasterScopeHeight(on ? config::SCOPE_H : 0);
    master->setScopeShown(on);
    for (auto& pin : masterPins) pin->setHeight(lanes::masterHeight());
    if (scopeAction) scopeAction->setChecked(on);
    timeline->relayout();
    prefs.scope = master->scope->settings.toJson().dump();
    prefs.save();
}

void MainWindow::chooseWinePrefixes() {
    // Options → Wine prefixes: where Windows plugins are looked for (remembered)
    std::vector<std::string> chosen;
    std::map<std::string, std::string> programs;
    {
        WinePrefixesDialog d(this, prefs.wineList(), prefs.wineProgramMap());
        if (!d.exec()) return;
        chosen = d.prefixes();
        programs = d.programs();
    }
    prefs.setWineList(chosen);
    prefs.setWineProgramMap(programs);
    prefs.save();
    wine::setPrefixes(prefs.wineList());
    wine::setWinePrograms(prefs.wineProgramMap());
    int found = 0;
    for (auto& p : plugins->rescan().plugins)
        if (p.looksWindows()) ++found;
    showStatus(std::to_string(found) + " Windows plugin" + (found == 1 ? "" : "s") + " found", 5000);
}

void MainWindow::installWindowsPlugin() {
    // Options → Install Windows plugin…: an installer run in a Wine prefix, then what it added looked for
    const std::string title = "Install Windows plugin";
    std::vector<std::string> prefixes = wine::prefixes();
    std::set<std::string> known, before = wine_setup::vst2Dlls(prefixes);
    for (auto& p : plugins->library().plugins) known.insert(p.path);
    std::string used, made;
    if (!wine_setup::installPlugin(this, prefixes, used, made)) return;
    if (!made.empty()) {  // (a prefix made for it: from now on searched)
        auto list = prefs.wineList();
        list.push_back(made);
        prefs.setWineList(list);
        prefs.save();
        wine::setPrefixes(prefs.wineList());
    }
    // VST2 plugins land wherever the installer chose: those in a usual folder not searched yet are offered
    auto folders = wine_setup::newVst2Folders(before, wine_setup::vst2Dlls({used}), prefs.vst2List());
    if (!folders.empty()) {
        std::string text = "The installer put VST2 plugins (.dll) in folders Tiny DAW doesn't look in yet:\n";
        for (auto& f : folders) text += "\n" + f;
        if (ui::msg::question(this, title, text + "\n\nAdd them to Options → VST2 plugin folders?") == ui::msg::Yes) {
            auto v = prefs.vst2List();
            v.insert(v.end(), folders.begin(), folders.end());
            prefs.setVst2List(v);
            prefs.save();
        }
    }
    std::vector<std::string> added;
    for (auto& p : plugins->rescan().plugins)
        if (p.looksWindows() && !known.count(p.path) && std::find(added.begin(), added.end(), p.name) == added.end())
            added.push_back(p.name);
    if (added.empty()) {
        ui::msg::information(this, title,
                             "No new Windows plugins were found in " + used + ".\n\nIf the installer is still open, "
                             "finish it, then look again (Options → Wine prefixes → OK). VST2 plugins installed "
                             "elsewhere are found once their folder is in Options → VST2 plugin folders.");
        return;
    }
    std::string names;
    for (auto& n : added) names += "\n" + n;
    showStatus(std::to_string(added.size()) + " new Windows plugin" + (added.size() == 1 ? "" : "s"), 5000);
    ui::msg::information(this, title, "New in " + used + ":\n" + names);
}

void MainWindow::showAudioProperties() {
    Preferences& p = prefs;
    std::string type, dev;
    int buffer;
    bool exclusive;
    int cores;
    {
        AudioPropertiesDialog d(this, p.driverType, device, p.bufferSize, p.wasapiExclusive, engine);
        if (!d.exec()) return;
        type = d.driverType;
        dev = d.device;
        buffer = d.bufferSize;
        exclusive = d.exclusive;
        cores = d.cores;
    }
    bool coresChanged = cores != p.processingCores;
    if (coresChanged) {  // (takes effect when the device opens again: below)
        p.processingCores = engine.cores = cores;
        workers::setChosenCores(cores);
        p.save();
    }
    if (type != p.driverType || dev != device || buffer != p.bufferSize || exclusive != p.wasapiExclusive)
        setAudioDevice(type, dev, buffer, exclusive);
    else if (coresChanged && !device.empty())
        openDevice();
}

void MainWindow::setAudioDevice(const std::string& driverType, const std::string& dev, int bufferSize, bool exclusive) {
    // play through `dev` (a device id of `driverType`) with `bufferSize` frames (0 = driver default);
    // `exclusive` where the driver has it. Remembered for next time. Takes effect by reopening the
    // device, which stops playback.
    device = dev;
    Preferences& p = prefs;
    p.driverType = driverType;
    p.wasapiExclusive = exclusive;
    p.deviceName.clear();
    for (auto& d : audio::outputDevices(driverType))
        if (d.id == dev) p.deviceName = d.name;
    p.bufferSize = engine.blocksize = bufferSize;
    p.save();
    if (!device.empty()) openDevice();
}

void MainWindow::setBufferSize(int frames) { setAudioDevice(prefs.driverType, device, frames, prefs.wasapiExclusive); }

void MainWindow::setRefreshRate(int fps) {
    prefs.refreshFps = fps;
    prefs.save();
    App::get().setFrameRate(fps);
}

void MainWindow::setMeterOption(const std::string& attr, bool on) {
    if (attr == "level_meters") prefs.levelMeters = on;
    else prefs.loudnessMeters = on;
    prefs.save();
    applyMeterOptions();
}

void MainWindow::applyMeterOptions() {
    // switched-off meters are hidden, and the engine stops measuring for them
    bool levels = prefs.levelMeters, loudness = prefs.loudnessMeters;
    engine.metering->levelsOn = levels;
    engine.metering->loudnessOn = loudness;
    panel->setMetersVisible(levels, loudness);
}

void MainWindow::connectControls() {
    TransportBar* t = transport;
    t->addFileAction->triggered.connect([this] { addFiles(); });
    t->addAudioTrackAction->triggered.connect([this] { timeline->addTrack(Kind::Audio); });
    t->addMidiTrackAction->triggered.connect([this] { timeline->addTrack(Kind::Midi); });
    t->addReturnTrackAction->triggered.connect([this] { timeline->addTrack(Kind::Return); });
    t->captureAction->triggered.connect([this] { capture(); });
    t->keysAction->toggled.connect([this](bool on) {  // the computer keyboard as a piano (computer_keyboard.h)
        if (keyboard) keyboard->setOn(on);
        if (keysAction) keysAction->checked = on;
    });
    t->onGridChange = [this](const GridChange& c) {
        switch (c.what) {
        case GridChange::Grid: changeGrid(c.grid); break;
        case GridChange::Snap: changeGrid({}, c.snap); break;
        case GridChange::Finer: changeGrid({}, {}, true); break;
        case GridChange::Coarser: changeGrid({}, {}, false); break;
        }
    };
    t->gridActions["finer"]->triggered.connect([this] { changeGrid({}, {}, true); });
    t->gridActions["coarser"]->triggered.connect([this] { changeGrid({}, {}, false); });
    t->gridActions["snap"]->triggered.connect([this] { changeGrid({}, !project.timing.snapOn); });
    t->gridActions["fixed"]->triggered.connect(
        [this] { changeGrid(project.timing.fixedStep() ? 0.0 : 0.25); });  // (1/16: a quarter of a beat)
    t->playAction->triggered.connect([this] { togglePlay(); });
    t->recordAction->triggered.connect([this] { recording->toggle(); });
    t->recordButton->rightClicked.connect([this](ui::MouseEvent&) { recording->showSettings(); });
    t->startAction->triggered.connect([this] { goToStart(); });
    t->loopAction->toggled.connect([this](bool on) { setLooping(on); });
    timeline->loopChanged.connect([this] { syncLoop(); });
    timeline->loopRangeChosen.connect([this] { setLooping(true); });
    timeline->tracksChanged.connect([this] { checkFxWindows(); });
    timeline->editNotes.connect([this](ClipPtr clip) { openMidiEditor(clip); });
    timeline->exportMidi.connect([this](std::vector<ClipPtr> clips) { exportMidi(clips); });
    timeline->clipboardChosen.connect([this](std::string what) { useClipboard(what); });
    timeline->timeEdited.connect([this](std::string message) { showTimeEdit(message); });
    timeline->timingChanged.connect([this] { setTiming({}); });  // (a tempo change on the ruler)
    t->bpmBox->valueChanged.connect([this](double v) { setTiming({v, {}, {}, {}}); });
    t->signatureBox->valueChanged.connect([this](double v) { setTiming({{}, (int)std::lround(v), {}, {}}); });
    t->beatsAction->toggled.connect([this](bool on) { setTiming({{}, {}, on, {}}); });
    t->tapButton->pressed.connect([this] { tap(); });  // on press, not release: the moment of the tap counts
    t->metronomeButton->setCheckedSilently(prefs.metronome);
    t->metronomeButton->toggled.connect([this](bool on) { setMetronomeOn(on); });
    t->metronomeButton->rightClicked.connect([this](ui::MouseEvent&) { showMetronomeSettings(); });
    applyMetronome();
    t->thirdsAction->toggled.connect([this](bool on) { setTiming({{}, {}, {}, on}); });
    for (ui::SpinBox* box : {t->bpmBox, t->signatureBox})  // after Enter, keys (Space, S, …) go back to the timeline
        box->editor()->returnPressed.connect([this] { timeline->setFocus(); });
    t->showTiming(project.timing);
}

void MainWindow::tap() {
    // tap tempo: a tap from the Tap button (or T)
    double bpm = tapTempo.tap(plat::now());
    int taps = tapTempo.count();
    if (bpm <= 0) {
        showStatus("Tap tempo: keep tapping in time…", 3000);
        return;
    }
    transport->bpmBox->setValue(bpm);  // sets the project's tempo
    showStatus("Tap tempo: " + fmtG(bpm) + " BPM from " + std::to_string(taps) + " taps", 3000);
}

// --- metronome ------------------------------------------------------------------------------

void MainWindow::setMetronomeOn(bool on) {
    prefs.metronome = on;
    prefs.save();
    applyMetronome();
}

void MainWindow::applyMetronome() {
    // give the engine the metronome's sounds while it is on
    if (prefs.metronome) engine.setMetronome(metronomeSettings);
    else engine.setMetronome(std::nullopt);
}

void MainWindow::showMetronomeSettings() {
    MetronomeDialog d(this, metronomeSettings, [this](const metronome::MetronomeSettings&) { applyMetronome(); },
                      [this](const metronome::Click& c) { testClick(c); });
    d.exec();
}

void MainWindow::testClick(const metronome::Click& click) { metronome_dialog::testClick(engine, click); }

void MainWindow::setLooping(bool on) {
    // Loop on or off. Turned on with a time range selected, the loop becomes that range; with no
    // loop range yet, the whole arrangement (or 4 bars of an empty project).
    Timing& t = project.timing;
    if (on) {
        double s0, s1;
        if (timeline->selection.range) {
            timeline->setLoop(timeline->selection.range->start, timeline->selection.range->end);
        } else if (!t.loopRange(s0, s1)) {
            int64_t bars = std::llround(4 * t.beatsPerBar * t.beatSeconds() * engine.sr);
            int64_t end = project.endFrame();
            timeline->setLoop(0, end ? end : bars);
        }
    }
    t.looping = on;
    transport->showTiming(t);
    syncLoop();
    timeline->update();
}

void MainWindow::syncLoop() {
    // hand the loop range to the engine while looping (in frames, at its sample rate)
    const Timing& t = project.timing;
    int sr = engine.sr;
    double s0, s1;
    std::optional<audio::LoopRange> want;
    if (t.looping && t.loopRange(s0, s1)) want = audio::LoopRange{std::llround(s0 * sr), std::llround(s1 * sr)};
    auto have = engine.loop();
    bool same = (bool)want == (bool)have && (!want || (want->start == have->start && want->end == have->end));
    if (same) return;
    if (want) engine.setLoop(want->start, want->end);
    else engine.clearLoop();
}

void MainWindow::setTiming(const TimingChange& c) {
    // change the tempo, time signature or grid view; clips keep their place in time
    Timing& t = project.timing;
    if (c.bpm) t.bpm = *c.bpm;
    if (c.beatsPerBar) t.beatsPerBar = *c.beatsPerBar;
    if (c.beats) t.beats = *c.beats;
    if (c.thirds) t.thirds = *c.thirds;
    // (MIDI effects work in beats: an arpeggio follows the tempo; no changes: one on the ruler)
    bool none = !c.bpm && !c.beatsPerBar && !c.beats && !c.thirds;
    if (none || c.bpm || c.beatsPerBar)
        for (auto& clip : project.clips) {
            TrackPtr track = std::atomic_load(&clip->track);
            if (clip->kind == Kind::Midi && track && !track->midiEffects.empty()) timeline->prepareLive(clip);
        }
    transport->showTiming(t);
    timeline->update();
}

void MainWindow::startTimers() {
    App::get().setFrameRate(prefs.refreshFps);
    std::weak_ptr<bool> alive = alive_;
    frameId_ = App::get().onFrame([this, alive] {
        if (!alive.expired()) onFrame();
    });
    cpuTimer_ = App::get().every(config::CPU_REFRESH_MS, [this, alive] {
        if (!alive.expired()) onCpuTimer();
    });
}

void MainWindow::openFirstDevice() {
    // Open the driver type and device used last time (Options → Audio properties). If that driver
    // type is gone, use the first available one; if the device is gone, that type's default device.
    Preferences& p = prefs;
    auto available = audio::driverTypes();
    bool have = false;
    for (auto& d : available) have = have || d.key == p.driverType;
    if (!have && !available.empty()) p.driverType = available[0].key;
    device.clear();
    for (auto& d : audio::outputDevices(p.driverType))
        if (!p.deviceName.empty() && d.name == p.deviceName) {
            device = d.id;
            break;
        }
    if (device.empty()) device = audio::defaultOutput(p.driverType);
    if (!device.empty()) openDevice();
    else showStatus("No audio output device found.");
}

// --- actions --------------------------------------------------------------------------------

void MainWindow::openDevice() {
    stop();
    int oldSr = engine.sr;
    bool unchanged = history && !hasUnsavedChanges();
    std::string error;
    if (engine.open(prefs.driverType, device, -1, prefs.wasapiExclusive, error)) {
        std::string kind = audio::driverLabel(engine.driverType) + (engine.exclusive ? " exclusive" : "");
        std::string buffer = !engine.blocksize ? "driver buffer" : std::to_string(engine.blocksize) + "-frame buffer";
        char lat[32];
        snprintf(lat, sizeof lat, "%.1f", engine.outputLatency() * 1000);
        showStatus(kind + ": " + engine.deviceName() + "  ·  " + std::to_string(engine.channels) + " outputs  ·  " +
                   std::to_string(engine.sr) + " Hz  ·  " + buffer + "  ·  " + lat + " ms output latency");
    } else {
        showStatus("Could not open the audio device: " + error + "  (Options → Audio properties…)");
    }
    if (engine.sr != oldSr && history) {  // (the clips were converted by the engine: the cursor and range follow)
        sampleRateChanged(oldSr, engine.sr);
        if (unchanged) savedSnapshot = snapshot();  // (a saved project stays saved: converting isn't an edit)
        history->reset(engine.sr);
    }
    panel->setChannels(engine.channels);
    applyMeterOptions();  // the engine's metering was recreated for the device
    timeline->relayout();
}

void MainWindow::addFiles() {
    for (auto& path : ui::files::openFiles(this, "Add audio or MIDI", "", config::FILE_FILTER))
        timeline->addFile(path, timeline->cursor);
}

void MainWindow::goToStart() {
    // both cursors to 0 s (Home)
    timeline->setCursor((int64_t)0);
    timeline->seek(0);
}

void MainWindow::togglePlay() {
    if (!engine.isOpen()) return;
    if (recording->active()) {  // stops recording, keeping the take
        recording->toggle();
    } else if (engine.playing) {
        engine.stop();  // the playback cursor stays where it stopped
    } else {
        timeline->seek(timeline->cursor);  // play always starts from the selection cursor
        resetMeters();
        engine.play();
    }
    transport->setPlaying(engine.playing);
}

void MainWindow::resetMeters() {
    // playback (or recording) starts: the loudest peaks and the clip lights count from now
    if (master->meter) master->meter->resetPeaks();
    for (auto& [track, meter] : panel->meters) {
        (void)track;
        if (meter) meter->resetPeaks();
    }
    for (auto& [group, meter] : panel->groupMeters) {
        (void)group;
        if (meter) meter->resetPeaks();
    }
}

// --- plugins --------------------------------------------------------------------------------

void MainWindow::openTrackPlugins(const TrackPtr& track) {
    if (track->frozen) {
        ui::msg::information(this, "Track frozen", track->name + " is frozen, so its plugins are locked.\n"
                                                                 "Unfreeze it first (right-click its row).");
        return;
    }
    showFxWindow({0, track.get()}, [this, track] {
        return new PluginsDialog(this, *plugins, "Effects: " + track->name, project, track, [this] { panel->rebuild(); });
    });
}

void MainWindow::openMasterPlugins() {
    showFxWindow({0, nullptr}, [this] {
        return new PluginsDialog(this, *plugins, "Effects: Master (outputs 1/2)", project, nullptr,
                                 [this] { master->refreshFx(*project.masterEffects); });
    });
}

void MainWindow::fillInstrumentMenu(ui::Menu& menu, const TrackPtr& track) {
    // a MIDI track's instruments to choose from (instrument_menu.h)
    instrument_menu::fillInstrumentMenu(&menu, this, *plugins, track, [this, track] { instrumentChosen(track); },
                                        [this](const TrackPtr& t) { openInstrumentPanel(t); });
}

void MainWindow::instrumentChosen(const TrackPtr& track) {
    // After a track's instrument changed: its row shows it; its instrument patch panel's window
    // closes if the panel isn't played any more.
    patch::ensureStates(project);
    panel->rebuild();
    auto it = fxWindows.find({1, track.get()});
    if (it != fxWindows.end() && !track->usesRack) it->second->close();
}

void MainWindow::openInstrumentPanel(const TrackPtr& track) {
    // a MIDI track's instrument patch panel (several instruments, each with its own volume)
    if (track->frozen) {
        ui::msg::information(this, "Track frozen", track->name + " is frozen, so its plugins are locked.\n"
                                                                 "Unfreeze it first (right-click its row).");
        return;
    }
    showFxWindow({1, track.get()}, [this, track] {
        return new InstrumentPanelDialog(this, *plugins, "Instruments: " + track->name, project, track,
                                         [this] { panel->rebuild(); });
    });
}

void MainWindow::editInstrument(const TrackPtr& track) {
    // open a MIDI track's instrument's own window, to configure it
    instrument_menu::editInstrument(this, *plugins, track, [this](const TrackPtr& t) { openInstrumentPanel(t); });
}

void MainWindow::showFxWindow(std::pair<int, const void*> key, const std::function<PluginsDialog*()>& make) {
    // The plugins window of a track (or the master), one per owner (a track's instrument patch
    // panel has its own). It doesn't block: the project can be played and edited while it is open.
    auto it = fxWindows.find(key);
    PluginsDialog* window;
    if (it == fxWindows.end()) {
        window = fxWindows[key] = make();
        // closed (Close, Esc, or by the program): forget it at once (it is deleted a moment later)
        std::weak_ptr<bool> alive = alive_;
        window->finished.connect([this, alive, key](int) {
            if (!alive.expired()) fxWindows.erase(key);
        });
    } else {
        window = it->second;
    }
    window->open();
    window->raiseWindow();
}

void MainWindow::checkFxWindows() {
    // After tracks changed: close the effects (and instrument patch panel) windows of tracks that
    // are gone or frozen (or not playing their panel); the others show their plugins afresh.
    project.forgetGoneTracks();  // (sidechains listening to a removed track)
    projectReplaced();           // (the MIDI effects and sampler windows of tracks that are gone)
    engine.metering->forgetGone(*project.tracks);
    patch::ensureStates(project);
    std::vector<PluginsDialog*> windows;
    for (auto& [key, w] : fxWindows) {
        (void)key;
        windows.push_back(w);
    }
    for (PluginsDialog* w : windows) {
        bool still = false;
        for (auto& [key, x] : fxWindows) {
            (void)key;
            still = still || x == w;
        }
        if (!still) continue;  // (closed meanwhile)
        const TrackPtr& owner = w->owner;
        if (!owner) w->refresh();
        else if (!project.tracks.contains(owner) || owner->frozen || (w->INSTRUMENTS && !owner->usesRack)) w->close();
        else w->refresh();
    }
}

void MainWindow::closeFxWindows() {
    std::vector<ui::Window*> windows;
    for (auto& [key, w] : fxWindows) {
        (void)key;
        windows.push_back(w);
    }
    for (auto& [key, e] : midiEditors) {
        (void)key;
        windows.push_back(e);
    }
    for (ui::Window* w : windows) w->close();
    fxWindows.clear();
    midiEditors.clear();
}

void MainWindow::openMidiEditor(const ClipPtr& clip) {
    // the MIDI editor of a clip (one per clip); like the FX windows it doesn't block anything
    auto it = midiEditors.find(clip.get());
    MidiEditor* editor;
    if (it == midiEditors.end()) {
        MidiEditor::TimelineLink link;
        link.cursor = [this] { return timeline->cursor; };
        link.setCursor = [this](int64_t f) { timeline->setCursor(f); };
        link.seek = [this](int64_t f) { timeline->seek(f); };
        link.editNotes = [this](ClipPtr c) { openMidiEditor(c); };
        editor = midiEditors[clip.get()] = new MidiEditor(this, clip, project, engine, [this] { notesEdited(); }, link);
        std::weak_ptr<bool> alive = alive_;
        const Clip* key = clip.get();
        editor->closed.connect([this, alive, key, editor] {
            if (alive.expired()) return;
            auto f = midiEditors.find(key);
            if (f != midiEditors.end() && f->second == editor) midiEditors.erase(f);
        });
    } else {
        editor = it->second;
    }
    editor->showWindow();
    editor->raiseWindow();
}

void MainWindow::notesEdited() { timeline->relayout(); }  // the clip may have grown

void MainWindow::checkMidiEditors() {
    // close the editors of clips that are gone (deleted, split, cut) or locked (frozen track)
    std::vector<std::pair<const Clip*, MidiEditor*>> editors(midiEditors.begin(), midiEditors.end());
    for (auto& [key, editor] : editors) {
        const ClipPtr& clip = editor->clip;
        TrackPtr track = clip ? std::atomic_load(&clip->track) : nullptr;
        if (!project.clips.contains(clip) || (track && track->frozen)) {
            midiEditors.erase(key);
            editor->close();
        } else {
            editor->updateTitle();
        }
    }
}

void MainWindow::restorePlugins() {
    // load the plugins of a just-opened project; say which could not be loaded
    std::vector<SlotPtr> slots = project.pluginSlots();
    std::vector<std::string> failed;
    if (!slots.empty()) failed = plugins->restore(slots);
    patch::ensureStates(project);
    panel->rebuild();
    master->refreshFx(*project.masterEffects);
    if (!failed.empty()) {
        std::string names;
        for (auto& f : failed) names += (names.empty() ? "" : "\n") + f;
        ui::msg::warning(this, "Plugins missing",
                         "These plugins could not be loaded, so they are silent for now:\n\n" + names +
                             "\n\nTheir settings are kept: saving the project does not lose them.");
    }
}

// --- projects -------------------------------------------------------------------------------

Json MainWindow::snapshot() {
    // the project without plugin settings (project_io::describeSlot); a plugin window being
    // opened counts as a change instead (plugins->edited)
    return project_io::describe(project, engine.sr, master->settings(), nullptr, false);
}

void MainWindow::markSaved() {
    // remember the project as it is now, to tell later whether it has unsaved changes
    savedSnapshot = snapshot();
    plugins->edited = false;
    std::string name = projectPath.empty() ? "Untitled" : util::baseName(projectPath);
    setTitle(name + " — Tiny DAW");
}

bool MainWindow::hasUnsavedChanges() { return plugins->edited || !(snapshot() == savedSnapshot); }

bool MainWindow::confirmDiscard() {
    // True if it is fine to replace or close the current project (saving it first if asked). A recording
    // in progress is finished first: its take goes into the project, so it counts as a change to save.
    if (recording->active()) recording->toggle();
    if (!hasUnsavedChanges()) return true;
    int answer = ui::msg::question(this, "Unsaved changes", "Save the changes to the current project?",
                                   ui::msg::Save | ui::msg::Discard | ui::msg::Cancel);
    if (answer == ui::msg::Save) return saveProject();
    return answer == ui::msg::Discard;
}

void MainWindow::stop() {
    if (recording->active()) recording->toggle();  // (keeping the take)
    engine.stop();
    transport->setPlaying(false);
}

void MainWindow::resetView() {
    // after the project was replaced: empty selection, both cursors at 0, everything redrawn
    timeline->selection.clear();
    timeline->setCursor((int64_t)0);
    timeline->seek(0);
    timeline->relayout();
    timeline->tracksChanged.emit();
    transport->showTiming(project.timing);
}

void MainWindow::newProject() {
    if (!confirmDiscard()) return;
    std::string start = templates::defaultTemplate(prefs);  // (Options → New projects start from)
    if (!start.empty() && startFromTemplate(start)) return;
    stop();
    closeFxWindows();  // they belong to the project being replaced
    plugins->closeEditor();
    project.clear();
    master->apply(project_io::MasterSettings{});
    master->refreshFx({});
    projectPath.clear();
    projectReplaced();
    resetView();
    history->reset(engine.sr);
    autosaver->clear();
    markSaved();
}

void MainWindow::openRecent(const std::string& path) {
    if (confirmDiscard()) openProjectFile(path);
}

void MainWindow::newFromTemplate(const std::string& path) {
    if (confirmDiscard()) startFromTemplate(path);
}

bool MainWindow::startFromTemplate(const std::string& path) {
    // a new, untitled project made from the template at `path` (Save asks where to keep it)
    if (!openProjectFile(path)) return false;
    projectPath.clear();
    markSaved();  // (unchanged until you change it: nothing to ask about)
    setTitle("Untitled — Tiny DAW");
    showStatus("New project from the template " + util::stem(path), 5000);
    return true;
}

void MainWindow::openProject() {
    if (!confirmDiscard()) return;
    std::string path = ui::files::openFile(this, "Open project", "", "Tiny DAW project (*.json)");
    if (!path.empty()) openProjectFile(path);
}

bool MainWindow::openProjectFile(const std::string& path) {
    stop();
    closeFxWindows();  // they belong to the project being replaced
    plugins->closeEditor();
    project_io::MasterSettings m;
    std::string error;
    if (!project_io::loadProject(path, project, engine.sr, m, error)) {
        ui::msg::warning(this, "Cannot open project", path + "\n\n" + error);
        return false;
    }
    master->apply(m);
    projectPath = path;
    projectReplaced();
    resetView();
    restorePlugins();
    history->reset(engine.sr);
    autosaver->clear();
    markSaved();
    templates::remember(prefs, path);  // (File → Open recent; not backups, recovery files, templates)
    showStatus("Opened " + path, 5000);
    return true;
}

void MainWindow::openBackup() {
    // File → Open a backup…: one of the versions kept each time the project was saved over
    // (project_io::keepBackup). It opens as unsaved changes to the project: Save makes it the project
    // again (keeping the version it replaces as a backup in turn).
    std::string path = projectPath;
    if (path.empty() || project_io::backups(path).empty()) {
        ui::msg::information(this, "Open a backup",
                             "There are no backups of this project yet. Each time you save over a project, the "
                             "version it replaces is kept in a folder next to it (“<name>_backups”), the newest 10.");
        return;
    }
    if (!confirmDiscard()) return;
    std::string chosen = ui::files::openFile(this, "Open a backup", project_io::backupsFolder(path),
                                             "Tiny DAW project (*.json)");
    if (chosen.empty() || !openProjectFile(chosen)) return;
    projectPath = path;
    savedSnapshot = Json();  // (unsaved: it differs from the project's file)
    setTitle(util::baseName(path) + " (from a backup) — Tiny DAW");
    showStatus("Opened the backup " + util::baseName(chosen) + ": save to make it the project again", 10000);
}

void MainWindow::recovered(const std::string& original) {
    projectPath = original;
    savedSnapshot = Json();  // (unsaved: it differs from the file, if there is one)
    std::string name = projectPath.empty() ? "Untitled" : util::baseName(projectPath);
    setTitle(name + " (recovered) — Tiny DAW");
    showStatus("Recovered: save it to keep it (File → Save)", 10000);
}

bool MainWindow::saveProject() {
    // save to the current file (asks for one the first time); true if saved
    if (projectPath.empty()) return saveProjectAs();
    return saveProjectFile(projectPath);
}

bool MainWindow::saveProjectAs() {
    std::string path = ui::files::saveFile(this, "Save project", projectPath.empty() ? "Untitled.json" : projectPath,
                                           "Tiny DAW project (*.json)");
    if (path.empty()) return false;
    if (util::extension(path) != ".json") path += ".json";
    return saveProjectFile(path);
}

bool MainWindow::saveProjectFile(const std::string& path) {
    if (!engine.isOpen())  // with a device open, the audio thread keeps the plugins processing (in sync)
        for (auto& slot : project.pluginSlots()) plugin_audio::syncState(*slot, engine.sr);
    std::string error, backup;
    if (!project_io::keepBackup(path, backup, error)) {  // (the version being replaced: File → Open a backup…)
        int answer = ui::msg::question(this, "Cannot keep a backup",
                                       "The version of " + path + " being replaced couldn't be kept as a backup:\n\n" +
                                           error + "\n\nSave over it anyway?");
        if (answer != ui::msg::Yes) return false;
    }
    error.clear();
    if (!project_io::saveProject(path, project, engine.sr, master->settings(), error)) {
        ui::msg::warning(this, "Cannot save project", path + "\n\n" + error);
        return false;
    }
    projectPath = path;
    autosaver->clear();  // (saved: the recovery copy isn't needed)
    markSaved();
    templates::remember(prefs, path);
    showStatus("Saved " + path + " (audio in " + project_io::audioFolder(path) + ")", 5000);
    return true;
}

// --- periodic updates -----------------------------------------------------------------------

void MainWindow::onFrame() {
    // playhead, time display, level meters and follow-scrolling
    Timeline* tl = timeline;
    syncLoop();  // (also after opening a project or changing the sample rate)
    engine.idle();
    if (midiInput) midiInput->poll();  // (what the MIDI keyboard played)
    automation::resolve(project);  // (parameters of plugins just loaded)
    modulation::resolve(project);  // (and the modulation cables' parameters)
    plugins->idle(project.pluginSlots());  // (values to their windows, latency changes)
    int64_t heard = engine.heardPos();  // (behind the engine's position while plugins hold sound back)
    tl->setPlayhead(heard);
    if (tl->recording) {  // the take grows on its lane
        auto tops = tl->trackTops();
        const TrackPtr& track = tl->recording->first;
        auto it = tops.find(track.get());
        if (it != tops.end()) tl->update(Rect(0, it->second, tl->width(), track->height));
    }
    overview->refresh();
    int64_t shown = engine.playing ? heard : tl->cursor;  // stopped: where Play will start
    transport->setTime((double)shown / engine.sr, project.timing);
    std::vector<float> masterPeaks;
    std::map<const Track*, std::vector<float>> trackPeaks;
    engine.metering->takePeaks(masterPeaks, trackPeaks);  // also queues audio for LUFS
    if (prefs.levelMeters) {
        master->meter->push(masterPeaks);
        for (auto& [track, meter] : panel->meters) {
            auto it = trackPeaks.find(track.get());
            meter->push(it != trackPeaks.end() ? &it->second : nullptr);  // null (silent) lets the meter fall back
        }
        for (auto& [group, meter] : panel->groupMeters) {  // a group's tracks together: the loudest
            std::vector<const std::vector<float>*> peaks;
            for (auto& t : project.members(group)) {
                auto it = trackPeaks.find(t.get());
                if (it != trackPeaks.end()) peaks.push_back(&it->second);
            }
            if (peaks.empty()) {
                meter->push(nullptr);
            } else {
                std::vector<float> loudest = level_meter::loudest(peaks);
                meter->push(&loudest);
            }
        }
    }
    panel->showMixerValues(engine.playing ? std::optional<int64_t>(heard) : std::nullopt, engine.sr);
    if (engine.playing) followPlayhead();
}

void MainWindow::followPlayhead() {
    Timeline* tl = timeline;
    ui::ScrollBar* hbar = scroll->horizontalScrollBar();
    int viewW = scroll->viewport()->width();
    if (tl->playheadX > tl->width() - viewW) tl->relayout();  // grow the timeline as playback runs past the end
    if (transport->followAction->isChecked()) {
        // Once the playhead reaches the middle of the view, the view moves with it and keeps it
        // centred; if it is off-screen (e.g. after a seek) the view jumps to centre it.
        int left = hbar->value(), centre = viewW / 2;
        if (tl->playheadX > left + centre || tl->playheadX < left) hbar->setValue(tl->playheadX - centre);
    }
}

void MainWindow::onCpuTimer() {
    // CPU monitor and LUFS readouts, a few times a second (and MIDI editors whose clip went)
    if (!midiEditors.empty()) checkMidiEditors();
    project.purgeRetired(plat::now());  // (plugins of removed slots, once the audio thread let go)
    std::optional<std::pair<double, double>> load;
    double cur = 0, avg = 0;
    if (engine.isOpen() && engine.cpu.read(config::CPU_AVERAGE_SECONDS, cur, avg)) load = std::make_pair(cur, avg);
    cpuMonitor->showLoad(load, engine.cpu.late);
    {  // each track's share of a core (the monitor's tooltip; the rows' bars: Options → CPU per track)
        std::vector<const Track*> known;
        for (auto& t : *project.tracks) known.push_back(t.get());
        auto shares = engine.trackCpu.read(known);
        if (load) {
            std::vector<std::pair<std::string, double>> top;
            for (auto& s : shares) {
                if (top.size() == 5 || s.second < 0.001) break;
                for (auto& t : *project.tracks)
                    if (t.get() == s.first) top.emplace_back(t->name, s.second);
            }
            cpuMonitor->showTracks(top);
        }
        if (prefs.trackCpu && load)
            for (auto& kv : panel->frames) {
                double share = 0;
                for (auto& s : shares)
                    if (s.first == kv.first.get()) share = s.second;
                kv.second->setLoad(share);
            }
    }
    if (!prefs.loudnessMeters) return;
    auto& metering = *engine.metering;
    metering.updateLoudness();
    double m, s, i;
    metering.read(audio::MASTER, m, s, i);
    master->loudness->showValues(m, s, i);
    for (auto& [track, display] : panel->loudness) {
        metering.read(track.get(), m, s, i);
        display->showValues(m, s, i);
    }
}

// --- window events --------------------------------------------------------------------------

void MainWindow::resized() {
    ui::Window::resized();
    if (explorerDock) setExplorerWidth(explorerWidth);
    if (timeline) timeline->relayout();
}

bool MainWindow::requestClose() {
    if (closing) return true;
    if (askBeforeClosing && !confirmDiscard()) return false;
    closing = true;
    beforeClose();
    autosaver->close();  // (closing normally: nothing to recover)
    closeFxWindows();
    plugins->closeEditor();
    plugins->stopFindingKinds();
    engine.close();
    App::get().quit();
    return true;
}
