// The model of Tiny DAW for Linux's features: the tempo map (conversions, grid lines, the fixed grid,
// snapping), clips (fades, loops, muted notes), undo / redo (history), inserting and deleting time
// (song_time), the new edits (cut pieces, reverse, stretch, join, crossfade), projects with every new
// field (round trips, 0.4 projects, backups, recovery), the data folder and MIDI file writing.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <string>
#include <vector>

#include "audio/automation.h"
#include "audio/file_io.h"
#include "audio/mixer.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "model/edits.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"
#include "model/song_time.h"
#include "model/timing.h"
#include "paths.h"

static const int SR = 48000;
static const std::string kDir = "build/test-tmp/model";

static void clearDir(const std::string& dir) {
    if (!util::isDir(dir)) return;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) {
            clearDir(p);
            rmdir(p.c_str());
        } else {
            util::removeFile(p);
        }
    }
}

static std::shared_ptr<AudioBuffer> tone(int64_t frames, double f, double amp) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize(frames * 2);
    for (int64_t i = 0; i < frames; i++) {
        b->samples[2 * i] = (float)(amp * std::sin(i * f));
        b->samples[2 * i + 1] = (float)(amp * std::cos(i * f * 1.01));
    }
    return b;
}

static std::shared_ptr<AudioBuffer> constant(int64_t frames, float v) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.assign(frames * 2, v);
    return b;
}

static std::shared_ptr<AudioBuffer> ramp(int64_t frames) {  // sample i = i (both channels)
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize(frames * 2);
    for (int64_t i = 0; i < frames; i++) b->samples[2 * i] = b->samples[2 * i + 1] = (float)i;
    return b;
}

