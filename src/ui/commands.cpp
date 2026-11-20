#include "ui/commands.h"

#include <algorithm>
#include <cctype>

#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"

namespace commands {

namespace {
constexpr size_t RECENT_MAX = 20;
constexpr int MASK = plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModMeta;

bool contains(const Keys& ks, const ui::KeySequence& k) {
    for (auto& x : ks)
        if (sameKey(x, k)) return true;
    return false;
}

Keys deduped(const Keys& in) {
    Keys out;
    for (auto& k : in)
        if (!k.empty() && !contains(out, k)) out.push_back(k);
    return out;
}

std::string lastPart(const std::string& id) {
    size_t s = id.rfind('/');
    return s == std::string::npos ? id : id.substr(s + 1);
}

std::string firstParts(const std::string& id) {
    size_t s = id.rfind('/');
    return s == std::string::npos ? "" : id.substr(0, s);
}

// every entry of a menu and its submenus, with its id (fill: aboutToShow first, as when it opens)
void walk(ui::Menu* m, const std::string& prefix, bool fill, const std::function<void(ui::Action*, const std::string&)>& fn) {
    if (fill && m->aboutToShow) {
        auto f = m->aboutToShow;  // (it may set itself anew)
        f(m);
    }
    std::set<std::string> seen;
    for (auto& up : m->actions()) {
        ui::Action* a = up.get();
        if (a->separator || a->section) continue;
        std::string name = plainText(a->text);
        if (name.empty()) continue;
        std::string base = prefix.empty() ? name : prefix + "/" + name, id = base;
        for (int n = 2; !seen.insert(id).second; n++) id = base + " (" + std::to_string(n) + ")";
        if (a->submenu) walk(a->submenu, id, fill, fn);
        else fn(a, id);
    }
}
}  // namespace

std::string plainText(const std::string& text) {
    std::string s = text.substr(0, text.find('\t')), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') out += s[++i];
            continue;
        }
        out += s[i];
    }
    return util::trim(out);
}

bool sameKey(const ui::KeySequence& a, const ui::KeySequence& b) { return a.key == b.key && a.mods == b.mods; }

std::string keysText(const Keys& keys, const std::string& none) {
    std::string s;
    for (auto& k : keys) s += (s.empty() ? "" : ", ") + k.text();
    return s.empty() ? none : s;
}

bool terminalSends(const ui::KeySequence& s) {
    // what tui::KeyDecoder makes of the bytes text consoles send (xterm's modifier parameters for the editing and
    // function keys; Ctrl+letter as a control character, which has no Shift; Alt as ESC first)
    int k = s.key, m = s.mods;
    bool ctrl = m & plat::ModCtrl, shift = m & plat::ModShift;
    if (!k || (m & plat::ModMeta)) return false;
    if (!m && (k == plat::Key_F1 || k == plat::Key_F2 || k == plat::Key_F6 || k == plat::Key_F10 || k == plat::Key_F12))
        return false;  // (the terminal front end's own)
    if (m == plat::ModCtrl && (k == 'P' || k == 'Q')) return false;
    if (k == plat::Key_Tab || k == plat::Key_Backtab) return false;
    if (k >= 'A' && k <= 'Z') {
        if (ctrl && shift) return false;
        if (ctrl && (k == 'H' || k == 'I' || k == 'J' || k == 'M')) return false;  // (Backspace, Tab, Return)
        return true;
    }
    if (k == plat::Key_Space) return !shift;
    if (k == plat::Key_Return || k == plat::Key_Enter || k == plat::Key_Backspace || k == plat::Key_Escape)
        return !ctrl && !shift;
    if (k >= plat::Key_Insert && k <= plat::Key_PageDown) return k != plat::Key_Pause && k != plat::Key_Print;
    if (k >= plat::Key_F1 && k <= plat::Key_F12) return true;
    if (k < 0x01000000) {  // a character
        if (shift) return false;  // (a shifted symbol is another character)
        if (ctrl) return k == '\\' || k == ']' || k == '^' || k == '_';
        return true;
    }
    return false;
}

