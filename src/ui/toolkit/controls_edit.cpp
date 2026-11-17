// LineEdit, SpinBox, ComboBox (and its drop-down list).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/util.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {
bool isWordChar(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

size_t utf8Count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool printable(const KeyEvent& e) {
    if (e.text.empty()) return false;
    unsigned char c0 = (unsigned char)e.text[0];
    if (c0 < 0x20 || c0 == 0x7f) return false;
    bool ctrl = e.mods & plat::ModCtrl, alt = e.mods & plat::ModAlt;
    if (e.mods & plat::ModMeta) return false;
    if (ctrl && !alt) return false;
    if (alt && !ctrl) return false;
    return true;
}

// a style's border and padding together: the room from its edge to its content
style::Insets around(const css::Style& st) {
    style::Insets b = style::insets(st, "border-width"), p = style::insets(st, "padding");
    return style::Insets{b.left + p.left, b.top + p.top, b.right + p.right, b.bottom + p.bottom};
}

// an icon's size (icon-size) and its lines' width (stroke-width; else 1.5 px at 10 px, as style::icon draws)
float iconSize(const css::Style& st, float def) { return (float)st.length("icon-size", def); }
float strokeWidth(const css::Style& st, float size) {
    return st.has("stroke-width") ? (float)std::max(0.0, st.length("stroke-width", 0)) : 1.5f * (size / 10.f);
}

// style::icon's ▴ / ▾ with the stroke width the stylesheets give (the same points)
void arrowIcon(Painter& p, float x, float y, float size, bool up, Color c, float stroke) {
    if (c.a == 0 || stroke <= 0 || size <= 0) return;
    static const float downPts[] = {2, 3.5f, 5, 6.5f, 8, 3.5f};
    static const float upPts[] = {2, 6.5f, 5, 3.5f, 8, 6.5f};
    const float* pts = up ? upPts : downPts;
    float s = size / 10.f;
    std::vector<PointF> v;
    for (int i = 0; i < 3; ++i) v.push_back(PointF(x + pts[i * 2] * s, y + pts[i * 2 + 1] * s));
    p.drawPolyline(v, c, stroke, LineCap::Round, LineJoin::Round);
}
}  // namespace

// ============================================================================================
// LineEdit
// ============================================================================================

LineEdit::LineEdit(const std::string& text, Widget* parent) : Widget(parent), text_(text) {
    setStyleType("LineEdit");
    cursor_ = anchor_ = text_.size();
    setFocusPolicy(FocusPolicy::Strong);
    setCursor(Cursor::IBeam);
    hPolicy = SizePolicy::Expanding;
    vPolicy = SizePolicy::Fixed;
}

LineEdit::~LineEdit() {
    if (blinkTimer_ >= 0) App::get().cancelTimer(blinkTimer_);
}

// (the program's own font, else the stylesheets')
Font LineEdit::font() const { return hasFont_ ? font_ : style::font(computedStyle(), theme::uiFont()); }

void LineEdit::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    if (!frame) out.push_back("frameless");
    if (readOnly_) out.push_back("read-only");
    if (clearHover_) out.push_back("clear-hover");
}

// the clear button: ::clear-button's width, its margin-right inside the border, the whole height
// (default.css: 16 px, 2 px from the border)
Rect LineEdit::clearRect() const {
    int right = width() - style::insets(computedStyle(), "border-width").right;
    const css::Style& cb = computedStyle("clear-button");
    int w = std::max(0, style::px(cb, "width", 16));
    return Rect(right - style::px(cb, "margin-right") - w, 0, w, height());
}

// the text's room: inside the border and padding (left and right; the text is centred in the whole height),
// up to the clear button's margin-left when it shows
Rect LineEdit::textArea() const {
    style::Insets a = around(computedStyle());
    int x = a.left;
    int right = width() - a.right;
    if (clearButton_ && !text_.empty()) right = clearRect().x - style::px(computedStyle("clear-button"), "margin-left");
    return Rect(x, 0, std::max(0, right - x), height());
}

