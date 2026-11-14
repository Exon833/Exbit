#include "ui/explorer.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "audio/file_io.h"
#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

namespace explorer {

const char* const FOLDERS_KEY = "explorer/folders";
const char* const PREVIEW_KEY = "explorer/preview";
const char* const LOOP_KEY = "explorer/loop";

std::vector<std::string> loadFolders() {
    Json stored = settings::get(FOLDERS_KEY);
    std::vector<std::string> out;
    if (stored.isNull()) {  // first run: the Music folder, if there is one
        std::string music = util::joinPath(util::expandUser("~"), "Music");
        if (util::isDir(music)) out.push_back(music);
        return out;
    }
    std::vector<std::string> all;
    if (stored.isString()) all.push_back(stored.toString());  // (a one-item list stored as a plain string)
    for (const Json& j : stored.items())
        if (j.isString()) all.push_back(j.toString());
    for (auto& f : all)
        if (util::isDir(f)) out.push_back(f);
    return out;
}

void saveFolders(const std::vector<std::string>& folders) {
    std::vector<Json> items;
    for (auto& f : folders) items.push_back(Json(f));
    settings::set(FOLDERS_KEY, Json::array(items));
}

namespace {

// os.walk (top-down, not into symlinked folders), hidden folders skipped; false: stop
bool walk(const std::string& dir, std::vector<Indexed>& found, size_t limit, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return false;
    DIR* d = opendir(dir.c_str());
    if (!d) return true;
    std::vector<std::string> subdirs;
    while (dirent* e = readdir(d)) {
        const char* name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        std::string path = util::joinPath(dir, name);
        bool isDir = false, isLink = false;
        if (e->d_type == DT_DIR) {
            isDir = true;
        } else if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
                isLink = true;
                isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            } else {
                isDir = S_ISDIR(st.st_mode);
            }
        }
        if (isDir) {
            if (name[0] != '.' && !isLink) subdirs.push_back(path);  // (os.walk doesn't follow links)
        } else if (file_io::isSample(name)) {
            found.push_back({util::lower(name), path});
        }
    }
    closedir(d);
    if (found.size() >= limit) return false;
    std::sort(subdirs.begin(), subdirs.end());
    for (auto& s : subdirs)
        if (!walk(s, found, limit, cancel)) return false;
    return true;
}

size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

}  // namespace

std::vector<Indexed> indexFolders(const std::vector<std::string>& folders, size_t limit, const std::atomic<bool>* cancel) {
    std::vector<Indexed> found;
    for (auto& folder : folders)
        if (!walk(folder, found, limit, cancel)) break;
    if (found.size() > limit) found.resize(limit);
    return found;
}

std::vector<std::string> search(const std::vector<Indexed>& index, const std::string& text, size_t limit,
                                size_t& total) {
    std::vector<std::string> words = util::splitWords(util::lower(text));
    std::vector<const Indexed*> hits;
    for (const Indexed& entry : index) {
        bool all = true;
        for (auto& w : words)
            if (entry.name.find(w) == std::string::npos) {
                all = false;
                break;
            }
        if (all) hits.push_back(&entry);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Indexed* a, const Indexed* b) {
        size_t la = codepoints(a->name), lb = codepoints(b->name);
        if (la != lb) return la < lb;
        return a->name < b->name;
    });
    total = hits.size();
    std::vector<std::string> out;
    for (size_t i = 0; i < hits.size() && i < limit; ++i) out.push_back(hits[i]->path);
    return out;
}

ui::DragData fileUrls(const std::vector<std::string>& paths) {
    ui::DragData d;
    d.files = paths;
    d.kind = "files";
    return d;
}

std::string resultFolder(const std::string& path, const std::vector<std::string>& folders) {
    std::string folder = util::dirName(path);
    for (auto& f : folders) {
        std::string prefix = util::endsWith(f, "/") ? f : f + "/";
        if (util::startsWith(path, prefix)) {
            std::string root = prefix.size() > 1 ? prefix.substr(0, prefix.size() - 1) : prefix;
            if (folder == root || folder + "/" == prefix) return ".";
            return folder.substr(prefix.size());
        }
    }
    return folder;
}

}  // namespace explorer

// =================================================================================================
// FileView
// =================================================================================================