int fuzzyScore(const std::string& query, const std::string& text) {
    auto low = [](unsigned char c) { return (char)std::tolower(c); };
    int score = 0, last = -2;
    size_t t = 0;
    bool any = false;
    for (unsigned char qc : query) {
        if (qc == ' ') continue;
        any = true;
        char q = low(qc);
        while (t < text.size() && low((unsigned char)text[t]) != q) t++;
        if (t >= text.size()) return -1;
        score += 1;
        if ((int)t == last + 1) score += 5;  // (in a row)
        if (t == 0 || !std::isalnum((unsigned char)text[t - 1])) score += 8;  // (a word's start)
        last = (int)t++;
    }
    return any ? score * 100 - (int)std::min<size_t>(text.size(), 99) : 0;  // (shorter texts first)
}

Registry::Registry(ui::MenuBar* bar) : bar_(bar) {
    for (const Json& j : settings::get("commands/recent").items())
        if (j.isString()) recent.push_back(j.toString());
}

Registry::~Registry() = default;

void Registry::add(KeyCommand c) {
    c.defaults = deduped(c.defaults);
    util::debugf("commands: key command %s (%s)\n", c.id.c_str(), keysText(c.defaults, "no key").c_str());
    keyCmds_.push_back(std::move(c));
}

void Registry::menuKeys(const std::string& id, ui::ShortcutScope scope, Keys more, bool evenDisabled) {
    menuOpts_[id] = MenuOpt{scope, std::move(more), evenDisabled};
}

void Registry::alias(const std::string& menuId, const std::string& keyId) { aliases_[menuId] = keyId; }

const KeyCommand* Registry::keyCommand(const std::string& id) const {
    for (auto& c : keyCmds_)
        if (c.id == id) return &c;
    return nullptr;
}

void Registry::walkBar(bool fill, const std::function<void(ui::Action*, const std::string&)>& fn) const {
    if (!bar_) return;
    for (auto& m : bar_->menus()) walk(m.get(), plainText(m->title), fill, fn);
}

ui::Action* Registry::find(const std::string& id) const {
    ui::Action* found = nullptr;
    walkBar(false, [&](ui::Action* a, const std::string& aid) {
        if (!found && aid == id) found = a;
    });
    return found;
}

Keys Registry::defaults(const std::string& id) const {
    auto al = aliases_.find(id);
    if (al != aliases_.end()) return defaults(al->second);
    if (const KeyCommand* c = keyCommand(id)) return c->defaults;
    auto d = menuDefaults_.find(id);
    return d == menuDefaults_.end() ? Keys() : d->second;
}

Keys Registry::keys(const std::string& id) const {
    auto al = aliases_.find(id);
    if (al != aliases_.end()) return keys(al->second);
    auto o = overrides_.find(id);
    return o != overrides_.end() ? o->second : defaults(id);
}

bool Registry::known(const std::string& id) const {
    return keyCommand(id) || menuDefaults_.count(id) || aliases_.count(id);
}

std::vector<std::string> Registry::ids() const {
    std::vector<std::string> out;
    for (auto& id : menuOrder_)
        if (!aliases_.count(id)) out.push_back(id);
    for (auto& c : keyCmds_) out.push_back(c.id);
    return out;
}

