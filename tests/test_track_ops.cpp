// Track operations (model/track_ops.h, lane_panel.h TRACK SELECTION), each at its worst:
//   - testDuplicateChain: a track with effects in parallel, a sidechain from another track, a modulator, automation
//     on an effect and on its fader, sends and linked clips, duplicated: the copy's cables, modulation and lanes
//     point at its OWN effects (new instances with the same settings); names, places, groups
//   - testMoves: moving up and down through groups (in, out, over a folded one), at the ends, past the returns,
//     several at once, dropped onto another row
//   - testUndo: duplicating and moving are each one undo step, undone and redone whole
//   - testDuringPlayback: tracks duplicated (with clips and plugins), moved and removed the whole time while the
//     audio thread plays 96 tracks in small blocks
//   - testWindow: 200 tracks selected, every fader moved at once by one; mute for all; Ctrl+D, Alt+Up, a row
//     dragged; an effect added to three selected tracks at once; one undo step each
//   - testTerminal: the terminal UI's keys: Shift+Up/Down, M, Left/Right, [ ], Alt+Up/Down, Ctrl+D, F2's entries
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/history.h"
#include "model/track_ops.h"
#include "platform/platform.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "tui/app.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/plugin_dialog.h"
#include "ui/solo_button.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace {

constexpr int SR = 48000;
const std::string TMP = "build/test-track-ops";
std::mt19937 rng(20261120);
int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

SlotPtr loaded(const std::string& path) {
    std::string error;
    std::unique_ptr<plug::Plugin> p = plug::Plugin::load(path, "", error);
    if (!p) return nullptr;
    p->prepare(SR);
    auto slot = std::make_shared<PluginSlot>();
    slot->path = path;
    slot->kind = "effect";
    slot->name = p->name();
    slot->instance = std::shared_ptr<plug::Plugin>(std::move(p));
    return slot;
}

// what the program does with a copy's slots (PluginHost::restore, without the window)
void loadAll(const std::vector<SlotPtr>& slots) {
    for (auto& s : slots) {
        std::string error;
        std::unique_ptr<plug::Plugin> p = plug::Plugin::load(s->path, s->pluginName, error);
        if (!p) continue;
        p->prepare(SR);
        if (!s->state.empty()) plug::setState(*p, s->state);
        std::atomic_store(&s->instance, std::shared_ptr<plug::Plugin>(std::move(p)));
    }
}

effects::Effect* fx(const SlotPtr& s) { return s ? effects::asEffect(aload(s->instance).get()) : nullptr; }

ClipPtr audioClip(const TrackPtr& t, int64_t start, int64_t frames, float value) {
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, value);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "clip";
    c->track = t;
    c->start = start;
    c->data = buf;
    c->source = buf;
    c->sourceSr = SR;
    return c;
}

bool ofTrack(const End& e, const Track& t) {
    if (e.type != End::Slot && e.type != End::Sidechain) return true;
    for (auto& s : t.effects)
        if (s.get() == e.slot) return true;
    return false;
}

std::vector<std::string> names(const Project& p) {
    std::vector<std::string> out;
    for (auto& t : p.tracks) out.push_back(t->name);
    return out;
}

void frames(double seconds) {
    for (double t = 0; t < seconds; t += 0.02) {
        ui::App::get().advanceTimers(0.02);
        ui::App::get().tick();
    }
}

void walk(ui::Widget* w, const std::function<void(ui::Widget*)>& f) {
    f(w);
    for (auto& c : w->children()) walk(c.get(), f);
}

void mouse(MainWindow* w, plat::Event::Type type, Point at, int buttons) {
    plat::Event m;
    m.type = type;
    m.x = at.x, m.y = at.y;
    m.button = plat::LeftButton;
    m.buttons = buttons;
    w->onEvent(nullptr, m);
}

void key(MainWindow* w, int k, int mods) {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k;
    e.mods = mods;
    w->onEvent(nullptr, e);
    e.type = plat::Event::KeyUp;
    w->onEvent(nullptr, e);
}

}  // namespace

