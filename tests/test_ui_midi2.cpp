// The successor's MIDI features, headless: the MIDI editor's ghost notes, the song's scale (shading,
// folding, Fit to scale), the chord stamp, box select, arrow keys, copy / paste across clips, Ctrl+drag
// copies, Quantize amounts, switched-off notes, loops lengthened by whole bars; the computer keyboard,
// MIDI input routing (sustain pedal), Capture's phrases and placement, MIDI export (read back), the MIDI
// effects window and the sampler window; a stylesheet restyling a meter's and the piano roll's parts and sizes.
// Paints PNGs into build/ui-samples/midi2/. No sound: the engine is never opened.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/model.h"
#include "ui/capture.h"
#include "ui/computer_keyboard.h"
#include "ui/level_meter.h"
#include "ui/midi_editor.h"
#include "ui/midi_effects_dialog.h"
#include "ui/midi_export.h"
#include "ui/midi_input.h"
#include "ui/sampler_dialog.h"
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
void keyDown(Window& w, int k, int mods = 0, bool repeat = false) {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k;
    e.mods = mods;
    e.autoRepeat = repeat;
    e.time = gTime;
    w.onEvent(nullptr, e);
}
void keyUp(Window& w, int k, int mods = 0) {
    plat::Event e;
    e.type = plat::Event::KeyUp;
    e.key = k;
    e.mods = mods;
    e.time = gTime;
    w.onEvent(nullptr, e);
}
void key(Window& w, int k, int mods = 0) {
    keyDown(w, k, mods);
    keyUp(w, k, mods);
}

std::string outDir() {
    std::string d = "build/ui-samples/midi2";
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
    TrackPtr track, other;
    ClipPtr clip, second;
    int changes = 0;
    int64_t cursor = 0;
    std::vector<ClipPtr> edited;
    Fixture() {
        project.timing.bpm = 120;  // a beat = 0.5 s, a bar = 2 s
        project.timing.grid = 1;   // a fixed grid of a beat, whatever the zoom
        track = project.addTrack(Kind::Midi);
        track->name = "Piano";
        track->needsInstrument = false;
        other = project.addTrack(Kind::Midi);
        other->name = "Bass";
        other->needsInstrument = false;
        clip = makeMidiClip("Melody", {note(0, 0.5, 60, 100), note(0.5, 1.0, 64, 80), note(1.0, 2.0, 67, 120),
                                       note(2.0, 2.25, 72, 60)},
                            4.0, engine.sr);
        clip->track = track;
        clip->start = 0;
        project.addClip(clip);
        second = makeMidiClip("Bassline", {note(0, 1.0, 48), note(1.0, 2.0, 55)}, 2.0, engine.sr);
        second->track = other;
        second->start = engine.sr * 1;  // 1 s into the timeline
        second->prepare(engine.sr);
        project.addClip(second);
    }
    MidiEditor::TimelineLink link() {
        MidiEditor::TimelineLink l;
        l.cursor = [this] { return cursor; };
        l.setCursor = [this](int64_t f) { cursor = f; };
        l.seek = [this](int64_t) {};
        l.editNotes = [this](ClipPtr c) { edited.push_back(c); };
        return l;
    }
};

Point notePoint(MidiEditor& ed, int index, double along = 0.5) {
    const Note& n = ed.notes[index];
    return Point((int)(ed.xOf(n.start + (n.end - n.start) * along)), (int)(*ed.yOf(n.pitch) + ed.rowH / 2));
}

int indexOfPitch(MidiEditor& ed, int pitch) {
    for (size_t i = 0; i < ed.notes.size(); ++i)
        if ((int)ed.notes[i].pitch == pitch) return (int)i;
    return -1;
}
}  // namespace

// ------------------------------------------------------------------------------------------
static void testGhosts() {
    Fixture f;
    settings::set("options/ghost_notes", Json("all"));
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK(ed.ghostMode == "all");
    // the other clip's notes, in this clip's note time (it starts 1 s later), within its window
    const auto& g = ed.ghosts();
    CHECK(g.size() == 1);
    if (g.size() == 1) {
        CHECK(g[0].clip == f.second);
        CHECK(g[0].notes.size() == 2);
        CHECK_NEAR(g[0].notes[0].start, 1.0, 1e-9);
        CHECK_NEAR(g[0].notes[0].end, 2.0, 1e-9);
        CHECK_NEAR(g[0].notes[1].start, 2.0, 1e-9);
        CHECK((int)g[0].notes[0].pitch == 48);
    }
    // hit testing a ghost; the editor of its clip via the timeline
    ed.vbar->setValue((int)(*ed.yOf(48) + ed.vbar->value() - 100));
    double y = *ed.yOf(48) + 3;
    CHECK(ed.ghostAt(ed.xOf(1.5), y) == f.second);
    CHECK(ed.ghostAt(ed.xOf(0.5), y) == nullptr);
    ed.editOther(f.second);
    CHECK(f.edited.size() == 1 && f.edited[0] == f.second);
    // "this track's other clips": none on Piano
    ed.setGhostMode("track");
    CHECK(ed.ghosts().empty());
    CHECK(settings::get("options/ghost_notes").toString("") == "track");
    // a clip of the same track shows then; a looping one with its passes repeated; a disabled one not
    ClipPtr loop = makeMidiClip("Loop", {note(0, 0.25, 50)}, 1.0, f.engine.sr);
    loop->track = f.track;
    loop->start = f.engine.sr * 4;
    loop->loop = true;
    loop->length = f.engine.sr * 3;
    loop->prepare(f.engine.sr);
    f.project.addClip(loop);
    CHECK(ed.ghosts().size() == 1);
    if (!ed.ghosts().empty()) {
        CHECK(ed.ghosts()[0].notes.size() == 3);  // (3 s of a 1 s loop)
        CHECK_NEAR(ed.ghosts()[0].notes[2].start, 4.0 + 2.0, 1e-6);
    }
    loop->disabled = true;
    CHECK(ed.ghosts().empty());
    ed.setGhostMode("off");
    loop->disabled = false;
    CHECK(ed.ghosts().empty());
    ed.setGhostMode("all");
    CHECK(ed.ghosts().size() == 2);
    // the menu shows the mode checked
    CHECK(ed.ghostActions["all"]->checked && !ed.ghostActions["off"]->checked);
    ed.fit();
    ed.vbar->setValue((int)(ed.nearestRow(58) * ed.rowH - ed.roll->height() / 2));
    savePicture(ed, "ghosts");
    ed.close();
}

