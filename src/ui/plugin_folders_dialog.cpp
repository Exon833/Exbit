// Options → VST2 plugin folders (plugin_folders_dialog.py).
#include "ui/plugin_folders_dialog.h"

#include <algorithm>

#include "core/util.h"
#include "plugins/library.h"
#include "plugins/wine.h"
#include "ui/toolkit/layout.h"

PluginFoldersDialog::PluginFoldersDialog(ui::Window* parent, const std::vector<std::string>& folders_)
    : ui::Dialog(parent, "VST2 plugin folders", 560, 360) {
    auto* lay = ui::vbox(this, 10, 6);
    lay->addWidget(Widget::add<ui::Label>("Folders with VST2 plugins (their subfolders are searched too):"));
    list = Widget::add<ui::ListWidget>();
    for (auto& f : folders_) list->addItem(f);
    list->currentRowChanged.connect([this](int) { updateButtons(); });
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    lay->addWidget(list, 1);
    auto row = std::make_unique<ui::HBox>();
    row->spacing = 6;
    auto* addB = Widget::add<ui::Button>("Add folder…");
    addB->clicked.connect([this] { addFolder(); });
    removeButton = Widget::add<ui::Button>("Remove");
    removeButton->clicked.connect([this] { removeFolder(); });
    usual = Widget::add<ui::Button>("Add the usual folders");
    std::string tip = "Adds those of these that are on this computer:";
    for (auto& f : usualCandidates()) tip += "\n" + f;
    usual->setToolTip(tip);
    usual->clicked.connect([this] { addUsual(); });
    for (ui::Button* b : {addB, removeButton, usual}) row->addWidget(b);
    row->addStretch(1);
    lay->addLayout(std::move(row));
    auto* hint = Widget::add<ui::Label>("VST3 plugins are always found in ~/.vst3, /usr/lib/vst3 and /usr/local/lib/vst3, and "
                                "in the VST3 folder of each Wine prefix (Options → Wine prefixes). Folders may hold "
                                "Linux plugins (.so) and Windows ones (.dll, run through Wine: 64- and 32-bit). Linux plugins "
                                "must be 64-bit; plugins that hold several (shells, such as Waves) aren't "
                                "supported yet.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto* buttons = Widget::add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    lay->addWidget(buttons);
    updateButtons();
}

std::string PluginFoldersDialog::normPath(const std::string& path) {
    if (path.empty()) return ".";
    bool absolute = path[0] == '/';
    std::vector<std::string> parts;
    for (auto& part : util::split(path, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else if (part == ".." && absolute) continue;
        else parts.push_back(part);
    }
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
    return out.empty() ? "." : out;
}

std::vector<std::string> PluginFoldersDialog::folders() const {
    std::vector<std::string> out;
    for (int i = 0; i < list->count(); ++i) out.push_back(list->item(i).text);
    return out;
}

void PluginFoldersDialog::add(const std::string& folder) {
    std::string f = normPath(folder);
    auto now = folders();
    if (std::find(now.begin(), now.end(), f) == now.end()) list->addItem(f);
    updateButtons();
}

std::vector<std::string> PluginFoldersDialog::usualCandidates() {
    auto f = plug::vst2UsualFolders();
    for (auto& w : wine::vst2UsualFolders(wine::prefixes())) f.push_back(w);  // (Windows plugins, through Wine)
    return f;
}

void PluginFoldersDialog::addFolder() {
    std::string start;
    for (auto& f : usualCandidates())
        if (util::isDir(f)) {
            start = f;
            break;
        }
    std::string folder = ui::files::folder(this, "Folder with VST2 plugins", start);
    if (!folder.empty()) add(folder);
}

void PluginFoldersDialog::removeFolder() {
    int row = list->currentRow();
    if (row >= 0) list->removeItem(row);
    updateButtons();
}

void PluginFoldersDialog::addUsual() {
    for (auto& f : usualFolders()) add(f);
}

std::vector<std::string> PluginFoldersDialog::usualFolders() const {
    auto now = folders();
    std::vector<std::string> out;
    for (auto& f : usualCandidates())
        if (util::isDir(f) && std::find(now.begin(), now.end(), normPath(f)) == now.end()) out.push_back(f);
    return out;
}

void PluginFoldersDialog::updateButtons() {
    removeButton->setEnabled(list->currentRow() >= 0 && list->currentRow() < list->count());
    usual->setEnabled(!usualFolders().empty());
}
