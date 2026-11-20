// The main window (main_window.py): creates the project, engine and widgets, and wires them
// together. Also the File menu: new, open, save (see model/project_io.h for the file format).
//
//     +---------------------------------------------------------------------+
//     | File menu                                                           |
//     | TransportBar                                                        |
//     +----------+------------------------------------------+---------------+
//     | Explorer | Overview (whole project, compressed)     | caption       |
//     | (dock,   +------------------------------------------+---------------+
//     | View     | Timeline (in a ScrollArea)               | CpuMonitor    |
//     | menu)    |   ruler                                  +---------------+
//     |          |   track lanes ...                        | LanePanel     |
//     |          |                                          |  (scroll-     |
//     |          |                                          |   synced)     |
//     |          | master lane (pinned over the tracks)     | master row    |
//     +----------+------------------------------------------+---------------+
//     | status bar                                                          |
//     +---------------------------------------------------------------------+
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio/engine.h"
#include "audio/metronome.h"
#include "core/json.h"
#include "model/history.h"
#include "model/model.h"
#include "model/timing.h"
#include "ui/preferences.h"
#include "ui/skin.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

class CpuMonitor;
class Explorer;
class HistoryWindow;
class LanePanel;
class MasterStrip;
class MidiEditor;
class MixExporter;
class Overview;
class PinToBottom;
class PluginHost;
class PluginsDialog;
class Recording;
class Timeline;
class TrackRenderer;
class TransportBar;
class UpdateChecker;
namespace autosave {
class Autosave;
}
namespace midi_input {
class MidiInput;
}
namespace midi_learn {
class Learner;
}
namespace computer_keyboard {
class ComputerKeyboard;
}

// how often a change to the project is looked for, to become an undo step (history.h)
constexpr int HISTORY_MS = 250;

// What the widgets lean on, kept in a base class so that it outlives them: the window's widgets
// (its children) are destroyed by ui::Window's destructor, which runs before this one's.
struct MainWindowCore {
    MainWindowCore();
    ~MainWindowCore();
    Project project;
    audio::Engine engine;
    Preferences prefs;
    std::unique_ptr<PluginHost> plugins;
    std::unique_ptr<MasterStrip> master;
    std::unique_ptr<TrackRenderer> renderer;
    std::unique_ptr<MixExporter> exporter;
    std::unique_ptr<Recording> recording;
    std::unique_ptr<history::History> history;     // undo / redo (Edit menu)
    std::unique_ptr<autosave::Autosave> autosaver; // autosave and crash recovery
    std::unique_ptr<midi_input::MidiInput> midiInput;                 // Options → MIDI input…
    std::unique_ptr<computer_keyboard::ComputerKeyboard> keyboard;    // the computer keyboard as a piano
};

class MainWindow : private MainWindowCore, public ui::Window {
public:
    MainWindow();
    ~MainWindow() override;

    using MainWindowCore::project;
    using MainWindowCore::engine;
    using MainWindowCore::prefs;
    using MainWindowCore::plugins;
    using MainWindowCore::master;
    using MainWindowCore::renderer;
    using MainWindowCore::exporter;
    using MainWindowCore::recording;
    using MainWindowCore::history;
    using MainWindowCore::autosaver;
    using MainWindowCore::midiInput;
    using MainWindowCore::keyboard;

    std::string projectPath;             // JSON file of the open project, once saved or opened ("" none)
    skin::Skin skin;
    std::vector<std::string> skinProblems;  // what its stylesheets left out (Options → Skin → Problems)
    // a track's (or the master's) open plugins window, one per owner: (0, track) effects, (1, track)
    // its instrument patch panel, (0, nullptr) the master's
    std::map<std::pair<int, const void*>, PluginsDialog*> fxWindows;
    std::map<const Clip*, MidiEditor*> midiEditors;  // a clip's open MIDI editor
    int shownWrites_ = 0;  // automation::recorder().writes() last repainted (curves written by moving controls)
    metronome::MetronomeSettings metronomeSettings;
    std::string device;                  // the output device's id ("" none)
    TapTempo tapTempo;
    Json savedSnapshot;                  // (null: unsaved whatever the project is, e.g. a backup or a recovery opened)
    // the windows that render their track's clips a moment after a change (MIDI effects, sampler):
    // registered by them, so undo / redo first lets them finish (a change and how the clips then sound
    // are one step), and they show the track afresh after it
    struct RenderWindow {
        std::function<bool()> pending;   // waiting to render
        std::function<void()> flush;     // render now
        std::function<void()> reload;    // show the track as it is now
    };
    std::map<const void*, RenderWindow> renderWindows;

