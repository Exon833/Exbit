// Commands and their keys (ui/commands.h, ui/shortcuts_dialog.h): ids from the menu paths, stable from one build
// of the menus to the next; 1,000 keys bound, conflicting and chorded, through the dialog; a damaged keymap file;
// a rebound key runs its command and the old one doesn't (a menu entry, a transport key, the timeline's own
// keys); the menus show the new key; reset; the palette filters and runs commands, a selected clip's right-click
// entries too; the terminal's Ctrl+P palette and a rebound key there; which keys text consoles send.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "tui/window_view.h"
#include "ui/commands.h"
#include "ui/main_window.h"
#include "ui/shortcuts_dialog.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"
#include "ui/toolkit/widget.h"

namespace {

const std::string TMP = "build/test-tmp/commands";
const std::string OUT = "build/ui-samples/commands";  // (pictures: the dialog, the terminal's palette)
using commands::Registry;
using ui::KeySequence;

KeySequence ks(const char* t) { return KeySequence::parse(t); }

void press(ui::Window& w, const char* keys) {
    KeySequence k = ks(keys);
    tui::Key key{k.key, k.mods, ""};
    if (!k.mods && k.key >= 'A' && k.key <= 'Z') key.text = std::string(1, (char)(k.key - 'A' + 'a'));
    tui::sendKey(w, key);
}

bool hasId(const std::vector<commands::Command>& cs, const std::string& id) {
    for (auto& c : cs)
        if (c.id == id) return true;
    return false;
}

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// --- without a window ---------------------------------------------------------------------------

void testBasics() {
    CHECK(commands::plainText("&Save as…") == "Save as…");
    CHECK(commands::plainText("Computer &keyboard\tCtrl+K") == "Computer keyboard");
    CHECK(commands::plainText("Rock && roll") == "Rock & roll");
    CHECK(commands::fuzzyScore("", "anything") == 0);
    CHECK(commands::fuzzyScore("xyz", "Undo") < 0);
    CHECK(commands::fuzzyScore("sva", "Save as…") >= 0);
    CHECK(commands::fuzzyScore("undo", "Edit Undo") > commands::fuzzyScore("undo", "Edit Unfold dots"));  // (in a row)
    // what text consoles send, checked against the terminal's own decoder for a few
    tui::KeyDecoder d;
    d.feed("\x1b[1;6A", 0);  // Ctrl+Shift+Up (xterm)
    auto got = d.take(1);
    CHECK(got.size() == 1 && got[0].key == plat::Key_Up && got[0].mods == (plat::ModCtrl | plat::ModShift));
    CHECK(commands::terminalSends(ks("Ctrl+Shift+Up")));
    CHECK(commands::terminalSends(ks("Ctrl+S")) && commands::terminalSends(ks("Alt+X")) && commands::terminalSends(ks("Space")));
    CHECK(!commands::terminalSends(ks("Ctrl+Shift+P")) && !commands::terminalSends(ks("Ctrl+Shift+Z")));
    CHECK(!commands::terminalSends(ks("Ctrl+I")) && !commands::terminalSends(ks("Ctrl+M")));  // (Tab, Return)
    CHECK(!commands::terminalSends(ks("Ctrl+1")) && !commands::terminalSends(ks("Meta+A")));
    CHECK(!commands::terminalSends(ks("Ctrl+P")) && !commands::terminalSends(ks("F1")));  // (the terminal's own)
    CHECK(shortcuts_dialog::keyLabel(ks("Ctrl+Shift+P")) == "Ctrl+Shift+P (not in the terminal)");
    CHECK(shortcuts_dialog::keyLabel(ks("Ctrl+S")) == "Ctrl+S");
}

// a menu bar of its own: ids, defaults, the keymap's text and a damaged file
void testKeymapFile() {
    ui::Window w{plat::WindowOptions{}};
    auto* bar = w.add<ui::MenuBar>();
    int saved = 0, undone = 0;
    ui::Menu* file = bar->addMenu("&File");
    file->addAction("&Save", [&] { saved++; }, "Ctrl+S");
    file->addAction("Save &as…", nullptr, "Ctrl+Shift+S");
    ui::Menu* sub = file->addMenu("&Recent");
    sub->addAction("a.json");
    sub->addAction("a.json");  // (the same text twice: "(2)")
    ui::Menu* edit = bar->addMenu("&Edit");
    edit->addAction("&Undo", [&] { undone++; }, "Ctrl+Z");
    w.showWindow();
    Registry r(bar);
    r.apply();
    std::vector<commands::Command> cs = r.list();
    CHECK(hasId(cs, "File/Save") && hasId(cs, "File/Save as…") && hasId(cs, "Edit/Undo"));
    CHECK(hasId(cs, "File/Recent/a.json") && hasId(cs, "File/Recent/a.json (2)"));
    CHECK(commands::keysText(r.keys("Edit/Undo")) == "Ctrl+Z");

    // a damaged file: each bad line named and skipped, the rest read, nothing crashes
    std::string bad = "# a comment\n"
                      "File/Save = Ctrl+Alt+S\r\n"          // (2: good, Windows line ends)
                      "this line has no equals sign\n"       // 3
                      " = Ctrl+Q\n"                         // 4: no command
                      "Edit/Undo = Ctrl+Bogus\n"            // 5: no such key
                      "Edit/Undo = Ctrl+Y\n"                // 6: good
                      "Edit/Undo = Alt+Backspace\n"         // 7: good (a second key)
                      "Nowhere/Gone = F7\n"                 // 8: unknown command: kept
                      "File/Save as… =\n"                   // 9: no keys at all
                      "\xff\xfe\x01 = \x80\x81\n" +          // 10: garbage bytes
                      std::string(100000, 'x') + " = Ctrl+" + std::string(5000, 'q') + "\n";  // 11: absurd
    std::vector<std::string> problems = r.load(bad, "keymap.txt");
    r.apply();
    CHECK(problems.size() == 5);
    if (problems.size() != 5)
        for (auto& p : problems) fprintf(stderr, "  %s\n", p.substr(0, 120).c_str());
    bool named = problems.size() == 5 && problems[0].find("keymap.txt line 3:") == 0 &&
                 problems[1].find("line 4:") != std::string::npos && problems[2].find("line 5:") != std::string::npos &&
                 problems[3].find("line 10:") != std::string::npos && problems[4].find("line 11:") != std::string::npos;
    CHECK(named);
    CHECK(commands::keysText(r.keys("File/Save")) == "Ctrl+Alt+S");
    CHECK(commands::keysText(r.keys("Edit/Undo")) == "Ctrl+Y, Alt+Backspace");
    CHECK(r.keys("File/Save as…").empty());
    CHECK(commands::keysText(r.keys("Nowhere/Gone")) == "F7");
    // the menus show the first key; the keys work, the old ones don't
    CHECK(file->actions()[0]->shortcut.text() == "Ctrl+Alt+S");
    press(w, "Ctrl+Alt+S");
    press(w, "Ctrl+S");
    CHECK(saved == 1);
    press(w, "Alt+Backspace");
    press(w, "Ctrl+Z");
    CHECK(undone == 1);
    // random bytes: never a crash
    std::mt19937 rng(7);
    for (int round = 0; round < 20; round++) {
        std::string junk(20000, '\0');
        for (auto& c : junk) c = (char)(rng() % 256);
        if (round % 2) for (size_t i = 0; i < junk.size(); i += 37) junk[i] = '\n';
        r.load(junk, "junk.txt");
    }
    r.resetAll();  // (path unset: nothing written)
    CHECK(commands::keysText(r.keys("File/Save")) == "Ctrl+S" && commands::keysText(r.keys("Edit/Undo")) == "Ctrl+Z");
    // the text written reads back the same
    r.setKeys("Edit/Undo", {ks("Ctrl+="), ks("Ctrl++"), ks("Shift+F3")});
    r.setKeys("File/Save", {});
    std::string text = r.text(false);
    r.resetAll();
    CHECK(r.load(text, "again.txt").empty());
    CHECK(commands::keysText(r.keys("Edit/Undo")) == "Ctrl+=, Ctrl++, Shift+F3" && r.keys("File/Save").empty());
    r.resetAll();
    // export: every command with keys; a file that can't be written is said so
    CHECK(r.exportTo(TMP + "/all.txt"));
    std::string all;
    CHECK(util::readFile(TMP + "/all.txt", all) && all.find("File/Save as… = Ctrl+Shift+S") != std::string::npos);
    CHECK(!r.exportTo("/nonexistent-dir/x/keymap.txt"));
}

// --- the main window ----------------------------------------------------------------------------

std::vector<std::string> idsOf(MainWindow& w) {
    std::vector<std::string> ids;
    for (auto& c : w.commands->list()) ids.push_back(c.id);
    return ids;
}

// the modal dialogs a test opens: each step runs once it is up
std::vector<std::function<void(ui::Window*)>> dialogSteps;

ui::Window* topDialog(ui::Window* main) {
    ui::Window* top = nullptr;
    for (ui::Window* x : ui::App::get().windows())
        if (x != main && x->isWindowShown() && dynamic_cast<ui::Dialog*>(x)) top = x;
    return top;
}

void testWindow() {
    ui::App& app = ui::App::get();
    std::vector<std::string> first;
    {  // (a damaged keymap in the settings folder: the program starts, its good lines used, the others named)
        util::writeFile(TMP + "/keymap.txt", "garbage\nFile/Save = Ctrl+Alt+Shift+S\n\xc3\x28 = \xff\n");
        auto* w = new MainWindow();
        CHECK(commands::keysText(w->commands->keys("File/Save")) == "Ctrl+Alt+Shift+S");
        for (int i = 0; i < 40; i++) ui::App::get().advanceTimers(0.05);
        CHECK(w->status->message().find("keymap.txt line 1:") == 0 && w->status->message().find("(and 1 more)") != std::string::npos);
        w->commands->resetAll();  // (keymap.txt rewritten: nothing changed)
        first = idsOf(*w);
        w->askBeforeClosing = false;
        delete w;
    }
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Registry& r = *w->commands;
    // ids: the same from one build of the menus to the next; the menus', the keys'
    std::vector<std::string> second = idsOf(*w);
    CHECK(!first.empty() && first == second);
    for (const char* id : {"File/Save", "Edit/Undo", "Edit/Redo", "View/Explorer", "View/Command palette…",
                           "Options/Keyboard shortcuts…", "Transport/Play", "Timeline/Split", "Tracks/Move the track up",
                           "Grid/Finer",
                           "File/Collect and save", "File/Save as archive…", "File/Find missing samples…"})
        CHECK(r.known(id));
    CHECK(commands::keysText(r.keys("Edit/Redo")) == "Ctrl+Shift+Z, Ctrl+Y");
    CHECK(commands::keysText(r.keys("Options/Computer keyboard")) == "Ctrl+K");  // (the transport's ⌨, shown)
    // no two commands start with the same key
    {
        std::map<std::string, std::string> seen;
        bool clash = false;
        for (auto& c : r.list())
            if (c.aliasOf.empty())
                for (auto& k : c.keys) {
                    std::string t = k.text();
                    if (seen.count(t)) {
                        fprintf(stderr, "  %s: %s and %s\n", t.c_str(), seen[t].c_str(), c.id.c_str());
                        clash = true;
                    }
                    seen[t] = c.id;
                }
        CHECK(!clash);
    }

    // a menu entry rebound: the new key works, the old one doesn't, the menu shows it; reset
    CHECK(w->explorerDock->isVisible());
    r.setKeys("View/Explorer", {ks("Ctrl+Alt+E")});
    CHECK(w->explorerAction->shortcut.text() == "Ctrl+Alt+E");
    press(*w, "Ctrl+E");
    CHECK(w->explorerDock->isVisible());
    press(*w, "Ctrl+Alt+E");
    CHECK(!w->explorerDock->isVisible());
    r.reset("View/Explorer");
    CHECK(w->explorerAction->shortcut.text() == "Ctrl+E");
    press(*w, "Ctrl+E");
    CHECK(w->explorerDock->isVisible());
    // a transport key: Loop from L to Ctrl+Shift+L
    bool looping = w->transport->loopAction->isChecked();
    r.setKeys("Transport/Loop", {ks("Ctrl+Shift+L")});
    press(*w, "L");
    CHECK(w->transport->loopAction->isChecked() == looping);
    press(*w, "Ctrl+Shift+L");
    CHECK(w->transport->loopAction->isChecked() != looping);
    CHECK(w->transport->loopAction->shortcut().text() == "Ctrl+Shift+L");
    r.reset("Transport/Loop");
    press(*w, "L");
    CHECK(w->transport->loopAction->isChecked() == looping);

    // the timeline's own keys: Split from S to X (and Delete from Del to S: swapped); the clip menu shows them
    TrackPtr track = w->timeline->addTrack(Kind::Midi);
    w->timeline->newMidiClip(track, 0, (int64_t)w->engine.sr * 4);
    w->timeline->setFocus();
    auto select = [&] {
        ClipPtr last;
        for (auto& c : w->project.clips) last = c;
        w->timeline->selection.selectOnly(last);
        w->timeline->cursor = (int64_t)w->engine.sr;
    };
    r.setKeys("Timeline/Split", {ks("X")});
    r.setKeys("Timeline/Delete the selection", {ks("S")});
    size_t clips = w->project.clips.size();
    select();
    press(*w, "Del");  // (no longer Delete's)
    CHECK(w->project.clips.size() == clips);
    select();
    press(*w, "X");    // split at the cursor
    CHECK(w->project.clips.size() == clips + 1);
    select();
    press(*w, "S");    // now Delete
    CHECK(w->project.clips.size() == clips);
    {
        ui::Menu m;
        select();
        w->timeline->fillMenu(m, std::nullopt);
        std::string split, del;
        for (auto& a : m.actions()) {
            if (a->text.rfind("Split", 0) == 0) split = a->shortcut.text();
            if (a->text.rfind("Delete clips", 0) == 0) del = a->shortcut.text();
        }
        CHECK(split == "X" && del == "S");
    }
    r.resetAll();
    {  // the file in the settings folder: written on every change
        std::string data;
        CHECK(util::readFile(r.path, data) && data.find("Timeline/Split") == std::string::npos);
    }

    // the palette: filtered by fuzzy match, recent first, runs a command; a selected clip's menu entry too
    app.nestedLoop = [w](const std::function<bool()>& done) {
        for (int guard = 0; guard < 50 && !done(); guard++) {
            ui::App::get().advanceTimers(0.02);
            ui::App::get().tick();
            ui::Window* top = topDialog(w);
            if (top && !dialogSteps.empty()) {
                auto step = dialogSteps.front();
                dialogSteps.erase(dialogSteps.begin());
                step(top);
            }
        }
    };
    {
        int shown = -1;
        std::string firstRow;
        dialogSteps = {[&](ui::Window* top) {
            auto* p = dynamic_cast<shortcuts_dialog::CommandPalette*>(top);
            CHECK(p != nullptr);
            if (!p) return;
            p->search->setText("vwexplr");  // View ▸ Explorer
            shown = p->list->count();
            firstRow = shown > 0 ? p->list->item(0).text : "";
            tui::sendKey(*p, tui::Key{plat::Key_Return, 0, ""});
        }};
        bool on = w->explorerDock->isVisible();
        CHECK(shortcuts_dialog::showPalette(w, r));
        CHECK(shown >= 1 && firstRow.rfind("\xe2\x9c\x93 Explorer", 0) == 0);  // (✓: checked)
        CHECK(w->explorerDock->isVisible() != on);
        CHECK(!r.recent.empty() && r.recent[0] == "View/Explorer");
    }
    {  // recent first: with nothing typed
        std::string firstRow;
        dialogSteps = {[&](ui::Window* top) {
            auto* p = dynamic_cast<shortcuts_dialog::CommandPalette*>(top);
            if (p) firstRow = p->list->count() ? p->list->item(0).text : "";
            if (p) p->reject();
        }};
        CHECK(!shortcuts_dialog::showPalette(w, r));
        CHECK(firstRow.find("Explorer") != std::string::npos);
    }
    {  // a disabled one says why and doesn't run
        std::string info;
        dialogSteps = {[&](ui::Window* top) {
            auto* p = dynamic_cast<shortcuts_dialog::CommandPalette*>(top);
            if (!p) return;
            w->timeline->selection.clear();
            p->search->setText("Make unique");
            for (int i = 0; i < p->list->count(); i++)
                if (p->list->item(i).text.find("Make unique") != std::string::npos) p->choose(i);
            info = p->info->text();
            p->reject();
        }};
        select();  // (a clip selected, not linked: Make unique is disabled)
        CHECK(!shortcuts_dialog::showPalette(w, r));
        CHECK(info.find("can't be used now") != std::string::npos);
    }
    {  // a selected clip's right-click entry: Clip ▸ Duplicate clips
        select();
        size_t before = w->project.clips.size();
        bool found = false;
        dialogSteps = {[&](ui::Window* top) {
            auto* p = dynamic_cast<shortcuts_dialog::CommandPalette*>(top);
            if (!p) return;
            p->search->setText("clip duplicate clips");
            for (int i = 0; i < p->list->count() && !found; i++)
                if (p->list->item(i).text.rfind("Duplicate clips", 0) == 0 && p->commands[(size_t)std::stoi(p->list->item(i).data)].context) {
                    found = true;
                    p->choose(i);
                }
            if (!found) p->reject();
        }};
        CHECK(shortcuts_dialog::showPalette(w, r));
        CHECK(found && w->project.clips.size() == before + 1);
    }

    // the dialog: 1,000 keys bound, chorded and conflicting; each key ends with one command at most
    {
        shortcuts_dialog::ShortcutsDialog d(w, r);
        d.open();
        d.search->setText("undo");
        app.tick();
        savePng(OUT + "/shortcuts.png", d.framebuffer());
        d.search->setText("");
        std::mt19937 rng(1);
        const int modsOf[] = {0, plat::ModCtrl, plat::ModShift, plat::ModAlt, plat::ModCtrl | plat::ModShift,
                              plat::ModCtrl | plat::ModAlt, plat::ModCtrl | plat::ModAlt | plat::ModShift,
                              plat::ModAlt | plat::ModShift};
        const int keys[] = {'A', 'B', 'K', 'Q', 'Z', '1', '7', plat::Key_F5, plat::Key_F9, plat::Key_Up, plat::Key_PageDown,
                            plat::Key_Insert, plat::Key_Delete, plat::Key_Home};
        int asked = 0, refused = 0;
        d.askReplace = [&](const std::string&) {
            asked++;
            bool yes = rng() % 4 != 0;  // (now and then Cancel)
            if (!yes) refused++;
            return yes;
        };
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; i++) {
            d.list->setCurrentRow((int)(rng() % (unsigned)d.list->count()));
            KeySequence k(keys[rng() % (sizeof keys / sizeof *keys)], modsOf[rng() % 8]);
            d.bind(k);
        }
        double took = seconds(t0);
        printf("  1000 keys bound in %.2f s (%d conflicts, %d cancelled)\n", took, asked, refused);
        CHECK(asked > 100 && refused > 10);
        CHECK(took < 8);
        std::map<std::string, int> uses;
        bool twice = false;
        for (auto& c : r.list())
            if (c.aliasOf.empty())
                for (auto& k : c.keys)
                    if (++uses[k.text()] > 1) twice = true;
        CHECK(!twice);
        // the menus show each entry's first key
        bool menusShow = true;
        for (auto& c : r.list())
            if (c.action && c.aliasOf.empty())
                menusShow = menusShow && (c.keys.empty() ? c.action->shortcut.empty() : commands::sameKey(c.action->shortcut, c.keys[0]));
        CHECK(menusShow);
        // the keymap file reads back the same keys
        Registry again(nullptr);
        CHECK(again.load(r.text(false), "keymap.txt").empty());
        CHECK(again.text(false) == r.text(false));
        // Remove, Reset this, Reset all (asked: yes)
        d.search->setText("View Explorer");
        d.select("View/Explorer");
        CHECK(d.currentId() == "View/Explorer");
        r.setKeys("View/Explorer", {ks("F8"), ks("Ctrl+Shift+F8")});
        d.refill();
        d.keyList->setCurrentRow(0);
        d.remove->click();
        CHECK(commands::keysText(r.keys("View/Explorer")) == "Ctrl+Shift+F8");
        d.resetThis->click();
        CHECK(commands::keysText(r.keys("View/Explorer")) == "Ctrl+E");
        d.askReplace = [](const std::string&) { return true; };
        d.resetAll->click();
        CHECK(commands::keysText(r.keys("File/Save")) == "Ctrl+S" && commands::keysText(r.keys("Transport/Play")) == "Space");
        // a conflict refused leaves both as they were
        d.select("View/Explorer");
        d.askReplace = [](const std::string&) { return false; };
        CHECK(!d.bind(ks("Ctrl+S")));
        CHECK(commands::keysText(r.keys("File/Save")) == "Ctrl+S" && commands::keysText(r.keys("View/Explorer")) == "Ctrl+E");
        // the key capture: Ctrl+Shift+F9 pressed after Add a key…, the window's shortcuts not run
        d.addKey->start();
        tui::sendKey(d, tui::Key{plat::Key_Control, plat::ModCtrl, ""});
        tui::sendKey(d, tui::Key{plat::Key_F9, plat::ModCtrl | plat::ModShift, ""});
        CHECK(commands::keysText(r.keys("View/Explorer")) == "Ctrl+E, Ctrl+Shift+F9");
        d.addKey->start();
        tui::sendKey(d, tui::Key{plat::Key_Escape, 0, ""});  // (cancelled: the dialog stays)
        CHECK(d.isWindowShown() || !d.isRunning());
        CHECK(commands::keysText(r.keys("View/Explorer")) == "Ctrl+E, Ctrl+Shift+F9");
        // a key text consoles can't send is flagged (Ctrl+Shift+P: the palette's, taken from it)
        CHECK(d.keyList->count() == 2 && d.keyList->item(1).text == "Ctrl+Shift+F9" &&
              d.info->text().find("not in the terminal") == std::string::npos);
        d.askReplace = [](const std::string&) { return true; };
        d.addKey->start();
        tui::sendKey(d, tui::Key{'P', plat::ModCtrl | plat::ModShift, ""});
        CHECK(d.keyList->count() == 3 && d.keyList->item(2).text == "Ctrl+Shift+P (not in the terminal)" &&
              d.info->text().find("not in the terminal") != std::string::npos);
        CHECK(r.keys("View/Command palette…").empty());
        // import a damaged file, export
        std::string bad = TMP + "/bad.txt";
        util::writeFile(bad, "Edit/Undo = Ctrl+Alt+U\nbroken\nTransport/Play = Hyper+Q\n");
        d.chooseImport = [&] { return bad; };
        d.importKeymap();
        CHECK(commands::keysText(r.keys("Edit/Undo")) == "Ctrl+Alt+U");
        CHECK(d.info->text().find("2 lines skipped") != std::string::npos && d.info->text().find("bad.txt line 2") != std::string::npos);
        d.chooseExport = [&] { return TMP + "/exported.txt"; };
        d.exportKeymap();
        std::string exported;
        CHECK(util::readFile(TMP + "/exported.txt", exported) && exported.find("Edit/Undo = Ctrl+Alt+U") != std::string::npos);
        r.resetAll();
    }
    app.nestedLoop = nullptr;
    delete w;
}

