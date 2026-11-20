// The missing samples window (see relink_dialog.h).
#include "ui/relink_dialog.h"

#include "audio/file_io.h"
#include "core/util.h"
#include "ui/toolkit/layout.h"

RelinkDialog::RelinkDialog(ui::Window* parent, Project& project, int sr, std::vector<std::string> folders,
                           std::function<void(const std::string&)> changed)
    : ui::Dialog(parent, "Missing samples", 720, 420), project_(project), sr_(sr), folders_(std::move(folders)),
      changed_(std::move(changed)) {
    auto* lay = ui::styleRoom(ui::vbox(this, 10, 6), computedStyle());
    status = Widget::add<ui::Label>();
    status->setWordWrap(true);
    lay->addWidget(status);
    list = Widget::add<ui::ListWidget>();
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    list->currentRowChanged.connect([this](int row) { locateButton->setEnabled(row >= 0); });
    list->itemActivated.connect([this](int) { chooseFile(); });
    lay->addWidget(list, 1);
    auto* hint = Widget::add<ui::Label>(
        "Their clips stay on the timeline, silent and marked [missing]; saving keeps naming the files where they were "
        "expected, so nothing is lost by leaving them for now. The Explorer's folders are searched first.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto* buttons = Widget::add<ui::ButtonBox>(0);
    buttons->addButton("Search a folder\xe2\x80\xa6", 2)->clicked.connect([this] { chooseFolder(); });
    locateButton = buttons->addButton("Locate\xe2\x80\xa6", 2);
    locateButton->clicked.connect([this] { chooseFile(); });
    buttons->addButton("Leave missing", 0);
    buttons->rejected.connect([this] { reject(); });
    lay->addWidget(buttons);
    refresh();
}

std::string RelinkDialog::rowText(const relink::Missing& m) {
    std::string users;
    for (size_t i = 0; i < m.users.size() && i < 3; i++) users += (i ? ", " : "") + m.users[i];
    if (m.users.size() > 3) users += util::format(" and %zu more", m.users.size() - 3);
    return util::baseName(m.path) + "   used by " + users + "   expected in " + util::dirName(m.path);
}

void RelinkDialog::refresh() {
    missing = relink::missing(project_);
    int row = list->currentRow();
    list->clear();
    for (auto& m : missing) {
        ui::ListWidget::Item item;
        item.text = rowText(m);
        item.data = m.path;
        item.tooltip = m.path;
        list->addItem(item);
    }
    if (!missing.empty()) list->setCurrentRow(std::max(0, std::min(row, (int)missing.size() - 1)));
    locateButton->setEnabled(list->currentRow() >= 0);
    status->setText(missing.empty() ? std::string("Every sample this project uses is where it should be.")
                                    : util::format("%zu sample file%s this project uses %s not where it expects:",
                                                   missing.size(), missing.size() == 1 ? "" : "s",
                                                   missing.size() == 1 ? "is" : "are"));
}

int RelinkDialog::put(relink::Found& found) {
    std::string error;
    int in = relink::apply(project_, found, sr_, error);
    size_t before = missing.size();
    refresh();
    std::string text = util::format("Found %d of %zu missing sample file%s", in, before, before == 1 ? "" : "s");
    if (changed_) changed_(text);
    if (!error.empty()) ui::msg::warning(this, "Missing samples", "These files were found but can't be read:\n\n" + error);
    if (missing.empty() && isRunning()) accept();
    return in;
}

int RelinkDialog::searchFolder(const std::string& folder) {
    std::vector<std::string> where = folders_;
    where.push_back(folder);
    ui::ProgressDialog busy(this, "Missing samples", "Looking for the missing samples\xe2\x80\xa6", true);
    busy.setRange(0, 0);
    double shownFrom = ui::App::get().now() + 0.3;
    relink::Found found;
    relink::search(missing, where, found, [&](size_t seen) {
        busy.setLabel(util::format("Looking for the missing samples\xe2\x80\xa6 (%zu files looked at)", seen));
        if (busy.isWindowShown() || ui::App::get().now() >= shownFrom) busy.setValue(0);
        else ui::App::get().processEvents();
        return !busy.wasCanceled();
    });
    if (busy.isWindowShown()) busy.hideWindow();
    return put(found);
}

int RelinkDialog::locate(int row, const std::string& file) {
    if (row < 0 || row >= (int)missing.size() || file.empty()) return 0;
    relink::Found found{{missing[(size_t)row].path, util::absolutePath(file)}};
    relink::alongside(missing, found);  // (a folder moved: the others where it went)
    return put(found);
}

void RelinkDialog::chooseFolder() {
    std::string start;
    for (auto& m : missing) {  // (the nearest folder still there above where the first one was)
        for (std::string d = util::dirName(m.path); !d.empty() && d != "/"; d = util::dirName(d))
            if (util::isDir(d)) {
                start = d;
                break;
            }
        break;
    }
    std::string folder = ui::files::folder(this, "Search a folder for the missing samples", start);
    if (!folder.empty()) searchFolder(folder);
}

void RelinkDialog::chooseFile() {
    int row = list->currentRow();
    if (row < 0 || row >= (int)missing.size()) return;
    const relink::Missing& m = missing[(size_t)row];
    std::string start;
    for (std::string d = util::dirName(m.path); !d.empty() && d != "/"; d = util::dirName(d))
        if (util::isDir(d)) {
            start = d;
            break;
        }
    std::string patterns;
    for (const std::string& e : file_io::AUDIO_EXTENSIONS) patterns += (patterns.empty() ? "*" : " *") + e;
    std::string file = ui::files::openFile(this, "Where is " + util::baseName(m.path) + "?", start,
                                           "Audio files (" + patterns + ");;All files (*)");
    if (!file.empty()) locate(row, file);
}
