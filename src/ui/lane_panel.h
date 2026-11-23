// The right-hand panel (lane_panel.py): one header per track (name, effects, mute, solo, remove,
// output pair, level meters) and the master strip.
//
// A track's Auto button (under its outputs) shows or hides its automation lanes; each lane has a row
// here: its plugin and parameter, Write (armed: while playing, moving the parameter writes the lane;
// the row is tinted red: automation_write.h), + to automate another parameter, ✕ to remove the lane.
// Right-click a track's row to arm (or disarm) all its lanes at once.
//
// Each track row has a volume slider under its meter (after its effects; double-click: 0 dB), with
// its setting in dB next to it. A MIDI track has an instrument button (🎹): click it to open its
// instrument's window, ▾ to choose another; a new one, until it has an instrument, shows only its
// name and a "Choose instrument" button across its row (instrument_menu), and is silent.
// Each track row's meter has a bar for each output it plays on (its pair); a group's for its
// tracks' outputs; the master's for every output any track plays on. Rows line up with the timeline
// lanes, each as tall as its track (drag a row's bottom edge, here or in the timeline, to resize
// it); the main window keeps their vertical scrolling in sync. Right-click a row to give the track a
// colour, with the same dialog and automatic contrast as clips, or to group tracks.
//
// Left of M, a pan knob (drag up/down, Shift: finer; double-click: centre). With return tracks in the
// project, → next to Auto opens a slider per return track: how much of the track it gets. A return
// track's row has no solo (soloing other tracks leaves the returns playing), nor groups, freezing or
// bouncing. While playing, the fader and pan of a track whose volume or pan is automated show the
// automation's values (showMixerValues). A track with a delay shows it after its name ("(+12 ms)").
// A take lane (Show takes, right-click the row) has a row with the take's name and length, Use (all
// of it) and ✕ (delete it from the takes).
//
// A group has a header row above its tracks (fold ▾/▸, mute, solo, ungroup); its tracks' rows have a bar in the
// group's colour down their left side. A group with a bus (model.h GROUP BUSES) has the bus's controls in its
// header, as a track's row has them: FX, pan, fader and dB, meter, output pair, Auto (its automation lanes under
// the header) and sends; drag its bottom edge to resize it. Its menu: Fold only (no bus), Output (where the bus
// plays: model/routing.h), Put in group (nesting). A group without a bus has a meter showing its tracks together.
// A track's menu has Output: Master, a group's bus or another track (greyed: it would make a loop).
// The rows follow the same lanes as the timeline (lanes.h).
//
// TRACK SELECTION. Click a track's row to select it; Shift-click selects the tracks from the one clicked last to it,
// Ctrl-click adds a track or takes it out. A change made on a selected track's row goes to every selected track:
// its fader and pan (each moved by as much), mute, solo, colour, an effect added in its FX window (added to each,
// wired in at the end of its chain: track_ops::wiredAtEnd), duplicating and moving; the status bar says how many.
// The current track (the timeline's) is always one of them; selecting another track alone selects only it.
// Right-click a track: Duplicate track (Ctrl+D when no clip is selected), Duplicate without clips, Move up / down
// (Alt+Up / Alt+Down); drag a row up or down by its name to move the track(s) there (model/track_ops.h).
//
// RECORDING SEVERAL TRACKS (ui/recording.h): ● after S arms the track (the selected ones; Shift-click: more); an
// audio track's row has its input before its meter (None, a device input or pair) and its monitoring (the speaker:
// Auto, In, Off) after Auto (record_arm.h). A change goes to every selected track.
//
// ADDING TRACKS: right-click the panel's empty space, or any row: "Add track ▸" (a section of its own on empty space
// and automation or take rows) with Audio track, MIDI track and Return track, the same as Ctrl+T, Ctrl+Shift+T and
// Ctrl+Alt+T (and their commands): placed right below the row's track, in its group (a group's row: below its last
// track, in it), or at the end on empty space; a return always goes at the very bottom. With no tracks yet the panel
// says "Right-click to add a track". (The transport has no track buttons any more.)
//
// KEYBOARD FOCUS (ui/midi_input.h): a MIDI track's row has its focus toggle (KeyFocusButton) before its meter, where
// an audio track has its input: lit while the keyboards play it; click to add the track to them or take it out
// (several lit: one keyboard plays them all); right-click it for Lock keyboard focus (the row's menu has both:
// "Keyboards play this track", for a row too low to show its meter line). The focus follows the selected track unless
// locked (followSelection, as another track becomes the current one).
//
// STYLES: the panel is the stylesheets' `TrackPanel`, its width the :root variable --panel-width. Each row is a
// node `TrackRow` under it: .track (:alternate every other), .automation (:armed while armed for writing),
// .take or .group, and the kind of its track (.audio, .midi, .return). A row reads its background-color (a
// track's or group's own colour wins), border-left-color (the stripe down its left: its kind's, or a group's),
// border-top-color (an automation row's line above it), padding (around its controls) and row-gap / column-gap
// (between its lines / its controls); TrackRow::tint (an armed automation row: a quarter of its colour is mixed
// in) and TrackRow::group-bar (width; background-color: a grouped track's bar, when the group has no colour).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/lanes.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/widgets.h"