Point LineEdit::sizeHint() const {
    // 17 'x' inside the border and padding (default.css: 1px + 3px 5px; frameless 4px 5px), and the clear button
    Font f = font();
    style::Insets a = around(computedStyle());
    int clear = 0;
    if (clearButton_) {
        const css::Style& cb = computedStyle("clear-button");
        clear = std::max(0, style::px(cb, "width", 16)) + style::px(cb, "margin-right");
    }
    Point h((int)std::ceil(17 * f.charWidth('x')) + a.w() + clear, style::textHeight(f) + a.h());
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point LineEdit::minimumSizeHint() const {
    Font f = font();
    style::Insets a = around(computedStyle());
    return Point(a.w() + (int)std::ceil(f.charWidth('x') * 2), style::textHeight(f) + a.h());
}

void LineEdit::setText(const std::string& t) {
    setTextSilently(t);
    textChanged.emit(text_);
}

void LineEdit::setTextSilently(const std::string& t) {
    text_ = t;
    cursor_ = anchor_ = text_.size();
    scroll_ = 0;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    ensureCursorVisible();
    update();
}

void LineEdit::selectAll() {
    anchor_ = 0;
    cursor_ = text_.size();
    ensureCursorVisible();
    update();
}

void LineEdit::setCursorPosition(size_t pos, bool select) {
    pos = std::min(pos, text_.size());
    // to a character boundary
    while (pos > 0 && pos < text_.size() && (((unsigned char)text_[pos]) & 0xC0) == 0x80) --pos;
    cursor_ = pos;
    if (!select) anchor_ = pos;
    blinkStart_ = App::get().now();
    ensureCursorVisible();
    update();
}

std::string LineEdit::selectedText() const {
    size_t a = std::min(cursor_, anchor_), b = std::max(cursor_, anchor_);
    return text_.substr(a, b - a);
}

void LineEdit::edited(const std::string& before, size_t beforeCursor) {
    undo_.push_back({before, beforeCursor});
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
    blinkStart_ = App::get().now();
    ensureCursorVisible();
    update();
    std::string t = text_;
    textChanged.emit(t);
    textEdited.emit(t);
}

void LineEdit::insert(const std::string& s0) {
    if (readOnly_) return;
    std::string s;
    for (char ch : s0) {
        if (ch == '\r') continue;
        s += (ch == '\n' || ch == '\t') ? ' ' : ch;
    }
    size_t a = std::min(cursor_, anchor_), b = std::max(cursor_, anchor_);
    std::string cand = text_.substr(0, a) + s + text_.substr(b);
    if ((int)utf8Count(cand) > maxLength_) {
        // as much as fits
        size_t room = (size_t)std::max(0, maxLength_ - (int)utf8Count(text_.substr(0, a) + text_.substr(b)));
        size_t i = 0, n = 0;
        while (i < s.size() && n < room) {
            i = utf8NextBoundary(s, i);
            ++n;
        }
        s = s.substr(0, i);
        cand = text_.substr(0, a) + s + text_.substr(b);
    }
    if (s.empty() && a == b) return;
    if (validator && !validator(cand)) return;
    std::string before = text_;
    size_t bc = cursor_;
    text_ = cand;
    cursor_ = anchor_ = a + s.size();
    edited(before, bc);
}

void LineEdit::deleteSelection() {
    if (!hasSelection() || readOnly_) return;
    size_t a = std::min(cursor_, anchor_), b = std::max(cursor_, anchor_);
    std::string before = text_;
    size_t bc = cursor_;
    text_.erase(a, b - a);
    cursor_ = anchor_ = a;
    edited(before, bc);
}

void LineEdit::copy() const {
    if (hasSelection()) setClipboard(selectedText());
}

void LineEdit::cut() {
    if (!hasSelection() || readOnly_) return;
    copy();
    deleteSelection();
}

void LineEdit::paste() {
    if (readOnly_) return;
    std::string t = clipboard();
    if (!t.empty()) insert(t);
}

void LineEdit::undo() {
    if (undo_.empty() || readOnly_) return;
    redo_.push_back({text_, cursor_});
    auto st = undo_.back();
    undo_.pop_back();
    text_ = st.first;
    cursor_ = anchor_ = std::min(st.second, text_.size());
    dirty_ = true;
    ensureCursorVisible();
    update();
    std::string t = text_;
    textChanged.emit(t);
    textEdited.emit(t);
}

void LineEdit::redo() {
    if (redo_.empty() || readOnly_) return;
    undo_.push_back({text_, cursor_});
    auto st = redo_.back();
    redo_.pop_back();
    text_ = st.first;
    cursor_ = anchor_ = std::min(st.second, text_.size());
    dirty_ = true;
    ensureCursorVisible();
    update();
    std::string t = text_;
    textChanged.emit(t);
    textEdited.emit(t);
}

void LineEdit::ensureCursorVisible() {
    Font f = font();
    Rect area = textArea();
    float tw = f.width(text_);
    float cx = f.width(text_.substr(0, cursor_));
    if (tw <= area.w - 1 || area.w <= 0) {
        scroll_ = 0;
        return;
    }
    if (cx - scroll_ > area.w - 1) scroll_ = cx - area.w + 1;
    if (cx - scroll_ < 0) scroll_ = cx;
    scroll_ = std::max(0.f, std::min(scroll_, tw - area.w + 1));
}

size_t LineEdit::posAt(int x) const {
    Font f = font();
    Rect area = textArea();
    float tw = f.width(text_);
    float off = 0;
    if (tw < area.w) {
        if (align_ & AlignRight) off = area.w - tw;
        else if (align_ & AlignHCenter) off = (area.w - tw) / 2;
    }
    float rel = x - area.x + scroll_ - off;
    if (rel <= 0) return 0;
    if (rel >= tw) return text_.size();
    return f.hitTest(text_, rel);
}

void LineEdit::paint(Painter& p) {
    bool en = isEnabled();
    // its box (frameless: none); its colours read at once (the parts' styles are computed after)
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Color tc = style::foreground(st);  // (default.css: --text; disabled: --disabled-text)
    Font f = font();
    Rect area = textArea();
    float asc = f.ascent(), th = f.ascent() + f.descent();
    float top = std::round((height() - th) / 2.f);
    float base = top + asc;
    float tw = f.width(text_);
    float off = 0;
    if (tw < area.w) {
        if (align_ & AlignRight) off = area.w - tw;
        else if (align_ & AlignHCenter) off = (area.w - tw) / 2;
    }
    float x0 = area.x - scroll_ + off;
    p.save();
    p.setClip(Rect(area.x - 1, 1, area.w + 2, std::max(0, height() - 2)));
    if (text_.empty() && !placeholder_.empty()) {
        Color pc = style::foreground(computedStyle("placeholder"));
        float pw = f.width(placeholder_);
        float px = area.x;
        if (pw < area.w) {
            if (align_ & AlignRight) px += area.w - pw;
            else if (align_ & AlignHCenter) px += (area.w - pw) / 2;
        }
        p.drawText(PointF(px, base), f.elided(placeholder_, (float)area.w), f, pc);
    } else {
        p.drawText(PointF(x0, base), text_, f, tc);
        if (hasSelection()) {
            // ::selection: a box over the selected text (its padding reaches past it: default.css 1px above and
            // below), the text again in its colour
            const css::Style& ss = computedStyle("selection");
            style::Insets pad = style::insets(ss, "padding");
            size_t a = std::min(cursor_, anchor_), b = std::max(cursor_, anchor_);
            float sx0 = x0 + f.width(text_.substr(0, a)), sx1 = x0 + f.width(text_.substr(0, b));
            Rect sel((int)std::floor(sx0) - pad.left, (int)top - pad.top,
                     (int)std::ceil(sx1) - (int)std::floor(sx0) + pad.w(), (int)std::ceil(th) + pad.h());
            p.save();
            p.setClip(sel);
            style::box(p, sel, ss);
            p.drawText(PointF(x0, base), text_, f, style::foreground(ss));
            p.restore();
        }
    }
    // the caret (::cursor: its colour, default.css the text's, and its width)
    Window* win = window();
    bool active = win && (win->isActiveWindow() || App::get().headless());
    if (hasFocus() && active && en && !readOnly_) {
        double t = App::get().now() - blinkStart_;
        if (std::fmod(t, 1.0) < 0.5) {
            const css::Style& cs = computedStyle("cursor");
            int cw = std::max(1, style::px(cs, "width", 1));
            float cx = x0 + f.width(text_.substr(0, cursor_));
            int ix = (int)std::floor(cx);
            if (ix >= area.right()) ix = area.right() - cw;
            p.fillRect(Rect(ix, (int)top, cw, (int)std::ceil(th)), style::background(cs));
        }
    }
    p.restore();
    if (clearButton_ && !text_.empty() && en) {
        // ::clear-button: a cross icon-size across, stroke-width, in its colour (:clear-hover under the mouse)
        Rect cr = clearRect();
        const css::Style& cb = computedStyle("clear-button");
        Color xc = style::foreground(cb);
        float d = iconSize(cb, 7) / 2.f, sw = strokeWidth(cb, 10);
        float cx = cr.x + cr.w / 2.f, cy = cr.y + cr.h / 2.f;
        if (xc.a && sw > 0) {
            p.drawLine(PointF(cx - d, cy - d), PointF(cx + d, cy + d), xc, sw, LineCap::Round);
            p.drawLine(PointF(cx - d, cy + d), PointF(cx + d, cy - d), xc, sw, LineCap::Round);
        }
    }
}

bool LineEdit::mousePress(MouseEvent& e) {
    if (e.button == plat::RightButton) return true;  // (the context menu follows)
    if (e.button != plat::LeftButton) return false;
    if (clearButton_ && !text_.empty() && clearRect().contains(e.pos) && !readOnly_) {
        std::string before = text_;
        size_t bc = cursor_;
        text_.clear();
        cursor_ = anchor_ = 0;
        edited(before, bc);
        return true;
    }
    size_t pos = posAt(e.pos.x);
    cursor_ = pos;
    if (!e.shift()) anchor_ = pos;
    selecting_ = true;
    blinkStart_ = App::get().now();
    ensureCursorVisible();
    update();
    return true;
}

bool LineEdit::mouseMove(MouseEvent& e) {
    if (clearButton_) {
        bool h = rect().contains(e.pos) && clearRect().contains(e.pos) && !text_.empty();
        if (h != clearHover_) {
            clearHover_ = h;
            update();
        }
    }
    if (!selecting_) return false;
    size_t pos = posAt(e.pos.x);
    if (pos != cursor_) {
        cursor_ = pos;
        blinkStart_ = App::get().now();
        ensureCursorVisible();
        update();
    }
    return true;
}

bool LineEdit::mouseRelease(MouseEvent& e) {
    if (e.button != plat::LeftButton) return e.button == plat::RightButton;
    selecting_ = false;
    return true;
}

bool LineEdit::mouseDoubleClick(MouseEvent& e) {
    if (e.button != plat::LeftButton) return mousePress(e);
    size_t pos = posAt(e.pos.x);
    // the word (or the run of other characters) around pos
    std::u32string u = utf8ToU32(text_);
    // byte offset -> char index
    size_t ci = utf8Count(text_.substr(0, pos));
    if (u.empty()) return true;
    if (ci >= u.size()) ci = u.size() - 1;
    bool w = isWordChar(u[ci]);
    size_t a = ci, b = ci + 1;
    while (a > 0 && isWordChar(u[a - 1]) == w && u[a - 1] != ' ') --a;
    while (b < u.size() && isWordChar(u[b]) == w && u[b] != ' ') ++b;
    anchor_ = u32ToUtf8(u.substr(0, a)).size();
    cursor_ = u32ToUtf8(u.substr(0, b)).size();
    selecting_ = false;
    ensureCursorVisible();
    update();
    return true;
}

bool LineEdit::contextMenu(MouseEvent& e) {
    Menu m;
    Action* a;
    a = m.addAction("Undo", [this] { undo(); }, "Ctrl+Z");
    a->enabled = !undo_.empty() && !readOnly_;
    a = m.addAction("Redo", [this] { redo(); }, "Ctrl+Shift+Z");
    a->enabled = !redo_.empty() && !readOnly_;
    m.addSeparator();
    a = m.addAction("Cut", [this] { cut(); }, "Ctrl+X");
    a->enabled = hasSelection() && !readOnly_;
    a = m.addAction("Copy", [this] { copy(); }, "Ctrl+C");
    a->enabled = hasSelection();
    a = m.addAction("Paste", [this] { paste(); }, "Ctrl+V");
    a->enabled = !readOnly_;
    a = m.addAction("Delete", [this] { deleteSelection(); });
    a->enabled = hasSelection() && !readOnly_;
    m.addSeparator();
    a = m.addAction("Select All", [this] { selectAll(); }, "Ctrl+A");
    a->enabled = !text_.empty();
    m.exec(e.globalPos, this);
    return true;
}

bool LineEdit::overridesShortcut(const KeyEvent& e) const {
    if (printable(e)) return !readOnly_;
    int k = e.key;
    bool ctrl = e.mods & plat::ModCtrl;
    bool alt = e.mods & plat::ModAlt;
    if (alt) return false;
    if (k == plat::Key_Left || k == plat::Key_Right || k == plat::Key_Home || k == plat::Key_End) return true;
    if (!readOnly_ && (k == plat::Key_Backspace || k == plat::Key_Delete)) return true;
    if (ctrl && (k == 'A' || k == 'C')) return true;
    if (ctrl && !readOnly_ && (k == 'V' || k == 'X' || k == 'Z' || k == 'Y')) return true;
    return false;
}

bool LineEdit::keyPress(KeyEvent& e) {
    bool ctrl = e.mods & plat::ModCtrl, shift = e.mods & plat::ModShift;
    int k = e.key;
    auto moveTo = [&](size_t pos) {
        cursor_ = pos;
        if (!shift) anchor_ = pos;
        blinkStart_ = App::get().now();
        ensureCursorVisible();
        update();
    };
    auto wordLeft = [&](size_t from) {
        std::u32string u = utf8ToU32(text_.substr(0, from));
        size_t i = u.size();
        while (i > 0 && !isWordChar(u[i - 1])) --i;
        while (i > 0 && isWordChar(u[i - 1])) --i;
        return u32ToUtf8(u.substr(0, i)).size();
    };
    auto wordRight = [&](size_t from) {
        std::u32string u = utf8ToU32(text_);
        size_t i = utf8Count(text_.substr(0, from));
        while (i < u.size() && isWordChar(u[i])) ++i;
        while (i < u.size() && !isWordChar(u[i])) ++i;
        return u32ToUtf8(u.substr(0, i)).size();
    };
    switch (k) {
    case plat::Key_Left:
        if (hasSelection() && !shift && !ctrl) moveTo(std::min(cursor_, anchor_));
        else moveTo(ctrl ? wordLeft(cursor_) : utf8PrevBoundary(text_, cursor_));
        return true;
    case plat::Key_Right:
        if (hasSelection() && !shift && !ctrl) moveTo(std::max(cursor_, anchor_));
        else moveTo(ctrl ? wordRight(cursor_) : (cursor_ < text_.size() ? utf8NextBoundary(text_, cursor_) : cursor_));
        return true;
    case plat::Key_Home:
        moveTo(0);
        return true;
    case plat::Key_End:
        moveTo(text_.size());
        return true;
    case plat::Key_Backspace:
        if (readOnly_) return true;
        if (hasSelection()) deleteSelection();
        else if (cursor_ > 0) {
            size_t from = ctrl ? wordLeft(cursor_) : utf8PrevBoundary(text_, cursor_);
            anchor_ = from;
            deleteSelection();
        }
        return true;
    case plat::Key_Delete:
        if (readOnly_) return true;
        if (shift && hasSelection()) {
            cut();
            return true;
        }
        if (hasSelection()) deleteSelection();
        else if (cursor_ < text_.size()) {
            size_t to = ctrl ? wordRight(cursor_) : utf8NextBoundary(text_, cursor_);
            anchor_ = to;
            deleteSelection();
        }
        return true;
    case plat::Key_Return:
    case plat::Key_Enter:
        returnPressed.emit();
        if (!Widget::alive(this)) return true;
        dirty_ = false;
        editingFinished.emit();
        return false;  // (goes on: a dialog's default button)
    case plat::Key_Escape:
        if (!escapePressed.empty()) {
            escapePressed.emit();
            return true;
        }
        return false;
    case plat::Key_Up:
    case plat::Key_Down:
    case plat::Key_PageUp:
    case plat::Key_PageDown:
        if (!keyNavigation.empty()) {
            keyNavigation.emit(k);
            return true;
        }
        return false;
    case plat::Key_Insert:
        if (shift) {
            paste();
            return true;
        }
        if (ctrl) {
            copy();
            return true;
        }
        return false;
    default:
        break;
    }
    if (ctrl && !(e.mods & plat::ModAlt)) {
        switch (k) {
        case 'A': selectAll(); return true;
        case 'C': copy(); return true;
        case 'X': cut(); return true;
        case 'V': paste(); return true;
        case 'Z':
            if (shift) redo();
            else undo();
            return true;
        case 'Y': redo(); return true;
        default: return false;
        }
    }
    if (printable(e)) {
        if (readOnly_) return false;
        insert(e.text);
        return true;
    }
    return false;
}

void LineEdit::focusIn() {
    blinkStart_ = App::get().now();
    Window* w = window();
    if (w && w->focusReason == 2) selectAll();
    if (blinkTimer_ < 0) blinkTimer_ = App::get().every(500, [this] { update(); });
    update();
}

void LineEdit::focusOut() {
    if (blinkTimer_ >= 0) {
        App::get().cancelTimer(blinkTimer_);
        blinkTimer_ = -1;
    }
    selecting_ = false;
    anchor_ = cursor_;
    update();
    if (dirty_) {
        dirty_ = false;
        editingFinished.emit();
    }
}

// ============================================================================================
// SpinBox
// ============================================================================================

SpinBox::SpinBox(Widget* parent) : Widget(parent) {
    setStyleType("SpinBox");
    edit_ = add<LineEdit>();
    edit_->frame = false;
    focusProxy = edit_;
    setFocusPolicy(FocusPolicy::Strong);
    vPolicy = SizePolicy::Fixed;
    setMouseTracking(true);
    edit_->textEdited.connect([this](const std::string& t) {
        if (!tracking_) return;
        double v;
        if (parse(t, v) && v >= lo_ - 1e-9 && v <= hi_ + 1e-9) {
            v = clampRound(v);
            if (v != value_) {
                value_ = v;
                valueChanged.emit(v);
            }
        }
    });
    edit_->editingFinished.connect([this] {
        commit();
        editingFinished.emit();
    });
    edit_->keyNavigation.connect([this](int key) {
        if (!isEnabled()) return;
        if (key == plat::Key_Up) stepBy(1);
        else if (key == plat::Key_Down) stepBy(-1);
        else if (key == plat::Key_PageUp) stepBy(10);
        else if (key == plat::Key_PageDown) stepBy(-10);
    });
    refresh();
}

SpinBox::~SpinBox() { stopRepeat(); }

void SpinBox::stopRepeat() {
    if (repeatTimer_ >= 0) {
        App::get().cancelTimer(repeatTimer_);
        repeatTimer_ = -1;
    }
}

double SpinBox::clampRound(double v) const {
    if (std::isnan(v)) v = lo_;
    v = std::max(lo_, std::min(hi_, v));
    double m = std::pow(10.0, decimals_);
    v = std::round(v * m) / m;
    if (v == 0) v = 0;  // (no -0)
    return std::max(lo_, std::min(hi_, v));
}

void SpinBox::setRange(double lo, double hi) {
    lo_ = lo;
    hi_ = std::max(lo, hi);
    value_ = clampRound(value_);
    refresh();
    updateGeometry();
}

void SpinBox::setValue(double v) {
    v = clampRound(v);
    if (v != value_) {
        value_ = v;
        refresh();
        valueChanged.emit(v);
    } else {
        refresh();
    }
}

void SpinBox::setValueSilently(double v) {
    value_ = clampRound(v);
    refresh();
}

void SpinBox::setAlignment(int a) { edit_->setAlignment(a); }

std::string SpinBox::format(double v) const {
    if (textFromValue) return textFromValue(v);
    double m = std::pow(10.0, decimals_);
    if (std::fabs(v) < 0.5 / m) v = 0;
    return prefix_ + util::fixed(v, decimals_) + suffix_;
}

std::string SpinBox::text() const { return edit_->text(); }

bool SpinBox::parse(const std::string& text, double& v) const {
    if (valueFromText) return valueFromText(text, v);
    std::string t = util::trim(text);
    std::string pre = util::trim(prefix_), suf = util::trim(suffix_);
    if (!pre.empty() && util::startsWith(t, pre)) t = t.substr(pre.size());
    if (!suf.empty() && util::endsWith(t, suf)) t = t.substr(0, t.size() - suf.size());
    t = util::trim(t);
    for (auto& ch : t)
        if (ch == ',') ch = '.';
    if (t.empty()) return false;
    char* end = nullptr;
    double d = std::strtod(t.c_str(), &end);
    if (!end || *end != 0) return false;
    if (decimals_ == 0 && t.find('.') != std::string::npos) {
        // an integer box takes "3.0" but not "3.5"
        if (std::fabs(d - std::round(d)) > 1e-9) return false;
    }
    v = d;
    return true;
}

void SpinBox::refresh() {
    std::string t = format(value_);
    if (edit_->text() != t) edit_->setTextSilently(t);
}

void SpinBox::commit() {
    double v;
    if (parse(edit_->text(), v)) {
        v = clampRound(v);
        bool changed = v != value_;
        value_ = v;
        edit_->setTextSilently(format(value_));
        if (changed) valueChanged.emit(v);
    } else {
        edit_->setTextSilently(format(value_));
    }
}

void SpinBox::stepBy(int steps) {
    if (!steps) return;
    // what's typed counts first (as Qt interprets the text before stepping)
    double cur = value_;
    double typed;
    if (parse(edit_->text(), typed)) cur = clampRound(typed);
    double v = cur + steps * step_;
    if (wrapping_) {
        double span = hi_ - lo_;
        if (v > hi_ + 1e-9) v = cur >= hi_ - 1e-9 ? lo_ : hi_;
        else if (v < lo_ - 1e-9) v = cur <= lo_ + 1e-9 ? hi_ : lo_;
        (void)span;
    }
    setValue(v);
    if (edit_->hasFocus()) edit_->selectAll();
}

void SpinBox::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    // :focus is its editor's (the focus proxy)
    out.erase(std::remove(out.begin(), out.end(), std::string("focus")), out.end());
    if (edit_ && edit_->hasFocus()) out.push_back("focus");
    if (hoverPart_ == 1) out.push_back("up-hover");
    if (hoverPart_ == -1) out.push_back("down-hover");
    if (pressPart_ == 1) out.push_back("up-pressed");
    if (pressPart_ == -1) out.push_back("down-pressed");
    if (!wrapping_ && value_ >= hi_) out.push_back("at-maximum");
    if (!wrapping_ && value_ <= lo_) out.push_back("at-minimum");
}

