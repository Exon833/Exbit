// Options → Wine prefixes: the Wine prefixes Windows plugins are found in (plugins/wine.h). In each,
// Windows VST3 plugins are found in C:\Program Files\Common Files\VST3; Windows VST2 plugins in
// the folders chosen in Options → VST2 plugin folders (which offers their usual folders in these
// prefixes). Each Windows plugin then runs in Exbit's own Wine host, in its prefix.
// Remembered between runs (Preferences::winePrefixes; until chosen, the default prefix ~/.wine).
//
// THE MAIN WINDOW: WinePrefixesDialog d(this, prefs.wineList()); if (d.exec()) { prefs.setWineList(
// d.prefixes()); prefs.save(); wine::setPrefixes(…); host.rescan(); "N Windows plugin(s) found" }.
#pragma once
#include <string>
#include <vector>

#include "ui/toolkit/controls.h"
#include <map>

#include "ui/toolkit/dialogs.h"

class WinePrefixesDialog : public ui::Dialog {
public:
    // `programs`: the wine program chosen for some prefixes (Preferences::wineProgramMap)
    WinePrefixesDialog(ui::Window* parent, const std::vector<std::string>& prefixes,
                       const std::map<std::string, std::string>& programs = {});
    using ui::Widget::add;  // (add<T>(…) for children, besides add(prefix))
    std::vector<std::string> prefixes() const;
    std::map<std::string, std::string> programs() const;  // (of the prefixes still listed)
    // adds a prefix; false (and nothing added) if the folder isn't one
    bool add(const std::string& prefix);
    void addPrefix();
    void newPrefix();  // makes one (wine_setup::newPrefix) and adds it
    void removePrefix();
    void addDefault();
    // the selected prefix's wine program: chosen (a file), or automatic again ("")
    void setWine(const std::string& program);
    void chooseWine();
    void updateButtons();
    // the selected prefix's facts, shown under the list: 64- or 32-bit, its Wine and its version
    std::string details(const std::string& prefix) const;

    ui::ListWidget* list;
    ui::Button* removeButton;
    ui::Button* defaultButton;
    ui::Button* wineButton;
    ui::Button* automaticButton;
    ui::Label* about;  // details() of the selected prefix
    ui::Label* status;
    std::map<std::string, std::string> chosen;
};
