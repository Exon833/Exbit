// The effects window's patch space: the effects as units on a patch panel, wired by hand
// (patch_panel.py).
//
// Between rails with screws, the sound comes in at the IN jack on the left and leaves at the OUT
// jack on the right edge. Each effect is a unit placed anywhere in the space: a picture of the
// plugin's own window (plugin_pictures.h; an empty faceplate until there is one), its name, its
// input jack (left), its sidechain jack ("SC", next to the input) if the plugin has a sidechain
// input, its output jack (right), and a light: green = on and wired, amber = placed but not wired
// in (its sound isn't heard), dark = bypassed (click the light), red = not loaded or stopped after
// an error (a stopped Windows plugin has a Reload button on its picture). Patch cables run from outputs (IN, a unit's right jack) to inputs (a unit's input or
// sidechain jack, OUT); an output can feed several inputs and an input adds up all its cables
// (audio/patch.h). What goes into a sidechain only steers the effect (a compressor or limiter
// listening to it): it isn't heard itself. Along the bottom, above the rail, every other track has
// an output jack too ("OTHER TRACKS"): its sound after its effects and fader, for sidechains only
// (e.g. the kick track into the bass compressor's SC). A track that already listens to this one,
// directly or through others, can't be plugged in (a loop): its jack is dimmed. A cable that would
// close a loop can't be plugged in.
//
// Mouse: drag from a jack to another to plug a cable in (outputs to inputs, either way round); drag
// a cable's plug off an input to move it to another input, or drop it anywhere else to unplug it;
// right-click a cable to remove it. Drag a unit to move it; the space grows as units go further
// right or down (drag one past the edge), and the window can be made bigger both ways. Click a
// unit to select it, double-click it to open its window, right-click it for its menu; double-click
// an empty spot to place an effect there, right-click it for the space's menu.
// Keys: Left / Right select, Enter opens, Delete removes. Space, Home and the transport keys still
// reach the transport (they are not used here).
// Colours come from the skin when painting; the cables have their own colours, like real ones.
//
// Styled as PatchPanel (default.css): its background-color, and --cable-1 … --cable-6 (the cables' colours, in
// turn). Parts: ::rail (background-color) and ::rail-edge (background-color, height: the line under / over a
// rail); ::screw (background-color, color: the slot, width: across, stroke-width: the slot's); ::sources (the
// other tracks' row: background-color, border-top-color / border-top-width: the line above it), ::sources-title
// and ::source (color, font) with ::source:blocked (opacity); ::jack (background-color: the hole, border-color /
// border-width: the ring around it), ::jack-ring and ::jack-ring:hover (where a cable can go: color,
// stroke-width); ::end-title and ::end-label (IN / OUT: color, font); ::empty-hint (color, font); ::unit,
// ::unit:hover (boxes) and ::unit:selected (outline-color, outline-width: drawn over all of it); ::picture (background-color) and ::picture:bypassed (opacity); ::faceplate (no picture: color,
// stroke-width) and ::faceplate-text (color, font); ::wave (a modulator's mark: color, stroke-width); ::reload
// and ::reload:hover (background-color, color, font, border-top-color, border-top-width, border-radius),
// ::reload-shade (background-color, over the picture); ::name and ::name:bypassed (color, font);
// ::sidechain-label (color, font); ::led (border-top-color, border-top-width) :on / :idle / :bypassed /
// :problem (background-color); ::status and ::status:hover (color, font); the fader: ::groove (background-color,
// border-radius), ::level and ::level:bypassed (background-color), ::zero-mark (color, stroke-width), ::knob and
// ::knob:hover (background-color, border-top-color, border-top-width, border-radius), ::value and ::value:hover
// (color, font); ::mod-link (color, stroke-width, font) and ::mod-label (a box behind its label); ::cable
// (stroke-width, outline-width: with its darker edge). Fonts are read in their plain state; the geometry
// (patch_panel:: below: where units, jacks and lights are) is the panel's own, as saved unit places use it.
// Boxes (drawn as CSS draws a box: ui::style::box) are the panel itself, ::rail, ::sources (its top border is
// the line above it), ::unit / ::unit:hover, ::picture, ::reload-shade and ::mod-label. The reload button, the
// fader's groove, level and knob are drawn as before (a stroked edge, fractional places, a radius of their own).
//
// A MIDI track's instrument patch panel uses the same panel with instruments=true: IN is the
// track's notes and OUT goes on to its effects; cables only run from IN to an instrument's input and
// from its output to OUT (no sidechains, no other tracks), and each unit has a volume fader under
// its name (drag it or turn the mouse wheel over it; double-click: 0 dB). Its light mutes it.
//
// Every effect unit has a fader under its name too: its Mix (dry / wet, e86a408), 0 % (its input
// only) … 100 % (its sound only); double-click: 100 %; the mouse wheel: 5 % a notch.
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gfx/image.h"
#include "gfx/painter.h"
#include "ui/panel_style.h"
#include "ui/toolkit/widget.h"