// the ▴▾ column's width: ::up-button's width (default.css 16 px)
int SpinBox::buttonsWidth() const { return buttons_ ? std::max(0, style::px(computedStyle("up-button"), "width", 16)) : 0; }

// the editor fills the inside of its border, but for the buttons
void SpinBox::resized() {
    style::Insets b = style::insets(computedStyle(), "border-width");
    edit_->setGeometry(b.left, b.top, std::max(0, width() - b.w() - buttonsWidth()), std::max(0, height() - b.h()));
}

Point SpinBox::sizeHint() const {
    // the widest number inside its border and padding (default.css: 1px + 3px 7px), and the buttons
    Font f = edit_->font();
    int w = std::max(style::textWidth(f, format(lo_)), style::textWidth(f, format(hi_)));
    w = std::max(w, style::textWidth(f, format(value_)));
    w = std::min(w, style::textWidth(f, "000000000000000000"));
    style::Insets a = around(computedStyle());
    Point h(w + a.w() + buttonsWidth(), style::textHeight(f) + a.h());
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

int SpinBox::partAt(Point p) const {
    if (!buttons_ || !rect().contains(p)) return 0;
    if (p.x < width() - style::insets(computedStyle(), "border-width").right - buttonsWidth()) return 0;
    return p.y < height() / 2 ? 1 : -1;
}

void SpinBox::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    if (!buttons_) return;
    // the buttons: the column inside the border at the right, halved; each part's box is the inside of the border
    // (its corners), clipped to the button; its arrow centred (:at-maximum / :at-minimum / :disabled: none)
    Rect inner = style::inside(rect(), style::insets(st, "border-width"));
    int bw = buttonsWidth();
    int bx = inner.right() - bw;
    int ih = inner.h;
    Rect up(bx, inner.y, bw, ih / 2), down(bx, inner.y + ih / 2, bw, ih - ih / 2);
    auto part = [&](const Rect& r, const char* name, bool isUp) {
        const css::Style& ps = computedStyle(name);
        p.save();
        p.setClip(r);
        style::box(p, inner, ps);
        p.restore();
        float size = iconSize(ps, 8);
        arrowIcon(p, r.x + (r.w - size) / 2.f, r.y + (r.h - size) / 2.f, size, isUp, style::foreground(ps), strokeWidth(ps, size));
    };
    part(up, "up-button", true);
    part(down, "down-button", false);
}

