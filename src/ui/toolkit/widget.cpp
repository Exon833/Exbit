// The toolkit's core: Widget (tree, geometry, painting, events), KeySequence / Shortcut, Window
// (a native window: framebuffer, mouse / key dispatch, hover, focus, modality) and App (the main
// loop, frame timer, timers, tooltips, drags, popups, deleteLater).
#include "ui/toolkit/widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {
std::unordered_set<const Widget*>& aliveSet() {
    static auto* s = new std::unordered_set<const Widget*>();  // (never destroyed: widgets may outlive statics)
    return *s;
}
const char* const kMouseNames[] = {"mousePress", "mouseDoubleClick"};
}  // namespace

bool Widget::alive(const Widget* w) { return w && aliveSet().count(w) > 0; }

// ============================================================================================
// Widget
// ============================================================================================

Widget::Widget(Widget* parent) {
    aliveSet().insert(this);
    if (parent) {
        parent_ = parent;
        parent->children_.push_back(std::unique_ptr<Widget>(this));
        parent->updateGeometry();
    }
}

Widget::~Widget() {
    aliveSet().erase(this);  // (from now on nothing schedules it or delivers to it)
    App::get().forget(this);
    // children first, while this widget's layout still exists (they remove themselves from it)
    while (!children_.empty()) {
        std::unique_ptr<Widget> c = std::move(children_.back());
        children_.pop_back();
        c.reset();
    }
    layout_.reset();
    if (parent_ && parent_->layout_ && alive(parent_)) parent_->layout_->forgetWidget(this);
    App::get().forget(this);
}

void Widget::adopt(std::unique_ptr<Widget> child) {
    if (!child) return;
    Widget* c = child.get();
    if (c->parent_ == this) {
        for (auto& ch : children_)
            if (ch.get() == c) { child.release(); return; }  // (made with this parent already)
    } else if (c->parent_) {
        Widget* old = c->parent_;
        for (auto it = old->children_.begin(); it != old->children_.end(); ++it)
            if (it->get() == c) { it->release(); old->children_.erase(it); break; }
        if (old->layout_) old->layout_->forgetWidget(c);
        App::get().detach(c);
        old->updateGeometry();
    }
    c->parent_ = this;
    children_.push_back(std::move(child));
    if (c->visible_) c->update();
    updateGeometry();
    if (layout_) App::get().scheduleLayout(this);
}

std::unique_ptr<Widget> Widget::release(Widget* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            child->update();
            App::get().detach(child);
            std::unique_ptr<Widget> out = std::move(*it);
            children_.erase(it);
            if (layout_) {
                layout_->forgetWidget(child);
                App::get().scheduleLayout(this);
            }
            child->parent_ = nullptr;
            return out;
        }
    }
    return nullptr;
}

void Widget::deleteLater() {
    if (deleted_) return;
    update();
    deleted_ = true;
    App& app = App::get();
    app.detach(this);
    app.trash.push_back({this, app.loopLevel()});
    if (parent_) parent_->updateGeometry();
}

void Widget::clearChildren() {
    for (auto& c : children_) c->deleteLater();
}

Window* Widget::window() const {
    const Widget* w = this;
    while (w->parent_) w = w->parent_;
    return dynamic_cast<Window*>(const_cast<Widget*>(w));
}

void Widget::raise() {
    if (!parent_) return;
    auto& v = parent_->children_;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].get() == this) {
            auto p = std::move(v[i]);
            v.erase(v.begin() + i);
            v.push_back(std::move(p));
            break;
        }
    update();
}

void Widget::lower() {
    if (!parent_) return;
    auto& v = parent_->children_;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].get() == this) {
            auto p = std::move(v[i]);
            v.erase(v.begin() + i);
            v.insert(v.begin(), std::move(p));
            break;
        }
    update();
}

bool Widget::isAncestorOf(const Widget* w) const {
    for (; w; w = w->parent_)
        if (w == this) return true;
    return false;
}

// --- geometry ---
void Widget::setGeometry(const Rect& r0) {
    Rect r = r0;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    if (r == geom_) {
        if (layoutDirty_) relayout();
        return;
    }
    Rect old = geom_;
    if (visible_ && parent_) {
        // the old place needs repainting (in the parent)
        Widget* p = parent_;
        Rect inP = old;
        if (!inP.empty() && p->isShown()) p->update(inP);
    }
    geom_ = r;
    bool sized = old.w != r.w || old.h != r.h;
    bool moved_ = old.x != r.x || old.y != r.y;
    if (sized || layoutDirty_) relayout();
    if (sized) {
        filterEvent("resize");
        resized();
    }
    if (moved_) {
        filterEvent("move");
        moved();
    }
    update();
}

Point Widget::mapToWindow(Point p) const {
    const Widget* w = this;
    while (w->parent_) {
        p = p + Point(w->geom_.x, w->geom_.y);
        w = w->parent_;
    }
    return p;
}

Point Widget::mapTo(const Widget* ancestor, Point p) const {
    const Widget* w = this;
    while (w && w != ancestor) {
        p = p + Point(w->geom_.x, w->geom_.y);
        w = w->parent_;
    }
    return p;
}

Point Widget::mapToGlobal(Point p) const {
    Point q = mapToWindow(p);
    Window* win = window();
    if (!win) return q;
    Rect g = win->screenGeometry();
    return q + Point(g.x, g.y);
}

Point Widget::mapFromGlobal(Point p) const {
    Point zero = mapToGlobal(Point(0, 0));
    return p - zero;
}

Point Widget::sizeHint() const {
    Point h(0, 0);
    if (layout_) {
        h = layout_->sizeHint();
        Rect cr = contentsRect(), r = rect();
        h.x += r.w - cr.w;
        h.y += r.h - cr.h;
    }
    if (minSize_.x == maxSize_.x) h.x = minSize_.x;
    if (minSize_.y == maxSize_.y) h.y = minSize_.y;
    return h;
}

Point Widget::minimumSizeHint() const {
    if (!layout_) return Point(0, 0);
    Point h = layout_->minimumSize();
    Rect cr = contentsRect(), r = rect();
    h.x += r.w - cr.w;
    h.y += r.h - cr.h;
    return h;
}

void Widget::setMinimumSize(int w, int h) {
    minSize_ = Point(std::max(0, w), std::max(0, h));
    maxSize_.x = std::max(maxSize_.x, minSize_.x);
    maxSize_.y = std::max(maxSize_.y, minSize_.y);
    updateGeometry();
}

void Widget::setMaximumSize(int w, int h) {
    maxSize_ = Point(std::max(0, w), std::max(0, h));
    minSize_.x = std::min(minSize_.x, maxSize_.x);
    minSize_.y = std::min(minSize_.y, maxSize_.y);
    updateGeometry();
}

void Widget::setFixedSize(int w, int h) {
    minSize_ = maxSize_ = Point(std::max(0, w), std::max(0, h));
    updateGeometry();
    if (geom_.w != w || geom_.h != h) resize(w, h);
}

void Widget::setFixedWidth(int w) {
    minSize_.x = maxSize_.x = std::max(0, w);
    updateGeometry();
    if (geom_.w != w) resize(w, geom_.h);
}

void Widget::setFixedHeight(int h) {
    minSize_.y = maxSize_.y = std::max(0, h);
    updateGeometry();
    if (geom_.h != h) resize(geom_.w, h);
}

void Widget::setLayout(std::unique_ptr<Layout> l) {
    layout_ = std::move(l);
    if (layout_) layout_->setOwner(this);
    updateGeometry();
    App::get().scheduleLayout(this);
    if (!geom_.empty()) relayout();
}

void Widget::relayout() {
    layoutDirty_ = false;
    if (layout_) layout_->apply(this, contentsRect());
}

void Widget::updateGeometry() {
    if (!alive(this)) return;
    Widget* c = this;
    Widget* p = parent_;
    while (p && alive(p)) {
        if (p->layout_) {
            App::get().scheduleLayout(p);
        } else {
            p->childSizeHintChanged(c);
            break;
        }
        c = p;
        p = p->parent_;
    }
    if (!parent_ && layout_) App::get().scheduleLayout(this);
}

// --- state ---
bool Widget::isShown() const {
    const Widget* w = this;
    while (true) {
        if (!w->visible_ || w->deleted_) return false;
        if (!w->parent_) break;
        w = w->parent_;
    }
    auto* win = dynamic_cast<const Window*>(w);
    return win && win->isWindowShown();
}

void Widget::setVisible(bool v) {
    if (visible_ == v) return;
    if (!v) {
        update();
        App::get().detach(this);
    }
    visible_ = v;
    if (v) {
        if (layoutDirty_ || (layout_ && !geom_.empty())) relayout();
        update();
    }
    if (parent_) parent_->updateGeometry();
    if (parent_ && parent_->layout_) App::get().scheduleLayout(parent_);
    filterEvent(v ? "show" : "hide");
    if (v) showEvent(); else hideEvent();
}

bool Widget::isEnabled() const {
    for (const Widget* w = this; w; w = w->parent_)
        if (!w->enabled_) return false;
    return true;
}

void Widget::setEnabled(bool e) {
    if (enabled_ == e) return;
    enabled_ = e;
    if (!e) {
        Window* win = window();
        if (win && win->focus_ && isAncestorOf(win->focus_)) win->setFocusWidget(nullptr);
    }
    update();
}

void Widget::setCursor(Cursor c) {
    cursor_ = c;
    cursorSet_ = true;
    Window* win = window();
    if (win) win->applyCursor();
}

void Widget::setFocus() {
    Widget* t = this;
    for (int guard = 0; t->focusProxy && guard < 8; ++guard) t = t->focusProxy;
    Window* win = t->window();
    if (win) win->setFocusWidget(t);
}

