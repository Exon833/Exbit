// Menus (QMenu / QAction / QMenuBar): each open menu is a popup window; submenus open on hover
// after a short delay, a press outside closes the whole chain, the keyboard moves through them.
// The look is default.css's (Lists, scrolling, tabs and menus): Menu (the popup: its box, padding, min-width),
// Menu::item (:hover :disabled :checkable :checked :section :submenu), Menu::separator, Menu::check (the check
// mark at the left), Menu::shortcut (right-aligned), Menu::arrow (of submenus); MenuBar and MenuBar::item
// (:hover :open :disabled).
#include "ui/toolkit/menu.h"

#include <algorithm>
#include <cmath>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {
// "Text\tshown on the right" (as Qt): the part before the tab, and the part after it (a shortcut shown only)
std::string leftPart(const std::string& s) {
    size_t t = s.find('\t');
    return t == std::string::npos ? s : s.substr(0, t);
}
std::string rightPart(const std::string& s) {
    size_t t = s.find('\t');
    return t == std::string::npos ? std::string() : s.substr(t + 1);
}

std::string stripAmp(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') {
                out += '&';
                ++i;
            }
            continue;
        }
        out += s[i];
    }
    return out;
}
bool selectable(const Action* a) { return a->visible && !a->separator && !a->section && a->enabled; }
}  // namespace

// ============================================================================================
// MenuPopup: one open menu
// ============================================================================================

class MenuPopup : public PopupWindow {
public:
    explicit MenuPopup(Menu* m) : menu(m) {
        setStyleType("Menu");
        setMouseTracking(true);
        paintsOwnBox_ = true;
    }
    ~MenuPopup() override {
        if (subTimer >= 0) App::get().cancelTimer(subTimer);
    }
    Menu* menu;
    Widget* owner = nullptr;
    MenuPopup* parentPopup = nullptr;
    MenuPopup* child = nullptr;
    int hover = -1, subTimer = -1, childIndex = -1;
    std::vector<Rect> rects;  // per action (empty: not shown)

    // an item's states (Menu::item, and its ::check, ::shortcut and ::arrow): hover (chosen by the mouse or the
    // keys), disabled, checkable, checked, section (a heading), submenu
    void itemStates(const Action* a, bool hot, std::vector<const char*>& out) const {
        if (hot) out.push_back("hover");
        if (!a->enabled) out.push_back("disabled");
        if (a->checkable) out.push_back("checkable");
        if (a->checkable && a->checked) out.push_back("checked");
        if (a->section) out.push_back("section");
        if (a->submenu) out.push_back("submenu");
    }

    // (default.css: a 1 px edge and 4 px above and below; items 5 px 24 px, their text, then 24 px and the
    // shortcuts; 4 px more with submenus; separators 1 px between 4 px 7 px margins; at least 60 px wide)
    Point measure() {
        const css::Style& ms = computedStyle();
        PartStyles parts(*this);
        Font base = theme::uiFont();
        style::Insets border = style::insets(ms, "border-width"), pad = style::insets(ms, "padding");
        int textCol = 0, scW = 0, padR = 0, gap = 0, arrowRoom = 0;
        auto& acts = menu->actions();
        rects.assign(acts.size(), Rect());
        std::vector<int> heights(acts.size(), 0);
        std::vector<const char*> states;
        for (size_t i = 0; i < acts.size(); ++i) {
            const Action* a = acts[i].get();
            if (!a->visible) continue;
            if (a->separator) {
                const css::Style& sep = parts.get("separator");
                heights[i] = std::max(0, style::px(sep, "height", 1)) + style::insets(sep, "margin").h();
                continue;
            }
            states.clear();
            itemStates(a, false, states);
            const css::Style& item = parts.get("item", states);
            const Font& f = parts.font(item, base);
            style::Insets ip = style::insets(item, "padding"), ib = style::insets(item, "border-width");
            textCol = std::max(textCol, ib.left + ip.left + style::textWidth(f, stripAmp(leftPart(a->text))));
            padR = std::max(padR, ip.right + ib.right);
            heights[i] = style::textHeight(f) + ip.h() + ib.h();
            std::string right = !a->shortcut.empty() ? a->shortcut.text() : rightPart(a->text);
            if (!right.empty()) {
                const css::Style& sc = parts.get("shortcut", states, &item);
                scW = std::max(scW, style::textWidth(parts.font(sc, base), right));
                gap = std::max(gap, style::px(sc, "margin-left", 24));
            }
            if (a->submenu) arrowRoom = std::max(arrowRoom, style::px(parts.get("arrow", states, &item), "margin-left", 4));
        }
        int w = border.left + pad.left + textCol + (scW ? gap + scW : 0) + padR + pad.right + border.right + arrowRoom;
        w = std::max(w, menu->minimumWidth);
        w = std::max(w, style::px(ms, "min-width", 60));
        int y = border.top + pad.top;
        for (size_t i = 0; i < acts.size(); ++i) {
            if (!acts[i]->visible) continue;
            rects[i] = Rect(border.left + pad.left, y, std::max(0, w - border.w() - pad.w()), heights[i]);
            y += heights[i];
        }
        return Point(w, y + pad.bottom + border.bottom);
    }

