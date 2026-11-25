// Commit to notes (ui/midi_commit.h) and switching MIDI effects off (MidiEffect::bypass), in the worst case:
//   - testThousandClips: 1,000 MIDI clips (looping ones, linked ones, notes with chances and repeats) through a
//     chord → arpeggiator → note length → groove chain with the song swinging, committed on every core: each
//     plays exactly what it played, the effects are switched off, the track plays straight, one undo step
//     (and undo brings all of it back)
//   - testNewTrack: committed onto a new track below: the new clips play what the originals did, the originals
//     are kept switched off, the original track's effects untouched; one undo step
//   - testSomeClips: one clip of several (and a linked one): only it holds the effects, the others still play
//     through them; its link partner on another track keeps its notes
//   - testBypassSavedLoaded: bypass and a committed clip saved, loaded (an older file: neither), undone
//   - testTui: the MIDI effects window in the terminal, its Commit to notes ▾ by keys
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <random>
#include <regex>

#include "audio/midi_effects.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/main_window.h"
#include "ui/midi_commit.h"
#include "ui/midi_effects_dialog.h"
#include "ui/song_scale.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

namespace {
const int SR = 48000;
const std::string TMP = "build/test-tmp/midi_commit";

MidiEffect fx(const std::string& kind, std::map<std::string, double> values) {
    MidiEffect e;
    MidiEffect::make(kind, values, e);
    return e;
}

std::vector<MidiEffect> chain() {
    return {fx("Chord", {{"chord", 2}}), fx("Arpeggiator", {{"rate", 3}, {"gate", 60}, {"octaves", 2}, {"style", 2}}),
            fx("Note Length", {{"beats", 0.2}}), fx("Groove", {{"swing", 62}, {"accent", 20}, {"timing", 5}})};
}

bool sameNotes(const std::shared_ptr<const std::vector<Note>>& a, const std::shared_ptr<const std::vector<Note>>& b) {
    if (!a || !b) return !a && !b;
    if (a->size() != b->size()) return false;
    for (size_t i = 0; i < a->size(); i++) {
        const Note &x = (*a)[i], &y = (*b)[i];
        if (std::fabs(x.start - y.start) > 1e-9 || std::fabs(x.end - y.end) > 1e-9 || x.pitch != y.pitch ||
            x.velocity != y.velocity)
            return false;
    }
    return true;
}

bool sameEvents(const std::shared_ptr<const std::vector<Clip::Event>>& a,
                const std::shared_ptr<const std::vector<Clip::Event>>& b) {
    if (!a || !b || a->size() != b->size()) return false;
    for (size_t i = 0; i < a->size(); i++) {
        const Clip::Event &x = (*a)[i], &y = (*b)[i];
        if (x.on != y.on || x.off != y.off || x.pitch != y.pitch || x.velocity != y.velocity) return false;
    }
    return true;
}

// a clip of `n` random notes in a beat or two, at `at` seconds
ClipPtr randomClip(std::mt19937& rng, const TrackPtr& t, double at, int n) {
    std::vector<Note> notes;
    for (int k = 0; k < n; k++) {
        Note x;
        x.start = (double)(rng() % 16) * 0.0625;
        x.end = x.start + 0.05 + (double)(rng() % 8) * 0.03;
        x.pitch = 40 + (double)(rng() % 40);
        x.velocity = 30 + (double)(rng() % 97);
        if (k % 5 == 1) x.chance = 50;
        if (k % 7 == 2) x.repeats = 3;
        notes.push_back(x);
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    ClipPtr c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->name = "c";
    c->notes = std::make_shared<const std::vector<Note>>(notes);
    c->lengthS = 1.0;
    c->track = t;
    c->start = (int64_t)(at * SR);
    return c;
}

struct Before {
    std::shared_ptr<const std::vector<Note>> played;
    std::shared_ptr<const std::vector<Clip::Event>> events;
};

void testThousandClips() {
    Project p;
    midi_effects::install(&p.timing);
    p.timing.swing = 30;  // (the song swings: written into the notes too)
    TrackPtr t = p.addTrack(Kind::Midi);
    t->midiEffects = chain();
    std::mt19937 rng(7);
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 1000; i++) {
        ClipPtr c = randomClip(rng, t, i * 1.1, 4 + i % 5);
        if (i % 9 == 0) {  // looping, longer than its notes
            c->loop = true;
            c->length = (int64_t)(2.5 * SR);
        }
        clips.push_back(c);
    }
    for (int i = 0; i + 1 < 1000; i += 50) {  // linked pairs (on this track, at other places)
        ClipPtr k = links::linkedCopy(clips[(size_t)i]);
        k->start = clips[(size_t)i + 1]->start + SR / 3;
        clips.push_back(k);
    }
    p.clips = clips;
    song_scale::reprepare(p, SR, {t});
    std::map<Clip*, Before> before;
    size_t notes = 0;
    for (auto& c : clips) {
        before[c.get()] = {c->played, c->events};
        notes += c->played ? c->played->size() : 0;
    }
    CHECK(notes > 5000);  // (the arpeggiator made many)
    auto originalNotes = clips[0]->notes;
    history::History h(p, SR);
    auto t0 = std::chrono::steady_clock::now();
    midi_commit::Result r = midi_commit::commit(p, SR, t, {}, false);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("committed %d clips (%zu notes) in %.2f s\n", r.clips, notes, took);
    CHECK(r.error.empty() && r.clips == (int)clips.size() && r.bypassed == 4 && !r.track);
    bool allSame = true, straight = true;
    for (auto& c : clips) {
        allSame = allSame && sameNotes(c->played, before[c.get()].played) && sameEvents(c->events, before[c.get()].events);
        straight = straight && !c->loop && !c->link && !c->effectsCommitted && sameNotes(c->notes, c->played);
    }
    CHECK(allSame);
    CHECK(straight);
    bool bypassed = true;
    for (auto& e : t->midiEffects) bypassed = bypassed && e.bypass;
    CHECK(bypassed && t->swing == 0 && !midi_effects::swings(*t));
    CHECK(h.check(SR) && h.undoSteps() == 1 && !h.check(SR));
    // undo: the effects on again, the notes as written, the loops and links back
    CHECK(h.undo(SR));
    CHECK(!t->midiEffects[0].bypass && t->swing == -1 && clips[0]->notes == originalNotes && clips[0]->loop &&
          clips[0]->link != nullptr);
    CHECK(sameNotes(clips[5]->played, before[clips[5].get()].played));
    // committing again after undo: the same
    CHECK(midi_commit::commit(p, SR, t, {}, false).clips == (int)clips.size());
    CHECK(sameNotes(clips[17]->played, before[clips[17].get()].played));
}

void testNewTrack() {
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = p.addTrack(Kind::Midi);
    TrackPtr below = p.addTrack(Kind::Midi);
    t->name = "Lead";
    t->midiEffects = chain();
    t->volumeDb = -6;
    std::mt19937 rng(3);
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 40; i++) clips.push_back(randomClip(rng, t, i * 1.5, 6));
    p.clips = clips;
    song_scale::reprepare(p, SR, {t});
    std::vector<Before> before;
    for (auto& c : clips) before.push_back({c->played, c->events});
    history::History h(p, SR);
    midi_commit::Result r = midi_commit::commit(p, SR, t, {}, true);
    CHECK(r.error.empty() && r.clips == 40 && r.track && r.bypassed == 0);
    if (!r.track) return;
    CHECK(p.indexOf(r.track) == p.indexOf(t) + 1 && p.indexOf(below) == p.indexOf(t) + 2);
    CHECK(r.track->name == "Lead (notes)" && r.track->midiEffects.empty() && r.track->volumeDb == -6);
    std::vector<ClipPtr> made = p.clipsOn(r.track);
    CHECK(made.size() == 40);
    bool same = made.size() == 40;
    for (size_t i = 0; same && i < made.size(); i++)
        same = made[i]->start == clips[i]->start && sameNotes(made[i]->played, before[i].played) &&
               sameEvents(made[i]->events, before[i].events);
    CHECK(same);
    bool kept = true;
    for (size_t i = 0; i < clips.size(); i++) kept = kept && clips[i]->disabled && sameNotes(clips[i]->played, before[i].played);
    CHECK(kept);
    bool on = true;
    for (auto& e : t->midiEffects) on = on && !e.bypass;
    CHECK(on);
    CHECK(h.check(SR) && h.undoSteps() == 1);
    CHECK(h.undo(SR) && !p.tracks.contains(r.track) && !clips[0]->disabled && p.clips.size() == 40);
}

