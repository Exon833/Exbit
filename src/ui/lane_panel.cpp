// The right-hand panel: one row per track, group and automation lane (lane_panel.py).
#include "ui/lane_panel.h"

#include "ui/commands.h"
#include "model/pad_tracks.h"
#include "model/routing.h"
#include "ui/pad_kit_dialog.h"
#include "core/util.h"
#include "ui/song_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/automation.h"
#include "audio/automation_write.h"
#include "audio/live.h"
#include "audio/modulation.h"
#include "audio/plugin_audio.h"
#include "plugins/wine.h"
#include "ui/automation_dialog.h"
#include "ui/color_picker.h"
#include "ui/colors.h"
#include "ui/master_strip.h"
#include "ui/midi_effects_dialog.h"
#include "ui/midi_input.h"
#include "ui/midi_ports_dialog.h"
#include "ui/paint_helpers.h"
#include "ui/record_arm.h"
#include "ui/recording.h"
#include "ui/solo_button.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/style.h"
#include "ui/track_dialogs.h"

namespace {

const char* kindClass(Kind kind) { return kind == Kind::Midi ? "midi" : kind == Kind::Return ? "return" : "audio"; }

// what the stylesheets give a row: TrackRow with these classes and states under the panel, its ::tint and ::group-bar
struct RowStyle {
    // (controls: a track's grid under its name; volume: its slider and dB; autoCell: Auto and the sends button)
    css::Style row, tint, bar, controls, volume, autoCell;
    RowStyle(const ui::Widget* panel, std::vector<std::string> classes, std::vector<std::string> states = {}) {
        auto st = paint::nodeStyles(panel, "TrackRow", classes, states,
                                    {"", "tint", "group-bar", "controls", "volume", "auto"});
        row = std::move(st[0]);
        tint = std::move(st[1]);
        bar = std::move(st[2]);
        controls = std::move(st[3]);
        volume = std::move(st[4]);
        autoCell = std::move(st[5]);
    }
    Color background(Color def) const { return ui::style::color(row, "background-color", def); }
    std::optional<Color> stripe() const {
        if (!row.has("border-left-color")) return std::nullopt;
        return ui::style::color(row, "border-left-color");
    }
    // padding (left, top, right, bottom) and gaps of its layouts
    void margins(ui::BoxLayout* layout, int left, int top, int right, int bottom) const {
        layout->setContentsMargins(ui::style::px(row, "padding-left", left), ui::style::px(row, "padding-top", top),
                                   ui::style::px(row, "padding-right", right), ui::style::px(row, "padding-bottom", bottom));
    }
    int rowGap(int def) const { return ui::style::px(row, "row-gap", def); }
    int columnGap(int def) const { return ui::style::px(row, "column-gap", def); }
};

// an automation lane's row as a box: TrackRow.automation (:armed while armed for writing)
css::Style automationRowStyle(const Automation& lane, const ui::Widget* panel, Kind kind) {
    std::vector<std::string> states;
    if (lane.armed) states.push_back("armed");
    return RowStyle(panel, {"automation", kindClass(kind)}, states).row;
}

}  // namespace

namespace lane_panel {

int panelWidth() { return theme::metric("--panel-width", config::PANEL_W); }

Color groupBarColor(const Group& group, const ui::Widget* panel) {
    if (!group.color.empty()) return paint::groupFill(group);
    RowStyle st(panel, {"group"});
    return st.stripe().value_or(theme::c("group_accent"));
}

std::vector<int> trackOutputs(const Track& track) { return {track.output, track.output + 1}; }

Color automationRowColor(const Automation& lane, const ui::Widget* panel, Kind kind) {
    std::vector<std::string> states;
    if (lane.armed) states.push_back("armed");
    RowStyle st(panel, {"automation", kindClass(kind)}, states);
    Color base = st.background(theme::c("automation_lane"));
    if (!lane.armed) return base;
    Color red = ui::style::background(st.tint);  // (armed for writing: tinted red, as in the timeline)
    if (red.a == 0) return base;
    auto mix = [](int a, int b) { return (a * 3 + b) / 4; };
    return Color(mix(base.r, red.r), mix(base.g, red.g), mix(base.b, red.b), base.a);
}

std::optional<std::vector<TrackPtr>> chooseTracks(ui::Window* parent, const std::vector<TrackPtr>& tracks,
                                                  const std::vector<TrackPtr>& checked) {
    ui::Dialog dialog(parent, "Group tracks", 320, 300);
    dialog.addStyleClass("group-tracks");  // (default.css: Dialog.group-tracks, its room)
    ui::VBox* col = ui::styleRoom(ui::vbox(&dialog, 9, 6), dialog.computedStyle());
    col->addWidget(dialog.add<ui::Label>("Tracks to put in a new group:"));
    auto* box = dialog.add<ui::ListWidget>();
    for (auto& track : tracks) {
        ui::ListWidget::Item item;
        item.text = track->name + (track->group ? "   (now in " + track->group->name + ")" : "");
        item.checkable = true;
        item.checked = std::find(checked.begin(), checked.end(), track) != checked.end();
        box->addItem(item);
    }
    col->addWidget(box, 1);
    auto* buttons = dialog.add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([&dialog] { dialog.accept(); });
    buttons->rejected.connect([&dialog] { dialog.reject(); });
    col->addWidget(buttons);
    if (dialog.exec() != ui::Dialog::Accepted) return std::nullopt;
    std::vector<TrackPtr> out;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (box->item((int)i).checked) out.push_back(tracks[i]);
    return out;
}

}  // namespace lane_panel

// ============================================================================================
// EdgeGrip
// ============================================================================================

EdgeGrip::EdgeGrip(Timeline& timeline, TrackPtr track, AutomationPtr automation)
    : timeline_(timeline), track_(std::move(track)), automation_(std::move(automation)) {
    setCursor(ui::Cursor::SizeVer);
    setToolTip("Drag to resize the track; double-click for the default height");
}

void EdgeGrip::place() {
    ui::Widget* row = parent();
    if (!row) return;
    setGeometry(0, row->height() - HEIGHT, row->width(), HEIGHT);
    raise();
}

int EdgeGrip::height() const { return track_ ? track_->height : automation_ ? automation_->height : 0; }

void EdgeGrip::setHeight(int h) {
    if (track_) timeline_.setTrackHeight(*track_, h);
    else if (automation_) timeline_.setTrackHeight(*automation_, h);
}

bool EdgeGrip::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton) {
        dragging_ = true;
        startY_ = e.globalPos.y;
        startHeight_ = height();
    }
    return true;
}

bool EdgeGrip::mouseMove(ui::MouseEvent& e) {
    if (dragging_) setHeight(startHeight_ + e.globalPos.y - startY_);
    return true;
}

bool EdgeGrip::mouseRelease(ui::MouseEvent& e) {
    dragging_ = false;
    return true;
}

bool EdgeGrip::mouseDoubleClick(ui::MouseEvent& e) {
    if (track_) setHeight(Timeline::defaultHeight(*track_));
    else if (automation_) setHeight(Timeline::defaultHeight(*automation_));
    return true;
}

namespace {

// the grip along the bottom of a row, following the row's size
void addGrip(LaneFrame* frame, Timeline& timeline, TrackPtr track, AutomationPtr automation) {
    auto* grip = frame->add<EdgeGrip>(timeline, std::move(track), std::move(automation));
    auto previous = frame->onEventFilter;
    frame->onEventFilter = [grip, previous](ui::Widget* w, const char* what) {
        if (std::string(what) == "resize") grip->place();
        return previous && previous(w, what);
    };
    grip->place();
}

template <class T>
T* nest(ui::BoxLayout* into, int stretch = 0) {
    auto l = std::make_unique<T>();
    T* raw = l.get();
    into->addLayout(std::move(l), stretch);
    return raw;
}

Color kindAccent(Kind kind) { return colors::trackAccent(kind); }  // (when TrackRow sets no border-left-color)

std::string signedMs(double ms) {  // Python's f"{ms:+g}"
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+g", ms);
    return buf;
}

// a widget holding a row of widgets (a grid cell with more than one)
ui::HBox* cellRow(ui::Widget* parent, ui::Widget*& cell, int spacing) {
    cell = parent->add<ui::Widget>();
    return ui::hbox(cell, 0, spacing);
}

}  // namespace

// ============================================================================================
// LanePanel
// ============================================================================================