// --- the terminal -------------------------------------------------------------------------------

void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    tui::Screen screen(110, 40);
    std::vector<tui::Key> queue;
    size_t at = 0;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            if (queue[at].key == plat::Key_Return) {  // (the palette as the terminal shows it)
                f.draw(screen);
                util::writeFile(OUT + "/tui-palette.txt", screen.lines());
            }
            f.key(queue[at++]);
        }
    };
    auto keys = [&](std::vector<tui::Key> k) {
        queue = std::move(k);
        at = 0;
        while (at < queue.size()) f.key(queue[at++]);
        for (int i = 0; i < 4; i++) {
            app.advanceTimers(0.05);
            app.tick();
        }
    };
    auto typed = [](const std::string& s) {
        std::vector<tui::Key> out;
        for (char c : s)
            out.push_back(c == ' ' ? tui::Key{plat::Key_Space, 0, " "}
                                   : tui::Key{c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c, 0, std::string(1, c)});
        return out;
    };
    // Ctrl+P: the palette, typed, Enter runs it
    bool on = w->explorerDock->isVisible();
    std::vector<tui::Key> k = {tui::Key{'P', plat::ModCtrl, ""}};
    for (auto& t : typed("view explorer")) k.push_back(t);
    k.push_back(tui::Key{plat::Key_Return, 0, ""});
    keys(k);
    CHECK(w->explorerDock->isVisible() != on);
    // a rebound key: Explorer on Alt+E... no: Alt+letters open the terminal's menus; on F7 then
    w->commands->setKeys("View/Explorer", {ks("F7")});
    keys({tui::Key{plat::Key_F7, 0, ""}});
    CHECK(w->explorerDock->isVisible() == on);
    keys({tui::Key{'E', plat::ModCtrl, ""}});  // (the old key: nothing)
    CHECK(w->explorerDock->isVisible() == on);
    // Space: Play's key as bound (rebound to P: Space no longer plays)
    int plays = 0;
    w->transport->playAction->triggered.connect([&plays] { plays++; });
    keys({tui::Key{plat::Key_Space, 0, " "}});
    CHECK(plays == 1);
    w->commands->setKeys("Transport/Play", {ks("P")});
    keys({tui::Key{plat::Key_Space, 0, " "}});
    CHECK(plays == 1);
    keys({tui::Key{'P', 0, "p"}});
    CHECK(plays == 2);
    // F1 lists the keys as bound
    keys({tui::Key{plat::Key_F1, 0, ""}});
    f.draw(screen);
    std::string text = screen.lines();
    CHECK(text.find("P                 play / stop") != std::string::npos);
    CHECK(text.find("Ctrl+P            every command") != std::string::npos);
    keys({tui::Key{plat::Key_Escape, 0, ""}});
    w->commands->resetAll();
    app.nestedLoop = nullptr;
    app.paintWindows = true;
    delete w;
}

}  // namespace

int main() {
    setenv("TINYDAW_NO_UPDATE_CHECK", "1", 1);
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    util::makeDirs(OUT);
    settings::setPath(TMP + "/settings.json");
    util::writeFile(TMP + "/keymap.txt", "");
    testBasics();
    testKeymapFile();
    testWindow();
    testTerminal();
    TEST_MAIN_END();
}
