// Automation that follows clips (model/auto_follow.h), headless: the curve pieces (cut, cleared, pasted keep
// the curve outside its span); clips dragged on their track, to another track and Ctrl-dragged as copies,
// with the switch on and off; copy, cut and paste between tracks with and without the same parameter; an
// automation lane's own range copied, pasted and cleared; Duplicate range over 64 tracks of dense automation
// 100 times, then all of it undone; Insert / Delete time moving automation, markers and tempo changes; the
// switch saved, loaded, undoable and in the Edit and Grid menus; clips and their automation moved back and forth
// between 64 tracks while the audio thread plays them; moves onto a duplicated track while several tracks are
// selected (a gain envelope kept); and the same in the terminal UI (--tui).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "model/auto_follow.h"
#include "model/history.h"
#include "model/project_io.h"
#include "model/song_time.h"
#include "paths.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "model/track_ops.h"
#include "ui/clipboard.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/transport_bar.h"

namespace {

const int SR = 48000;
const std::string TMP = "build/ui-samples/auto-follow";

int64_t sec(double s) { return (int64_t)std::llround(s * SR); }

ClipPtr audioClip(Project& p, const TrackPtr& t, double start, double seconds) {
    static AudioPtr silence = [] {
        auto a = std::make_shared<AudioBuffer>();
        a->samples.assign((size_t)sec(8) * 2, 0.0f);
        return a;
    }();
    auto c = makeAudioClip("Take", silence, SR, SR);
    c->track = t;
    c->start = sec(start);
    c->length = sec(seconds);
    p.addClip(c);
    return c;
}

AutomationPtr addLane(const TrackPtr& t, const SlotPtr& slot, const std::string& param, std::vector<AutoPoint> pts) {
    auto a = std::make_shared<Automation>();
    a->slot = slot;
    a->param = param;
    a->setPoints(std::move(pts));
    t->automation.append(a);
    t->showAutomation = true;
    return a;
}

AutomationPtr laneOf(const TrackPtr& t, const std::string& param) {
    for (auto& a : t->automation)
        if (a->param == param) return a;
    return nullptr;
}

SlotPtr effect(const TrackPtr& t, const std::string& path) {
    auto s = std::make_shared<PluginSlot>();
    s->path = path;
    s->name = path;
    s->kind = "effect";
    std::vector<SlotPtr> fx = t->effects.copy();
    fx.push_back(s);
    t->effects = fx;
    return s;
}

double val(const AutomationPtr& a, double s) { return automation::valueAt(*a->points, s); }

// a bump: 0.5, up to 0.9 at 1.5 s, down to 0.1 at 2.5 s, 0.5 again from 4 s
std::vector<AutoPoint> bump() { return {{0, 0.5}, {1.5, 0.9, 0.4}, {2.5, 0.1}, {4, 0.5}, {9, 0.5}}; }

ui::MouseEvent mouse(double x, double y, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point((int)x, (int)y);
    e.globalPos = e.pos;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    e.mods = mods;
    return e;
}

// drag the clip's name strip by `dt` seconds and `dy` px (Ctrl: copies)
void dragClip(Timeline& tl, const ClipPtr& c, double dt, double dy, int mods = 0) {
    tl.relayout();
    RectF r = *tl.clipRect(*c, tl.trackTops());
    double x = r.x + r.w / 2, y = r.y + 6;  // (the middle of its name strip: not an end, not a fade handle)
    auto e = mouse(x, y, mods);
    tl.mousePress(e);
    e = mouse(x + dt * tl.pps, y + dy, mods | plat::ModAlt);  // (Alt: off the grid, exactly)
    tl.mouseMove(e);
    tl.mouseRelease(e);
}

struct Rig {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    Timeline* tl;
    TrackPtr a1, a2;
    Rig() {
        scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
        scroll.resize(1400, 700);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        engine.sr = SR;
        a1 = project.addTrack(Kind::Audio);
        a2 = project.addTrack(Kind::Audio);
        tl->relayout();
    }
    void drag(const ClipPtr& c, double dt, double dy, int mods = 0) { dragClip(*tl, c, dt, dy, mods); }
    std::optional<Lane> lane(const AutomationPtr& a) {
        for (auto& l : tl->lanes())
            if (l.automation == a) return l;
        return std::nullopt;
    }
};

void testCurves() {
    using namespace auto_follow;
    std::vector<AutoPoint> ramp{{0, 0}, {10, 1}};
    auto piece = cut(ramp, 2, 2);
    CHECK(piece.size() == 2 && piece[0].t == 0 && std::fabs(piece[0].v - 0.2) < 1e-12 && piece[1].t == 2 &&
          std::fabs(piece[1].v - 0.4) < 1e-12);
    CHECK(cut({}, 2, 2).empty());
    // cleared: a straight line across, the curve outside as it was (a curved segment keeps its shape)
    std::vector<AutoPoint> b = bump();
    auto c = cleared(b, 2, 1);
    CHECK_NEAR(automation::valueAt(c, 2.5), (automation::valueAt(b, 2) + automation::valueAt(b, 3)) / 2, 1e-9);
    for (double s : {0.5, 1.0, 1.7, 1.99, 3.01, 3.5, 6.0}) CHECK_NEAR(automation::valueAt(c, s), automation::valueAt(b, s), 1e-9);
    // pasted: the piece over its span exactly, the curve around it unchanged (a jump EDGE_S wide at each edge)
    auto p = pasted(b, piece, 5, 2);
    CHECK_NEAR(automation::valueAt(p, 5), 0.2, 1e-12);
    CHECK_NEAR(automation::valueAt(p, 6), 0.3, 1e-12);
    CHECK_NEAR(automation::valueAt(p, 7), 0.4, 1e-12);
    for (double s : {0.5, 1.0, 1.7, 2.5, 3.5, 4.99, 7.01, 8.0}) CHECK_NEAR(automation::valueAt(p, s), automation::valueAt(b, s), 1e-9);
    // pasting into a curved segment: both sides keep their shape
    std::vector<AutoPoint> curved{{0, 0, 0.7}, {10, 1}};
    auto q = pasted(curved, piece, 4, 2);
    for (double s : {1.0, 2.0, 3.9, 6.1, 7.0, 9.5}) CHECK_NEAR(automation::valueAt(q, s), automation::valueAt(curved, s), 1e-9);
    // into an empty curve: the piece alone; an empty piece changes nothing; at 0 s: no point before it
    auto e = pasted({}, piece, 3, 2);
    CHECK(e.size() == 2 && e[0].t == 3 && e[1].t == 5);
    CHECK(pasted(b, {}, 3, 2) == b);
    auto z = pasted(b, piece, 0, 2);
    CHECK_NEAR(automation::valueAt(z, 0), 0.2, 1e-12);
    for (auto& pt : z) CHECK(pt.t >= 0);
    // cut → pasted at the same place: the same curve there
    auto same = pasted(b, cut(b, 1, 2), 1, 2);
    for (double s = 0; s < 9; s += 0.05) CHECK_NEAR(automation::valueAt(same, s), automation::valueAt(b, s), 1e-9);
    // a worst case: a piece of 100 000 points pasted inside a curve of 100 000 points, in order
    std::vector<AutoPoint> dense;
    for (int i = 0; i < 100000; i++) dense.push_back({i * 0.001, (i % 7) / 7.0});
    auto big = pasted(dense, cut(dense, 10, 50), 30, 50);
    bool ordered = true;
    for (size_t i = 1; i < big.size(); i++) ordered = ordered && big[i - 1].t <= big[i].t;
    CHECK(ordered);
    CHECK_NEAR(automation::valueAt(big, 30.0105), automation::valueAt(dense, 10.0105), 1e-9);
}

void testMove(bool on) {
    Rig r;
    r.project.timing.automationFollows = on;
    auto clip = audioClip(r.project, r.a1, 1, 2);
    auto vol = addLane(r.a1, r.a1->controls, automation::VOLUME, bump());
    auto fx = effect(r.a1, "/plugins/Delay.vst3");
    auto mix = addLane(r.a1, fx, "Mix", bump());
    std::vector<AutoPoint> before = vol->points.copy();
    // along its own track: 1…3 s → 5…7 s
    r.drag(clip, 4, 0);
    CHECK(clip->start == sec(5) && clip->track == r.a1);
    if (on) {
        for (double s : {1.2, 1.5, 2.0, 2.9}) {
            CHECK_NEAR(val(vol, s + 4), automation::valueAt(before, s), 1e-6);
            CHECK_NEAR(val(mix, s + 4), automation::valueAt(before, s), 1e-6);
        }
        double a = automation::valueAt(before, 1), b = automation::valueAt(before, 3);
        CHECK_NEAR(val(vol, 2), (a + b) / 2, 1e-9);  // (where it was: a straight line)
        CHECK_NEAR(val(vol, 0.5), automation::valueAt(before, 0.5), 1e-9);
        CHECK_NEAR(val(vol, 8), automation::valueAt(before, 8), 1e-9);
    } else {
        CHECK(*vol->points == before);
    }
    // to the track below: Volume goes (a lane made there), the Delay's Mix stays (no Delay there)
    std::vector<AutoPoint> volNow = vol->points.copy(), mixNow = mix->points.copy();
    int lanesY = 0;
    for (auto& l : r.tl->lanes())
        if (l.track == r.a2) lanesY = l.top;
    RectF rect = *r.tl->clipRect(*clip, r.tl->trackTops());
    r.drag(clip, 0, lanesY - rect.y + 4);
    CHECK(clip->track == r.a2);
    AutomationPtr vol2 = laneOf(r.a2, automation::VOLUME);
    CHECK(*mix->points == mixNow);
    CHECK(!laneOf(r.a2, "Mix"));
    if (on) {
        CHECK(vol2 && vol2->slot == r.a2->controls && r.a2->showAutomation);
        if (vol2)
            for (double s : {5.2, 5.5, 6.0, 6.9}) CHECK_NEAR(val(vol2, s), automation::valueAt(volNow, s), 1e-6);
    } else {
        CHECK(!vol2 && *vol->points == volNow);
    }
    // Ctrl+drag: a copy, its automation copied (the original's stays)
    r.tl->selection.clear();
    if (vol2) {
        std::vector<AutoPoint> v2 = vol2->points.copy();
        r.drag(clip, -4, 0, plat::ModCtrl);
        CHECK(r.project.clips.size() == 2);
        if (on) {
            for (double s : {5.2, 5.5, 6.0, 6.9}) {
                CHECK_NEAR(val(vol2, s), automation::valueAt(v2, s), 1e-6);
                CHECK_NEAR(val(vol2, s - 4), automation::valueAt(v2, s), 1e-6);
            }
        }
    }
}

void testClipboard() {
    Rig r;
    Timeline& tl = *r.tl;
    auto clip = audioClip(r.project, r.a1, 1, 2);
    auto vol = addLane(r.a1, r.a1->controls, automation::VOLUME, bump());
    auto fx = effect(r.a1, "/plugins/Delay.vst3");
    auto mix = addLane(r.a1, fx, "Mix", bump());
    std::vector<AutoPoint> orig = vol->points.copy();
    tl.selection.selectOnly(clip);
    CHECK(clipboard::use(tl, "copy") == "Copied 1 clip");
    CHECK(clipboard::held && clipboard::held->automation.size() == 1 && clipboard::held->automation[0].pieces.size() == 2);
    // pasted onto the other track at 8 s: Volume comes (a lane made), Mix doesn't (no Delay there)
    tl.setCurrentTrack(r.a2);
    tl.setCursor(sec(8));
    CHECK(clipboard::use(tl, "paste") == "Pasted 1 clip");
    AutomationPtr vol2 = laneOf(r.a2, automation::VOLUME);
    CHECK(vol2 && !laneOf(r.a2, "Mix"));
    if (vol2)
        for (double s : {1.1, 1.5, 2.2, 2.9}) CHECK_NEAR(val(vol2, s + 7), automation::valueAt(orig, s), 1e-6);
    // with the same plugin there (another slot of the same file): its Mix lane is made and gets the curve
    auto fx2 = effect(r.a2, "/plugins/Delay.vst3");
    tl.setCursor(sec(12));
    clipboard::use(tl, "paste");
    AutomationPtr mix2 = laneOf(r.a2, "Mix");
    CHECK(mix2 && mix2->slot == fx2);
    if (mix2) CHECK_NEAR(val(mix2, 12.5), automation::valueAt(orig, 1.5), 1e-6);
    // switched off: the clips come alone
    r.project.timing.automationFollows = false;
    std::vector<AutoPoint> v2 = vol2 ? vol2->points.copy() : std::vector<AutoPoint>{};
    tl.setCursor(sec(20));
    clipboard::use(tl, "paste");
    CHECK(vol2 && *vol2->points == v2);
    r.project.timing.automationFollows = true;
    // cut: the clip goes, and its automation with it (a straight line where it was); pasted back: as it was
    tl.selection.selectOnly(clip);
    CHECK(clipboard::use(tl, "cut") == "Cut 1 clip");
    CHECK(!r.project.clips.contains(clip));
    CHECK_NEAR(val(vol, 2), (automation::valueAt(orig, 1) + automation::valueAt(orig, 3)) / 2, 1e-9);
    CHECK(*mix->points != orig);
    tl.setCurrentTrack(r.a1);
    tl.setCursor(sec(1));
    clipboard::use(tl, "paste");
    for (double s = 0.5; s < 8; s += 0.1) CHECK_NEAR(val(vol, s), automation::valueAt(orig, s), 1e-6);
    // a range over both tracks: each track's automation over all of it, even where no clip is
    tl.selection.setRange(TimeRange{sec(0), sec(4), {r.a1, r.a2}}, r.project.clipsIn({r.a1, r.a2}, sec(0), sec(4)));
    clipboard::use(tl, "copy");
    CHECK(clipboard::held && clipboard::held->automation.size() == 2);
}

void testLaneRange() {
    Rig r;
    Timeline& tl = *r.tl;
    auto clip = audioClip(r.project, r.a1, 1, 2);
    auto vol = addLane(r.a1, r.a1->controls, automation::VOLUME, bump());
    std::vector<AutoPoint> orig = vol->points.copy();
    tl.relayout();
    auto lane = r.lane(vol);
    CHECK(lane.has_value());
    if (!lane) return;
    // Shift+drag in the lane: a range on its track, the lane's own
    double y = lane->top + lane->height / 2.0;
    auto e = mouse(tl.xOf(sec(1)), y, plat::ModShift | plat::ModAlt);
    tl.mousePress(e);
    CHECK(std::holds_alternative<RangeDrag>(tl.drag) && tl.currentAutomation == vol);
    e = mouse(tl.xOf(sec(3)), y, plat::ModShift | plat::ModAlt);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    tl.drag = std::monostate();
    CHECK(tl.selection.range && tl.selection.range->start == sec(1) && tl.selection.range->end == sec(3));
    CHECK(vol->points->size() == orig.size());  // (no point added by the press)
    CHECK(clipboard::laneInUse(tl) == vol);
    CHECK(clipboard::use(tl, "copy").find("Copied the selected part of Volume") == 0);
    CHECK(clipboard::heldCurve && std::fabs(clipboard::heldCurve->length - 2) < 1e-9);
    tl.setCursor(sec(5));
    CHECK(clipboard::use(tl, "paste") == "Pasted the curve into Volume");
    for (double s : {1.1, 1.5, 2.0, 2.9}) CHECK_NEAR(val(vol, s + 4), automation::valueAt(orig, s), 1e-6);
    CHECK(tl.cursor == sec(7));
    // Del clears the range in the lane (the clip under it stays)
    tl.deleteSelected();
    CHECK(r.project.clips.contains(clip));
    CHECK_NEAR(val(vol, 2), (automation::valueAt(orig, 1) + automation::valueAt(orig, 3)) / 2, 1e-9);
    CHECK_NEAR(val(vol, 0.5), automation::valueAt(orig, 0.5), 1e-9);
    // clicking the track's lane: back to clips
    for (auto& l : tl.lanes())
        if (l.track == r.a2) {
            auto p = mouse(tl.xOf(sec(9)), l.top + l.height / 2.0);
            tl.mousePress(p);
            tl.mouseRelease(p);
        }
    CHECK(!tl.currentAutomation && !clipboard::laneInUse(tl, false));
}

void testDuplicateStress() {
    // 64 tracks, each with a Volume and a Pan lane of 256 points over the range, duplicated 100 times, undone
    Rig r;
    Timeline& tl = *r.tl;
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < 64; i++) tracks.push_back(i < 2 ? (i ? r.a2 : r.a1) : r.project.addTrack(Kind::Audio));
    std::vector<std::vector<AutoPoint>> origs;
    for (size_t i = 0; i < tracks.size(); i++) {
        std::vector<AutoPoint> pts;
        for (int k = 0; k < 256; k++) pts.push_back({k * 2.0 / 256, std::fmod(k * 0.37 + i * 0.01, 1.0), k % 3 ? 0.3 : 0.0});
        addLane(tracks[i], tracks[i]->controls, automation::VOLUME, pts);
        addLane(tracks[i], tracks[i]->controls, automation::PAN, pts);
        origs.push_back(pts);
        if (i % 8 == 0) audioClip(r.project, tracks[i], 0, 2);
    }
    tl.relayout();
    history::History h(r.project, SR);
    tl.selection.setRange(TimeRange{0, sec(2), tracks}, r.project.clipsIn(tracks, 0, sec(2)));
    auto t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < 100; n++) {
        tl.duplicateSelected();
        h.check(SR);
    }
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("100 duplicates of 64 tracks × 2 lanes in %.2f s\n", took);
    CHECK(h.undoSteps() == 100);
    CHECK(r.project.clips.size() == 8 * 101);
    // every copy has the curve: 2 s after 2 s…
    bool same = true;
    for (size_t i = 0; i < tracks.size(); i += 7) {
        auto vol = laneOf(tracks[i], automation::VOLUME);
        for (int n : {1, 37, 100})
            for (double s : {0.01, 0.5, 1.3, 1.99}) same = same && std::fabs(val(vol, n * 2 + s) - automation::valueAt(origs[i], s)) < 1e-6;
    }
    CHECK(same);
    int undone = 0;
    while (h.undo(SR)) undone++;
    CHECK(undone == 100);
    CHECK(r.project.clips.size() == 8);
    bool back = true;
    for (size_t i = 0; i < tracks.size(); i++)
        back = back && *laneOf(tracks[i], automation::VOLUME)->points == origs[i] &&
               *laneOf(tracks[i], automation::PAN)->points == origs[i];
    CHECK(back);
    // the switch off: duplicating leaves the automation alone
    r.project.timing.automationFollows = false;
    tl.selection.setRange(TimeRange{0, sec(2), tracks}, r.project.clipsIn(tracks, 0, sec(2)));
    tl.duplicateSelected();
    CHECK(*laneOf(tracks[5], automation::VOLUME)->points == origs[5]);
    // duplicated clips (Ctrl+D without a range): their own spans' automation
    r.project.timing.automationFollows = true;
    tl.selection.clear();
    tl.selection.selectOnly(r.project.clipsOn(tracks[8])[0]);
    tl.duplicateSelected();
    CHECK_NEAR(val(laneOf(tracks[8], automation::VOLUME), 2.5), automation::valueAt(origs[8], 0.5), 1e-6);
}