static void testDuplicateChain() {
    Project p;
    TrackPtr other = p.addTrack(Kind::Audio);  // "Audio 1"
    TrackPtr bass = p.addTrack(Kind::Audio);
    bass->name = "Bass";
    GroupPtr g = p.groupTracks({other, bass});
    TrackPtr below = p.addTrack(Kind::Audio);  // (outside the group, under it)
    TrackPtr ret = p.addTrack(Kind::Return);   // "Return A"
    SlotPtr eq = loaded("builtin:eq"), comp = loaded("builtin:compressor"), lfo = loaded("builtin:mod_lfo");
    CHECK(eq && comp && lfo);
    if (!eq || !comp || !lfo) return;
    fx(eq)->windowEdit(1, 0.66);
    eq->placed = true, eq->atX = 140, eq->atY = 60;
    comp->bypass = true;
    comp->mix = 0.5;
    bass->effects = std::vector<SlotPtr>{eq, comp, lfo};
    bass->cables = std::vector<Cable>{{End::in(), End::of(eq.get())},       {End::of(eq.get()), End::out()},
                                      {End::in(), End::of(comp.get())},     {End::of(comp.get()), End::out()},
                                      {End::of(eq.get()), End::sidechain(comp.get())},
                                      {End::source(other.get()), End::sidechain(comp.get())},
                                      {End::in(), End::of(lfo.get())}};
    ModCable m;
    m.from = lfo.get();
    m.to = eq.get();
    m.param = fx(eq)->params()[2].name;
    m.depth = -0.4;
    m.state->base = 0.3;
    bass->modCables = std::vector<ModCable>{m};
    auto laneOn = [&](const SlotPtr& s, const std::string& param) {
        auto a = std::make_shared<Automation>();
        a->slot = s;
        a->param = param;
        a->setPoints({{0, 0.2}, {1, 0.8, 0.5}});
        return a;
    };
    bass->automation = std::vector<AutomationPtr>{laneOn(eq, fx(eq)->params()[1].name), laneOn(bass->controls, "Volume")};
    bass->sends = std::vector<Send>{{ret, -6, true}};
    bass->volumeDb = -3;
    bass->pan = 0.25;
    bass->muted = true;
    bass->color = "#aa3300";
    p.addClip(audioClip(bass, 0, SR, 0.5f));
    p.addClip(audioClip(bass, SR * 2, SR, 0.25f));
    p.addClip(audioClip(other, 0, SR, 0.5f));

    TrackPtr c = track_ops::duplicate(p, bass, true, loadAll);
    CHECK(c->name == "Bass 2");
    CHECK(p.indexOf(c) == p.indexOf(bass) + 1 && c->group == g && p.indexOf(below) == p.indexOf(c) + 1);
    CHECK(c->volumeDb == -3 && c->pan == 0.25 && c->muted && c->color == "#aa3300" && c->sends == bass->sends);
    // its own effects: new instances with the same settings and places
    CHECK(c->effects.size() == 3);
    for (size_t i = 0; i < c->effects.size() && i < 3; i++) {
        CHECK(c->effects[i] != bass->effects[i] && c->effects[i]->path == bass->effects[i]->path);
        CHECK(aload(c->effects[i]->instance) && aload(c->effects[i]->instance) != aload(bass->effects[i]->instance));
    }
    if (c->effects.size() == 3) {
        CHECK_NEAR(fx(c->effects[0])->getParam(1), fx(eq)->getParam(1), 1e-9);
        CHECK(c->effects[0]->placed && c->effects[0]->atX == 140 && c->effects[1]->bypass && c->effects[1]->mix == 0.5);
    }
    // every cable as the original's, but to the copy's own effects (the other track's sidechain stays)
    CHECK(c->cables.size() == bass->cables.size());
    bool own = true, sidechain = false;
    for (auto& k : c->cables) {
        own = own && ofTrack(k.first, *c) && ofTrack(k.second, *c);
        sidechain = sidechain || (k.first.type == End::TrackSource && k.first.track == other.get());
    }
    CHECK(own && sidechain);
    CHECK(c->modCables.size() == 1);
    if (c->modCables.size() == 1 && c->effects.size() == 3) {
        const ModCable& k = c->modCables[0];
        CHECK(k.from == c->effects[2].get() && k.to == c->effects[0].get() && k.depth == -0.4 && k.param == m.param);
        CHECK(k.state != m.state && k.state->base.load() == 0.3);
    }
    CHECK(c->automation.size() == 2);
    if (c->automation.size() == 2 && c->effects.size() == 3) {
        CHECK(c->automation[0]->slot == c->effects[0] && c->automation[1]->slot == c->controls);
        CHECK(c->automation[0] != bass->automation[0] && c->automation[0]->points == bass->automation[0]->points);
    }
    auto copies = p.clipsOn(c);
    CHECK(copies.size() == 2 && p.clipsOn(bass).size() == 2);
    if (copies.size() == 2) CHECK(copies[0]->data == p.clipsOn(bass)[0]->data && copies[0] != p.clipsOn(bass)[0]);

    // names: again ("Bass 3"), a new track's name ("Audio" numbering goes on), a return ("Return B", among the returns)
    CHECK(track_ops::duplicate(p, bass, false, loadAll)->name == "Bass 3");
    CHECK(p.clipsOn(p.tracks[(size_t)p.indexOf(bass) + 1]).empty());
    TrackPtr o2 = track_ops::duplicate(p, other, false);
    CHECK(o2->name == "Audio 4" && p.indexOf(o2) == p.indexOf(other) + 1);
    CHECK(p.addTrack(Kind::Audio)->name == "Audio 5");
    TrackPtr r2 = track_ops::duplicate(p, ret, false);
    CHECK(r2->name == "Return B" && p.tracks[p.tracks.size() - 1] == r2 && r2->kind == Kind::Return && !r2->group);

    // linked MIDI clips stay linked (the copy's too)
    TrackPtr keys = p.addTrack(Kind::Midi);
    keys->needsInstrument = false;
    ClipPtr a = makeMidiClip("riff", {Note{0, 0.5, 60, 100}}, 1.0, SR);
    a->track = keys;
    ClipPtr b = links::linkedCopy(a);
    b->start = SR * 2;
    p.addClip(a);
    p.addClip(b);
    TrackPtr k2 = track_ops::duplicate(p, keys, true);
    CHECK(p.clipsOn(k2).size() == 2 && links::count(p, *a) == 4);
    // a frozen track's copy: frozen too, with a rendering of its own
    keys->frozen = audioClip(nullptr, 0, SR, 0.1f);
    TrackPtr k3 = track_ops::duplicate(p, keys, false);
    CHECK(k3->frozen && k3->frozen != keys->frozen && k3->frozen->data == keys->frozen->data);
    printf("duplicate: %zu tracks, every copy's cables, modulation and lanes on its own effects\n", p.tracks.size());
}

