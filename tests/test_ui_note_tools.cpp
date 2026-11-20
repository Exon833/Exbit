// The MIDI editor's note tools, headless: Tools ▾ opens the panel, its preview is drawn but never put in the clip,
// Cancel drops it, Apply is one change (one undo step), 1,000 times over with undo; switching clips and closing
// (or deleting) the editor drop the preview; the tools at the clip's edges; the knife (K: a drag across notes,
// Enter at the keyboard's cursor), Ctrl+J, Shift+click a key (and P), the two overlap commands of Tools ▾'s menu;
// and changes to the notes while a preview shows. Pictures in build/ui-samples/note-tools/.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "model/model.h"
#include "ui/midi_editor.h"
#include "ui/note_tools_panel.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;
using note_tools::Tool;

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
void press(Widget* w, Point p, int mods = 0) {
    gTime += 1.0;
    Window* win = w->window();
    Point q = w->mapToWindow(p);
    win->onEvent(nullptr, mouseEv(plat::Event::MouseMove, q, 0, 0, mods));
    win->onEvent(nullptr, mouseEv(plat::Event::MouseDown, q, plat::LeftButton, plat::LeftButton, mods));
}
void dragTo(Widget* w, Point p, int mods = 0) {
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseMove, w->mapToWindow(p), 0, plat::LeftButton, mods));
}
void release(Widget* w, Point p, int mods = 0) {
    w->window()->onEvent(nullptr, mouseEv(plat::Event::MouseUp, w->mapToWindow(p), plat::LeftButton, 0, mods));
}
void key(Window& w, int k, int mods = 0) {
    for (auto type : {plat::Event::KeyDown, plat::Event::KeyUp}) {
        plat::Event e;
        e.type = type;
        e.key = k;
        e.mods = mods;
        e.time = gTime;
        w.onEvent(nullptr, e);
    }
}

Note note(double s, double e, int pitch, int vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

void picture(Window& w, const std::string& name) {
    util::makeDirs("build/ui-samples/note-tools");
    w.update();
    App::get().tick();
    savePng("build/ui-samples/note-tools/" + name + ".png", w.framebuffer());
}

struct Fixture {
    Project project;
    audio::Engine engine{project};
    TrackPtr track;
    ClipPtr clip, other;
    int changes = 0;
    std::vector<ClipPtr> edited;
    Fixture() {
        project.timing.bpm = 120;  // a beat = 0.5 s
        project.timing.grid = 1;
        track = project.addTrack(Kind::Midi);
        track->needsInstrument = false;
        clip = makeMidiClip("Chords", {note(0, 1, 60), note(0, 1, 64), note(0, 1, 67), note(2, 3, 62)}, 4.0, engine.sr);
        clip->track = track;
        project.addClip(clip);
        other = makeMidiClip("Other", {note(0, 1, 48)}, 2.0, engine.sr);
        other->track = track;
        other->start = engine.sr * 8;
        project.addClip(other);
    }
    MidiEditor::TimelineLink link() {
        MidiEditor::TimelineLink l;
        l.cursor = [] { return (int64_t)0; };
        l.setCursor = [](int64_t) {};
        l.seek = [](int64_t) {};
        l.editNotes = [this](ClipPtr c) { edited.push_back(c); };
        return l;
    }
};

Action* actionOf(Menu& m, const std::string& start) {
    for (auto& a : m.actions())
        if (a->text.compare(0, start.size(), start) == 0) return a.get();
    return nullptr;
}

double minStart(const Clip& c) {
    double m = 1e300;
    for (const Note& n : *c.notes) m = std::min(m, n.start);
    return m;
}
}  // namespace

