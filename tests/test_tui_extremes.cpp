// The terminal UI (exbit --tui) at its extremes, the worst cases: every terminal size from 1x1 to
// 500x200 with the main window and each of the program's windows open (the MIDI editor and its Steps view, the
// effects, an effect's knobs, the synth, the Drawn synth's canvas, the sampler, the Pad Kit, the command palette,
// the menus, F1's list, a question, a message, the file chooser, a rename dialog), resized between them over and
// over; names in every script (CJK, emoji, a ZWJ sequence, combining accents, Hebrew, control characters).
// Each picture is fed, as bytes, to an emulated terminal (tests/tui_vt.h): what it shows must be the picture, cell
// for cell (wide characters in two cells, never cut in half), nothing past the right edge, no control character
// sent; its colours readable on a dark and on a light terminal; highlights still shown after reverse video;
// dialogs answerable and closable at the tiniest sizes; a frame within the loop's 33 ms at 500x200.
// The pictures: build/ui-samples/tui-audit/*.txt.
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "audio/synth_patch.h"
#include "check.h"
#include "core/util.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/pad_kit_dialog.h"
#include "ui/sampler_dialog.h"
#include "ui/synth_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"
#include "tui_vt.h"

using namespace tui;
using tui_vt::mismatches;
using tui_vt::Vt;

namespace {

const std::string OUT = "build/ui-samples/tui-audit";
const int SR = 48000;
const std::vector<std::pair<int, int>> SIZES = {{1, 1},   {2, 2},   {10, 5},   {20, 8},    {40, 12},   {59, 13},
                                                {60, 14}, {80, 24}, {132, 43}, {300, 100}, {500, 200}};
const std::vector<std::string> NAMES = {"ドラムとベース",
                                        "🥁 Drums",
                                        "Cafe\xCC\x81 au lait",  // (a combining accent)
                                        "שלום עולם",
                                        "👩‍🎤 Singer",
                                        "中文音轨名称很长很长很长很长很长很长很长很长",
                                        "tab\there del\x7F c1\xC2\x85 bell\x07 esc\x1B[2J",
                                        "plain"};

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }
void settle(int frames = 2) {
    for (int i = 0; i < frames; i++) {
        ui::App::get().advanceTimers(1.0 / 30);
        ui::App::get().tick();
    }
}
std::string sizeText(const Screen& s) { return std::to_string(s.width()) + "x" + std::to_string(s.height()); }
std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s;
}

// --- what a cell looks like on a dark terminal (its ground black, its text light) or a light one ---
struct Look {
    int fg, bg;
};
Look lookOf(const Style& st, bool dark) {
    int fg = st.fg == Default ? (dark ? 7 : 0) : st.fg, bg = st.bg == Default ? (dark ? 0 : 7) : st.bg;
    if (st.attr & Reverse) std::swap(fg, bg);
    return Look{fg, bg};
}
bool unreadable(const Style& st, bool dark) {
    Look l = lookOf(st, dark);
    if (l.fg == l.bg) return true;
    if (dark) return l.bg == Black && l.fg == Black;  // (blue on black: the grid's dots, faint on purpose)
    return l.bg == White && (l.fg == Yellow || l.fg == Cyan || l.fg == White);
}

int failures = 0;  // (each kind reported once a picture, not once a cell)
void fail(const std::string& what, const Screen& s, int row) {
    CHECK(false);
    if (++failures < 60)
        fprintf(stderr, "  %s (%s), row %d: \"%s\"\n", what.c_str(), sizeText(s).c_str(), row,
                row >= 0 && row < s.height() ? s.line(row).c_str() : "");
}