static void testScaleAndFold() {
    // the scale helpers (midi_effects)
    Steps dMajor = scaleSteps(2, "Major");
    CHECK(dMajor == (Steps{2, 4, 6, 7, 9, 11, 1}));
    CHECK(scaleSteps(0, "Nope").empty());
    CHECK(nearestInScale(61, scaleSteps(0, "Major")) == 60);  // C# → C (the lower when as near)
    CHECK(nearestInScale(66, scaleSteps(0, "Major")) == 65);
    CHECK(inScaleChord(62, scaleSteps(0, "Major"), 3) == (std::vector<int>{62, 65, 69}));
    CHECK(inScaleChord(61, scaleSteps(0, "Major"), 4) == (std::vector<int>{60, 64, 67, 71}));

    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK(!ed.scale());
    CHECK(ed.rows().size() == 128);
    CHECK(!ed.keyBox->isEnabled() && !ed.fitButton->isEnabled() && !ed.foldActions["scale"]->enabled);
    CHECK(ed.foldButton->text() == "Fold");
    // choose D major in the toolbar: saved in the song's timing
    ed.scaleBox->setCurrentIndex(ed.scaleBox->findData("Major"));
    ed.keyBox->setCurrentIndex(2);
    CHECK(f.project.timing.scale == "Major" && f.project.timing.key == 2);
    CHECK(ed.scale() && *ed.scale() == dMajor);
    CHECK(ed.keyBox->isEnabled() && ed.fitButton->isEnabled());
    // folded to the scale: only its rows, top to bottom
    ed.setFold("scale");
    CHECK(ed.foldButton->text() == "Folded: scale");
    const std::vector<int>& rows = ed.rows();
    CHECK(rows.size() == 75);  // 7 of 12 pitch classes over 0…127
    bool allIn = true;
    for (int p : rows) allIn = allIn && dMajor.count(p % 12);
    CHECK(allIn && rows.front() == 127 && rows.back() == 1);
    CHECK(!ed.yOf(60));  // C is folded away
    CHECK(ed.yOf(62) && ed.yOf(61) && *ed.yOf(61) - *ed.yOf(62) == ed.rowH);  // C# right under D
    savePicture(ed, "folded_scale");
    // folded: Up moves a row (a scale step), not a semitone
    int d = -1;
    ed.notes.push_back(note(3.0, 3.5, 62));
    d = (int)ed.notes.size() - 1;
    ed.selected = {d};
    key(ed, plat::Key_Up);
    CHECK(indexOfPitch(ed, 64) >= 0);  // D → E
    // Fit to scale: the selected notes (none: all) to the nearest scale note
    ed.selected.clear();
    ed.setFold("off");
    ed.fitToScale();
    std::set<int> pitches;
    for (const Note& n : ed.notes) pitches.insert((int)n.pitch);
    CHECK(pitches == (std::set<int>{59, 64, 67, 71}));  // C → B (B and C# as near: the lower), C5 → B4
    // fold to the notes used
    ed.setFold("used");
    CHECK(ed.rows() == (std::vector<int>{71, 67, 64, 59}));
    CHECK(ed.foldButton->text() == "Folded: used");
    // the scale off: fold to the scale goes too, the "In the scale" stamps too
    ed.setFold("scale");
    ed.setStamp("In the scale: triad");
    f.project.timing.scale = "";
    ed.followCursors();  // (another editor changed it: noticed)
    CHECK(ed.fold == "off" && ed.rows().size() == 128);
    CHECK(ed.stamp == "Off" && !ed.stampActions["In the scale: triad"]->enabled);
    ed.close();
}

static void testStamp() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK(stamps().front() == "Off" && stamps().back() == "In the scale: 7th");
    CHECK(std::find(stamps().begin(), stamps().end(), "Octave down") == stamps().end());
    CHECK(ed.stamped(60) == std::vector<int>{60});
    ed.setStamp("Major");
    CHECK(ed.stampButton->text() == "Major");
    CHECK(ed.stamped(60) == (std::vector<int>{60, 64, 67}));
    CHECK(ed.stamped(125) == (std::vector<int>{125}));  // (beyond 127: left out)
    // a click places the whole chord, all selected, the root held
    size_t before = ed.notes.size();
    double t = 3.0;
    Point p((int)ed.xOf(t + 0.1), (int)*ed.yOf(62) + 3);
    press(ed.roll, p);
    CHECK(ed.notes.size() == before + 3);
    CHECK(ed.held && *ed.held == 62);
    dragTo(ed.roll, Point((int)ed.xOf(t + 1.6), p.y));  // dragging lengthens all of it
    release(ed.roll, Point((int)ed.xOf(t + 1.6), p.y));
    CHECK(ed.selected.size() == 3);
    for (int i : ed.selected) {
        CHECK_NEAR(ed.notes[i].start, 3.0, 1e-9);
        CHECK_NEAR(ed.notes[i].end, 4.5, 1e-9);
    }
    CHECK(indexOfPitch(ed, 66) >= 0 && indexOfPitch(ed, 69) >= 0);
    // in the scale: needs one; C major, a 7th on D: D F A C
    f.project.timing.scale = "Major";
    f.project.timing.key = 0;
    ed.syncScaleTools();
    ed.setStamp("In the scale: 7th");
    CHECK(ed.stampButton->text() == "7th");
    CHECK(ed.stamped(62) == (std::vector<int>{62, 65, 69, 72}));
    ed.setStamp("In the scale: triad");
    CHECK(ed.stamped(64) == (std::vector<int>{64, 67, 71}));
    // folded: rows folded away get none
    ed.setStamp("Major");
    ed.setFold("scale");
    CHECK(ed.stamped(62) == (std::vector<int>{62, 69}));  // (F# is not in C major)
    ed.close();
}

