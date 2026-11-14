// Recent projects (File → Open recent) and project templates (File → New from template, Save as
// template…) (templates.py).
//
// Recent: the last RECENT_KEPT projects opened or saved, newest first (Preferences::recentProjects, a
// JSON list), only those still there when the menu opens. Backups, recovery files and templates
// aren't listed.
//
// Templates are projects to start from: the program's own in `templates/` of its data folder (no
// audio: tracks, their effects and sends, the tempo), and yours in the templates folder of your Tiny
// DAW folder (paths::dataDir("templates")), saved with File → Save as template… (with the project's
// audio, in "<name>_audio"). Starting from one opens it as a new, untitled project: Save asks where to
// keep it. Options → New projects start from… makes File → New (and the program's start) use one.
#pragma once
#include <string>
#include <vector>

#include "ui/preferences.h"

class MainWindow;
namespace ui {
class Menu;
}

namespace templates {

constexpr int RECENT_KEPT = 10;

std::string programDir();  // the program's own templates (data/templates)
std::string userDir();     // yours (made when one is saved)
// tests: other folders ("" = back to the usual ones)
void setDirs(const std::string& program, const std::string& user);

// --- recent projects ---
// is `path` a file that isn't one of the user's projects (a backup, a recovery file, a template)?
bool keptOut(const std::string& path);
// `path` was opened or saved: first in the recent list (saved in the preferences)
void remember(Preferences& prefs, const std::string& path);
// the recent projects, newest first (those still there, unless allOfThem). (Older versions joined them with "|".)
std::vector<std::string> recent(const Preferences& prefs, bool allOfThem = false);
void clearRecent(Preferences& prefs);
// File → Open recent ▸ (filled each time it opens)
void fillRecentMenu(ui::Menu* menu, MainWindow& window);

// --- templates ---
struct Template {
    std::string name, path;
    bool own;  // the program's own (else yours)
};
// the program's templates first, then yours, by name
std::vector<Template> list();
// File → New from template ▸
void fillTemplateMenu(ui::Menu* menu, MainWindow& window);
// Options → New projects start from ▸: empty, or a template (remembered)
void fillDefaultMenu(ui::Menu* menu, MainWindow& window);
void setDefault(MainWindow& window, const std::string& path);
// the template new projects start from, if one is chosen and still there; else ""
std::string defaultTemplate(const Preferences& prefs);
void openFolder();
// File → Save as template…: the project as it is now (with its audio), under a name. The project itself
// is unchanged (its file, whether it is saved). Returns the template's path ("" if not saved).
std::string saveAsTemplate(MainWindow& window);
// the same without asking (the name given; replaces one of that name): "" and `error` if it failed
std::string writeTemplate(MainWindow& window, const std::string& name, std::string& error);

}  // namespace templates
