// The tempo lane (ui/timeline_tempo.cpp) and the terminal's ruler: points added, dragged (time and BPM), typed
// (the dialog: tempo, ramp, time signature), ramped, removed; the timebase moving clips with every edit as one
// undo step; the ruler's menu; and the worst: thousands of points painted and dragged back and forth. The
// terminal: Enter on a point at the ruler edits it, F2 offers its menu (add, remove, Timebase). The lane's range
// (Min, Max, Auto; fallbacks; saved; points outside it at its edge; dragged, the range changed mid-drag) and its row
// in the main window (✕, View → Tempo lane).
#include <algorithm>
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
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/timeline_look.h"
#include "ui/timing_rows.h"
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

// the lane's BPM range: Auto (fitted), a fixed Min…Max (the row's), fallbacks to Auto, saved with the song and not
// an undo step; points outside it drawn at its edge (with a mark) and still dragged; the range changed mid-drag
void testRange() {
    Rig r;
    Timeline& tl = *r.tl;
    Project& p = r.project;
    Timing& t = p.timing;
    lanes::setTempoLaneShown(true);
    tl.relayout();
    t.setChange(TempoChange{3, 180, 4});
    history::History hist(p, SR);
    double lo, hi;
    tl.tempoRange(lo, hi);
    CHECK(!t.tempoLaneFixed() && lo <= 120 && hi >= 180);  // (Auto: both points inside)
    int changes = 0;
    tl.tempoRangeChanged.connect([&] { ++changes; });
    tl.setTempoLaneRange(100, 150);
    tl.tempoRange(lo, hi);
    CHECK(t.tempoLaneFixed() && lo == 100 && hi == 150 && changes == 1);
    CHECK(!hist.check(SR));  // (a view: not an undo step)
    int top = lanes::rulerHeight(), h = lanes::tempoLaneHeight(), pad = timeline::automationPad();
    CHECK_NEAR(tl.tempoY(150), top + pad, 1e-9);
    CHECK_NEAR(tl.tempoY(100), top + h - pad, 1e-9);
    // 180 is above it: at its top edge, found there by the mouse
    double x3 = t.barSeconds(2) * tl.pps;
    CHECK_NEAR(tl.tempoPointY(180), tl.tempoY(150), 1e-9);
    std::optional<TempoRef> at = tl.tempoPointAt(PointF((float)x3, (float)tl.tempoY(150)));
    CHECK(at && at->bar == 3);
    tl.update();
    Image img = paintScroll(r.scroll);
    savePng(outDir() + "/range-clipped.png", img);
    {  // (its ▲ left of the point, in the point's colour)
        const LaneLook& lk = const_cast<Timeline&>(tl).look().lanes->get(LaneKind::Automation);
        int mx = (int)std::lround(x3 - lk.pointW / 2.0 - 6), my = (int)std::lround(tl.tempoY(150)) + 2;
        Color c = img.colorAt(mx, my), want = lk.pointColor;
        CHECK(std::abs(c.r - want.r) + std::abs(c.g - want.g) + std::abs(c.b - want.b) < 60);
    }
    // the fallbacks: Max not above Min, outside MIN_BPM…MAX_BPM, 0 / 0: Auto
    for (auto [a, b] : std::vector<std::pair<double, double>>{{150, 120}, {130, 130}, {5, 120}, {100, 5000}, {0, 0}}) {
        tl.setTempoLaneRange(100, 150);
        tl.setTempoLaneRange(a, b);
        CHECK(!t.tempoLaneFixed() && t.tempoLaneLo == 0 && t.tempoLaneHi == 0);
    }
    // saved and loaded; a bad range in a file: Auto; old files (none): Auto
    tl.setTempoLaneRange(90, 210);
    Timing back = project_io::readTiming(Json::parse(project_io::describeTiming(t).dump()));
    CHECK(back.tempoLaneFixed() && back.tempoLaneLo == 90 && back.tempoLaneHi == 210);
    Json bad = project_io::describeTiming(t);
    bad.set("tempo_lane_range", Json::array({Json(300.0), Json(200.0)}));
    CHECK(!project_io::readTiming(bad).tempoLaneFixed());
    bad.set("tempo_lane_range", Json("fast"));
    CHECK(!project_io::readTiming(bad).tempoLaneFixed());
    Timing copy = t;  // (a copy keeps it: projects opened, templates)
    CHECK(copy.tempoLaneLo == 90 && copy.tempoLaneHi == 210);
    tl.setTempoLaneRange(0, 0);
    CHECK(!project_io::describeTiming(t).has("tempo_lane_range"));
    // a clipped point dragged down into the range: its tempo from the height there
    tl.setTempoLaneRange(100, 150);
    dragFromTo(tl, x3, tl.tempoY(150), x3, tl.tempoY(125));
    CHECK(t.changeAtBar(3) && std::fabs(t.changeAtBar(3)->bpm - 125) < 1.5);
    CHECK(hist.check(SR) && hist.labels().back() == "Tempo");
    // the range changed while dragging: the next move reads the height on the new range
    ui::MouseEvent e = mouse(x3, tl.tempoY(125));
    tl.mousePress(e);
    ui::MouseEvent m1 = mouse(x3, tl.tempoY(140));
    tl.mouseMove(m1);
    CHECK(std::fabs(t.changeAtBar(3)->bpm - 140) < 1.5);
    tl.setTempoLaneRange(60, 260);
    ui::MouseEvent m2 = mouse(x3, tl.tempoY(240));
    tl.mouseMove(m2);
    CHECK(std::fabs(t.changeAtBar(3)->bpm - 240) < 3);
    tl.setTempoLaneRange(0, 0);  // (Auto mid-drag: kept as it was while the drag lasts)
    double y = tl.tempoY(200);
    ui::MouseEvent m3 = mouse(x3, y);
    tl.mouseMove(m3);
    CHECK(std::fabs(t.changeAtBar(3)->bpm - 200) < 3);
    tl.mouseRelease(m3);
    CHECK(hist.check(SR) && !hist.check(SR));  // (the whole drag: one step)
    // the lane's menu offers the range; its dialog sets it
    ui::Menu menu;
    tl.fillTempoMenu(menu, 0, std::nullopt);
    ui::Action* range = actionStarting(menu, "The lane's range");
    CHECK(range != nullptr);
    TempoRangeDialog::testRun = [](TempoRangeDialog& d) {
        CHECK(d.autoBox->isChecked());
        d.minBox->setValue(70);
        d.maxBox->setValue(170);
        return true;
    };
    if (range) ui::Menu::activate(range);
    CHECK(t.tempoLaneFixed() && t.tempoLaneLo == 70 && t.tempoLaneHi == 170);
    TempoRangeDialog::testRun = [](TempoRangeDialog& d) {
        d.autoBox->setChecked(true);
        return true;
    };
    tl.editTempoLaneRange();
    CHECK(!t.tempoLaneFixed());
    TempoRangeDialog::testRun = nullptr;
    lanes::setTempoLaneShown(false);
}