static bool hasLine(const std::vector<GridLine>& lines, double s, int strength, const std::string& label = "?") {
    for (auto& l : lines)
        if (std::fabs(l.seconds - s) < 1e-9 && l.strength == strength && (label == "?" || l.label == label)) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------

static void testFrac() {
    CHECK(Frac::fromDouble(0.125) == Frac(1, 8));
    CHECK(Frac::fromDouble(1.0 / 12) == Frac(1, 12));
    CHECK(Frac::fromDouble(2.0 / 3) == Frac(2, 3));
    CHECK(Frac::fromDouble(4.0 / 3) == Frac(4, 3));
    CHECK(Frac::fromDouble(4.0) == Frac(4));
    CHECK(Frac::fromDouble(0.3333) == Frac(1, 3));
    CHECK(Frac(7, 2).floor() == 3);
    CHECK(Frac(-7, 2).floor() == -4);
}

static void testTempoMap() {
    Timing t;  // 120 BPM 4/4, from bar 3: 60 BPM 3/4
    t.setChange(3, 60, 3);
    auto& segs = t.segments();
    CHECK(segs.size() == 2);
    CHECK_NEAR(segs[1].seconds, 4.0, 1e-12);
    CHECK_NEAR(segs[1].beat, 8.0, 1e-12);
    CHECK(segs[1].bar == 2);
    CHECK_NEAR(t.beatAt(2.0), 4.0, 1e-12);
    CHECK_NEAR(t.beatAt(5.0), 9.0, 1e-12);
    CHECK_NEAR(t.secondsAt(9.0), 5.0, 1e-12);
    CHECK_NEAR(t.secondsAt(3.0), 1.5, 1e-12);
    CHECK_NEAR(t.barSeconds(2), 4.0, 1e-12);
    CHECK_NEAR(t.barSeconds(3), 7.0, 1e-12);
    CHECK_NEAR(t.barSeconds(1), 2.0, 1e-12);
    CHECK(t.barAt(6.99) == 2);
    CHECK(t.barAt(7.0) == 3);
    CHECK(t.barAt(1.0) == 0);
    CHECK_NEAR(t.bpmAt(3.9), 120, 0);
    CHECK_NEAR(t.bpmAt(4.0), 60, 0);
    CHECK(t.beatsPerBarAt(10) == 3);
    CHECK_NEAR(t.beatSAt(10), 1.0, 1e-12);
    auto beats = t.beatTimes(3, 5.5);
    CHECK(beats.size() == 4);
    if (beats.size() == 4) {
        CHECK_NEAR(beats[0].seconds, 3.0, 1e-12);
        CHECK(!beats[0].barStart);
        CHECK_NEAR(beats[1].seconds, 3.5, 1e-12);
        CHECK_NEAR(beats[2].seconds, 4.0, 1e-12);
        CHECK(beats[2].barStart);
        CHECK_NEAR(beats[3].seconds, 5.0, 1e-12);
        CHECK(!beats[3].barStart);
    }
    // position(): bars and beats counted on through the change
    int bar, beat, part;
    t.position(5.0, bar, beat, part);
    CHECK(bar == 3 && beat == 2 && part == 1);
    t.position(7.25, bar, beat, part);
    CHECK(bar == 4 && beat == 1 && part == 2);
    // two changes at one bar: the later one
    Timing u;
    u.changes = {{3, 90, 4}, {3, 60, 3}};
    CHECK(u.segments().size() == 2);
    CHECK_NEAR(u.segments()[1].bpm, 90, 0);  // (sorted: (3, 60) then (3, 90): the later one is 90)
    // setChange / removeChange / changeAtBar
    t.setChange(5, 100, 4);
    CHECK(t.changes.size() == 2 && t.changeAtBar(5) && t.changeAtBar(5)->bpm == 100);
    t.setChange(5, 110, 4);
    CHECK(t.changes.size() == 2 && t.changeAtBar(5)->bpm == 110);
    t.removeChange(5);
    CHECK(t.changes.size() == 1 && !t.changeAtBar(5));

    // the audio thread's map: published by map() / publish(), and by assignment
    Timing a;
    a.publish();
    CHECK(a.audioMap()->bpm == 120);
    a.bpm = 90;
    CHECK(a.audioMap()->bpm == 120);  // (not yet published)
    a.publish();
    CHECK(a.audioMap()->bpm == 90);
    Timing b;
    b.setChange(2, 140, 7);
    a = b;
    CHECK(a.audioMap()->hasChanges() && a.audioMap()->segments()[1].beatsPerBar == 7);
    // the old map stays valid for whoever holds it
    TempoMapPtr held = a.audioMap();
    a.changes.clear();
    a.publish();
    CHECK(held->hasChanges() && !a.audioMap()->hasChanges());
}

static void testGrid() {
    // without changes: as 0.4
    Timing t;
    CHECK(t.gridStep(100) == Frac(1, 2));  // (a beat is 50 px: a half beat is 25 px, a quarter 12.5)
    CHECK_NEAR(t.snap(0.3, 100), 0.25, 1e-12);
    CHECK_NEAR(t.snap(0.3, 100, true), 0.25, 1e-12);
    CHECK_NEAR(t.snap(0.49, 100, true), 0.25, 1e-12);
    auto lines = t.lines(0, 2.5, 100);
    CHECK(hasLine(lines, 0, timing::BAR, "1"));
    CHECK(hasLine(lines, 0.5, timing::BEAT, "1.2"));
    CHECK(hasLine(lines, 0.25, timing::FINE, ""));
    CHECK(hasLine(lines, 2.0, timing::BAR, "2"));
    for (auto& l : lines) CHECK(l.seconds >= -1e-9 && l.seconds <= 2.5 + 1e-9);

    // with a change at bar 3 (60 BPM 3/4): the grid follows each segment
    t.setChange(3, 60, 3);
    CHECK(t.gridStep(100, 5.0) == Frac(1, 4));
    lines = t.lines(0, 8, 100);
    CHECK(hasLine(lines, 4.0, timing::BAR, "3"));
    CHECK(hasLine(lines, 5.0, timing::BEAT, "3.2"));
    CHECK(hasLine(lines, 4.25, timing::FINE, ""));
    CHECK(hasLine(lines, 7.0, timing::BAR, "4"));
    CHECK(hasLine(lines, 3.75, timing::FINE, ""));
    CHECK(!hasLine(lines, 4.125, timing::FINE));
    // snapping in the second segment, and towards the next segment's first bar
    CHECK_NEAR(t.snap(5.1, 100), 5.0, 1e-12);
    CHECK_NEAR(t.snap(5.2, 100), 5.25, 1e-12);

    // a fixed grid in 3/4: a half note (2 beats) doesn't fit a bar, so it counts from each bar line
    Timing w;
    w.bpm = 60;
    w.beatsPerBar = 3;
    w.grid = 2;
    CHECK(w.fixedStep() && *w.fixedStep() == Frac(2));
    CHECK(w.gridName() == "1/2");
    lines = w.lines(0, 7, 100);
    CHECK(hasLine(lines, 0, timing::BAR));
    CHECK(hasLine(lines, 2, timing::BEAT));
    CHECK(hasLine(lines, 3, timing::BAR));
    CHECK(hasLine(lines, 5, timing::BEAT));
    CHECK(hasLine(lines, 6, timing::BAR));
    CHECK(!hasLine(lines, 4, timing::BEAT));
    CHECK(lines.size() == 5);  // 0, 2, 3, 5, 6
    CHECK_NEAR(w.snap(2.4, 100), 2.0, 1e-12);
    CHECK_NEAR(w.snap(2.6, 100), 3.0, 1e-12);
    CHECK_NEAR(w.snap(4.4, 100), 5.0, 1e-12);
    CHECK_NEAR(w.snap(5.9, 100, true), 5.0, 1e-12);
    // "1 bar" in 3/4 is 3 beats
    w.grid = 4;
    CHECK(w.gridStep(100) == Frac(3));
    CHECK(w.gridName() == "1 bar");
    CHECK_NEAR(w.snap(4.4, 100), 3.0, 1e-12);
    // zoomed far out, a fine fixed grid draws coarser lines (but still snaps to its own)
    w.grid = 0.25;
    CHECK(w.gridStep(1, 0, true) > Frac(1, 4));
    CHECK(w.gridStep(1) == Frac(1, 4));
    CHECK(w.gridName() == "1/16");
    // names and stepping (Ctrl+1 / Ctrl+2)
    Timing g;
    g.stepGrid(true, 100);  // (shown: 1/2 beat) → 1/4 beat
    CHECK_NEAR(g.grid, 0.25, 1e-12);
    g.stepGrid(false);
    CHECK_NEAR(g.grid, 0.5, 1e-12);
    g.grid = 4;
    g.stepGrid(false);
    CHECK_NEAR(g.grid, 4, 1e-12);  // (the coarsest stays)
    g.grid = 0;
    g.stepGrid(false, 5);  // (zoomed out: bars shown → the coarsest is "1 bar")
    CHECK_NEAR(g.grid, 4, 1e-12);
    g.thirds = true;
    g.grid = 1.0 / 3;
    CHECK(g.gridName() == "1/8 triplets");
    g.grid = 0.5;  // (thirds switched since: the nearest of theirs, the first of two as near)
    CHECK(*g.fixedStep() == Frac(1, 3));
    g.grid = 0.6;
    CHECK(*g.fixedStep() == Frac(2, 3));
    g.grid = 0;
    CHECK(g.gridName() == "follows the zoom");
    // snapping on / off, Alt the other way
    Timing s;
    CHECK(s.snaps() && !s.snaps(true));
    s.snapOn = false;
    CHECK(!s.snaps() && s.snaps(true));
    s.beats = false;
    CHECK(!s.snaps(true));
}

// ---------------------------------------------------------------------------------------------

static void testClips() {
    // fades and gain
    auto c = makeAudioClip("a", constant(1000, 1.0f), SR, SR);
    CHECK(!c->hasLevels() && c->level(10) == 1.0f);
    c->fadeIn = 100;
    c->fadeOut = 200;
    CHECK_NEAR(c->level(0), 0.01, 1e-6);
    CHECK_NEAR(c->level(99), 1.0, 1e-6);
    CHECK_NEAR(c->level(500), 1.0, 1e-6);
    CHECK_NEAR(c->level(900), 0.5, 1e-6);
    c->gainDb = -6.0206;
    CHECK_NEAR(c->level(500), 0.5, 1e-4);
    c->length = 150;  // (trimmed: the fades shortened in proportion)
    int64_t fi, fo;
    c->fades(fi, fo);
    CHECK(fi == 50 && fo == 100);
    // MIDI loops repeat their notes; muted notes don't play
    std::vector<Note> notes{{0, 0.25, 60, 100}, {0.5, 0.75, 64, 100}, {1.2, 1.3, 70, 90}};
    auto m = makeMidiClip("m", notes, 1.0, SR);
    m->loop = true;
    m->length = 3 * SR;  // 3 passes of 1 s
    auto rep = m->repeated(SR);
    CHECK(rep.size() == 6);  // (the note at 1.2 s is past one pass: left out)
    CHECK_NEAR(rep[2].start, 1.0, 1e-12);
    CHECK_NEAR(rep[5].start, 2.5, 1e-12);
    m->offset = SR / 2;  // (window 0.5…3.5 s: the notes of 4 passes as far as 3.5 s)
    auto heard = m->heardNotes(SR);
    CHECK(heard.size() == 6 && std::fabs(heard[0].start) < 1e-12 && heard[0].pitch == 64);
    m->prepare(SR);
    CHECK(m->played && m->played->size() == 7);
    CHECK(m->data && m->data->frames() >= (int64_t)(3.5 * SR));
    CHECK(m->events && m->events->size() == 7 && (*m->events)[0].off > (*m->events)[0].on);
    m->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{{0.6, 0.7, 50, 50}});
    CHECK(m->heardNotes(SR).size() == 6);  // (switched-off notes never play)
    // a looping audio clip: its loop written out as far as the window reaches
    auto l = makeAudioClip("loop", ramp(1000), SR, SR);
    l->loop = true;
    l->loopStart = 200.0 / SR;
    l->loopLength = 300.0 / SR;
    l->length = 2000;
    l->prepare(SR);
    CHECK(l->baseFrames == 1000);
    CHECK(l->data->frames() >= 2000);
    CHECK_NEAR(l->data->frame(499)[0], 499, 1e-3);
    int64_t seam = (int64_t)std::nearbyint(config::LOOP_SEAM_S * SR);
    CHECK_NEAR(l->data->frame(500 + seam + 10)[0], 200 + seam + 10, 1e-3);  // (the loop again, after its seam)
    CHECK_NEAR(l->data->frame(500)[0], 500, 1e-3);  // (the seam starts from what followed the loop)
    CHECK_NEAR(l->data->frame(800 + seam + 1)[0], 200 + seam + 1, 1e-3);
    // controls: a track's own volume and pan for automation
    Track t("x");
    CHECK(t.controls && t.controls->isControls() && t.controls->name == "Track volume and pan");
}

static void testProjectTracks() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto r = p.addTrack(Kind::Return);
    auto b = p.addTrack(Kind::Audio);  // (above the returns)
    auto r2 = p.addTrack(Kind::Return);
    CHECK(r->name == "Return A" && r2->name == "Return B");
    CHECK(p.tracks[0] == a && p.tracks[1] == b && p.tracks[2] == r && p.tracks[3] == r2);
    CHECK(p.returns().size() == 2);
    auto g = p.groupTracks({a, r});
    CHECK(!r->group && a->group == g);
    a->soloed = true;
    CHECK(p.audible(*r) && p.audible(*a) && !p.audible(*b));
    a->sends = std::vector<Send>{{r, -6}, {r2, -3}};
    p.removeTrack(r2);
    p.forgetGoneTracks();
    CHECK(a->sends.size() == 1 && a->sends[0].track == r);
    // markers kept by time
    p.setMarkers({{5, "B"}, {1, "A"}});
    CHECK(p.markers[0].name == "A");
    // another sample rate: starts and ends converted, so clips that met still meet
    auto c1 = makeAudioClip("1", constant(SR, 0.5f), SR, SR);
    c1->track = a;
    c1->length = 1001;
    c1->fadeIn = 480;
    auto c2 = copyClip(c1);
    c2->start = c1->end();
    c2->offset = 1001;
    c2->length = 999;
    p.clips = std::vector<ClipPtr>{c1, c2};
    p.changeSampleRate(SR, 44100);
    CHECK(c1->end() == c2->start);
    CHECK(c1->fadeIn == 441);
    p.changeSampleRate(44100, SR);
    CHECK(c1->end() == c2->start);
}