static void testMoves() {
    Project p;
    auto make = [&](const std::string& n, Kind k = Kind::Audio) {
        TrackPtr t = p.addTrack(k);
        t->name = n;
        return t;
    };
    TrackPtr A = make("A"), B = make("B"), C = make("C"), D = make("D"), E = make("E"), F = make("F");
    TrackPtr R1 = make("R1", Kind::Return), R2 = make("R2", Kind::Return);
    GroupPtr G = p.groupTracks({D, E});
    using V = std::vector<std::string>;
    CHECK(names(p) == (V{"A", "B", "C", "D", "E", "F", "R1", "R2"}));
    CHECK(!track_ops::step(p, {A}, -1));                 // the top
    CHECK(track_ops::step(p, {C}, 1) && C->group == G);  // into the group, at its top
    CHECK(names(p) == (V{"A", "B", "C", "D", "E", "F", "R1", "R2"}));
    CHECK(track_ops::step(p, {C}, 1) && names(p) == (V{"A", "B", "D", "C", "E", "F", "R1", "R2"}));
    CHECK(track_ops::step(p, {C}, -1) && track_ops::step(p, {C}, -1) && !C->group);  // out again, above it
    CHECK(names(p) == (V{"A", "B", "C", "D", "E", "F", "R1", "R2"}));
    CHECK(track_ops::step(p, {F}, -1) && F->group == G);  // into it at the bottom
    CHECK(track_ops::step(p, {F}, 1) && !F->group);
    CHECK(!track_ops::step(p, {F}, 1));                   // the returns stay below
    CHECK(!track_ops::step(p, {R1}, -1));
    CHECK(track_ops::step(p, {R2}, -1) && names(p) == (V{"A", "B", "C", "D", "E", "F", "R2", "R1"}));
    G->folded = true;  // over the whole folded group
    CHECK(track_ops::step(p, {C}, 1) && !C->group && names(p) == (V{"A", "B", "D", "E", "C", "F", "R2", "R1"}));
    CHECK(track_ops::step(p, {C}, -1) && names(p) == (V{"A", "B", "C", "D", "E", "F", "R2", "R1"}));
    G->folded = false;
    // several: stuck at the top together; down together, keeping their order
    CHECK(!track_ops::step(p, {A, B}, -1));
    CHECK(track_ops::step(p, {A, B}, 1) && names(p) == (V{"C", "A", "B", "D", "E", "F", "R2", "R1"}));
    CHECK(track_ops::step(p, {A, B, F}, 1) && F->group == nullptr);  // (F stuck at the returns; A, B into G)
    CHECK(names(p) == (V{"C", "A", "B", "D", "E", "F", "R2", "R1"}) && B->group == G && A->group == G);
    A->group = B->group = nullptr;
    p.gatherGroups();
    // dropped onto another row: into its group, above or below it
    CHECK(track_ops::moveTo(p, {A}, E, true) && A->group == G);
    CHECK(names(p) == (V{"C", "B", "D", "E", "A", "F", "R2", "R1"}));
    CHECK(track_ops::moveTo(p, {A, C}, B, false) && !A->group && names(p) == (V{"C", "A", "B", "D", "E", "F", "R2", "R1"}));
    CHECK(!track_ops::moveTo(p, {A}, A, true));
    CHECK(!track_ops::moveTo(p, {A}, C, true));  // (already there)
    // a return dropped among the tracks: still at the bottom, first of the returns now
    CHECK(track_ops::moveTo(p, {R1}, A, true) && names(p) == (V{"C", "A", "B", "D", "E", "F", "R1", "R2"}));
    // worst case: 2000 tracks in 50 groups, everything moved down and up again many times
    Project big;
    for (int i = 0; i < 2000; i++) big.addTrack(i % 20 == 19 ? Kind::Return : Kind::Audio);
    std::vector<TrackPtr> all = big.tracks.copy();
    for (int gi = 0; gi < 50; gi++) big.groupTracks({all[(size_t)gi * 30], all[(size_t)gi * 30 + 1]});
    std::vector<TrackPtr> some;
    for (size_t i = 0; i < all.size(); i += 3) some.push_back(all[i]);
    int moves = 0;
    for (int r = 0; r < 60; r++) moves += track_ops::step(big, some, r % 4 < 2 ? 1 : -1);
    bool returnsLast = true, whole = big.tracks.size() == 2000;
    bool seen = false;
    for (auto& t : big.tracks) {
        if (t->kind == Kind::Return) seen = true;
        else if (seen) returnsLast = false;
        if (t->kind == Kind::Return && t->group) returnsLast = false;
    }
    CHECK(moves > 0 && returnsLast && whole);
    // groups stay together
    bool together = true;
    for (auto& grp : big.groups()) {
        auto ms = big.members(grp);
        int first = big.indexOf(ms.front());
        for (size_t i = 0; i < ms.size(); i++) together = together && big.indexOf(ms[i]) == first + (int)i;
    }
    CHECK(together);
    printf("moves: groups in and out, folded, returns, several at once; 2000 tracks moved 60 times\n");
}