// what must hold of every picture
void checkPicture(const Screen& s, const std::string& where, bool dump = true) {
    int W = s.width(), H = s.height();
    if (dump && W * H <= 300 * 100) std::ofstream(OUT + "/" + fileSafe(where) + "-" + sizeText(s) + ".txt") << s.lines();
    // a wide character's halves together; no line wider than the terminal
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char32_t c = s.at(x, y)->ch;
            if (c == WIDE_TAIL && (x == 0 || cellWidth(s.at(x - 1, y)->ch) != 2)) return fail(where + ": half a wide character", s, y);
            if (c != WIDE_TAIL && cellWidth(c) == 2 && (x + 1 >= W || s.at(x + 1, y)->ch != WIDE_TAIL))
                return fail(where + ": a wide character without its second cell", s, y);
            if (c != WIDE_TAIL && cellWidth(c) < 1) return fail(where + ": a control or zero-width character in a cell", s, y);
        }
        if (columns(s.line(y)) > W) return fail(where + ": a line wider than the terminal", s, y);
    }
    // the bytes, to a terminal: what it shows is the picture, styles and all; nothing but escape sequences and text
    Screen shown(1, 1);
    std::string out = s.output(shown, true);
    for (size_t i = 0; i < out.size(); i++) {
        unsigned char b = (unsigned char)out[i];
        if ((b < 0x20 && b != 0x1B) || b == 0x7F || (b == 0xC2 && i + 1 < out.size() && (unsigned char)out[i + 1] < 0xA0))
            return fail(where + ": a control character sent (byte " + std::to_string(b) + ")", s, -1);
    }
    Vt vt(W, H);
    vt.feed(out);
    int row = -1;
    if (mismatches(s, vt, row, true)) return fail(where + ": the terminal shows something else", s, row);
    if (vt.overflow) return fail(where + ": drawn past the right edge", s, -1);
    // without UTF-8: ASCII only, every line still where it is (the box lines' columns)
    Screen shownA(1, 1);
    std::string ascii = s.output(shownA, false);
    for (unsigned char b : ascii)
        if (b >= 0x80) return fail(where + ": non-ASCII sent to an ASCII terminal", s, -1);
    Vt va(W, H);
    va.feed(ascii);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (s.at(x, y)->ch == U'│' && !(x == W - 1 && y == H - 1) && va.at(x, y).ch != U'|')
                return fail(where + ": ASCII: a line out of its column", s, y);
    // readable: dark, as the cells are; light, as they are sent to a light terminal
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const Cell& c = *s.at(x, y);
            if (c.ch == U' ' || c.ch == U'█' || c.ch == WIDE_TAIL) continue;
            if (unreadable(c.st, true)) return fail(where + ": unreadable on a dark terminal at col " + std::to_string(x), s, y);
        }
    Screen shownL(1, 1);
    Vt vl(W, H);
    vl.feed(s.output(shownL, true, true));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const Cell& c = vl.at(x, y);
            if (c.ch == U' ' || c.ch == U'█' || c.ch == 0) continue;
            if (unreadable(c.st, false)) return fail(where + ": unreadable on a light terminal at col " + std::to_string(x), s, y);
        }
}

// --- the program ---
struct Rig {
    MainWindow* w = nullptr;
    Front* f = nullptr;
    std::vector<TrackPtr> tracks;
    ClipPtr midi;
};
std::function<void(const std::function<bool()>&)> inModal;

Rig makeRig(int nTracks, int clipsPerTrack) {
    Rig r;
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(1280, 800);
    r.w->showWindow();
    r.w->engine.close();
    std::vector<Note> notes;
    for (int n = 0; n < 8; n++) notes.push_back({n * 0.5, n * 0.5 + 0.4, 60.0 + n, 100});
    ClipPtr proto = makeMidiClip("proto", notes, 4, SR);
    std::vector<ClipPtr> clips;
    for (int i = 0; i < nTracks; i++) {
        TrackPtr t = r.w->project.addTrack(Kind::Midi);
        t->name = NAMES[(size_t)i % NAMES.size()];
        r.tracks.push_back(t);
        for (int k = 0; k < clipsPerTrack; k++) {
            ClipPtr c = std::make_shared<Clip>(*proto);
            c->name = NAMES[(size_t)(i + k + 1) % NAMES.size()];
            c->track = t;
            c->start = (int64_t)((k * 2.5 + (i % 3) * 0.25) * SR);
            clips.push_back(c);
            if (!r.midi) r.midi = c;
        }
    }
    r.w->project.clips = clips;
    r.w->project.markers.append(Marker{2.0, "マーカー 🎯"});
    r.w->timeline->lanesChanged();
    r.w->timeline->tracksChanged.emit();
    r.f = new Front(*r.w);
    app.nestedLoop = [](const std::function<bool()>& done) {
        if (inModal) inModal(done);
        for (int i = 0; i < 40 && !done(); i++) settle(1);
    };
    settle(3);
    return r;
}

