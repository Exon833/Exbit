// tinydaw --tui (tui/): the terminal's keys decoded from what terminals send (the Linux console's and
// xterm's sequences), the picture's output (only changed cells; ASCII without UTF-8), and the front end
// over the headless main window: its layout, the keys of each part, the menus, the program's dialogs as
// text worked by keys (a nested loop fed from a list, as the terminal's loop is), rendering, effects
// (added, wired, bypassed, their windows; a plugin's parameters as a list), the Pad Kit, the MIDI editor
// and its Steps view by the keyboard's cursor, automation points, sends, the ruler's tempo dialog, and that
// it never loads X11. Pictures of the screen go to build/ui-samples/tui/.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/sound_file.h"
#include "check.h"
#include "core/discord_presence.h"
#include "core/util.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "tui/text_symbols.h"
#include "config.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/param_window.h"
#include "ui/timeline.h"
#include "ui/toolkit/widget.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/tui";
const std::string TMP = OUT + "/tmp";

std::vector<Key> decode(const std::string& bytes, double later = 1.0) {
    KeyDecoder d;
    d.feed(bytes, 0);
    std::vector<Key> keys = d.take(0);
    for (const Key& k : d.take(later)) keys.push_back(k);  // (what waited: a lone ESC)
    return keys;
}

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

void testKeys() {
    // characters: letters as uppercase keys with what they type
    CHECK((decode("a") == std::vector<Key>{key('A', 0, "a")}));
    CHECK((decode("A") == std::vector<Key>{key('A', plat::ModShift, "A")}));
    CHECK((decode(" ") == std::vector<Key>{key(plat::Key_Space, 0, " ")}));
    CHECK((decode(",") == std::vector<Key>{key(',', 0, ",")}));
    CHECK((decode("\xc3\xa9") == std::vector<Key>{key(0xE9, 0, "\xc3\xa9")}));  // é
    // control characters
    CHECK((decode("\x01") == std::vector<Key>{key('A', plat::ModCtrl)}));
    CHECK((decode("\x13") == std::vector<Key>{key('S', plat::ModCtrl)}));  // (Ctrl+S: a key, not XOFF)
    CHECK((decode("\r") == std::vector<Key>{key(plat::Key_Return)}));
    CHECK((decode("\t") == std::vector<Key>{key(plat::Key_Tab)}));
    CHECK((decode("\x7f") == std::vector<Key>{key(plat::Key_Backspace)}));
    // arrows, both forms; xterm's modifiers
    CHECK((decode("\x1b[A") == std::vector<Key>{key(plat::Key_Up)}));
    CHECK((decode("\x1bOD") == std::vector<Key>{key(plat::Key_Left)}));
    CHECK((decode("\x1b[1;5C") == std::vector<Key>{key(plat::Key_Right, plat::ModCtrl)}));
    CHECK((decode("\x1b[1;2B") == std::vector<Key>{key(plat::Key_Down, plat::ModShift)}));
    CHECK((decode("\x1b[Z") == std::vector<Key>{key(plat::Key_Backtab, plat::ModShift)}));
    // the editing keys and function keys: xterm's and the Linux console's (F1…F5: ESC [ [ A…E)
    CHECK((decode("\x1b[3~") == std::vector<Key>{key(plat::Key_Delete)}));
    CHECK((decode("\x1b[2~") == std::vector<Key>{key(plat::Key_Insert)}));
    CHECK((decode("\x1b[1~") == std::vector<Key>{key(plat::Key_Home)}));
    CHECK((decode("\x1b[4~") == std::vector<Key>{key(plat::Key_End)}));
    CHECK((decode("\x1b[H") == std::vector<Key>{key(plat::Key_Home)}));
    CHECK((decode("\x1b[5~\x1b[6~") == std::vector<Key>{key(plat::Key_PageUp), key(plat::Key_PageDown)}));
    CHECK((decode("\x1b[[A") == std::vector<Key>{key(plat::Key_F1)}));
    CHECK((decode("\x1b[[E") == std::vector<Key>{key(plat::Key_F5)}));
    CHECK((decode("\x1bOP") == std::vector<Key>{key(plat::Key_F1)}));
    CHECK((decode("\x1b[11~") == std::vector<Key>{key(plat::Key_F1)}));
    CHECK((decode("\x1b[21~") == std::vector<Key>{key(plat::Key_F10)}));
    CHECK((decode("\x1b[24~") == std::vector<Key>{key(plat::Key_F12)}));
    // Alt+key: ESC then the key
    CHECK((decode("\x1b" "f") == std::vector<Key>{key('F', plat::ModAlt, "f")}));
    // a lone ESC: Escape, once nothing followed it for a moment; until then it waits
    {
        KeyDecoder d;
        d.feed("\x1b", 0);
        CHECK(d.take(0.01).empty() && d.pending());
        CHECK((d.take(1.0) == std::vector<Key>{key(plat::Key_Escape)}));
        CHECK(!d.pending());
    }
    // a sequence in two reads is one key
    {
        KeyDecoder d;
        d.feed("\x1b[", 0);
        CHECK(d.take(0.001).empty());
        d.feed("B", 0.002);
        CHECK((d.take(0.003) == std::vector<Key>{key(plat::Key_Down)}));
    }
    // CSI u (fixterms, kitty; the Windows console's Ctrl+Shift+letter, Ctrl+Enter…): code point ; modifiers
    CHECK((decode("\x1b[97;6u") == std::vector<Key>{key('A', plat::ModCtrl | plat::ModShift)}));
    CHECK((decode("\x1b[13;5u") == std::vector<Key>{key(plat::Key_Return, plat::ModCtrl)}));
    CHECK((decode("\x1b[9;5u") == std::vector<Key>{key(plat::Key_Tab, plat::ModCtrl)}));
    CHECK((decode("\x1b[97;2u") == std::vector<Key>{key('A', plat::ModShift, "A")}));
    CHECK((decode("\x1b[97u") == std::vector<Key>{key('A', 0, "a")}));
    CHECK((decode("\x1b[233;3u") == std::vector<Key>{key(0xE9, plat::ModAlt)}));
    // ESC ESC: Escape at once (the Windows console's Escape key)
    {
        KeyDecoder d;
        d.feed("\x1b\x1b", 0);
        CHECK((d.take(0) == std::vector<Key>{key(plat::Key_Escape)}));
    }
    // a terminal's reply (not a key) is left out; what follows comes through
    CHECK((decode("\x1b[?1;2cq") == std::vector<Key>{key('Q', 0, "q")}));
}