namespace {

// small pictures for the rows: a folder, a file
void folderIcon(Painter& p, float x, float y, Color c) {
    Path path;
    path.moveTo(x + 1.5f, y + 4);
    path.lineTo(x + 6, y + 4);
    path.lineTo(x + 7.5f, y + 5.5f);
    path.lineTo(x + 14.5f, y + 5.5f);
    path.lineTo(x + 14.5f, y + 13);
    path.lineTo(x + 1.5f, y + 13);
    path.close();
    p.fillPath(path, c.withAlpha(90));
    p.strokePath(path, c, 1);
}

void fileIcon(Painter& p, float x, float y, Color c) {
    Path path;
    path.moveTo(x + 3.5f, y + 2.5f);
    path.lineTo(x + 9.5f, y + 2.5f);
    path.lineTo(x + 12.5f, y + 5.5f);
    path.lineTo(x + 12.5f, y + 13.5f);
    path.lineTo(x + 3.5f, y + 13.5f);
    path.close();
    p.strokePath(path, c, 1);
    // a little waveform inside
    p.drawVLine((int)x + 6, (int)y + 8, (int)y + 12, c);
    p.drawVLine((int)x + 8, (int)y + 6, (int)y + 12, c);
    p.drawVLine((int)x + 10, (int)y + 9, (int)y + 12, c);
}

// the stack of the tree and the results: one shown, filling it all
class Stack : public ui::Widget {
public:
    Stack() { setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding); }
    void resized() override {
        for (auto& c : children()) c->setGeometry(rect());
    }
    Point sizeHint() const override { return Point(200, 200); }
    Point minimumSizeHint() const override { return Point(40, 40); }
};

// a word-wrapped label as tall as its text needs at its width (QLabel's height-for-width)
class WrapLabel : public ui::Label {
public:
    explicit WrapLabel(const std::string& text = "") : ui::Label(text) { setWordWrap(true); }
    int heightFor(int width) const {
        Font f = font();
        int lines = 0;
        for (const std::string& para : util::split(text(), '\n')) {
            std::string line;
            int n = 1;
            for (const std::string& word : util::split(para, ' ')) {
                std::string cand = line.empty() ? word : line + " " + word;
                if (!line.empty() && f.width(cand) > width) {
                    ++n;
                    line = word;
                } else {
                    line = cand;
                }
            }
            lines += n;
        }
        if (lines <= 0) return 0;
        return (int)std::ceil((lines - 1) * f.lineSpacing() + f.ascent() + f.descent()) + 2 * margin;
    }
    Point sizeHint() const override {
        Point h = ui::Label::sizeHint();
        h.y = heightFor(width() > 0 ? width() - 2 * margin : 360);
        return h;
    }
    Point minimumSizeHint() const override { return Point(16, sizeHint().y); }
    void resized() override {
        if (width() != laidFor_) {
            laidFor_ = width();
            if (heightFor(width() - 2 * margin) != height()) updateGeometry();
        }
    }
private:
    int laidFor_ = -1;
};

}  // namespace

FileView::FileView(Mode mode, ui::Widget* parent) : ui::Widget(parent), mode_(mode) {
    bar_ = add<ui::ScrollBar>(ui::Orientation::Vertical);
    bar_->setVisible(false);
    bar_->valueChanged.connect([this](int) { update(); });
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMouseTracking(true);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    root_.depth = -1;
    root_.expanded = root_.filled = true;
}

void FileView::fill(Item* item) {
    item->filled = true;
    item->children.clear();
    std::vector<util::DirEntry> entries = util::listDir(item->path);  // (sorted naturally)
    std::stable_partition(entries.begin(), entries.end(), [](const util::DirEntry& e) { return e.isDir; });  // folders first
    for (auto& e : entries) {
        if (!e.name.empty() && e.name[0] == '.') continue;  // (hidden)
        if (!e.isDir && !file_io::isSample(e.name)) continue;  // only audio and MIDI files
        auto child = std::make_unique<Item>();
        child->path = util::joinPath(item->path, e.name);
        child->name = e.name;
        child->isDir = e.isDir;
        child->depth = item->depth + 1;
        child->parent = item;
        item->children.push_back(std::move(child));
    }
}

void FileView::collect(Item* item) {
    for (auto& c : item->children) {
        rows_.push_back(c.get());
        if (c->expanded) collect(c.get());
    }
}

