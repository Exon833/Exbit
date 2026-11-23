// The tempo lane (ui/timeline_tempo.cpp) and the terminal's ruler: points added, dragged (time and BPM), typed
// (the dialog: tempo, ramp, time signature), ramped, removed; the timebase moving clips with every edit as one
// undo step; the ruler's menu; and the worst: thousands of points painted and dragged back and forth. The
// terminal: Enter on a point at the ruler edits it, F2 offers its menu (add, remove, Timebase).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace {

const int SR = 48000;

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
    Rig(int w = 1200, int h = 400) {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(w, h);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        tl->pps = 100;
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

std::string outDir() {
    std::string d = "build/ui-samples/tempo-lane";
    util::makeDirs(d);
    return d;
}

void click(Timeline& tl, double x, double y, int mods = 0) {
    ui::MouseEvent e = mouse(x, y, plat::LeftButton, mods);
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

void testLane() {
    Rig r;
    Timeline& tl = *r.tl;
    Project& p = r.project;
    Timing& t = p.timing;
    CHECK(t.timebaseBeats);  // (a new project: Beats)
    int lanesTop = lanes::lanesTop();
    lanes::setTempoLaneShown(true);
    tl.relayout();
    CHECK(lanes::lanesTop() == lanesTop + config::TEMPO_LANE_H && tl.lanes()[0].top == lanes::lanesTop());
    int top = lanes::rulerHeight(), h = lanes::tempoLaneHeight();
    CHECK(tl.inTempoLane(top + 1) && !tl.inTempoLane(top - 1) && !tl.inTempoLane(top + h));
    // an audio clip at bar 4 (6 s) and a MIDI clip at bar 3 (4 s): they follow the tempo (Beats)
    auto track = p.addTrack(Kind::Audio);
    auto source = std::make_shared<AudioBuffer>();
    source->samples.assign((size_t)SR * 2, 0.1f);
    ClipPtr a = makeAudioClip("a", source, SR, SR);
    a->track = track;
    a->start = SR * 6;
    p.addClip(a);
    auto midi = p.addTrack(Kind::Midi);
    midi->needsInstrument = false;
    ClipPtr n = makeMidiClip("n", {{0.0, 0.5, 60, 100}, {1.0, 1.5, 64, 100}}, 2.0, SR);
    n->track = midi;
    n->start = SR * 4;
    n->prepare(SR);
    p.addClip(n);
    history::History hist(p, SR);
    int timings = 0;
    tl.timingChanged.connect([&] { ++timings; });
    // a click on the lane at bar 2 (2 s: 200 px), high up: a step there at that tempo; one undo step with the clips
    double y = tl.tempoY(140);
    click(tl, 203, y);
    const TempoChange* c = t.changeAtBar(2);
    CHECK(c && std::fabs(c->bpm - 140) < 0.6 && !c->ramp && c->beatsPerBar == 4 && timings == 1);
    double bpm = c ? c->bpm : 140;
    CHECK_NEAR((double)a->start / SR, 2 + 8 * 60 / bpm, 1.0 / SR);  // (still on beat 12)
    CHECK_NEAR((double)n->start / SR, 2 + 4 * 60 / bpm, 1.0 / SR);
    CHECK(hist.check(SR) && hist.labels().back() == "Tempo");
    // the point drawn where it is; its menu: ramp it (to nothing: no next change yet), then a change at bar 5
    CHECK(tl.tempoPointAt(PointF(200, (float)tl.tempoY(bpm))).has_value());
    ui::Menu menu;
    tl.fillTempoMenu(menu, 2.0, TempoRef{2, 0});
    CHECK(actionStarting(menu, "Ramp to the next change") && !actionStarting(menu, "Ramp to the next change")->enabled);
    CHECK(actionStarting(menu, "Remove the tempo change at bar 2") && actionStarting(menu, "Remove the tempo change")->enabled);
    TempoDialog::testRun = [](TempoDialog& d) {
        CHECK(d.bar == 5 && d.beat == 0 && !d.removeButton && !d.rampBox->isChecked());
        d.tempo->setValue(90);
        d.signature->setValue(3);
        CHECK(d.unit->currentDataInt() == 4 && d.unit->isEnabled());
        d.unit->setCurrentIndex(d.unit->findData(8));  // (3/8)
        savePng(outDir() + "/tempo-dialog.png", paintScroll(d));
        return true;
    };
    tl.addTempoPoint(t.barSeconds(4) + 0.01);
    CHECK(t.changeAtBar(5) && t.changeAtBar(5)->bpm == 90 && t.changeAtBar(5)->sig() == (TimeSig{3, 8}));
    CHECK(t.barBeat(5) - t.barBeat(4) == 1.5);  // (a bar of 3/8: one and a half quarter notes)
    // now bar 2's can ramp: its menu ramps it; the tempo then rises… falls from bar 2's to 90 at bar 5
    ui::Menu menu2;
    tl.fillTempoMenu(menu2, 2.0, TempoRef{2, 0});
    ui::Action* ramp = actionStarting(menu2, "Ramp to the next change");
    CHECK(ramp && ramp->enabled && !ramp->checked);
    if (ramp && ramp->toggled) ramp->toggled(true);
    else if (ramp) ui::Menu::activate(ramp);
    CHECK(t.changeAtBar(2) && t.changeAtBar(2)->ramp);
    CHECK_NEAR(t.bpmAt(t.secondsAt(10)), (bpm + 90) / 2, 1e-6);  // (halfway between bar 2 and bar 5)
    CHECK_NEAR(t.beatAt((double)a->start / SR), 12, 1e-4);         // (the audio clip: still on its beat)
    // double-click the point: its dialog (tempo 128, the ramp kept)
    TempoDialog::testRun = [](TempoDialog& d) {
        CHECK(d.bar == 2 && d.rampBox->isChecked() && d.removeButton != nullptr && d.signature->isEnabled());
        d.tempo->setValue(128);
        return true;
    };
    ui::MouseEvent dbl = mouse(t.barSeconds(1) * tl.pps, tl.tempoY(t.changeAtBar(2)->bpm));
    tl.mousePress(dbl);
    tl.mouseRelease(dbl);
    tl.mouseDoubleClick(dbl);
    CHECK(t.changeAtBar(2) && t.changeAtBar(2)->bpm == 128 && t.changeAtBar(2)->ramp);
    tl.update();
    savePng(outDir() + "/ramp.png", paintScroll(r.scroll));
    CHECK(hist.check(SR));
    // drag the point at bar 2 by the grid to beat 2 of bar 2 (half a beat's grid: 1/8 at this zoom), lower
    t.grid = 1;  // (a beat)
    double x0 = t.barSeconds(1) * tl.pps, y0 = tl.tempoY(128);
    double x1 = t.secondsAt(5) * tl.pps + 3, y1 = tl.tempoY(110);
    dragFromTo(tl, x0, y0, x1, y1);
    CHECK(!t.changeAtBar(2) && t.changeAt(2, 1) && t.changeAt(2, 1)->ramp);
    CHECK(t.changeAt(2, 1) && std::fabs(t.changeAt(2, 1)->bpm - 110) < 1.5);
    CHECK_NEAR(t.beatAt((double)a->start / SR), 12, 1e-4);
    CHECK(hist.check(SR));
    // undo: the drag and what moved with it, at once
    int64_t aAt = a->start;
    CHECK(hist.undo(SR));
    CHECK(t.changeAtBar(2) && t.changeAtBar(2)->bpm == 128 && a->start != aAt);
    CHECK_NEAR(t.beatAt((double)a->start / SR), 12, 1e-4);
    CHECK(hist.redo(SR));
    // a time signature's point moves by whole bars only
    double x5 = t.barSeconds(4) * tl.pps, y5 = tl.tempoY(90);
    dragFromTo(tl, x5, y5, x5 + 40, y5);
    CHECK(t.changeAtBar(5) && t.changeAtBar(5)->beatsPerBar == 3);
    // bar 1's point: only its tempo
    dragFromTo(tl, 0, tl.tempoY(t.bpm), 150, tl.tempoY(100));
    CHECK(std::fabs(t.bpm - 100) < 1.5 && t.changes.size() == 2);
    // right-click a point: Remove; Timebase: Time, then a tempo edit leaves the clips in seconds
    ui::Menu menu3;
    tl.fillTempoMenu(menu3, 0, TempoRef{2, 1});
    ui::Menu::activate(actionStarting(menu3, "Remove the tempo change"));
    CHECK(!t.changeAt(2, 1) && t.changes.size() == 1);
    CHECK(hist.check(SR));
    tl.setTimebase(false);
    CHECK(!t.timebaseBeats);
    CHECK(hist.check(SR) && hist.labels().back() == "Timebase");
    int64_t before = a->start;
    click(tl, t.secondsAt(2) * tl.pps, top + h / 2);  // (inside bar 1: a tempo change on its beat 3)
    CHECK(t.changes.size() == 2 && t.changeAt(1, 2) && a->start == before);
    tl.update();
    savePng(outDir() + "/lane.png", paintScroll(r.scroll));
    TempoDialog::testRun = nullptr;
    lanes::setTempoLaneShown(false);
}

void testWorstCase() {
    // 2,000 points (ramps and steps, inside bars too) painted at every zoom; a point dragged back and forth 300
    // times: the clips end where they began (each move follows from where the drag began)
    Rig r(1400, 400);
    Timeline& tl = *r.tl;
    Project& p = r.project;
    Timing& t = p.timing;
    for (int k = 0; k < 2000; k++)
        t.changes.push_back(TempoChange{2 + k, 60.0 + (k * 37) % 150, 3 + k % 3, k % 4 == 1 ? 1.5 : 0.0, k % 3 != 0});
    std::sort(t.changes.begin(), t.changes.end());
    auto track = p.addTrack(Kind::Midi);
    track->needsInstrument = false;
    std::vector<ClipPtr> clips;
    for (int k = 0; k < 40; k++) {
        ClipPtr c = makeMidiClip("m", {{0.0, 0.25, 60, 100}, {0.5, 1.0, 62, 100}}, 1.0, SR);
        c->track = track;
        c->start = (int64_t)std::llround(t.barSeconds(1 + k * 3) * SR);
        c->prepare(SR);
        p.addClip(c);
        clips.push_back(c);
    }
    std::vector<double> beats;
    for (auto& c : clips) beats.push_back(t.beatAt((double)c->start / SR));
    lanes::setTempoLaneShown(true);
    tl.relayout();
    auto began = std::chrono::steady_clock::now();
    for (double pps : {0.5, 5.0, 50.0, 400.0}) {
        tl.pps = pps;
        tl.relayout();
        Image img = paintScroll(r.scroll);
        CHECK(img.width() > 0);
    }
    double paintS = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    tl.pps = 50;
    tl.relayout();
    TempoRef first{2, 0};
    double x = t.barSeconds(1) * tl.pps, y = tl.tempoY(t.changeAtBar(2)->bpm);
    began = std::chrono::steady_clock::now();
    ui::MouseEvent e = mouse(x, y);
    tl.mousePress(e);
    CHECK(std::holds_alternative<TempoDrag>(tl.drag));
    for (int k = 0; k < 300; k++) {
        ui::MouseEvent m = mouse(x + (k % 2 ? 37 : -11), y + (k % 2 ? -9 : 7));
        tl.mouseMove(m);
    }
    ui::MouseEvent back = mouse(x, y);
    tl.mouseMove(back);
    tl.mouseRelease(back);
    double dragS = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    printf("2,000 tempo points: 4 paints %.2f s, a drag of 300 moves %.2f s\n", paintS, dragS);
    CHECK(t.changeAtBar(2) != nullptr);
    double worst = 0;
    for (size_t k = 0; k < clips.size(); k++) worst = std::max(worst, std::fabs(t.beatAt((double)clips[k]->start / SR) - beats[k]));
    CHECK(worst < 1e-3);
    CHECK(paintS < 20 && dragS < 30);
    (void)first;
    lanes::setTempoLaneShown(false);
}

// --- the terminal ---

using namespace tui;

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{110, 34};
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
    // the ruler (Up from the first lane), the cursor at bar 3: Enter makes a change there, typed in its dialog
    d.keys({key(plat::Key_Tab), key('A', 0, "a"), key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab)});
    f.selectLane(0);
    w->timeline->setCursor((int64_t)std::llround(t.barSeconds(2) * w->engine.sr));
    bool sawDialog = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);
        sawDialog = sawDialog || (d.shows("Tempo from bar 3") && d.shows("Ramp to the next change"));
        d.step(done);
    };
    d.keys(std::vector<Key>{key(plat::Key_Up), key(plat::Key_Return), key('A', plat::ModCtrl), key('9', 0, "9"),
                            key('0', 0, "0"), key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawDialog && f.laneRow == -1);
    CHECK(t.changeAtBar(3) && t.changeAtBar(3)->bpm == 90 && !t.changeAtBar(3)->ramp);
    d.keys({});
    CHECK(d.shows("♩") || d.shows("T"));  // (the change's mark on the ruler)
    ui::App::get().advanceTimers(11);  // (the skin's message at the start goes after 10 s)
    d.keys({});
    CHECK(d.shows("tempo change at the cursor"));  // (the status line's hint)
    // a change at bar 5 too (140), then Enter on bar 3's: its own dialog, Remove offered; ramp it (Tab to the
    // box, Space), Enter: a ramp from 90 to 140
    w->timeline->setCursor((int64_t)std::llround(t.barSeconds(4) * w->engine.sr));
    d.keys(std::vector<Key>{key(plat::Key_Return), key('A', plat::ModCtrl), key('1', 0, "1"), key('4', 0, "4"),
                            key('0', 0, "0"), key(plat::Key_Return)});
    CHECK(t.changeAtBar(5) && t.changeAtBar(5)->bpm == 140);
    w->timeline->setCursor((int64_t)std::llround(t.barSeconds(2) * w->engine.sr));
    bool sawRemove = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        d.f->draw(d.screen);
        sawRemove = sawRemove || (d.shows("Tempo from bar 3") && d.shows("Remove this change"));
        d.step(done);
    };
    d.keys({key(plat::Key_Return), key(plat::Key_Tab), key(plat::Key_Space, 0, " "), key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawRemove && t.changeAtBar(3) && t.changeAtBar(3)->ramp && t.changeAtBar(3)->bpm == 90);
    CHECK_NEAR(t.bpmAt(t.barSeconds(3)), 115, 1e-6);  // (halfway through the ramp)
    d.keys({});
    CHECK(d.shows("↗") || d.shows("/"));  // (a ramp's mark)
    d.keys({key(plat::Key_F2)});
    bool f2 = f.menuOpen() && d.shows("Tempo changes") && d.shows("Tempo and time signature from bar 3");
    d.keys({key(plat::Key_Escape)});
    if (!f2) printf("%s\n", d.screen.lines().c_str());
    CHECK(f2);
    // the Timebase, as the menu sets it (the timeline's own function behind it)
    w->timeline->setTimebase(false);
    CHECK(!t.timebaseBeats);
    w->timeline->setTimebase(true);
    // the change removed through the ruler's menu entry (as F2 → Tempo changes → Remove does)
    ui::Menu menu;
    w->timeline->fillRulerMenu(menu, PointF((float)w->timeline->xOf(w->timeline->cursor), 5));
    ui::Menu* sub = nullptr;
    for (auto& a : menu.actions())
        if (a->text == "Tempo changes") sub = a->submenu;
    CHECK(sub && actionStarting(*sub, "Remove the tempo change at bar 3"));
    if (sub) ui::Menu::activate(actionStarting(*sub, "Remove the tempo change at bar 3"));
    CHECK(!t.changeAtBar(3));
    // F1 lists the ruler's keys
    d.keys({key(plat::Key_F1)});
    CHECK(f.helpShown);
    d.keys({key(plat::Key_Escape)});
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs("build/test-ui-tempo-lane");
    settings::setPath("build/test-ui-tempo-lane/settings.json");
    testLane();
    testWorstCase();
    testTerminal();
    TEST_MAIN_END();
}
