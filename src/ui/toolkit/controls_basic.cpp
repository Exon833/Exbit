// Label, Button, CheckBox, GroupBox, Separator, SplitterHandle, StatusBar, TabWidget — in the flat
// look of theme.py's stylesheet.
#include <algorithm>
#include <cmath>

#include "gfx/svg.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// greedy word wrapping at spaces (as the painter's text blocks do)
std::vector<std::string> wrapLines(const std::string& text, const Font& f, float width) {
    std::vector<std::string> out;
    for (const std::string& para : splitLines(text)) {
        if (f.width(para) <= width) {
            out.push_back(para);
            continue;
        }
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && f.width(cand) > width) {
                out.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        out.push_back(line);
    }
    return out;
}

int linesHeight(const Font& f, int n) {
    if (n <= 0) return 0;
    return (int)std::ceil((n - 1) * f.lineSpacing() + f.ascent() + f.descent());
}
}  // namespace

// ============================================================================================
// Label
// ============================================================================================

Label::Label(const std::string& text, Widget* parent) : Widget(parent), text_(text) {}

void Label::setText(const std::string& t) {
    if (t == text_) return;
    text_ = t;
    updateGeometry();
    update();
}

Font Label::font() const {
    if (hasFont_) return font_;
    if (objectName == "time") return theme::monoFont(12);
    if (objectName == "lufs") return theme::monoFont(8);
    if (property("dimmed") == "true") return theme::dimmedFont();
    return theme::uiFont();
}

Color Label::color() const {
    if (hasColor_) return color_;
    if (!isEnabled()) return theme::c("disabled_text");
    if (!link_.empty()) return theme::c("accent");
    if (property("hint") == "true") return theme::c("hint_text");
    if (property("dimmed") == "true") return theme::c("dimmed_text");
    return theme::c("window_text");
}

