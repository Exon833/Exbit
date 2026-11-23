// Adding tracks from the track panel (lane_panel.h ADDING TRACKS), headless over the main window: the transport has
// no track buttons (Add file stays), the panel's right-click menus (empty space, a track's row, a group's, an
// automation row) add an audio, MIDI or return track where they say, Ctrl+T / Ctrl+Shift+T / Ctrl+Alt+T and the
// commands still add them, the empty panel's hint, the terminal's keys (A, N, F2 on a row and on the master) and a
// worst case (hundreds added at random rows). Pictures in build/ui-samples/keyboard-focus/.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/commands.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/transport_bar.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/keyboard-focus";
const std::string TMP = OUT + "/tmp-add";

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    MainWindow* w;
    Front* f;
    Screen screen{120, 34};
    void keys(const std::vector<Key>& ks) {
        for (const Key& k : ks) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(k);
        }
        settle();
    }
    void settle() {
        for (int i = 0; i < 6; i++) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    bool shows(const std::string& t) const { return screen.lines().find(t) != std::string::npos; }
};

Image paintWidget(ui::Widget* w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    w->paintTree(p, w->rect());
    return img;
}

std::vector<std::string> texts(const ui::Menu& m) {
    std::vector<std::string> out;
    for (auto& a : m.actions()) out.push_back(a->separator ? "---" : a->text);
    return out;
}

ui::Action* find(const ui::Menu& m, const std::string& text) {
    for (auto& a : m.actions())
        if (a->text == text) return a.get();
    return nullptr;
}

// the Add entries of a menu: its "Add track" submenu, or the menu itself (a section)
ui::Menu* addEntries(ui::Menu& m) {
    ui::Action* sub = find(m, "Add track");
    return sub && sub->submenu ? sub->submenu : &m;
}

bool choose(ui::Menu& m, const std::string& text) {
    ui::Action* a = find(*addEntries(m), text);
    if (!a || !a->triggered) return false;
    a->triggered();
    return true;
}

int indexOf(const Project& p, const TrackPtr& t) { return p.tracks.indexOf(t); }

bool returnsLast(const Project& p) {  // (the returns all at the bottom)
    bool seen = false;
    for (auto& t : p.tracks) {
        if (t->kind == Kind::Return) seen = true;
        else if (seen) return false;
    }
    return true;
}

// any button on the bar showing a track's "+ …"
bool trackButtonShown(TransportBar* bar) {
    std::function<bool(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            auto* b = dynamic_cast<ui::Button*>(c.get());
            if (b && b->isVisible() && b->window() && b->text().rfind("+ ", 0) == 0) return true;
            if (c->isVisible() && walk(c.get())) return true;
        }
        return false;
    };
    return walk(bar);
}

void testToolbar(MainWindow& w) {
    TransportBar* bar = w.transport;
    for (int width : {1800, 1200, 900, 700, 500, 300}) {
        bar->setGeometry(0, 0, width, bar->sizeHint().y);
        ui::App::get().doLayouts();
        CHECK(!trackButtonShown(bar));
        CHECK(!bar->addAudioTrackAction->button->isVisible() && !bar->addMidiTrackAction->button->isVisible() &&
              !bar->addReturnTrackAction->button->isVisible());
        ui::Menu more;
        bar->fillMoreMenu(more);
        CHECK(!find(more, "Audio track") && !find(more, "MIDI track") && !find(more, "Return track"));
        if (width >= 900) CHECK(bar->addFileAction->button->isVisible());  // (Add file stays)
        if (!bar->addFileAction->button->isVisible()) CHECK(find(more, "Add file…") != nullptr);
    }
    bar->setGeometry(0, 0, 1280, bar->sizeHint().y);
    ui::App::get().doLayouts();
    util::makeDirs(OUT);
    savePng(OUT + "/transport.png", paintWidget(bar));
}