void Widget::clearFocus() {
    Window* win = window();
    if (win && win->focus_ && (win->focus_ == this || (focusProxy && win->focus_ == focusProxy)))
        win->setFocusWidget(nullptr);
}

bool Widget::hasFocus() const {
    Window* win = window();
    if (!win || !win->focus_) return false;
    return win->focus_ == this || (focusProxy && win->focus_ == focusProxy);
}

void Widget::setProperty(const std::string& key, const std::string& value) {
    auto it = props_.find(key);
    if (it != props_.end() && it->second == value) return;
    props_[key] = value;
    classesVersion_++;
    update();
}

// --- style ---
void Widget::setStyleType(const std::string& name) {
    if (types_.back() != name) types_.push_back(name);
    classesVersion_++;
}

void Widget::replaceStyleType(const std::string& was, const std::string& now) {
    for (auto& t : types_)
        if (t == was) {
            t = now;
            classesVersion_++;
            return;
        }
    setStyleType(now);
}

bool Widget::isType(const std::string& name) const {
    for (auto& t : types_)
        if (t == name) return true;
    return false;
}

bool Widget::hasClass(const std::string& name) const {
    for (auto& c : classes_)
        if (c == name) return true;
    for (auto& [key, value] : props_) {
        if (value.empty() || value == "false" || value == "0") continue;
        if (name == key) return true;
        // .key-value (toggle=mute: .toggle-mute)
        if (value != "true" && name.size() == key.size() + 1 + value.size() && name.compare(0, key.size(), key) == 0 &&
            name[key.size()] == '-' && name.compare(key.size() + 1, std::string::npos, value) == 0)
            return true;
    }
    return false;
}

void Widget::addStyleClass(const std::string& name) {
    for (auto& c : classes_)
        if (c == name) return;
    classes_.push_back(name);
    classesVersion_++;
    update();
}

void Widget::removeStyleClass(const std::string& name) {
    auto it = std::find(classes_.begin(), classes_.end(), name);
    if (it == classes_.end()) return;
    classes_.erase(it);
    classesVersion_++;
    update();
}

void Widget::styleStates(std::vector<std::string>& out) const {
    if (hovered_) out.push_back("hover");
    if (pressed_) out.push_back("pressed");
    if (!isEnabled()) out.push_back("disabled");
    if (hasFocus()) out.push_back("focus");
}

void Widget::statesChanged() {
    // a state of this one changed: the siblings after it may look different when the sheets use A + B / A ~ B
    // (their styles see it: Widget::styleShared); repaint them
    if (!parent_ || !(theme::styles().uses() & css::Engine::UsesSiblings)) return;
    bool after = false;
    for (auto& c : parent_->children_) {
        if (after) c->update();
        else if (c.get() == this) after = true;
    }
}

bool Widget::hasState(const std::string& name) const {
    std::vector<std::string> states;
    styleStates(states);
    for (auto& s : states)
        if (s == name) return true;
    return false;
}

int Widget::childIndex() const {
    if (!parent_) return -1;
    const auto& v = parent_->children_;
    if (indexHint_ >= 0 && indexHint_ < (int)v.size() && v[(size_t)indexHint_].get() == this) return indexHint_;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].get() == this) return indexHint_ = (int)i;
    return -1;
}

const css::Node* Widget::sibling(int index) const {
    if (!parent_ || index < 0 || index >= (int)parent_->children_.size()) return nullptr;
    return parent_->children_[(size_t)index].get();
}

std::shared_ptr<const css::Style> Widget::styleShared(const std::string& part) const {
    const css::Engine& engine = theme::styles();
    std::vector<std::string> states;
    styleStates(states);
    std::string key;
    for (auto& s : states) key += s + " ";
    // the parent's style first (it is what this one inherits from, and changes with the parent's own states)
    std::shared_ptr<const css::Style> up = parent_ ? parent_->styleShared("") : nullptr;
    int gen = engine.generation();
    // (its place, and the siblings before it: their order, types and classes; only if the sheets look at them)
    int uses = engine.uses(), index = -1, count = 0;
    size_t siblings = 0;
    if (uses) {
        index = childIndex();
        count = siblingCount();
        if ((uses & css::Engine::UsesSiblings) && index > 0)
            for (int i = 0; i < index; i++) {
                const Widget* w = parent_->children_[(size_t)i].get();
                siblings = (siblings * 1000003u) ^ (std::hash<const void*>()(w) + (size_t)w->classesVersion_ * 0x9E3779B9u);
                // (and their states: A:hover + B)
                std::vector<std::string> theirs;
                w->styleStates(theirs);
                for (auto& st : theirs) siblings = (siblings * 31u) ^ std::hash<std::string>()(st);
            }
    }
    for (size_t i = 0; i < styles_.size(); i++) {
        StyleEntry& e = styles_[i];
        if (e.part == part && e.states == key && e.parent == up && e.generation == gen && e.classes == classesVersion_ &&
            e.index == index && e.count == count && e.siblings == siblings) {
            if (i > 0) std::rotate(styles_.begin(), styles_.begin() + i, styles_.begin() + i + 1);  // (most recent first)
            return styles_[0].style;
        }
    }
    StyleEntry e;
    e.part = part;
    e.states = key;
    e.parent = up;
    e.generation = gen;
    e.classes = classesVersion_;
    e.index = index;
    e.count = count;
    e.siblings = siblings;
    // a part inherits from its node (Slider::handle from Slider)
    std::shared_ptr<const css::Style> base = part.empty() ? up : styleShared("");
    e.style = std::make_shared<const css::Style>(engine.compute(*this, base.get(), part));
    styles_.insert(styles_.begin(), std::move(e));
    if (styles_.size() > 12) styles_.pop_back();  // (states and parts of it seen lately)
    return styles_[0].style;
}

const css::Style& Widget::computedStyle(const std::string& part) const {
    // (the entry keeps it alive until it is pushed out: what callers hold is used at once, in one paint)
    return *styleShared(part);
}

std::string Widget::property(const std::string& key) const {
    auto it = props_.find(key);
    return it == props_.end() ? std::string() : it->second;
}

// --- painting ---
void Widget::update() { update(rect()); }

void Widget::update(const Rect& area) {
    if (deleted_ && !visible_) return;
    Rect r = area.intersected(rect());
    if (r.empty()) return;
    const Widget* c = this;
    while (c->parent_) {
        if (!c->visible_) return;
        r = r.translated(c->geom_.x, c->geom_.y);
        if (c->parent_->clipsChildren_) r = r.intersected(c->parent_->rect());
        if (r.empty()) return;
        c = c->parent_;
    }
    auto* win = dynamic_cast<const Window*>(c);
    if (!win || !win->isWindowShown()) return;
    const_cast<Window*>(win)->invalidate(r);
}

void Widget::paintTree(Painter& p, const Rect& dirty) {
    if (!visible_ || deleted_) return;
    Rect area = dirty.intersected(rect());
    if (area.empty() && clipsChildren_) return;
    p.save();
    p.setClip(rect());
    if (!area.empty()) {
        if (hasBackground_) p.fillRect(area, background_);
        paint(p);
    }
    if (!clipsChildren_) {
        p.restore();
        p.save();
    }
    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible_ || c->deleted_) continue;
        Rect cr = c->geom_;
        Rect cd = dirty.intersected(cr);
        if (cd.empty()) continue;
        p.save();
        p.translate((float)cr.x, (float)cr.y);
        c->paintTree(p, dirty.translated(-cr.x, -cr.y));
        p.restore();
    }
    if (!area.empty()) {
        p.setClip(rect());
        paintOver(p);
    }
    p.restore();
}

Widget* Widget::childAt(Point p) {
    for (size_t i = children_.size(); i-- > 0;) {
        Widget* c = children_[i].get();
        if (!c->visible_ || c->deleted_) continue;
        if (c->geom_.contains(p)) {
            Point q = p - Point(c->geom_.x, c->geom_.y);
            Widget* d = c->childAt(q);
            return d ? d : c;
        }
    }
    return nullptr;
}

void Widget::startDrag(const DragData& data, const Image* picture) {
    App::get().beginDrag(data, this);
    App& app = App::get();
    if (picture && !picture->empty() && !app.headless()) {
        // a small picture following the pointer
        plat::WindowOptions o;
        o.popup = true;
        o.takesFocus = false;
        o.resizable = false;
        o.w = picture->width();
        o.h = picture->height();
        // (style type DragImage: its box, and ::image's opacity)
        struct Pic : Window {
            Image img;
            explicit Pic(const plat::WindowOptions& o) : Window(o) { setStyleType("DragImage"); }
            void paint(Painter& p) override {
                style::box(p, rect(), computedStyle());
                float opacity = (float)computedStyle("image").number("opacity", 1);
                p.save();
                p.setOpacity(std::max(0.f, std::min(1.f, opacity)));
                p.drawImage(Point(0, 0), img);
                p.restore();
            }
        };
        auto pic = std::make_unique<Pic>(o);
        pic->img = *picture;
        Point g = app.cursorPos();
        pic->moveWindow(g.x + 12, g.y + 12);
        pic->showWindow();
        app.dragPic_ = std::move(pic);
    }
}

// ============================================================================================
// KeySequence / Shortcut
// ============================================================================================