bool SpinBox::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    int part = partAt(e.pos);
    if (!part) return false;
    pressPart_ = part;
    update();  // (:up-pressed / :down-pressed)
    stepBy(part);
    stopRepeat();
    repeatTimer_ = App::get().singleShot(300, [this] {
        repeatTimer_ = App::get().every(100, [this] {
            if (pressPart_ && isPressed() && partAt(mapFromGlobal(App::get().cursorPos())) == pressPart_) stepBy(pressPart_);
        });
    });
    return true;
}

bool SpinBox::mouseRelease(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    stopRepeat();
    if (pressPart_) update();
    pressPart_ = 0;
    return true;
}

bool SpinBox::mouseMove(MouseEvent& e) {
    int part = partAt(e.pos);
    if (part != hoverPart_) {
        hoverPart_ = part;
        update();
    }
    return pressPart_ != 0;
}

bool SpinBox::wheel(WheelEvent& e) {
    if (!wheelOn_ || !isEnabled()) return false;
    wheelAcc_ += e.dy;
    int steps = (int)wheelAcc_;
    wheelAcc_ -= steps;
    if (e.ctrl()) steps *= 10;
    if (steps) stepBy(steps);
    return true;
}

bool SpinBox::keyPress(KeyEvent& e) {
    if (e.key == plat::Key_Up) { stepBy(1); return true; }
    if (e.key == plat::Key_Down) { stepBy(-1); return true; }
    if (e.key == plat::Key_PageUp) { stepBy(10); return true; }
    if (e.key == plat::Key_PageDown) { stepBy(-10); return true; }
    return false;
}

