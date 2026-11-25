// Every command of the program in one list, each bound to keys you choose (Options → Keyboard shortcuts…) and
// found by typing (the command palette: Ctrl+Shift+P; exbit --tui: Ctrl+P).
//
// - The menu bar's entries are walked as they are (with their submenus): nothing lists them by hand, so a menu
//   entry added anywhere is a command at once. Each one's id is its menu path in English, without the &
//   mnemonic: "Edit/Undo", "Options/Skin/Edit this skin…" (two entries of one menu with the same text: "… (2)").
//   Ids are what keymaps keep, so they stay the same from build to build (translations will keep the English
//   path as the id). Enabled, checkable and checked come from the entries themselves.
// - Keys that aren't menu entries (Space plays, R records, the timeline's S splits…) are registered by their
//   widgets (KeyCommand: TransportBar, Timeline, LanePanel registerCommands) so that they can be rebound too.
//   Those a widget handles in its own keyPress (the timeline's) go through translate(): a key bound to one of
//   them comes to the widget as its default key, and a default key taken away does nothing there.
// - The right-click menus of what is selected (a clip's, a track's) are commands of the palette only (their
//   texts follow the selection: "Duplicate clips", "Duplicate range"); their keys are the timeline's.
// - Keys: several per command. The menus show the first one. The keymap is a text file in the settings folder
//   (keymap.txt, written on every change: only the commands whose keys you changed), one line per key:
//       # comments
//       Edit/Undo = Ctrl+Z
//       Edit/Redo = Ctrl+Shift+Z
//       Edit/Redo = Ctrl+Y
//       Transport/Loop =              (no keys at all)
//   A command in the file gets exactly the keys listed for it; the others keep theirs. Export writes every
//   command's keys (a keymap to share); Import takes a file's lines over. A line that can't be read is skipped and
//   named ("keymap.txt line 7: …"); ids not known (a menu filled only when it opens, another version's) are kept.
//
// (Later the same ids can be learnt from a MIDI controller: run(id) runs any of them.)
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace commands {

using Keys = std::vector<ui::KeySequence>;

// a command as the dialog and the palette show it
struct Command {
    std::string id;       // "Edit/Undo"
    std::string name;     // "Undo"
    std::string where;    // "Edit" (the menu path above it, or "Transport", "Timeline", "Clip"…)
    Keys keys, defaults;
    ui::Action* action = nullptr;  // its menu entry (null: a key only)
    std::string aliasOf;  // a menu entry showing another command's keys (File/Add audio or MIDI file…: Transport's)
    bool enabled = true, visible = true, checkable = false, checked = false;
    bool context = false;  // of a right-click menu (the palette only)
    std::string tooltip;   // (disabled: often why)
};

// a key that isn't a menu entry, registered by its widget
struct KeyCommand {
    std::string id, name;  // "Transport/Play", "Play"
    Keys defaults;
    std::function<void()> run;
    std::function<bool()> active;  // false: the key goes on as if unbound (null: always)
    ui::Widget* owner = nullptr;   // its keys work while it is shown (its window's, or every window's: scope)
    ui::ShortcutScope scope = ui::ShortcutScope::Window;
    std::function<void(const ui::KeySequence&)> shown;  // told its first key (a button showing it; null key: none)
    std::string widget;  // set: keys the widget's keyPress handles itself ("Timeline": translate()), owner unused
};

class Registry {
public:
    explicit Registry(ui::MenuBar* bar);
    ~Registry();

    // --- set up (then load(), apply()) ---
    void add(KeyCommand c);
    // a menu entry's keys: every window's (undo from the MIDI editor), more default keys, run even when the entry
    // shows disabled (its state is only brought up to date when the menu opens)
    void menuKeys(const std::string& id, ui::ShortcutScope scope, Keys more = {}, bool evenDisabled = false);
    // a menu entry that only shows a key command's keys (the key command runs; the entry isn't listed apart)
    void alias(const std::string& menuId, const std::string& keyId);
    // the right-click menus of what is selected now, for the palette: (title, menu) — "Clip", "Track"
    std::function<void(std::vector<std::pair<std::string, std::shared_ptr<ui::Menu>>>&)> contextMenus;
    std::function<void(const std::string&)> message;  // the status bar

    // --- the commands ---
    // every one; palette: the menus filled as when they open (their aboutToShow: up-to-date states), the
    // context menus added (kept until the next call), hidden ones left out
    std::vector<Command> list(bool palette = false);
    Keys keys(const std::string& id) const;
    Keys defaults(const std::string& id) const;
    bool known(const std::string& id) const;
    void setKeys(const std::string& id, Keys keys);  // (then applied and saved)
    void reset(const std::string& id);
    void resetAll();
    std::vector<std::string> usersOf(const ui::KeySequence& k, const std::string& except = "") const;
    bool run(const std::string& id);
    bool run(const Command& c);  // (a context menu's entry: from the last list(true))
    // a key a widget handles itself (keyPress): false: swallow it (its default key was taken away); else e is
    // the key to handle (a rebound key turned into the command's default key)
    bool translate(const std::string& widget, ui::KeyEvent& e) const;
    // a right-click menu's entries showing default keys show the keys now bound instead
    void showKeys(ui::Menu& menu) const;
    // the menus' shown keys and every key's shortcut, as bound now
    void apply();

    // --- recent (the palette's first rows) ---
    std::vector<std::string> recent;
    void noteRecent(const std::string& id);

    // --- the keymap file ---
    std::string path;  // keymap.txt in the settings folder ("" none: tests)
    std::string text(bool every) const;  // every: all commands' keys (export); else the changed ones
    // the lines read over the keys (a command named gets the keys listed for it); problems: the lines skipped
    std::vector<std::string> load(const std::string& text, const std::string& name);
    std::vector<std::string> loadFile(const std::string& file);  // (missing: nothing, no problem)
    bool save() const;
    bool exportTo(const std::string& file) const;

private:
    ui::MenuBar* bar_;
    std::vector<KeyCommand> keyCmds_;
    struct MenuOpt {
        ui::ShortcutScope scope = ui::ShortcutScope::Window;
        Keys more;
        bool evenDisabled = false;
    };
    std::map<std::string, MenuOpt> menuOpts_;
    std::map<std::string, std::string> aliases_;
    std::map<std::string, Keys> menuDefaults_;  // as first seen (the entries then show the keys bound)
    std::vector<std::string> menuOrder_;
    std::map<std::string, Keys> overrides_;
    std::vector<std::unique_ptr<ui::Shortcut>> shortcuts_;
    std::vector<std::shared_ptr<ui::Menu>> context_;
    const KeyCommand* keyCommand(const std::string& id) const;
    ui::Action* find(const std::string& id) const;
    void walkBar(bool fill, const std::function<void(ui::Action*, const std::string&)>& fn) const;
    std::vector<std::string> ids() const;  // the bindable ones (aliases left out), menus first
};

// a menu entry's text as the id has it: no &, nothing after a tab, trimmed
std::string plainText(const std::string& text);
// a key sequence the terminal (exbit --tui) gets: what text consoles send (tui/term.h), less the terminal
// front end's own keys (F1, F2, F6, F10, F12, Tab, Ctrl+P, Ctrl+Q)
bool terminalSends(const ui::KeySequence& k);
bool sameKey(const ui::KeySequence& a, const ui::KeySequence& b);
std::string keysText(const Keys& keys, const std::string& none = "");  // "Ctrl+Shift+Z, Ctrl+Y"
// how well a typed text matches (its letters in order; at word starts and in a row score more); -1: no match
int fuzzyScore(const std::string& query, const std::string& text);

}  // namespace commands
