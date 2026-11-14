#include "ui/skin.h"

#include <algorithm>
#include <cmath>

#include "core/util.h"
#include "platform/platform.h"
#include "paths.h"
#include "ui/colors.h"
#include "ui/toolkit/dialogs.h"

namespace skin {

const char* const FORMAT = "tinydaw-skin";
const char* const DEFAULT = "Dark";

namespace {
std::string gUserDir, gBuiltinDir;
std::vector<std::pair<int, std::function<void()>>>& hooks() {
    static std::vector<std::pair<int, std::function<void()>>> h;
    return h;
}

bool knownColor(const std::string& name) {
    for (auto& info : theme::colors())
        if (name == info.name) return true;
    return false;
}

// Python's repr() of a JSON value, for the problems' texts
std::string repr(const Json& v) {
    switch (v.type()) {
    case Json::Null: return "None";
    case Json::Bool: return v.toBool() ? "True" : "False";
    case Json::String: {
        const std::string& s = v.toString();
        bool single = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
        char q = single ? '"' : '\'';
        std::string out(1, q);
        for (char ch : s) {
            if (ch == '\\') out += "\\\\";
            else if (ch == q) out += std::string("\\") + q;
            else if (ch == '\n') out += "\\n";
            else if (ch == '\t') out += "\\t";
            else out += ch;
        }
        return out + q;
    }
    case Json::Number: {
        std::string d = v.dump();
        return d;
    }
    default: return v.dump();
    }
}
}  // namespace

std::string builtinDir() { return gBuiltinDir.empty() ? util::joinPath(plat::dataDir(), "skins") : gBuiltinDir; }
void setBuiltinDir(const std::string& dir) { gBuiltinDir = dir; }
std::string userDir() { return gUserDir.empty() ? paths::dataDir("skins") : gUserDir; }
void setUserDir(const std::string& dir) { gUserDir = dir; }

std::vector<std::pair<std::string, std::string>> Skin::completeColors() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& info : theme::colors()) {
        auto it = colors.find(info.name);
        out.emplace_back(info.name, it == colors.end() ? std::string(info.def) : it->second);
    }
    return out;
}

std::string Skin::color(const std::string& name) const {
    auto it = colors.find(name);
    if (it != colors.end()) return it->second;
    for (auto& info : theme::colors())
        if (name == info.name) return info.def;
    return "";
}

bool parseColor(const std::string& text, Color* out) {
    if (text.empty() || text[0] != '#' || (text.size() != 7 && text.size() != 9)) return false;
    for (size_t i = 1; i < text.size(); i++)
        if (!isxdigit((unsigned char)text[i])) return false;
    bool ok = false;
    Color c = Color::fromHex(text, &ok);
    if (!ok) return false;
    if (out) *out = c;
    return true;
}

Json toJson(const Skin& s) {
    Json colors = Json::object();
    for (auto& kv : s.completeColors()) colors.set(kv.first, kv.second);
    Json fonts = Json::object();
    fonts.set("ui", s.fonts.ui);
    double size = s.fonts.uiSize;
    if (size == std::floor(size)) fonts.set("ui_size", (int)size);
    else fonts.set("ui_size", size);
    fonts.set("mono", s.fonts.mono);
    Json j = Json::object();
    j.set("format", FORMAT);
    j.set("version", VERSION);
    j.set("name", s.name);
    j.set("colors", colors);
    j.set("fonts", fonts);
    return j;
}

bool read(const std::string& path, Skin& out, std::vector<std::string>& problems, std::string& error, bool builtin) {
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Can't read the file.";
        return false;
    }
    std::string perr;
    Json data = Json::parse(text, &perr);
    if (data.isNull() && !perr.empty()) {
        error = "Not valid JSON: " + perr;
        return false;
    }
    if (!data.isObject() || data["format"].toString() != FORMAT || !data["format"].isString()) {
        error = std::string("Not a Tiny DAW skin (it needs \"format\": \"") + FORMAT + "\").";
        return false;
    }
    for (const char* part : {"colors", "fonts"}) {
        const Json& j = data[part];
        bool falsy = j.isNull() || (j.isBool() && !j.toBool()) || (j.isNumber() && j.toDouble() == 0) ||
                     (j.isString() && j.toString().empty()) || (j.isArray() && j.size() == 0);
        if (!falsy && !j.isObject()) {  // (data.get(part) or {}: a dict)
            error = std::string("Its \"") + part + "\" must be names with their values: {\"name\": value, …}.";
            return false;
        }
    }
    problems.clear();
    Skin s;
    for (auto& kv : data["colors"].members()) {
        const std::string& name = kv.first;
        const Json& value = kv.second;
        if (!knownColor(name)) problems.push_back("unknown colour name: " + name);
        else if (!value.isString() || !parseColor(value.toString()))
            problems.push_back(name + ": not a colour (" + repr(value) + "; use \"#rrggbb\" or \"#rrggbbaa\")");
        else s.colors[name] = util::lower(value.toString());
    }
    for (auto& kv : data["fonts"].members()) {
        const std::string& name = kv.first;
        const Json& value = kv.second;
        if (name != "ui" && name != "ui_size" && name != "mono") problems.push_back("unknown font setting: " + name);
        else if (name == "ui_size" && !(value.isNumber() && value.toDouble() >= 0 && value.toDouble() <= 40))
            problems.push_back("ui_size: not a size in points (" + repr(value) + ")");
        else if (name != "ui_size" && !value.isString())
            problems.push_back(name + ": not a font name (" + repr(value) + ")");
        else if (name == "ui") s.fonts.ui = value.toString();
        else if (name == "mono") s.fonts.mono = value.toString();
        else s.fonts.uiSize = value.toDouble();
    }
    const Json& n = data["name"];
    std::string name = n.isString() && !util::trim(n.toString()).empty() ? n.toString() : util::stem(path);
    s.name = util::trim(name);
    s.path = path;
    s.builtin = builtin;
    out = std::move(s);
    return true;
}