Point Label::sizeHint() const {
    Point h(0, 0);
    if (!image_.empty()) h = Point(image_.width(), image_.height());
    if (!text_.empty()) {
        Font f = font();
        std::vector<std::string> lines = splitLines(text_);
        float w = 0;
        for (auto& l : lines) w = std::max(w, f.width(l));
        if (wrap_ && w > 360) {
            lines = wrapLines(text_, f, 360);
            w = 0;
            for (auto& l : lines) w = std::max(w, f.width(l));
        }
        h.x = std::max(h.x, (int)std::ceil(w));
        h.y = std::max(h.y, linesHeight(f, (int)lines.size()));
    }
    h.x += 2 * margin;
    h.y += 2 * margin;
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point Label::minimumSizeHint() const {
    Point h = sizeHint();
    if (wrap_ || elide_) h.x = std::min(h.x, 16 + 2 * margin);
    return h;
}

void Label::paint(Painter& p) {
    Rect r = rect().adjusted(margin, margin, -margin, -margin);
    if (!image_.empty()) {
        int x = r.x, y = r.y;
        if (align_ & AlignHCenter) x += (r.w - image_.width()) / 2;
        else if (align_ & AlignRight) x += r.w - image_.width();
        if (align_ & AlignVCenter) y += (r.h - image_.height()) / 2;
        else if (align_ & AlignBottom) y += r.h - image_.height();
        if (!isEnabled()) {
            p.save();
            p.setOpacity(0.45f);
            p.drawImage(Point(x, y), image_);
            p.restore();
        } else {
            p.drawImage(Point(x, y), image_);
        }
    }
    if (text_.empty()) return;
    Font f = font();
    Color c = color();
    bool multi = text_.find('\n') != std::string::npos;
    if (multi || wrap_) {
        p.drawTextBlock(RectF(r), align_, text_, f, c, wrap_);
        return;
    }
    p.drawText(RectF(r), align_, text_, f, c, elide_);
    if (!link_.empty()) {
        float tw = std::min((float)r.w, f.width(text_));
        float x = (float)r.x;
        if (align_ & AlignHCenter) x += (r.w - tw) / 2;
        else if (align_ & AlignRight) x += r.w - tw;
        float th = f.ascent() + f.descent();
        float top = (float)r.y;
        if (align_ & AlignVCenter) top += (r.h - th) / 2;
        else if (align_ & AlignBottom) top += r.h - th;
        int y = (int)std::floor(top + f.ascent() + 1.5f);
        p.drawHLine((int)x, (int)std::ceil(x + tw), y, c);
    }
}

bool Label::mousePress(MouseEvent& e) {
    if (!link_.empty() && e.button == plat::LeftButton) {
        onLink.emit(link_);
        return true;
    }
    return false;
}

bool Label::mouseDoubleClick(MouseEvent& e) {
    if (!doubleClicked.empty()) {
        doubleClicked.emit();
        return true;
    }
    return mousePress(e);
}

// ============================================================================================
// Button
// ============================================================================================

Button::Button(const std::string& text, Widget* parent) : Widget(parent), text_(text) {
    setFocusPolicy(FocusPolicy::Strong);
    vPolicy = SizePolicy::Fixed;
}

Button::~Button() { stopRepeat(); }

void Button::stopRepeat() {
    if (repeatTimer_ >= 0) {
        App::get().cancelTimer(repeatTimer_);
        repeatTimer_ = -1;
    }
}

void Button::setStyle(Style s) {
    style_ = s;
    if (s == Style::Tool) {
        hPolicy = SizePolicy::Fixed;
        if (focusPolicy() == FocusPolicy::Strong) setFocusPolicy(FocusPolicy::Tab);
    } else {
        hPolicy = SizePolicy::Preferred;
    }
    updateGeometry();
    update();
}

void Button::setText(const std::string& t) {
    if (t == text_) return;
    text_ = t;
    updateGeometry();
    update();
}

void Button::setIcon(const Image& icon) {
    icon_ = icon;
    iconSvg_.clear();
    iconPx_ = std::max(icon.width(), icon.height());
    tinted_ = Image();
    updateGeometry();
    update();
}

void Button::setIconSvg(const std::string& svgText, int px) {
    iconSvg_ = svgText;
    iconPx_ = px;
    icon_ = Image();
    tinted_ = Image();
    updateGeometry();
    update();
}

void Button::clearIcon() {
    icon_ = Image();
    iconSvg_.clear();
    iconPx_ = 0;
    tinted_ = Image();
    updateGeometry();
    update();
}

void Button::setChecked(bool on) {
    if (checked_ == on) return;
    checked_ = on;
    update();
    toggled.emit(on);
}

void Button::setMenu(Menu* menu, bool splitArrow) {
    menu_ = menu;
    split_ = menu && splitArrow;
    if (split_) setMouseTracking(true);
    updateGeometry();
    update();
}

Font Button::font() const { return hasFont_ ? font_ : theme::uiFont(); }

bool Button::lit() const {
    if (!isEnabled()) return false;
    if (property("lit") == "true") return true;
    if (checked_ && property("accent") == "true") return true;
    if (style_ == Style::Tool && checked_ && property("autoRaise") == "true") return true;
    if (style_ == Style::Tool && checked_ && !property("toggle").empty()) return true;
    if (style_ == Style::Push && default_) return true;
    return false;
}

Color Button::fillColor() const {
    const theme::Shades& sh = theme::shades();
    bool autoRaise = style_ == Style::Tool && property("autoRaise") == "true";
    if (!isEnabled()) {
        if (style_ == Style::Push) return sh.disabled;
        return autoRaise ? Color(0, 0, 0, 0) : theme::c("button");
    }
    bool hover = underMouse() && (!down_ || inside_);
    if (lit()) {
        std::string role = property("toggle");
        bool toggleLit = style_ == Style::Tool && checked_ && !role.empty() && property("lit") != "true" &&
                         property("accent") != "true";
        if (toggleLit) return hover ? theme::toggleHover(role) : theme::toggleColor(role);
        return hover ? sh.accentHover : theme::c("accent");
    }
    bool down = (down_ && inside_) || checked_ || menuArrowDown_;
    if (down) return sh.pressed;
    if (hover) return sh.hover;
    return autoRaise ? Color(0, 0, 0, 0) : theme::c("button");
}

Color Button::textColor() const {
    if (!isEnabled()) return theme::c("disabled_text");
    if (lit()) return theme::c("accent_text");
    return theme::c("button_text");
}

Point Button::sizeHint() const {
    Font f = font();
    int tw = text_.empty() ? 0 : style::textWidth(f, text_);
    int th = style::textHeight(f);
    bool hasIcon = iconPx_ > 0 && (!iconSvg_.empty() || !icon_.empty());
    Point h;
    if (style_ == Style::Push) {
        int cw = tw + (hasIcon ? iconPx_ + (text_.empty() ? 0 : 4) : 0);
        int ch = std::max(16, std::max(th, hasIcon ? iconPx_ : 0));
        h = Point(cw + 24 + 2, ch + 8 + 2);
        if (menu_ && !split_) h.x += 12;
    } else {
        bool autoRaise = property("autoRaise") == "true";
        int cw = hasIcon ? iconPx_ : tw;
        int ch = hasIcon ? std::max(iconPx_, text_.empty() ? 0 : 0) : th;
        if (hasIcon) h = Point(cw + 6 + 2, ch + 6 + 2);
        else h = Point(cw + 12, ch + 8);
        if (autoRaise) h = Point(h.x + 4, h.y + 2);
        if (menu_ && split_) h.x += 13;
        else if (menu_) h.x += 6;
    }
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void Button::paint(Painter& p) {
    const theme::Shades& sh = theme::shades();
    Rect r = rect();
    float rad = (float)theme::radius();
    Color fillC = fillColor();
    Color edge = fillC;
    bool isLit = lit();
    if (style_ == Style::Push && hasFocus() && !isLit && isEnabled()) edge = theme::c("accent");
    style::frame(p, r, fillC, edge, rad);
    Color tc = textColor();
    Rect content;
    bool autoRaise = style_ == Style::Tool && property("autoRaise") == "true";
    if (style_ == Style::Push) content = r.adjusted(13, 5, -13, -5);
    else if (autoRaise) content = r.adjusted(5, 4, -5, -4);
    else content = r.adjusted(3, 3, -3, -3);
    // the menu parts
    if (menu_ && split_) {
        Rect ar(r.right() - 13, 0, 13, r.h);
        if (isEnabled() && (hoverArrow_ || menuArrowDown_)) {
            p.save();
            p.setClip(ar);
            style::fill(p, r.adjusted(0, 0, 0, 0), sh.pressed, rad);
            p.restore();
        }
        p.fillRect(Rect(ar.x, 1, 1, r.h - 2), sh.line);
        style::icon(p, ar.x + 1 + (12 - 8) / 2.f, (r.h - 8) / 2.f, 8, "down", tc);
        content.w = std::max(0, ar.x - content.x - (autoRaise ? 4 : 2));
        if (!autoRaise) content.x = std::max(content.x, 3);
    } else if (menu_ && style_ == Style::Push) {
        style::icon(p, (float)(r.right() - 19), (r.h - 10) / 2.f, 10, "down", tc);
        content.w -= 10;
    } else if (menu_) {
        style::icon(p, (float)(r.right() - 9), (float)(r.h - 9), 6, "down", tc);
    }
    // icon and text
    Font f = font();
    bool hasIcon = iconPx_ > 0 && (!iconSvg_.empty() || !icon_.empty());
    const Image* img = nullptr;
    if (hasIcon) {
        if (!iconSvg_.empty()) {
            int px = p.scale() == 1 ? iconPx_ : (int)std::lround(iconPx_ * p.scale());  // (sharp at any interface size)
            if (tinted_.empty() || tintedFor_ != tc || tinted_.width() != px) {
                tinted_ = renderSvgTinted(iconSvg_, px, px, tc);
                tintedFor_ = tc;
            }
            img = &tinted_;
        } else {
            img = &icon_;
        }
        if (img->empty()) img = nullptr;
    }
    auto drawIcon = [&](float x, float y) {
        if (!img) return;
        RectF dst(x, y, (float)iconPx_, (float)iconPx_);
        bool dim = !isEnabled() && iconSvg_.empty();
        if (dim) {
            p.save();
            p.setOpacity(0.4f);
        }
        if (img->width() == iconPx_ && img->height() == iconPx_) p.drawImage(Point((int)x, (int)y), *img);
        else p.drawImage(dst, *img);
        if (dim) p.restore();
    };
    if (img && (style_ == Style::Tool || text_.empty())) {
        float x = content.x + (content.w - iconPx_) / 2.f, y = content.y + (content.h - iconPx_) / 2.f;
        drawIcon(std::round(x), std::round(y));
        return;
    }
    if (img) {
        int tw = style::textWidth(f, text_);
        int total = iconPx_ + 4 + tw;
        int x = content.x + std::max(0, (content.w - total) / 2);
        drawIcon((float)x, std::round(content.y + (content.h - iconPx_) / 2.f));
        Rect tr(x + iconPx_ + 4, r.y, std::max(0, content.right() - (x + iconPx_ + 4)), r.h);
        style::text(p, tr, AlignLeft | AlignVCenter, text_, f, tc, true);
        return;
    }
    if (!text_.empty()) {
        // (centred over the whole height, as the padding is symmetric)
        Rect tr(content.x - 2, r.y, content.w + 4, r.h);
        style::text(p, tr, AlignCenter, text_, f, tc, true);
    }
}

bool Button::mousePress(MouseEvent& e) {
    if (e.button == plat::RightButton) {
        if (!rightClicked.empty()) {
            rightClicked.emit(e);
            return true;
        }
        return false;
    }
    if (e.button != plat::LeftButton) return false;
    if (menu_ && (!split_ || e.pos.x >= width() - 13)) {
        menuArrowDown_ = true;
        update();
        showMenu();
        if (Widget::alive(this)) {
            menuArrowDown_ = false;
            down_ = false;
            update();
        }
        return true;
    }
    down_ = true;
    inside_ = true;
    update();
    pressed.emit();
    if (autoRepeat_) {
        stopRepeat();
        repeatTimer_ = App::get().singleShot(300, [this] {
            repeatTimer_ = App::get().every(100, [this] {
                if (down_ && inside_) clicked.emit();
            });
            if (down_ && inside_) clicked.emit();
        });
    }
    return true;
}

bool Button::mouseMove(MouseEvent& e) {
    bool handled = false;
    if (split_) {
        bool h = rect().contains(e.pos) && e.pos.x >= width() - 13;
        if (h != hoverArrow_) {
            hoverArrow_ = h;
            update();
        }
        handled = true;
    }
    if (down_) {
        bool in = rect().contains(e.pos);
        if (in != inside_) {
            inside_ = in;
            update();
        }
        handled = true;
    }
    return handled;
}

bool Button::mouseRelease(MouseEvent& e) {
    if (e.button != plat::LeftButton || !down_) return false;
    down_ = false;
    stopRepeat();
    update();
    released.emit();
    if (rect().contains(e.pos) && isEnabled()) {
        if (checkable_) setChecked(!checked_);
        clicked.emit();
    }
    return true;
}

bool Button::mouseDoubleClick(MouseEvent& e) { return mousePress(e); }

bool Button::keyPress(KeyEvent& e) {
    bool plain = !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModMeta));
    if (plain && e.key == plat::Key_Space && !e.autoRepeat) {
        if (menu_ && !split_) showMenu();
        else click();
        return true;
    }
    if (plain && style_ == Style::Push && (e.key == plat::Key_Return || e.key == plat::Key_Enter)) {
        if (menu_ && !split_) showMenu();
        else click();
        return true;
    }
    return false;
}

