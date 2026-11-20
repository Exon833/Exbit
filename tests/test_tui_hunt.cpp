// A hunt for what the terminal UI (tinydaw --tui) shows wrong or slowly: it drives tui::Front over the headless
// main window, as tests/test_tui.cpp does, and feeds Screen::output's bytes into a small terminal emulator of its
// own (cells as wide as wcwidth() says, autowrap off as Terminal::open sets it), so what a terminal would show is
// compared with what the program meant. It measures the bytes per frame (idle, playing, scrolling, resized) and the
// time a frame takes with a heavy project; checks what reverse video makes of the highlights (menus, lists, the text
// cursor, selected notes), whether each control with the keyboard is marked, with and without colour, and the order
// Tab goes in; Unicode names; resizing with a menu or a window open; mouse and paste sequences.
// Findings are reported, not failed (the run exits 0): build/ui-samples/tui-hunt/findings.txt, screens beside it.
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/sound_file.h"
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
#include "ui/midi_editor.h"
#include "ui/midi_ports_dialog.h"
#include "ui/pad_kit_dialog.h"
#include "ui/sampler_dialog.h"
#include "ui/synth_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/tui-hunt";
const int SR = 48000;
std::vector<std::string> findings;
auto started = std::chrono::steady_clock::now();
double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }
double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}
void found(const std::string& s) {
    findings.push_back(s);
    printf("tui hunt: %s\n", s.c_str());
    fflush(stdout);
}
std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s;
}
std::string dump(const Screen& s, const std::string& name) {
    std::string file = OUT + "/" + fileSafe(name) + ".txt";
    std::ofstream(file) << s.lines();
    return file;
}
Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

void settle(int frames = 3) {
    for (int i = 0; i < frames; i++) {
        ui::App::get().advanceTimers(1.0 / 30);
        ui::App::get().tick();
    }
}

// --- what a terminal shows: a grid fed with the bytes (CUP, ED, SGR; characters as wide as wcwidth) ---
struct Vt {
    int w, h, cx = 0, cy = 0;
    std::vector<Cell> g;  // (ch 0: the right half of a wide character)
    Style cur;
    int cups = 0, sgrs = 0, clears = 0, chars = 0;
    std::map<size_t, std::u32string> marks;  // (combining characters on a cell: shown with it)
    Vt(int w_, int h_) : w(w_), h(h_), g((size_t)w_ * h_) {}
    Cell& at(int x, int y) { return g[(size_t)y * w + x]; }
    void put(char32_t cp) {
        int cw = wcwidth((wchar_t)cp);
        if (cw < 0) cw = 1;
        chars++;
        if (cw == 0) {  // (combining, zero-width joiner: on the cell before, the cursor stays)
            if (cx > 0) marks[(size_t)cy * w + cx - 1] += cp;
            return;
        }
        if (cx >= w) cx = w - 1;  // (no autowrap: the last column is written over)
        at(cx, cy) = Cell{cp, cur};
        marks.erase((size_t)cy * w + cx);
        if (cw == 2 && cx + 1 < w) at(cx + 1, cy) = Cell{0, cur};
        cx = std::min(w, cx + cw);
    }
    void feed(const std::string& b) {
        for (size_t i = 0; i < b.size();) {
            unsigned char c = (unsigned char)b[i];
            if (c == 0x1b && i + 1 < b.size() && b[i + 1] == '[') {
                size_t j = i + 2;
                bool priv = j < b.size() && b[j] == '?';
                if (priv) j++;
                std::vector<int> ps{0};
                while (j < b.size() && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
                    if (b[j] == ';') ps.push_back(0);
                    else ps.back() = ps.back() * 10 + (b[j] - '0');
                    j++;
                }
                char f = j < b.size() ? b[j] : 0;
                if (!priv && f == 'H') {
                    cups++;
                    cy = std::clamp((ps[0] ? ps[0] : 1) - 1, 0, h - 1);
                    cx = std::clamp((ps.size() > 1 && ps[1] ? ps[1] : 1) - 1, 0, w - 1);
                } else if (!priv && f == 'J') {
                    clears++;
                    for (auto& cell : g) cell = Cell{U' ', cur};
                    marks.clear();
                } else if (!priv && f == 'm') {
                    sgrs++;
                    for (int p : ps) {
                        if (p == 0) cur = Style();
                        else if (p == 1) cur.attr |= Bold;
                        else if (p == 4) cur.attr |= Underline;
                        else if (p == 7) cur.attr |= Reverse;
                        else if (p >= 30 && p <= 37) cur.fg = (uint8_t)(p - 30);
                        else if (p >= 40 && p <= 47) cur.bg = (uint8_t)(p - 40);
                    }
                }
                i = j + 1;
                continue;
            }
            int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            std::u32string cps = codePoints(b.substr(i, (size_t)n));
            for (char32_t cp : cps) put(cp);
            i += (size_t)n;
        }
    }
    std::string line(int y) {
        std::string s;
        for (int x = 0; x < w; x++) {
            if (at(x, y).ch) s += utf8Of(at(x, y).ch);
            auto m = marks.find((size_t)y * w + x);
            if (m != marks.end())
                for (char32_t c : m->second) s += utf8Of(c);
        }
        size_t e = s.find_last_not_of(' ');
        return e == std::string::npos ? "" : s.substr(0, e + 1);
    }
};

// the cells that differ between what was meant and what a terminal shows (the first row that differs)
int mismatches(const Screen& s, Vt& vt, int& firstRow, bool styles = false) {
    int n = 0;
    firstRow = -1;
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            if (x == s.width() - 1 && y == s.height() - 1) continue;  // (never written: reported on its own)
            const Cell& a = *s.at(x, y);
            const Cell& b = vt.at(x, y);
            if (a.ch != b.ch || (styles && a.st != b.st)) {
                n++;
                if (firstRow < 0) firstRow = y;
            }
        }
    return n;
}

// --- what a cell looks like: its colours after reverse video (a dark terminal: the default background black,
// the default text light grey); a space is its background, a full block its text colour ---
struct Look {
    int fg, bg, attr;
    bool operator==(const Look& o) const { return fg == o.fg && bg == o.bg && attr == o.attr; }
};
Look lookOf(const Style& st, bool dark = true) {
    int fg = st.fg == Default ? (int)(dark ? White : Black) : (int)st.fg, bg = st.bg == Default ? (int)(dark ? Black : White) : (int)st.bg;
    if (st.attr & Reverse) std::swap(fg, bg);
    return Look{fg, bg, st.attr & (Bold | Underline)};
}
int solidOf(const Cell& c) {  // the one colour the cell shows, -1: text on a ground
    Look l = lookOf(c.st);
    if (c.ch == U' ') return l.bg;
    if (c.ch == U'█') return l.fg;
    return -1;
}
const char* colorName(int c) {
    static const char* n[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    return c >= 0 && c < 8 ? n[c] : "?";
}
std::string styleText(const Style& st) {
    auto c = [](int v) { return v == Default ? std::string("default") : std::string(colorName(v)); };
    std::string a;
    if (st.attr & Bold) a += "+bold";
    if (st.attr & Reverse) a += "+reverse";
    if (st.attr & Underline) a += "+underline";
    return c(st.fg) + " on " + c(st.bg) + a;
}
std::string lookText(const Look& l) {
    return std::string(colorName(l.fg)) + " on " + colorName(l.bg) + (l.attr & Bold ? " bold" : "");
}

// the picture as a terminal without colour shows it (characters, reverse, bold, underline), and with colour
std::vector<std::pair<char32_t, int>> mono(const Screen& s) {
    std::vector<std::pair<char32_t, int>> v;
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) v.push_back({s.at(x, y)->ch, s.at(x, y)->st.attr});
    return v;
}
std::vector<std::tuple<char32_t, int, int, int>> looks(const Screen& s) {
    std::vector<std::tuple<char32_t, int, int, int>> v;
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            const Cell& c = *s.at(x, y);
            Look l = lookOf(c.st);
            // (a space shows only its ground; a block only its colour)
            if (c.ch == U' ') v.push_back({U' ', 0, l.bg, 0});
            else if (c.ch == U'█') v.push_back({U'█', l.fg, 0, 0});
            else v.push_back({c.ch, l.fg, l.bg, l.attr});
        }
    return v;
}