class Timeline;
namespace commands { class Registry; }
class MasterStrip;
class LanePanel;

namespace lane_panel {
constexpr int GROUP_BAR_W = 5;  // (default: TrackRow::group-bar's width) px: bar down the left of a group's tracks' rows
int panelWidth();               // --panel-width (PANEL_W)
// the bar down a group's tracks' rows, and its header row's stripe: its own colour, else the skin's (`panel`: the
// node above the rows in the stylesheets; null: the root)
Color groupBarColor(const Group& group, const ui::Widget* panel = nullptr);
std::vector<int> trackOutputs(const Track& track);  // the device outputs (0-based) a track plays on: its pair
// an automation lane's row (of a track of `kind`): tinted red while armed for writing
Color automationRowColor(const Automation& lane, const ui::Widget* panel = nullptr, Kind kind = Kind::Audio);
// A small dialog to tick tracks (those in `checked` already ticked). The ticked ones, or nullopt.
std::optional<std::vector<TrackPtr>> chooseTracks(ui::Window* parent, const std::vector<TrackPtr>& tracks,
                                                  const std::vector<TrackPtr>& checked);
}  // namespace lane_panel

// Invisible strip along the bottom of a track's (or an automation lane's) row: drag it to resize
// it, double-click it for the default height. (The timeline sets and clamps the height.)
class EdgeGrip : public ui::Widget {
public:
    static constexpr int HEIGHT = 5;  // the row's bottom margin, so it never covers the controls
    EdgeGrip(Timeline& timeline, TrackPtr track, AutomationPtr automation);  // one of the two
    void place();                     // along the bottom of its row
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;

private:
    Timeline& timeline_;
    TrackPtr track_;
    AutomationPtr automation_;
    bool dragging_ = false;
    int startY_ = 0, startHeight_ = 0;  // (global mouse y, height) while dragging
    int height() const;
    void setHeight(int h);
};

// A track's row (TRACK SELECTION above): a click selects its track, a drag up or down moves it
class TrackRow : public LaneFrame {
public:
    TrackRow(LanePanel& panel, TrackPtr track, Color color, std::optional<Color> accent, int height,
             std::optional<Color> text);
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;

private:
    LanePanel& panel_;
    TrackPtr track_;
    int startY_ = 0;  // (global y of the press)
    bool pressed_ = false, dragging_ = false;
};

class LanePanel : public ui::Widget {
public:
    // The instruments of a MIDI track, given by the main window (instrument_menu): fill(menu, track)
    // adds the instruments to choose from; edit(track) opens its instrument's window (or patch panel).
    struct Instruments {
        std::function<void(ui::Menu&, const TrackPtr&)> fill;
        std::function<void(const TrackPtr&)> edit;
    };
    // openPlugins(track): the track's plugins window (FX); fillRenderMenu(menu, track): adds freezing
    // and bouncing to a track's right-click menu (track_render)
    LanePanel(Project& project, Timeline& timeline, MasterStrip& masterStrip, int channels,
              std::function<void(const TrackPtr&)> openPlugins,
              std::function<void(ui::Menu&, const TrackPtr&)> fillRenderMenu = nullptr,
              Instruments instruments = {});
    ~LanePanel() override;

