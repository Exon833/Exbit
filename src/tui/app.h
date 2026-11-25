// exbit --tui: the whole program in a terminal, by the keyboard alone (no mouse, no window system:
// a bare text console with an audio driver is enough).
//
// It is the graphical program's own main window, built without a display (the toolkit headless: its
// windows are never shown, its timers moved on by the terminal's loop), drawn as text: everything it
// does, it does by the same code. Laid out as the window is:
//
//     | File  Edit  View  Options                              project name      |  the menu bar
//     | ▶ 0:01.250  2.1.1   120 BPM  4/4   Loop  Metronome  Grid 1/8  Snap      |  the transport
//     | Explorer   | overview                                 | CPU · device   |
//     | folders,   | ruler: bars                              | Tracks         |
//     | samples,   | a lane per track: its clips              | name M S dB ▮▮ |  the track panel
//     | projects   | master lane                              | Master         |
//     | status: the last message, or the keys of the part with the keyboard  |
//
// Tab (or F6) moves the keyboard between the transport, the Explorer, the timeline and the track panel;
// F10 (or Alt + a menu's letter) opens the menus, which are the window's own (File, Edit, View,
// Options) and do what they do there; F2 opens the menu of what is selected (a clip's, a track's), F1
// lists the keys. The program's other windows (dialogs, the effects' patch panel, an effect's knobs, a
// plugin's parameters, the Pad Kit, the MIDI editor…) are drawn as text over it and take the keys
// (window_view.h): Ctrl+W closes the one on top, F12 hides them for the main window. The timeline's rows
// are its lanes (tracks, automation, takes, group headers) and the ruler above them. Ctrl+Q quits
// (asking first about unsaved work).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/util.h"
#include "model/model.h"
#include "ui/lanes.h"
#include <optional>
#include "tui/screen.h"
#include "tui/term.h"

class MainWindow;
namespace ui {
class Action;
class Menu;
class Window;
}  // namespace ui

namespace tui {

// runs the program in the terminal (argv: what follows --tui: a project, or files to add); the exit code
int run(const std::vector<std::string>& files);

// the main window drawn as text and worked by keys (the terminal aside: tests drive it)
class Front {
public:
    explicit Front(MainWindow& w);
    ~Front();
    enum class Pane { Transport, Explorer, Timeline, Panel };

    void key(const Key& k);  // to the dialog or menu open, else the part with the keyboard
    void draw(Screen& s);    // everything as it is now (frames: the main window's frame callbacks ran)
    bool wantsQuit() const;  // the main window closed (Ctrl+Q, File → Quit)

    // what is shown (public for the tests)
    Pane pane = Pane::Timeline;
    double secondsPerColumn = 0;  // the timeline's zoom (0: a bar is 8 columns, set when first drawn)
    double viewStart = 0;         // the time at the timeline's first column
    int firstRow = 0;             // the first track shown (scrolled down)
    std::string folder;           // the Explorer's
    std::vector<util::DirEntry> entries;
    int entry = 0, entryTop = 0;
    int control = 0;              // the transport's control with the keyboard
    int laneRow = 0;              // the lane with the keyboard (timeline and track panel): a track's, an automation
                                  // lane's, a take's, a group's header; the track panel: one past the last, the master
    bool helpShown = false;
    int helpTop = 0;  // F1's list: the first line shown (Up/Down, PgUp/PgDn scroll it when it doesn't fit)
    bool windowsHidden = false;   // F12: the open windows (not dialogs) hidden, the main window has the keys
    int64_t markFrame = -1;       // I: a range's start (O ends it at the cursor)
    TrackPtr markTrack;

    // the menus open: the bar's (barMenu >= 0) or a menu of its own (F2), and its submenus, each with the item
    // highlighted (items' indices in Menu::actions(); -1: the bar's own Quit)
    struct Open {
        ui::Menu* menu;
        int item;
    };
    std::vector<Open> open;
    int barMenu = -1;
    bool menuOpen() const { return !open.empty(); }
    void openBarMenu(int i);
    void closeMenus();
    void dropGoneMenus();  // (a control's menu closed meanwhile: forgotten)

    // (tests) the folder shown by the Explorer
    void showFolder(const std::string& path);
    void setCurrent(const TrackPtr& t);   // the current track (its lane the keyboard's)
    void selectLane(int i);               // (its track, or the track it belongs to, becomes the current one)
    std::optional<Lane> currentLane() const;

private:
    MainWindow& w_;
    std::unique_ptr<ui::Menu> ownMenu_;  // F2's (filled for what is selected), while open
    int bodyRows_ = 0;                   // track rows shown (last drawn)
    int timelineCols_ = 60;

    bool unsaved_ = false;          // the menu bar's " *" (asked of the project at most every UNSAVED_S: it is slow)
    double unsavedAt_ = -1, unsavedCost_ = 0;
    ui::Window* lastTop_ = nullptr;
    void closeUnder();              // a window that has just opened over F1's list or the bar's menu: they close

    ui::Window* topWindow() const;  // a dialog or popup over the main window, if any
    bool drawsWindow(ui::Window* win) const;
    int openWindows() const;        // the program's windows of their own open (an editor…), not dialogs
    int shownWindows_ = 0;
    bool menuKey(const Key& k);
    void chooseItem(ui::Menu* m, int item);
    void contextMenu();
    void showOwnMenu(std::unique_ptr<ui::Menu> menu);  // a menu of its own (F2's, the instrument's…), open
    void moveFocus(int dir);
    bool transportKey(const Key& k);
    bool explorerKey(const Key& k);
    bool timelineKey(const Key& k);
    bool panelKey(const Key& k);
    void forward(const Key& k);  // to the main window (its shortcuts; the timeline's keys)

    std::vector<TrackPtr> tracks() const;
    std::vector<Lane> lanes() const;     // the timeline's, top to bottom, the master's left out
    int pointAtCursor(const Automation& a) const;  // an automation point within half a column of the cursor, or -1
    bool automationKey(const Key& k, const Lane& lane);
    double laneValue(const Lane& lane, double seconds) const;        // its curve there (none: the parameter now), 0…1
    std::string laneText(const Lane& lane, double value) const;      // as the parameter shows it ("-3.0 dB")
    TrackPtr current() const;
    double stepBeats() const;          // the grid's step (beats) where the cursor is
    void moveCursor(double seconds);   // (snapped to the grid)
    void moveSelection(int64_t frames, int rows);
    void keepCursorInView();

    void drawMenuBar(Screen& s);
    void drawTransport(Screen& s);
    void drawExplorer(Screen& s, int x, int y, int w, int h);
    void drawTimeline(Screen& s, int x, int y, int w, int h);
    void drawPanel(Screen& s, int x, int y, int w, int h);
    void drawStatus(Screen& s);
    void drawMenus(Screen& s);
    void drawHelp(Screen& s);
    std::string keysOf(const std::string& id) const;  // a command's keys as bound (ui/commands.h), those consoles send
};

// the colour of the 8 nearest a "#rrggbb" ("": none: def)
uint8_t nearestColor(const std::string& hex, uint8_t def);
// a menu entry's text without its & (mnemonic: the letter after it, uppercase; 0: none)
std::string menuText(const std::string& text, char* mnemonic = nullptr);

}  // namespace tui