LanePanel::LanePanel(Project& project_, Timeline& timeline_, MasterStrip& masterStrip_, int channels_,
                     std::function<void(const TrackPtr&)> openPlugins_,
                     std::function<void(ui::Menu&, const TrackPtr&)> fillRenderMenu_, Instruments instruments_)
    : project(project_), timeline(timeline_), masterStrip(masterStrip_), openPlugins(std::move(openPlugins_)),
      fillRenderMenu(std::move(fillRenderMenu_)), instruments(std::move(instruments_)), channels(channels_) {
    openMidiEffects = [](const TrackPtr& track) { midi_effects_dialog::openMidiEffects(track); };
    setStyleType("TrackPanel");
    setFixedWidth(lane_panel::panelWidth());
    ui::VBox* lay = ui::vbox(this, 0, 0);
    lay->addStretch(1);  // (the master row stays at the bottom of the screen: the main window pins it there)
    std::weak_ptr<bool> alive = alive_;
    timeline.tracksChanged.connect([this, alive] {
        if (!alive.expired()) rebuild();
    });
    timeline.currentTrackChanged.connect([this, alive] {
        if (alive.expired()) return;
        midi_input::followSelection(project, timeline.currentTrack);  // (KEYBOARD FOCUS: unless locked)
        showCurrentTrack();
    });
    timeline.heightsChanged.connect([this, alive] {
        if (!alive.expired()) updateHeights();
    });
    // TRACK SELECTION's keys: Ctrl+D with no clip selected (the timeline's), Alt+Up / Alt+Down
    timeline.duplicateTracks = [this, alive] {
        if (!alive.expired() && timeline.currentTrack) duplicate(timeline.currentTrack, true);
    };
    for (int dir : {-1, 1})
        shortcuts_.push_back(std::make_unique<ui::Shortcut>(
            this, ui::KeySequence::parse(dir < 0 ? "Alt+Up" : "Alt+Down"), [this, dir] {
                if (timeline.currentTrack) move(timeline.currentTrack, dir);
            }));
    rebuild();
}

LanePanel::~LanePanel() { *alive_ = false; }

ui::Window* LanePanel::dialogParent() const {
    ui::Window* w = window();
    return w ? w : ui::App::get().mainWindow;
}

Point LanePanel::sizeHint() const {
    return Point(lane_panel::panelWidth(), lanes::layout(project).back().bottom() + theme::scrollbarWidth());
}

void LanePanel::setChannels(int channels_) {
    channels = channels_;
    for (auto& track : project.tracks)
        if (track->output >= channels) track->output = 0;
    masterStrip.meter->setChannels(channels);
    rebuild();
}

namespace {
// The rows, one under another, each as tall as its lane (a fixed height), all as wide as the panel: placed by
// hand, not by a VBox, which measures every row's controls on any change (a row's height dragged with 400 tracks:
// a quarter of a second each step)
class RowStack : public ui::Widget {
public:
    Point sizeHint() const override {
        int h = 0;
        for (auto& c : children())
            if (c->isVisible() && !c->deleted_) h += c->minimumSize().y;
        return Point(width(), h);
    }
    Point minimumSizeHint() const override { return sizeHint(); }
    void resized() override { place(); }
    void childSizeHintChanged(ui::Widget*) override {
        if (building) return;  // (the rows being made: placed once, after)
        place();
        updateGeometry();
    }
    bool building = true;
    void place() {
        if (width() <= 0) return;  // (not placed in the panel yet: the rows laid out once, when it is)
        int y = 0;
        for (auto& c : children()) {
            if (!c->isVisible() || c->deleted_) continue;
            int h = c->minimumSize().y;
            c->setGeometry(Rect(0, y, width(), h));
            y += h;
        }
    }
};
}  // namespace

void LanePanel::rebuild() {
    if (rowsBox_) {
        rowsBox_->hide();  // (deleted after the current event)
        static_cast<ui::BoxLayout*>(layout())->removeWidget(rowsBox_);
        rowsBox_->deleteLater();
    }
    auto oldMeters = meters;  // (the new ones go on from them: takeOver; the old rows are deleted later)
    auto oldGroups = groupMeters;
    meters.clear();
    frames.clear();
    loudness.clear();
    groupMeters.clear();
    groupFrames.clear();
    automationFrames.clear();
    mixers.clear();
    takeFrames.clear();
    focusButtons.clear();
    bool pruned = false;
    for (auto& t : project.allTracks()) pruned = automation::prune(*t) || pruned;  // (lanes of plugins that are gone)
    // (another skin: its sizes, the panel's width and the lanes' heights, may be others; the main window rebuilds
    // the rows after a skin change)
    bool restyled = styled_ != theme::styles().generation();
    styled_ = theme::styles().generation();
    if (restyled) setFixedWidth(lane_panel::panelWidth());
    if (pruned || restyled) timeline.relayout();
    auto* rows = add<RowStack>();
    rowsBox_ = rows;
    static_cast<ui::BoxLayout*>(layout())->insertWidget(0, rowsBox_);
    int row = 0;  // track rows alternate in shade, like their lanes
    std::vector<Lane> all = lanes::layout(project);
    for (size_t i = 0; i + 1 < all.size(); ++i) {
        const Lane& lane = all[i];
        LaneFrame* frame;
        if (lane.automation) {
            frame = automationHeader(rowsBox_, lane);
            automationFrames[lane.automation] = frame;
        } else if (lane.take) {
            frame = takeHeader(rowsBox_, lane);
            takeFrames[lane.take] = frame;
        } else if (lane.group) {
            frame = groupHeader(rowsBox_, lane.group);
        } else {
            frame = trackHeader(rowsBox_, lane.track, row % 2 != 0);
            frames[lane.track] = frame;
            ++row;
        }
        frame->applyTextColor();
    }
    rows->building = false;
    rows->place();
    for (auto& [track, meter] : meters) {
        auto it = oldMeters.find(track);
        if (meter && it != oldMeters.end() && it->second && ui::Widget::alive(it->second)) meter->takeOver(*it->second);
    }
    for (auto& [group, meter] : groupMeters) {
        auto it = oldGroups.find(group);
        if (meter && it != oldGroups.end() && it->second && ui::Widget::alive(it->second)) meter->takeOver(*it->second);
    }
    showUsedOutputs();
    setMetersVisible(showLevels, showLoudness);
    showCurrentTrack();
    fitHeight();
    update();  // (the hint with no tracks: shown or gone)
}

