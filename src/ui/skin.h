// Skins: the interface's colours and fonts, kept in JSON files (skin.py).
//
//     {
//       "format": "tinydaw-skin",
//       "version": 1,
//       "name": "Midnight",
//       "colors": {"lane": "#161a28", "playhead": "#ff6b81", "selection_fill": "#7c8cff2d", …},
//       "fonts": {"ui": "Segoe UI", "ui_size": 9, "mono": "Consolas"}
//     }
//
// The colour names are theme::colors() ("#rrggbb", or "#rrggbbaa" with transparency); a skin may
// name only some of them, the rest keep the default ("Dark"). Fonts: "" or 0 for the system's own.
//
// The program's own skins are in plat::dataDir()/skins (Dark, Light, Midnight, OLED, High contrast); your own in
// the skins folder of your Tiny DAW folder (paths::dataDir("skins")), where Options → Skin → Customize… saves them and Import… copies them.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "gfx/geom.h"
#include "ui/theme.h"

namespace ui { class Window; }

namespace skin {

extern const char* const FORMAT;   // "tinydaw-skin"
constexpr int VERSION = 1;
extern const char* const DEFAULT;  // "Dark"

std::string builtinDir();          // plat::dataDir() + "/skins"
std::string userDir();             // paths::dataDir("skins") (or what setUserDir set)
void setUserDir(const std::string& dir);  // tests: another folder ("" = back to the usual one)
void setBuiltinDir(const std::string& dir);  // tests: the program's own skins from another folder

struct Skin {
    std::string name;
    std::map<std::string, std::string> colors;  // name -> "#rrggbb"[aa]: only those the skin sets
    theme::Fonts fonts;                          // (its defaults are theme's FONTS: "", 0, "Consolas")
    std::string path;                            // the file it came from
    bool builtin = false;                        // one of the program's own (not changed in place; "Save as" instead)

    // every colour, the skin's own or the default, in theme::colors() order
    std::vector<std::pair<std::string, std::string>> completeColors() const;
    // the colour by name, the skin's own or the default
    std::string color(const std::string& name) const;
};

// "#rrggbb" or "#rrggbbaa" (theme.parse_color); false if it isn't one
bool parseColor(const std::string& text, Color* out = nullptr);

Json toJson(const Skin& skin);  // every colour and font (complete), as written to files

// A skin file: true and `out` (and `problems`: what was ignored — unknown names, values that
// aren't colours — as text); false and `error` if it can't be read or isn't a skin.
bool read(const std::string& path, Skin& out, std::vector<std::string>& problems, std::string& error,
          bool builtin = false);
bool write(const Skin& skin, const std::string& path, std::string* error = nullptr);  // (makes the folder)
std::string fileName(const std::string& name);  // "My skin" -> "My skin.json" (odd characters: "_")

// Every skin there is: the program's own (Dark first), then yours, by name. Files that can't be
// read are left out.
std::vector<Skin> library(const std::string& userDir = "");
// The skin of that name (yours first, if you have one of the same name), or the default.
Skin find(const std::string& name, const std::string& userDir = "");

// Put `skin` on the whole program: theme colours and fonts; cached drawings that used the old
// colours are dropped (colors::clearCache, and whatever was registered with onApply). The caller
// then rebuilds / repaints what was built with them (MainWindow::restyle).
void apply(const Skin& skin);
// called after every apply() (e.g. the waveform tiles' cache: clear it); returns an id
int onApply(std::function<void()> fn);
void removeOnApply(int id);

// --- Options → Skin actions (main_window.py import_skin / export_skin / open_skins_folder) ---
// Asks for a skin file, copies it into your skins folder (asking before replacing one of the same
// name), then choose(skin) and tells what was left out of it. False if nothing was imported.
bool importSkin(ui::Window* parent, const std::function<void(const Skin&)>& choose);
// Asks where to write `skin` (a .json file); returns the path written, "" if none. (The caller
// shows "Skin saved as <path>" in the status bar.)
std::string exportSkin(ui::Window* parent, const Skin& skin);
void openSkinsFolder();  // (made if missing)

}  // namespace skin
