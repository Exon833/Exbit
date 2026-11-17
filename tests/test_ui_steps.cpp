// The MIDI editor's per-note properties and its Steps view, headless: the lane chooser (Velocity / Chance /
// Repeats / Pan / Fine), setting values by clicking and dragging (relative with several notes selected),
// undo, Slide (G, the button, the lane's right-click menu), copy and paste keeping them; the Steps view
// toggled from the toolbar, pads lit and cleared (one-step notes), painting along a row, "+ Row", and the
// lane working there too. Paints PNGs into build/ui-samples/steps/. No sound: the engine is never opened.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "model/model.h"
#include "ui/midi_editor.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;
using namespace midi_editor;

namespace {
double gTime = 100;

plat::Event mouseEv(plat::Event::Type t, Point p, int button, int buttons, int mods = 0) {
    plat::Event e;
    e.type = t;
    e.x = p.x;
    e.y = p.y;
    e.button = button;
    e.buttons = buttons;
    e.mods = mods;
    e.time = gTime;
    return e;
}
void press(Widget* w, Point p, int button = plat::LeftButton, int mods = 0) {
    gTime += 1.0;
    Window* win = w->window();
    Point q = w->mapToWindow(p);
    win->onEvent(nullptr, mouseEv(plat::Event::MouseMove, q, 0, 0, mods));
    win->onEvent(nullptr, mouseEv(plat::Event::MouseDown, q, button, button, mods));
}
void dragTo(Widget* w, Point p, int buttons = plat::LeftButton, int mods = 0) {
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseMove, w->mapToWindow(p), 0, buttons, mods));
}
void release(Widget* w, Point p, int button = plat::LeftButton, int mods = 0) {
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseUp, w->mapToWindow(p), button, 0, mods));
}
void click(Widget* w, Point p, int button = plat::LeftButton, int mods = 0) {
    press(w, p, button, mods);
    release(w, p, button, mods);
}
void key(Window& w, int k, int mods = 0) {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k;
    e.mods = mods;
    e.time = gTime;
    w.onEvent(nullptr, e);
    e.type = plat::Event::KeyUp;
    w.onEvent(nullptr, e);
}

std::string outDir() {
    std::string d = "build/ui-samples/steps";
    util::makeDirs(d);
    return d;
}
void savePicture(Window& w, const std::string& name) {
    w.update();
    App::get().tick();
    savePng(outDir() + "/" + name + ".png", w.framebuffer());
}

Note note(double s, double e, int pitch, int vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

struct Fixture {
    Project project;
    audio::Engine engine{project};
    TrackPtr track;
    ClipPtr clip;
    int changes = 0;
    int64_t cursor = 0;
    Fixture() {
        project.timing.bpm = 120;  // a beat = 0.5 s, a bar = 2 s
        track = project.addTrack(Kind::Midi);
        track->name = "Drums";
        track->needsInstrument = false;
        clip = makeMidiClip("Beat", {note(0, 0.25, 36, 100), note(0.5, 0.75, 38, 80), note(1.0, 1.25, 36, 120),
                                     note(1.5, 1.75, 42, 60)},
                            2.0, engine.sr);
        clip->track = track;
        clip->start = 0;
        project.addClip(clip);
    }
    MidiEditor::TimelineLink link() {
        MidiEditor::TimelineLink l;
        l.cursor = [this] { return cursor; };
        l.setCursor = [this](int64_t f) { cursor = f; };
        l.seek = [this](int64_t) {};
        l.editNotes = [](ClipPtr) {};
        return l;
    }
};

int barX(MidiEditor& ed, int index) { return (int)ed.xOf(ed.notes[(size_t)index].start) + 2; }
int indexOf(MidiEditor& ed, double start, int pitch) {
    for (size_t i = 0; i < ed.notes.size(); ++i)
        if ((int)ed.notes[i].pitch == pitch && std::fabs(ed.notes[i].start - start) < 1e-9) return (int)i;
    return -1;
}
}  // namespace