namespace {
struct KeyName { int key; const char* name; };
const KeyName kKeyNames[] = {
    {plat::Key_Escape, "Esc"}, {plat::Key_Tab, "Tab"}, {plat::Key_Backtab, "Backtab"},
    {plat::Key_Backspace, "Backspace"}, {plat::Key_Return, "Return"}, {plat::Key_Enter, "Enter"},
    {plat::Key_Insert, "Ins"}, {plat::Key_Delete, "Del"}, {plat::Key_Pause, "Pause"}, {plat::Key_Print, "Print"},
    {plat::Key_Home, "Home"}, {plat::Key_End, "End"}, {plat::Key_Left, "Left"}, {plat::Key_Up, "Up"},
    {plat::Key_Right, "Right"}, {plat::Key_Down, "Down"}, {plat::Key_PageUp, "PgUp"}, {plat::Key_PageDown, "PgDown"},
    {plat::Key_Space, "Space"}, {plat::Key_Menu, "Menu"},
    {plat::Key_F1, "F1"}, {plat::Key_F2, "F2"}, {plat::Key_F3, "F3"}, {plat::Key_F4, "F4"}, {plat::Key_F5, "F5"},
    {plat::Key_F6, "F6"}, {plat::Key_F7, "F7"}, {plat::Key_F8, "F8"}, {plat::Key_F9, "F9"}, {plat::Key_F10, "F10"},
    {plat::Key_F11, "F11"}, {plat::Key_F12, "F12"},
};
const KeyName kKeyAliases[] = {
    {plat::Key_Escape, "escape"}, {plat::Key_Delete, "delete"}, {plat::Key_Insert, "insert"},
    {plat::Key_PageUp, "pageup"}, {plat::Key_PageDown, "pagedown"}, {plat::Key_PageDown, "pgdn"},
    {plat::Key_Return, "enter_"}, {plat::Key_Backspace, "back"}, {'+', "plus"}, {'-', "minus"},
};

std::string lowerAscii(std::string s) {
    for (auto& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    return s;
}

int parseKeyName(const std::string& tok) {
    if (tok.empty()) return 0;
    std::string l = lowerAscii(tok);
    for (auto& k : kKeyNames)
        if (lowerAscii(k.name) == l) return k.key;
    for (auto& k : kKeyAliases)
        if (l == k.name) return k.key;
    // one character
    size_t i = 0;
    uint32_t cp = utf8Next(tok, i);
    if (i == tok.size()) {
        if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 'A';
        return (int)cp;
    }
    return 0;
}
}  // namespace

KeySequence KeySequence::parse(const std::string& text0) {
    KeySequence ks;
    std::string text = text0;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.empty()) return ks;
    std::vector<std::string> toks;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            toks.push_back(cur);
            cur.clear();
        } else {
            cur += text[i];
        }
    }
    toks.push_back(cur);
    int key = 0;
    // "Ctrl++" → [Ctrl, "", ""]; "+" → ["", ""]
    if (toks.size() >= 2 && toks.back().empty()) {
        key = '+';
        while (!toks.empty() && toks.back().empty()) toks.pop_back();
    }
    int mods = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        std::string t = lowerAscii(toks[i]);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        bool last = (i + 1 == toks.size()) && key == 0;
        if (!last) {
            if (t == "ctrl" || t == "control") mods |= plat::ModCtrl;
            else if (t == "shift") mods |= plat::ModShift;
            else if (t == "alt") mods |= plat::ModAlt;
            else if (t == "meta" || t == "win" || t == "super") mods |= plat::ModMeta;
            else return KeySequence();
        } else {
            std::string raw = toks[i];
            while (!raw.empty() && raw.back() == ' ') raw.pop_back();
            while (!raw.empty() && raw.front() == ' ') raw.erase(raw.begin());
            key = parseKeyName(raw);
            if (!key) return KeySequence();
        }
    }
    ks.key = key;
    ks.mods = mods;
    return ks;
}

std::string KeySequence::text() const {
    if (!key) return "";
    std::string s;
    if (mods & plat::ModMeta) s += "Meta+";
    if (mods & plat::ModCtrl) s += "Ctrl+";
    if (mods & plat::ModAlt) s += "Alt+";
    if (mods & plat::ModShift) s += "Shift+";
    for (auto& k : kKeyNames)
        if (k.key == key) return s + k.name;
    return s + codepointToUtf8((uint32_t)key);
}

bool KeySequence::matches(const KeyEvent& e) const {
    if (!key) return false;
    const int mask = plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModMeta;
    int em = e.mods & mask;
    int ek = e.key;
    if (ek >= 'a' && ek <= 'z') ek = ek - 'a' + 'A';
    if (ek == plat::Key_Backtab && key == plat::Key_Tab) ek = plat::Key_Tab;
    if (ek != key) return false;
    if (em == mods) return true;
    // symbols typed with Shift (Ctrl++ is Ctrl+Shift+= on many keyboards): Shift doesn't count
    bool symbol = key < 0x01000000 && key != ' ' && !(key >= 'A' && key <= 'Z') && !(key >= '0' && key <= '9');
    if (symbol && !(mods & plat::ModShift) && (em & ~plat::ModShift) == mods) return true;
    if (ek == plat::Key_Tab && e.key == plat::Key_Backtab && (em | plat::ModShift) == mods) return true;
    return false;
}

Shortcut::Shortcut(Widget* owner_, KeySequence seq_, std::function<void()> fn_, ShortcutScope scope_)
    : seq(seq_), fn(std::move(fn_)), scope(scope_), owner(owner_) {
    App::get().allShortcuts.push_back(this);
}

