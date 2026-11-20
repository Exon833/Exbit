// Editing many clips at once (model/clip_edits.h, ui/timeline_slip.cpp), headless, at its worst:
//   - slip: an audio clip's window moves inside its audio and stops at either end; a gain envelope stays on
//     the same sound; a looping clip's phase wraps; a MIDI clip's notes move (those moved out kept, unheard),
//     its linked clips' with them, a looping one's wrapped
//   - Ctrl+Alt+drag on a clip: on the grid's steps (Shift as well: freely), "Slip" in the status bar; Alt+Shift
//     on a name strip still makes linked copies
//   - an edge or a fade handle of a selected clip drags every selected clip's
//   - Crop to the selection across 64 tracks; Fades at every boundary… and Slip contents… from the menu
//   - 1,000 selected clips slipped, trimmed, faded and cropped while the engine plays them on another thread
//   - the terminal UI's Slip contents… dialog (←/→ by the grid) and the fade dialog for several clips
//   - every edit one undo step
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "model/clip_edits.h"
#include "model/clip_envelope.h"
#include "model/history.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "ui/clip_edit_dialogs.h"
#include "ui/clip_painter.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;
int64_t sec(double s) { return (int64_t)std::llround(s * SR); }

AudioPtr tone(double seconds) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = sec(seconds);
    a->samples.resize((size_t)n * 2);
    for (int64_t i = 0; i < n; ++i) a->samples[(size_t)i * 2] = a->samples[(size_t)i * 2 + 1] = 0.3f * (float)std::sin(i * 0.01);
    return a;
}

ClipPtr audioClip(Project& p, const TrackPtr& t, double start, double offset, double length, double source = 4) {
    auto c = makeAudioClip("Tone", tone(source), SR, SR);
    c->track = t;
    c->start = sec(start);
    c->offset = sec(offset);
    c->length = sec(length);
    p.addClip(c);
    return c;
}

Note note(double s, double e, double pitch) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    return n;
}

ClipPtr midiClip(Project& p, const TrackPtr& t, double start, double seconds) {
    auto c = makeMidiClip("Riff", {note(0, 0.25, 60), note(0.5, 0.75, 64), note(1.0, 1.5, 67)}, seconds, SR);
    c->track = t;
    c->start = sec(start);
    c->length = sec(seconds);
    p.addClip(c);
    return c;
}

ui::MouseEvent mouse(double x, double y, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point((int)x, (int)y);
    e.globalPos = e.pos;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
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
    std::string status;
    Rig() {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(1200, 600);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        tl->status = [this](const std::string& t, int) { status = t; };
    }
    TrackPtr track(Kind k) {
        TrackPtr t = project.addTrack(k);
        t->needsInstrument = false;
        tl->relayout();
        return t;
    }
    RectF rect(const ClipPtr& c) { return *tl->clipRect(*c, tl->trackTops()); }
};