static void testLaneChooser() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    VelocityLane* lane = ed.velocity;
    // the chooser: visible at the lane's left, a choice for each property, with a tooltip
    CHECK(lane->chooser && lane->chooser->isVisible() && !lane->chooser->toolTip().empty());
    CHECK(lane->chooser->text().find("Velocity") == 0);
    CHECK(ed.laneActions.size() == 5);
    CHECK(!lane->toolTipAt(Point(KEYS_W + 20, 40)).empty());
    Menu::activate(ed.laneActions[(int)Lane::Chance]);
    CHECK(ed.lane == Lane::Chance && lane->chooser->text().find("Chance") == 0);
    CHECK(ed.laneActions[(int)Lane::Chance]->checked && !ed.laneActions[(int)Lane::Velocity]->checked);
    savePicture(ed, "lane_chance");
    // the lane's scale: the bottom is the lowest value, the top the highest
    CHECK(lane->valueAt(lane->height() - 3) == 0 && lane->valueAt(3) == 100);
    ed.setLane(Lane::Pan);
    CHECK(lane->valueAt(lane->height() - 3) == -1 && lane->valueAt(3) == 1);
    CHECK(std::fabs(lane->valueAt(lane->height() / 2)) <= 0.02);
    ed.setLane(Lane::Repeats);
    CHECK(lane->valueAt(lane->height() - 3) == 1 && lane->valueAt(3) == 8);
    ed.setLane(Lane::Fine);
    CHECK(lane->valueAt(lane->height() - 3) == -100 && lane->valueAt(3) == 100);
    ed.setLane(Lane::Velocity);
    CHECK(lane->valueAt(3) == 127 && lane->valueAt(lane->height() - 3) == 1);  // (as velocityAt)
    ed.deleteOnClose = false;
    ed.close();
}

static void testLaneEditingAndUndo() {
    Fixture f;
    history::History h(f.project, f.engine.sr);
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    VelocityLane* lane = ed.velocity;
    ed.setLane(Lane::Chance);
    // a click on a note's bar sets its chance (absolute); the change goes into the clip on release
    int y = lane->height() / 4;
    press(lane, Point(barX(ed, 1), y));
    double want = lane->valueAt(y);
    CHECK(want > 60 && want < 90);
    CHECK(ed.notes[1].chance == want);
    CHECK(f.changes == 0);
    release(lane, Point(barX(ed, 1), y));
    CHECK(f.changes == 1 && (*f.clip->notes)[1].chance == want);
    CHECK((*f.clip->notes)[0].chance == 100);  // (the others untouched)
    // it is an undo step like any note edit
    CHECK(h.check(f.engine.sr));
    CHECK(h.undo(f.engine.sr));
    CHECK((*f.clip->notes)[1].chance == 100);
    ed.reload();
    CHECK(ed.notes[1].chance == 100);
    CHECK(h.redo(f.engine.sr));
    ed.reload();
    CHECK(ed.notes[1].chance == want);
    // dragging across sets each bar passed (repeats)
    ed.setLane(Lane::Repeats);
    press(lane, Point(barX(ed, 0), 3));
    dragTo(lane, Point(barX(ed, 2), 3));
    release(lane, Point(barX(ed, 2), 3));
    CHECK(ed.notes[0].repeats == 8 && ed.notes[2].repeats == 8 && ed.notes[1].repeats == 1);
    CHECK((*f.clip->notes)[2].repeats == 8);
    size_t heard = 8 + 8 + 1 + (note_props::sounds((*f.clip->notes)[1], 0) ? 1 : 0);
    CHECK(f.clip->played->size() == heard);  // (the rolls play: 8 hits each)
    // several selected: dragging one of their bars moves them all by as much (relative), within the range
    ed.setLane(Lane::Velocity);
    ed.selected = {0, 2, 3};  // velocities 100, 120, 60
    int changes = f.changes;
    int y0 = lane->height() / 2;
    press(lane, Point(barX(ed, 2), y0));
    CHECK(ed.drag.kind == MidiEditor::Drag::LaneShift);
    int y1 = y0 - 20;  // up: louder
    dragTo(lane, Point(barX(ed, 2), y1));
    double by = lane->valueAt(y1, false) - lane->valueAt(y0, false);
    CHECK(by > 20);
    release(lane, Point(barX(ed, 2), y1));
    CHECK(f.changes == changes + 1);
    CHECK(ed.notes[0].velocity == std::min(127.0, std::nearbyint(100 + by)));
    CHECK(ed.notes[2].velocity == 127);  // (kept in range)
    CHECK(ed.notes[3].velocity == std::nearbyint(60 + by));
    CHECK(ed.notes[1].velocity == 80);   // (not selected: untouched)
    // pan, relative, from the middle
    ed.setLane(Lane::Pan);
    ed.selected = {0, 3};
    press(lane, Point(barX(ed, 0), y0));
    dragTo(lane, Point(barX(ed, 0), y0 + 10));
    release(lane, Point(barX(ed, 0), y0 + 10));
    CHECK(ed.notes[0].pan < 0 && ed.notes[0].pan == ed.notes[3].pan && ed.notes[1].pan == 0);
    CHECK((*f.clip->notes)[0].pan == ed.notes[0].pan);
    savePicture(ed, "lane_pan");
    // the right-click menu: the lanes, Slide, the default back
    click(lane, Point(KEYS_W + 30, y0), plat::RightButton);
    CHECK(lane->context->isOpen());
    lane->context->close();
    ed.resetLane();
    CHECK(ed.notes[0].pan == 0 && ed.notes[3].pan == 0);
    // fine tuning, absolute on one note
    ed.setLane(Lane::Fine);
    ed.selected.clear();
    click(lane, Point(barX(ed, 1), 3));
    CHECK(ed.notes[1].fine == 100 && (*f.clip->notes)[1].fine == 100);
    ed.deleteOnClose = false;
    ed.close();
}