void FileView::rebuildRows() {
    rows_.clear();
    collect(&root_);
    std::set<const Item*> visible(rows_.begin(), rows_.end());
    auto gone = [&](Item*& p) {
        if (p && !visible.count(p)) p = nullptr;
    };
    // (rows inside a closed folder keep nothing: a click there is impossible)
    for (auto it = selected_.begin(); it != selected_.end();)
        it = visible.count(*it) ? std::next(it) : selected_.erase(it);
    gone(anchor_);
    gone(hover_);
    gone(pressItem_);
    if (current_ && !visible.count(current_)) current_ = nullptr;
    syncBar();
}

void FileView::setRootPath(const std::string& folder) {
    root_.children.clear();
    rows_.clear();
    selected_.clear();
    current_ = anchor_ = hover_ = pressItem_ = nullptr;
    rootPath_ = folder;
    root_.path = folder;
    if (!folder.empty()) fill(&root_);
    bar_->setValue(0);
    rebuildRows();
}

void FileView::reload() {
    if (mode_ != Mode::Tree) return;
    std::set<std::string> open, chosen;
    std::function<void(Item*)> remember = [&](Item* it) {
        for (auto& c : it->children) {
            if (c->expanded) open.insert(c->path);
            remember(c.get());
        }
    };
    remember(&root_);
    for (auto* s : selected_) chosen.insert(s->path);
    std::string currentPath = current_ ? current_->path : "";
    int scroll = bar_->value();
    setRootPath(rootPath_);
    std::function<void(Item*)> reopen = [&](Item* it) {
        for (auto& c : it->children) {
            if (c->isDir && open.count(c->path)) {
                fill(c.get());
                c->expanded = true;
                reopen(c.get());
            }
        }
    };
    reopen(&root_);
    rebuildRows();
    for (Item* r : rows_) {
        if (chosen.count(r->path)) selected_.insert(r);
        if (r->path == currentPath) current_ = anchor_ = r;
    }
    bar_->setValue(scroll);
    update();
}

void FileView::clear() {
    root_.children.clear();
    rows_.clear();
    selected_.clear();
    current_ = anchor_ = hover_ = pressItem_ = nullptr;
    bar_->setValue(0);
    syncBar();
}

void FileView::addResult(const std::string& path, const std::string& folderText) {
    auto item = std::make_unique<Item>();
    item->path = path;
    item->name = util::baseName(path);
    item->folder = folderText;
    item->tipName = path;
    item->tipFolder = util::dirName(path);
    item->depth = 0;
    item->parent = &root_;
    rows_.push_back(item.get());
    root_.children.push_back(std::move(item));
    syncBar();
}

void FileView::resizeColumnToContents() {
    Font f = theme::uiFont();
    int w = ui::style::textWidth(f, "Name") + 12;
    for (Item* r : rows_) w = std::max(w, ui::style::textWidth(f, r->name) + 12);
    col0_ = w;
    update();
}

void FileView::setExpanded(Item* item, bool on) {
    if (!item || !item->isDir) return;
    if (on && !item->filled) fill(item);
    item->expanded = on;
    if (!on && current_) {
        for (Item* p = current_->parent; p; p = p->parent)
            if (p == item) {
                current_ = anchor_ = item;
                break;
            }
    }
    rebuildRows();
}

int FileView::rowOf(const Item* item) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == item) return (int)i;
    return -1;
}

int FileView::viewWidth() const { return width() - (bar_->isVisible() ? theme::SCROLLBAR_W : 0); }

FileView::Item* FileView::itemAt(Point p) const {
    if (p.x < 0 || p.x >= viewWidth() || p.y < top() || p.y >= height()) return nullptr;
    int row = (p.y - top() + bar_->value()) / std::max(1, rowHeight);
    return itemAtRow(row);
}

void FileView::syncBar() {
    int total = (int)rows_.size() * rowHeight, vh = height() - top();
    bool need = total > vh && vh > 0;
    bar_->setVisible(need);
    bar_->setGeometry(width() - theme::SCROLLBAR_W, top(), theme::SCROLLBAR_W, std::max(0, vh));
    bar_->setPageStep(std::max(1, vh));
    bar_->setSingleStep(rowHeight);
    bar_->setRange(0, std::max(0, total - vh));
    update();
}

void FileView::resized() { syncBar(); }

void FileView::scrollTo(const Item* item) {
    int row = rowOf(item);
    if (row < 0) return;
    int y = row * rowHeight, v = bar_->value(), vh = height() - top();
    if (y < v) v = y;
    else if (y + rowHeight > v + vh) v = y + rowHeight - vh;
    bar_->setValue(v);
}