    int itemAt(Point p) const {
        for (size_t i = 0; i < rects.size(); ++i)
            if (!rects[i].empty() && rects[i].contains(p)) return (int)i;
        return -1;
    }

    // a disabled item's tooltip (Action::tooltip): why it can't be chosen now
    std::string toolTipAt(Point p) const override {
        int i = itemAt(p);
        auto& acts = menu->actions();
        if (i < 0 || i >= (int)acts.size()) return "";
        const Action* a = acts[i].get();
        return a->enabled || a->separator || a->section ? std::string() : a->tooltip;
    }

    void paint(Painter& p) override {
        // its box (Menu), each separator's (Menu::separator, inside its margins) and item's (Menu::item in its
        // states): its check mark (::check), text, shortcut (::shortcut) and submenu arrow (::arrow)
        style::box(p, rect(), computedStyle());
        PartStyles parts(*this);
        Font base = theme::uiFont();
        auto& acts = menu->actions();
        std::vector<const char*> states;
        for (size_t i = 0; i < acts.size() && i < rects.size(); ++i) {
            const Action* a = acts[i].get();
            Rect r = rects[i];
            if (r.empty()) continue;
            if (a->separator) {
                const css::Style& sep = parts.get("separator");
                Rect line = style::inside(r, style::insets(sep, "margin"));
                line.h = std::max(0, style::px(sep, "height", 1));
                style::box(p, line, sep);
                continue;
            }
            states.clear();
            itemStates(a, (int)i == hover && selectable(a), states);
            const css::Style& item = parts.get("item", states);
            style::box(p, r, item);
            Rect in = style::inside(r, style::insets(item, "border-width"));
            Rect c = style::contents(r, item);
            Color tc = style::foreground(item);
            if (a->checkable && a->checked) {
                const css::Style& ck = parts.get("check", states, &item);
                float size = (float)std::max(0, style::px(ck, "width", 10));
                style::icon(p, (float)(in.x + style::px(ck, "margin-left", 7)), in.y + (in.h - size) / 2.f, size, "check",
                            style::foreground(ck));
            }
            style::text(p, c, AlignLeft | AlignVCenter, stripAmp(leftPart(a->text)), parts.font(item, base), tc, item);
            std::string right = !a->shortcut.empty() ? a->shortcut.text() : rightPart(a->text);
            if (!right.empty()) {
                const css::Style& sc = parts.get("shortcut", states, &item);
                style::text(p, Rect(in.x, c.y, std::max(0, c.right() - in.x), c.h), AlignRight | AlignVCenter, right,
                            parts.font(sc, base), style::foreground(sc), sc);
            }
            if (a->submenu) {
                const css::Style& ar = parts.get("arrow", states, &item);
                int aw = std::max(0, style::px(ar, "width", 10));
                style::icon(p, (float)(in.right() - style::px(ar, "margin-right", 6) - aw), in.y + (in.h - aw) / 2.f,
                            (float)aw, "right", style::foreground(ar));
            }
        }
    }

    MenuPopup* rootPopup() {
        MenuPopup* p = this;
        while (p->parentPopup) p = p->parentPopup;
        return p;
    }

    void cancelTimer() {
        if (subTimer >= 0) {
            App::get().cancelTimer(subTimer);
            subTimer = -1;
        }
    }

    void closeChild() {
        cancelTimer();
        if (!child) return;
        MenuPopup* c = child;
        child = nullptr;
        childIndex = -1;
        c->closeChild();
        App::get().closePopup(c);
        update();
    }

    void closeChain() {
        closeChild();
        App::get().closePopup(this);
    }

    void dismiss() override { closeChain(); }

    void hideEvent() override {
        cancelTimer();
        Menu* m = menu;
        if (m->aboutToHide) m->aboutToHide();
        m->hidden.emit();
    }