void closeWindows(Rig& r) {
    for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
        if (win != r.w && win->isWindowShown() && !win->isPopup()) win->hideWindow();
    ui::App::get().closeAllPopups();
    r.f->closeMenus();
    r.f->helpShown = false;
    settle(1);
}

void closeRig(Rig& r) {
    inModal = nullptr;
    r.w->closeFxWindows();
    closeWindows(r);
    delete r.f;
    delete r.w;
    r = Rig();
    settle(1);
    ui::App::get().nestedLoop = nullptr;
}

ui::Action* findAction(ui::Menu& m, const std::string& prefix) {
    if (m.aboutToShow) m.aboutToShow(&m);
    for (auto& a : m.actions()) {
        if (menuText(a->text).compare(0, prefix.size(), prefix) == 0 && !a->submenu) return a.get();
        if (a->submenu)
            if (ui::Action* x = findAction(*a->submenu, prefix)) return x;
    }
    return nullptr;
}

ui::Window* topWindow(Rig& r) {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (*it != r.w && (*it)->isWindowShown() && !ui::Menu::ofWindow(*it)) return *it;
    return nullptr;
}

// every size: drawn and checked
void everySize(Rig& r, const std::string& name) {
    for (auto [W, H] : SIZES) {
        Screen s(W, H);
        r.f->draw(s);
        checkPicture(s, name);
        // (a window over the main window leaves its menu bar alone)
        if (W >= 60 && H >= 14 && topWindow(r)) CHECK(s.line(0).find("File") != std::string::npos);
    }
}

// Tab through the window on top at this size: the control with the keyboard is always shown (the picture changes)
void tabWalk(Rig& r, int W, int H, const std::string& name) {
    ui::Window* win = topWindow(r);
    if (!win) return;
    Screen s(W, H);
    int stops = 0, unseen = 0;
    std::vector<ui::Widget*> seen;
    for (int i = 0; i < 40; i++) {
        ui::Widget* fw = win->focusWidget();
        if (fw && std::find(seen.begin(), seen.end(), fw) != seen.end()) break;
        seen.push_back(fw);
        r.f->draw(s);
        std::string before = s.lines();
        auto styles = [&] {
            std::string o;
            for (int y = 0; y < s.height(); y++)
                for (int x = 0; x < s.width(); x++) o += (char)s.at(x, y)->st.attr, o += (char)s.at(x, y)->st.fg;
            return o;
        };
        std::string beforeSt = styles();
        r.f->key(key(plat::Key_Tab));
        settle(1);
        if (!ui::Widget::alive(win) || !win->isWindowShown()) break;
        r.f->draw(s);
        if (win->focusWidget() != fw) {
            stops++;
            if (s.lines() == before && styles() == beforeSt) {
                unseen++;
                ui::Widget* now = win->focusWidget();
                Point p = now ? now->mapTo(win, Point(0, 0)) : Point(-1, -1);
                fprintf(stderr, "  %s at %dx%d: Tab to a %s at %d,%d (%dx%d px) changed nothing shown\n", name.c_str(), W, H,
                        now ? now->type().c_str() : "nothing", p.x, p.y, now ? now->width() : 0, now ? now->height() : 0);
            }
        }
    }
    if (unseen) {
        fprintf(stderr, "  %s at %dx%d: %d of %d Tabs changed nothing shown\n", name.c_str(), W, H, unseen, stops);
        r.f->draw(s);
        std::ofstream(OUT + "/tab-unseen-" + fileSafe(name) + "-" + sizeText(s) + ".txt") << s.lines();
    }
    CHECK(unseen == 0);
}

