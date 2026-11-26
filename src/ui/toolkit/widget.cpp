// The toolkit's core: Widget (tree, geometry, painting, events), KeySequence / Shortcut, Window
// (a native window: framebuffer, mouse / key dispatch, hover, focus, modality) and App (the main
// loop, frame timer, timers, tooltips, drags, popups, deleteLater).
#include "ui/toolkit/widget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_set>

#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/frame.h"
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
    if (layout_) layout_->apply(this, layoutArea());
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
    // (and its id: objectName is often set after the widget was made, when its style may have been looked up already)
    if (!objectName.empty()) key += "#" + objectName;
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
namespace {
// whether the stylesheets set opacity / box-shadow on widgets at all (else neither is looked up: an int compare)
struct NodeLooks {
    int generation = -1;
    bool opacity = false, shadow = false;
};
const NodeLooks& nodeLooks() {
    static NodeLooks n;
    const css::Engine& engine = theme::styles();
    if (engine.generation() != n.generation) {
        n.generation = engine.generation();
        n.opacity = css::declaresOnNodes(engine, "opacity");
        n.shadow = css::declaresOnNodes(engine, "box-shadow");
    }
    return n;
}
}  // namespace

Rect Widget::paintRect() const {
    if (!nodeLooks().shadow) return rect();
    const style::Insets& o = style::boxLook(computedStyle()).overflow;
    return rect().adjusted(-o.left, -o.top, o.right, o.bottom);
}

void Widget::update() { update(paintRect().united(paintedRect_)); }

void Widget::update(const Rect& area) {
    if (deleted_ && !visible_) return;
    Rect r = area.intersected(paintRect().united(paintedRect_));
    if (r.empty()) return;
    const Widget* c = this;
    while (c->parent_) {
        if (!c->visible_) return;
        r = r.translated(c->geom_.x, c->geom_.y);
        if (c->parent_->clipsChildren_ && !c->inFrame_) r = r.intersected(c->parent_->rect());
        if (r.empty()) return;
        c = c->parent_;
    }
    auto* win = dynamic_cast<const Window*>(c);
    if (!win || !win->isWindowShown()) return;
    const_cast<Window*>(win)->invalidate(r);
}

void Widget::paintTree(Painter& p, const Rect& dirty) {
    if (!visible_ || deleted_) return;
    // its opacity: only looked up while some rule of the stylesheets sets one for widgets (else this costs an
    // int compare); kept with its computed style (style::boxLook)
    float opacity = nodeLooks().opacity ? style::boxLook(computedStyle()).opacity : 1.f;
    if (opacity >= 1.f) {
        paintSelf(p, dirty);
        return;
    }
    if (opacity <= 0.f) return;
    // into a layer (device pixels: the part of the dirty area it may draw on), put on at once
    float s = p.scale();
    PointF t = p.translation();
    Rect lim = clipsChildren_ ? dirty.intersected(paintRect()) : dirty;
    if (lim.empty()) return;
    int x0 = (int)std::floor((lim.x + t.x) * s), y0 = (int)std::floor((lim.y + t.y) * s);
    int x1 = (int)std::ceil((lim.right() + t.x) * s), y1 = (int)std::ceil((lim.bottom() + t.y) * s);
    Rect dev = Rect(x0, y0, x1 - x0, y1 - y0).intersected(p.deviceClip());
    if (dev.empty()) return;
    Image layer(dev.w, dev.h);
    Painter lp(layer, s);
    lp.translate(t.x - dev.x / s, t.y - dev.y / s);
    paintSelf(lp, dirty);
    p.blendLayer(layer, dev.x, dev.y, opacity);
}

void Widget::paintSelf(Painter& p, const Rect& dirty) {
    Rect own = paintRect();  // (its rect, and where its outer shadows reach)
    paintedRect_ = own;
    Rect area = dirty.intersected(own);
    if (area.empty() && clipsChildren_) return;
    p.save();
    p.setClip(own);
    if (!area.empty()) {
        if (hasBackground_) p.fillRect(area.intersected(rect()), background_);
        paint(p);
    }
    p.restore();
    p.save();
    if (clipsChildren_) p.setClip(rect());
    for (size_t i = 0; i < children_.size(); ++i) {
        Widget* c = children_[i].get();
        if (!c->visible_ || c->deleted_) continue;
        Rect cr = c->geom_;
        Rect cd = dirty.intersected(c->paintRect().translated(cr.x, cr.y));
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
            explicit Pic(const plat::WindowOptions& o) : Window(o) {
                setStyleType("DragImage");
                paintsOwnBox_ = true;
            }
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
    explicit TipWindow(const plat::WindowOptions& o) : Window(o) {
        setStyleType("ToolTip");
        paintsOwnBox_ = true;
    }
    std::vector<std::string> lines;  // the text as shown: wrapped to the screen's width
    // its size for that text, at most `maxW` wide (the lines wrapped at spaces to fit)
    Point measure(const std::string& text, int maxW) {
        const css::Style& st = computedStyle();
        Font f = style::font(st, theme::uiFont());
        style::Insets b = style::insets(st, "border-width"), pad = style::insets(st, "padding");
        lines = wrapLines(text, f, (float)std::max(40, maxW - b.w() - pad.w()));
        float w = 0;
        for (auto& l : lines) w = std::max(w, f.width(l));
        int n = (int)lines.size();
        int h = (int)std::ceil(n * f.lineSpacing() - (f.lineSpacing() - f.height()));
        return Point(std::min((int)std::ceil(w) + b.w() + pad.w(), std::max(maxW, 1)), h + b.h() + pad.h());
    }
    void paint(Painter& p) override {
        const css::Style& st = computedStyle();
        style::box(p, rect(), st);
        Rect content = style::contents(rect(), st);
        Font f = style::font(st, theme::uiFont());
        Color tc = style::foreground(st);
        float y = (float)content.y;
        p.save();
        p.setClip(content);  // (a word wider than the screen: cut at its edge)
        for (auto& line : lines) {
            p.drawText(PointF((float)content.x, y + f.ascent()), line, f, tc);
            y += f.lineSpacing();
        }
        p.restore();
    }
};
}  // namespace

