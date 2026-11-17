#include "ui/history_window.h"

#include "ui/theme.h"
#include "ui/toolkit/layout.h"

const char* const HistoryWindow::START = "Opened";

HistoryWindow::HistoryWindow(ui::Window* parent, const history::History& history,
                             std::function<void(int position)> jump)
    : Dialog(parent, "History", 300, 360), history_(history), jump_(std::move(jump)) {
    ui::VBox* box = ui::vbox(this, 11, 6);
    list = add<ui::ListWidget>();
    list->setToolTip("Every change to the project, oldest at the top. Click one to go back (or forward) to just "
                     "after it; the greyed ones are what Redo would bring back.");
    list->itemClicked.connect([this](int row) { clicked(row); });
    list->itemActivated.connect([this](int row) { clicked(row); });  // (Return, or a double-click)
    box->addWidget(list, 1);
    auto* bb = add<ui::ButtonBox>(ui::ButtonBox::Close);
    bb->rejected.connect([this] { reject(); });
    box->addWidget(bb);
    setMinimumSize(240, 200);
    rebuild();
}

void HistoryWindow::refresh() {
    if (history_.changes() != shownChanges_) rebuild();
}

void HistoryWindow::rebuild() {
    shownChanges_ = history_.changes();
    std::vector<std::string> labels = history_.labels();
    int at = history_.position();
    list->clear();
    for (int row = 0; row <= (int)labels.size(); row++) {
        ui::ListWidget::Item it;
        it.text = row == 0 ? START : labels[(size_t)row - 1];
        it.data = std::to_string(row);
        it.bold = row == at;
        if (row > at) {  // (undone: what Redo would bring back)
            it.hasColor = true;
            it.color = theme::c("disabled_text");
        }
        list->addItem(std::move(it));
    }
    list->setCurrentRow(at);  // (highlighted)
    list->scrollToRow(at);
}

void HistoryWindow::clicked(int row) {
    if (row < 0 || row == history_.position()) return;
    if (jump_) jump_(row);
    refresh();  // (the jump changed the history's position; also when it was refused, the highlight goes back)
    if (history_.changes() == shownChanges_ && list->currentRow() != history_.position())
        list->setCurrentRow(history_.position());
}
