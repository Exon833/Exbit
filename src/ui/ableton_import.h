// File → Import Ableton Live set…: an .als turned into a Exbit project (importers/ableton.h), saved where
// the user picks, then opened, with a list of what was approximated or left out (ui/ableton_import.py).
//
// The main window's menu entry ("&Import Ableton Live set…", after "Open a &backup…") calls
//
//     ableton_import::run(this, {[this] { return confirmDiscard(); },
//                                [this] { stop(); },
//                                [this](const std::string& path) { return openProjectFile(path); }});
//
// The flow: ask to save the current project (confirmDiscard), choose the .als, then where to save the new
// project (proposed: "<set's parent folder>/<stem> (Exbit)/<stem>.json" when that folder is there, else
// "<set's parent folder>/<stem> (Exbit).json"); its audio folder must be empty or new (else a warning and
// nothing happens). Playback stops, a progress window follows the conversion (no Cancel: it can't stop
// half-way), then the saved project is opened and a message tells what came over ("Show details": what was
// approximated or left out). Errors (a sample missing from the set's folder, a damaged file…) are shown in a
// warning naming the set; the current project stays open then.
#pragma once
#include <functional>
#include <string>

namespace ui { class Window; }

namespace ableton_import {

struct Hooks {
    std::function<bool()> confirmDiscard;                     // MainWindow::confirmDiscard: false = the user cancelled
    std::function<void()> stop;                               // MainWindow::stop (playback and recording)
    std::function<bool(const std::string&)> openProjectFile;  // MainWindow::openProjectFile: open the saved project
};

// The whole flow (import_ableton); true if a set was imported and opened. GUI thread.
bool run(ui::Window* parent, const Hooks& hooks);
inline bool importAbleton(ui::Window* parent, const Hooks& hooks) { return run(parent, hooks); }

// the project file proposed for a set (see the top)
std::string proposedOutput(const std::string& als);

}  // namespace ableton_import