std::vector<std::string> FileView::selectedPaths() const {
    std::vector<std::string> out;
    for (Item* r : rows_)
        if (selected_.count(r)) out.push_back(r->path);
    return out;
}

void FileView::selectRange(Item* a, Item* b) {
    int ra = rowOf(a), rb = rowOf(b);
    if (ra < 0) ra = rb;
    if (rb < 0) return;
    selected_.clear();
    for (int i = std::min(ra, rb); i <= std::max(ra, rb); ++i) selected_.insert(rows_[i]);
}

void FileView::setCurrent(Item* item, int mods) {
    if (!item) return;
    if (mods & plat::ModShift) {
        selectRange(anchor_ ? anchor_ : item, item);
    } else if (!(mods & plat::ModCtrl)) {
        selected_ = {item};
        anchor_ = item;
    }
    bool changed = item != current_;
    current_ = item;
    scrollTo(item);
    update();
    if (changed) currentChanged.emit(item->path);
}

void FileView::paint(Painter& p) {
    const theme::Shades& sh = theme::shades();
    p.fillRect(rect(), theme::c("base"));
    int vw = viewWidth();
    Font f = theme::uiFont();
    int scroll = bar_->value();
    int head = top();
    p.save();
    p.setClip(Rect(0, head, vw, height() - head));
    int first = std::max(0, scroll / std::max(1, rowHeight));
    for (int row = first; row < (int)rows_.size(); ++row) {
        int y = head + row * rowHeight - scroll;
        if (y >= height()) break;
        Item* it = rows_[row];
        Rect r(0, y, vw, rowHeight);
        bool sel = selected_.count(it) > 0;
        if (sel) p.fillRect(r, theme::c("highlight"));
        else if (it == hover_) p.fillRect(r, sh.itemHover);
        Color tc = sel ? theme::c("highlighted_text") : theme::c("text");
        if (mode_ == Mode::Tree) {
            int x = it->depth * indent;
            if (it->isDir && !(it->filled && it->children.empty())) {
                Color ac = sel ? theme::c("highlighted_text") : theme::c("hint_text");
                ui::style::icon(p, x + (indent - 10) / 2.f, y + (rowHeight - 10) / 2.f, 10, it->expanded ? "down" : "right", ac);
            }
            x += indent;
            Color ic = sel ? theme::c("highlighted_text") : theme::c("hint_text");
            if (it->isDir) folderIcon(p, (float)x, y + (rowHeight - 16) / 2.f, ic);
            else fileIcon(p, (float)x, y + (rowHeight - 16) / 2.f, ic);
            x += 16 + 4;
            ui::style::text(p, Rect(x, y, std::max(0, vw - x - 4), rowHeight), AlignLeft | AlignVCenter, it->name, f, tc, true);
        } else {
            int c0 = std::min(col0_, std::max(40, vw - 40));
            ui::style::text(p, Rect(4, y, std::max(0, c0 - 8), rowHeight), AlignLeft | AlignVCenter, it->name, f, tc, true);
            ui::style::text(p, Rect(c0 + 6, y, std::max(0, vw - c0 - 10), rowHeight), AlignLeft | AlignVCenter, it->folder,
                            f, tc, true);
        }
    }
    p.restore();
    if (mode_ == Mode::Results) {  // the header: QHeaderView::section
        int c0 = std::min(col0_, std::max(40, vw - 40));
        p.fillRect(Rect(0, 0, width(), head), theme::c("window"));
        p.fillRect(Rect(0, head - 1, width(), 1), sh.line);
        Color hc = theme::c("hint_text");
        ui::style::text(p, Rect(6, 0, std::max(0, c0 - 12), head - 1), AlignLeft | AlignVCenter, "Name", f, hc, true);
        ui::style::text(p, Rect(c0 + 6, 0, std::max(0, width() - c0 - 12), head - 1), AlignLeft | AlignVCenter, "Folder",
                        f, hc, true);
    }
}