    void openSubmenu(int i, bool selectFirst) {
        cancelTimer();
        auto& acts = menu->actions();
        if (i < 0 || i >= (int)acts.size()) return;
        Action* a = acts[i].get();
        if (!a->submenu || !a->enabled) return;
        if (child && childIndex == i) {
            if (selectFirst) child->selectNext(1, -1);
            return;
        }
        closeChild();
        Menu* sm = a->submenu;
        if (sm->aboutToShow) sm->aboutToShow(sm);
        if (sm->isEmpty()) return;
        if (sm->popup_ && !sm->popup_->isOpen() && sm->popup_->madeOpaque()) sm->popup_.reset();  // (made again)
        if (!sm->popup_) sm->popup_ = std::make_unique<MenuPopup>(sm);
        MenuPopup* c = sm->popup_.get();
        c->owner = owner;
        c->parentPopup = this;
        c->hover = -1;
        Point sz = c->measure();
        Rect me = screenGeometry();
        Rect r = rects[i];
        // (its first item level with this one: above it by its edge and padding)
        const css::Style& cs = c->computedStyle();
        int above = style::insets(cs, "border-width").top + style::insets(cs, "padding").top;
        int x = me.right() - 3, y = me.y + r.y - above;
        Rect scr = App::get().screenAt(Point(me.x + me.w / 2, me.y + r.y));
        if (x + sz.x > scr.right()) x = std::max(scr.x, me.x - sz.x + 3);
        if (y + sz.y > scr.bottom()) y = std::max(scr.y, scr.bottom() - sz.y);
        child = c;
        childIndex = i;
        if (selectFirst) c->selectNext(1, -1);
        c->open(Point(x, y), sz.x, sz.y, owner);
        update();
    }

    void scheduleSubmenu(int i) {
        cancelTimer();
        auto& acts = menu->actions();
        bool hasSub = i >= 0 && acts[i]->submenu && acts[i]->enabled;
        if (child && childIndex == i) return;
        if (!hasSub && !child) return;
        subTimer = App::get().singleShot(hasSub ? 250 : 300, [this, i, hasSub] {
            subTimer = -1;
            if (hover != i) return;
            if (hasSub) openSubmenu(i, false);
            else closeChild();
        });
    }

    void selectNext(int dir, int from) {
        auto& acts = menu->actions();
        int n = (int)acts.size();
        if (!n) return;
        int i = from;
        for (int k = 0; k < n; ++k) {
            i = ((i + dir) % n + n) % n;
            if (from < 0 && k == 0 && dir > 0) i = 0;
            if (selectable(acts[i].get())) {
                hover = i;
                update();
                return;
            }
        }
    }

    void choose(Action* a) {
        MenuPopup* root = rootPopup();
        Menu* rootMenu = root->menu;
        rootMenu->chosen_ = a;
        bool execing = rootMenu->execing_;
        root->closeChain();
        if (!execing) {
            std::weak_ptr<bool> alive = rootMenu->alive_;
            App::get().singleShot(0, [alive, rootMenu, a] {
                auto p = alive.lock();
                if (p && *p && rootMenu->contains(a)) {
                    rootMenu->chosen_ = nullptr;
                    Menu::activate(a);
                }
            });
        }
    }

    bool mouseMove(MouseEvent& e) override {
        int i = rect().contains(e.pos) ? itemAt(e.pos) : -1;
        auto& acts = menu->actions();
        if (i >= 0 && !selectable(acts[i].get())) i = -1;
        if (!rect().contains(e.pos)) {
            // the pointer left for the parent menu (or outside): keep an open submenu's item lit
            if (child) i = childIndex;
            else i = -1;
        }
        if (i != hover) {
            hover = i;
            update();
        }
        if (rect().contains(e.pos)) scheduleSubmenu(i);
        if (parentPopup && rect().contains(e.pos)) {
            // the parent keeps its item (this submenu) lit
            parentPopup->cancelTimer();
            if (parentPopup->hover != parentPopup->childIndex) {
                parentPopup->hover = parentPopup->childIndex;
                parentPopup->update();
            }
        }
        return true;
    }

    bool mousePress(MouseEvent& e) override { return true; }

    bool mouseRelease(MouseEvent& e) override {
        if (!rect().contains(e.pos)) return true;
        int i = itemAt(e.pos);
        if (i < 0) return true;
        Action* a = menu->actions()[i].get();
        if (!selectable(a)) return true;
        if (a->submenu) {
            openSubmenu(i, false);
            return true;
        }
        choose(a);
        return true;
    }

