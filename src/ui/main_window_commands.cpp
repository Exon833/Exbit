// The main window's commands and their keys (ui/commands.h): the menus walked, the keys that aren't menu
// entries registered by their widgets, the keymap read from the settings folder; Options → Keyboard shortcuts…
// and the command palette (ui/shortcuts_dialog.h).
#include "core/util.h"
#include "ui/toolkit/widget.h"
#include "ui/commands.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/shortcuts_dialog.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"

void MainWindow::setUpCommands() {
    commands = std::make_unique<commands::Registry>(menuBar);
    // undo and redo from any of the program's windows (MIDI editor, FX windows too), whether or not the Edit menu
    // was opened since the last change (it brings their enabled state up to date)
    commands->menuKeys("Edit/Undo", ui::ShortcutScope::ApplicationWide, {}, true);
    commands->menuKeys("Edit/Redo", ui::ShortcutScope::ApplicationWide, {ui::KeySequence::parse("Ctrl+Y")}, true);
    commands->menuKeys("View/Command palette…", ui::ShortcutScope::ApplicationWide);
    transport->registerCommands(*commands);
    timeline->registerCommands(*commands);
    panel->registerCommands(*commands);
    // menu entries showing the toolbar's keys (its buttons run them)
    commands->alias("File/Add audio or MIDI file…", "Transport/Add file…");
    commands->alias("Options/Computer keyboard", "Transport/Computer keyboard");
    commands->message = [this](const std::string& t) { showStatus(t, 4000); };
    // the palette's right-click menus: the selected clips', the current track's
    commands->contextMenus = [this](std::vector<std::pair<std::string, std::shared_ptr<ui::Menu>>>& out) {
        if (!timeline->selection.clips.empty() || timeline->selection.range) {
            auto m = std::make_shared<ui::Menu>();
            timeline->fillMenu(*m, std::nullopt);
            out.push_back({"Clip", m});
        }
        if (timeline->currentTrack) {
            auto m = std::make_shared<ui::Menu>();
            panel->fillTrackMenu(*m, timeline->currentTrack);
            out.push_back({"Track", m});
        }
    };
    commands->path = util::joinPath(util::dirName(settings::filePath()), "keymap.txt");
    std::vector<std::string> problems = commands->loadFile(commands->path);
    if (!problems.empty()) {  // (a damaged keymap: its good lines used, the others named, once the window is up)
        std::string text = problems[0] + (problems.size() > 1 ? " (and " + std::to_string(problems.size() - 1) + " more)" : "");
        std::weak_ptr<bool> alive = alive_;
        ui::App::get().singleShot(1500, [this, alive, text] {
            auto a = alive.lock();
            if (a && *a) showStatus(text, 10000);
        });
    }
    commands->apply();
}

void MainWindow::showShortcuts() { shortcuts_dialog::showShortcuts(this, *commands); }

void MainWindow::showPalette() { shortcuts_dialog::showPalette(this, *commands); }