void testMenus(MainWindow& w, Driver& d) {
    Project& p = w.project;
    LanePanel* panel = w.panel;
    // an empty panel says how to add a track
    CHECK(p.tracks.empty());
    d.settle();
    Image empty = paintWidget(panel);
    savePng(OUT + "/empty-panel.png", empty);
    ::Color bg = empty.colorAt(panel->width() - 3, 60);
    int inked = 0;  // (the hint's letters, near the top)
    for (int y = 10; y < 34; y++)
        for (int x = 0; x < panel->width(); x++) inked += empty.colorAt(x, y) != bg;
    CHECK(inked > 50);

    // empty space: the Add entries as a section, with the transport's keys; at the end
    ui::Menu space;
    panel->fillAddMenu(space);
    CHECK((texts(space) == std::vector<std::string>{"Add a track", "Audio track", "MIDI track", "Return track"}));
    CHECK(find(space, "Audio track")->shortcut.text() == "Ctrl+T");
    CHECK(find(space, "MIDI track")->shortcut.text() == "Ctrl+Shift+T");
    CHECK(find(space, "Return track")->shortcut.text() == "Ctrl+Alt+T");
    CHECK(choose(space, "Audio track"));
    CHECK(p.tracks.size() == 1 && p.tracks[0]->kind == Kind::Audio);
    TrackPtr a1 = p.tracks[0];
    d.settle();
    Image one = paintWidget(panel);
    int inkedAfter = 0;  // (the hint gone: the row there instead)
    for (int y = 10; y < 34; y++)
        for (int x = 0; x < panel->width(); x++) inkedAfter += one.colorAt(x, y) == empty.colorAt(x, y) ? 0 : 1;
    CHECK(inkedAfter > 0);
    ui::Menu space2;
    panel->fillAddMenu(space2);
    CHECK(choose(space2, "MIDI track") && p.tracks.size() == 2 && p.tracks[1]->kind == Kind::Midi);
    TrackPtr m1 = p.tracks[1];
    ui::Menu space3;
    panel->fillAddMenu(space3);
    CHECK(choose(space3, "Return track") && p.tracks[p.tracks.size() - 1]->kind == Kind::Return);
    TrackPtr r1 = p.tracks[p.tracks.size() - 1];

    // a track's row: its own menu keeps its entries, with "Add track ▸" (below it)
    ui::Menu row;
    panel->fillTrackMenu(row, a1);
    auto rt = texts(row);
    CHECK(rt.size() > 6 && rt[0] == "Rename track…" && find(row, "Duplicate track") && find(row, "Track colour…"));
    CHECK(find(row, "Add track") && find(row, "Add track")->submenu);
    CHECK((texts(*addEntries(row)) == std::vector<std::string>{"Audio track", "MIDI track", "Return track"}));
    CHECK(choose(row, "MIDI track"));
    CHECK(p.tracks.size() == 4 && indexOf(p, a1) == 0 && p.tracks[1]->kind == Kind::Midi && p.tracks[1] != m1);
    TrackPtr m2 = p.tracks[1];
    ui::Menu row2;
    panel->fillTrackMenu(row2, a1);
    CHECK(choose(row2, "Audio track") && p.tracks[1]->kind == Kind::Audio && indexOf(p, m2) == 2);
    // below a return: above the returns; a return from any row: at the very bottom
    ui::Menu ret;
    panel->fillTrackMenu(ret, r1);
    CHECK(find(ret, "Add track") != nullptr);
    CHECK(choose(ret, "Audio track") && p.tracks[p.tracks.size() - 1] == r1 && returnsLast(p));
    ui::Menu ret2;
    panel->fillTrackMenu(ret2, a1);
    CHECK(choose(ret2, "Return track") && p.tracks[p.tracks.size() - 1]->kind == Kind::Return && p.tracks[p.tracks.size() - 1] != r1);
    CHECK(returnsLast(p));

    // a group's row: in the group, below its last track
    GroupPtr g = p.groupTracks({a1, p.tracks[1]});
    w.timeline->tracksRegrouped();
    panel->rebuild();
    std::vector<TrackPtr> members = p.members(g);
    CHECK(members.size() == 2);
    ui::Menu gm;
    panel->fillGroupMenu(gm, g);
    CHECK(find(gm, "Add track") != nullptr && find(gm, "Rename group…") && find(gm, "Ungroup (the tracks stay)"));
    CHECK(choose(gm, "MIDI track"));
    members = p.members(g);
    CHECK(members.size() == 3 && members.back()->kind == Kind::Midi && members.back()->group == g);
    // a grouped track's row: the new one in its group, right below it
    ui::Menu gr;
    panel->fillTrackMenu(gr, a1);
    CHECK(choose(gr, "Audio track"));
    CHECK(p.tracks[indexOf(p, a1) + 1]->group == g && p.members(g).size() == 4);

    // an automation row: its menu is the Add section, below its track
    TrackPtr belowM2 = panel->addTrack(Kind::Audio, m2);
    CHECK(belowM2 && indexOf(p, belowM2) == indexOf(p, m2) + 1);
    // (a track that is gone: at the end, no crash)
    TrackPtr gone = std::make_shared<Track>("Gone", Kind::Audio);
    size_t before = p.tracks.size();
    CHECK(panel->addTrack(Kind::Midi, gone) && p.tracks.size() == before + 1 && returnsLast(p));
    panel->rebuild();
    CHECK(panel->frames.size() == p.tracks.size());
    // a picture of the row's menu, open
    ui::Menu shown;
    panel->fillTrackMenu(shown, m1);
    shown.popup(panel->mapToGlobal(Point(20, 40)), panel);
    ui::App::get().doLayouts();
    if (!ui::App::get().popups().empty()) savePng(OUT + "/track-menu.png", paintWidget(ui::App::get().popups().back()));
    shown.close();
    ui::Menu sub;
    panel->fillAddMenu(sub, m1, nullptr, true);
    if (ui::Menu* s = addEntries(sub)) {
        s->popup(panel->mapToGlobal(Point(20, 40)), panel);
        ui::App::get().doLayouts();
        if (!ui::App::get().popups().empty()) savePng(OUT + "/add-submenu.png", paintWidget(ui::App::get().popups().back()));
        s->close();
    }
    savePng(OUT + "/panel-added.png", paintWidget(panel));
}

