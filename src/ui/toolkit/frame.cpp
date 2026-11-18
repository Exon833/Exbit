// TitleBar: the title bar of the frames the program draws (frame.h). The rest of the frame (the border, the
// resize band, placing the inside below the bar) is Window's (widget.cpp).
#include "ui/toolkit/frame.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "gfx/svg.h"
#include "ui/theme.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {
// how far the pointer goes with the button held before a press on the bar moves the window (a click, or the
// first of a double click, doesn't)
constexpr int kMoveThreshold = 4;
}  // namespace

TitleBar::TitleBar(Window* window) : Widget(nullptr), win_(window) {
    setStyleType("TitleBar");
    setMouseTracking(true);  // (a button's :hover)
}

TitleBar::~TitleBar() = default;

const char* TitleBar::partName(Part p) {
    switch (p) {
    case Icon: return "icon";
    case Title: return "title";
    case Minimize: return "minimize";
    case Maximize: return "maximize";
    case Close: return "close";
    default: return "";
    }
}

int TitleBar::barHeight() { return std::max(0, theme::metric("title-bar-height", 30)); }

bool TitleBar::hasButton(Part b) const {
    switch (b) {
    case Minimize: return !win_->isModal();
    case Maximize: return win_->isResizable();
    case Close: return true;
    default: return false;
    }
}

const css::Style& TitleBar::partStyle(Part p) const {
    styled_ = p;
    const css::Style& st = computedStyle(partName(p));
    styled_ = NoPart;
    return st;
}

void TitleBar::styleStates(std::vector<std::string>& out) const {
    if (!isEnabled()) out.push_back("disabled");
    out.push_back(win_->isActiveWindow() ? "active" : "inactive");
    if (win_->isWindowMaximized()) out.push_back("maximized");
    if (styled_ == NoPart) {  // the bar's own
        if (underMouse()) out.push_back("hover");
        if (isPressed()) out.push_back("pressed");
        return;
    }
    // a button's: under the pointer (and not another one held down), held down with the pointer on it
    if (hover_ == styled_ && (down_ == NoPart || down_ == styled_)) out.push_back("hover");
    if (down_ == styled_ && hover_ == styled_) out.push_back("pressed");
    // the first and the last of the buttons it has, as they are shown from left to right (the ends of the group)
    if (styled_ == Minimize || styled_ == Maximize || styled_ == Close) {
        Part shown[3];
        int n = shownButtons(shown);
        if (n > 0 && shown[0] == styled_) out.push_back("first");
        if (n > 0 && shown[n - 1] == styled_) out.push_back("last");
    }
}

bool TitleBar::buttonsLeft() {
    if (std::optional<std::string> side = theme::styles().root().variable("--title-buttons")) {
        std::string v = *side;
        v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return ch == ' ' || ch == '"' || ch == '\''; }), v.end());
        return v == "left";
    }
    return false;
}

// the buttons it has, from left to right as shown: minimize, maximize, close on the right (close outermost); close,
// maximize, minimize on the left
int TitleBar::shownButtons(Part out[3]) const {
    bool left = buttonsLeft();
    int n = 0;
    for (Part b : {Minimize, Maximize, Close})
        if (hasButton(b)) out[n++] = b;
    if (left) std::reverse(out, out + n);
    return n;
}

namespace {
// where a part `h` high goes in the bar's inside (from y, c high): centred in what its margin-top and margin-bottom
// leave of it (no margins: centred in the bar)
int partY(int y, int c, int h, const style::Insets& m) {
    int room = c - m.top - m.bottom - h;
    return y + m.top + (room >= 0 ? room / 2 : -((-room + 1) / 2));
}
}  // namespace