static void testBoxSelectAndCopyDrag() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    // Shift+drag from an empty spot over notes 0 and 1 (0…1 s, 60 and 64)
    Point a((int)ed.xOf(0.2), (int)*ed.yOf(65) + 2), b((int)ed.xOf(0.9), (int)*ed.yOf(59) + 2);
    press(ed.roll, a, plat::LeftButton, plat::ModShift);
    CHECK(ed.drag.kind == MidiEditor::Drag::Box);
    dragTo(ed.roll, b, plat::LeftButton, plat::ModShift);
    CHECK(ed.selected == (std::set<int>{0, 1}));
    savePicture(ed, "box_select");
    release(ed.roll, b, plat::LeftButton, plat::ModShift);
    CHECK(ed.drag.kind == MidiEditor::Drag::None);
    CHECK(f.changes == 0);
    // Ctrl+Shift adds to the selection
    Point c((int)ed.xOf(1.2), (int)*ed.yOf(68) + 2), d((int)ed.xOf(1.4), (int)*ed.yOf(66) + 2);
    press(ed.roll, c, plat::LeftButton, plat::ModShift | plat::ModCtrl);
    dragTo(ed.roll, d, plat::LeftButton, plat::ModShift | plat::ModCtrl);
    release(ed.roll, d, plat::LeftButton, plat::ModShift | plat::ModCtrl);
    CHECK(ed.selected == (std::set<int>{0, 1, 2}));
    // Ctrl+drag a note: copies of the selection are dropped there, the notes stay
    ed.selected = {0, 1};
    Point from = notePoint(ed, 1, 0.5);
    press(ed.roll, from, plat::LeftButton, plat::ModCtrl);
    Point to(from.x + (int)(2 * ed.pps), from.y);  // 2 s later
    dragTo(ed.roll, Point(from.x + 1, from.y), plat::LeftButton, plat::ModCtrl);  // (under the threshold)
    CHECK(ed.notes.size() == 4);
    dragTo(ed.roll, to, plat::LeftButton, plat::ModCtrl);
    CHECK(ed.notes.size() == 6);
    release(ed.roll, to, plat::LeftButton, plat::ModCtrl);
    CHECK(f.changes == 1);
    CHECK(f.clip->notes->size() == 6);
    CHECK(ed.selected.size() == 2);
    for (int i : ed.selected) CHECK(ed.notes[i].start >= 2.0 - 1e-9);
    // Ctrl+click without moving: in or out of the selection
    ed.selected = {0};
    click(ed.roll, notePoint(ed, 0), plat::LeftButton, plat::ModCtrl);
    CHECK(ed.selected.empty());
    ed.close();
}

static void testArrows() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    double step = ed.stepS();
    CHECK_NEAR(step, 0.5, 1e-12);  // (the fixed grid: a beat)
    ed.selected = {1};  // 0.5–1.0, 64
    key(ed, plat::Key_Right);
    CHECK_NEAR(ed.notes[1].start, 1.0, 1e-9);
    CHECK(f.changes == 1);
    key(ed, plat::Key_Left);
    key(ed, plat::Key_Left);
    CHECK_NEAR(ed.notes[ed.selected.count(0) ? 0 : *ed.selected.begin()].start, 0.0, 1e-9);
    int changes = f.changes;
    key(ed, plat::Key_Left);  // (nothing before the clip's start: no change)
    CHECK(f.changes == changes);
    int i = *ed.selected.begin();
    key(ed, plat::Key_Up);
    CHECK((int)ed.notes[*ed.selected.begin()].pitch == 65);
    key(ed, plat::Key_Down, 0);
    key(ed, plat::Key_Down, 0);
    CHECK((int)ed.notes[*ed.selected.begin()].pitch == 63);
    key(ed, plat::Key_Up, plat::ModShift);
    CHECK((int)ed.notes[*ed.selected.begin()].pitch == 75);
    i = *ed.selected.begin();
    double len = ed.notes[i].end - ed.notes[i].start;
    key(ed, plat::Key_Right, plat::ModShift);
    i = *ed.selected.begin();
    CHECK_NEAR(ed.notes[i].end - ed.notes[i].start, len + 0.5, 1e-9);
    key(ed, plat::Key_Left, plat::ModShift);
    key(ed, plat::Key_Left, plat::ModShift);
    i = *ed.selected.begin();
    CHECK_NEAR(ed.notes[i].end - ed.notes[i].start, 0.5, 1e-9);  // (never shorter than a step)
    // an octave out of range: nothing
    ed.notes[i].pitch = 120;
    changes = f.changes;
    key(ed, plat::Key_Up, plat::ModShift);
    CHECK(f.changes == changes && (int)ed.notes[i].pitch == 120);
    // the roll claims the editing keys before any shortcut (Delete, arrows, 0, Ctrl+C…); not Space
    KeyEvent e;
    e.key = plat::Key_Left;
    CHECK(ed.roll->overridesShortcut(e));
    e.key = '0';
    CHECK(ed.roll->overridesShortcut(e));
    e.key = 'C';
    e.mods = plat::ModCtrl;
    CHECK(ed.roll->overridesShortcut(e));
    e.key = plat::Key_Space;
    e.mods = 0;
    CHECK(!ed.roll->overridesShortcut(e));
    e.key = '1';
    e.mods = plat::ModCtrl;  // (Ctrl+1/2/4/5: the transport's grid keys, application-wide)
    CHECK(!ed.roll->overridesShortcut(e));
    ed.close();
}

static void testCopyPasteAcrossClips() {
    Fixture f;
    noteClipboard.clear();
    MidiEditor a(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    a.showWindow();
    a.selected = {1, 2};  // 0.5–1.0 64, 1.0–2.0 67
    a.off = {2};
    key(a, 'C', plat::ModCtrl);
    CHECK(noteClipboard.size() == 2);
    CHECK_NEAR(noteClipboard[0].start, 0.0, 1e-12);
    CHECK_NEAR(noteClipboard[1].start, 0.5, 1e-12);
    CHECK(!noteClipboard[0].off && noteClipboard[1].off);
    // into the other clip, at the selection cursor (over that clip: 1.5 s on the timeline = 0.5 s in its notes)
    MidiEditor b(nullptr, f.second, f.project, f.engine, [&] { f.changes++; }, f.link());
    b.showWindow();
    f.cursor = f.engine.sr * 3 / 2;
    key(b, 'V', plat::ModCtrl);
    CHECK(b.notes.size() == 4);
    CHECK(b.selected.size() == 2);
    bool at = false;
    for (int i : b.selected)
        if ((int)b.notes[i].pitch == 64 && std::fabs(b.notes[i].start - 0.5) < 1e-9) at = true;
    CHECK(at);
    CHECK(f.second->notes->size() == 3);  // (the switched-off one kept apart)
    CHECK(f.second->mutedNotes && f.second->mutedNotes->size() == 1);
    // the cursor not over the clip: after its last note
    f.cursor = f.engine.sr * 30;
    key(b, 'V', plat::ModCtrl);
    double last = 0;
    for (int i : b.selected) last = std::max(last, b.notes[i].start);
    CHECK_NEAR(last, 2.0 + 0.5, 1e-9);
    // Ctrl+X: copied and gone
    a.selected = {0};
    key(a, 'X', plat::ModCtrl);
    CHECK(a.notes.size() == 3 && noteClipboard.size() == 1 && (int)noteClipboard[0].pitch == 60);
    a.close();
    b.close();
}

static void testQuantize() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    ed.notes = {note(0.1, 0.3, 60), note(0.9, 1.2, 62), note(1.65, 1.8, 64)};
    ed.off.clear();
    ed.selected.clear();
    ed.quantize(0.5);  // none selected: all, half way to the grid (a beat: 0.5 s)
    CHECK_NEAR(ed.notes[0].start, 0.05, 1e-9);
    CHECK_NEAR(ed.notes[0].end, 0.25, 1e-9);  // (lengths kept)
    CHECK_NEAR(ed.notes[1].start, 0.95, 1e-9);
    CHECK_NEAR(ed.notes[2].start, 1.575, 1e-9);
    CHECK(f.changes == 1);
    // the menu: 100 / 75 / 50 / 25 %, ends too
    Menu* menu = ed.quantizeButton->menu();
    CHECK(menu && menu->actions().size() == 6);
    CHECK(menu->actions()[1]->text == "Quantize 75 %");
    ed.selected = {0};
    Menu::activate(menu->actions()[0].get());  // 100 %
    CHECK_NEAR(ed.notes[*ed.selected.begin()].start, 0.0, 1e-9);
    // the ends too (never shorter than a grid step)
    ed.quantizeEnds->checked = true;
    ed.selected.clear();
    ed.quantize(1.0);
    for (const Note& n : ed.notes) {
        CHECK_NEAR(std::fmod(n.start + 1e-9, 0.5), 0.0, 1e-6);
        CHECK(n.end - n.start >= 0.5 - 1e-9);
        CHECK_NEAR(std::fmod(n.end + 1e-9, 0.5), 0.0, 1e-6);
    }
    // Ctrl+U = 100 %
    ed.quantizeEnds->checked = false;
    ed.notes = {note(0.2, 0.4, 60)};
    ed.selected = {0};
    key(ed, 'U', plat::ModCtrl);
    CHECK_NEAR(ed.notes[0].start, 0.0, 1e-9);
    ed.close();
}