// resized over and over, at random, keys between: the terminal always shows the picture
void rapidResizes(Rig& r, const std::string& name, std::mt19937& rng) {
    Screen s(80, 24), shown(1, 1);
    Vt vt(80, 24);
    const int keys[] = {plat::Key_Tab, plat::Key_Down, plat::Key_Up, plat::Key_Right, plat::Key_Left};
    int bad = 0;
    for (int i = 0; i < 60; i++) {
        int W, H;
        if (i % 3 == 0) std::tie(W, H) = SIZES[rng() % SIZES.size()];
        else W = 1 + (int)(rng() % 520), H = 1 + (int)(rng() % 210);
        if (i % 5 == 4) W = s.width(), H = s.height();  // (the same size: only what changed is sent)
        if (W != s.width() || H != s.height()) {
            s.resize(W, H);
            shown.resize(1, 1);  // (as the loop does on SIGWINCH: drawn afresh)
            vt = Vt(W, H);
        }
        if (i % 2) r.f->key(key(keys[rng() % 5]));
        r.tracks[0]->name = NAMES[rng() % NAMES.size()];  // (a name of another width each time)
        settle(1);
        r.f->draw(s);
        vt.feed(s.output(shown, true));
        int row = -1;
        if (mismatches(s, vt, row, true) && bad++ < 3)
            fprintf(stderr, "  %s: after resizing to %dx%d the terminal shows something else at row %d\n", name.c_str(), W, H, row);
    }
    CHECK(bad == 0);
}

// ============================================================================================
void screenBasics() {
    // widths as the terminal counts them
    for (char32_t c : {U'a', U'é', U'─', U'█', U'…', U'ド', U'中', U'🥁', U'👩', U'🎤', U'́', U'‍', U'א', U'♩', U'⏮', U'▶'})
        CHECK(cellWidth(c) == wcwidth((wchar_t)c));
    CHECK(cellWidth(0x85) < 0 && cellWidth(0x7F) < 0 && cellWidth(0x1B) < 0);
    CHECK(columns("ドラム") == 6);
    CHECK(fit("ドラム", 4) == "ド…");
    CHECK(fit("ドラム", 5) == "ドラ…");
    CHECK(fit("👩‍🎤 Singer", 2, false) == "👩");
    CHECK(fit("abc", 3) == "abc");
    // text cut: never inside a wide character, "…" at the end
    Screen s(10, 3);
    CHECK(s.text(0, 0, "ドラムとベース", Style()) == 10);
    CHECK(s.line(0) == "ドラムと…");
    s.text(0, 1, "abcdefghijklmnop", Style(), 5);
    CHECK(s.line(1) == "abcd…");
    s.text(0, 2, "ab   ", Style(), 2);  // (only spaces cut: no "…")
    CHECK(s.line(2) == "ab");
    // half of a wide character written over: the other half a space
    Screen t(6, 1);
    t.text(0, 0, "中文", Style());
    t.set(1, 0, U'a', Style());
    CHECK(t.line(0) == " a文");
    t.set(2, 0, U'b', Style());
    CHECK(t.line(0) == " ab");
    t.set(5, 0, U'中', Style());  // (no room for its second cell)
    CHECK(t.at(5, 0)->ch == U' ');
    // control characters: never sent
    Screen u(20, 2);
    u.text(0, 0, NAMES[6], Style());
    checkPicture(u, "controls", false);
    // colours: only the 8 (38 starts an extended colour); darker on a light ground, reverse video left alone
    Screen v(4, 2), none(1, 1);
    v.set(0, 0, U'x', Style{8, Default, Plain});
    CHECK(v.output(none, true).find("38") == std::string::npos);
    Screen l(4, 2), shown(1, 1);
    l.set(0, 0, U'y', style(Yellow));
    l.set(1, 0, U'r', style(Yellow, Default, Reverse));
    l.set(2, 0, U'b', style(Yellow, Blue));
    std::string light = l.output(shown, true, true);
    CHECK(light.find(";35m") != std::string::npos && light.find(";7;33m") != std::string::npos && light.find(";33;44m") != std::string::npos);
    // the terminal's answer about its background; COLORFGBG
    CHECK(lightFromReply("\x1b]11;rgb:ffff/ffff/ffff\x07") == 1);
    CHECK(lightFromReply("\x1b]11;rgb:0000/0000/0000\x1b\\") == 0);
    CHECK(lightFromReply("\x1b]11;rgb:fd/f6/e3\x07") == 1);  // (Solarized light)
    CHECK(lightFromReply("\x1b]11;rgb:1e1e/1e1e/2e2e\x07") == 0);
    CHECK(lightFromReply("\x1b[?62;22c") == -1 && lightFromReply("\x1b]11;rgb:zz\x07") == -1);
    CHECK(lightFromColorFgBg("15;0") == 0 && lightFromColorFgBg("0;15") == 1 && lightFromColorFgBg("0;default;7") == 1);
    CHECK(lightFromColorFgBg(nullptr) == -1 && lightFromColorFgBg("") == -1 && lightFromColorFgBg("15;default") == -1);
    // random text, anywhere, at any width, sent bit by bit: the terminal always shows the picture
    std::mt19937 rng(7);
    Screen p(37, 9), ps(1, 1);
    Vt vt(37, 9);
    int bad = 0;
    for (int i = 0; i < 3000; i++) {
        p.text((int)(rng() % 45) - 4, (int)(rng() % 11) - 1, NAMES[rng() % NAMES.size()], style((uint8_t)(1 + rng() % 7)),
               (int)(rng() % 12) - 1);
        if (i % 7 == 0) p.set((int)(rng() % 37), (int)(rng() % 9), U'│', Style());
        vt.feed(p.output(ps, true));
        int row = -1;
        bad += mismatches(p, vt, row, true) > 0;
    }
    CHECK(bad == 0);
    checkPicture(p, "random-text");
}