LaneFrame* LanePanel::trackHeader(ui::Widget* parent, const TrackPtr& track, bool alternate, const GroupPtr& asGroup) {
    std::vector<std::string> states;
    if (alternate) states.push_back("alternate");
    // (a group's header with its bus: the group's look, the bus's controls)
    RowStyle style(this, asGroup ? std::vector<std::string>{"group"} : std::vector<std::string>{"track", kindClass(track->kind)},
                   states);
    Color color = style.background(theme::c(asGroup ? "group_lane" : alternate ? "row_alt" : "row"));
    std::optional<Color> text;
    const std::string& ownColor = asGroup ? asGroup->color : track->color;
    if (!ownColor.empty()) {
        Color bg, fg;
        colors::trackColors(ownColor, asGroup ? asGroup->tone : track->tone, bg, fg);
        color = bg;
        text = fg;
    }
    LaneFrame* frame;
    if (asGroup) {
        frame = parent->add<LaneFrame>(color, lane_panel::groupBarColor(*asGroup, this), lanes::headerHeight(*asGroup), text);
        frame->fillMenu = [this, asGroup](ui::Menu& menu) { fillGroupMenu(menu, asGroup); };
    } else {  // (clicking the row selects the track, dragging it moves it: TrackRow)
        frame = parent->add<TrackRow>(*this, track, color, style.stripe().value_or(kindAccent(track->kind)), track->height,
                                      text);
        frame->fillMenu = [this, track](ui::Menu& menu) { fillTrackMenu(menu, track); };
    }
    frame->setRowStyle(style.row);  // (its box: TrackRow's, in its colour)
    ui::HBox* outer = ui::hbox(frame, 0, 0);
    if (track->group && !asGroup) {
        auto* bar = frame->add<ui::Canvas>();
        GroupPtr group = track->group;
        // (TrackRow::group-bar: a box in the group's colour, else its own)
        Color fill;
        if (!paint::groupColor(*group, fill)) fill = ui::style::color(style.bar, "background-color", theme::c("group_accent"));
        css::Style barBox = paint::withBackground(style.bar, fill);
        bar->onPaint = [bar, barBox](Painter& p) { ui::style::box(p, bar->rect(), barBox); };
        bar->setFixedWidth(ui::style::px(style.bar, "width", lane_panel::GROUP_BAR_W));
        bar->vPolicy = ui::SizePolicy::Expanding;
        bar->setToolTip("In " + group->name);
        outer->addWidget(bar);
    }
    auto* col = nest<ui::VBox>(outer, 1);
    style.margins(col, 8, 5, 8, 5);
    col->setSpacing(style.rowGap(4));
    auto* row = nest<ui::HBox>(col);
    row->setSpacing(style.columnGap(4));
    Timeline* tl = &timeline;
    std::string delay = track->delayMs ? "  (" + signedMs(track->delayMs) + " ms)" : "";
    if (asGroup) {
        row->addWidget(widgets::iconButton(frame, asGroup->folded ? "▸" : "▾", [tl, asGroup] { tl->toggleFold(asGroup); },
                                           asGroup->folded ? "Show its tracks" : "Fold its tracks away"));
        auto* name = frame->add<NameLabel>(asGroup->name + delay, true);
        name->doubleClicked.connect([this, asGroup] { rename(asGroup); });
        name->setToolTip("A group bus: its tracks play through its effects, fader and pan. Double-click to rename; "
                         "right-click for its colour, output, or Fold only (no bus)");
        row->addWidget(name, 1);
    } else {
        auto* name = frame->add<NameLabel>((track->frozen ? "❄ " + track->name : track->name) + delay);
        if (track->kind == Kind::Return) name->addStyleClass("return");  // (larger: NameLabel.return)
        name->doubleClicked.connect([this, track] { rename(track); });
        std::string input = pad_tracks::inputName(project, *track);  // (a Pad Kit's pad on its own track)
        name->setToolTip(track->frozen ? "Frozen: it plays its rendering; its clips and plugins are locked until you unfreeze "
                                         "it (right-click)"
                         : !input.empty() ? "Input: " + input + " (that pad's sound, before its effects). Double-click to "
                                            "rename; right-click the row for Back to the kit"
                                          : "Double-click to rename. Right-click the row for the track colour, groups, freezing "
                                            "and bouncing");
        row->addWidget(name, 1);
    }
    ui::Button* remove = asGroup ? widgets::iconButton(frame, "✕", [tl, asGroup] { tl->ungroup(asGroup); },
                                                       "Ungroup (the tracks stay)")
                                 : widgets::iconButton(frame, "✕", [tl, track] { tl->removeTrack(track); },
                                                       "Remove " + track->name);
    if (track->kind == Kind::Return && !asGroup) {  // a return's own layout (lane_panel_return.cpp)
        row->addWidget(remove, 0, AlignTop);
        returnControls(frame, col, track);
        addGrip(frame, timeline, track, nullptr);
        return frame;
    }
    if (track->kind == Kind::Midi && track->needsInstrument) {  // nothing else until it has an instrument
        row->addWidget(remove);
        auto* choose = frame->add<ui::Button>("Choose instrument");
        choose->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
        choose->setFocusPolicy(ui::FocusPolicy::None);
        choose->setToolTip("The track is silent until it has an instrument: the built-in synth or a VST3 instrument");
        ui::Menu* menu = widgets::ownedMenu(choose);
        auto fill = instruments.fill;
        menu->aboutToShow = [fill, track](ui::Menu* m) {
            m->clear();
            if (fill) fill(*m, track);
        };
        choose->setMenu(menu);  // (clicking shows it)
        col->addWidget(choose, 1);
        addGrip(frame, timeline, track, nullptr);
        return frame;
    }

    std::weak_ptr<bool> alive = alive_;
    // (mute, solo: every selected track's)
    auto setFlag = [this, alive, tl, track](bool Track::*flag, bool on, const char* what) {
        auto ts = alive.expired() ? std::vector<TrackPtr>{track} : targets(track);
        for (auto& t : ts) (*t).*flag = on;
        tl->update();  // silent clips are drawn grey
        if (ts.size() > 1 && !alive.expired()) {
            rebuild();  // (their buttons)
            say(what, ts.size());
        }
    };
    if (track->kind == Kind::Midi) {
        auto fill = instruments.fill;
        auto edit = instruments.edit;
        row->addWidget(widgets::instrumentButton(
            frame, track->instrument, [edit, track] { if (edit) edit(track); },
            [fill, track](ui::Menu& m) { if (fill) fill(m, track); }, track->usesRack ? (int)track->rack.size() : -1,
            !track->instrument && !track->usesRack ? track->sampler : nullptr,
            !track->instrument && !track->usesRack ? track->padKit : nullptr,
            !track->instrument && !track->usesRack && track->synth && track->synth->drawn));
    }
    auto open = openPlugins;
    row->addWidget(widgets::fxButton(frame, *track->effects, [open, track] { if (open) open(track); }));
    auto* pan = frame->add<PanKnob>(track->pan, [this, alive, track](double v) {
        if (!alive.expired())  // (the other selected tracks' by as much)
            for (auto& t : targets(track))
                if (t != track) {
                    t->pan = std::clamp(t->pan + v - track->pan, -1.0, 1.0);
                    showMixer(t);
                }
        track->pan = v;  // the mixer reads it every buffer
        automation::recorder().move(automation::armedControlLane(*track, automation::PAN), (v + 1) / 2);
    });
    if (asGroup) pan->addStyleClass("group");  // (PanKnob.group: on a group's row, whose colour is the knob's)
    // (its Pan lane armed for writing, while playing: the knob held writes it: automation_write.h)
    pan->grabbed = [this, alive, track, pan](bool on) {
        if (on && !alive.expired()) say("Pan", targets(track).size());
        AutomationPtr lane = automation::armedControlLane(*track, automation::PAN);
        if (on) automation::recorder().grab(lane, (pan->pan + 1) / 2);
        else automation::recorder().release(lane);
    };
    row->addWidget(pan);
    if (asGroup) {  // (the group's mute and solo: everything in it)
        auto groupFlag = [tl](bool& flag, bool on) {
            flag = on;
            tl->update();  // silent clips are drawn grey
        };
        row->addWidget(widgets::toggleButton(frame, "M", "mute", asGroup->muted,
                                             [groupFlag, asGroup](bool on) { groupFlag(asGroup->muted, on); },
                                             "Mute the group"));
        row->addWidget(widgets::toggleButton(frame, "S", "solo", asGroup->soloed,
                                             [groupFlag, asGroup](bool on) { groupFlag(asGroup->soloed, on); },
                                             "Solo the group"));
    } else {
        row->addWidget(widgets::toggleButton(frame, "M", "mute", track->muted,
                                             [setFlag](bool on) { setFlag(&Track::muted, on, on ? "Mute" : "Unmute"); },
                                             "Mute"));
    }
    if (track->kind != Kind::Return && !asGroup)  // (soloing other tracks leaves the returns playing)
        row->addWidget(frame->add<SoloButton>(track, [this, tl, track, setFlag](SoloButton::Change c) {
            if (c == SoloButton::Change::Exclusive) {
                project.soloOnly(track);
                rebuild();  // (the other rows' S buttons let go)
            } else if (c == SoloButton::Change::Solo) {
                setFlag(&Track::soloed, track->soloed, track->soloed ? "Solo" : "Unsolo");  // (every selected track)
            } else {
                setFlag(&Track::soloSafe, track->soloSafe, track->soloSafe ? "Solo safe" : "Not solo safe");
            }
            tl->update();  // silent clips are drawn grey
        }));
    if (track->kind != Kind::Return && !asGroup)  // ● (recording several tracks: ui/recording.h)
        row->addWidget(frame->add<ArmButton>(track, [this, alive, track](bool on, bool more) {
            if (!alive.expired()) armTrack(track, on, more);
        }));
    row->addWidget(remove);

    auto* meter = frame->add<LevelMeter>(channels);
    meter->showChannels(lane_panel::trackOutputs(*track));
    auto* display = frame->add<LoudnessDisplay>();
    auto toVolume = [](double db) { return (db - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB); };
    auto* slider = frame->add<VolumeSlider>(track->volumeDb, [this, alive, track, toVolume](double db) {
        if (!alive.expired())  // (the other selected tracks' by as much)
            for (auto& t : targets(track))
                if (t != track) {
                    t->volumeDb = std::clamp(t->volumeDb + db - track->volumeDb, (double)config::MIN_DB,
                                             (double)config::MAX_DB);
                    showMixer(t);
                }
        track->volumeDb = db;  // the mixer reads it every buffer
        automation::recorder().move(automation::armedControlLane(*track, automation::VOLUME), toVolume(db));
    });
    // (its Volume lane armed for writing, while playing: the fader held writes it: automation_write.h)
    slider->sliderPressed.connect([this, alive, track, slider, toVolume] {
        if (!alive.expired()) say("Volume", targets(track).size());
        automation::recorder().grab(automation::armedControlLane(*track, automation::VOLUME), toVolume(slider->value()));
    });
    slider->sliderReleased.connect(
        [track] { automation::recorder().release(automation::armedControlLane(*track, automation::VOLUME)); });
    slider->hPolicy = ui::SizePolicy::Expanding;
    // outputs | the meter, the volume slider and its dB under it | LUFS (its two lines over both rows:
    // one row is too low for them at the default track height)
    auto glayout = std::make_unique<ui::GridLayout>();
    ui::GridLayout* bottom = glayout.get();
    bottom->hSpacing = std::max(0, ui::style::px(style.controls, "column-gap", 6));
    bottom->vSpacing = std::max(0, ui::style::px(style.controls, "row-gap", 2));
    col->addLayout(std::move(glayout));
    col->addStretch(1);  // (a tall row: its controls stay together at the top)
    std::vector<ui::Widget*> selectors;  // (hidden first in a row too low for all: lane_panel::fitControls)
    ui::ComboBox* outputs = widgets::outputSelector(frame, channels, track->output, [this, alive, track, meter](int first) {
        track->output = first;
        meter->showChannels(lane_panel::trackOutputs(*track));
        if (!alive.expired()) showUsedOutputs();  // the master's and its group's meters show the outputs in use
    });
    // (as tall as Qt's flat combo box comes out in a default-height row: the row's controls fit in 72 px)
    outputs->setFixedHeight(theme::metric("output-selector-height", 18));
    bottom->addWidget(outputs, 0, 0, 1, 1, AlignTop);
    selectors.push_back(outputs);
    bool recordsAudio = track->kind == Kind::Audio && !asGroup;  // (its input and monitoring: ui/recording.h)
    ui::Widget* beforeMeter = nullptr;  // (an audio track's input; a MIDI track's keyboard focus)
    if (recordsAudio) {  // its input, then the meter
        auto* input = record_arm::inputSelector(frame, *track, inputs, [this, alive, track](const std::string& setting) {
            if (alive.expired()) return;
            auto ts = targets(track);
            for (auto& t : ts)
                if (t->kind == Kind::Audio) recording::setInput(*t, setting);
            util::debugf("recording: input %s for %zu track(s)\n", setting.c_str(), ts.size());
            if (ts.size() > 1) rebuild();
            say("Input " + recording::inputName(*track), ts.size());
        });
        input->setFixedHeight(theme::metric("output-selector-height", 18));
        selectors.push_back(input);
        beforeMeter = input;
    } else if (track->kind == Kind::Midi && !asGroup) {  // its keyboard focus (KEYBOARD FOCUS: lit as showKeyFocus says)
        auto* focus = frame->add<KeyFocusButton>(
            track, false, project.keyFocusLocked,
            [this, alive, track](bool on) { if (!alive.expired()) toggleFocus(track, on); },
            [this, alive](ui::Menu& m) { if (!alive.expired()) fillFocusMenu(m); });
        focus->setFixedHeight(theme::metric("output-selector-height", 18));  // (as an audio track's input there)
        focusButtons[track] = focus;
        beforeMeter = focus;
    }
    if (beforeMeter) {
        ui::Widget* meterCell;
        ui::HBox* meterRow = cellRow(frame, meterCell, std::max(0, ui::style::px(style.volume, "column-gap", 4)));
        meterCell->adopt(frame->release(beforeMeter));
        meterCell->adopt(frame->release(meter));
        meterRow->addWidget(beforeMeter, 0, AlignTop);
        meterRow->addWidget(meter, 1);
        bottom->addWidget(meterCell, 0, 1);
    } else {
        bottom->addWidget(meter, 0, 1);
    }
    bottom->addWidget(display, 0, 2, 2, 1, AlignVCenter | AlignRight);
    ui::Widget* volumeCell;
    ui::HBox* volume = cellRow(frame, volumeCell, std::max(0, ui::style::px(style.volume, "column-gap", 4)));
    auto* reading = frame->add<DbReading>(slider);
    volumeCell->adopt(frame->release(slider));
    volumeCell->adopt(frame->release(reading));
    volume->addWidget(slider, 1);
    volume->addWidget(reading);
    bottom->addWidget(volumeCell, 1, 1);
    std::vector<TrackPtr> returns = project.returns();
    auto* autoButton = frame->add<ui::Button>("Auto");
    autoButton->setStyle(ui::Button::Style::Tool);
    autoButton->setCheckable(true);
    autoButton->setCheckedSilently(track->showAutomation);
    bool alone = (track->kind == Kind::Return || returns.empty()) && !recordsAudio;  // (narrower beside others)
    autoButton->setFixedSize(alone ? theme::metric("auto-button-width", 58) : theme::metric("auto-button-short-width", 36),
                             theme::metric("auto-button-height", 16));
    autoButton->setFocusPolicy(ui::FocusPolicy::None);
    autoButton->setProperty("accent", "true");  // lit while its lanes show
    size_t count = track->automation.size();
    autoButton->setToolTip(count ? "Show or hide this track's automation (" + std::to_string(count) + " lane" +
                                       (count != 1 ? "s" : "") + ")"
                                 : "Automate this track's volume or pan, or a parameter of its instrument or effects");
    autoButton->toggled.connect([this, alive, track](bool on) {
        if (!alive.expired()) toggleAutomation(track, on);
    });
    ui::Widget* autoCell;  // Auto, and → (sends) when there are return tracks to send to
    ui::HBox* cell = cellRow(frame, autoCell, std::max(0, ui::style::px(style.autoCell, "column-gap", 2)));
    autoCell->adopt(frame->release(autoButton));
    cell->addWidget(autoButton);
    if (!returns.empty() && track->kind != Kind::Return) {
        Timeline* tl = &timeline;
        cell->addWidget(autoCell->add<SendsButton>(track, returns, [tl] { tl->update(); }));
    }
    if (recordsAudio)  // its monitoring (the speaker): every selected audio track's
        cell->addWidget(autoCell->add<MonitorButton>(track, [this, alive, track](Track::Monitor m) {
            if (alive.expired()) return;
            auto ts = targets(track);
            for (auto& t : ts)
                if (t->kind == Kind::Audio) t->monitor = m;
            if (ts.size() > 1) rebuild();
            say(std::string("Monitoring ") + record_arm::monitorName(m), ts.size());
        }));
    cell->addStretch(1);
    bottom->addWidget(autoCell, 1, 0);
    bottom->setColumnStretch(1, 1);
    // a row too low for all of it (MIN_TRACK_H, a skin's bigger controls): the LUFS readout goes first, then the
    // output and input selectors; the name, buttons, meter, volume and Auto stay
    frame->addOptional(selectors);
    frame->addOptional({display});
    addGrip(frame, timeline, track, nullptr);
    meters[track] = meter;
    loudness[track] = display;
    mixers[track] = Mixer{slider, reading, pan};  // (automation shows its values on them while playing)
    return frame;
}

