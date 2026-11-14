// Options → VST2 plugin folders (plugin_folders_dialog.py): where the program looks for VST2
// plugins (.so files), as well as the standard VST3 folders. Subfolders are searched too.
// Remembered between runs (Preferences::vst2Folders). VST2 plugins then appear wherever plugins
// are chosen, named "… (VST2)".
//
// THE MAIN WINDOW: PluginFoldersDialog d(this, prefs.vst2List()); if (d.exec()) { prefs.setVst2List(
// d.folders()); prefs.save(); host.rescan() …; "N VST2 plugin(s) found" in the status bar }.
#pragma once
#include <string>
#include <vector>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class PluginFoldersDialog : public ui::Dialog {
public:
    PluginFoldersDialog(ui::Window* parent, const std::vector<std::string>& folders);
    using ui::Widget::add;  // (add<T>(…) for children, besides add(folder))
    std::vector<std::string> folders() const;
    void add(const std::string& folder);
    void addFolder();
    void removeFolder();
    void addUsual();
    std::vector<std::string> usualFolders() const;  // the usual VST2 folders on this computer not listed yet
    void updateButtons();
    static std::string normPath(const std::string& path);  // os.path.normpath

    ui::ListWidget* list;
    ui::Button* removeButton;
    ui::Button* usual;
};