// no two clips on a track overlap
bool noOverlaps(const Project& p) {
    for (auto& t : p.tracks) {
        auto cs = p.clipsOn(t);
        std::sort(cs.begin(), cs.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
        for (size_t i = 1; i < cs.size(); i++)
            if (cs[i]->start < cs[i - 1]->end()) return false;
    }
    return true;
}

void testSlipAudio() {
    Rig r;
    TrackPtr t = r.track(Kind::Audio);
    ClipPtr c = audioClip(r.project, t, 5, 1, 1);  // 4 s of audio, frames 1…2 s of it heard from 5 s
    history::History h(r.project, SR);
    r.tl->selection.selectOnly(c);
    r.tl->slipSelected(sec(0.5));  // later: the window starts earlier in the audio
    CHECK(c->offset == sec(0.5) && c->start == sec(5) && c->length == sec(1));
    CHECK(r.status.find("Slip") != std::string::npos);
    CHECK(h.check(SR));
    r.tl->slipSelected(sec(10));  // past the audio's start: stops there
    CHECK(c->offset == 0 && c->start == sec(5) && c->length == sec(1));
    CHECK(h.check(SR));
    r.tl->slipSelected(-sec(100));  // past its end: the window ends at the audio's end
    CHECK(c->offset == sec(3) && c->length == sec(1));
    CHECK(h.check(SR));
    // undo, step by step
    CHECK(h.undo(SR) && c->offset == 0);
    CHECK(h.undo(SR) && c->offset == sec(0.5));
    CHECK(h.undo(SR) && c->offset == sec(1));
    // a clip playing to the end of its audio (length -1): its window can only go later
    c->offset = sec(2);
    c->length = -1;
    r.tl->slipSelected(-sec(1));
    CHECK(c->offset == sec(2) && c->frames() == sec(2));
    r.tl->slipSelected(sec(1));
    CHECK(c->offset == sec(1) && c->length == sec(2));
    // a frozen track's clip: left alone
    t->frozen = std::make_shared<Clip>();
    r.tl->slipSelected(sec(1));
    CHECK(c->offset == sec(1));
    t->frozen = nullptr;
    // slipRoom says so too
    auto from = clip_edits::slipStart(r.project, {c});
    int64_t lo, hi;
    CHECK(from.size() == 1);
    if (!from.empty()) {
        clip_edits::slipRoom(from[0], lo, hi);
        CHECK(hi == sec(1) && lo == -sec(1));
    }
}

void testSlipEnvelope() {
    Rig r;
    TrackPtr t = r.track(Kind::Audio);
    ClipPtr c = audioClip(r.project, t, 0, 1, 2);
    c->gainEnvelope = clip_envelope::make({{(double)sec(1.2), 0.2}, {(double)sec(2.0), 0.9}, {(double)sec(2.6), 0.4}});
    auto envelope = c->gainEnvelope;
    std::vector<float> before;
    for (int64_t pos = 0; pos < sec(2); pos += sec(0.05)) before.push_back(c->level(pos));
    r.tl->selection.selectOnly(c);
    r.tl->slipSelected(sec(0.25));  // the sound 0.25 s later in the clip: its envelope with it
    CHECK(c->gainEnvelope == envelope);
    bool same = true;
    for (size_t i = 0; i < before.size(); i++) {
        int64_t pos = (int64_t)i * sec(0.05) + sec(0.25);
        if (pos < sec(2) && std::fabs(c->level(pos) - before[i]) > 1e-5f) same = false;
    }
    CHECK(same);
}

void testSlipLooping() {
    Rig r;
    TrackPtr t = r.track(Kind::Audio);
    ClipPtr c = audioClip(r.project, t, 0, 0.5, 0.5, 2);
    r.tl->setLoopNotes({c}, true);  // its window (0.5…1 s of the audio) is its loop
    r.tl->trimTo(c, ClipSide::Right, sec(3));  // made 3 s long: six passes
    CHECK(c->loopingAudio() && c->loopFrom == sec(0.5) && c->loopFor == sec(0.5) && c->frames() == sec(3));
    history::History h(r.project, SR);
    r.tl->selection.selectOnly(c);
    r.tl->slipSelected(sec(0.1));  // the loop's phase: wrapped into the loop
    CHECK(c->offset == sec(0.9) && c->start == 0 && c->length == sec(3));
    CHECK(c->data && c->data->frames() >= c->offset + c->length);
    r.tl->slipSelected(sec(5));  // ten passes: no end to reach
    CHECK(c->offset == sec(0.9));
    r.tl->slipSelected(-sec(0.2));
    CHECK(c->offset == sec(0.6));
    CHECK(h.check(SR) && h.undo(SR));
    CHECK(c->offset == sec(0.5) && c->data && c->data->frames() >= c->offset + c->length);
}

void testSlipMidi() {
    Rig r;
    TrackPtr t = r.track(Kind::Midi);
    ClipPtr c = midiClip(r.project, t, 0, 2);
    ClipPtr copy = links::linkedCopy(c);
    copy->start = sec(4);
    r.project.addClip(copy);
    AudioPtr copyData = copy->data;
    history::History h(r.project, SR);
    r.tl->selection.selectOnly(c);
    r.tl->slipSelected(sec(0.25));
    CHECK(c->notes && c->notes->size() == 3 && std::fabs((*c->notes)[0].start - 0.25) < 1e-9);
    CHECK(copy->notes == c->notes && copy->data != copyData && c->start == 0 && copy->start == sec(4));
    CHECK(h.check(SR));
    r.tl->slipSelected(-sec(1));  // two notes before the clip's start: kept, not heard
    CHECK(c->notes->size() == 3 && std::fabs((*c->notes)[0].start + 0.75) < 1e-9);
    CHECK(c->heardNotes(SR).size() == 1);
    r.tl->slipSelected(sec(1));  // back: heard again
    CHECK(c->heardNotes(SR).size() == 3 && std::fabs((*c->notes)[0].start - 0.25) < 1e-9);
    CHECK(h.check(SR));
    // both linked clips selected: slipped once, not twice
    r.tl->selection.clips = {c, copy};
    r.tl->slipSelected(sec(0.25));
    CHECK(std::fabs((*c->notes)[0].start - 0.5) < 1e-9 && copy->notes == c->notes);
    CHECK(h.check(SR));
    // undo: both clips' notes back together
    CHECK(h.undo(SR) && h.undo(SR) && h.undo(SR));
    CHECK(std::fabs((*c->notes)[0].start) < 1e-9 && copy->notes == c->notes);
    // a looping clip: its notes wrap around its pass
    r.tl->setLoopNotes({c}, true);
    r.tl->selection.selectOnly(c);
    r.tl->slipSelected(sec(1.9));  // the note at 0.5 s goes to 2.4 s: 0.4 s into its pass
    bool wrapped = false;
    for (auto& n : *c->notes) {
        wrapped = wrapped || std::fabs(n.start - 0.4) < 1e-6;
        CHECK(n.start >= 0 && n.start < c->lengthS);
    }
    CHECK(wrapped);
}

void testDragSlip() {
    Rig r;
    TrackPtr t = r.track(Kind::Audio);
    ClipPtr c = audioClip(r.project, t, 1, 1, 2);
    history::History h(r.project, SR);
    RectF box = r.rect(c);
    double y = box.y + box.h * 0.7, x = box.x + box.w / 2;
    auto e = mouse(x, y, plat::ModCtrl | plat::ModAlt);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<SlipDrag>(r.tl->drag) && r.tl->selection.contains(c));
    CHECK(r.status.find("Slip") != std::string::npos);
    int64_t step = (int64_t)std::llround(r.tl->gridStepAt(c->start));
    CHECK(step > 0);
    double moved = r.tl->pps * 0.3 + 7;  // (0.3 s and a bit: whole steps)
    e = mouse(x + moved, y, plat::ModCtrl | plat::ModAlt);
    r.tl->mouseMove(e);
    int64_t frames = r.tl->frameAt(x + moved) - r.tl->frameAt(x);
    int64_t snapped = (int64_t)std::llround(std::nearbyint((double)frames / step) * step);
    CHECK(c->offset == sec(1) - snapped && c->start == sec(1) && c->length == sec(2));
    CHECK(snapped % step == 0 && r.status.find("Slip +") != std::string::npos);
    // Shift as well: off the grid
    e = mouse(x + moved, y, plat::ModCtrl | plat::ModAlt | plat::ModShift);
    r.tl->mouseMove(e);
    CHECK(c->offset == sec(1) - frames);
    r.tl->mouseRelease(e);
    CHECK(h.check(SR) && !h.check(SR));  // one step
    CHECK(h.undo(SR) && c->offset == sec(1));
    // on the name strip too (not a copy)
    e = mouse(x, box.y + 4, plat::ModCtrl | plat::ModAlt);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<SlipDrag>(r.tl->drag));
    e = mouse(x - r.tl->pps * 0.5, box.y + 4, plat::ModCtrl | plat::ModAlt);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    CHECK(r.project.clips.size() == 1 && c->offset > sec(1));
    // Alt+Shift on the name strip: a copy (linked for MIDI), no slip
    e = mouse(x, box.y + 4, plat::ModAlt | plat::ModShift);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<CopyDrag>(r.tl->drag) && std::get<CopyDrag>(r.tl->drag).linked);
    r.tl->mouseRelease(e);
    // a slip pressed but not moved: nothing changes
    int64_t was = c->offset;
    e = mouse(x, y, plat::ModCtrl | plat::ModAlt);
    r.tl->mousePress(e);
    e = mouse(x + 1, y, plat::ModCtrl | plat::ModAlt);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    CHECK(c->offset == was);
}