Shortcut::~Shortcut() {
    auto& v = App::get().allShortcuts;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

// ============================================================================================
// Window
// ============================================================================================

namespace {
// the tooltip window (style type ToolTip: a box (background-color, border) with padding around the text (color,
// font-*); default.css: a 1 px edge and 4 px of padding)
class TipWindow : public Window {
public:
    explicit TipWindow(const plat::WindowOptions& o) : Window(o) { setStyleType("ToolTip"); }
    std::string text;
    // its size for that text
    Point measure(const std::string& text) const {
        const css::Style& st = computedStyle();
        Font f = style::font(st, theme::uiFont());
        style::Insets b = style::insets(st, "border-width"), pad = style::insets(st, "padding");
        float w = 0;
        int lines = 0;
        size_t start = 0;
        while (true) {
            size_t nl = text.find('\n', start);
            std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            w = std::max(w, f.width(line));
            ++lines;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        int h = (int)std::ceil(lines * f.lineSpacing() - (f.lineSpacing() - f.height()));
        return Point((int)std::ceil(w) + b.w() + pad.w(), h + b.h() + pad.h());
    }
    void paint(Painter& p) override {
        const css::Style& st = computedStyle();
        style::box(p, rect(), st);
        Rect content = style::contents(rect(), st);
        Font f = style::font(st, theme::uiFont());
        Color tc = style::foreground(st);
        float y = (float)content.y;
        size_t start = 0;
        while (true) {
            size_t nl = text.find('\n', start);
            std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            p.drawText(PointF((float)content.x, y + f.ascent()), line, f, tc);
            y += f.lineSpacing();
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
};
}  // namespace

Window::Window(const plat::WindowOptions& opts) : Widget(nullptr), opts_(opts) {
    setStyleType("Window");
    App& app = App::get();
    scale_ = app.scale();
    hgeom_ = Rect(opts.x < 0 ? 100 : opts.x, opts.y < 0 ? 100 : opts.y, std::max(1, opts.w), std::max(1, opts.h));
    title_ = opts.title;
    physW_ = toDevice(std::max(1, opts.w));
    physH_ = toDevice(std::max(1, opts.h));
    if (!app.headless()) {
        plat::WindowOptions o = opts;  // (in screen pixels)
        o.w = physW_;
        o.h = physH_;
        if (o.x >= 0) o.x = app.toScreen(o.x);
        if (o.y >= 0) o.y = app.toScreen(o.y);
        o.minW = toDevice(o.minW);
        o.minH = toDevice(o.minH);
        native_ = plat::Window::create(o, this);
    }
    setGeometry(0, 0, std::max(1, opts.w), std::max(1, opts.h));
    app.addWindow(this);
}

int Window::toDevice(int v) const { return scale_ == 1 ? v : (int)std::lround(v * (double)scale_); }

Rect Window::toDeviceRect(const Rect& r) const {
    if (scale_ == 1) return r;
    double s = scale_;
    int x0 = (int)std::floor(r.x * s), y0 = (int)std::floor(r.y * s);
    int x1 = (int)std::ceil(r.right() * s), y1 = (int)std::ceil(r.bottom() * s);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

void Window::setScale(float s) {
    if (!(s > 0) || s == scale_) return;
    int w = width(), h = height();
    scale_ = s;
    resizeWindow(w, h);
    Point m = minimumSize();
    if (native_ && (m.x > 0 || m.y > 0)) native_->setMinimumSize(toDevice(m.x), toDevice(m.y));
    invalidate(rect());
}

plat::Event Window::toLogical(const plat::Event& e) const {
    if (scale_ == 1 && App::get().scale() == 1) return e;
    plat::Event le = e;
    auto div = [](int v, double s) { return (int)std::floor(v / s); };
    switch (e.type) {
    case plat::Event::Move:  // (screen coordinates)
        le.x = App::get().fromScreen(e.x);
        le.y = App::get().fromScreen(e.y);
        break;
    case plat::Event::Resize:
        le.w = std::max(1, (int)std::lround(e.w / (double)scale_));
        le.h = std::max(1, (int)std::lround(e.h / (double)scale_));
        break;
    default:
        le.x = div(e.x, scale_);
        le.y = div(e.y, scale_);
        break;
    }
    return le;
}

Window::~Window() {
    App& app = App::get();
    if (app.toolTipWindow() != this) app.forget(this);
    if (shown_) {
        if (std::find(app.popups().begin(), app.popups().end(), this) != app.popups().end()) app.closePopup(this);
    }
    // the children go first, while this is still a Window
    while (!children_.empty()) {
        std::unique_ptr<Widget> c = std::move(children_.back());
        children_.pop_back();
        c.reset();
    }
    focus_ = hover_ = grab_ = nullptr;
    app.removeWindow(this);
    native_.reset();
}

void Window::setTitle(const std::string& t) {
    title_ = t;
    if (native_) native_->setTitle(t);
}

void Window::showWindow() {
    App& app = App::get();
    relayout();
    app.doLayouts();
    if (layout()) {
        Point m = layout()->minimumSize();
        Point mn = minimumSize();
        int mw = std::max(m.x, mn.x), mh = std::max(m.y, mn.y);
        if (native_ && (mw > 0 || mh > 0) && !opts_.popup) native_->setMinimumSize(toDevice(mw), toDevice(mh));
    }
    bool was = shown_;
    shown_ = true;
    showSerial_ = ++app.showCounter_;
    if (native_) native_->show();
    if (modal_ && !was) {
        app.grabEpoch_++;
        for (Window* w : app.windows()) if (w != this) w->releaseGrab();
        app.closeAllPopups();
    }
    if (!focus_ && opts_.dialog) focusNext(true);
    invalidate(rect());
    if (!was) {
        filterEvent("show");
        showEvent();
    }
}

void Window::hideWindow() {
    if (!shown_) return;
    App& app = App::get();
    if (app.toolTipWindow() != this) app.hideToolTip();
    shown_ = false;
    if (native_) native_->hide();
    setHover(nullptr);
    releaseGrab();
    if (app.drag() && app.dragTarget() && isAncestorOf(app.dragTarget())) app.endDrag();
    active_ = false;
    app.noteActive(this, false);
    filterEvent("hide");
    hideEvent();
}

void Window::close() {
    if (closeRequested && !closeRequested()) return;
    hideWindow();
    closed.emit();
    if (deleteOnClose) deleteLater();
}

void Window::raiseWindow() {
    if (native_) native_->raise();
    showSerial_ = ++App::get().showCounter_;
}

void Window::resizeWindow(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    hgeom_.w = w;
    hgeom_.h = h;
    physW_ = toDevice(w);
    physH_ = toDevice(h);
    if (native_) native_->resize(physW_, physH_);
    setGeometry(0, 0, w, h);
}

void Window::moveWindow(int x, int y) {
    hgeom_.x = x;
    hgeom_.y = y;
    if (native_) native_->move(App::get().toScreen(x), App::get().toScreen(y));
}

Rect Window::screenGeometry() const {
    if (native_) {
        Rect g = native_->geometry();
        if (!g.empty()) {
            App& app = App::get();
            if (app.scale() == 1) return g;
            int x = app.fromScreen(g.x), y = app.fromScreen(g.y);
            return Rect(x, y, app.fromScreen(g.right()) - x, app.fromScreen(g.bottom()) - y);
        }
    }
    return Rect(hgeom_.x, hgeom_.y, width(), height());
}

void Window::setMinimumWindowSize(int w, int h) {
    setMinimumSize(w, h);
    if (native_) native_->setMinimumSize(toDevice(w), toDevice(h));
}

void Window::centerOn(Window* other) {
    Rect area;
    if (other && other->isWindowShown()) area = other->screenGeometry();
    else area = App::get().screenAt(App::get().cursorPos());
    int x = area.x + (area.w - width()) / 2, y = area.y + (area.h - height()) / 2;
    Rect scr = App::get().screenAt(Point(area.x + area.w / 2, area.y + area.h / 2));
    if (!scr.empty()) {
        x = std::max(scr.x, std::min(x, scr.right() - width()));
        y = std::max(scr.y, std::min(y, scr.bottom() - height()));
    }
    moveWindow(x, y);
}

void Window::setFocusWidget(Widget* w) {
    if (w && (w->window() != this || w->deleted_)) return;
    if (focus_ == w) return;
    Widget* old = focus_;
    focus_ = w;
    if (old && Widget::alive(old)) {
        old->filterEvent("focusOut");
        old->focusOut();
        old->update();
        if (Widget::alive(old)) old->statesChanged();
    }
    if (w && Widget::alive(w) && focus_ == w) {
        w->filterEvent("focusIn");
        w->focusIn();
        w->update();
        if (Widget::alive(w)) w->statesChanged();
    }
    focusReason = 0;
}

void Window::flush() {
    if (!shown_) return;
    if (scale_ != 1) {
        // device pixels: the native window's size (logical × scale, give or take the rounding)
        int fw = native_ ? std::max(1, physW_) : std::max(1, toDevice(width()));
        int fh = native_ ? std::max(1, physH_) : std::max(1, toDevice(height()));
        if (fb_.width() != fw || fb_.height() != fh) {
            fb_.resize(fw, fh);
            fb_.fill(style::background(computedStyle()));
            dirty_ = rect();
        }
        if (dirty_.empty()) return;
        Rect d = dirty_.intersected(rect());
        dirty_ = Rect();
        if (d.empty()) return;
        Painter p(fb_, scale_);
        p.setClip(d);
        paintBackground(p);
        paintTree(p, d);
        if (native_) native_->present(fb_, toDeviceRect(d).intersected(fb_.rect()));
        return;
    }
    if (fb_.width() != width() || fb_.height() != height()) {
        fb_.resize(std::max(1, width()), std::max(1, height()));
        dirty_ = rect();
    }
    if (dirty_.empty()) return;
    Rect d = dirty_.intersected(rect());
    dirty_ = Rect();
    if (d.empty()) return;
    Painter p(fb_);
    p.setClip(d);
    paintBackground(p);
    paintTree(p, d);
    if (native_) native_->present(fb_, d);
}

void Window::paintBackground(Painter& p) {
    // (the painter is clipped to what is repainted; a see-through background goes over black: nothing shows
    // through a window)
    const css::Style& st = computedStyle();
    if (style::background(st).a < 255) p.fillRect(rect(), Color(0, 0, 0));
    style::box(p, rect(), st);
}

void Window::invalidate(const Rect& r) {
    // (at a fractional scale a logical pixel's edge falls inside a device pixel, and anti-aliased
    // drawing reaches a little beyond its widget: one more pixel around, so nothing stale is left)
    Rect a = scale_ == 1 || scale_ == std::floor(scale_) ? r : Rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2);
    dirty_ = dirty_.united(a.intersected(rect()));
}

bool Window::blockedByModal() const {
    if (opts_.popup) return false;
    Window* m = App::get().modalWindow();
    if (!m || m == this) return false;
    if (opts_.transientFor && m->native() && opts_.transientFor == m->native() && showSerial_ > m->showSerial_) return false;
    return true;
}

void Window::releaseGrab() {
    if (grab_ && Widget::alive(grab_)) {
        grab_->pressed_ = false;
        grab_->update();
        grab_->statesChanged();
    }
    grab_ = nullptr;
    heldButtons_ = 0;
}

void Window::applyCursor() {
    Widget* w = grab_ ? grab_ : hover_;
    Cursor c = Cursor::Arrow;
    for (; w; w = w->parent_)
        if (w->cursorSet_) { c = w->cursor_; break; }
    App& app = App::get();
    if (app.drag() && app.dragSource_ && app.dragSource_->window() == this)
        c = app.dragTarget_ && app.dragAccepted_ ? Cursor::Arrow : Cursor::Forbidden;
    if (native_) native_->setCursor(c);
}

void Window::setHover(Widget* w) {
    if (w == hover_) return;
    std::vector<Widget*> oldChain, newChain;
    for (Widget* x = hover_; x; x = x->parent_) oldChain.push_back(x);
    for (Widget* x = w; x; x = x->parent_) newChain.push_back(x);
    hover_ = w;
    App::get().toolTipLeave();
    for (Widget* x : oldChain) {
        if (std::find(newChain.begin(), newChain.end(), x) != newChain.end()) continue;
        if (!Widget::alive(x)) continue;
        x->hovered_ = false;
        x->statesChanged();
        x->filterEvent("leave");
        if (Widget::alive(x)) x->leave();
    }
    for (size_t i = newChain.size(); i-- > 0;) {
        Widget* x = newChain[i];
        if (std::find(oldChain.begin(), oldChain.end(), x) != oldChain.end()) continue;
        if (!Widget::alive(x)) continue;
        x->hovered_ = true;
        x->statesChanged();
        x->filterEvent("enter");
        if (Widget::alive(x)) x->enter();
    }
    applyCursor();
}

void Window::updateHover(Point p, const plat::Event& e) {
    Widget* w = nullptr;
    if (rect().contains(p)) {
        w = childAt(p);
        if (!w) w = this;
    }
    setHover(w);
}

namespace {
MouseEvent makeMouse(Widget* w, Point winPos, const plat::Event& e, Window* win) {
    MouseEvent me;
    Point origin = w->mapToWindow(Point(0, 0));
    me.pos = winPos - origin;
    Rect g = win->screenGeometry();
    me.globalPos = winPos + Point(g.x, g.y);
    me.button = e.button;
    me.buttons = e.buttons;
    me.mods = e.mods;
    me.time = e.time;
    return me;
}
}  // namespace

void Window::dispatchMouse(const plat::Event& e) {
    App& app = App::get();
    Point p(e.x, e.y);
    lastMouse_ = p;
    Rect sg = screenGeometry();
    app.lastCursor_ = p + Point(sg.x, sg.y);
    if (blockedByModal()) {
        if (e.type == plat::Event::MouseDown) {
            Window* m = app.modalWindow();
            if (!app.headless()) plat::beep();
            if (m) m->raiseWindow();
        }
        if (e.type == plat::Event::MouseUp) releaseGrab();
        return;
    }
    // an internal drag: the pointer goes to the drag, not to widgets
    if (app.drag() && app.dragSource_ && (e.type == plat::Event::MouseMove || e.type == plat::Event::MouseUp)) {
        if (e.type == plat::Event::MouseMove) {
            if (e.buttons == 0) {
                app.dragFinish(app.lastCursor_, e.mods, false);
            } else {
                app.dragMoveTo(app.lastCursor_, e.mods);
                applyCursor();
            }
            return;
        }
        app.dragFinish(app.lastCursor_, e.mods, true);
        // the source still gets its release (so it forgets its press)
    }
    switch (e.type) {
    case plat::Event::MouseDown: {
        app.hideToolTip();
        if (grab_ && ((e.buttons & ~e.button) == 0 || !Widget::alive(grab_))) releaseGrab();
        Widget* target = grab_;
        if (!target) {
            updateHover(p, e);
            target = childAt(p);
            if (!target) target = this;
        }
        double dct = app.headless() ? 0.4 : plat::doubleClickTime();
        int dcd = app.headless() ? 4 : plat::doubleClickDistance();
        bool dbl = e.button == lastClickButton_ && e.time - lastClickTime_ < dct &&
                   std::abs(p.x - lastClickPos_.x) <= dcd && std::abs(p.y - lastClickPos_.y) <= dcd;
        if (dbl) {
            lastClickTime_ = -100;
        } else {
            lastClickTime_ = e.time;
            lastClickPos_ = p;
            lastClickButton_ = e.button;
        }
        heldButtons_ = e.buttons | e.button;
        if (grab_) {  // another button while one is held: to the same widget
            MouseEvent me = makeMouse(grab_, p, e, this);
            if (grab_->isEnabled() && !grab_->filterEvent(kMouseNames[dbl])) {
                if (dbl) grab_->mouseDoubleClick(me);
                else grab_->mousePress(me);
            }
            return;
        }
        // focus on click
        for (Widget* w = target; w; w = w->parent_) {
            if (!w->isEnabled()) continue;
            Widget* f = w;
            for (int guard = 0; f->focusProxy && guard < 8; ++guard) f = f->focusProxy;
            if (f->focusPolicy_ == FocusPolicy::Click || f->focusPolicy_ == FocusPolicy::Strong) {
                if (focus_ != f) {
                    focusReason = 1;
                    setFocusWidget(f);
                }
                break;
            }
        }
        int epoch = app.grabEpoch_;
        Widget* handler = nullptr;
        for (Widget* w = target; w;) {
            Widget* parent = w->parent_;
            if (w->isEnabled()) {
                MouseEvent me = makeMouse(w, p, e, this);
                bool handled = w->filterEvent(kMouseNames[dbl]);
                if (!handled && Widget::alive(w)) handled = dbl ? w->mouseDoubleClick(me) : w->mousePress(me);
                if (!Widget::alive(w)) return;
                if (handled) {
                    handler = w;
                    break;
                }
            }
            w = parent;
        }
        if (!Widget::alive(target)) return;
        if (epoch == app.grabEpoch_ && shown_) {
            grab_ = handler ? handler : target;
            grab_->pressed_ = true;
            grab_->update();
            grab_->statesChanged();
            applyCursor();
        }
        if (e.button == plat::RightButton && Widget::alive(target) && epoch == app.grabEpoch_) {
            for (Widget* w = target; w;) {
                Widget* parent = w->parent_;
                if (w->isEnabled()) {
                    MouseEvent me = makeMouse(w, p, e, this);
                    bool handled = w->filterEvent("contextMenu");
                    if (!handled && Widget::alive(w)) handled = w->contextMenu(me);
                    if (!Widget::alive(w) || handled) break;
                }
                w = parent;
            }
        }
        break;
    }
    case plat::Event::MouseUp: {
        heldButtons_ = e.buttons & ~e.button;
        Widget* g = grab_;
        if (g && Widget::alive(g)) {
            MouseEvent me = makeMouse(g, p, e, this);
            if (g->isEnabled() && !g->filterEvent("mouseRelease")) g->mouseRelease(me);
        }
        if (heldButtons_ == 0) {
            if (grab_ == g) releaseGrab();
            if (shown_) updateHover(p, e);
            applyCursor();
        }
        break;
    }
    case plat::Event::MouseMove: {
        if (grab_ && (e.buttons == 0 || !Widget::alive(grab_))) {
            releaseGrab();
            applyCursor();
        }
        if (grab_) {
            Widget* g = grab_;
            MouseEvent me = makeMouse(g, p, e, this);
            if (g->isEnabled() && !g->filterEvent("mouseMove")) g->mouseMove(me);
            return;
        }
        updateHover(p, e);
        if (e.buttons == 0) {
            app.toolTipWake(hover_, app.lastCursor_);
            app.hintWake(this);
        }
        for (Widget* w = hover_; w;) {
            Widget* parent = w->parent_;
            if (w->tracking_ && w->isEnabled()) {
                MouseEvent me = makeMouse(w, p, e, this);
                bool handled = w->filterEvent("mouseMove");
                if (!handled && Widget::alive(w)) handled = w->mouseMove(me);
                if (!Widget::alive(w) || handled) break;
            }
            w = parent;
        }
        break;
    }
    default:
        break;
    }
}

void Window::dispatchWheel(const plat::Event& e) {
    App& app = App::get();
    if (blockedByModal()) return;
    app.hideToolTip();
    Point p(e.x, e.y);
    Widget* target = childAt(p);
    if (!target) target = this;
    Rect sg = screenGeometry();
    for (Widget* w = target; w;) {
        Widget* parent = w->parent_;
        if (w->isEnabled()) {
            WheelEvent we;
            we.pos = p - w->mapToWindow(Point(0, 0));
            we.globalPos = p + Point(sg.x, sg.y);
            we.dx = e.dx;
            we.dy = e.dy;
            we.mods = e.mods;
            bool handled = w->filterEvent("wheel");
            if (!handled && Widget::alive(w)) handled = w->wheel(we);
            if (!Widget::alive(w) || handled) return;
        }
        w = parent;
    }
}

void Window::dispatchKey(const plat::Event& e) {
    App& app = App::get();
    KeyEvent ke;
    ke.key = e.key;
    ke.mods = e.mods;
    ke.text = e.text;
    ke.autoRepeat = e.autoRepeat;
    if (ke.key >= 'a' && ke.key <= 'z') ke.key = ke.key - 'a' + 'A';
    if (e.type == plat::Event::KeyDown) app.hideToolTip();
    if (app.drag() && app.dragSource_ && e.type == plat::Event::KeyDown && ke.key == plat::Key_Escape) {
        app.dragFinish(app.cursorPos(), e.mods, false);
        return;
    }
    if (app.keyFilter) {
        Window* self = this;
        if (app.keyFilter(self, ke, e.type == plat::Event::KeyDown)) return;
    }
    if (blockedByModal()) return;
    Widget* f = focus_;
    if (f && (!f->isShown() || !f->isEnabled())) f = nullptr;
    if (e.type == plat::Event::KeyUp) {
        for (Widget* w = f ? f : this; w;) {
            Widget* parent = w->parent_;
            if (w->isEnabled()) {
                bool handled = w->filterEvent("keyRelease");
                if (!handled && Widget::alive(w)) handled = w->keyRelease(ke);
                if (!Widget::alive(w) || handled) return;
            }
            w = parent;
        }
        return;
    }
    bool overridden = false;
    for (Widget* w = f; w; w = w->parent_)
        if (w->overridesShortcut(ke)) { overridden = true; break; }
    if (!overridden && !opts_.popup && app.triggerShortcut(this, ke)) return;
    for (Widget* w = f ? f : this; w;) {
        Widget* parent = w->parent_;
        if (w->isEnabled()) {
            bool handled = w->filterEvent("keyPress");
            if (!handled && Widget::alive(w)) handled = w->keyPress(ke);
            if (!Widget::alive(w) || handled) return;
        }
        w = parent;
    }
    if (overridden && !opts_.popup && app.triggerShortcut(this, ke)) return;
    if ((ke.key == plat::Key_Tab || ke.key == plat::Key_Backtab) && !(ke.mods & (plat::ModCtrl | plat::ModAlt))) {
        focusNext(ke.key == plat::Key_Tab && !(ke.mods & plat::ModShift));
    }
}

void Window::focusNext(bool forward) {
    std::vector<Widget*> chain;
    std::function<void(Widget*)> walk = [&](Widget* w) {
        if (!w->visible_ || w->deleted_ || !w->enabled_) return;
        if (w != this && !w->focusProxy &&
            (w->focusPolicy_ == FocusPolicy::Tab || w->focusPolicy_ == FocusPolicy::Strong))
            chain.push_back(w);
        for (auto& c : w->children_) walk(c.get());
    };
    walk(this);
    if (chain.empty()) return;
    int i = -1;
    for (size_t k = 0; k < chain.size(); ++k)
        if (chain[k] == focus_) i = (int)k;
    int n = (int)chain.size();
    int next = i < 0 ? (forward ? 0 : n - 1) : ((i + (forward ? 1 : -1)) % n + n) % n;
    focusReason = 2;
    setFocusWidget(chain[next]);
}

void Window::externalDrag(const plat::Event& e) {
    App& app = App::get();
    Rect sg = screenGeometry();
    Point g = Point(e.x, e.y) + Point(sg.x, sg.y);
    if (app.dragSource_) return;  // (an internal drag is going on)
    switch (e.type) {
    case plat::Event::DragEnter:
        app.drag_ = DragData();
        app.drag_.files = e.files;
        app.drag_.kind = "files";
        app.dragActive_ = true;
        app.dragSource_ = nullptr;
        app.dragTarget_ = nullptr;
        app.dragAccepted_ = false;
        app.dragMoveTo(g, e.mods);
        break;
    case plat::Event::DragMove:
        if (!app.dragActive_) {
            app.drag_ = DragData();
            app.drag_.kind = "files";
            app.dragActive_ = true;
        }
        if (!e.files.empty()) app.drag_.files = e.files;
        app.dragMoveTo(g, e.mods);
        break;
    case plat::Event::DragLeave:
        if (app.dragActive_) app.dragFinish(g, e.mods, false);
        break;
    case plat::Event::Drop:
        if (!app.dragActive_) {
            app.drag_ = DragData();
            app.drag_.kind = "files";
            app.dragActive_ = true;
        }
        if (!e.files.empty()) app.drag_.files = e.files;
        app.dragFinish(g, e.mods, true);
        break;
    default:
        break;
    }
}

void Window::onEvent(plat::Window*, const plat::Event& physical) {
    App& app = App::get();
    if (physical.type == plat::Event::Resize) {
        physW_ = physical.w;
        physH_ = physical.h;
    }
    const plat::Event e = toLogical(physical);  // (the interface size: in logical pixels from here on)
    switch (e.type) {
    case plat::Event::MouseDown:
    case plat::Event::MouseUp:
    case plat::Event::MouseMove:
    case plat::Event::Wheel: {
        if (!app.popupStack_.empty()) {
            // popups take the mouse: to the one under the pointer; a press outside closes them
            Rect sg = screenGeometry();
            Point g = Point(e.x, e.y) + Point(sg.x, sg.y);
            app.lastCursor_ = g;
            for (size_t i = app.popupStack_.size(); i-- > 0;) {
                Window* pw = app.popupStack_[i];
                Rect pg = pw->screenGeometry();
                if (pg.contains(g)) {
                    plat::Event te = e;
                    te.x = g.x - pg.x;
                    te.y = g.y - pg.y;
                    if (e.type == plat::Event::Wheel) pw->dispatchWheel(te);
                    else pw->dispatchMouse(te);
                    app.collectGarbage();
                    return;
                }
            }
            Widget* owner = app.popupInfo_.empty() ? nullptr : app.popupInfo_.front().first;
            bool onOwner = false;
            if (owner && Widget::alive(owner) && owner->isShown()) {
                Point lp = owner->mapFromGlobal(g);
                onOwner = owner->rect().contains(lp);
            }
            if (e.type == plat::Event::MouseDown) {
                app.closeAllPopups();
                if (!onOwner) {
                    // the press goes on to what is under it (as Qt replays it)
                    Window* under = app.windowAt(g);
                    if (under && !under->isPopup()) {
                        Rect ug = under->screenGeometry();
                        plat::Event te = e;
                        te.x = g.x - ug.x;
                        te.y = g.y - ug.y;
                        under->dispatchMouse(te);
                    }
                }
            } else if (e.type == plat::Event::MouseMove || e.type == plat::Event::MouseUp) {
                if (onOwner && e.type == plat::Event::MouseMove && owner->hasMouseTracking()) {
                    MouseEvent me;
                    me.pos = owner->mapFromGlobal(g);
                    me.globalPos = g;
                    me.buttons = e.buttons;
                    me.mods = e.mods;
                    me.time = e.time;
                    owner->mouseMove(me);
                }
                if (!app.popupStack_.empty()) {
                    Window* top = app.popupStack_.back();
                    Rect pg = top->screenGeometry();
                    plat::Event te = e;
                    te.x = g.x - pg.x;
                    te.y = g.y - pg.y;
                    top->dispatchMouse(te);
                }
            }
            app.collectGarbage();
            return;
        }
        if (e.type == plat::Event::Wheel) dispatchWheel(e);
        else dispatchMouse(e);
        break;
    }
    case plat::Event::KeyDown:
    case plat::Event::KeyUp:
        if (!app.popupStack_.empty()) {
            app.popupStack_.back()->dispatchKey(e);
        } else {
            dispatchKey(e);
        }
        break;
    case plat::Event::Enter:
        break;
    case plat::Event::Leave:
        if (!grab_) setHover(nullptr);
        app.toolTipLeave();
        app.hintLeave(this);
        break;
    case plat::Event::FocusIn:
        active_ = true;
        app.noteActive(this, true);
        if (focus_) focus_->update();
        activated.emit();
        break;
    case plat::Event::FocusOut:
        active_ = false;
        app.noteActive(this, false);
        if (focus_) focus_->update();
        break;
    case plat::Event::Resize:
        if (e.w > 0 && e.h > 0 && (e.w != width() || e.h != height())) {
            hgeom_.w = e.w;
            hgeom_.h = e.h;
            setGeometry(0, 0, e.w, e.h);
            invalidate(rect());
        } else if (scale_ != 1 && (fb_.width() != physW_ || fb_.height() != physH_)) {
            invalidate(rect());  // (the same logical size, another device size: a new framebuffer)
        }
        break;
    case plat::Event::Move:
        hgeom_.x = e.x;
        hgeom_.y = e.y;
        break;
    case plat::Event::Expose:
        invalidate(rect());
        break;
    case plat::Event::Close:
        if (opts_.popup) break;
        if (blockedByModal()) break;
        close();
        break;
    case plat::Event::DragEnter:
    case plat::Event::DragMove:
    case plat::Event::DragLeave:
    case plat::Event::Drop:
        if (!blockedByModal()) externalDrag(e);
        break;
    default:
        break;
    }
    app.collectGarbage();
}

// ============================================================================================
// App
// ============================================================================================

App& App::get() {
    static App app;
    return app;
}

App::~App() {
    // at exit: the windows still around are not destroyed (their destructors would need the App)
    tip_.release();
    dragPic_.release();
}

void App::init(int argc, char** argv) {
    if (inited_) return;
    inited_ = true;
    plat::init(argc, argv);
    theme::c("window");  // the default skin, until one is chosen
    startFrameTimer();
}

void App::startFrameTimer() {
    // one tick per picture of the (fastest) screen; a timer of whole milliseconds would beat against
    // it (16 ms: 62.5 a second on a 60 Hz screen, a picture shown twice now and then)
    if (frameTimer_ >= 0) cancelTimer(frameTimer_);
    double hz = headless_ ? 0 : plat::displayRefreshRate();
    displayHz_ = hz >= 20 && hz <= 1000 ? hz : 60;
    lastFrameFns_ = -1;
    frameTimer_ = addTimerMs(1000.0 / displayHz_, [this] { frameTimer(); }, true);
}

void App::frameTimer() {
    // the onFrame callbacks (playhead, meters, plugins' idle) at the chosen rate, in step with the
    // screen's pictures (60 a second on a 180 Hz screen: every third); painting at every picture
    double t = now();
    double period = fps_ > 0 ? 1.0 / fps_ : 0;
    if (lastFrameFns_ < 0 || t - lastFrameFns_ >= period - 0.5 / displayHz_) {
        lastFrameFns_ = lastFrameFns_ < 0 || t - lastFrameFns_ > period + 1.0 / displayHz_ ? t : lastFrameFns_ + period;
        tick();
        return;
    }
    collectGarbage();
    doLayouts();
    for (Window* w : std::vector<Window*>(windows_))
        if (std::find(windows_.begin(), windows_.end(), w) != windows_.end()) w->flush();
}

void App::initHeadless() {
    headless_ = true;
    inited_ = true;
    clock_ = 0;
    theme::c("window");
}

double App::now() const { return headless_ ? clock_ : plat::now(); }

int App::exec() {
    depth_ = 1;
    quit_ = false;
    if (!headless_) plat::run();
    depth_ = 0;
    return 0;
}

void App::quit() {
    quit_ = true;
    if (!headless_) plat::quit();
}

void App::runUntil(const std::function<bool()>& done) {
    ++depth_;
    if (headless_) {
        // tests: only timers can end it; give up after a minute of virtual time
        for (int i = 0; i < 6000 && !done() && !quit_; ++i) {
            advanceTimers(0.01);
            collectGarbage();
        }
    } else {
        while (!done() && !plat::quitting()) {
            plat::processEvents(-1);
            collectGarbage();
        }
    }
    --depth_;
}

void App::processEvents() {
    ++depth_;
    if (!headless_) plat::processEvents(0);
    doLayouts();
    for (Window* w : std::vector<Window*>(windows_))
        if (std::find(windows_.begin(), windows_.end(), w) != windows_.end()) w->flush();
    collectGarbage();
    --depth_;
}

int App::onFrame(std::function<void()> fn) {
    int id = ++nextFrame_;
    frames_[id] = std::move(fn);
    return id;
}

void App::removeFrame(int id) { frames_.erase(id); }

void App::setFrameRate(int fps) {
    fps_ = std::max(0, fps);
    if (frameTimer_ >= 0) startFrameTimer();  // (the screen may have changed too)
}

int App::addTimer(int ms, std::function<void()> fn, bool repeat) { return addTimerMs(ms, std::move(fn), repeat); }

int App::addTimerMs(double ms, std::function<void()> fn, bool repeat) {
    int id = ++nextTimer_;
    Timer t;
    t.fn = std::move(fn);
    t.repeat = repeat;
    t.interval = std::max(ms, repeat ? 1.0 : 0.0) / 1000.0;
    t.due = now() + t.interval;
    if (!headless_) t.plat = plat::addTimerMs(std::max(0.0, ms), [this, id] { fireTimer(id); }, repeat);
    timers_[id] = std::move(t);
    return id;
}

void App::fireTimer(int id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) return;
    std::function<void()> fn = it->second.fn;
    if (!it->second.repeat) timers_.erase(it);
    else it->second.due += it->second.interval;
    if (fn) fn();
    collectGarbage();
}

int App::singleShot(int ms, std::function<void()> fn) { return addTimer(ms, std::move(fn), false); }
int App::every(int ms, std::function<void()> fn) { return addTimer(ms, std::move(fn), true); }

void App::cancelTimer(int id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) return;
    if (it->second.plat >= 0) plat::removeTimer(it->second.plat);
    timers_.erase(it);
}

void App::advanceTimers(double seconds) {
    double target = clock_ + std::max(0.0, seconds);
    for (int guard = 0; guard < 100000; ++guard) {
        int best = -1;
        double due = 0;
        for (auto& kv : timers_)
            if (kv.second.due <= target + 1e-9 && (best < 0 || kv.second.due < due)) {
                best = kv.first;
                due = kv.second.due;
            }
        if (best < 0) break;
        clock_ = std::max(clock_, due);
        fireTimer(best);
    }
    clock_ = target;
}

Window* App::activeWindow() const {
    if (active_ && std::find(windows_.begin(), windows_.end(), active_) != windows_.end()) return active_;
    return nullptr;
}

void App::noteActive(Window* w, bool on) {
    if (on) {
        active_ = w;
    } else if (active_ == w) {
        active_ = nullptr;
        // (the focus may only be going to another of our windows: deactivated if none has it a moment later)
        if (!headless_)
            singleShot(150, [this] {
                if (!active_) deactivated.emit();
            });
    }
}

Window* App::modalWindow() const {
    Window* best = nullptr;
    for (Window* w : windows_)
        if (w->isModal() && w->isWindowShown() && (!best || w->showSerial_ > best->showSerial_)) best = w;
    return best;
}

void App::repaintAll() {
    for (Window* w : windows_) {
        w->update();
        std::function<void(Widget*)> all = [&](Widget* x) {
            x->updateGeometry();
            x->resized();  // (when its size doesn't change, the layout won't call it: its insides may move anyway)
            for (auto& c : x->children()) all(c.get());
        };
        all(w);
    }
}

Window* App::windowAt(Point g) const {
    Window* best = nullptr;
    for (Window* w : windows_) {
        if (!w->isWindowShown() || w == tip_.get() || w == dragPic_.get()) continue;
        if (!w->screenGeometry().contains(g)) continue;
        if (!best || w->showSerial_ > best->showSerial_) best = w;
    }
    return best;
}

Rect App::screenAt(Point g) const {
    if (headless_) return Rect(0, 0, 1920, 1080);
    if (scale_ != 1) {
        Rect r = plat::screenGeometry(Point(toScreen(g.x), toScreen(g.y)));
        if (r.empty()) r = plat::primaryScreenGeometry();
        int x = fromScreen(r.x), y = fromScreen(r.y);
        return Rect(x, y, fromScreen(r.right()) - x, fromScreen(r.bottom()) - y);
    }
    Rect r = plat::screenGeometry(g);
    return r.empty() ? plat::primaryScreenGeometry() : r;
}

Point App::cursorPos() const {
    if (headless_) return lastCursor_;
    Point p = plat::cursorPos();
    return scale_ == 1 ? p : Point(fromScreen(p.x), fromScreen(p.y));
}

void App::setScale(float s) { scale_ = s > 0 && std::isfinite(s) ? s : 1.f; }

int App::toScreen(int v) const { return scale_ == 1 ? v : (int)std::lround(v * (double)scale_); }

int App::fromScreen(int v) const { return scale_ == 1 ? v : (int)std::floor(v / (double)scale_); }

// --- drags ---
void App::beginDrag(const DragData& d, Widget* source) {
    drag_ = d;
    if (drag_.kind.empty()) drag_.kind = drag_.files.empty() ? "text" : "files";
    dragActive_ = true;
    dragSource_ = source;
    dragTarget_ = nullptr;
    dragAccepted_ = false;
    hideToolTip();
    dragMoveTo(cursorPos(), headless_ ? 0 : plat::heldModifiers());
    if (source && source->window()) source->window()->applyCursor();
}

void App::followPointerDrag(const DragData& d) {
    if (headless_ && !pointerForDrags) return;
    if (followTimer_ >= 0) cancelTimer(followTimer_);
    if (dragActive_) dragFinish(cursorPos(), 0, false);
    beginDrag(d, nullptr);
    followTimer_ = every(16, [this] {
        if (!dragActive_) {  // (ended some other way)
            cancelTimer(followTimer_);
            followTimer_ = -1;
            return;
        }
        std::pair<Point, int> at = pointerForDrags ? pointerForDrags()
                                                   : std::make_pair(cursorPos(), plat::heldButtons());
        int mods = headless_ ? 0 : plat::heldModifiers();
        if (!(at.second & plat::LeftButton)) {  // let go: dropped here
            cancelTimer(followTimer_);
            followTimer_ = -1;
            dragFinish(at.first, mods, true);
            return;
        }
        dragMoveTo(at.first, mods);
    });
}

void App::endDrag() {
    Window* src = dragSource_ && Widget::alive(dragSource_) ? dragSource_->window() : nullptr;
    dragActive_ = false;
    dragSource_ = nullptr;
    dragTarget_ = nullptr;
    dragAccepted_ = false;
    drag_ = DragData();
    dragPic_.reset();
    if (src) src->applyCursor();
}

void App::dragMoveTo(Point g, int mods) {
    if (!dragActive_) return;
    lastCursor_ = g;
    if (dragPic_) dragPic_->moveWindow(g.x + 12, g.y + 12);
    Window* win = windowAt(g);
    Widget* t = nullptr;
    if (win && !win->blockedByModal() && !win->isPopup()) {
        Rect sg = win->screenGeometry();
        Point lp = g - Point(sg.x, sg.y);
        Widget* w = win->childAt(lp);
        if (!w) w = win;
        while (w && !(w->acceptsDrops() && w->isEnabled())) w = w->parent();
        t = w;
    }
    DragEvent de;
    de.globalPos = g;
    de.mods = mods;
    de.data = &drag_;
    if (t != dragTarget_) {
        if (dragTarget_ && Widget::alive(dragTarget_)) dragTarget_->dragLeave();
        dragTarget_ = t;
        dragAccepted_ = false;
        if (t) {
            de.pos = t->mapFromGlobal(g);
            t->dragEnter(de);
            dragAccepted_ = de.accepted;
        }
    } else if (t) {
        de.pos = t->mapFromGlobal(g);
        de.accepted = dragAccepted_;
        t->dragMove(de);
        dragAccepted_ = de.accepted;
    }
}

void App::dragFinish(Point g, int mods, bool dropIt) {
    if (!dragActive_) return;
    if (dropIt) dragMoveTo(g, mods);
    Widget* t = dragTarget_ && Widget::alive(dragTarget_) ? dragTarget_ : nullptr;
    bool accepted = dragAccepted_;
    DragData data = drag_;
    endDrag();
    if (!t) return;
    if (dropIt && accepted) {
        DragEvent de;
        de.globalPos = g;
        de.pos = t->mapFromGlobal(g);
        de.mods = mods;
        de.data = &data;
        de.accepted = true;
        t->drop(de);
    } else {
        t->dragLeave();
    }
}

// --- tooltips ---
void App::showToolTip(Point g, const std::string& text, Widget* owner) {
    if (text.empty()) {
        hideToolTip();
        return;
    }
    if (tipTimer_ >= 0) {
        cancelTimer(tipTimer_);
        tipTimer_ = -1;
    }
    if (!tip_) {
        plat::WindowOptions o;
        o.popup = true;
        o.takesFocus = false;
        o.resizable = false;
        o.title = "tooltip";
        o.w = 10;
        o.h = 10;
        tip_ = std::make_unique<TipWindow>(o);
    }
    auto* tw = static_cast<TipWindow*>(tip_.get());
    tw->text = text;
    tipText_ = text;
    Point sz = tw->measure(text);
    Rect scr = screenAt(g);
    int x = g.x + 2, y = g.y + 18;
    if (x + sz.x > scr.right()) x = std::max(scr.x, scr.right() - sz.x);
    if (y + sz.y > scr.bottom()) y = std::max(scr.y, g.y - sz.y - 4);
    tw->resizeWindow(sz.x, sz.y);
    tw->moveWindow(x, y);
    tw->update();
    if (!tw->isWindowShown()) tw->showWindow();
    else tw->raiseWindow();
    tipOwner_ = owner;
}

void App::hideToolTip() {
    if (tipTimer_ >= 0) {
        cancelTimer(tipTimer_);
        tipTimer_ = -1;
    }
    if (tip_ && tip_->isWindowShown()) {
        tip_->hideWindow();
        tipHiddenAt_ = now();
    }
    tipOwner_ = nullptr;
    tipText_.clear();
}

void App::toolTipLeave() {
    if (tipTimer_ >= 0) {
        cancelTimer(tipTimer_);
        tipTimer_ = -1;
    }
    if (tip_ && tip_->isWindowShown()) hideToolTip();
}

void App::toolTipWake(Widget* w, Point g) {
    if (!w || drag()) return;
    auto tipAt = [](Widget* x, Point gp, Widget** who) {
        for (; x; x = x->parent()) {
            std::string t = x->toolTipAt(x->mapFromGlobal(gp));
            if (!t.empty()) {
                *who = x;
                return t;
            }
        }
        *who = nullptr;
        return std::string();
    };
    if (tip_ && tip_->isWindowShown()) {
        Widget* who = nullptr;
        std::string t = tipAt(w, g, &who);
        if (t.empty()) hideToolTip();
        else if (t != tipText_ || who != tipOwner_) showToolTip(g, t, who);
        return;
    }
    if (tipTimer_ >= 0) cancelTimer(tipTimer_);
    int delay = now() - tipHiddenAt_ < 1.0 ? 80 : 700;
    tipTimer_ = singleShot(delay, [this, tipAt] {
        tipTimer_ = -1;
        if (drag()) return;
        Point g2 = lastCursor_;
        Window* win = windowAt(g2);
        if (!win || win->isPopup() || (!headless_ && plat::heldButtons())) return;
        Rect sg = win->screenGeometry();
        Point lp = g2 - Point(sg.x, sg.y);
        Widget* x = win->childAt(lp);
        if (!x) x = win;
        Widget* who = nullptr;
        std::string t = tipAt(x, g2, &who);
        if (!t.empty()) showToolTip(g2, t, who);
    });
}

// --- the hint line (hintChanged) ---
void App::sendHint(Window* w, const std::string& text) {
    if (w == hintWindow_ && text == hintText_) return;
    if (hintWindow_ && hintWindow_ != w && !hintText_.empty()) {  // (the old window's hint goes first)
        Window* old = hintWindow_;
        hintWindow_ = nullptr;
        hintText_.clear();
        hintChanged.emit(old, std::string());
    }
    hintWindow_ = text.empty() ? nullptr : w;
    hintText_ = text;
    hintChanged.emit(w, text);
}

void App::hintWake(Window* w) {
    // (moving on: a pending look stays pending, so a hint follows a moving pointer every HINT_MS)
    if (!w || hintTimer_ >= 0 || hintChanged.empty()) return;
    hintTimer_ = singleShot(HINT_MS, [this] {
        hintTimer_ = -1;
        if (hintChanged.empty()) return;
        Point g = lastCursor_;
        Window* win = windowAt(g);
        if (!win || win->isPopup()) return;  // (a menu: the hint stays as it was)
        Rect sg = win->screenGeometry();
        Widget* x = win->childAt(g - Point(sg.x, sg.y));
        if (!x) x = win;
        std::string text;
        for (; x && text.empty(); x = x->parent()) text = x->toolTipAt(x->mapFromGlobal(g));
        size_t eol = text.find('\n');
        if (eol != std::string::npos) text.resize(eol);
        sendHint(win, text);
    });
}

void App::hintLeave(Window* w) {
    if (hintTimer_ >= 0) {
        cancelTimer(hintTimer_);
        hintTimer_ = -1;
    }
    if (w && w == hintWindow_) sendHint(w, std::string());
}

// --- popups ---
void App::openPopup(Window* popup, Widget* owner, std::function<void()> dismiss) {
    hideToolTip();
    auto it = std::find(popupStack_.begin(), popupStack_.end(), popup);
    if (it == popupStack_.end()) {
        popupStack_.push_back(popup);
        popupInfo_.push_back({owner, std::move(dismiss)});
    }
    grabEpoch_++;
    for (Window* w : windows_)
        if (w != popup) w->releaseGrab();
    popup->showWindow();
    popup->raiseWindow();
    if (popup->native()) popup->native()->grabPointer(true);
}

void App::closePopup(Window* popup) {
    auto it = std::find(popupStack_.begin(), popupStack_.end(), popup);
    if (it == popupStack_.end()) return;
    size_t i = it - popupStack_.begin();
    popupStack_.erase(it);
    popupInfo_.erase(popupInfo_.begin() + i);
    if (popup->native()) popup->native()->grabPointer(false);
    popup->hideWindow();
    if (!popupStack_.empty() && popupStack_.back()->native()) popupStack_.back()->native()->grabPointer(true);
}

void App::closeAllPopups() {
    for (int guard = 0; guard < 64 && !popupStack_.empty(); ++guard) {
        Window* top = popupStack_.back();
        std::function<void()> dismiss = popupInfo_.back().second;
        if (dismiss) dismiss();
        if (!popupStack_.empty() && popupStack_.back() == top) closePopup(top);
    }
}

Widget* App::popupOwner(Window* popup) const {
    for (size_t i = 0; i < popupStack_.size(); ++i)
        if (popupStack_[i] == popup) return popupInfo_[i].first;
    return nullptr;
}

// --- layouts ---
void App::scheduleLayout(Widget* w) {
    if (!w || w->layoutDirty_ || !Widget::alive(w)) return;
    w->layoutDirty_ = true;
    pendingLayouts_.push_back(w);
}

void App::doLayouts() {
    for (int round = 0; round < 16 && !pendingLayouts_.empty(); ++round) {
        std::vector<Widget*> list;
        list.swap(pendingLayouts_);
        auto depth = [](Widget* w) {
            int d = 0;
            for (; w; w = w->parent()) ++d;
            return d;
        };
        std::vector<std::pair<int, Widget*>> sorted;
        for (Widget* w : list) sorted.push_back({depth(w), w});
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const std::pair<int, Widget*>& a, const std::pair<int, Widget*>& b) { return a.first < b.first; });
        for (auto& dw : sorted) {
            Widget* w = dw.second;
            if (!Widget::alive(w) || !w->layoutDirty_) continue;
            w->relayout();
        }
    }
}

