// Options → Wine prefixes (see wine_prefixes_dialog.h).
#include "ui/wine_prefixes_dialog.h"

#include <algorithm>

#include "core/util.h"
#include "plugins/wine.h"
#include "ui/plugin_folders_dialog.h"
#include "ui/toolkit/layout.h"
#include "ui/wine_setup.h"

WinePrefixesDialog::WinePrefixesDialog(ui::Window* parent, const std::vector<std::string>& prefixes_,
                                       const std::map<std::string, std::string>& programs_)
    : ui::Dialog(parent, "Wine prefixes", 620, 440) {
    auto* lay = ui::vbox(this, 10, 6);
    lay->addWidget(Widget::add<ui::Label>("Wine prefixes with Windows plugins in them:"));
    list = Widget::add<ui::ListWidget>();
    for (auto& p : prefixes_) list->addItem(p);
    for (auto& [prefix, program] : programs_)
        if (!program.empty()) chosen[PluginFoldersDialog::normPath(prefix)] = program;
    list->currentRowChanged.connect([this](int) { updateButtons(); });
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    lay->addWidget(list, 1);
    auto row = std::make_unique<ui::HBox>();
    row->spacing = 6;
    auto* addB = Widget::add<ui::Button>("Add prefix…");
    addB->clicked.connect([this] { addPrefix(); });
    auto* newB = Widget::add<ui::Button>("New prefix…");
    newB->setToolTip("Make a new 64-bit Wine prefix for Windows plugins (in " + wine_setup::prefixesHome() + ")");
    newB->clicked.connect([this] { newPrefix(); });
    removeButton = Widget::add<ui::Button>("Remove");
    removeButton->clicked.connect([this] { removePrefix(); });
    defaultButton = Widget::add<ui::Button>("Add the default prefix");
    std::string d = wine::defaultPrefix();
    defaultButton->setToolTip(d.empty() ? "There is no default Wine prefix (~/.wine) on this computer" : d);
    defaultButton->clicked.connect([this] { addDefault(); });
    for (ui::Button* b : {addB, newB, removeButton, defaultButton}) row->addWidget(b);
    row->addStretch(1);
    lay->addLayout(std::move(row));
    // the selected prefix: its facts, and which Wine runs its plugins
    about = Widget::add<ui::Label>("");
    about->setWordWrap(true);
    lay->addWidget(about);
    auto wineRow = std::make_unique<ui::HBox>();
    wineRow->spacing = 6;
    wineButton = Widget::add<ui::Button>("Wine program…");
    wineButton->setToolTip("Run this prefix's plugins with another Wine (e.g. the one it was made with)");
    wineButton->clicked.connect([this] { chooseWine(); });
    automaticButton = Widget::add<ui::Button>("Automatic");
    automaticButton->setToolTip("The prefix's own Wine if it belongs to Bottles or Lutris, else the usual one");
    automaticButton->clicked.connect([this] { setWine(""); });
    wineRow->addWidget(wineButton);
    wineRow->addWidget(automaticButton);
    wineRow->addStretch(1);
    lay->addLayout(std::move(wineRow));
    auto* hint = Widget::add<ui::Label>(
        "In each prefix, VST3 plugins are found in C:\\Program Files\\Common Files\\VST3. For VST2 plugins (.dll), "
        "add their folders in Options → VST2 plugin folders, which offers the usual ones of these prefixes. Each "
        "Windows plugin runs in a Wine process of its own (Tiny DAW's own host; yabridge isn't needed), with the "
        "prefix's Wine: a Bottles bottle's runner, a Lutris game's Wine version, or the one chosen here. Only 64-bit "
        "plugins can be used.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    status = Widget::add<ui::Label>("");
    status->setWordWrap(true);
    std::string why;
    if (!wine::available(why)) status->setText(why);
    else {
        std::string version = wine::wineVersion(wine::winePath());
        status->setText("The usual Wine: " + wine::winePath() + (version.empty() ? "" : " (" + version + ")"));
    }
    status->setProperty("hint", "true");
    lay->addWidget(status);
    auto* buttons = Widget::add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    lay->addWidget(buttons);
    if (list->count()) list->setCurrentRow(0);
    updateButtons();
}

std::vector<std::string> WinePrefixesDialog::prefixes() const {
    std::vector<std::string> out;
    for (int i = 0; i < list->count(); ++i) out.push_back(list->item(i).text);
    return out;
}

std::map<std::string, std::string> WinePrefixesDialog::programs() const {
    std::map<std::string, std::string> out;
    for (auto& p : prefixes()) {
        auto it = chosen.find(p);
        if (it != chosen.end() && !it->second.empty()) out[p] = it->second;
    }
    return out;
}

bool WinePrefixesDialog::add(const std::string& prefix) {
    std::string p = PluginFoldersDialog::normPath(prefix);
    if (!wine::isPrefix(p)) return false;
    auto now = prefixes();
    if (std::find(now.begin(), now.end(), p) == now.end()) {
        list->addItem(p);
        list->setCurrentRow(list->count() - 1);
    }
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

void WinePrefixesDialog::newPrefix() {
    std::string made = wine_setup::newPrefix(window());
    if (!made.empty()) add(made);
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

void WinePrefixesDialog::setWine(const std::string& program) {
    int row = list->currentRow();
    if (row < 0 || row >= list->count()) return;
    std::string p = list->item(row).text;
    if (program.empty()) chosen.erase(p);
    else chosen[p] = program;
    updateButtons();
}

void WinePrefixesDialog::chooseWine() {
    int row = list->currentRow();
    if (row < 0 || row >= list->count()) return;
    std::string p = list->item(row).text;
    auto it = chosen.find(p);
    std::string start = util::dirName(it != chosen.end() ? it->second : wine::winePath());
    std::string file = ui::files::openFile(this, "Wine program for " + p, start.empty() ? "/usr/bin" : start,
                                           "All files (*)");
    if (file.empty()) return;
    if (wine::wineVersion(file).empty()) {
        ui::msg::warning(this, "Wine prefixes",
                         file + " isn't a wine program that runs here (it didn't answer `wine --version`). Choose "
                                "the `wine` file in the bin folder of a Wine installation.");
        return;
    }
    setWine(file);
}

std::string WinePrefixesDialog::details(const std::string& prefix) const {
    std::string arch = wine::prefixArch(prefix);
    std::string out = arch == "win32" ? "A 32-bit prefix: its plugins can't be used (Tiny DAW's Wine host is 64-bit)."
                                      : arch.empty() ? "A prefix not finished yet (its registry isn't written)."
                                                     : "A 64-bit prefix.";
    std::string source, program;
    auto it = chosen.find(prefix);
    if (it != chosen.end()) {
        program = it->second;
        source = "chosen";
    } else {
        program = wine::automaticWine(prefix, source);
    }
    if (program.empty()) return out + " No Wine for it" + (source.empty() ? "" : " (" + source + ")") + ".";
    std::string version = wine::wineVersion(program);
    out += " Wine: " + program;
    out += version.empty() ? " (doesn't answer `--version`)" : " (" + version + ")";
    if (!source.empty()) out += ", " + source;
    out += ".";
    int major = wine::wineMajor(version);
    if (major > 0 && major < wine::TESTED_WINE_MAJOR)
        out += " Tiny DAW's Wine host is tested with Wine " + std::to_string(wine::TESTED_WINE_MAJOR) +
               "; older ones may not run every plugin.";
    return out;
}

void WinePrefixesDialog::updateButtons() {
    int row = list->currentRow();
    bool selected = row >= 0 && row < list->count();
    removeButton->setEnabled(selected);
    wineButton->setEnabled(selected);
    automaticButton->setEnabled(selected && chosen.count(list->item(row).text));
    about->setText(selected ? details(list->item(row).text) : "");
    std::string d = wine::defaultPrefix();
    auto now = prefixes();
    defaultButton->setEnabled(!d.empty() && std::find(now.begin(), now.end(), d) == now.end());
}
