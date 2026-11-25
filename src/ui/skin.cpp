#include "ui/skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

#include "core/format_marker.h"
#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"
#include "paths.h"
#include "ui/colors.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/style.h"

namespace skin {

const char* const DEFAULT = "Dark";

namespace {
std::string gUserDir, gBuiltinDir;
std::vector<std::string> gWatched;
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

css::SheetPtr parseFile(const std::string& path, std::vector<css::Problem>& problems, bool& ok,
                        std::string* textOut = nullptr) {
    std::string text;
    ok = util::readFile(path, text);
    if (textOut) *textOut = text;
    css::ParseOptions options;  // (@import and @font-face's files: next to the stylesheet)
    options.baseDir = util::dirName(path);
    options.readFile = [](const std::string& file, std::string& out) { return util::readFile(file, out); };
    // (url() images: a PNG or SVG that can be read, else reported where it is named; kept, decoded, for drawing)
    options.checkImage = [](const std::string& file, std::string& why) { return ui::style::imageFileOk(file, why); };
    return ok ? css::parse(text, util::baseName(path), problems, options) : nullptr;
}

// where `variable` is declared last among the files ({name, text}, in cascade order): its line and column
bool declaredAt(const std::vector<std::pair<std::string, std::string>>& files, const std::string& variable,
                css::Problem& where) {
    for (size_t f = files.size(); f-- > 0;) {
        const std::string& t = files[f].second;
        size_t found = std::string::npos;
        for (size_t at = t.find(variable); at != std::string::npos; at = t.find(variable, at + 1)) {
            unsigned char before = at ? (unsigned char)t[at - 1] : ' ';
            if (isalnum(before) || before == '-' || before == '_') continue;
            size_t k = at + variable.size();
            while (k < t.size() && (t[k] == ' ' || t[k] == '\t')) k++;
            if (k < t.size() && t[k] == ':') found = at;  // (the last one counts)
        }
        if (found == std::string::npos) continue;
        where.source = files[f].first;
        where.line = 1 + (int)std::count(t.begin(), t.begin() + (long)found, '\n');
        size_t lineStart = t.rfind('\n', found);
        where.column = (int)(found - (lineStart == std::string::npos ? 0 : lineStart + 1)) + 1;
        return true;
    }
    return false;
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

// --- a skin's files, copied with it ---
// "/a/b" "/a/c/d.png" -> "../c/d.png" (both absolute and normalised)
std::string relativePath(const std::string& fromDir, const std::string& to) {
    std::vector<std::string> a, b;
    for (auto& p : util::split(fromDir, '/'))
        if (!p.empty()) a.push_back(p);
    for (auto& p : util::split(to, '/'))
        if (!p.empty()) b.push_back(p);
    size_t common = 0;
    while (common < a.size() && common + 1 < b.size() && a[common] == b[common]) common++;
    std::string out;
    for (size_t i = common; i < a.size(); i++) out += "../";
    for (size_t i = common; i < b.size(); i++) out += b[i] + (i + 1 < b.size() ? "/" : "");
    return out;
}

// What copying a stylesheet with its files does (planCopy(), then writeFiles()): the files it uses (its url() images,
// its @font-face fonts and the stylesheets it @imports, at any depth: those there are) go into a folder of their
// own beside the copy, named after it ("My skin.css": "My skin/"), keeping where they are from one another; the
// url()s of the copy and of the stylesheets it imports are changed to name them there. A file already there that
// is the same is left as it is; one that isn't is never overwritten: "My skin (2)/" is used then (and so on).
struct FileCopy {
    std::string from, to;  // (absolute paths)
    std::string data;      // what `to` gets (an imported stylesheet: its url()s changed)
    bool sheet = false;
};
struct SkinFiles {
    std::string folder;  // where the files go ("": none to copy)
    std::vector<FileCopy> files;
    std::string text;    // the stylesheet's own text, its url()s changed
};

bool sameFile(const std::string& path, const std::string& data) {
    std::string now;
    return !util::isDir(path) && util::readFile(path, now) && now == data;
}

// the stylesheet `from` (its text `text`) copied to `target`, with its files: false and `error` if a file it uses
// can't be read
bool planCopy(const std::string& fromPath, const std::string& text, const std::string& targetPath, SkinFiles& out,
              std::string& error) {
    std::string from = util::absolutePath(fromPath), target = util::absolutePath(targetPath);
    std::string fromDir = util::dirName(from), targetDir = util::dirName(target);
    out = SkinFiles();
    out.text = text;
    // the files it uses (resolved as the parser resolves them)
    css::ParseOptions options;
    options.baseDir = fromDir;
    options.readFile = [](const std::string& file, std::string& data) { return util::readFile(file, data); };
    options.checkImage = [](const std::string&, std::string&) { return true; };  // (only their names here)
    std::vector<css::Problem> ignored;
    css::SheetPtr sheet = css::parse(text, util::baseName(from), ignored, options);
    std::vector<std::string> used;
    std::vector<std::string> imports = css::importedFiles(sheet);
    auto add = [&](const std::string& f) {
        std::string path = util::absolutePath(f);
        if (path.empty() || path == from || std::find(used.begin(), used.end(), path) != used.end()) return;
        if (!util::fileExists(path) || util::isDir(path)) return;  // (missing: reported when the skin is put on)
        used.push_back(path);
    };
    for (auto& f : imports) add(f);
    for (auto& f : css::imageFiles(sheet)) add(f);
    std::vector<std::string> fontDirs;
    for (auto& f : css::fontFaces(sheet)) {
        size_t before = used.size();
        add(f.file);
        if (used.size() > before && std::find(fontDirs.begin(), fontDirs.end(), util::dirName(used.back())) == fontDirs.end())
            fontDirs.push_back(util::dirName(used.back()));
    }
    // (a font's licence goes with it: the text files beside it, *.txt, *.md, LICENSE, COPYING…)
    for (auto& dir : fontDirs)
        for (auto& e : util::listDir(dir)) {
            std::string n = util::lower(e.name), ext = util::extension(e.name);
            if (!e.isDir && (ext == ".txt" || ext == ".md" || ext.empty() || n.find("license") != std::string::npos ||
                             n.find("licence") != std::string::npos || n.find("copying") != std::string::npos))
                add(util::joinPath(dir, e.name));
        }
    for (auto& f : imports) f = util::absolutePath(f);
    if (used.empty()) return true;
    // where each goes in the folder: as it is from the stylesheet's own folder (those outside it: by their name),
    // less a folder all of them are in ("Mine/xp/a.svg", "Mine/fonts/b.ttf": "xp/a.svg", "fonts/b.ttf")
    std::vector<std::string> rel;
    for (auto& f : used) {
        std::string r = util::startsWith(f, fromDir == "/" ? "/" : fromDir + "/") ? f.substr(fromDir.size() + (fromDir == "/" ? 0 : 1))
                                                                                    : util::baseName(f);
        std::string unique = r;
        for (int n = 2; std::find(rel.begin(), rel.end(), unique) != rel.end(); n++)
            unique = util::joinPath(util::dirName(r), util::stem(r) + " (" + std::to_string(n) + ")" + util::extension(r));
        rel.push_back(unique);
    }
    for (;;) {
        size_t slash = rel[0].find('/');
        if (slash == std::string::npos) break;
        std::string first = rel[0].substr(0, slash + 1);
        bool all = true;
        for (auto& r : rel) all = all && util::startsWith(r, first) && r.size() > first.size();
        if (!all) break;
        for (auto& r : rel) r = r.substr(first.size());
    }
    // the folder: the copy's name, or the first "name (n)" where no other file would be overwritten
    std::string name = util::stem(target);
    for (int n = 1;; n++) {
        if (n > 1000) {
            error = "No folder for the skin's files beside " + target;
            return false;
        }
        std::string folder = util::joinPath(targetDir, n == 1 ? name : name + " (" + std::to_string(n) + ")");
        if (util::fileExists(folder) && !util::isDir(folder)) continue;
        out.folder = folder;
        out.files.clear();
        std::map<std::string, std::string> where;  // from -> to
        for (size_t i = 0; i < used.size(); i++) where[used[i]] = util::joinPath(folder, rel[i]);
        // a url() of a stylesheet that was in `dir` and is now in `nowDir`
        auto rewrite = [&](const std::string& data, const std::string& dir, const std::string& nowDir) {
            return css::rewriteUrls(data, [&](const std::string& url) -> std::string {
                std::string path = css::resolveUrl(dir, url);
                if (path.empty()) return "";
                auto it = where.find(util::absolutePath(path));
                return it == where.end() ? "" : relativePath(nowDir, it->second);
            });
        };
        bool clash = false;
        for (size_t i = 0; i < used.size() && !clash; i++) {
            FileCopy c;
            c.from = used[i];
            c.to = where[used[i]];
            if (!util::readFile(c.from, c.data)) {
                error = "Can't read " + c.from;
                return false;
            }
            c.sheet = std::find(imports.begin(), imports.end(), c.from) != imports.end();
            if (c.sheet) c.data = rewrite(c.data, util::dirName(c.from), util::dirName(c.to));
            if (c.to != c.from && util::fileExists(c.to) && !sameFile(c.to, c.data)) clash = true;
            for (std::string d = util::dirName(c.to); !clash && d.size() > folder.size(); d = util::dirName(d))
                clash = util::fileExists(d) && !util::isDir(d);  // (a file where a folder must be)
            out.files.push_back(std::move(c));
        }
        if (clash) continue;
        out.text = rewrite(text, fromDir, targetDir);
        return true;
    }
}

// writes the files planCopy found (not the stylesheet itself); false and `error` if one can't be written
bool writeFiles(const SkinFiles& plan, std::string& error) {
    for (const FileCopy& c : plan.files) {
        if (c.to == c.from || sameFile(c.to, c.data)) continue;
        util::makeDirs(util::dirName(c.to));
        if (!util::writeFile(c.to, c.data)) {
            error = "Can't write " + c.to;
            return false;
        }
    }
    return true;
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
    ui::style::clearImageCaches();  // (the old skin's images: the new one's are read as its stylesheets are)
    std::vector<css::Problem> problems;
    std::vector<css::SheetPtr> sheets;
    std::vector<std::pair<std::string, std::string>> files;  // (name, text) of each, for where things are
    bool ok = false;
    std::string def = defaultSheet(), text;
    if (css::SheetPtr d = parseFile(def, problems, ok, &text)) {
        sheets.push_back(d);
        files.emplace_back(util::baseName(def), text);
    }
    else {  // (the copy built into the program instead)
        problems.push_back({util::baseName(def), 0, 0, "can't read the program's own stylesheet " + def + " (using the one built in)"});
        sheets.push_back(theme::builtinSheet());
    }
    if (!s.path.empty() && util::absolutePath(s.path) != util::absolutePath(def)) {
        if (css::SheetPtr sheet = parseFile(s.path, problems, ok, &text)) {
            sheets.push_back(sheet);
            files.emplace_back(util::baseName(s.path), text);
        } else {
            problems.push_back({util::baseName(s.path), 0, 0, "can't read " + s.path});
        }
    }
    // the sheets' own fonts (@font-face), before the styles that name them
    for (size_t i = 0, built = sheets.size() - files.size(); i < sheets.size(); i++)
        for (const css::FontFace& f : css::fontFaces(sheets[i]))
            if (!Font::addFamily(f.family, f.file, f.bold, f.italic))
                problems.push_back({i >= built ? files[i - built].first : util::baseName(def), 0, 0,
                                    "@font-face '" + f.family + "': no font that can be read in " + f.file});
    gWatched.clear();
    gWatched.push_back(def);
    if (!s.path.empty() && util::absolutePath(s.path) != util::absolutePath(def)) gWatched.push_back(s.path);
    for (auto& sheet : sheets) {
        for (auto& f : css::importedFiles(sheet))
            if (std::find(gWatched.begin(), gWatched.end(), f) == gWatched.end()) gWatched.push_back(f);
        for (auto& f : css::imageFiles(sheet))  // (its url() images: saving one shows it at once)
            if (std::find(gWatched.begin(), gWatched.end(), f) == gWatched.end()) gWatched.push_back(f);
    }
    theme::setStyles(std::move(sheets), &problems);
    // values with var() that won't do wherever they are used (computing a node finds them only for that node)
    theme::styles().checkVarUses();
    for (css::Problem& p : theme::styles().takeProblems()) problems.push_back(std::move(p));
    // colour variables that aren't colours (any text is a variable's value: only its use tells)
    for (const std::string& var : theme::colorVariables()) {
        std::optional<std::string> v = theme::styles().root().variable(var);
        css::Rgba rgba;
        if (!v || css::parseColor(*v, rgba)) continue;
        css::Problem p{"", 0, 0, var + ": '" + *v + "' is not a colour (the default is used)"};
        if (!declaredAt(files, var, p)) p.source = s.name;
        problems.push_back(p);
    }
    colors::clearCache();
    auto copy = hooks();
    for (auto& h : copy)
        if (h.second) h.second();
    return texts(problems);
}

std::vector<std::string> watchedFiles() { return gWatched; }

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
    if (!data.isObject() || !data["format"].isString() || !format_marker::isFormat(data["format"].toString(), "exbit-skin")) {
        error = "Not an Exbit skin.";
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
          ": a skin of Exbit, made from a JSON skin of an earlier version. It is put on top of the program's\n"
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
                         ". It is put on top of Exbit's own\n"
                         "   default.css: change any variable or rule here, save, and Exbit shows it at once.\n"
                         "   docs/skins.md lists every variable, selector, part and property there is. */\n\n";
    std::string path = util::joinPath(userDir(), fileName(name));
    SkinFiles files;  // (its images, fonts and imports: into a folder of its own beside it)
    if (!s.path.empty()) {
        if (!planCopy(s.path, text, path, files, error)) return "";
        text = files.text;
    }
    util::makeDirs(userDir());
    if (!writeFiles(files, error)) return "";
    if (!util::writeFile(path, header + withName(withoutHeader(text), name))) {
        error = "Can't write " + path;
        return "";
    }
    return path;
}

bool openInEditor(const std::string& path) { return plat::openUrl(path); }

bool importFrom(const std::string& path, Skin& out, std::vector<std::string>& problems, std::string& error,
                const std::function<bool(const std::string& name)>& replace) {
    Skin s;
    std::string text;
    error.clear();
    if (!read(path, s, problems, error) || !util::readFile(path, text)) {
        if (error.empty()) error = "Can't read the file.";
        return false;
    }
    std::string target = util::joinPath(userDir(), fileName(s.name));
    if (util::absolutePath(target) != util::absolutePath(path)) {
        if (util::fileExists(target) && replace && !replace(s.name)) return false;  // (not replaced: error "")
        SkinFiles files;
        if (!planCopy(path, text, target, files, error)) return false;
        util::makeDirs(userDir());
        if (!writeFiles(files, error)) return false;
        if (!util::writeFile(target, files.text)) {
            error = "Can't write " + target;
            return false;
        }
        if (!read(target, s, problems, error)) return false;  // (what is left out: of the copy, as it is put on)
    }
    s.path = target;
    s.builtin = false;
    out = s;
    return true;
}

bool importSkin(ui::Window* parent, const std::function<void(const Skin&)>& choose) {
    std::string path = ui::files::openFile(parent, "Import skin", "", "Exbit skin (*.css)");
    if (path.empty()) return false;
    Skin s;
    std::vector<std::string> problems;
    std::string err;
    auto replace = [&](const std::string& name) {
        return ui::msg::question(parent, "Skin exists", "You already have a skin called " + name + ". Replace it?") ==
               ui::msg::Yes;
    };
    if (!importFrom(path, s, problems, err, replace)) {
        if (!err.empty()) ui::msg::warning(parent, "Cannot import skin", path + "\n\n" + err);
        return false;
    }
    if (choose) choose(s);
    if (!problems.empty()) {
        std::string list;
        for (size_t i = 0; i < problems.size() && i < 20; i++) list += (i ? "\n" : "") + problems[i];
        ui::msg::information(parent, "Skin imported",
                             s.name + " is in use. These parts of it were left out (the default stays):\n\n" + list);
    }
    return true;
}

bool exportTo(const Skin& s, const std::string& path, std::string& error) {
    std::string text = ":root {\n    --skin-name: " + quoted(s.name) + ";\n}\n";
    if (!s.path.empty() && !util::readFile(s.path, text)) {
        error = "Can't read " + s.path;
        return false;
    }
    if (!s.path.empty() && util::absolutePath(s.path) == util::absolutePath(path)) return true;  // (itself: there)
    SkinFiles files;
    if (!s.path.empty()) {
        if (!planCopy(s.path, text, path, files, error)) return false;
        text = files.text;
    }
    if (!util::dirName(path).empty()) util::makeDirs(util::dirName(path));
    if (!writeFiles(files, error)) return false;
    if (!util::writeFile(path, text)) {
        error = "Can't write " + path;
        return false;
    }
    return true;
}

std::string exportSkin(ui::Window* parent, const Skin& s) {
    std::string path = ui::files::saveFile(parent, "Export skin", fileName(s.name), "Exbit skin (*.css)");
    if (path.empty()) return "";
    if (util::extension(path) != ".css") path += ".css";
    std::string err;
    if (!exportTo(s, path, err)) {
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
