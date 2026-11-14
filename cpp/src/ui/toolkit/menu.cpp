// Menus (QMenu / QAction / QMenuBar): each open menu is a popup window; submenus open on hover
// after a short delay, a press outside closes the whole chain, the keyboard moves through them.
// The look: theme.py's QMenu rules (base fill, 1 px line edge, 4 px above and below, items with
// 5 px 24 px padding, the highlight colour under the pointer, separators 1 px with 4 px 8 px
// margins, a check mark at the left, the shortcut right-aligned, ▸ for submenus).
#include "ui/toolkit/menu.h"

#include <algorithm>
#include <cmath>

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
    explicit MenuPopup(Menu* m) : menu(m) { setMouseTracking(true); }
    ~MenuPopup() override {
        if (subTimer >= 0) App::get().cancelTimer(subTimer);
    }
    Menu* menu;
    Widget* owner = nullptr;
    MenuPopup* parentPopup = nullptr;
    MenuPopup* child = nullptr;
    int hover = -1, subTimer = -1, childIndex = -1;
    std::vector<Rect> rects;  // per action (empty: not shown)
    int itemH = 24;

    static Font font() { return theme::uiFont(); }

    Point measure() {
        Font f = font();
        itemH = style::textHeight(f) + 10;
        int textW = 0, scW = 0;
        bool anySub = false;
        auto& acts = menu->actions();
        rects.assign(acts.size(), Rect());
        for (auto& a : acts) {
            if (!a->visible || a->separator) continue;
            Font ff = a->section ? f.withBold(true) : f;
            textW = std::max(textW, style::textWidth(ff, stripAmp(leftPart(a->text))));
            if (!a->shortcut.empty()) scW = std::max(scW, style::textWidth(f, a->shortcut.text()));
            if (!rightPart(a->text).empty()) scW = std::max(scW, style::textWidth(f, rightPart(a->text)));
            if (a->submenu) anySub = true;
        }
        int w = 1 + 24 + textW + (scW ? 24 + scW : 0) + 24 + 1;
        if (anySub) w += 4;
        w = std::max(w, menu->minimumWidth);
        w = std::max(w, 60);
        int y = 1 + 4;
        for (size_t i = 0; i < acts.size(); ++i) {
            const Action* a = acts[i].get();
            if (!a->visible) continue;
            int h = a->separator ? 9 : itemH;
            rects[i] = Rect(1, y, w - 2, h);
            y += h;
        }
        return Point(w, y + 4 + 1);
    }

    int itemAt(Point p) const {
        for (size_t i = 0; i < rects.size(); ++i)
            if (!rects[i].empty() && rects[i].contains(p)) return (int)i;
        return -1;
    }

    void paint(Painter& p) override {
        const theme::Shades& sh = theme::shades();
        p.fillRect(rect(), sh.line);
        p.fillRect(rect().adjusted(1, 1, -1, -1), theme::c("base"));
        Font f = font();
        auto& acts = menu->actions();
        for (size_t i = 0; i < acts.size() && i < rects.size(); ++i) {
            const Action* a = acts[i].get();
            Rect r = rects[i];
            if (r.empty()) continue;
            if (a->separator) {
                p.fillRect(Rect(r.x + 8 - 1, r.y + 4, r.w - 16 + 2, 1), sh.line);
                continue;
            }
            bool hot = (int)i == hover && selectable(a);
            if (hot) p.fillRect(r, theme::c("highlight"));
            Color tc = !a->enabled || a->section ? theme::c("disabled_text") : hot ? theme::c("highlighted_text") : theme::c("text");
            if (a->checkable && a->checked)
                style::icon(p, r.x + 6 + 1.f, r.y + (r.h - 12) / 2.f + 1, 10, "check",
                            a->enabled ? theme::c("text") : theme::c("disabled_text"));
            Rect tr(r.x + 24, r.y, std::max(0, r.w - 48), r.h);
            style::text(p, tr, AlignLeft | AlignVCenter, stripAmp(leftPart(a->text)), a->section ? f.withBold(true) : f, tc);
            std::string right = !a->shortcut.empty() ? a->shortcut.text() : rightPart(a->text);
            if (!right.empty())
                style::text(p, Rect(r.x, r.y, r.w - 24, r.h), AlignRight | AlignVCenter, right, f, tc);
            if (a->submenu) style::icon(p, (float)(r.right() - 6 - 10), r.y + (r.h - 10) / 2.f, 10, "right", tc);
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
        if (!sm->popup_) sm->popup_ = std::make_unique<MenuPopup>(sm);
        MenuPopup* c = sm->popup_.get();
        c->owner = owner;
        c->parentPopup = this;
        c->hover = -1;
        Point sz = c->measure();
        Rect me = screenGeometry();
        Rect r = rects[i];
        int x = me.right() - 3, y = me.y + r.y - 5;
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

Rect MenuBar::itemRect(int i) const {
    Font f = theme::uiFont();
    int x = 0;
    for (int k = 0; k < (int)menus_.size(); ++k) {
        int w = style::textWidth(f, stripAmp(menus_[k]->title)) + 16;
        if (k == i) return Rect(x, 0, w, height());
        x += w;
    }
    return Rect();
}

Point MenuBar::sizeHint() const {
    Font f = theme::uiFont();
    int w = 0;
    for (auto& m : menus_) w += style::textWidth(f, stripAmp(m->title)) + 16;
    return Point(w, style::textHeight(f) + 8);
}

void MenuBar::paint(Painter& p) {
    const theme::Shades& sh = theme::shades();
    Font f = theme::uiFont();
    for (int i = 0; i < (int)menus_.size(); ++i) {
        Rect r = itemRect(i);
        if (i == open_) p.fillRect(r, sh.pressed);
        else if (i == hover_ && isEnabled()) p.fillRect(r, sh.hover);
        style::text(p, r, AlignCenter, stripAmp(menus_[i]->title), f,
                    isEnabled() ? theme::c("window_text") : theme::c("disabled_text"));
    }
}

bool MenuBar::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    int i = -1;
    for (int k = 0; k < (int)menus_.size(); ++k)
        if (itemRect(k).contains(e.pos)) i = k;
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
    int i = -1;
    for (int k = 0; k < (int)menus_.size(); ++k)
        if (itemRect(k).contains(e.pos)) i = k;
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
