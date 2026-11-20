// Options → Keyboard shortcuts… (every command with its menu path and keys, searchable: choose one, Add a key…
// and press it; a key another command has asks whether to take it from that one) and the command palette
// (View → Command palette…, Ctrl+Shift+P; the terminal's Ctrl+P: type a few letters of a command, Enter runs it).
// Both over ui/commands.h's Registry.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/commands.h"
#include "ui/toolkit/dialogs.h"

namespace shortcuts_dialog {

// a button that, once clicked, takes the next key pressed (any: the window's shortcuts too) as a key to bind
class KeyCapture : public ui::Button {
public:
    explicit KeyCapture(const std::string& text);
    bool capturing = false;
    void start();                                       // (as a click)
    std::function<void(const ui::KeySequence&)> captured;
    std::function<void()> cancelled;                    // Esc, or a key that can't be bound
    bool overridesShortcut(const ui::KeyEvent& e) const override { return capturing; }
    bool keyPress(ui::KeyEvent& e) override;
private:
    std::string idle_;
};

class ShortcutsDialog : public ui::Dialog {
public:
    ShortcutsDialog(ui::Window* parent, commands::Registry& r);
    ui::LineEdit* search;
    ui::ListWidget* list;     // the commands (data: the id)
    ui::ListWidget* keyList;  // the chosen one's keys
    ui::Label* info;
    KeyCapture* addKey;
    ui::Button *remove, *resetThis, *resetAll, *importButton, *exportButton;
    void refill();                       // the commands matching the search
    std::string currentId() const;
    void select(const std::string& id);
    // a key for the chosen command: taken from the others using it if they agree (askReplace); false: cancelled
    bool bind(const ui::KeySequence& k);
    // (tests answer these instead of a question box and the file chooser)
    std::function<bool(const std::string& question)> askReplace;
    std::function<std::string()> chooseImport, chooseExport;
    void importKeymap();
    void exportKeymap();
private:
    commands::Registry& r_;
    std::vector<commands::Command> all_;
    void showCurrent();
};

class CommandPalette : public ui::Dialog {
public:
    CommandPalette(ui::Window* parent, commands::Registry& r);
    ui::LineEdit* search;
    ui::ListWidget* list;     // the commands shown (data: their index in commands)
    ui::Label* info;
    std::vector<commands::Command> commands;
    int chosen = -1;          // (accepted: the one to run)
    void refill();
    void choose(int row);     // runs it once the palette closes (disabled: says why)
private:
    commands::Registry& r_;
};

void showShortcuts(ui::Window* parent, commands::Registry& r);
// the palette, modal; the command chosen runs after it closes. false: none run
bool showPalette(ui::Window* parent, commands::Registry& r);
// the text of a key in the dialog: "Ctrl+Shift+P (not in the terminal)"
std::string keyLabel(const ui::KeySequence& k);

}  // namespace shortcuts_dialog
