// The File menu's project safety (model/collect.h, model/relink.h), for the main window:
//
//   Collect and save        saves the project (asking where the first time), every sample it uses copied into its
//                           audio folder if it isn't there yet, then says how many files and how much that is
//   Save as archive…        the project and its samples as one .tinydaw.zip (the project stays as it is)
//   Open project…           also opens a .tinydaw.zip: unpacked into a folder chosen (beside it, named after it, to
//                           start with), then opened from there
//   Find missing samples…   the missing samples window (ui/relink_dialog.h), also shown when a project opens with
//                           samples missing
//
// Long work shows a progress window (after 0.3 s). In the terminal UI these are the same menus and windows.
#pragma once
#include <functional>
#include <memory>
#include <string>

class MainWindow;
namespace ui {
class ProgressDialog;
class Window;
}  // namespace ui

namespace project_safety {

bool collectAndSave(MainWindow& window);
bool saveArchive(MainWindow& window);
// the archive unpacked (asking where) and its project opened
bool openArchive(MainWindow& window, const std::string& zipPath);
// the missing samples window, if any are missing (`always`: also to say none are)
void showMissing(MainWindow& window, bool always = false);

// progress(done, total) shown in `dialog` once it has taken 0.3 s (the window drawn meanwhile)
std::function<void(int done, int total)> progressIn(ui::ProgressDialog& dialog);

}  // namespace project_safety