static void testSwitchedOff() {
    Fixture f;
    {
        MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
        ed.showWindow();
        ed.selected = {0, 2};
        key(ed, '0');
        CHECK(ed.off == (std::set<int>{0, 2}));
        CHECK(f.clip->notes->size() == 2);
        CHECK(f.clip->mutedNotes && f.clip->mutedNotes->size() == 2);
        // switched off: not played, so not heard (nor exported)
        CHECK(f.clip->heardNotes(f.engine.sr).size() == 2);
        savePicture(ed, "switched_off");
        // 0 again (all selected are off): back on
        key(ed, '0');
        CHECK(ed.off.empty() && f.clip->notes->size() == 4 && !f.clip->mutedNotes);
        ed.selected = {3};
        ed.toggleOff();
        // a mixed selection: all off
        ed.selected = {2, 3};
        ed.switchButton->click();
        CHECK(ed.off.size() == 2);
        // deleting one keeps the others' marks
        ed.deleteNotes({0});
        CHECK(ed.off.size() == 2 && ed.notes.size() == 3);
        ed.close();
    }
    // reopened: the switched-off notes come back, marked off
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    CHECK(ed.notes.size() == 3 && ed.off.size() == 2);
    // undo replaced the clip's notes: reload takes them again
    f.clip->notes = std::make_shared<const std::vector<Note>>(std::vector<Note>{note(0, 1, 50)});
    f.clip->mutedNotes = nullptr;
    ed.reload();
    CHECK(ed.notes.size() == 1 && ed.off.empty() && ed.selected.empty());
    ed.deleteOnClose = false;
    ed.close();
}