LaneFrame* LanePanel::automationHeader(ui::Widget* parent, const Lane& lane) {
    AutomationPtr a = lane.automation;
    TrackPtr track = lane.owner;
    RowStyle style(this, {"automation", kindClass(track->kind)});
    Color accent = style.stripe().value_or(kindAccent(track->kind));
    auto* frame = parent->add<LaneFrame>(lane_panel::automationRowColor(*a, this, track->kind), accent, lane.height);
    frame->setRowStyle(automationRowStyle(*a, this, track->kind));  // (its box: TrackRow.automation's, :armed)
    // (a line above it, as in the timeline: stacked lanes stay apart)
    if (style.row.has("border-top-color")) frame->setTopLine(ui::style::color(style.row, "border-top-color"));
    frame->onPress = [this, track] { timeline.setCurrentTrack(track); };
    frame->fillMenu = [this, track](ui::Menu& menu) { fillAddMenu(menu, track); };
    ui::VBox* col = ui::vbox(frame, 0, style.rowGap(2));
    style.margins(col, 14, 4, 8, 4);
    auto* row = nest<ui::HBox>(col);
    row->setSpacing(style.columnGap(2));
    std::string slotName = a->slot ? a->slot->name : "";
    auto* name = frame->add<ui::Label>(a->param);
    name->setToolTip(slotName + ": " + a->param);
    name->setMinimumWidth(10);
    name->setElide(true);
    name->hPolicy = ui::SizePolicy::Expanding;
    row->addWidget(name, 1);
    // --- Write: moving the control while playing writes this lane (automation_write.h) ---
    if (!track->frozen || (a->slot && a->slot->isControls())) {  // (a frozen track's plugins are locked)
        Timeline* tl = &timeline;
        std::weak_ptr<bool> alive = alive_;
        Kind kind = track->kind;
        ui::Button* write = widgets::toggleButton(
            frame, "Write", "record", a->armed,
            [this, alive, tl, a, frame, accent, kind](bool on) {
                a->armed = on;
                if (!on) automation::recorder().release(a);
                const ui::Widget* panel = alive.expired() ? nullptr : this;
                frame->setColors(lane_panel::automationRowColor(*a, panel, kind), accent);
                frame->setRowStyle(automationRowStyle(*a, panel, kind));
                tl->update();  // (armed lanes are tinted red there too)
            },
            "Write: while the song plays, moving this parameter (" + a->param +
                ") writes its curve, from when you grab the control until you let go");
        write->setFixedWidth(theme::metric("write-button-width", 40));
        row->addWidget(write);
    }
    if (!track->frozen) {
        std::weak_ptr<bool> alive = alive_;
        row->addWidget(widgets::iconButton(frame, "+", [this, alive, track] {
            if (!alive.expired()) addAutomation(track);
        }, "Automate another parameter of this track"));
        Timeline* tl = &timeline;
        row->addWidget(widgets::iconButton(frame, "✕", [tl, track, a] { tl->removeAutomation(track, a); },
                                           "Remove this automation lane"));
    }
    bool missing = std::atomic_load(&a->targetPlugin) && !a->hasTarget;  // its plugin has no such parameter
    auto* detail = frame->add<ui::Label>(slotName + (missing ? "  (no such parameter now)" : ""));
    detail->setProperty("hint", "true");
    detail->setElide(true);
    col->addWidget(detail);
    col->addStretch(1);
    frame->addOptional({detail});  // (hidden in a lane too low for it)
    addGrip(frame, timeline, nullptr, a);
    return frame;
}

