// The File menu's project safety (see project_safety.h).
#include "ui/project_safety.h"

#include "audio/plugin_audio.h"
#include "core/util.h"
#include "model/collect.h"
#include "model/relink.h"
#include "ui/explorer.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/relink_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"

namespace project_safety {

std::function<void(int done, int total)> progressIn(ui::ProgressDialog& dialog) {
    double shownFrom = ui::App::get().now() + 0.3;  // (QProgressDialog's minimum duration: 300 ms)
    return [&dialog, shownFrom](int done, int total) {
        dialog.setRange(0, std::max(1, total));
        if (dialog.isWindowShown() || ui::App::get().now() >= shownFrom) dialog.setValue(std::min(done, std::max(1, total) - 1));
        else ui::App::get().processEvents();
    };
}

namespace {
void finished(ui::ProgressDialog& dialog) {
    if (dialog.isWindowShown()) dialog.hideWindow();
}
}  // namespace

bool collectAndSave(MainWindow& w) {
    std::string path = w.projectPath;
    if (path.empty()) {
        path = ui::files::saveFile(&w, "Collect and save", "Untitled.json", "Exbit project (*.json)");
        if (path.empty()) return false;
        if (util::extension(path) != ".json") path += ".json";
    }
    std::set<std::string> before = collect::inFolder(path, w.project);
    bool ok;
    {
        ui::ProgressDialog busy(&w, "Collect and save", "Copying the samples into the project's folder\xe2\x80\xa6", false);
        ok = w.saveProjectFile(path, progressIn(busy));
        finished(busy);
    }
    if (!ok) return false;
    collect::Summary s = collect::summary(path, w.project, before);
    ui::msg::information(&w, "Collect and save", collect::describe(s));
    w.showStatus(util::format("Collected %d files in %s", s.files, s.folder.c_str()), 8000);
    return true;
}

bool saveArchive(MainWindow& w) {
    std::string start = w.projectPath.empty() ? std::string("Untitled")
                                              : util::joinPath(util::dirName(w.projectPath), util::stem(w.projectPath));
    std::string path = ui::files::saveFile(&w, "Save as archive", start + collect::ARCHIVE_EXTENSION,
                                           "Exbit archive (*.exbit.zip)");
    if (path.empty()) return false;
    if (!collect::isArchive(path)) path += collect::ARCHIVE_EXTENSION;
    if (!w.engine.isOpen())  // (as saving: the plugins' settings as they are now)
        for (auto& slot : w.project.pluginSlots()) plugin_audio::syncState(*slot, w.engine.sr);
    std::string error;
    bool ok;
    {
        ui::ProgressDialog busy(&w, "Save as archive", "Packing the project and its samples\xe2\x80\xa6", false);
        ok = collect::saveArchive(path, w.project, w.engine.sr, w.master->settings(), error, progressIn(busy));
        finished(busy);
    }
    if (!ok) {
        ui::msg::warning(&w, "Cannot save the archive", path + "\n\n" + error);
        return false;
    }
    double mb = (double)std::max<int64_t>(0, util::fileSize(path)) / (1024.0 * 1024.0);
    w.showStatus("Saved the archive " + path + " (" + util::fixed(mb, 1) + " MB)", 8000);
    return true;
}

bool openArchive(MainWindow& w, const std::string& zipPath) {
    std::string folder = collect::defaultFolder(zipPath);
    if (!ui::getText(&w, "Open archive", "Unpack the project into this new folder:", folder)) return false;
    folder = util::absolutePath(util::expandUser(util::trim(folder)));
    if (folder.empty()) return false;
    std::string json, error;
    bool ok;
    {
        ui::ProgressDialog busy(&w, "Open archive", "Unpacking the project\xe2\x80\xa6", false);
        ok = collect::openArchive(zipPath, folder, json, error, progressIn(busy));
        finished(busy);
    }
    if (!ok) {
        ui::msg::warning(&w, "Cannot open the archive", zipPath + "\n\n" + error);
        return false;
    }
    return w.openProjectFile(json);
}

void showMissing(MainWindow& w, bool always) {
    if (relink::missing(w.project).empty()) {
        if (always) ui::msg::information(&w, "Missing samples", "Every sample this project uses is where it should be.");
        return;
    }
    RelinkDialog dialog(&w, w.project, w.engine.sr, explorer::loadFolders(), [&w](const std::string& text) {
        w.timeline->relayout();
        w.timeline->update();
        w.showStatus(text, 8000);
    });
    dialog.exec();
}

}  // namespace project_safety