// the highlights, after reverse video: each still shows, in colour and without
void highlights() {
    Rig r = makeRig(4, 2);
    Front& f = *r.f;
    Screen s(100, 30);
    // the bar's menu: the entry highlighted on a ground of its own
    f.openBarMenu(0);
    f.draw(s);
    int hy = -1, hx = -1;
    for (int y = 1; y < s.height() && hy < 0; y++)
        for (int x = 0; x < 40 && hy < 0; x++)
            if (s.at(x, y)->st.attr & Reverse && s.at(x, y)->ch != U' ') hy = y, hx = x;
    CHECK(hy > 0);
    if (hy > 0) {
        CHECK(lookOf(s.at(hx, hy)->st, true).bg != lookOf(s.at(hx, hy + 1)->st, true).bg);
    }
    f.closeMenus();
    // the pane with the keyboard: its title reversed (without colour too), the row chosen there reversed and bold
    f.pane = Front::Pane::Panel;
    f.selectLane(0);
    f.draw(s);
    bool titleReversed = false, rowBold = false;
    for (int x = 0; x < s.width(); x++) {
        if (s.line(3).find("Tracks") != std::string::npos && (s.at(x, 3)->st.attr & Reverse)) titleReversed = true;
        if ((s.at(x, 4)->st.attr & (Reverse | Bold)) == (Reverse | Bold)) rowBold = true;
    }
    CHECK(titleReversed && rowBold);
    // the command palette (a modal dialog: looked at inside its loop): the text cursor in its field, and the list's
    // row chosen with the keyboard
    int cursors = 0, rowsChosen = 0;
    inModal = [&](const std::function<bool()>& done) {
        f.key(key('Z', 0, "z"));
        settle(1);
        f.draw(s);
        for (int y = 0; y < s.height(); y++)
            for (int x = 1; x < s.width(); x++)
                if ((s.at(x, y)->st.attr & Reverse) && s.at(x - 1, y)->st == style(Black, White, Bold)) {
                    cursors++;
                    CHECK(lookOf(s.at(x, y)->st, true).bg != lookOf(s.at(x - 1, y)->st, true).bg);
                }
        std::ofstream(OUT + "/highlight-palette-field.txt") << s.lines();
        f.key(key(plat::Key_Backspace));
        f.key(key(plat::Key_Tab));
        settle(1);
        f.draw(s);
        for (int y = 0; y < s.height(); y++) {
            size_t at = s.line(y).find("> ");
            if (at == std::string::npos) continue;
            rowsChosen++;
            int x = columns(s.line(y).substr(0, at));  // (the row's mark)
            CHECK(lookOf(s.at(x, y)->st, true).bg != Blue);  // (not the window's own ground)
            CHECK(s.at(x, y)->st.attr & Reverse);           // (without colour: reversed)
        }
        std::ofstream(OUT + "/highlight-palette-list.txt") << s.lines();
        for (int i = 0; i < 5 && !done(); i++) f.key(key(plat::Key_Escape)), settle(1);
    };
    r.w->showPalette();
    settle(1);
    inModal = nullptr;
    CHECK(cursors == 1);
    CHECK(rowsChosen == 1);
    closeWindows(r);
    closeRig(r);
}