static void testUndo() {
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    SlotPtr eq = loaded("builtin:eq");
    if (eq) a->effects = std::vector<SlotPtr>{eq};
    a->cables = chain(*a->effects);
    p.addClip(audioClip(a, 0, SR, 0.5f));
    history::History h(p, SR);
    TrackPtr c = track_ops::duplicate(p, a, true, loadAll);
    CHECK(h.check(SR) && h.undoSteps() == 1 && h.labels().back() == "Add track");
    CHECK(h.undo(SR) && p.tracks.size() == 2 && p.clips.size() == 1);
    CHECK(h.redo(SR) && p.tracks.size() == 3 && p.clips.size() == 2 && p.tracks[1] == c && c->effects.size() == (eq ? 1 : 0));
    CHECK(!h.check(SR));
    CHECK(track_ops::step(p, {c}, 1));
    CHECK(h.check(SR) && h.undoSteps() == 2 && h.labels().back() == "Move tracks");
    CHECK(h.undo(SR) && p.tracks[1] == c && p.tracks[2] == b);
    GroupPtr g = p.groupTracks({b});
    h.check(SR);
    CHECK(track_ops::moveTo(p, {a}, b, true) && a->group == g);
    CHECK(h.check(SR) && h.undo(SR) && !a->group && p.tracks[0] == a);
}

