// The MIDI editor, the Explorer and its waveform, headless: note hit testing, snapping, adding /
// moving / resizing / deleting / duplicating notes with the mouse and keys, the velocity lane, the
// ruler's cursor, wheel modifiers, key height, Expand; folder indexing limits, search word matching,
// previews loaded in the background, drags out, folders remembered. Paints both into PNGs in
// build/ui-samples/midi_explorer/. No sound: the engine is never opened.
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/model.h"
#include "ui/explorer.h"
#include "ui/midi_editor.h"
#include "ui/sample_waveform.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
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
// positions are in `w`'s coordinates
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
// a double-click held down (its second press: dragging it lengthens what it added)
void doublePress(Widget* w, Point p, int mods = 0) {
    click(w, p, plat::LeftButton, mods);
    gTime += 0.1;
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseDown, w->mapToWindow(p), plat::LeftButton, plat::LeftButton, mods));
}
void doubleClick(Widget* w, Point p) {
    click(w, p);
    gTime += 0.1;
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseDown, w->mapToWindow(p), plat::LeftButton, plat::LeftButton));
    release(w, p);
}
void wheelAt(Widget* w, Point p, float dy, int mods = 0) {
    plat::Event e;
    e.type = plat::Event::Wheel;
    Point q = w->mapToWindow(p);
    e.x = q.x;
    e.y = q.y;
    e.dy = dy;
    e.mods = mods;
    e.time = gTime;
    w->window()->onEvent(nullptr, e);
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
    std::string d = "build/ui-samples/midi_explorer";
    util::makeDirs(d);
    return d;
}
void savePicture(Window& w, const std::string& name) {
    w.update();
    App::get().tick();
    savePng(outDir() + "/" + name + ".png", w.framebuffer());
}

// let background threads finish and their results arrive (plat::postToMain)
template <class F>
bool waitFor(F done, double seconds = 10) {
    double until = plat::now() + seconds;
    while (!done() && plat::now() < until) plat::processEvents(10);
    return done();
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
        project.timing.bpm = 120;  // a beat = 0.5 s
        track = project.addTrack(Kind::Midi);
        track->name = "Piano";
        std::vector<Note> notes = {note(0, 0.5, 60, 100), note(0.5, 1.0, 64, 80), note(1.0, 2.0, 67, 120),
                                   note(2.0, 2.25, 72, 60)};
        clip = makeMidiClip("Melody", notes, 4.0, engine.sr);
        clip->track = track;
        clip->start = engine.sr * 2;  // two seconds into the timeline
        project.addClip(clip);
    }
    MidiEditor::TimelineLink link() {
        MidiEditor::TimelineLink l;
        l.cursor = [this] { return cursor; };
        l.setCursor = [this](int64_t f) { cursor = f; };
        l.seek = [this](int64_t) {};
        return l;
    }
};

Point notePoint(MidiEditor& ed, int index, double along = 0.5) {
    const Note& n = ed.notes[index];
    return Point((int)(ed.xOf(n.start + (n.end - n.start) * along)), (int)(*ed.yOf((int)n.pitch) + ed.rowH / 2));
}
}  // namespace

// ------------------------------------------------------------------------------------------
static void testNames() {
    CHECK(noteName(60) == "C4");
    CHECK(noteName(0) == "C-1");
    CHECK(noteName(61) == "C#4");
    CHECK(noteName(127) == "G9");
    CHECK(isBlackKey(61) && isBlackKey(70) && !isBlackKey(60) && !isBlackKey(64));
}