void testTogether() {
    Rig r;
    TrackPtr a = r.track(Kind::Audio), b = r.track(Kind::Audio);
    ClipPtr c1 = audioClip(r.project, a, 1, 1, 1), c2 = audioClip(r.project, a, 4, 1, 1), c3 = audioClip(r.project, b, 2, 0.5, 2);
    history::History h(r.project, SR);
    r.tl->selection.clips = {c1, c2, c3};
    // the right end of one: all of them by as much (Alt: freely)
    RectF box = r.rect(c1);
    double y = box.y + box.h * 0.7;
    auto e = mouse(box.right() - 1, y);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(r.tl->drag) && std::get<EdgeDrag>(r.tl->drag).origin.size() == 3);
    int64_t to = r.tl->frameAt(box.right() - 1 + r.tl->pps * 0.5);
    e = mouse(box.right() - 1 + r.tl->pps * 0.5, y, plat::ModAlt);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    int64_t grew = to - sec(2);
    CHECK(c1->end() == sec(2) + grew && c2->end() == sec(5) + grew && c3->end() == sec(4) + grew);
    CHECK(h.check(SR) && !h.check(SR));
    CHECK(h.undo(SR) && c1->end() == sec(2) && c2->end() == sec(5) && c3->end() == sec(4));
    // the left end: each start earlier, each window into its audio as far as it goes (c3's: 0.5 s)
    box = r.rect(c1);
    e = mouse(box.x + 1, y);
    r.tl->mousePress(e);
    e = mouse(box.x + 1 - r.tl->pps * 0.75, y, plat::ModAlt);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    int64_t moved = sec(1) - c1->start;
    CHECK(moved > sec(0.5) && c2->start == sec(4) - moved && c3->start == sec(1.5) && c3->offset == 0);
    CHECK(c1->end() == sec(2) && c2->end() == sec(5) && c3->end() == sec(4));
    CHECK(h.check(SR) && h.undo(SR) && c1->start == sec(1) && c3->start == sec(2));
    // a fade-in handle: every selected audio clip's fade-in
    box = r.rect(c1);
    RectF body = box.adjusted(0, (float)clip_painter::headerHeight(), 0, 0), in, out;
    CHECK(clip_painter::fadeHandles(*c1, body, r.tl->pps, SR, in, out));
    e = mouse(in.x + in.w / 2, in.y + in.h / 2);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<FadeDrag>(r.tl->drag));
    e = mouse(box.x + r.tl->pps * 0.2, in.y + in.h / 2);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    int64_t fade = c1->fadeIn;
    CHECK(fade > sec(0.15) && fade < sec(0.25) && c2->fadeIn == fade && c3->fadeIn == fade);
    CHECK(h.check(SR) && !h.check(SR));
    CHECK(h.undo(SR) && c1->fadeIn == 0 && c2->fadeIn == 0 && c3->fadeIn == 0);
    // two selected clips that meet, both made longer: no overlap after
    ClipPtr d1 = audioClip(r.project, b, 6, 0, 1), d2 = audioClip(r.project, b, 7, 0, 1);
    ClipPtr after = audioClip(r.project, b, 8, 0, 1);  // (not selected: what d2 now covers of it is cut away)
    r.tl->selection.clips = {d1, d2};
    box = r.rect(d1);
    e = mouse(box.right() - 1, y + (r.rect(d1).y - r.rect(c1).y));
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(r.tl->drag));
    e = mouse(box.right() - 1 + r.tl->pps * 0.5, e.pos.y, plat::ModAlt);
    r.tl->mouseMove(e);
    r.tl->mouseRelease(e);
    CHECK(noOverlaps(r.project));
    CHECK(!r.project.clips.contains(after));  // (replaced by its piece after d2's new end)
}