Window::Window(const plat::WindowOptions& opts) : Widget(nullptr), opts_(opts) {
    setStyleType("Window");
    App& app = App::get();
    scale_ = app.scale();
    hgeom_ = Rect(opts.x < 0 ? 100 : opts.x, opts.y < 0 ? 100 : opts.y, std::max(1, opts.w), std::max(1, opts.h));
    title_ = opts.title;
    // its own frame (the title bar above the inside, the border around): the native window that much bigger
    if (wantsFrame()) {
        framed_ = true;
        titleBar_ = std::make_unique<TitleBar>(this);
        titleBar_->setParentInternal(this);
        titleBar_->inFrame_ = true;
        frame_ = computeFrameInsets();
    }
    FrameInsets d = deviceFrame();
    physW_ = toDevice(std::max(1, opts.w)) + d.left + d.right;
    physH_ = toDevice(std::max(1, opts.h)) + d.top + d.bottom;
    if (!app.headless() || app.nativeWindowFactory) {
        plat::WindowOptions o = opts;  // (in screen pixels)
        o.w = physW_;
        o.h = physH_;
        if (o.x >= 0) o.x = app.toScreen(o.x) - d.left;
        if (o.y >= 0) o.y = app.toScreen(o.y) - d.top;
        if (o.minW > 0) o.minW = toDevice(o.minW) + d.left + d.right;
        if (o.minH > 0) o.minH = toDevice(o.minH) + d.top + d.bottom;
        o.decorated = !framed_;
        native_ = app.headless() ? app.nativeWindowFactory(o, this) : plat::Window::create(o, this);
    }
    setGeometry(0, 0, std::max(1, opts.w), std::max(1, opts.h));
    placeTitleBar();
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
    if (native_ && (m.x > 0 || m.y > 0)) setNativeMinimum(m.x, m.y);
    invalidate(outerRect());
}