void SpinBox::focusIn() { edit_->setFocus(); }
void SpinBox::focusOut() {}

// ============================================================================================
// ComboBox and its list
// ============================================================================================

// Stylesheets: ComboPopup (its box and padding; parts ::item and ::separator: see ComboBox in controls.h). An
// item's states (:hover and :selected the highlighted row, :disabled a disabled one) are the popup's own while
// that item's style is computed (itemStyle).
class ComboPopup : public PopupWindow {
public:
    explicit ComboPopup(ComboBox* c) : combo(c) {
        setStyleType("ComboPopup");
        setMouseTracking(true);
        bar = add<ScrollBar>(Orientation::Vertical);
        bar->setVisible(false);
        bar->valueChanged.connect([this](int v) {
            scrollY = v;
            update();
        });
    }
    ComboBox* combo;
    ScrollBar* bar;
    int hover = -1, scrollY = 0, viewH = 0, rowH = 20, sepH = 7;
    bool armed = false;
    std::vector<int> ys, hs;
    // room so that the widest item is never cut short (beyond the item's padding)
    static const int kSlack = 4;

    enum ItemMode { NotItem, Item, Highlighted, Disabled };
    mutable ItemMode itemMode_ = NotItem;
    void styleStates(std::vector<std::string>& out) const override {
        if (itemMode_ == NotItem) {
            PopupWindow::styleStates(out);
            return;
        }
        if (itemMode_ == Highlighted) {
            out.push_back("hover");
            out.push_back("selected");
        } else if (itemMode_ == Disabled) {
            out.push_back("disabled");
        }
    }
    // ::item as an item in that mode looks (a copy: kept while every row is drawn)
    css::Style itemStyle(ItemMode mode) const {
        itemMode_ = mode;
        css::Style s = computedStyle("item");
        itemMode_ = NotItem;
        return s;
    }