static void testGeometryAndSnapping() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    CHECK(ed.title() == "Notes: Melody \xe2\x80\x94 Piano");
    // (960x560, or wider: a window opens at least at its layout's minimum, its toolbar never cut)
    int openW = std::max(960, ed.layout()->minimumSize().x);
    CHECK(ed.width() == openW && ed.height() == 560);
    CHECK(ed.roll->width() > 800 && ed.roll->height() >= 200);
    CHECK(ed.velocity->height() == VELOCITY_H && ed.ruler->height() == RULER_H);
    // fit: the clip (4 s) across the roll; the notes (60…72) around the middle
    double wFrom, wTo;
    ed.windowS(wFrom, wTo);
    CHECK_NEAR(wTo - wFrom, 4.0 + synth::RELEASE_S, 1e-3);  // (the clip's audio has the synth's release tail)
    CHECK_NEAR(ed.pps, (ed.roll->width() - KEYS_W - 20) / (wTo - wFrom), 1e-6);
    int mid = ed.pitchAt(ed.roll->height() / 2.0);
    CHECK(mid >= 64 && mid <= 68);
    // x/t and y/pitch are inverses
    CHECK_NEAR(ed.tAt(ed.xOf(1.25)), 1.25, 1e-9);
    CHECK(ed.pitchAt(*ed.yOf(67) + 1) == 67);
    CHECK(ed.pitchAt(*ed.yOf(67) + ed.rowH - 1) == 67);
    CHECK(ed.pitchAt(-1e6) == 127 && ed.pitchAt(1e6) == 0);
    // hit testing: the body, the right end (within EDGE_PX), outside, later notes on top
    bool atEnd = true;
    Point body = notePoint(ed, 2, 0.3);
    CHECK(ed.noteAt(body.x, body.y, &atEnd) == 2 && !atEnd);
    double endX = ed.xOf(ed.notes[2].end);
    CHECK(ed.noteAt(endX - 2, body.y, &atEnd) == 2 && atEnd);
    CHECK(ed.noteAt(endX + 1, body.y, &atEnd) == 2);         // (one px past the end still counts)
    CHECK(ed.noteAt(endX + 4, body.y) == -1);
    CHECK(ed.noteAt(body.x, *ed.yOf(66) + 2) == -1);           // another pitch
    // the grid: bars and beats at this zoom; snapping follows the timeline's grid (origin = clip start)
    double step = ed.stepS();
    CHECK(step > 0);
    CHECK_NEAR(step, f.project.timing.gridStep(ed.pps).value() * 0.5, 1e-12);
    CHECK_NEAR(ed.snap(step * 2.4, 0), step * 2, 1e-9);
    CHECK_NEAR(ed.snap(step * 2.6, 0), step * 3, 1e-9);
    CHECK_NEAR(ed.snap(step * 2.9, 0, true), step * 2, 1e-9);
    CHECK_NEAR(ed.snap(step * 2.4, plat::ModAlt), step * 2.4, 1e-12);  // Alt: free
    CHECK_NEAR(ed.snap(-1, 0), 0, 1e-12);
    // grid steps count from the timeline's bar lines: a clip starting off the grid snaps to it
    f.clip->start = f.engine.sr * 2 + f.engine.sr / 10;  // 2.1 s
    CHECK_NEAR(ed.snap(0.5, 0) + 2.1, std::round((0.5 + 2.1) / step) * step, 1e-9);
    f.clip->start = f.engine.sr * 2;
    // in minutes and seconds nothing snaps, new notes are a quarter of a ruler step
    f.project.timing.beats = false;
    CHECK(!ed.snapping(0));
    CHECK_NEAR(ed.snap(0.123, 0), 0.123, 1e-12);
    f.project.timing.beats = true;
}