    // A device with a different number of outputs was opened: tracks routed to outputs it lacks go
    // back to Out 1/2, and the rows are rebuilt with the new output list.
    void setChannels(int channels);
    void rebuild();                 // recreate the rows: one per lane of the timeline (group headers and the tracks shown)
    // automation
    void toggleAutomation(const TrackPtr& track, bool on);  // Auto: show or hide its lanes; none yet: choose a parameter
    // choose the track's volume or pan, or a parameter of its plugins, and add a lane; true if one was added
    bool addAutomation(const TrackPtr& track);
    void toggleTakes(const TrackPtr& track);                // Show / Hide takes
    void setDelay(const TrackPtr& track);                   // Track delay…: ask how much later (or earlier) it plays
    void setBendRange(const TrackPtr& track);               // Pitch bend range…: ask (MIDI tracks)
    // While playing (pos: the playhead frame), the fader and pan of tracks whose volume or pan is
    // automated show the automation's values; stopped (none), the tracks' own. Every frame.
    void showMixerValues(std::optional<int64_t> pos, int sr);
    void showCurrentTrack();        // outline the selected track's row (the one the timeline records onto)
    void showUsedOutputs();         // group meters: their tracks' outputs; the master meter: every output in use
    // right-click menus
    void fillTrackMenu(ui::Menu& menu, const TrackPtr& track);
    // ADDING TRACKS (the top): Audio, MIDI and Return track, placed below `after` (in `group`: its own when none), else
    // at the end; asSubmenu: under "Add track ▸", else a section of the menu
    void fillAddMenu(ui::Menu& menu, const TrackPtr& after = nullptr, const GroupPtr& group = nullptr,
                     bool asSubmenu = false);
    TrackPtr addTrack(Kind kind, TrackPtr after = nullptr, GroupPtr group = nullptr);  // (what its entries do)
    // KEYBOARD FOCUS (the top): Lock keyboard focus (checkable), for a focus toggle's and a MIDI track's menus; with
    // `track`, first its toggle as an entry ("Keyboards play this track")
    void fillFocusMenu(ui::Menu& menu, const TrackPtr& track = nullptr);
    void setFocusLocked(bool on);  // locking keeps the tracks the keyboards play now (the selected one too)
    void toggleFocus(const TrackPtr& track, bool on);  // its toggle clicked (the terminal's Shift+K)
    void showKeyFocus();           // every focus toggle lit as the keyboards' tracks are now
    std::map<TrackPtr, KeyFocusButton*> focusButtons;  // each MIDI track row's toggle
    bool contextMenu(ui::MouseEvent& e) override;  // empty space: the Add entries
    void paint(Painter& p) override;               // with no tracks: "Right-click to add a track"
    // Alt+Up / Alt+Down as commands that can be rebound (ui/commands.h); its menus then show the keys bound
    void registerCommands(commands::Registry& r);
    commands::Registry* commands_ = nullptr;
    void fillGroupMenu(ui::Menu& menu, const GroupPtr& group);
    // Output: where the track (or a group's bus) plays (model/routing.h); a loop greyed out
    void fillOutputMenu(ui::Menu& menu, const TrackPtr& track);
    void chooseColor(const TrackPtr& track);
    void chooseColor(const GroupPtr& group);
    void groupWith(const TrackPtr& track);
    void rename(const TrackPtr& track);
    void rename(const GroupPtr& group);
    // Options → Level meters / Loudness: show or hide them on every row, master included
    void setMetersVisible(bool levels, bool loudness);
    void updateHeights();           // a lane was resized: resize the rows (not rebuilt, so a drag continues)
    void fitHeight();               // all lanes + master + the timeline's horizontal scroll bar