std::vector<Command> Registry::list(bool palette) {
    std::vector<Command> out;
    walkBar(palette, [&](ui::Action* a, const std::string& id) {
        if (!menuDefaults_.count(id)) {  // (as first seen: what it was built with)
            Keys d;
            if (!a->shortcut.empty()) d.push_back(a->shortcut);
            auto opt = menuOpts_.find(id);
            if (opt != menuOpts_.end()) d.insert(d.end(), opt->second.more.begin(), opt->second.more.end());
            menuDefaults_[id] = deduped(d);
            menuOrder_.push_back(id);
        }
        Command c;
        c.id = id;
        c.name = lastPart(id);
        c.where = firstParts(id);
        c.action = a;
        auto al = aliases_.find(id);
        if (al != aliases_.end()) c.aliasOf = al->second;
        c.keys = keys(id);
        c.defaults = defaults(id);
        c.enabled = a->enabled;
        c.visible = a->visible;
        c.checkable = a->checkable;
        c.checked = a->checked;
        c.tooltip = a->tooltip;
        a->shortcut = c.keys.empty() ? ui::KeySequence() : c.keys[0];  // (what the menu shows)
        if (!palette || c.visible) out.push_back(std::move(c));
    });
    for (auto& k : keyCmds_) {
        Command c;
        c.id = k.id;
        c.name = k.name;
        c.where = firstParts(k.id);
        c.keys = keys(k.id);
        c.defaults = k.defaults;
        c.enabled = !k.active || k.active();
        out.push_back(std::move(c));
    }
    if (palette) {
        context_.clear();
        std::vector<std::pair<std::string, std::shared_ptr<ui::Menu>>> menus;
        if (contextMenus) contextMenus(menus);
        for (auto& [title, menu] : menus) {
            if (!menu) continue;
            context_.push_back(menu);
            showKeys(*menu);
            walk(menu.get(), title, false, [&](ui::Action* a, const std::string& id) {
                if (!a->visible) return;
                Command c;
                c.id = id;
                c.name = lastPart(id);
                c.where = firstParts(id);
                c.action = a;
                c.context = true;
                if (!a->shortcut.empty()) c.keys.push_back(a->shortcut);
                c.enabled = a->enabled;
                c.checkable = a->checkable;
                c.checked = a->checked;
                c.tooltip = a->tooltip;
                out.push_back(std::move(c));
            });
        }
    }
    return out;
}

void Registry::setKeys(const std::string& id, Keys ks) {
    auto al = aliases_.find(id);
    if (al != aliases_.end()) return setKeys(al->second, std::move(ks));
    ks = deduped(ks);
    Keys d = defaults(id);
    bool same = ks.size() == d.size();
    for (size_t i = 0; same && i < ks.size(); i++) same = sameKey(ks[i], d[i]);
    if (same) overrides_.erase(id);
    else overrides_[id] = ks;
    util::debugf("commands: %s = %s\n", id.c_str(), keysText(ks, "(no keys)").c_str());
    apply();
    save();
}

void Registry::reset(const std::string& id) {
    auto al = aliases_.find(id);
    overrides_.erase(al != aliases_.end() ? al->second : id);
    util::debugf("commands: %s reset to %s\n", id.c_str(), keysText(defaults(id), "(no keys)").c_str());
    apply();
    save();
}

void Registry::resetAll() {
    overrides_.clear();
    util::debugf("commands: every key reset\n");
    apply();
    save();
}

std::vector<std::string> Registry::usersOf(const ui::KeySequence& k, const std::string& except) const {
    std::vector<std::string> out;
    std::vector<std::string> all = ids();
    for (auto& [id, ks] : overrides_)
        if (std::find(all.begin(), all.end(), id) == all.end()) all.push_back(id);
    for (auto& id : all)
        if (id != except && contains(keys(id), k)) out.push_back(id);
    return out;
}

bool Registry::run(const std::string& id) {
    if (const KeyCommand* c = keyCommand(id)) {
        if (c->active && !c->active()) {
            util::debugf("commands: %s: not now\n", id.c_str());
            return false;
        }
        util::debugf("commands: run %s\n", id.c_str());
        auto fn = c->run;  // (it may change the registry)
        if (fn) fn();
        return (bool)fn;
    }
    ui::Action* a = find(id);
    if (!a) {
        util::debugf("commands: %s: no such entry now\n", id.c_str());
        return false;
    }
    auto opt = menuOpts_.find(id);
    if (opt != menuOpts_.end() && opt->second.evenDisabled && !a->checkable) {
        util::debugf("commands: run %s\n", id.c_str());
        auto fn = a->triggered;
        if (fn) fn();
        return true;
    }
    if (!a->enabled || !a->visible) {
        util::debugf("commands: %s is disabled\n", id.c_str());
        if (message) message(lastPart(id) + " can't be used now" + (a->tooltip.empty() ? "" : ": " + a->tooltip));
        return false;
    }
    util::debugf("commands: run %s\n", id.c_str());
    ui::Menu::activate(a);
    return true;
}