void testSomeClips() {
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = p.addTrack(Kind::Midi), other = p.addTrack(Kind::Midi);
    t->midiEffects = chain();
    other->midiEffects = std::vector<MidiEffect>{fx("Pitch", {{"semitones", 5}})};
    std::mt19937 rng(11);
    ClipPtr a = randomClip(rng, t, 0, 5), b = randomClip(rng, t, 2, 5);
    ClipPtr partner = links::linkedCopy(a);
    partner->track = other;
    p.clips = std::vector<ClipPtr>{a, b, partner};
    song_scale::reprepare(p, SR);
    auto aPlayed = a->played, bPlayed = b->played, partnerNotes = partner->notes, partnerPlayed = partner->played;
    // nothing selected: nothing done
    midi_commit::Result none = midi_commit::commit(p, SR, other, {a}, false);
    CHECK(!none.error.empty() && none.clips == 0);
    midi_commit::Result r = midi_commit::commit(p, SR, t, {a}, false);
    CHECK(r.error.empty() && r.clips == 1 && r.bypassed == 0);
    CHECK(a->effectsCommitted && !b->effectsCommitted && sameNotes(a->played, aPlayed) && sameNotes(b->played, bPlayed));
    CHECK(!a->link && !partner->link && partner->notes == partnerNotes && sameNotes(partner->played, partnerPlayed));
    bool on = true;
    for (auto& e : t->midiEffects) on = on && !e.bypass;
    CHECK(on);
    // its notes edited later play as they are (the effects stay off for it); the other clip still gets them
    std::vector<Note> more = *a->notes;
    more.push_back(Note{0.9, 0.95, 90, 100});
    a->notes = std::make_shared<const std::vector<Note>>(more);
    a->prepare(SR);
    CHECK(a->played->size() == more.size());
    // a bypassed effect changes nothing; switched on again it does
    std::vector<MidiEffect> off = t->midiEffects.copy();
    for (auto& e : off) e.bypass = true;
    t->midiEffects = off;
    b->prepare(SR);
    CHECK(b->played->size() == b->performed(SR).size());
    // a frozen track can't be committed
    t->frozen = std::make_shared<Clip>();
    CHECK(!midi_commit::commit(p, SR, t, {}, false).error.empty());
}