// unreadable colour pairs: text in the colour of its ground, or blue/black text on black
std::map<std::string, std::string> unreadable;  // style → where first
void scanColours(const Screen& s, const std::string& where) {
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            const Cell& c = *s.at(x, y);
            if (c.ch == U' ' || c.ch == U'█' || c.ch == 0) continue;
            Look l = lookOf(c.st);
            bool same = l.fg == l.bg;
            bool dim = l.bg == Black && (l.fg == Blue || (l.fg == Black)) && !(c.st.attr & Bold);
            if (!same && !dim) continue;
            std::string k = styleText(c.st) + (same ? " (text in its ground's colour)" : " (dark text on black)");
            if (!unreadable.count(k)) {
                std::string ctx = s.line(y);
                unreadable[k] = where + ", row " + std::to_string(y) + " col " + std::to_string(x) + " '" + utf8Of(c.ch) + "'";
            }
        }
}

// --- the program ---
struct Rig {
    MainWindow* w = nullptr;
    Front* f = nullptr;
    std::vector<TrackPtr> tracks;
    ClipPtr audio, midi;
};

AudioPtr sine(double seconds, double hz) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[(size_t)i * 2] = a->samples[(size_t)i * 2 + 1] = (float)(0.4 * std::sin(2 * M_PI * hz * i / SR));
    return a;
}

std::function<void(const std::function<bool()>&)> inModal;  // what a modal dialog's loop does (set by each part)

Rig makeRig(int nTracks, int clipsPerTrack, const std::vector<std::string>& names = {}) {
    Rig r;
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(1280, 800);
    r.w->showWindow();
    r.w->engine.close();  // (no sound out of the machine, no audio thread beside callback() called by hand)
    AudioPtr buf = sine(2, 220);
    std::vector<Note> protoNotes;
    for (int n = 0; n < 8; n++) protoNotes.push_back({n * 0.5, n * 0.5 + 0.4, 60.0 + n, 100});
    ClipPtr proto = makeMidiClip("proto", protoNotes, 4, SR);  // (copied: each clip's own rendering would take long)
    std::vector<ClipPtr> clips;
    for (int i = 0; i < nTracks; i++) {
        TrackPtr t = r.w->project.addTrack(i % 2 ? Kind::Midi : Kind::Audio);  // (the timeline's: the panel rebuilt each time)
        t->name = i < (int)names.size() ? names[(size_t)i] : (i % 2 ? "Synth " : "Audio ") + std::to_string(i + 1);
        r.tracks.push_back(t);
        for (int k = 0; k < clipsPerTrack; k++) {
            ClipPtr c;
            std::string cname = t->name + " " + std::to_string(k + 1);
            if (t->kind == Kind::Midi) {
                c = std::make_shared<Clip>(*proto);
                c->name = cname;
            } else {
                c = makeAudioClip(cname, buf, SR, SR);
            }
            c->track = t;
            c->start = (int64_t)((k * 2.5 + (i % 3) * 0.25) * SR);
            clips.push_back(c);
            if (!r.midi && t->kind == Kind::Midi) r.midi = c;
            if (!r.audio && t->kind == Kind::Audio) r.audio = c;
        }
    }
    r.w->project.clips = clips;
    r.w->timeline->lanesChanged();
    r.w->timeline->tracksChanged.emit();
    r.f = new Front(*r.w);
    app.nestedLoop = [](const std::function<bool()>& done) {
        if (inModal) inModal(done);
        for (int i = 0; i < 40 && !done(); i++) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
    };
    settle(3);
    return r;
}

void closeRig(Rig& r) {
    ui::App& app = ui::App::get();
    inModal = nullptr;
    r.w->closeFxWindows();
    for (ui::Window* win : std::vector<ui::Window*>(app.windows()))
        if (win != r.w && win->isWindowShown() && !win->isPopup()) win->hideWindow();
    app.closeAllPopups();
    delete r.f;
    delete r.w;
    r = Rig();
    settle(1);
    app.nestedLoop = nullptr;
}

// one frame as the terminal's loop makes it: drawn, then the bytes that change the terminal's picture
struct Frame {
    size_t bytes;
    double drawMs, outMs;
};
Frame frame(Front& f, Screen& s, Screen& shown, Vt* vt = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    f.draw(s);
    double d = ms(t0);
    auto t1 = std::chrono::steady_clock::now();
    std::string out = s.output(shown, true);
    double o = ms(t1);
    if (vt) vt->feed(out);
    return Frame{out.size(), d, o};
}

