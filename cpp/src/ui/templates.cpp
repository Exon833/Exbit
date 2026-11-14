// Recent projects and project templates (templates.py). See templates.h.
#include "ui/templates.h"

#include <map>

#include "core/json.h"
#include "core/util.h"
#include "paths.h"
#include "platform/platform.h"
#include "ui/autosave.h"

namespace templates {

namespace {
std::string gProgramDir, gUserDir;

bool inside(const std::string& path, const std::string& dir) {
    if (dir.empty()) return false;
    std::string d = util::absolutePath(dir);
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return util::startsWith(util::absolutePath(path), d + "/");
}
}  // namespace

std::string programDir() { return gProgramDir.empty() ? util::joinPath(plat::dataDir(), "templates") : gProgramDir; }
std::string userDir() { return gUserDir.empty() ? paths::dataDir("templates") : gUserDir; }

void setDirs(const std::string& program, const std::string& user) {
    gProgramDir = program;
    gUserDir = user;
}

// --- recent projects ---

bool keptOut(const std::string& path) {
    std::string folder = util::dirName(util::absolutePath(path));
    return inside(path, autosave::recoveryDir()) || inside(path, userDir()) || inside(path, programDir()) ||
           util::endsWith(folder, "_backups");
}

std::vector<std::string> recent(const Preferences& prefs, bool allOfThem) {
    const std::string& text = prefs.recentProjects;
    std::vector<std::string> found;
    bool parsed = false;
    if (util::startsWith(text, "[")) {  // (a JSON list: any character may be in a path)
        std::string error;
        Json list = Json::parse(text, &error);
        if (error.empty() && list.isArray()) {
            parsed = true;
            for (auto& item : list.items())
                if (item.isString() && !item.toString().empty()) found.push_back(item.toString());
        }
    }
    if (!parsed)
        for (auto& p : util::split(text, '|'))
            if (!p.empty()) found.push_back(p);
    if (allOfThem) return found;
    std::vector<std::string> there;
    for (auto& p : found)
        if (util::fileExists(p) && !util::isDir(p)) there.push_back(p);
    return there;
}

void remember(Preferences& prefs, const std::string& pathIn) {
    if (pathIn.empty() || keptOut(pathIn)) return;
    std::string path = util::absolutePath(pathIn);
    Json list = Json::array();
    list.push(path);
    int n = 1;
    for (auto& p : recent(prefs, true)) {
        if (p == path) continue;
        if (n++ >= RECENT_KEPT) break;
        list.push(p);
    }
    prefs.recentProjects = list.dump();
    prefs.save();
}

void clearRecent(Preferences& prefs) {
    prefs.recentProjects.clear();
    prefs.save();
}

// --- templates ---

std::vector<Template> list() {
    std::vector<Template> found;
    for (int own = 1; own >= 0; --own) {
        std::string folder = own ? programDir() : userDir();
        if (!util::isDir(folder)) continue;
        std::vector<std::string> names;
        for (auto& e : util::listDir(folder))
            if (!e.isDir && util::extension(e.name) == ".json") names.push_back(e.name);
        std::sort(names.begin(), names.end());
        for (auto& n : names) found.push_back({util::stem(n), util::joinPath(folder, n), own == 1});
    }
    return found;
}

std::string defaultTemplate(const Preferences& prefs) {
    const std::string& path = prefs.defaultTemplate;
    return !path.empty() && util::fileExists(path) && !util::isDir(path) ? path : "";
}

void openFolder() {
    util::makeDirs(userDir());
    plat::openFolder(userDir());
}

}  // namespace templates