// --- windows, deletion ---
void App::addWindow(Window* w) { windows_.push_back(w); }

void App::removeWindow(Window* w) {
    windows_.erase(std::remove(windows_.begin(), windows_.end(), w), windows_.end());
    for (size_t i = 0; i < popupStack_.size(); ++i)
        if (popupStack_[i] == w) {
            popupStack_.erase(popupStack_.begin() + i);
            popupInfo_.erase(popupInfo_.begin() + i);
            break;
        }
    if (active_ == w) active_ = nullptr;
    if (mainWindow == w) mainWindow = nullptr;
    if (hintWindow_ == w) {  // (gone: nothing to tell it)
        hintWindow_ = nullptr;
        hintText_.clear();
    }
}

void App::detach(Widget* w) {
    for (Window* win : windows_) {
        if (win->hover_ && w->isAncestorOf(win->hover_)) win->setHover(w->parent() && win->isAncestorOf(w) ? w->parent() : nullptr);
        if (win->grab_ && w->isAncestorOf(win->grab_)) win->releaseGrab();
        if (win->focus_ && w->isAncestorOf(win->focus_)) win->setFocusWidget(nullptr);
    }
    if (tipOwner_ && w->isAncestorOf(tipOwner_)) hideToolTip();
    if (dragTarget_ && w->isAncestorOf(dragTarget_)) dragTarget_ = nullptr;
}