// every window, every size; Tab at small sizes; rapid resizes
void windows() {
    Rig r = makeRig(8, 3);
    Front& f = *r.f;
    MainWindow& w = *r.w;
    std::mt19937 rng(11);
    TrackPtr synthT = r.tracks[1], drawnT = r.tracks[2], samplerT = r.tracks[3], kitT = r.tracks[5];
    std::atomic_store(&drawnT->synth, subsynth::PatchPtr(std::make_shared<const subsynth::Patch>(subsynth::drawnPatch())));
    samplerT->sampler = sampler_dialog::newSettings(r.midi);
    kitT->padKit = std::make_shared<PadKit>();
    struct Case {
        std::string name;
        std::function<void()> open;
    };
    std::vector<Case> cases = {
        {"main", [] {}},
        {"bar-menu", [&] {
             f.key(key(plat::Key_F10));
             for (int i = 0; i < 30; i++) f.key(key(plat::Key_Down));  // (to its end: the menu scrolls)
         }},
        {"context-menu", [&] { f.key(key(plat::Key_F2)); }},
        {"F1", [&] {
             f.key(key(plat::Key_F1));
             for (int i = 0; i < 5; i++) f.key(key(plat::Key_Down));
         }},
        {"midi-editor", [&] { w.openMidiEditor(r.midi); }},
        {"midi-steps", [&] {
             w.openMidiEditor(r.midi);
             settle(1);
             f.key(key(plat::Key_Tab));
             f.key(key(plat::Key_Space, 0, " "));
         }},
        {"effects", [&] { w.openTrackPlugins(r.tracks[0]); }},
        {"synth", [&] { synth_dialog::openSynth(synthT); }},
        {"drawn-synth", [&] { synth_dialog::openSynth(drawnT); }},
        {"sampler", [&] { sampler_dialog::openSampler(samplerT); }},
        {"pad-kit", [&] { pad_kit_dialog::openPadKit(kitT); }},
    };
    for (auto& c : cases) {
        c.open();
        settle(3);
        everySize(r, c.name);
        if (topWindow(r)) {
            tabWalk(r, 80, 24, c.name);
            tabWalk(r, 40, 12, c.name);
        }
        rapidResizes(r, c.name, rng);
        // closable at the tiniest size: Ctrl+W (a window), Esc (a menu, F1's list)
        Screen tiny(10, 5);
        for (int i = 0; i < 6 && (topWindow(r) || f.menuOpen() || f.helpShown); i++) {
            f.draw(tiny);
            f.key(topWindow(r) && !f.menuOpen() ? key('W', plat::ModCtrl) : key(plat::Key_Escape));
            settle(1);
        }
        f.draw(tiny);
        if (topWindow(r) || f.menuOpen() || f.helpShown) fprintf(stderr, "  %s: not closed at 10x5\n", c.name.c_str());
        CHECK(!topWindow(r) && !f.menuOpen() && !f.helpShown);
        closeWindows(r);
        r.tracks[0]->name = NAMES[0];
    }
    // modal dialogs: checked inside their loop, then answered at 10x5 by Esc
    std::vector<Case> modal = {
        {"question", [&] { ui::msg::question(&w, "未保存の変更 — unsaved", "Save the changes to “ドラムとベース 🥁”? " + NAMES[5] + " " + NAMES[5],
                                            ui::msg::Save | ui::msg::Discard | ui::msg::Cancel); }},
        {"message", [&] { ui::msg::information(&w, NAMES[4], NAMES[6] + " " + NAMES[3]); }},
        {"palette", [&] { w.showPalette(); }},
        {"file-chooser", [&] { ui::files::openFile(&w, "Open a project — プロジェクト", util::absolutePath(OUT), "Projects (*.json);;All files (*)"); }},
        {"rename", [&] {
             ui::Menu m;
             w.panel->fillTrackMenu(m, r.tracks[0]);
             if (ui::Action* a = findAction(m, "Rename")) ui::Menu::activate(a);
         }},
    };
    for (auto& c : modal) {
        bool seen = false, closed = false;
        inModal = [&](const std::function<bool()>& done) {
            if (seen) return;
            seen = true;
            settle(1);
            everySize(r, c.name);
            tabWalk(r, 80, 24, c.name);
            tabWalk(r, 20, 8, c.name);
            Screen tiny(10, 5);
            for (int i = 0; i < 6 && !done(); i++) {
                f.draw(tiny);
                f.key(key(plat::Key_Escape));
                settle(1);
            }
            closed = done();
        };
        c.open();
        settle(1);
        inModal = nullptr;
        CHECK(seen);
        if (!closed) fprintf(stderr, "  %s: not closed by Esc at 10x5\n", c.name.c_str());
        CHECK(closed);
        closeWindows(r);
    }
    closeRig(r);
}

