// tinydaw --tui (see app.h)
#include "tui/app.h"
#include "ui/song_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>

#include <fcntl.h>
#include <unistd.h>

#include "audio/automation.h"
#include "audio/file_io.h"
#include "config.h"
#include "core/util.h"
#include "model/auto_follow.h"
#include "model/edits.h"
#include "model/routing.h"
#include "paths.h"
#include "platform/platform.h"
#include "ui/autosave.h"
#include "ui/clipboard.h"
#include "ui/commands.h"
#include "ui/computer_keyboard.h"
#include "ui/recording.h"
#include "ui/record_arm.h"
#include "ui/transport_bar.h"
#include "ui/widgets.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/midi_editor.h"
#include "ui/synth_dialog.h"
#include "ui/plugin_host.h"
#include "ui/timeline.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"
#include "tui/window_view.h"

namespace tui {

namespace {

constexpr int EXPLORER_MIN = 18, EXPLORER_MAX = 30;  // its width (columns)
constexpr int PANEL_MIN = 32, PANEL_MAX = 40;
constexpr int METER_COLS = 6;
constexpr double FRAME_S = 1.0 / 30;  // the terminal is drawn (and the main window's frames run) this often

const Style kBar = style(Black, White);                 // the menu bar, the transport, the status bar
const Style kPane = style(Default, Default);
const Style kTitle = style(Default, Default, Bold);
// (a highlight reversed is given its colours the other way round, so reverse video shows them as meant: black on
// cyan, white on blue; and a terminal without colour still shows it reversed)
const Style kFocusTitle = style(Cyan, Black, Bold | Reverse);  // the title of the part with the keyboard: black on cyan
const Style kMenu = style(Black, White);
const Style kMenuHi = style(Blue, White, Bold | Reverse);      // the menu entry, the transport's control: white on blue
const Style kSelFocus = kFocusTitle;                           // the row chosen in the part with the keyboard
const Style kSelIdle = style(Default, Default, Reverse);       // …in the part without it

uint8_t groundOf(const Style& st) { return st.attr & Reverse ? st.fg : st.bg; }  // (the colour a cell shows behind its text)

double dbOf(double gain) { return gain > 1e-6 ? 20 * std::log10(gain) : -120; }

// a meter: how far its loudest channel reaches over MIN_DB … 0 dB, in cells
std::string meterBar(const LevelMeter* m, int width, bool utf8) {
    double level = 0;
    if (m)
        for (double v : m->levels()) level = std::max(level, v);
    double db = dbOf(level);
    double f = std::clamp((db - config::MIN_DB) / -config::MIN_DB, 0.0, 1.0);
    int n = (int)std::lround(f * width);
    std::string out;
    for (int i = 0; i < width; i++) out += i < n ? (utf8 ? "█" : "#") : (utf8 ? "·" : ".");
    if (m && m->clipped() && width > 0) out = out.substr(0, out.size() - (utf8 ? 2 : 1)) + "!";
    return out;
}

std::string clock(double seconds) {
    if (seconds < 0) seconds = 0;
    int m = (int)(seconds / 60);
    double s = seconds - m * 60;
    char buf[32];
    snprintf(buf, sizeof buf, "%d:%06.3f", m, s);
    return buf;
}

std::string dbText(double db) {
    if (db <= config::MIN_DB) return "-inf";
    char buf[16];
    snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}

uint8_t textOn(uint8_t bg) { return bg == Yellow || bg == Cyan || bg == Green || bg == White ? Black : White; }

}  // namespace

uint8_t nearestColor(const std::string& hex, uint8_t def) {
    if (hex.size() != 7 || hex[0] != '#') return def;
    int v = (int)strtol(hex.c_str() + 1, nullptr, 16);
    int r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255;
    struct C {
        uint8_t c;
        int r, g, b;
    } table[] = {{Red, 205, 49, 49},    {Green, 13, 188, 121}, {Yellow, 229, 229, 16}, {Blue, 36, 114, 200},
                 {Magenta, 188, 63, 188}, {Cyan, 17, 168, 205}, {White, 229, 229, 229}};
    int best = 1 << 30;
    uint8_t out = def;
    for (const C& c : table) {
        int d = (r - c.r) * (r - c.r) + (g - c.g) * (g - c.g) + (b - c.b) * (b - c.b);
        if (d < best) best = d, out = c.c;
    }
    return out;
}

std::string menuText(const std::string& text, char* mnemonic) {
    std::string out;
    if (mnemonic) *mnemonic = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '&' && i + 1 < text.size()) {
            if (text[i + 1] == '&') {
                out += '&';
                i++;
                continue;
            }
            if (mnemonic && !*mnemonic) *mnemonic = (char)toupper((unsigned char)text[i + 1]);
            continue;
        }
        out += text[i];
    }
    return out;
}

// --- the front end ---

Front::Front(MainWindow& w) : w_(w) {
    const char* home = getenv("HOME");
    showFolder(home && *home ? home : "/");
    w_.timeline->setFocus();
    secondsPerColumn = w_.project.timing.barSeconds(1) / 8;  // (a bar: 8 columns)
}

Front::~Front() = default;

bool Front::wantsQuit() const { return w_.closing; }

std::vector<TrackPtr> Front::tracks() const { return w_.timeline->visibleTracks(); }

TrackPtr Front::current() const {
    auto ts = tracks();
    if (ts.empty()) return nullptr;
    TrackPtr cur = w_.timeline->currentTrack;
    if (cur && std::find(ts.begin(), ts.end(), cur) != ts.end()) return cur;
    return ts[0];
}

void Front::setCurrent(const TrackPtr& t) {
    w_.timeline->currentTrack = t;
    w_.panel->showCurrentTrack();
    auto ls = lanes();
    for (int i = 0; i < (int)ls.size(); i++)
        if (ls[(size_t)i].track == t) laneRow = i;
}

std::vector<Lane> Front::lanes() const {
    std::vector<Lane> ls = w_.timeline->lanes();
    if (!ls.empty() && ls.back().isMaster()) ls.pop_back();
    return ls;
}

void Front::selectLane(int i) {
    auto ls = lanes();
    // (-1: the timeline's ruler; the track panel: one past the last is the master)
    laneRow = std::clamp(i, pane == Pane::Timeline ? -1 : 0, (int)ls.size());
    if (laneRow >= 0 && laneRow < (int)ls.size()) {
        const Lane& l = ls[(size_t)laneRow];
        TrackPtr t = l.track ? l.track : l.owner;
        if (t) {
            w_.timeline->currentTrack = t;
            w_.panel->showCurrentTrack();
        }
    }
}

std::optional<Lane> Front::currentLane() const {
    auto ls = lanes();
    if (laneRow >= 0 && laneRow < (int)ls.size()) return ls[(size_t)laneRow];
    return std::nullopt;
}

int Front::pointAtCursor(const Automation& a) const {
    double t = (double)w_.timeline->cursor / w_.engine.sr;
    const auto& pts = *a.points;
    for (int i = 0; i < (int)pts.size(); i++)
        if (std::fabs(pts[(size_t)i].t - t) < secondsPerColumn / 2) return i;
    return -1;
}

namespace {
bool isControls(const Lane& lane) { return lane.owner && lane.automation->slot == lane.owner->controls; }
}  // namespace

double Front::laneValue(const Lane& lane, double seconds) const {
    const Automation& a = *lane.automation;
    if (!a.points->empty()) return automation::valueAt(*a.points, seconds, a.targetSteps);
    // (no curve yet: the parameter as it is)
    if (isControls(lane)) return automation::controlValue(*lane.owner, a.param);
    if (a.hasTarget && a.targetPlugin) return a.targetPlugin->getParam(a.targetParam);
    return 0.5;
}

std::string Front::laneText(const Lane& lane, double value) const {
    const Automation& a = *lane.automation;
    if (isControls(lane)) return automation::controlText(a.param, value);
    if (a.hasTarget && a.targetPlugin) return a.targetPlugin->paramText(a.targetParam, value);
    return std::to_string((int)std::lround(value * 100)) + " %";
}