// ============================================================================================
// 1. bytes per frame: idle, playing, scrolling, resized
void huntBytes() {
    Rig r = makeRig(60, 12);
    Front& f = *r.f;
    for (auto [W, H] : std::vector<std::pair<int, int>>{{120, 40}, {300, 100}}) {
        Screen s(W, H), shown(1, 1);
        Vt vt(W, H);
        std::string at = std::to_string(W) + "x" + std::to_string(H);
        f.secondsPerColumn = 0.02;  // (zoomed in: the view a few seconds wide, so playing pages it)
        Frame first = frame(f, s, shown, &vt);
        int firstCups = vt.cups, firstSgrs = vt.sgrs;
        // idle: frames with nothing done (the loop draws 30 a second)
        size_t idle = 0;
        for (int i = 0; i < 10; i++) {
            settle(1);
            idle += frame(f, s, shown, &vt).bytes;
        }
        // playing: the playhead moves (a frame's worth of audio between frames)
        audio::Engine& e = r.w->engine;
        e.offline = true;
        e.play();
        std::vector<float> buf((size_t)(SR / 30) * 2);
        size_t play = 0, playMax = 0;
        int jumps = 0;
        size_t jumpBytes = 0;
        double viewBefore = f.viewStart;
        for (int i = 0; i < 120; i++) {  // (4 s)
            e.callback(nullptr, buf.data(), SR / 30);
            settle(1);
            Frame fr = frame(f, s, shown, &vt);
            play += fr.bytes;
            playMax = std::max(playMax, fr.bytes);
            if (f.viewStart != viewBefore) jumps++, jumpBytes = std::max(jumpBytes, fr.bytes);
            viewBefore = f.viewStart;
        }
        e.stop();
        settle(1);
        frame(f, s, shown, &vt);
        // scrolling: the lanes down a row at a time (Down), the timeline sideways a bar (PgDn)
        f.pane = Front::Pane::Timeline;
        f.selectLane(0);
        frame(f, s, shown, &vt);
        size_t down = 0, downMax = 0, side = 0;
        int rows = 0;
        for (int i = 0; i < 40; i++) {
            int top = f.firstRow;
            f.key(key(plat::Key_Down));
            settle(1);
            Frame fr = frame(f, s, shown, &vt);
            if (f.firstRow != top) {
                rows++;
                down += fr.bytes;
                downMax = std::max(downMax, fr.bytes);
            }
        }
        double vs = f.viewStart;
        int sideways = 0;
        for (int i = 0; i < 10; i++) {
            f.key(key(plat::Key_PageDown));
            settle(1);
            Frame fr = frame(f, s, shown, &vt);
            if (f.viewStart != vs) sideways++, side += fr.bytes;
            vs = f.viewStart;
        }
        // resized: as the loop does on SIGWINCH (shown.resize(1, 1): drawn afresh)
        shown.resize(1, 1);
        Vt vt2(W, H);
        f.draw(s);
        std::string re = s.output(shown, true);
        vt2.feed(re);
        size_t clearAt = re.find("\x1b[2J");
        int mm = 0, row = -1;
        mm = mismatches(s, vt, row, true);
        int cells = W * H;
        found("bytes " + at + ": first frame " + std::to_string(first.bytes) + " B (" + std::to_string(firstCups) +
              " cursor moves, " + std::to_string(firstSgrs) + " SGR sequences for " + std::to_string(cells) +
              " cells); idle " + std::to_string(idle / 10) + " B/frame; playing " + std::to_string(play / 120) +
              " B/frame avg, " + std::to_string(playMax) + " B max (" + std::to_string(jumps) +
               " view jumps in 4 s, the biggest " + std::to_string(jumpBytes) + " B); a lane scrolled " + std::to_string(rows ? down / rows : 0) +
              " B avg, " + std::to_string(downMax) + " B max; a bar sideways " + std::to_string(sideways ? side / sideways : 0) +
              " B; after a resize " + std::to_string(re.size()) + " B, starting with ESC[2J (the screen blanked first: " +
              (clearAt != std::string::npos ? "yes" : "no") + "); the emulated terminal after it all differs from the picture in " +
              std::to_string(mm) + " cells (styles included)");
        // synchronized output (DEC mode 2026): none, so a big frame shows half-drawn
        if (re.find("\x1b[?2026h") == std::string::npos && re.size() > 4096)
            found("no synchronized update: a full frame at " + at + " is " + std::to_string(re.size()) +
                  " B written with no ESC[?2026h/l around it (a pty takes ~4 KB at a time: the terminal shows it half "
                  "drawn, the top new and the bottom old, after ESC[2J has blanked it)");
        // the bottom-right cell: never written, though autowrap is off (Terminal::open sends ESC[?7l)
        const Cell& br = *s.at(W - 1, H - 1);
        if (vt2.at(W - 1, H - 1).st != br.st || vt2.at(W - 1, H - 1).ch != br.ch)
            found("the bottom-right cell is never written (Screen::output skips it, though Terminal::open turns autowrap off): "
                  "the status bar meant '" + utf8Of(br.ch) + "' in " + styleText(br.st) + ", the terminal shows '" +
                  utf8Of(vt2.at(W - 1, H - 1).ch) + "' in " + styleText(vt2.at(W - 1, H - 1).st) +
                  " (a notch at the status bar's end)");
        // the overhead: a full SGR (ESC[0;…m, reset first) at every style change, a CUP per run
        size_t payload = (size_t)vt2.chars;
        found("bytes " + at + " full frame: " + std::to_string(re.size()) + " B for " + std::to_string(payload) +
              " characters drawn (" + std::to_string(vt2.cups) + " CUP, " + std::to_string(vt2.sgrs) +
              " SGR, every SGR restarting with 0: " + std::to_string((int)std::lround(100.0 * (double)(re.size() - payload) /
                                                                                       (double)re.size())) + " % escape bytes)");
        dump(s, "bytes-" + at);
    }
    closeRig(r);
}

