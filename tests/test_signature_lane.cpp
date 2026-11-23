// The time-signature lane (ui/timeline_signature.cpp) and the rows beside the tempo and time-signature lanes
// (ui/timing_rows.cpp): a signature added by clicking a bar (typed in its dialog), dragged along the bars (bar 1's
// stays; never onto another), typed by a double-click, removed from its menu or its dialog; every edit one undo
// step that follows the Timebase; saved and loaded; 7/8 and 6/8 blocks drawn as long as their bars. The main
// window's rows (their ✕ and the View menu's toggles), the terminal's keys (Shift+S, /, Shift+Del), and the worst
// case: a signature change at every bar for 500 bars, painted zoomed out and in and dragged.
// Pictures (CLAUDE.md rule 18): build/ui-samples/signature-lane/.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/cpu_monitor.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/skin.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/timing_rows.h"
#include "ui/paint_helpers.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"

namespace {

const int SR = 48000;
const std::string OUT = "build/ui-samples/signature-lane";

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

ui::MouseEvent mouse(double x, double y, int button = plat::LeftButton, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point((int)x, (int)y);
    e.globalPos = e.pos;
    e.button = button;
    e.buttons = button;
    e.mods = mods;
    return e;
}

struct Rig {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    Timeline* tl;
    Rig(int w = 1200, int h = 300) {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(w, h);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        tl->pps = 60;
        tl->relayout();
    }
};

Image paintScroll(ui::Widget& w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w.width()), std::max(1, w.height()), theme::c("window"));
    Painter p(img);
    w.paintTree(p, w.rect());
    return img;
}

void click(Timeline& tl, double x, double y) {
    ui::MouseEvent e = mouse(x, y);
    tl.mousePress(e);
    tl.mouseRelease(e);
}

void dragFromTo(Timeline& tl, double x0, double y0, double x1, double y1) {
    ui::MouseEvent e = mouse(x0, y0);
    tl.mousePress(e);
    for (int k = 1; k <= 8; k++) {
        ui::MouseEvent m = mouse(x0 + (x1 - x0) * k / 8, y0 + (y1 - y0) * k / 8);
        tl.mouseMove(m);
    }
    ui::MouseEvent up = mouse(x1, y1);
    tl.mouseRelease(up);
}