    bool wheel(WheelEvent&) override { return true; }

    bool keyPress(KeyEvent& e) override {
        auto& acts = menu->actions();
        MenuBar* bar = dynamic_cast<MenuBar*>(rootPopup()->owner);
        switch (e.key) {
        case plat::Key_Up: selectNext(-1, hover < 0 ? 0 : hover); return true;
        case plat::Key_Down: selectNext(1, hover); return true;
        case plat::Key_Home: selectNext(1, -1); return true;
        case plat::Key_End: selectNext(-1, 0); return true;
        case plat::Key_Right:
            if (hover >= 0 && acts[hover]->submenu && acts[hover]->enabled) openSubmenu(hover, true);
            else if (bar && Widget::alive(bar)) bar->navigate(1);
            return true;
        case plat::Key_Left:
            if (parentPopup) parentPopup->closeChild();
            else if (bar && Widget::alive(bar)) bar->navigate(-1);
            return true;
        case plat::Key_Return:
        case plat::Key_Enter:
        case plat::Key_Space:
            if (hover >= 0 && selectable(acts[hover].get())) {
                if (acts[hover]->submenu) openSubmenu(hover, true);
                else choose(acts[hover].get());
            }
            return true;
        case plat::Key_Escape:
            if (parentPopup) parentPopup->closeChild();
            else closeChain();
            return true;
        default:
            break;
        }
        // a letter: the next item starting with it
        if (!e.text.empty() && (unsigned char)e.text[0] > 0x20 && !(e.mods & (plat::ModCtrl | plat::ModAlt))) {
            std::string ch = e.text;
            for (auto& c : ch)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            int n = (int)acts.size();
            for (int k = 1; k <= n; ++k) {
                int i = ((hover < 0 ? -1 : hover) + k + n) % n;
                std::string t = stripAmp(acts[i]->text);
                for (auto& c : t)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (selectable(acts[i].get()) && t.compare(0, ch.size(), ch) == 0) {
                    hover = i;
                    update();
                    break;
                }
            }
        }
        return true;
    }
};

// ============================================================================================
// Menu
// ============================================================================================

Menu::Menu(const std::string& t) : title(t) {}

Menu::~Menu() {
    *alive_ = false;
    if (popup_ && popup_->isOpen()) popup_->closeChain();
    shortcuts_.clear();
}

Action* Menu::addAction(const std::string& text, std::function<void()> fn, const std::string& shortcut) {
    auto a = std::make_unique<Action>();
    a->text = text;
    a->triggered = std::move(fn);
    if (!shortcut.empty()) a->shortcut = KeySequence::parse(shortcut);
    a->menu = this;
    Action* raw = a.get();
    actions_.push_back(std::move(a));
    return raw;
}

Action* Menu::addCheckable(const std::string& text, bool checked, std::function<void(bool)> fn, const std::string& shortcut) {
    Action* a = addAction(text, nullptr, shortcut);
    a->checkable = true;
    a->checked = checked;
    a->toggled = std::move(fn);
    return a;
}

Action* Menu::addSeparator() {
    Action* a = addAction("");
    a->separator = true;
    return a;
}

Menu* Menu::addMenu(const std::string& t) {
    auto m = std::make_unique<Menu>(t);
    m->parentMenu = this;
    Menu* raw = m.get();
    submenus_.push_back(std::move(m));
    Action* a = addAction(t);
    a->submenu = raw;
    return raw;
}

Action* Menu::addSection(const std::string& text) {
    Action* a = addAction(text);
    a->section = true;
    a->enabled = false;
    return a;
}

void Menu::clear() {
    if (popup_ && popup_->child) popup_->closeChild();
    shortcuts_.clear();
    actions_.clear();
    submenus_.clear();
    chosen_ = nullptr;
    if (popup_ && popup_->isOpen()) {
        Point sz = popup_->measure();
        popup_->hover = -1;
        popup_->resizeWindow(sz.x, sz.y);
        popup_->update();
    }
}

bool Menu::contains(const Action* a) const {
    for (auto& x : actions_) {
        if (x.get() == a) return true;
        if (x->submenu && x->submenu->contains(a)) return true;
    }
    return false;
}