static void testMouseEditing() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    double step = ed.stepS();
    // one click on an empty spot only selects: nothing added, the selection cleared; a drag there selects with a box
    Point empty((int)ed.xOf(3.0 + step * 0.7), (int)*ed.yOf(57) + 3);
    ed.selected = {0};
    click(ed.roll, empty);
    CHECK(ed.notes.size() == 4);
    CHECK(ed.selected.empty());
    CHECK(f.changes == 0);
    Point corner = notePoint(ed, 1, 0.5);
    Point outside((int)ed.xOf(0.01), corner.y - 2 * ed.rowH);
    if (ed.noteAt(outside.x, outside.y) < 0) {
        press(ed.roll, outside);
        dragTo(ed.roll, Point(corner.x, corner.y + 1));
        release(ed.roll, Point(corner.x, corner.y + 1));
        CHECK(ed.notes.size() == 4);
        CHECK(ed.selected.count(1) == 1);
        CHECK(f.changes == 0);
    }
    gTime += 1.0;  // (not a double-click with the next press)
    // double-click an empty spot: a new note, one grid step long, starting on the grid line before the click
    doublePress(ed.roll, empty);
    CHECK(ed.notes.size() == 5);
    CHECK(ed.held && *ed.held == 57);  // it sounds while held
    CHECK(f.changes == 0);              // nothing goes into the clip before the release
    release(ed.roll, empty);
    CHECK(!ed.held);
    CHECK(f.changes == 1);
    CHECK(f.clip->notes->size() == 5);
    const Note* added = nullptr;
    for (auto& n : ed.notes)
        if ((int)n.pitch == 57) added = &n;
    CHECK(added != nullptr);
    if (added) {
        CHECK_NEAR(added->start, ed.snap(3.0 + step * 0.7, 0, true), 1e-9);
        CHECK_NEAR(added->end - added->start, step, 1e-9);
        CHECK_NEAR(added->velocity, DEFAULT_VELOCITY, 1e-9);
    }
    CHECK(ed.selected.size() == 1);
    // the notes are kept in time order after a commit; the selection follows its note
    for (size_t i = 1; i < ed.notes.size(); ++i) CHECK(ed.notes[i - 1].start <= ed.notes[i].start);
    CHECK(ed.selected.count(4) == 1 && (int)ed.notes[4].pitch == 57);

    // drag a note: time snapped to the grid, pitch by rows; it goes in at the release
    int idx = 1;  // 0.5–1.0, pitch 64
    Point from = notePoint(ed, idx, 0.3);
    press(ed.roll, from);
    CHECK(ed.selected == std::set<int>{idx});
    CHECK(ed.lastVelocity == 80);
    Point to(from.x + (int)(step * 2.2 * ed.pps), from.y - 3 * ed.rowH);
    dragTo(ed.roll, to);
    CHECK_NEAR(ed.notes[idx].start, 0.5 + 2 * step, 1e-9);
    CHECK_NEAR(ed.notes[idx].pitch, 67, 1e-9);
    CHECK(ed.held && *ed.held == 67);  // the pitch it was moved to sounds
    release(ed.roll, to);
    CHECK(f.changes == 2);
    // nothing goes before the clip's start
    int first = 0;
    Point p0 = notePoint(ed, first, 0.5);
    press(ed.roll, p0);
    dragTo(ed.roll, Point(p0.x - 400, p0.y));
    CHECK_NEAR(ed.notes[first].start, 0, 1e-9);
    release(ed.roll, Point(p0.x - 400, p0.y));

    // resize by the right end: the end snaps; never shorter than a grid step
    int last = (int)ed.notes.size() - 1;
    double start = ed.notes[last].start;
    Point endP((int)ed.xOf(ed.notes[last].end) - 2, (int)*ed.yOf((int)ed.notes[last].pitch) + 3);
    press(ed.roll, endP);
    dragTo(ed.roll, Point((int)ed.xOf(start + step * 3.1), endP.y));
    CHECK_NEAR(ed.notes[last].end, ed.snap(start + step * 3.1, 0), 1e-9);
    dragTo(ed.roll, Point((int)ed.xOf(start - 1), endP.y));
    CHECK_NEAR(ed.notes[last].end - ed.notes[last].start, step, 1e-9);
    release(ed.roll, Point((int)ed.xOf(start - 1), endP.y));

    // a note past the clip's end lengthens it
    double before = f.clip->lengthS;
    int total = (int)ed.notes.size();
    Point far((int)ed.xOf(before + step * 0.2), (int)*ed.yOf(62) + 3);
    if (far.x < ed.roll->width()) {
        doublePress(ed.roll, far);
        dragTo(ed.roll, Point(far.x + (int)(step * 4 * ed.pps), far.y));
        release(ed.roll, Point(far.x + (int)(step * 4 * ed.pps), far.y));
        CHECK((int)ed.notes.size() == total + 1);
        CHECK(f.clip->lengthS > before);
        CHECK_NEAR(f.clip->lengthS, ed.notes.back().end > before ? std::max(before, ed.notes.back().end) : before, 1.0);
    }

    // Ctrl+click adds to / removes from the selection
    ed.selected = {0};
    click(ed.roll, notePoint(ed, 2), plat::LeftButton, plat::ModCtrl);
    CHECK(ed.selected == (std::set<int>{0, 2}));
    click(ed.roll, notePoint(ed, 2), plat::LeftButton, plat::ModCtrl);
    CHECK(ed.selected == std::set<int>{0});

    // right-click a note: deleted
    int n = (int)ed.notes.size(), changes = f.changes;
    click(ed.roll, notePoint(ed, 0), plat::RightButton);
    CHECK((int)ed.notes.size() == n - 1);
    CHECK(f.changes == changes + 1);
    CHECK(f.clip->notes->size() == ed.notes.size());

    // keys: Ctrl+A, Escape, Delete
    key(ed, 'A', plat::ModCtrl);
    CHECK((int)ed.selected.size() == (int)ed.notes.size());
    key(ed, plat::Key_Escape);
    CHECK(ed.selected.empty());
    ed.selected = {0, 1};
    n = (int)ed.notes.size();
    key(ed, plat::Key_Delete);
    CHECK((int)ed.notes.size() == n - 2 && ed.selected.empty());

    // the transport's application-wide keys (Space, Home) work while the editor has the focus
    {
        Window main;
        main.showWindow();
        int plays = 0, homes = 0;
        Shortcut space(&main, KeySequence(plat::Key_Space), [&] { plays++; }, ShortcutScope::ApplicationWide);
        Shortcut home(&main, KeySequence(plat::Key_Home), [&] { homes++; }, ShortcutScope::ApplicationWide);
        ed.roll->setFocus();
        key(ed, plat::Key_Space);
        key(ed, plat::Key_Home);
        CHECK(plays == 1 && homes == 1);
    }

    // the keys on the left: press plays, dragging over them plays every key passed
    press(ed.roll, Point(10, (int)*ed.yOf(60) + 2));
    CHECK(ed.held && *ed.held == 60);
    dragTo(ed.roll, Point(10, (int)*ed.yOf(64) + 2));
    CHECK(ed.held && *ed.held == 64);
    release(ed.roll, Point(10, (int)*ed.yOf(64) + 2));
    CHECK(!ed.held);
}