void testCrop() {
    Rig r;
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < 64; i++) tracks.push_back(r.track(i % 4 == 3 ? Kind::Midi : Kind::Audio));
    std::vector<ClipPtr> clips;
    for (auto& t : tracks)
        clips.push_back(t->kind == Kind::Midi ? midiClip(r.project, t, 0, 4) : audioClip(r.project, t, 0, 0, 4));
    ClipPtr later = audioClip(r.project, tracks[0], 6, 0, 1);  // after the range: left alone
    ClipPtr other = audioClip(r.project, r.track(Kind::Audio), 0, 0, 4);  // a track outside the range, not selected
    history::History h(r.project, SR);
    TimeRange range{sec(1), sec(3), tracks};
    r.tl->selection.setRange(range, r.project.clipsIn(tracks, range.start, range.end));
    {
        ui::Menu menu;
        r.tl->fillMenu(menu, Point(100, 60));
        ui::Action* crop = actionStarting(menu, "Crop to the selection");
        CHECK(crop && crop->enabled && !crop->tooltip.empty());
        if (crop) ui::Menu::activate(crop);
    }
    CHECK(r.project.clips.size() == 66);
    int cut = 0;
    for (auto& c : r.project.clips) {
        if (c == later || c == other) continue;
        cut += c->start == sec(1) && c->frames() == sec(2) && c->offset == sec(1);
    }
    CHECK(cut == 64 && r.project.clips.contains(later) && r.project.clips.contains(other) && other->frames() == sec(4));
    CHECK(r.tl->selection.clips.size() == 64 && r.status.find("Cropped 64") != std::string::npos);
    CHECK(noOverlaps(r.project));
    CHECK(h.check(SR) && !h.check(SR));
    CHECK(h.undo(SR) && r.project.clips.size() == 66 && r.project.clips.contains(clips[0]) && clips[0]->frames() == sec(4));
    // without a range: off
    r.tl->selection.clear();
    ui::Menu menu;
    r.tl->fillMenu(menu, Point(100, 60));
    ui::Action* crop = actionStarting(menu, "Crop to the selection");
    CHECK(crop && !crop->enabled);
}