void testShortcuts(MainWindow& w, Driver& d) {
    Project& p = w.project;
    size_t n = p.tracks.size();
    // the window's keys (through the terminal, as typed into the window)
    d.keys({key('T', plat::ModCtrl)});
    CHECK(p.tracks.size() == n + 1);
    CHECK(std::count_if(p.tracks.begin(), p.tracks.end(), [](const TrackPtr& t) { return t->kind == Kind::Audio; }) > 0);
    d.keys({key('T', plat::ModCtrl | plat::ModShift)});
    CHECK(p.tracks.size() == n + 2);
    d.keys({key('T', plat::ModCtrl | plat::ModAlt)});
    CHECK(p.tracks.size() == n + 3 && p.tracks[p.tracks.size() - 1]->kind == Kind::Return && returnsLast(p));
    // the commands (the palette's entries)
    CHECK(w.commands->run("Transport/Add an audio track") && p.tracks.size() == n + 4);
    CHECK(w.commands->run("Transport/Add a MIDI track") && p.tracks.size() == n + 5);
    CHECK(w.commands->run("Transport/Add a return track") && p.tracks.size() == n + 6 && returnsLast(p));
    // the shortcuts as the transport's actions have them (the menus show these)
    CHECK(w.transport->addAudioTrackAction->shortcut().text() == "Ctrl+T");
    CHECK(w.transport->addMidiTrackAction->shortcut().text() == "Ctrl+Shift+T");
    CHECK(w.transport->addReturnTrackAction->shortcut().text() == "Ctrl+Alt+T");
}

