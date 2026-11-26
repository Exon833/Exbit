// A message box whose text holds a word wider than a wrapped label's usual width (a long path, a log line) is as
// tall as its text: no empty band between the text and the buttons. (Dialog::adjustSize laid the dialog out once,
// with the labels' heights counted at their usual wrap width; laid out wider, they took fewer lines than counted.)
#include <functional>
#include <string>

#include "check.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

using namespace ui;

// shows a message box, and returns its height and the least height its layout needs
static void measureMessageBox(const std::string& text, const std::string& details, int& height, int& leastHeight) {
    App& app = App::get();
    height = leastHeight = -1;
    app.singleShot(30, [&] {
        Window* top = app.modalWindow();
        if (!top) return;
        app.doLayouts();
        height = top->height();
        leastHeight = top->leastSize().y;
        if (auto* dialog = dynamic_cast<Dialog*>(top)) dialog->reject();
    });
    msg::box(nullptr, msg::Icon::Warning, "Install Windows plugin", text, msg::Ok, msg::Ok, details);
}

int main() {
    App::get().initHeadless();
    const std::string longPath = "/home/someone/.local/share/exbit/install-logs/2026-11-26_19-47-52_a_long_installer_"
                                 "name_without_any_spaces_in_it.log";
    std::string text = "The installer ended with exit status 1603. What Wine and the installer printed is in its log:\n" +
                       longPath + "\n\nIts last lines are shown below.";
    std::string details;
    for (int line = 0; line < 12; ++line) details += "0024:fixme:msi:MSI_ViewExecute " + std::string(150, 'x') + "\n";
    int height = 0, leastHeight = 0;
    // a long path in the text: the box is as tall as its parts (not taller)
    measureMessageBox(text, "", height, leastHeight);
    CHECK(height > 0 && height == leastHeight);
    // and long log lines in its details
    measureMessageBox(text, details, height, leastHeight);
    CHECK(height > 0 && height == leastHeight);
    // a short text still fits (no change there)
    measureMessageBox("Save the changes to the current project?", "", height, leastHeight);
    CHECK(height > 0 && height == leastHeight);
    TEST_MAIN_END();
}
