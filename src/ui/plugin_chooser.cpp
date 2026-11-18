// Choosing an effect or an instrument to add (plugin_chooser.py).
#include "ui/plugin_chooser.h"

#include <algorithm>
#include <regex>

#include "core/json.h"
#include "core/util.h"
#include "effects/effects.h"
#include "ui/toolkit/layout.h"

using namespace ui;

namespace plugin_chooser {

namespace {
// (category, words in a plugin's name that tell it), first match wins
const std::vector<std::pair<std::string, std::regex>>& guesses() {
    static const std::vector<std::pair<std::string, std::regex>> g = [] {
        std::vector<std::pair<std::string, const char*>> raw = {
            {"EQ", R"(\beq|equal|parametric|graphic)"},
            {"Dynamics", R"(comp|limit|gate|expand|transient|dynamic|de-?ess|duck)"},
            {"Reverb", R"(reverb|verb|room|hall|plate|convol|space)"},
            {"Delay", R"(delay|echo|tape)"},
            {"Modulation", R"(chorus|flang|phas|tremolo|vibrato|rotary|ensemble|modul)"},
            {"Distortion", R"(distort|satur|drive|fuzz|crush|clip|tube|amp\b|amplif|warm)"},
            {"Filter", R"(filter|wah|formant)"},
            {"Utility", R"(utility|gain|width|stereo|mono|pan|trim|tool)"},
            {"Analyzer", R"(analy|meter|scope|spectr|tuner|loudness)"},
        };
        std::vector<std::pair<std::string, std::regex>> out;
        for (auto& [category, words] : raw) out.emplace_back(category, std::regex(words, std::regex::ECMAScript));
        return out;
    }();
    return g;
}

std::string setting(const char* key) { return settings::get(key).toString(); }
}  // namespace

const std::vector<std::string>& categories() {
    static const std::vector<std::string> c = {"EQ",      "Dynamics", "Reverb",   "Delay",      "Modulation", "Distortion",
                                               "Filter",  "Utility",  "Analyzer", "Instrument", "Other"};
    return c;
}

std::string keyOf(const plug::PluginInfo& info) { return info.path + "|" + info.pluginName; }

std::string guessCategory(const std::string& name) {
    std::string low = util::lower(name);
    for (auto& [category, words] : guesses())
        if (std::regex_search(low, words)) return category;
    return "Other";
}

std::string categoryOf(const plug::PluginInfo& info) {
    if (effects::isBuiltin(info.path)) return effects::categoryOf(info.path);
    if (info.kind == plug::INSTRUMENT) return "Instrument";
    return guessCategory(info.name);
}

std::vector<std::string> keysIn(const std::string& text) {
    std::vector<std::string> out;
    for (auto& k : util::split(text, '\n'))
        if (!k.empty()) out.push_back(k);
    return out;
}

std::string favouritesText() { return setting(FAVOURITES_KEY); }
std::string recentText() { return setting(RECENT_KEY); }
void setFavouritesText(const std::string& text) { settings::set(FAVOURITES_KEY, text); }
void setRecentText(const std::string& text) { settings::set(RECENT_KEY, text); }

std::optional<plug::PluginInfo> choosePlugin(Window* parent, const std::vector<plug::PluginInfo>& choices,
                                             const std::string& noun) {
    PluginChooser dialog(parent, choices, noun);
    if (dialog.exec() != Dialog::Accepted) return std::nullopt;
    return dialog.chosen();
}

}  // namespace plugin_chooser

using namespace plugin_chooser;

// ---------------------------------------------------------------------------------------------

bool PluginChooserList::mousePress(MouseEvent& e) {
    int row = rowAt(e.pos);
    if (e.button == plat::LeftButton && row >= 0 && e.pos.x <= 4 + STAR_W && starClicked && starClicked(row))
        return true;
    return ListWidget::mousePress(e);
}