// ============================================================================================
// 2. lag: a heavy project (200 tracks, 10,000 clips; a 100,000-note clip in the MIDI editor) at 300x100
void huntLag() {
    // what the time goes with: tracks, or clips
    for (auto [nt, nc] : std::vector<std::pair<int, int>>{{200, 0}, {4, 2500}, {200, 1}}) {
        Rig v = makeRig(nt, nc);
        Screen s(300, 100), shown(1, 1);
        frame(*v.f, s, shown);
        double sum = 0;
        for (int i = 0; i < 5; i++) sum += frame(*v.f, s, shown).drawMs;
        char b[200];
        snprintf(b, sizeof b, "lag 300x100 breakdown: %d tracks, %d clips: draw %.1f ms", nt, nt * nc, sum / 5);
        found(b);
        closeRig(v);
    }
    Rig r = makeRig(200, 50);
    Front& f = *r.f;
    Screen s(300, 100), shown(1, 1);
    // the big clip
    std::vector<Note> ns;
    ns.reserve(100000);
    for (int i = 0; i < 100000; i++) ns.push_back({i * 0.05, i * 0.05 + 0.04, 36.0 + (i * 7) % 60, 90});
    ClipPtr big = std::make_shared<Clip>(*r.midi);  // (its sound not rendered: 5,000 s of it)
    big->name = "Big";
    big->notes = std::make_shared<const std::vector<Note>>(ns);
    big->lengthS = 100000 * 0.05;
    big->track = r.tracks[1];
    big->start = 0;
    r.w->project.addClip(big);
    r.w->timeline->lanesChanged();
    settle(2);
    auto measure = [&](const std::string& what, int n, const std::function<void()>& between) {
        double worst = 0, sum = 0, tickWorst = 0, outSum = 0;
        size_t bytes = 0;
        for (int i = 0; i < n; i++) {
            auto t0 = std::chrono::steady_clock::now();
            between();
            settle(1);
            tickWorst = std::max(tickWorst, ms(t0));
            Frame fr = frame(f, s, shown);
            worst = std::max(worst, fr.drawMs + fr.outMs);
            sum += fr.drawMs;
            outSum += fr.outMs;
            bytes += fr.bytes;
        }
        char b[400];
        snprintf(b, sizeof b, "lag 300x100 %s: draw %.1f ms avg, output() %.1f ms avg, worst frame %.1f ms (the loop's budget: 33 ms); "
                 "the toolkit's tick + keys before it up to %.1f ms; %zu B/frame",
                 what.c_str(), sum / n, outSum / n, worst, tickWorst, bytes / (size_t)n);
        found(b);
        return worst;
    };
    frame(f, s, shown);
    measure("main window idle", 10, [] {});
    audio::Engine& e = r.w->engine;
    e.offline = true;
    e.play();
    std::vector<float> buf((size_t)(SR / 30) * 2);
    double cb = 0;
    measure("main window playing", 3, [&] {
        auto t0 = std::chrono::steady_clock::now();
        e.callback(nullptr, buf.data(), SR / 30);
        cb = std::max(cb, ms(t0));
    });
    found("lag: (the audio callback for a frame's 1/30 s with 10,000 clips, called by hand here, took up to " +
          std::to_string((int)cb) + " ms: not the TUI's; it is in the 'tick + keys' figure above)");
    e.stop();
    f.pane = Front::Pane::Timeline;
    f.selectLane(0);
    measure("lanes scrolled (Down)", 10, [&] { f.key(key(plat::Key_Down)); });
    {  // a new track (A in the track panel) with 200 there
        f.pane = Front::Pane::Panel;
        auto t0 = std::chrono::steady_clock::now();
        f.key(key('A', 0, "a"));
        double add = ms(t0);
        settle(1);
        double withTick = ms(t0);
        char b[300];
        snprintf(b, sizeof b, "lag: A (a new audio track) in the track panel with 200 tracks and 10,000 clips takes %.0f ms, %.0f ms with "
                 "the frame after it (Timeline::addTrack: relayout() and tracksChanged for each one)", add, withTick);
        found(b);
        f.pane = Front::Pane::Timeline;
    }
    // the parts of a frame: the overview (every clip for every column) and the timeline's lanes
    {
        auto t0 = std::chrono::steady_clock::now();
        double songS = (double)r.w->project.endFrame() / SR;
        int any = 0;
        for (int c = 0; c < 238; c++) {  // (the overview's loop as drawTimeline has it: each column, every clip)
            double a = songS * c / 238, b = songS * (c + 1) / 238;
            for (auto& cl : r.w->project.clips)
                if ((double)cl->start / SR < b && (double)cl->end() / SR > a) any++;
        }
        CHECK(any > 0);
        char b[300];
        snprintf(b, sizeof b, "lag: drawTimeline's overview alone (238 columns x %zu clips, src/tui/app.cpp drawTimeline: "
                 "`for (auto& cl : w_.project.clips)` inside `for (int c = 0; c < w; c++)`) takes %.1f ms a frame",
                 r.w->project.clips.size(), ms(t0));
        found(b);
    }
    {  // the menu bar's " *": MainWindow::hasUnsavedChanges(), asked by every frame
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; i++) r.w->hasUnsavedChanges();
        char b[400];
        snprintf(b, sizeof b, "lag: drawMenuBar asks MainWindow::hasUnsavedChanges() every frame (src/tui/app.cpp drawMenuBar, "
                 "`if (w_.hasUnsavedChanges()) name += \" *\"`), which describes the whole project as JSON and compares it "
                 "(main_window.cpp snapshot()): %.1f ms a call with 200 tracks and 10,000 clips", ms(t0) / 3);
        found(b);
        // the toolkit's frames (the loop runs one before each picture): their worst over 2 s idle
        double worst = 0;
        int over = 0;
        for (int i = 0; i < 60; i++) {
            auto t1 = std::chrono::steady_clock::now();
            settle(1);
            double d = ms(t1);
            worst = std::max(worst, d);
            over += d > 33;
        }
        snprintf(b, sizeof b, "lag: the toolkit's frame (App::tick, which tui::run's loop runs before each picture) over 2 s idle "
                 "with 200 tracks: worst %.0f ms, %d of 60 over 33 ms", worst, over);
        found(b);
    }
    // the MIDI editor with the big clip
    r.w->openMidiEditor(big);
    settle(3);
    frame(f, s, shown);
    dump(s, "lag-midi-editor-300x100");
    measure("MIDI editor, 100,000 notes, idle", 5, [] {});
    measure("MIDI editor, 100,000 notes, cursor moved (Right)", 5, [&] { f.key(key(plat::Key_Right)); });
    {
        auto* ed = dynamic_cast<MidiEditor*>(ui::App::get().windows().back());
        if (ed) {
            for (int i = 0; i < (int)ed->notes.size(); i++) ed->selected.insert(i);
            measure("MIDI editor, 100,000 notes all selected", 5, [] {});
        }
    }
    e.play();
    measure("MIDI editor, 100,000 notes, playing", 3, [&] { e.callback(nullptr, buf.data(), SR / 30); });
    e.stop();
    closeRig(r);
}

// ============================================================================================
// 3. Unicode names: wide (CJK, emoji), combining accents, right-to-left, zero-width joiners
void huntUnicode() {
    std::vector<std::string> names = {"ドラムとベース", "🥁 Drums", "Café au lait", "שלום עולם", "👩‍🎤 Singer",
                                      "plain ASCII", "éééé accents", "中文音轨名称很长很长很长"};
    Rig r = makeRig((int)names.size(), 2, names);
    Front& f = *r.f;
    for (auto [W, H] : std::vector<std::pair<int, int>>{{80, 24}, {120, 40}}) {
        std::string at = std::to_string(W) + "x" + std::to_string(H);
        Screen s(W, H), shown(1, 1);
        Vt vt(W, H);
        frame(f, s, shown, &vt);
        int row = -1, n = mismatches(s, vt, row);
        std::string file = dump(s, "unicode-" + at + "-meant");
        std::ofstream(OUT + "/unicode-" + at + "-shown.txt") << [&] {
            std::string o;
            for (int y = 0; y < H; y++) o += vt.line(y) + "\n";
            return o;
        }();
        // the panel's left border: where it is meant, and where the terminal shows it on each row with a name
        int border = -1;
        for (int x = W - 1; x > 0 && border < 0; x--)
            if (s.at(x, 3)->ch == U'│') border = x;
        int moved = 0;
        std::string example;
        for (int y = 4; y < H - 2; y++) {
            if (border < 0 || s.at(border, y)->ch != U'│') continue;
            if (vt.at(border, y).ch != U'│') {
                moved++;
                if (example.empty()) example = "row " + std::to_string(y) + ": meant \"" + s.line(y) + "\", shown \"" + vt.line(y) + "\"";
            }
        }
        found("unicode " + at + " (track and clip names: CJK, emoji, combining accents, Hebrew, a ZWJ sequence): " +
              std::to_string(n) + " cells differ between the picture and the emulated terminal (first at row " +
              std::to_string(row) + "); the timeline/panel border is out of place on " + std::to_string(moved) + " rows. " + example +
              ". Pictures: " + file + " and " + OUT + "/unicode-" + at + "-shown.txt");
        // an update after it: one name changed (only the changed cells sent, at the columns the picture thinks)
        r.tracks[0]->name = "ドラム";
        settle(1);
        frame(f, s, shown, &vt);
        int row2 = -1, n2 = mismatches(s, vt, row2);
        r.tracks[0]->name = names[0];
        found("unicode " + at + " incremental: after one track renamed, " + std::to_string(n2) +
              " cells differ (debris stays until a full redraw: only a resize redraws all)");
    }
    // fit() cuts between a letter and its accent, or inside a ZWJ sequence
    std::string cut = fit("Café", 4, false), cut2 = fit("👩‍🎤 Singer", 2, false);
    found("fit() counts code points, not cells: fit(\"Café\" with a combining accent, 4) = \"" + cut + "\" (accent cut off), "
          "fit(\"👩‍🎤 Singer\", 2) = \"" + cut2 + "\" (a dangling ZWJ), columns(\"ドラム\") = " + std::to_string(columns("ドラム")) +
          " (the terminal: 6)");
    closeRig(r);
}