    // --- widgets ---
    ui::MenuBar* menuBar = nullptr;
    TransportBar* transport = nullptr;
    ui::ScrollArea* scroll = nullptr;
    Timeline* timeline = nullptr;
    ui::ScrollArea* panelScroll = nullptr;
    LanePanel* panel = nullptr;
    CpuMonitor* cpuMonitor = nullptr;
    Overview* overview = nullptr;
    ui::Label* overviewCaption = nullptr;
    ui::Widget* frameGap = nullptr;      // above the CPU monitor: as tall as the timeline frame's top
    Explorer* explorer = nullptr;
    ui::Widget* explorerDock = nullptr;  // the Explorer with its title bar
    ui::SplitterHandle* explorerHandle = nullptr;
    int explorerWidth = 0;
    ui::StatusBar* status = nullptr;
    std::vector<std::unique_ptr<PinToBottom>> masterPins;
    std::unique_ptr<UpdateChecker> updates;
    // menu entries
    ui::Action* explorerAction = nullptr;
    ui::Action* scopeAction = nullptr;  // Options → Master scope
    ui::Menu* skinMenu = nullptr;
    ui::Action* roundedAction = nullptr;
    ui::Action* transparencyAction = nullptr;  // Options → Window frames → Transparency
    ui::Action* undoAction = nullptr;
    ui::Action* redoAction = nullptr;
    std::map<int, ui::Action*> sizeActions;  // Options → Interface size (%)
    std::vector<std::unique_ptr<ui::Shortcut>> extraShortcuts;  // (Ctrl+Y: redo's second shortcut)
    std::unique_ptr<HistoryWindow> historyWindow;  // Edit → History… (made when first opened; hidden when closed)

    void showStatus(const std::string& text, int ms = 0);

    // --- setup ---
    void buildLayout();
    void buildFileMenu();
    void buildEditMenu();
    void buildViewMenu();
    void showAllAutomation(bool on);
    void buildOptionsMenu();
    void setExplorerVisible(bool on);    // View → Explorer, the dock's ✕
    void setExplorerWidth(int w);
    // the timeline's frame (ScrollArea#timeline's border and padding): the track panel's rows moved down by its
    // top, its bottom kept free under the master row, so they still line up with the lanes
    void fitTimelineFrame();

    // --- Edit menu: undo / redo (history.h), clips' clipboard, time ---
    void showTimeEdit(const std::string& message);
    void useClipboard(const std::string& what);  // "cut", "copy", "paste"
    void noteChanges();                  // a change becomes an undo step (while no mouse button is held)
    bool rendersPending();
    void flushRenders();
    void showUndoState();
    void undo();
    void redo();
    void stepHistory(bool forward);
    void showHistory();                  // Edit → History…: the steps by name (history_window.h)
    void jumpHistory(int position);      // …a row clicked: undo or redo to just after that step
    void historyMoved(bool forward);     // (after undo / redo / a jump: everything shows the project afresh)
    bool mouseHeld() const;
    bool dragging();                     // the timeline or a MIDI editor is in the middle of a drag
    void reloadEditors();                // the MIDI editors show their clip as it is now (after undo)
    std::string insertTime();            // Edit → Insert time… (the timeline's; a message for the status bar)
    std::string deleteTime();
    void addCaptureAction(ui::Menu* menu);

    // --- File menu additions ---
    void importAbleton();                // Import Ableton Live set…
    void exportStems();
    void exportMidi(const std::vector<ClipPtr>& clips);

    // --- Options → Interface size (from the next start: main.cpp) ---
    bool interfaceSizeFits(int size);
    void offerInterfaceSizes();
    void setInterfaceScale(int size);
    void setWindowFrames(bool own);  // Options → Window frames: Tiny DAW's own, or the system's
    void setTransparency(bool on);   // Options → Window frames → Transparency
    void offerTransparency();        // …enabled only while a compositing manager runs
    void addMidiOptions(ui::Menu* menu); // Options: Recording offset…, MIDI input…, Computer keyboard

    // --- skins ---
    void fillSkinMenu();
    void collectSkinProblems();  // what the stylesheets showed wrong only while drawing (a var()'s value), added to the list
    void chooseSkin(const skin::Skin& s);
    void showSkin(const skin::Skin& s);
    void restyle();
    void setRoundedCorners(bool on);
    void overviewResized(int height);
    void setColourOption(const std::string& attr, bool on);  // "random_track_colors" / "clips_follow_track"
    void applyColourOptions();
    void editSkin();          // Options → Skin → Edit this skin…: its stylesheet in your text editor
    void showSkinProblems();  // what the skin's stylesheets left out
    std::string skinFilesStamp() const;  // the skin's and default.css's times and sizes
    void checkSkinFile();     // reloads the skin when its stylesheet (or default.css) was saved
    void updateMedia();       // the window's size and the interface size, for @media and vw / vh
    void importSkin();
    void exportSkin();
    void openSkinsFolder();
    void chooseVst2Folders();
    void chooseWinePrefixes();
    void showScope(bool on);  // Options → Master scope
    void installWindowsPlugin();  // Options → Install Windows plugin… (wine_setup.h)
    void showAudioProperties();
    void setAudioDevice(const std::string& driverType, const std::string& device, int bufferSize, bool exclusive = false);
    void setBufferSize(int frames);
    void setRefreshRate(int fps);
    void setMeterOption(const std::string& attr, bool on);   // "level_meters" / "loudness_meters"
    void applyMeterOptions();
    void connectControls();
    void connectScale();  // the transport's Key ▾ and Swing ▾ (main_window_scale.cpp, ui/song_scale.h)
    void tap();