    int itemH(int i) const { return combo->isSeparator(i) ? sepH : rowH; }

    void openFor() {
        // its size: the rows (an item: its font's height and padding; a separator: its height) inside its border
        // and padding (default.css: 1px + 2px; items 3px 6px; separators 7px), as wide as the widest item
        style::Insets around0 = around(computedStyle());
        css::Style item = itemStyle(Item);
        style::Insets itemAround = around(item);
        Font f = style::font(item, theme::uiFont());
        rowH = style::textHeight(f) + itemAround.h();
        sepH = std::max(0, style::px(computedStyle("separator"), "height", 7));
        int n = combo->count();
        ys.assign(n, 0);
        hs.assign(n, 0);
        int y = 0, maxW = 0;
        for (int i = 0; i < n; ++i) {
            ys[i] = y;
            hs[i] = itemH(i);
            y += hs[i];
            if (!combo->isSeparator(i)) maxW = std::max(maxW, style::textWidth(f, combo->itemText(i)));
        }
        int total = y;
        int visRows = std::max(1, std::min(n, combo->maxVisibleItems));
        int limit = 0;
        for (int i = 0; i < visRows; ++i) limit += hs[i];
        viewH = std::min(total, std::max(limit, rowH));
        bool needBar = total > viewH;
        int w = std::max(combo->width(), maxW + itemAround.w() + around0.w() + (needBar ? theme::SCROLLBAR_W : 0) + kSlack);
        int h = viewH + around0.h();
        bar->setVisible(needBar);
        bar->setGeometry(w - around0.right - theme::SCROLLBAR_W, around0.top, theme::SCROLLBAR_W, viewH);
        bar->setRange(0, std::max(0, total - viewH));
        bar->setPageStep(viewH);
        bar->setSingleStep(rowH);
        hover = combo->currentIndex();
        scrollY = 0;
        bar->setValue(0);
        ensureVisible(hover);
        armed = false;
        Point g = combo->mapToGlobal(Point(0, 0));
        Point at = placeBelow(Rect(g.x, g.y, combo->width(), combo->height()), w, h);
        open(at, w, h, combo);
        combo->update();
    }
    void ensureVisible(int i) {
        if (i < 0 || i >= (int)ys.size()) return;
        int v = scrollY;
        if (ys[i] < v) v = ys[i];
        if (ys[i] + hs[i] > v + viewH) v = ys[i] + hs[i] - viewH;
        bar->setValue(v);
        scrollY = bar->value();
    }
    int rowAt(Point p) const {
        // (the whole width inside its border, the rows' height inside its padding)
        const css::Style& st = computedStyle();
        style::Insets b = style::insets(st, "border-width"), a = around(st);
        Rect view(b.left, a.top, width() - b.w(), viewH);
        if (!view.contains(p)) return -1;
        int y = p.y - view.y + scrollY;
        for (size_t i = 0; i < ys.size(); ++i)
            if (y >= ys[i] && y < ys[i] + hs[i]) return (int)i;
        return -1;
    }
    void paint(Painter& p) override {
        const css::Style& st = computedStyle();
        style::box(p, rect(), st);
        style::Insets a = around(st);
        // each kind of row's style once (not per row)
        css::Style sep = computedStyle("separator");
        css::Style looks[3] = {itemStyle(Item), itemStyle(Highlighted), itemStyle(Disabled)};
        Font fonts[3];
        for (int k = 0; k < 3; ++k) fonts[k] = style::font(looks[k], theme::uiFont());
        style::Insets sepMargin = style::insets(sep, "margin");
        int sepLine = std::max(0, style::px(sep, "stroke-width", 1));
        int right = width() - a.right - (bar->isVisible() ? theme::SCROLLBAR_W : 0);
        Rect view(a.left, a.top, right - a.left, viewH);
        p.save();
        p.setClip(view);
        for (int i = 0; i < (int)ys.size(); ++i) {
            int y = view.y + ys[i] - scrollY;
            if (y + hs[i] < view.y || y > view.bottom()) continue;
            Rect r(view.x, y, view.w, hs[i]);
            if (combo->isSeparator(i)) {
                // a line stroke-width thick across the middle, inside its margin
                Rect line(r.x + sepMargin.left, r.y + (r.h - sepLine) / 2, r.w - sepMargin.w(), sepLine);
                if (sepLine > 0) style::box(p, line, sep);
                continue;
            }
            bool en = combo->isItemEnabled(i);
            int k = !en ? 2 : i == hover ? 1 : 0;
            const css::Style& is = looks[k];
            style::box(p, r, is);
            // the text inside its padding's left and right, centred in the row's height
            style::Insets ia = around(is);
            style::text(p, r.adjusted(ia.left, 0, -ia.right, 0), AlignLeft | AlignVCenter, combo->itemText(i), fonts[k],
                        style::foreground(is), true);
        }
        p.restore();
    }
    void choose(int i) {
        ComboBox* c = combo;
        App::get().closePopup(this);
        c->update();
        c->choose(i);
    }
    void dismiss() override {
        App::get().closePopup(this);
        combo->update();
    }
    bool mouseMove(MouseEvent& e) override {
        int r = rowAt(e.pos);
        if (r >= 0 && combo->isItemEnabled(r)) {
            if (r != hover) {
                hover = r;
                update();
            }
            if (e.buttons) armed = true;
        }
        return true;
    }
    bool mousePress(MouseEvent& e) override {
        if (rect().contains(e.pos)) armed = true;
        return true;
    }
    bool mouseRelease(MouseEvent& e) override {
        int r = rowAt(e.pos);
        if (armed && r >= 0 && combo->isItemEnabled(r)) choose(r);
        return true;
    }
    bool wheel(WheelEvent& e) override {
        bar->setValue(bar->value() - (int)std::lround(e.dy * 3 * rowH));
        scrollY = bar->value();
        update();
        return true;
    }
    int stepFrom(int from, int dir, int count = 1) const {
        int n = combo->count(), i = from, last = from;
        for (int k = 0; k < count; ++k) {
            int j = i + dir;
            while (j >= 0 && j < n && !combo->isItemEnabled(j)) j += dir;
            if (j < 0 || j >= n) break;
            i = j;
            last = j;
        }
        return last;
    }
    bool keyPress(KeyEvent& e) override {
        int n = combo->count();
        switch (e.key) {
        case plat::Key_Up: hover = stepFrom(hover < 0 ? n : hover, -1); break;
        case plat::Key_Down: hover = stepFrom(hover, 1); break;
        case plat::Key_PageUp: hover = stepFrom(hover < 0 ? n : hover, -1, combo->maxVisibleItems - 1); break;
        case plat::Key_PageDown: hover = stepFrom(hover, 1, combo->maxVisibleItems - 1); break;
        case plat::Key_Home: hover = stepFrom(-1, 1); break;
        case plat::Key_End: hover = stepFrom(n, -1); break;
        case plat::Key_Return:
        case plat::Key_Enter:
        case plat::Key_Space:
            if (hover >= 0 && combo->isItemEnabled(hover)) choose(hover);
            else dismiss();
            return true;
        case plat::Key_Escape:
        case plat::Key_F4:
        case plat::Key_Tab:
            dismiss();
            return true;
        default:
            if (!e.text.empty() && (unsigned char)e.text[0] >= 0x20) {
                // jump to the next item starting with the typed letter
                std::string ch = util::lower(e.text);
                for (int k = 1; k <= n; ++k) {
                    int i = ((hover < 0 ? -1 : hover) + k) % n;
                    if (combo->isItemEnabled(i) && util::startsWith(util::lower(combo->itemText(i)), ch)) {
                        hover = i;
                        break;
                    }
                }
            }
            break;
        }
        ensureVisible(hover);
        update();
        return true;
    }
};

