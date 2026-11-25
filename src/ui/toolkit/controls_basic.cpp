// Label, Button, CheckBox, GroupBox, Separator, SplitterHandle, StatusBar, TabWidget — styled by the skin's
// stylesheets (default.css: Buttons, and the Basic controls section).
#include <algorithm>
#include <cmath>
#include <cstdio>

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

}  // namespace

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

namespace {
int linesHeight(const Font& f, int n) {
    if (n <= 0) return 0;
    return (int)std::ceil((n - 1) * f.lineSpacing() + f.ascent() + f.descent());
}
}  // namespace

// ============================================================================================
// Label
// ============================================================================================

Label::Label(const std::string& text, Widget* parent) : Widget(parent), text_(text) { setStyleType("Label"); }

void Label::setText(const std::string& t) {
    if (t == text_) return;
    text_ = t;
    updateGeometry();
    update();
}

// (default.css: .dimmed 8 pt, #time and #lufs in the numbers font)
Font Label::font() const {
    if (hasFont_) return style::smoothing(font_, computedStyle());  // (setFont's, smoothed as the stylesheets say)
    return style::font(computedStyle(), theme::uiFont());
}

// (default.css: :disabled, then :link, .hint, .dimmed, else the window's text colour)
Color Label::color() const {
    if (hasColor_) return color_;
    return style::foreground(computedStyle());
}

void Label::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    if (!link_.empty()) out.push_back("link");
}