static void testDuplicate() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    double step = ed.stepS();
    // notes 0 and 1: 0 … 1.0 s; the span rounded up to the grid step
    ed.selected = {0, 1};
    key(ed, 'D', plat::ModCtrl);
    double span = std::max(1.0, std::ceil(1.0 / step - 1e-9)) * step;
    CHECK(ed.notes.size() == 6);
    CHECK(f.changes == 1);
    CHECK(ed.selected.size() == 2);
    for (int i : ed.selected) {
        const Note& c = ed.notes[i];
        CHECK((c.pitch == 60 && std::fabs(c.start - span) < 1e-9) || (c.pitch == 64 && std::fabs(c.start - (0.5 + span)) < 1e-9));
    }
    // a span that isn't a whole number of steps is rounded up (bars and beats)
    std::vector<Note> odd = {note(0, step * 1.3, 60)};
    ed.notes = odd;
    ed.selected = {0};
    ed.duplicate(0);
    CHECK(ed.notes.size() == 2);
    CHECK_NEAR(ed.notes[1].start, 2 * step, 1e-9);
    // Alt (no snapping): by the exact span
    ed.selected = {1};
    ed.duplicate(plat::ModAlt);
    CHECK_NEAR(ed.notes[2].start, 2 * step + step * 1.3, 1e-9);
    // nothing selected: nothing happens
    int changes = f.changes;
    ed.selected.clear();
    ed.duplicate(0);
    CHECK(f.changes == changes);
}

static void testVelocityRulerWheel() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    VelocityLane* lane = ed.velocity;
    CHECK(lane->velocityAt(lane->height() - 3) == 1);
    CHECK(lane->velocityAt(3) == 127);
    CHECK(lane->velocityAt(-50) == 127);
    // click on a note's bar: its velocity; drag across sets each bar passed
    int x2 = (int)ed.xOf(ed.notes[2].start) + 2;
    int y = lane->height() / 2;
    press(lane, Point(x2, y));
    CHECK((int)ed.notes[2].velocity == lane->velocityAt(y));
    int x3 = (int)ed.xOf(ed.notes[3].start) + 2;
    dragTo(lane, Point(x3, 20));
    CHECK((int)ed.notes[3].velocity == lane->velocityAt(20));
    CHECK(f.changes == 0);
    release(lane, Point(x3, 20));
    CHECK(f.changes == 1);
    CHECK((int)(*f.clip->notes)[3].velocity == lane->velocityAt(20));
    CHECK(ed.lastVelocity == lane->velocityAt(20));
    // missing every bar: nothing changes
    click(lane, Point(ed.xOf(3.5), y));
    CHECK(f.changes == 1);

    // the ruler: click moves the selection cursor onto the grid (timeline frames)
    double step = ed.stepS();
    click(ed.ruler, Point((int)ed.xOf(1.0 + step * 0.4), 10));
    CHECK(f.cursor == (int64_t)std::nearbyint((ed.originS() + ed.snap(ed.tAt((int)ed.xOf(1.0 + step * 0.4)), 0)) * f.engine.sr));
    CHECK(ed.cursorX() && std::fabs(*ed.cursorX() - ed.xOf(f.cursor / (double)f.engine.sr - ed.originS())) < 1e-6);
    click(ed.ruler, Point(10, 10));  // over the keys: nothing
    CHECK(f.cursor != 0);

    // wheel: up/down, Shift sideways, Ctrl zoom, Alt key height
    ed.vbar->setValue(300);
    wheelAt(ed.roll, Point(300, 100), -1);
    CHECK(ed.vbar->value() == 360);
    int h0 = ed.hbar->value();
    ed.hbar->setValue(200);
    h0 = ed.hbar->value();
    wheelAt(ed.roll, Point(300, 100), -1, plat::ModShift);
    CHECK(ed.hbar->value() == std::min(ed.hbar->maximum(), h0 + 120));
    ed.hbar->setValue(0);
    double pps = ed.pps;
    double tUnder = ed.tAt(400);
    wheelAt(ed.velocity, Point(400, 30), 1, plat::ModCtrl);
    CHECK_NEAR(ed.pps, pps * 1.25, 1e-9);
    CHECK(std::fabs(ed.tAt(400) - tUnder) < 2 / ed.pps);  // the time under the mouse stays
    int row = ed.rowH;
    wheelAt(ed.ruler, Point(400, 10), 1, plat::ModAlt);
    CHECK(ed.rowH == row + 2);
    // Keys − / +, within MIN_ROW_H … MAX_ROW_H
    ed.setRowHeight(100);
    CHECK(ed.rowH == MAX_ROW_H);
    ed.setRowHeight(1);
    CHECK(ed.rowH == MIN_ROW_H);
    ed.setRowHeight(ROW_H);

    // Expand: the editor fills the screen, and the clip is fitted across the new width
    ed.expandButton->click();
    CHECK(ed.isExpanded() && ed.expandButton->isChecked());
    CHECK(ed.width() == 1920);
    App::get().advanceTimers(0.01);
    double wFrom, wTo;
    ed.windowS(wFrom, wTo);
    CHECK(ed.roll->width() == 1920 - 10);
    CHECK_NEAR(ed.pps, (ed.roll->width() - KEYS_W - 20) / (wTo - wFrom), 1e-6);
    ed.expandButton->click();
    CHECK(!ed.isExpanded() && ed.width() == std::max(960, ed.layout()->minimumSize().x));

    // the clip went away: a commit closes the editor
    bool closed = false;
    ed.closed.connect([&] { closed = true; });
    f.project.removeClips({f.clip});
    ed.deleteOnClose = false;  // (on the stack here)
    ed.commit();
    CHECK(closed);
}