// ---------------------------------------------------------------------------------------------

static void testHistory() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto c = makeAudioClip("a", tone(SR, 0.01, 0.5), SR, SR);
    c->track = a;
    p.addClip(c);
    history::History h(p, SR);
    CHECK(!h.check(SR) && !h.canUndo());
    // a clip moved (in place)
    c->start = 1000;
    CHECK(h.check(SR));
    CHECK(!h.check(SR));
    CHECK(h.canUndo() && !h.canRedo());
    CHECK(h.undo(SR));
    CHECK(c->start == 0 && p.clips[0] == c);  // (the same object, its field put back)
    CHECK(h.canRedo());
    CHECK(h.redo(SR));
    CHECK(c->start == 1000);
    // view fields aren't steps
    a->height = 200;
    a->showAutomation = true;
    p.timing.grid = 0.25;
    p.timing.beats = false;
    p.timing.snapOn = false;
    CHECK(!h.check(SR));
    // a track added, then undone
    auto b = p.addTrack(Kind::Midi);
    CHECK(h.check(SR));
    h.undo(SR);
    CHECK(p.tracks.size() == 1);
    h.redo(SR);
    CHECK(p.tracks.size() == 2 && p.tracks[1] == b);
    // effects and cables
    auto fx = std::make_shared<PluginSlot>();
    fx->name = "Echo";
    fx->kind = "effect";
    a->effects = std::vector<SlotPtr>{fx};
    a->cables = chain({fx});
    CHECK(h.check(SR));
    fx->mix = 0.5;
    CHECK(h.check(SR));
    fx->recent = {"Feedback"};  // (runtime)
    CHECK(!h.check(SR));
    h.undo(SR);
    CHECK(fx->mix == 1.0);
    h.undo(SR);
    CHECK(a->effects.empty() && a->cables.size() == 1 && a->cables[0].first.type == End::In);
    h.redo(SR);
    CHECK(a->effects.size() == 1 && a->effects[0] == fx && a->cables.size() == 2);
    // tempo changes (and the audio thread's map follows)
    p.timing.setChange(3, 90, 3);
    p.timing.loopStart = 1;
    p.timing.loopEnd = 3;
    CHECK(h.check(SR));
    h.undo(SR);
    CHECK(p.timing.changes.empty() && !p.timing.audioMap()->hasChanges() && p.timing.loopEnd == 0);
    CHECK(p.timing.grid == 0.25);  // (view: as it was left)
    h.redo(SR);
    CHECK(p.timing.changes.size() == 1 && p.timing.audioMap()->hasChanges());
    // sends, pan, markers
    auto r = p.addTrack(Kind::Return);
    h.check(SR);
    a->sends = std::vector<Send>{{r, -12}};
    a->pan = -0.5;
    p.setMarkers({{2, "Verse"}});
    CHECK(h.check(SR));
    h.undo(SR);
    CHECK(a->sends.empty() && a->pan == 0 && p.markers.empty());
    // a looping audio clip: its loop audio built again, not kept
    c->loop = true;
    c->loopStart = 0.1;
    c->loopLength = 0.2;
    c->length = 2 * SR;
    c->prepare(SR);
    h.check(SR);
    AudioPtr longer = c->data;
    c->length = 3 * SR;
    c->prepare(SR);
    CHECK(h.check(SR));
    h.undo(SR);
    CHECK(c->length == 2 * SR && c->data && c->data->frames() >= 2 * SR);
    c->loopLength = 0.3;  // (a shape changed: undo builds the audio again)
    c->prepare(SR);
    h.check(SR);
    h.undo(SR);
    CHECK(c->loopLength == 0.2 && c->data->frames() >= c->offset + c->frames());
    // a new step forgets the redo steps
    c->start = 5;
    h.check(SR);
    CHECK(!h.canRedo());
    // at most 100 steps
    for (int i = 0; i < 150; i++) {
        c->start = 10 + i;
        h.check(SR);
    }
    CHECK(h.undoSteps() == history::MAX_STEPS);
    while (h.undo(SR)) {
    }
    CHECK(c->start == 10 + 150 - 1 - history::MAX_STEPS);
    // another sample rate starts afresh
    h.redo(SR);
    CHECK(h.canUndo());
    h.check(44100);
    CHECK(!h.canUndo() && !h.canRedo());
}

// the steps by name (Edit → History…), and jumping to one
static void testHistoryNames() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto c = makeAudioClip("a", tone(SR, 0.01, 0.5), SR, SR);
    c->track = a;
    p.addClip(c);
    auto c2 = makeAudioClip("b", tone(SR, 0.01, 0.5), SR, SR);
    c2->track = a;
    c2->start = SR;
    p.addClip(c2);
    history::History h(p, SR);
    int changes = h.changes();
    CHECK(h.labels().empty() && h.position() == 0);
    p.addTrack(Kind::Midi);
    CHECK(h.check(SR));
    CHECK(h.changes() != changes);
    c->start = 500;
    h.check(SR);
    c->start = 600;  // (both moved: plural)
    c2->start = SR + 100;
    h.check(SR);
    a->volumeDb = -6;
    h.check(SR);
    c->fadeIn = 100;
    h.check(SR);
    p.removeClips({c2});
    h.check(SR);
    a->muted = true;
    a->name = "Drums";
    h.check(SR);  // (one step: the first change found names it)
    h.nameNext("Something special");
    p.timing.bpm = 100;
    h.check(SR);
    std::vector<std::string> want = {"Add track", "Move clip", "Move clips", "Volume", "Fades", "Delete clip", "Mute",
                                     "Something special"};
    CHECK(h.labels() == want);
    CHECK(h.position() == 8);
    // tempo is named by itself (the explicit name was for one step only)
    p.timing.bpm = 110;
    h.check(SR);
    CHECK(h.labels().back() == "Tempo");
    // jumping back keeps the names, the undone ones after the position (in order)
    changes = h.changes();
    CHECK(h.jumpTo(3, SR));
    CHECK(h.changes() != changes);
    CHECK(h.position() == 3 && h.undoSteps() == 3 && h.redoSteps() == 6);
    CHECK(c->start == 600 && c2->start == SR + 100 && a->volumeDb == 0 && c->fadeIn == 0);
    CHECK(p.clips.size() == 2 && p.timing.bpm != 110);
    CHECK(h.labels().size() == 9 && h.labels()[3] == "Volume" && h.labels()[8] == "Tempo");
    CHECK(!h.jumpTo(3, SR));   // (already there)
    CHECK(!h.jumpTo(10, SR));  // (out of range)
    CHECK(!h.jumpTo(-1, SR));
    CHECK(h.jumpTo(9, SR));  // (all the way forward)
    CHECK(h.position() == 9 && !h.canRedo() && p.timing.bpm == 110 && a->name == "Drums" && p.clips.size() == 1);
    CHECK(h.jumpTo(0, SR));  // (as opened)
    CHECK(h.position() == 0 && p.tracks.size() == 1 && c->start == 0 && p.clips.size() == 2);
    // a new step after undoing forgets the redo steps' names
    h.jumpTo(2, SR);
    a->pan = 0.5;
    h.check(SR);
    want = {"Add track", "Move clip", "Pan"};
    CHECK(h.labels() == want && h.position() == 3);
    // a change not yet noted is a step of its own before jumping
    a->soloed = true;
    CHECK(h.jumpTo(1, SR));
    CHECK(h.labels().size() == 4 && h.labels()[3] == "Solo" && !a->soloed && h.position() == 1);
    // returns, sends, markers, clips added, tracks deleted
    h.jumpTo(4, SR);
    auto r = p.addTrack(Kind::Return);
    h.check(SR);
    CHECK(h.labels().back() == "Add track");
    a->sends = std::vector<Send>{{r, -6}};
    h.check(SR);
    CHECK(h.labels().back() == "Sends");
    p.setMarkers({{1, "Verse"}});
    h.check(SR);
    CHECK(h.labels().back() == "Markers");
    auto c3 = copyClip(c);
    c3->start = 3 * SR;
    p.addClip(c3);
    h.check(SR);
    CHECK(h.labels().back() == "Add clip");
    p.removeTrack(r);
    h.check(SR);
    CHECK(h.labels().back() == "Delete track");
    // at most MAX_STEPS names, as many as steps
    for (int i = 0; i < 120; i++) {
        c->start = 1000 + i;
        h.check(SR);
    }
    CHECK((int)h.labels().size() == history::MAX_STEPS && h.position() == history::MAX_STEPS);
    // another sample rate starts afresh
    h.check(44100);
    CHECK(h.labels().empty() && h.position() == 0);
}