    Project& project;
    Timeline& timeline;
    MasterStrip& masterStrip;
    std::function<void(const TrackPtr&)> openPlugins;
    std::function<void(ui::Menu&, const TrackPtr&)> fillRenderMenu;
    Instruments instruments;
    // right-click a MIDI track's row → MIDI effects…: opens its MIDI effects window (by default
    // midi_effects_dialog::openMidiEffects; unset: the entry isn't offered)
    std::function<void(const TrackPtr&)> openMidiEffects;
    // right-click a track's row: its presets and MIDI controller links (presets_ui::fillTrackMenu; unset: none)
    std::function<void(ui::Menu&, const TrackPtr&)> fillPresetMenu;
    int channels;
    bool showLevels = true, showLoudness = true;  // Options menu
    // fed by the main window every frame (meters) and every CPU_REFRESH_MS (loudness)
    std::map<TrackPtr, LevelMeter*> meters;
    std::map<TrackPtr, LoudnessDisplay*> loudness;
    std::map<GroupPtr, LevelMeter*> groupMeters;  // a group's tracks together (a group without a bus)
    std::map<GroupPtr, LaneFrame*> groupFrames;   // a group's header row while it has a bus (its meter: meters[bus])
    std::map<TrackPtr, LaneFrame*> frames;        // each track's row
    std::map<AutomationPtr, LaneFrame*> automationFrames;
    struct Mixer {                                // a track row's fader, its dB and its pan knob
        VolumeSlider* slider;
        DbReading* reading;
        PanKnob* pan;
    };
    std::map<TrackPtr, Mixer> mixers;
    std::map<ClipPtr, LaneFrame*> takeFrames;     // each take lane's row

    Point sizeHint() const override;

    // --- TRACK SELECTION (see the top; lane_panel_tracks.cpp) ---
    std::vector<TrackPtr> selected;  // in the project's order
    // what a change on `track`'s row goes to: the selection when it is in it, else the track alone
    std::vector<TrackPtr> targets(const TrackPtr& track) const;
    void select(const TrackPtr& track, int mods = 0);  // a click on its row (plat::ModShift / ModCtrl)
    void duplicate(const TrackPtr& track, bool withClips);  // targets(track), each right below itself
    void move(const TrackPtr& track, int dir);              // targets(track) a lane up (-1) or down (1)
    void dropAt(const TrackPtr& track, int y);              // its row dragged and let go at y (the panel's)
    // an effect `slot` added in owner's FX window: a copy on every other selected track, wired in at the end of
    // its chain; how many tracks got one
    int addToSelected(const TrackPtr& owner, const SlotPtr& slot);
    void say(const std::string& what, size_t count);  // the status bar: "<what>: 3 tracks" when it went to several
    // ● on track's row (and the terminal's Shift+R): arm (or disarm) it, or the selected tracks; arming them alone
    // disarms the other tracks unless `more` (Shift-click). Through `arm` when set (Recording::setArmed: punching
    // in and out while recording), else the flags alone
    void armTrack(const TrackPtr& track, bool on, bool more);
    std::function<void(const std::vector<TrackPtr>&, bool)> arm;
    int inputs = 0;  // the device's input channels (an audio track's input chooser; set by the main window)
    // set by the main window: the status bar, and loading plugins (PluginHost::restore: the names of those that
    // could not be loaded); unset: nothing said, plugins left unloaded
    std::function<void(const std::string&, int)> status;
    std::function<std::vector<std::string>(const std::vector<SlotPtr>&)> loadPlugins;

private:
    TrackPtr anchor_;  // the row clicked last (Shift-click selects from it)
    std::vector<std::unique_ptr<ui::Shortcut>> shortcuts_;
    void showMixer(const TrackPtr& track);           // its fader and pan as the track has them
    ui::Widget* rowsBox_ = nullptr;  // holds the rows (replaced on every rebuild)
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int styled_ = -1;                // the stylesheets' generation the rows were made with
    // a track's row; with asGroup, that group's header row with its bus's controls (track: the bus)
    LaneFrame* trackHeader(ui::Widget* parent, const TrackPtr& track, bool alternate, const GroupPtr& asGroup = nullptr);
    // a return track's row under its name (lane_panel_return.cpp): output and mute and pan, then Auto, FX, the fader on
    // its meter, its dB and its loudness
    void returnControls(LaneFrame* frame, ui::BoxLayout* col, const TrackPtr& track);
    LaneFrame* automationHeader(ui::Widget* parent, const Lane& lane);
    LaneFrame* takeHeader(ui::Widget* parent, const Lane& lane);
    LaneFrame* groupHeader(ui::Widget* parent, const GroupPtr& group);
    ui::Window* dialogParent() const;
};
