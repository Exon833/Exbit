// Linked MIDI clips on the timeline, headless (timeline.h, model.h): Ctrl+Alt+drag makes linked copies
// (staying on the grid while that Alt is held), Ctrl+drag plain ones; the right-click menu's Duplicate as
// linked and Make unique (with tooltips); splitting and joining make unique pieces; paste keeps clips
// copied together linked among themselves only; the loop switch and the MIDI editor's edits reach every
// linked clip; undo restores them together; the chain mark and its tooltip. And the MIDI effects window's
// Groove. Paints a sample into build/ui-samples/linked/.
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "ui/clip_painter.h"
#include "ui/clipboard.h"
#include "ui/midi_editor.h"
#include "ui/midi_effects_dialog.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;

Note note(double s, double e, double pitch, double vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

ClipPtr midiClip(Project& p, const TrackPtr& t, double start, double seconds) {
    auto c = makeMidiClip("Riff", {note(0, 0.25, 60), note(0.5, 0.75, 64), note(1.0, 1.5, 67)}, seconds, SR);
    c->track = t;
    c->start = (int64_t)std::llround(start * SR);
    c->length = (int64_t)std::llround(seconds * SR);
    p.addClip(c);
    return c;
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

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

struct Rig {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    Timeline* tl;
    TrackPtr m1, m2;
    Rig(int w = 1200, int h = 420) {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(w, h);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        m1 = project.addTrack(Kind::Midi);  // lane at y 24…96 (its clips' name strips: y 26…42)
        m1->needsInstrument = false;
        m2 = project.addTrack(Kind::Midi);
        m2->needsInstrument = false;
        tl->relayout();
    }
    int64_t sec(double s) const { return (int64_t)std::llround(s * SR); }
};

void testCtrlAltDrag() {
    Rig r;
    Timeline& tl = *r.tl;
    auto clip = midiClip(r.project, r.m1, 1, 2);  // x 100…300
    tl.relayout();
    // Ctrl+Alt+drag on its name strip: a linked copy moves, the clip stays
    auto e = mouse(150, 30, plat::LeftButton, plat::ModCtrl | plat::ModAlt);
    tl.mousePress(e);
    CHECK(std::holds_alternative<CopyDrag>(tl.drag) && std::get<CopyDrag>(tl.drag).linked);
    e = mouse(463, 30, plat::LeftButton, plat::ModCtrl | plat::ModAlt);  // (+3.13 s)
    tl.mouseMove(e);
    CHECK(std::holds_alternative<MoveDrag>(tl.drag) && std::get<MoveDrag>(tl.drag).altLinks);
    CHECK(r.project.clips.size() == 2);
    ClipPtr copy = r.project.clips.size() == 2 ? r.project.clips[1] : nullptr;
    // that Alt doesn't free it: it is on the grid
    CHECK(copy && copy->start % (SR / 4) == 0);
    // Alt let go, then held again: freely
    e = mouse(463, 30, plat::LeftButton, plat::ModCtrl);
    tl.mouseMove(e);
    e = mouse(463, 30, plat::LeftButton, plat::ModCtrl | plat::ModAlt);
    tl.mouseMove(e);
    CHECK(copy && copy->start == r.sec(1) + tl.frameAt(463) - tl.frameAt(150));
    tl.mouseRelease(e);
    CHECK(r.project.clips.size() == 2 && clip->start == r.sec(1));
    CHECK(copy && clip->link && copy->link == clip->link && copy->notes == clip->notes && links::linked(r.project, *clip));
    // plain Ctrl+drag of a linked clip: an independent copy
    tl.selection.clear();
    e = mouse(150, 30, plat::LeftButton, plat::ModCtrl);
    tl.mousePress(e);
    CHECK(std::holds_alternative<CopyDrag>(tl.drag) && !std::get<CopyDrag>(tl.drag).linked);
    e = mouse(150, 100, plat::LeftButton, plat::ModCtrl);  // (onto the second MIDI lane)
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(r.project.clips.size() == 3);
    if (r.project.clips.size() == 3) {
        ClipPtr plain = r.project.clips[2];
        CHECK(!plain->link && plain->track == r.m2 && links::count(r.project, *clip) == 2);
    }
    // an audio clip in a Ctrl+Alt drag: copied plainly
    auto a = r.project.addTrack(Kind::Audio);
    auto audio = makeAudioClip("Tone", std::make_shared<AudioBuffer>(), SR, SR);
    CHECK(!links::linkedCopy(audio)->link && !audio->link);
    (void)a;
}

void testMenuAndEdits() {
    Rig r;
    Timeline& tl = *r.tl;
    history::History h(r.project, SR);
    auto clip = midiClip(r.project, r.m1, 0, 2);
    tl.relayout();
    tl.selection.selectOnly(clip);
    // the right-click menu: Duplicate as linked (with a tooltip); Make unique off (not linked)
    {
        ui::Menu menu;
        tl.fillMenu(menu, Point(50, 50));
        ui::Action* dup = actionStarting(menu, "Duplicate as linked");
        ui::Action* unique = actionStarting(menu, "Make unique");
        CHECK(dup && dup->enabled && !dup->tooltip.empty());
        CHECK(unique && !unique->enabled && !unique->tooltip.empty());
        if (dup) ui::Menu::activate(dup);
    }
    CHECK(h.check(SR));
    CHECK(r.project.clips.size() == 2);
    ClipPtr copy = r.project.clips.size() == 2 ? r.project.clips[1] : nullptr;
    if (!copy) return;
    CHECK(copy->start == r.sec(2) && copy->link == clip->link && clip->link && tl.selection.contains(copy) &&
          !tl.selection.contains(clip));
    // the chain mark: its tooltip says how many share the notes; elsewhere no tip
    RectF rect = *tl.clipRect(*copy, tl.trackTops()), badge;
    CHECK(clip_painter::linkBadge(rect, badge));
    Point at((int)(badge.x + badge.w / 2), (int)(badge.y + badge.h / 2));
    CHECK(tl.linkBadgeAt(PointF(at)) == copy);
    std::string tip = tl.toolTipAt(at);
    CHECK(tip.find("2 clips") != std::string::npos);
    CHECK(tl.toolTipAt(Point(at.x - 80, at.y)).find("Linked") == std::string::npos);
    // painted: the mark is there on a linked clip, not on an unlinked one
    ui::App::get().doLayouts();
    auto paintAt = [&](const std::string& name) {
        Image img(std::min(480, tl.width()), std::min(170, tl.height()), theme::c("window"));  // (the clips)
        Painter p(img);
        tl.paintArea(p, Rect(0, 0, img.width(), img.height()));
        util::makeDirs("build/ui-samples/linked");
        savePng("build/ui-samples/linked/" + name + ".png", img);
        return img;
    };
    Image linked = paintAt("linked");
    // the loop switch reaches the linked copy
    tl.setLoopNotes({clip}, true);
    CHECK(clip->loop && copy->loop && copy->lengthS == clip->lengthS);
    tl.setLoopNotes({clip}, false);
    CHECK(!copy->loop);
    // Make unique (now on): the copy leaves, so the clip is alone (no link at all)
    tl.selection.selectOnly(copy);
    {
        ui::Menu menu;
        tl.fillMenu(menu, Point(250, 50));
        ui::Action* unique = actionStarting(menu, "Make unique");
        CHECK(unique && unique->enabled);
        if (unique) ui::Menu::activate(unique);
    }
    CHECK(!copy->link && !clip->link && copy->notes == clip->notes);
    Image unlinked = paintAt("unlinked");
    int differ = 0;
    for (int y = (int)badge.y; y < (int)badge.bottom(); y++)
        for (int x = (int)badge.x; x < (int)badge.right(); x++) differ += linked.pixel(x, y) != unlinked.pixel(x, y);
    CHECK(differ > 4);
    CHECK(tl.linkBadgeAt(PointF(at)) == nullptr);
    // undo: linked again, both
    CHECK(h.check(SR));
    CHECK(h.undo(SR));
    CHECK(copy->link && copy->link == clip->link);
    // Make unique off for a frozen-free unlinked selection; Duplicate as linked off for audio only
    auto a = r.project.addTrack(Kind::Audio);
    auto audio = makeAudioClip("Tone", std::make_shared<AudioBuffer>(), SR, SR);
    audio->track = a;
    r.project.addClip(audio);
    tl.selection.selectOnly(audio);
    {
        ui::Menu menu;
        tl.fillMenu(menu, Point(50, 50));
        CHECK(!actionStarting(menu, "Duplicate as linked")->enabled && !actionStarting(menu, "Make unique")->enabled);
    }
}

void testSplitJoinPaste() {
    Rig r;
    Timeline& tl = *r.tl;
    auto a = midiClip(r.project, r.m1, 0, 2);
    ClipPtr b = links::linkedCopy(a);
    b->start = r.sec(3);
    r.project.addClip(b);
    ClipPtr c = links::linkedCopy(a);
    c->start = r.sec(6);
    r.project.addClip(c);
    tl.relayout();
    // split b at the cursor (S): its pieces are unique; a and c stay linked
    tl.selection.selectOnly(b);
    tl.setCursor(r.sec(4));
    tl.splitSelected();
    CHECK(r.project.clips.size() == 4);
    int unlinked = 0;
    for (auto& x : r.project.clips) unlinked += !x->link;
    CHECK(unlinked == 2 && a->link && c->link == a->link);
    // join a and c (Ctrl+J; not neighbours, but on one track): one new clip, not linked
    tl.selection.clear();
    tl.selection.clips = {a, c};
    tl.joinSelected();
    CHECK(r.project.clips.size() == 3);
    for (auto& x : r.project.clips) CHECK(!x->link || !links::linked(r.project, *x));
    // paste: linked clips copied together are linked among themselves, not to the originals
    Rig s;
    Timeline& t2 = *s.tl;
    auto x = midiClip(s.project, s.m1, 0, 1);
    ClipPtr y = links::linkedCopy(x);
    y->start = s.sec(1);
    s.project.addClip(y);
    auto z = midiClip(s.project, s.m1, 2, 1);
    t2.relayout();
    t2.selection.clear();
    t2.selection.clips = {x, y, z};
    CHECK(clipboard::copy(t2) == 3);
    t2.setCurrentTrack(s.m2);
    t2.setCursor(s.sec(4));
    std::string reason;
    auto placed = clipboard::paste(t2, reason);
    CHECK(reason.empty() && placed.size() == 3);
    if (placed.size() == 3) {
        CHECK(placed[0]->link && placed[1]->link == placed[0]->link && placed[0]->link != x->link && !placed[2]->link);
        CHECK(links::count(s.project, *x) == 2 && links::count(s.project, *placed[0]) == 2);
    }
    // one linked clip copied alone: an independent paste
    t2.selection.clear();
    t2.selection.clips = {x};
    CHECK(clipboard::copy(t2) == 1);
    t2.setCursor(s.sec(10));
    placed = clipboard::paste(t2, reason);
    CHECK(placed.size() == 1 && !placed[0]->link && links::count(s.project, *x) == 2);
    // Ctrl+D: an independent copy
    t2.selection.clear();
    t2.selection.clips = {y};
    t2.duplicateSelected();
    CHECK(t2.selection.clips.size() == 1 && !(*t2.selection.clips.begin())->link && links::count(s.project, *x) == 2);
}

void testEditorEdits() {
    Rig r;
    auto a = midiClip(r.project, r.m1, 0, 2);
    ClipPtr b = links::linkedCopy(a);
    b->start = r.sec(4);
    b->track = r.m2;
    b->prepare(SR);
    r.project.addClip(b);
    int changes = 0;
    MidiEditor::TimelineLink link;
    MidiEditor ed(nullptr, a, r.project, r.engine, [&] { changes++; }, link);
    ed.showWindow();
    MidiEditor other(nullptr, b, r.project, r.engine, [&] {}, link);
    // a note edit (the editor's working copy, committed): b gets the notes, prepared
    ed.notes.push_back(note(1.75, 1.9, 72));
    ed.commit();
    CHECK(changes == 1 && b->notes == a->notes && b->notes->size() == 4 && b->events->size() == 4);
    // quantize: both
    ed.selected.clear();
    ed.quantize(1.0);
    CHECK(b->notes == a->notes);
    for (const Note& n : *b->notes) CHECK_NEAR(std::fmod(n.start + 1e-9, 0.125), 0.0, 1e-6);
    // switching notes off: both
    ed.selected = {0};
    ed.toggleOff();
    CHECK(b->mutedNotes == a->mutedNotes && a->mutedNotes && a->mutedNotes->size() == 1);
    // the other's editor shows them once reloaded (the main window does it: notesEdited)
    other.reload();
    CHECK(other.notes.size() == 4 && other.off.size() == 1);
    // its place and window stay its own
    CHECK(b->start == r.sec(4) && b->track == r.m2 && a->start == 0);
    ed.close();
    other.close();
}

void testGrooveWindow() {
    Rig r;
    sampler_dialog::Host host;
    host.project = &r.project;
    host.engine = &r.engine;
    host.relayout = [] {};
    sampler_dialog::setHost(host);
    midi_effects_dialog::MidiEffectsDialog* d = midi_effects_dialog::openMidiEffects(r.m1);
    CHECK(d != nullptr);
    if (!d) return;
    midi_effects_dialog::MidiEffectsDialog& dialog = *d;
    ui::Menu* menu = dialog.addButton->menu();
    ui::Action* groove = menu ? actionStarting(*menu, "Groove") : nullptr;
    CHECK(groove && groove->text == "Groove  (clips only)" && !groove->tooltip.empty());
    if (menu)
        for (auto& act : menu->actions()) CHECK(!act->tooltip.empty());
    if (groove) ui::Menu::activate(groove);
    CHECK(r.m1->midiEffects.size() == 1 && r.m1->midiEffects[0].kind == "Groove");
    const midi_effects_dialog::Kind* kind = midi_effects_dialog::kindOf("Groove");
    CHECK(kind && !kind->live && kind->params.size() == 6);
    if (kind) {
        CHECK(kind->params[0].text(66) == "66.0 %");
        CHECK(kind->params[1].text(1) == "1/16");
        CHECK(kind->params[3].text(5) == "5.0 ms");
    }
    dialog.setValue(0, "swing", 70);
    CHECK(r.m1->midiEffects[0].get("swing") == 70);
    dialog.close();
}

}  // namespace

int main() {
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs("build/test-linked");
    std::remove("build/test-linked/settings.json");
    settings::setPath("build/test-linked/settings.json");
    testCtrlAltDrag();
    testMenuAndEdits();
    testSplitJoinPaste();
    testEditorEdits();
    testGrooveWindow();
    TEST_MAIN_END();
}