// ---------------------------------------------------------------------------------------------

static void testSongTime() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto c = makeAudioClip("a", ramp(10 * SR), SR, SR);  // 0…10 s
    c->track = a;
    p.addClip(c);
    auto later = makeAudioClip("b", ramp(SR), SR, SR);  // 12…13 s
    later->track = a;
    later->start = 12 * SR;
    p.addClip(later);
    auto lane = std::make_shared<Automation>();
    lane->slot = a->controls;
    lane->param = "Volume";
    lane->setPoints({{0, 0}, {10, 1}});
    a->automation = std::vector<AutomationPtr>{lane};
    p.setMarkers({{1, "A"}, {6, "B"}, {11, "C"}});
    p.timing.loopStart = 4;
    p.timing.loopEnd = 8;
    std::string note, error;
    // insert 2 s at 5 s: the clip is split there, what follows moves on
    CHECK(song_time::insertTime(p, 5 * SR, 2 * SR, SR, note, error));
    CHECK(p.clips.size() == 3);
    CHECK(p.clips[0]->end() == 5 * SR && p.clips[1]->start == 7 * SR && p.clips[1]->offset == 5 * SR);
    CHECK(later->start == 14 * SR);
    CHECK_NEAR(automation::valueAt(*lane->points, 5), 0.5, 1e-9);
    CHECK_NEAR(automation::valueAt(*lane->points, 6.5), 0.5, 1e-9);
    CHECK_NEAR(automation::valueAt(*lane->points, 9.5), 0.75, 1e-9);
    CHECK_NEAR(automation::valueAt(*lane->points, 12), 1.0, 1e-9);
    CHECK(p.markers[0].seconds == 1 && p.markers[1].seconds == 8 && p.markers[2].seconds == 13);
    CHECK(p.timing.loopStart == 4 && p.timing.loopEnd == 10);
    CHECK(note.empty());
    // delete 3…9 s: clips cut at the edges, the marker inside goes, the loop range shrinks
    CHECK(song_time::deleteTime(p, 3 * SR, 9 * SR, SR, note, error));
    CHECK(p.clips.size() == 3);
    CHECK(p.clips[0]->end() == 3 * SR);
    CHECK(p.clips[1]->start == 3 * SR && p.clips[1]->offset == 7 * SR);  // (the piece from 9 s: audio from 7 s)
    CHECK(later->start == 8 * SR);
    CHECK(p.markers.size() == 2 && p.markers[1].seconds == 7);
    CHECK(p.timing.loopStart == 3 && p.timing.loopEnd == 4);
    CHECK_NEAR(automation::valueAt(*lane->points, 2.9), 0.29, 1e-6);   // (the curve before the cut…)
    CHECK_NEAR(automation::valueAt(*lane->points, 3.01), 0.7, 1e-2);   // (…jumps to what followed it)
    // a frozen track's rendering isn't cut
    auto f = p.addTrack(Kind::Audio);
    f->frozen = makeAudioClip("f", constant(4 * SR, 0.1f), SR, SR);
    CHECK(!song_time::insertTime(p, 2 * SR, SR, SR, note, error));
    CHECK(error == "Audio 2 is frozen: unfreeze it first (right-click its row)");
    CHECK(song_time::insertTime(p, 5 * SR, SR, SR, note, error));  // (after it)
    p.removeTrack(f);

    // tempo changes move with whole bars only
    Project q;
    q.timing.setChange(5, 90, 3);  // (bar 5 at 8 s)
    CHECK(song_time::insertTime(q, 2 * SR, 2 * SR, SR, note, error));  // (bar 2, one bar)
    CHECK(q.timing.changes.size() == 1 && q.timing.changes[0].bar == 6 && note.empty());
    CHECK(song_time::insertTime(q, 2 * SR, SR, SR, note, error));  // (half a bar)
    CHECK(q.timing.changes[0].bar == 6);
    CHECK(note == "Tempo and time-signature changes stay at their bars (the time wasn't whole bars).");
    // deleting bars 5…6 (8…12 s), where the change at bar 6 is: what follows keeps its tempo
    CHECK(song_time::deleteTime(q, 8 * SR, 12 * SR, SR, note, error));
    CHECK(q.timing.changes.size() == 1 && q.timing.changes[0].bar == 5 && q.timing.changes[0].bpm == 90);
    // deleting the first bars, with a change in them
    Project r;
    r.timing.setChange(2, 60, 3);
    CHECK(song_time::deleteTime(r, 0, 2 * SR, SR, note, error));
    CHECK(r.timing.changes.empty() && r.timing.bpm == 60 && r.timing.beatsPerBar == 3);
    // a deleted loop range goes
    Project s;
    s.timing.loopStart = 1;
    s.timing.loopEnd = 2;
    s.timing.looping = true;
    CHECK(song_time::deleteTime(s, SR / 2, 3 * SR, SR, note, error));
    CHECK(s.timing.loopEnd == 0 && !s.timing.looping);
    // takes move too
    Project t;
    auto tt = t.addTrack(Kind::Audio);
    auto take = makeAudioClip("take", constant(SR, 0.2f), SR, SR);
    take->start = 3 * SR;
    tt->takes = std::vector<ClipPtr>{take};
    CHECK(song_time::insertTime(t, SR, SR, SR, note, error));
    CHECK(tt->takes[0]->start == 4 * SR && take->start == 3 * SR);  // (a moved copy)
}

// ---------------------------------------------------------------------------------------------