// the main window: the tempo lane's row beside it (Min, Max, Auto, ✕) and View → Tempo lane
void testRow() {
    auto* w = new MainWindow();
    w->askBeforeClosing = false;
    w->resizeWindow(1280, 720);
    w->showWindow();
    Timing& t = w->project.timing;
    w->showTempoLane(true);
    ui::App::get().doLayouts();
    TimingLaneRow* row = w->tempoRow;
    CHECK(row->isVisible() && row->height() == lanes::tempoLaneHeight() && row->autoBox->isChecked());
    double lo, hi;
    w->timeline->tempoRange(lo, hi);
    CHECK(row->minBox->value() == lo && row->maxBox->value() == hi);
    row->minBox->setValue(90);
    CHECK(t.tempoLaneFixed() && t.tempoLaneLo == 90 && !row->autoBox->isChecked());
    // set elsewhere (the lane's menu, the terminal): the row follows
    w->timeline->setTempoLaneRange(110, 130);
    CHECK(row->minBox->value() == 110 && row->maxBox->value() == 130 && !row->autoBox->isChecked());
    // a project with a range opened: the row shows it (resetView)
    Timing loaded = t;
    loaded.tempoLaneLo = 50, loaded.tempoLaneHi = 250;
    t = loaded;
    w->resetView();
    CHECK(row->minBox->value() == 50 && row->maxBox->value() == 250);
    // ✕: hidden, View → Tempo lane unticked, remembered
    row->close->click();
    CHECK(!lanes::tempoLaneShown() && !row->isVisible() && !w->tempoLaneAction->checked && !w->prefs.tempoLane);
    if (w->tempoLaneAction->toggled) w->tempoLaneAction->toggled(true);
    CHECK(lanes::tempoLaneShown() && row->isVisible() && Preferences::load().tempoLane);
    w->showTempoLane(false);
    w->closeFxWindows();
    delete w;
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

// the labels never over one another: the tempo lane's (a point's tempo high at the view's right under "160 BPM", low
// points at bars where the time signature changes, over "7/8"), and the ruler's with hundreds of changes (the lane
// hidden: thinned out, never over one another nor over a bar's number) at every zoom
void testLabelsApart() {
    auto apart = [](const std::vector<RectF>& rs) {
        for (size_t i = 0; i < rs.size(); i++)
            for (size_t j = i + 1; j < rs.size(); j++)
                if (rs[i].x < rs[j].right() && rs[j].x < rs[i].right() && rs[i].y < rs[j].bottom() && rs[j].y < rs[i].bottom())
                    return false;
        return true;
    };
    Rig r(749, 300);
    Timeline& tl = *r.tl;
    Timing& t = r.project.timing;
    lanes::setTempoLaneShown(true);
    t.tempoLaneLo = 80, t.tempoLaneHi = 160;
    t.setChange(TempoChange{2, 82, 7, 0, false, 8});
    t.setChange(TempoChange{3, 158, 7, 0, false, 8});
    t.setChange(TempoChange{4, 84, 4, 0, false, 4});
    t.setChange(TempoChange{6, 157, 4, 0, false, 4});
    t.setChange(TempoChange{7, 150, 4, 0, false, 4});
    tl.pps = 60;
    tl.relayout();
    Image img = paintScroll(r.scroll);
    savePng(outDir() + "/labels-apart.png", img);
    const std::vector<RectF>& ls = tl.tempoLabelsShown;
    CHECK(apart(ls));
    CHECK(ls.size() >= 8);  // (every point's tempo in view, and each time signature: moved, not left out)
    int top = lanes::rulerHeight(), h = lanes::tempoLaneHeight();
    for (const RectF& b : ls) CHECK(b.y >= top && b.bottom() <= top + h);
    // the ruler, the lane hidden: 400 changes, from far out to close in
    lanes::setTempoLaneShown(false);
    t.changes.clear();
    t.tempoLaneLo = t.tempoLaneHi = 0;
    for (int b = 2; b < 400; b++) t.setChange(TempoChange{b, 100.0 + (b % 7) * 7, b % 2 ? 7 : 4, 0, false, b % 2 ? 8 : 4});
    int shownAt60 = 0;
    for (double pps : {2.0, 15.0, 40.0, 60.0, 200.0}) {
        tl.pps = pps;
        tl.relayout();
        r.scroll.scrollTo(0, 0);
        paintScroll(r.scroll);
        std::vector<RectF> all = tl.rulerTempoShown;
        // (the bar and beat numbers there, as the ruler draws them)
        const TimelineLook& lk = tl.look();
        for (auto& gl : t.lines(0, 749 / pps, pps))
            if (!gl.label.empty()) {
                float x = (float)(int)(gl.seconds * pps) + lk.labelPadLeft;
                all.push_back(RectF(x, 0, lk.labelFont.width(gl.label), 1));
            }
        CHECK(apart(all));
        if (pps == 60) shownAt60 = (int)tl.rulerTempoShown.size();
        // and the same labels when only a part of the ruler is painted (a scroll's strip)
        if (pps == 15) {
            std::vector<RectF> whole = tl.rulerTempoShown;
            Image part(749, 300);
            Painter pp(part);
            tl.paintTree(pp, Rect(300, 0, 200, lanes::rulerHeight()));
            for (const RectF& b : tl.rulerTempoShown)
                CHECK(std::find_if(whole.begin(), whole.end(), [&](const RectF& w) { return w.x == b.x; }) != whole.end());
        }
    }
    CHECK(shownAt60 >= 4);  // (room for them there: shown)
    lanes::setTempoLaneShown(false);
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs("build/test-ui-tempo-lane");
    settings::setPath("build/test-ui-tempo-lane/settings.json");
    testLane();
    testWorstCase();
    testRange();
    testRow();
    testTerminal();
    testLabelsApart();
    TEST_MAIN_END();
}