void testDuringPlayback() {
    // the audio thread plays 64 tracks of automated clips as fast as it can while the GUI thread moves them,
    // with their automation, back and forth between the tracks (lanes made on the way) and duplicates ranges
    Project p;
    audio::Engine e(p);
    e.offline = true;
    e.sr = SR;
    std::vector<TrackPtr> tracks;
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 64; i++) {
        tracks.push_back(p.addTrack(Kind::Audio));
        clips.push_back(audioClip(p, tracks.back(), 0, 1));
        std::vector<AutoPoint> pts;
        for (int k = 0; k < 64; k++) pts.push_back({k / 16.0, (k % 5) / 4.0});
        if (i % 2 == 0) addLane(tracks.back(), tracks.back()->controls, automation::VOLUME, pts);
        addLane(tracks.back(), tracks.back()->controls, automation::PAN, pts);
    }
    const int BLOCK = 64;
    const int64_t LENGTH = sec(3);
    std::atomic<bool> done{false};
    bool finite = true;
    e.play();
    std::thread audio([&] {
        std::vector<float> out((size_t)BLOCK * 2);
        for (int64_t f = 0; f < LENGTH; f += BLOCK) {
            e.callback(nullptr, out.data(), BLOCK);
            for (float v : out) finite = finite && std::isfinite(v);
        }
        done = true;
    });
    int rounds = 0;
    while (!done) {
        std::vector<auto_follow::Move> moves;
        for (size_t i = 0; i < clips.size(); i++) {
            const ClipPtr& c = clips[i];
            TrackPtr from = c->track;
            int64_t start = c->start;
            c->start = (c->start + sec(0.25)) % sec(3);
            std::atomic_store(&c->track, tracks[(i + (size_t)rounds + 1) % tracks.size()]);
            moves.push_back(auto_follow::moveOf(*c, from, start, SR));
        }
        auto_follow::carry(p, moves, rounds % 3 == 0);
        e.idle();
        rounds++;
    }
    audio.join();
    printf("during playback: %d rounds of 64 clips moved with their automation\n", rounds);
    CHECK(finite && rounds > 0 && e.pos == LENGTH && e.lastError().empty());
    bool ordered = true;
    for (auto& t : tracks)
        for (auto& lane : t->automation) {
            CHECK(lane->slot == t->controls);  // (a Volume lane made on a track only for its own Volume)
            const auto& pts = *lane->points;
            for (size_t k = 1; k < pts.size(); k++) ordered = ordered && pts[k - 1].t <= pts[k].t;
        }
    CHECK(ordered);
    e.stop();
}