// --- the panel: preview, Cancel, Apply ---
static void testPanel() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.showWindow();
    auto before = f.clip->notes;
    CHECK(ed.toolsButton && ed.toolsButton->menu());
    ed.toolsButton->click();
    NoteToolsPanel* panel = ed.toolsPanel;
    CHECK(panel && panel->isWindowShown());
    if (!panel) return;
    panel->setTool(Tool::Chop);
    panel->spins[0]->setValue(2);
    CHECK(ed.preview.size() == 8);  // (none selected: all four, each in two)
    CHECK(f.clip->notes == before && f.changes == 0);  // (a preview never reaches the clip)
    picture(ed, "preview-chop");
    picture(*panel, "panel-chop");
    // the selection: only its notes
    ed.selected = {0};
    panel->updatePreview();
    CHECK(ed.preview.size() == 2 && panel->target->text() == "On the 1 selected notes");
    // Cancel
    panel->reject();
    CHECK(!ed.toolsPanel && ed.preview.empty() && f.clip->notes == before && f.changes == 0);
    App::get().collectGarbage();
    // Apply: into the clip, once
    ed.selected.clear();
    ed.openTools();
    CHECK(ed.toolsPanel);
    if (!ed.toolsPanel) return;
    ed.toolsPanel->setTool(Tool::Strum);
    ed.toolsPanel->spins[0]->setValue(30);
    ed.toolsPanel->choices[0]->setCurrentIndex(0);
    ed.toolsPanel->applyButton->click();
    CHECK(!ed.toolsPanel && ed.preview.empty());
    CHECK(f.changes == 1 && f.clip->notes != before && f.clip->notes->size() == 4);
    std::vector<Note> ns = *f.clip->notes;
    CHECK(ns.size() == 4 && std::fabs(ns[1].start - 0.03) < 1e-9 && std::fabs(ns[2].start - 0.06) < 1e-9);
    // the controls are remembered for the next time
    ed.openTools();
    CHECK(ed.toolsPanel && ed.toolsPanel->tool->currentIndex() == (int)Tool::Strum &&
          ed.toolsPanel->spins[0]->value() == 30);
    // the notes change while it previews (an edit, an undo): the preview follows them
    size_t was = ed.preview.size();
    ed.notes.push_back(note(3, 3.5, 70));
    ed.commit();
    CHECK(ed.preview.size() == was + 1);
    // every tool previews without touching the clip
    auto now = f.clip->notes;
    for (int t = 0; t < note_tools::TOOL_COUNT; ++t) {
        ed.toolsPanel->tool->setCurrentIndex(t);
        CHECK(f.clip->notes == now);
        CHECK(!ed.preview.empty());
    }
    ed.close();
    CHECK(!ed.toolsPanel && ed.preview.empty());
    App::get().collectGarbage();
}

// --- Apply, Cancel and undo, 1,000 times ---
static void testThousand() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.showWindow();
    // (a short clip: each Apply and undo renders it again)
    f.clip->lengthS = 0.25;
    ed.notes = {note(0, 0.1, 60), note(0, 0.1, 64), note(0, 0.1, 67), note(0.15, 0.2, 62)};
    ed.off.clear();
    ed.commit();
    history::History h(f.project, f.engine.sr);
    std::vector<Note> original = *f.clip->notes;
    auto t0 = std::chrono::steady_clock::now();
    int bad = 0;
    for (int i = 0; i < 1000; ++i) {
        ed.openTools();
        if (!ed.toolsPanel) {
            bad++;
            break;
        }
        ed.toolsPanel->setTool((Tool)(i % note_tools::TOOL_COUNT));
        if (i % 3 == 2) {  // Cancel: nothing changes
            ed.toolsPanel->reject();
            if (h.check(f.engine.sr)) bad++;
        } else {
            ed.toolsPanel->apply();
            h.check(f.engine.sr);
            if (h.canUndo()) h.undo(f.engine.sr);  // (Legato on these: nothing to do, nothing to undo)
            ed.reload();
        }
        const std::vector<Note>& back = *f.clip->notes;
        bool same = back.size() == original.size() && ed.preview.empty() && !ed.toolsPanel;
        for (size_t k = 0; same && k < back.size(); ++k)
            same = back[k].start == original[k].start && back[k].end == original[k].end &&
                   back[k].pitch == original[k].pitch && back[k].velocity == original[k].velocity;
        if (!same) bad++;
        if (i % 50 == 0) App::get().collectGarbage();
    }
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  1000 x (open, apply or cancel, undo): %.2f s\n", took);
    CHECK(bad == 0);
    CHECK(took < 30.0);  // (about 5 s here alone: a new panel each time; generous for a loaded machine)
    CHECK(!h.canUndo());
    ed.close();
    App::get().collectGarbage();
}