void testMenuDialogs() {
    Rig r;
    TrackPtr a = r.track(Kind::Audio), m = r.track(Kind::Midi);
    ClipPtr c1 = audioClip(r.project, a, 0, 1, 1), c2 = audioClip(r.project, a, 2, 1, 1), mc = midiClip(r.project, m, 0, 2);
    history::History h(r.project, SR);
    r.tl->selection.clips = {c1, c2, mc};
    clip_edit_dialogs::answerFades = [](clip_edit_dialogs::FadeChoice& f) {
        f.inMs = 50;
        f.outMs = 120;
        f.inShape = FadeShape::Smooth;
        f.outShape = FadeShape::EqualPower;
        return true;
    };
    clip_edit_dialogs::answerSlip = [](double& amount) {
        amount = 2;  // beats (1 s at 120 BPM)
        return true;
    };
    {
        ui::Menu menu;
        r.tl->fillMenu(menu, Point(50, 60));
        ui::Action* fades = actionStarting(menu, "Fades at every boundary");
        ui::Action* slip = actionStarting(menu, "Slip contents");
        CHECK(fades && fades->enabled && slip && slip->enabled);
        if (fades) ui::Menu::activate(fades);
    }
    for (auto& c : {c1, c2}) {
        CHECK(c->fadeIn == sec(0.05) && c->fadeOut == sec(0.12));
        CHECK(c->fadeInShape == FadeShape::Smooth && c->fadeOutShape == FadeShape::EqualPower);
    }
    CHECK(mc->fadeIn == 0);
    CHECK(h.check(SR) && !h.check(SR));
    {
        ui::Menu menu;
        r.tl->fillMenu(menu, Point(50, 60));
        if (ui::Action* slip = actionStarting(menu, "Slip contents")) ui::Menu::activate(slip);
    }
    CHECK(c1->offset == 0 && c2->offset == 0);  // (1 s later: the start of their audio)
    CHECK(std::fabs((*mc->notes)[0].start - 1.0) < 1e-9);
    CHECK(h.check(SR) && !h.check(SR));
    CHECK(h.undo(SR) && c1->offset == sec(1) && std::fabs((*mc->notes)[0].start) < 1e-9);
    CHECK(h.undo(SR) && c1->fadeIn == 0 && c2->fadeOut == 0);
    clip_edit_dialogs::answerFades = nullptr;
    clip_edit_dialogs::answerSlip = nullptr;
    // nothing selected: off
    r.tl->selection.clear();
    ui::Menu menu;
    r.tl->fillMenu(menu, Point(50, 60));
    CHECK(!actionStarting(menu, "Slip contents")->enabled && !actionStarting(menu, "Fades at every boundary")->enabled);
}