void testWithTrackSelection() {
    // with what came with the track panel's selection: several tracks selected while clips move; a duplicated
    // track (track_ops: copies of its effects) takes its original's effect lanes; a clip's gain envelope stays
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Timeline& tl = *w->timeline;
    TrackPtr t1 = w->project.addTrack(Kind::Audio), t3 = w->project.addTrack(Kind::Audio);
    auto fx = effect(t1, "/plugins/Delay.vst3");
    auto mix = addLane(t1, fx, "Mix", bump());
    auto vol = addLane(t1, t1->controls, automation::VOLUME, bump());
    std::vector<AutoPoint> orig = bump();
    TrackPtr t2 = track_ops::duplicate(w->project, t1, false);  // (right below t1: Delay copied, unloaded)
    CHECK(w->project.indexOf(t2) == 1 && t2->effects.size() == 1 && t2->effects[0] != fx);
    for (auto& lane : *t2->automation) lane->setPoints({});  // (the copy's own curves: none)
    auto clip = audioClip(w->project, t1, 1, 2);
    clip->gainEnvelope = std::make_shared<const std::vector<AutoPoint>>(std::vector<AutoPoint>{{0, 0.3}, {1, 0.8}});
    auto envelope = clip->gainEnvelope;
    w->timeline->lanesChanged();
    tl.relayout();
    // three tracks selected in the panel
    w->panel->select(t1);
    w->panel->select(t3, plat::ModShift);
    CHECK(w->panel->selected.size() == 3);
    // the clip dragged onto the duplicate: Volume and the Delay's Mix go (onto the copy's own Delay), the envelope stays
    int y2 = 0;
    for (auto& l : tl.lanes())
        if (l.track == t2) y2 = l.top;
    RectF r = *tl.clipRect(*clip, tl.trackTops());
    tl.selection.selectOnly(clip);
    dragClip(tl, clip, 0, y2 - r.y + 4);
    CHECK(clip->track == t2 && clip->start == sec(1));
    CHECK(clip->gainEnvelope == envelope);
    AutomationPtr mix2 = laneOf(t2, "Mix"), vol2 = laneOf(t2, automation::VOLUME);
    CHECK(mix2 && mix2->slot == t2->effects[0] && vol2 && vol2->slot == t2->controls);
    if (mix2 && vol2)
        for (double s : {1.2, 1.5, 2.5}) {
            CHECK_NEAR(val(mix2, s), automation::valueAt(orig, s), 1e-6);
            CHECK_NEAR(val(vol2, s), automation::valueAt(orig, s), 1e-6);
        }
    CHECK_NEAR(val(mix, 2), (automation::valueAt(orig, 1) + automation::valueAt(orig, 3)) / 2, 1e-9);
    CHECK(w->panel->selected.size() == 3);  // (the track selection untouched)
    // onto t3 (no Delay): Volume goes, Mix stays on the copy
    std::vector<AutoPoint> mixNow = mix2 ? mix2->points.copy() : std::vector<AutoPoint>{};
    int y3 = 0;
    for (auto& l : tl.lanes())
        if (l.track == t3) y3 = l.top;
    r = *tl.clipRect(*clip, tl.trackTops());
    dragClip(tl, clip, 0, y3 - r.y + 4);
    CHECK(clip->track == t3 && laneOf(t3, automation::VOLUME) && !laneOf(t3, "Mix"));
    CHECK(mix2 && *mix2->points == mixNow);
    (void)vol;
    delete w;
    app.paintWindows = true;
}

