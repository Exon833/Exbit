// The right-hand panel (lane_panel.py): one header per track (name, effects, mute, solo, remove,
// output pair, level meters) and the master strip.
//
// A track's Auto button (under its outputs) shows or hides its automation lanes; each lane has a row
// here: its plugin and parameter, + to automate another parameter, ✕ to remove the lane.
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
// A group has a header row above its tracks (fold ▾/▸, mute, solo, ungroup, and a meter showing
// its tracks together); its tracks' rows have a bar in the group's colour down their left side.
// The rows follow the same lanes as the timeline (lanes.h).
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
class MasterStrip;

namespace lane_panel {
constexpr int GROUP_BAR_W = 5;  // px: bar down the left of a group's tracks' rows
Color groupBarColor(const Group& group);
std::vector<int> trackOutputs(const Track& track);  // the device outputs (0-based) a track plays on: its pair
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
    // While playing (pos: the playhead frame), the fader and pan of tracks whose volume or pan is
    // automated show the automation's values; stopped (none), the tracks' own. Every frame.
    void showMixerValues(std::optional<int64_t> pos, int sr);
    void showCurrentTrack();        // outline the selected track's row (the one the timeline records onto)
    void showUsedOutputs();         // group meters: their tracks' outputs; the master meter: every output in use
    // right-click menus
    void fillTrackMenu(ui::Menu& menu, const TrackPtr& track);
    void fillGroupMenu(ui::Menu& menu, const GroupPtr& group);
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
    std::map<GroupPtr, LevelMeter*> groupMeters;  // a group's tracks together
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

private:
    ui::Widget* rowsBox_ = nullptr;  // holds the rows (replaced on every rebuild)
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    LaneFrame* trackHeader(ui::Widget* parent, const TrackPtr& track, Color color);
    LaneFrame* automationHeader(ui::Widget* parent, const Lane& lane);
    LaneFrame* takeHeader(ui::Widget* parent, const Lane& lane);
    LaneFrame* groupHeader(ui::Widget* parent, const GroupPtr& group);
    ui::Window* dialogParent() const;
};
