// New Wine prefixes and plugin installers, with their windows (see wine_setup.h).
#include "ui/wine_setup.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/install_log.h"
#include "plugins/wine.h"
#include "plugins/wine_prefix_fixes.h"
#include "ui/toolkit/dialogs.h"
#include "ui/wine_install_steps.h"
#include "ui/toolkit/widget.h"

namespace wine_setup {

std::string prefixesHome() {
    const char* x = getenv("XDG_DATA_HOME");
    std::string data = x && *x ? std::string(x) : util::joinPath(plat::homeDir(), ".local/share");
    return util::joinPath(data, "wineprefixes");
}

bool validName(const std::string& name) {
    return !name.empty() && name[0] != '.' && name.find('/') == std::string::npos && name == util::trim(name);
}

bool waitFor(ui::Window* parent, int pid, const std::string& title, const std::string& label, int& code) {
    ui::ProgressDialog dialog(parent, title, label, true);
    dialog.setRange(0, 0);  // (busy: how long it takes isn't known)
    for (;;) {
        if (wine::finished(pid, code)) {
            if (dialog.isWindowShown()) dialog.hideWindow();
            return true;
        }
        if (dialog.wasCanceled()) {
            wine::stop(pid);
            if (dialog.isWindowShown()) dialog.hideWindow();
            return false;
        }
        dialog.setValue(0);  // (repaints and handles what is pending)
        usleep(20000);
    }
}

std::string newPrefix(ui::Window* parent) {
    const std::string title = "New Wine prefix";
    std::string home = prefixesHome(), name = "Plugins", dir;
    for (int n = 2; util::fileExists(util::joinPath(home, name)); n++) name = "Plugins " + std::to_string(n);
    for (;;) {
        if (!ui::getText(parent, title, "Name of the new prefix (it is made in " + home + "):", name)) return "";
        name = util::trim(name);
        if (!validName(name)) {
            ui::msg::warning(parent, title, "A prefix's name can't be empty, start with a dot or hold a '/'.");
            continue;
        }
        dir = util::joinPath(home, name);
        if (util::fileExists(dir)) {
            ui::msg::warning(parent, title, dir + " already exists: choose another name.");
            continue;
        }
        break;
    }
    std::string wine = wine::winePath(), why;
    int pid = wine::startPrefix(dir, wine, why);
    if (pid < 0) {
        ui::msg::warning(parent, title, "Cannot make the prefix.\n\n" + why);
        return "";
    }
    int code = 0;
    if (!waitFor(parent, pid, title, "Making " + dir + "…\n\nWine sets up its Windows there (about half a minute).",
                 code)) {
        ui::msg::information(parent, title, "Stopped. What was made so far is in " + dir + " (delete it, or make "
                                            "the prefix again with another name).");
        return "";
    }
    // its registry (system.reg, which makes it a prefix) is written as its wineserver ends
    int server = wine::startServerWait(dir, wine, why);
    if (server >= 0) waitFor(parent, server, title, "Finishing " + dir + "…", code);
    if (!wine::isPrefix(dir)) {
        ui::msg::warning(parent, title, "Wine didn't finish making the prefix in " + dir + "." +
                                            (why.empty() ? "" : "\n\n" + why));
        return "";
    }
    // Exbit's fixes for what some installers need of Wine (wine_prefix_fixes.h); a prefix without them still works
    if (!wine::fixesFolder().empty()) applyFixesTo(parent, dir, true);
    return dir;
}

bool installPlugin(ui::Window* parent, const std::vector<std::string>& prefixes, std::string& prefixUsed,
                   std::string& madePrefix, std::string* endNote, std::string* logUsed) {
    const std::string title = "Install Windows plugin";
    prefixUsed.clear();
    madePrefix.clear();
    std::vector<std::string> usable;
    for (const auto& p : prefixes)
        if (wine::isPrefix(p)) usable.push_back(p);
    std::string downloads = util::joinPath(plat::homeDir(), "Downloads");
    std::string installer = ui::files::openFile(parent, "Windows plugin installer",
                                                util::isDir(downloads) ? downloads : plat::homeDir(),
                                                "Windows installers (*.exe *.msi);;All files (*)");
    if (installer.empty()) return false;
    std::string prefix;
    if (usable.empty()) {
        if (ui::msg::question(parent, title,
                              "There is no Wine prefix for Windows plugins yet (Options → Wine prefixes). Make a new "
                              "one and install the plugin there?") != ui::msg::Yes)
            return false;
        prefix = newPrefix(parent);
        if (prefix.empty()) return false;
        madePrefix = prefix;
    } else if (usable.size() == 1) {
        prefix = usable[0];
    } else {
        int index = 0;
        if (!ui::getItem(parent, title, "Install " + util::baseName(installer) + " in which Wine prefix?", usable,
                              index))
            return false;
        prefix = usable[std::clamp(index, 0, (int)usable.size() - 1)];
    }
    if (madePrefix.empty()) offerFixes(parent, prefix);
    // its log: what Wine and the installer print (the newest few kept)
    std::string logs = install_log::folder(), logFile;
    if (!logs.empty()) {
        install_log::rotate(logs, install_log::KEPT_LOGS - 1);
        logFile = install_log::newLogPath(logs, installer, (long long)util::wallClock());
    }
    std::string why;
    int pid = wine::startInstaller(prefix, installer, why, logFile);
    if (pid < 0) {
        ui::msg::warning(parent, title, "Cannot run the installer.\n\n" + why);
        return false;
    }
    prefixUsed = prefix;
    InstallEnd end = waitForInstaller(parent, pid, prefix, installer, logFile);
    if (end.endedAsHung) reportHungInstall(parent, installer, logFile);
    if (endNote) *endNote = installEndNote(end, logFile);
    if (logUsed) *logUsed = logFile;
    return end.ended || end.endedAsHung;
}

std::set<std::string> vst2Dlls(const std::vector<std::string>& prefixes) {
    std::set<std::string> out;
    for (const auto& folder : wine::vst2UsualFolders(prefixes))
        util::walkFiles(folder, [&](const std::string& path) {
            if (util::extension(path) == ".dll") out.insert(path);
            return true;
        });
    return out;
}

std::vector<std::string> newVst2Folders(const std::set<std::string>& before, const std::set<std::string>& after,
                                        const std::vector<std::string>& chosen) {
    auto inside = [](const std::string& path, const std::string& folder) {
        return path == folder || util::startsWith(path, folder + "/");
    };
    std::vector<std::string> out;
    for (const auto& dll : after) {
        if (before.count(dll)) continue;
        if (std::any_of(chosen.begin(), chosen.end(), [&](const std::string& c) { return inside(dll, c); })) continue;
        // (the usual folder it is in, which the scan searches with its subfolders)
        for (const auto& usual : wine::vst2UsualFolders({wine::prefixOf(dll)}))
            if (inside(dll, usual) && std::find(out.begin(), out.end(), usual) == out.end()) out.push_back(usual);
    }
    return out;
}

}  // namespace wine_setup