void testScreen() {
    Screen s(10, 3);
    CHECK(s.text(1, 1, "héllo", style(Red)) == 5);
    CHECK(s.line(1) == " héllo");
    CHECK(s.text(8, 1, "long", Style()) == 2);  // (cut at the edge)
    CHECK(fit("abcdef", 4) == "abc…" && fit("abc", 4) == "abc" && columns("é…") == 2);
    // the first output draws everything; the same picture again: nothing
    Screen shown(1, 1);
    std::string first = s.output(shown, true);
    CHECK(first.find("\x1b[2J") != std::string::npos && first.find("h\xc3\xa9llo") != std::string::npos);
    CHECK(s.output(shown, true).empty());
    // one cell changed: it alone (moved to, row 3 column 2, in its style)
    s.set(1, 2, U'x', style(Green, Default, Bold));
    std::string one = s.output(shown, true);
    CHECK(one.find("\x1b[3;2H") != std::string::npos && one.find("x") != std::string::npos &&
          one.find("\x1b[0;1;32m") != std::string::npos && one.find("llo") == std::string::npos);
    // without UTF-8: lines and blocks as ASCII, other characters '?'
    Screen b(6, 3);
    b.box(0, 0, 6, 3, Style());
    b.set(2, 1, U'█', Style());
    b.set(3, 1, U'é', Style());
    Screen none(1, 1);
    std::string ascii = b.output(none, false);
    CHECK(ascii.find("+----+") != std::string::npos);
    CHECK(ascii.find("#?") != std::string::npos);
    bool allAscii = true;
    for (char c : ascii) allAscii = allAscii && (unsigned char)c < 0x80;
    CHECK(allAscii);
    // symbols that have an emoji form (⏮ ✔ ❄ ⚠…: some terminals draw them as colour emoji, two cells wide, over
    // the next cell): a look-alike that has none where there is one, else the symbol followed by U+FE0E ("as
    // text"); one cell either way, as the layout counted them
    Screen e(12, 1);
    CHECK(columns("⏮[ ]✔❄⚠▶↗©") == 10);
    CHECK(e.text(0, 0, "⏮[ ]✔❄⚠▶↗©", Style()) == 10);
    CHECK(e.line(0) == "⇤[ ]✓*!►↗©");
    Screen none2(1, 1);
    std::string drawn = e.output(none2, true);
    CHECK(drawn.find("↗\xef\xb8\x8e") != std::string::npos && drawn.find("©\xef\xb8\x8e") != std::string::npos);
    CHECK(drawn.find("⏮") == std::string::npos && drawn.find("⇤\xef\xb8\x8e") == std::string::npos);
    int wrong = 0;
    for (char32_t c = 0xA0; c < 0x20000; c++) {
        if (!hasEmojiForm(c)) continue;
        char32_t t = textForm(c);
        if (cellWidth(c) < 1 || cellWidth(t) != cellWidth(c) || (t != c && hasEmojiForm(t))) {
            if (wrong++ < 5) printf("U+%04X: %d cells, its text form U+%04X %d\n", (unsigned)c, cellWidth(c), (unsigned)t, cellWidth(t));
        }
    }
    CHECK(wrong == 0);
    CHECK(hasEmojiForm(U'⏮') && hasEmojiForm(U'©') && !hasEmojiForm(U'●') && !hasEmojiForm(U'a') && !hasEmojiForm(U'🥁'));
    // colours: the nearest of the 8; menu texts without &
    CHECK(nearestColor("#e03030", Blue) == Red && nearestColor("#30c040", Blue) == Green && nearestColor("", Blue) == Blue);
    char m = 0;
    CHECK(menuText("Save &as…", &m) == "Save as…" && m == 'A');
}