bool FileView::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton && e.button != plat::RightButton) return true;
    Item* it = itemAt(e.pos);
    pressItem_ = nullptr;
    pressDeferred_ = false;
    if (!it) {
        if (e.pos.y >= top() && e.button == plat::LeftButton && !(e.mods & (plat::ModCtrl | plat::ModShift))) {
            selected_.clear();  // a click below the rows: nothing selected
            update();
        }
        return true;
    }
    if (e.button == plat::LeftButton && mode_ == Mode::Tree && it->isDir) {
        int x = it->depth * indent;
        if (e.pos.x >= x && e.pos.x < x + indent) {  // the arrow: open or close it
            setExpanded(it, !it->expanded);
            return true;
        }
    }
    bool changed = it != current_;
    if (e.button == plat::RightButton) {
        if (!selected_.count(it)) {
            selected_ = {it};
            anchor_ = it;
        }
    } else if (e.mods & plat::ModCtrl) {
        if (selected_.count(it)) selected_.erase(it);
        else selected_.insert(it);
        anchor_ = it;
    } else if (e.mods & plat::ModShift) {
        selectRange(anchor_ ? anchor_ : it, it);
    } else if (selected_.count(it)) {
        pressDeferred_ = true;  // (it may be the start of dragging all of them)
    } else {
        selected_ = {it};
        anchor_ = it;
    }
    current_ = it;
    update();
    if (e.button == plat::LeftButton) {
        pressItem_ = it;
        pressPos_ = e.pos;
    }
    if (changed) currentChanged.emit(it->path);
    return true;
}

bool FileView::mouseMove(ui::MouseEvent& e) {
    Item* it = itemAt(e.pos);
    if (it != hover_) {
        hover_ = it;
        update();
    }
    if (pressItem_ && (e.buttons & plat::LeftButton)) {
        int d = std::abs(e.pos.x - pressPos_.x) + std::abs(e.pos.y - pressPos_.y);
        if (d >= 10) {  // (QApplication::startDragDistance)
            pressItem_ = nullptr;
            pressDeferred_ = false;
            std::vector<std::string> paths = selectedPaths();
            if (!paths.empty()) startDrag(explorer::fileUrls(paths));
        }
    }
    return true;
}

bool FileView::mouseRelease(ui::MouseEvent& e) {
    Item* pressed = pressItem_;
    pressItem_ = nullptr;
    if (pressed && pressDeferred_) {
        selected_ = {pressed};
        anchor_ = pressed;
        update();
    }
    pressDeferred_ = false;
    if (pressed && e.button == plat::LeftButton && itemAt(e.pos) == pressed) {
        std::string path = pressed->path;
        clicked.emit(path);
    }
    return true;
}

bool FileView::mouseDoubleClick(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return mousePress(e);
    Item* it = itemAt(e.pos);
    if (!it) return true;
    pressItem_ = nullptr;
    pressDeferred_ = false;
    if (mode_ == Mode::Tree && it->isDir) setExpanded(it, !it->expanded);  // (expands on double-click)
    std::string path = it->path;
    doubleClicked.emit(path);
    return true;
}

bool FileView::wheel(ui::WheelEvent& e) {
    int old = bar_->value();
    bar_->setValue(old - (int)std::lround(e.dy * 3 * rowHeight));
    return true;
}

bool FileView::keyPress(ui::KeyEvent& e) {
    int n = (int)rows_.size();
    if (!n) return false;
    if (e.key == 'A' && (e.mods & plat::ModCtrl)) {
        selected_ = std::set<const Item*>(rows_.begin(), rows_.end());
        update();
        return true;
    }
    int row = current_ ? rowOf(current_) : -1;
    int page = std::max(1, (height() - top()) / std::max(1, rowHeight) - 1);
    int mods = e.mods & (plat::ModShift | plat::ModCtrl);
    switch (e.key) {
    case plat::Key_Up: row = row < 0 ? 0 : std::max(0, row - 1); break;
    case plat::Key_Down: row = std::min(n - 1, row + 1); break;
    case plat::Key_PageUp: row = std::max(0, row - page); break;
    case plat::Key_PageDown: row = std::min(n - 1, std::max(0, row) + page); break;
    case plat::Key_Home: row = 0; break;
    case plat::Key_End: row = n - 1; break;
    case plat::Key_Right:
        if (mode_ != Mode::Tree || !current_) return false;
        if (current_->isDir && !current_->expanded) {
            setExpanded(current_, true);
            update();
            return true;
        }
        if (current_->expanded && !current_->children.empty()) row = rowOf(current_->children[0].get());
        mods = 0;
        break;
    case plat::Key_Left:
        if (mode_ != Mode::Tree || !current_) return false;
        if (current_->expanded) {
            setExpanded(current_, false);
            update();
            return true;
        }
        if (current_->parent && current_->parent != &root_) row = rowOf(current_->parent);
        mods = 0;
        break;
    default:
        return false;
    }
    if (row >= 0 && row < n) setCurrent(rows_[row], mods);
    return true;
}