LaneFrame* LanePanel::takeHeader(ui::Widget* parent, const Lane& lane) {
    // the take's name and length, Use (all of it) and ✕ (delete it from the takes)
    ClipPtr take = lane.take;
    TrackPtr track = lane.owner;
    RowStyle style(this, {"take", kindClass(track->kind)});
    auto* frame = parent->add<LaneFrame>(style.background(theme::c("automation_lane")),
                                         style.stripe().value_or(kindAccent(track->kind)), lane.height);
    frame->setRowStyle(style.row);
    frame->onPress = [this, track] { timeline.setCurrentTrack(track); };
    frame->fillMenu = [this, track](ui::Menu& menu) { fillAddMenu(menu, track); };
    ui::HBox* row = ui::hbox(frame, 0, style.columnGap(4));
    style.margins(row, 14, 4, 8, 4);
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.1f", (double)take->frames() / timeline.engine.sr);
    auto* name = frame->add<ui::Label>(take->name + "  (" + seconds + " s)");
    name->setToolTip("A recorded take: drag across a part of it in its lane to put that part on the track");
    name->setMinimumWidth(10);
    name->setElide(true);
    name->hPolicy = ui::SizePolicy::Expanding;
    row->addWidget(name, 1);
    Timeline* tl = &timeline;
    auto* use = frame->add<ui::Button>("Use");
    use->setToolTip("Put all of this take on the track (or drag across a part of it)");
    use->setFocusPolicy(ui::FocusPolicy::None);
    use->clicked.connect([tl, track, take] { tl->useTake(track, take); });
    row->addWidget(use);
    row->addWidget(widgets::iconButton(frame, "✕", [tl, track, take] { tl->deleteTake(track, take); },
                                       "Delete this take (from the takes)"));
    return frame;
}

LaneFrame* LanePanel::groupHeader(ui::Widget* parent, const GroupPtr& group) {
    if (routing::busOn(*group)) {  // a bus: its fader, pan, meter, effects, sends and automation, as a track's
        LaneFrame* frame = trackHeader(parent, group->bus, false, group);
        groupFrames[group] = frame;
        return frame;
    }
    RowStyle style(this, {"group"});
    Color fill = style.background(theme::c("group_lane"));
    std::optional<Color> text;
    if (!group->color.empty()) {
        Color fg;
        colors::trackColors(group->color, group->tone, fill, fg);
        text = fg;
    }
    auto* frame = parent->add<LaneFrame>(fill, lane_panel::groupBarColor(*group, this), lanes::groupHeight(), text);
    frame->setRowStyle(style.row);  // (in the group's own colour, if it has one)
    frame->fillMenu = [this, group](ui::Menu& menu) { fillGroupMenu(menu, group); };
    ui::VBox* col = ui::vbox(frame, 0, style.rowGap(3));
    style.margins(col, 4, 4, 8, 4);
    auto* row = nest<ui::HBox>(col);
    row->setSpacing(style.columnGap(3));
    Timeline* tl = &timeline;
    row->addWidget(widgets::iconButton(frame, group->folded ? "▸" : "▾", [tl, group] { tl->toggleFold(group); },
                                       group->folded ? "Show its tracks" : "Fold its tracks away"));
    auto* name = frame->add<NameLabel>(group->name, true);
    name->doubleClicked.connect([this, group] { rename(group); });
    name->setToolTip("Double-click to rename. Right-click for its colour; double-click its lane to fold it");
    row->addWidget(name, 1);
    auto setFlag = [tl](bool& flag, bool on) {
        flag = on;
        tl->update();  // silent clips are drawn grey
    };
    row->addWidget(widgets::toggleButton(frame, "M", "mute", group->muted,
                                         [setFlag, group](bool on) { setFlag(group->muted, on); }, "Mute the group"));
    row->addWidget(widgets::toggleButton(frame, "S", "solo", group->soloed,
                                         [setFlag, group](bool on) { setFlag(group->soloed, on); }, "Solo the group"));
    row->addWidget(widgets::iconButton(frame, "✕", [tl, group] { tl->ungroup(group); }, "Ungroup (the tracks stay)"));
    auto* meter = frame->add<LevelMeter>(channels);
    auto* meterRow = nest<ui::HBox>(col, 1);
    meterRow->setContentsMargins(theme::metric("group-meter-indent", 26), 0, 0, 0);  // under the name, past the fold button
    meterRow->addWidget(meter);
    groupMeters[group] = meter;
    return frame;
}

// --- automation ----------------------------------------------------------------------------

void LanePanel::toggleAutomation(const TrackPtr& track, bool on) {
    if (on && track->automation.empty()) {
        if (!addAutomation(track)) rebuild();  // (Auto goes off again)
        return;
    }
    track->showAutomation = on;
    timeline.lanesChanged();
}

bool LanePanel::addAutomation(const TrackPtr& track) {
    if (track->frozen) {
        ui::msg::information(dialogParent(), "Track frozen", track->name + " is frozen: unfreeze it to automate it.");
        return false;
    }
    ParameterDialog dialog(dialogParent(), track);
    if (dialog.exec() != ui::Dialog::Accepted || !dialog.chosenSlot) return false;
    auto a = std::make_shared<Automation>();
    a->slot = dialog.chosenSlot;
    a->param = dialog.chosenParam;
    std::vector<AutomationPtr> lanes = track->automation.copy();
    lanes.push_back(a);
    track->automation = lanes;
    track->showAutomation = true;
    automation::resolve(project);
    modulation::resolve(project);  // (and the modulation cables' parameters)
    timeline.lanesChanged();
    return true;
}

void LanePanel::toggleTakes(const TrackPtr& track) {
    track->showTakes = !track->showTakes;
    timeline.lanesChanged();
}