// a big project at 500x200: a frame (drawn and sent) within the loop's 33 ms
void frameTimes() {
    Rig r = makeRig(60, 40);
    Front& f = *r.f;
    auto measure = [&](const std::string& what) {
        Screen s(500, 200), shown(1, 1);
        f.draw(s);
        s.output(shown, true);
        double worst = 0;
        for (int i = 0; i < 8; i++) {
            f.key(key(i % 2 ? plat::Key_Down : plat::Key_Right));
            settle(1);
            auto t0 = std::chrono::steady_clock::now();
            f.draw(s);
            s.output(shown, true);
            worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        printf("   500x200 %s: worst frame %.1f ms\n", what.c_str(), worst);
        CHECK(worst < 33);
    };
    measure("main window, 60 tracks, 2400 clips");
    r.w->openMidiEditor(r.midi);
    settle(2);
    measure("MIDI editor");
    closeWindows(r);
    closeRig(r);
}

// the keyboard focus's marks (♪ before the names it plays, "Tracks  ♪ locked"), a 7/8 time signature on the
// transport, and an empty panel's hint, at every size: checked as every picture, and there where there is room
void focusAndSignature() {
    Rig r = makeRig(4, 1);
    MainWindow& w = *r.w;
    for (auto& t : r.tracks) t->needsInstrument = false;
    w.timeline->setCurrentTrack(r.tracks[0]);
    w.panel->toggleFocus(r.tracks[2], true);
    ui::Menu m;
    w.panel->fillFocusMenu(m);
    for (auto& a : m.actions())
        if (a->text == "Lock keyboard focus" && a->toggled) a->toggled(true);
    w.project.timing.beatsPerBar = 7, w.project.timing.beatUnit = 8;
    w.timeline->timingChanged.emit();
    settle(3);
    everySize(r, "focus-locked-7-8");
    for (auto [W, H] : std::vector<std::pair<int, int>>{{80, 24}, {300, 100}}) {
        Screen s(W, H);
        r.f->draw(s);
        std::string all = s.lines();
        CHECK(w.project.keyFocusLocked && all.find("Tracks  ♪ locked") != std::string::npos);
        CHECK(all.find("♪ ") != std::string::npos);
        CHECK(all.find("7/8") != std::string::npos);
    }
    closeRig(r);
    Rig e = makeRig(0, 0);
    settle(2);
    everySize(e, "empty-panel");
    Screen s(80, 24);
    e.f->draw(s);
    CHECK(s.lines().find("add a track") != std::string::npos);
    closeRig(e);
}

}  // namespace

int main() {
    setlocale(LC_CTYPE, "C.UTF-8");  // (wcwidth, for the emulated terminal)
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(OUT + "/tmp");
    for (auto& e : std::filesystem::directory_iterator(OUT))
        if (e.is_regular_file() && e.path().extension() == ".txt") std::filesystem::remove(e.path());
    paths::setDataDir(util::absolutePath(OUT + "/tmp/data"));
    settings::setPath(OUT + "/tmp/settings.json");
    auto t0 = std::chrono::steady_clock::now();
    auto part = [&](const char* name, void (*fn)()) {
        fn();
        printf("tui extremes: %s (%.1f s)\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        fflush(stdout);
    };
    part("screen", screenBasics);
    part("highlights", highlights);
    part("windows", windows);
    part("keyboard focus and signature", focusAndSignature);
    part("frame times", frameTimes);
    TEST_MAIN_END();
}