void Button::click() {
    if (!isEnabled()) return;
    if (checkable_) setChecked(!checked_);
    clicked.emit();
}

void Button::showMenu() {
    if (!menu_) return;
    Point g = mapToGlobal(Point(0, 0));
    menuArrowDown_ = true;
    update();
    menu_->execBelow(Rect(g.x, g.y, width(), height()), this);
    if (Widget::alive(this)) {
        menuArrowDown_ = false;
        update();
    }
}

Button* pushButton(Widget* parent, const std::string& text, std::function<void()> onClick) {
    Button* b = parent ? parent->add<Button>(text) : new Button(text);
    if (onClick) b->clicked.connect(std::move(onClick));
    return b;
}

Button* toolButton(Widget* parent, const std::string& text, std::function<void()> onClick, const std::string& tooltip) {
    Button* b = parent ? parent->add<Button>(text) : new Button(text);
    b->setStyle(Button::Style::Tool);
    if (!tooltip.empty()) b->setToolTip(tooltip);
    if (onClick) b->clicked.connect(std::move(onClick));
    return b;
}

// ============================================================================================
// CheckBox
// ============================================================================================

CheckBox::CheckBox(const std::string& text, Widget* parent) : Widget(parent), text_(text) {
    setFocusPolicy(FocusPolicy::Strong);
    vPolicy = SizePolicy::Fixed;
}