std::string FileView::toolTipAt(Point p) const {
    Item* it = itemAt(p);
    if (it && mode_ == Mode::Results) {
        int c0 = std::min(col0_, std::max(40, viewWidth() - 40));
        return p.x < c0 ? it->tipName : it->tipFolder;
    }
    return toolTip();
}

// =================================================================================================
// Explorer
// =================================================================================================

namespace {
ui::Button* smallButton(ui::Widget* parent, const std::string& text, const std::string& tip, std::function<void()> slot) {
    ui::Button* b = ui::toolButton(parent, text, std::move(slot), tip);
    b->setFixedSize(explorer::SMALL_BUTTON, explorer::SMALL_BUTTON);
    return b;
}
}  // namespace

Explorer::Explorer(std::function<void(const std::string&)> addFile_, audio::Engine& engine_, ui::Widget* parent)
    : ui::Widget(parent), addFile(std::move(addFile_)), engine(engine_), life_(std::make_shared<int>(0)) {
    waveform = add<SampleWaveform>();
    waveform->clicked.connect([this](double f) { playFrom(f); });
    folders = explorer::loadFolders();

    folderBox = add<ui::ComboBox>();
    folderBox->setToolTip("Folder to browse");
    folderBox->hPolicy = ui::SizePolicy::Expanding;
    folderBox->setMinimumWidth(60);  // (long folder names give way to the buttons)
    folderBox->currentIndexChanged.connect([this](int) {
        if (!filling_) showFolder();
    });
    addButton = smallButton(this, "+", "Add a folder with samples", [this] { addFolder(); });
    removeButton = smallButton(this, "\xe2\x88\x92", "Remove this folder from the explorer (nothing is deleted)",
                               [this] { removeFolder(); });
    rescanButton = smallButton(this, "\xe2\x9f\xb3", "Look for new samples in the folders", [this] {
        tree->reload();  // (the tree has no watcher of its own: read it again too)
        reindex();
    });
    auto top = std::make_unique<ui::HBox>();
    top->spacing = 3;
    top->addWidget(folderBox, 1);
    for (ui::Button* b : {addButton, removeButton, rescanButton}) top->addWidget(b);

    searchBox = add<ui::LineEdit>();
    searchBox->setPlaceholder("Search samples\xe2\x80\xa6");
    searchBox->setClearButton(true);
    searchBox->textChanged.connect([this](const std::string&) {  // search once typing pauses
        if (searchTimer_ >= 0) ui::App::get().cancelTimer(searchTimer_);
        std::weak_ptr<int> alive = life_;
        searchTimer_ = ui::App::get().singleShot(150, [this, alive] {
            if (!alive.lock()) return;
            searchTimer_ = -1;
            runSearch();
        });
    });

    stack_ = add<Stack>();
    tree = stack_->add<FileView>(FileView::Mode::Tree);
    results = stack_->add<FileView>(FileView::Mode::Results);
    for (FileView* view : {tree, results}) {
        view->doubleClicked.connect([this](const std::string& path) { openPath(path); });
        view->clicked.connect([this](const std::string& path) { preview(path); });
        view->currentChanged.connect([this](const std::string& path) { preview(path); });
    }
    results->setVisible(false);
    status = add<WrapLabel>();

    previewButton = ui::toolButton(this, "\xf0\x9f\x94\x8a Preview", nullptr,
                                   "Play a sample when you click it (or move to it with the arrow keys)");
    previewButton->setCheckable(true);
    previewButton->setCheckedSilently(settings::value(explorer::PREVIEW_KEY, Json(true)).toBool(true));
    previewButton->setProperty("accent", "true");  // lit in the skin's accent colour when on
    previewButton->toggled.connect([this](bool on) { setPreviewOn(on); });
    stopButton = smallButton(this, "\xe2\x96\xa0", "Stop the preview", [this] { stopPreview(); });
    loopButton = ui::toolButton(this, "\xe2\x9f\xb2 Loop", nullptr, "Play previews over and over, until \xe2\x96\xa0 or another sample");
    loopButton->setCheckable(true);
    loopButton->setCheckedSilently(settings::value(explorer::LOOP_KEY, Json(false)).toBool(false));
    loopButton->setProperty("accent", "true");
    loopButton->toggled.connect([this](bool on) { setLoopOn(on); });
    auto previewRow = std::make_unique<ui::HBox>();
    previewRow->spacing = 6;
    previewRow->addWidget(previewButton);
    previewRow->addWidget(loopButton);
    previewRow->addWidget(stopButton);
    previewRow->addStretch(1);

    hint = add<WrapLabel>("Click a sample to hear it. Drag samples onto a track, or double-click to add it at the cursor.");
    hint->setEnabled(false);  // dimmed

    ui::VBox* col = ui::vbox(this, 6, 5);
    col->addWidget(waveform);
    col->addLayout(std::move(top));
    col->addWidget(searchBox);
    col->addLayout(std::move(previewRow));
    col->addWidget(stack_, 1);
    col->addWidget(status);
    col->addWidget(hint);
    fillFolderBox();
    reindex();
}