static void testLoopLengthening() {
    Fixture f;
    ClipPtr loop = makeMidiClip("Loop", {note(0, 0.5, 60), note(1.0, 1.5, 62)}, 2.0, f.engine.sr);
    loop->track = f.track;
    loop->start = f.engine.sr * 10;
    loop->loop = true;
    loop->length = f.engine.sr * 8;  // 4 passes of a bar
    loop->prepare(f.engine.sr);
    f.project.addClip(loop);
    MidiEditor ed(nullptr, loop, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK_NEAR(ed.wholeBars(2.3), 4.0, 1e-9);
    CHECK_NEAR(ed.wholeBars(2.0), 2.0, 1e-9);
    savePicture(ed, "loop_passes");
    // a note past the loop: it lengthens by whole bars (2 s each), the window stays
    ed.notes.push_back(note(2.1, 2.3, 64));
    ed.commit();
    CHECK_NEAR(loop->lengthS, 4.0, 1e-9);
    CHECK(loop->length == f.engine.sr * 8);
    // its notes repeat every 4 s now
    std::vector<Note> rep = loop->repeated(f.engine.sr);
    CHECK(rep.size() == 6);
    // with a tempo change at bar 2 (60 BPM there), bars are counted through it
    f.project.timing.setChange(7, 60, 4);  // bar 7 (from 12 s on the timeline: note time 2) is 4 s long
    CHECK_NEAR(ed.wholeBars(2.1), 6.0, 1e-9);
    f.project.timing.removeChange(7);
    // a clip that doesn't loop grows to its last note, its window too (it replaces what it covers)
    ed.close();
    MidiEditor plain(nullptr, f.second, f.project, f.engine, [&] { f.changes++; }, f.link());
    f.second->length = f.engine.sr * 2;
    plain.notes.push_back(note(2.5, 3.25, 60));
    plain.commit();
    CHECK_NEAR(f.second->lengthS, 3.25, 1e-9);
    CHECK(f.second->length == (int64_t)std::nearbyint(3.25 * f.engine.sr));
    plain.deleteOnClose = false;
    plain.close();
}

// ------------------------------------------------------------------------------------------
static void testComputerKeyboard() {
    using namespace computer_keyboard;
    CHECK(noteOf('A', 4) == 60);
    CHECK(noteOf('W', 4) == 61);
    CHECK(noteOf('K', 4) == 72);
    CHECK(noteOf(';', 4) == 76);
    CHECK(noteOf('\'', 4) == 77);
    CHECK(noteOf('P', 4) == 75);
    CHECK(noteOf('Q', 4) == -1);
    CHECK(noteOf('A', 0) == 12);
    CHECK(computer_keyboard::noteName(60) == "C4");
    Fixture f;
    midi_input::MidiInput input(f.engine, f.project, [&] { return f.track; }, nullptr);
    double clock = 10;
    input.clock = [&] { return clock; };
    std::vector<std::string> messages;
    ComputerKeyboard kb(input, [&](const std::string& m, int) { messages.push_back(m); });
    CHECK(active() == &kb);
    std::string shown;
    kb.showState = [&](bool visible, const std::string& text) { shown = visible ? text : ""; };
    kb.octave = 4;
    kb.velocity = 100;
    kb.setOn(true);
    CHECK(kb.isOn() && shown == "\xe2\x8c\xa8 C4  \xc2\xb7  velocity 100");
    CHECK(!messages.empty() && messages.back().find("Computer keyboard: plays the selected MIDI track") == 0);
    CHECK(settings::get("options/computer_keyboard").toBool(false));
    // the keys a plugin's window grabs while it is on (so they play notes even while a Windows plugin has
    // the keys): its 18 note keys and 4 setting keys, nothing else
    {
        auto keys = keysInUse();
        auto has = [&](int k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };
        CHECK(keys.size() == 22);
        CHECK(has('A') && has('W') && has(';') && has('\'') && has('Z') && has('X') && has('C') && has('V'));
        CHECK(!has('Q') && !has('R') && !has(' ') && !has('B'));
    }
    // the letters reach it before the window's shortcuts
    Window win;
    win.showWindow();
    int splits = 0;
    Shortcut s(&win, KeySequence('S'), [&] { splits++; });
    Shortcut space(&win, KeySequence(plat::Key_Space), [&] { splits += 100; });
    auto send = [&](plat::Event::Type t, int k, int mods = 0, bool repeat = false) {
        plat::Event e;
        e.type = t;
        e.key = k;
        e.mods = mods;
        e.autoRepeat = repeat;
        if (!filter(&win, e)) win.onEvent(nullptr, e);
    };
    send(plat::Event::KeyDown, 'S');
    CHECK(splits == 0);
    CHECK(kb.held().count('S') && kb.held().at('S') == 62);
    CHECK(input.log.size() == 1 && input.log.back().on && input.log.back().pitch == 62 && input.log.back().velocity == 100);
    send(plat::Event::KeyDown, 'S', 0, true);  // (auto-repeat: nothing new)
    CHECK(input.log.size() == 1);
    send(plat::Event::KeyUp, 'S');
    CHECK(kb.held().empty() && input.log.size() == 2 && !input.log.back().on);
    send(plat::Event::KeyDown, plat::Key_Space);  // other keys work as always
    CHECK(splits == 100);
    send(plat::Event::KeyDown, 'S', plat::ModCtrl);  // Ctrl+S stays a shortcut (here: none) — not a note
    CHECK(kb.held().empty());
    // octave and velocity
    send(plat::Event::KeyDown, 'X');
    CHECK(kb.octave == 5 && shown.find("C5") != std::string::npos);
    send(plat::Event::KeyDown, 'A');
    CHECK(kb.held().at('A') == 72);
    send(plat::Event::KeyDown, 'Z');  // (the octave changes while A is held: A lets go of its own note)
    send(plat::Event::KeyUp, 'A');
    CHECK(input.log.back().pitch == 72 && !input.log.back().on);
    kb.velocity = 1;
    send(plat::Event::KeyDown, 'V');
    CHECK(kb.velocity == 20);
    send(plat::Event::KeyDown, 'V');
    CHECK(kb.velocity == 40);
    kb.velocity = 120;
    send(plat::Event::KeyDown, 'V');
    CHECK(kb.velocity == 127);
    send(plat::Event::KeyDown, 'C');
    CHECK(kb.velocity == 107);
    CHECK(settings::get("options/keyboard_velocity").toInt(0) == 107);
    // typing in a text field: the keys are its
    LineEdit* edit = win.add<LineEdit>();
    edit->setGeometry(0, 0, 100, 20);
    edit->setFocus();
    size_t logged = input.log.size();
    send(plat::Event::KeyDown, 'D');
    CHECK(input.log.size() == logged);
    win.setFocusWidget(nullptr);
    // the same note held by two keys: let go when both are
    kb.octave = 4;
    send(plat::Event::KeyDown, 'K');  // C5
    kb.octave = 5;
    send(plat::Event::KeyDown, 'A');  // C5 again
    logged = input.log.size();
    send(plat::Event::KeyUp, 'K');
    CHECK(input.log.size() == logged);
    send(plat::Event::KeyUp, 'A');
    CHECK(input.log.size() == logged + 1);
    // off: the letters are shortcuts again
    kb.setOn(false);
    send(plat::Event::KeyDown, 'S');
    CHECK(splits == 101);
    CHECK(keysInUse().empty());  // (off: a plugin's window grabs none)
    // the MIDI editor passes its keys through it too
    kb.setOn(true, false);
    kb.octave = 4;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    logged = input.log.size();
    keyDown(ed, 'G');
    CHECK(input.log.size() == logged + 1 && input.log.back().pitch == 55 + 12);
    keyUp(ed, 'G');
    ed.selected = {0};
    key(ed, 'D', plat::ModCtrl);  // (Ctrl+D still duplicates)
    CHECK(ed.notes.size() == 5);
    ed.close();
}

static void testMidiInputRouting() {
    Fixture f;
    TrackPtr current = f.track;
    std::vector<std::string> messages;
    midi_input::MidiInput input(f.engine, f.project, [&] { return current; },
                                [&](const std::string& m, int) { messages.push_back(m); });
    double clock = 50;
    input.clock = [&] { return clock; };
    std::vector<midi_in::Event> recorded;
    input.recorder = [&](const midi_in::Event& e) { recorded.push_back(e); };
    CHECK(input.target() == f.track);
    input.handle(midi_in::Event::noteOn(60, 90));
    CHECK(recorded.size() == 1 && input.log.size() == 1);
    // the sustain pedal holds a key let go
    input.handle(midi_in::Event::control(midi_in::SUSTAIN, 127));
    input.handle(midi_in::Event::noteOff(60));
    CHECK(recorded.size() == 1);
    clock = 51;
    input.handle(midi_in::Event::control(midi_in::SUSTAIN, 0));
    CHECK(recorded.size() == 2 && recorded.back().type == midi_in::Event::NoteOff);
    CHECK(input.log.back().clock == 51 && !input.log.back().on);
    // no MIDI track selected: a message, nothing heard (still logged for Capture)
    current = nullptr;
    input.handle(midi_in::Event::noteOn(62, 90));
    CHECK(!messages.empty() && messages.back().find("To play notes, select a MIDI track") == 0);
    CHECK(input.log.size() == 3);
    // a frozen or removed track, or one waiting for an instrument, isn't a target
    current = f.track;
    f.track->needsInstrument = true;
    CHECK(input.target() == nullptr);
    f.track->needsInstrument = false;
    TrackPtr gone = std::make_shared<Track>("Gone", Kind::Midi);
    current = gone;
    CHECK(input.target() == nullptr);
    current = f.track;
    // releaseAll: every note sounding gets its note-off
    input.handle(midi_in::Event::noteOn(64, 90));
    size_t before = recorded.size();
    input.releaseAll();
    CHECK(recorded.size() >= before + 1);
    // the log forgets notes older than KEPT_S
    clock = 50 + capture::KEPT_S + 10;
    input.handle(midi_in::Event::noteOn(65, 90));
    CHECK(input.log.front().clock > 51);
    // the live notes go through the track's MIDI effects (Pitch +12)
    MidiEffect pitch;
    MidiEffect::make("Pitch", {{"semitones", 12}}, pitch);
    f.track->midiEffects = std::vector<MidiEffect>{pitch};
    auto sounding = midi_input::playLive(f.engine, f.track, 60, 100, 0.1);
    CHECK(sounding == std::vector<int>{72});
    MidiEffect chord;
    MidiEffect::make("Chord", {{"chord", 2}}, chord);  // Major
    f.track->midiEffects = std::vector<MidiEffect>{chord};
    CHECK(midi_input::playLive(f.engine, f.track, 60, 100, 0.1) == (std::vector<int>{60, 64, 67}));
    f.track->midiEffects = std::vector<MidiEffect>{};
    // a port that isn't there
    CHECK(!input.open("No Such Keyboard"));
    CHECK(input.name() == "No Such Keyboard");
    CHECK(input.open(""));
}

static void testCapture() {
    using midi_input::LogEntry;
    auto entry = [](double clock, bool on, int pitch, std::optional<int64_t> heard = std::nullopt, int vel = 100) {
        LogEntry e;
        e.clock = clock;
        e.on = on;
        e.pitch = pitch;
        e.velocity = vel;
        e.heard = heard;
        return e;
    };
    // notes_of: a held note ends `now`; one struck again ends there
    std::vector<LogEntry> log = {entry(1, true, 60), entry(1.5, false, 60), entry(2, true, 62), entry(2.5, true, 62),
                                 entry(3, false, 62), entry(3.2, true, 64)};
    auto notes = capture::notesOf(log, 4.0);
    CHECK(notes.size() == 4);
    CHECK_NEAR(notes[1].on, 2, 1e-12);
    CHECK_NEAR(notes[1].off, 2.5, 1e-12);
    CHECK_NEAR(notes[3].off, 4.0, 1e-12);
    // phrase: after the last pause of more than 4 s
    std::vector<LogEntry> two = {entry(1, true, 60), entry(1.5, false, 60), entry(10, true, 62), entry(10.5, false, 62),
                                 entry(11, true, 64), entry(11.5, false, 64)};
    auto p = capture::phrase(two, 20);
    CHECK(p.size() == 2 && p[0].pitch == 62);
    CHECK(capture::phrase({}, 5).empty());
    // a long note bridges the pause (nothing held is what counts)
    std::vector<LogEntry> held = {entry(1, true, 60), entry(9, false, 60), entry(10, true, 62), entry(10.5, false, 62)};
    CHECK(capture::phrase(held, 20).size() == 2);

    Timing timing;
    timing.bpm = 120;  // bar = 2 s
    int sr = 48000;
    // played while stopped: at the cursor, from the first note, whole bars
    auto place = capture::placement(p, sr, timing, 3.0);
    CHECK_NEAR(place.start, 3.0, 1e-12);
    CHECK(place.notes.size() == 2);
    CHECK_NEAR(place.notes[0].start, 0.0, 1e-12);
    CHECK_NEAR(place.notes[1].start, 1.0, 1e-12);
    CHECK_NEAR(place.lengthS, 2.0, 1e-12);
    // played along with playback: where it was heard, from the bar of the first note, to the end of the bar
    // its last note ends in
    std::vector<LogEntry> along = {entry(100, true, 60, (int64_t)(5.3 * sr)), entry(100.5, false, 60),
                                   entry(101, true, 62, (int64_t)(6.31 * sr)), entry(102.6, false, 62)};
    auto q = capture::phrase(along, 103);
    auto pl = capture::placement(q, sr, timing, 0);
    CHECK_NEAR(pl.start, 4.0, 1e-12);  // (bar 3 starts at 4 s)
    CHECK_NEAR(pl.notes[0].start, 1.3 + 0.005, 0.01);
    CHECK_NEAR(pl.lengthS, 4.0, 1e-9);  // to 8 s (the last note ends at ~7.9 s)
    // the playhead jumped meanwhile: at the cursor instead
    std::vector<LogEntry> jumped = {entry(100, true, 60, (int64_t)(5.3 * sr)), entry(100.5, false, 60),
                                    entry(101, true, 62, (int64_t)(20.0 * sr)), entry(101.5, false, 62)};
    auto pj = capture::placement(capture::phrase(jumped, 103), sr, timing, 1.0);
    CHECK_NEAR(pj.start, 1.0, 1e-12);

    // the action: a clip on the selected track, once
    Fixture f;
    midi_input::MidiInput input(f.engine, f.project, [&] { return f.track; }, nullptr);
    double clock = 200;
    input.clock = [&] { return clock; };
    input.handle(midi_in::Event::noteOn(60, 100));
    clock = 200.5;
    input.handle(midi_in::Event::noteOff(60));
    clock = 201;
    input.handle(midi_in::Event::noteOn(67, 90));
    clock = 202;
    input.handle(midi_in::Event::noteOff(67));
    std::vector<std::string> messages;
    ClipPtr placed;
    int64_t placedAt = -1;
    int takes = 0;
    capture::Host host;
    host.input = &input;
    host.cursor = [&] { return (int64_t)f.engine.sr * 6; };
    host.placeClip = [&](const ClipPtr& c, int64_t at, const TrackPtr& t) {
        placed = c;
        placedAt = at;
        c->track = t;
        c->start = at;
        f.project.addClip(c);
    };
    host.nextTake = [&] { return ++takes; };
    host.status = [&](const std::string& m, int) { messages.push_back(m); };
    ClipPtr c = capture::capture(host);
    CHECK(c && placed == c && placedAt == f.engine.sr * 6);
    CHECK(c && c->name == "Capture 1" && c->notes->size() == 2);
    CHECK(c && std::fabs(c->lengthS - 2.0) < 1e-9 && c->length == f.engine.sr * 2);
    CHECK(c && c->track == f.track);
    CHECK(!messages.empty() && messages.back() == "Captured 2 notes onto Piano");
    // again right away: nothing new
    CHECK(capture::capture(host) == nullptr);
    CHECK(messages.back().find("Capture: nothing new was played") == 0);
}

// ------------------------------------------------------------------------------------------
static void testMidiExport() {
    Fixture f;
    // the names offered
    CHECK(midi_export::fileName({f.clip}, "") == "Melody.mid");
    CHECK(midi_export::fileName({f.clip, f.second}, "/home/x/My Song.json") == "My Song.mid");
    CHECK(midi_export::fileName({f.clip, f.second}, "") == "MIDI clips.mid");
    ClipPtr odd = copyClip(f.clip);
    odd->name = "a/b:c?.mid";
    CHECK(midi_export::fileName({odd}, "") == "a_b_c_.mid");
    // parts: one per track; from the bar the earliest clip starts in (second clip at 1 s: bar 1 at 0 s)
    f.clip->start = f.engine.sr * 3;  // in bar 2 (2…4 s)
    auto parts = midi_export::partsOf({f.clip, f.second}, f.engine.sr, f.project.timing);
    CHECK_NEAR(parts.start, 0.0, 1e-12);
    CHECK(parts.parts.size() == 2);
    CHECK(parts.parts.size() == 2 && parts.parts[0].name == "Bass" && parts.parts[1].name == "Piano");
    // switched off notes and the parts outside the window are left out
    f.clip->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{note(3, 3.5, 90)});
    f.clip->length = f.engine.sr * 2;  // only the first 2 s of its notes play
    std::string path = "build/test-midi2/export.mid";
    util::makeDirs("build/test-midi2");
    std::string error;
    size_t written = 0;
    CHECK(midi_export::writeFile(path, {f.clip}, f.engine.sr, f.project.timing, error, &written));
    CHECK(written == 3);  // (72 at 2 s is outside the window)
    std::vector<MidiFileNote> back;
    double len = 0;
    CHECK(readMidiFile(path, back, len, error));
    CHECK(back.size() == 3);
    if (back.size() == 3) {  // the clip starts 1 s into its bar (3 s, bar 2 at 2 s)
        CHECK_NEAR(back[0].start, 1.0, 1e-3);
        CHECK(back[0].pitch == 60 && back[0].velocity == 100);
        CHECK_NEAR(back[2].start, 2.0, 1e-3);
        CHECK_NEAR(back[2].end, 3.0, 1e-3);
        CHECK(back[1].pitch == 64 && back[1].velocity == 80);
    }
    // two tracks in one file, in time with each other
    CHECK(midi_export::writeFile(path, {f.clip, f.second}, f.engine.sr, f.project.timing, error, &written));
    CHECK(readMidiFile(path, back, len, error));
    CHECK(back.size() == 5);
    bool bass = false;
    for (auto& n : back)
        if (n.pitch == 48 && std::fabs(n.start - 1.0) < 1e-3) bass = true;
    CHECK(bass);
    // a looping clip: its passes written
    ClipPtr loop = makeMidiClip("Loop", {note(0, 0.25, 50)}, 1.0, f.engine.sr);
    loop->track = f.track;
    loop->loop = true;
    loop->length = f.engine.sr * 3;
    loop->prepare(f.engine.sr);
    CHECK(midi_export::writeFile(path, {loop}, f.engine.sr, f.project.timing, error, &written));
    CHECK(written == 3);
}