void App::forget(Widget* w) {
    for (Window* win : windows_) {
        if (win->hover_ == w) win->hover_ = w->parent() && Widget::alive(w->parent()) ? w->parent() : nullptr;
        if (win->grab_ == w) {
            win->grab_ = nullptr;
            win->heldButtons_ = 0;
        }
        if (win->focus_ == w) win->focus_ = nullptr;
    }
    if (tipOwner_ == w) {
        tipOwner_ = nullptr;
        if (tip_ && tip_.get() != w && tip_->isWindowShown()) tip_->hideWindow();
    }
    if (dragSource_ == w) dragSource_ = nullptr;
    if (dragTarget_ == w) dragTarget_ = nullptr;
    for (Shortcut* s : allShortcuts)
        if (s->owner == w) s->owner = nullptr;
    for (auto& pi : popupInfo_)
        if (pi.first == w) pi.first = nullptr;
    trash.erase(std::remove_if(trash.begin(), trash.end(), [w](const std::pair<Widget*, int>& t) { return t.first == w; }),
                trash.end());
    pendingLayouts_.erase(std::remove(pendingLayouts_.begin(), pendingLayouts_.end(), w), pendingLayouts_.end());
}

void App::collectGarbage() {
    for (int guard = 0; guard < 100000; ++guard) {
        size_t i = 0;
        for (; i < trash.size(); ++i)
            if (trash[i].second >= depth_) break;
        if (i >= trash.size()) break;
        Widget* w = trash[i].first;
        trash.erase(trash.begin() + i);
        if (!Widget::alive(w)) continue;
        if (w->parent()) {
            std::unique_ptr<Widget> up = w->parent()->release(w);
            up.reset();
        } else {
            delete w;
        }
    }
}