namespace ui { class ScrollArea; }

namespace patch_panel {
constexpr int RAIL = 16;          // px, the rails at the top and bottom (with the screws)
constexpr int PAD = 14;           // px, between the rails and the units
constexpr int END_W = 54;         // px, the IN and OUT ends
constexpr int CARD_W = 236, CARD_H = 194;  // px, a unit
constexpr int PIC_X = 8, PIC_Y = 8, PIC_W = 220, PIC_H = 136;  // where the picture is, in a unit
constexpr int NAME_Y = 150;       // px, top of the name line, in a unit
constexpr int JACK_Y = 176;       // px, centre of the jacks and the light, in a unit
constexpr int JACK_R = 7;         // px
constexpr int LED_R = 5;          // px
constexpr int GAP = 34;           // px, between units placed side by side
constexpr int SAG = 38;           // px, a short cable hangs this far below its plugs (longer ones more)
constexpr int SNAP = 8;           // px, units are placed on this grid
constexpr int MARGIN = 24;        // px, room right of and below the last units
constexpr const char* CABLES[] = {"#e0533d", "#e8b23a", "#4aa3df", "#5bbf6a", "#b07cd8", "#e57fb0"};  // cable colours
constexpr int CABLE_COLOURS = 6;
constexpr int SCREW_EVERY = 150;  // px
constexpr int SIDE_DX = 24;       // px from a unit's input jack to its sidechain jack
constexpr int SOURCES_H = 52;     // px, the row of other tracks' jacks along the bottom
constexpr int SOURCE_W = 132;     // px from one track's jack to the next
constexpr int SOURCES_X = 118;    // px, the first track's jack
constexpr int VOLUME_H = 26;      // px, a unit's fader under its name: an instrument's volume, an effect's Mix (dry / wet)
constexpr int VOLUME_TEXT_W = 58; // px, its value, right of the fader (at least: valueWidth())
constexpr int KNOB_HALF_W = 5;    // px, half a fader knob's width
}  // namespace patch_panel

// A cable's end on the panel: IN, OUT, unit `index`, unit `index`'s sidechain input, or other track
// `index` (in the panel's `sources`). (patch_panel.py's IN / OUT / i / ("side", i) / ("track", k).)
struct PanelNode {
    enum Type { In, Out, Unit, Side, Track };
    Type type = In;
    int index = -1;
    static PanelNode in() { return {In, -1}; }
    static PanelNode out() { return {Out, -1}; }
    static PanelNode unit(int i) { return {Unit, i}; }
    static PanelNode side(int i) { return {Side, i}; }
    static PanelNode track(int k) { return {Track, k}; }
    bool operator==(const PanelNode& o) const { return type == o.type && index == o.index; }
    bool operator!=(const PanelNode& o) const { return !(*this == o); }
    bool operator<(const PanelNode& o) const { return type != o.type ? type < o.type : index < o.index; }
};
using PanelCable = std::pair<PanelNode, PanelNode>;  // (from, to)

