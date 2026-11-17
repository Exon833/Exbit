#include "ui/skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"
#include "paths.h"
#include "ui/colors.h"
#include "ui/toolkit/dialogs.h"

namespace skin {

const char* const DEFAULT = "Dark";

namespace {
std::string gUserDir, gBuiltinDir;
std::vector<std::pair<int, std::function<void()>>>& hooks() {
    static std::vector<std::pair<int, std::function<void()>>> h;
    return h;
}

const char* const OLD_FOLDER = "old JSON skins";

bool knownColor(const std::string& name) {
    for (auto& info : theme::colors())
        if (name == info.name) return true;
    return false;
}

// "#rrggbb" or "#rrggbbaa", as JSON skins had them
bool jsonColor(const std::string& text) {
    if (text.empty() || text[0] != '#' || (text.size() != 7 && text.size() != 9)) return false;
    for (size_t i = 1; i < text.size(); i++)
        if (!isxdigit((unsigned char)text[i])) return false;
    return true;
}

// text that can go inside a /* comment */
std::string commentSafe(std::string s) {
    for (size_t at; (at = s.find("*/")) != std::string::npos;) s.replace(at, 2, "* /");
    return s;
}

std::vector<std::string> texts(const std::vector<css::Problem>& problems) {
    std::vector<std::string> out;
    for (auto& p : problems) out.push_back(p.text());
    return out;
}

css::SheetPtr parseFile(const std::string& path, std::vector<css::Problem>& problems, bool& ok) {
    std::string text;
    ok = util::readFile(path, text);
    return ok ? css::parse(text, util::baseName(path), problems) : nullptr;
}

// the stylesheet's text with its --skin-name set to `name` (added at its end if it has none)
std::string withName(const std::string& text, const std::string& name) {
    size_t at = 0;
    while ((at = text.find("--skin-name", at)) != std::string::npos) {
        size_t colon = at + 11;
        while (colon < text.size() && isspace((unsigned char)text[colon])) colon++;
        if (colon < text.size() && text[colon] == ':') {
            size_t end = colon + 1;
            char quote = 0;
            for (; end < text.size(); end++) {  // up to the declaration's end, past strings
                char ch = text[end];
                if (quote) {
                    if (ch == '\\') end++;
                    else if (ch == quote) quote = 0;
                } else if (ch == '"' || ch == '\'') {
                    quote = ch;
                } else if (ch == ';' || ch == '}') {
                    break;
                }
            }
            return text.substr(0, colon + 1) + " " + quoted(name) + text.substr(std::min(end, text.size()));
        }
        at = colon;
    }
    std::string out = text;
    if (!out.empty() && out.back() != '\n') out += "\n";
    return out + "\n:root {\n    --skin-name: " + quoted(name) + ";\n}\n";
}

// the text without the comment it starts with (a built-in skin's header)
std::string withoutHeader(const std::string& text) {
    size_t b = 0;
    while (b < text.size() && isspace((unsigned char)text[b])) b++;
    if (text.compare(b, 2, "/*") != 0) return text;
    size_t e = text.find("*/", b + 2);
    if (e == std::string::npos) return text;
    e += 2;
    while (e < text.size() && (text[e] == '\n' || text[e] == '\r')) e++;
    return text.substr(e);
}
}  // namespace

std::string builtinDir() { return gBuiltinDir.empty() ? util::joinPath(plat::dataDir(), "skins") : gBuiltinDir; }
void setBuiltinDir(const std::string& dir) { gBuiltinDir = dir; }
std::string userDir() { return gUserDir.empty() ? paths::dataDir("skins") : gUserDir; }
void setUserDir(const std::string& dir) { gUserDir = dir; }
std::string defaultSheet() { return util::joinPath(builtinDir(), "default.css"); }

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            out += "\\a ";
            continue;
        }
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "\"";
}

bool read(const std::string& path, Skin& out, std::vector<std::string>& problems, std::string& error, bool builtin) {
    std::vector<css::Problem> found;
    bool ok = false;
    css::SheetPtr sheet = parseFile(path, found, ok);
    if (!ok) {
        error = "Can't read the file.";
        return false;
    }
    css::Engine e;  // (only for its name: the sheet's :root alone)
    e.setSheets({sheet});
    std::string name;
    if (std::optional<std::string> v = e.root().variable("--skin-name")) name = util::trim(css::unquote(*v));
    for (auto& p : e.takeProblems()) found.push_back(p);
    problems = texts(found);
    Skin s;
    s.name = name.empty() ? util::stem(path) : name;
    s.path = path;
    s.builtin = builtin;
    out = std::move(s);
    return true;
}