static void testPaintEditor() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.showWindow();
    ed.selected = {1, 2};
    f.cursor = f.clip->start + f.engine.sr / 2 * 3;  // 1.5 s into the notes
    f.engine.seek(f.clip->start + f.engine.sr * 3);  // the playback cursor at 3 s
    ed.followCursors();
    ed.held = 67;
    savePicture(ed, "midi_editor");
    // a pixel check: the selected note's outline is the selected-note colour
    const Note& sel = ed.notes[2];
    Point tl = ed.roll->mapToWindow(Point((int)ed.xOf(sel.start) + 8, (int)*ed.yOf((int)sel.pitch) + 1));
    Color c = ed.framebuffer().colorAt(tl.x, tl.y);
    CHECK(c.r > 200 && c.g > 200 && c.b > 200);
    ed.held.reset();
    // split clip (only part of the notes plays: the rest shaded), minutes and seconds
    f.clip->offset = f.engine.sr / 2;
    f.clip->length = f.engine.sr * 2;
    f.project.timing.beats = false;
    ed.fit();
    ed.setRowHeight(18);
    savePicture(ed, "midi_editor_split_seconds");
    ed.close();
}

// ------------------------------------------------------------------------------------------
// Explorer

namespace {
std::string tempRoot() {
    std::string d = util::absolutePath("build/test-explorer");
    return d;
}
void writeTone(const std::string& path, double seconds, double freq, bool stereoDiff = false) {
    int sr = 44100;
    size_t n = (size_t)(seconds * sr);
    std::vector<float> s(n * 2);
    for (size_t i = 0; i < n; ++i) {
        float env = (float)(1.0 - (double)i / n);
        float v = (float)std::sin(2 * M_PI * freq * i / sr) * 0.8f * env;
        s[i * 2] = v;
        s[i * 2 + 1] = stereoDiff ? (float)std::sin(2 * M_PI * freq * 1.5 * i / sr) * 0.5f : v;
    }
    std::string err;
    writeWav(path, s.data(), n, 2, sr, SampleFormat::Pcm16, err);
}
void writeMidi(const std::string& path) {
    std::string d = "MThd";
    d += std::string("\0\0\0\6\0\0\0\1\1\xe0", 10);  // format 0, one track, 480 per beat
    std::string track;
    for (int i = 0; i < 4; ++i) {
        track += std::string("\0", 1) + "\x90" + (char)(60 + i * 4) + "\x64";
        track += std::string("\x83\x60", 2) + "\x80" + (char)(60 + i * 4) + std::string("\0", 1);
    }
    track += std::string("\0\xff\x2f\0", 4);
    d += "MTrk";
    uint32_t len = (uint32_t)track.size();
    d += (char)(len >> 24);
    d += (char)(len >> 16);
    d += (char)(len >> 8);
    d += (char)len;
    d += track;
    util::writeFile(path, d);
}
void makeLibrary(const std::string& root) {
    std::string cmd = "rm -rf '" + root + "'";
    if (system(cmd.c_str()) != 0) return;
    util::makeDirs(root + "/Drums/Kicks");
    util::makeDirs(root + "/Drums/Snares");
    util::makeDirs(root + "/Loops");
    util::makeDirs(root + "/.hidden");
    writeTone(root + "/Drums/Kicks/Kick Deep.wav", 0.4, 55);
    writeTone(root + "/Drums/Kicks/Kick Punchy 02.wav", 0.3, 70);
    writeTone(root + "/Drums/Kicks/kick.WAV", 0.2, 60);
    writeTone(root + "/Drums/Snares/Snare Tight.wav", 0.25, 200);
    writeTone(root + "/Loops/Pad Wide Stereo.wav", 1.5, 220, true);
    writeTone(root + "/.hidden/Kick Secret.wav", 0.2, 50);
    writeTone(root + "/Top Kick.wav", 0.2, 65);
    writeMidi(root + "/Loops/Arp.mid");
    util::writeFile(root + "/Drums/readme.txt", "not a sample");
}
}  // namespace