// The unit (or OUT) a cable's far end goes into: a sidechain is its unit too.
PanelNode into(const PanelNode& node);

// A jack: the way sound leaves (Source: IN, a unit's output, another track) or comes in (Target: a
// unit's input, OUT; SideIn: a unit's sidechain input). node is IN / OUT / Unit / Track (never Side).
struct Jack {
    enum Kind { Source, Target, SideIn };
    Kind kind = Source;
    PanelNode node;
    bool operator==(const Jack& o) const { return kind == o.kind && node == o.node; }
    bool operator!=(const Jack& o) const { return !(*this == o); }
};

struct PanelUnit {
    std::string name;
    Image picture;                 // empty: none yet (a faceplate is drawn)
    bool bypass = false;
    std::string status = "On";     // shown next to the light
    bool problem = false;          // not loaded or stopped: the light is red
    bool reload = false;           // stopped, and can be started afresh: a Reload button on its picture
    bool idle = false;             // not wired in (its sound isn't heard): the light is amber
    bool sidechain = false;        // it has a sidechain input (a jack next to its input)
    std::string tooltip;
    std::optional<Point> pos;      // top-left in the space; none: the panel finds it a free spot
    std::optional<double> volumeDb;  // instruments: its own volume (a fader on the unit); none: no fader
    std::optional<double> mix;       // effects: its Mix, 0 (its input only) … 1 (its sound only), a fader too
    bool modulator = false;          // a modulator (an LFO…): it moves other units' knobs (drawn with a wave mark)
    // the fader's value: dB (volumeDb) or 0…1 (mix); none when the unit has no fader
    std::optional<double> fader() const { return volumeDb ? volumeDb : mix; }
};

class PatchPanel : public ui::Widget {
public:
    explicit PatchPanel(bool instruments = false);

    ui::Signal<int> selected;        // a unit was selected (its index)
    ui::Signal<int> opened;          // double-click / Enter: open its window
    ui::Signal<int> bypassToggled;
    ui::Signal<int, Point> unitMoved;  // dragged to a new top-left
    // (cable to remove or none, cable to add or none)
    ui::Signal<std::optional<PanelCable>, std::optional<PanelCable>> patched;
    ui::Signal<std::optional<Point>> addRequested;  // place an effect: at this top-left, or none: a free spot
    ui::Signal<int> removeRequested;
    ui::Signal<int> reloadRequested;                // a stopped unit's Reload button (its index)
    ui::Signal<int, Point> menuRequested;           // right-click on a unit (global position)
    ui::Signal<Point, Point> spaceMenuRequested;    // right-click on an empty spot (there, global position)
    // a unit's fader moved (its index, dB for an instrument, 0…1 for a Mix)
    ui::Signal<int, double> volumeChanged;

    bool instruments;                // an instrument patch panel (see the top)
    int cardH, jackY, row;           // a unit's height, its jacks' y, from one row of units to the next

    // --- content ---
    // cables: (from, to): from = IN, a unit or another track; to = a unit, a sidechain or OUT;
    // sources: the other tracks, (name, blocked: wiring it in would make a loop)
    void setUnits(std::vector<PanelUnit> units, std::vector<PanelCable> cables, std::optional<int> current = {},
                  std::vector<std::pair<std::string, bool>> sources = {});
    const std::vector<PanelUnit>& units() const { return units_; }
    const std::vector<PanelCable>& cables() const { return cables_; }
    // modulation (audio/modulation.h): a dashed line from a modulator unit to each unit whose knob it moves,
    // labelled with the knob's name and how far it moves it ("Cutoff +50 %")
    struct ModLink {
        int from, to;  // units
        std::string label;
    };
    void setModLinks(std::vector<ModLink> links);
    const std::vector<ModLink>& modLinks() const { return modLinks_; }
    const std::vector<std::pair<std::string, bool>>& sources() const { return sources_; }
    int current = -1;
    void setCurrent(int index);
    ui::ScrollArea* scrollArea() const;
    void ensureVisible(const Rect& r);