bool Registry::run(const Command& c) {
    if (!c.context) return run(c.id);
    if (!c.action || !c.action->enabled) return false;
    util::debugf("commands: run %s (right-click menu)\n", c.id.c_str());
    ui::Menu::activate(c.action);
    return true;
}

bool Registry::translate(const std::string& widget, ui::KeyEvent& e) const {
    if (e.key == plat::Key_Shift || e.key == plat::Key_Control || e.key == plat::Key_Alt || e.key == plat::Key_Meta)
        return true;
    for (auto& c : keyCmds_) {
        if (c.widget != widget) continue;
        for (auto& k : keys(c.id))
            if (k.matches(e)) {
                if (c.defaults.empty()) return false;
                if (!sameKey(k, c.defaults[0]))
                    util::debugf("commands: %s: %s as %s\n", c.id.c_str(), k.text().c_str(), c.defaults[0].text().c_str());
                e.key = c.defaults[0].key;
                e.mods = (e.mods & ~MASK) | c.defaults[0].mods;
                e.text.clear();
                return true;
            }
    }
    // a default key no longer bound to its command: nothing (it isn't the command's any more)
    for (auto& id : ids())
        for (auto& d : defaults(id))
            if (d.matches(e) && !contains(keys(id), d)) {
                util::debugf("commands: %s no longer %s: ignored\n", d.text().c_str(), id.c_str());
                return false;
            }
    return true;
}

void Registry::showKeys(ui::Menu& menu) const {
    std::vector<std::string> all = ids();
    walk(&menu, "", false, [&](ui::Action* a, const std::string&) {
        if (a->shortcut.empty()) return;
        for (auto& id : all)
            if (contains(defaults(id), a->shortcut)) {
                Keys now = keys(id);
                a->shortcut = now.empty() ? ui::KeySequence() : now[0];
                return;
            }
    });
}

void Registry::apply() {
    shortcuts_.clear();
    list(false);  // (every entry's default noted; the menus show the keys bound)
    int n = 0;
    for (auto& c : keyCmds_) {
        Keys ks = keys(c.id);
        if (c.shown) c.shown(ks.empty() ? ui::KeySequence() : ks[0]);
        if (!c.widget.empty() || !c.owner || !ui::Widget::alive(c.owner)) continue;
        for (auto& k : ks) {
            std::string id = c.id;
            auto s = std::make_unique<ui::Shortcut>(c.owner, k, [this, id] { run(id); }, c.scope);
            s->active = c.active;
            shortcuts_.push_back(std::move(s));
            n++;
        }
    }
    std::vector<std::string> menuIds;
    for (auto& id : menuOrder_)
        if (!aliases_.count(id)) menuIds.push_back(id);
    for (auto& [id, ks] : overrides_)  // (entries of menus filled only when they open)
        if (!keyCommand(id) && !aliases_.count(id) && !menuDefaults_.count(id)) menuIds.push_back(id);
    if (bar_ && ui::Widget::alive(bar_))
        for (auto& id : menuIds) {
            auto opt = menuOpts_.find(id);
            MenuOpt o = opt != menuOpts_.end() ? opt->second : MenuOpt();
            for (auto& k : keys(id)) {
                auto s = std::make_unique<ui::Shortcut>(bar_, k, [this, id] { run(id); }, o.scope);
                bool even = o.evenDisabled;
                s->active = [this, id, even] {
                    ui::Action* a = find(id);
                    return a && a->visible && (a->enabled || even);
                };
                shortcuts_.push_back(std::move(s));
                n++;
            }
        }
    util::debugf("commands: %d keys bound (%zu commands changed from their defaults)\n", n, overrides_.size());
}