// ============================================================================================
// 4. highlights after reverse video, and what marks the control with the keyboard
struct WinCheck {
    std::string name;
    int stops = 0, invisible = 0, monoInvisible = 0, boldOnly = 0, backward = 0, still = 0;
    std::vector<std::string> invisibleKinds, monoKinds, boldKinds;
    std::string order;
};
std::vector<WinCheck> winChecks;

std::string kindOf(ui::Widget* w) {
    if (!w) return "none";
    if (dynamic_cast<ui::LineEdit*>(w)) return "LineEdit";
    if (dynamic_cast<ui::SpinBox*>(w)) return "SpinBox";
    if (dynamic_cast<ui::ComboBox*>(w)) return "ComboBox";
    if (dynamic_cast<ui::CheckBox*>(w)) return "CheckBox";
    if (dynamic_cast<ui::Button*>(w)) return "Button" + (dynamic_cast<ui::Button*>(w)->text().empty() ? std::string("(icon)") : "");
    if (dynamic_cast<ui::Slider*>(w)) return "Slider";
    if (dynamic_cast<ui::ListWidget*>(w)) return "ListWidget";
    if (dynamic_cast<ui::TreeWidget*>(w)) return "TreeWidget";
    return w->type();
}

// Tab through the window on top: is each control with the keyboard marked (with colour; without), in what order
void focusWalk(Front& f, Screen& s, const std::string& name) {
    ui::Window* win = nullptr;
    for (auto it = ui::App::get().windows().rbegin(); it != ui::App::get().windows().rend(); ++it)
        if ((*it)->isWindowShown() && !ui::Menu::ofWindow(*it) && (*it)->type() != "MainWindow") {
            win = *it;
            break;
        }
    if (!win) return;
    WinCheck wc;
    wc.name = name + " (" + win->title() + ")";
    std::vector<ui::Widget*> seen;
    Point last(-1, -1);
    for (int i = 0; i < 30; i++) {
        ui::Widget* fw = win->focusWidget();
        if (fw && std::find(seen.begin(), seen.end(), fw) != seen.end()) break;
        if (fw) {
            seen.push_back(fw);
            wc.stops++;
            f.draw(s);
            auto withL = looks(s);
            auto withM = mono(s);
            fw->clearFocus();
            f.draw(s);
            auto withoutL = looks(s);
            bool same = withoutL == withL, sameM = mono(s) == withM;
            bool bold = false;  // (only bold differs)
            if (!same) {
                bold = true;
                for (size_t k = 0; k < withL.size() && bold; k++) {
                    auto a = withL[k], b = withoutL[k];
                    std::get<3>(a) &= ~Bold;
                    std::get<3>(b) &= ~Bold;
                    bold = a == b;
                }
            }
            fw->setFocus();
            if (bold) {
                wc.boldOnly++;
                wc.boldKinds.push_back(kindOf(fw));
            }
            if (same) {
                wc.invisible++;
                wc.invisibleKinds.push_back(kindOf(fw));
            } else if (sameM) {
                wc.monoInvisible++;
                wc.monoKinds.push_back(kindOf(fw));
            }
            Point p = fw->mapTo(win, Point(0, 0));
            if (last.x >= 0 && (p.y < last.y - 8 || (std::abs(p.y - last.y) <= 8 && p.x < last.x - 8))) wc.backward++;
            if (wc.order.size() < 400) wc.order += (wc.order.empty() ? "" : " → ") + kindOf(fw) + "@" + std::to_string(p.x) + "," + std::to_string(p.y);
            last = p;
        }
        f.draw(s);
        auto beforeTab = looks(s);
        f.key(key(plat::Key_Tab));
        settle(1);
        if (!ui::Widget::alive(win) || !win->isWindowShown()) break;
        f.draw(s);
        if (win->focusWidget() != fw && looks(s) == beforeTab) wc.still++;  // (Tab moved the keyboard, nothing on screen changed)
    }
    winChecks.push_back(wc);
}

