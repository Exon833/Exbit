// Options → Wine prefixes (see wine_prefixes_dialog.h).
#include "ui/wine_prefixes_dialog.h"

#include <algorithm>

#include "core/util.h"
#include "plugins/wine.h"
#include "ui/plugin_folders_dialog.h"
#include "ui/toolkit/layout.h"

WinePrefixesDialog::WinePrefixesDialog(ui::Window* parent, const std::vector<std::string>& prefixes_)
    : ui::Dialog(parent, "Wine prefixes", 580, 380) {
    auto* lay = ui::vbox(this, 10, 6);
    lay->addWidget(Widget::add<ui::Label>("Wine prefixes with Windows plugins in them:"));
    list = Widget::add<ui::ListWidget>();
    for (auto& p : prefixes_) list->addItem(p);
    list->currentRowChanged.connect([this](int) { updateButtons(); });
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    lay->addWidget(list, 1);
    auto row = std::make_unique<ui::HBox>();
    row->spacing = 6;
    auto* addB = Widget::add<ui::Button>("Add prefix…");
    addB->clicked.connect([this] { addPrefix(); });
    removeButton = Widget::add<ui::Button>("Remove");
    removeButton->clicked.connect([this] { removePrefix(); });
    defaultButton = Widget::add<ui::Button>("Add the default prefix");
    std::string d = wine::defaultPrefix();
    defaultButton->setToolTip(d.empty() ? "There is no default Wine prefix (~/.wine) on this computer" : d);
    defaultButton->clicked.connect([this] { addDefault(); });
    for (ui::Button* b : {addB, removeButton, defaultButton}) row->addWidget(b);
    row->addStretch(1);
    lay->addLayout(std::move(row));
    auto* hint = Widget::add<ui::Label>(
        "In each prefix, VST3 plugins are found in C:\\Program Files\\Common Files\\VST3. For VST2 plugins (.dll), "
        "add their folders in Options → VST2 plugin folders, which offers the usual ones of these prefixes. Each "
        "Windows plugin runs in a Wine process of its own (Tiny DAW's own host; yabridge isn't needed). Only 64-bit "
        "plugins can be used.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    status = Widget::add<ui::Label>("");
    status->setWordWrap(true);
    std::string why;
    if (!wine::available(why)) status->setText(why);
    else status->setText("Wine: " + wine::winePath());
    status->setProperty("hint", "true");
    lay->addWidget(status);
    auto* buttons = Widget::add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    lay->addWidget(buttons);
    updateButtons();
}

std::vector<std::string> WinePrefixesDialog::prefixes() const {
    std::vector<std::string> out;
    for (int i = 0; i < list->count(); ++i) out.push_back(list->item(i).text);
    return out;
}

bool WinePrefixesDialog::add(const std::string& prefix) {
    std::string p = PluginFoldersDialog::normPath(prefix);
    if (!wine::isPrefix(p)) return false;
    auto now = prefixes();
    if (std::find(now.begin(), now.end(), p) == now.end()) list->addItem(p);
    updateButtons();
    return true;
}

void WinePrefixesDialog::addPrefix() {
    std::string start = wine::defaultPrefix();
    std::string folder = ui::files::folder(this, "Wine prefix (the folder holding drive_c)", start.empty() ? util::expandUser("~") : start);
    if (folder.empty()) return;
    if (!add(folder))
        ui::msg::warning(this, "Wine prefixes",
                             folder + " isn't a Wine prefix: a prefix is a folder holding drive_c and system.reg "
                                      "(for example ~/.wine).");
}

void WinePrefixesDialog::removePrefix() {
    int row = list->currentRow();
    if (row >= 0) list->removeItem(row);
    updateButtons();
}

void WinePrefixesDialog::addDefault() {
    std::string d = wine::defaultPrefix();
    if (!d.empty()) add(d);
}

void WinePrefixesDialog::updateButtons() {
    removeButton->setEnabled(list->currentRow() >= 0 && list->currentRow() < list->count());
    std::string d = wine::defaultPrefix();
    auto now = prefixes();
    defaultButton->setEnabled(!d.empty() && std::find(now.begin(), now.end(), d) == now.end());
}
