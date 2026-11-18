// The explorer on the left (explorer.py): browse your sample folders or search them by name, listen
// to a sample by clicking it, then drag samples onto the timeline (or double-click one to add it at
// the selection cursor).
//
// The folders are the ones you add with "+" (remembered between runs). Browsing shows one folder at
// a time as a tree, with only audio and MIDI files. Searching looks through all folders and their
// subfolders for files whose names contain every word typed. The list of files to search is built
// in a background thread (indexFolders), when the explorer opens and whenever the folders change or
// you press ⟳, so typing stays instant even for big libraries.
//
// Clicking a sample, or moving to it with the arrow keys, shows its waveform at the top
// (sample_waveform.h; loaded in a background thread) and, with 🔊 Preview on (remembered), plays it
// through the engine (Engine::preview, outputs 1/2): once, or over and over with ⟲ Loop on
// (remembered); ■ stops it. Click the waveform to play from that point.
//
// The main window puts it in the left dock (config::EXPLORER_W wide, its width draggable).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "config.h"
#include "ui/panel_style.h"
#include "ui/sample_waveform.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace explorer {
extern const char* const FOLDERS_KEY;  // "explorer/folders" (the settings keys of the Python program)
extern const char* const PREVIEW_KEY;  // "explorer/preview"
extern const char* const LOOP_KEY;     // "explorer/loop"
constexpr double REPEAT_GAP_S = 0.3;   // a press and its click both ask to preview the same sample: play it once
constexpr int SMALL_BUTTON = 24;       // px: the +, −, ⟳ and ■ buttons

std::vector<std::string> loadFolders();  // first run: ~/Music, if there is one
void saveFolders(const std::vector<std::string>& folders);

struct Indexed {
    std::string name;  // the file name, lower case
    std::string path;
};
// every sample in `folders` and their subfolders (hidden folders skipped), at most `limit`;
// `cancel` (if given) set stops it early (a newer index is wanted)
std::vector<Indexed> indexFolders(const std::vector<std::string>& folders, size_t limit = config::EXPLORER_MAX_FILES,
                                  const std::atomic<bool>* cancel = nullptr);
// paths whose file name contains every word of `text` (any case), shortest names first; `total`:
// how many matched in all
std::vector<std::string> search(const std::vector<Indexed>& index, const std::string& text,
                                size_t limit, size_t& total);
// drag data for files: the same as dragging them from the file manager
ui::DragData fileUrls(const std::vector<std::string>& paths);
// the folder the result's own folder is shown as: relative to the explorer folder holding it ("." for
// itself), else the full folder
std::string resultFolder(const std::string& path, const std::vector<std::string>& folders);
}  // namespace explorer

// The list under the folder box: one folder as a tree (Tree; folders filled when first opened, only
// audio and MIDI files), or the search results (Results: name and folder columns under a header).
// Several rows can be selected (click, Ctrl+click, Shift+click, Shift+arrows); dragging drags the
// selected files out.
//
// Styled as FileView (default.css): a box, with color and font (the rows' text); ::row:hover and ::row:selected
// (boxes, and color); ::icon and ::icon:selected (the arrows and the folder and file pictures: color;
// fill-opacity: a folder's inside); ::header (Results: a box, its bottom border the line under it; color, font). :root's --explorer-row-height, --explorer-indent (a tree level)
// and --explorer-header-height (rowHeight, indent and headerH() follow them).
class FileView : public ui::Widget {
public:
    enum class Mode { Tree, Results };
    explicit FileView(Mode mode, ui::Widget* parent = nullptr);

    struct Item {
        std::string path, name, folder;     // folder: the Results' second column
        std::string tipName, tipFolder;     // tooltips of the two columns (Results)
        bool isDir = false, filled = false, expanded = false;
        int depth = 0;
        Item* parent = nullptr;
        std::vector<std::unique_ptr<Item>> children;
    };

    // Tree: show this folder (its contents, folders first); "" shows nothing
    void setRootPath(const std::string& folder);
    const std::string& rootPath() const { return rootPath_; }
    void reload();                          // read the folders shown again (keeps open ones open)
    // Results
    void clear();
    void addResult(const std::string& path, const std::string& folderText);
    void resizeColumnToContents();          // the name column as wide as its widest name

    void setExpanded(Item* item, bool on);
    int rowCount() const { return (int)rows_.size(); }
    Item* itemAtRow(int row) const { return row >= 0 && row < (int)rows_.size() ? rows_[row] : nullptr; }
    int rowOf(const Item* item) const;
    Item* itemAt(Point p) const;
    Item* current() const { return current_; }
    void setCurrent(Item* item, int mods = 0);  // as the keyboard moves there (selects it; Shift extends)
    std::vector<std::string> selectedPaths() const;  // in row order
    bool isSelected(const Item* item) const { return selected_.count(item) > 0; }
    void scrollTo(const Item* item);
    int nameColumnWidth() const { return col0_; }

    ui::Signal<const std::string&> clicked;         // released over the row it was pressed on
    ui::Signal<const std::string&> doubleClicked;
    ui::Signal<const std::string&> currentChanged;  // moved to with the mouse or the keys