// ------------------------------------------------------------------------------------------
static void testMidiEffectsDialog() {
    Fixture f;
    sampler_dialog::Host host;
    host.project = &f.project;
    host.engine = &f.engine;
    int relayouts = 0;
    host.relayout = [&] { relayouts++; };
    sampler_dialog::setHost(host);
    // the kinds, as audio/midi_effects has them
    const auto& kinds = midi_effects_dialog::kinds();
    CHECK(kinds.size() == 8 && kinds[0].name == "Arpeggiator" && kinds[6].name == "Pitch" && kinds[7].name == "Groove");
    CHECK(!kinds[0].live && kinds[1].live);
    const midi_effects_dialog::Param& gate = kinds[0].params[2];
    CHECK(gate.key == "gate" && gate.text(80) == "80 %");
    const midi_effects_dialog::Param& beats = midi_effects_dialog::kindOf("Note Length")->params[1];
    CHECK(beats.text(0.25) == "0.250 beats");
    CHECK_NEAR(beats.fromNormal(beats.toNormal(0.5)), 0.5, 1e-9);
    CHECK(midi_effects_dialog::kindOf("Pitch")->params[0].text(-3) == "-3 semitones");

    midi_effects_dialog::MidiEffectsDialog* d = midi_effects_dialog::openMidiEffects(f.track);
    CHECK(d && d->title() == "MIDI effects \xe2\x80\x94 Piano");
    CHECK(midi_effects_dialog::openMidiEffects(f.track) == d);  // (one per track)
    CHECK(d->list->count() == 0);
    // Add ▾: the eight, clips-only ones marked
    Menu* menu = d->addButton->menu();
    CHECK(menu->actions().size() == 8);
    CHECK(menu->actions()[0]->text == "Arpeggiator  (clips only)" && menu->actions()[1]->text == "Chord");
    Menu::activate(menu->actions()[6].get());  // Pitch
    Menu::activate(menu->actions()[1].get());  // Chord
    CHECK(f.track->midiEffects.size() == 2 && f.track->midiEffects[1].kind == "Chord");
    CHECK(d->list->count() == 2 && d->list->currentRow() == 1);
    // changes render the track's clips a moment after the last one
    std::shared_ptr<const std::vector<Note>> playedBefore = f.clip->played;
    d->setValue(0, "semitones", 5);
    CHECK(f.track->midiEffects[0].get("semitones") == 5);
    App::get().advanceTimers(0.3);
    CHECK(relayouts >= 1 && f.clip->played != playedBefore);
    // reorder, remove
    d->list->setCurrentRow(1);
    d->move(-1);
    CHECK(f.track->midiEffects[0].kind == "Chord" && d->list->currentRow() == 0);
    savePicture(*d, "midi_effects");
    d->remove();
    CHECK(f.track->midiEffects.size() == 1 && f.track->midiEffects[0].kind == "Pitch");
    // undo replaced the chain: reload
    f.track->midiEffects = std::vector<MidiEffect>{};
    midi_effects_dialog::reloadAll();
    CHECK(d->list->count() == 0);
    d->close();
    App::get().collectGarbage();
    CHECK(midi_effects_dialog::openFor(f.track.get()) == nullptr);
}