void testBypassSavedLoaded() {
    util::makeDirs(TMP);
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = p.addTrack(Kind::Midi);
    std::vector<MidiEffect> effects = chain();
    effects[1].bypass = true;
    t->midiEffects = effects;
    std::mt19937 rng(5);
    ClipPtr a = randomClip(rng, t, 0, 4), b = randomClip(rng, t, 2, 4);
    p.clips = std::vector<ClipPtr>{a, b};
    song_scale::reprepare(p, SR);
    CHECK(midi_commit::commit(p, SR, t, {a}, false).clips == 1);
    std::string path = TMP + "/bypass.exbit", error;
    project_io::MasterSettings master;
    CHECK(project_io::saveProject(path, p, SR, master, error));
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, error));
    TrackPtr lt = q.tracks.empty() ? nullptr : q.tracks[0];
    CHECK(lt && lt->midiEffects.size() == 4);
    if (lt && lt->midiEffects.size() == 4)
        CHECK(!lt->midiEffects[0].bypass && lt->midiEffects[1].bypass && !lt->midiEffects[2].bypass &&
              lt->midiEffects[1].kind == "Arpeggiator");
    std::vector<ClipPtr> loaded = lt ? q.clipsOn(lt) : std::vector<ClipPtr>{};
    CHECK(loaded.size() == 2 && loaded[0]->effectsCommitted && !loaded[1]->effectsCommitted);
    if (loaded.size() == 2) CHECK(sameNotes(loaded[0]->played, a->played) && sameNotes(loaded[1]->played, b->played));
    // written only when set: the file has one "bypass" and one "effects_committed"
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t first = text.find("\"bypass\"");
    CHECK(first != std::string::npos && text.find("\"bypass\"", first + 1) == std::string::npos);
    CHECK(text.find("\"effects_committed\"") != std::string::npos);
    // an older file (neither written): all on, nothing committed
    std::string older = std::regex_replace(text, std::regex(",\\s*\"(bypass|effects_committed)\": true"), "");
    CHECK(older.find("bypass") == std::string::npos && older.find("effects_committed") == std::string::npos);
    std::ofstream(TMP + "/older.exbit") << older;
    Project o;
    CHECK(project_io::loadProject(TMP + "/older.exbit", o, SR, m2, error));
    bool allOn = !o.tracks.empty();
    for (auto& e : o.tracks[0]->midiEffects) allOn = allOn && !e.bypass;
    CHECK(allOn && !o.clipsOn(o.tracks[0])[0]->effectsCommitted);
    // switching one off is an undo step of its own
    history::History h(p, SR);
    std::vector<MidiEffect> now = t->midiEffects.copy();
    now[0].bypass = true;
    t->midiEffects = now;
    CHECK(h.check(SR) && h.labels().back() == "MIDI effects" && h.undo(SR) && !t->midiEffects[0].bypass);
}

