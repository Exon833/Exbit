// (internal to the toolkit) drawing helpers: what the skin's stylesheets give a widget (Widget::computedStyle) turned
// into colours, boxes, padding and fonts; the flat look's small drawings; and the base of popup windows (menus,
// drop-down lists).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>

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
// A box as CSS draws one: its background, inside its border (one colour: border-top-color; a side of 0 px has
// none), with rounded corners. (A border of the background's colour, or transparent, is drawn as the fill alone.)
inline void box(Painter& p, const Rect& r, const css::Style& s) {
    Color bg = background(s);
    Insets b = insets(s, "border-width");
    Color edge = color(s, "border-top-color", foreground(s));
    float rad = radius(s);
    if ((b.w() == 0 && b.h() == 0) || edge.a == 0 || edge == bg) {
        fill(p, r, bg, rad);
        return;
    }
    fill(p, r, edge, rad);
    int most = std::max(std::max(b.left, b.right), std::max(b.top, b.bottom));
    fill(p, inside(r, b), bg, std::max(0.f, rad - (float)most));
}
// the font it is drawn in: `def` (the control's own) with what the stylesheets change of it: font-family (its
// first name; system-ui and sans-serif: the interface font, monospace: the numbers font), font-size, font-weight
inline Font font(const css::Style& s, const Font& def) {
    Font f = def;
    std::vector<std::string> families = s.list("font-family");
    if (!families.empty()) {
        std::string first = families[0], key;
        for (char ch : first) key += (char)std::tolower((unsigned char)ch);
        std::string family = key == "system-ui" || key == "sans-serif" || key == "ui-sans-serif" ? Font::ui().family()
                             : key == "monospace" || key == "ui-monospace" ? Font::mono().family()
                                                                           : first;
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
