// The missing samples window (model/relink.h): shown when a project opens with sample files that aren't where
// it names them, and from File → Find missing samples….
//
// A row per file: its name, the folder it was expected in, and what uses it (clips, takes, samplers, pads).
// "Search a folder…" looks through a folder and its subfolders, the Explorer's folders first, and puts in
// every file it finds (by name, then renamed ones by size and sound; a moved folder found as a whole);
// "Locate…" picks the selected row's file by hand (the others are then tried beside it); "Leave missing"
// closes it: the clips stay silent, marked "[missing]", and saving keeps naming the files where they were
// expected, so nothing is lost. When none are left it closes by itself. In the terminal UI it is the same
// window, drawn as text (tui/window_view.h).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/relink.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class RelinkDialog : public ui::Dialog {
public:
    // `folders`: searched before the one chosen (the Explorer's); `changed(text)`: after files were put in (the
    // timeline redrawn, the status bar told)
    RelinkDialog(ui::Window* parent, Project& project, int sr, std::vector<std::string> folders,
                 std::function<void(const std::string& text)> changed);

    void refresh();                       // the list as the project is now
    int searchFolder(const std::string& folder);   // the search ("Search a folder…" once chosen): files put in
    int locate(int row, const std::string& file);  // "Locate…" once chosen: files put in (others beside it too)
    void chooseFolder();                  // the buttons
    void chooseFile();
    static std::string rowText(const relink::Missing& m);

    std::vector<relink::Missing> missing;
    ui::ListWidget* list;
    ui::Label* status;
    ui::Button* locateButton;

private:
    Project& project_;
    int sr_;
    std::vector<std::string> folders_;
    std::function<void(const std::string&)> changed_;
    int put(relink::Found& found);  // apply() and tell
};