// out[part]: each part's rect (empty: not shown). The buttons go to the side --title-buttons names (right or
// left), close outermost; the icon to the other side; the title fills what is between (text-align: within it,
// or, centred, across the whole bar while that clears the rest)
void TitleBar::layoutParts(Rect out[6]) const {
    for (int i = 0; i < 6; ++i) out[i] = Rect();
    const css::Style& st = computedStyle();
    Rect c = style::contents(rect(), st);
    if (c.w <= 0 || c.h <= 0) return;
    bool left = buttonsLeft();
    int lo = c.x, hi = c.right();  // what is left between the two sides
    for (Part b : {Close, Maximize, Minimize}) {
        if (!hasButton(b)) continue;
        const css::Style& bs = partStyle(b);
        style::Insets m = style::insets(bs, "margin");
        int w = std::max(0, style::px(bs, "width", c.h));
        int h = std::min(c.h, std::max(0, style::px(bs, "height", c.h)));
        int y = partY(c.y, c.h, h, m);
        if (left) {
            out[b] = Rect(lo + m.left, y, w, h);
            lo += m.left + w + m.right;
        } else {
            out[b] = Rect(hi - m.right - w, y, w, h);
            hi -= m.left + w + m.right;
        }
    }
    if (!win_->options().iconSvg.empty()) {
        const css::Style& is = partStyle(Icon);
        style::Insets m = style::insets(is, "margin");
        int w = std::max(0, style::px(is, "width", 16)), h = std::min(c.h, std::max(0, style::px(is, "height", w)));
        int y = partY(c.y, c.h, h, m);
        if (left) {
            out[Icon] = Rect(hi - m.right - w, y, w, h);
            hi -= m.left + w + m.right;
        } else {
            out[Icon] = Rect(lo + m.left, y, w, h);
            lo += m.left + w + m.right;
        }
    }
    const css::Style& ts = partStyle(Title);
    style::Insets m = style::insets(ts, "margin");
    out[Title] = Rect(lo + m.left, c.y, std::max(0, hi - lo - m.w()), c.h);
}

Rect TitleBar::partRect(Part p) const {
    if (p <= NoPart || p > Close) return Rect();
    Rect r[6];
    layoutParts(r);
    return r[p];
}

TitleBar::Part TitleBar::partAt(Point p) const {
    Rect r[6];
    layoutParts(r);
    for (Part b : {Minimize, Maximize, Close, Icon})
        if (r[b].contains(p)) return b;
    return rect().contains(p) ? Title : NoPart;
}

std::string TitleBar::toolTipAt(Point p) const {
    switch (partAt(p)) {
    case Minimize: return "Minimize";
    case Maximize: return win_->isWindowMaximized() ? "Restore" : "Maximize";
    case Close: return "Close";
    default: return "";
    }
}

void TitleBar::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Rect r[6];
    layoutParts(r);
    // the icon
    if (!r[Icon].empty()) {
        int px = std::min(r[Icon].w, r[Icon].h);
        float scale = p.scale();
        int dev = std::max(1, (int)std::lround(px * scale));
        if (icon_.empty() || iconPx_ != dev) {
            icon_ = renderSvg(win_->options().iconSvg, dev, dev);
            iconPx_ = dev;
        }
        if (!icon_.empty())
            p.drawImage(RectF((float)r[Icon].x, (float)r[Icon].y + (r[Icon].h - px) / 2.f, (float)px, (float)px), icon_);
    }
    // the title
    if (!r[Title].empty()) {
        const css::Style& ts = partStyle(Title);
        style::box(p, r[Title], ts);
        Rect tr = style::contents(r[Title], ts);
        Font f = style::font(ts, theme::uiFont());
        std::string align = ts.keyword("text-align", "left");
        int flags = AlignVCenter;
        if (align == "center") {
            // centred across the whole bar while it fits there, beside the buttons and the icon
            int w = style::textWidth(f, win_->title());
            Rect whole = style::contents(rect(), st);
            int x = whole.x + (whole.w - w) / 2;
            if (x >= tr.x && x + w <= tr.right()) tr = Rect(x, tr.y, w, tr.h);
            flags |= AlignHCenter;
        } else if (align == "right" || align == "end") {
            flags |= AlignRight;
        } else {
            flags |= AlignLeft;
        }
        if (tr.w > 0) style::text(p, tr, flags, win_->title(), f, style::foreground(ts), ts, true);
    }
    // the buttons: each a box, its glyph drawn over it
    for (Part b : {Minimize, Maximize, Close}) {
        if (r[b].empty()) continue;
        const css::Style& bs = partStyle(b);
        style::box(p, r[b], bs);
        if (style::glyph(p, RectF(r[b]), bs)) continue;
        Color c = style::foreground(bs);
        float size = (float)std::max(2.0, bs.length("icon-size", 10));
        float sw = (float)std::max(0.5, bs.length("stroke-width", 1));
        // centred on a whole pixel, so 1 px lines are sharp (a line on x + .5)
        float cx = std::floor(r[b].x + r[b].w / 2.f), cy = std::floor(r[b].y + r[b].h / 2.f);
        float h = std::round(size / 2.f), half = std::fmod(std::round(sw), 2.f) == 1.f ? 0.5f : 0.f;
        float x0 = cx - h + half, x1 = cx + h - half, y0 = cy - h + half, y1 = cy + h - half;
        if (b == Minimize) {
            p.drawLine(PointF(x0, cy + half), PointF(x1, cy + half), c, sw);
        } else if (b == Maximize && !win_->isWindowMaximized()) {
            p.strokeRect(RectF(x0, y0, x1 - x0, y1 - y0), c, sw);
        } else if (b == Maximize) {
            // restore: a smaller square in front, the corner of another behind it
            float d = std::max(2.f, std::round(size / 5.f));
            p.strokeRect(RectF(x0, y0 + d, x1 - x0 - d, y1 - y0 - d), c, sw);
            p.drawPolyline({PointF(x0 + d, y0 + d), PointF(x0 + d, y0), PointF(x1, y0), PointF(x1, y1 - d),
                            PointF(x1 - d, y1 - d)},
                           c, sw);
        } else {
            p.drawLine(PointF(x0, y0), PointF(x1, y1), c, sw, LineCap::Square);
            p.drawLine(PointF(x0, y1), PointF(x1, y0), c, sw, LineCap::Square);
        }
    }
}