void inspect(Front& f, Screen& s, const std::string& name) {
    settle(2);
    f.draw(s);
    std::string at = std::to_string(s.width()) + "x" + std::to_string(s.height());
    dump(s, "window-" + name + "-" + at);
    scanColours(s, name + " " + at);
    Screen shown(1, 1);
    Vt vt(s.width(), s.height());
    vt.feed(s.output(shown, true));
    int row = -1, n = mismatches(s, vt, row);
    if (n) found("window " + name + " " + at + ": " + std::to_string(n) + " cells shown elsewhere by a terminal (wide characters), first at row " +
                 std::to_string(row) + ": \"" + s.line(row) + "\"");
    if (s.width() == 120) focusWalk(f, s, name + " " + at);
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

void huntWindows() {
    Rig r = makeRig(6, 3);
    Front& f = *r.f;
    MainWindow& w = *r.w;
    // a sampler with a sample, a Pad Kit, a synth: on MIDI tracks
    TrackPtr synthT = r.tracks[1], samplerT = r.tracks[3], kitT = r.tracks[5];
    samplerT->sampler = sampler_dialog::newSettings(r.audio);
    kitT->padKit = std::make_shared<PadKit>();
    for (auto [W, H] : std::vector<std::pair<int, int>>{{80, 24}, {120, 40}}) {
        Screen s(W, H);
        std::string at = std::to_string(W) + "x" + std::to_string(H);
        auto closeAll = [&] {
            for (int i = 0; i < 5; i++) {
                f.key(key('W', plat::ModCtrl));
                settle(1);
            }
            for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
                if (win != &w && win->isWindowShown() && !win->isPopup()) win->hideWindow();
            ui::App::get().closeAllPopups();
            settle(1);
        };
        // windows of their own
        std::vector<std::pair<std::string, std::function<void()>>> own = {
            {"midi-editor", [&] { w.openMidiEditor(r.midi); }},
            {"effects-patch", [&] { w.openTrackPlugins(r.tracks[0]); }},
            {"synth", [&] { synth_dialog::openSynth(synthT); }},
            {"sampler", [&] { sampler_dialog::openSampler(samplerT); }},
            {"pad-kit", [&] { pad_kit_dialog::openPadKit(kitT); }},
        };
        for (auto& [name, open] : own) {
            open();
            settle(3);
            inspect(f, s, name);
            closeAll();
        }
        // the Steps view: the editor, its toolbar's first button
        w.openMidiEditor(r.midi);
        settle(2);
        f.key(key(plat::Key_Tab));
        f.key(key(plat::Key_Space, 0, " "));
        settle(2);
        inspect(f, s, "midi-steps");
        closeAll();
        // an effect's own window (its knobs): the patch panel, Insert a Bit-crusher, Enter
        w.openTrackPlugins(r.tracks[0]);
        settle(2);
        if (r.tracks[0]->effects.empty()) {
            f.key(key(plat::Key_Insert));
            settle(1);
            f.key(key(plat::Key_Return));
            settle(2);
        }
        f.key(key(plat::Key_Return));
        settle(2);
        inspect(f, s, "effect-knobs");
        closeAll();
        // modal dialogs, and windows, from the menus: inspected inside their loop
        std::vector<std::pair<std::string, std::function<ui::Action*()>>> fromMenus;
        auto bar = [&](const std::string& entry) {
            return [&w, entry]() -> ui::Action* {
                for (auto& m : w.menuBar->menus())
                    if (ui::Action* a = findAction(*m, entry)) return a;
                return nullptr;
            };
        };
        ui::Menu trackMenu, clipMenu, kitMenu;
        auto inMenu = [&](ui::Menu& m, const std::string& entry) { return [&m, entry]() { return findAction(m, entry); }; };
        w.panel->fillTrackMenu(trackMenu, r.tracks[0]);
        w.panel->fillTrackMenu(kitMenu, kitT);
        w.timeline->selection.clear();
        w.timeline->selection.selectOnly(r.audio);
        w.timeline->fillMenu(clipMenu, std::nullopt);
        for (const char* e : {"MIDI ports", "Keyboard shortcuts", "Command palette", "Find missing samples", "History", "Audio properties"})
            fromMenus.push_back({e, bar(e)});
        for (const char* e : {"Slice", "Warp"}) fromMenus.push_back({e, inMenu(clipMenu, e)});
        for (const char* e : {"Track delay", "Rename"}) fromMenus.push_back({e, inMenu(trackMenu, e)});
        for (auto& [name, get] : fromMenus) {
            ui::Action* a = get();
            if (!a) {
                found("(no menu entry \"" + name + "\": not inspected)");
                continue;
            }
            bool modalSeen = false;
            inModal = [&, name = name](const std::function<bool()>& done) {
                if (modalSeen) return;
                modalSeen = true;
                inspect(f, s, fileSafe(name));
                for (int i = 0; i < 10 && !done(); i++) {
                    f.key(key(plat::Key_Escape));
                    settle(1);
                }
            };
            ui::Menu::activate(a);
            settle(2);
            inModal = nullptr;
            if (!modalSeen) {
                inspect(f, s, fileSafe(name));
                closeAll();
            }
        }
        // the External synth's dialog (the instrument menu's "External synth…")
        {
            bool seen = false;
            inModal = [&](const std::function<bool()>& done) {
                if (seen) return;
                seen = true;
                inspect(f, s, "external-synth");
                for (int i = 0; i < 10 && !done(); i++) {
                    f.key(key(plat::Key_Escape));
                    settle(1);
                }
            };
            midi_ports_dialog::editExternal(&w, synthT);
            settle(1);
            inModal = nullptr;
        }
        // the tempo dialog: the ruler, Enter
        {
            closeAll();
            f.pane = Front::Pane::Timeline;
            f.selectLane(0);
            bool seen = false;
            inModal = [&](const std::function<bool()>& done) {
                if (seen) return;
                seen = true;
                inspect(f, s, "tempo");
                for (int i = 0; i < 10 && !done(); i++) {
                    f.key(key(plat::Key_Escape));
                    settle(1);
                }
            };
            f.key(key(plat::Key_Up));
            f.key(key(plat::Key_Return));
            settle(2);
            inModal = nullptr;
            f.selectLane(0);
        }
        // the menus and the F1 list
        for (int m = 0; m < 4; m++) {
            f.openBarMenu(m);
            f.draw(s);
            scanColours(s, "menu " + std::to_string(m) + " " + at);
            f.closeMenus();
        }
        f.key(key(plat::Key_F1));
        f.draw(s);
        scanColours(s, "F1 " + at);
        f.key(key(plat::Key_Escape));
        closeAll();
    }
    closeRig(r);
}

// the highlights themselves: menu entry, the transport's control, a list's row, the text cursor, selected notes
void huntHighlights() {
    Rig r = makeRig(4, 2);
    Front& f = *r.f;
    MainWindow& w = *r.w;
    Screen s(120, 40);
    // a bar menu: the highlighted entry against the others
    f.openBarMenu(0);
    f.key(key(plat::Key_Down));
    f.draw(s);
    dump(s, "highlight-file-menu");
    {
        int hy = -1, hx = -1;
        for (int y = 2; y < 40 && hy < 0; y++)
            for (int x = 1; x < 60; x++)
                if (s.at(x, y)->st == style(White, Blue, Bold | Reverse)) { hy = y; hx = x; break; }
        if (hy >= 0) {
            Look hi = lookOf(s.at(hx + 2, hy)->st), lo = lookOf(s.at(hx + 2, hy + 2)->st);
            found("menu highlight: the entry chosen is drawn " + styleText(s.at(hx + 2, hy)->st) + " (kMenuHi, src/tui/app.cpp:60) "
                  "beside " + styleText(s.at(hx + 2, hy + 2)->st) + " entries: after reverse video it shows " + lookText(hi) +
                  " against " + lookText(lo) + (hi.bg == lo.bg ? ": the SAME ground, no bar — only the text's colour (blue) and bold mark it" : ""));
        }
    }
    f.closeMenus();
    // the transport's control with the keyboard
    f.pane = Front::Pane::Transport;
    f.control = 3;
    f.draw(s);
    {
        int x = (int)s.line(1).find("Loop");
        (void)x;
        Look hi{}, lo{};
        for (int c = 0; c < 120; c++)
            if (s.at(c, 1)->st.attr & Reverse) { hi = lookOf(s.at(c, 1)->st); break; }
        lo = lookOf(s.at(0, 1)->st);
        found("transport: the control with the keyboard shows " + lookText(hi) + " on a bar of " + lookText(lo) +
              (hi.bg == lo.bg ? " (the same ground: blue text is all that marks it; nothing marks it without colour but bold)" : ""));
    }
    // the panes' titles: with the keyboard (kFocusTitle) and without (kTitle), with no colours
    {
        f.pane = Front::Pane::Timeline;
        f.draw(s);
        auto a = mono(s);
        f.pane = Front::Pane::Panel;
        f.draw(s);
        auto b = mono(s);
        int diff = 0;
        for (size_t i = 0; i < a.size(); i++) diff += a[i] != b[i];
        found("panes without colour: moving the keyboard from the timeline to the track panel changes " + std::to_string(diff) +
              " cells' characters or attributes (kTitle and kFocusTitle are both bold: only cyan marks the pane with the "
              "keyboard, against screen.h's \"never relies on colour alone\")" + (diff ? " — the selected row's reverse differs" : ""));
    }
    // a track and a clip in dark colours (their nearest of the 8: black), a disabled clip: on a dark terminal
    {
        r.tracks[0]->color = "#1a1a1a";
        r.audio->color = "#202020";
        r.audio->disabled = true;
        f.pane = Front::Pane::Timeline;
        f.draw(s);
        scanColours(s, "main window, a dark track colour and a disabled dark clip");
        dump(s, "highlight-dark-colours");
        r.audio->disabled = false;
        r.tracks[0]->color = "";
        r.audio->color = "";
    }
    // the track panel's selected row: with the keyboard (Black on Cyan, bold) and without (reverse), no colour
    {
        f.setCurrent(r.tracks[1]);
        f.pane = Front::Pane::Panel;
        f.draw(s);
        std::string a, b;
        for (int y = 3; y < 40 && a.empty(); y++)
            for (int x = 60; x + 1 < 120; x++)
                if (s.at(x, y)->ch == U'S' && s.at(x + 1, y)->ch == U'y' && s.at(x - 1, y)->ch == U'▌') {
                    a = styleText(s.at(x, y)->st);
                    f.pane = Front::Pane::Timeline;
                    f.draw(s);
                    b = styleText(s.at(x, y)->st);
                    break;
                }
        found("the track panel's selected row: with the keyboard " + a + ", without it (the timeline has it) " + b +
              ": without colour the row with the keyboard is only bold, the one without it reversed (the emphasis inverted)");
    }
    // focus after a window closes: the timeline's keys work again
    {
        f.pane = Front::Pane::Timeline;
        f.selectLane(0);
        w.timeline->setCursor(0);
        w.openMidiEditor(r.midi);
        settle(2);
        f.key(key('W', plat::ModCtrl));
        settle(2);
        int64_t before = w.timeline->cursor;
        f.key(key(plat::Key_Right));
        settle(1);
        if (w.timeline->cursor == before) found("after Ctrl+W closed the MIDI editor, Right in the timeline did nothing (the keyboard lost)");
    }
    // a list's selected row with the keyboard (the palette's), the text cursor (Rename's field)
    {
        bool seen = false;
        inModal = [&](const std::function<bool()>& done) {
            if (seen) return;
            seen = true;
            settle(2);
            f.draw(s);
            dump(s, "highlight-palette");
            ui::Window* win = ui::App::get().windows().back();
            for (int i = 0; i < 6; i++) {
                ui::Widget* fw = win->focusWidget();
                if (dynamic_cast<ui::ListWidget*>(fw)) break;
                f.key(key(plat::Key_Tab));
                settle(1);
            }
            f.draw(s);
            for (int y = 0; y < 40; y++) {
                std::string l = s.line(y);
                size_t at = l.find("> ");
                if (at == std::string::npos || y < 3) continue;
                int x = columns(l.substr(0, at)) + 3;
                Look sel = lookOf(s.at(x, y)->st), frame = lookOf(s.at(x, y + 1)->st);
                found("a list's selected row with the keyboard (src/tui/window_view.cpp:289/301/319, style(Blue, White, Reverse | Bold)) "
                      "shows " + lookText(sel) + " beside unselected rows' " + lookText(frame) +
                      (sel.bg == frame.bg ? ": the same ground, no bar (reverse undoes the swap), only bold and \"> \" mark it" : ""));
                break;
            }
            for (int i = 0; i < 10 && !done(); i++) {
                f.key(key(plat::Key_Escape));
                settle(1);
            }
        };
        w.showPalette();
        settle(2);
        inModal = nullptr;
        // Rename: the track panel's Enter
        seen = false;
        inModal = [&](const std::function<bool()>& done) {
            if (seen) return;
            seen = true;
            settle(2);
            f.key(key(plat::Key_End));
            f.draw(s);
            dump(s, "highlight-rename");
            for (int y = 0; y < 40; y++)
                for (int x = 0; x + 1 < 120; x++) {
                    const Cell& c = *s.at(x, y);
                    if (c.st == style(White, Black, Reverse | Bold)) {
                        Look cur = lookOf(c.st), field = lookOf(s.at(x - 1, y)->st);
                        found("the text cursor in a text field (window_view.cpp:256, style(White, Black, Reverse | Bold) in a "
                              "field of style(Black, White)) shows " + lookText(cur) + " beside the field's " + lookText(field) +
                              (cur.bg == field.bg && cur.fg == field.fg ? ": INVISIBLE (the terminal's own cursor is hidden: ESC[?25l)" : "") +
                              " — row " + std::to_string(y) + ": \"" + s.line(y) + "\"");
                        y = 40;
                        break;
                    }
                }
            for (int i = 0; i < 10 && !done(); i++) {
                f.key(key(plat::Key_Escape));
                settle(1);
            }
        };
        f.pane = Front::Pane::Panel;
        f.setCurrent(r.tracks[0]);
        f.key(key(plat::Key_Return));
        settle(2);
        inModal = nullptr;
    }
    // selected notes in the piano roll: a full block in reverse video is the ground's colour
    {
        w.openMidiEditor(r.midi);
        settle(3);
        auto* ed = dynamic_cast<MidiEditor*>(ui::App::get().windows().back());
        f.draw(s);
        int before = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 120; x++)
                if (solidOf(*s.at(x, y)) == Green) before++;
        if (ed) {
            for (int i = 0; i < (int)ed->notes.size(); i++) ed->selected.insert(i);
            f.draw(s);
            dump(s, "highlight-notes-selected");
            int after = 0, black = 0;
            for (int y = 0; y < 40; y++)
                for (int x = 0; x < 120; x++) {
                    const Cell& c = *s.at(x, y);
                    if (solidOf(c) == Green) after++;
                    if (c.ch == U'█' && (c.st.attr & Reverse) && solidOf(c) == Black) black++;
                }
            found("selected notes in the piano roll (src/ui/midi_editor.cpp:1576, '█' with Bold|Reverse): green note cells " +
                  std::to_string(before) + " before selecting all, " + std::to_string(after) + " after; " + std::to_string(black) +
                  " note cells now show as the ground's black — selected notes look like holes (empty)");
            // the keyboard's cursor on a note: the same (fg 3 with reverse on '█')
        }
        for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
            if (win != &w && win->isWindowShown()) win->hideWindow();
        settle(1);
    }
    closeRig(r);
}