void App::tick() {
    std::vector<std::function<void()>> fns;
    fns.reserve(frames_.size());
    for (auto& kv : frames_) fns.push_back(kv.second);
    for (auto& f : fns)
        if (f) f();
    collectGarbage();
    doLayouts();
    for (Window* w : std::vector<Window*>(windows_))
        if (std::find(windows_.begin(), windows_.end(), w) != windows_.end()) w->flush();
}

bool App::triggerShortcut(Window* from, const KeyEvent& e) {
    int k = e.key;
    if (k == plat::Key_Shift || k == plat::Key_Control || k == plat::Key_Alt || k == plat::Key_Meta || k == 0) return false;
    Window* modal = modalWindow();
    Shortcut* winMatch = nullptr;
    Shortcut* appMatch = nullptr;
    for (Shortcut* s : allShortcuts) {
        if (!s->enabled || !s->owner || !s->fn || s->seq.empty()) continue;
        if (!s->seq.matches(e)) continue;
        if (s->active && !s->active()) continue;
        Window* ow = s->owner->window();
        if (!ow || !s->owner->isEnabled()) continue;
        if (s->scope == ShortcutScope::Window) {
            if (ow != from || !s->owner->isShown()) continue;
            if (!winMatch) winMatch = s;
        } else {
            if (modal && ow != modal && from != ow) continue;
            if (modal && from != modal) continue;
            if (!appMatch) appMatch = s;
        }
    }
    Shortcut* s = winMatch ? winMatch : appMatch;
    if (!s) return false;
    std::function<void()> fn = s->fn;
    fn();
    return true;
}

// --- clipboard ---
void setClipboard(const std::string& text) {
    App& app = App::get();
    if (app.headless()) app.clip_ = text;
    else plat::setClipboardText(text);
}

std::string clipboard() {
    App& app = App::get();
    if (app.headless()) return app.clip_;
    return plat::clipboardText();
}

}  // namespace ui
