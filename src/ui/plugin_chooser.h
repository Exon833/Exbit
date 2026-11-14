// Choosing an effect (or an instrument, in an instrument patch panel) to add: the FX window's Add…,
// a double-click on an empty spot of its space (plugin_chooser.py).
//
// A search box (it has the keys at once: type part of a name) over a list of every plugin of the
// kind, and a category chooser: All, Favourites, Recently used, Built-in, then categories (EQ,
// Dynamics, Reverb, Delay, Modulation, Distortion, Filter, Utility, Analyzer, Instrument, Other). A
// built-in effect's category is known; a plugin's is guessed from its name ("…Comp…" is Dynamics),
// else Other. ☆ / ★ next to a name (or F with it highlighted) makes it a favourite; favourites and
// the last RECENT_KEPT chosen are remembered in the settings ("options/favourite_plugins",
// "options/recent_plugins", as the successor's Preferences keys: "path|plugin name" a line). Enter or
// a double-click chooses the highlighted one. Up / Down / Page Up / Page Down in the search box move
// through the list.
//
// The two settings are read and written here directly, at the moment they are used (never kept in
// Preferences: a stale copy saved later would undo a change made here).
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "plugins/library.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace plugin_chooser {
constexpr int RECENT_KEPT = 8;
constexpr const char* ALL = "All";
constexpr const char* FAVOURITES = "Favourites";
constexpr const char* RECENT = "Recently used";
constexpr const char* BUILTIN = "Built-in";
constexpr const char* FAVOURITES_KEY = "options/favourite_plugins";
constexpr const char* RECENT_KEY = "options/recent_plugins";
// EQ, Dynamics, Reverb, Delay, Modulation, Distortion, Filter, Utility, Analyzer, Instrument, Other
const std::vector<std::string>& categories();
// "path|plugin name"
std::string keyOf(const plug::PluginInfo& info);
// its category: the built-in effect's own, else a guess from its name (Instrument for instruments)
std::string categoryOf(const plug::PluginInfo& info);
// the guess alone, from a name ("Other" when nothing fits)
std::string guessCategory(const std::string& name);
// the non-empty lines of a setting
std::vector<std::string> keysIn(const std::string& text);
// the settings (read fresh each time)
std::string favouritesText();
std::string recentText();
void setFavouritesText(const std::string& text);
void setRecentText(const std::string& text);
// the chooser as a dialog: the plugin chosen, or none
std::optional<plug::PluginInfo> choosePlugin(ui::Window* parent, const std::vector<plug::PluginInfo>& choices,
                                             const std::string& noun = "effect");
}  // namespace plugin_chooser

// the list, with its stars: a click on an item's ☆ / ★ (its first few pixels) or F toggles it
class PluginChooserList : public ui::ListWidget {
public:
    std::function<bool(int row)> starClicked;  // true: it was a star (the click is used up)
    std::function<void()> starKey;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    static constexpr int STAR_W = 22;  // px from the text's left edge
};

class PluginChooser : public ui::Dialog {
public:
    // pick one of `choices`; noun: "effect" / "instrument"
    PluginChooser(ui::Window* parent, std::vector<plug::PluginInfo> choices, const std::string& noun = "effect");
    std::vector<plug::PluginInfo> choices;
    std::string noun;
    std::set<std::string> favourites;
    ui::LineEdit* search;
    ui::ComboBox* category;
    PluginChooserList* list;
    // the choices that pass the search and the category, favourites first, then by name (recent: newest first)
    std::vector<const plug::PluginInfo*> shown() const;
    void fill();
    void toggleFavourite();
    // the one highlighted (remembered as recently used); none if nothing is
    std::optional<plug::PluginInfo> chosen();
    const plug::PluginInfo* current() const;  // the highlighted one, or null
private:
    std::vector<const plug::PluginInfo*> rows_;  // what each row of the list shows (null: the "Nothing here" line)
    std::unique_ptr<ui::Shortcut> findKey_;
};
