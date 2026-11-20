// Menus (QMenu, QAction, QActionGroup, QMenuBar): popup windows above everything, with
// submenus, separators, check marks, shortcuts shown on the right, disabled items, and keyboard
// navigation (arrows, Return, Escape, the item's &-mnemonic is not used).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/toolkit/widget.h"

namespace ui {

class Menu;

class Action {
public:
    std::string text;
    std::string tooltip;         // (shown while the pointer rests on it when it is disabled: why)
    KeySequence shortcut;        // shown on the right; also triggers it when the menu's owner window has it registered
    bool checkable = false;
    bool checked = false;
    bool enabled = true;
    bool visible = true;
    bool separator = false;
    bool section = false;        // a heading (addSection): bold, never chosen
    Menu* submenu = nullptr;     // owned by the parent menu
    Menu* menu = nullptr;        // the menu it is in
    std::string data;            // for the program
    int group = 0;               // exclusive group id (0: none): checking one unchecks the others
    std::function<void()> triggered;           // when chosen
    std::function<void(bool)> toggled;         // checkable: the new state
    void setChecked(bool on) { checked = on; }
    void setEnabled(bool on) { enabled = on; }
};

class Menu {
public:
    explicit Menu(const std::string& title = "");
    ~Menu();
    std::string title;
    Action* addAction(const std::string& text, std::function<void()> fn = nullptr, const std::string& shortcut = "");
    Action* addCheckable(const std::string& text, bool checked, std::function<void(bool)> fn = nullptr,
                         const std::string& shortcut = "");
    Action* addSeparator();
    Menu* addMenu(const std::string& title);       // a submenu (owned)
    Action* addSection(const std::string& text);   // a disabled, bold heading ("Recently tweaked")
    void clear();
    bool isEmpty() const { return actions_.empty(); }
    const std::vector<std::unique_ptr<Action>>& actions() const { return actions_; }
    // called each time just before it opens (to fill it)
    std::function<void(Menu*)> aboutToShow;
    std::function<void()> aboutToHide;
    // shows it at a screen position (its top-left there; kept on screen) and returns at once
    void popup(Point globalPos, Widget* owner = nullptr);
    // shows it and waits; returns the chosen action (nullptr: none; it was dismissed). The chosen
    // action's toggled / triggered run before it returns.
    Action* exec(Point globalPos, Widget* owner = nullptr);
    // the same, under a widget's screen rectangle (above it when there is no room below): button menus
    Action* execBelow(const Rect& anchor, Widget* owner = nullptr);
    void popupBelow(const Rect& anchor, Widget* owner = nullptr);
    // triggers an action of this menu (or a submenu) as if chosen: checks it (exclusive groups), then
    // toggled / triggered
    static void activate(Action* a);
    // (tinydaw --tui: menus shown as text) a choice as a click on it: an exec()'d menu (or its submenu's) returns it,
    // else it is activated; and the menu a popup window shows (null: not a menu's)
    static void choose(Action* a);
    static Menu* ofWindow(const Window* w);
    Signal<Action*> triggeredAction;  // any of its actions chosen (QMenu::triggered)
    Signal<> hidden;                  // its popup closed (after aboutToHide)
    void close();
    bool isOpen() const;
    // registers the actions' shortcuts on a window (QMainWindow menus): they work while it has the focus
    void registerShortcuts(Widget* owner, ShortcutScope scope = ShortcutScope::Window);
    int minimumWidth = 0;
    Menu* parentMenu = nullptr;      // (set for submenus)
    bool contains(const Action* a) const;  // one of its actions, or of a submenu's
    class MenuPopup* popupWindow() const { return popup_.get(); }
private:
    friend class MenuBar;
    Action* chosen_ = nullptr;
    bool execing_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    void show(Point at, const Rect* anchor, Widget* owner);
    std::vector<std::unique_ptr<Action>> actions_;
    std::vector<std::unique_ptr<Menu>> submenus_;
    std::unique_ptr<class MenuPopup> popup_;
    std::vector<std::unique_ptr<Shortcut>> shortcuts_;
    friend class MenuPopup;
};

// the menu bar across the top of the main window. Styles: MenuBar (its box), MenuBar::item (each title's box:
// :hover, :open while its menu is open, :disabled)
class MenuBar : public Widget {
public:
    explicit MenuBar(Widget* parent = nullptr);
    Menu* addMenu(const std::string& title);        // owned
    const std::vector<std::unique_ptr<Menu>>& menus() const { return menus_; }
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    void leave() override { hover_ = -1; update(); }
    void registerShortcuts(ShortcutScope scope = ShortcutScope::Window);  // every menu's actions
    void navigate(int dir);                         // (from an open menu's Left / Right keys)
    int openIndex() const { return open_; }
    // opens menu i under its title, as a click on the title does (the title :open while it is open)
    void openMenu(int i);
    Rect itemRect(int i) const;                         // title i's (in the bar's coordinates)
private:
    std::vector<std::unique_ptr<Menu>> menus_;
    int hover_ = -1, open_ = -1;
    std::vector<Rect> itemRects() const;                // every title's (each as wide as its style makes it)
    int itemAt(Point p) const;
    void itemStates(int i, std::vector<const char*>& out) const;
};

}  // namespace ui