void TitleBar::setHoverPart(Part p) {
    if (p == hover_) return;
    hover_ = p;
    update();
}

void TitleBar::leave() {
    setHoverPart(NoPart);
}

bool TitleBar::mousePress(MouseEvent& e) {
    Part part = partAt(e.pos);
    setHoverPart(part);
    if (e.button != plat::LeftButton) return e.button == plat::RightButton;  // (right: the menu, contextMenu)
    if (part == Minimize || part == Maximize || part == Close) {
        down_ = part;
        update();
        return true;
    }
    // the bar: moved once the pointer goes far enough with the button held
    armed_ = true;
    moving_ = false;
    pressGlobal_ = win_->framePointer();
    Rect g = win_->screenGeometry();
    startPos_ = Point(g.x, g.y);
    return true;
}

bool TitleBar::mouseDoubleClick(MouseEvent& e) {
    Part part = partAt(e.pos);
    if (e.button == plat::LeftButton && (part == Title || part == Icon || part == NoPart)) {
        armed_ = false;
        if (win_->isResizable()) {
            win_->setWindowMaximized(!win_->isWindowMaximized());
            update();
        }
        return true;
    }
    return mousePress(e);
}

bool TitleBar::mouseMove(MouseEvent& e) {
    setHoverPart(partAt(e.pos));
    if (!(e.buttons & plat::LeftButton)) {
        armed_ = moving_ = false;
        return true;
    }
    Point pointer = win_->framePointer();
    if (armed_ && !moving_) {
        Point d = pointer - pressGlobal_;
        if (std::abs(d.x) < kMoveThreshold && std::abs(d.y) < kMoveThreshold) return true;
        armed_ = false;
        if (win_->beginSystemMove(e.globalPos)) return true;  // (the window manager follows the pointer now)
        moving_ = !win_->isWindowMaximized();  // (by hand: not while maximized)
    }
    if (moving_) {
        Point d = pointer - pressGlobal_;
        win_->moveWindow(startPos_.x + d.x, startPos_.y + d.y);
    }
    return true;
}

bool TitleBar::mouseRelease(MouseEvent& e) {
    Part was = down_;
    down_ = NoPart;
    armed_ = moving_ = false;
    setHoverPart(partAt(e.pos));
    update();
    if (e.button == plat::LeftButton && was != NoPart && partAt(e.pos) == was) trigger(was);
    return true;
}

void TitleBar::trigger(Part b) {
    switch (b) {
    case Minimize: win_->minimizeWindow(); break;
    case Maximize: win_->setWindowMaximized(!win_->isWindowMaximized()); break;
    case Close: win_->close(); break;  // (as the window manager's ✕: closeRequested asks first)
    default: break;
    }
}

bool TitleBar::contextMenu(MouseEvent& e) {
    showMenu(e.globalPos);
    return true;
}

void TitleBar::showMenu(Point globalPos) {
    if (!menu_) menu_ = std::make_unique<Menu>("Window");
    menu_->clear();
    Window* w = win_;
    // (run after the menu closed: closing may ask, or delete the window and with it this menu)
    auto later = [w](std::function<void(Window*)> fn) {
        return [w, fn] {
            App::get().singleShot(0, [w, fn] {
                if (Widget::alive(w) && w->isWindowShown()) fn(w);
            });
        };
    };
    Action* mn = menu_->addAction("Minimize", later([](Window* x) { x->minimizeWindow(); }));
    mn->enabled = hasButton(Minimize);
    bool maxed = w->isWindowMaximized();
    Action* mx = menu_->addAction(maxed ? "Restore" : "Maximize",
                                  later([maxed](Window* x) { x->setWindowMaximized(!maxed); }));
    mx->enabled = hasButton(Maximize);
    menu_->addSeparator();
    menu_->addAction("Close", later([](Window* x) { x->close(); }));
    menu_->popup(globalPos, this);
}

}  // namespace ui