static void testSamplerDialog() {
    CHECK(sampler_dialog::rootFromName("Piano C4") == 60);
    CHECK(sampler_dialog::rootFromName("flute_A#3") == 58);
    CHECK(sampler_dialog::rootFromName("kick") == 60);
    CHECK(sampler_dialog::noteName(61) == "C#4");
    Fixture f;
    sampler_dialog::Host host;
    host.project = &f.project;
    host.engine = &f.engine;
    sampler_dialog::setHost(host);
    auto sample = std::make_shared<Clip>();
    sample->kind = Kind::Audio;
    sample->name = "Bell E5.wav";
    auto audio = std::make_shared<AudioBuffer>();
    audio->samples.resize(24000 * 2);
    for (size_t i = 0; i < 24000; ++i)
        audio->samples[i * 2] = audio->samples[i * 2 + 1] = (float)(std::sin(i * 0.05) * std::exp(-(double)i / 6000));
    sample->source = audio;
    sample->sourceSr = 48000;
    SamplerPtr s = sampler_dialog::newSettings(sample);
    CHECK(s->root == 76 && s->sample == sample);
    std::atomic_store(&f.track->sampler, s);
    CHECK(f.track->playsSampler());
    sampler_dialog::SamplerDialog* d = sampler_dialog::openSampler(f.track);
    CHECK(d && d->title() == "Built-in sampler \xe2\x80\x94 Piano");
    CHECK(d->root->text() == "E5 (76)");
    CHECK(d->sampleName->text() == "Bell E5.wav  (0.50 s)");
    // a change replaces the track's settings whole
    d->start->setValue(25);
    CHECK(f.track->sampler != s && std::fabs(f.track->sampler->start - 0.25) < 1e-9);
    CHECK(d->waveform->start == f.track->sampler->start);
    d->mode->setCurrentIndex(1);
    CHECK(f.track->sampler->oneShot && !d->loop->isEnabled());
    d->end->setValue(20);  // (end before start: kept apart)
    CHECK(f.track->sampler->end > f.track->sampler->start);
    savePicture(*d, "sampler");
    // undo: reload shows it without it counting as a change
    SamplerPtr before = f.track->sampler;
    std::atomic_store(&f.track->sampler, s);
    sampler_dialog::reloadAll();
    CHECK(f.track->sampler == s && d->mode->currentIndex() == 0);
    d->playRoot();
    d->close();
    App::get().collectGarbage();
    CHECK(sampler_dialog::openFor(f.track.get()) == nullptr);
}