ComboBox::ComboBox(Widget* parent) : Widget(parent) {
    setStyleType("ComboBox");
    setFocusPolicy(FocusPolicy::Strong);
    vPolicy = SizePolicy::Fixed;
}

void ComboBox::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    if (isPopupOpen()) out.push_back("open");
    if (edit_) out.push_back("editable");
}

// the arrow's box: the right-hand ::arrow width (default.css 18 px, over the border)
int ComboBox::arrowWidth() const { return std::max(0, style::px(computedStyle("arrow"), "width", 18)); }

ComboBox::~ComboBox() {
    if (popup_ && popup_->isOpen()) App::get().closePopup(popup_.get());
    popup_.reset();
}

void ComboBox::addItem(const std::string& text, const std::string& data) {
    Item it;
    it.text = text;
    it.data = data;
    items_.push_back(it);
    updateGeometry();
    update();
    if (current_ < 0) {
        current_ = (int)items_.size() - 1;
        if (edit_) edit_->setTextSilently(text);
        currentIndexChanged.emit(current_);
    }
}

void ComboBox::insertSeparator() {
    Item it;
    it.separator = true;
    it.enabled = false;
    items_.push_back(it);
}

void ComboBox::clear() {
    hidePopup();
    items_.clear();
    bool had = current_ >= 0;
    current_ = -1;
    if (edit_) edit_->setTextSilently("");
    updateGeometry();
    update();
    if (had) currentIndexChanged.emit(-1);
}

void ComboBox::setCurrentIndex(int i) {
    if (i < -1 || i >= count() || i == current_) return;
    if (i >= 0 && items_[i].separator) return;
    current_ = i;
    if (edit_) edit_->setTextSilently(currentText());
    update();
    currentIndexChanged.emit(i);
}

