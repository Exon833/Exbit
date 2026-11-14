// Recent projects and templates: the menus and what needs the main window (templates.h).
#include <map>

#include "core/util.h"
#include "model/project_io.h"
#include "ui/autosave.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/skin.h"
#include "ui/templates.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"

namespace templates {

void fillRecentMenu(ui::Menu* menu, MainWindow& window) {
    menu->clear();
    std::vector<std::string> found = recent(window.prefs);
    MainWindow* w = &window;
    for (auto& path : found) {
        ui::Action* a = menu->addAction(util::stem(path) + "\t" + util::dirName(path), [w, path] { w->openRecent(path); });
        a->tooltip = path;
    }
    if (found.empty()) menu->addAction("No recent projects yet")->enabled = false;
    menu->addSeparator();
    ui::Action* clear = menu->addAction("Clear the list", [w] { clearRecent(w->prefs); });
    clear->enabled = !window.prefs.recentProjects.empty();
}

void fillTemplateMenu(ui::Menu* menu, MainWindow& window) {
    menu->clear();
    std::vector<Template> found = list();
    MainWindow* w = &window;
    for (auto& t : found) {
        std::string path = t.path;
        menu->addAction(t.name + (t.own ? "" : "  (yours)"), [w, path] { w->newFromTemplate(path); });
    }
    if (found.empty()) menu->addAction("No templates")->enabled = false;
    menu->addSeparator();
    menu->addAction("Open the templates folder", [] { openFolder(); });
}

void fillDefaultMenu(ui::Menu* menu, MainWindow& window) {
    menu->clear();
    const std::string& current = window.prefs.defaultTemplate;
    MainWindow* w = &window;
    ui::Action* empty = menu->addCheckable("An empty project", current.empty() || !util::fileExists(current));
    empty->triggered = [w] { setDefault(*w, ""); };
    empty->group = 1;
    for (auto& t : list()) {
        std::string path = t.path;
        bool on = !current.empty() && util::absolutePath(path) == util::absolutePath(current);
        ui::Action* a = menu->addCheckable(t.name + (t.own ? "" : "  (yours)"), on);
        a->triggered = [w, path] { setDefault(*w, path); };
        a->group = 1;
    }
}

void setDefault(MainWindow& window, const std::string& path) {
    window.prefs.defaultTemplate = path;
    window.prefs.save();
    std::string name = path.empty() ? "an empty project" : util::stem(path);
    window.showStatus("New projects start from " + name + " (File → New)", 4000);
}

std::string writeTemplate(MainWindow& window, const std::string& name, std::string& error) {
    std::string path = util::joinPath(userDir(), skin::fileName(name));
    if (util::fileExists(path)) {  // (its old audio goes with it)
        util::removeFile(path);
        std::string audio = project_io::audioFolder(path);
        if (util::isDir(audio)) autosave::removeTree(audio);
    }
    util::makeDirs(userDir());
    std::vector<ClipPtr> holders = project_io::audioHolders(window.project);
    std::map<const Clip*, std::string> kept;  // (saveProject points clips at the files it wrote: undo that)
    for (auto& c : holders) kept[c.get()] = c->sourcePath;
    bool ok = project_io::saveProject(path, window.project, window.engine.sr, window.master->settings(), error);
    for (auto& c : holders) c->sourcePath = kept[c.get()];
    return ok ? path : "";
}

std::string saveAsTemplate(MainWindow& window) {
    std::string name = window.projectPath.empty() ? "My template" : util::stem(window.projectPath);
    if (!ui::getText(&window, "Save as template", "Name of the template:", name)) return "";
    name = util::trim(name);
    if (name.empty()) return "";
    std::string path = util::joinPath(userDir(), skin::fileName(name));
    if (util::fileExists(path) &&
        ui::msg::question(&window, "Save as template",
                          "There is a template called " + name + " already. Replace it?") != ui::msg::Yes)
        return "";
    std::string error;
    std::string saved = writeTemplate(window, name, error);
    if (saved.empty()) {
        ui::msg::warning(&window, "Cannot save the template", path + "\n\n" + error);
        return "";
    }
    window.showStatus("Saved the template " + name + " (File → New from template)", 5000);
    return saved;
}

}  // namespace templates