void Registry::noteRecent(const std::string& id) {
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    recent.insert(recent.begin(), id);
    if (recent.size() > RECENT_MAX) recent.resize(RECENT_MAX);
    Json j = Json::array();
    for (auto& r : recent) j.push(r);
    settings::set("commands/recent", j);
}

std::string Registry::text(bool every) const {
    std::string s = "# Tiny DAW keymap: one key per line, \"command = key\" (\"command =\": no keys at all).\n"
                    "# A command named here gets exactly the keys listed for it; the others keep theirs.\n";
    std::vector<std::string> all = every ? ids() : std::vector<std::string>();
    for (auto& [id, ks] : overrides_)
        if (std::find(all.begin(), all.end(), id) == all.end()) all.push_back(id);
    for (auto& id : all) {
        Keys ks = keys(id);
        if (every && ks.empty() && defaults(id).empty()) continue;  // (never had any)
        if (ks.empty()) s += id + " =\n";
        for (auto& k : ks) s += id + " = " + k.text() + "\n";
    }
    return s;
}

std::vector<std::string> Registry::load(const std::string& text, const std::string& name) {
    std::vector<std::string> problems;
    std::map<std::string, Keys> read;
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = util::trim(text.substr(pos, end - pos));
        pos = end + 1;
        lineNo++;
        if (line.empty() || line[0] == '#') continue;
        auto skip = [&](const std::string& why) {
            std::string p = name + " line " + std::to_string(lineNo) + ": " + why + " (skipped)";
            util::debugf("commands: %s\n", p.c_str());
            problems.push_back(p);
        };
        // "id = key": the first " =" followed by a space or the end (a key may be "=" itself)
        size_t eq = line[0] == '=' ? 0 : std::string::npos;
        for (size_t i = line.find(" ="); eq == std::string::npos && i != std::string::npos; i = line.find(" =", i + 1))
            if (i + 2 == line.size() || line[i + 2] == ' ') {
                eq = i;
                break;
            }
        if (eq == std::string::npos) {
            skip("no \" = \" between a command and its key");
            continue;
        }
        std::string id = util::trim(line.substr(0, eq)), keyText = util::trim(line.substr(std::min(line.size(), eq + 2)));
        if (id.empty()) {
            skip("no command before the =");
            continue;
        }
        Keys& ks = read[id];  // (named: its keys are these)
        if (keyText.empty()) continue;
        ui::KeySequence k = ui::KeySequence::parse(keyText);
        if (k.empty() || k.key == 0xFFFD || k.key < 0x20 || !sameKey(ui::KeySequence::parse(k.text()), k)) {  // (bytes not UTF-8)
            skip("\"" + keyText + "\" isn't a key");
            continue;
        }
        if (!contains(ks, k)) ks.push_back(k);
    }
    for (auto& [id, ks] : read) {
        if (!known(id)) util::debugf("commands: %s: %s isn't a command now (kept)\n", name.c_str(), id.c_str());
        auto al = aliases_.find(id);
        overrides_[al != aliases_.end() ? al->second : id] = ks;
    }
    util::debugf("commands: %s: %zu commands read, %zu lines skipped\n", name.c_str(), read.size(), problems.size());
    return problems;
}

std::vector<std::string> Registry::loadFile(const std::string& file) {
    std::string data;
    if (!util::readFile(file, data)) {
        util::debugf("commands: no keymap at %s\n", file.c_str());
        return {};
    }
    return load(data, util::baseName(file));
}

bool Registry::save() const {
    if (path.empty()) return true;
    util::makeDirs(util::dirName(path));
    bool ok = util::writeFile(path, text(false));
    util::debugf("commands: keymap %s %s\n", ok ? "saved to" : "NOT saved to", path.c_str());
    return ok;
}

bool Registry::exportTo(const std::string& file) const {
    bool ok = util::writeFile(file, text(true));
    util::debugf("commands: keymap %s %s\n", ok ? "exported to" : "NOT exported to", file.c_str());
    return ok;
}

}  // namespace commands