    // --- metronome ---
    void setMetronomeOn(bool on);
    void applyMetronome();
    void showMetronomeSettings();
    void testClick(const metronome::Click& click);
    void setLooping(bool on);
    void syncLoop();
    struct TimingChange {
        std::optional<double> bpm;
        std::optional<int> beatsPerBar;
        std::optional<bool> beats, thirds;
    };
    void setTiming(const TimingChange& changes);
    void startTimers();
    void openFirstDevice();

    // --- actions ---
    void openDevice();
    void addFiles();
    void goToStart();
    void togglePlay();
    void resetMeters();                  // playback starts: the loudest peaks and clip lights count from now

    // --- plugins ---
    void openTrackPlugins(const TrackPtr& track);
    void openMasterPlugins();
    void fillInstrumentMenu(ui::Menu& menu, const TrackPtr& track);
    void instrumentChosen(const TrackPtr& track);
    void openInstrumentPanel(const TrackPtr& track);
    void editInstrument(const TrackPtr& track);
    void showFxWindow(std::pair<int, const void*> key, const std::function<PluginsDialog*()>& make);
    void checkFxWindows();
    void closeFxWindows();
    void openMidiEditor(const ClipPtr& clip);
    void notesEdited(const Clip* from = nullptr);  // (from: the clip whose editor changed it; its linked clips' editors reload)
    void checkMidiEditors();
    void restorePlugins();

    // --- projects ---
    Json snapshot();
    void markSaved();
    bool hasUnsavedChanges();
    bool confirmDiscard();
    void stop();
    void resetView();
    void newProject();
    void openProject();
    void openRecent(const std::string& path);         // File → Open recent ▸ (templates.h)
    void newFromTemplate(const std::string& path);    // File → New from template ▸
    bool startFromTemplate(const std::string& path);  // a new, untitled project from a template; true if done
    void openBackup();                                // File → Open a backup…
    void recovered(const std::string& original);      // (autosave.h) a recovery file was opened
    void projectReplaced();              // (after project.clear() / loading: what follows the project's timing…)
    void sampleRateChanged(int oldSr, int newSr);
    void beforeClose();                  // the computer keyboard, MIDI input let go
    void setUpMidi();                    // MIDI input, the computer keyboard, Capture, the MIDI windows' host
    void capture();                      // Capture: what was just played, as a clip
    void chooseMidiInput();
    void showRecordOffset();             // Options → Recording offset…
    void fillGridMenu(ui::Menu* menu);   // the Grid ▾ button's menu
    void changeGrid(std::optional<double> grid, std::optional<bool> snap = {}, std::optional<bool> finer = {});
    ui::Action* keysAction = nullptr;    // Options → Computer keyboard (the transport's ⌨ when it has one)
    bool openProjectFile(const std::string& path);
    bool saveProject();
    bool saveProjectAs();
    bool saveProjectFile(const std::string& path);

    // --- presets and MIDI learn (main_window_presets.cpp; presets_ui.h, midi_learn.h) ---
    std::unique_ptr<midi_learn::Learner> learner;  // MIDI controller links (the project's), learnt from MIDI input
    void setUpPresets();                 // the presets' menus, the Explorer's Presets place, MIDI learn
    void addPresetOptions(ui::Menu* menu);  // Options → MIDI controller links…
    void showMidiLinks();
    void showMixerOf(const TrackPtr& track);  // a track's fader and pan moved by a MIDI controller: its row shows it

    // --- periodic updates ---
    void onFrame();
    void followPlayhead();
    void onCpuTimer();

    // --- window events ---
    void resized() override;
    bool requestClose();                 // the title bar's ✕: confirm, then shut down
    bool closing = false;
    bool askBeforeClosing = true;        // (the test script's quit: false)

private:
    int frameId_ = -1, cpuTimer_ = -1, historyTimer_ = -1, skinTimer_ = -1, updateTimer_ = -1, noticeTimer_ = -1;
    std::string skinStamp_;  // skinFilesStamp() when the skin was last put on
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