Explorer::~Explorer() {
    ui::App& app = ui::App::get();
    if (positionTimer_ >= 0) app.cancelTimer(positionTimer_);
    if (searchTimer_ >= 0) app.cancelTimer(searchTimer_);
    if (indexCancel_) indexCancel_->store(true);
}

// --- folders ---

void Explorer::fillFolderBox(const std::string& current) {
    filling_ = true;
    folderBox->clear();
    for (auto& folder : folders) {
        std::string trimmed = folder;
        while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
        std::string name = util::baseName(trimmed);
        folderBox->addItem(name.empty() ? folder : name, folder);
    }
    if (folders.empty()) folderBox->addItem("No folders yet: add one with +");
    int at = 0;
    for (size_t i = 0; i < folders.size(); ++i)
        if (folders[i] == current) at = (int)i;
    folderBox->setCurrentIndexSilently(at);
    filling_ = false;
    removeButton->setEnabled(!folders.empty());
    showFolder();
}

void Explorer::showFolder() {
    std::string folder = folderBox->currentData();
    if (!folder.empty() && folder != tree->rootPath()) tree->setRootPath(folder);
    if (folder.empty() && !tree->rootPath().empty()) tree->setRootPath("");
    tree->setVisible(!folder.empty() && !showingResults());
}

void Explorer::addFolder() {
    std::string folder = ui::files::folder(window(), "Add a folder with samples", "");
    if (folder.empty()) return;
    addFolderPath(folder);
}

void Explorer::addFolderPath(const std::string& chosen) {
    std::string folder = chosen;
    while (folder.size() > 1 && folder.back() == '/') folder.pop_back();  // (os.path.normpath)
    if (std::find(folders.begin(), folders.end(), folder) == folders.end()) {
        folders.push_back(folder);
        explorer::saveFolders(folders);
        reindex();
    }
    fillFolderBox(folder);
}

void Explorer::removeFolder() {
    std::string folder = folderBox->currentData();
    auto it = std::find(folders.begin(), folders.end(), folder);
    if (it != folders.end()) {
        folders.erase(it);
        explorer::saveFolders(folders);
        fillFolderBox();
        reindex();
    }
}

// --- searching ---

void Explorer::reindex() {
    int generation = ++generation_;
    std::vector<std::string> list = folders;
    indexing = true;
    showStatus();
    if (indexCancel_) indexCancel_->store(true);  // a newer request wins
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    indexCancel_ = cancel;
    std::weak_ptr<int> alive = life_;
    std::thread([this, alive, generation, list, cancel] {
        auto found = std::make_shared<std::vector<explorer::Indexed>>(
            explorer::indexFolders(list, config::EXPLORER_MAX_FILES, cancel.get()));
        if (cancel->load()) return;
        plat::postToMain([this, alive, generation, found] {
            if (alive.lock()) onIndexed(generation, std::move(*found));
        });
    }).detach();
}

void Explorer::onIndexed(int generation, std::vector<explorer::Indexed> found) {
    if (generation != generation_) return;  // the folders changed meanwhile; a newer index is on its way
    index = std::move(found);
    indexing = false;
    runSearch();
}

bool Explorer::showingResults() const { return !util::trim(searchBox->text()).empty(); }