static void testPaintScaleShading() {
    Fixture f;
    f.project.timing.key = 9;  // A minor
    f.project.timing.scale = "Minor";
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    ed.selected = {2};
    ed.setRowHeight(14);
    savePicture(ed, "scale_shading");
    // a row outside the scale is darker than one in it (both white keys: C# vs … pick F (in) and F# (out))
    std::optional<double> inY = ed.yOf(65), outY = ed.yOf(66);
    if (inY && outY && *inY > 0 && *outY > 0 && *inY < ed.roll->height() && *outY < ed.roll->height()) {
        Point a = ed.roll->mapToWindow(Point(ed.roll->width() - 20, (int)*inY + 3));
        Point b = ed.roll->mapToWindow(Point(ed.roll->width() - 20, (int)*outY + 3));
        Color ca = ed.framebuffer().colorAt(a.x, a.y), cb = ed.framebuffer().colorAt(b.x, b.y);
        CHECK(ca.r + ca.g + ca.b > cb.r + cb.g + cb.b);
    }
    ed.setFold("scale");
    savePicture(ed, "scale_folded_keys");
    ed.close();
}

// The stylesheets reach what the panels and editors draw: a stylesheet over the built-in one recolours a level
// meter's bar (LevelMeter::bar:ok) and the piano roll's keyboard (PianoRoll::keys), and changes a size
// (--piano-keys-width, --meter-gap); theme::setSkin() puts all of it back.
static void testPanelStylesheet() {
    auto sheet = [](const char* text) {
        std::vector<css::Problem> problems;
        css::SheetPtr s = css::parse(text, "test.css", problems);
        CHECK(problems.empty());
        return s;
    };
    theme::setSkin();
    // a meter: two bars, the top one at -20 dB (green: up to -12 dB), 2 px thick with a 1 px gap
    LevelMeter meter(2);
    meter.setGeometry(0, 0, 60, 5);
    meter.push(std::vector<float>{0.1f, 0.1f});
    auto meterAt = [&](int x, int y) {
        Image img(60, 5, Color(0, 0, 0));
        Painter p(img);
        meter.paint(p);
        return img.colorAt(x, y);
    };
    std::vector<std::pair<int, int>> bars = meter.barRects();
    CHECK(bars.size() == 2 && bars[1].first - bars[0].first - bars[0].second == 1);  // (the default gap)
    int barY = bars.empty() ? 0 : bars[0].first;
    CHECK(meterAt(2, barY) == theme::c("meter_ok"));
    CHECK(meterAt(55, barY) == theme::c("meter_bg"));  // (the track beyond the level)
    // the piano roll's keyboard
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    auto keyAt = [&]() {  // a white key's colour, at the keyboard's left edge (C4's row: a white key)
        ed.update();
        App::get().tick();
        std::optional<double> y = ed.yOf(60);
        CHECK(y.has_value());
        Point at = ed.roll->mapToWindow(Point(1, (int)(y ? *y : 0) + 3));
        return ed.framebuffer().colorAt(at.x, at.y);
    };
    CHECK(keyAt() == theme::c("keys_white"));
    CHECK(keysW() == KEYS_W);

    theme::setStyles({theme::builtinSheet(), sheet("LevelMeter::bar:ok { background-color: #ff00ff; }\n"
                                                   "LevelMeter::track { background-color: rgb(0, 0, 255); }\n"
                                                   "PianoRoll::keys { background-color: #00ff00; }\n"
                                                   ":root { --piano-keys-width: 80px; --meter-gap: 0px; }")});
    CHECK(meterAt(2, barY) == Color(255, 0, 255));
    CHECK(meterAt(55, barY) == Color(0, 0, 255));
    bars = meter.barRects();
    CHECK(bars.size() == 2 && bars[1].first == bars[0].first + bars[0].second);  // (no gap now)
    CHECK(keyAt() == Color(0, 255, 0));
    CHECK(keysW() == 80);

    theme::setSkin();
    CHECK(meterAt(2, barY) == theme::c("meter_ok"));
    CHECK(meterAt(55, barY) == theme::c("meter_bg"));
    bars = meter.barRects();
    CHECK(bars.size() == 2 && bars[1].first - bars[0].first - bars[0].second == 1);
    CHECK(keyAt() == theme::c("keys_white"));
    CHECK(keysW() == KEYS_W);
    ed.close();
}

int main() {
    App::get().initHeadless();
    util::makeDirs("build/test-midi2");
    std::remove("build/test-midi2/settings.json");
    settings::setPath("build/test-midi2/settings.json");
    midi_effects::install(nullptr);
    testGhosts();
    testScaleAndFold();
    testStamp();
    testBoxSelectAndCopyDrag();
    testArrows();
    testCopyPasteAcrossClips();
    testQuantize();
    testSwitchedOff();
    testLoopLengthening();
    testComputerKeyboard();
    testMidiInputRouting();
    testCapture();
    testMidiExport();
    testMidiEffectsDialog();
    testSamplerDialog();
    testPaintScaleShading();
    testPanelStylesheet();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