// (the native window's events: in device pixels, from its top-left corner; the frame's insets taken off, so
// positions are the inside's, the title bar's negative)
plat::Event Window::toLogical(const plat::Event& e) const {
    FrameInsets d = deviceFrame();
    bool framed = d.left || d.top || d.right || d.bottom;
    if (scale_ == 1 && App::get().scale() == 1 && !framed) return e;
    plat::Event le = e;
    auto div = [](int v, double s) { return (int)std::floor(v / s); };
    switch (e.type) {
    case plat::Event::Move:  // (screen coordinates)
        le.x = App::get().fromScreen(e.x + d.left);
        le.y = App::get().fromScreen(e.y + d.top);
        break;
    case plat::Event::Resize:
        le.w = std::max(1, (int)std::lround((e.w - d.left - d.right) / (double)scale_));
        le.h = std::max(1, (int)std::lround((e.h - d.top - d.bottom) / (double)scale_));
        break;
    default:
        le.x = div(e.x - d.left, scale_);
        le.y = div(e.y - d.top, scale_);
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
    titleBar_.reset();  // (its frame's: not one of the children)
    focus_ = hover_ = grab_ = nullptr;
    app.removeWindow(this);
    native_.reset();
}

void Window::setTitle(const std::string& t) {
    title_ = t;
    if (native_) native_->setTitle(t);
    if (titleBar_) titleBar_->update();
}

void Window::showWindow() {
    App& app = App::get();
    // (its style type is its class's by now: Dialog…; maximized or full screen, the window manager keeps its size.
    // A popup: room for its shadow while see-through)
    if (framed_) applyFrameInsets(isWindowMaximized() || (native_ && native_->isFullScreen()));
    else if (opts_.popup) applyFrameInsets(false);
    relayout();
    app.doLayouts();
    if (!everShown_ && scrollsWhenSmall()) fitScreen();
    everShown_ = true;
    if (layout()) {
        Point m = leastSize();
        if (native_ && (m.x > 0 || m.y > 0) && !opts_.popup) setNativeMinimum(m.x, m.y);
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
    invalidate(outerRect());
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
    edgeHover_ = 0;
    frameDrag_ = FrameDrag();
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
    FrameInsets d = deviceFrame();
    physW_ = toDevice(w) + d.left + d.right;
    physH_ = toDevice(h) + d.top + d.bottom;
    if (native_) native_->resize(physW_, physH_);
    setGeometry(0, 0, w, h);
    placeTitleBar();
}

void Window::moveWindow(int x, int y) {
    moved_ = true;
    hgeom_.x = x;
    hgeom_.y = y;
    FrameInsets d = deviceFrame();
    if (native_) native_->move(App::get().toScreen(x) - d.left, App::get().toScreen(y) - d.top);
}

Rect Window::screenGeometry() const {
    if (native_) {
        Rect g = native_->geometry();
        FrameInsets d = deviceFrame();
        g = Rect(g.x + d.left, g.y + d.top, g.w - d.left - d.right, g.h - d.top - d.bottom);
        if (!g.empty()) {
            App& app = App::get();
            if (app.scale() == 1) return g;
            int x = app.fromScreen(g.x), y = app.fromScreen(g.y);
            return Rect(x, y, app.fromScreen(g.right()) - x, app.fromScreen(g.bottom()) - y);
        }
    }
    return Rect(hgeom_.x, hgeom_.y, width(), height());
}

bool Window::scrollsWhenSmall() const {
    return !opts_.popup && !opts_.offscreen && App::get().mainWindow != this;
}

Point Window::contentMinimum() { return leastSize(); }  // (one least size: a window's own override counts here too)

Point Window::leastSize() const {
    Point m = minimumSize();
    if (layout()) {
        Point lm = layout()->minimumSize();
        m = Point(std::max(m.x, lm.x), std::max(m.y, lm.y));
    }
    return m;
}

Rect Window::layoutArea() {
    Rect r = rect();
    if (!layout() || !scrollsWhenSmall()) return r;
    Point m = contentMinimum();
    bool needH = m.x > r.w, needV = m.y > r.h;
    if (!needH && !needV) {
        scroll_ = Point(0, 0);
        for (ScrollBar* b : scrollBars_)
            if (b && b->isVisible()) b->hide();
        return r;
    }
    for (int i = 0; i < 2; i++)
        if (!scrollBars_[i]) {
            scrollBars_[i] = add<ScrollBar>(i == 0 ? Orientation::Vertical : Orientation::Horizontal);
            scrollBars_[i]->hide();
            scrollBars_[i]->valueChanged.connect([this, i](int v) {
                if (!placingBars_) scrollTo(i == 0 ? Point(scroll_.x, v) : Point(v, scroll_.y));
            });
        }
    ScrollBar *vb = scrollBars_[0], *hb = scrollBars_[1];
    int tv = vb->thickness(), th = hb->thickness();
    // (the bars lie over its edges, its widgets keep their width: a bar showing never changes how they wrap; with
    // both, each scrolls that much further, past the other)
    int W = std::max(r.w, m.x), H = std::max(r.h, m.y);
    int rangeX = needH ? W + (needV ? tv : 0) - r.w : 0, rangeY = needV ? H + (needH ? th : 0) - r.h : 0;
    scroll_ = Point(std::max(0, std::min(scroll_.x, rangeX)), std::max(0, std::min(scroll_.y, rangeY)));
    placingBars_ = true;
    vb->setVisible(needV);
    hb->setVisible(needH);
    if (needV) {
        vb->setGeometry(r.w - tv, 0, tv, r.h - (needH ? th : 0));
        vb->setRange(0, rangeY);
        vb->setPageStep(r.h);
        vb->setValue(scroll_.y);
        vb->raise();
    }
    if (needH) {
        hb->setGeometry(0, r.h - th, r.w - (needV ? tv : 0), th);
        hb->setRange(0, rangeX);
        hb->setPageStep(r.w);
        hb->setValue(scroll_.x);
        hb->raise();
    }
    placingBars_ = false;
    return Rect(-scroll_.x, -scroll_.y, W, H);
}

bool Window::overflows() const {
    return (scrollBars_[0] && scrollBars_[0]->isVisible()) || (scrollBars_[1] && scrollBars_[1]->isVisible());
}

void Window::scrollTo(Point offset) {
    if (offset.x == scroll_.x && offset.y == scroll_.y) return;
    scroll_ = Point(std::max(0, offset.x), std::max(0, offset.y));
    relayout();  // (clamps it)
    update();
}

void Window::ensureVisible(Widget* w) {
    if (!w || w == this || !isAncestorOf(w) || !scrollsWhenSmall()) return;
    Point at = w->mapToWindow(Point(0, 0));
    int vw = width() - (scrollBars_[0] && scrollBars_[0]->isVisible() ? scrollBars_[0]->width() : 0);
    int vh = height() - (scrollBars_[1] && scrollBars_[1]->isVisible() ? scrollBars_[1]->height() : 0);
    Point to = scroll_;
    if (at.x + w->width() > vw) to.x += at.x + std::min(w->width(), vw) - vw;
    if (at.x < 0) to.x += at.x;
    if (at.y + w->height() > vh) to.y += at.y + std::min(w->height(), vh) - vh;
    if (at.y < 0) to.y += at.y;
    scrollTo(to);
}

bool Window::wheel(WheelEvent& e) {
    // (reached when no widget under the pointer took it): a window scrolled because it is too small
    bool horiz = e.shift() || std::fabs(e.dx) > std::fabs(e.dy);
    ScrollBar* b = scrollBars_[horiz ? 1 : 0];
    if (!b || !b->isVisible()) b = scrollBars_[horiz ? 0 : 1];  // (the one bar there is)
    return b && b->isVisible() && b->wheel(e);
}

// (the first time it shows) at least its layout's minimum, at most the screen (its frame too), on the screen
void Window::fitScreen() {
    Rect sg = screenGeometry();
    Rect scr = App::get().screenAt(Point(sg.x + sg.w / 2, sg.y + sg.h / 2));
    if (scr.empty()) return;
    Point m = contentMinimum();
    int maxW = std::max(1, scr.w - frame_.left - frame_.right), maxH = std::max(1, scr.h - frame_.top - frame_.bottom);
    int w = std::min(std::max(width(), m.x), maxW), h = std::min(std::max(height(), m.y), maxH);
    if (!layout()) w = width(), h = height();  // (it places its own widgets, a plugin's view: it can't scroll, kept)
    if (w != width() || h != height()) {
        util::debugf("window \"%s\": %dx%d made %dx%d to fit the screen %dx%d (its least %dx%d: it scrolls below that)\n",
                     title_.c_str(), width(), height(), w, h, scr.w, scr.h, m.x, m.y);
        resizeWindow(w, h);
    }
    // (placed by the program, or without a display: kept on the screen; else the platform centres it there)
    if (native_ && !moved_) return;
    int x = sg.x - (w - sg.w) / 2, y = sg.y - (h - sg.h) / 2;  // (its middle kept)
    x = std::max(scr.x + frame_.left, std::min(x, scr.right() - frame_.right - w));
    y = std::max(scr.y + frame_.top, std::min(y, scr.bottom() - frame_.bottom - h));
    if (x != sg.x || y != sg.y) moveWindow(x, y);
}

void Window::setMinimumWindowSize(int w, int h) {
    setMinimumSize(w, h);
    if (native_) setNativeMinimum(w, h);
}

void Window::setNativeMinimum(int w, int h) {
    if (!native_) return;
    if (scrollsWhenSmall()) {  // (it scrolls when smaller: never asks for more than the screen)
        Rect sg = screenGeometry();
        Rect scr = App::get().screenAt(Point(sg.x + sg.w / 2, sg.y + sg.h / 2));
        w = std::min(w, std::max(1, scr.w - frame_.left - frame_.right));
        h = std::min(h, std::max(1, scr.h - frame_.top - frame_.bottom));
    }
    FrameInsets d = deviceFrame();
    native_->setMinimumSize(toDevice(w) + d.left + d.right, toDevice(h) + d.top + d.bottom);
}

bool Window::followMinimum() {
    if (opts_.popup || !shown_ || !layout()) return false;
    Point m = leastSize();
    if (m == followedMin_) return false;
    followedMin_ = m;
    if (native_ && (m.x > 0 || m.y > 0)) setNativeMinimum(m.x, m.y);
    if (height() >= m.y || isWindowMaximized() || (native_ && native_->isFullScreen())) return false;
    int h = m.y;
    if (scrollsWhenSmall()) {  // (never past the screen: below that it scrolls)
        Rect sg = screenGeometry();
        Rect scr = App::get().screenAt(Point(sg.x + sg.w / 2, sg.y + sg.h / 2));
        if (!scr.empty()) h = std::min(h, std::max(1, scr.h - frame_.top - frame_.bottom));
    }
    if (h <= height()) return false;
    util::debugf("window \"%s\": its content needs %d px high at %d wide (was %d): grown\n", title().c_str(), h,
                 width(), height());
    resizeWindow(width(), h);
    // grown downwards: kept on the screen (a dialog centred for its first height, then narrowed and rewrapped)
    Rect sg = screenGeometry();
    Rect scr = App::get().screenAt(Point(sg.x + sg.w / 2, sg.y + sg.h / 2));
    if (!scr.empty() && (!native_ || moved_)) {
        int y = std::max(scr.y + frame_.top, std::min(sg.y, scr.bottom() - frame_.bottom - h));
        if (y != sg.y) moveWindow(sg.x, y);
    }
    return true;
}

void Window::centerOn(Window* other) {
    Rect area;
    if (other && other->isWindowShown()) area = other->outerScreenGeometry();
    else area = App::get().screenAt(App::get().cursorPos());
    // (the whole window, its frame too, centred and on the screen)
    int ow = width() + frame_.left + frame_.right, oh = height() + frame_.top + frame_.bottom;
    int x = area.x + (area.w - ow) / 2, y = area.y + (area.h - oh) / 2;
    Rect scr = App::get().screenAt(Point(area.x + area.w / 2, area.y + area.h / 2));
    if (!scr.empty()) {
        x = std::max(scr.x, std::min(x, scr.right() - ow));
        y = std::max(scr.y, std::min(y, scr.bottom() - oh));
    }
    moveWindow(x + frame_.left, y + frame_.top);
}

void Window::setFocusWidget(Widget* w) {
    if (w && (w->window() != this || w->deleted_)) return;
    if (focus_ == w) return;
    if (w && overflows()) ensureVisible(w);
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
    if (!shown_ || !App::get().paintWindows) return;
    // (the framebuffer is the whole native window: the frame around the inside, frame_ / deviceFrame(); its
    // painter is moved by the frame's insets, so the inside is painted at (0, 0) as without one)
    FrameInsets fd = deviceFrame();
    bool framed = fd.left || fd.top || fd.right || fd.bottom;
    Rect outer = outerRect();
    if (scale_ != 1) {
        // device pixels: the native window's size (logical × scale, give or take the rounding)
        int fw = native_ ? std::max(1, physW_) : std::max(1, toDevice(width()) + fd.left + fd.right);
        int fh = native_ ? std::max(1, physH_) : std::max(1, toDevice(height()) + fd.top + fd.bottom);
        if (fb_.width() != fw || fb_.height() != fh) {
            fb_.resize(fw, fh);
            fb_.fill(style::background(computedStyle()));
            dirty_ = outer;
        }
        if (dirty_.empty()) return;
        Rect d = dirty_.intersected(outer);
        dirty_ = Rect();
        if (d.empty()) return;
        Painter p(fb_, scale_);
        if (framed) p.translate(fd.left / scale_, fd.top / scale_);
        p.setClip(d);
        paintBackground(p);
        paintFrame(p, d);
        paintContents(p, d);
        Rect dev = toDeviceRect(d).translated(fd.left, fd.top).intersected(fb_.rect());
        if (framed_ && hasAlpha()) clipCorners(dev);
        if (native_) native_->present(fb_, dev);
        return;
    }
    if (fb_.width() != outer.w || fb_.height() != outer.h) {
        fb_.resize(std::max(1, outer.w), std::max(1, outer.h));
        dirty_ = outer;
    }
    if (dirty_.empty()) return;
    Rect d = dirty_.intersected(outer);
    dirty_ = Rect();
    if (d.empty()) return;
    Painter p(fb_);
    if (framed) p.translate((float)fd.left, (float)fd.top);
    p.setClip(d);
    paintBackground(p);
    paintFrame(p, d);
    paintContents(p, d);
    if (framed_ && hasAlpha()) clipCorners(d.translated(fd.left, fd.top).intersected(fb_.rect()));
    if (native_) native_->present(fb_, d.translated(fd.left, fd.top));
}

// its widgets. Its own `opacity` is the native window's while transparency is on (the compositing manager fades
// it: _NET_WM_WINDOW_OPACITY), so they are drawn at full opacity; else (no native window, transparency off) as
// any widget's: in a layer over its background
void Window::paintContents(Painter& p, const Rect& d) {
    if (!native_) {
        paintTree(p, d);
        return;
    }
    bool own = App::get().transparency();
    float o = own && nodeLooks().opacity ? style::boxLook(computedStyle()).opacity : 1.f;
    if (o != opacitySent_) {
        native_->setOpacity(o);
        opacitySent_ = o;
    }
    if (own) paintSelf(p, d);
    else paintTree(p, d);
}

bool Window::hasAlpha() const {
    if (native_) return native_->hasAlpha();
    const App& app = App::get();
    return app.headless() && app.headlessAlpha_ && app.transparency_;
}

bool Window::madeOpaque() const {
    const App& app = App::get();
    return native_ && !native_->hasAlpha() && app.transparency() && app.transparencyAvailable();
}

Window::FrameInsets Window::nativeInsets() const {
    FrameInsets n;
    n.left = frame_.left + shadow_.left;
    n.top = frame_.top + shadow_.top;
    n.right = frame_.right + shadow_.right;
    n.bottom = frame_.bottom + shadow_.bottom;
    return n;
}

// (see-through popups) room around the inside for its style's outer box-shadows: as far as they reach (paintRect)
Window::FrameInsets Window::computeShadowInsets() const {
    FrameInsets f;
    if (!opts_.popup || opts_.offscreen || !hasAlpha()) return f;
    Rect pr = paintRect();
    const int most = 128;  // (a shadow reaching farther is cut there)
    f.left = std::min(most, std::max(0, -pr.x));
    f.top = std::min(most, std::max(0, -pr.y));
    f.right = std::min(most, std::max(0, pr.right() - width()));
    f.bottom = std::min(most, std::max(0, pr.bottom() - height()));
    return f;
}

namespace {
// how much of each pixel of a corner square of radius r lies inside its quarter circle (0…255, 4×4 samples), the
// top-left corner's (the others mirrored); kept per radius
const std::vector<uint8_t>& cornerCoverage(int r) {
    static std::map<int, std::vector<uint8_t>> cache;
    auto it = cache.find(r);
    if (it != cache.end()) return it->second;
    std::vector<uint8_t> c((size_t)r * r);
    double rr = (double)r * r;
    for (int y = 0; y < r; y++)
        for (int x = 0; x < r; x++) {
            int in = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    double dx = r - (x + (i + 0.5) / 4), dy = r - (y + (j + 0.5) / 4);
                    if (dx * dx + dy * dy <= rr) in++;
                }
            c[(size_t)y * r + x] = (uint8_t)((in * 255 + 8) / 16);
        }
    if (cache.size() > 64) cache.clear();
    return cache.emplace(r, std::move(c)).first->second;
}
}  // namespace

// (see-through, framed) nothing outside the rounded corners of its frame (shapeRadii_: the Window's border-*-radius,
// as the opaque window's shape cuts them), whatever was drawn there (a title bar's square corners, a skin's shadow):
// each pixel's alpha at most how much of it is inside, anti-aliased against the desktop. Only in `dev` (repainted)
void Window::clipCorners(const Rect& dev) {
    int W = fb_.width(), H = fb_.height();
    for (int k = 0; k < 4; k++) {
        int r = std::min({shapeRadii_[k], W / 2, H / 2});
        if (r <= 0) continue;
        bool right = k == 1 || k == 2, bottom = k == 2 || k == 3;
        Rect sq(right ? W - r : 0, bottom ? H - r : 0, r, r);
        Rect a = sq.intersected(dev);
        if (a.empty()) continue;
        const std::vector<uint8_t>& cov = cornerCoverage(r);
        for (int y = a.y; y < a.bottom(); y++) {
            int cy = bottom ? H - 1 - y : y;
            uint32_t* row = fb_.row(y);
            for (int x = a.x; x < a.right(); x++) {
                int cx = right ? W - 1 - x : x;
                uint32_t c = cov[(size_t)cy * r + cx], px = row[x], al = px >> 24;
                if (al <= c) continue;
                if (!c) {
                    row[x] = 0;
                    continue;
                }
                // (premultiplied: every channel scaled by c / alpha, the colour kept)
                auto ch = [&](int sh) { return (((px >> sh) & 255u) * c + al / 2) / al; };
                row[x] = (c << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
            }
        }
    }
}

void Window::paintBackground(Painter& p) {
    // (the painter is clipped to what is repainted; a see-through background goes over black: nothing shows
    // through a window). Framed: its box around the frame too (its border is the frame's edge), and its ::frame
    // part's ring inside that border
    const css::Style& st = computedStyle();
    Rect r = framed_ ? outerRect() : rect();
    if (hasAlpha()) {
        // see-through: what is repainted starts transparent (the desktop shows where nothing is drawn)
        fb_.fill(p.deviceClip(), Color(0, 0, 0, 0));
        if (!paintsOwnBox_) style::box(p, r, st);
        paintFrameRing(p);
        return;
    }
    if (style::background(st).a < 255) p.fillRect(r, Color(0, 0, 0));
    style::box(p, r, st);
    paintFrameRing(p);
}

// (framed) the ::frame part's box, from inside the Window's border to around the title bar and the inside: drawn
// everywhere but over the inside (whose background stays the Window's), so only its ring and what is under the
// title bar show. Nothing when the part has nothing to draw (default.css: no ::frame)
void Window::paintFrameRing(Painter& p) {
    if (!framed_) return;
    const css::Style& ring = computedStyle("frame");
    const style::BoxLook& look = style::boxLook(ring);
    if (!look.more && look.fillOnly && look.bg.a == 0) return;
    Rect outer = style::inside(outerRect(), style::insets(computedStyle(), "border-width"));
    if (outer.empty()) return;
    Rect in = rect();
    // the four bands around the inside (above it: the ring's top and the title bar)
    Rect bands[4] = {Rect(outer.x, outer.y, outer.w, in.y - outer.y), Rect(outer.x, in.bottom(), outer.w, outer.bottom() - in.bottom()),
                     Rect(outer.x, in.y, in.x - outer.x, in.h), Rect(in.right(), in.y, outer.right() - in.right(), in.h)};
    for (const Rect& band : bands) {
        if (band.empty()) continue;
        p.save();
        p.setClip(band);
        style::box(p, outer, ring);
        p.restore();
    }
}

// the native window's corners cut round where the frame's are (the Window's border-*-radius): framed, not
// maximized, Options → Rounded corners on; else square (checked on each frame paint: a skin change shows there)
// See-through, the same radii cut the corners of where it can be clicked (a popup's: its box's, inside the room for
// its shadow, which clicks go through), and blur behind it (backdrop-filter on it, or on its title bar) is asked.
void Window::updateShape() {
    if (!native_) {
        if (framed_ && hasAlpha() && theme::rounded && !isWindowMaximized()) {  // (tests: clipCorners)
            css::Edges e = computedStyle().radii(css::Edges());
            double v[4] = {e.top, e.right, e.bottom, e.left};
            for (int i = 0; i < 4; i++) shapeRadii_[i] = (int)std::lround(std::max(0.0, v[i]) * scale_);
        } else {
            std::fill(shapeRadii_, shapeRadii_ + 4, 0);
        }
        return;
    }
    bool alpha = native_->hasAlpha();
    bool popup = opts_.popup && alpha;
    int r[4] = {0, 0, 0, 0};
    if ((framed_ || popup) && theme::rounded && !isWindowMaximized() && !native_->isFullScreen()) {
        css::Edges e = computedStyle().radii(css::Edges());
        double v[4] = {e.top, e.right, e.bottom, e.left};
        for (int i = 0; i < 4; i++) r[i] = (int)std::lround(std::max(0.0, v[i]) * scale_);
    }
    if (!std::equal(r, r + 4, shapeRadii_)) {
        std::copy(r, r + 4, shapeRadii_);
        native_->setCornerRadii(r[0], r[1], r[2], r[3]);
    }
    FrameInsets d = deviceFrame();
    Rect input = popup ? Rect(d.left, d.top, toDevice(width()), toDevice(height())) : Rect();
    if (!(input == inputSent_)) {
        inputSent_ = input;
        native_->setInputArea(input);
    }
    // the blur: all of it (the Window's backdrop-filter), or the frame's top (its TitleBar's)
    bool on = false;
    Rect area;
    if (alpha) {
        if (computedStyle().backdropBlur() > 0) {
            on = true;
        } else if (titleBar_ && titleBar_->isVisible() && titleBar_->computedStyle().backdropBlur() > 0) {
            on = true;
            area = Rect(0, 0, std::max(1, physW_), d.top);
        }
    }
    native_->setBlurBehind(on, area);
}

void Window::paintFrame(Painter& p, const Rect& dirty) {
    updateShape();
    if (!titleBar_ || !titleBar_->isVisible()) return;
    Rect g = titleBar_->geometry();
    if (!dirty.intersects(titleBar_->paintRect().translated(g.x, g.y))) return;
    p.save();
    p.translate((float)g.x, (float)g.y);
    titleBar_->paintTree(p, dirty.translated(-g.x, -g.y));
    p.restore();
}

void Window::invalidate(const Rect& r) {
    // (at a fractional scale a logical pixel's edge falls inside a device pixel, and anti-aliased
    // drawing reaches a little beyond its widget: one more pixel around, so nothing stale is left)
    Rect a = scale_ == 1 || scale_ == std::floor(scale_) ? r : Rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2);
    dirty_ = dirty_.united(a.intersected(outerRect()));
}

// ---------------------------------------------------------------------------------------------------
// the frame (isFramed)

bool Window::wantsFrame() const {
    return !opts_.popup && !opts_.offscreen && App::get().framesDrawn();
}

Window::FrameInsets Window::computeFrameInsets() const {
    FrameInsets f;
    if (!framed_ || (native_ && native_->isFullScreen())) return f;  // (full screen: nothing around it)
    // the Window's border, then its ::frame part's border and padding (a ring inside the border, around the title
    // bar and the inside)
    style::Insets b = style::insets(computedStyle(), "border-width");
    const css::Style& ring = computedStyle("frame");
    style::Insets rb = style::insets(ring, "border-width"), rp = style::insets(ring, "padding");
    f.left = std::max(0, b.left) + std::max(0, rb.left + rp.left);
    f.right = std::max(0, b.right) + std::max(0, rb.right + rp.right);
    f.bottom = std::max(0, b.bottom) + std::max(0, rb.bottom + rp.bottom);
    f.top = std::max(0, b.top) + std::max(0, rb.top + rp.top) +
            (titleBar_ && titleBar_->isVisible() ? TitleBar::barHeight() : 0);
    return f;
}

Window::FrameInsets Window::deviceFrame() const {
    FrameInsets n = nativeInsets();
    if (scale_ == 1) return n;
    FrameInsets d;
    d.left = toDevice(n.left);
    d.top = toDevice(n.top);
    d.right = toDevice(n.right);
    d.bottom = toDevice(n.bottom);
    return d;
}

Rect Window::outerRect() const {
    FrameInsets n = nativeInsets();
    return Rect(-n.left, -n.top, width() + n.left + n.right, height() + n.top + n.bottom);
}

Rect Window::outerScreenGeometry() const {
    // (the frame's insets are this window's logical pixels; global ones are App::scale()'s: a plugin's window, at
    // scale 1, has a smaller frame in them when the interface is bigger)
    Rect g = screenGeometry();
    double k = scale_ / (double)App::get().scale();
    auto c = [k](int v) { return k == 1 ? v : (int)std::lround(v * k); };
    int l = c(frame_.left), t = c(frame_.top), r = c(frame_.right), b = c(frame_.bottom);
    return Rect(g.x - l, g.y - t, g.w + l + r, g.h + t + b);
}

void Window::placeTitleBar() {
    if (!titleBar_) return;
    int h = TitleBar::barHeight();
    titleBar_->setGeometry(0, -h, width(), h);
}

// the frame's insets as they should be now (on, off, the stylesheets' sizes, full screen): applied. keepOuter: the
// native window keeps its size, the inside takes what is left (the window manager decides it: maximized, full
// screen, or about to be restored); else the inside keeps its size, the native window grows or shrinks
void Window::applyFrameInsets(bool keepOuter) {
    FrameInsets n = computeFrameInsets(), sh = computeShadowInsets();
    if (n == frame_ && sh == shadow_) {
        placeTitleBar();
        return;
    }
    FrameInsets old = frame_;
    bool shadowOnly = n == frame_;
    frame_ = n;
    shadow_ = sh;
    FrameInsets d = deviceFrame();
    if (keepOuter) {
        int ow = width() + old.left + old.right, oh = height() + old.top + old.bottom;
        int w = std::max(1, ow - n.left - n.right), h = std::max(1, oh - n.top - n.bottom);
        if (native_ && physW_ > 0 && physH_ > 0) {  // (the native size as it is: exactly)
            w = std::max(1, (int)std::lround((physW_ - d.left - d.right) / (double)scale_));
            h = std::max(1, (int)std::lround((physH_ - d.top - d.bottom) / (double)scale_));
        }
        hgeom_.w = w;
        hgeom_.h = h;
        setGeometry(0, 0, w, h);
        placeTitleBar();
    } else {
        resizeWindow(width(), height());  // (the native window: the inside's size and the new frame)
        // (a popup's shadow came or went: the inside stays where it was put)
        if (shadowOnly && opts_.popup) moveWindow(hgeom_.x, hgeom_.y);
    }
    Point m = leastSize();
    if (native_ && !opts_.popup) setNativeMinimum(m.x, m.y);
    invalidate(outerRect());
    frameChanged();
}

void Window::updateFrame() {
    bool want = wantsFrame();
    bool toggled = want != framed_;
    Rect was = screenGeometry();
    if (want != framed_) {
        framed_ = want;
        edgeHover_ = 0;
        frameDrag_ = FrameDrag();
        if (want) {
            titleBar_ = std::make_unique<TitleBar>(this);
            titleBar_->setParentInternal(this);
            titleBar_->inFrame_ = true;
        } else {
            if (hover_ && titleBar_ && titleBar_->isAncestorOf(hover_)) setHover(nullptr);
            if (grab_ && titleBar_ && titleBar_->isAncestorOf(grab_)) releaseGrab();
            titleBar_.reset();
        }
        if (native_) native_->setDecorated(!want);
        statesChanged();
        updateShape();
    }
    if (framed_ || frame_ != FrameInsets() || shadow_ != FrameInsets() || opts_.popup) {
        bool keepOuter = isWindowMaximized() || (native_ && native_->isFullScreen());
        applyFrameInsets(keepOuter);
        // (turned on or off: the inside stays where it was on the screen, the frame grows or goes around it)
        if (toggled && !keepOuter && shown_) moveWindow(was.x, was.y);
        invalidate(outerRect());
    }
}

void Window::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    if (hasAlpha()) out.push_back("transparent");
    if (!framed_) return;
    out.push_back("framed");
    out.push_back(active_ ? "active" : "inactive");
    if (isWindowMaximized()) out.push_back("maximized");
}