void Menu::show(Point at, const Rect* anchor, Widget* owner) {
    if (aboutToShow) aboutToShow(this);
    if (actions_.empty()) return;
    if (popup_ && !popup_->isOpen() && popup_->madeOpaque()) popup_.reset();  // (see-through now: made again)
    if (!popup_) popup_ = std::make_unique<MenuPopup>(this);
    MenuPopup* p = popup_.get();
    if (p->isOpen()) p->closeChain();
    p->owner = owner;
    p->parentPopup = nullptr;
    p->hover = -1;
    Point sz = p->measure();
    Point pos;
    if (anchor) {
        pos = placeBelow(*anchor, sz.x, sz.y);
    } else {
        Rect scr = App::get().screenAt(at);
        int x = at.x, y = at.y;
        if (x + sz.x > scr.right()) x = at.x - sz.x >= scr.x ? at.x - sz.x : std::max(scr.x, scr.right() - sz.x);
        if (y + sz.y > scr.bottom()) y = at.y - sz.y >= scr.y ? at.y - sz.y : std::max(scr.y, scr.bottom() - sz.y);
        pos = Point(x, y);
    }
    chosen_ = nullptr;
    p->open(pos, sz.x, sz.y, owner);
}

void Menu::popup(Point globalPos, Widget* owner) { show(globalPos, nullptr, owner); }

void Menu::popupBelow(const Rect& anchor, Widget* owner) { show(Point(anchor.x, anchor.bottom()), &anchor, owner); }

Action* Menu::exec(Point globalPos, Widget* owner) {
    execing_ = true;
    show(globalPos, nullptr, owner);
    std::weak_ptr<bool> alive = alive_;
    if (isOpen()) App::get().runUntil([this, alive] {
        auto p = alive.lock();
        return !p || !*p || !isOpen();
    });
    auto p = alive.lock();
    if (!p || !*p) return nullptr;
    execing_ = false;
    Action* a = chosen_;
    chosen_ = nullptr;
    if (a && contains(a)) {
        activate(a);
        return a;
    }
    return nullptr;
}

Action* Menu::execBelow(const Rect& anchor, Widget* owner) {
    execing_ = true;
    show(Point(anchor.x, anchor.bottom()), &anchor, owner);
    std::weak_ptr<bool> alive = alive_;
    if (isOpen()) App::get().runUntil([this, alive] {
        auto p = alive.lock();
        return !p || !*p || !isOpen();
    });
    auto p = alive.lock();
    if (!p || !*p) return nullptr;
    execing_ = false;
    Action* a = chosen_;
    chosen_ = nullptr;
    if (a && contains(a)) {
        activate(a);
        return a;
    }
    return nullptr;
}

void Menu::close() {
    if (popup_ && popup_->isOpen()) popup_->closeChain();
}

bool Menu::isOpen() const { return popup_ && popup_->isOpen(); }

void Menu::activate(Action* a) {
    if (!a || !a->enabled || a->separator || a->section || a->submenu) return;
    Menu* m = a->menu;
    std::vector<std::pair<std::function<void(bool)>, bool>> toggles;
    if (a->checkable) {
        if (a->group && m) {
            bool was = a->checked;
            for (auto& o : m->actions_)
                if (o.get() != a && o->group == a->group && o->checkable && o->checked) {
                    o->checked = false;
                    if (o->toggled) toggles.push_back({o->toggled, false});
                }
            a->checked = true;
            if (!was && a->toggled) toggles.push_back({a->toggled, true});
        } else {
            a->checked = !a->checked;
            if (a->toggled) toggles.push_back({a->toggled, a->checked});
        }
    }
    std::function<void()> trig = a->triggered;
    std::vector<std::pair<std::weak_ptr<bool>, Menu*>> menus;
    for (Menu* x = m; x; x = x->parentMenu) menus.push_back({x->alive_, x});
    for (auto& t : toggles) t.first(t.second);
    if (trig) trig();
    for (auto& wm : menus) {
        auto p = wm.first.lock();
        if (!p || !*p) break;
        // (the action may be gone by now: only menus still alive hear of it)
        wm.second->triggeredAction.emit(a);
    }
}

void Menu::registerShortcuts(Widget* owner, ShortcutScope scope) {
    shortcuts_.clear();
    for (auto& up : actions_) {
        Action* a = up.get();
        if (a->submenu) {
            a->submenu->registerShortcuts(owner, scope);
            continue;
        }
        if (a->shortcut.empty() || a->separator) continue;
        auto s = std::make_unique<Shortcut>(owner, a->shortcut, [a] { Menu::activate(a); }, scope);
        s->active = [a] { return a->enabled && a->visible; };
        shortcuts_.push_back(std::move(s));
    }
}

// ============================================================================================
// MenuBar
// ============================================================================================