Point Label::sizeHint() const {
    Point h(0, 0);
    if (!image_.empty()) h = Point(image_.width(), image_.height());
    if (!text_.empty()) {
        Font f = font();
        std::vector<std::string> lines = splitLines(text_);
        float w = 0;
        for (auto& l : lines) w = std::max(w, f.width(l));
        float wrapW = (float)theme::metric("label-wrap-width", 360);  // (a wrapped text's widest line)
        int n = (int)lines.size();
        if (wrap_ && w > wrapW) {
            lines = wrapLines(text_, f, wrapW);
            w = 0;
            for (auto& l : lines) w = std::max(w, f.width(l));
            // (laid out wider than that: its lines at its width, fewer, so no empty line under it; narrower, more:
            // minimumSizeHint's)
            n = width() > wrapW ? wrappedLines(width()) : (int)lines.size();
        }
        h.x = std::max(h.x, (int)std::ceil(w));
        h.y = std::max(h.y, linesHeight(f, n));
    }
    style::Insets pad = style::insets(computedStyle(), "padding");
    h.x += 2 * margin + pad.w();
    h.y += 2 * margin + pad.h();
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point Label::minimumSizeHint() const {
    Point h = sizeHint();
    style::Insets pad = style::insets(computedStyle(), "padding");
    if (wrap_ || elide_) h.x = std::min(h.x, theme::metric("label-min-width", 16) + 2 * margin + pad.w());
    if (wrap_ && !text_.empty() && width() > 0)
        h.y = std::max(h.y, linesHeight(font(), wrappedLines(width())) + pad.h() + 2 * margin);
    return h;
}

int Label::wrappedLines(int width) const {
    float room = (float)(width - style::insets(computedStyle(), "padding").w() - 2 * margin);
    return room > 0 ? (int)wrapLines(text_, font(), room).size() : 1;
}

void Label::resized() {
    if (!wrap_ || text_.empty()) return;
    int n = wrappedLines(width());
    if (n != lines_) {
        lines_ = n;
        updateGeometry();
    }
}

void Label::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Rect r = style::inside(rect().adjusted(margin, margin, -margin, -margin), style::insets(st, "padding"));
    bool underline = st.keyword("text-decoration", "none").find("underline") != std::string::npos;
    if (!image_.empty()) {
        int x = r.x, y = r.y;
        if (align_ & AlignHCenter) x += (r.w - image_.width()) / 2;
        else if (align_ & AlignRight) x += r.w - image_.width();
        if (align_ & AlignVCenter) y += (r.h - image_.height()) / 2;
        else if (align_ & AlignBottom) y += r.h - image_.height();
        float opacity = (float)computedStyle("image").number("opacity", 1);  // (default.css: 0.45 when disabled)
        if (opacity < 1) {
            p.save();
            p.setOpacity(std::max(0.f, opacity));
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
    const std::vector<css::Style::Shadow>& shadows = style::boxLook(st).textShadows;  // (text-shadow)
    if (multi || wrap_) {
        if (!shadows.empty()) style::textBlockShadows(p, RectF(r), align_, text_, f, shadows, wrap_);
        p.drawTextBlock(RectF(r), align_, text_, f, c, wrap_);
        return;
    }
    if (!shadows.empty()) style::textShadows(p, RectF(r), align_, text_, f, shadows, elide_);
    p.drawText(RectF(r), align_, text_, f, c, elide_);
    if (underline) {  // (default.css: a link's)
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
    setStyleType("Button");
    setStyleType("PushButton");
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
    if (s == Style::Tool) replaceStyleType("PushButton", "ToolButton");
    else replaceStyleType("ToolButton", "PushButton");
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

Font Button::font() const { return style::font(computedStyle(), hasFont_ ? font_ : theme::uiFont()); }

bool Button::lit() const {
    if (!isEnabled()) return false;
    if (property("lit") == "true") return true;
    if (checked_ && property("accent") == "true") return true;
    if (style_ == Style::Tool && checked_ && property("autoRaise") == "true") return true;
    if (style_ == Style::Tool && checked_ && !property("toggle").empty()) return true;
    if (style_ == Style::Push && default_) return true;
    return false;
}

void Button::styleStates(std::vector<std::string>& out) const {
    bool enabled = isEnabled();
    if (!enabled) out.push_back("disabled");
    if (hasFocus()) out.push_back("focus");
    if (enabled && underMouse() && (!down_ || inside_)) out.push_back("hover");
    if ((down_ && inside_) || menuArrowDown_) out.push_back("pressed");
    if (checked_) out.push_back("checked");
    if (default_) out.push_back("default");
    if (lit()) {
        out.push_back("lit");
        if (style_ == Style::Tool && checked_ && !property("toggle").empty() && property("lit") != "true" &&
            property("accent") != "true")
            out.push_back("toggled");
    }
    if (iconPx_ > 0 && (!iconSvg_.empty() || !icon_.empty())) out.push_back("icon");
    if (enabled && menu_ && split_ && (hoverArrow_ || menuArrowDown_)) out.push_back("arrow-hover");
}

Color Button::fillColor() const { return style::background(computedStyle()); }

Color Button::textColor() const { return style::foreground(computedStyle()); }

Point Button::sizeHint() const {
    // its content (text, icon) inside its padding and border (default.css: PushButton 4px 12px + 1px, ToolButton
    // 3px 5px + 1px, with an icon 3px + 1px; autoRaise 1px and 2px more)
    Font f = font();
    const css::Style& st = computedStyle();
    style::Insets pad = style::insets(st, "padding"), border = style::insets(st, "border-width");
    int tw = text_.empty() ? 0 : style::textWidth(f, text_);
    int th = style::textHeight(f);
    bool hasIcon = iconPx_ > 0 && (!iconSvg_.empty() || !icon_.empty());
    Point h;
    if (style_ == Style::Push) {
        int cw = tw + (hasIcon ? iconPx_ + (text_.empty() ? 0 : 4) : 0);
        int ch = std::max(16, std::max(th, hasIcon ? iconPx_ : 0));
        h = Point(cw + pad.w() + border.w(), ch + pad.h() + border.h());
        if (menu_ && !split_) h.x += 12;
    } else {
        int cw = hasIcon ? iconPx_ : tw;
        int ch = hasIcon ? iconPx_ : th;
        h = Point(cw + pad.w() + border.w(), ch + pad.h() + border.h());
        if (menu_ && split_) h.x += 13;
        else if (menu_) h.x += 6;
    }
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void Button::paint(Painter& p) {
    const css::Style& st = computedStyle();
    Rect r = rect();
    float rad = style::radius(st);
    style::box(p, r, st);
    Color tc = style::foreground(st);
    // (its content: inside the border and padding; a tool button's centred in the whole of it, as before)
    bool autoRaise = style_ == Style::Tool && property("autoRaise") == "true";
    Rect content = style_ == Style::Push ? style::contents(r, st)
                   : autoRaise           ? r.adjusted(5, 4, -5, -4)
                                         : r.adjusted(3, 3, -3, -3);
    // the menu parts
    if (menu_ && split_) {
        Rect ar(r.right() - 13, 0, 13, r.h);
        Color arrowFill = style::background(computedStyle("arrow"));
        if (arrowFill.a) {
            p.save();
            p.setClip(ar);
            style::fill(p, r.adjusted(0, 0, 0, 0), arrowFill, rad);
            p.restore();
        }
        p.fillRect(Rect(ar.x, 1, 1, r.h - 2), style::background(computedStyle("separator")));
        if (!style::glyph(p, RectF((float)ar.x + 1, 0, 12, (float)r.h), computedStyle("arrow")))
            style::icon(p, ar.x + 1 + (12 - 8) / 2.f, (r.h - 8) / 2.f, 8, "down", tc);
        content.w = std::max(0, ar.x - content.x - (autoRaise ? 4 : 2));
        if (!autoRaise) content.x = std::max(content.x, 3);
    } else if (menu_ && style_ == Style::Push) {
        if (!style::glyph(p, RectF((float)(r.right() - 19), 0, 10, (float)r.h), computedStyle("arrow")))
            style::icon(p, (float)(r.right() - 19), (r.h - 10) / 2.f, 10, "down", tc);
        content.w -= 10;
    } else if (menu_) {
        if (!style::glyph(p, RectF((float)(r.right() - 9), (float)(r.h - 9), 6, 6), computedStyle("arrow")))
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
        style::text(p, tr, AlignLeft | AlignVCenter, text_, f, tc, st, true);
        return;
    }
    if (!text_.empty()) {
        // (centred over the whole height, as the padding is symmetric)
        Rect tr(content.x - 2, r.y, content.w + 4, r.h);
        // (one glyph, an icon such as ✕: drawn whole over the padding rather than made a "…" as wide)
        size_t glyphs = std::count_if(text_.begin(), text_.end(), [](char c) { return ((unsigned char)c & 0xC0) != 0x80; });
        style::text(p, tr, AlignCenter, text_, f, tc, st, glyphs > 1);
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
    if (plain && (e.key == plat::Key_Return || e.key == plat::Key_Enter)) {  // (a tool button too: the keyboard alone)
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
    setStyleType("CheckBox");
    setFocusPolicy(FocusPolicy::Strong);
    vPolicy = SizePolicy::Fixed;
}

void CheckBox::setChecked(bool on) {
    if (checked_ == on) return;
    checked_ = on;
    update();
    toggled.emit(on);
}

void CheckBox::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    if (checked_) out.push_back("checked");
}

Point CheckBox::sizeHint() const {
    // (default.css: the 14 px box, 6 px from the text, 2 px above and below; the text's ::label box (its padding
    // and border) around it)
    const css::Style& st = computedStyle();
    Font f = style::font(st, theme::uiFont());
    style::Insets pad = style::insets(st, "padding");
    const css::Style& ind = computedStyle("indicator");
    int iw = style::px(ind, "width", 14), ih = style::px(ind, "height", 14), gap = style::px(ind, "margin-right", 6);
    style::Insets lab = text_.empty() ? style::Insets() : style::contentInsets(computedStyle("label"));
    int tw = text_.empty() ? 0 : style::textWidth(f, text_) + gap + lab.w();
    Point h(pad.w() + iw + tw, std::max(style::textHeight(f) + lab.h(), ih) + pad.h());
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void CheckBox::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Font f = style::font(st, theme::uiFont());
    style::Insets pad = style::insets(st, "padding");
    // the box (centred over the whole height), its check mark inside its border and padding
    const css::Style& is = computedStyle("indicator");
    int iw = style::px(is, "width", 14), ih = style::px(is, "height", 14), gap = style::px(is, "margin-right", 6);
    Rect ind(pad.left, (height() - ih) / 2, iw, ih);
    style::box(p, ind, is);
    if (checked_) {
        Rect mark = style::contents(ind, is);
        if (!style::glyph(p, RectF(mark), is))
            style::icon(p, (float)mark.x, (float)mark.y, (float)mark.w, "check", style::foreground(is));
    }
    if (!text_.empty()) {
        // ::label: a box shrink-wrapped around the text (its padding and border; centred in the height), and its
        // outline (CheckBox::label:focus { outline: 1px dotted }: a focus rectangle); the text centred in the height
        const css::Style& ls = computedStyle("label");
        style::Insets lab = style::contentInsets(ls);
        int x = pad.left + iw + gap;
        int avail = std::max(0, width() - x - pad.right);
        const style::BoxLook& look = style::boxLook(ls);
        if (look.more || !look.fillOnly || look.bg.a || ls.has("outline-style")) {
            int bh = style::textHeight(f) + lab.h();
            Rect lr(x, (height() - bh) / 2, std::min(avail, style::textWidth(f, text_) + lab.w()), bh);
            style::box(p, lr, ls);
            paintOutline(p, lr, ls);
        }
        Rect tr(x + lab.left, 0, std::max(0, avail - lab.w()), height());
        style::text(p, tr, AlignLeft | AlignVCenter, text_, f, style::foreground(ls),
                    ls.has("text-shadow") ? ls : st, true);
    }
}

// CSS's outline: a line around r, outside it (outline-offset farther out; negative: in), outline-width thick, of
// outline-style (solid, dotted, dashed, double, groove, ridge, inset, outset: drawn as a border of that style is),
// in outline-color (else the style's color). Square corners; nothing when the style is none (the default).
void paintOutline(Painter& p, const Rect& r, const css::Style& s) {
    std::string kind = s.keyword("outline-style", "none");
    if (kind == "none" || kind == "hidden") return;
    if (kind == "auto") kind = "solid";
    int w = std::max(0, style::px(s, "outline-width", 3));  // (medium, as CSS's)
    Color c = style::color(s, "outline-color", style::foreground(s));
    if (w == 0 || c.a == 0) return;
    int out = style::px(s, "outline-offset", 0) + w;
    Rect o = r.adjusted(-out, -out, out, out);
    if (o.w < 2 * w || o.h < 2 * w) return;
    // (drawn as a box with that border alone)
    css::Style b;
    char colour[16];
    std::snprintf(colour, sizeof colour, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    for (const char* side : {"top", "right", "bottom", "left"}) {
        std::string pre = std::string("border-") + side;
        b.props_[pre + "-width"] = std::to_string(w) + "px";
        b.props_[pre + "-style"] = kind;
        b.props_[pre + "-color"] = colour;
    }
    style::box(p, o, b);
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

GroupBox::GroupBox(const std::string& title, Widget* parent) : Widget(parent), title_(title) { setStyleType("GroupBox"); }

namespace {
// (default.css: the box starts 20 px down, its contents 8 px below that and 6 px in from its other sides)
Rect groupBoxRect(const Rect& r, const css::Style& st) { return style::inside(r, style::insets(st, "margin")); }
// whether the title is drawn as a box of its own: it has a background (a colour, a gradient, a shadow), a
// border or padding
bool titleIsBox(const css::Style& ts) {
    const style::BoxLook& b = style::boxLook(ts);
    style::Insets pad = style::insets(ts, "padding");
    return b.bg.a != 0 || b.more || b.border.w() + b.border.h() > 0 || pad.w() + pad.h() > 0;
}
// Where the title goes, placed against the frame (the box: `frame`). Its row: `height` tall (default 18), its top
// margin-top below the frame's top (negative: above it; default.css's -19px puts it in the 20 px above the
// frame), from margin-left to margin-right in from the frame's sides. The text is centred in the row; a title
// that is a box (titleIsBox) is shrink-wrapped around its text (its padding and border around it, no wider than
// the row) and centred in the row as the text is. `bottom`: the lowest it reaches, with its margin-bottom.
struct GroupTitle {
    Rect row, box, text;
    bool isBox = false;
    int bottom = 0;
};
GroupTitle groupTitle(const Rect& frame, const css::Style& ts, const std::string& title) {
    GroupTitle t;
    style::Insets m = style::insets(ts, "margin");
    t.row = Rect(frame.x + m.left, frame.y + m.top, frame.w - m.w(), style::px(ts, "height", 18));
    t.text = t.row;
    t.isBox = titleIsBox(ts);
    if (t.isBox) {
        Font f = style::font(ts, theme::uiFont());
        style::Insets pad = style::insets(ts, "padding"), b = style::insets(ts, "border-width");
        int w = std::min(style::textWidth(f, title) + pad.w() + b.w(), std::max(0, t.row.w));
        int h = style::textHeight(f) + pad.h() + b.h();
        t.box = Rect(t.row.x, t.row.y + (t.row.h - h) / 2, w, h);
        t.text = style::contents(t.box, ts);
    }
    t.bottom = std::max(t.row.bottom(), t.isBox ? t.box.bottom() : t.row.bottom()) + m.bottom;
    return t;
}
// how far the title reaches below the frame's top border (0: not that far): the contents start that much lower,
// as a fieldset's legend widens its top border
int titleOverhang(const css::Style& st, const css::Style& ts, const std::string& title) {
    if (title.empty()) return 0;
    GroupTitle t = groupTitle(Rect(0, 0, 1 << 16, 0), ts, title);
    return std::max(0, t.bottom - style::insets(st, "border-width").top);
}
// the room the box takes around its contents: margin, border and padding, and the title's overhang
Point groupBoxExtra(const css::Style& st, const css::Style& ts, const std::string& title) {
    style::Insets m = style::insets(st, "margin"), b = style::insets(st, "border-width"), pad = style::insets(st, "padding");
    return Point(m.w() + b.w() + pad.w(), m.h() + b.h() + pad.h() + titleOverhang(st, ts, title));
}
// the room the title takes across: its text, its margins and the box's (and its padding and border when it is a box)
int groupTitleWidth(const css::Style& st, const css::Style& ts, const std::string& title) {
    int w = style::textWidth(style::font(ts, theme::uiFont()), title) + style::insets(ts, "margin").w() +
            style::insets(st, "margin").w();
    if (titleIsBox(ts)) w += style::insets(ts, "padding").w() + style::insets(ts, "border-width").w();
    return w;
}
}  // namespace

Rect GroupBox::contentsRect() const {
    const css::Style& st = computedStyle();
    Rect cr = style::contents(groupBoxRect(rect(), st), st);
    int over = title_.empty() ? 0 : titleOverhang(st, computedStyle("title"), title_);
    return cr.adjusted(0, over, 0, 0);
}

Point GroupBox::sizeHint() const {
    Point h = Widget::sizeHint();
    const css::Style& st = computedStyle();
    const css::Style& ts = computedStyle("title");
    if (!layout()) {
        Point extra = groupBoxExtra(st, ts, title_);
        h = Point(std::max(h.x, extra.x), std::max(h.y, extra.y));
    }
    if (!title_.empty()) h.x = std::max(h.x, groupTitleWidth(st, ts, title_));
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point GroupBox::minimumSizeHint() const {
    Point h = Widget::minimumSizeHint();
    const css::Style& st = computedStyle();
    const css::Style& ts = computedStyle("title");
    if (!layout()) h = groupBoxExtra(st, ts, title_);
    if (!title_.empty()) h.x = std::max(h.x, groupTitleWidth(st, ts, title_));
    return h;
}

void GroupBox::paint(Painter& p) {
    const css::Style& st = computedStyle();
    Rect frame = groupBoxRect(rect(), st);
    style::box(p, frame, st);
    if (!title_.empty()) {
        const css::Style& ts = computedStyle("title");
        GroupTitle t = groupTitle(frame, ts, title_);
        if (t.isBox) style::box(p, t.box, ts);  // (over the frame: across its line, it hides it)
        style::text(p, t.text, AlignLeft | AlignVCenter, title_, style::font(ts, theme::uiFont()), style::foreground(ts),
                    ts, true);
    }
}

// ============================================================================================
// Separator, SplitterHandle, StatusBar
// ============================================================================================

Separator::Separator(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    setStyleType("Separator");
    if (o == Orientation::Vertical) {
        hPolicy = SizePolicy::Fixed;
        vPolicy = SizePolicy::Preferred;
    } else {
        hPolicy = SizePolicy::Preferred;
        vPolicy = SizePolicy::Fixed;
    }
}

void Separator::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    out.push_back(orientation == Orientation::Vertical ? "vertical" : "horizontal");
}

namespace {
// a line's thickness (line-width: Separator, SplitterHandle)
int lineWidth(const css::Style& st) { return std::max(0, style::px(st, "line-width", 1)); }
// A separator whose style has a border is a box across its thickness (an etched line: two borders of their own
// colours): `width` thick when vertical, `height` when horizontal (else its borders across it); 0 when it has no
// border (a line of `color`, line-width thick)
int separatorBox(const css::Style& st, bool vertical) {
    style::Insets b = style::insets(st, "border-width");
    if (b.w() + b.h() == 0) return 0;
    return std::max(1, style::px(st, vertical ? "width" : "height", vertical ? b.w() : b.h()));
}
}  // namespace

Point Separator::sizeHint() const {
    const css::Style& st = computedStyle();
    style::Insets m = style::insets(st, "margin");
    int lw = lineWidth(st);
    if (int box = separatorBox(st, orientation == Orientation::Vertical)) lw = box;
    Point h = orientation == Orientation::Vertical ? Point(lw + m.w(), 1 + m.h()) : Point(1 + m.w(), lw + m.h());
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void Separator::paint(Painter& p) {
    const css::Style& st = computedStyle();
    Color c = style::foreground(st);
    style::Insets m = style::insets(st, "margin");
    int lw = lineWidth(st);
    bool vertical = orientation == Orientation::Vertical;
    if (int across = separatorBox(st, vertical)) {  // (a box, along the middle of the room inside its margin)
        Rect r = vertical ? Rect(m.left + (width() - m.w()) / 2 - across / 2, m.top, across, height() - m.h())
                          : Rect(m.left, m.top + (height() - m.h()) / 2 - across / 2, width() - m.w(), across);
        if (!r.empty()) style::box(p, r, st);
        return;
    }
    // (along the middle of the room inside its margin)
    if (vertical) {
        int x = m.left + (width() - m.w()) / 2 - lw / 2;
        for (int k = 0; k < lw; ++k) p.drawVLine(x + k, m.top, height() - m.bottom, c);
    } else {
        int y = m.top + (height() - m.h()) / 2 - lw / 2;
        for (int k = 0; k < lw; ++k) p.drawHLine(m.left, width() - m.right, y + k, c);
    }
}

SplitterHandle::SplitterHandle(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    setStyleType("SplitterHandle");
    setCursor(o == Orientation::Horizontal ? Cursor::SizeHor : Cursor::SizeVer);
    if (o == Orientation::Horizontal) hPolicy = SizePolicy::Fixed;
    else vPolicy = SizePolicy::Fixed;
}

void SplitterHandle::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    out.push_back(orientation == Orientation::Horizontal ? "horizontal" : "vertical");
}

Point SplitterHandle::sizeHint() const {
    const css::Style& st = computedStyle();
    Point h = orientation == Orientation::Horizontal ? Point(style::px(st, "width", 5), 0) : Point(0, style::px(st, "height", 5));
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void SplitterHandle::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Color c = style::foreground(st);  // (default.css: the accent colour under the pointer and while dragged)
    int lw = lineWidth(st);
    if (orientation == Orientation::Horizontal) p.fillRect(Rect(width() / 2 - lw / 2, 0, lw, height()), c);
    else p.fillRect(Rect(0, height() / 2 - lw / 2, width(), lw), c);
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

StatusBar::StatusBar(Widget* parent) : Widget(parent) {
    setStyleType("StatusBar");
    vPolicy = SizePolicy::Fixed;
}

StatusBar::~StatusBar() {
    if (timer_ >= 0) App::get().cancelTimer(timer_);
    if (hintSlot_ >= 0) App::get().hintChanged.disconnect(hintSlot_);
}

void StatusBar::followHints(bool on) {
    if (on == followsHints()) return;
    if (on) {
        hintSlot_ = App::get().hintChanged.connect([this](Window* w, const std::string& text) {
            if (w && w == window()) setHint(text);
        });
    } else {
        App::get().hintChanged.disconnect(hintSlot_);
        hintSlot_ = -1;
        setHint("");
    }
}

void StatusBar::setHint(const std::string& text) {
    if (text == hint_) return;
    hint_ = text;
    update();
}

std::string StatusBar::shownText() const {
    if (timer_ >= 0 || hint_.empty()) return text_;  // (a timed message wins over a hint)
    return hint_;
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
    // (default.css: a 1 px line, then the text with 4 px above and below)
    const css::Style& st = computedStyle();
    Font f = style::font(st, theme::uiFont());
    style::Insets pad = style::insets(st, "padding"), b = style::insets(st, "border-width");
    int line = std::max(0, style::px(computedStyle("separator"), "height", 1));
    Point h(100, style::textHeight(f) + line + pad.h() + b.h());
    if (layout()) h = Point(std::max(h.x, layout()->sizeHint().x), std::max(h.y, layout()->sizeHint().y + line));
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
    if (permanent_.empty()) return 0;
    const css::Style& ps = computedStyle("permanent");  // (default.css: 6 px on each side of it)
    return style::textWidth(style::font(ps, theme::uiFont()), permanent_) + style::insets(ps, "padding").w();
}

std::string StatusBar::toolTipAt(Point p) const {
    if (!permanent_.empty() && p.x >= width() - permanentW()) return permanentTip_;
    return toolTip();
}

void StatusBar::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Rect content = style::contents(rect(), st);
    Font f = style::font(st, theme::uiFont());
    Color tc = style::foreground(st);
    // the line along its top; the texts are centred over the height below it
    const css::Style& ls = computedStyle("separator");
    int line = std::max(0, style::px(ls, "height", 1));
    style::box(p, Rect(0, 0, width(), line), ls);
    int pw = permanentW();
    std::string shown = shownText();
    if (!shown.empty())
        style::text(p, Rect(content.x, line, content.w - pw, height() - line), AlignLeft | AlignVCenter, shown, f, tc, st, true);
    if (pw) {
        const css::Style& ps = computedStyle("permanent");
        style::text(p, Rect(width() - pw, line, pw - style::insets(ps, "padding").right, height() - line),
                    AlignRight | AlignVCenter, permanent_, style::font(ps, theme::uiFont()), style::foreground(ps), ps);
    }
}

// ============================================================================================
// TabWidget
// ============================================================================================

TabWidget::TabWidget(Widget* parent) : Widget(parent) {
    setStyleType("TabWidget");
    setMouseTracking(true);
}

void TabWidget::styleStates(std::vector<std::string>& out) const {
    if (styledTab_ == -1) {
        Widget::styleStates(out);
        return;
    }
    // a tab's: under the pointer, the current one, the first and the last of the row
    bool enabled = isEnabled();
    if (!enabled) out.push_back("disabled");
    if (styledTab_ >= 0 && styledTab_ == hoverTab_ && enabled && !styleNoHover_) out.push_back("hover");
    if (styledTab_ >= 0 && styledTab_ == current_) out.push_back("selected");
    if (styledTab_ == 0) out.push_back("first");
    if (styledTab_ >= 0 && styledTab_ == count() - 1) out.push_back("last");
}

const css::Style& TabWidget::tabStyle(int i, const std::string& part, bool noHover) const {
    styledTab_ = i;
    styleNoHover_ = noHover;
    const css::Style& st = computedStyle(part);
    styledTab_ = -1;
    styleNoHover_ = false;
    return st;
}

// Where the tabs are (out[i]: tab i's box) and the bar's height. Each tab is its title's width and height inside its
// padding and border (in its own states), placed after the one before and its margin, its bottom margin-bottom above
// the bar's bottom; the bar is as high as the highest tab with its margin-top and margin-bottom (as if none were
// under the pointer: hovering doesn't move the page). A negative margin makes a tab overlap its neighbours or the
// pane below (the selected tab is drawn last). (default.css: the title 6 px below the top, 12 px from the sides,
// 6 px above a 2 px line; no margins)
int TabWidget::tabGeometry(std::vector<Rect>* out) const {
    int n = count();
    if (n == 0) {
        const css::Style& st = tabStyle(-2, "tab");
        return style::textHeight(style::font(st, theme::uiFont())) + style::insets(st, "padding").h() +
               style::insets(st, "border-width").h();
    }
    struct Box {
        int w, h;
        style::Insets m;
    };
    std::vector<Box> boxes((size_t)n);
    int bar = 0;
    for (int i = 0; i < n; ++i) {
        auto measure = [&](bool noHover) {
            const css::Style& st = tabStyle(i, "tab", noHover);
            Font f = style::font(st, theme::uiFont());
            style::Insets pad = style::insets(st, "padding"), border = style::insets(st, "border-width");
            return Box{style::textWidth(f, titles_[(size_t)i]) + pad.w() + border.w(),
                       style::textHeight(f) + pad.h() + border.h(), style::insets(st, "margin")};
        };
        Box b = measure(false);
        boxes[(size_t)i] = b;
        if (i == hoverTab_) b = measure(true);
        bar = std::max(bar, b.m.top + b.h + b.m.bottom);
    }
    if (out) {
        out->assign((size_t)n, Rect());
        int x = 0;
        for (int i = 0; i < n; ++i) {
            const Box& b = boxes[(size_t)i];
            x += b.m.left;
            (*out)[(size_t)i] = Rect(x, bar - b.m.bottom - b.h, b.w, b.h);
            x += b.w + b.m.right;
        }
    }
    return bar;
}

int TabWidget::barHeight() const { return tabGeometry(nullptr); }

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
    if (i < 0 || i >= count()) return Rect();
    std::vector<Rect> r;
    tabGeometry(&r);
    return r[(size_t)i];
}

// (the selected tab first: it is drawn over its neighbours)
int TabWidget::tabAt(Point p) const {
    std::vector<Rect> r;
    tabGeometry(&r);
    if (current_ >= 0 && current_ < count() && r[(size_t)current_].contains(p)) return current_;
    for (int i = 0; i < count(); ++i)
        if (r[(size_t)i].contains(p)) return i;
    return -1;
}

// the bar and every tab (a tab may reach beyond the bar: a negative margin)
Rect TabWidget::barArea() const {
    std::vector<Rect> r;
    Rect a(0, 0, width(), tabGeometry(&r));
    for (const Rect& t : r) a = a.united(t);
    return a;
}

// the pane: the box under the tabs, inside its margin (default.css: none, so all of the room under the tabs)
Rect TabWidget::paneRect() const {
    int bh = barHeight();
    return style::inside(Rect(0, bh, width(), std::max(0, height() - bh)), style::insets(computedStyle("pane"), "margin"));
}

namespace {
// the room the pane takes around the page: its margin, border and padding
Point paneExtra(const css::Style& ps) {
    style::Insets m = style::insets(ps, "margin"), b = style::insets(ps, "border-width"), pad = style::insets(ps, "padding");
    return Point(m.w() + b.w() + pad.w(), m.h() + b.h() + pad.h());
}
}  // namespace

void TabWidget::resized() {
    if (current_ < 0 || current_ >= count()) return;
    // (the current page: inside the pane's border and padding)
    Rect page = style::contents(paneRect(), computedStyle("pane"));
    pages_[current_]->setGeometry(page.x, page.y, std::max(0, page.w), std::max(0, page.h));
}

Point TabWidget::sizeHint() const {
    Point h(0, 0);
    for (Widget* p : pages_) {
        Point s = p->sizeHint();
        h.x = std::max(h.x, s.x);
        h.y = std::max(h.y, s.y);
    }
    // (the row of tabs: as far as the last one reaches)
    std::vector<Rect> r;
    int bar = tabGeometry(&r);
    int tw = 0;
    for (int i = 0; i < count(); ++i) tw = std::max(tw, r[(size_t)i].right() + style::px(tabStyle(i, "tab"), "margin-right"));
    Point pane = paneExtra(computedStyle("pane"));
    h.x = std::max(h.x + pane.x, tw);
    h.y += bar + pane.y;
    return h;
}

Point TabWidget::minimumSizeHint() const {
    Point h(0, 0);
    for (Widget* p : pages_) {
        Point s = p->minimumSizeHint(), m = p->minimumSize();
        h.x = std::max(h.x, std::max(s.x, m.x));
        h.y = std::max(h.y, std::max(s.y, m.y));
    }
    Point pane = paneExtra(computedStyle("pane"));
    h.x += pane.x;
    h.y += barHeight() + pane.y;
    return h;
}

void TabWidget::paint(Painter& p) {
    style::box(p, rect(), computedStyle());
    style::box(p, paneRect(), computedStyle("pane"));  // (under the tabs, around the page)
    int bh = barHeight();
    {  // the line under the tabs (they are drawn over it)
        const css::Style& bs = computedStyle("base");
        int lh = std::max(0, style::px(bs, "height", 1));
        style::box(p, Rect(0, bh - lh, width(), lh), bs);
    }
    // the tabs, the selected one last (over its neighbours, where a negative margin takes it)
    std::vector<Rect> rects;
    tabGeometry(&rects);
    auto paintTab = [&](int i) {
        Rect r = rects[(size_t)i];
        // (each style is used before the next is asked for)
        const css::Style& ts = tabStyle(i, "tab");
        style::box(p, r, ts);
        style::text(p, style::contents(r, ts), AlignCenter, titles_[i], style::font(ts, theme::uiFont()),
                    style::foreground(ts), ts);
        const css::Style& ls = tabStyle(i, "tab-line");
        int lh = std::max(0, style::px(ls, "height", 2));
        style::box(p, Rect(r.x, r.bottom() - lh, r.w, lh), ls);
    };
    for (int i = 0; i < count(); ++i)
        if (i != current_) paintTab(i);
    if (current_ >= 0 && current_ < count()) paintTab(current_);
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
        update(barArea());  // (where the tabs were, and are now: a tab's size may change with :hover)
        hoverTab_ = i;
        update(barArea());
    }
    return i >= 0;
}

}  // namespace ui
