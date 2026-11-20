#include "ui/shortcuts_dialog.h"

#include <algorithm>

#include "core/util.h"
#include "platform/platform.h"
#include "ui/toolkit/layout.h"

namespace shortcuts_dialog {

namespace {
const char* const ARROW = " \xe2\x96\xb8 ";  // " ▸ "

std::string shownPath(const std::string& where) {
    std::string s;
    for (char c : where) s += c == '/' ? std::string(ARROW) : std::string(1, c);
    return s;
}

std::string keysLabel(const commands::Keys& ks) {
    std::string s;
    for (auto& k : ks) s += (s.empty() ? "" : ", ") + keyLabel(k);
    return s;
}

std::string homeDir() { return util::expandUser("~"); }
}  // namespace

std::string keyLabel(const ui::KeySequence& k) {
    return k.text() + (commands::terminalSends(k) ? "" : " (not in the terminal)");
}

// --- KeyCapture ---------------------------------------------------------------------------------

KeyCapture::KeyCapture(const std::string& text) : ui::Button(text), idle_(text) {
    clicked.connect([this] { start(); });
}

void KeyCapture::start() {
    capturing = true;
    setText("Press the key… (Esc: cancel)");
    setFocus();
    util::debugf("keyboard shortcuts: waiting for a key\n");
}

bool KeyCapture::keyPress(ui::KeyEvent& e) {
    if (!capturing) return ui::Button::keyPress(e);
    int k = e.key >= 'a' && e.key <= 'z' ? e.key - 'a' + 'A' : e.key;
    if (k == plat::Key_Shift || k == plat::Key_Control || k == plat::Key_Alt || k == plat::Key_Meta || k == 0)
        return true;  // (a chord being held: its key comes next)
    capturing = false;
    setText(idle_);
    int mods = e.mods & (plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModMeta);
    if (k == plat::Key_Escape && !mods) {
        if (cancelled) cancelled();
        return true;
    }
    if (k == plat::Key_Backtab) {
        k = plat::Key_Tab;
        mods |= plat::ModShift;
    }
    // a symbol typed with Shift is that symbol ("#", not Shift+3): KeySequence::matches lets Shift through for them
    bool symbol = k < 0x01000000 && k != ' ' && !(k >= 'A' && k <= 'Z') && !(k >= '0' && k <= '9');
    if (symbol) mods &= ~plat::ModShift;
    ui::KeySequence seq(k, mods);
    if (!commands::sameKey(ui::KeySequence::parse(seq.text()), seq)) {  // (a key without a name: CapsLock…)
        util::debugf("keyboard shortcuts: key %#x can't be bound\n", k);
        if (cancelled) cancelled();
        return true;
    }
    util::debugf("keyboard shortcuts: key pressed: %s\n", seq.text().c_str());
    if (captured) captured(seq);
    return true;
}

// --- ShortcutsDialog ----------------------------------------------------------------------------

ShortcutsDialog::ShortcutsDialog(ui::Window* parent, commands::Registry& r)
    : Dialog(parent, "Keyboard shortcuts", 760, 520), r_(r) {
    ui::VBox* box = ui::vbox(this, 11, 6);
    search = add<ui::LineEdit>();
    search->setPlaceholder("Find a command (its name, menu or key)");
    search->setClearButton(true);
    search->textChanged.connect([this](const std::string&) { refill(); });
    search->keyNavigation.connect([this](int key) {
        int row = list->currentRow() + (key == plat::Key_Up ? -1 : key == plat::Key_Down ? 1 : key == plat::Key_PageUp ? -10 : 10);
        if (list->count()) list->setCurrentRow(std::clamp(row, 0, list->count() - 1));
    });
    box->addWidget(search);
    auto row = std::make_unique<ui::HBox>();
    row->spacing = 6;
    list = add<ui::ListWidget>();
    list->setToolTip("Every command: its name, where it is (its menu) and its keys. Choose one, then Add a key…");
    list->currentRowChanged.connect([this](int) { showCurrent(); });
    row->addWidget(list, 1);
    auto side = std::make_unique<ui::VBox>();
    side->spacing = 6;
    keyList = add<ui::ListWidget>();
    keyList->setToolTip("The chosen command's keys (any of them runs it; the menus show the first)");
    keyList->setMinimumWidth(230);
    side->addWidget(keyList, 1);
    addKey = add<KeyCapture>("Add a key…");
    addKey->setToolTip("Click, then press the key or chord (Ctrl, Shift, Alt with a key) to run the chosen command");
    addKey->captured = [this](const ui::KeySequence& k) { bind(k); };
    addKey->cancelled = [this] { info->setText("No key added"); };
    side->addWidget(addKey);
    remove = add<ui::Button>("Remove");
    remove->setToolTip("The chosen key (or the command's only one) no longer runs it");
    remove->clicked.connect([this] {
        std::string id = currentId();
        commands::Keys ks = r_.keys(id);
        if (id.empty() || ks.empty()) return;
        int at = keyList->currentRow();
        ks.erase(ks.begin() + (at >= 0 && at < (int)ks.size() ? at : (int)ks.size() - 1));
        r_.setKeys(id, ks);
        refill();
        select(id);
    });
    side->addWidget(remove);
    resetThis = add<ui::Button>("Reset this");
    resetThis->setToolTip("The chosen command's own keys again");
    resetThis->clicked.connect([this] {
        std::string id = currentId();
        if (id.empty()) return;
        r_.reset(id);
        refill();
        select(id);
        info->setText("Back to its own keys");
    });
    side->addWidget(resetThis);
    row->addLayout(std::move(side));
    box->addLayout(std::move(row), 1);
    info = add<ui::Label>("");
    info->setWordWrap(true);
    box->addWidget(info);
    auto bottom = std::make_unique<ui::HBox>();
    bottom->spacing = 6;
    resetAll = add<ui::Button>("Reset all");
    resetAll->setToolTip("Every command's own keys again (your changes forgotten)");
    resetAll->clicked.connect([this] {
        std::string q = "Give every command its own keys again? The keys you changed are forgotten.";
        bool yes = askReplace ? askReplace(q)
                              : ui::msg::question(this, "Keyboard shortcuts", q, ui::msg::Yes | ui::msg::Cancel,
                                                  ui::msg::Yes) == ui::msg::Yes;
        if (!yes) return;
        std::string id = currentId();
        r_.resetAll();
        refill();
        select(id);
        info->setText("Every command has its own keys again");
    });
    bottom->addWidget(resetAll);
    importButton = add<ui::Button>("Import…");
    importButton->setToolTip("Keys from a keymap file (a text file, \"Edit/Undo = Ctrl+Z\" on each line)");
    importButton->clicked.connect([this] { importKeymap(); });
    bottom->addWidget(importButton);
    exportButton = add<ui::Button>("Export…");
    exportButton->setToolTip("Every command's keys in a keymap file, to keep or share");
    exportButton->clicked.connect([this] { exportKeymap(); });
    bottom->addWidget(exportButton);
    bottom->addStretch(1);
    auto* close = add<ui::Button>("Close");
    ui::dialogButton(close);
    close->clicked.connect([this] { accept(); });
    bottom->addWidget(close);
    box->addLayout(std::move(bottom));
    refill();
    if (list->count()) list->setCurrentRow(0);
    search->setFocus();
}

void ShortcutsDialog::refill() {
    std::string keep = currentId();
    all_.clear();
    for (auto& c : r_.list(false))
        if (c.aliasOf.empty()) all_.push_back(c);
    const std::string& q = search->text();
    std::vector<std::pair<int, int>> order;  // (score, index)
    for (int i = 0; i < (int)all_.size(); i++) {
        const auto& c = all_[(size_t)i];
        int s = commands::fuzzyScore(q, c.where + " " + c.name + " " + commands::keysText(c.keys));
        int byName = commands::fuzzyScore(q, c.name);
        if (byName >= 0) s = std::max(s, byName + 5000);
        if (s >= 0) order.push_back({q.empty() ? 0 : -s, i});
    }
    std::stable_sort(order.begin(), order.end());
    list->clear();
    for (auto& [s, i] : order) {
        (void)s;
        const auto& c = all_[(size_t)i];
        ui::ListWidget::Item it;
        it.text = c.name + "      " + shownPath(c.where) + "      " + keysLabel(c.keys);
        it.data = c.id;
        it.tooltip = c.tooltip;
        list->addItem(std::move(it));
    }
    if (!keep.empty()) select(keep);
    if (list->currentRow() < 0 && list->count()) list->setCurrentRow(0);
    showCurrent();
}

std::string ShortcutsDialog::currentId() const {
    int row = list ? list->currentRow() : -1;
    return row >= 0 && row < list->count() ? list->item(row).data : "";
}

void ShortcutsDialog::select(const std::string& id) {
    int row = list->findData(id);
    if (row >= 0) list->setCurrentRow(row);
}

void ShortcutsDialog::showCurrent() {
    std::string id = currentId();
    keyList->clear();
    for (auto& k : r_.keys(id)) keyList->addItem(keyLabel(k));
    if (keyList->count()) keyList->setCurrentRow(0);
    commands::Keys d = r_.defaults(id);
    remove->setEnabled(!id.empty() && keyList->count() > 0);
    resetThis->setEnabled(!id.empty());
    addKey->setEnabled(!id.empty());
    if (!id.empty()) info->setText(id + "   (its own keys: " + commands::keysText(d, "none") + ")");
}

bool ShortcutsDialog::bind(const ui::KeySequence& k) {
    std::string id = currentId();
    if (id.empty() || k.empty()) return false;
    commands::Keys ks = r_.keys(id);
    for (auto& x : ks)
        if (commands::sameKey(x, k)) {
            info->setText(k.text() + " already runs it");
            return true;
        }
    std::vector<std::string> users = r_.usersOf(k, id);
    if (!users.empty()) {
        std::string names;
        for (auto& u : users) {
            std::string shown = u;
            for (auto& c : all_)
                if (c.id == u) shown = c.name + " (" + shownPath(c.where) + ")";
            names += (names.empty() ? "" : ", ") + shown;
        }
        std::string q = k.text() + " already runs " + names + ". Take it from " + (users.size() > 1 ? "them" : "it") + "?";
        bool yes = askReplace ? askReplace(q)
                              : ui::msg::question(this, "Keyboard shortcuts", q, ui::msg::Yes | ui::msg::Cancel,
                                                  ui::msg::Cancel) == ui::msg::Yes;
        util::debugf("keyboard shortcuts: %s is %s's: %s\n", k.text().c_str(), names.c_str(), yes ? "taken" : "cancelled");
        if (!yes) {
            info->setText("Cancelled: " + k.text() + " still runs " + names);
            return false;
        }
        for (auto& u : users) {
            commands::Keys theirs = r_.keys(u);
            theirs.erase(std::remove_if(theirs.begin(), theirs.end(),
                                        [&](const ui::KeySequence& x) { return commands::sameKey(x, k); }),
                         theirs.end());
            r_.setKeys(u, theirs);
        }
    }
    ks.push_back(k);
    r_.setKeys(id, ks);
    refill();
    select(id);
    info->setText(k.text() + " now runs " + id +
                  (commands::terminalSends(k) ? "" : "  (not in the terminal: text consoles can't send it)"));
    return true;
}

void ShortcutsDialog::importKeymap() {
    std::string file = chooseImport ? chooseImport()
                                    : ui::files::openFile(this, "Import a keymap", homeDir(), "Keymaps (*.txt);;All files (*)");
    if (file.empty()) return;
    std::string data;
    if (!util::readFile(file, data)) {
        info->setText("Couldn't read " + file);
        return;
    }
    std::vector<std::string> problems = r_.load(data, util::baseName(file));
    r_.apply();
    r_.save();
    refill();
    std::string text = "Keys read from " + util::baseName(file);
    if (!problems.empty()) {
        text += "; " + std::to_string(problems.size()) + " line" + (problems.size() > 1 ? "s" : "") + " skipped: ";
        for (size_t i = 0; i < problems.size() && i < 3; i++) text += (i ? "; " : "") + problems[i];
        if (problems.size() > 3) text += "…";
    }
    info->setText(text);
}

void ShortcutsDialog::exportKeymap() {
    std::string file = chooseExport ? chooseExport()
                                    : ui::files::saveFile(this, "Export the keymap", util::joinPath(homeDir(), "keymap.txt"),
                                                          "Keymaps (*.txt)");
    if (file.empty()) return;
    info->setText(r_.exportTo(file) ? "Every command's keys saved in " + file : "Couldn't write " + file);
}

void showShortcuts(ui::Window* parent, commands::Registry& r) {
    ShortcutsDialog d(parent, r);
    d.exec();
}

// --- CommandPalette -----------------------------------------------------------------------------

CommandPalette::CommandPalette(ui::Window* parent, commands::Registry& r) : Dialog(parent, "Commands", 620, 420), r_(r) {
    commands = r.list(true);
    ui::VBox* box = ui::vbox(this, 11, 6);
    search = add<ui::LineEdit>();
    search->setPlaceholder("Type a command (a few of its letters)");
    search->textChanged.connect([this](const std::string&) { refill(); });
    search->keyNavigation.connect([this](int key) {
        int row = list->currentRow() + (key == plat::Key_Up ? -1 : key == plat::Key_Down ? 1 : key == plat::Key_PageUp ? -10 : 10);
        if (list->count()) list->setCurrentRow(std::clamp(row, 0, list->count() - 1));
    });
    search->returnPressed.connect([this] { choose(list->currentRow()); });
    box->addWidget(search);
    list = add<ui::ListWidget>();
    list->itemActivated.connect([this](int row) { choose(row); });
    list->currentRowChanged.connect([this](int row) {
        if (row < 0 || row >= list->count()) return;
        const auto& c = commands[(size_t)std::stoi(list->item(row).data)];
        info->setText(c.enabled ? "Enter: run it   Up/Down: another   Esc: close"
                                : "Not now" + (c.tooltip.empty() ? std::string() : ": " + c.tooltip));
    });
    box->addWidget(list, 1);
    info = add<ui::Label>("Enter: run it   Up/Down: another   Esc: close");
    info->setWordWrap(true);
    box->addWidget(info);
    refill();
    search->setFocus();
}

void CommandPalette::refill() {
    const std::string& q = search->text();
    struct Row {
        int recent, score, index;
    };
    std::vector<Row> rows;
    for (int i = 0; i < (int)commands.size(); i++) {
        const auto& c = commands[(size_t)i];
        if (!c.visible) continue;
        int s = commands::fuzzyScore(q, c.where + " " + c.name), byName = commands::fuzzyScore(q, c.name);
        if (byName >= 0) s = std::max(s, byName + 5000);  // (its name matching counts most)
        if (s < 0) continue;
        auto at = std::find(r_.recent.begin(), r_.recent.end(), c.id);
        int recent = at == r_.recent.end() || c.context ? 1 << 20 : (int)(at - r_.recent.begin());
        rows.push_back({recent, q.empty() ? 0 : -s, i});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.recent != b.recent ? a.recent < b.recent : a.score < b.score; });
    list->clear();
    for (auto& row : rows) {
        const auto& c = commands[(size_t)row.index];
        ui::ListWidget::Item it;
        std::string mark = c.checkable ? (c.checked ? "\xe2\x9c\x93 " : "   ") : "";  // ✓
        it.text = mark + c.name + "      " + shownPath(c.where) + (c.keys.empty() ? "" : "      " + commands::keysText(c.keys)) +
                  (c.enabled || c.tooltip.empty() ? "" : "      (" + c.tooltip + ")");
        it.data = std::to_string(row.index);
        it.enabled = c.enabled;
        it.tooltip = c.tooltip;
        list->addItem(std::move(it));
    }
    if (list->count()) list->setCurrentRow(0);
    util::debugf("command palette: \"%s\": %d of %zu commands\n", q.c_str(), list->count(), commands.size());
}

void CommandPalette::choose(int row) {
    if (row < 0 || row >= list->count()) return;
    int i = std::stoi(list->item(row).data);
    const auto& c = commands[(size_t)i];
    if (!c.enabled) {
        info->setText(c.name + " can't be used now" + (c.tooltip.empty() ? "" : ": " + c.tooltip));
        util::debugf("command palette: %s is disabled\n", c.id.c_str());
        return;
    }
    chosen = i;
    accept();
}

bool showPalette(ui::Window* parent, commands::Registry& r) {
    CommandPalette d(parent, r);
    if (d.exec() != ui::Dialog::Accepted || d.chosen < 0) return false;
    const commands::Command c = d.commands[(size_t)d.chosen];
    if (!c.context) r.noteRecent(c.id);
    util::debugf("command palette: %s\n", c.id.c_str());
    return r.run(c);
}

}  // namespace shortcuts_dialog
