// The top bar (transport_bar.py): file/track buttons, the playback buttons (centred), time display,
// tempo, metronome and grid. (The audio driver and device are chosen in Options → Audio properties…)
//
// The playback buttons are square, each showing an icon from the program's data folder
// (plat::dataDir() + "/icons/<name>.svg"), drawn in the skin's text colour for the button's state
// (accent_text when on, disabled_text when off limits), so they follow skin changes at once; their
// names are in their tooltips. A button whose icon file is missing shows a symbol instead (Loop has
// no icon yet: ⟲).
//
// When the window is too narrow for all of it (a tiling desktop can make it narrower than its
// minimum, e.g. half a screen), the bar fits itself instead of letting controls overlap (fit): first
// the time display takes less room, the file and track buttons become one "+ Add ▾" button and some
// labels get short (♩, ⅓, Metro); the window can't be made narrower than that by hand
// (minimumSizeHint). Narrower still (only a tiling desktop does that), the least needed controls go,
// one by one, into a "»" menu at the right end, which has them all. Their keyboard shortcuts keep
// working whether they're shown or not.
//
// It only creates the controls; the main window connects them to the timeline and engine.
//
// THE MAIN WINDOW (the successor's additions): addReturnTrackAction (Ctrl+Alt+T) → addTrack(Return);
// keysAction (⌨, Ctrl+K, checkable) toggled → the computer keyboard; captureAction (Ctrl+Alt+R) →
// capture; metronomeAction is the metronome button's action (its toggled = metronomeButton->toggled);
// gridActions["finer" / "coarser" / "snap" / "fixed"] (Ctrl+1 / 2 / 4 / 5, application-wide) →
// change_grid; onGridChange: the Grid ▾ menu's choices (the bar fills that menu from the timing it was
// last shown, transport_bar::fillGridMenu); showTiming() after any change of the grid. The » menu's
// "Recording settings…" / "Metronome sounds…" emit recordButton / metronomeButton->rightClicked.
//
// Styled as Transport (default.css; its #id is #Transport): its box (background-color; the line under it is
// its border), padding (around its three parts) and column-gap (between its controls). IconButton (a
// ToolButton): ::icon's color is the icon's in the button's states (:checked, :disabled…). :root's
// --transport-button-size and --transport-icon-size (the playback buttons and their icons),
// --transport-time-width (the time display's room) and --transport-separator-width (around the upright lines).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "model/timing.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

// The part of a QAction the bar needs: a button in the bar plus its keyboard shortcut.
// triggered: clicked or its shortcut pressed (a checkable one is toggled first: toggled(on)).
class TransportAction {
public:
    TransportAction(ui::Button* button, const std::string& text);
    ~TransportAction();
    ui::Button* button;                       // the button showing it
    std::string text;                         // (the action's name; an IconButton shows it only in its tooltip)
    void setText(const std::string& t);
    void setToolTip(const std::string& t) { button->setToolTip(t); }
    std::string toolTip() const { return button->toolTip(); }
    void setShortcut(const std::string& keys, ui::ShortcutScope scope = ui::ShortcutScope::Window);
    void setShortcutContext(ui::ShortcutScope scope);  // ApplicationWide: also from the plugins (FX) windows
    std::string iconText;                     // what its (text) button shows ("+ Audio"); text is for menus ("Audio track")
    void setIconText(const std::string& t);
    ui::Widget* shortcutOwner = nullptr;      // its shortcut works while this is shown (the bar: hidden buttons too)
    const ui::KeySequence& shortcut() const { return seq_; }
    void setCheckable(bool on) { button->setCheckable(on); }
    bool isCheckable() const { return button->isCheckable(); }
    void setChecked(bool on) { button->setChecked(on); }   // emits toggled when it changes
    void setCheckedSilently(bool on) { button->setCheckedSilently(on); }
    bool isChecked() const { return button->isChecked(); }
    void setEnabled(bool on) { button->setEnabled(on); }
    bool isEnabled() const { return button->isEnabled(); }
    void trigger();                           // as its shortcut: toggles a checkable one, then triggered
    ui::Signal<> triggered;
    ui::Signal<bool> toggled;
    bool signalsBlocked = false;              // (QObject::blockSignals: show_timing)

private:
    ui::KeySequence seq_;
    ui::ShortcutScope scope_ = ui::ShortcutScope::Window;
    std::unique_ptr<ui::Shortcut> shortcut_;
};