static void testEdits() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    // a faded clip cut in two: the pieces go on with the fades as the whole clip had them
    auto c = makeAudioClip("a", constant(1000, 1.0f), SR, SR);
    c->track = a;
    c->fadeIn = 400;
    c->fadeOut = 400;
    p.addClip(c);
    ClipPtr l, r;
    CHECK(edits::splitClip(c, 300, l, r));
    CHECK(l->fadeIn == 400 && l->fadeOut == 0 && l->fadeTail == 700);
    CHECK(r->fadeIn == 400 && r->fadeLead == 300 && r->fadeOut == 400);
    for (int64_t i : {0, 150, 299}) CHECK_NEAR(l->level(i), c->level(i), 1e-6);
    for (int64_t i : {0, 50, 99, 500, 699}) CHECK_NEAR(r->level(i), c->level(i + 300), 1e-6);
    int64_t pi, po;
    double ls, le;
    r->fadeParts(pi, po, ls, le);
    CHECK(pi == 100 && po == 400 && std::fabs(ls - 0.75) < 1e-9 && le == 0.0);
    // reversed: the same part of its audio, the other way round, its fades swapped
    auto w = makeAudioClip("w", ramp(1000), SR, SR);
    w->offset = 100;
    w->length = 300;  // frames 100…399
    w->fadeIn = 10;
    auto rev = edits::reversedClip(w, SR);
    CHECK(rev->reverse && rev->offset == 600 && rev->length == 300 && rev->fadeOut == 10 && rev->fadeIn == 0);
    CHECK_NEAR(rev->data->frame(rev->offset)[0], 399, 1e-3);
    CHECK_NEAR(rev->data->frame(rev->offset + 299)[0], 100, 1e-3);
    auto back = edits::reversedClip(rev, SR);
    CHECK(!back->reverse && back->offset == 100 && back->length == 300);
    // stretched twice as long: its window and fades follow
    auto s = makeAudioClip("s", tone(SR, 0.05, 0.5), SR, SR);
    s->offset = 1000;
    s->length = 10000;
    s->fadeOut = 500;
    auto st = edits::restretched(s, 2.0, 0.0, SR);
    CHECK(st->stretch == 2.0 && st->offset == 2000 && st->length == 20000 && st->fadeOut == 1000);
    CHECK(std::llabs(st->data->frames() - 2 * SR) < SR / 50);
    // joined: audio as it comes out of the clips
    auto j1 = makeAudioClip("j", constant(1000, 0.5f), SR, SR);
    j1->track = a;
    j1->gainDb = -6.0206;
    auto j2 = makeAudioClip("k", constant(1000, 0.5f), SR, SR);
    j2->track = a;
    j2->start = 2000;
    auto joined = edits::consolidated({j2, j1}, SR);
    CHECK(joined->name == "j (joined)" && joined->start == 0 && joined->frames() == 3000);
    CHECK_NEAR(joined->data->frame(500)[0], 0.25, 1e-3);
    CHECK_NEAR(joined->data->frame(1500)[0], 0.0, 1e-9);
    CHECK_NEAR(joined->data->frame(2500)[0], 0.5, 1e-3);
    CHECK(joined->track == a);
    // MIDI joined: notes as they play, muted notes cut to the windows
    auto m1 = makeMidiClip("m", {{0, 0.5, 60, 100}, {0.8, 1.5, 62, 100}}, 1.0, SR);
    m1->length = SR;  // (1 s: the second note is cut at its end)
    m1->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{{0.9, 2.0, 50, 80}});
    auto m2 = makeMidiClip("n", {{0, 0.25, 64, 90}}, 0.5, SR);
    m2->start = 2 * SR;
    auto mj = edits::consolidated({m1, m2}, SR);
    CHECK(mj->kind == Kind::Midi && mj->notes->size() == 3 && mj->length == m2->end());
    CHECK_NEAR((*mj->notes)[1].end, 1.0, 1e-9);
    CHECK_NEAR((*mj->notes)[2].start, 2.0, 1e-9);
    CHECK(mj->mutedNotes->size() == 1 && std::fabs((*mj->mutedNotes)[0].end - 1.0) < 1e-9);
    // crossfaded: two clips with audio beyond their edges overlap around where they meet
    auto x1 = makeAudioClip("x", constant(2000, 1.0f), SR, SR);
    x1->length = 1000;
    auto x2 = makeAudioClip("y", constant(2000, 1.0f), SR, SR);
    x2->start = 1000;
    x2->offset = 1000;
    x2->length = 1000;
    ClipPtr nl, nr;
    CHECK(edits::crossfaded(x1, x2, 200, nl, nr));
    CHECK(nl->end() == 1100 && nr->start == 900 && nr->offset == 900 && nl->fadeOut == 200 && nr->fadeIn == 200);
    // no audio beyond either edge: nothing to overlap
    auto y1 = makeAudioClip("x", constant(1000, 1.0f), SR, SR);
    auto y2 = makeAudioClip("y", constant(1000, 1.0f), SR, SR);
    y2->start = 1000;
    CHECK(!edits::crossfaded(y1, y2, 200, nl, nr));
}

// ---------------------------------------------------------------------------------------------