void testTerminal(MainWindow& w, Front& f, Driver& d) {
    Project& p = w.project;
    // the track panel: A / N below the current track, as before
    while (f.pane != Front::Pane::Panel) d.keys({key(plat::Key_Tab)});
    TrackPtr plain;  // (an ungrouped track: the new ones go right below it)
    for (auto& t : p.tracks)
        if (!t->group && t->kind != Kind::Return) plain = t;
    f.setCurrent(plain);
    d.settle();
    size_t n = p.tracks.size();
    d.keys({key('A', 0, "a")});
    CHECK(p.tracks.size() == n + 1 && p.tracks[indexOf(p, plain) + 1]->kind == Kind::Audio);
    TrackPtr madeA = p.tracks[indexOf(p, plain) + 1];
    d.keys({key('N', 0, "n")});
    CHECK(p.tracks.size() == n + 2 && p.tracks[indexOf(p, madeA) + 1]->kind == Kind::Midi);
    // F2 on a track's row: its menu, with Add track ▸
    f.setCurrent(p.tracks[0]);
    d.keys({key(plat::Key_F2)});
    CHECK(f.menuOpen() && find(*f.open[0].menu, "Add track") != nullptr);
    CHECK(find(*f.open[0].menu, "Rename track…") != nullptr);
    d.keys({key(plat::Key_Escape)});
    CHECK(!f.menuOpen());
    // F2 on the master's row: the Add entries; Enter on the first adds an audio track at the end (above the returns)
    f.laneRow = 1 << 20;  // (past the last lane: the master's row)
    d.keys({key(plat::Key_F2)});
    CHECK(f.menuOpen() && find(*f.open[0].menu, "Audio track") && find(*f.open[0].menu, "Return track"));
    size_t before = p.tracks.size();
    d.keys({key(plat::Key_Return)});
    CHECK(p.tracks.size() == before + 1 && returnsLast(p));
    // the keys' list and the status line name them
    d.keys({key(plat::Key_F1)});
    std::string help;
    for (int i = 0; i < 200 && !d.shows("Add track (audio, MIDI, return: below it)"); i++) d.keys({key(plat::Key_Down)});
    CHECK(d.shows("Add track (audio, MIDI, return: below it)"));
    d.keys({key(plat::Key_Escape)});
    CHECK(!f.helpShown);
}

// --- the worst case: hundreds of tracks added from random rows' menus, removed, undone ---
void testWorst(MainWindow& w) {
    Project& p = w.project;
    LanePanel* panel = w.panel;
    std::mt19937 rng(7);
    const char* kinds[] = {"Audio track", "MIDI track", "Return track"};
    for (int i = 0; i < 300; i++) {
        ui::Menu m;
        int pick = (int)(rng() % (p.tracks.size() + 1));
        if (pick == (int)p.tracks.size()) panel->fillAddMenu(m);
        else panel->fillTrackMenu(m, p.tracks[(size_t)pick]);
        CHECK(choose(m, kinds[rng() % 3]));
        if (i % 37 == 0 && !p.tracks.empty()) {  // (one removed now and then; the rows rebuilt)
            TrackPtr t = p.tracks[(size_t)(rng() % p.tracks.size())];
            if (p.clips.empty()) w.timeline->removeTrack(t);
        }
        if (i % 50 == 0) ui::App::get().tick();
    }
    ui::App::get().collectGarbage();
    panel->rebuild();
    CHECK(returnsLast(p));
    CHECK(panel->frames.size() == p.tracks.size());
    std::printf("worst case: %zu tracks after 300 adds from random rows\n", p.tracks.size());
}

}  // namespace

int main() {
    util::setDebug(false);
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    util::removeFile(TMP + "/settings.json");
    settings::setPath(TMP + "/settings.json");
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{w, &f};
    app.nestedLoop = [&d](const std::function<bool()>& done) {
        while (!done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            d.f->key(Key{plat::Key_Escape});  // (no dialog is expected: closed)
        }
    };
    d.settle();
    testToolbar(*w);
    testMenus(*w, d);
    testShortcuts(*w, d);
    testTerminal(*w, f, d);
    testWorst(*w);
    TEST_MAIN_END();
}
