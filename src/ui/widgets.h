// Small building blocks shared by the panel widgets (widgets.py).
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/paint_helpers.h"
#include "ui/panel_style.h"
#include "ui/toolkit/menu.h"

// A frame exactly one lane tall, coloured like the timeline lane next to it (or the track's colour)
// (lane_frame / lane_style, the stylesheet "#lane"). `accent` draws a 3 px stripe down the left
// edge (the track kind's colour), else a 1 px frame_edge line; `text` colours every Label in it.
// A selected track's row (setCurrent) has a 1 px accent edge at its top, right and bottom; an
// automation row a line above it (topLine). Its layout fills the inside of those edges.
// Styled as LaneFrame (#lane; the master's row .master: its background-color is the master lane's colour, its
// padding and row-gap its controls'): ::stripe (width: the accent stripe, in its data colour), ::edge (the line
// down its left without one: background-color, width), ::current (the selected row's edges: border-color,
// border-width per side), ::load (the CPU bar: background-color, height, margin-bottom) and ::load-bar :ok /
// :warn / :hot. The row's own colour is data (the lane's or the track's).
// It is drawn as a box (style::box) in that colour: a panel row as its TrackRow style gives it (setRowStyle: the
// border and its styles, radii, gradients over the colour, inset and outer shadows), the master's row as its own
// (LaneFrame.master); its controls go inside that box's border (and the stripe, edge and current-row outline).
class LaneFrame : public ui::Widget {
public:
    LaneFrame(Color color, std::optional<Color> accent = std::nullopt, int height = config::TRACK_H,
              std::optional<Color> text = std::nullopt);
    // new colours (a skin change; lane_style): repainted, labels recoloured
    void setColors(Color color, std::optional<Color> accent = std::nullopt, std::optional<Color> text = std::nullopt);
    void setCurrent(bool on);              // the property "current"
    bool isCurrent() const { return current_; }
    void setTopLine(std::optional<Color> c);
    // the box it is drawn as (its TrackRow style: lane_panel), with its own colour as the background; without one,
    // its own style's (the master's row)
    void setRowStyle(const css::Style& style);
    // Options → CPU per track: a thin bar along its bottom: the track's share of a core (full width: half a core;
    // < 0: none)
    void setLoad(double share);
    void applyTextColor();                 // give the labels inside it the text colour (after adding some)
    Color color() const { return color_; }
    std::optional<Color> accent() const { return accent_; }
    std::optional<Color> textColor() const { return text_; }
    Rect contentsRect() const override;
    Rect paintRect() const override;       // (and where its box's outer shadows reach)
    void paint(Painter& p) override;
    void paintOver(Painter& p) override;
    // right-click menu (with_menu): filled by fillMenu each time
    std::function<void(ui::Menu&)> fillMenu;
    std::function<void()> onPress;         // any mouse button pressed on the row itself (selects the track)
    bool mousePress(ui::MouseEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    // controls hidden while the row is too low for them all (a track dragged to its smallest): the group added
    // last is hidden first; shown again when the row is tall enough
    void addOptional(std::vector<ui::Widget*> group);
    void fitControls();
    void restoreControls();  // those it hid shown again (before others are shown or hidden: then fitControls)
    size_t controlsHidden() const { return hidden_.size(); }
    void resized() override { fitControls(); }

private:
    std::vector<std::vector<ui::Widget*>> optional_;
    std::vector<ui::Widget*> hidden_;  // (by fitControls)
    Color color_;
    std::optional<Color> accent_, text_, topLine_;
    bool current_ = false;
    double load_ = -1;
    // what it is drawn with (kept: every row repaints its CPU bar)
    struct Look {
        int stripeW = 3, edgeW = 1, loadH = 2, loadBelow = 1;
        ui::style::Insets current{0, 1, 1, 1};
        Color edge, currentEdge, load[3];
        const css::Style* loadTrack = nullptr;
        ui::style::Insets loadInset;  // (its border and padding: the bar inside them)
        const css::Style* loadBar[3] = {nullptr, nullptr, nullptr};  // (more than a colour: drawn as boxes)
    };
    const Look& look() const;
    mutable panel_style::Parts parts_{*this};
    mutable Look look_;
    // the box it is drawn as: its row style (or its own) with its colour as the background, worked out again when
    // either changes, or the stylesheets (and the master's own states: boxStamp_)
    std::optional<css::Style> rowStyle_;
    const css::Style& box() const;
    mutable css::Style box_;
    mutable bool boxStale_ = true;
    mutable int boxGeneration_ = -1;
    mutable paint::Stamp boxStamp_;
};

// Keeps `widget` across the bottom of `viewport` (a scroll area's), `margin` px above its bottom
// edge and on top of the scrolled contents, whatever the scroll position or size. The margin
// below it is covered too (in the window colour), or the scrolled contents would show through.
class PinToBottom {
public:
    // the widget is moved into the viewport (released from its parent, which must exist)
    PinToBottom(ui::Widget* widget, ui::Widget* viewport, int height, int margin = 0);
    // the same for a widget not in the tree yet
    PinToBottom(std::unique_ptr<ui::Widget> widget, ui::Widget* viewport, int height, int margin = 0);
    ~PinToBottom();
    PinToBottom(const PinToBottom&) = delete;
    PinToBottom& operator=(const PinToBottom&) = delete;
    void place();
    void setHeight(int height);  // (the master's row: taller while the master scope is shown)
    void setMargin(int margin);  // (room kept free on the right: a scroll bar's width, after a skin change)
    ui::Widget* widget() const { return widget_; }
    ui::Widget* cover() const { return cover_; }

private:
    void init(int height, int margin);
    ui::Widget* widget_;
    ui::Widget* viewport_;
    ui::Widget* cover_ = nullptr;
    int height_ = 0, margin_ = 0, frame_ = -1;
    std::function<bool(ui::Widget*, const char*)> previousFilter_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// widgets.py's PinBelowView keeps a widget under a scroll area's view (its viewport made that much
// shorter) instead of over it, so Qt can move what is drawn when scrolling. The toolkit's scroll
// area repaints its view when the contents move either way, so PinToBottom serves for both here;
// a timeline whose master lane is pinned over its view counts that lane's place in its height
// (Timeline::masterBelowView = false).

// A track's or group's name; double-click it to rename (its doubleClicked signal). bold: a group's (NameLabel.group:
// its font).
class NameLabel : public ui::Label {
public:
    explicit NameLabel(const std::string& text = "", bool bold = false);
};

// A track's volume in dB (MIN_DB = silent … MAX_DB); double-click for 0 dB. Ignores the mouse
// wheel, so scrolling the panel never changes a volume by accident.
class VolumeSlider : public ui::Slider {
public:
    VolumeSlider(double db, std::function<void(double)> slot);
    void showTip();
};

// A volume slider's setting in dB, shown next to it; double-click it for 0 dB.
class DbReading : public ui::Label {
public:
    explicit DbReading(ui::Slider* slider);
    void showValue(int db);
    ui::Slider* slider;
};

// A track's pan (-1 left … 1 right) as a small knob: drag up or right to turn it right, down or left
// to turn it left (Shift: finer); double-click to centre it. Ignores the mouse wheel, as the volume
// slider does. slot(pan) when the user turns it (rounded to 0.01). Styled as PanKnob: background-color (the
// knob's), padding (around it); ::arc (the turn from the centre) and ::pointer: color, stroke-width.
class PanKnob : public ui::Widget {
public:
    static constexpr int PIXELS = 100;  // of dragging, from the centre to one side
    PanKnob(double pan, std::function<void(double)> slot);
    double pan;
    void showValue(double pan);         // show `pan` without changing the track (automation moving it)
    void setPan(double pan);            // as the user turning it: clamped, rounded, slot called
    void showTip();
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override { return false; }
    std::function<void(bool)> grabbed;  // pressed (true) and let go (false): writing automation (automation_write.h)

private:
    std::function<void(double)> slot_;
    bool dragging_ = false;
    Point start_;
    double startPan_ = 0;
};

// The sliders of SendsButton's small window: "<track> sends to", then a row per return track
// (its name, a volume slider, its dB, and Pre: the send taken before the track's fader and pan, after its
// effects; model.h Send). Paintable without a window (tests). Styled as SendsPanel: its box (background-color,
// border), padding, row-gap and column-gap; ::title (its font).
class SendsPanel : public ui::Widget {
public:
    SendsPanel(TrackPtr track, std::vector<TrackPtr> returns, std::function<void(const TrackPtr&, double)> setLevel,
               std::function<void(const TrackPtr&, bool)> setPreFader = nullptr);
    std::vector<VolumeSlider*> sliders;  // one per return, in order
    std::vector<ui::Button*> preButtons; // …and its Pre toggle
    void paint(Painter& p) override;
};

// → on a track's row: how much of its sound (after its fader, or before it: Pre) goes to each return
// track, as sliders in a small window that opens under it (−inf: none); lit while any send is up.
class SendsButton : public ui::Button {
public:
    SendsButton(TrackPtr track, std::vector<TrackPtr> returns, std::function<void()> changed);
    ~SendsButton() override;
    double level(const TrackPtr& target) const;   // dB sent to it (MIN_DB: none)
    void setLevel(const TrackPtr& target, double db);  // (the mixer reads the track's sends every buffer)
    bool preFader(const TrackPtr& target) const;  // is the send to it taken before the fader?
    void setPreFader(const TrackPtr& target, bool on);
    void showState();                             // lit, and the tooltip listing the sends that are up
    void showSends();                             // opens the window under it
    TrackPtr track;
    std::vector<TrackPtr> returns;
    std::function<void()> changed;

private:
    std::unique_ptr<ui::Window> popup_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    void closePopup();
};

namespace widgets {
// Small checkable button (M / S) that lights up in the skin's "mute" or "solo" colour (`role`) when on.
ui::Button* toggleButton(ui::Widget* parent, const std::string& text, const std::string& role, bool checked,
                         std::function<void(bool)> slot, const std::string& tooltip = "");
// [(first output index, label)]: (0, "1/2"), (2, "3/4"), ...; an odd last output stands alone
std::vector<std::pair<int, std::string>> outputPairs(int channels);
// Combo box of the device's output pairs (ignores the wheel); calls slot(first output index) on change.
ui::ComboBox* outputSelector(ui::Widget* parent, int channels, int current, std::function<void(int)> slot);
// Tooltip text listing a track's (or the master's) effects.
std::string effectsSummary(const std::vector<SlotPtr>& effects);
// 'FX' button opening the effects window; lit while there are effects.
ui::Button* fxButton(ui::Widget* parent, const std::vector<SlotPtr>& effects, std::function<void()> slot);
// A MIDI track's instrument button (🎹): clicking it calls edit() (the instrument's window, or the
// instrument patch panel when it plays one: rack = how many instruments it holds, -1 = none), or,
// with the built-in synth (no settings), shows the menu; its ▾ shows the menu, which fillMenu(menu)
// fills each time it opens (the instruments to choose from). The menu is owned by the button.
// sampler: the track's built-in sampler when it plays one (no instrument plugin): click opens its window;
// padKit: its Pad Kit likewise (it wins over the sampler, as Track::playsPadKit says); drawn: it plays the Drawn
// synth (its built-in synth with a drawn wave): click opens its canvas
ui::Button* instrumentButton(ui::Widget* parent, const SlotPtr& instrument, std::function<void()> edit,
                             std::function<void(ui::Menu&)> fillMenu, int rack = -1, SamplerPtr sampler = nullptr,
                             PadKitPtr padKit = nullptr, bool drawn = false);
ui::Button* iconButton(ui::Widget* parent, const std::string& text, std::function<void()> slot,
                       const std::string& tooltip = "");
// a menu owned by a widget (deleted with it): for buttons' setMenu
ui::Menu* ownedMenu(ui::Widget* owner);
}  // namespace widgets