static void testDuringPlayback() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    const int BLOCK = 64;
    const int64_t LENGTH = SR * 3;
    for (int i = 0; i < 96; i++) {
        TrackPtr t = p.addTrack(i % 16 == 15 ? Kind::Return : Kind::Audio);
        if (t->kind == Kind::Return) continue;
        p.addClip(audioClip(t, pick(0, SR), SR * 2, 0.1f));
        if (i % 8 == 0) {
            SlotPtr eq = loaded("builtin:eq"), comp = loaded("builtin:compressor");
            if (eq && comp) {
                t->effects = std::vector<SlotPtr>{eq, comp};
                t->cables = std::vector<Cable>{{End::in(), End::of(eq.get())}, {End::of(eq.get()), End::of(comp.get())},
                                               {End::of(eq.get()), End::sidechain(comp.get())}, {End::of(comp.get()), End::out()}};
            }
        }
    }
    patch::ensureStates(p);
    std::atomic<bool> done{false};
    bool finite = true;
    e.play();
    std::thread audio([&] {
        std::vector<float> out((size_t)BLOCK * 2);
        for (int64_t f = 0; f < LENGTH; f += BLOCK) {
            e.callback(nullptr, out.data(), BLOCK);
            for (float v : out)
                if (!std::isfinite(v)) finite = false;
        }
        done = true;
    });
    int duplicated = 0, stepped = 0, dropped = 0, removed = 0;
    while (!done) {
        auto ts = p.tracks.copy();
        TrackPtr t = ts[(size_t)pick(0, (int)ts.size() - 1)];
        track_ops::duplicate(p, t, pick(0, 1), loadAll);
        duplicated++;
        std::vector<TrackPtr> some;
        for (int i = 0; i < 8; i++) some.push_back(ts[(size_t)pick(0, (int)ts.size() - 1)]);
        stepped += track_ops::step(p, some, pick(0, 1) ? 1 : -1);
        dropped += track_ops::moveTo(p, {ts[(size_t)pick(0, (int)ts.size() - 1)]}, ts[(size_t)pick(0, (int)ts.size() - 1)], pick(0, 1));
        if (p.tracks.size() > 120) {
            TrackPtr gone = p.tracks[(size_t)pick(0, (int)p.tracks.size() - 1)];  // (held: the list is replaced)
            p.removeTrack(gone);
            p.forgetGoneTracks();
            removed++;
        }
        e.idle();
    }
    audio.join();
    printf("during playback: %d duplicated, %d steps, %d drops, %d removed while 3 s played in %d-frame blocks\n",
           duplicated, stepped, dropped, removed, BLOCK);
    CHECK(finite && duplicated > 0 && e.lastError().empty() && e.pos == LENGTH);
    e.stop();
}