static void testRoundTrip() {
    clearDir(kDir);
    util::makeDirs(kDir);
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto m = p.addTrack(Kind::Midi);
    auto r = p.addTrack(Kind::Return);
    a->pan = -0.25;
    a->delayMs = 12.5;
    a->sends = std::vector<Send>{{r, -9}};
    a->showTakes = true;
    auto fx = std::make_shared<PluginSlot>();
    fx->path = "/x/Echo.vst3";
    fx->name = "Echo";
    fx->kind = "effect";
    fx->mix = 0.4;
    r->effects = std::vector<SlotPtr>{fx};
    r->cables = chain({fx});
    auto lane = std::make_shared<Automation>();
    lane->slot = a->controls;
    lane->param = "Pan";
    lane->setPoints({{0, 0.5}, {2, 1}});
    a->automation = std::vector<AutomationPtr>{lane};
    m->needsInstrument = false;
    MidiEffect arp;
    MidiEffect::make("Arpeggiator", {{"rate", 4}, {"bogus", 1}}, arp);
    m->midiEffects = std::vector<MidiEffect>{arp};
    auto sampler = std::make_shared<SamplerSettings>();
    sampler->sample = makeAudioClip("Piano C4", tone(4000, 0.02, 0.4), 44100, SR);
    sampler->root = 72;
    sampler->oneShot = true;
    sampler->end = 0.8;
    m->sampler = sampler;
    auto ac = makeAudioClip("vox", tone(SR, 0.01, 0.5), SR, SR);
    ac->track = a;
    ac->gainDb = -3;
    ac->fadeIn = 4800;
    ac->fadeOut = 2400;
    ac->fadeLead = 480;
    ac->stretch = 1.5;
    ac->pitch = 2;
    ac->sourceBpm = 100;
    ac->reverse = true;
    ac->loop = true;
    ac->loopStart = 0.1;
    ac->loopLength = 0.25;
    ac->length = SR;
    ac->prepare(SR);
    auto mc = makeMidiClip("notes", {{0, 0.5, 60, 100}}, 1.0, SR);
    mc->track = m;
    mc->loop = true;
    mc->length = 2 * SR;
    mc->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{{0.5, 0.75, 67, 80}});
    mc->prepare(SR);
    p.clips = std::vector<ClipPtr>{ac, mc};
    auto take = makeAudioClip("take 1", tone(SR / 2, 0.03, 0.3), SR, SR);
    take->start = SR;
    a->takes = std::vector<ClipPtr>{take};
    auto mtake = makeMidiClip("take 2", {{0, 0.1, 50, 70}}, 0.5, SR);
    m->takes = std::vector<ClipPtr>{mtake};
    p.timing.setChange(3, 90, 3);
    p.timing.grid = 0.25;
    p.timing.snapOn = false;
    p.timing.key = 9;
    p.timing.scale = "Minor";
    p.setMarkers({{2.5, "Chorus"}, {0, "Intro"}});

    project_io::MasterSettings master{-2, false};
    std::string path = kDir + "/song.json", error;
    CHECK(project_io::saveProject(path, p, SR, master, error));
    CHECK(!sampler->sample->sourcePath.empty() && !take->sourcePath.empty());
    std::string text;
    util::readFile(path, text);
    const Json data = Json::parse(text);
    const Json& t0 = data["tracks"][size_t(0)];
    CHECK(t0["pan"].toDouble() == -0.25 && t0["delay_ms"].toDouble() == 12.5);
    CHECK(t0["sends"].dump() == "[[2, -9.0]]");
    CHECK(t0["automation"][size_t(0)]["plugin"].toString() == "track");
    CHECK(data["tracks"][size_t(2)]["kind"].toString() == "return");
    CHECK(data["tracks"][size_t(2)]["effects"][size_t(0)]["mix"].toDouble() == 0.4);
    const Json& t1 = data["tracks"][size_t(1)];
    CHECK(t1["midi_effects"][size_t(0)]["kind"].toString() == "Arpeggiator");
    CHECK(t1["midi_effects"][size_t(0)]["values"]["rate"].toDouble() == 4);
    CHECK(t1["midi_effects"][size_t(0)]["values"]["gate"].toDouble() == 80);
    CHECK(!t1["midi_effects"][size_t(0)]["values"].has("bogus"));
    CHECK(t1["sampler"]["root"].toInt() == 72 && t1["sampler"]["one_shot"].toBool());
    CHECK(util::startsWith(t1["sampler"]["file"].toString(), "song_audio/"));
    CHECK(t1["takes"][size_t(0)]["notes"].size() == 1);
    CHECK(data["timing"]["changes"].dump() == "[[3, 90.0, 3]]");
    CHECK(data["timing"]["scale"].toString() == "Minor" && data["timing"]["key"].toInt() == 9);
    CHECK(data["markers"].dump() == "[[0.0, \"Intro\"], [2.5, \"Chorus\"]]");
    CHECK(data["tracks_made"]["return"].toInt() == 1);
    const Json& c0 = data["clips"][size_t(0)];
    CHECK(c0["source_bpm"].toDouble() == 100 && c0["reverse"].toBool() && c0["loop"].toBool());
    CHECK(c0.has("fade_lead") && c0["fade_tail"].toDouble() == 0);
    const Json& c1 = data["clips"][size_t(1)];
    CHECK(c1["muted_notes"].size() == 1 && c1["loop"].toBool());

    Project q;
    project_io::MasterSettings got;
    CHECK(project_io::loadProject(path, q, SR, got, error));
    if (q.tracks.size() != 3 || q.clips.size() != 2) {
        CHECK(false);
        return;
    }
    auto qa = q.tracks[0], qm = q.tracks[1], qr = q.tracks[2];
    CHECK(qr->kind == Kind::Return && qa->pan == -0.25 && qa->delayMs == 12.5);
    CHECK(qa->sends.size() == 1 && qa->sends[0].track == qr && qa->sends[0].db == -9);
    CHECK(qa->automation.size() == 1 && qa->automation[0]->slot == qa->controls);
    CHECK(qr->effects.size() == 1 && qr->effects[0]->mix == 0.4);
    CHECK(qm->midiEffects.size() == 1 && qm->midiEffects[0] == arp);
    CHECK(qm->sampler && qm->sampler->root == 72 && qm->sampler->oneShot && qm->sampler->end == 0.8);
    CHECK(qm->sampler->sample && qm->sampler->sample->source &&
          qm->sampler->sample->source->frames() == sampler->sample->source->frames() && qm->sampler->sample->sourceSr == 44100);
    CHECK(qm->sampler->sample->name == "Piano C4");
    CHECK(qa->takes.size() == 1 && qa->takes[0]->start == SR && qa->takes[0]->source && qa->showTakes);
    CHECK(qm->takes.size() == 1 && qm->takes[0]->notes->size() == 1);
    auto qc = q.clips[0], qmc = q.clips[1];
    CHECK(qc->gainDb == -3 && qc->fadeIn == 4800 && qc->fadeOut == 2400 && qc->fadeLead == 480);
    CHECK(qc->stretch == 1.5 && qc->pitch == 2 && qc->sourceBpm == 100 && qc->reverse && qc->loop);
    CHECK(qc->loopStart == 0.1 && qc->loopLength == 0.25 && qc->length == SR);
    CHECK(qc->data && qc->data->frames() >= qc->offset + qc->frames());
    CHECK(qmc->loop && qmc->mutedNotes && qmc->mutedNotes->size() == 1 && qmc->length == 2 * SR);
    CHECK(qmc->played && qmc->played->size() == 2);
    CHECK(q.timing.changes.size() == 1 && q.timing.changes[0] == (TempoChange{3, 90, 3}));
    CHECK(q.timing.audioMap()->hasChanges());
    CHECK(q.timing.grid == 0.25 && !q.timing.snapOn && q.timing.key == 9 && q.timing.scale == "Minor");
    CHECK(q.markers.size() == 2 && q.markers[1].name == "Chorus");
    CHECK(q.tracksMade["return"] == 1);
    // the same description again
    const Json again = project_io::describe(q, SR, got, nullptr, false), before = project_io::describe(p, SR, master, nullptr, false);
    CHECK(again["timing"] == before["timing"] && again["markers"] == before["markers"]);
    CHECK(again["tracks"][size_t(0)]["sends"] == before["tracks"][size_t(0)]["sends"]);

    // values out of range are left out
    Json bad = Json::object({{"changes", Json::parse("[[1, 90, 3], [4, 5000, 3], [5, 100, 3], [6, 100, 40]]")},
                             {"key", Json(14)},
                             {"scale", Json("Major")},
                             {"grid", Json(20)}});
    Timing bt = project_io::readTiming(bad);
    // (…but a time signature's numerator out of range is kept to 1…MAX_BEATS_PER_BAR)
    CHECK(bt.changes.size() == 2 && bt.changes[0].bar == 5 && bt.key == 0 && bt.scale.empty() && bt.grid == 0);
    CHECK(bt.changes[1].bar == 6 && bt.changes[1].beatsPerBar == config::MAX_BEATS_PER_BAR);
    Timing unknown = project_io::readTiming(Json::object({{"key", Json(3)}, {"scale", Json("Lydian")}}));
    CHECK(unknown.scale.empty() && unknown.key == 0);
}

static void testOldProject() {
    // a project written by Tiny DAW 0.4: none of the new keys
    clearDir(kDir);
    util::makeDirs(kDir + "/old_audio");
    auto audio = tone(SR, 0.02, 0.5);
    std::string error;
    CHECK(writeWav(kDir + "/old_audio/a.wav", audio->samples.data(), SR, 2, SR, SampleFormat::Float32, error));
    std::string json = R"({"format": "tinydaw-project", "version": 1, "master": {"volume_db": -3, "mute": false},
      "tracks_made": {"audio": 1, "midi": 1, "group": 0},
      "timing": {"bpm": 100.0, "beats_per_bar": 3, "beats": true, "thirds": false, "loop_start": 0.0, "loop_end": 0.0, "looping": false},
      "groups": [],
      "tracks": [{"name": "Audio 1", "kind": "audio", "muted": false, "soloed": false, "output": 0, "volume_db": 0.0,
                  "height": 72, "color": null, "tone": null, "needs_instrument": false, "show_automation": false,
                  "uses_rack": false, "group": null, "frozen": null, "instrument": null,
                  "effects": [{"path": "/p/Fx.vst3", "name": "Fx", "kind": "effect", "plugin_name": null, "bypass": false,
                               "volume_db": 0.0, "state": null}],
                  "cables": [["in", 0], [0, "out"]], "rack": [], "rack_cables": [],
                  "automation": [{"plugin": 0, "param": "Mix", "height": 64, "points": [[0.0, 0.5]]}]},
                 {"name": "MIDI 1", "kind": "midi", "muted": false, "soloed": false, "output": 0, "volume_db": 0.0,
                  "height": 72, "color": null, "tone": null, "needs_instrument": false, "show_automation": false,
                  "uses_rack": false, "group": null, "frozen": null, "instrument": null, "effects": [], "cables": [["in", "out"]],
                  "rack": [], "rack_cables": [], "automation": []}],
      "clips": [{"kind": "audio", "name": "a", "track": 0, "start": 0.5, "offset": 0.0, "length": 0.5, "color": null,
                 "tone": null, "disabled": false, "file": "old_audio/a.wav"},
                {"kind": "midi", "name": "m", "track": 1, "start": 0.0, "offset": 0.0, "length": null, "color": null,
                 "tone": null, "disabled": false, "notes": [[0.0, 0.5, 60.0, 100.0]], "midi_length": 1.0}],
      "master_effects": [], "master_cables": [["in", "out"]]})";
    util::writeFile(kDir + "/old.json", json);
    Project p;
    project_io::MasterSettings master;
    CHECK(project_io::loadProject(kDir + "/old.json", p, SR, master, error));
    CHECK(p.tracks.size() == 2 && p.clips.size() == 2 && master.volumeDb == -3);
    CHECK(p.timing.bpm == 100 && p.timing.beatsPerBar == 3 && p.timing.changes.empty() && p.timing.snapOn);
    CHECK(p.markers.empty() && p.tracksMade["return"] == 0);
    auto c = p.clips[0];
    CHECK(c->start == SR / 2 && c->length == SR / 2 && c->stretch == 1.0 && c->gainDb == 0 && !c->loop);
    CHECK(p.tracks[0]->effects[0]->mix == 1.0 && p.tracks[0]->pan == 0 && p.tracks[0]->sends.empty());
    CHECK(p.clips[1]->mutedNotes && p.clips[1]->mutedNotes->empty() && !p.clips[1]->loop);
    CHECK(p.tracks[0]->automation.size() == 1);
}