bool PluginChooserList::mouseDoubleClick(MouseEvent& e) {
    int row = rowAt(e.pos);
    if (e.button == plat::LeftButton && row >= 0 && e.pos.x <= 4 + STAR_W && starClicked && starClicked(row))
        return true;  // (a quick second click on the star counts as another click on it)
    return ListWidget::mouseDoubleClick(e);
}

bool PluginChooserList::keyPress(KeyEvent& e) {
    if (e.key == 'F' && !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModMeta))) {
        if (starKey) starKey();
        return true;
    }
    return ListWidget::keyPress(e);
}

// ---------------------------------------------------------------------------------------------

PluginChooser::PluginChooser(Window* parent, std::vector<plug::PluginInfo> choices_, const std::string& noun_)
    : Dialog(parent, "Add " + noun_, 460, 520), choices(std::move(choices_)), noun(noun_) {
    for (auto& k : keysIn(favouritesText())) favourites.insert(k);
    setStyleType("PluginChooser");
    VBox* col = styleRoom(vbox(this, 10, 6), computedStyle());
    auto row = std::make_unique<HBox>();
    row->setSpacing(6);
    styleRoom(row.get(), computedStyle("row"));
    search = add<LineEdit>();
    search->setPlaceholder("Search " + noun + "s by name or kind (Ctrl+F)…");
    search->setClearButton(true);
    row->addWidget(search, 1);
    category = add<ComboBox>();
    std::vector<std::string> names = {ALL, FAVOURITES, RECENT, BUILTIN};
    for (auto& c : plugin_chooser::categories())
        if (std::any_of(choices.begin(), choices.end(), [&](const plug::PluginInfo& i) { return categoryOf(i) == c; }))
            names.push_back(c);
    for (auto& n : names) category->addItem(n, n);
    row->addWidget(category);
    col->addLayout(std::move(row));
    list = add<PluginChooserList>();
    list->setSizePolicy(SizePolicy::Expanding, SizePolicy::Expanding);
    col->addWidget(list, 1);
    Label* hint = add<Label>("☆ (or F) makes the highlighted one a favourite. Enter or a double-click adds it.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    col->addWidget(hint);
    auto* buttons = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    buttons->button(ButtonBox::Ok)->setText("Add");
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    col->addWidget(buttons);

    search->textChanged.connect([this](const std::string&) { fill(); });
    category->currentIndexChanged.connect([this](int) { fill(); });
    list->itemActivated.connect([this](int) { accept(); });
    list->starClicked = [this](int r) {
        if (r < 0 || r >= (int)rows_.size() || !rows_[r]) return false;
        list->setCurrentRow(r);
        toggleFavourite();
        return true;
    };
    list->starKey = [this] { toggleFavourite(); };
    search->returnPressed.connect([this] { accept(); });
    // Up, Down, Page Up and Page Down typed in the search box move the highlight in the list (the box
    // keeps the keys, so typing goes on)
    search->keyNavigation.connect([this](int key) {
        int n = list->count();
        if (!n || rows_.empty() || !rows_[0]) return;
        int step = key == plat::Key_Up ? -1 : key == plat::Key_Down ? 1 : key == plat::Key_PageUp ? -10 : 10;
        int r = list->currentRow();
        list->setCurrentRow(std::max(0, std::min(n - 1, (r >= 0 ? r : 0) + step)));
        list->scrollToRow(list->currentRow());
    });
    findKey_ = std::make_unique<Shortcut>(this, KeySequence::parse("Ctrl+F"), [this] { search->setFocus(); });
    // (of this kind: the instruments' and effects' are kept together)
    std::vector<std::string> recent = keysIn(recentText());
    bool anyRecent = std::any_of(choices.begin(), choices.end(), [&](const plug::PluginInfo& i) {
        return std::find(recent.begin(), recent.end(), keyOf(i)) != recent.end();
    });
    category->setCurrentIndexSilently(std::max(0, category->findText(anyRecent ? RECENT : ALL)));
    fill();
    search->setFocus();
}

std::vector<const plug::PluginInfo*> PluginChooser::shown() const {
    std::vector<std::string> words = util::splitWords(util::lower(search->text()));
    std::string cat = category->currentText();
    std::vector<std::string> recent = keysIn(recentText());
    bool special = cat == ALL || cat == FAVOURITES || cat == RECENT || cat == BUILTIN;
    std::vector<const plug::PluginInfo*> found;
    for (auto& info : choices) {
        std::string key = keyOf(info), kind = categoryOf(info);
        bool builtin = effects::isBuiltin(info.path);
        std::string text = util::lower(info.name + " " + kind + " " + (builtin ? "built-in" : ""));
        bool all = true;
        for (auto& w : words) all = all && text.find(w) != std::string::npos;
        if (!all) continue;
        if ((cat == FAVOURITES && !favourites.count(key)) ||
            (cat == RECENT && std::find(recent.begin(), recent.end(), key) == recent.end()) ||
            (cat == BUILTIN && !builtin) || (!special && kind != cat))
            continue;
        found.push_back(&info);
    }
    if (cat == RECENT) {
        auto at = [&](const plug::PluginInfo* i) { return std::find(recent.begin(), recent.end(), keyOf(*i)) - recent.begin(); };
        std::stable_sort(found.begin(), found.end(), [&](auto a, auto b) { return at(a) < at(b); });
        return found;
    }
    std::stable_sort(found.begin(), found.end(), [&](const plug::PluginInfo* a, const plug::PluginInfo* b) {
        bool fa = !favourites.count(keyOf(*a)), fb = !favourites.count(keyOf(*b));
        if (fa != fb) return fa < fb;
        return util::lower(a->label()) < util::lower(b->label());
    });
    return found;
}

const plug::PluginInfo* PluginChooser::current() const {
    int r = list->currentRow();
    return r >= 0 && r < (int)rows_.size() ? rows_[r] : nullptr;
}

void PluginChooser::fill() {
    const plug::PluginInfo* was = current();
    list->clear();
    rows_.clear();
    int select = -1;
    for (const plug::PluginInfo* info : shown()) {
        std::string star = favourites.count(keyOf(*info)) ? "★" : "☆";
        list->addItem(star + "  " + info->label() + "    · " + categoryOf(*info));
        if (info == was) select = (int)rows_.size();
        rows_.push_back(info);
    }
    if (select >= 0) list->setCurrentRow(select);
    else if (!rows_.empty()) list->setCurrentRow(0);
    if (rows_.empty()) {
        ListWidget::Item empty;
        empty.text = "Nothing here: " + std::string(category->currentText() == FAVOURITES
                                                        ? "no favourites yet (☆ makes one)"
                                                        : "try other words, or All");
        empty.enabled = false;
        empty.selectable = false;
        list->addItem(empty);
        rows_.push_back(nullptr);
    }
    if (list->currentRow() >= 0) list->scrollToRow(list->currentRow());
}

void PluginChooser::toggleFavourite() {
    const plug::PluginInfo* info = current();
    if (!info) return;
    std::string key = keyOf(*info);
    if (favourites.count(key)) favourites.erase(key);
    else favourites.insert(key);
    std::string text;
    for (auto& k : favourites) text += (text.empty() ? "" : "\n") + k;  // (sorted, as the set is)
    setFavouritesText(text);
    fill();
}

std::optional<plug::PluginInfo> PluginChooser::chosen() {
    const plug::PluginInfo* info = current();
    if (!info) return std::nullopt;
    std::string key = keyOf(*info);
    std::vector<std::string> recent = {key};
    for (auto& k : keysIn(recentText()))
        if (k != key) recent.push_back(k);
    if ((int)recent.size() > RECENT_KEPT) recent.resize(RECENT_KEPT);
    std::string text;
    for (auto& k : recent) text += (text.empty() ? "" : "\n") + k;
    setRecentText(text);
    return *info;
}