// ============================================================================================
// 5. windows and menus while the screen is resized; a dialog that opens over the F1 list or a menu
void huntResize() {
    Rig r = makeRig(8, 3);
    Front& f = *r.f;
    MainWindow& w = *r.w;
    Screen s(120, 40);
    // a long menu, its last entry highlighted, shrunk to 60x14
    f.openBarMenu(0);
    for (int i = 0; i < 40; i++) f.key(key(plat::Key_Down));
    f.draw(s);
    int item = f.open.empty() ? -1 : f.open.back().item;
    std::string text = item >= 0 && item < (int)f.open.back().menu->actions().size() ? menuText(f.open.back().menu->actions()[(size_t)item]->text) : "Quit";
    for (auto [W, H] : std::vector<std::pair<int, int>>{{60, 14}, {300, 100}, {60, 14}}) {
        Screen t(W, H);
        f.draw(t);
        bool shown = t.lines().find(text) != std::string::npos;
        if (!shown)
            found("resize with a menu open: the highlighted entry \"" + text + "\" isn't on the " + std::to_string(W) + "x" + std::to_string(H) +
                  " screen (" + dump(t, "resize-menu-" + std::to_string(W) + "x" + std::to_string(H)) + ")");
    }
    f.closeMenus();
    // the F1 list, then a window opens (as the recovery offer and the plugin scan's do, from timers)
    f.key(key(plat::Key_F1));
    w.openMidiEditor(r.midi);
    settle(2);
    f.draw(s);
    bool helpOver = f.helpShown && s.lines().find("Keys") != std::string::npos;
    std::string file = dump(s, "help-over-window");
    f.key(key(plat::Key_Right));  // (to the editor, under the list)
    f.draw(s);
    if (helpOver && f.helpShown)
        found("F1's list stays drawn over a window that opened meanwhile, and keys go to the window under it "
              "(src/tui/app.cpp Front::key: topWindow() is asked before helpShown; draw() draws the help last): "
              "Right moved the hidden editor's cursor, the list still shown — " + file);
    for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
        if (win != &w && win->isWindowShown()) win->hideWindow();
    f.helpShown = false;
    settle(1);
    // a bar menu open, then a window opens: the menu stays drawn, keys go to the window
    f.openBarMenu(1);
    w.openMidiEditor(r.midi);
    settle(2);
    f.draw(s);
    bool menuUnder = f.menuOpen();
    f.key(key(plat::Key_Escape));
    settle(1);
    if (menuUnder && f.menuOpen())
        found("a bar menu open when a window opens stays open under it, deaf: Esc went to the window, the menu still open "
              "(Front::key: topWindow() first) — " + dump(s, "menu-under-window"));
    f.closeMenus();
    for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
        if (win != &w && win->isWindowShown()) win->hideWindow();
    settle(1);
    // a window open while resizing: from 120x40 to 60x14 and back
    w.openMidiEditor(r.midi);
    settle(2);
    for (auto [W, H] : std::vector<std::pair<int, int>>{{60, 14}, {61, 15}, {300, 100}}) {
        Screen t(W, H);
        auto t0 = std::chrono::steady_clock::now();
        f.draw(t);
        double d = ms(t0);
        if (d > 33) found("resize with the MIDI editor open: a frame at " + std::to_string(W) + "x" + std::to_string(H) + " takes " + std::to_string((int)d) + " ms");
        dump(t, "resize-editor-" + std::to_string(W) + "x" + std::to_string(H));
    }
    for (ui::Window* win : std::vector<ui::Window*>(ui::App::get().windows()))
        if (win != &w && win->isWindowShown()) win->hideWindow();
    closeRig(r);
}

