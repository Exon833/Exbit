// The History window (Edit → History…): every undo step by name, oldest at the top.
//
// The first row is the project as it was opened (or as the history started); each row below is a
// step (history.h NAMES: "Move clip", "Add track", "Volume"…). The row the project is at now is
// highlighted; the steps after it (the ones Redo would bring back) are greyed. Clicking a row undoes
// or redoes as many steps as it takes to get there. It doesn't block: the project can be edited while
// it is open, and it follows (refresh(), called by the main window as it notes changes).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/history.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class HistoryWindow : public ui::Dialog {
public:
    // jump(position): take the project to that place in the history (0 = as opened, k = after step k)
    HistoryWindow(ui::Window* parent, const history::History& history, std::function<void(int position)> jump);
    // show the history as it is now (only rebuilt when it changed)
    void refresh();
    ui::ListWidget* list = nullptr;
    static const char* const START;  // the first row's text: "Opened"

private:
    const history::History& history_;
    std::function<void(int)> jump_;
    int shownChanges_ = -1;  // history.changes() when last shown
    void rebuild();
    void clicked(int row);
};
