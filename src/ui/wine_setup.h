// Setting up Windows plugins from inside Exbit: a new Wine prefix (Options → Wine prefixes → New
// prefix…) and running a plugin's installer in one (Options → Install Windows plugin…). plugins/wine.h
// starts the Wine processes; these are the windows around them. Each waits with a progress window
// that keeps the program responsive (the processes run on their own; nothing blocks the main loop).
//
// THE MAIN WINDOW: installWindowsPlugin() { std::string used, made; auto before = vst2Dlls(prefixes);
// if (!wine_setup::installPlugin(this, prefixes, used, made)) return; if (!made.empty()) add it to the
// prefixes; rescan; report the new plugins; offer folders of new .dll files (newVst2Folders) }.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace ui {
class Window;
}

namespace wine_setup {

// where New prefix… makes prefixes: $XDG_DATA_HOME/wineprefixes (~/.local/share/wineprefixes, as winetricks)
std::string prefixesHome();
// is this a usable name for a new prefix there (no '/', not ".", "..", nor hidden)?
bool validName(const std::string& name);

// New prefix…: asks its name, makes it with the usual Wine (64-bit), a progress window meanwhile;
// the new prefix, or "" (cancelled, or it failed: said in a message)
std::string newPrefix(ui::Window* parent);

// Install Windows plugin…: asks for the installer (.exe or .msi) and, among `prefixes` (more than one),
// where; with none, offers to make one (`madePrefix`: that one, for the caller to add). Runs it there
// and waits for it (its own windows are where it is answered). True if it ran to its end.
bool installPlugin(ui::Window* parent, const std::vector<std::string>& prefixes, std::string& prefixUsed,
                   std::string& madePrefix);

// waits for `pid` (a child) with a busy progress window: true when it ended (`code`: its exit status),
// false if Cancel was pressed (it is then stopped)
bool waitFor(ui::Window* parent, int pid, const std::string& title, const std::string& label, int& code);

// the .dll files now in the usual VST2 folders of these prefixes (to see what an installer added)
std::set<std::string> vst2Dlls(const std::vector<std::string>& prefixes);
// the folders of the .dll files in `after` but not in `before` that aren't among `chosen` (VST2 plugin folders)
std::vector<std::string> newVst2Folders(const std::set<std::string>& before, const std::set<std::string>& after,
                                        const std::vector<std::string>& chosen);

}  // namespace wine_setup