int colorDistance(Color a, Color b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

void testLane() {
    Rig r;
    Timeline& tl = *r.tl;
    Project& p = r.project;
    Timing& t = p.timing;
    lanes::setTempoLaneShown(true);
    lanes::setSignatureLaneShown(true);
    tl.relayout();
    int top = lanes::signatureLaneTop(), h = lanes::signatureLaneHeight();
    CHECK(top == lanes::rulerHeight() + lanes::tempoLaneHeight() && h == config::SIGNATURE_LANE_H);
    CHECK(lanes::lanesTop() == top + h && tl.lanes()[0].top == lanes::lanesTop());
    CHECK(tl.inSignatureLane(top) && tl.inSignatureLane(top + h - 1) && !tl.inSignatureLane(top - 1) && !tl.inSignatureLane(top + h));
    CHECK(!tl.inTempoLane(top));
    double mid = top + h / 2.0;
    // a MIDI clip on beat 16 (bar 5 in 4/4): it keeps its beat (Timebase: Beats)
    auto midi = p.addTrack(Kind::Midi);
    midi->needsInstrument = false;
    ClipPtr n = makeMidiClip("n", {{0.0, 0.5, 60, 100}}, 1.0, SR);
    n->track = midi;
    n->start = SR * 8;
    n->prepare(SR);
    p.addClip(n);
    history::History hist(p, SR);
    int timings = 0;
    tl.timingChanged.connect([&] { ++timings; });
    // click an empty bar (3): its dialog with the signature in force, typed 7/8
    bool asked = false;
    SignatureDialog::testRun = [&](SignatureDialog& d) {
        asked = true;
        CHECK(d.bar == 3 && d.sig == (TimeSig{4, 4}) && !d.removeButton);
        d.numerator->setValue(7);
        d.unit->setCurrentIndex(d.unit->findData(8));
        return true;
    };
    click(tl, t.barSeconds(2) * tl.pps + 30, mid);
    CHECK(asked && t.sigAtBar(3) == (TimeSig{7, 8}) && t.sigAtBar(2) == (TimeSig{4, 4}) && timings == 1);
    CHECK_NEAR(t.beatAt((double)n->start / SR), 16, 1e-4);
    CHECK(hist.check(SR));
    // 6/8 at bar 5 (through editSignature, as the menu's Add does)
    SignatureDialog::testRun = [](SignatureDialog& d) {
        CHECK(d.bar == 5 && d.sig == (TimeSig{7, 8}));
        d.numerator->setValue(6);
        return true;
    };
    tl.editSignature(5);
    CHECK(t.sigAtBar(5) == (TimeSig{6, 8}) && t.sigAtBar(4) == (TimeSig{7, 8}));
    CHECK(hist.check(SR));
    std::vector<Meter> sigs = tl.signatures();
    CHECK(sigs.size() == 3 && sigs[1].bar == 2 && sigs[2].bar == 4);
    // drawn right: a 7/8 bar is 3.5 quarter notes, a 6/8 bar 3; each block starts at its bar (its line)
    CHECK_NEAR(t.barBeat(3) - t.barBeat(2), 3.5, 1e-9);
    CHECK_NEAR(t.barBeat(5) - t.barBeat(4), 3.0, 1e-9);
    tl.update();
    Image img = paintScroll(r.scroll);
    savePng(OUT + "/lane.png", img);
    Color line = ui::style::color(paint::nodeStyle(&tl, "TimeSignature", {}, {}), "border-left-color");
    for (int bar : {2, 4}) {
        int x = (int)std::lround(t.barSeconds(bar) * tl.pps);
        bool found = false;  // (its border-left: the line's colour at the bar, within a pixel)
        for (int dx = -1; dx <= 2; dx++) found = found || colorDistance(img.colorAt(x + dx, (int)mid), line) < 40;
        CHECK(found);
        if (!found) printf("  no line at bar %d (x %d)\n", bar + 1, x);
    }
    {  // (and none inside the 7/8 block, between its bar lines)
        int x = (int)std::lround(t.barSeconds(2) * tl.pps + t.barSeconds(3) * tl.pps) / 2;
        CHECK(colorDistance(img.colorAt(x, top + h - 5), line) >= 40);
    }
    // drag the 7/8 block's start from bar 3 to bar 4 (by the bars as the drag began); the clip keeps its beat
    double x3 = t.barSeconds(2) * tl.pps + 2, x4 = t.barSeconds(3) * tl.pps + 3;
    CHECK(tl.signatureAt(PointF((float)x3, (float)mid)) == std::optional<int>(3));
    dragFromTo(tl, x3, mid, x4, mid);
    CHECK(t.sigAtBar(3) == (TimeSig{4, 4}) && t.sigAtBar(4) == (TimeSig{7, 8}) && t.sigAtBar(5) == (TimeSig{6, 8}));
    CHECK_NEAR(t.beatAt((double)n->start / SR), 16, 1e-4);
    CHECK(hist.check(SR) && hist.labels().back() == "Tempo");
    CHECK(hist.undo(SR));  // (one step: the drag and what followed it)
    CHECK(t.sigAtBar(3) == (TimeSig{7, 8}) && t.sigAtBar(4) == (TimeSig{7, 8}));
    CHECK_NEAR(t.beatAt((double)n->start / SR), 16, 1e-4);
    CHECK(hist.redo(SR) && t.sigAtBar(3) == (TimeSig{4, 4}));
    // onto another signature's bar (5): it stays where it was; bar 1's doesn't move
    double x4b = t.barSeconds(3) * tl.pps + 2;
    dragFromTo(tl, x4b, mid, t.barSeconds(4) * tl.pps + 2, mid);
    CHECK(t.sigAtBar(4) == (TimeSig{7, 8}) && t.sigAtBar(5) == (TimeSig{6, 8}));
    dragFromTo(tl, 2, mid, t.barSeconds(2) * tl.pps, mid);
    CHECK(t.sigAtBar(1) == (TimeSig{4, 4}) && t.sigAtBar(3) == (TimeSig{4, 4}));
    CHECK(!hist.check(SR));  // (neither changed anything)
    // double-click a block: typed (5/16); its dialog offers Remove
    SignatureDialog::testRun = [](SignatureDialog& d) {
        CHECK(d.bar == 5 && d.removeButton != nullptr);
        d.numerator->setValue(5);
        d.unit->setCurrentIndex(d.unit->findData(16));
        return true;
    };
    ui::MouseEvent dbl = mouse(t.barSeconds(4) * tl.pps + 2, mid);
    tl.mousePress(dbl);
    tl.mouseRelease(dbl);
    tl.mouseDoubleClick(dbl);
    CHECK(t.sigAtBar(5) == (TimeSig{5, 16}));
    CHECK(hist.check(SR));
    // the menus: a block's (Edit…, Remove), bar 1's (Remove off), and Add on a bar without its own
    ui::Menu m5;
    tl.fillSignatureMenu(m5, 5);
    CHECK(actionStarting(m5, "Edit the time signature at bar 5 (5/16)") && !actionStarting(m5, "Add a time signature"));
    ui::Action* rem = actionStarting(m5, "Remove the time signature at bar 5");
    CHECK(rem && rem->enabled);
    if (rem) ui::Menu::activate(rem);
    CHECK(t.sigAtBar(5) == (TimeSig{7, 8}));  // (bar 4's from there)
    ui::Menu m1;
    tl.fillSignatureMenu(m1, 2);
    CHECK(actionStarting(m1, "Remove the time signature at bar 1") && !actionStarting(m1, "Remove the time signature at bar 1")->enabled);
    CHECK(actionStarting(m1, "Add a time signature at bar 2"));
    // the dialog's Remove
    SignatureDialog::testRun = [](SignatureDialog& d) {
        CHECK(d.bar == 4 && d.removeButton);
        d.remove();
        return true;
    };
    tl.editSignature(4);
    CHECK(tl.signatures().size() == 1 && t.sigAtBar(9) == (TimeSig{4, 4}));
    CHECK_NEAR(t.beatAt((double)n->start / SR), 16, 1e-4);
    CHECK(hist.check(SR));
    // the one in force typed for a bar without its own: nothing; bar 1's typed: the song's own
    CHECK(!tl.setSignature(6, TimeSig{4, 4}) && t.changes.empty());
    CHECK(!tl.removeSignature(1) && !tl.removeSignature(6));
    CHECK(tl.setSignature(1, TimeSig{3, 4}) && t.beatsPerBar == 3);
    // Timebase: Time keeps the clip in seconds
    tl.setTimebase(false);
    int64_t before = n->start;
    CHECK(tl.setSignature(2, TimeSig{12, 8}));
    CHECK(n->start == before && t.sigAtBar(2) == (TimeSig{12, 8}));
    CHECK(hist.check(SR));
    // saved and loaded (the project's timing entry): every signature back, a bad denominator read as 4
    Timing back = project_io::readTiming(Json::parse(project_io::describeTiming(t).dump()));
    CHECK(back.changes == t.changes && back.sig() == t.sig() && back.sigAtBar(2) == (TimeSig{12, 8}));
    tl.update();
    savePng(OUT + "/lane-edited.png", paintScroll(r.scroll));
    SignatureDialog::testRun = nullptr;
    lanes::setSignatureLaneShown(false);
    lanes::setTempoLaneShown(false);
}

void testWorstCase() {
    // a signature change at every bar for 500 bars (4/4, 7/8, 6/8, 5/16, 3/2 in turn), painted zoomed out and in;
    // every block blocked by its neighbours (it stays), and the last one (bar 500) dragged back and forth past the
    // end: the clips after it end where they began
    Rig r(1400, 300);
    Timeline& tl = *r.tl;
    Project& p = r.project;
    Timing& t = p.timing;
    const TimeSig kinds[] = {{4, 4}, {7, 8}, {6, 8}, {5, 16}, {3, 2}};
    for (int bar = 2; bar <= 500; bar++) {
        TimeSig s = kinds[bar % 5];
        t.changes.push_back(TempoChange{bar, 120, s.num, 0, false, s.den});
    }
    lanes::setSignatureLaneShown(true);
    CHECK(tl.signatures().size() == 500);
    auto track = p.addTrack(Kind::Midi);
    track->needsInstrument = false;
    std::vector<ClipPtr> clips;
    for (int k = 0; k < 30; k++) {
        ClipPtr c = makeMidiClip("m", {{0.0, 0.25, 60, 100}}, 1.0, SR);
        c->track = track;
        c->start = (int64_t)std::llround(t.barSeconds(480 + k) * SR);
        c->prepare(SR);
        p.addClip(c);
        clips.push_back(c);
    }
    std::vector<double> beats;
    for (auto& c : clips) beats.push_back(t.beatAt((double)c->start / SR));
    auto began = std::chrono::steady_clock::now();
    int k = 0;
    for (double pps : {0.5, 3.0, 30.0, 400.0}) {
        tl.pps = pps;
        tl.relayout();
        r.scroll.horizontalScrollBar()->setValue((int)(t.barSeconds(495) * pps) - 200);
        Image img = paintScroll(r.scroll);
        CHECK(img.width() > 0);
        savePng(OUT + "/worst-" + std::to_string(k++) + ".png", img);
    }
    double paintS = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    tl.pps = 30;
    tl.relayout();
    double y = lanes::signatureLaneTop() + lanes::signatureLaneHeight() / 2.0;
    double x250 = t.barSeconds(249) * tl.pps + 1;
    CHECK(tl.signatureAt(PointF((float)x250, (float)y)) == std::optional<int>(250));
    TimeSig at250 = t.sigAtBar(250);
    dragFromTo(tl, x250, y, x250 + 300, y);  // (bars 251… have their own: it stays)
    CHECK(t.sigAtBar(250) == at250 && tl.signatures().size() == 500);
    double x = t.barSeconds(499) * tl.pps + 1;
    TimeSig moving = t.sigAtBar(500);
    int moves = 0;
    tl.timingChanged.connect([&] { ++moves; });
    began = std::chrono::steady_clock::now();
    ui::MouseEvent e = mouse(x, y);
    tl.mousePress(e);
    CHECK(std::holds_alternative<SignatureDrag>(tl.drag));
    bool went = false;
    for (int i = 0; i < 300; i++) {
        ui::MouseEvent m = mouse(x + (i % 2 ? 160 : -23), y);
        tl.mouseMove(m);
        went = went || t.sigAtBar(500) != moving;
    }
    CHECK(went);  // (it moved past the end)
    ui::MouseEvent back = mouse(x, y);
    tl.mouseMove(back);
    tl.mouseRelease(back);
    double dragS = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    printf("500 signatures: 4 paints %.2f s, a drag of 300 moves %.2f s\n", paintS, dragS);
    CHECK(t.sigAtBar(500) == moving && t.sigAtBar(499) != moving && tl.signatures().size() == 500 && moves == 1);
    double worst = 0;
    for (size_t i = 0; i < clips.size(); i++) worst = std::max(worst, std::fabs(t.beatAt((double)clips[i]->start / SR) - beats[i]));
    CHECK(worst < 1e-6);
    CHECK(paintS < 20 && dragS < 30);
    lanes::setSignatureLaneShown(false);
}

// --- the main window: the rows beside the lanes, their ✕ and the View menu; pictures ---

void settle(int frames = 3) {
    for (int i = 0; i < frames; i++) {
        ui::App::get().advanceTimers(0.05);
        ui::App::get().tick();
    }
    ui::App::get().doLayouts();
}

ui::Action* viewAction(MainWindow& w, const std::string& text) {
    for (auto& m : w.menuBar->menus())
        for (auto& a : m->actions())
            if (a->text == text) return a.get();
    return nullptr;
}

void shot(MainWindow& w, const std::string& name) {
    settle();
    w.invalidate(w.outerRect());
    w.flush();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
}

void testWindow() {
    auto* w = new MainWindow();
    w->askBeforeClosing = false;
    w->resizeWindow(1280, 720);
    w->showWindow();
    Timeline& tl = *w->timeline;
    Timing& t = w->project.timing;
    for (int i = 0; i < 3; i++) tl.addTrack(i % 2 ? Kind::Midi : Kind::Audio);
    t.setSignature(3, TimeSig{7, 8});
    t.setSignature(5, TimeSig{6, 8});
    t.setSignature(8, TimeSig{5, 4});
    t.setChange(TempoChange{4, 150, 7, 0, false, 8});
    t.setChange(TempoChange{9, 90, 5, 0, false, 4});
    ui::Action* tempoAct = viewAction(*w, "&Tempo lane");
    ui::Action* sigAct = viewAction(*w, "Time &signature lane");
    CHECK(tempoAct && sigAct);
    // View → both lanes: their rows under the CPU monitor, as tall as the lanes; the track rows still line up
    w->showTempoLane(true);
    w->showSignatureLane(true);
    settle();
    CHECK(w->tempoRow->isVisible() && w->signatureRow->isVisible() && sigAct->checked && w->prefs.signatureLane);
    CHECK(w->tempoRow->height() == lanes::tempoLaneHeight() && w->signatureRow->height() == lanes::signatureLaneHeight());
    CHECK(w->cpuMonitor->height() == lanes::rulerHeight());
    Point rowAt = w->signatureRow->mapTo(w, Point(0, w->signatureRow->height()));
    Point laneAt = tl.mapTo(w, Point(0, lanes::lanesTop()));
    CHECK(rowAt.y == laneAt.y);  // (the first track's row starts where its lane does)
    if (rowAt.y != laneAt.y) printf("  rows end at %d, lanes start at %d\n", rowAt.y, laneAt.y);
    // the row's controls fit inside it
    for (ui::Widget* c : {(ui::Widget*)w->tempoRow->minBox, (ui::Widget*)w->tempoRow->maxBox, (ui::Widget*)w->tempoRow->autoBox,
                          (ui::Widget*)w->tempoRow->close, (ui::Widget*)w->signatureRow->close}) {
        Rect g = c->geometry();
        CHECK(g.x >= 0 && g.y >= 0 && g.right() <= c->parent()->width() && g.bottom() <= c->parent()->height() && g.w > 8);
        if (!(g.bottom() <= c->parent()->height())) printf("  %s below its row: %d > %d\n", c->type().c_str(), g.bottom(), c->parent()->height());
    }
    CHECK(w->tempoRow->autoBox->isChecked());
    shot(*w, "window-auto");
    // the row's Min and Max: a fixed range (Auto off); not an undo step
    int steps = w->history->undoSteps();
    w->tempoRow->maxBox->setValue(200);
    w->tempoRow->minBox->setValue(100);
    CHECK(t.tempoLaneFixed() && t.tempoLaneLo == 100 && t.tempoLaneHi == 200 && !w->tempoRow->autoBox->isChecked());
    w->history->check(w->engine.sr);
    CHECK(w->history->undoSteps() == steps);
    shot(*w, "window-fixed");
    // Max under Min: Auto
    w->tempoRow->maxBox->setValue(80);
    CHECK(!t.tempoLaneFixed() && w->tempoRow->autoBox->isChecked());
    w->tempoRow->autoBox->setChecked(false);  // (Auto off: the range fitted now, fixed)
    CHECK(t.tempoLaneFixed());
    w->tempoRow->autoBox->setChecked(true);
    CHECK(!t.tempoLaneFixed());
    // zoomed out and in
    tl.pps = 8;
    tl.relayout();
    shot(*w, "window-zoomed-out");
    tl.pps = 300;
    tl.relayout();
    shot(*w, "window-zoomed-in");
    tl.pps = config::DEFAULT_PPS;
    tl.relayout();
    // the window at its smallest and very large: the rows keep their lanes' heights, their controls inside them
    for (Point size : {Point(640, 420), Point(2560, 1440)}) {
        w->resizeWindow(size.x, size.y);
        settle();
        CHECK(w->tempoRow->height() == lanes::tempoLaneHeight() && w->signatureRow->height() == lanes::signatureLaneHeight());
        CHECK(w->tempoRow->autoBox->geometry().right() <= w->tempoRow->width());
        shot(*w, "window-" + std::to_string(size.x) + "x" + std::to_string(size.y));
    }
    w->resizeWindow(1280, 720);
    settle();
    // every skin (rule 24: contrast; look at them)
    for (const skin::Skin& s : skin::library()) {
        w->chooseSkin(s);
        settle();
        CHECK(w->tempoRow->height() == lanes::tempoLaneHeight());
        std::string name = s.name;
        for (auto& c : name)
            if (!isalnum((unsigned char)c)) c = '-';
        shot(*w, "skin-" + name);
    }
    w->chooseSkin(skin::find("Dark"));
    settle();
    // the ✕s hide the lanes (and the View menu's ticks follow); the View menu shows them again
    w->signatureRow->close->click();
    settle();
    CHECK(!lanes::signatureLaneShown() && !w->signatureRow->isVisible() && !sigAct->checked && !w->prefs.signatureLane);
    w->tempoRow->close->click();
    settle();
    CHECK(!lanes::tempoLaneShown() && !w->tempoRow->isVisible() && !tempoAct->checked);
    CHECK(lanes::lanesTop() == lanes::rulerHeight());
    if (sigAct->toggled) sigAct->toggled(true);
    settle();
    CHECK(lanes::signatureLaneShown() && w->signatureRow->isVisible());
    shot(*w, "window-signature-only");
    Preferences again = Preferences::load();
    CHECK(again.signatureLane && !again.tempoLane);
    w->showSignatureLane(false);
    w->closeFxWindows();
    delete w;
    settle(1);
}

// --- the terminal ---

using namespace tui;

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{120, 34};
    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    void keys(std::vector<Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
};

void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    Timing& t = w->project.timing;
    // a track; the ruler (Up from the first lane); Shift+S: the time-signature lane as a row under the ruler
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab), key('A', 0, "a"), key(plat::Key_Tab)});
    f.selectLane(0);
    d.keys({key(plat::Key_Up), key('S', plat::ModShift, "S")});
    CHECK(f.laneRow == -1 && lanes::signatureLaneShown() && w->prefs.signatureLane);
    CHECK(d.shows("|4/4") && d.shows("Signature  4/4 at bar 1"));
    // the cursor at bar 3: / types its signature (7/8 here)
    w->timeline->setCursor((int64_t)std::llround(t.barSeconds(2) * w->engine.sr));
    bool sawDialog = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);
        sawDialog = sawDialog || d.shows("Time signature at bar 3");
        d.step(done);
    };
    d.keys({key('/', 0, "/"), key('A', plat::ModCtrl), key('7', 0, "7"), key(plat::Key_Tab), key(plat::Key_Down),
            key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawDialog && t.sigAtBar(3).num == 7);
    if (t.sigAtBar(3) != (TimeSig{7, 8})) printf("  typed: %s\n", t.sigAtBar(3).text().c_str());
    d.keys({});
    CHECK(d.shows("|" + t.sigAtBar(3).text()) && d.shows("Signature  " + t.sigAtBar(3).text() + " at bar 3"));
    // Shift+Del: removed again; at bar 1: a word why not
    d.keys({key(plat::Key_Delete, plat::ModShift)});
    CHECK(t.sigAtBar(3) == (TimeSig{4, 4}) && t.changes.empty());
    w->timeline->setCursor(0);
    d.keys({key(plat::Key_Delete, plat::ModShift)});
    CHECK(d.shows("song's own"));
    // Shift+T: the tempo lane's row too; Shift+R its range (Max typed: Auto off)
    d.keys({key('T', plat::ModShift, "T")});
    CHECK(lanes::tempoLaneShown() && d.shows("Tempo  ") && d.shows("(Auto)"));
    TempoRangeDialog::testRun = [](TempoRangeDialog& dlg) {
        CHECK(dlg.autoBox->isChecked());
        dlg.maxBox->setValue(180);
        dlg.minBox->setValue(60);
        CHECK(!dlg.autoBox->isChecked());
        return true;
    };
    d.keys({key('R', plat::ModShift, "R")});
    TempoRangeDialog::testRun = nullptr;
    CHECK(t.tempoLaneFixed() && t.tempoLaneLo == 60 && t.tempoLaneHi == 180);
    d.keys({});
    CHECK(d.shows("Tempo  60-180 BPM") && !d.shows("(Auto)"));
    std::string screen = d.screen.lines();
    // F1 lists the keys
    d.keys({key(plat::Key_F1)});
    CHECK(f.helpShown);
    bool listed = false;
    for (int i = 0; i < 12 && !listed; i++) {
        listed = d.shows("Shift+T, Shift+S") && d.shows("Shift+Del");
        if (!listed) d.keys({key(plat::Key_PageDown)});
    }
    CHECK(listed);
    d.keys({key(plat::Key_Escape)});
    // both hidden again
    d.keys({key('T', plat::ModShift, "T"), key('S', plat::ModShift, "S")});
    CHECK(!lanes::tempoLaneShown() && !lanes::signatureLaneShown());
    FILE* out = fopen((OUT + "/terminal.txt").c_str(), "w");
    if (out) {
        fputs(screen.c_str(), out);
        fclose(out);
    }
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    skin::setBuiltinDir("data/skins");
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    util::makeDirs("build/test-signature-lane");
    settings::setPath("build/test-signature-lane/settings.json");
    testLane();
    testWorstCase();
    testWindow();
    testTerminal();
    TEST_MAIN_END();
}