// --- switching clips, closing or deleting the editor: the preview goes, never left applied ---
static void testSwitchAndClose() {
    Fixture f;
    auto before = f.clip->notes;
    {
        MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
        ed.deleteOnClose = false;  // (on the stack)
        ed.showWindow();
        ed.openTools();
        ed.toolsPanel->setTool(Tool::Arpeggiate);
        CHECK(!ed.preview.empty());
        ed.editOther(f.other);
        CHECK(f.edited.size() == 1 && !ed.toolsPanel && ed.preview.empty());
        CHECK(f.clip->notes == before && f.changes == 0);
        ed.close();
    }
    App::get().collectGarbage();
    // deleted with the panel open: the panel closes, nothing applied
    auto* ed = new MidiEditor(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed->showWindow();
    ed->openTools();
    NoteToolsPanel* panel = ed->toolsPanel;
    CHECK(panel);
    delete ed;
    CHECK(panel && !panel->isWindowShown() && !panel->editor);
    App::get().collectGarbage();
    CHECK(f.clip->notes == before && f.changes == 0);
    // the editor's clip is gone (deleted elsewhere): Apply closes the editor, and nothing breaks
    MidiEditor ed2(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed2.deleteOnClose = false;  // (on the stack)
    ed2.showWindow();
    ed2.openTools();
    f.project.removeClips({f.clip});
    if (ed2.toolsPanel) ed2.toolsPanel->apply();
    CHECK(!ed2.toolsPanel && ed2.preview.empty());
    ed2.close();
    App::get().collectGarbage();
}

// --- the tools at the clip's edges: nothing before 0; past the end, the clip grows as with any note ---
static void testEdges() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.showWindow();
    for (int t = 0; t < note_tools::TOOL_COUNT; ++t) {
        ed.notes = {note(0, 0.002, 60), note(0, 0.5, 64), note(0.001, 0.4, 67), note(3.9, 4.0, 60)};
        ed.off.clear();
        ed.selected.clear();
        ed.commit();
        ed.openTools();
        ed.toolsPanel->setTool((Tool)t);
        ed.toolsPanel->spins[0]->setValue(ed.toolsPanel->spins[0]->maximum());  // (the most: the longest strum…)
        ed.toolsPanel->choices[0]->setCurrentIndex(1);                         // (down, grid steps…)
        for (const Note& n : ed.preview) CHECK(n.start >= 0 && n.end > n.start);
        ed.toolsPanel->apply();
        CHECK(minStart(*f.clip) >= 0);
        for (const Note& n : *f.clip->notes) CHECK(n.end > n.start);
        double last = 0;
        for (const Note& n : *f.clip->notes) last = std::max(last, n.end);
        CHECK(f.clip->lengthS >= last - 1e-9);
    }
    ed.close();
    App::get().collectGarbage();
}

// --- the knife, join, a key's row, the overlap commands ---
static void testCommands() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.resizeWindow(960, 560);
    ed.showWindow();
    ed.pps = 200;
    ed.updateRanges();
    // K: the knife; a drag at 0.5 s from the row of 60 to the row of 64 cuts both (not 67, not 62)
    key(ed, 'K');
    CHECK(ed.knife);
    int x = (int)ed.xOf(0.5);
    press(ed.roll, Point(x, (int)*ed.yOf(60) + ed.rowH / 2));
    dragTo(ed.roll, Point(x, (int)*ed.yOf(64) + ed.rowH / 2));
    picture(ed, "knife");
    release(ed.roll, Point(x, (int)*ed.yOf(64) + ed.rowH / 2));
    CHECK(f.clip->notes->size() == 6);
    int at60 = 0, at67 = 0;
    for (const Note& n : *f.clip->notes) {
        if ((int)n.pitch == 60) {
            at60++;
            CHECK(n.end == 0.5 || n.start == 0.5);
        }
        if ((int)n.pitch == 67) at67++;
    }
    CHECK(at60 == 2 && at67 == 1);
    // Enter with the knife: at the keyboard's cursor, on its row (none selected)
    ed.keyT = 0.25;
    ed.keyPitch = 67;
    key(ed, plat::Key_Return);
    CHECK(f.clip->notes->size() == 7);
    // Esc puts it away (the selection stays)
    key(ed, plat::Key_Escape);
    CHECK(!ed.knife);
    // Shift+click the key of 60: its row selected; Ctrl+J joins them back
    press(ed.roll, Point(10, (int)*ed.yOf(60) + ed.rowH / 2), plat::ModShift);
    release(ed.roll, Point(10, (int)*ed.yOf(60) + ed.rowH / 2), plat::ModShift);
    CHECK(ed.selected.size() == 2);
    for (int i : ed.selected) CHECK((int)ed.notes[(size_t)i].pitch == 60);
    key(ed, 'J', plat::ModCtrl);
    CHECK(f.clip->notes->size() == 6);
    // P: the row of the selected note (here 60: one note again)
    key(ed, 'P');
    CHECK(ed.selected.size() == 1);
    // Tools ▾'s menu: the overlaps of a pitch, then legato across pitches
    ed.selected.clear();
    ed.notes = {note(0, 1, 60), note(0.5, 1.5, 60), note(0.25, 2, 64), note(3, 4, 67)};
    ed.off = {1};  // (a switched-off note keeps being switched off)
    ed.commit();
    Menu menu;
    ed.toolsMenu(menu);
    Action* overlaps = actionOf(menu, "Remove overlaps (same pitch)");
    Action* legato = actionOf(menu, "Legato");
    CHECK(overlaps && legato && actionOf(menu, "Tools"));
    if (overlaps) Menu::activate(overlaps);
    double end60 = 0;
    for (const Note& n : ed.notes)
        if ((int)n.pitch == 60 && n.start == 0) end60 = n.end;
    CHECK_NEAR(end60, 0.5, 1e-9);
    CHECK(f.clip->mutedNotes && f.clip->mutedNotes->size() == 1);
    if (legato) Menu::activate(legato);
    for (const Note& n : ed.notes) {
        if (n.start == 0) CHECK_NEAR(n.end, 0.25, 1e-9);
        if (n.start == 0.25) CHECK_NEAR(n.end, 0.5, 1e-9);
        if (n.start == 0.5) CHECK_NEAR(n.end, 3.0, 1e-9);
        if (n.start == 3) CHECK_NEAR(n.end, 4.0, 1e-9);
    }
    // everything at once: the knife, a drag across every row, many times over (no crash, nothing before 0)
    key(ed, 'K');
    for (int i = 0; i < 50; ++i) {
        int cx = (int)ed.xOf(0.125 * (i % 30));
        press(ed.roll, Point(cx, 2));
        dragTo(ed.roll, Point(cx, ed.roll->height() - 2));
        release(ed.roll, Point(cx, ed.roll->height() - 2));
    }
    CHECK(minStart(*f.clip) >= 0);
    key(ed, 'K');
    ed.close();
    App::get().collectGarbage();
}