    // --- geometry ---
    int top() const { return patch_panel::RAIL + patch_panel::PAD; }
    Point contentSize() const;       // room for every unit (the space grows as units are moved further)
    int sourcesTop() const;
    void fit();
    Point sizeHint() const override { return contentSize(); }
    Rect unitRect(int index) const;
    int visibleWidth() const;
    // top-left of the first free place on the grid of rows (left to right, top to bottom) among the
    // `taken` units' places, within the visible width
    Point freeSpot(const std::vector<PanelUnit>& taken) const;
    Point spotAt(Point point) const; // top-left of a unit centred on `point`, on the grid and inside the space
    PointF jackPoint(const Jack& jack) const;
    std::vector<Jack> jacks() const;
    std::optional<Jack> jackAt(Point pos) const;
    Rect ledRect(int index) const;   // the light and its word (click to bypass)
    std::optional<Rect> reloadRect(int index) const;  // a stopped unit's Reload button, on its picture (none: no button)
    std::optional<Rect> volumeRect(int index) const;  // a unit's fader (volume or Mix), with its value
    std::pair<double, double> groove(int index) const;  // left and right end (x) of a fader's travel
    int valueWidth() const;  // px right of a fader for its value: its widest text, clear of the knob
    // the value for the knob at x: a volume in half dB, caught at 0 dB; a Mix in whole percent
    double volumeAt(int index, double x) const;
    double volumeX(int index, double value) const;
    std::optional<int> volumeUnder(Point pos) const;
    // move a unit's fader to `value` (dB for a volume, 0…1 for a Mix)
    void setVolume(int index, double value);
    std::optional<int> unitAt(Point pos) const;
    std::vector<int> paintOrder() const;  // bottom to top: the selected one on top
    Path cablePath(PointF a, PointF b) const;
    std::pair<PointF, PointF> cableEnds(const PanelCable& cable) const;
    static Jack targetJack(const PanelNode& node);       // the jack a cable's far end is plugged into
    static PanelNode targetNode(const Jack& jack);       // a cable's far end, for the jack it is plugged into
    std::optional<PanelCable> cableAt(Point pos) const;
    std::string nodeName(const PanelNode& node) const;

    // --- wiring rules ---
    // does sound from `start`'s output reach `goal` through `cables`?
    static bool reaches(const PanelNode& start, const PanelNode& goal, const std::vector<PanelCable>& cables);
    // can `cable` be plugged in (`without` the one being moved)? Not twice, not a unit into itself, never a loop
    bool canPlug(const PanelCable& cable, const std::optional<PanelCable>& without = {}) const;
    // the cable plugging the one being dragged into `jack` would make, or none if it can't go there
    std::optional<PanelCable> wiringTarget(const std::optional<Jack>& jack) const;

    // while a cable is being plugged in: the jack it hangs from, the cable lifted off an input (if any),
    // and where its loose end is
    struct Wiring { Jack from; std::optional<PanelCable> lifted; PointF to; };
    std::optional<Wiring> wiring;
    void startWiring(const Jack& jack, PointF pos);
    // what the mouse is over
    struct Hover {
        enum Kind { None, Unit, Led, Volume, JackHit, Reload };
        Kind kind = None;
        int index = -1;
        Jack jack;
        bool operator==(const Hover& o) const {
            return kind == o.kind && (kind == JackHit ? jack == o.jack : index == o.index);
        }
        bool operator!=(const Hover& o) const { return !(*this == o); }
    };
    Hover hover;
    std::optional<int> volumeDrag;   // the unit whose fader is being dragged