bool Front::automationKey(const Key& k, const Lane& lane) {
    Automation& a = *lane.automation;
    double t = (double)w_.timeline->cursor / w_.engine.sr;
    std::vector<AutoPoint> pts = a.points.copy();
    int at = pointAtCursor(a);
    double now = laneValue(lane, t);
    switch (k.key) {
    case plat::Key_Return:
    case plat::Key_Enter:  // a point at the cursor, on the curve as it is
        if (at < 0) pts.push_back(AutoPoint{t, now});
        break;
    case '[':
    case ']': {  // the curve at the cursor lower / higher (a point there)
        double d = k.key == '[' ? -0.05 : 0.05;
        if (at < 0) pts.push_back(AutoPoint{t, std::clamp(now + d, 0.0, 1.0)});
        else pts[(size_t)at].v = std::clamp(pts[(size_t)at].v + d, 0.0, 1.0);
        break;
    }
    case plat::Key_Delete:
        if (w_.timeline->selection.range && clipboard::clearLaneRange(*w_.timeline)) return true;  // (I / O: a range)
        if (at < 0) return true;
        pts.erase(pts.begin() + at);
        break;
    default: return false;
    }
    std::sort(pts.begin(), pts.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
    a.setPoints(std::move(pts));
    w_.timeline->update();
    w_.showStatus(a.param + ": " + laneText(lane, laneValue(lane, t)), 2000);
    return true;
}

void Front::showFolder(const std::string& path) {
    folder = path;
    entries.clear();
    for (auto& e : util::listDir(path)) {
        if (e.name.empty() || e.name[0] == '.') continue;
        std::string full = util::joinPath(path, e.name);
        if (e.isDir || file_io::isSample(full) || util::extension(full) == ".json") entries.push_back(e);
    }
    std::stable_partition(entries.begin(), entries.end(), [](const util::DirEntry& e) { return e.isDir; });
    entry = 0;
    entryTop = 0;
}

bool Front::drawsWindow(ui::Window* win) const {
    // (F12 hides the open windows of their own; dialogs waiting for an answer, and menus, stay)
    return win != &w_ && win->isWindowShown() && (!windowsHidden || win->isModal() || ui::Menu::ofWindow(win));
}

ui::Window* Front::topWindow() const {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (drawsWindow(*it)) return *it;
    return nullptr;
}

int Front::openWindows() const {
    int n = 0;
    for (ui::Window* win : ui::App::get().windows())
        if (win != &w_ && win->isWindowShown() && !win->isModal() && !ui::Menu::ofWindow(win)) n++;
    return n;
}

// --- keys ---

void Front::dropGoneMenus() {
    // a menu a control opened (its exec()): gone with its popup (then destroyed: never touched again)
    if (open.empty() || barMenu >= 0 || ownMenu_) return;
    for (ui::Window* win : ui::App::get().windows())
        if (win->isWindowShown() && ui::Menu::ofWindow(win) == open[0].menu) return;
    open.clear();
}

void Front::closeUnder() {
    // (a window opened by a timer, a recording ending, autosave's recovery offer… while F1's list or the bar's menu
    // was up: it takes the keys, so they go, rather than staying drawn over it or open under it, deaf)
    ui::Window* top = topWindow();
    if (top && top != lastTop_ && !ui::Menu::ofWindow(top)) {
        helpShown = false;
        if (barMenu >= 0 || ownMenu_) closeMenus();
    }
    lastTop_ = top;
}

void Front::key(const Key& k) {
    dropGoneMenus();
    closeUnder();
    {  // the automation lane with the keyboard is the timeline's current one (its range: Ctrl+C / X / V, Del)
        std::optional<Lane> lane = pane == Pane::Timeline ? currentLane() : std::nullopt;
        w_.timeline->currentAutomation = lane && lane->automation ? lane->automation : nullptr;
    }
    // F12: the open windows (an editor, an effect's) hidden, the main window's keys; again, back
    int n = openWindows();
    if (n > shownWindows_) windowsHidden = false;  // (one just opened: shown)
    shownWindows_ = n;
    if (k.key == plat::Key_F12 && !k.mods && (n || windowsHidden)) {
        windowsHidden = !windowsHidden;
        return;
    }
    // a dialog, a popup, or another of the program's windows over the main window: it has the keyboard
    if (ui::Window* top = topWindow()) {
        if (ui::Menu* m = ui::Menu::ofWindow(top)) {  // (a menu a control opened: drawn as the bar's are)
            if (open.empty() || open[0].menu != m) {
                closeMenus();
                open.push_back({m, -1});
                barMenu = -1;
                Key down{plat::Key_Down};
                menuKey(down);
            }
            if (!menuKey(k)) {
            }
            return;
        }
        if (k.key == 'W' && k.mods == plat::ModCtrl) {  // Ctrl+W: closed, as by its ✕ (Esc is the window's own key)
            top->close();
            return;
        }
        sendKey(*top, k);
        return;
    }
    if (helpShown) {
        if (k.key == plat::Key_Down || k.key == plat::Key_Up || k.key == plat::Key_PageDown || k.key == plat::Key_PageUp) {
            int step = (k.key == plat::Key_PageDown || k.key == plat::Key_PageUp) ? 10 : 1;
            helpTop += (k.key == plat::Key_Down || k.key == plat::Key_PageDown) ? step : -step;  // (drawHelp keeps it in range)
            return;
        }
        helpShown = false;
        return;
    }
    if (menuOpen()) {
        menuKey(k);
        return;
    }
    // the computer keyboard while it is on: its letters play the current track (as in the window)
    if (!k.mods && pane != Pane::Explorer) {
        std::vector<int> keys = computer_keyboard::keysInUse();
        if (std::find(keys.begin(), keys.end(), k.key) != keys.end()) {
            sendKey(w_, k);
            return;
        }
    }
    // the whole window's
    if (k.key == plat::Key_F10 && !k.mods) {
        openBarMenu(0);
        return;
    }
    if ((k.mods & plat::ModAlt) && k.key < 128 && isalpha(k.key)) {
        const auto& menus = w_.menuBar->menus();
        for (int i = 0; i < (int)menus.size(); i++) {
            char m = 0;
            menuText(menus[i]->title, &m);
            if (m == (char)toupper(k.key)) {
                openBarMenu(i);
                return;
            }
        }
    }
    if (k.key == plat::Key_F1) {
        helpShown = true;
        helpTop = 0;
        lastTop_ = topWindow();  // (a window opening after it: it closes)
        return;
    }
    if (k.key == plat::Key_F2 && !k.mods) {
        contextMenu();
        return;
    }
    if (k.key == plat::Key_Tab || k.key == plat::Key_F6) {
        moveFocus((k.mods & plat::ModShift) ? -1 : 1);
        return;
    }
    if (k.key == plat::Key_Backtab) {
        moveFocus(-1);
        return;
    }
    if (k.key == 'Q' && k.mods == plat::ModCtrl) {
        w_.requestClose();
        return;
    }
    if (k.key == 'P' && k.mods == plat::ModCtrl) {  // every command by name (the window's Ctrl+Shift+P: consoles can't send it)
        util::debugf("tui: the command palette\n");
        w_.showPalette();
        return;
    }
    bool used = false;
    switch (pane) {
    case Pane::Transport: used = transportKey(k); break;
    case Pane::Explorer: used = explorerKey(k); break;
    case Pane::Timeline: used = timelineKey(k); break;
    case Pane::Panel: used = panelKey(k); break;
    }
    if (!used) forward(k);
}

void Front::forward(const Key& k) {
    // the main window's shortcuts wherever the keyboard is (Ctrl+S, Ctrl+Z, R records, F follows, T taps…); in the
    // timeline also its own keys (S, Del, C, Ctrl+D…). Elsewhere nothing has the focus: only shortcuts take them.
    if (pane == Pane::Timeline) w_.timeline->setFocus();
    else if (ui::Widget* f = w_.focusWidget()) f->clearFocus();
    sendKey(w_, k);
}

void Front::moveFocus(int dir) {
    const Pane order[] = {Pane::Transport, Pane::Explorer, Pane::Timeline, Pane::Panel};
    int i = 0;
    for (int j = 0; j < 4; j++)
        if (order[j] == pane) i = j;
    for (int step = 0; step < 4; step++) {
        i = (i + dir + 4) % 4;
        if (order[i] == Pane::Explorer && !w_.explorerDock->isVisible()) continue;
        break;
    }
    pane = order[i];
    if (pane == Pane::Timeline) w_.timeline->setFocus();
}

// the transport's controls: Play, go to the start, Loop, Metronome, tempo, beats per bar, grid, snap
// (Record, Loop, Follow, Keys: the bar's own actions, as its buttons and shortcuts do)
// (Key and Swing: the song's key and swing, ui/song_scale.h; Sync: what the transport follows, audio/midi_sync.h)
enum { C_PLAY, C_RECORD, C_START, C_LOOP, C_FOLLOW, C_METRONOME, C_KEYS, C_TEMPO, C_BEATS, C_GRID, C_SNAP, C_KEY, C_SWING,
       C_SYNC, C_COUNT };

bool Front::transportKey(const Key& k) {
    Timing& t = w_.project.timing;
    bool enter = k.key == plat::Key_Return || k.key == plat::Key_Enter;
    if (k.key == plat::Key_Left) control = (control + C_COUNT - 1) % C_COUNT;
    else if (k.key == plat::Key_Right) control = (control + 1) % C_COUNT;
    else if (k.key == plat::Key_Up || k.key == plat::Key_Down || enter) {
        int d = k.key == plat::Key_Up ? 1 : k.key == plat::Key_Down ? -1 : 0;
        switch (control) {
        case C_PLAY:
            if (enter) w_.togglePlay();
            break;
        case C_RECORD:
            if (enter) w_.transport->recordAction->trigger();
            break;
        case C_START:
            if (enter) w_.transport->startAction->trigger();
            break;
        case C_LOOP:
            if (enter) w_.transport->loopAction->trigger();
            break;
        case C_FOLLOW:
            if (enter) w_.transport->followAction->trigger();
            break;
        case C_KEYS:
            if (enter && w_.transport->keysAction) w_.transport->keysAction->trigger();
            break;
        case C_METRONOME:
            if (enter) w_.setMetronomeOn(!w_.prefs.metronome);
            break;
        case C_TEMPO:
            if (d) {
                MainWindow::TimingChange c;
                c.bpm = std::clamp(std::round(t.bpm) + d, (double)config::MIN_BPM, (double)config::MAX_BPM);
                w_.setTiming(c);
            } else {
                char buf[32];
                snprintf(buf, sizeof buf, "%g", t.bpm);
                std::string text = buf;
                if (ui::getText(&w_, "Tempo", "Beats per minute:", text)) {
                    char* end = nullptr;
                    double v = strtod(text.c_str(), &end);
                    if (end != text.c_str() && v >= config::MIN_BPM && v <= config::MAX_BPM) {
                        MainWindow::TimingChange c;
                        c.bpm = v;
                        w_.setTiming(c);
                    } else {
                        w_.showStatus("The tempo is a number from " + std::to_string((int)config::MIN_BPM) + " to " +
                                          std::to_string((int)config::MAX_BPM),
                                      4000);
                    }
                }
            }
            break;
        case C_BEATS:
            if (d) {
                MainWindow::TimingChange c;
                c.beatsPerBar = std::clamp(t.beatsPerBar + d, 1, 32);
                w_.setTiming(c);
            }
            break;
        case C_GRID:
            if (d) w_.changeGrid(std::nullopt, std::nullopt, d > 0);
            else w_.changeGrid(0.0);  // (Enter: back to following the zoom)
            break;
        case C_SNAP:
            if (enter) w_.changeGrid(std::nullopt, !t.snapOn);
            break;
        case C_KEY:  // Up / Down: the next key (a major one when there was no scale); Enter: its menu
            if (d) {
                t.key = ((t.key + d) % 12 + 12) % 12;
                if (t.scale.empty()) t.scale = "Major";
                util::debugf("tui: the song's key %s\n", song_scale::scaleName(t.key, t.scale).c_str());
                w_.transport->showTiming(t);
            } else {
                contextMenu();
            }
            break;
        case C_SWING:  // Up / Down: 5 % more or less; Enter: its menu
            if (d) {
                song_scale::setSongSwing(w_.project, w_.engine.sr, std::clamp(std::round(t.swing / 5) * 5 + 5 * d, 0.0, 100.0));
                w_.transport->showTiming(t);
            } else {
                contextMenu();
            }
            break;
        case C_SYNC:  // Up / Down: the next of Internal, MIDI clock, JACK; Enter: its menu
            if (d) w_.setSync((midi_sync::Mode)(((int)w_.syncMode + d + 3) % 3));
            else contextMenu();
            break;
        }
    } else {
        return false;
    }
    return true;
}

bool Front::explorerKey(const Key& k) {
    int n = (int)entries.size();
    auto pathOf = [&](int i) { return util::joinPath(folder, entries[(size_t)i].name); };
    switch (k.key) {
    case plat::Key_Up: entry = std::max(0, entry - 1); return true;
    case plat::Key_Down: entry = std::max(0, std::min(n - 1, entry + 1)); return true;  // (an empty folder: 0)
    case plat::Key_PageUp: entry = std::max(0, entry - 10); return true;
    case plat::Key_PageDown: entry = std::max(0, std::min(n - 1, entry + 10)); return true;
    case plat::Key_Home: entry = 0; return true;
    case plat::Key_End: entry = std::max(0, n - 1); return true;
    case plat::Key_Left:
    case plat::Key_Backspace: {
        std::string up = util::dirName(folder);
        std::string was = util::baseName(folder);
        if (up.empty() || up == folder) return true;
        showFolder(up);
        for (int i = 0; i < (int)entries.size(); i++)
            if (entries[(size_t)i].name == was) entry = i;
        return true;
    }
    case plat::Key_Right:
    case plat::Key_Return:
    case plat::Key_Enter: {
        if (entry < 0 || entry >= n) return true;
        std::string path = pathOf(entry);
        if (entries[(size_t)entry].isDir) {
            showFolder(path);
        } else if (k.key != plat::Key_Right) {
            if (util::extension(path) == ".json") {
                if (w_.confirmDiscard()) w_.openProjectFile(path);
            } else {
                w_.timeline->addFile(path, w_.timeline->cursor);
                w_.showStatus("Added " + util::baseName(path), 3000);
            }
        }
        return true;
    }
    case plat::Key_Space:
    case 'P': {  // hear it (again: stop)
        if (w_.engine.previewVoice()) {
            w_.engine.stopPreview();
            return true;
        }
        if (entry < 0 || entry >= n || entries[(size_t)entry].isDir) return true;
        AudioPtr audio;
        std::string error;
        if (file_io::loadPlayable(pathOf(entry), w_.engine.sr, audio, error)) w_.engine.preview(audio);
        else w_.showStatus(error, 4000);
        return true;
    }
    case plat::Key_Escape: w_.engine.stopPreview(); return true;
    default: return false;
    }
}

double Front::stepBeats() const {
    const Timing& t = w_.project.timing;
    double cursorS = (double)w_.timeline->cursor / w_.engine.sr;
    double pps = 8.0 / std::max(1e-6, secondsPerColumn);  // (a column as 8 pixels: what the grid follows)
    double step = t.gridStep(pps, cursorS).value();
    return step > 0 ? step : 1;
}

void Front::moveCursor(double seconds) {
    int sr = w_.engine.sr;
    int64_t f = std::max<int64_t>(0, (int64_t)std::llround(seconds * sr));
    w_.timeline->setCursor(f);
    if (!w_.engine.playing) w_.engine.seek(f);
    keepCursorInView();
}

void Front::keepCursorInView() {
    double t = (double)w_.timeline->cursor / w_.engine.sr;
    double span = std::max(1, timelineCols_) * secondsPerColumn;
    if (t < viewStart) viewStart = std::max(0.0, t - span / 4);
    else if (t >= viewStart + span) viewStart = t - span * 3 / 4;
}

void Front::moveSelection(int64_t df, int rows) {
    Timeline& tl = *w_.timeline;
    std::vector<ClipPtr> clips(tl.selection.clips.begin(), tl.selection.clips.end());
    if (clips.empty()) {
        w_.showStatus("Select clips first: Enter selects the clip at the cursor", 3000);
        return;
    }
    auto ts = tracks();
    auto rowOf = [&](const TrackPtr& t) { return (int)(std::find(ts.begin(), ts.end(), t) - ts.begin()); };
    for (auto& c : clips) {
        df = std::max(df, -c->start);
        int row = rowOf(c->track) + rows;
        if (rows && !(row >= 0 && row < (int)ts.size() && ts[(size_t)row]->kind == c->kind && !ts[(size_t)row]->frozen)) {
            w_.showStatus("The clips can't go there (another kind of track, or none)", 3000);
            return;
        }
    }
    std::map<ClipPtr, TrackPtr> before;
    std::map<ClipPtr, std::optional<int64_t>> starts;
    std::vector<auto_follow::Move> follow;
    for (auto& c : clips) {
        before[c] = c->track;
        starts[c] = c->start;
        c->start += df;
        if (rows) std::atomic_store(&c->track, ts[(size_t)(rowOf(c->track) + rows)]);  // (the audio thread atomic_loads it)
        follow.push_back(auto_follow::moveOf(*c, before[c], *starts[c], w_.engine.sr));
    }
    edits::clearUnder(w_.project, clips);  // moved clips replace what they now cover
    if (auto_follow::carry(w_.project, follow, false)) tl.lanesChanged();  // (their automation along)
    tl.rerenderMoved(before, starts);
    tl.relayout();
    tl.update();
}

bool Front::timelineKey(const Key& k) {
    Timeline& tl = *w_.timeline;
    const Timing& t = w_.project.timing;
    int sr = w_.engine.sr;
    double cursorS = (double)tl.cursor / sr;
    auto ts = tracks();
    TrackPtr cur = current();
    int row = cur ? (int)(std::find(ts.begin(), ts.end(), cur) - ts.begin()) : 0;
    std::optional<Lane> lane = currentLane();
    // the ruler's (Up from the first lane): Enter edits the tempo change at the cursor (its tempo, ramp, time
    // signature), else makes one at the cursor's bar
    if (laneRow < 0 && !k.mods && (k.key == plat::Key_Return || k.key == plat::Key_Enter)) {
        if (auto point = tl.tempoPointNear(cursorS, std::max(0.5, secondsPerColumn / 2 * tl.pps)))
            tl.editTempoPoint(*point);
        else
            tl.editTempo(t.barAt(cursorS + 1e-9) + 1);
        return true;
    }
    // an automation lane's, a take's, a group header's own keys
    if (lane && lane->automation && !k.mods && automationKey(k, *lane)) return true;
    if (lane && lane->take && !k.mods) {
        if (k.key == plat::Key_Return || k.key == plat::Key_Enter) {  // used: all of it, or the range selected
            if (tl.selection.range) tl.comp(lane->owner, lane->take, tl.selection.range->start, tl.selection.range->end);
            else tl.useTake(lane->owner, lane->take);
            return true;
        }
        if (k.key == plat::Key_Delete) {
            tl.deleteTake(lane->owner, lane->take);
            return true;
        }
    }
    if (lane && lane->group && !k.mods && (k.key == plat::Key_Return || k.key == plat::Key_Enter)) {
        tl.toggleFold(lane->group);
        return true;
    }
    auto byGrid = [&](int dir) {
        double step = stepBeats(), beat = t.beatAt(cursorS);
        double snapped = std::round(beat / step) * step;
        double target = std::fabs(snapped - beat) > 1e-6 && ((snapped - beat) * dir > 0) ? snapped : snapped + dir * step;
        moveCursor(t.secondsAt(std::max(0.0, target)));
    };
    if (k.mods & plat::ModCtrl) {  // (xterm's Ctrl+arrows: a bar)
        if (k.key == plat::Key_Left || k.key == plat::Key_Right) {
            int bar = t.barAt(cursorS + 1e-9);
            double start = t.barSeconds(bar);
            moveCursor(k.key == plat::Key_Left ? (cursorS - start > 1e-6 ? start : t.barSeconds(std::max(0, bar - 1)))
                                               : t.barSeconds(bar + 1));
            return true;
        }
        return false;
    }
    switch (k.key) {
    case plat::Key_Left: byGrid(-1); return true;
    case plat::Key_Right: byGrid(1); return true;
    case plat::Key_PageUp: {
        int bar = t.barAt(cursorS + 1e-9);
        double start = t.barSeconds(bar);
        moveCursor(cursorS - start > 1e-6 ? start : t.barSeconds(std::max(0, bar - 1)));
        return true;
    }
    case plat::Key_PageDown: moveCursor(t.barSeconds(t.barAt(cursorS + 1e-9) + 1)); return true;
    case plat::Key_Home: moveCursor(0); return true;
    case plat::Key_End: moveCursor((double)w_.project.endFrame() / sr); return true;
    case plat::Key_Up:
        selectLane(std::max(-1, laneRow - 1));  // (above the first lane: the ruler)
        return true;
    case plat::Key_Down:
        selectLane(std::min((int)lanes().size() - 1, laneRow + 1));
        return true;
    case 'M':  // a marker at the cursor
        if (k.mods) return false;
        tl.addMarker(tl.cursor);
        return true;
    case '+':
    case '=':
        secondsPerColumn = std::max(0.002, secondsPerColumn / 2);
        keepCursorInView();
        return true;
    case '-':
        secondsPerColumn = std::min(60.0, secondsPerColumn * 2);
        keepCursorInView();
        return true;
    case plat::Key_Return:
    case plat::Key_Enter:
    case plat::Key_Insert: {  // the clip at the cursor: alone (Enter), or into the selection (Insert)
        ClipPtr at;
        if (cur)
            for (auto& c : w_.project.clipsOn(cur))
                if (c->start <= tl.cursor && tl.cursor < c->end()) at = c;
        if (!at) {
            if (k.key != plat::Key_Insert) tl.clearSelection();
            w_.showStatus("No clip at the cursor on this track", 2000);
        } else if (k.key == plat::Key_Insert) {
            tl.selection.toggle(at);
        } else {
            tl.selection.selectOnly(at);
        }
        tl.update();
        return true;
    }
    case 'I':
        if (k.mods) return false;
        markFrame = tl.cursor;
        markTrack = cur;
        w_.showStatus("Range from here: move the cursor, then O", 3000);
        return true;
    case 'O': {
        if (k.mods) return false;
        if (markFrame < 0 || !cur) {
            w_.showStatus("Press I where the range starts first", 3000);
            return true;
        }
        int64_t a = std::min(markFrame, tl.cursor), b = std::max(markFrame, tl.cursor);
        int r0 = row, r1 = row;
        for (int i = 0; i < (int)ts.size(); i++)
            if (ts[(size_t)i] == markTrack) r0 = i;
        std::vector<TrackPtr> rangeTracks;
        for (int i = std::min(r0, r1); i <= std::max(r0, r1); i++)
            if (!ts[(size_t)i]->frozen) rangeTracks.push_back(ts[(size_t)i]);
        if (a == b || rangeTracks.empty()) {
            tl.selection.range.reset();
        } else {
            tl.selection.setRange(TimeRange{a, b, rangeTracks}, w_.project.clipsIn(rangeTracks, a, b));
            tl.setCursor(a);
        }
        tl.update();
        return true;
    }
    case ',': moveSelection(-(int64_t)std::llround((cursorS - t.secondsAt(std::max(0.0, t.beatAt(cursorS) - stepBeats()))) * sr), 0); return true;
    case '.': moveSelection((int64_t)std::llround((t.secondsAt(t.beatAt(cursorS) + stepBeats()) - cursorS) * sr), 0); return true;
    case '[': moveSelection(0, -1); return true;
    case ']': moveSelection(0, 1); return true;
    case 'E':
    case 'N': {  // E: the MIDI editor of the clip selected (or at the cursor); N: a new MIDI clip at the cursor, edited
        if (k.mods) return false;
        if (!cur) return true;
        ClipPtr clip;
        if (k.key == 'N') {
            if (cur->kind != Kind::Midi) {
                w_.showStatus("New MIDI clips go on MIDI tracks (Tab to the track panel, N makes one)", 3000);
                return true;
            }
            clip = tl.newMidiClip(cur, tl.cursor);
        } else {
            for (auto& c : tl.selection.clips)
                if (c->kind == Kind::Midi) clip = c;
            if (!clip)
                for (auto& c : w_.project.clipsOn(cur))
                    if (c->kind == Kind::Midi && c->start <= tl.cursor && tl.cursor < c->end()) clip = c;
        }
        if (clip) w_.openMidiEditor(clip);
        else w_.showStatus("No MIDI clip selected or at the cursor (N makes one)", 3000);
        return true;
    }
    case 'L': {
        if (k.mods) return false;
        if (tl.selection.range) {  // the range becomes the loop
            w_.project.timing.loopStart = (double)tl.selection.range->start / sr;
            w_.project.timing.loopEnd = (double)tl.selection.range->end / sr;
            w_.setLooping(true);
        } else {
            w_.setLooping(!t.looping);
        }
        return true;
    }
    default: return false;
    }
}

bool Front::panelKey(const Key& k) {
    auto ls = lanes();
    int n = (int)ls.size();
    laneRow = std::clamp(laneRow, 0, n);
    bool master = laneRow == n;
    auto changed = [&] {
        w_.panel->rebuild();
        w_.timeline->update();
    };
    if (k.key == plat::Key_Up || k.key == plat::Key_Down) {
        int d = k.key == plat::Key_Up ? -1 : 1;
        TrackPtr cur = current();
        if (k.mods == plat::ModAlt) {  // the track (the selected ones) a lane up or down
            if (cur && !master) {
                w_.panel->move(cur, d);
                setCurrent(cur);
            }
            return true;
        }
        if (k.mods == plat::ModShift) {  // the selection from the track it started at to the next one
            int j = laneRow + d;
            if (j >= 0 && j < n && !master) {
                const Lane& l = ls[(size_t)j];
                if (TrackPtr to = l.track ? l.track : l.owner) w_.panel->select(to, plat::ModShift);
                selectLane(j);
            }
            return true;
        }
        if (k.mods) return false;
        w_.panel->selected.clear();  // (only the track it moves to)
        selectLane(laneRow + d);
        return true;
    }
    // an automation lane's row: A another lane, W write it while playing, Del remove it; a group's: Enter renames
    if (!master && ls[(size_t)laneRow].automation && !k.mods) {
        const Lane& l = ls[(size_t)laneRow];
        switch (k.key) {
        case 'A': w_.panel->addAutomation(l.owner); return true;
        case 'W': l.automation->armed = !l.automation->armed; changed(); return true;
        case plat::Key_Delete: w_.timeline->removeAutomation(l.owner, l.automation); return true;
        default: return false;
        }
    }
    if (!master && ls[(size_t)laneRow].group) {
        GroupPtr g = ls[(size_t)laneRow].group;
        if (k.key == plat::Key_Return || k.key == plat::Key_Enter) {
            w_.panel->rename(g);
            return true;
        }
        // a group's row as a track's (model.h GROUP BUSES): M S its mute and solo; with a bus, Left/Right its fader,
        // [ ] its pan, E its effects, U its automation lanes, D its sends
        if (k.mods) return false;
        if (k.key == 'M' || k.key == 'S') {
            bool& flag = k.key == 'M' ? g->muted : g->soloed;
            flag = !flag;
            util::debugf("tui: group %s %s %s", g->name.c_str(), k.key == 'M' ? "mute" : "solo", flag ? "on" : "off");
            changed();
            w_.showStatus(g->name + (k.key == 'M' ? (flag ? ": muted" : ": unmuted") : (flag ? ": soloed" : ": unsoloed")), 3000);
            return true;
        }
        if (!routing::busOn(*g)) return false;
        TrackPtr bus = g->bus;
        switch (k.key) {
        case plat::Key_Left:
        case plat::Key_Right:
            bus->volumeDb = std::clamp(std::round(bus->volumeDb) + (k.key == plat::Key_Left ? -1 : 1), (double)config::MIN_DB,
                                       (double)config::MAX_DB);
            changed();
            w_.showStatus(g->name + " bus: " + dbText(bus->volumeDb) + " dB", 2000);
            return true;
        case '[':
        case ']':
            bus->pan = std::clamp(std::round(bus->pan * 10 + (k.key == '[' ? -1 : 1)) / 10, -1.0, 1.0);
            changed();
            return true;
        case 'E': w_.openTrackPlugins(bus); return true;
        case 'U': w_.panel->toggleAutomation(bus, !bus->showAutomation); return true;
        case 'D': {
            std::function<SendsButton*(ui::Widget*)> find = [&](ui::Widget* w) -> SendsButton* {
                if (auto* b = dynamic_cast<SendsButton*>(w))
                    if (b->track == bus) return b;
                for (auto& c : w->children())
                    if (SendsButton* b = find(c.get())) return b;
                return nullptr;
            };
            if (SendsButton* b = find(w_.panel)) b->showSends();
            else w_.showStatus("No return tracks to send to: Ctrl+Alt+T (or F2: Add track) makes one", 3000);
            return true;
        }
        default: return false;
        }
    }
    if (!master && ls[(size_t)laneRow].take) return false;
    TrackPtr t = master ? nullptr : ls[(size_t)laneRow].track;
    // (mute, solo, volume, pan: every selected track's: lane_panel.h TRACK SELECTION)
    std::vector<TrackPtr> ts = t ? w_.panel->targets(t) : std::vector<TrackPtr>{};
    if (k.key == 'D' && k.mods == plat::ModCtrl) {  // Ctrl+D: duplicate it (them)
        if (t) w_.panel->duplicate(t, true);
        if (w_.timeline->currentTrack) setCurrent(w_.timeline->currentTrack);  // (the keyboard on the first copy)
        return true;
    }
    // KEYBOARD FOCUS (ui/midi_input.h): Shift+K the keyboards play it too, or no more (♪ before its name)
    if (t && k.key == 'K' && k.mods == plat::ModShift) {
        if (t->kind != Kind::Midi) {
            w_.showStatus("Only MIDI tracks are played by the keyboards", 3000);
            return true;
        }
        auto plays = midi_input::keyboardTracks(w_.project, w_.timeline->currentTrack);
        bool on = std::find(plays.begin(), plays.end(), t) == plays.end();
        w_.panel->toggleFocus(t, on);
        if (!w_.project.keyFocusLocked)
            w_.showStatus(w_.status->message() + "  (F2: Lock keyboard focus keeps them while you move to other tracks)",
                          5000);
        return true;
    }
    // RECORDING SEVERAL TRACKS (ui/recording.h): Shift+R arms (R stays Record), H its monitoring, Shift+H its input
    if (t && k.key == 'R' && k.mods == plat::ModShift) {
        if (t->kind == Kind::Return) return true;
        w_.panel->armTrack(t, !t->armed, true);  // (more: the others stay armed)
        util::debugf("tui: %s %s", t->name.c_str(), t->armed ? "armed" : "disarmed");
        w_.showStatus(t->name + (t->armed ? ": armed: ● Rec (R) records it, with the other armed tracks"
                                          : ": disarmed"), 3000);
        return true;
    }
    if (t && k.key == 'H' && (k.mods == 0 || k.mods == plat::ModShift)) {
        if (t->kind != Kind::Audio) {
            w_.showStatus("Only audio tracks hear an input (a MIDI track plays what reaches it)", 3000);
            return true;
        }
        if (k.mods == plat::ModShift) {  // its input: a menu of the device's
            auto menu = std::make_unique<ui::Menu>();
            std::string own = recording::inputSetting(*t);
            for (auto& [label, setting] : recording::trackInputChoices(w_.panel->inputs)) {
                std::string chosen = setting;
                menu->addCheckable("Input: " + label, setting == own, [this, t, chosen](bool) {
                    for (auto& x : w_.panel->targets(t))
                        if (x->kind == Kind::Audio) recording::setInput(*x, chosen);
                    w_.panel->rebuild();
                    w_.showStatus(t->name + ": input " + recording::inputName(*t), 3000);
                });
            }
            showOwnMenu(std::move(menu));
            return true;
        }
        Track::Monitor m = record_arm::nextMonitor(t->monitor);
        for (auto& x : ts)
            if (x->kind == Kind::Audio) x->monitor = m;
        util::debugf("tui: %s monitoring %s", t->name.c_str(), record_arm::monitorName(m));
        changed();
        w_.showStatus(t->name + ": monitoring " + record_arm::monitorName(m) +
                          (m == Track::Monitor::In ? " (always hears its input)"
                           : m == Track::Monitor::Off ? " (never hears its input)" : " (hears its input while armed)"),
                      3000);
        return true;
    }
    switch (k.key) {
    case 'M':
        if (k.mods) return false;
        if (master) {
            w_.master->setMute(!w_.engine.masterMute);
        } else {
            bool on = !t->muted;
            for (auto& x : ts) x->muted = on;
            changed();
            w_.panel->say(t->muted ? "Mute" : "Unmute", ts.size());
        }
        return true;
    case 'S': {  // solo; Shift+S: solo safe (heard while other tracks are soloed); on every selected track
        if ((k.mods & ~plat::ModShift) || master) return false;
        bool safe = k.mods & plat::ModShift;
        bool on = safe ? !t->soloSafe : !t->soloed;
        for (auto& x : ts) (safe ? x->soloSafe : x->soloed) = on;
        util::debugf("tui: %s %s %s", t->name.c_str(), safe ? "solo safe" : "solo", on ? "on" : "off");
        changed();
        w_.panel->say(safe ? (on ? "Solo safe" : "Not solo safe") : (on ? "Solo" : "Unsolo"), ts.size());
        return true;
    }
    case plat::Key_Left:
    case plat::Key_Right: {
        int d = k.key == plat::Key_Left ? -1 : 1;
        if (master) {
            w_.master->setVolume(std::clamp(w_.master->volume->value() + d, config::MIN_DB, config::MAX_DB));
        } else {
            for (auto& x : ts)
                x->volumeDb = std::clamp(std::round(x->volumeDb) + d, (double)config::MIN_DB, (double)config::MAX_DB);
            changed();
            w_.panel->say("Volume", ts.size());
        }
        return true;
    }
    case '[':
    case ']':
        if (master) return true;
        for (auto& x : ts) x->pan = std::clamp(std::round(x->pan * 10 + (k.key == '[' ? -1 : 1)) / 10, -1.0, 1.0);
        changed();
        w_.panel->say("Pan", ts.size());
        return true;
    case plat::Key_Return:
    case plat::Key_Enter:
        if (t) w_.panel->rename(t);
        return true;
    case 'A':
    case 'N': {  // a new audio (A) or MIDI (N) track below this one
        if (k.mods) return false;
        TrackPtr made = w_.timeline->addTrack(k.key == 'A' ? Kind::Audio : Kind::Midi, nullptr, t);
        if (made) setCurrent(made);
        return true;
    }
    case plat::Key_Delete:
        if (t) w_.timeline->removeTrack(t);
        return true;
    // the row's buttons: E its effects (FX), I its instrument (Shift+I: edit it), U its automation lanes (Auto),
    // K its takes, O the next output pair
    case 'E':
        if (k.mods) return false;
        if (master) w_.openMasterPlugins();
        else w_.openTrackPlugins(t);
        return true;
    case 'I':
        if (master || t->kind != Kind::Midi || (k.mods & ~plat::ModShift)) return false;
        if (k.mods & plat::ModShift) {
            w_.editInstrument(t);
        } else {
            auto menu = std::make_unique<ui::Menu>();
            w_.fillInstrumentMenu(*menu, t);
            showOwnMenu(std::move(menu));
        }
        return true;
    case 'U':
        if (k.mods || master) return false;
        w_.panel->toggleAutomation(t, !t->showAutomation);
        return true;
    case 'K':
        if (k.mods || master) return false;
        w_.panel->toggleTakes(t);
        return true;
    case 'D': {  // its sends to the return tracks (the row's Sends button)
        if (k.mods || master) return false;
        std::function<SendsButton*(ui::Widget*)> find = [&](ui::Widget* w) -> SendsButton* {
            if (auto* b = dynamic_cast<SendsButton*>(w))
                if (b->track == t) return b;
            for (auto& c : w->children())
                if (SendsButton* b = find(c.get())) return b;
            return nullptr;
        };
        if (SendsButton* b = find(w_.panel)) b->showSends();
        else w_.showStatus("No return tracks to send to: Ctrl+Alt+T (or F2: Add track) makes one", 3000);
        return true;
    }
    case 'O':
        if (k.mods || master) return false;
        t->output = w_.engine.channels > 2 ? (t->output + 2) % (w_.engine.channels & ~1) : 0;
        changed();
        w_.showStatus(t->name + " plays on outputs " + std::to_string(t->output + 1) + "/" + std::to_string(t->output + 2), 3000);
        return true;
    default: return false;
    }
}

void Front::showOwnMenu(std::unique_ptr<ui::Menu> menu) {
    if (!menu || menu->isEmpty()) return;
    closeMenus();
    ownMenu_ = std::move(menu);
    lastTop_ = topWindow();  // (a window opening after it: it closes)
    open.push_back({ownMenu_.get(), -1});
    menuKey(Key{plat::Key_Down});
}

// --- menus ---

void Front::openBarMenu(int i) {
    const auto& menus = w_.menuBar->menus();
    if (menus.empty()) return;
    i = (i % (int)menus.size() + (int)menus.size()) % (int)menus.size();
    closeMenus();
    barMenu = i;
    lastTop_ = topWindow();  // (a window opening after it: it closes)
    ui::Menu* m = menus[(size_t)i].get();
    if (m->aboutToShow) m->aboutToShow(m);
    open.push_back({m, -1});
    menuKey(Key{plat::Key_Down});
}

void Front::closeMenus() {
    for (auto it = open.rbegin(); it != open.rend(); ++it)
        if (it->menu->aboutToHide) it->menu->aboutToHide();
    open.clear();
    barMenu = -1;
    ownMenu_.reset();
}

void Front::contextMenu() {
    if (pane == Pane::Transport) {
        if (control == C_RECORD) w_.recording->showSettings();
        else if (control == C_GRID) {
            auto menu = std::make_unique<ui::Menu>();
            w_.fillGridMenu(menu.get());
            showOwnMenu(std::move(menu));
        } else if (control == C_KEY || control == C_SWING || control == C_SYNC) {
            auto menu = std::make_unique<ui::Menu>();
            auto& fill = control == C_KEY     ? w_.transport->fillKeyMenu
                         : control == C_SWING ? w_.transport->fillSwingMenu
                                              : w_.transport->fillSyncMenu;
            if (fill) fill(*menu);
            showOwnMenu(std::move(menu));
        } else if (control == C_METRONOME) {
            w_.showMetronomeSettings();
        } else if (control == C_TEMPO || control == C_BEATS) {  // a tempo change at the cursor's bar
            const Timing& tm = w_.project.timing;
            w_.timeline->editTempo(tm.barAt((double)w_.timeline->cursor / w_.engine.sr + 1e-9) + 1);
        }
        return;
    }
    auto menu = std::make_unique<ui::Menu>();
    std::optional<Lane> lane = currentLane();
    Timeline& tlm = *w_.timeline;
    if (pane == Pane::Timeline && laneRow < 0) {  // the ruler's (markers, loop, tempo)
        tlm.fillRulerMenu(*menu, PointF((float)tlm.xOf(tlm.cursor), (float)(lanes::rulerHeight() / 2)));
    } else if (lane && lane->group) {
        w_.panel->fillGroupMenu(*menu, lane->group);
    } else if (lane && lane->take) {
        tlm.fillTakeMenu(*menu, *lane);
    } else if (lane && lane->automation && pane == Pane::Timeline) {
        tlm.fillAutomationMenu(*menu, *lane, PointF((float)tlm.xOf(tlm.cursor), (float)(lane->top + lane->height / 2)));
    } else if (pane == Pane::Panel) {  // (a track's menu; an automation row's, the master's or no track: Add a track)
        if (lane && lane->track) w_.panel->fillTrackMenu(*menu, lane->track);
        else w_.panel->fillAddMenu(*menu, lane ? lane->owner : nullptr);
    } else {
        Timeline& tl = *w_.timeline;
        std::optional<Point> pos;
        if (TrackPtr cur = current()) {
            auto tops = tl.trackTops();
            auto it = tops.find(cur.get());
            if (it != tops.end()) pos = Point((int)tl.xOf(tl.cursor) + 1, it->second + 2);
        }
        tl.fillMenu(*menu, pos);
    }
    showOwnMenu(std::move(menu));
}

namespace {
// the File menu's last entry here: the window's ✕
bool hasQuit(const Front& f) { return f.barMenu == 0 && f.open.size() == 1; }
bool selectable(const ui::Action& a) { return a.visible && !a.separator && !a.section && a.enabled; }
}  // namespace

bool Front::menuKey(const Key& k) {
    Open& o = open.back();
    const auto& acts = o.menu->actions();
    int n = (int)acts.size();
    bool quit = hasQuit(*this);
    int last = quit ? n : n - 1;  // (n: the Quit entry)
    auto ok = [&](int i) { return i == n ? quit : (i >= 0 && i < n && selectable(*acts[(size_t)i])); };
    auto step = [&](int dir) {
        int i = o.item;
        for (int tries = 0; tries <= last + 1; tries++) {
            i += dir;
            if (i > last) i = 0;
            if (i < 0) i = last;
            if (ok(i)) {
                o.item = i;
                return;
            }
        }
    };
    bool fromControl = ui::Menu::ofWindow(topWindow()) == open[0].menu;
    switch (k.key) {
    case plat::Key_Up: step(-1); return true;
    case plat::Key_Down: step(1); return true;
    case plat::Key_Home: o.item = -1; step(1); return true;
    case plat::Key_End: o.item = last + 1; step(-1); return true;
    case plat::Key_Escape:
        if (open.size() > 1) {
            open.pop_back();
        } else {
            if (fromControl) open[0].menu->close();
            closeMenus();
        }
        return true;
    case plat::Key_Left:
        if (open.size() > 1) open.pop_back();
        else if (barMenu >= 0) openBarMenu(barMenu - 1);
        return true;
    case plat::Key_Right:
        if (o.item >= 0 && o.item < n && acts[(size_t)o.item]->submenu && selectable(*acts[(size_t)o.item])) {
            chooseItem(o.menu, o.item);
        } else if (barMenu >= 0) {
            openBarMenu(barMenu + 1);
        }
        return true;
    case plat::Key_Return:
    case plat::Key_Enter:
    case plat::Key_Space:
        chooseItem(o.menu, o.item);
        return true;
    case plat::Key_F10:
        if (fromControl) open[0].menu->close();
        closeMenus();
        return true;
    default:
        break;
    }
    // an entry's letter
    if (!k.mods && k.key < 128 && isalnum(k.key)) {
        for (int i = 0; i < n; i++) {
            char m = 0;
            menuText(acts[(size_t)i]->text, &m);
            if (m == (char)toupper(k.key) && ok(i)) {
                o.item = i;
                chooseItem(o.menu, i);
                return true;
            }
        }
        if (quit && toupper(k.key) == 'Q') {
            chooseItem(o.menu, n);
            return true;
        }
    }
    return true;
}

void Front::chooseItem(ui::Menu* m, int item) {
    int n = (int)m->actions().size();
    if (item == n && hasQuit(*this)) {
        closeMenus();
        w_.requestClose();
        return;
    }
    if (item < 0 || item >= n) return;
    ui::Action* a = m->actions()[(size_t)item].get();
    if (!selectable(*a)) return;
    if (a->submenu) {
        if (a->submenu->aboutToShow) a->submenu->aboutToShow(a->submenu);
        open.push_back({a->submenu, -1});
        menuKey(Key{plat::Key_Down});
        return;
    }
    bool fromControl = ui::Menu::ofWindow(topWindow()) == open[0].menu;
    // closed first, so a dialog it opens is shown alone; F2's menu is kept until the action is done
    std::unique_ptr<ui::Menu> keep = std::move(ownMenu_);
    closeMenus();
    if (fromControl) ui::Menu::choose(a);  // (the control's exec() returns it)
    else ui::Menu::activate(a);
}

// --- drawing ---

void Front::draw(Screen& s) {
    dropGoneMenus();
    closeUnder();
    s.clear();
    int W = s.width(), H = s.height();
    // too small for the main window: what it needs, said in as many rows as it takes; a dialog or window open is
    // still drawn (over the whole terminal), and a menu, so they can be answered and closed
    bool tiny = W < 60 || H < 14;
    if (tiny) {
        std::string say = "Tiny DAW needs a terminal of at least 60 x 14 (this one: " + std::to_string(W) + " x " + std::to_string(H) + ")";
        std::string line;
        int y = 0;
        for (size_t i = 0; i <= say.size() && y < H; i++) {
            size_t sp = std::min(say.find(' ', i), say.size());
            std::string word = say.substr(i, sp - i);
            if (!line.empty() && columns(line) + 1 + columns(word) > W) s.text(0, y++, line, kTitle), line.clear();
            line += (line.empty() ? "" : " ") + word;
            i = sp;
        }
        if (y < H) s.text(0, y, line, kTitle);
    } else {
        if (secondsPerColumn <= 0) secondsPerColumn = w_.project.timing.barSeconds(1) / 8;
        drawMenuBar(s);
        drawTransport(s);
        int top = 2, bodyH = H - 3;
        int ex = w_.explorerDock->isVisible() ? std::clamp(W / 5, EXPLORER_MIN, EXPLORER_MAX) : 0;
        int pw = std::clamp(W / 4, PANEL_MIN, PANEL_MAX);
        int tx = ex ? ex + 1 : 0, tw = W - tx - pw - 1;
        if (ex) {
            drawExplorer(s, 0, top, ex, bodyH);
            s.vline(ex, top, bodyH, kPane);
        }
        drawTimeline(s, tx, top, tw, bodyH);
        s.vline(tx + tw, top, bodyH, kPane);
        drawPanel(s, tx + tw + 1, top, pw, bodyH);
        drawStatus(s);
    }
    if (menuOpen()) drawMenus(s);
    // the program's windows over it (dialogs, a menu a control opened: drawn above as menus)
    int n = openWindows();
    if (n > shownWindows_) windowsHidden = false;  // (one just opened: shown)
    shownWindows_ = n;
    for (ui::Window* win : ui::App::get().windows())
        if (drawsWindow(win) && !ui::Menu::ofWindow(win)) {
            if (tiny) drawWindow(s, *win, 0, 0, W, H);
            else drawWindow(s, *win, 2, 1, W - 4, H - 2);
        }
    if (!tiny && topWindow() && !ui::Menu::ofWindow(topWindow())) {  // (how to get out of it)
        std::string help = topWindow()->isModal() ? " Tab: next  Enter: OK  Esc: cancel " : " Ctrl+W: close it  F12: the main window ";
        if (dynamic_cast<MidiEditor*>(topWindow())) help = " F2: tools  K: knife  Ctrl+J: join  P: row" + help;
        if (auto* synth = dynamic_cast<synth_dialog::SynthDialog*>(topWindow()))
            help = (synth->wave->isVisible() ? " Left/Right: a point  Up/Down: its value"  // (a Drawn synth's canvas)
                                             : " Up/Down: control  Left/Right: change  M: Modulate…") + help;
        s.text(std::max(0, W - columns(help) - 1), H - 1, help, style(Black, Yellow));
    } else if (!tiny && windowsHidden && n) {
        std::string help = " F12: " + std::to_string(n) + (n == 1 ? " window" : " windows") + " open ";
        s.text(std::max(0, W - columns(help) - 1), H - 1, help, style(Black, Yellow));
    }
    if (ui::Menu* m = ui::Menu::ofWindow(topWindow())) {
        if (open.empty() || open[0].menu != m) {  // (opened by a control while drawing: shown at once)
            closeMenus();
            open.push_back({m, -1});
            menuKey(Key{plat::Key_Down});
        }
        drawMenus(s);
    }
    if (helpShown) drawHelp(s);
}

void Front::drawMenuBar(Screen& s) {
    s.fill(0, 0, s.width(), 1, U' ', kBar);
    int x = 1;
    const auto& menus = w_.menuBar->menus();
    for (int i = 0; i < (int)menus.size(); i++) {
        char m = 0;
        std::string t = " " + menuText(menus[(size_t)i]->title, &m) + " ";
        Style st = i == barMenu ? kMenuHi : kBar;
        s.text(x, 0, t, st);
        // (its letter underlined: Alt + it opens it)
        std::string plain = menuText(menus[(size_t)i]->title);
        for (int j = 0; j < (int)plain.size(); j++)
            if (toupper((unsigned char)plain[(size_t)j]) == m) {
                if (Cell* c = s.at(x + 1 + j, 0)) c->st.attr |= Underline;
                break;
            }
        x += columns(t) + 1;
    }
    std::string name = w_.projectPath.empty() ? "Untitled" : util::stem(w_.projectPath);
    // (MainWindow::hasUnsavedChanges describes the whole project: 40 ms with 10,000 clips; asked every UNSAVED_S,
    // or less often when it is slow: at most a 50th of the time)
    constexpr double UNSAVED_S = 0.5;
    double now = plat::now();
    if (unsavedAt_ < 0 || now - unsavedAt_ >= std::max(UNSAVED_S, unsavedCost_ * 50) || now < unsavedAt_) {
        unsaved_ = w_.hasUnsavedChanges();
        unsavedCost_ = plat::now() - now;
        unsavedAt_ = now;
    }
    if (unsaved_) name += " *";
    name += "   F1 keys";
    int nx = std::max(x + 1, s.width() - columns(name) - 1);
    s.text(nx, 0, name, kBar);
}

void Front::drawTransport(Screen& s) {
    s.fill(0, 1, s.width(), 1, U' ', kBar);
    const Timing& t = w_.project.timing;
    audio::Engine& e = w_.engine;
    int64_t pos = e.playing ? e.heardPos() : w_.timeline->cursor;
    double sec = (double)pos / e.sr;
    int bar = 0, beat = 0, part = 0;
    t.position(sec, bar, beat, part);
    bool focused = pane == Pane::Transport;
    TransportBar& tb = *w_.transport;
    bool recording = w_.recording->active();
    auto box = [](bool on) { return std::string(on ? "[x]" : "[ ]"); };
    std::vector<std::string> items = {
        e.playing ? "■ Stop" : "▶ Play",
        recording ? "● REC" : "● Rec",
        Terminal::utf8() ? "⏮" : "|<",
        box(t.looping) + "Loop",
        box(tb.followAction->isChecked()) + "Follow",
        box(w_.prefs.metronome) + "Click",
        tb.keysAction ? box(tb.keysAction->isChecked()) + "Keys" : "",
        [&] {
            char buf[32];
            snprintf(buf, sizeof buf, "%g BPM", t.bpm);
            return std::string(buf);
        }(),
        std::to_string(t.beatsPerBar) + "/4",
        "Grid " + t.gridName(),
        box(t.snapOn) + "Snap",
        "Key " + (t.scale.empty() ? std::string("-") : song_scale::scaleName(t.key, t.scale)),
        "Swing " + (t.swing > 0 ? song_scale::swingText(t.swing) + (t.swingGrid ? "" : " 8ths") : std::string("0 %")),
        std::string("Sync ") + midi_sync::modeName(w_.syncMode),
    };
    int x = 1;
    std::string time = clock(sec) + " " + std::to_string(bar) + "." + std::to_string(beat) + "." + std::to_string(part);
    s.text(x, 1, time, style(Black, White, Bold));
    x += columns(time) + 2;
    // (a narrow terminal: the controls before the one with the keyboard make room for it, so it is always shown)
    int first = 0, end = x;
    for (int i = 0; i <= control && i < (int)items.size(); i++)
        if (!items[(size_t)i].empty()) end += columns(items[(size_t)i]) + 1;
    for (; focused && end > s.width() && first < control; first++)
        if (!items[(size_t)first].empty()) end -= columns(items[(size_t)first]) + 1;
    for (int i = first; i < (int)items.size(); i++) {
        if (items[(size_t)i].empty()) continue;
        Style st = kBar;
        if (i == C_RECORD && recording) st = style(White, Red, Bold);
        if (focused && i == control) st = kMenuHi;
        x += s.text(x, 1, items[(size_t)i], st) + 1;
    }
    if (!e.isOpen()) s.text(std::max(x, s.width() - 18), 1, "No audio device", style(Red, White, Bold));
}

void Front::drawExplorer(Screen& s, int x, int y, int w, int h) {
    bool focused = pane == Pane::Explorer;
    s.fill(x, y, w, 1, U' ', focused ? kFocusTitle : kTitle);
    s.text(x, y, " Explorer", focused ? kFocusTitle : kTitle, w);
    s.text(x, y + 1, fit(folder, w), style(Cyan), w);
    int rows = h - 2;
    if (entry < entryTop) entryTop = entry;
    if (entry >= entryTop + rows) entryTop = entry - rows + 1;
    for (int i = 0; i < rows && entryTop + i < (int)entries.size(); i++) {
        int k = entryTop + i;
        const auto& e = entries[(size_t)k];
        std::string name = e.isDir ? e.name + "/" : e.name;
        bool sel = k == entry;
        Style st = sel ? (focused ? kSelFocus : kSelIdle) : kPane;
        if (sel) s.fill(x, y + 2 + i, w, 1, U' ', st);
        s.text(x, y + 2 + i, (sel ? ">" : " ") + fit(name, w - 1), st, w);
    }
    if (entries.empty()) s.text(x + 1, y + 2, "(nothing to open here)", kPane, w - 1);
}

void Front::drawTimeline(Screen& s, int x, int y, int w, int h) {
    timelineCols_ = w;
    Timeline& tl = *w_.timeline;
    const Timing& t = w_.project.timing;
    TempoMapPtr tmap = t.map();  // (once: the grid below asks it for every column)
    const TempoMap& tm = *tmap;
    audio::Engine& e = w_.engine;
    int sr = e.sr;
    bool focused = pane == Pane::Timeline;
    bool utf8 = Terminal::utf8();
    double spc = secondsPerColumn;
    auto colOf = [&](double sec) { return (int)std::floor((sec - viewStart) / spc + 1e-9); };
    // the playhead followed while playing
    double heard = (double)e.heardPos() / sr;
    if (e.playing && (heard < viewStart || heard >= viewStart + w * spc)) viewStart = std::max(0.0, heard - w * spc / 8);

    // the overview: the whole song in a row, the part shown marked
    double songS = std::max({(double)w_.project.endFrame() / sr, viewStart + w * spc, 1.0});
    std::vector<char> any((size_t)std::max(0, w), 0);  // (the columns a clip is in: each clip once)
    for (auto& cl : w_.project.clips) {
        double a = (double)cl->start / sr * w / songS, b = (double)cl->end() / sr * w / songS;
        for (int c = std::max(0, (int)std::floor(a)); c < std::min(w, (int)std::ceil(b)); c++) any[(size_t)c] = 1;
    }
    for (int c = 0; c < w; c++) {
        double a = songS * c / w, b = songS * (c + 1) / w;
        bool shown = b > viewStart && a < viewStart + w * spc;
        s.set(x + c, y, any[(size_t)c] ? U'▄' : U' ', shown ? style(Default, Default, Reverse) : style(Cyan));
    }
    // the title row and ruler: bar numbers, the loop range
    int ry = y + 1;
    s.fill(x, ry, w, 1, U' ', focused ? kFocusTitle : kTitle);
    double loopA = 0, loopB = 0;
    bool loop = t.looping && t.loopRange(loopA, loopB);
    for (int c = 0; c < w; c++) {
        double a = viewStart + c * spc;
        if (loop && a + spc > loopA && a < loopB) s.set(x + c, ry, U'═', focused ? kFocusTitle : style(Yellow, Default, Bold));
    }
    int firstBar = std::max(0, tm.barAt(viewStart) - 1);
    Style rs = focused ? kFocusTitle : kTitle;
    if (focused && laneRow < 0) {  // (the ruler has the keyboard: Enter a tempo change, F2 its menu)
        rs = style(Black, Yellow, Bold);
        s.fill(x, ry, w, 1, U' ', rs);
    }
    for (int b = firstBar;; b++) {
        double bs = tm.barSeconds(b);
        int c = colOf(bs);
        if (c >= w) break;
        if (c >= 0) s.text(x + c, ry, "|" + std::to_string(b + 1), rs, w - c);
        // (zoomed in, bars far apart: their beats too, as bar.beat)
        double barCols = (tm.barSeconds(b + 1) - bs) / spc;
        int beats = std::max(1, (int)std::lround(tm.beatAt(tm.barSeconds(b + 1)) - tm.beatAt(bs)));
        if (barCols / beats >= 6)
            for (int k = 1; k < beats; k++) {
                int bc = colOf(tm.secondsAt(tm.beatAt(bs) + k));
                if (bc >= 0 && bc < w) s.text(x + bc, ry, "'" + std::to_string(b + 1) + "." + std::to_string(k + 1), rs, w - bc);
            }
        if (b > firstBar + 2000) break;
    }
    // the tempo changes (the tempo lane's points): ♩ a step, ↗ a ramp to the next (Enter on one: its dialog)
    for (const TempoPoint& p : tm.points()) {
        int c = colOf(p.seconds);
        if (c < 0 || c >= w || (p.change < 0 && !p.ramp)) continue;  // (bar 1's own: only when it ramps)
        char32_t mark = utf8 ? (p.ramp ? U'↗' : U'♩') : (p.ramp ? U'/' : U'T');
        s.set(x + c, ry, mark, focused && laneRow < 0 ? style(Black, Yellow, Bold) : style(Magenta, Default, Bold));
    }
    // the lanes: a row each (a track's, its automation lanes and takes, a group's header), the master's at the bottom
    std::vector<Lane> ls = lanes();
    int lanesTop = y + 2, rows = h - 3;
    bodyRows_ = rows;
    laneRow = std::clamp(laneRow, -1, (int)ls.size());  // (-1: the ruler)
    int curRow = std::clamp(laneRow, 0, std::max(0, (int)ls.size() - 1));
    if (curRow < firstRow) firstRow = curRow;
    if (curRow >= firstRow + rows) firstRow = curRow - rows + 1;
    firstRow = std::clamp(firstRow, 0, std::max(0, (int)ls.size() - rows));
    const Selection& sel = tl.selection;
    static const char32_t BLOCKS[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
    auto grid = [&](int ly) {  // the beats as dots, bars as lines
        for (int c = 0; c < w; c++) {
            double a = viewStart + c * spc;
            // (a bar or beat starting in this column: between its left edge and the next's)
            bool barLine = tm.barAt(a + spc - 1e-9) != tm.barAt(a - 1e-9) || a < 1e-9;
            bool beat = std::floor(tm.beatAt(a + spc - 1e-9)) != std::floor(tm.beatAt(a - 1e-9));
            char32_t ch = barLine ? U'¦' : beat ? U'·' : U' ';
            if (!utf8 && ch == U'¦') ch = U':';
            s.set(x + c, ly, ch, style(Blue));
        }
    };
    auto block = [&](int ly, const ClipPtr& cl, uint8_t trackColor, bool on, bool faint) {  // a clip as a bar of colour
        int a = colOf((double)cl->start / sr), b = colOf((double)cl->end() / sr - 1e-9) + 1;
        b = std::max(b, a + 1);
        if (b <= 0 || a >= w) return;
        uint8_t bg = nearestColor(cl->color, trackColor);
        Style st = cl->disabled || faint ? style(bg, Default) : style(textOn(bg), bg, on ? Bold | Reverse : Plain);
        int a0 = std::max(0, a), b0 = std::min(w, b);
        s.fill(x + a0, ly, b0 - a0, 1, cl->disabled || faint ? U'░' : U' ', st);
        std::string label = std::string(on ? "[" : "") + (cl->missing ? "[missing] " : "") + cl->name +  // (its file not found: relink.h)
                            (cl->gainEnvelope ? " env" : "");  // (a gain envelope)
        if (b0 - a0 > 1) s.text(x + a0, ly, label, st, b0 - a0 - (on ? 1 : 0));
        if (on && b0 == b) s.set(x + b0 - 1, ly, U']', st);
    };
    for (int r = 0; r < rows; r++) {
        int ly = lanesTop + r;
        int k = firstRow + r;
        if (k >= (int)ls.size()) break;
        const Lane& lane = ls[(size_t)k];
        if (lane.group) {  // a group's header
            s.fill(x, ly, w, 1, U'─', style(Magenta));
            s.text(x, ly, std::string(lane.group->folded ? "▸ " : "▾ ") + lane.group->name + " ", style(Magenta, Default, Bold), w);
            continue;
        }
        grid(ly);
        if (lane.automation) {  // its curve: a block as high as the value, the points marked
            const Automation& au = *lane.automation;
            std::vector<AutoPoint> pts = au.points.copy();
            for (int c = 0; c < w && !pts.empty(); c++) {
                double v = automation::valueAt(pts, viewStart + (c + 0.5) * spc, au.targetSteps);
                s.set(x + c, ly, BLOCKS[std::clamp((int)std::lround(v * 8) - 1, 0, 7)], style(au.armed ? Red : Cyan));
            }
            for (const AutoPoint& p : pts) {
                int c = colOf(p.t);
                if (c >= 0 && c < w) s.set(x + c, ly, U'●', style(Yellow, Default, Bold));
            }
            if (pts.empty()) s.text(x + 1, ly, au.param + ": no points (Enter or [ ] at the cursor)", style(Cyan), w - 1);
            continue;
        }
        if (lane.take) {  // a recorded take, faint: Enter uses it (or the range selected of it)
            block(ly, lane.take, nearestColor(lane.owner ? lane.owner->color : "", Blue), false, true);
            continue;
        }
        const TrackPtr& tr = lane.track;
        if (!tr) continue;
        if (sel.range && std::find(sel.range->tracks.begin(), sel.range->tracks.end(), tr) != sel.range->tracks.end()) {
            int a = std::max(0, colOf((double)sel.range->start / sr)), b = std::min(w, colOf((double)sel.range->end / sr - 1e-9) + 1);
            for (int c = a; c < b; c++)
                if (Cell* cell = s.at(x + c, ly)) cell->st = style(Black, Cyan);
        }
        uint8_t trackColor = nearestColor(tr->color, tr->kind == Kind::Midi ? Green : Blue);
        for (auto& cl : w_.project.clipsOn(tr)) block(ly, cl, trackColor, sel.contains(cl), false);
    }
    if (ls.empty()) s.text(x + 1, lanesTop, "No tracks: Tab to the track panel, then A (audio) or N (MIDI)", kPane, w - 1);
    // the markers, on the ruler
    for (const Marker& m : w_.project.markers) {
        int c = colOf(m.seconds);
        if (c >= 0 && c < w) s.text(x + c, ry, "▼" + m.name, style(Black, Yellow), std::min(w - c, 12));
    }
    // the master lane
    int my = y + h - 1;
    s.fill(x, my, w, 1, U'─', style(Default));
    s.text(x, my, " Master ", kTitle, w);
    // the cursor (where Play starts) and the playhead
    int cc = colOf((double)tl.cursor / sr);
    if (cc >= 0 && cc < w)
        for (int r = 0; r < rows && firstRow + r < (int)ls.size(); r++)
            if (Cell* cell = s.at(x + cc, lanesTop + r)) {
                bool curLane = firstRow + r == laneRow;
                Style st = cell->st;
                st.attr ^= Reverse;  // (over a selected clip, itself reversed: un-reversed, so it still shows)
                if (curLane && focused) st.attr |= Bold;
                char32_t ch = cell->ch == U' ' || cell->ch == U'·' || cell->ch == U'¦' || cell->ch == U':' ? (curLane ? U'│' : U' ') : cell->ch;
                s.set(x + cc, lanesTop + r, ch == WIDE_TAIL ? U' ' : ch, st);  // (half a wide character: a space)
            }
    if (e.playing) {
        int pc = colOf(heard);
        if (pc >= 0 && pc < w)
            for (int r = -1; r < rows + 1; r++) s.set(x + pc, lanesTop + r, U'┃', style(Yellow, Default, Bold));
    }
}

void Front::drawPanel(Screen& s, int x, int y, int w, int h) {
    bool focused = pane == Pane::Panel;
    bool utf8 = Terminal::utf8();
    // the caption: CPU and the device
    double cur = 0, avg = 0;
    std::string cap;
    if (w_.engine.isOpen() && w_.engine.cpu.read(config::CPU_AVERAGE_SECONDS, cur, avg)) cap = "CPU " + std::to_string((int)std::lround(cur * 100)) + "%  ";
    cap += w_.engine.isOpen() ? std::to_string(w_.engine.sr / 1000) + " kHz " + w_.engine.driverType : "no device";
    s.text(x + 1, y, fit(cap, w - 1), style(Cyan), w - 1);
    s.fill(x, y + 1, w, 1, U' ', focused ? kFocusTitle : kTitle);
    // (KEYBOARD FOCUS locked: said in the title; the tracks the keyboards play have ♪ before their names)
    s.text(x, y + 1, w_.project.keyFocusLocked ? (utf8 ? " Tracks  ♪ locked" : " Tracks  keys locked") : " Tracks",
           focused ? kFocusTitle : kTitle, w);
    std::vector<TrackPtr> plays = midi_input::keyboardTracks(w_.project, w_.timeline->currentTrack);
    int rows = h - 3;
    int nameW = std::max(4, w - 15 - METER_COLS);
    auto row = [&](int ry, const std::string& name, bool muted, bool soloed, double db, double pan, const LevelMeter* m,
                   bool sel, uint8_t color, bool isMaster, bool safe = false) {
        Style st = sel ? (focused ? kSelFocus : kSelIdle) : kPane;
        if (sel) s.fill(x, ry, w, 1, U' ', st);
        s.set(x, ry, isMaster ? U' ' : U'▌', style(color, sel ? groundOf(st) : (uint8_t)Default));
        s.text(x + 1, ry, fit(name, nameW), st, nameW);
        // (name M S pan dB meter: 1 + name + 1 + 2 + 1 + 3 + 1 + 5 + 1 + meter)
        int cx = x + 1 + nameW + 1;
        s.text(cx, ry, muted ? "M" : "-", muted ? style(Black, Yellow, Bold) : st);
        // (solo safe: a blue s, or S when soloed too)
        if (!isMaster)
            s.text(cx + 1, ry, soloed ? "S" : safe ? "s" : "-",
                   soloed || safe ? style(Black, safe ? Cyan : Green, Bold) : st);
        char panText[8] = "  C";
        if (!isMaster && std::fabs(pan) > 0.04) snprintf(panText, sizeof panText, "%c%2d", pan < 0 ? 'L' : 'R', (int)std::lround(std::fabs(pan) * 100));
        if (!isMaster) s.text(cx + 3, ry, panText, st);
        std::string v = dbText(db);
        s.text(cx + 7, ry, std::string(std::max(0, 5 - columns(v)), ' ') + v, st);
        s.text(cx + 13, ry, meterBar(m, METER_COLS, utf8), style(Green, sel ? groundOf(st) : (uint8_t)Default));
    };
    std::vector<Lane> ls = lanes();
    laneRow = std::clamp(laneRow, -1, (int)ls.size());
    double cursorS = (double)w_.timeline->cursor / w_.engine.sr;
    if (ls.empty() && rows > 0) s.text(x + 1, y + 2, "F2 (or A / N): add a track", style(Cyan), w - 1);
    for (int r = 0; r < rows; r++) {
        int k = firstRow + r;
        if (k >= (int)ls.size()) break;
        const Lane& lane = ls[(size_t)k];
        int ry = y + 2 + r;
        bool sel = k == laneRow && (focused || pane == Pane::Timeline);
        Style st = sel ? (focused ? kSelFocus : kSelIdle) : kPane;
        if (lane.track) {
            const TrackPtr& t = lane.track;
            auto it = w_.panel->meters.find(t);
            bool many = w_.panel->selected.size() > 1;  // (the selected tracks marked when there are several)
            bool in = std::find(w_.panel->selected.begin(), w_.panel->selected.end(), t) != w_.panel->selected.end();
            // (before its name: ● armed; an audio track's monitoring when not Auto: » In, × Off)
            std::string marks = t->armed ? std::string(utf8 ? "●" : "R") : "";
            if (t->kind == Kind::Audio && t->monitor != Track::Monitor::Auto)
                marks += t->monitor == Track::Monitor::In ? (utf8 ? "»" : ">") : (utf8 ? "×" : "x");
            if (std::find(plays.begin(), plays.end(), t) != plays.end()) marks += utf8 ? "♪" : "k";  // (the keyboards play it)
            row(ry, (marks.empty() ? "" : marks + " ") + (many && in ? "* " : "") + t->name, t->muted, t->soloed,
                t->volumeDb, t->pan, it != w_.panel->meters.end() ? it->second : nullptr, sel,
                nearestColor(t->color, t->kind == Kind::Midi ? Green : Blue), false, t->soloSafe);
            if (t->armed) s.set(x + 1, ry, utf8 ? U'●' : U'R', style(Red, sel ? groundOf(st) : (uint8_t)Default, Bold));
            continue;
        }
        if (lane.group && routing::busOn(*lane.group)) {  // a group bus: its row as a track's (its group's M and S)
            const Group& g = *lane.group;
            auto it = w_.panel->meters.find(g.bus);
            row(ry, std::string(g.folded ? "▸ " : "▾ ") + g.name, g.muted, g.soloed, g.bus->volumeDb, g.bus->pan,
                it != w_.panel->meters.end() ? it->second : nullptr, sel, Magenta, false);
            continue;
        }
        if (sel) s.fill(x, ry, w, 1, U' ', st);
        if (lane.group) {
            s.text(x, ry, std::string(lane.group->folded ? "▸ " : "▾ ") + lane.group->name + "  (fold only)",
                   sel ? st : style(Magenta, Default, Bold), w);
        } else if (lane.automation) {
            const Automation& au = *lane.automation;
            std::string text = "  ↳ " + au.param + "  " + laneText(lane, laneValue(lane, cursorS)) + (au.armed ? "  [W]" : "");
            s.text(x, ry, text, sel ? st : style(au.armed ? Red : Cyan), w);
        } else if (lane.take) {
            s.text(x, ry, "  take: " + lane.take->name + "  (Enter in the timeline: use it)", sel ? st : style(Default), w);
        }
    }
    // the master's true peak since Play (ITU-R BS.1770-4: between the samples too), in dBTP ("Master" when there's room)
    double tp = w_.engine.metering ? w_.engine.metering->truePeakDb() : -INFINITY;
    char tpText[32] = "- dBTP";
    if (tp > -200) snprintf(tpText, sizeof tpText, "%.1f dBTP", tp);
    row(y + h - 1, nameW >= 7 + columns(tpText) ? std::string("Master ") + tpText : std::string(tpText), w_.engine.masterMute, false, w_.master->volume->value(), 0, w_.master->meter,
        focused && laneRow == (int)ls.size(), White, true);
}

void Front::drawStatus(Screen& s) {
    int y = s.height() - 1;
    s.fill(0, y, s.width(), 1, U' ', kBar);
    std::string msg = w_.status->message();
    if (msg.empty()) {
        switch (pane) {
        case Pane::Transport:
            msg = control == C_KEY     ? "The song's key: Up/Down: the next key  Enter/F2: key, scale, Detect"
                  : control == C_SWING ? "The song's swing: Up/Down: 5 % more or less  Enter/F2: amount, 8ths/16ths, Commit"
                  : control == C_SYNC  ? "What the transport follows: Up/Down: Internal, MIDI clock, JACK  Enter/F2: its menu"
                                       : "Left/Right: a control  Enter: use it  Up/Down: change it  F2: its settings (Rec, Click, "
                                         "Grid, Key, Swing, Sync)  " + keysOf("Transport/Record") + ": record  Ctrl+P: commands";
            break;
        case Pane::Explorer: msg = "Enter: open / add at the cursor  Space or P: hear it  Backspace: up  Tab: next part"; break;
        case Pane::Timeline:
            if (laneRow < 0)
                msg = "The ruler: Enter: the tempo change at the cursor (tempo, ramp, time signature), or a new one at this "
                      "bar  F2: its menu (tempo changes, Timebase, markers, loop)  Down: the lanes";
            else if (currentLane() && currentLane()->automation)
                msg = "Automation: Enter: a point at the cursor  [ ]: lower / raise it  Del: remove it (or the range)  "
                      "I/O: range  Ctrl+C/X/V: its curve  F2: its menu";
            else if (currentLane() && currentLane()->take) msg = "A take: Enter: use it (or the range selected of it)  Del: delete it  F2: menu";
            else
                msg = "Left/Right: grid  PgUp/PgDn: bar  Up/Down: lane  Enter: select clip  I/O: range  ,/.: move  " +
                      keysOf("Timeline/Split") + ": split  " + keysOf("Timeline/Delete the selection") +
                      ": delete  +/-: zoom  F2: menu  Ctrl+P: commands";
            break;
        case Pane::Panel:
            if (currentLane() && currentLane()->group)
                msg = routing::busOn(*currentLane()->group)
                          ? "A group bus: M S: mute solo  Left/Right: volume  [/]: pan  E: effects  U: automation  D: sends  "
                            "Enter: rename  F2: menu (Fold only, Output, Put in group)"
                          : "A group (fold only): M S: mute solo  Enter: rename  F2: menu (its bus back: Fold only off)";
            else msg = "M S: mute solo  Shift+R: arm  H: monitor  Shift+H: input  Shift+S: solo safe  Left/Right: volume  [/]: pan  "
                  "E: effects  I: instrument  U: automation  K: takes  Shift+K: keyboard plays it  Enter: rename  A/N: new (F2: return)  Shift+Up/Down: select  "
                  "Alt+Up/Down: move  Ctrl+D: duplicate  F2: menu";
            break;
        }
    }
    // the update notice at the right (as the master strip's in the window), when there's room for both
    int room = s.width() - 2;
    const auto& r = w_.master->updateResult;
    std::string notice = update_notice::noticeText(r ? &*r : nullptr).plain();
    int nw = columns(notice);
    if (nw + 20 <= room) {
        bool news = r && r->state == "waiting";
        s.text(s.width() - 1 - nw, y, notice, news ? Style{kBar.fg, kBar.bg, Bold} : kBar);
        room -= nw + 2;
    }
    s.text(1, y, fit(msg, room), kBar, room);
}

void Front::drawMenus(Screen& s) {
    int x = 1;
    if (barMenu >= 0) {
        const auto& menus = w_.menuBar->menus();
        for (int i = 0; i < barMenu && i < (int)menus.size(); i++) x += columns(menuText(menus[(size_t)i]->title)) + 3;
    } else {
        x = std::max(1, s.width() / 3);
    }
    int y = barMenu >= 0 ? 1 : 3;
    for (size_t level = 0; level < open.size(); level++) {
        const Open& o = open[level];
        const auto& acts = o.menu->actions();
        bool quit = level == 0 && hasQuit(*this);
        // its width: the longest text and shortcut
        int textW = 4, keyW = 0;
        std::vector<int> shown;
        for (int i = 0; i < (int)acts.size(); i++) {
            if (!acts[(size_t)i]->visible) continue;
            shown.push_back(i);
            textW = std::max(textW, columns(menuText(acts[(size_t)i]->text)) + 4);
            if (!acts[(size_t)i]->shortcut.empty()) keyW = std::max(keyW, columns(acts[(size_t)i]->shortcut.text()) + 2);
        }
        if (quit) {
            shown.push_back(-2);  // (a separator)
            shown.push_back((int)acts.size());
            keyW = std::max(keyW, 8);
        }
        int mw = std::min(s.width() - 2, textW + keyW + 2), mh = std::min(s.height() - y - 1, (int)shown.size() + 2);
        if (x + mw > s.width()) x = std::max(0, s.width() - mw);
        s.fill(x, y, mw, mh, U' ', kMenu);
        s.box(x, y, mw, mh, kMenu, barMenu < 0 && level == 0 ? "Menu" : "");
        // (the highlighted entry kept in view)
        int visible = mh - 2, hiAt = 0;
        for (int j = 0; j < (int)shown.size(); j++)
            if (shown[(size_t)j] == o.item) hiAt = j;
        int first = std::clamp(hiAt - visible + 1, 0, std::max(0, (int)shown.size() - visible));
        first = std::min(first, hiAt);
        for (int j = 0; j < visible && first + j < (int)shown.size(); j++) {
            int i = shown[(size_t)(first + j)];
            int ry = y + 1 + j;
            if (i == -2 || (i < (int)acts.size() && acts[(size_t)i]->separator)) {
                s.hline(x + 1, ry, mw - 2, kMenu);
                continue;
            }
            std::string text, keys;
            bool enabled = true, sub = false, section = false;
            std::string mark = "  ";
            if (i == (int)acts.size()) {
                text = "Quit";
                keys = "Ctrl+Q";
            } else {
                const ui::Action& a = *acts[(size_t)i];
                text = menuText(a.text);
                keys = a.shortcut.empty() ? "" : a.shortcut.text();
                enabled = a.enabled;
                sub = a.submenu != nullptr;
                section = a.section;
                if (a.checkable) mark = a.group ? (a.checked ? "(*" : "( ") : (a.checked ? "[x" : "[ ");
                if (a.checkable) mark += a.group ? ")" : "]";
            }
            bool hi = i == o.item;
            Style st = hi ? kMenuHi : section ? style(Black, White, Bold | Underline) : kMenu;
            if (hi) s.fill(x + 1, ry, mw - 2, 1, U' ', st);
            std::string line = (mark.size() > 2 ? mark + " " : "  ") + (enabled || section || (!text.empty() && text[0] == '(') ? text : "(" + text + ")");
            s.text(x + 1, ry, line, st, mw - 2 - keyW);
            if (sub) s.set(x + mw - 2, ry, U'▸', st);
            else if (!keys.empty()) s.text(x + mw - 1 - columns(keys) - 1, ry, keys, st);
        }
        // a submenu: beside this one, at its entry
        x += mw - 1;
        y += 1 + std::max(0, hiAt - first);
    }
}

std::string Front::keysOf(const std::string& id) const {
    // the command's keys a text console can send (Options → Keyboard shortcuts… changes them)
    commands::Keys ks;
    if (w_.commands)
        for (auto& k : w_.commands->keys(id))
            if (commands::terminalSends(k)) ks.push_back(k);
    return commands::keysText(ks, "(none)");
}

void Front::drawHelp(Screen& s) {
    auto pad = [](std::string t, int n) {
        int c = columns(t);
        return c + 1 >= n ? t + " " : t + std::string((size_t)(n - c), ' ');
    };
    std::vector<std::string> lines = {
        "Everywhere",
        "  F10, Alt+letter   the menus (Left/Right: the next one, Enter: choose, Esc: close)",
        "  Tab, F6           the next part: transport, Explorer, timeline, track panel",
        "  " + pad(keysOf("Transport/Play"), 18) + "play / stop (from the cursor)",
        "  F2                the menu of what is selected (a clip's, a track's)",
        "  Ctrl+P            every command by name: type a few letters, Enter runs it",
        "  " + pad(keysOf("Edit/Undo") + ", " + keysOf("Edit/Redo"), 18) + "undo, redo",
        "  " + pad(keysOf("File/Save"), 18) + "save   " + keysOf("File/Open project…") + " open   " +
            keysOf("File/New project") + " new",
        "  " + pad(keysOf("Transport/Record"), 18) + "record   " + keysOf("Transport/Loop") + " loop   " +
            keysOf("Transport/Follow the playhead") + " follow   " + keysOf("Transport/Tap tempo") + " tap",
        "  Ctrl+Q            quit",
        "  Ctrl+W, F12       close the window on top (an editor…); hide the open windows (again: back)",
        "Transport (Tab to it)",
        "  Key, Swing        Up/Down: the next key, 5 % more or less swing; Enter/F2: their menus (Detect, Commit)",
        "  Sync              Up/Down: Internal, MIDI clock (Options > MIDI ports: Clock in), JACK; Enter/F2: its menu",
        "Timeline",
        "  Left/Right        the cursor by the grid's step (PgUp/PgDn: a bar; Home/End)",
        "  Up/Down           the track      +/-  zoom",
        "  Enter, Insert     select the clip at the cursor (Insert: add it to the selection)",
        "  I then O          a range from I to the cursor (across the tracks between)",
        "  , .  [ ]          move the selected clips by the grid / to the track above, below",
        "  " + pad(keysOf("Timeline/Split") + "  " + keysOf("Timeline/Delete the selection") + "  " +
                       keysOf("Timeline/Clip colour…") + "  " + keysOf("Timeline/Switch clips off or on"), 18) +
            "split, delete, colour, switch off",
        "  " + pad(keysOf("Edit/Copy clips") + " " + keysOf("Edit/Cut clips") + " " + keysOf("Edit/Paste clips at the cursor"), 18) +
            "copy, cut, paste   " + keysOf("Timeline/Duplicate") + " " + keysOf("Timeline/Join") + " " +
            keysOf("Timeline/Rename…") + ": duplicate, join, rename",
        "  L                 loop the range (or switch looping on and off)",
        "  E  N              the MIDI editor of the clip (N: a new MIDI clip at the cursor, edited)",
        "  F2: Slip contents… the selected clips' audio or notes moved inside them (Left/Right: by the grid);",
        "                    Crop to the selection, Fades at every boundary… (every selected clip)",
        "  F2: Warp…         an audio clip follows the song's tempo (Beats, Tones, Complex); its markers'",
        "                    list: Left/Right move the chosen one by the grid, Del removes it;",
        "                    Warp from here: a marker at the cursor on the nearest bar",
        "  automation lane   Enter, [ ]: a point; I then O: a range of its curve: Ctrl+C/X/V, Del clears it",
        "                    (Edit → Automation follows clips: moved and copied clips take it along)",
        "  ruler (Up above   tempo changes: ♩ a step, ↗ a ramp. Enter: the one at the cursor (tempo, ramp to the",
        "   the first lane)  next: yes/no, time signature), else a new one at the cursor's bar",
        "  F2 on the ruler   Tempo changes ▸ add, edit, ramp, remove; Timebase: Beats (the song keeps its",
        "                    bars when the tempo changes) or Time (it keeps its seconds); markers, the loop",
        "MIDI editor (Tab to its notes)",
        "  arrows            no note selected: the cursor (a grid step, a pitch); else move the notes",
        "  Enter             select the note at the cursor, or put one there   Esc: let them go",
        "  , .  + -          shorter, longer; velocity    Del  0  G  Ctrl+D/U/C/X/V as in the window",
        "  F2 (Steps view)   the row at the cursor: its swing (the track's, or its own)",
        "  F2 (piano roll)   the note tools: Tools… (strum, arpeggiate, chop, flam, legato, quantize,",
        "                    humanize, velocity shape: previewed, Enter applies), overlaps, legato",
        "  K, Enter          the knife: Enter splits the notes at the cursor (K or Esc: put it away)",
        "  Ctrl+J  P         join the selected notes of a pitch; select the cursor's row (every note of it)",
        "Track panel",
        "  M S               mute, solo      Left/Right: volume (dB)    [ ]: pan",
        "  Shift+S           solo safe: heard while other tracks are soloed (a blue s)",
        "  Enter             rename          A / N: a new audio / MIDI track    Del: remove it",
        "  E  I  Shift+I     effects (the master's on its row), instrument menu, edit the instrument",
        "  U  K  O  D        automation lanes, takes, the next output pair, sends to return tracks",
        "  Shift+R           arm it for recording (●): R records every armed track, each from its input",
        "  H  Shift+H        an audio track's monitoring (Auto; In: »; Off: ×), its input (● marks armed)",
        "  Shift+Up/Down     select several tracks (changes then go to all of them)",
        "  Alt+Up/Down       move the track(s)   Ctrl+D: duplicate (F2: without clips)",
        "  F2                its menu: Add track (audio, MIDI, return: below it), Output (master, a group's bus or",
        "                    another track), groups, freezing; Lock keyboard focus (a MIDI track)",
        "  Shift+K           the keyboards play this MIDI track too, or no more (♪ before its name); several",
        "                    play together; F2: Lock keyboard focus keeps them while you move to other tracks",
        "  F2 (master, none) Add a track: audio, MIDI or return, at the end",
        "A group's row (its bus)",
        "  M S               mute, solo everything in it    Enter: rename",
        "  Left/Right  [ ]   its bus's fader (dB) and pan",
        "  E  U  D           its bus's effects, automation lanes, sends to return tracks",
        "  F2                its menu: Add track (in it), Fold only (no bus), Output, Put in group, Ungroup",
        "Explorer",
        "  Enter             open a folder or project; add a sample at the cursor    Space: hear it",
        "The built-in synth (I on a MIDI track: Synth…; Shift+I while it plays it)",
        "  Up/Down           a control (its section, name, value, and what modulates it: ~LFO +25 %)",
        "  Left/Right        change it a step (Shift: finer; PgUp/PgDn: ten)  Home/End: its ends",
        "  Backspace         its default      Enter: a choice's next (wave, filter type…)",
        "  M, Modulate…      a source (Env 1, Env 2, LFO) onto a control, and how far (0 %: removed)",
        "  Presets, Save     Tab to them: presets by category (Init (classic): the old voice); your own",
        "  Modulation        Tab to it: every routing; Add…, Edit…, Remove",
        "The Drawn synth (I on a MIDI track: Drawn synth…; Shift+I while it plays it): its wave",
        "  Left/Right        a point (Shift: 16)  Home/End: the first, the last",
        "  Up/Down           its value a 32nd up or down (Shift: a 256th)",
        "Dialogs: Tab moves between controls, Enter is the default button, Esc cancels.",
        "Keys: Options > Keyboard shortcuts… changes them (those marked \"not in the terminal\" only work in the window).",
        "",
        "Any other key closes this.",
    };
    int w = 4;
    for (auto& l : lines) w = std::max(w, columns(l) + 4);
    w = std::min(w, s.width() - 2);
    int h = std::min((int)lines.size() + 2, s.height() - 2);
    int x = (s.width() - w) / 2, y = (s.height() - h) / 2;
    int rows = std::max(0, h - 2);
    bool scrolls = (int)lines.size() > rows;  // (a short terminal: Up/Down, PgUp/PgDn scroll it)
    helpTop = std::clamp(helpTop, 0, std::max(0, (int)lines.size() - rows));
    s.fill(x, y, w, h, U' ', kMenu);
    s.box(x, y, w, h, kMenu, scrolls ? "Keys (Up/Down: more)" : "Keys");
    for (int i = 0; i < rows; i++) {
        const std::string& l = lines[(size_t)(helpTop + i)];
        bool heading = !l.empty() && l[0] != ' ';
        s.text(x + 2, y + 1 + i, l, heading ? style(Black, White, Bold) : kMenu, w - 4);
    }
}

// --- the terminal's loop ---

namespace {

struct Loop {
    Terminal& term;
    Front& front;
    KeyDecoder keys;
    Screen screen, shown;
    double lastFrame = 0;
    bool utf8 = Terminal::utf8();

    void size() {
        int w = 80, h = 24;
        term.size(w, h);
        if (w != screen.width() || h != screen.height()) screen.resize(w, h);
    }

    void frame() {
        double now = plat::now();
        ui::App& app = ui::App::get();
        app.advanceTimers(std::min(1.0, std::max(0.0, now - lastFrame)));
        lastFrame = now;
        app.tick();
        if (term.resized()) {
            size();
            shown.resize(1, 1);  // (drawn afresh)
        }
        front.draw(screen);
        // (as one update, where the terminal can (DEC mode 2026; others ignore it): a big frame never shown half drawn)
        std::string out = screen.output(shown, utf8, term.light());
        if (!out.empty()) term.write("\x1b[?2026h" + out + "\x1b[?2026l");
    }

    void run(const std::function<bool()>& done) {
        while (!done() && !front.wantsQuit()) {
            double now = plat::now();
            int wait = (int)std::ceil(std::max(0.0, lastFrame + FRAME_S - now) * 1000);
            if (keys.pending()) wait = std::min(wait, (int)(ESC_WAIT_S * 1000) + 1);
            plat::processEvents(wait);
            for (const Key& k : keys.take(plat::now())) {
                front.key(k);
                if (done() || front.wantsQuit()) break;
            }
            if (plat::now() - lastFrame >= FRAME_S) frame();
        }
    }
};

}  // namespace

int run(const std::vector<std::string>& files) {
    Terminal term;
    std::string why;
    if (!term.open(why)) {
        fprintf(stderr, "Tiny DAW: %s\n", why.c_str());
        return 1;
    }
    // diagnostics (always on, unless TINYDAW_DEBUG=0): stderr is the screen here, so they go to a file,
    // named when the program ends; stderr sent elsewhere already (2>file) is left as it is
    std::string log;
    int screenErr = -1;
    bool diagnostics = util::debug();
    if (diagnostics && isatty(2)) {
        std::string dir = paths::makeDataDir();
        int fd = dir.empty() ? -1 : ::open((dir + "/terminal.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            log = dir + "/terminal.log";
            screenErr = dup(2);
            dup2(fd, 2);
            ::close(fd);
        } else {
            diagnostics = false;  // (they'd be drawn over the screen)
        }
    }
    if (diagnostics) setenv("TINYDAW_WINE_TRACE", "1", 0);  // (the Wine host's diagnostics too)
    plat::initWithoutDisplay();
    ui::App& app = ui::App::get();
    app.initHeadless();  // (the toolkit without a display: windows aren't shown; its clock is moved on by Loop)
    util::setDebug(diagnostics);  // (initHeadless turns them off, for the tests)
    util::debugf("tui: the terminal's background: %s, UTF-8: %s\n", term.light() ? "light" : "dark", Terminal::utf8() ? "yes" : "no");
    int rc = 0;
    {
        auto* window = new MainWindow();
        window->resizeWindow(1280, 800);
        app.paintWindows = false;  // (shown, as the toolkit sees it: its shortcuts and focus work; drawn as text)
        window->showWindow();
        for (const std::string& path : files) {
            if (util::extension(path) == ".json") window->openProjectFile(path);
            else window->timeline->addFile(path);
        }
        Front front(*window);
        Loop loop{term, front};
        loop.size();
        loop.lastFrame = plat::now();
        int input = plat::watchFd(0, [&loop] { loop.keys.feed(loop.term.read(), plat::now()); });
        loop.keys.feed(term.read(), plat::now());  // (keys typed while the terminal was set up)
        app.nestedLoop = [&loop](const std::function<bool()>& done) { loop.run(done); };
        // work kept by a run that didn't close normally; plugins whose kind isn't known yet (as main.cpp)
        app.singleShot(0, [window] {
            if (!window->closing) window->autosaver->offerRecovery();
        });
        app.singleShot(1500, [window] {
            if (!window->closing) window->plugins->findKinds();
        });
        loop.frame();
        loop.run([] { return false; });
        app.nestedLoop = nullptr;
        plat::unwatchFd(input);
        if (!window->closing) window->requestClose();
        delete window;
    }
    app.collectGarbage();
    term.close();
    plat::shutdown();
    if (screenErr >= 0) {
        dup2(screenErr, 2);
        ::close(screenErr);
        fprintf(stderr, "Tiny DAW: this run's diagnostics are in %s\n", log.c_str());
    }
    return rc;
}

}  // namespace tui