static void testIndexAndSearch() {
    std::string root = tempRoot();
    makeLibrary(root);
    std::vector<explorer::Indexed> all = explorer::indexFolders({root});
    CHECK(all.size() == 7);  // the hidden folder and the text file are left out
    bool hidden = false;
    for (auto& e : all) {
        CHECK(e.name == util::lower(util::baseName(e.path)));
        if (e.path.find(".hidden") != std::string::npos) hidden = true;
    }
    CHECK(!hidden);
    // the limit
    CHECK(explorer::indexFolders({root}, 3).size() == 3);
    CHECK(explorer::indexFolders({root, root}, 10).size() == 10);
    CHECK(explorer::indexFolders({root + "/missing"}).empty());
    std::atomic<bool> cancel{true};
    CHECK(explorer::indexFolders({root}, 100, &cancel).empty());
    // every word matching, any case, shortest names first
    size_t total = 0;
    std::vector<std::string> hits = explorer::search(all, "KICK", 500, total);
    CHECK(total == 4 && hits.size() == 4);
    CHECK(util::baseName(hits[0]) == "kick.WAV");
    CHECK(util::baseName(hits[1]) == "Top Kick.wav");
    hits = explorer::search(all, "  kick   deep ", 500, total);
    CHECK(total == 1 && util::baseName(hits[0]) == "Kick Deep.wav");
    hits = explorer::search(all, "kick snare", 500, total);
    CHECK(total == 0 && hits.empty());
    hits = explorer::search(all, "wav", 2, total);
    CHECK(total == 6 && hits.size() == 2);  // only the first `limit`
    hits = explorer::search(all, "", 500, total);
    CHECK(total == 7);
    // the folder shown for a result
    CHECK(explorer::resultFolder(root + "/Drums/Kicks/a.wav", {root}) == "Drums/Kicks");
    CHECK(explorer::resultFolder(root + "/a.wav", {root}) == ".");
    CHECK(explorer::resultFolder("/elsewhere/a.wav", {root}) == "/elsewhere");
    CHECK(explorer::resultFolder(root + "/Loops/a.wav", {"/x", root + "/"}) == "Loops");
    ui::DragData d = explorer::fileUrls({"/a.wav", "/b.wav"});
    CHECK(d.files.size() == 2 && d.kind == "files");
}

static void testFolderSettings() {
    std::string root = tempRoot();
    settings::remove(explorer::FOLDERS_KEY);
    std::vector<std::string> first = explorer::loadFolders();  // first run: ~/Music if it exists
    CHECK(first.size() <= 1);
    explorer::saveFolders({root, root + "/gone"});
    std::vector<std::string> back = explorer::loadFolders();
    CHECK(back.size() == 1 && back[0] == root);  // folders that are gone are left out
    settings::set(explorer::FOLDERS_KEY, Json(root));  // a single string
    CHECK(explorer::loadFolders().size() == 1);
}