// ============================================================================================
// 6. the mouse, paste and focus reports: what the key decoder makes of them (the TUI never turns them on, but a
// program before it may have left them on, and some terminals send them anyway)
void huntMouse() {
    auto decode = [](const std::string& bytes) {
        KeyDecoder d;
        d.feed(bytes, 0);
        std::vector<Key> keys = d.take(0);
        for (const Key& k : d.take(1.0)) keys.push_back(k);
        return keys;
    };
    auto text = [](const std::vector<Key>& ks) {
        std::string o;
        for (const Key& k : ks) {
            std::string n = k.key == plat::Key_Enter ? "Enter" : k.key == plat::Key_Return ? "Return" : k.key == plat::Key_Space ? "Space"
                          : k.key < 128 && k.key > 32 ? std::string(1, (char)k.key) : "key" + std::to_string(k.key);
            if (k.mods & plat::ModShift) n = "Shift+" + n;
            if (k.mods & plat::ModAlt) n = "Alt+" + n;
            if (k.mods & plat::ModMeta) n = "Meta+" + n;
            if (k.mods & plat::ModCtrl) n = "Ctrl+" + n;
            o += (o.empty() ? "" : ", ") + n;
        }
        return o.empty() ? std::string("nothing") : o;
    };
    struct Case { const char* what; std::string bytes; };
    for (const Case& c : std::vector<Case>{{"an SGR mouse press (ESC[<0;10;5M)", "\x1b[<0;10;5M"},
                                           {"its release (ESC[<0;10;5m)", "\x1b[<0;10;5m"},
                                           {"an X10 mouse press (ESC[M then 3 bytes)", std::string("\x1b[M") + " *%"},
                                           {"a wheel turn, SGR (ESC[<64;10;5M)", "\x1b[<64;10;5M"},
                                           {"a bracketed paste of \"lol\" (ESC[200~lol ESC[201~)", "\x1b[200~lol\x1b[201~"},
                                           {"focus in / out (ESC[I ESC[O)", "\x1b[I\x1b[O"}}) {
        std::vector<Key> ks = decode(c.bytes);
        found(std::string("mouse/paste: ") + c.what + " decodes as: " + text(ks));
    }
}

}  // namespace

int main() {
    setlocale(LC_CTYPE, "C.UTF-8");  // (wcwidth for the emulated terminal)
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(OUT + "/tmp");
    for (auto& e : std::filesystem::directory_iterator(OUT))
        if (e.is_regular_file() && e.path().extension() == ".txt") std::filesystem::remove(e.path());
    paths::setDataDir(util::absolutePath(OUT + "/tmp/data"));
    settings::setPath(OUT + "/tmp/settings.json");
    const char* only = getenv("TUI_HUNT_ONLY");  // (one part alone: bytes, unicode, highlights, windows, resize, mouse, lag)
    auto timed = [only](const char* name, void (*fn)()) {
        if (only && std::string(only) != name) return;
        double t0 = elapsed();
        fn();
        printf("tui hunt: %s done in %.1f s\n", name, elapsed() - t0);
        fflush(stdout);
    };
    timed("bytes", huntBytes);
    timed("unicode", huntUnicode);
    timed("highlights", huntHighlights);
    timed("windows", huntWindows);
    timed("resize", huntResize);
    timed("mouse", huntMouse);
    timed("lag", huntLag);
    for (const WinCheck& wc : winChecks) {
        std::string inv, mon, bold;
        for (auto& k : wc.invisibleKinds) inv += (inv.empty() ? "" : ", ") + k;
        for (auto& k : wc.monoKinds) mon += (mon.empty() ? "" : ", ") + k;
        for (auto& k : wc.boldKinds) bold += (bold.empty() ? "" : ", ") + k;
        found("focus " + wc.name + ": " + std::to_string(wc.stops) + " Tab stops; unmarked: " + std::to_string(wc.invisible) +
              (inv.empty() ? "" : " [" + inv + "]") + "; marked by colour alone: " + std::to_string(wc.monoInvisible) +
              (mon.empty() ? "" : " [" + mon + "]") + "; marked by bold alone: " + std::to_string(wc.boldOnly) +
              (bold.empty() ? "" : " [" + bold + "]") + "; Tabs that changed nothing on screen: " + std::to_string(wc.still) +
              "; out of visual order: " + std::to_string(wc.backward) + " steps. Order: " + wc.order);
    }
    for (auto& [k, where] : unreadable) found("colours: " + k + " — first at " + where);
    std::ofstream o(OUT + "/findings.txt");
    for (auto& s : findings) o << s << "\n";
    printf("tui hunt: %zu findings in %.1f s: %s/findings.txt\n", findings.size(), elapsed(), OUT.c_str());
    CHECK(true);
    TEST_MAIN_END();
}