static void testBackups() {
    clearDir(kDir);
    util::makeDirs(kDir);
    std::string path = kDir + "/song.json", backup, error;
    // nothing there yet, or not a project: no backup
    CHECK(project_io::keepBackup(path, backup, error) && backup.empty());
    util::writeFile(path, "{\"something\": 1}");
    CHECK(project_io::keepBackup(path, backup, error) && backup.empty());
    Project p;
    p.addTrack(Kind::Audio);
    project_io::MasterSettings master;
    for (int i = 0; i < 13; i++) {
        p.timing.bpm = 100 + i;
        CHECK(project_io::keepBackup(path, backup, error));
        CHECK(project_io::saveProject(path, p, SR, master, error));
        // each saved version a second older than the next (the backups are named and sorted by their time)
        struct timespec times[2];
        times[0].tv_sec = times[1].tv_sec = 1700000000 + i * 10;
        times[0].tv_nsec = times[1].tv_nsec = 0;
        utimensat(AT_FDCWD, path.c_str(), times, 0);
    }
    auto found = project_io::backups(path);
    CHECK(found.size() == project_io::BACKUPS_KEPT);
    // the newest is the version saved before the last one, pointing back to the project's folder
    std::string text;
    util::readFile(found[0].path, text);
    const Json data = Json::parse(text);
    CHECK(data["timing"]["bpm"].toDouble() == 111 && data["base"].toString() == "..");
    CHECK(found[0].mtime > found[1].mtime);
    // the same file again: no new backup
    CHECK(project_io::keepBackup(path, backup, error));
    std::string again;
    CHECK(project_io::keepBackup(path, again, error) && again == backup);
    // a backup opens as it was (its audio from the project's audio folder)
    Project q;
    auto a = q.addTrack(Kind::Audio);
    auto c = makeAudioClip("x", tone(1000, 0.1, 0.4), SR, SR);
    c->track = a;
    q.addClip(c);
    CHECK(project_io::saveProject(path, q, SR, master, error));
    CHECK(project_io::keepBackup(path, backup, error) && !backup.empty());
    Project r;
    CHECK(project_io::loadProject(backup, r, SR, master, error));
    CHECK(r.clips.size() == 1 && r.clips[0]->source->frames() == 1000);
}

static void testRecovery() {
    clearDir(kDir);
    util::makeDirs(kDir);
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto c = makeAudioClip("rec", tone(1000, 0.1, 0.4), SR, SR);  // (not saved anywhere)
    c->track = a;
    auto d = copyClip(c);  // (the same audio)
    d->start = 2000;
    p.clips = std::vector<ClipPtr>{c, d};
    std::map<std::string, std::string> written;
    std::vector<project_io::PendingAudio> toWrite;
    project_io::MasterSettings master;
    std::string folder = util::absolutePath(kDir + "/recovery"), error;
    Json data = project_io::describeRecovery(p, SR, master, folder, written, toWrite);
    CHECK(toWrite.size() == 1 && written.size() == 1);
    const Json& clips = data["clips"];  // (const: a non-const Json's [size_t] is ambiguous)
    CHECK(clips[size_t(0)]["file"].toString() == folder + "/rec.wav");
    CHECK(project_io::writeRecovery(folder + "/project.json", data, toWrite, error));
    // a second look: nothing more to write
    std::vector<project_io::PendingAudio> more;
    project_io::describeRecovery(p, SR, master, folder, written, more);
    CHECK(more.empty());
    Project q;
    CHECK(project_io::loadProject(folder + "/project.json", q, SR, master, error));
    CHECK(q.clips.size() == 2 && q.clips[1]->start == 2000);
    CHECK(project_io::audioHolders(p).size() == 2);
}

// ---------------------------------------------------------------------------------------------

static void testMidiFiles() {
    clearDir(kDir);
    util::makeDirs(kDir);
    Timing t;  // 120 BPM, from bar 3: 60 BPM 3/4
    t.setChange(3, 60, 3);
    std::vector<file_io::MidiPart> parts{{"Piano", {{0, 0.5, 60, 100}, {4.0, 5.0, 64, 0}, {1.0, 1.0, 130, 200}}},
                                         {"Bass", {{2.0, 6.0, 40, 90}}}};
    std::string path = kDir + "/out.mid", error;
    CHECK(file_io::saveMidi(path, parts, t, 0, error));
    std::vector<Note> notes;
    double length = 0;
    CHECK(file_io::loadMidi(path, notes, length, error));
    CHECK(notes.size() == 4);
    if (notes.size() == 4) {
        // read back in seconds through the file's own tempo map: where they were
        CHECK_NEAR(notes[0].start, 0, 1e-6);
        CHECK_NEAR(notes[0].end, 0.5, 1e-3);
        CHECK(notes[1].pitch == 127 && notes[1].velocity == 127);  // (kept in range; at least a tick long)
        CHECK_NEAR(notes[2].start, 2.0, 1e-3);
        CHECK_NEAR(notes[2].end, 6.0, 1e-3);
        CHECK_NEAR(notes[3].start, 4.0, 1e-3);
        CHECK_NEAR(notes[3].end, 5.0, 1e-3);
        CHECK(notes[3].velocity == 1);
    }
    // in beats
    std::vector<Note> beats;
    double lengthBeats = 0;
    CHECK(file_io::loadMidiBeats(path, beats, lengthBeats, error));
    CHECK(beats.size() == 4);
    if (beats.size() == 4) {
        CHECK_NEAR(beats[2].start, 4.0, 1e-9);  // (2 s at 120 BPM)
        CHECK_NEAR(beats[2].end, 10.0, 1e-9);   // (8 beats to 4 s, then 2 at 60 BPM)
    }
    CHECK_NEAR(lengthBeats, 10.0, 1e-9);
    // from a point in the song: the tempo then first, times from there
    CHECK(file_io::saveMidi(path, {{"Late", {{0, 1, 60, 100}}}}, t, 5.0, error));
    CHECK(file_io::loadMidi(path, notes, length, error));
    CHECK(notes.size() == 1 && std::fabs(notes[0].end - 1.0) < 1e-3);  // (at 60 BPM)
    std::string bytes;
    util::readFile(path, bytes);
    CHECK(bytes.substr(0, 4) == "MThd" && bytes[9] == 1 && bytes[11] == 2);  // (type 1, two tracks)
}

static void testPaths() {
    setenv("XDG_DATA_HOME", "/tmp/xdg-test", 1);
    CHECK(paths::dataDir() == "/tmp/xdg-test/TinyDAW");
    CHECK(paths::dataDir("templates") == "/tmp/xdg-test/TinyDAW/templates");
    unsetenv("XDG_DATA_HOME");
    CHECK(util::endsWith(paths::dataDir(), "/.local/share/TinyDAW"));
    paths::setDataDir(util::absolutePath(kDir + "/data"));
    CHECK(paths::makeDataDir("skins") == util::absolutePath(kDir + "/data/skins") &&
          util::isDir(kDir + "/data/skins"));
    paths::setDataDir("");
}