void testTui() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    tui::Screen screen(160, 40);
    std::vector<tui::Key> queue;
    size_t at = 0;
    auto step = [&](const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            f.key(queue[at++]);
        }
    };
    app.nestedLoop = [&](const std::function<bool()>& done) { step(done); };
    auto keys = [&](std::vector<tui::Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 8; i++) {
            app.advanceTimers(0.05);
            app.tick();
        }
        f.draw(screen);
    };
    auto K = [](int k) { return tui::Key{k, 0, ""}; };
    int sr = w->engine.sr;
    TrackPtr t = w->timeline->addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->midiEffects = chain();
    std::mt19937 rng(9);
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 3; i++) {
        ClipPtr c = randomClip(rng, t, i * 2.0, 6);
        c->prepare(sr);
        w->project.addClip(c);
        clips.push_back(c);
    }
    auto played = clips[1]->played;
    midi_effects_dialog::MidiEffectsDialog* d = midi_effects_dialog::openMidiEffects(t);
    CHECK(d != nullptr);
    if (!d) return;
    keys({});
    std::ofstream(TMP + "/tui-midi-effects.txt") << screen.lines();
    CHECK(screen.lines().find("Commit to notes") != std::string::npos);
    CHECK(screen.lines().find("[x] On") != std::string::npos);
    // Tab to the button, Enter: its menu; the last item (every clip, onto a new track) is reachable by keys
    int tabs = 0;
    while (d->focusWidget() != d->commitButton && tabs < 40) {
        keys({K(plat::Key_Tab)});
        tabs++;
    }
    CHECK(d->focusWidget() == d->commitButton);
    keys({K(plat::Key_Return)});
    std::ofstream(TMP + "/tui-commit-menu.txt") << screen.lines();
    CHECK(f.menuOpen() && screen.lines().find("Every clip of the track, into their notes") != std::string::npos);
    // nothing selected: the first two are off, the third is the first chosen; Enter
    keys({K(plat::Key_Return)});
    CHECK(!f.menuOpen());
    bool off = true;
    for (auto& e : t->midiEffects) off = off && e.bypass;
    CHECK(off && sameNotes(clips[1]->played, played) && sameNotes(clips[1]->notes, played));
    std::ofstream(TMP + "/tui-committed.txt") << screen.lines();
    CHECK(screen.lines().find("(off)") != std::string::npos);
    // the "On" box by keys: Tab to it, Space: the chosen effect on again
    auto onBox = [&]() -> ui::CheckBox* {
        auto* cb = dynamic_cast<ui::CheckBox*>(d->focusWidget());
        return cb && cb->text() == "On" && !cb->isChecked() ? cb : nullptr;
    };
    tabs = 0;
    while (!onBox() && tabs < 40) {
        keys({K(plat::Key_Tab)});
        tabs++;
    }
    CHECK(onBox() != nullptr);
    keys({tui::Key{plat::Key_Space, 0, " "}});
    int onAgain = 0;
    for (auto& e : t->midiEffects) onAgain += !e.bypass;
    CHECK(onAgain == 1);
    d->close();
    keys({});
    // joining clips with and without committed effects is refused (a mix would play effects twice or not at all);
    // clips that agree join, and the joined clip keeps the flag
    clips[0]->effectsCommitted = true;
    clips[1]->effectsCommitted = false;
    size_t before = w->project.clips.size();
    w->timeline->selection.clear();
    w->timeline->selection.clips = {clips[0], clips[1]};
    w->timeline->joinSelected();
    CHECK(w->project.clips.size() == before && w->project.clips.contains(clips[0]) && w->project.clips.contains(clips[1]));
    CHECK(w->status->message().find("Can't join") != std::string::npos);
    clips[1]->effectsCommitted = true;
    w->timeline->selection.clips = {clips[0], clips[1]};
    w->timeline->joinSelected();
    CHECK(w->project.clips.size() == before - 1 && !w->project.clips.contains(clips[0]));
    bool joinedKeeps = false;
    for (auto& c : *w->project.clips)
        if (c->name.find("(joined)") != std::string::npos) joinedKeeps = c->effectsCommitted;
    CHECK(joinedKeeps);
}
}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testThousandClips();
    testNewTrack();
    testSomeClips();
    testBypassSavedLoaded();
    testTui();
    TEST_MAIN_END();
}