void LanePanel::setDelay(const TrackPtr& track) {
    if (track_dialogs::askTrackDelay(dialogParent(), *track)) rebuild();
}

void LanePanel::setBendRange(const TrackPtr& track) {
    if (track_dialogs::askBendRange(dialogParent(), *track)) rebuild();
}

void LanePanel::showMixerValues(std::optional<int64_t> pos, int sr) {
    for (auto& [track, mixer] : mixers) {
        bool automated = false;
        for (auto& a : track->automation)
            if (a->slot == track->controls && !a->points.empty()) automated = true;
        if (!automated) continue;
        // (volume dB, pan) the track plays with at `pos`: its own, or its lanes' values (one being written: the hand's)
        double volumeDb = track->volumeDb, pan = track->pan;
        automation::mixerValues(*track, pos.has_value(), pos.value_or(0), sr, volumeDb, pan);
        int db = (int)std::nearbyint(volumeDb);
        if (!mixer.slider->isSliderDown()) {  // (never under the hand moving it)
            mixer.slider->setValueSilently(db);  // (shown only: the track's own volume stays as it is)
            mixer.reading->showValue(db);
        }
        mixer.pan->showValue(pan);
    }
}

void LanePanel::showCurrentTrack() {
    // (the selection: the current track one of them, and none that is gone)
    TrackPtr current = timeline.currentTrack;
    if (current && std::find(selected.begin(), selected.end(), current) == selected.end()) {
        selected = {current};
        anchor_ = current;  // (Shift-click selects from it now)
    }
    std::vector<TrackPtr> kept;
    for (auto& t : project.tracks)
        if (current && std::find(selected.begin(), selected.end(), t) != selected.end()) kept.push_back(t);
    selected = kept;
    for (auto& [track, frame] : frames)
        frame->setCurrent(std::find(selected.begin(), selected.end(), track) != selected.end());
    showKeyFocus();
}

// --- ADDING TRACKS and KEYBOARD FOCUS (lane_panel.h) ----------------------------------------------------------------

TrackPtr LanePanel::addTrack(Kind kind, TrackPtr after, GroupPtr group) {
    if (after && after->busOf) {  // (a group bus's lane: below the group's last track, in it)
        for (auto& g : project.groups())
            if (g.get() == after->busOf) group = g;
        std::vector<TrackPtr> members = group ? project.members(group) : std::vector<TrackPtr>{};
        after = members.empty() ? nullptr : members.back();
    }
    if (kind == Kind::Return || (after && after->kind == Kind::Return) || (after && !project.tracks.contains(after))) {
        after = nullptr;  // (a return goes at the very bottom; a track below a return: above the returns)
        if (kind == Kind::Return) group = nullptr;
    }
    if (!group && after) group = after->group;  // (in the group of the track it goes below)
    TrackPtr made = timeline.addTrack(kind, kind == Kind::Return ? nullptr : group, after);
    util::debugf("panel: added %s%s%s", made ? made->name.c_str() : "(none)", after ? " below " : "",
                 after ? after->name.c_str() : "");
    return made;
}

void LanePanel::fillAddMenu(ui::Menu& menu, const TrackPtr& after, const GroupPtr& group, bool asSubmenu) {
    std::weak_ptr<bool> alive = alive_;
    ui::Menu* into = &menu;
    if (asSubmenu) into = menu.addMenu("Add track");
    else menu.addSection("Add a track");
    // (the transport's actions' keys: Registry::showKeys shows them as rebound)
    struct Entry {
        Kind kind;
        const char* text;
        const char* keys;
        const char* tip;
    };
    static const Entry entries[] = {
        {Kind::Audio, "Audio track", "Ctrl+T", "A track for recordings and audio files"},
        {Kind::Midi, "MIDI track", "Ctrl+Shift+T", "A track for notes, played by an instrument (choose it on its row)"},
        {Kind::Return, "Return track", "Ctrl+Alt+T",
         "Effects (a reverb, a delay) the other tracks share, each sending it as much as you like (\xe2\x86\x92 on their "
         "rows); returns stay at the bottom"},
    };
    for (const Entry& e : entries) {
        Kind kind = e.kind;
        into->addAction(e.text, [this, alive, kind, after, group] {
            if (!alive.expired()) addTrack(kind, after, group);
        }, e.keys)->tooltip = e.tip;
    }
    if (commands_) commands_->showKeys(*into);
}

void LanePanel::fillFocusMenu(ui::Menu& menu, const TrackPtr& track) {
    std::weak_ptr<bool> alive = alive_;
    if (track && track->kind == Kind::Midi) {  // (its toggle as an entry: also where the row is too low to show it)
        auto ts = midi_input::keyboardTracks(project, timeline.currentTrack);
        bool on = std::find(ts.begin(), ts.end(), track) != ts.end();
        menu.addCheckable("Keyboards play this track", on, [this, alive, track](bool now) {
            if (!alive.expired()) toggleFocus(track, now);
        })->tooltip = "Its keyboard focus toggle: MIDI keyboards and the computer keyboard play every track it is lit on";
    }
    menu.addCheckable("Lock keyboard focus", project.keyFocusLocked, [this, alive](bool on) {
        if (!alive.expired()) setFocusLocked(on);
    })->tooltip = "Keep the tracks the keyboards play (the lit focus toggles) while you select other tracks";
}

void LanePanel::setFocusLocked(bool on) {
    if (on)  // (what plays now stays: the selected track too, when it plays for want of a lit toggle)
        for (auto& t : midi_input::keyboardTracks(project, timeline.currentTrack)) t->keyFocus = true;
    project.keyFocusLocked = on;
    util::debugf("panel: keyboard focus %s", on ? "locked" : "unlocked");
    if (status)
        status(on ? "Keyboard focus locked: selecting tracks leaves it; its toggles still change it"
                  : "Keyboard focus unlocked: it follows the selected track again",
               3000);
    showKeyFocus();
}

void LanePanel::toggleFocus(const TrackPtr& track, bool on) {
    midi_input::setFocus(track, on);
    std::vector<TrackPtr> plays = midi_input::keyboardTracks(project, timeline.currentTrack);
    if (status) {
        std::string names;
        for (auto& t : plays) names += (names.empty() ? "" : ", ") + t->name;
        status(plays.empty() ? std::string("The keyboards play no track (select a MIDI track with an instrument)")
                             : "The keyboards play " + names, 3000);
    }
    showKeyFocus();
}

void LanePanel::showKeyFocus() {
    std::vector<TrackPtr> plays = midi_input::keyboardTracks(project, timeline.currentTrack);
    for (auto& [track, button] : focusButtons)
        if (button && ui::Widget::alive(button))
            button->show(std::find(plays.begin(), plays.end(), track) != plays.end(), project.keyFocusLocked);
}

bool LanePanel::contextMenu(ui::MouseEvent& e) {
    ui::Menu menu;  // (empty space below the rows: at the end)
    fillAddMenu(menu);
    menu.exec(e.globalPos, this);
    return true;
}

void LanePanel::paint(Painter& p) {
    ui::Widget::paint(p);
    if (!project.tracks.empty()) return;
    // (no tracks yet: how to add one; ADDING TRACKS)
    Color c = ui::style::color(computedStyle(), "color", theme::c("disabled_text"));
    p.drawTextBlock(RectF(8, 12, (float)std::max(0, width() - 16), 60), AlignHCenter | AlignTop,
                    "Right-click to add a track", ui::style::font(computedStyle(), theme::uiFont()), c);
}

void LanePanel::showUsedOutputs() {
    for (auto& [group, meter] : groupMeters) {
        std::vector<int> outs;
        for (auto& t : routing::inside(project, group.get()))
            for (int c : lane_panel::trackOutputs(*t)) outs.push_back(c);
        meter->showChannels(outs);
    }
    std::vector<int> used;
    for (auto& t : project.tracks)
        for (int c : lane_panel::trackOutputs(*t)) used.push_back(c);
    masterStrip.meter->showChannels(used.empty() ? std::vector<int>{0, 1} : used);
}

// --- right-click menus ---------------------------------------------------------------------