void testTime() {
    // insert and delete time (whole bars at 120 BPM: 2 s each) move automation, markers and tempo changes
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto vol = addLane(t, t->controls, automation::VOLUME, {{0, 0}, {4, 1}, {8, 0}});
    p.setMarkers({{4, "Chorus"}, {9, "Outro"}});
    p.timing.setChange(4, 90, 4);  // (bar 4: 6 s)
    std::string note, error;
    CHECK(song_time::insertTime(p, sec(2), sec(2), SR, note, error));  // a bar at bar 2
    CHECK_NEAR(val(vol, 6), 1, 1e-9);
    CHECK_NEAR(val(vol, 1), 0.25, 1e-9);
    CHECK(p.markers.size() == 2 && p.markers[0].seconds == 6 && p.markers[1].seconds == 11);
    CHECK(p.timing.changeAtBar(5) && !p.timing.changeAtBar(4));
    CHECK(song_time::deleteTime(p, sec(2), sec(4), SR, note, error));
    CHECK_NEAR(val(vol, 4), 1, 1e-6);
    CHECK_NEAR(val(vol, 8), 0, 1e-6);
    CHECK(p.markers.size() == 2 && p.markers[0].seconds == 4 && p.markers[1].seconds == 9);
    CHECK(p.timing.changeAtBar(4) && !p.timing.changeAtBar(5));
}