bool Window::isWindowMaximized() const { return native_ ? native_->isMaximized() : maximizedHeadless_; }

bool Window::isWindowMinimized() const { return native_ ? native_->isMinimized() : minimizedHeadless_; }

void Window::minimizeWindow() {
    App::get().frameAction.emit(this, "minimize");
    if (native_) native_->minimize();
    else minimizedHeadless_ = true;
}

void Window::setWindowMaximized(bool on) {
    App::get().frameAction.emit(this, on ? "maximize" : "restore");
    if (native_) native_->setMaximized(on);
    else maximizedHeadless_ = on;
    frameStateChanged();
}

// maximized, restored, minimized (by the frame, or the window manager: Event::StateChanged)
void Window::frameStateChanged() {
    edgeHover_ = 0;
    frameDrag_ = FrameDrag();
    if (!framed_) return;
    applyFrameInsets(true);  // (maximized: the stylesheets may take the border off)
    statesChanged();
    updateShape();
    if (titleBar_) titleBar_->update();
    invalidate(outerRect());  // (:maximized, its border)
    applyCursor();
}

void Window::setResizable(bool on) {
    opts_.resizable = on;
    if (native_) native_->setResizable(on);
    if (titleBar_) titleBar_->update();
}

int Window::resizeEdgeAt(Point p) const {
    if (!framed_ || !opts_.resizable || isWindowMaximized() || (native_ && native_->isFullScreen())) return 0;
    Rect o = outerRect();
    if (!o.contains(p)) return 0;
    int band = std::max({theme::metric("window-resize-border", 4), frame_.left, frame_.right, frame_.bottom,
                         frame_.top - (titleBar_ && titleBar_->isVisible() ? TitleBar::barHeight() : 0)});
    if (band <= 0) return 0;
    int corner = std::max(band * 3, 16);  // (how far along an edge its corner reaches)
    bool l = p.x < o.x + band, r = p.x >= o.right() - band, t = p.y < o.y + band, b = p.y >= o.bottom() - band;
    if (!(l || r || t || b)) return 0;
    if (l || r) {
        if (p.y < o.y + corner) t = true;
        else if (p.y >= o.bottom() - corner) b = true;
    }
    if (t || b) {
        if (p.x < o.x + corner) l = true;
        else if (p.x >= o.right() - corner) r = true;
    }
    if (l && r) r = false;  // (a window narrower than two bands)
    if (t && b) b = false;
    return (l ? plat::EdgeLeft : 0) | (r ? plat::EdgeRight : 0) | (t ? plat::EdgeTop : 0) | (b ? plat::EdgeBottom : 0);
}