void CheckBox::setChecked(bool on) {
    if (checked_ == on) return;
    checked_ = on;
    update();
    toggled.emit(on);
}

Point CheckBox::sizeHint() const {
    Font f = theme::uiFont();
    int tw = text_.empty() ? 0 : style::textWidth(f, text_) + 6;
    Point h(14 + tw, std::max(style::textHeight(f), 14) + 4);
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void CheckBox::paint(Painter& p) {
    const theme::Shades& sh = theme::shades();
    float rad = (float)theme::radius();
    Rect ind(0, (height() - 14) / 2, 14, 14);
    if (!isEnabled()) {
        style::frame(p, ind, sh.disabled, sh.disabled, rad);
        if (checked_) style::icon(p, ind.x + 2.f, ind.y + 2.f, 10, "check", theme::c("accent_text"));
    } else if (checked_) {
        style::frame(p, ind, theme::c("accent"), theme::c("accent"), rad);
        style::icon(p, ind.x + 2.f, ind.y + 2.f, 10, "check", theme::c("accent_text"));
    } else {
        style::frame(p, ind, theme::c("base"), underMouse() ? theme::c("accent") : sh.fieldHover, rad);
    }
    if (!text_.empty()) {
        Rect tr(20, 0, std::max(0, width() - 20), height());
        style::text(p, tr, AlignLeft | AlignVCenter, text_, theme::uiFont(),
                    isEnabled() ? theme::c("window_text") : theme::c("disabled_text"), true);
    }
}

bool CheckBox::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    down_ = true;
    return true;
}