void testSetting() {
    // saved only when off; an older file (without it): on
    Timing t;
    CHECK(t.automationFollows);
    CHECK(project_io::describeTiming(t)["automation_follows_clips"].isNull());
    t.automationFollows = false;
    Json j = project_io::describeTiming(t);
    CHECK(j["automation_follows_clips"].isBool() && !j["automation_follows_clips"].toBool());
    CHECK(!project_io::readTiming(j).automationFollows);
    CHECK(project_io::readTiming(Json::object({{"bpm", Json(100.0)}})).automationFollows);
    Timing copy = t;
    CHECK(!copy.automationFollows);
    // a whole project saved and opened again
    util::makeDirs(TMP);
    Project p;
    p.addTrack(Kind::Audio);
    p.timing.automationFollows = false;
    project_io::MasterSettings m;
    std::string error;
    CHECK(project_io::saveProject(TMP + "/off.json", p, SR, m, error));
    Project q;
    CHECK(project_io::loadProject(TMP + "/off.json", q, SR, m, error) && !q.timing.automationFollows);
    // undoable, named
    history::History h(p, SR);
    p.timing.automationFollows = true;
    CHECK(h.check(SR));
    CHECK(h.labels().back() == "Automation follows clips");
    CHECK(h.undo(SR) && !p.timing.automationFollows);
    // the Grid menu's switch
    ui::Menu grid;
    GridChange chosen;
    transport_bar::fillGridMenu(grid, p.timing, [&](const GridChange& c) { chosen = c; });
    ui::Action* follow = nullptr;
    for (auto& a : grid.actions())
        if (a->text == "Automation follows clips") follow = a.get();
    CHECK(follow && follow->checkable && !follow->checked);
    if (follow) ui::Menu::activate(follow);
    CHECK(chosen.what == GridChange::Follow && chosen.snap);
}