static void testSlide() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK(ed.slideButton && ed.slideButton->isVisible() && !ed.slideButton->toolTip().empty());
    // nothing selected: nothing slides
    key(ed, 'G');
    CHECK(f.changes == 0 && !ed.slideButton->isChecked());
    ed.selected = {1};
    ed.refresh();
    key(ed, 'G');
    CHECK(ed.notes[1].slide && (*f.clip->notes)[1].slide && f.changes == 1);
    CHECK(ed.slideButton->isChecked());  // (it shows the selected notes slide)
    savePicture(ed, "slide");
    // the button does the same: again, they stop sliding
    ed.slideButton->click();
    CHECK(!ed.notes[1].slide && !(*f.clip->notes)[1].slide && !ed.slideButton->isChecked());
    // a mixed selection: all of them slide
    ed.notes[1].slide = true;
    ed.selected = {1, 2};
    ed.toggleSlide();
    CHECK(ed.notes[1].slide && ed.notes[2].slide);
    // copied and pasted notes keep their properties
    ed.notes[2].chance = 40;
    ed.notes[2].pan = 0.25;
    ed.commit();
    ed.selected = {2};
    noteClipboard.clear();
    key(ed, 'C', plat::ModCtrl);
    CHECK(noteClipboard.size() == 1 && noteClipboard[0].note.chance == 40);
    ed.selected.clear();
    key(ed, 'V', plat::ModCtrl);
    CHECK(ed.notes.size() == 5);
    bool found = false;
    for (int i : ed.selected)
        found = found || (ed.notes[(size_t)i].chance == 40 && ed.notes[(size_t)i].pan == 0.25 && ed.notes[(size_t)i].slide);
    CHECK(found);
    ed.deleteOnClose = false;
    ed.close();
}