static void testExplorerWidget() {
    std::string root = tempRoot();
    explorer::saveFolders({root});
    settings::set(explorer::PREVIEW_KEY, Json(true));
    settings::set(explorer::LOOP_KEY, Json(false));
    Project project;
    audio::Engine engine(project);
    std::vector<std::string> added;
    Window host;
    host.resizeWindow(config::EXPLORER_W, 720);
    Explorer* ex = host.add<Explorer>([&](const std::string& p) { added.push_back(p); }, engine);
    vbox(&host)->addWidget(ex);
    host.showWindow();
    CHECK(ex->folders.size() == 1);
    CHECK(ex->status->text() == "Looking through the folders\xe2\x80\xa6");
    CHECK(waitFor([&] { return !ex->indexing; }));
    CHECK(ex->index.size() == 7);
    CHECK(ex->status->text() == "7 samples in 1 folder");
    CHECK(ex->previewButton->isChecked() && !ex->loopButton->isChecked());
    CHECK(ex->waveform->height() == SampleWaveform::HEIGHT);
    CHECK(ex->addButton->width() == 24 && ex->addButton->height() == 24);
    CHECK(ex->tree->isVisible() && !ex->results->isVisible());
    // the tree: folders first, only samples, hidden ones left out
    FileView* tree = ex->tree;
    CHECK(tree->rowCount() == 3);
    CHECK(tree->itemAtRow(0)->name == "Drums" && tree->itemAtRow(1)->name == "Loops");
    CHECK(tree->itemAtRow(2)->name == "Top Kick.wav");
    tree->setExpanded(tree->itemAtRow(0), true);
    CHECK(tree->rowCount() == 5);  // Kicks, Snares (readme.txt left out)
    tree->setExpanded(tree->itemAtRow(1), true);
    CHECK(tree->rowCount() == 8);
    savePicture(host, "explorer_tree_empty_waveform");

    // clicking a sample: its waveform loads in the background, then it plays (the engine isn't open: silent)
    int kickRow = 2;
    CHECK(tree->itemAtRow(kickRow)->name == "Kick Deep.wav");
    Point rowP(60, kickRow * tree->rowHeight + 5);
    click(tree, rowP);
    CHECK(waitFor([&] { return ex->shownAudio() != nullptr; }));
    CHECK(ex->waveform->audio() != nullptr);
    CHECK(ex->waveform->name() == "Kick Deep.wav");
    CHECK_NEAR(ex->waveform->seconds(), 0.4, 0.01);
    CHECK(ex->waveform->lanes(200).size() == 1);  // both channels the same: one lane
    CHECK(engine.previewVoice() != nullptr);
    CHECK(ex->waveform->position().has_value());
    // the line follows the preview; it goes when the preview has finished
    engine.previewVoice()->pos = engine.previewVoice()->data->frames() / 2;
    App::get().advanceTimers(0.05);
    CHECK(ex->waveform->position() && std::fabs(*ex->waveform->position() - 0.5) < 0.01);
    // click the waveform: plays from there
    click(ex->waveform, Point(ex->waveform->width() / 4, 30));
    CHECK(engine.previewVoice() && std::fabs((double)engine.previewVoice()->pos / engine.previewVoice()->data->frames() - 0.25) < 0.01);
    // ⟲ Loop follows at once; ■ stops
    ex->loopButton->click();
    CHECK(engine.previewVoice()->loop.load());
    CHECK(settings::value(explorer::LOOP_KEY, Json(false)).toBool() == true);
    ex->stopButton->click();
    CHECK(engine.previewVoice() == nullptr);
    CHECK(!ex->waveform->position());
    ex->loopButton->click();

    // the arrow keys move to the next sample and preview it
    tree->setFocus();
    key(host, plat::Key_Down);
    CHECK(tree->current() && tree->current()->name == "Kick Punchy 02.wav");
    CHECK(waitFor([&] { return ex->shownPath() == tree->current()->path; }));
    // the same sample again plays at once (no loading)
    ex->stopPreview();
    App::get().advanceTimers(1.0);
    ex->preview(tree->current()->path);
    CHECK(engine.previewVoice() != nullptr);
    // a press and its click within REPEAT_GAP_S preview once
    ex->stopPreview();
    ex->preview(tree->current()->path);
    CHECK(engine.previewVoice() == nullptr);
    // Shift+Down extends the selection; a drag carries every selected file
    key(host, plat::Key_Down, plat::ModShift);
    CHECK(tree->selectedPaths().size() == 2);
    Point second(60, tree->rowOf(tree->current()) * tree->rowHeight + 5);
    press(tree, second);
    dragTo(tree, Point(second.x + 30, second.y + 2));
    CHECK(App::get().drag() != nullptr);
    if (App::get().drag()) {
        CHECK(App::get().drag()->files.size() == 2);
        CHECK(App::get().drag()->kind == "files");
    }
    release(tree, Point(second.x + 30, second.y + 2));
    App::get().endDrag();
    // a plain click on a selected row (no drag) selects only it
    click(tree, second);
    CHECK(tree->selectedPaths().size() == 1);
    // double-click: added at the cursor; a folder only opens
    doubleClick(tree, rowP);
    CHECK(added.size() == 1 && util::baseName(added[0]) == "Kick Deep.wav");
    doubleClick(tree, Point(60, 5));  // Drums
    CHECK(added.size() == 1);
    CHECK(!tree->itemAtRow(0)->expanded);

    // searching: every word, through all folders
    ex->searchBox->setText("kick");
    CHECK(!ex->results->isVisible());  // (once typing pauses)
    App::get().advanceTimers(0.2);
    CHECK(ex->results->isVisible() && !ex->tree->isVisible());
    CHECK(ex->results->rowCount() == 4);
    CHECK(ex->status->text() == "4 found");
    CHECK(ex->results->itemAtRow(0)->name == "kick.WAV");
    CHECK(ex->results->itemAtRow(0)->folder == "Drums/Kicks");
    CHECK(ex->results->itemAtRow(1)->folder == ".");
    CHECK(ex->results->toolTipAt(Point(5, FileView::HEADER_H + 5)) == ex->results->itemAtRow(0)->path);
    ex->searchBox->setText("loop pad");
    App::get().advanceTimers(0.2);
    CHECK(ex->results->rowCount() == 0);
    CHECK(ex->status->text() == "No samples found.");
    ex->searchBox->setText("pad wide");
    App::get().advanceTimers(0.2);
    CHECK(ex->results->rowCount() == 1);
    click(ex->results, Point(20, FileView::HEADER_H + 5));
    CHECK(waitFor([&] { return util::baseName(ex->shownPath()) == "Pad Wide Stereo.wav"; }));
    CHECK(ex->waveform->lanes(200).size() == 2);  // left and right differ
    savePicture(host, "explorer_search_stereo");
    doubleClick(ex->results, Point(20, FileView::HEADER_H + 5));
    CHECK(added.size() == 2);

    // a MIDI file: the waveform shows what the synth plays
    ex->searchBox->setText("arp");
    App::get().advanceTimers(0.2);
    CHECK(ex->results->rowCount() == 1);
    App::get().advanceTimers(1.0);
    ex->results->setCurrent(ex->results->itemAtRow(0));
    CHECK(waitFor([&] { return util::baseName(ex->shownPath()) == "Arp.mid"; }));
    CHECK(ex->shownAudio() && ex->shownAudio()->frames() > engine.sr);  // 4 beats at 120 BPM: 2 s (+ the release)
    savePicture(host, "explorer_search_midi");

    // 🔊 Preview off: selecting only shows the waveform
    ex->previewButton->click();
    CHECK(!ex->previewButton->isChecked());
    CHECK(settings::value(explorer::PREVIEW_KEY, Json(true)).toBool(true) == false);
    CHECK(engine.previewVoice() == nullptr);
    ex->searchBox->setText("");
    App::get().advanceTimers(0.2);
    CHECK(ex->tree->isVisible() && !ex->results->isVisible());
    CHECK(ex->status->text() == "7 samples in 1 folder");
    App::get().advanceTimers(1.0);
    click(tree, Point(60, tree->rowOf(tree->itemAtRow(tree->rowCount() - 1)) * tree->rowHeight + 5));
    CHECK(waitFor([&] { return util::baseName(ex->shownPath()) == "Top Kick.wav"; }));
    CHECK(engine.previewVoice() == nullptr);
    ex->previewButton->click();

    // folders: + adds (remembered), − removes (nothing deleted)
    ex->addFolderPath(root + "/Drums/");
    CHECK(ex->folders.size() == 2 && ex->folders[1] == root + "/Drums");
    CHECK(ex->folderBox->count() == 3 && ex->folderBox->currentText() == "Drums");
    CHECK(explorer::loadFolders().size() == 2);
    CHECK(ex->tree->rootPath() == root + "/Drums");
    CHECK(waitFor([&] { return !ex->indexing; }));
    CHECK(ex->index.size() == 11);  // (Drums twice)
    CHECK(ex->status->text() == "11 samples in 2 folders");
    ex->removeFolder();
    CHECK(ex->folders.size() == 1 && explorer::loadFolders().size() == 1);
    CHECK(util::isDir(root + "/Drums"));
    ex->removeFolder();
    CHECK(ex->folders.empty());
    CHECK(ex->folderBox->count() == 2 && ex->folderBox->itemText(1) == "Presets (track presets)" && ex->folderBox->currentText() == "No folders yet: add one with +");
    CHECK(!ex->removeButton->isEnabled());
    CHECK(!ex->tree->isVisible());
    CHECK(waitFor([&] { return !ex->indexing; }));
    CHECK(ex->status->text() == "Add a folder with your samples (+).");
    savePicture(host, "explorer_no_folders");
    explorer::saveFolders({root});
}