// 1,000 selected clips (audio, looping audio, MIDI, linked MIDI) slipped, trimmed, faded and cropped while the
// engine plays them on another thread; then everything undone
void testThousandWhilePlaying() {
    Rig r;
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < 64; i++) tracks.push_back(r.track(i % 4 == 3 ? Kind::Midi : Kind::Audio));
    for (int k = 0; k < 1000; k++) {
        TrackPtr t = tracks[(size_t)(k % 64)];
        double start = 0.2 + (k / 64) * 0.6;
        if (t->kind == Kind::Midi) {
            ClipPtr c = midiClip(r.project, t, start, 0.5);
            if (k % 3 == 0) c->link = std::make_shared<ClipLink>();  // (some share their notes with the next)
        } else {
            ClipPtr c = audioClip(r.project, t, start, 0.25, 0.5, 1);
            if (k % 5 == 0) r.tl->setLoopNotes({c}, true);
        }
    }
    {  // linked pairs: the next clip on the track shares the first one's link and notes
        std::map<TrackPtr, ClipPtr> lastLinked;
        for (auto& c : r.project.clips) {
            if (c->kind != Kind::Midi) continue;
            auto it = lastLinked.find(c->track);
            if (c->link) lastLinked[c->track] = c;
            else if (it != lastLinked.end() && it->second) {
                c->link = it->second->link;
                c->notes = it->second->notes;
                it->second = nullptr;
            }
        }
    }
    CHECK(r.project.clips.size() == 1000);
    std::vector<ClipPtr> original = r.project.clips.copy();
    std::vector<std::array<int64_t, 3>> fields;
    for (auto& c : original) fields.push_back({c->start, c->offset, c->length});
    history::History h(r.project, SR);
    r.engine.offline = true;
    r.engine.play();
    std::atomic<bool> done{false};
    bool finite = true;
    int blocks = 0;
    std::thread audio([&] {
        std::vector<float> out(256 * 2);
        while (!done) {
            r.engine.callback(nullptr, out.data(), 256);
            for (float v : out) finite = finite && std::isfinite(v);
            blocks++;
        }
    });
    r.tl->selectAll();
    CHECK(r.tl->selection.clips.size() == 1000);
    int steps = 0;
    // slip: back and forth, on and off the grid
    ClipPtr first = original[0];
    RectF box = r.rect(first);
    double x = box.x + box.w / 2, y = box.y + box.h * 0.7;
    auto e = mouse(x, y, plat::ModCtrl | plat::ModAlt);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<SlipDrag>(r.tl->drag));
    for (int i = 0; i < 24; i++) {
        double dx = (i % 2 ? -1 : 1) * r.tl->pps * (0.05 + 0.13 * (i % 7));
        e = mouse(x + dx, y, plat::ModCtrl | plat::ModAlt | (i % 3 == 0 ? plat::ModShift : 0));
        r.tl->mouseMove(e);
    }
    r.tl->mouseRelease(e);
    steps += h.check(SR);
    // the right ends of all of them, longer then shorter (Alt: freely)
    box = r.rect(first);
    e = mouse(box.right() - 1, y);
    r.tl->mousePress(e);
    CHECK(std::holds_alternative<EdgeDrag>(r.tl->drag));
    for (int i = 0; i < 16; i++) {
        e = mouse(box.right() - 1 + (i % 2 ? -0.3 : 0.4) * r.tl->pps * (1 + i % 3) / 3, y, plat::ModAlt);
        r.tl->mouseMove(e);
    }
    r.tl->mouseRelease(e);
    steps += h.check(SR);
    // their fade-ins
    std::vector<ClipPtr> now;
    for (auto& c : r.project.clips)
        if (r.tl->selection.contains(c)) now.push_back(c);
    ClipPtr audioOne;  // (the longest: wide enough for its handles)
    for (auto& c : now)
        if (c->kind == Kind::Audio && (!audioOne || c->frames() > audioOne->frames())) audioOne = c;
    r.tl->zoom(4, 0);
    if (audioOne) {
        box = r.rect(audioOne);
        RectF body = box.adjusted(0, (float)clip_painter::headerHeight(), 0, 0), in, out;
        if (clip_painter::fadeHandles(*audioOne, body, r.tl->pps, SR, in, out)) {
            e = mouse(in.x + in.w / 2, in.y + in.h / 2);
            r.tl->mousePress(e);
            CHECK(std::holds_alternative<FadeDrag>(r.tl->drag));
            for (int i = 0; i < 16; i++) {
                e = mouse(box.x + r.tl->pps * 0.02 * (i + 1), in.y + in.h / 2);
                r.tl->mouseMove(e);
            }
            r.tl->mouseRelease(e);
            steps += h.check(SR);
        }
    }
    // slip contents from the menu, then crop to a range over every track
    r.tl->slipSelected(-sec(0.123));
    steps += h.check(SR);
    TimeRange range{sec(1), sec(5), tracks};
    r.tl->selection.setRange(range, r.project.clipsIn(tracks, range.start, range.end));
    r.tl->cropSelected();
    steps += h.check(SR);
    CHECK(noOverlaps(r.project));
    for (auto& c : r.project.clips) CHECK(c->frames() > 0 && c->offset >= 0);
    // everything undone while it still plays
    for (int i = 0; i < steps; i++) CHECK(h.undo(SR));
    done = true;
    audio.join();
    CHECK(finite && blocks > 0 && r.engine.lastError().empty());
    CHECK(steps == 5);
    bool same = r.project.clips.size() == original.size();
    for (size_t i = 0; same && i < original.size(); i++) {
        const ClipPtr& c = r.project.clips[i];
        same = c == original[i] && c->start == fields[i][0] && c->offset == fields[i][1] && c->length == fields[i][2];
    }
    CHECK(same);
    printf("1,000 clips: %d undo steps, %d blocks played meanwhile\n", steps, blocks);
}