// --- the front end ---

struct Driver {
    MainWindow* w;
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{100, 30};

    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    // the keys, through dialogs they open (each key after a frame of 50 ms); then a picture
    void keys(std::vector<Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        settle();
    }
    void settle(double seconds = 0.3) {  // (the undo history looks every 250 ms)
        for (double t = 0; t < seconds; t += 0.05) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    void picture(const std::string& name) {
        f->draw(screen);
        std::ofstream(OUT + "/" + name + ".txt") << screen.lines();
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
};

std::vector<Key> typed(const std::string& s) {
    std::vector<Key> out;
    for (char c : s) {
        std::string t(1, c);
        if (c >= 'a' && c <= 'z') out.push_back(key(c - 'a' + 'A', 0, t));
        else if (c >= 'A' && c <= 'Z') out.push_back(key(c, plat::ModShift, t));
        else out.push_back(key(c, 0, t));
    }
    return out;
}

std::vector<Key> operator+(std::vector<Key> a, const std::vector<Key>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// the windows of the program open (dialogs and others), but the main one
int openWindowsBut(ui::Window* main) {
    int n = 0;
    for (ui::Window* w : ui::App::get().windows())
        if (w != main && w->isWindowShown()) n++;
    return n;
}

ui::Window* topWindowBut(ui::Window* main) {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (*it != main && (*it)->isWindowShown()) return *it;
    return nullptr;
}

bool x11Loaded() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
        if (line.find("libX11") != std::string::npos) return true;
    return false;
}

void testFront() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;  // (as tui::run: shown to the toolkit, drawn as text)
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{w, &f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    f.showFolder(TMP);
    d.settle();

    // the layout: the window's, top to bottom and left to right
    d.picture("start");
    std::string top = d.screen.line(0), transport = d.screen.line(1);
    CHECK(top.find("File") != std::string::npos && top.find("Edit") < top.find("View") &&
          top.find("View") < top.find("Options") && top.find("Untitled") != std::string::npos);
    CHECK(transport.find("Play") != std::string::npos && transport.find("120 BPM") != std::string::npos &&
          transport.find("1.1.1") != std::string::npos);
    CHECK(transport.find("⇤ [ ]Loop") != std::string::npos);  // (to the start: as text, one cell)
    std::string third = d.screen.line(2) + d.screen.line(3);
    CHECK(third.find("Explorer") != std::string::npos && third.find("Tracks") != std::string::npos);
    CHECK(third.find("Explorer") < third.find("Tracks"));
    CHECK(d.screen.line(28).find("Master") != std::string::npos);
    CHECK(d.shows("beep.wav"));  // (the Explorer's folder)

    // the track panel: a new audio track (A), renamed through the program's own dialog (Enter)
    d.keys({key(plat::Key_Tab), key('A', 0, "a")});
    CHECK(f.pane == Front::Pane::Panel && w->project.tracks.size() == 1);
    d.keys(std::vector<Key>{key(plat::Key_Return), key('A', plat::ModCtrl)} + typed("Drums") + std::vector<Key>{key(plat::Key_Return)});
    CHECK(!w->project.tracks.empty() && w->project.tracks[0]->name == "Drums");
    CHECK(d.shows("Drums"));
    // mute, solo, volume, pan
    d.keys({key('M', 0, "m"), key('S', 0, "s"), key(plat::Key_Left), key(plat::Key_Left), key(']', 0, "]")});
    TrackPtr t = w->project.tracks[0];
    CHECK(t->muted && t->soloed && t->volumeDb == -2 && std::fabs(t->pan - 0.1) < 1e-9);
    d.picture("panel");

    // the Explorer: a sample added at the cursor (on the empty audio track)
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab)});
    CHECK(f.pane == Front::Pane::Explorer);
    int wav = -1;
    for (int i = 0; i < (int)f.entries.size(); i++)
        if (f.entries[(size_t)i].name == "beep.wav") wav = i;
    CHECK(wav >= 0);
    f.entry = wav;
    d.keys({key(plat::Key_Return)});
    CHECK(w->project.clips.size() == 1 && w->project.clips[0]->track == t);
    // an empty folder: Down and PgDn stay on the first row (once -1, read before the list: test_stress found it)
    util::makeDirs(TMP + "/empty");
    f.showFolder(TMP + "/empty");
    d.keys({key(plat::Key_Down), key(plat::Key_PageDown)});
    CHECK(f.entries.empty() && f.pane == Front::Pane::Explorer);
    CHECK(f.entry == 0 && d.shows("(nothing"));
    f.showFolder(TMP);

    // the timeline: select the clip at the cursor, split it a beat in, move the right piece by the grid
    d.keys({key(plat::Key_Tab), key(plat::Key_Return)});
    CHECK(f.pane == Front::Pane::Timeline && w->timeline->selection.clips.size() == 1);
    w->project.timing.grid = 1;  // (a beat: the grid steps the keys go by)
    d.keys({key(plat::Key_Right), key('S', 0, "s")});
    CHECK(w->timeline->cursor == w->engine.sr / 2);  // (a beat at 120 BPM)
    CHECK(w->project.clips.size() == 2);
    d.keys({key(plat::Key_Right), key(plat::Key_Return)});
    CHECK(w->timeline->selection.clips.size() == 1);
    ClipPtr right = w->timeline->selection.clips.empty() ? nullptr : *w->timeline->selection.clips.begin();
    CHECK(right && right->start == w->engine.sr / 2);
    d.keys({key('.', 0, ".")});
    CHECK(right && right->start == w->engine.sr);
    d.picture("timeline");
    // undone (Ctrl+Z, the window's shortcut): back where it was
    d.keys({key('Z', plat::ModCtrl)});
    CHECK(right && right->start == w->engine.sr / 2);
    // a range (I … O) over the track, looped (L)
    d.keys({key(plat::Key_Home), key('I', 0, "i"), key(plat::Key_Right), key(plat::Key_Right), key('O', 0, "o"), key('L', 0, "l")});
    CHECK(w->timeline->selection.range && w->timeline->selection.range->end == w->engine.sr);
    CHECK(w->project.timing.looping && w->project.timing.loopEnd == 1.0);

    // the menus: F10 opens File, Right the next one, Esc closes; Alt+E opens Edit
    d.keys({key(plat::Key_F10)});
    CHECK(f.barMenu == 0 && f.menuOpen());
    d.picture("file-menu");
    CHECK(d.shows("Quit") && d.shows("Open project…"));
    d.keys({key(plat::Key_Right)});
    CHECK(f.barMenu == 1);
    d.keys({key(plat::Key_Escape)});
    CHECK(!f.menuOpen());
    d.keys({key('E', plat::ModAlt, "e")});
    CHECK(f.barMenu == 1 && d.shows("Undo"));
    d.keys({key(plat::Key_Escape)});
    // F2: the selection's menu (the timeline's own)
    d.keys({key(plat::Key_F2)});
    CHECK(f.menuOpen() && f.barMenu < 0);
    d.keys({key(plat::Key_Escape)});
    // Options → Discord status: off by default, the status said; another application ID typed in (kept off here,
    // so a Discord running on this computer is never talked to)
    CHECK(!w->prefs.discordPresence && w->prefs.discordApplicationId.empty());
    d.keys({key('O', plat::ModAlt, "o"), key('D', 0, "d")});
    d.picture("discord-menu");
    CHECK(d.shows("Show in Discord") && d.shows("Application ID") && d.shows("Status: Off"));
    d.keys(std::vector<Key>{key('A', 0, "a"), key('A', plat::ModCtrl)} + typed(" 1234 ") + std::vector<Key>{key(plat::Key_Return)});
    CHECK(w->prefs.discordApplicationId == "1234" && !w->prefs.discordPresence);
    CHECK(Preferences::load().discordApplicationId == "1234");
    CHECK(w->discordPresence && w->discordPresence->state() == discord_presence::Presence::State::Off);
    if (f.menuOpen()) d.keys({key(plat::Key_Escape), key(plat::Key_Escape)});

    // saving: Ctrl+S, the program's file chooser as text, a name typed
    std::string saved = std::filesystem::absolute(TMP + "/song.json").string();  // (the chooser starts elsewhere)
    util::removeFile(saved);
    d.keys(std::vector<Key>{key('S', plat::ModCtrl), key('A', plat::ModCtrl)} + typed(saved) + std::vector<Key>{key(plat::Key_Return)});
    // (the same file however it is spelled: Windows' absolute() writes backslashes, the program keeps "C:/…")
    CHECK(util::fileExists(saved) && std::filesystem::exists(w->projectPath) &&
          std::filesystem::equivalent(w->projectPath, saved));
    if (w->projectPath != saved) printf("saved as %s (asked: %s)\n", w->projectPath.c_str(), saved.c_str());
    CHECK(d.screen.line(0).find("song") != std::string::npos);
    // a dialog drawn: Ctrl+O's chooser, as text; Esc cancels it
    d.queue = {key('O', plat::ModCtrl)};
    d.at = 0;
    bool sawChooser = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);
        sawChooser = d.shows("Open project") && d.shows("File name") && d.shows("Tiny DAW project");  // (wherever it starts)
        std::ofstream(OUT + "/open-dialog.txt") << d.screen.lines();
        d.queue.push_back(key(plat::Key_Escape));
        d.step(done);
    };
    d.step([] { return false; });
    CHECK(sawChooser);
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };

    // --- phase 3: a render, through File → Render project… and the file chooser ---
    {
        std::string mix = std::filesystem::absolute(TMP + "/mix.wav").string();
        util::removeFile(mix);
        d.keys(std::vector<Key>{key(plat::Key_F10), key('P', 0, "p"), key(plat::Key_Return), key('A', plat::ModCtrl)} +
               typed(mix) + std::vector<Key>{key(plat::Key_Return)});
        d.settle(1.0);
        CHECK(util::fileExists(mix) && util::fileSize(mix) > 1000);
        while (ui::Window* top = topWindowBut(w)) top->close();  // (a message about it, if any)
    }

    // --- phase 4: a track's effects (E), added, wired, bypassed, opened ---
    d.keys({key(plat::Key_Tab)});  // (to the track panel)
    f.setCurrent(t);
    d.keys({key('E', 0, "e")});
    ui::Window* fx = topWindowBut(w);
    CHECK(fx && d.shows("IN → OUT"));
    // (the chooser: the Bit-crusher looked for, whatever else is installed)
    d.keys(std::vector<Key>{key(plat::Key_Insert)} + typed("bit-crusher") + std::vector<Key>{key(plat::Key_Return)});
    CHECK(t->effects.size() == 1);
    d.picture("effects");
    CHECK(d.shows("Bit-crusher"));
    // C: plug it in after IN (the menu's choices, in its exec(): IN → its input chosen there)
    bool sawPlug = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);  // (the control's menu, taken up as the bar's are)
        sawPlug = f.menuOpen() && d.shows("IN → its input");
        if (f.menuOpen())
            for (int i = 0; i < (int)f.open.back().menu->actions().size(); i++)
                if (f.open.back().menu->actions()[(size_t)i]->text == "IN → its input") f.open.back().item = i;
        d.queue.push_back(key(plat::Key_Return));
        d.step(done);
    };
    d.keys({key('C', 0, "c")});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawPlug);
    CHECK(d.shows("[on]  Bit-crusher") && d.shows("from IN"));
    d.keys({key('B', 0, "b")});
    CHECK(t->effects[0]->bypass);
    d.keys({key('B', 0, "b"), key(plat::Key_Return)});  // its own window: its knobs as sliders
    CHECK(!t->effects[0]->bypass && d.shows("Bits") && d.shows("Downsample"));
    d.keys({key('W', plat::ModCtrl)});
    // a plugin's parameters as a list (what a VST plugin shows without a window system)
    {
        ParamWindow pw(w, t->effects[0], "Parameters");
        CHECK(pw.attach() && pw.list->count() >= 3);
        auto plugin = std::atomic_load(&t->effects[0]->instance);
        uint32_t id = pw.params[0].id;
        double before = plugin->getParam(id);
        ui::KeyEvent right;
        right.key = plat::Key_Right;
        pw.keyPress(right);
        CHECK(plugin->getParam(id) > before);
        pw.close();
    }
    while (ui::Window* top = topWindowBut(w)) top->close();

    // --- the Pad Kit (a MIDI track, I: its instrument menu), its pads by the arrows ---
    d.keys({key('N', 0, "n"), key('I', 0, "i"), key(plat::Key_Down), key(plat::Key_Down), key(plat::Key_Down),
            key(plat::Key_Return)});  // (past the built-in synth, the Drawn synth and the sampler)
    TrackPtr midi = w->timeline->currentTrack;
    CHECK(midi && midi->kind == Kind::Midi && midi->padKit);
    CHECK(d.shows("13 empty") && d.shows("> 1 empty"));
    d.picture("pad-kit");
    while (ui::Window* top = topWindowBut(w)) top->close();

    // --- the MIDI editor: N (the timeline) a new clip, notes by the keyboard's cursor ---
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab)});  // (panel → transport → Explorer → timeline)
    CHECK(f.pane == Front::Pane::Timeline);
    f.setCurrent(midi);
    w->timeline->setCursor(0);
    d.keys({key('N', 0, "n")});
    ClipPtr clip;
    for (auto& c : w->project.clipsOn(midi)) clip = c;
    CHECK(clip && topWindowBut(w));
    d.keys({key(plat::Key_Return), key(plat::Key_Escape), key(plat::Key_Right), key(plat::Key_Up), key(plat::Key_Up),
            key(plat::Key_Return), key('.', 0, ".")});
    CHECK(clip && clip->notes && clip->notes->size() == 2);
    if (clip && clip->notes && clip->notes->size() == 2) {
        const auto& ns = *clip->notes;
        CHECK(std::fabs(ns[1].pitch - ns[0].pitch) == 2);
        CHECK(ns[1].end - ns[1].start > ns[0].end - ns[0].start);  // (the second: a step longer)
    }
    d.picture("midi-editor");
    CHECK(d.shows("▐"));
    // the Steps view (its toolbar's first button: Tab to it, Space), a pad lit by Enter
    d.keys({key(plat::Key_Escape), key(plat::Key_Tab), key(plat::Key_Space, 0, " "), key(plat::Key_Right),
            key(plat::Key_Right), key(plat::Key_Right), key(plat::Key_Return)});
    CHECK(clip && clip->notes && clip->notes->size() == 3);
    CHECK(d.shows("+ Row (R)"));
    d.keys({key('W', plat::ModCtrl)});
    CHECK(!topWindowBut(w));

    // --- an automation lane (U: its Volume), a point and the curve raised at the cursor ---
    d.keys({key(plat::Key_Tab)});  // (the track panel)
    f.setCurrent(t);
    d.keys({key('U', 0, "u"), key(plat::Key_Return)});  // (the parameter: the first, Volume)
    CHECK(!t->automation.empty());
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Down), key(plat::Key_Home),
            key(plat::Key_Return), key(plat::Key_Right), key(']', 0, "]"), key(']', 0, "]")});
    CHECK(f.currentLane() && f.currentLane()->automation);
    if (!t->automation.empty()) {
        const auto& pts = *t->automation[0]->points;
        CHECK(pts.size() == 2 && pts[1].v > pts[0].v);
    }
    d.picture("automation");
    CHECK(d.shows("●") && d.shows("↳ Volume"));

    // --- sends: a return track (Ctrl+Alt+T), the track's sends (D) raised ---
    d.keys({key('T', plat::ModCtrl | plat::ModAlt), key(plat::Key_Tab)});
    f.setCurrent(t);
    d.keys({key('D', 0, "d"), key(plat::Key_PageUp), key(plat::Key_PageUp)});
    CHECK(!t->sends.empty() && t->sends[0].db > config::MIN_DB);
    d.keys({key(plat::Key_Escape)});
    while (ui::Window* top = topWindowBut(w)) top->close();

    // --- the transport's time signature: shown N/D; Enter types one ("7/8") ---
    CHECK(transport.find("4/4") != std::string::npos);
    Front::Pane paneWas = f.pane;
    int controlWas = f.control;
    f.pane = Front::Pane::Transport;
    f.control = 8;  // (C_BEATS)
    d.queue = {key(plat::Key_Return)};
    d.at = 0;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.queue.push_back(key(plat::Key_End));
        for (int i = 0; i < 6; i++) d.queue.push_back(key(plat::Key_Backspace));
        for (const char* c : {"7", "/", "8"}) d.queue.push_back(key(c[0], 0, c));
        d.queue.push_back(key(plat::Key_Return));
        d.step(done);
    };
    d.step([] { return false; });
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(w->project.timing.sig() == (TimeSig{7, 8}));
    d.settle();
    CHECK(d.screen.line(1).find("7/8") != std::string::npos);
    w->project.timing.setSignature(1, {4, 4});
    w->setTiming({});
    f.pane = paneWas, f.control = controlWas;

    // --- the ruler (Up from the first lane): Enter, the tempo dialog; F12 hides an open window ---
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab)});
    f.selectLane(0);
    d.queue = {key(plat::Key_Up), key(plat::Key_Return)};
    d.at = 0;
    bool sawTempo = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);
        sawTempo = d.shows("Tempo from bar 1") && d.shows("BPM");
        d.queue.push_back(key(plat::Key_Escape));
        d.step(done);
    };
    d.step([] { return false; });
    CHECK(sawTempo && f.laneRow == -1);
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    d.keys({key(plat::Key_Down), key('E', 0, "e")});  // (a window of its own: the track's effects)
    CHECK(openWindowsBut(w) == 1);
    d.keys({key(plat::Key_F12)});
    CHECK(f.windowsHidden && !d.shows("IN →"));
    d.keys({key(plat::Key_F12), key('W', plat::ModCtrl)});
    CHECK(!f.windowsHidden && openWindowsBut(w) == 0);

    // the keys
    d.keys({key(plat::Key_F1)});
    CHECK(f.helpShown && d.shows("Keys"));
    d.keys({key(plat::Key_PageDown), key(plat::Key_Down)});  // (scrolls when it doesn't fit; stays open)
    CHECK(f.helpShown);
    d.keys({key(plat::Key_Space, 0, " ")});
    CHECK(!f.helpShown);

    // the update notice at the status line's right, as the master strip's (compared with what the strip holds
    // after drawing: the program's own check may answer meanwhile)
    {
        update_check::Result up;
        up.state = "waiting";
        up.latest = "3f9a2c1d4b5e6f708192a3b4c5d6e7f8091a2b3c";
        up.latestTime = (double)time(nullptr) - 4 * 3600;
        w->master->showUpdate(up);
        d.settle();
        const auto& r = w->master->updateResult;
        std::string notice = update_notice::noticeText(r ? &*r : nullptr).plain();
        CHECK(!notice.empty() && util::endsWith(d.screen.line(d.screen.height() - 1), notice));
        Screen narrow{30, 10};
        f.draw(narrow);  // (too narrow for both: the keys' hint alone)
        CHECK(!util::endsWith(narrow.line(9), notice));
    }

    // never the window system's libraries
    CHECK(!x11Loaded());

    // Ctrl+Q: closed (the main window's own closing)
    d.keys({key('Q', plat::ModCtrl)});
    CHECK(f.wantsQuit());
    app.nestedLoop = nullptr;
    delete w;
    app.paintWindows = true;
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    util::removeFile(TMP + "/settings.json");  // (a run's recent plugins and projects aren't the next one's)
    settings::setPath(TMP + "/settings.json");
    // a sample for the Explorer: 2 s of a quiet tone
    {
        std::vector<float> data(48000 * 2 * 2);
        for (size_t i = 0; i < data.size(); i++) data[i] = 0.1f * (float)std::sin((double)(i / 2) * 0.05);
        std::string error;
        CHECK(writeWav(TMP + "/beep.wav", data.data(), data.size() / 2, 2, 48000, SampleFormat::Pcm16, error));
    }
    testKeys();
    testScreen();
    testFront();
    TEST_MAIN_END();
}