void LanePanel::fillTrackMenu(ui::Menu& menu, const TrackPtr& track) {
    std::weak_ptr<bool> alive = alive_;
    auto guard = [alive](std::function<void()> fn) {
        return [alive, fn] {
            if (!alive.expired()) fn();
        };
    };
    menu.addAction("Rename track…", guard([this, track] { rename(track); }));
    menu.addAction("Track colour…", guard([this, track] { chooseColor(track); }));
    {  // (every selected track: TRACK SELECTION)
        size_t n = targets(track).size();
        std::string many = n > 1 ? " (" + std::to_string(n) + " tracks)" : "";
        menu.addAction("Duplicate track" + many, guard([this, track] { duplicate(track, true); }), "Ctrl+D")->tooltip =
            "A copy right below it, with its clips, plugins and their settings, cables, automation and sends";
        menu.addAction("Duplicate without clips" + many, guard([this, track] { duplicate(track, false); }))->tooltip =
            "A copy right below it with its plugins, cables, automation and sends, but no clips";
        menu.addAction("Move up" + many, guard([this, track] { move(track, -1); }), "Alt+Up");
        menu.addAction("Move down" + many, guard([this, track] { move(track, 1); }), "Alt+Down");
    }
    fillAddMenu(menu, track, nullptr, true);  // (ADDING TRACKS: below this one)
    auto edit = instruments.edit;
    if (track->kind == Kind::Midi && track->usesRack && !track->needsInstrument) {
        menu.addAction("Open its instrument patch panel", [edit, track] { if (edit) edit(track); });
    } else if (track->kind == Kind::Midi && track->instrument && !track->needsInstrument) {
        auto instance = aload(track->instrument->instance);
        bool reload = instance && track->instrument->failed.load() && plug::isBridged(*instance);  // (edit() asks)
        menu.addAction((reload ? "Reload the " : "Open the ") + track->instrument->name +
                           (reload ? " (it stopped)…" : " window"),
                       [edit, track] { if (edit) edit(track); });
    }
    if (!track->frozen) menu.addAction("Automate a parameter…", guard([this, track] { addAutomation(track); }));
    menu.addAction("Track delay…" + (track->delayMs ? " (" + signedMs(track->delayMs) + " ms)" : std::string()),
                   guard([this, track] { setDelay(track); }));
    if (track->kind == Kind::Midi && !track->frozen && openMidiEffects) {
        size_t count = track->midiEffects.size();
        auto openFx = openMidiEffects;
        menu.addAction("MIDI effects…" + (count ? " (" + std::to_string(count) + ")" : std::string()),
                       [openFx, track] { openFx(track); });
    }
    if (track->kind == Kind::Midi && !track->frozen) {  // its MIDI input, its External synth (ui/midi_ports_dialog.h)
        Timeline* tl = &timeline;
        midi_ports_dialog::addTrackMenu(menu, track, [tl] { tl->update(); });
        if (track->ext.on)
            menu.addAction("External synth: " + track->ext.port + "\xe2\x80\xa6",
                           guard([this, track] { midi_ports_dialog::editExternal(dialogParent(), track); }));
    }
    if (track->kind == Kind::Midi)  // (the bend wheel of a MIDI keyboard: midi_input.h)
        menu.addAction("Pitch bend range…" + (track->bendRange != live::DEFAULT_BEND_RANGE
                                                 ? " (±" + std::to_string(track->bendRange) + ")"
                                                 : std::string()),
                       guard([this, track] { setBendRange(track); }));
    if (track->kind == Kind::Midi) fillFocusMenu(menu, track);  // (KEYBOARD FOCUS: its toggle, Lock keyboard focus)
    if (track->kind == Kind::Midi && !track->frozen) {  // Swing: the song's / its own (ui/song_scale.h)
        Timeline* tl = &timeline;
        song_scale::addTrackSwingMenu(menu, project, timeline.engine.sr, track, [tl] { tl->update(); });
    }
    if (!track->automation.empty()) {
        menu.addAction(track->showAutomation ? "Hide automation" : "Show automation",
                       guard([this, track] { toggleAutomation(track, !track->showAutomation); }));
        // --- writing automation (automation_write.h) ---
        bool armed = automation::anyArmed(*track);
        ui::Action* arm = menu.addAction(
            armed ? "Disarm its automation lanes (stop writing)" : "Arm all its automation lanes for writing",
            guard([this, track, armed] {
                automation::armAll(*track, !armed);
                if (armed)
                    for (auto& lane : track->automation) automation::recorder().release(lane);
                if (!armed) track->showAutomation = true;  // (shown: their Write switches lit)
                timeline.lanesChanged();  // (the rows: their Write switches and tint)
            }));
        arm->tooltip = armed ? "Their Write switches off: playing the song moves the controls by their curves again"
                             : "Every lane's Write switch on: while the song plays, moving a control writes its lane";
    }
    if (!track->takes.empty()) {
        menu.addAction(track->showTakes ? "Hide takes" : "Show takes (" + std::to_string(track->takes.size()) + ")",
                       guard([this, track] { toggleTakes(track); }));
    }
    if (fillPresetMenu) fillPresetMenu(menu, track);  // (track presets, MIDI controller links: presets_ui.h)
    if (track->kind == Kind::Return) return;  // (no groups, freezing or bouncing for returns)
    menu.addSeparator();
    menu.addAction("Group tracks…", guard([this, track] { groupWith(track); }));
    std::vector<GroupPtr> others;
    for (auto& g : project.groups())
        if (g != track->group) others.push_back(g);
    Timeline* tl = &timeline;
    if (!others.empty()) {
        ui::Menu* join = menu.addMenu("Add to group");
        for (auto& group : others) join->addAction(group->name, [tl, track, group] { tl->setGroup(track, group); });
    }
    if (track->group)  // (nested: into the group around it)
        menu.addAction("Remove from " + track->group->name,
                       [tl, track] { tl->setGroup(track, std::atomic_load(&track->group->parent)); });
    std::string input = pad_tracks::inputName(project, *track);  // (a Pad Kit's pad on its own track: model/pad_tracks.h)
    if (!input.empty()) {
        ui::Menu* from = menu.addMenu("Input: " + input);
        from->addAction("Back to the kit", [track] { pad_kit_dialog::backToKit(track); })->tooltip =
            "The pad plays in its kit's own sound again (this track stays, without it)";
    }
    fillOutputMenu(*menu.addMenu("Output: " + routing::targetName(*track)), track);
    if (fillRenderMenu) fillRenderMenu(menu, track);
    if (commands_) commands_->showKeys(menu);  // (the keys as rebound)
}

void LanePanel::registerCommands(commands::Registry& r) {
    shortcuts_.clear();  // (Alt+Up / Alt+Down: the registry's from now on)
    for (int dir : {-1, 1}) {
        commands::KeyCommand c;
        c.id = dir < 0 ? "Tracks/Move the track up" : "Tracks/Move the track down";
        c.name = dir < 0 ? "Move the track up" : "Move the track down";
        c.defaults = {ui::KeySequence::parse(dir < 0 ? "Alt+Up" : "Alt+Down")};
        c.run = [this, dir] {
            if (timeline.currentTrack) move(timeline.currentTrack, dir);
        };
        c.owner = this;
        r.add(std::move(c));
    }
    commands_ = &r;
}

void LanePanel::fillOutputMenu(ui::Menu& menu, const TrackPtr& track) {
    // OUTPUT ROUTING (model/routing.h): its default (its group's bus, or the master), a group's bus or another track;
    // what would make a loop is greyed out
    std::weak_ptr<bool> alive = alive_;
    TrackPtr now = std::atomic_load(&track->into);
    auto route = [this, alive, track](TrackPtr to) {
        if (alive.expired()) return;
        bool ok = routing::setInto(track, to);
        if (status)
            status(ok ? (track->busOf ? track->busOf->name : track->name) + " plays into " + routing::targetName(*track)
                      : std::string("That would make a loop: its sound would come back to it"),
                   3000);
        rebuild();
        timeline.update();
    };
    std::string byDefault = "Master";
    {
        TrackPtr was = now;
        std::atomic_store(&track->into, TrackPtr());  // (what it plays into by default: asked of the model)
        byDefault = routing::targetName(*track);
        std::atomic_store(&track->into, was);
    }
    ui::Action* def = menu.addCheckable(byDefault == "Master" ? "Master" : byDefault + " (its group)", !now,
                                        [route](bool) { route(nullptr); });
    def->tooltip = "Where it plays by default: its group's bus, or the master";
    std::vector<GroupPtr> buses;
    for (auto& g : project.groups())
        if (routing::busOn(*g) && g->bus != track) buses.push_back(g);
    if (!buses.empty()) menu.addSection("Group buses");
    for (auto& g : buses) {
        TrackPtr bus = g->bus;
        ui::Action* a = menu.addCheckable(g->name, now == bus, [route, bus](bool) { route(bus); });
        a->setEnabled(routing::canRoute(*track, bus.get()));
        if (!a->enabled) a->tooltip = "Its sound would come back to it";
    }
    std::vector<TrackPtr> others;
    for (auto& t : project.tracks)
        if (t != track && t->kind != Kind::Return && track->kind != Kind::Return) others.push_back(t);
    if (!others.empty()) menu.addSection("Tracks (into their input, before their effects)");
    for (auto& t : others) {
        ui::Action* a = menu.addCheckable(t->name, now == t, [route, t](bool) { route(t); });
        a->setEnabled(routing::canRoute(*track, t.get()));
        if (!a->enabled) a->tooltip = "Its sound would come back to it";
    }
}