static void testWindow() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;  // (shown to the toolkit, not painted: 200 rows a frame are slow to paint)
    auto* w = new MainWindow();
    w->askBeforeClosing = false;
    w->resizeWindow(1280, 800);
    w->showWindow();
    for (int i = 0; i < 200; i++) w->project.addTrack(Kind::Audio);  // (the rows made once)
    w->timeline->relayout();
    w->timeline->tracksChanged.emit();
    frames(0.4);
    LanePanel& panel = *w->panel;
    std::vector<TrackPtr> ts = w->project.tracks.copy();
    // 200 tracks selected (click, Shift-click), every fader moved at once by one
    panel.select(ts[0]);
    panel.select(ts[199], plat::ModShift);
    CHECK(panel.selected.size() == 200 && w->timeline->currentTrack == ts[199]);
    for (auto& t : ts) t->volumeDb = -10 + pick(0, 4);
    std::vector<double> before;
    for (auto& t : ts) before.push_back(t->volumeDb);
    panel.mixers[ts[5]].slider->setValue((int)ts[5]->volumeDb - 6);
    bool all = true;
    for (size_t i = 0; i < ts.size(); i++) all = all && ts[i]->volumeDb == before[i] - 6;
    CHECK(all);
    CHECK(panel.mixers[ts[150]].slider->value() == (int)ts[150]->volumeDb);
    // mute: every one of them, the status bar says how many
    ui::Button* mute = nullptr;
    walk(panel.frames[ts[3]], [&](ui::Widget* x) {
        if (auto* b = dynamic_cast<ui::Button*>(x); b && b->isCheckable() && b->text() == "M") mute = b;
    });
    CHECK(mute);
    if (mute) mute->click();
    all = true;
    for (auto& t : ts) all = all && t->muted;
    CHECK(all && w->status->message().find("200 tracks") != std::string::npos);
    // solo and (right-click) solo safe: every selected track too
    auto soloOn = [&](const TrackPtr& t) {
        SoloButton* found = nullptr;
        walk(panel.frames[t], [&](ui::Widget* x) { if (auto* b = dynamic_cast<SoloButton*>(x)) found = b; });
        return found;
    };
    CHECK(soloOn(ts[7]));
    if (SoloButton* solo = soloOn(ts[7])) solo->click();
    all = true;
    for (auto& t : ts) all = all && t->soloed && !t->soloSafe;
    CHECK(all);
    frames(0.4);  // (the rows built again)
    if (SoloButton* solo = soloOn(ts[7])) {
        ui::Menu menu;
        solo->fillMenu(menu);
        for (auto& a : menu.actions())
            if (a->text == "Solo safe") ui::Menu::activate(a.get());
    }
    all = true;
    for (auto& t : ts) all = all && t->soloSafe && t->soloed;
    CHECK(all);
    for (auto& t : ts) t->soloed = t->soloSafe = false;
    frames(0.4);
    // a click alone selects only it; Ctrl-click adds
    panel.select(ts[0]);
    panel.select(ts[1], plat::ModCtrl);
    panel.select(ts[2], plat::ModCtrl);
    CHECK(panel.selected.size() == 3 && panel.targets(ts[1]).size() == 3 && panel.targets(ts[9]).size() == 1);
    panel.select(ts[1], plat::ModCtrl);
    CHECK(panel.selected.size() == 2);
    panel.select(ts[1], plat::ModCtrl);
    frames(0.4);
    // Ctrl+D (nothing selected in the timeline): the three duplicated, one undo step
    int steps = w->history->undoSteps();
    w->timeline->selection.clear();
    w->timeline->duplicateSelected();
    frames(0.4);
    CHECK(w->project.tracks.size() == 203 && w->history->undoSteps() == steps + 1);
    CHECK(w->project.tracks[1]->name == "Audio 201" && panel.selected.size() == 3);
    // Alt+Up: the selected copies a lane up, one undo step
    std::vector<TrackPtr> copies = panel.selected;
    key(w, plat::Key_Up, plat::ModAlt);
    frames(0.4);
    CHECK(w->project.indexOf(copies[0]) == 0 && w->history->undoSteps() == steps + 2);
    w->undo();
    CHECK(w->project.indexOf(copies[0]) == 1);
    // a row dragged by its name below the third row under it
    TrackPtr first = w->project.tracks[0], third = w->project.tracks[2];
    frames(0.1);
    ui::Widget* row = panel.frames[first];
    ui::Widget* to = panel.frames[third];
    CHECK(row && to);
    if (row && to) {
        Point from = row->mapToWindow(Point(30, 12));
        Point at = to->mapToWindow(Point(30, to->height() - 4));
        mouse(w, plat::Event::MouseDown, from, plat::LeftButton);
        mouse(w, plat::Event::MouseMove, Point(from.x, from.y + 20), plat::LeftButton);
        mouse(w, plat::Event::MouseMove, at, plat::LeftButton);
        mouse(w, plat::Event::MouseUp, at, 0);
        frames(0.4);
        CHECK(w->project.indexOf(first) == w->project.indexOf(third) + 1);
    }
    // an effect added in a selected track's FX window: on all three, each wired in at the end of its chain
    panel.select(ts[10]);
    panel.select(ts[12], plat::ModShift);
    w->openTrackPlugins(ts[11]);
    PluginsDialog* d = w->fxWindows.count({0, ts[11].get()}) ? w->fxWindows[{0, ts[11].get()}] : nullptr;
    CHECK(d);
    SlotPtr eq = loaded("builtin:eq");
    if (d && eq) {
        d->place(eq);
        frames(0.2);
        for (int i = 10; i <= 12; i++) {
            const Track& t = *ts[(size_t)i];
            CHECK(t.effects.size() == 1);
            if (t.effects.size() != 1) continue;
            CHECK(aload(t.effects[0]->instance) && (i == 11 || t.effects[0] != eq));
            bool in = false, out = false;
            for (auto& c : t.cables) {
                in = in || (c.first.type == End::In && c.second.slot == t.effects[0].get());
                out = out || (c.first.slot == t.effects[0].get() && c.second.type == End::Out);
            }
            CHECK(in && out);
        }
        CHECK(w->status->message().find("3 tracks") != std::string::npos);
    }
    for (auto& [k, win] : std::map<std::pair<int, const void*>, PluginsDialog*>(w->fxWindows)) win->close();
    frames(0.2);
    printf("window: 200 faders and mutes at once, Ctrl+D, Alt+Up, a row dragged, an effect on 3 tracks\n");
    delete w;
    app.collectGarbage();
    app.paintWindows = true;
}