bool Window::beginSystemMove(Point globalPos) {
    App& app = App::get();
    app.frameAction.emit(this, "move");
    if (!native_ || !app.systemMoves) return false;
    if (!native_->beginMoveResize(plat::EdgeNone, Point(app.toScreen(globalPos.x), app.toScreen(globalPos.y)),
                                  plat::LeftButton))
        return false;
    releaseGrab();  // (the window manager has the pointer: no release comes here)
    applyCursor();
    return true;
}

bool Window::beginSystemResize(int edges, Point globalPos) {
    App& app = App::get();
    app.frameAction.emit(this, "resize");
    if (!native_ || !app.systemMoves) return false;
    return native_->beginMoveResize(edges, Point(app.toScreen(globalPos.x), app.toScreen(globalPos.y)), plat::LeftButton);
}

// a resize by the band without the window manager's help: the inside's new geometry from where the pointer is
void Window::resizeByHand(Point g) {
    int edges = frameDrag_.edges;
    Rect r = frameDrag_.geom;
    Point d = g - frameDrag_.start;
    Point m = minimumSize();
    if (layout()) {
        Point lm = layout()->minimumSize();
        m = Point(std::max(m.x, lm.x), std::max(m.y, lm.y));
    }
    int minW = std::max(m.x, 1), minH = std::max(m.y, 1);
    int x = r.x, y = r.y, w = r.w, h = r.h;
    if (edges & plat::EdgeLeft) {
        w = std::max(minW, r.w - d.x);
        x = r.right() - w;
    } else if (edges & plat::EdgeRight) {
        w = std::max(minW, r.w + d.x);
    }
    if (edges & plat::EdgeTop) {
        h = std::max(minH, r.h - d.y);
        y = r.bottom() - h;
    } else if (edges & plat::EdgeBottom) {
        h = std::max(minH, r.h + d.y);
    }
    Point mx = maximumSize();
    w = std::min(w, std::max(minW, mx.x));
    h = std::min(h, std::max(minH, mx.y));
    if (w != width() || h != height()) resizeWindow(w, h);
    if (x != r.x || y != r.y || (edges & (plat::EdgeLeft | plat::EdgeTop))) moveWindow(x, y);
}