namespace transport_bar {
constexpr int TIME_W = 200;  // the time display's width, with room to spare (less when the bar is short of room)
extern std::string iconFolder;  // "" = the program's own: plat::dataDir() + "/icons" (tests set another)
std::string iconDir();
int timeW();  // TIME_W as the stylesheets set it (--transport-time-width)
// icons/<name>.svg drawn px pixels square in `color`; an empty image without the file (cached)
const Image& tinted(const std::string& name, Color color, int px);
}  // namespace transport_bar

// A square playback button showing icons/<icon>.svg (or `glyph` without the file) in the skin's text
// colour for its state; its action's text and shortcut are only in its tooltip.
class IconButton : public ui::Button {
public:
    IconButton(const std::string& icon, const std::string& glyph);
    void setIcon(const std::string& icon, const std::string& glyph);
    const std::string& iconName() const { return icon_; }
    const std::string& glyph() const { return glyph_; }
    void paint(Painter& p) override;

private:
    std::string icon_, glyph_;
};

// A keyboard shortcut without a button (QAction added to the bar only for its keys): the grid's keys.
class KeyAction {
public:
    KeyAction(ui::Widget* owner, const std::string& keys, ui::ShortcutScope scope = ui::ShortcutScope::Window);
    ui::Signal<> triggered;
    void trigger() { triggered.emit(); }
    const ui::KeySequence& shortcut() const { return seq_; }
private:
    ui::KeySequence seq_;
    std::unique_ptr<ui::Shortcut> shortcut_;
};

// What the Grid ▾ menu chose (main_window.py's change_grid arguments)
struct GridChange {
    enum What { Grid, Snap, Finer, Coarser, Follow } what = Grid;
    double grid = 0;    // Grid: the fixed step in beats (0: it follows the zoom)
    bool snap = true;   // Snap: snapping on or off; Follow: automation follows clips or not (auto_follow.h)
};

namespace transport_bar {
// The Grid ▾ button's menu (main_window.py's fill_grid_menu): following the zoom, the fixed steps,
// snapping, finer / coarser; choose(change) for the one chosen.
void fillGridMenu(ui::Menu& menu, const Timing& timing, const std::function<void(const GridChange&)>& choose);
}  // namespace transport_bar