bool write(const Skin& s, const std::string& path, std::string* error) {
    std::string dir = util::dirName(path);
    if (!dir.empty() && !util::isDir(dir)) util::makeDirs(dir);
    if (!util::writeFile(path, toJson(s).dump(2))) {
        if (error) *error = "Can't write " + path;
        return false;
    }
    return true;
}

std::string fileName(const std::string& name) {
    // re.sub(r"[^\w\- ]+", "_", name): runs of other characters become one "_" (bytes of UTF-8
    // letters count as word characters, as Python's \w takes non-ASCII letters)
    std::string out;
    bool inRun = false;
    for (unsigned char ch : name) {
        bool word = isalnum(ch) || ch == '_' || ch == '-' || ch == ' ' || ch >= 0x80;
        if (word) {
            out += (char)ch;
            inRun = false;
        } else if (!inRun) {
            out += '_';
            inRun = true;
        }
    }
    out = util::trim(out);
    return (out.empty() ? std::string("skin") : out) + ".json";
}

std::vector<Skin> library(const std::string& user) {
    std::vector<Skin> found;
    std::pair<std::string, bool> folders[] = {{builtinDir(), true}, {user.empty() ? userDir() : user, false}};
    for (auto& f : folders) {
        if (!util::isDir(f.first)) continue;
        for (auto& e : util::listDir(f.first)) {
            if (e.isDir || !util::endsWith(util::lower(e.name), ".json")) continue;
            Skin s;
            std::vector<std::string> problems;
            std::string err;
            try {  // (any broken skin file is left out: none may stop the program from starting)
                if (read(util::joinPath(f.first, e.name), s, problems, err, f.second)) found.push_back(std::move(s));
            } catch (...) {
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Skin& a, const Skin& b) {
        auto key = [](const Skin& s) { return std::make_tuple(!s.builtin, s.name != DEFAULT, util::lower(s.name)); };
        return key(a) < key(b);
    });
    return found;
}

Skin find(const std::string& name, const std::string& user) {
    std::vector<Skin> skins = library(user);
    for (auto& s : skins)
        if (!s.builtin && s.name == name) return s;
    for (auto& s : skins)
        if (s.builtin && s.name == name) return s;
    for (auto& s : skins)
        if (s.name == DEFAULT) return s;
    Skin d;
    d.name = DEFAULT;
    d.builtin = true;
    return d;
}

void apply(const Skin& s) {
    theme::setSkin(s.completeColors(), s.fonts);
    colors::clearCache();
    auto copy = hooks();
    for (auto& h : copy)
        if (h.second) h.second();
}

void removeOnApply(int id) {
    auto& h = hooks();
    h.erase(std::remove_if(h.begin(), h.end(), [id](const std::pair<int, std::function<void()>>& x) { return x.first == id; }),
            h.end());
}

int onApply(std::function<void()> fn) {
    static int next = 0;
    hooks().emplace_back(++next, std::move(fn));
    return next;
}

bool importSkin(ui::Window* parent, const std::function<void(const Skin&)>& choose) {
    std::string path = ui::files::openFile(parent, "Import skin", "", "Tiny DAW skin (*.json)");
    if (path.empty()) return false;
    Skin s;
    std::vector<std::string> problems;
    std::string err;
    if (!read(path, s, problems, err)) {
        ui::msg::warning(parent, "Cannot import skin", path + "\n\n" + err);
        return false;
    }
    std::string target = util::joinPath(userDir(), fileName(s.name));
    if (util::fileExists(target) &&
        ui::msg::question(parent, "Skin exists", "You already have a skin called " + s.name + ". Replace it?") !=
            ui::msg::Yes)
        return false;
    s.path = target;
    s.builtin = false;
    if (!write(s, target, &err)) {
        ui::msg::warning(parent, "Cannot import skin", path + "\n\n" + err);
        return false;
    }
    if (choose) choose(s);
    if (!problems.empty()) {
        std::string list;
        for (size_t i = 0; i < problems.size() && i < 20; i++) list += (i ? "\n" : "") + problems[i];
        ui::msg::information(parent, "Skin imported",
                             s.name + " is in use. These parts of it were left out (they keep the default):\n\n" + list);
    }
    return true;
}

std::string exportSkin(ui::Window* parent, const Skin& s) {
    std::string path = ui::files::saveFile(parent, "Export skin", fileName(s.name), "Tiny DAW skin (*.json)");
    if (path.empty()) return "";
    if (!util::endsWith(util::lower(path), ".json")) path += ".json";
    std::string err;
    if (!write(s, path, &err)) {
        ui::msg::warning(parent, "Cannot export skin", err);
        return "";
    }
    return path;
}

void openSkinsFolder() {
    std::string dir = userDir();
    util::makeDirs(dir);
    plat::openFolder(dir);
}

}  // namespace skin
