// Worst case for the main window's sizes: the window from 1 x 1 to 7680 x 4320 at every
// interface size; every resizable part at its limits at once (the Explorer, every kind of row at its smallest and
// largest, the timeline zoomed all the way in and out, hundreds of tracks); a menu taller than the screen.
// Fails on: a crash, negative or absurd geometry, a track panel row's controls cut off or squashed, the Explorer's
// chosen width lost after the window was narrow, a menu's items out of reach, a layout taking far too long.
// Pictures of the extremes: build/ui-samples/size-audit/ (look at them).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "config.h"
#include "core/util.h"
#include "gfx/image.h"
#include "layout_audit.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"
#include "ui/lane_panel.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/overview.h"
#include "ui/preferences.h"
#include "ui/widgets.h"
#include "ui/skin.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;

namespace {

const std::string OUT = "build/ui-samples/size-audit";
const int SR = 48000;

void settle(int frames = 2) {
    for (int i = 0; i < frames; i++) {
        App::get().advanceTimers(0.02);
        App::get().tick();
    }
    App::get().doLayouts();
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string fileSafe(std::string s) {
    for (auto& c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '-';
    return s;
}

AudioPtr sine(double seconds) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.resize(n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)(0.5 * std::sin(0.01 * i));
    return a;
}

struct Rig {
    MainWindow* w = nullptr;
    std::vector<TrackPtr> tracks;
    GroupPtr group, inner;
    TrackPtr ret;
};

// audio and MIDI tracks, a group with a nested group, a return, automation and take lanes, the tempo lane
Rig makeMain(int W, int H, int extraTracks = 0) {
    Rig r;
    r.w = new MainWindow();
    r.w->askBeforeClosing = false;
    r.w->resizeWindow(W, H);
    r.w->showWindow();
    Timeline* tl = r.w->timeline;
    Project& p = r.w->project;
    for (int i = 0; i < 8 + extraTracks; i++) {
        TrackPtr t = i < 8 ? tl->addTrack(i % 2 ? Kind::Midi : Kind::Audio) : p.addTrack(i % 2 ? Kind::Midi : Kind::Audio);
        t->name = i == 2 ? "Rhythm guitar (doubled, left and right)" : "Track " + std::to_string(i + 1);
        r.tracks.push_back(t);
        if (i == 3 || i == 5) t->needsInstrument = false;  // (MIDI rows with all their controls: the keyboard focus toggle)
        if (i < 8 && i % 2 == 0) {
            auto c = makeAudioClip("Clip " + std::to_string(i), sine(4), SR, SR);
            c->track = t;
            c->start = (int64_t)i * SR;
            p.addClip(c);
        }
    }
    auto lane = std::make_shared<Automation>();
    lane->slot = r.tracks[0]->controls;
    lane->param = "Volume";
    lane->setPoints({{0, 0.2}, {2, 0.9}});
    r.tracks[0]->automation = std::vector<AutomationPtr>{lane};
    r.tracks[0]->showAutomation = true;
    auto take = makeAudioClip("Take 1", sine(3), SR, SR);
    r.tracks[2]->takes = std::vector<ClipPtr>{take};
    r.tracks[2]->showTakes = true;
    r.group = tl->groupTracks({r.tracks[0], r.tracks[1], r.tracks[2]});
    r.inner = tl->groupTracks({r.tracks[1], r.tracks[2]});
    r.ret = tl->addTrack(Kind::Return);
    r.ret->name = "Reverb return";
    r.w->showTempoLane(true);
    tl->lanesChanged();
    tl->tracksChanged.emit();
    settle();
    return r;
}

void closeMain(Rig& r) {
    r.w->closeFxWindows();
    App::get().closeAllPopups();
    delete r.w;
    r.w = nullptr;
    settle(1);
}

// every widget's geometry sane: no negative size, nothing absurdly far away (an overflow, a NaN cast to int)
int badGeometry(Widget* w, std::string& first) {
    int bad = 0;
    for (auto& up : w->children()) {
        Widget* c = up.get();
        if (!c->isVisible() || dynamic_cast<Window*>(c)) continue;
        Rect g = c->geometry();
        if (g.w < 0 || g.h < 0 || std::abs(g.x) > 1000000 || std::abs(g.y) > 1000000 || g.w > 1000000 || g.h > 4000000) {
            if (!bad) first = audit::pathOf(c) + " " + audit::rectText(g);
            bad++;
        }
        bad += badGeometry(c, first);
    }
    return bad;
}

void dump(Widget* w, int depth = 0) {
    Rect g = w->geometry();
    Point h = w->sizeHint(), m = w->minimumSizeHint();
    printf("%*s%s %s  hint %dx%d min %dx%d%s\n", depth * 2, "", audit::describe(w).c_str(), audit::rectText(g).c_str(), h.x,
           h.y, m.x, m.y, w->isVisible() ? "" : " (hidden)");
    for (auto& c : w->children()) dump(c.get(), depth + 1);
}

void collectFrames(Widget* w, std::vector<LaneFrame*>& out) {
    for (auto& up : w->children()) {
        Widget* c = up.get();
        if (!c->isVisible() || dynamic_cast<Window*>(c)) continue;
        if (auto* f = dynamic_cast<LaneFrame*>(c)) out.push_back(f);
        collectFrames(c, out);
    }
}

int failures = 0;  // (the first contexts' problems printed)

// the track panel's rows and the master's: their controls inside them, none squashed or overlapping, none cut
int rowProblems(MainWindow& w, const std::string& ctx) {
    std::vector<LaneFrame*> frames;
    collectFrames(&w, frames);
    audit::Report rep;
    rep.context = ctx;
    audit::Options o;
    o.text = false;  // (names are elided with …, as meant)
    for (LaneFrame* f : frames) {
        Rect in = f->contentsRect();
        if (f->layout() && f->layout()->minimumSize().y > in.h)
            rep.add("row-too-low", audit::pathOf(f), audit::describe(f),
                    "its controls need " + std::to_string(f->layout()->minimumSize().y) + " px, it has " + std::to_string(in.h) + " (" + audit::rectText(f->geometry()) + ", current " + std::to_string(f->isCurrent()) + ", " + std::to_string(f->controlsHidden()) + " controls left out)");
        audit::checkTree(f, rep, o);
    }
    if (rep.size() && failures++ < 12) printf("%s: track panel rows:\n%s", ctx.c_str(), rep.text().c_str());
    return rep.size();
}

// rows at their default height that had to leave controls out (they all fit there)
int shedAtDefault(MainWindow& w, const std::string& ctx) {
    std::vector<LaneFrame*> frames;
    collectFrames(&w, frames);
    int n = 0;
    for (LaneFrame* f : frames)
        if (f->controlsHidden()) {
            printf("%s: %s left %zu controls out at %d px\n", ctx.c_str(), audit::pathOf(f).c_str(), f->controlsHidden(),
                   f->height());
            n++;
        }
    return n;
}

// the window at one size: laid out in time, sane geometry, whole rows; a picture when asked
void checkAt(Rig& r, int W, int H, const std::string& ctx, bool picture) {
    auto t0 = std::chrono::steady_clock::now();
    r.w->resizeWindow(W, H);
    App::get().doLayouts();
    double ms = msSince(t0);
    settle(1);
    std::string first;
    int bad = badGeometry(r.w, first);
    if (bad) printf("%s: %d widgets with broken geometry, the first %s\n", ctx.c_str(), bad, first.c_str());
    CHECK(bad == 0);
    CHECK(rowProblems(*r.w, ctx) == 0);
    // at least as large as the window may be made (the window manager is told leastSize()): its menu bar,
    // transport, body and status bar all inside it
    Point least = r.w->leastSize();
    if (W >= least.x && H >= least.y)
        for (auto& up : r.w->children()) {
            Widget* c = up.get();
            if (!c->isVisible() || dynamic_cast<Window*>(c) || c->inFrame()) continue;
            Rect g = c->geometry();
            if (g.intersected(r.w->rect()) != g) {
                printf("%s: %s at %s, outside the window\n", ctx.c_str(), audit::describe(c).c_str(), audit::rectText(g).c_str());
                CHECK(false);
            }
        }
    // (a layout of the whole window: well under a second even at 7680 x 4320 on a slow machine)
    if (ms > 500) printf("%s: the layout took %.0f ms\n", ctx.c_str(), ms);
    CHECK(ms < 500);
    if (picture) {
        r.w->update();
        App::get().tick();
        savePng(OUT + "/" + fileSafe(ctx) + ".png", r.w->framebuffer());
    }
}

// every track's, group bus's, return's and automation lane's height: its smallest (-1), default (0), largest (1);
// an audio track current (its outline takes a pixel of the row)
void setAllHeights(Rig& r, int which) {
    Timeline* tl = r.w->timeline;
    // (each set as a drag would, but the lanes laid out once: setTrackHeight on the last one)
    for (auto& t : r.w->project.tracks) {
        int least = t->kind == Kind::Return ? config::RETURN_MIN_H : config::MIN_TRACK_H;
        t->height = which < 0 ? least : which > 0 ? config::MAX_TRACK_H : t->kind == Kind::Return ? config::RETURN_H : config::TRACK_H;
        for (auto& a : *t->automation)
            a->height = which < 0 ? config::AUTOMATION_MIN_H : which > 0 ? config::AUTOMATION_MAX_H : config::AUTOMATION_H;
    }
    TrackPtr last = r.w->project.tracks[r.w->project.tracks.size() - 1];
    tl->setTrackHeight(*last, last->height);
    tl->lanesChanged();
    r.w->panel->select(r.tracks[0]);
    settle();
}

// --- 1. the window from 1 x 1 to 7680 x 4320 (device pixels) at every interface size; every part at its limits ---
void testWindowSizes() {
    for (int size : prefs::interfaceSizes()) {
        float s = size / 100.f;
        App::get().setScale(s);
        bool pictures = size == 75 || size == 100 || size == 150 || size == 200;
        Rig r = makeMain(1280, 800);
        App::get().doLayouts();
        Point mn = r.w->layout()->minimumSize();
        std::string at = " at " + std::to_string(size) + "%";
        int W8 = (int)(7680 / s), H8 = (int)(4320 / s);
        for (auto [W, H] : std::vector<std::pair<int, int>>{{1, 1}, {320, 240}, {640, 480}, {mn.x, mn.y}, {1280, 800},
                                                            {1920, 1080}, {(int)(3840 / s), (int)(2160 / s)}, {W8, H8},
                                                            {mn.x, H8}, {W8, mn.y}}) {
            bool big = W * s > 7000 || H * s > 4000;  // (8K: one picture, the whole 7680 x 4320 at 100 %: 130 MB to write)
            bool shoot = pictures && (W == mn.x || W == 640 || W * s >= 3800 || (W == 1280 && size == 100));
            checkAt(r, W, H, "window " + std::to_string(W) + "x" + std::to_string(H) + at, shoot && (!big || (size == 100 && W == W8 && H == H8)));
        }
        // every part at its limits at once, in the smallest window and the largest
        r.w->setExplorerWidth(100000);
        setAllHeights(r, -1);
        r.w->timeline->zoom(1e-9, 0);
        r.w->overview->resizedTo.emit(r.w->overview->setHeight(100000));  // (its bottom edge dragged down all the way)
        App::get().doLayouts();
        Point big = r.w->leastSize();  // (the window may have to be taller now; the Explorer narrows with it)
        checkAt(r, big.x, big.y, "smallest window, rows smallest, zoomed out, Explorer widest, overview tallest" + at, pictures);
        r.w->overview->resizedTo.emit(r.w->overview->setHeight(0));
        setAllHeights(r, 1);
        r.w->timeline->zoom(1e9, 0);
        r.w->setExplorerWidth(0);
        checkAt(r, (int)(3840 / s), (int)(2160 / s), "4K window, rows largest, zoomed in, Explorer narrowest" + at, pictures);
        setAllHeights(r, -1);
        checkAt(r, 1280, 800, "rows smallest" + at, pictures);
        setAllHeights(r, 0);
        checkAt(r, 1280, 800, "rows default" + at, false);
        CHECK(shedAtDefault(*r.w, "rows default" + at) == 0);
        closeMain(r);
    }
    App::get().setScale(1);
}

// --- 2. every skin: the rows whole at their smallest, nothing left out at the default height ---
void testSkins() {
    Rig r = makeMain(1280, 800);
    for (const skin::Skin& sk : skin::library()) {
        r.w->chooseSkin(sk);
        settle();
        // the transport's number fields: their text whole inside the field (its editor's padding, not under the ▴▾)
        for (SpinBox* b : {r.w->transport->bpmBox, r.w->transport->signatureBox}) {
            LineEdit* e = b->editor();
            int need = ui::style::textWidth(e->font(), e->text());
            int room = e->width() - ui::style::insets(e->computedStyle(), "padding").w();
            if (need > room) printf("%s: \"%s\" needs %d px in its field, has %d\n", sk.name.c_str(), e->text().c_str(), need, room);
            CHECK(need <= room);
        }
        setAllHeights(r, -1);
        checkAt(r, 1280, 800, "rows smallest in " + sk.name, true);
        setAllHeights(r, 0);
        checkAt(r, 1280, 800, "rows default in " + sk.name, true);
        CHECK(shedAtDefault(*r.w, "rows default in " + sk.name) == 0);
    }
    r.w->chooseSkin(skin::find("Dark"));
    closeMain(r);
}

// the Explorer's own controls whole (none cut, none squashed)
int explorerProblems(MainWindow& w, const std::string& ctx) {
    audit::Report rep;
    rep.context = ctx;
    audit::Options o;
    o.text = false;
    o.ignore = {"ComboBox"};  // (the folder list: narrower than its longest folder on purpose, its text cut with "…")
    App::get().doLayouts();
    audit::checkTree(w.explorerDock, rep, o);
    if (rep.size()) printf("%s: the Explorer:\n%s", ctx.c_str(), rep.text().c_str());
    return rep.size();
}

// --- 3. the Explorer dragged to its limits, the window narrowed under it (as far as 1 x 1) and widened again ---
void testExplorer() {
    for (int size : {75, 100, 200}) {
        App::get().setScale(size / 100.f);
        Rig r = makeMain(1600, 900);
        MainWindow& w = *r.w;
        std::string at = " at " + std::to_string(size) + "%";
        w.setExplorerWidth(0);
        settle();
        CHECK(explorerProblems(w, "Explorer narrowest" + at) == 0);
        int leastBefore = w.leastSize().x;
        w.setExplorerWidth(100000);
        settle();
        CHECK(explorerProblems(w, "Explorer widest" + at) == 0);
        CHECK(w.explorerWidth <= w.width() - lane_panel::panelWidth() - 200);
        if (w.explorerDock->width() != w.explorerWidth) printf("Explorer: %d wide, chosen %d, window %d%s\n", w.explorerDock->width(), w.explorerWidth, w.width(), at.c_str());
        CHECK(w.explorerDock->width() == w.explorerWidth);
        // (the window may still be made as narrow as before: a maximized window restored, a tiling window manager)
        CHECK(w.leastSize().x == leastBefore);
        // the width dragged to, kept through any narrower window (a tiling window manager, a maximize undone)
        w.setExplorerWidth(420);
        int chosen = w.explorerWidth;
        for (auto [W, H] : std::vector<std::pair<int, int>>{{900, 700}, {600, 400}, {1, 1}, {1280, 800}, {3000, 1500}, {1600, 900}}) {
            w.resizeWindow(W, H);
            settle(1);
        }
        if (w.explorerWidth != chosen) printf("Explorer: dragged to %d, %d after the window was narrow%s\n", chosen, w.explorerWidth, at.c_str());
        CHECK(w.explorerWidth == chosen);
        // hidden and shown again, the skin changed: still that wide
        w.setExplorerVisible(false);
        w.resizeWindow(700, 500);
        settle(1);
        w.setExplorerVisible(true);
        w.resizeWindow(1600, 900);
        w.chooseSkin(skin::find("Light"));
        w.chooseSkin(skin::find("Dark"));
        settle(1);
        CHECK(w.explorerWidth == chosen);
        closeMain(r);
    }
    App::get().setScale(1);
}

// --- 4. a menu taller than the screen (headless: 1920 x 1080): it fits the screen and scrolls to every item ---
void testTallMenu() {
    Rig r = makeMain(1280, 800);
    Menu menu;
    int chosen = -1;
    for (int i = 0; i < 300; i++) menu.addAction("Item " + std::to_string(i), [&chosen, i] { chosen = i; });
    Menu* sub = menu.addMenu("More");
    for (int i = 0; i < 300; i++) sub->addAction("Sub item " + std::to_string(i));
    Rect screen = App::get().screenAt(Point(0, 0));
    for (Point at : {Point(10, 10), Point(500, 900), Point(1900, 1070)}) {
        menu.popup(at, r.w);
        CHECK(menu.isOpen());
        Window* p = App::get().popups().back();
        Rect g = p->screenGeometry();
        if (g.h > screen.h || g.y < screen.y || g.bottom() > screen.bottom())
            printf("a 300-item menu at %d,%d: %s on a screen of %s\n", at.x, at.y, audit::rectText(g).c_str(),
                   audit::rectText(screen).c_str());
        CHECK(g.h <= screen.h && g.y >= screen.y && g.bottom() <= screen.bottom());
        menu.close();
    }
    // End: the last item chosen with the keys (scrolled into view), Return picks it
    menu.popup(Point(10, 10), r.w);
    Window* p = App::get().popups().back();
    plat::Event k{};
    k.type = plat::Event::KeyDown;
    k.key = plat::Key_Up;  // (from the top, Up wraps to the last: More)
    r.w->onEvent(nullptr, k);
    k.key = plat::Key_Up;
    r.w->onEvent(nullptr, k);  // Item 299
    k.key = plat::Key_Return;
    r.w->onEvent(nullptr, k);
    App::get().advanceTimers(0.001);
    CHECK(chosen == 299);
    // the wheel: scrolled to the bottom, the last item is under the pointer near the bottom edge, and a click there
    // chooses it
    chosen = -1;
    menu.popup(Point(10, 10), r.w);
    p = App::get().popups().back();
    for (int i = 0; i < 400; i++) {
        plat::Event we{};
        we.type = plat::Event::Wheel;
        we.x = 40;
        we.y = 200;
        we.dy = -1;
        p->onEvent(nullptr, we);
    }
    p->update();
    App::get().tick();
    savePng(OUT + "/menu-300-items-scrolled-to-the-end.png", p->framebuffer());
    // an item just above the bottom arrows' strip, under the pointer: Return chooses it (one of the last)
    plat::Event m{};
    m.type = plat::Event::MouseMove;
    m.x = 40;
    m.y = p->height() - 60;
    p->onEvent(nullptr, m);
    k.key = plat::Key_Return;
    r.w->onEvent(nullptr, k);
    App::get().advanceTimers(0.001);
    if (chosen < 295) printf("the menu scrolled to its end chose item %d above its bottom\n", chosen);
    CHECK(chosen >= 295);
    // and a click there chooses it too
    int before = chosen;
    chosen = -1;
    menu.popup(Point(10, 10), r.w);
    p = App::get().popups().back();
    for (int i = 0; i < 400; i++) {
        plat::Event we{};
        we.type = plat::Event::Wheel;
        we.x = 40;
        we.y = 200;
        we.dy = -1;
        p->onEvent(nullptr, we);
    }
    m.type = plat::Event::MouseMove;
    p->onEvent(nullptr, m);
    m.type = plat::Event::MouseDown;
    m.button = plat::LeftButton;
    p->onEvent(nullptr, m);
    m.type = plat::Event::MouseUp;
    p->onEvent(nullptr, m);
    App::get().advanceTimers(0.001);
    CHECK(chosen == before);
    closeMain(r);
}

// --- 5. hundreds of tracks at their smallest and largest: laid out in time, scrolled to the end ---
void testManyTracks() {
    auto t0 = std::chrono::steady_clock::now();
    Rig r = makeMain(1920, 1080, 392);  // (400 tracks)
    printf("400 tracks: made in %.0f ms\n", msSince(t0));
    for (int which : {-1, 1}) {
        t0 = std::chrono::steady_clock::now();
        setAllHeights(r, which);
        printf("400 tracks: every height set in %.0f ms\n", msSince(t0));
        r.w->panelScroll->verticalScrollBar()->setValue(1 << 30);
        checkAt(r, 1920, 1080, std::string("400 tracks ") + (which < 0 ? "smallest" : "largest") + ", scrolled to the end", true);
        // the rows are as tall as their lanes: the panel and the timeline scroll together to the same end
        CHECK(r.w->panelScroll->verticalScrollBar()->maximum() == r.w->scroll->verticalScrollBar()->maximum());
    }
    // a track's height dragged: each step of the drag (the lanes and rows laid out, the window painted) in time
    double worst = 0;
    for (int step = 0; step < 10; step++) {
        auto t0 = std::chrono::steady_clock::now();
        r.w->timeline->setTrackHeight(*r.tracks[3], config::MIN_TRACK_H + step * 20);
        App::get().doLayouts();
        r.w->flush();
        worst = std::max(worst, msSince(t0));
    }
    printf("400 tracks: a step of a row's height drag takes at most %.1f ms\n", worst);
    CHECK(worst < 50);  // (was 250-400 ms with a VBox of rows; a slow machine still well under a visible stall)
    closeMain(r);
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    std::filesystem::remove_all(OUT);  // (a fresh start: no skin or settings from the last run)
    skin::setBuiltinDir("data/skins");
    paths::setDataDir(util::absolutePath(OUT + "/tmp/data"));
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT + "/tmp");
    settings::setPath(OUT + "/tmp/settings.json");

    if (getenv("DUMP")) {  // (DUMP=1 SKIN=XP: the rows' geometry, to see what doesn't fit)
        Rig r = makeMain(1280, 800);
        if (getenv("SKIN")) r.w->chooseSkin(skin::find(getenv("SKIN")));
        r.w->panel->select(r.tracks[0]);
        for (int h : {72, 60}) {
            for (auto& t : r.tracks) r.w->timeline->setTrackHeight(*t, h);
            settle();
            std::vector<LaneFrame*> fs;
            collectFrames(r.w, fs);
            for (LaneFrame* f : fs)
                printf("frame %s contents %s current %d hidden %zu\n", audit::rectText(f->geometry()).c_str(),
                       audit::rectText(f->contentsRect()).c_str(), (int)f->isCurrent(), f->controlsHidden());
            printf("=== rows at %d\n", h);
            dump(r.w->panel);
        }
        closeMain(r);
        return 0;
    }
    util::makeDirs(OUT);
    // (ONLY=Explorer: that part alone, while working on it)
    auto run = [](const char* name, void (*fn)()) {
        if (!getenv("ONLY") || strstr(name, getenv("ONLY"))) fn();
    };
    run("WindowSizes", testWindowSizes);
    run("Skins", testSkins);
    run("Explorer", testExplorer);
    run("TallMenu", testTallMenu);
    run("ManyTracks", testManyTracks);
    TEST_MAIN_END();
}