void ComboBox::setCurrentIndexSilently(int i) {
    if (i < -1 || i >= count()) return;
    current_ = i;
    if (edit_) edit_->setTextSilently(i >= 0 ? items_[i].text : "");
    update();
}

std::string ComboBox::currentText() const {
    if (edit_) return edit_->text();
    return current_ >= 0 && current_ < count() ? items_[current_].text : std::string();
}

std::string ComboBox::currentData() const { return current_ >= 0 && current_ < count() ? items_[current_].data : std::string(); }

int ComboBox::currentDataInt(int def) const {
    std::string d = currentData();
    if (d.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(d.c_str(), &end, 10);
    return end && *end == 0 ? (int)v : def;
}

std::string ComboBox::itemText(int i) const { return i >= 0 && i < count() ? items_[i].text : std::string(); }
std::string ComboBox::itemData(int i) const { return i >= 0 && i < count() ? items_[i].data : std::string(); }

void ComboBox::setItemText(int i, const std::string& t) {
    if (i < 0 || i >= count()) return;
    items_[i].text = t;
    if (i == current_ && edit_) edit_->setTextSilently(t);
    updateGeometry();
    update();
}

void ComboBox::setItemEnabled(int i, bool on) {
    if (i >= 0 && i < count()) items_[i].enabled = on;
    update();
}

int ComboBox::findData(const std::string& data) const {
    for (int i = 0; i < count(); ++i)
        if (!items_[i].separator && items_[i].data == data) return i;
    return -1;
}

int ComboBox::findText(const std::string& text) const {
    for (int i = 0; i < count(); ++i)
        if (!items_[i].separator && items_[i].text == text) return i;
    return -1;
}

void ComboBox::setEditable(bool on) {
    if (on == editable_) return;
    editable_ = on;
    if (on) {
        edit_ = add<LineEdit>();
        edit_->frame = false;
        edit_->setTextSilently(current_ >= 0 ? items_[current_].text : "");
        focusProxy = edit_;
        resized();
    } else if (edit_) {
        focusProxy = nullptr;
        edit_->deleteLater();
        edit_ = nullptr;
    }
    update();
}

// an editable one's editor fills the inside of its border, but for the arrow's width
void ComboBox::resized() {
    if (!edit_) return;
    style::Insets b = style::insets(computedStyle(), "border-width");
    edit_->setGeometry(b.left, b.top, std::max(0, width() - b.w() - arrowWidth()), std::max(0, height() - b.h()));
}

bool ComboBox::isPopupOpen() const { return popup_ && popup_->isOpen(); }

void ComboBox::hidePopup() {
    if (isPopupOpen()) App::get().closePopup(popup_.get());
    update();
}

void ComboBox::showPopup() {
    if (count() == 0 || !isEnabled()) return;
    if (!popup_) popup_ = std::make_unique<ComboPopup>(this);
    popup_->openFor();
}

void ComboBox::choose(int i) {
    if (i < 0 || i >= count() || !isItemEnabled(i)) return;
    setCurrentIndex(i);
    activated.emit(i);
}

int ComboBox::step(int from, int dir) const {
    for (int i = from + dir; i >= 0 && i < count(); i += dir)
        if (isItemEnabled(i)) return i;
    return from;
}

Point ComboBox::sizeHint() const {
    // the widest item inside its border and padding (default.css: 1px + 3px 7px 3px 5px), and the arrow's box
    const css::Style& st = computedStyle();
    Font f = style::font(st, theme::uiFont());
    style::Insets a = around(st);
    int w = style::textWidth(f, "xxxxx");
    for (auto& it : items_)
        if (!it.separator) w = std::max(w, style::textWidth(f, it.text));
    Point h(w + a.w() + arrowWidth(), style::textHeight(f) + a.h());
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

void ComboBox::paint(Painter& p) {
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Color tc = style::foreground(st);
    Font f = style::font(st, theme::uiFont());
    int textX = around(st).left;
    // ::arrow: its box at the right (over the border), the ▾ at its padding-left, centred in the height
    const css::Style& as = computedStyle("arrow");
    int aw = std::max(0, style::px(as, "width", 18));
    Rect ar(width() - aw, 0, aw, height());
    if (!edit_) {
        // the text from the padding's left up to the arrow's box, centred in the whole height
        Rect tr(textX, 0, std::max(0, ar.x - textX), height());
        style::text(p, tr, AlignLeft | AlignVCenter, currentText(), f, tc, true);
    }
    style::box(p, ar, as);
    float size = iconSize(as, 10);
    arrowIcon(p, (float)(ar.x + style::insets(as, "padding").left), (height() - size) / 2.f, size, false,
              style::foreground(as), strokeWidth(as, size));
}

bool ComboBox::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    if (isPopupOpen()) hidePopup();
    else showPopup();
    return true;
}

bool ComboBox::wheel(WheelEvent& e) {
    if (!wheelOn_ || !isEnabled() || count() == 0) return false;
    wheelAcc_ += e.dy;
    int steps = (int)wheelAcc_;
    wheelAcc_ -= steps;
    int i = current_;
    for (int k = 0; k < std::abs(steps); ++k) i = step(i < 0 ? (steps > 0 ? count() : -1) : i, steps > 0 ? -1 : 1);
    if (i != current_ && i >= 0) {
        setCurrentIndex(i);
        activated.emit(i);
    }
    return true;
}

bool ComboBox::keyPress(KeyEvent& e) {
    int k = e.key;
    int i = -1;
    if (k == plat::Key_Up || k == plat::Key_Left) i = step(current_ < 0 ? count() : current_, -1);
    else if (k == plat::Key_Down && (e.mods & plat::ModAlt)) {
        showPopup();
        return true;
    } else if (k == plat::Key_Down || k == plat::Key_Right) i = step(current_, 1);
    else if (k == plat::Key_Home) i = step(-1, 1);
    else if (k == plat::Key_End) i = step(count(), -1);
    else if (k == plat::Key_F4 || (k == plat::Key_Space && !edit_)) {
        showPopup();
        return true;
    } else {
        return false;
    }
    if (i >= 0 && i != current_) {
        setCurrentIndex(i);
        activated.emit(i);
    }
    return true;
}

}  // namespace ui
