// Skins: stylesheets that give the whole interface its look (core/css.h is the CSS they are written in).
//
// The program's own stylesheet, default.css, styles everything: every colour is a variable of :root
// (--lane, --playhead…, theme.h), and its rules style the controls and the program's surfaces from them.
// A skin is a stylesheet put on top of it: it changes what it names (some variables, or any rule) and keeps
// the rest. Its name is its --skin-name (else the file's name):
//
//     :root {
//         --skin-name: "Midnight";
//         --lane: #161a28;
//         --playhead: #ff6b81;
//     }
//
// docs/skins.md lists every variable, selector, part and property.
//
// The program's own skins are in plat::dataDir()/skins (Dark, Light, Midnight, OLED, High contrast, and
// default.css); yours are in the skins folder of your Tiny DAW folder (paths::dataDir("skins")). Skins are
// edited in a text editor of your choice: Options → Skin → Edit this skin… opens it (a copy of it in your
// folder, for one of the program's own), and the program shows what you save at once (MainWindow watches
// the file). JSON skins of earlier versions found in your folder are turned into stylesheets once
// (convertOldSkins); JSON isn't read as a skin otherwise.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/css.h"
#include "ui/theme.h"

namespace ui { class Window; }

namespace skin {

extern const char* const DEFAULT;  // "Dark"

std::string builtinDir();          // plat::dataDir() + "/skins"
std::string userDir();             // paths::dataDir("skins") (or what setUserDir set)
void setUserDir(const std::string& dir);  // tests: another folder ("" = back to the usual one)
void setBuiltinDir(const std::string& dir);  // tests: the program's own skins from another folder
std::string defaultSheet();        // builtinDir() + "/default.css": what every skin is put on top of

struct Skin {
    std::string name;
    std::string path;                            // its stylesheet ("" none: default.css alone)
    bool builtin = false;                        // one of the program's own (edited as a copy of yours)
};

// A skin's stylesheet: true and `out` (and `problems`: what in it was left out, as "file:line:col: …");
// false and `error` if the file can't be read.
bool read(const std::string& path, Skin& out, std::vector<std::string>& problems, std::string& error,
          bool builtin = false);
std::string safeName(const std::string& name);  // "a/b:c?" -> "a_b_c_" (a name made fit for a file's name)
std::string fileName(const std::string& name);  // "My skin" -> "My skin.css"
// a CSS string of the text: "…" with \" and \\ escaped
std::string quoted(const std::string& text);

// Every skin there is: the program's own (Dark first), then yours, by name (default.css itself isn't one).
// Your old JSON skins are converted first (convertOldSkins).
std::vector<Skin> library(const std::string& userDir = "");
// The skin of that name (yours first, if you have one of the same name), or the default.
Skin find(const std::string& name, const std::string& userDir = "");

// Put `skin` on the whole program (default.css, then its stylesheet): theme colours, shades and fonts, and the
// controls' styles; cached drawings that used the old look are dropped (colors::clearCache, and whatever was
// registered with onApply). Returns what in the stylesheets was left out ("file:line:col: …"). The caller then
// rebuilds / repaints what was built with them (MainWindow::restyle).
std::vector<std::string> apply(const Skin& skin);
// called after every apply() (e.g. the waveform tiles' cache: clear it); returns an id
int onApply(std::function<void()> fn);
void removeOnApply(int id);

// --- skins of earlier versions (JSON) ---
// A JSON skin ({"format": "tinydaw-skin", "name", "colors": {name: "#rrggbb[aa]"}, "fonts": {ui, ui_size,
// mono}}) as a stylesheet; false and `error` if it isn't one. What can't be kept is noted in comments.
bool cssFromJson(const std::string& jsonText, const std::string& fallbackName, std::string& css, std::string& error);
// Each JSON skin in `dir` becomes a stylesheet beside it (unless one of that file name is there already) and
// is moved into its "old JSON skins" folder; returns the paths of the stylesheets written.
std::vector<std::string> convertOldSkins(const std::string& dir);

// --- Options → Skin actions ---
// your own copy of `skin` called `name` in your skins folder (its stylesheet, with that --skin-name): the path,
// or "" and `error`
std::string makeCopy(const Skin& skin, const std::string& name, std::string& error);
// opens the stylesheet in the system's editor for it (false if that couldn't be started)
bool openInEditor(const std::string& path);
// Asks for a stylesheet, copies it into your skins folder (asking before replacing one of the same name),
// then choose(skin) and tells what was left out of it. False if nothing was imported.
bool importSkin(ui::Window* parent, const std::function<void(const Skin&)>& choose);
// Asks where to write `skin`'s stylesheet (a .css file); returns the path written, "" if none. (The caller
// shows "Skin saved as <path>" in the status bar.)
std::string exportSkin(ui::Window* parent, const Skin& skin);
void openSkinsFolder();  // (made if missing)

}  // namespace skin