// --- the terminal UI ---
using tui::Front;
using tui::Key;

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

void settle() {
    for (int i = 0; i < 6; i++) {
        ui::App::get().advanceTimers(0.05);
        ui::App::get().tick();
    }
}

void testTui() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    tui::Screen screen(160, 48);
    auto press = [&](std::vector<Key> ks) {
        for (auto& k : ks) {
            f.key(k);
            settle();
        }
        f.draw(screen);
    };
    // Edit → Automation follows clips: on, ticked; switched off and on from the menu
    ui::Menu* edit = nullptr;
    for (auto& m : w->menuBar->menus())
        if (m->title == "&Edit") edit = m.get();
    ui::Action* follows = nullptr;
    if (edit)
        for (auto& a : edit->actions())
            if (a->text == "&Automation follows clips") follows = a.get();
    CHECK(follows && follows->checkable);
    if (!follows) return;
    if (edit->aboutToShow) edit->aboutToShow(edit);
    CHECK(follows->checked);
    ui::Menu::activate(follows);
    CHECK(!w->project.timing.automationFollows);
    ui::Menu::activate(follows);
    CHECK(w->project.timing.automationFollows);

    // a track with a clip at 0…2 s and a Volume curve under it
    TrackPtr t = w->project.addTrack(Kind::Audio);
    TrackPtr t2 = w->project.addTrack(Kind::Audio);
    auto clip = audioClip(w->project, t, 0, 2);
    auto vol = addLane(t, t->controls, automation::VOLUME, {{0, 0.2}, {1, 0.8}, {2, 0.2}, {6, 0.2}});
    std::vector<AutoPoint> orig = vol->points.copy();
    w->timeline->lanesChanged();
    f.pane = Front::Pane::Timeline;
    f.setCurrent(t);
    f.draw(screen);
    // the clip moved a bar later (Enter selects it, '.' moves it a grid step: repeated to 2 s)
    press({key(plat::Key_Home), key(plat::Key_Return)});
    CHECK(w->timeline->selection.contains(clip));
    int guard = 0;
    while (clip->start < sec(2) && guard++ < 64) press({key('.', 0, ".")});
    CHECK(clip->start == sec(2));
    for (double s : {0.3, 1.0, 1.7}) CHECK_NEAR(val(vol, s + 2), automation::valueAt(orig, s), 1e-6);
    // ']' : to the track below: its Volume comes along
    press({key(']', 0, "]")});
    CHECK(clip->track == t2 && laneOf(t2, automation::VOLUME));
    if (auto v2 = laneOf(t2, automation::VOLUME)) CHECK_NEAR(val(v2, 3), 0.8, 1e-6);
    // the automation lane: I … O a range, Ctrl+C, the cursor on, Ctrl+V; Del clears the range
    vol->setPoints(orig);
    f.setCurrent(t);
    int row = -1;
    auto ls = w->timeline->lanes();  // (the TUI's rows: these, the master's left out)
    for (int i = 0; i < (int)ls.size(); i++)
        if (ls[(size_t)i].automation == vol) row = i;
    CHECK(row >= 0);
    f.selectLane(row);
    press({key(plat::Key_Home), key('I', 0, "i")});
    w->timeline->setCursor(sec(2));
    press({key('O', 0, "o")});
    CHECK(w->timeline->selection.range && w->timeline->selection.range->end == sec(2));
    CHECK(w->timeline->currentAutomation == vol);
    press({key('C', plat::ModCtrl)});
    CHECK(clipboard::heldCurve && std::fabs(clipboard::heldCurve->length - 2) < 1e-9);
    w->timeline->setCursor(sec(3));
    press({key('V', plat::ModCtrl)});
    for (double s : {0.3, 1.0, 1.7}) CHECK_NEAR(val(vol, s + 3), automation::valueAt(orig, s), 1e-6);
    press({key(plat::Key_Delete)});
    CHECK_NEAR(val(vol, 1), 0.2, 1e-9);  // (a straight line from 0.2 to 0.2)
    CHECK_NEAR(val(vol, 4), automation::valueAt(orig, 1), 1e-6);  // (the pasted part stays)
    // F1 lists the keys
    press({key(plat::Key_F1)});
    CHECK(screen.lines().find("a range of its curve") != std::string::npos);
    press({key(plat::Key_Escape)});
    delete w;
    app.paintWindows = true;
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testCurves();
    testMove(true);
    testMove(false);
    testClipboard();
    testLaneRange();
    testDuplicateStress();
    testDuringPlayback();
    testWithTrackSelection();
    testTime();
    testSetting();
    testTui();
    TEST_MAIN_END();
}