static void testSynth() {
    // a note played again is the same sound, rendered once
    auto one = synth::render({{0, 0.2, 60, 100}}, SR, 0.5);
    auto two = synth::render({{0, 0.2, 60, 100}, {0.3, 0.5, 60, 100}}, SR, 0.5);
    int64_t at = (int64_t)(0.3 * SR);
    for (int64_t i : {(int64_t)10, (int64_t)500, (int64_t)5000})
        CHECK_NEAR(two->frame(at + i)[0] - one->frame(at + i)[0], one->frame(i)[0], 1e-5);
    auto direct = synth::renderNote(69, 127, 0.1, SR);
    CHECK_NEAR(direct[100], synth::NOTE_GAIN * std::min(1.0, 100.0 / SR / synth::ATTACK_S) *
                                (std::sin(2 * M_PI * 440 * 100.0 / SR) + 0.35 * std::sin(2 * M_PI * 880 * 100.0 / SR) +
                                 0.15 * std::sin(2 * M_PI * 1320 * 100.0 / SR)),
               1e-5);
}

// fade shapes (model.h FADE SHAPES): each curve's values, equal-power crossfades keeping their loudness,
// the mixer playing them as Clip::level says, reversing, and the names files and menus use
static void testFadeShapes() {
    const double PI = 3.14159265358979323846;
    // the curves at a few points: a fade-in (x grows with time) and a fade-out (x counts back from its end)
    for (double x : {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0}) {
        CHECK(fadeCurve(FadeShape::Linear, x, false) == x && fadeCurve(FadeShape::Linear, x, true) == x);
        double s = x * x * (3 - 2 * x);
        CHECK_NEAR(fadeCurve(FadeShape::Smooth, x, false), s, 1e-12);
        CHECK_NEAR(fadeCurve(FadeShape::Smooth, x, true), s, 1e-12);
        CHECK_NEAR(fadeCurve(FadeShape::Fast, x, false), 1 - std::pow(1 - x, 3), 1e-12);  // loud soon
        CHECK_NEAR(fadeCurve(FadeShape::Slow, x, false), std::pow(x, 3), 1e-12);          // loud late
        CHECK_NEAR(fadeCurve(FadeShape::Fast, x, true), std::pow(x, 3), 1e-12);           // a fade-out: quiet soon
        CHECK_NEAR(fadeCurve(FadeShape::Slow, x, true), 1 - std::pow(1 - x, 3), 1e-12);
        CHECK_NEAR(fadeCurve(FadeShape::EqualPower, x, false), std::sin(x * PI / 2), 1e-7);
        CHECK_NEAR(fadeCurve(FadeShape::EqualPower, x, true), std::sin(x * PI / 2), 1e-7);
    }
    // every shape goes from silence to full, never outside 0…1, never back down
    for (FadeShape shape : FADE_SHAPES)
        for (bool out : {false, true}) {
            CHECK_NEAR(fadeCurve(shape, 0.0, out), 0.0, 1e-9);
            CHECK_NEAR(fadeCurve(shape, 1.0, out), 1.0, 1e-7);
            double last = -1;
            bool rising = true, inside = true;
            for (int k = 0; k <= 1000; k++) {
                double v = fadeCurve(shape, k / 1000.0, out);
                if (v < last) rising = false;
                if (v < -1e-9 || v > 1 + 1e-7) inside = false;
                last = v;
            }
            CHECK(rising && inside);
        }
    CHECK(fadeCurve(FadeShape::Fast, 0.5, false) > 0.8 && fadeCurve(FadeShape::Slow, 0.5, false) < 0.2);
    // names for files and menus; unknown names are straight lines
    for (FadeShape shape : FADE_SHAPES) CHECK(fadeShapeFromName(fadeShapeName(shape)) == shape);
    CHECK(fadeShapeFromName("") == FadeShape::Linear && fadeShapeFromName("wobbly") == FadeShape::Linear);
    CHECK(std::string(fadeShapeName(FadeShape::EqualPower)) == "equal_power");
    CHECK(std::string(fadeShapeLabel(FadeShape::Smooth)) == "Smooth (S-curve)");

    // a clip's level follows its shapes; Linear is exactly the straight line it always was
    auto c = makeAudioClip("c", constant(1000, 1.0f), SR, SR);
    c->fadeIn = 400;
    c->fadeOut = 200;
    for (int64_t i : {0, 99, 199, 399}) CHECK(c->level(i) == (float)((i + 1) / 400.0));
    c->fadeInShape = FadeShape::Smooth;
    c->fadeOutShape = FadeShape::Fast;
    for (int64_t i : {0, 99, 199, 399, 500})
        CHECK_NEAR(c->level(i), fadeCurve(FadeShape::Smooth, std::min(1.0, (i + 1) / 400.0), false), 1e-6);
    for (int64_t i : {800, 850, 900, 999})
        CHECK_NEAR(c->level(i), fadeCurve(FadeShape::Fast, (1000 - i) / 200.0, true), 1e-6);
    // the mixer plays the clip at those levels (before the short fade at its very edges)
    {
        std::vector<float> l(1000, 0.0f), r(1000, 0.0f);
        float* buf[2] = {l.data(), r.data()};
        mixer::addClip(buf, 2, 1000, *c, 0);
        for (int64_t i = config::EDGE_FADE_FRAMES; i < 1000 - config::EDGE_FADE_FRAMES; i += 7) {
            CHECK_NEAR(l[(size_t)i], c->level(i), 2e-6);
            CHECK(l[(size_t)i] == r[(size_t)i]);
        }
    }
    // reversed: the fade-in becomes the fade-out, heard backwards (a fast rise: a late, quick fall)
    auto rev = edits::reversedClip(c, SR);
    CHECK(rev->fadeOutShape == FadeShape::Smooth && rev->fadeInShape == FadeShape::Slow);
    for (int64_t i : {0, 50, 150, 199, 600, 900, 999}) CHECK_NEAR(rev->level(i), c->level(999 - i), 2e-3);
    // a piece cut from a shaped fade goes on along the same curve
    ClipPtr left, right;
    CHECK(edits::splitClip(c, 100, left, right));
    for (int64_t i : {0, 100, 299}) CHECK_NEAR(right->level(i), c->level(i + 100), 1e-6);

    // a crossfade of two clips that meet: equal power, as loud all through (within 0.1 dB)
    auto a = makeAudioClip("a", constant(SR, 0.5f), SR, SR);
    a->length = SR / 2;  // (audio beyond its end to overlap with)
    auto b = makeAudioClip("b", constant(SR, 0.5f), SR, SR);
    b->offset = SR / 2;  // (audio before its start)
    b->length = SR / 2;
    b->start = SR / 2;
    ClipPtr na, nb;
    CHECK(edits::crossfaded(a, b, SR / 10, na, nb));
    CHECK(na && nb && na->fadeOutShape == FadeShape::EqualPower && nb->fadeInShape == FadeShape::EqualPower);
    if (na && nb) {
        CHECK(na->end() - nb->start == SR / 10 && na->fadeOut == SR / 10 && nb->fadeIn == SR / 10);
        double worst = 0;
        for (int64_t t = nb->start; t < na->end(); t++) {
            double ga = na->level(t - na->start), gb = nb->level(t - nb->start);
            worst = std::max(worst, std::fabs(10 * std::log10(ga * ga + gb * gb)));
        }
        CHECK(worst < 0.1);
        // (a straight crossfade dips by up to 3 dB in the middle: what equal power is for)
        na->fadeOutShape = nb->fadeInShape = FadeShape::Linear;
        int64_t mid = (nb->start + na->end()) / 2;
        double ga = na->level(mid - na->start), gb = nb->level(mid - nb->start);
        CHECK(10 * std::log10(ga * ga + gb * gb) < -2.9);
    }
}

int main() {
    testFrac();
    testTempoMap();
    testGrid();
    testClips();
    testProjectTracks();
    testHistory();
    testHistoryNames();
    testSongTime();
    testEdits();
    testFadeShapes();
    testRoundTrip();
    testOldProject();
    testBackups();
    testRecovery();
    testMidiFiles();
    testPaths();
    testSynth();
    TEST_MAIN_END();
}