static void testSteps() {
    Fixture f;
    history::History h(f.project, f.engine.sr);
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    // the toggle in the toolbar, with a tooltip
    CHECK(ed.stepsButton && ed.stepsButton->isVisible() && !ed.stepsButton->toolTip().empty());
    CHECK(!ed.stepsView && ed.roll->isVisible() && !ed.steps->isVisible());
    ed.stepsButton->click();
    App::get().tick();
    CHECK(ed.stepsView && !ed.roll->isVisible() && ed.steps->isVisible() && ed.stepsButton->isChecked());
    CHECK(ed.steps->width() > 200 && ed.steps->height() > 100);
    // a step is a 1/16 (the grid follows the zoom), STEP_CELL_PX wide
    double step = ed.stepS(0);
    CHECK_NEAR(step, 0.125, 1e-9);
    CHECK_NEAR(step * ed.pps, STEP_CELL_PX, 1e-6);
    // rows: the pitches used, top to bottom
    CHECK(ed.stepRows() == (std::vector<int>{42, 38, 36}));
    CHECK(f.changes == 0);  // (just looking)
    savePicture(ed, "steps");
    StepGrid* grid = ed.steps;
    auto cellPoint = [&](int row, int column) {
        return Point((int)ed.xOf((column + 0.5) * step), *grid->yOfRow(row) + STEP_ROW_H / 2);
    };
    // a click on an empty pad: a note one step long there
    click(grid, cellPoint(2, 2));  // (row 2: pitch 36, at 0.25 s)
    CHECK(f.changes == 1 && ed.notes.size() == 5);
    int added = indexOf(ed, 0.25, 36);
    CHECK(added >= 0);
    if (added >= 0) {
        CHECK_NEAR(ed.notes[(size_t)added].end - ed.notes[(size_t)added].start, step, 1e-9);
        CHECK(ed.notes[(size_t)added].velocity == ed.lastVelocity);
    }
    CHECK(f.clip->notes->size() == 5);  // (into the clip: ordinary notes)
    // again: it goes
    click(grid, cellPoint(2, 2));
    CHECK(f.changes == 2 && ed.notes.size() == 4 && indexOf(ed, 0.25, 36) < 0);
    // a note written in the piano roll is lit where it starts; a click there removes it
    click(grid, cellPoint(1, 4));  // (pitch 38 at 0.5 s)
    CHECK(f.changes == 3 && ed.notes.size() == 3 && indexOf(ed, 0.5, 38) < 0);
    CHECK(ed.stepRows() == (std::vector<int>{42, 38, 36}));  // (its row stays while the editor is open)
    // drag along a row: every pad passed lights up, one commit
    press(grid, cellPoint(0, 8));
    dragTo(grid, cellPoint(0, 9));
    dragTo(grid, cellPoint(0, 10));
    dragTo(grid, cellPoint(0, 11));
    CHECK(f.changes == 3);
    release(grid, cellPoint(0, 11));
    CHECK(f.changes == 4);
    for (int c = 8; c <= 11; c++) CHECK(indexOf(ed, c * step, 42) >= 0);
    CHECK(f.clip->notes->size() == 3 + 4);
    // dragging from a lit pad clears instead
    CHECK(h.check(f.engine.sr));  // (as the main window does now and then: the edits so far are a step)
    press(grid, cellPoint(0, 9));
    dragTo(grid, cellPoint(0, 10));
    release(grid, cellPoint(0, 10));
    CHECK(indexOf(ed, 9 * step, 42) < 0 && indexOf(ed, 10 * step, 42) < 0 && indexOf(ed, 8 * step, 42) >= 0);
    // undo puts it back, as any note edit
    CHECK(h.check(f.engine.sr));
    size_t before = f.clip->notes->size();
    CHECK(h.undo(f.engine.sr));
    CHECK(f.clip->notes->size() == before + 2);
    ed.reload();
    CHECK(indexOf(ed, 9 * step, 42) >= 0);
    // Ctrl+click selects a lit pad's notes; the lane works on them as in the piano roll
    click(grid, cellPoint(0, 8), plat::LeftButton, plat::ModCtrl);
    click(grid, cellPoint(0, 9), plat::LeftButton, plat::ModCtrl);
    CHECK(ed.selected.size() == 2);
    ed.setLane(Lane::Chance);
    VelocityLane* lane = ed.velocity;
    int y0 = lane->height() / 2;
    press(lane, Point((int)ed.xOf(8 * step) + 2, 3));
    CHECK(ed.drag.kind == MidiEditor::Drag::LaneShift);
    dragTo(lane, Point((int)ed.xOf(8 * step) + 2, y0));
    release(lane, Point((int)ed.xOf(8 * step) + 2, y0));
    for (int i : ed.selected) CHECK(ed.notes[(size_t)i].chance < 60 && ed.notes[(size_t)i].chance > 40);
    key(ed, 'G');
    for (int i : ed.selected) CHECK(ed.notes[(size_t)i].slide);
    savePicture(ed, "steps_props");
    // right-click a lit pad: its notes go
    size_t count = ed.notes.size();
    click(grid, cellPoint(0, 9), plat::RightButton);
    CHECK(ed.notes.size() == count - 1);
    // "+ Row": a menu of pitches; a row for another one
    int plusY = *grid->yOfRow((int)ed.stepRows().size()) + STEP_ROW_H / 2;
    CHECK(grid->rowAt(plusY) == (int)ed.stepRows().size());
    CHECK(!grid->toolTipAt(Point(10, plusY)).empty());
    click(grid, Point(10, plusY));
    CHECK(grid->rowMenu->isOpen() && grid->rowMenu->actions().size() > 10);
    grid->rowMenu->close();
    Menu::activate(grid->rowMenu->actions()[0].get());  // "Above: G#2" (one above the top row)
    CHECK(ed.stepRows().front() == 43);
    ed.addStepRow(49);
    CHECK(ed.stepRows() == (std::vector<int>{49, 43, 42, 38, 36}));
    int row49 = 0;
    click(grid, cellPoint(row49, 0));
    CHECK(indexOf(ed, 0.0, 49) >= 0 && (*f.clip->notes).size() == ed.notes.size());
    // a pad wholly before the clip's notes start can't be lit; the clip's MIDI effects and playing are as ever
    CHECK(!ed.setCell(49, -1.0, true));
    CHECK(f.clip->played && f.clip->played->size() >= f.clip->notes->size() - 1);
    // the keys work there too (Ctrl+A, Delete)
    key(ed, 'A', plat::ModCtrl);
    CHECK(ed.selected.size() == ed.notes.size());
    // back to the piano roll: the same notes
    ed.stepsButton->click();
    App::get().tick();
    CHECK(!ed.stepsView && ed.roll->isVisible() && !ed.steps->isVisible());
    CHECK(ed.notes.size() == f.clip->notes->size());
    savePicture(ed, "roll_after_steps");
    ed.deleteOnClose = false;
    ed.close();
}