// --- with Snap to scale on and a scale (Phase A): the tools move notes in time only, never off their pitches;
// the knife and join keep them too ---
static void testWithScale() {
    Fixture f;
    f.project.timing.key = 0;
    f.project.timing.scale = "Major";
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.showWindow();
    ed.snapScaleButton->setChecked(true);
    ed.notes = {note(0, 1, 61), note(0, 1, 66), note(1, 2, 70)};  // (outside C major on purpose)
    ed.off.clear();
    ed.commit();
    for (int t = 0; t < note_tools::TOOL_COUNT; ++t) {
        ed.openTools();
        ed.toolsPanel->setTool((Tool)t);
        ed.toolsPanel->apply();
        for (const Note& n : *f.clip->notes) CHECK((int)n.pitch == 61 || (int)n.pitch == 66 || (int)n.pitch == 70);
    }
    ed.splitNotes(0.5, ed.toolTargets());
    ed.joinNotes();
    for (const Note& n : *f.clip->notes) CHECK((int)n.pitch == 61 || (int)n.pitch == 66 || (int)n.pitch == 70);
    ed.snapScaleButton->setChecked(false);
    ed.close();
    App::get().collectGarbage();
}

// --- 100,000 notes: every tool previewed (painted too), one applied, the knife across all of them ---
static void testHuge() {
    Fixture f;
    MidiEditor ed(nullptr, f.clip, f.project, f.engine, [&] { f.changes++; }, f.link());
    ed.deleteOnClose = false;  // (on the stack)
    ed.showWindow();
    std::vector<Note> many;
    for (int i = 0; i < 100000; ++i) many.push_back(note((i % 4000) * 0.01, (i % 4000) * 0.01 + 0.05, 36 + i % 48));
    ed.notes = many;
    ed.off.clear();
    ed.selected.clear();
    auto t0 = std::chrono::steady_clock::now();
    ed.openTools();
    for (int t = 0; t < note_tools::TOOL_COUNT && ed.toolsPanel; ++t) {
        ed.toolsPanel->tool->setCurrentIndex(t);
        CHECK(!ed.preview.empty());
        ed.update();
        App::get().tick();  // (painted with its preview)
    }
    double previewed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    if (ed.toolsPanel) {
        ed.toolsPanel->setTool(Tool::VelocityShape);
        ed.toolsPanel->apply();
    }
    double applied = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  100000 notes: every tool previewed and painted %.2f s, applied %.2f s\n", previewed, applied);
    CHECK(f.clip->notes->size() == 100000 && f.changes == 1);
    CHECK(previewed < 20.0);
    ed.removeOverlaps();
    ed.joinNotes();
    ed.splitNotes(10.0, ed.toolTargets());
    CHECK(minStart(*f.clip) >= 0);
    ed.close();
    App::get().collectGarbage();
}

int main() {
    App::get().initHeadless();
    util::makeDirs("build/test-note-tools");
    std::remove("build/test-note-tools/settings.json");
    settings::setPath("build/test-note-tools/settings.json");
    midi_effects::install(nullptr);
    testPanel();
    testThousand();
    testSwitchAndClose();
    testEdges();
    testCommands();
    testWithScale();
    testHuge();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