static void testWaveformPeaks() {
    SampleWaveform w;
    w.setGeometry(0, 0, 100, SampleWaveform::HEIGHT);
    auto a = std::make_shared<AudioBuffer>();
    a->samples.resize(10 * 2);
    for (int i = 0; i < 10; ++i) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)i / 10;
    w.setAudio(a, "x", 10);
    CHECK_NEAR(w.seconds(), 1.0, 1e-9);
    CHECK(w.toolTip() == "Click to play from there");
    // fewer samples than columns: each column one sample (np.reduceat)
    auto& l = w.lanes(20);
    CHECK(l.size() == 1 && l[0].mins.size() == 20);
    CHECK_NEAR(l[0].maxs[19], 0.9, 1e-6);
    auto& l2 = w.lanes(2);
    CHECK_NEAR(l2[0].mins[0], 0.0, 1e-6);
    CHECK_NEAR(l2[0].maxs[0], 0.4, 1e-6);
    CHECK_NEAR(l2[0].mins[1], 0.5, 1e-6);
    CHECK_NEAR(l2[0].maxs[1], 0.9, 1e-6);
    double got = -1;
    w.clicked.connect([&](double f) { got = f; });
    MouseEvent e;
    e.button = plat::LeftButton;
    e.pos = Point(25, 30);
    w.mousePress(e);
    CHECK_NEAR(got, 0.25, 1e-9);
    w.setAudio(nullptr, "", 10);
    got = -1;
    w.mousePress(e);
    CHECK(got < 0);
    CHECK(w.toolTip().empty());
}

int main() {
    App::get().initHeadless();
    util::makeDirs("build/test-explorer-settings");
    settings::setPath("build/test-explorer-settings/settings.json");
    testNames();
    testGeometryAndSnapping();
    testMouseEditing();
    testDuplicate();
    testVelocityRulerWheel();
    testPaintEditor();
    testIndexAndSearch();
    testFolderSettings();
    testWaveformPeaks();
    testExplorerWidget();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