static void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->askBeforeClosing = false;
    w->resizeWindow(1280, 800);
    w->showWindow();
    for (int i = 0; i < 5; i++) w->timeline->addTrack(Kind::Audio);
    {
        tui::Front f(*w);
        tui::Screen screen{120, 40};
        auto press = [&](int k, int mods = 0) {
            f.key(tui::Key{k, mods, ""});
            app.advanceTimers(0.05);
            app.tick();
            f.draw(screen);
        };
        std::vector<TrackPtr> ts = w->project.tracks.copy();
        f.pane = tui::Front::Pane::Panel;
        f.setCurrent(ts[0]);
        press(plat::Key_Down, plat::ModShift);
        press(plat::Key_Down, plat::ModShift);
        CHECK(w->panel->selected.size() == 3 && w->timeline->currentTrack == ts[2]);
        CHECK(screen.lines().find("* Audio 2") != std::string::npos);
        press(plat::Key_Up, plat::ModShift);  // (back: two)
        CHECK(w->panel->selected.size() == 2);
        press(plat::Key_Down, plat::ModShift);
        press('M');
        CHECK(ts[0]->muted && ts[1]->muted && ts[2]->muted && !ts[3]->muted);
        press(plat::Key_Right);
        press('[');
        CHECK(ts[0]->volumeDb == 1 && ts[2]->volumeDb == 1 && ts[3]->volumeDb == 0);
        CHECK_NEAR(ts[1]->pan, -0.1, 1e-9);
        press(plat::Key_Down, plat::ModAlt);  // the three a lane down
        CHECK(w->project.tracks[0] == ts[3] && w->project.tracks[1] == ts[0] && w->timeline->currentTrack == ts[2]);
        press(plat::Key_Up, plat::ModAlt);
        CHECK(w->project.tracks[0] == ts[0]);
        press('D', plat::ModCtrl);  // duplicated, the copies selected
        CHECK(w->project.tracks.size() == 8 && w->panel->selected.size() == 3);
        // F2: the track's menu, Duplicate without clips (for the selection)
        press(plat::Key_F2);
        CHECK(f.menuOpen());
        int target = -1;
        if (f.menuOpen()) {
            auto& acts = f.open.back().menu->actions();
            for (int i = 0; i < (int)acts.size(); i++)
                if (acts[(size_t)i]->text.rfind("Duplicate without clips", 0) == 0) target = i;
        }
        CHECK(target >= 0);
        for (int i = 0; i < 40 && f.menuOpen() && f.open.back().item != target; i++) press(plat::Key_Down);
        press(plat::Key_Return);
        CHECK(w->project.tracks.size() == 11);
        press(plat::Key_Up);  // (alone: a single track again)
        CHECK(w->panel->selected.size() == 1);
        w->stop();
    }
    delete w;
    app.collectGarbage();
    app.paintWindows = true;
}

int main() {
    util::makeDirs(TMP);
    setenv("XDG_DATA_HOME", (TMP + "/data").c_str(), 1);
    setenv("TINYDAW_NO_UPDATE_CHECK", "1", 1);
    settings::setPath(TMP + "/settings.json");
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    for (auto test : {testDuplicateChain, testMoves, testUndo, testDuringPlayback, testWindow, testTerminal}) {
        double t0 = plat::now();
        test();
        printf("  (%.2f s)\n", plat::now() - t0);
    }
    TEST_MAIN_END();
}