// Three parts: the file and track buttons on the left, the playback buttons in the middle of the
// window (kept there whatever the window's width, as long as everything fits), and the time,
// tempo, metronome and grid on the right.
class TransportBar : public ui::Widget {
public:
    TransportBar();
    ~TransportBar() override;
    std::unique_ptr<TransportAction> addFileAction, addAudioTrackAction, addMidiTrackAction, addReturnTrackAction;
    std::unique_ptr<TransportAction> playAction, recordAction, startAction, loopAction, followAction;
    std::unique_ptr<TransportAction> keysAction, captureAction;  // ⌨ the computer keyboard (Ctrl+K), Capture (Ctrl+Alt+R)
    std::unique_ptr<TransportAction> metronomeAction;            // the metronome button's (Ctrl+M)
    std::vector<ui::Button*> addButtons;  // Add file, + Audio, + MIDI, + Return
    ui::Menu* addMenu = nullptr;          // the same, as one button when the bar is short of room
    ui::Button* addButton = nullptr;      // "+ Add ▾"
    ui::Widget* timeSeparator = nullptr;
    ui::Button* beatsButton = nullptr;
    ui::Button* thirdsButton = nullptr;
    ui::Button* gridButton = nullptr;     // "Grid 1/16 ▾": its menu (gridMenu) is filled as it opens
    ui::Menu* gridMenu = nullptr;
    std::map<std::string, std::unique_ptr<KeyAction>> gridActions;  // "finer", "coarser", "snap", "fixed"
    std::function<void(const GridChange&)> onGridChange;            // the Grid ▾ menu's choices (main window)
    // the song's key and scale (Key ▾) and swing (Swing ▾): their menus are filled as they open (main window,
    // ui/song_scale.h)
    ui::Button* keyButton = nullptr;
    ui::Menu* keyMenu = nullptr;
    ui::Button* swingButton = nullptr;
    ui::Menu* swingMenu = nullptr;
    std::function<void(ui::Menu&)> fillKeyMenu, fillSwingMenu;
    // what the transport follows (Sync ▾: Internal, MIDI clock, JACK; midi_sync.h): its menu filled as it opens (main
    // window, ui/midi_ports_dialog.h); its text "Sync: MIDI clock ▾" set by the main window
    ui::Button* syncButton = nullptr;
    ui::Menu* syncMenu = nullptr;
    std::function<void(ui::Menu&)> fillSyncMenu;
    ui::Button* moreButton = nullptr;     // "»"
    ui::Menu* moreMenu = nullptr;
    // fitting the bar to its width
    int level = 0;                        // how many of the fitting steps are taken
    bool compactTime = false, shortLabels = false, addCollapsed = false, addHidden = false, beatsShown = true;
    std::string gridName = "auto";        // as the grid button shows it ("" = no snap)
    static constexpr int LOSSLESS = 3;    // the first steps keep every control on the bar
    int stepCount() const { return (int)steps_.size(); }
    void apply(int level);                // the first `level` steps taken
    int needed() const;                   // how wide the parts' controls are at their narrowest, as they are now
    std::vector<int> measure(int width = 0);  // the width needed at each level, up to the first that fits
    void fit();                           // show as much as fits the bar's width, without overlaps
    int narrowestWidth();                 // the bar's narrowest with every control still on it
    void sizeTimeLabel();
    void showGridName();
    void fillMoreMenu(ui::Menu& menu);    // the » menu: the controls there's no room for, in the bar's order
    void askTempo();                      // » → Tempo… (the BPM box hidden)
    void askSignature();                  // » → Time signature…
    Point minimumSizeHint() const override;
    IconButton* recordButton;   // right-click (rightClicked): the main window opens its settings
    IconButton* playButton;
    std::vector<IconButton*> middleButtons;
    ui::Label* timeLabel;
    ui::SpinBox* bpmBox;
    ui::Button* tapButton;      // `pressed` is the tap (T presses it too)
    ui::Button* metronomeButton;  // checkable (Ctrl+M); right-click (rightClicked): its sounds
    ui::SpinBox* signatureBox;
    std::unique_ptr<TransportAction> beatsAction, thirdsAction;

    void showTiming(const Timing& timing);   // the project's tempo and grid settings, without change signals
    void setRecording(bool on);              // ● Rec lit (red) while recording
    void setPlaying(bool playing);
    void setTime(double seconds, const Timing& timing);

    void paint(Painter& p) override;         // the toolbar's background and the line under it
    Point sizeHint() const override;
    void resized() override;                 // fits it, and keeps the middle part centred

private:
    ui::Widget* left_;
    ui::Widget* middle_;
    ui::Widget* right_;
    std::unique_ptr<ui::Shortcut> tapShortcut_;
    std::vector<std::function<void(bool)>> steps_;
    int narrowest_ = -1;
    bool fitting_ = false;
    const Timing* timing_ = nullptr;      // as last shown (the grid menu is filled from it)
    void buildFitting();
    ui::Button* menuButton(ui::Widget* part, const std::string& text, ui::Menu** menu, const std::string& tip);
    std::unique_ptr<TransportAction> button(ui::Widget* part, const std::string& text, const std::string& shortText = "");
    std::unique_ptr<TransportAction> iconButton(const std::string& text, const std::string& icon,
                                                const std::string& glyph);
    void buildTimingControls();
    void arrange();
    ui::style::Insets frame() const;      // its border and padding together
};