std::string safeName(const std::string& name) {
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
    return out.empty() ? std::string("skin") : out;
}

std::string fileName(const std::string& name) { return safeName(name) + ".css"; }

std::vector<Skin> library(const std::string& user) {
    std::string mine = user.empty() ? userDir() : user;
    if (util::isDir(mine)) convertOldSkins(mine);
    std::vector<Skin> found;
    std::pair<std::string, bool> folders[] = {{builtinDir(), true}, {mine, false}};
    for (auto& f : folders) {
        if (!util::isDir(f.first)) continue;
        for (auto& e : util::listDir(f.first)) {
            if (e.isDir || util::extension(e.name) != ".css") continue;
            if (f.second && util::lower(e.name) == "default.css") continue;  // (what every skin is put on)
            Skin s;
            std::vector<std::string> problems;
            std::string err;
            try {  // (no skin file may stop the program from starting)
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

std::vector<std::string> apply(const Skin& s) {
    std::vector<css::Problem> problems;
    std::vector<css::SheetPtr> sheets;
    bool ok = false;
    std::string def = defaultSheet();
    if (css::SheetPtr d = parseFile(def, problems, ok)) sheets.push_back(d);
    else {  // (the copy built into the program instead)
        problems.push_back({util::baseName(def), 0, 0, "can't read the program's own stylesheet " + def + " (using the one built in)"});
        sheets.push_back(theme::builtinSheet());
    }
    if (!s.path.empty() && util::absolutePath(s.path) != util::absolutePath(def)) {
        if (css::SheetPtr sheet = parseFile(s.path, problems, ok)) sheets.push_back(sheet);
        else problems.push_back({util::baseName(s.path), 0, 0, "can't read " + s.path});
    }
    theme::setStyles(std::move(sheets), &problems);
    colors::clearCache();
    auto copy = hooks();
    for (auto& h : copy)
        if (h.second) h.second();
    return texts(problems);
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

// --- skins of earlier versions ---

bool cssFromJson(const std::string& jsonText, const std::string& fallbackName, std::string& out, std::string& error) {
    std::string perr;
    Json data = Json::parse(jsonText, &perr);
    if (!data.isObject() || !data["format"].isString() || data["format"].toString() != "tinydaw-skin") {
        error = "Not a Tiny DAW skin.";
        return false;
    }
    const Json& n = data["name"];
    std::string name = n.isString() && !util::trim(n.toString()).empty() ? util::trim(n.toString()) : fallbackName;
    std::vector<std::string> left;
    std::string fonts, cols;
    const Json& f = data["fonts"];
    if (f.isObject()) {
        for (auto& kv : f.members()) {
            const Json& v = kv.second;
            if (kv.first == "ui" && v.isString()) {
                if (!util::trim(v.toString()).empty()) fonts += "    font-family: " + quoted(v.toString()) + ";\n";
            } else if (kv.first == "ui_size" && v.isNumber() && v.toDouble() >= 0 && v.toDouble() <= 40) {
                if (v.toDouble() > 0) {
                    char size[32];
                    snprintf(size, sizeof size, "%g", v.toDouble());
                    fonts += std::string("    font-size: ") + size + "pt;\n";
                }
            } else if (kv.first == "mono" && v.isString()) {
                if (!util::trim(v.toString()).empty() && v.toString() != "Consolas")
                    fonts += "    --mono-font: " + quoted(v.toString()) + ";\n";
            } else {
                left.push_back("font setting " + kv.first + ": " + v.dump());
            }
        }
    }
    const Json& c = data["colors"];
    if (c.isObject()) {
        for (auto& info : theme::colors()) {  // (in the table's order, as default.css has them)
            const Json& v = c[info.name];
            if (v.isString() && jsonColor(v.toString()))
                cols += "    " + theme::variableName(info.name) + ": " + util::lower(v.toString()) + ";\n";
        }
        for (auto& kv : c.members())
            if (!knownColor(kv.first)) left.push_back("unknown colour " + kv.first + ": " + kv.second.dump());
            else if (!kv.second.isString() || !jsonColor(kv.second.toString()))
                left.push_back(kv.first + ": not a colour (" + kv.second.dump() + ")");
    }
    out = "/* " + commentSafe(name) +
          ": a skin of Tiny DAW, made from a JSON skin of an earlier version. It is put on top of the program's\n"
          "   own default.css: it changes the variables it names. docs/skins.md lists all there is to style. */\n\n"
          ":root {\n    --skin-name: " +
          quoted(name) + ";\n" + fonts + (fonts.empty() ? "" : "\n") + cols + "}\n";
    if (!left.empty()) {
        out += "\n/* left out of the JSON skin (not understood):\n";
        for (auto& l : left) out += "   " + commentSafe(l) + "\n";
        out += "*/\n";
    }
    return true;
}

std::vector<std::string> convertOldSkins(const std::string& dir) {
    std::vector<std::string> written;
    for (auto& e : util::listDir(dir)) {
        if (e.isDir || util::extension(e.name) != ".json") continue;
        std::string path = util::joinPath(dir, e.name), text, css, err;
        if (!util::readFile(path, text) || !cssFromJson(text, util::stem(path), css, err)) continue;  // (not a skin)
        std::string target = util::joinPath(dir, util::stem(path) + ".css");
        if (!util::fileExists(target)) {
            if (!util::writeFile(target, css)) continue;  // (left as it is: tried again next time)
            written.push_back(target);
        }
        std::string old = util::joinPath(dir, OLD_FOLDER);
        util::makeDirs(old);
        std::string moved = util::joinPath(old, e.name);
        for (int i = 2; util::fileExists(moved); i++)
            moved = util::joinPath(old, util::stem(e.name) + " (" + std::to_string(i) + ").json");
        std::rename(path.c_str(), moved.c_str());
    }
    return written;
}

// --- Options → Skin actions ---

std::string makeCopy(const Skin& s, const std::string& name, std::string& error) {
    std::string text;
    if (!s.path.empty() && !util::readFile(s.path, text)) {
        error = "Can't read " + s.path;
        return "";
    }
    std::string header = "/* " + commentSafe(name) + ": your own skin" +
                         (s.name.empty() ? std::string() : ", made from " + commentSafe(s.name)) +
                         ". It is put on top of Tiny DAW's own\n"
                         "   default.css: change any variable or rule here, save, and Tiny DAW shows it at once.\n"
                         "   docs/skins.md lists every variable, selector, part and property there is. */\n\n";
    std::string path = util::joinPath(userDir(), fileName(name));
    util::makeDirs(userDir());
    if (!util::writeFile(path, header + withName(withoutHeader(text), name))) {
        error = "Can't write " + path;
        return "";
    }
    return path;
}

bool openInEditor(const std::string& path) { return plat::openUrl(path); }

bool importSkin(ui::Window* parent, const std::function<void(const Skin&)>& choose) {
    std::string path = ui::files::openFile(parent, "Import skin", "", "Tiny DAW skin (*.css)");
    if (path.empty()) return false;
    Skin s;
    std::vector<std::string> problems;
    std::string err, text;
    if (!read(path, s, problems, err) || !util::readFile(path, text)) {
        ui::msg::warning(parent, "Cannot import skin", path + "\n\n" + (err.empty() ? "Can't read the file." : err));
        return false;
    }
    std::string target = util::joinPath(userDir(), fileName(s.name));
    if (util::absolutePath(target) != util::absolutePath(path)) {
        if (util::fileExists(target) &&
            ui::msg::question(parent, "Skin exists", "You already have a skin called " + s.name + ". Replace it?") !=
                ui::msg::Yes)
            return false;
        util::makeDirs(userDir());
        if (!util::writeFile(target, text)) {
            ui::msg::warning(parent, "Cannot import skin", path + "\n\nCan't write " + target);
            return false;
        }
    }
    s.path = target;
    s.builtin = false;
    if (choose) choose(s);
    if (!problems.empty()) {
        std::string list;
        for (size_t i = 0; i < problems.size() && i < 20; i++) list += (i ? "\n" : "") + problems[i];
        ui::msg::information(parent, "Skin imported",
                             s.name + " is in use. These parts of it were left out (the default stays):\n\n" + list);
    }
    return true;
}

std::string exportSkin(ui::Window* parent, const Skin& s) {
    std::string path = ui::files::saveFile(parent, "Export skin", fileName(s.name), "Tiny DAW skin (*.css)");
    if (path.empty()) return "";
    if (util::extension(path) != ".css") path += ".css";
    std::string text = ":root {\n    --skin-name: " + quoted(s.name) + ";\n}\n";
    if (!s.path.empty() && !util::readFile(s.path, text)) {
        ui::msg::warning(parent, "Cannot export skin", "Can't read " + s.path);
        return "";
    }
    if (!util::writeFile(path, text)) {
        ui::msg::warning(parent, "Cannot export skin", "Can't write " + path);
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