bool CheckBox::mouseMove(MouseEvent& e) { return down_; }

bool CheckBox::mouseRelease(MouseEvent& e) {
    if (e.button != plat::LeftButton || !down_) return false;
    down_ = false;
    if (rect().contains(e.pos)) setChecked(!checked_);
    return true;
}

bool CheckBox::keyPress(KeyEvent& e) {
    if (e.key == plat::Key_Space && !(e.mods & (plat::ModCtrl | plat::ModAlt))) {
        setChecked(!checked_);
        return true;
    }
    return false;
}

// ============================================================================================
// GroupBox
// ============================================================================================

GroupBox::GroupBox(const std::string& title, Widget* parent) : Widget(parent), title_(title) {}

Rect GroupBox::contentsRect() const { return Rect(6, 28, width() - 12, height() - 34); }

Point GroupBox::sizeHint() const {
    Point h = Widget::sizeHint();
    if (!layout()) h = Point(std::max(h.x, 12), std::max(h.y, 34));
    if (!title_.empty()) h.x = std::max(h.x, style::textWidth(theme::uiFont(), title_) + 8);
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point GroupBox::minimumSizeHint() const {
    Point h = Widget::minimumSizeHint();
    if (!layout()) h = Point(12, 34);
    if (!title_.empty()) h.x = std::max(h.x, style::textWidth(theme::uiFont(), title_) + 8);
    return h;
}

void GroupBox::paint(Painter& p) {
    style::fill(p, Rect(0, 20, width(), height() - 20), theme::shades().panel, (float)theme::radius());
    if (!title_.empty()) {
        style::text(p, Rect(4, 1, width() - 8, 18), AlignLeft | AlignVCenter, title_, theme::uiFont(),
                    isEnabled() ? theme::c("window_text") : theme::c("disabled_text"), true);
    }
}

// ============================================================================================
// Separator, SplitterHandle, StatusBar
// ============================================================================================

Separator::Separator(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    if (o == Orientation::Vertical) {
        marginAlong = 6;
        marginAcross = 3;
        hPolicy = SizePolicy::Fixed;
        vPolicy = SizePolicy::Preferred;
    } else {
        marginAlong = 0;
        marginAcross = 0;
        hPolicy = SizePolicy::Preferred;
        vPolicy = SizePolicy::Fixed;
    }
}

Point Separator::sizeHint() const {
    Point h = orientation == Orientation::Vertical ? Point(1 + 2 * marginAcross, 2 * marginAlong + 1)
                                                   : Point(2 * marginAlong + 1, 1 + 2 * marginAcross);
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void Separator::paint(Painter& p) {
    Color c = theme::shades().line;
    if (orientation == Orientation::Vertical) {
        int x = width() / 2;
        p.drawVLine(x, marginAlong, height() - marginAlong, c);
    } else {
        int y = height() / 2;
        p.drawHLine(marginAlong, width() - marginAlong, y, c);
    }
}

SplitterHandle::SplitterHandle(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    setCursor(o == Orientation::Horizontal ? Cursor::SizeHor : Cursor::SizeVer);
    if (o == Orientation::Horizontal) hPolicy = SizePolicy::Fixed;
    else vPolicy = SizePolicy::Fixed;
}

Point SplitterHandle::sizeHint() const {
    Point h = orientation == Orientation::Horizontal ? Point(5, 0) : Point(0, 5);
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void SplitterHandle::paint(Painter& p) {
    Color c = underMouse() || isPressed() ? theme::c("accent") : theme::shades().line;
    if (orientation == Orientation::Horizontal) p.fillRect(Rect(width() / 2, 0, 1, height()), c);
    else p.fillRect(Rect(0, height() / 2, width(), 1), c);
}

bool SplitterHandle::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    start_ = e.globalPos;
    pressed.emit();
    update();
    return true;
}

bool SplitterHandle::mouseMove(MouseEvent& e) {
    if (!isPressed()) return false;
    int d = orientation == Orientation::Horizontal ? e.globalPos.x - start_.x : e.globalPos.y - start_.y;
    dragged.emit(d);
    return true;
}

bool SplitterHandle::mouseRelease(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    released.emit();
    update();
    return true;
}

bool SplitterHandle::mouseDoubleClick(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    doubleClicked.emit();
    return true;
}

StatusBar::StatusBar(Widget* parent) : Widget(parent) { vPolicy = SizePolicy::Fixed; }

StatusBar::~StatusBar() {
    if (timer_ >= 0) App::get().cancelTimer(timer_);
}

void StatusBar::showMessage(const std::string& text, int ms) {
    if (timer_ >= 0) {
        App::get().cancelTimer(timer_);
        timer_ = -1;
    }
    text_ = text;
    update();
    if (ms > 0) timer_ = App::get().singleShot(ms, [this] {
        timer_ = -1;
        clearMessage();
    });
}

void StatusBar::clearMessage() {
    text_.clear();
    update();
}

Point StatusBar::sizeHint() const {
    Point h(100, style::textHeight(theme::uiFont()) + 9);
    if (layout()) h = Point(std::max(h.x, layout()->sizeHint().x), std::max(h.y, layout()->sizeHint().y + 1));
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void StatusBar::setPermanentText(const std::string& text, const std::string& tip) {
    permanent_ = text;
    permanentTip_ = tip;
    update();
}

int StatusBar::permanentW() const {
    return permanent_.empty() ? 0 : style::textWidth(theme::uiFont(), permanent_) + 12;
}

std::string StatusBar::toolTipAt(Point p) const {
    if (!permanent_.empty() && p.x >= width() - permanentW()) return permanentTip_;
    return toolTip();
}

void StatusBar::paint(Painter& p) {
    p.fillRect(Rect(0, 0, width(), 1), theme::shades().line);
    int pw = permanentW();
    if (!text_.empty())
        style::text(p, Rect(6, 1, width() - 12 - pw, height() - 1), AlignLeft | AlignVCenter, text_, theme::uiFont(),
                    theme::c("window_text"), true);
    if (pw)
        style::text(p, Rect(width() - pw, 1, pw - 6, height() - 1), AlignRight | AlignVCenter, permanent_,
                    theme::uiFont(), theme::c("window_text"));
}

// ============================================================================================
// TabWidget
// ============================================================================================

TabWidget::TabWidget(Widget* parent) : Widget(parent) { setMouseTracking(true); }

int TabWidget::barHeight() const { return style::textHeight(theme::uiFont()) + 12 + 2; }

Widget* TabWidget::addTab(std::unique_ptr<Widget> page, const std::string& title) {
    Widget* raw = page.get();
    adopt(std::move(page));
    pages_.push_back(raw);
    titles_.push_back(title);
    raw->setVisible(false);
    if (current_ < 0) {
        current_ = 0;
        raw->setVisible(true);
    }
    resized();
    updateGeometry();
    update();
    return raw;
}

void TabWidget::setCurrentIndex(int i) {
    if (i < 0 || i >= count() || i == current_) return;
    if (current_ >= 0 && current_ < count()) pages_[current_]->setVisible(false);
    current_ = i;
    pages_[i]->setVisible(true);
    resized();
    update();
    currentChanged.emit(i);
}

Rect TabWidget::tabRect(int i) const {
    Font f = theme::uiFont();
    int x = 0;
    for (int k = 0; k < count(); ++k) {
        int w = style::textWidth(f, titles_[k]) + 24;
        if (k == i) return Rect(x, 0, w, barHeight());
        x += w;
    }
    return Rect();
}

int TabWidget::tabAt(Point p) const {
    for (int i = 0; i < count(); ++i)
        if (tabRect(i).contains(p)) return i;
    return -1;
}

void TabWidget::resized() {
    int bh = barHeight();
    for (int i = 0; i < count(); ++i)
        if (i == current_) pages_[i]->setGeometry(0, bh, width(), std::max(0, height() - bh));
}

Point TabWidget::sizeHint() const {
    Point h(0, 0);
    for (Widget* p : pages_) {
        Point s = p->sizeHint();
        h.x = std::max(h.x, s.x);
        h.y = std::max(h.y, s.y);
    }
    int tw = 0;
    Font f = theme::uiFont();
    for (auto& t : titles_) tw += style::textWidth(f, t) + 24;
    h.x = std::max(h.x, tw);
    h.y += barHeight();
    return h;
}

Point TabWidget::minimumSizeHint() const {
    Point h(0, 0);
    for (Widget* p : pages_) {
        Point s = p->minimumSizeHint(), m = p->minimumSize();
        h.x = std::max(h.x, std::max(s.x, m.x));
        h.y = std::max(h.y, std::max(s.y, m.y));
    }
    h.y += barHeight();
    return h;
}

void TabWidget::paint(Painter& p) {
    const theme::Shades& sh = theme::shades();
    int bh = barHeight();
    p.fillRect(Rect(0, bh - 1, width(), 1), sh.line);
    Font f = theme::uiFont();
    for (int i = 0; i < count(); ++i) {
        Rect r = tabRect(i);
        bool sel = i == current_, hov = i == hoverTab_ && isEnabled();
        if (hov) p.fillRect(r, sh.hover);
        Color tc = !isEnabled() ? theme::c("disabled_text") : (sel || hov) ? theme::c("window_text") : theme::c("hint_text");
        style::text(p, Rect(r.x + 12, r.y + 6, r.w - 24, r.h - 14), AlignCenter, titles_[i], f, tc);
        if (sel) p.fillRect(Rect(r.x, bh - 2, r.w, 2), theme::c("accent"));
    }
}

bool TabWidget::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    int i = tabAt(e.pos);
    if (i < 0) return false;
    setCurrentIndex(i);
    return true;
}

bool TabWidget::mouseMove(MouseEvent& e) {
    int i = tabAt(e.pos);
    if (i != hoverTab_) {
        hoverTab_ = i;
        update(Rect(0, 0, width(), barHeight()));
    }
    return i >= 0;
}

}  // namespace ui
