// (internal to the toolkit) drawing helpers for the flat look of theme.py's stylesheet, and the
// base of popup windows (menus, drop-down lists).
#pragma once
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