MenuBar::MenuBar(Widget* parent) : Widget(parent) {
    setStyleType("MenuBar");
    setMouseTracking(true);
    vPolicy = SizePolicy::Fixed;
}

Menu* MenuBar::addMenu(const std::string& title) {
    auto m = std::make_unique<Menu>(title);
    Menu* raw = m.get();
    int index = (int)menus_.size();
    raw->hidden.connect([this, index] {
        if (open_ == index) {
            open_ = -1;
            update();
        }
    });
    menus_.push_back(std::move(m));
    updateGeometry();
    update();
    return raw;
}

void MenuBar::itemStates(int i, std::vector<const char*>& out) const {
    if (i == hover_ && isEnabled()) out.push_back("hover");
    if (i == open_) out.push_back("open");
}

std::vector<Rect> MenuBar::itemRects() const {
    // each title with its padding and border (default.css: 8 px either side), as tall as the bar
    PartStyles parts(*this);
    Font base = theme::uiFont();
    std::vector<Rect> out;
    std::vector<const char*> states;
    int x = 0;
    for (int k = 0; k < (int)menus_.size(); ++k) {
        states.clear();
        itemStates(k, states);
        const css::Style& item = parts.get("item", states);
        int w = style::textWidth(parts.font(item, base), stripAmp(menus_[k]->title)) + style::insets(item, "padding").w() +
                style::insets(item, "border-width").w();
        out.push_back(Rect(x, 0, w, height()));
        x += w;
    }
    return out;
}

Rect MenuBar::itemRect(int i) const {
    std::vector<Rect> r = itemRects();
    return i >= 0 && i < (int)r.size() ? r[i] : Rect();
}

int MenuBar::itemAt(Point p) const {
    std::vector<Rect> r = itemRects();
    int i = -1;
    for (int k = 0; k < (int)r.size(); ++k)
        if (r[k].contains(p)) i = k;
    return i;
}

Point MenuBar::sizeHint() const {
    // (default.css: the titles' height and 4 px above and below)
    PartStyles parts(*this);
    const css::Style& item = parts.get("item");
    Font f = style::font(item, theme::uiFont());
    int w = 0;
    for (const Rect& r : itemRects()) w += r.w;
    return Point(w, style::textHeight(f) + style::insets(item, "padding").h() + style::insets(item, "border-width").h());
}

void MenuBar::paint(Painter& p) {
    style::box(p, rect(), computedStyle());
    PartStyles parts(*this);
    Font base = theme::uiFont();
    std::vector<Rect> rects = itemRects();
    std::vector<const char*> states;
    for (int i = 0; i < (int)menus_.size() && i < (int)rects.size(); ++i) {
        states.clear();
        itemStates(i, states);
        const css::Style& item = parts.get("item", states);
        style::box(p, rects[i], item);
        style::text(p, style::contents(rects[i], item), AlignCenter, stripAmp(menus_[i]->title), parts.font(item, base),
                    style::foreground(item), item);
    }
}

bool MenuBar::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    int i = itemAt(e.pos);
    if (i < 0) return false;
    if (open_ == i) {
        menus_[i]->close();
        open_ = -1;
        update();
    } else {
        openMenu(i);
    }
    return true;
}

bool MenuBar::mouseMove(MouseEvent& e) {
    int i = itemAt(e.pos);
    if (i != hover_) {
        hover_ = i;
        update();
    }
    if (open_ >= 0 && i >= 0 && i != open_) openMenu(i);
    return i >= 0;
}

void MenuBar::openMenu(int i) {
    if (i < 0 || i >= (int)menus_.size()) return;
    if (open_ >= 0 && open_ < (int)menus_.size()) menus_[open_]->close();
    App::get().closeAllPopups();
    open_ = i;
    update();
    Rect r = itemRect(i);
    Point g = mapToGlobal(Point(r.x, r.y));
    menus_[i]->popupBelow(Rect(g.x, g.y, r.w, r.h), this);
    if (!menus_[i]->isOpen()) open_ = -1;
    update();
}

void MenuBar::navigate(int dir) {
    int n = (int)menus_.size();
    if (!n) return;
    int i = open_ < 0 ? 0 : ((open_ + dir) % n + n) % n;
    openMenu(i);
    if (menus_[i]->popup_) menus_[i]->popup_->selectNext(1, -1);
}

void MenuBar::registerShortcuts(ShortcutScope scope) {
    for (auto& m : menus_) m->registerShortcuts(this, scope);
}

}  // namespace ui
