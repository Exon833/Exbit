// (internal to the toolkit) drawing helpers: what the skin's stylesheets give a widget (Widget::computedStyle) turned
// into colours, boxes, padding and fonts; the flat look's small drawings; and the base of popup windows (menus,
// drop-down lists).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/painter.h"
#include "ui/theme.h"
#include "ui/toolkit/widget.h"

namespace ui {
namespace style {

// a rounded fill (square when the skin's corners are off)
inline void fill(Painter& p, const Rect& r, Color c, float rad) {
    if (r.empty() || c.a == 0) return;
    if (rad <= 0.01f) p.fillRect(r, c);
    else p.fillRoundedRect(RectF(r), rad, c);
}

// a fill with a 1 px edge inside r (as a CSS border with border-radius)
inline void frame(Painter& p, const Rect& r, Color fillC, Color edge, float rad) {
    if (r.empty()) return;
    if (edge == fillC || edge.a == 0) {
        fill(p, r, fillC, rad);
        return;
    }
    fill(p, r, edge, rad);
    Rect in = r.adjusted(1, 1, -1, -1);
    if (fillC.a == 255) {
        fill(p, in, fillC, std::max(0.f, rad - 1));
    } else if (fillC.a > 0) {
        // (a see-through fill over the edge colour would tint it: cut the inside first)
        fill(p, in, fillC, std::max(0.f, rad - 1));
    }
}

// the small drawings of theme.ICON_LINES, in a size x size box at (x, y), stroke 1.5 px at 10 px
inline void icon(Painter& p, float x, float y, float size, const char* kind, Color c) {
    static const float down[] = {2, 3.5f, 5, 6.5f, 8, 3.5f};
    static const float up[] = {2, 6.5f, 5, 3.5f, 8, 6.5f};
    static const float right[] = {3.5f, 2, 6.5f, 5, 3.5f, 8};
    static const float left[] = {6.5f, 2, 3.5f, 5, 6.5f, 8};
    static const float check[] = {1.8f, 5.2f, 4.1f, 7.5f, 8.3f, 2.6f};
    const float* pts = down;
    std::string k = kind;
    if (k == "up") pts = up;
    else if (k == "right") pts = right;
    else if (k == "left") pts = left;
    else if (k == "check") pts = check;
    float s = size / 10.f;
    std::vector<PointF> v;
    for (int i = 0; i < 3; ++i) v.push_back(PointF(x + pts[i * 2] * s, y + pts[i * 2 + 1] * s));
    p.drawPolyline(v, c, 1.5f * s, LineCap::Round, LineJoin::Round);
}

// --- what the stylesheets give (Widget::computedStyle(part)) ---
inline Color toColor(const css::Rgba& c) { return Color(c.r, c.g, c.b, c.a); }
inline css::Rgba toRgba(const Color& c) { return css::Rgba{c.r, c.g, c.b, c.a}; }
// a colour property (color, background-color, border-top-color…); `def` when not set or not a colour
inline Color color(const css::Style& s, const std::string& property, Color def = Color(0, 0, 0, 0)) {
    return toColor(s.color(property, toRgba(def)));
}
inline Color background(const css::Style& s) { return color(s, "background-color"); }
inline Color foreground(const css::Style& s) { return color(s, "color", theme::c("window_text")); }
// a length in px (rounded to whole pixels, as the toolkit places things)
inline int px(const css::Style& s, const std::string& property, int def = 0) {
    return (int)std::lround(s.length(property, def));
}
// padding / margin / border-width as whole pixels: (left, top, right, bottom) to take off a rect
struct Insets {
    int left = 0, top = 0, right = 0, bottom = 0;
    int w() const { return left + right; }
    int h() const { return top + bottom; }
};
inline Insets insets(const css::Style& s, const std::string& prefix) {
    css::Edges e = s.edges(prefix, css::Edges());
    return Insets{(int)std::lround(e.left), (int)std::lround(e.top), (int)std::lround(e.right), (int)std::lround(e.bottom)};
}
inline Rect inside(const Rect& r, const Insets& i) { return r.adjusted(i.left, i.top, -i.right, -i.bottom); }
// the area for its content: inside its border and padding
inline Rect contents(const Rect& r, const css::Style& s) {
    return inside(inside(r, insets(s, "border-width")), insets(s, "padding"));
}
// its corners' radius (one for all four: border-top-left-radius), 0 when Options → Rounded corners is off
inline float radius(const css::Style& s) {
    return theme::rounded ? (float)std::max(0.0, s.length("border-top-left-radius", 0)) : 0.f;
}
// What a box is drawn from, worked out once per style (kept in the style's memo: drawing a box every frame
// costs a pointer check, not parsing): its background, its border's colour and widths, its corners' radius.
// A box with no more than that (one border colour and style, one radius, a flat background) is a plain box,
// drawn here inline; anything more (per-side colours or styles, four radii, gradients, shadows: BoxPaint) is
// worked out once too and drawn by drawBox (style_box.cpp).
struct BoxPaint;
struct BoxLook {
    Color bg, edge;
    Insets border;
    float radius = 0;  // (before Options → Rounded corners)
    bool fillOnly = true;  // no border to draw (none, transparent, or of the background's colour)
    std::shared_ptr<const BoxPaint> more;  // null: a plain box
    std::vector<css::Style::Shadow> textShadows;  // text-shadow (the first on top): style::text
    float opacity = 1;  // the node's whole opacity (Widget::paintTree), 0..1
};
// what a box has beyond a plain one (null: nothing), and drawing such a box (style_box.cpp)
std::shared_ptr<const BoxPaint> boxPaint(const css::Style& s, const BoxLook& look);
void drawBox(Painter& p, const RectF& r, const BoxLook& look);
// (tests, timings) how many shadow masks were made (not found kept), and forgetting the kept ones
int shadowMasksMade();
void clearBoxCaches();
inline const BoxLook& boxLook(const css::Style& s) {
    if (!s.memo_.value) {
        auto look = std::make_shared<BoxLook>();
        look->bg = background(s);
        look->border = insets(s, "border-width");
        look->edge = color(s, "border-top-color", foreground(s));
        look->radius = (float)std::max(0.0, s.length("border-top-left-radius", 0));
        look->fillOnly = (look->border.w() == 0 && look->border.h() == 0) || look->edge.a == 0 || look->edge == look->bg;
        look->more = boxPaint(s, *look);
        if (s.has("text-shadow")) look->textShadows = s.shadows("text-shadow");
        if (s.has("opacity")) look->opacity = (float)std::clamp(s.number("opacity", 1), 0.0, 1.0);
        s.memo_.value = look;
    }
    return *static_cast<const BoxLook*>(s.memo_.value.get());
}
// A box as CSS draws one: its shadows, its background (colour and gradients) inside its border (each side its
// width, colour and style), with rounded corners (each its radius). (A border of the background's colour, or
// transparent, is drawn as the fill alone.)
inline void box(Painter& p, const Rect& r, const css::Style& s) {
    const BoxLook& b = boxLook(s);
    if (b.more) {
        drawBox(p, RectF(r), b);
        return;
    }
    float rad = theme::rounded ? b.radius : 0.f;
    if (b.fillOnly) {
        fill(p, r, b.bg, rad);
        return;
    }
    fill(p, r, b.edge, rad);
    int most = std::max(std::max(b.border.left, b.border.right), std::max(b.border.top, b.border.bottom));
    fill(p, inside(r, b.border), b.bg, std::max(0.f, rad - (float)most));
}
// the same on a fractional rectangle (a knob's cap, a fader's groove, a pad): its edges anti-aliased
inline void box(Painter& p, const RectF& r, const css::Style& s) {
    const BoxLook& b = boxLook(s);
    if (b.more) {
        drawBox(p, r, b);
        return;
    }
    auto fillF = [&p](const RectF& q, Color c, float rad) {
        if (q.empty() || c.a == 0) return;
        if (rad <= 0.01f) p.fillRect(q, c);
        else p.fillRoundedRect(q, rad, c);
    };
    float rad = theme::rounded ? b.radius : 0.f;
    if (b.fillOnly) {
        fillF(r, b.bg, rad);
        return;
    }
    fillF(r, b.edge, rad);
    int most = std::max(std::max(b.border.left, b.border.right), std::max(b.border.top, b.border.bottom));
    fillF(r.adjusted((float)b.border.left, (float)b.border.top, (float)-b.border.right, (float)-b.border.bottom), b.bg,
          std::max(0.f, rad - (float)most));
}
// the font it is drawn in: `def` (the control's own) with what the stylesheets change of it: font-family (the
// first of its list this computer has, theme::pickFamily; system-ui and sans-serif: the interface font,
// monospace: the numbers font), font-size, font-weight
inline Font font(const css::Style& s, const Font& def) {
    Font f = def;
    std::vector<std::string> families = s.list("font-family");
    if (!families.empty()) {
        std::string picked = theme::pickFamily(families);
        std::string family = picked.empty() ? Font::ui().family() : picked == "monospace" ? Font::mono().family() : picked;
        if (family != f.family()) f = Font(family, f.pixelSize(), f.bold());
    }
    if (s.has("font-size")) {
        float size = (float)s.fontSizePx(f.pixelSize());
        if (size > 0 && std::fabs(size - f.pixelSize()) > 0.01f) f = f.withPixelSize(size);
    }
    if (s.has("font-weight")) {
        std::string w = s.keyword("font-weight", "");
        double n = s.number("font-weight", 0);
        bool bold = w == "bold" || w == "bolder" || n >= 600;
        if (w == "normal" || w == "lighter" || (n > 0 && n < 600)) bold = false;
        else if (!bold) bold = f.bold();
        if (bold != f.bold()) f = f.withBold(bold);
    }
    return f;
}

inline int textHeight(const Font& f) { return (int)std::ceil(f.ascent() + f.descent()); }
inline int textWidth(const Font& f, const std::string& s) { return (int)std::ceil(f.width(s)); }

// one line of text, vertically centred in r (baseline as Qt places it)
inline void text(Painter& p, const Rect& r, int align, const std::string& s, const Font& f, Color c, bool elide = false) {
    p.drawText(RectF(r), align, s, f, c, elide);
}
// text-shadow's shadows of that text, drawn before it (the last first; blurred ones through a blurred mask,
// kept for the next repaint) (style_box.cpp)
void textShadows(Painter& p, const RectF& r, int align, const std::string& s, const Font& f,
                 const std::vector<css::Style::Shadow>& shadows, bool elide);
// the same for Painter::drawTextBlock's lines
void textBlockShadows(Painter& p, const RectF& r, int align, const std::string& s, const Font& f,
                      const std::vector<css::Style::Shadow>& shadows, bool wrap);
// the same with the style it is drawn in: its text-shadow drawn under it
inline void text(Painter& p, const Rect& r, int align, const std::string& s, const Font& f, Color c,
                 const css::Style& style, bool elide = false) {
    const BoxLook& b = boxLook(style);
    if (!b.textShadows.empty()) textShadows(p, RectF(r), align, s, f, b.textShadows, elide);
    p.drawText(RectF(r), align, s, f, c, elide);
}

}  // namespace style

// a window for menus and drop-down lists: no frame, above everything, never takes the focus;
// open() shows it through App::openPopup (it gets the mouse and keys first; a press outside closes it)
class PopupWindow : public Window {
public:
    PopupWindow() : Window(opts()) {}
    static plat::WindowOptions opts() {
        plat::WindowOptions o;
        o.popup = true;
        o.takesFocus = false;
        o.resizable = false;
        o.title = "popup";
        o.w = 10;
        o.h = 10;
        return o;
    }
    // shows it with its top-left at `at` (screen coordinates), sized w x h
    void open(Point at, int w, int h, Widget* owner) {
        resizeWindow(w, h);
        moveWindow(at.x, at.y);
        App::get().openPopup(this, owner, [this] { dismiss(); });
    }
    virtual void dismiss() { App::get().closePopup(this); }
    bool isOpen() const {
        auto& v = App::get().popups();
        for (Window* w : v)
            if (w == this) return true;
        return false;
    }
};

// where a popup of size (w, h) goes below `anchor` (screen rect), or above it when there is no room
inline Point placeBelow(const Rect& anchor, int w, int h) {
    Rect scr = App::get().screenAt(Point(anchor.x + anchor.w / 2, anchor.y + anchor.h / 2));
    int x = anchor.x, y = anchor.bottom();
    if (y + h > scr.bottom() && anchor.y - h >= scr.y) y = anchor.y - h;
    else if (y + h > scr.bottom()) y = std::max(scr.y, scr.bottom() - h);
    if (x + w > scr.right()) x = std::max(scr.x, scr.right() - w);
    if (x < scr.x) x = scr.x;
    return Point(x, y);
}

}  // namespace ui