static void testStepsEmptyClip() {
    Fixture f;
    f.clip->notes = std::make_shared<const std::vector<Note>>();
    f.clip->prepare(f.engine.sr);
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    ed.setStepsView(true);
    App::get().tick();
    CHECK(ed.stepRows() == (std::vector<int>{STEP_FIRST_PITCH}));  // (somewhere to start)
    double step = ed.stepS(0);
    Point p((int)ed.xOf(step * 0.5), *ed.steps->yOfRow(0) + STEP_ROW_H / 2);
    click(ed.steps, p);
    CHECK(f.clip->notes->size() == 1 && (*f.clip->notes)[0].pitch == STEP_FIRST_PITCH);
    // a fixed grid of a beat: the steps follow it
    f.project.timing.grid = 1;
    ed.setStepsView(false);
    ed.setStepsView(true);
    CHECK_NEAR(ed.stepS(0), 0.5, 1e-9);
    CHECK_NEAR(ed.stepS(0) * ed.pps, STEP_CELL_PX, 1e-6);
    click(ed.steps, Point((int)ed.xOf(1.25), *ed.steps->yOfRow(0) + STEP_ROW_H / 2));
    CHECK(f.clip->notes->size() == 2 && std::fabs((*f.clip->notes)[1].start - 1.0) < 1e-9 &&
          std::fabs((*f.clip->notes)[1].end - 1.5) < 1e-9);
    savePicture(ed, "steps_beats");
    ed.deleteOnClose = false;
    ed.close();
}

int main() {
    App::get().initHeadless();
    util::makeDirs("build/test-steps");
    std::remove("build/test-steps/settings.json");
    settings::setPath("build/test-steps/settings.json");
    midi_effects::install(nullptr);
    testLaneChooser();
    testLaneEditingAndUndo();
    testSlide();
    testSteps();
    testStepsEmptyClip();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
