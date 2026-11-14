// File → Import Ableton Live set… (ui/ableton_import.py; see ableton_import.h).
#include "ui/ableton_import.h"

#include <cmath>

#include "core/util.h"
#include "importers/ableton.h"
#include "model/project_io.h"
#include "ui/toolkit/dialogs.h"

namespace ableton_import {

namespace {
const char* const TITLE = "Import Ableton Live set";
}

std::string proposedOutput(const std::string& als) {
    std::string stem = util::stem(als);
    std::string parent = util::dirName(util::dirName(util::absolutePath(als)));
    std::string folder = util::joinPath(parent, stem + " (Tiny DAW)");
    // (the file chooser starts in a folder that is there: a new one can't be proposed inside the path)
    if (util::isDir(folder)) return util::joinPath(folder, stem + ".json");
    return util::joinPath(parent, stem + " (Tiny DAW).json");
}

bool run(ui::Window* window, const Hooks& hooks) {
    if (hooks.confirmDiscard && !hooks.confirmDiscard()) return false;
    std::string als = ui::files::openFile(window, TITLE, "", "Ableton Live set (*.als)");
    if (als.empty()) return false;
    std::string output = ui::files::saveFile(window, "Save the Tiny DAW project as", proposedOutput(als),
                                             "Tiny DAW project (*.json)");
    if (output.empty()) return false;
    if (!util::endsWith(util::lower(output), ".json")) output += ".json";
    std::string folder = project_io::audioFolder(output);
    if (util::isDir(folder) && !util::listDir(folder).empty()) {
        ui::msg::warning(window, TITLE, folder + " already holds files: save the project somewhere new (an empty folder).");
        return false;
    }
    std::string dir = util::dirName(util::absolutePath(output));
    if (!util::isDir(dir)) util::makeDirs(dir);
    if (hooks.stop) hooks.stop();

    Project project;
    ableton::Result result;
    std::string error;
    bool ok;
    {
        ui::ProgressDialog dialog(window, TITLE, "Reading the set\xE2\x80\xA6", false);  // (no Cancel: it can't stop half-way)
        dialog.setRange(0, 100);
        dialog.setValue(0);
        auto progress = [&](const std::string& text, double share) {
            dialog.setLabel(text + "\xE2\x80\xA6");
            dialog.setValue((int)std::nearbyint(share * 100));  // (at 100, done: it goes)
        };
        ok = ableton::convert(als, output, project, result, error, progress);
        if (dialog.isWindowShown()) dialog.hideWindow();
    }
    if (!ok) {
        ui::msg::warning(window, "Cannot import the set", als + "\n\n" + error);
        return false;
    }
    project.clear();  // (the main window opens the saved file)
    if (hooks.openProjectFile && !hooks.openProjectFile(output)) return false;
    std::string details;
    for (size_t i = 1; i < result.report.size(); i++) details += (i > 1 ? "\n" : "") + result.report[i];
    if (details.empty()) details = "Everything carried over.";
    ui::msg::box(window, ui::msg::Icon::Information, "Ableton Live set imported",
                 "Saved as " + output + ".\n\n" + (result.report.empty() ? std::string() : result.report[0]) +
                     "\n\nInstruments Tiny DAW can't play are frozen to a rendering made from the set; see the details for "
                     "what was approximated or left out.",
                 ui::msg::Ok, ui::msg::Ok, details);
    return true;
}

}  // namespace ableton_import