void LanePanel::fillGroupMenu(ui::Menu& menu, const GroupPtr& group) {
    std::weak_ptr<bool> alive = alive_;
    Timeline* tl = &timeline;
    menu.addAction("Rename group…", [this, alive, group] { if (!alive.expired()) rename(group); });
    menu.addAction("Group colour…", [this, alive, group] { if (!alive.expired()) chooseColor(group); });
    menu.addAction(group->folded ? "Show its tracks" : "Fold its tracks away", [tl, group] { tl->toggleFold(group); });
    std::vector<TrackPtr> members = project.members(group);  // (ADDING TRACKS: in it, below its last track)
    fillAddMenu(menu, members.empty() ? nullptr : members.back(), group, true);
    // GROUP BUSES (model.h): its bus off ("Fold only") or on, where the bus plays, its effects and automation
    ui::Action* only = menu.addCheckable("Fold only (no bus)", group->foldOnly, [this, alive, group](bool on) {
        if (alive.expired()) return;
        group->foldOnly = on;
        routing::fixLoops(project);
        util::debugf("group %s: %s", group->name.c_str(), on ? "fold only (no bus)" : "a bus");
        if (status) status(group->name + (on ? ": fold only, its tracks play on without a bus" : ": a bus again"), 3000);
        rebuild();
        timeline.lanesChanged();
    });
    only->tooltip = "Fold, mute and solo its tracks only: no fader, effects or sends of its own";
    if (routing::busOn(*group)) {
        TrackPtr bus = group->bus;
        auto open = openPlugins;
        menu.addAction("Effects…", [open, bus] { if (open) open(bus); });
        menu.addAction("Automate a parameter…", [this, alive, bus] { if (!alive.expired()) addAutomation(bus); });
        if (!bus->automation.empty())
            menu.addAction(bus->showAutomation ? "Hide automation" : "Show automation",
                           [this, alive, bus] { if (!alive.expired()) toggleAutomation(bus, !bus->showAutomation); });
        menu.addAction("Bus delay…" + (bus->delayMs ? " (" + signedMs(bus->delayMs) + " ms)" : std::string()),
                       [this, alive, bus] { if (!alive.expired()) setDelay(bus); });
        fillOutputMenu(*menu.addMenu("Output: " + routing::targetName(*bus)), bus);
    }
    // nesting: inside another group (not itself or one inside it), or out of the one it is in
    ui::Menu* nest = menu.addMenu("Put in group");
    GroupPtr parent = std::atomic_load(&group->parent);
    for (auto& g : project.groups()) {
        if (g == group || g == parent) continue;
        ui::Action* a = nest->addAction(g->name, [this, alive, group, g] {
            if (alive.expired()) return;
            routing::setParent(project, group, g);
            timeline.tracksRegrouped();
        });
        a->setEnabled(routing::canNest(group, g));
        if (!a->enabled) a->tooltip = "It is inside " + group->name;
    }
    if (nest->actions().empty()) nest->addAction("(no other group)")->setEnabled(false);
    if (parent)
        menu.addAction("Take out of " + parent->name, [this, alive, group, parent] {
            if (alive.expired()) return;
            routing::setParent(project, group, std::atomic_load(&parent->parent));
            timeline.tracksRegrouped();
        });
    menu.addSeparator();
    menu.addAction("Ungroup (the tracks stay)", [tl, group] { tl->ungroup(group); });
}

void LanePanel::chooseColor(const TrackPtr& track) {
    std::string initial = !track->color.empty() ? track->color : colors::defaultColor(track->kind);
    ColorPickerDialog dialog(dialogParent(), initial, track->tone, ColorSubject::Track);
    if (dialog.exec() == ui::Dialog::Accepted) {
        auto ts = targets(track);
        for (auto& t : ts) {
            t->color = dialog.chosen;  // "": back to the standard look
            t->tone = dialog.tone;
        }
        say("Colour", ts.size());
        rebuild();
        timeline.update();
    }
}

void LanePanel::chooseColor(const GroupPtr& group) {
    std::string initial = !group->color.empty() ? group->color : lane_panel::groupBarColor(*group, this).hexRgb();
    ColorPickerDialog dialog(dialogParent(), initial, group->tone, ColorSubject::Group);
    if (dialog.exec() == ui::Dialog::Accepted) {
        group->color = dialog.chosen;
        group->tone = dialog.tone;
        rebuild();
        timeline.update();  // a group's colour shows on its header lane too
    }
}

void LanePanel::groupWith(const TrackPtr& track) {
    std::vector<TrackPtr> tracks;
    for (auto& t : project.tracks)
        if (t->kind != Kind::Return) tracks.push_back(t);
    auto chosen = lane_panel::chooseTracks(dialogParent(), tracks, {track});
    if (chosen && !chosen->empty()) timeline.groupTracks(*chosen);
}

namespace {
bool askName(ui::Window* parent, const std::string& what, std::string& name) {
    std::string text = name;
    if (!ui::getText(parent, "Rename " + what, "Name:", text)) return false;
    // (Python's str.strip())
    size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    name = text.substr(a, b - a + 1);
    return true;
}
}  // namespace

void LanePanel::rename(const TrackPtr& track) {
    if (askName(dialogParent(), "track", track->name)) rebuild();
}

void LanePanel::rename(const GroupPtr& group) {
    if (askName(dialogParent(), "group", group->name)) {
        group->bus->name = group->name;  // (its bus goes by its name: the effects window, sidechains)
        rebuild();
    }
}

void LanePanel::setMetersVisible(bool levels, bool loud) {
    std::vector<LaneFrame*> rows;  // (what a row too low hid is shown first, then hidden again if still too low)
    for (auto& [t, f] : frames) rows.push_back(f);
    for (auto& [g, f] : groupFrames) rows.push_back(f);
    for (LaneFrame* f : rows) f->restoreControls();
    showLevels = levels;
    showLoudness = loud;
    for (auto& [t, m] : meters) m->setVisible(levels);
    for (auto& [g, m] : groupMeters) m->setVisible(levels);
    masterStrip.meter->setVisible(levels);
    for (auto& [t, d] : loudness) d->setVisible(loud);
    masterStrip.loudness->setVisible(loud);
    for (LaneFrame* f : rows) f->fitControls();
}

void LanePanel::updateHeights() {
    auto fit = [](LaneFrame* frame, int h) {  // (only those that changed: each change places the rows again)
        if (frame->minimumSize().y != h || frame->maximumSize().y != h) frame->setFixedHeight(h);
    };
    for (auto& [track, frame] : frames) fit(frame, track->height);
    for (auto& [a, frame] : automationFrames) fit(frame, a->height);
    for (auto& [g, frame] : groupFrames) fit(frame, lanes::headerHeight(*g));
    fitHeight();
}

void LanePanel::fitHeight() {
    // All lanes + master, plus room matching the timeline's horizontal scrollbar, so both scroll
    // areas have the same range (the timeline's ruler is matched by the CPU monitor).
    // (and the timeline frame's bottom: ScrollArea#timeline's border and padding, nothing in the default look)
    auto* view = dynamic_cast<const TimelineView*>(timeline.scroll);
    int frame = view ? std::max(0, view->insetBottom()) : 0;
    int h = lanes::layout(project).back().bottom() + theme::scrollbarWidth() + frame;
    setFixedHeight(h);
    if (height() != h) resize(width() > 0 ? width() : lane_panel::panelWidth(), h);
}