void Explorer::runSearch() {
    std::string text = util::trim(searchBox->text());
    results->setVisible(!text.empty());
    tree->setVisible(text.empty() && !folderBox->currentData().empty());
    results->clear();
    if (!text.empty()) {
        size_t total = 0;
        std::vector<std::string> paths = explorer::search(index, text, config::EXPLORER_MAX_RESULTS, total);
        for (auto& path : paths) results->addResult(path, explorer::resultFolder(path, folders));
        results->resizeColumnToContents();
        showStatus((long)paths.size(), (long)total);
    } else {
        showStatus();
    }
}

void Explorer::showStatus(long shown, long total) {
    std::string text;
    if (indexing) text = "Looking through the folders\xe2\x80\xa6";
    else if (folders.empty()) text = "Add a folder with your samples (+).";
    else if (shown < 0)
        text = std::to_string(index.size()) + " samples in " + std::to_string(folders.size()) + " folder" +
               (folders.size() != 1 ? "s" : "");
    else if (total == 0) text = "No samples found.";
    else text = std::to_string(total) + " found" + (shown < total ? ", showing the first " + std::to_string(shown) : "");
    status->setText(text);
}

// --- previewing ---

void Explorer::setPreviewOn(bool on) {
    settings::set(explorer::PREVIEW_KEY, Json(on));
    if (!on) stopPreview();
}

void Explorer::setLoopOn(bool on) {
    settings::set(explorer::LOOP_KEY, Json(on));
    std::shared_ptr<audio::PreviewVoice> voice = engine.previewVoice();
    if (voice_ && voice == voice_) voice->loop = on;  // the preview playing now follows at once
}

void Explorer::preview(const std::string& path) {
    if (path.empty() || !file_io::isSample(path) || !util::fileExists(path) || util::isDir(path)) return;
    double now = ui::App::get().now();
    if (lastPreviewPath_ == path && now - lastPreviewTime_ < explorer::REPEAT_GAP_S) return;  // (a press and its click both ask)
    lastPreviewPath_ = path;
    lastPreviewTime_ = now;
    int generation = ++previewGeneration_;
    int sr = engine.sr;
    if (!shownPath_.empty() && shownPath_ == path && shownSr_ == sr) {
        onPreviewLoaded(generation, path, shownAudio_);
        return;
    }
    std::weak_ptr<int> alive = life_;
    std::thread([this, alive, generation, path, sr] {
        AudioPtr audio;
        std::string error;
        if (!file_io::loadPlayable(path, sr, audio, error)) audio.reset();  // unreadable: no waveform, stay silent
        plat::postToMain([this, alive, generation, path, audio] {
            if (alive.lock()) onPreviewLoaded(generation, path, audio);
        });
    }).detach();
}

void Explorer::onPreviewLoaded(int generation, const std::string& path, AudioPtr audio) {
    if (generation != previewGeneration_) return;  // only the latest one asked for
    if (shownPath_ != path || shownAudio_ != audio) {
        shownPath_ = path;
        shownSr_ = engine.sr;
        shownAudio_ = audio;
        waveform->setAudio(audio, util::baseName(path), engine.sr);
    }
    if (audio && previewButton->isChecked()) playFrom(0.0);
}

void Explorer::playFrom(double fraction) {
    AudioPtr audio = shownAudio_;
    if (!audio) return;
    int64_t first = (int64_t)(fraction * audio->frames());
    engine.preview(audio, first, loopButton->isChecked(), config::PREVIEW_GAIN);  // (a loop starts over from the beginning)
    voice_ = engine.previewVoice();
    ui::App& app = ui::App::get();
    if (positionTimer_ >= 0) app.cancelTimer(positionTimer_);
    std::weak_ptr<int> alive = life_;
    positionTimer_ = app.every(30, [this, alive] {
        if (alive.lock()) showPosition();
    });
    showPosition();
}

void Explorer::showPosition() {
    AudioPtr audio = shownAudio_;
    if (!voice_ || engine.previewVoice() != voice_ || !audio || audio->frames() == 0) {
        voice_.reset();
        if (positionTimer_ >= 0) ui::App::get().cancelTimer(positionTimer_);
        positionTimer_ = -1;
        waveform->setPosition(std::nullopt);
        return;
    }
    waveform->setPosition(std::min(1.0, (double)voice_->pos.load() / audio->frames()));
}

void Explorer::stopPreview() {
    ++previewGeneration_;  // a preview still loading won't start either
    engine.stopPreview();
    showPosition();
}

void Explorer::openPath(const std::string& path) {
    if (!path.empty() && file_io::isSample(path) && util::fileExists(path) && !util::isDir(path) && addFile) addFile(path);
}