// where the pointer is on the screen, for moving or resizing the window by hand: the system's own pointer (an
// event's position is the window's as it was when the event was made, and the window is moving under it), or,
// without a display or from the test script (App::systemMoves off: its pointer is the events'), the last event's
Point Window::framePointer() const {
    App& app = App::get();
    if (native_ && app.systemMoves) return app.cursorPos();
    return app.lastCursor_;
}

Widget* Window::widgetAt(Point p) {
    if (titleBar_ && titleBar_->isVisible() && titleBar_->geometry().contains(p)) {
        Rect g = titleBar_->geometry();
        Widget* w = titleBar_->childAt(p - Point(g.x, g.y));
        return w ? w : titleBar_.get();
    }
    if (!rect().contains(p)) return nullptr;
    Widget* w = childAt(p);
    return w ? w : this;
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

namespace {
Cursor edgeCursor(int edges) {
    bool l = edges & plat::EdgeLeft, r = edges & plat::EdgeRight, t = edges & plat::EdgeTop, b = edges & plat::EdgeBottom;
    if ((t && l) || (b && r)) return Cursor::SizeFDiag;
    if ((t && r) || (b && l)) return Cursor::SizeBDiag;
    if (l || r) return Cursor::SizeHor;
    return Cursor::SizeVer;
}
}  // namespace

void Window::applyCursor() {
    Widget* w = grab_ ? grab_ : hover_;
    Cursor c = Cursor::Arrow;
    for (; w; w = w->parent_)
        if (w->cursorSet_) { c = w->cursor_; break; }
    if (frameDrag_.edges) c = edgeCursor(frameDrag_.edges);
    else if (edgeHover_ && !grab_) c = edgeCursor(edgeHover_);
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
    setHover(widgetAt(p));
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
    // the frame's resize band (isFramed): over the edges of what is inside, before the widgets get the pointer
    if (framed_) {
        if (frameDrag_.edges) {  // (a resize by hand: no _NET_WM_MOVERESIZE)
            if (e.type == plat::Event::MouseMove && (e.buttons & plat::LeftButton)) {
                resizeByHand(framePointer());
                return;
            }
            if (e.type == plat::Event::MouseUp || e.type == plat::Event::MouseMove) {
                frameDrag_ = FrameDrag();
                edgeHover_ = 0;
                applyCursor();
                if (e.type == plat::Event::MouseUp) return;
            }
        }
        if (!grab_ && !app.drag()) {
            int edges = resizeEdgeAt(p);
            if (e.type == plat::Event::MouseMove && e.buttons == 0) {
                if (edges != edgeHover_) {
                    edgeHover_ = edges;
                    if (edges) {
                        setHover(nullptr);
                        app.hideToolTip();
                    }
                    applyCursor();
                }
                if (edges) return;
            } else if (e.type == plat::Event::MouseDown && edges) {
                app.hideToolTip();
                if (e.button == plat::LeftButton && !beginSystemResize(edges, app.lastCursor_)) {
                    frameDrag_.edges = edges;
                    frameDrag_.start = framePointer();
                    frameDrag_.geom = screenGeometry();
                    applyCursor();
                }
                return;
            }
        }
    }
    switch (e.type) {
    case plat::Event::MouseDown: {
        app.hideToolTip();
        if (grab_ && ((e.buttons & ~e.button) == 0 || !Widget::alive(grab_))) releaseGrab();
        Widget* target = grab_;
        if (!target) {
            updateHover(p, e);
            target = widgetAt(p);
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
    Widget* target = widgetAt(p);
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
    if (app.eventFilter && app.eventFilter(this, physical)) return;
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
    case plat::Event::FocusOut: {
        bool in = e.type == plat::Event::FocusIn;
        // (framed: :active / :inactive; the frame repainted only if that changes its look: the Window's style (its
        // background, its border) or its ::frame's; its insets applied again if they change)
        std::map<std::string, std::string> lookWas, ringWas;
        if (framed_) {
            lookWas = computedStyle().props_;
            ringWas = computedStyle("frame").props_;
        }
        active_ = in;
        app.noteActive(this, in);
        if (focus_) focus_->update();
        if (framed_) {
            statesChanged();
            if (titleBar_) titleBar_->update();
            if (computeFrameInsets() != frame_) applyFrameInsets(isWindowMaximized() || (native_ && native_->isFullScreen()));
            if (computedStyle().props_ != lookWas || computedStyle("frame").props_ != ringWas) invalidate(outerRect());
        }
        if (in) activated.emit();
        break;
    }
    case plat::Event::Resize:
        if (e.w > 0 && e.h > 0 && (e.w != width() || e.h != height())) {
            hgeom_.w = e.w;
            hgeom_.h = e.h;
            setGeometry(0, 0, e.w, e.h);
            placeTitleBar();
            invalidate(outerRect());
        } else if (scale_ != 1 && (fb_.width() != physW_ || fb_.height() != physH_)) {
            invalidate(outerRect());  // (the same logical size, another device size: a new framebuffer)
        } else if (framed_ && (fb_.width() != physW_ || fb_.height() != physH_)) {
            invalidate(outerRect());  // (the inside's size as it was, the frame's another: maximized)
        }
        break;
    case plat::Event::StateChanged:
        frameStateChanged();
        break;
    case plat::Event::Move:
        hgeom_.x = e.x;
        hgeom_.y = e.y;
        break;
    case plat::Event::Expose:
        invalidate(outerRect());
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
    plat::setTransparency(transparency_);
    plat::setCompositingListener([this] { transparencyChanged(); });
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

namespace {
// (util::debug) a frame that took longer than a picture of the screen, on stderr: at most once a second, with how
// many more there were since
struct FrameClock {
    double start, budget;  // (start < 0: not timed)
    ~FrameClock() {
        static double told = -1;
        static int untold = 0;
        if (start < 0) return;
        double end = plat::now(), took = end - start;
        if (took <= budget) return;
        if (told >= 0 && end - told < 1) {
            untold++;
            return;
        }
        fprintf(stderr, "Exbit: slow frame: %.1f ms (%.1f ms a picture at %.0f Hz)", took * 1000, budget * 1000,
                1 / budget);
        if (untold) fprintf(stderr, ", %d more in the last %.1f s", untold, end - told);
        fprintf(stderr, "\n");
        told = end;
        untold = 0;
    }
};
}  // namespace

void App::frameTimer() {
    // the onFrame callbacks (playhead, meters, plugins' idle) at the chosen rate, in step with the
    // screen's pictures (60 a second on a 180 Hz screen: every third); painting at every picture
    FrameClock clock{util::debug() && !headless_ ? plat::now() : -1, 1.0 / displayHz_};
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

void App::setTransparency(bool on) {
    if (on == transparency_) return;
    transparency_ = on;
    if (!headless_) plat::setTransparency(on);  // (its listener: transparencyChanged)
    else transparencyChanged();
}

bool App::transparencyAvailable() const { return !headless_ && plat::compositing(); }

void App::setAlphaWhenHeadless(bool on) {
    if (on == headlessAlpha_) return;
    headlessAlpha_ = on;
    transparencyChanged();
}

void App::transparencyChanged() {
    repaintAll();  // (:transparent: restyled, laid out again; popups' room for their shadows, frames)
    for (Window* w : windows_) w->invalidate(w->outerRect());
}

// every window's frame as the option says (one going may take others with it: its title bar's menu)
void App::updateFrames() {
    for (Window* w : std::vector<Window*>(windows_))
        if (std::find(windows_.begin(), windows_.end(), w) != windows_.end()) w->updateFrame();
}

void App::setOwnFrames(bool on) {
    if (on == ownFrames_) return;
    ownFrames_ = on;
    updateFrames();
}

void App::setFramesWhenHeadless(bool on) {
    if (on == headlessFrames_) return;
    headlessFrames_ = on;
    updateFrames();
}

void App::initHeadless() {
    util::setDebug(false);  // (tests: no diagnostics among their output)
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
    if (headless_ && nestedLoop) {
        nestedLoop(done);
    } else if (headless_) {
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
    updateFrames();  // (the frames' sizes: the skin's)
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
        if (!w->outerScreenGeometry().contains(g)) continue;
        if (!best || w->showSerial_ > best->showSerial_) best = w;
    }
    return best;
}

Rect App::screenAt(Point g) const {
    if (headless_) return headlessScreen;
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
    if (tip_ && !tip_->isWindowShown() && tip_->madeOpaque()) tip_.reset();  // (see-through now: made again)
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
    tipText_ = text;
    Rect scr = screenAt(g);
    Point sz = tw->measure(text, scr.w);
    int x = g.x + 2, y = g.y + 18;
    if (x + sz.x > scr.right()) x = std::max(scr.x, scr.right() - sz.x);
    if (y + sz.y > scr.bottom()) y = std::max(scr.y, g.y - sz.y - 4);
    sz.y = std::min(sz.y, std::max(1, scr.h));  // (taller than the screen: cut)
    if (y + sz.y > scr.bottom()) y = std::max(scr.y, scr.bottom() - sz.y);
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
    for (int pass = 0; pass < 3; ++pass) {  // (a window grown to its content's minimum: laid out again)
        layoutRounds();
        bool grew = false;
        for (Window* w : std::vector<Window*>(windows_)) grew |= Widget::alive(w) && w->followMinimum();
        if (!grew) break;
    }
}

void App::layoutRounds() {
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