// --- the terminal UI ---

struct Driver {
    tui::Front* f;
    std::vector<tui::Key> queue;
    size_t at = 0;
    tui::Screen screen{100, 30};
    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    void keys(std::vector<tui::Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (double t = 0; t < 0.3; t += 0.05) {  // (the undo history looks every 250 ms)
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
    // F2's entry starting so, chosen (its index highlighted, then Enter)
    bool choose(const std::string& prefix, std::vector<tui::Key> then) {
        keys({tui::Key{plat::Key_F2, 0, ""}});
        if (!f->menuOpen()) return false;
        const auto& acts = f->open.back().menu->actions();
        int index = -1;
        for (size_t i = 0; i < acts.size(); i++)
            if (acts[i]->text.compare(0, prefix.size(), prefix) == 0) index = (int)i;
        if (index < 0) return false;
        f->open.back().item = index;
        then.insert(then.begin(), tui::Key{plat::Key_Return, 0, ""});
        keys(then);
        return true;
    }
};

tui::Key key(int k, int mods = 0, std::string text = "") { return tui::Key{k, mods, std::move(text)}; }

void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    Driver d{&f};
    bool sawDialog = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        f.draw(d.screen);
        sawDialog = sawDialog || (d.shows("Slip contents") && d.shows("grid step"));
        d.step(done);
    };
    Timeline& tl = *w->timeline;
    TrackPtr t = tl.addTrack(Kind::Audio);
    ClipPtr c1 = audioClip(w->project, t, 0, 1, 1), c2 = audioClip(w->project, t, 3, 1, 1);
    w->project.timing.grid = 1;  // (a beat: 0.5 s at 120 BPM)
    tl.relayout();
    d.keys({});
    f.pane = tui::Front::Pane::Timeline;
    tl.selection.selectOnly(c1);
    // Slip contents…: Right twice (two grid steps: a beat each), Left once, Enter
    CHECK(d.choose("Slip contents", {key(plat::Key_Right), key(plat::Key_Right), key(plat::Key_Left), key(plat::Key_Return)}));
    CHECK(sawDialog);
    CHECK(c1->offset == sec(0.5) && c1->start == 0 && c1->length == sec(1) && c2->offset == sec(1));
    // typed: -1 beat; then Ctrl+Z undoes it
    CHECK(d.choose("Slip contents", std::vector<tui::Key>{key('A', plat::ModCtrl), key('-', 0, "-"), key('1', 0, "1"),
                                                          key(plat::Key_Return)}));
    CHECK(c1->offset == sec(1));
    d.keys({key('Z', plat::ModCtrl)});
    CHECK(c1->offset == sec(0.5));
    // both clips selected: the fade dialog fades both (the fade-in typed, the rest as it was)
    tl.selection.clips = {c1, c2};
    CHECK(d.choose("Fades at every boundary", {key('A', plat::ModCtrl), key('4', 0, "4"), key('0', 0, "0"), key(plat::Key_Return)}));
    CHECK(c1->fadeIn == sec(0.04) && c2->fadeIn == sec(0.04));
    // and Slip contents… slips both
    CHECK(d.choose("Slip contents", {key(plat::Key_Left), key(plat::Key_Return)}));
    CHECK(c1->offset == sec(1) && c2->offset == sec(1.5));
    // Esc cancels
    CHECK(d.choose("Slip contents", {key(plat::Key_Right), key(plat::Key_Escape)}));
    CHECK(c1->offset == sec(1));
    app.nestedLoop = nullptr;
    w->close();
    app.tick();
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs("build/test-clip-slip");
    std::remove("build/test-clip-slip/settings.json");
    settings::setPath("build/test-clip-slip/settings.json");
    testSlipAudio();
    testSlipEnvelope();
    testSlipLooping();
    testSlipMidi();
    testDragSlip();
    testTogether();
    testCrop();
    testMenuDialogs();
    testThousandWhilePlaying();
    testTerminal();
    TEST_MAIN_END();
}
