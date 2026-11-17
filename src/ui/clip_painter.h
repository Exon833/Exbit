// Drawing one clip: its box, its contents (waveform or MIDI notes) and its name (clip_painter.py).
//
// The timeline often repaints only a strip (playhead moves, scrolling), so everything here draws
// only what falls inside `visible` and gives identical pixels whether the clip is painted whole or
// strip by strip. The contents come as ready-made images from waveform_cache.h.
//
// STYLES (docs/skins.md): a clip is a node `Clip` under the widget it is drawn in (the Timeline), with the class
// .audio or .midi and the states :selected :disabled :frozen :muted (not heard: muted, or another track soloed)
// :linked :looping. What it reads (Look): Clip's stroke-width (its outline) and padding (a switched-off clip's
// outline is that far inside its box); the boxes Clip::body (the whole clip) and Clip::header (its name strip), in
// the clip's own colours (data: its colour, or its kind's --clip-audio / --clip-midi, with automatic contrast);
// Clip::overlay (a box over all of it: a frozen track's icy tint); Clip::name (padding, font); Clip::fade
// (background-color: the shade over what a fade quietens; stroke-width: its curve); Clip::loop-start
// (stroke-width, height: a looping clip's notches); Clip::link (stroke-width: the chain mark). The sizes the mouse
// also uses are :root variables: --clip-header-height, --clip-edge-width, --fade-handle-size, --link-badge-width,
// --link-badge-height.
#pragma once
#include <functional>
#include <map>
#include <string>

#include "core/css.h"
#include "gfx/painter.h"
#include "model/model.h"
#include "ui/paint_helpers.h"
#include "ui/selection.h"

namespace ui { class Widget; }

namespace clip_painter {

// --- the sizes the skin sets (:root variables; the config constants when unset). Also where the mouse grabs. ---
int headerHeight();     // --clip-header-height (CLIP_HANDLE_H): the name strip, which moves the clip
int edgeWidth();        // --clip-edge-width (CLIP_EDGE_PX): each end, which trims it
int fadeHandleSize();   // --fade-handle-size (FADE_HANDLE_PX): the squares that set an audio clip's fades
int linkBadgeWidth();   // --link-badge-width (LINK_BADGE_W): a linked clip's chain mark
int linkBadgeHeight();  // --link-badge-height (LINK_BADGE_H)

constexpr int FADE_HANDLE_PX = 7;  // (the default) the square at each top corner of an audio clip's body: drag it to fade

// What the stylesheets give a clip in one state (see the top), read once per state and stylesheet change.
struct Look {
    float border = 1.5f;          // Clip: stroke-width
    float disabledInset = 0.75f;  // Clip: padding (a switched-off clip's outline)
    css::Style body, header, overlay;  // Clip::body, ::header, ::overlay (boxes; the first two in the clip's colours)
    bool overlayShown = false;    // (whether ::overlay draws anything: only a frozen track's clips, by default)
    Font nameFont;                // Clip::name
    int namePadTop = 3, namePadLeft = 6, namePadRight = 6;
    Color fadeShade;              // Clip::fade: background-color
    float fadeStroke = 1.2f;      // …and stroke-width
    float loopStroke = 2;         // Clip::loop-start: stroke-width
    int loopHeight = 20;          // …and height
    float linkStroke = 1.6f;      // Clip::link: stroke-width
    // the body / header box in a clip's colour (kept: there are few colours, used every frame)
    const css::Style& bodyIn(Color c) const { return bodies_.get(body, c); }
    const css::Style& headerIn(Color c) const { return headers_.get(header, c); }
    paint::Recolored bodies_, headers_;
};

// A clip's looks in each state, under `owner` (the parent of its node in the stylesheets).
class Looks {
public:
    explicit Looks(const ui::Widget* owner = nullptr) : owner_(owner) {}
    // read them again if the stylesheets or the owner's states changed (call once per paint, before get)
    void refresh();
    const Look& get(const Clip& clip, bool selected, bool audible, bool frozen, int linkedWith);

private:
    const ui::Widget* owner_;
    int generation_ = -1;
    std::string states_;
    std::map<int, Look> looks_;
};

// the pixels a QRectF covers when filled without antialiasing (pixel centres inside it)
Rect pixels(const RectF& r);

// Box, darker handle strip on top (drag to move), contents below (select / cut), name. Colours come
// from the clip's colour (or its kind's default) with automatic contrast. A frozen track's clips get
// an icy tint (they are locked). A disabled clip is only its outline and name, in its colour. An audio
// clip shows its fades and their handles (handles = false: not on a take lane, where its fades aren't
// edited), and a looping one a notch where each repeat begins. A MIDI clip linked to others (linkedWith:
// how many clips share its link, itself included; links::counts) shows a chain mark at the right of its
// name strip. `look`: Looks::get for this clip in this state.
void paintClip(Painter& p, const Look& look, const Clip& clip, const RectF& rect, const Rect& visible, int viewLeft,
               double pps, int sr, bool selected, bool audible, bool frozen = false, bool handles = true,
               int linkedWith = 0);
// --- linked MIDI clips ---
constexpr int LINK_BADGE_W = 14, LINK_BADGE_H = 8;  // (the defaults) the chain mark in a linked clip's name strip
// where a linked clip's chain mark goes in its box (`rect`); false: the clip is too narrow to show it
bool linkBadge(const RectF& rect, RectF& badge);
void paintLinkBadge(Painter& p, const RectF& badge, Color color, float width = 1.6f);
// a looping audio clip: a notch (`height` from the clip's top, `width` wide) where each repeat of its loop begins
void paintLoopStarts(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, double pps, int sr,
                     Color color, float width, int height);
// An audio clip's fade handles (in, out): squares where its fades end (at least clear of the clip's
// trimming edges); false for a clip too narrow to have them.
bool fadeHandles(const Clip& clip, const RectF& body, double pps, int sr, RectF& in, RectF& out);
// each fade as its shape's curve (a straight line for Linear, `width` wide) from silence at the clip's edge to
// full level where it ends, the part it quietens shaded; and the two handles
void paintFades(Painter& p, const Clip& clip, const RectF& body, double pps, int sr, Color color, Color shade,
                float width);
// its name, and its gain when it has one ("Kick   -3.0 dB")
std::string clipLabel(const Clip& clip);
// name sticks to the view's left edge, but stays inside the clip
void paintName(Painter& p, const Look& look, const Clip& clip, const RectF& rect, int viewLeft, Color color);
// the waveform or MIDI notes: copies of ready-made tiles, only those that overlap `visible` (in the
// painter's device pixels when it draws at another scale: sharp at any interface size)
void paintContents(Painter& p, const Clip& clip, const RectF& body, const Rect& visible, double pps, int sr,
                   Color color);
// the selected time range: a box (`fill`) over each selected lane (spans: (lane top, height) per track), and a
// line at each end across them all
void paintRange(Painter& p, const TimeRange& range, const std::vector<std::pair<int, int>>& spans,
                const std::function<double(int64_t)>& xOf, const Rect& visible, const css::Style& fill, Color edge,
                int edgeWidth = 1);

}  // namespace clip_painter