    // --- painting and events ---
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    void leave() override;
    std::string toolTipAt(Point p) const override;
    void hoverAt(Point pos);
    Color cableColor(const PanelCable& cable) const;  // the colours in turn, in the order they were plugged in

private:
    std::vector<PanelUnit> units_;
    std::vector<Image> pixmaps_;     // the pictures fitted to a unit's picture area
    std::vector<PanelCable> cables_;
    std::vector<ModLink> modLinks_;
    std::vector<std::pair<std::string, bool>> sources_;
    struct Press { int index; Point start, offset; };
    std::optional<Press> press_;
    bool moving_ = false;
    Image scaled(const Image& image) const;
    Font smallFont(bool bold = false, float size = -1.0f) const;
    void rightClick(Point pos, Point where);
    void paintPanel(Painter& p);
    void paintSources(Painter& p);
    void paintScrew(Painter& p, PointF c);
    void paintJack(Painter& p, const Jack& jack);
    void paintEnd(Painter& p, bool in);
    void paintUnit(Painter& p, int index);
    void paintVolume(Painter& p, int index);
    void paintFaceplate(Painter& p, const Rect& pic);
    void paintCable(Painter& p, PointF a, PointF b, Color cable);
    void paintWiring(Painter& p);
    // what it is drawn with, from the stylesheets (kept: it repaints while a cable or unit is dragged)
    struct Look {
        // (the boxes it draws: their styles, kept until parts_.refresh() answers true)
        const css::Style *self = nullptr, *railBox = nullptr, *sourcesBox = nullptr, *unitBox = nullptr,
                         *unitHoverBox = nullptr, *pictureBox = nullptr, *reloadShadeBox = nullptr, *modLabelBox = nullptr;
        Color railEdge, screw, screwSlot;
        int railEdgeH = 1;
        float screwR = 4, screwSlotW = 1.4f;
        Color sourcesTitle, source;
        float blockedOpacity = 0.35f;
        Font sourcesTitleFont, sourceFont;
        Color jack, jackRing, ring, ringHot;
        float jackRingW = 3, ringW = 2, ringHotW = 3;
        Color endTitle, endLabel, emptyHint;
        Font endTitleFont, endLabelFont, emptyHintFont;
        Color selected;
        float unitRadius = 3, selectedW = 2;
        float bypassedOpacity = 0.35f;
        Color faceplate, faceplateText, wave;
        float faceplateW = 1.6f, waveW = 3;
        Font faceplateFont;
        Color reload, reloadHot, reloadEdge, reloadText, reloadHotText;
        float reloadRadius = 4, reloadEdgeW = 1;  // (the radius before Options → Rounded corners)
        int reloadW = 96, reloadH = 28;           // (also where it is clicked)
        Font reloadFont;
        Color name, nameBypassed, sidechain;
        Font nameFont, sidechainFont;
        Color led[4], ledEdge;  // on, idle, bypassed, problem
        float ledEdgeW = 1;
        Color status, statusHot;
        Font statusFont;
        Color groove, level, levelBypassed, zeroMark, knob, knobHot, knobEdge, value, valueHot;
        float grooveRadius = 2, zeroMarkW = 1, knobRadius = 3, knobEdgeW = 1;  // (radii before Rounded corners)
        // the groove's thickness, the knob's size (as drawn: where the fader's values go is KNOB_HALF_W's)
        float grooveH = 4, knobW = 2 * patch_panel::KNOB_HALF_W, knobH = 16;
        Font valueFont;
        Color modLink;
        float modLinkW = 2;
        Font modFont;
        Color cables[patch_panel::CABLE_COLOURS];
        float cableW = 3.6f, cableOutlineW = 6;
    };
    const Look& look() const;
    mutable panel_style::Parts parts_{*this};
    mutable Look look_;
    void paintModLinks(Painter& p);
};