    int rowHeight = 20;
    int indent = 16;
    static constexpr int HEADER_H = 22;  // (the default of --explorer-header-height)
    static int headerH();

    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override;
    bool keyPress(ui::KeyEvent& e) override;
    void resized() override;
    void leave() override { hover_ = nullptr; update(); }
    std::string toolTipAt(Point p) const override;
    Point sizeHint() const override { return Point(200, 200); }
    Point minimumSizeHint() const override { return Point(40, 40); }

private:
    Mode mode_;
    std::string rootPath_;
    Item root_;
    std::vector<Item*> rows_;
    std::set<const Item*> selected_;
    Item* current_ = nullptr;
    Item* anchor_ = nullptr;
    Item* hover_ = nullptr;
    Item* pressItem_ = nullptr;
    Point pressPos_;
    bool pressDeferred_ = false;  // a press on a selected row: select only it at the release, unless it drags
    int col0_ = 0;
    ui::ScrollBar* bar_;
    void fill(Item* item);
    void rebuildRows();
    void collect(Item* item);
    void syncBar();
    int top() const { return mode_ == Mode::Results ? headerH() : 0; }
    int viewWidth() const;
    void selectRange(Item* a, Item* b);
    // what it is drawn with, from the stylesheets (kept: it repaints as the mouse moves over it)
    struct Look {
        // (its boxes: kept until parts_.refresh() answers true)
        const css::Style *self = nullptr, *hover = nullptr, *selected = nullptr, *header = nullptr;
        Color text, hoverText, selectedText, icon, iconSelected, headerText;
        int headerEdgeH = 1, folderAlpha = 90;
        Font font, headerFont;
    };
    const Look& look();
    panel_style::Parts parts_{*this};
    Look look_;
};

// Styled as Explorer (default.css): padding and row-gap (around and between its rows), ::folder-row and
// ::preview-row (column-gap: between their controls); :root's --explorer-button-size (the +, −, ⟳ and ■ buttons).
// Read when it is made.
class Explorer : public ui::Widget {
public:
    // addFile(path): add a sample at the selection cursor (double-click). engine plays the previews.
    Explorer(std::function<void(const std::string&)> addFile, audio::Engine& engine, ui::Widget* parent = nullptr);
    ~Explorer() override;

    std::function<void(const std::string&)> addFile;
    audio::Engine& engine;
    std::vector<std::string> folders;
    std::vector<explorer::Indexed> index;
    bool indexing = false;

    SampleWaveform* waveform;
    ui::ComboBox* folderBox;
    ui::Button* addButton;
    ui::Button* removeButton;
    ui::Button* rescanButton;
    ui::LineEdit* searchBox;
    ui::Button* previewButton;
    ui::Button* loopButton;
    ui::Button* stopButton;
    FileView* tree;
    FileView* results;
    ui::Label* status;
    ui::Label* hint;

    // --- folders ---
    void fillFolderBox(const std::string& current = "");
    void showFolder();
    void addFolder();                        // asks for a folder (file chooser)
    void addFolderPath(const std::string& folder);  // (the chooser's answer; also for tests)
    void removeFolder();                     // the one shown (nothing is deleted)

    // --- searching ---
    void reindex();                          // list every sample in the folders, in a background thread; a newer request wins
    void runSearch();
    void showStatus(long shown = -1, long total = -1);
    bool showingResults() const;

    // --- previewing ---
    void setPreviewOn(bool on);
    void setLoopOn(bool on);
    // a sample was selected: show its waveform and, if previewing is on, play it once. Loading
    // happens in a thread; the sample shown last is kept, so selecting it again plays at once.
    void preview(const std::string& path);
    void playFrom(double fraction);          // preview the shown sample from `fraction` (0…1) of the way in
    void showPosition();                     // move the waveform's line with the preview
    void stopPreview();
    void openPath(const std::string& path);  // double-click: add it at the cursor

    // --- the track presets' place (the last entry of the folder box; model/presets.h) ---
    // a track preset double-clicked (unset: nothing happens); dragging one out drags its file
    std::function<void(const std::string& path)> openPreset;
    void presetsChanged();                   // presets saved, renamed or deleted: their list shown afresh

    // the sample in the waveform
    const std::string& shownPath() const { return shownPath_; }
    AudioPtr shownAudio() const { return shownAudio_; }

private:
    int generation_ = 0, previewGeneration_ = 0;
    std::string lastPreviewPath_;
    double lastPreviewTime_ = -1e9;
    std::string shownPath_;
    int shownSr_ = 0;
    AudioPtr shownAudio_;
    bool filling_ = false;                   // (fillFolderBox: no showFolder per item)
    std::shared_ptr<audio::PreviewVoice> voice_;  // the engine's voice while previewing
    int positionTimer_ = -1, searchTimer_ = -1;
    std::shared_ptr<std::atomic<bool>> indexCancel_;
    std::shared_ptr<int> life_;              // posted results check it is still alive
    ui::Widget* stack_;
    void onIndexed(int generation, std::vector<explorer::Indexed> index);
    void onPreviewLoaded(int generation, const std::string& path, AudioPtr audio);
};

// The Explorer's dock (QDockWidget "Explorer", the main window's): a title bar with ✕ over the Explorer (`content`,
// adopted). Styled as ExplorerDock: ::title-bar is the bar (a box: a caption bar's gradient, a border; padding and
// column-gap: the room around and between the title and ✕; color and the font: its title's, when it sets them,
// else the title Label's own); :root's --dock-title-height and --dock-close-size.
class ExplorerDock : public ui::Widget {
public:
    ExplorerDock(ui::Widget* content, std::function<void()> onClose);
    // (also after a skin change: App::repaintAll calls every widget's resized())
    void resized() override { fit(); }
    ui::Widget* titleBar() const { return bar_; }
    ui::Label* title() const { return title_; }

private:
    ui::Widget* bar_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Button* close_ = nullptr;
    ui::HBox* row_ = nullptr;
    int titled_ = -1;  // (the stylesheets' generation the title's colour and font were read for)
    void fit();
};
