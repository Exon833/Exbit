// Writing automation (automation_write.h) and curved segments (AutoPoint::bend): valueAt with bends
// (none: the old values exactly; monotonic; exact ends), splitting a curve, thinning, a touch written
// through the recorder (only its range, thinned, one undo step, looping), the audio thread leaving a
// touched lane alone, a plugin window's edits grouped into one touch, a modulated parameter's base,
// shapes, the project file (bends saved only when there are some), render == playback with bent
// automation, and the GUI: the Write switch on a lane's row, arming a track's lanes from its menu, the
// fader writing its lane, Ctrl+drag bending a segment, the lane's menu (curve, shapes). Paints into
// build/ui-samples/automation_write/.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include "audio/automation.h"
#include "audio/automation_write.h"
#include "audio/engine.h"
#include "audio/patch.h"
#include "audio/render.h"
#include "check.h"
#include "config.h"
#include "core/json.h"
#include "core/util.h"
#include "effects/effects.h"
#include "gfx/image.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"
#include "plugins/plugin.h"
#include "ui/lane_panel.h"
#include "ui/master_strip.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/widgets.h"

namespace {

const int SR = 48000;
using automation::valueAt;

std::string outDir() {
    std::string d = "build/ui-samples/automation_write";
    util::makeDirs(d);
    return d;
}

// the curve's value as it was before bends (straight lines between points)
double straightValueAt(const std::vector<AutoPoint>& points, double seconds) {
    if (points.empty()) return 0;
    size_t i = std::upper_bound(points.begin(), points.end(), seconds,
                                [](double s, const AutoPoint& p) { return s < p.t; }) - points.begin();
    if (i == 0) return points[0].v;
    if (i == points.size()) return points[i - 1].v;
    const AutoPoint &a = points[i - 1], &b = points[i];
    return b.t > a.t ? a.v + (b.v - a.v) * (seconds - a.t) / (b.t - a.t) : b.v;
}

SlotPtr builtinSlot(const char* id) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->instance = fx;
    return s;
}

AutomationPtr laneOf(const SlotPtr& slot, const std::string& param, std::vector<AutoPoint> points) {
    auto lane = std::make_shared<Automation>();
    lane->slot = slot;
    lane->param = param;
    lane->setPoints(std::move(points));
    return lane;
}

// --- curves ---------------------------------------------------------------------------------------

void testBends() {
    // no bend: exactly the values of straight lines, as before
    std::vector<AutoPoint> pts{{0, 0.2}, {1, 0.9}, {3, 0.4}, {4.5, 0.4}, {6, 1}};
    int differ = 0;
    for (int k = -10; k <= 700; ++k) {
        double s = k * 0.01;
        if (valueAt(pts, s) != straightValueAt(pts, s)) ++differ;
    }
    CHECK(differ == 0);
    for (double r : {0.3, -0.5, 0.0})
        for (double x : {0.0, 0.25, 0.5, 1.0}) CHECK(automation::bendShape(x, 0.0, r) == x);
    // bent: monotonic, within the ends, the ends exact; > 0 bows up, < 0 down, whichever way it goes
    for (double bend : {-1.0, -0.7, -0.3, -0.01, 0.01, 0.3, 0.7, 1.0}) {
        for (bool rising : {true, false}) {
            double v0 = rising ? 0.2 : 0.9, v1 = rising ? 0.9 : 0.1;
            std::vector<AutoPoint> seg{{1, v0, bend}, {3, v1}};
            CHECK(valueAt(seg, 1.0) == v0);
            CHECK(valueAt(seg, 3.0) == v1);
            CHECK(valueAt(seg, 0.5) == v0 && valueAt(seg, 4.0) == v1);
            bool monotonic = true, inside = true;
            double prev = v0;
            for (int k = 1; k <= 2000; ++k) {
                double v = valueAt(seg, 1 + 2.0 * k / 2000);
                if (rising ? v < prev - 1e-15 : v > prev + 1e-15) monotonic = false;
                if (v < std::min(v0, v1) - 1e-12 || v > std::max(v0, v1) + 1e-12) inside = false;
                prev = v;
            }
            CHECK(monotonic);
            CHECK(inside);
            CHECK_NEAR(valueAt(seg, 3.0 - 1e-9), v1, 1e-6);  // (continuous into the end)
            double mid = valueAt(seg, 2.0), line = (v0 + v1) / 2;
            CHECK(bend > 0 ? mid > line : mid < line);
        }
    }
    // a full bend curves strongly; a flat segment stays flat
    std::vector<AutoPoint> strong{{0, 0, 1.0}, {1, 1}};
    CHECK(valueAt(strong, 0.5) > 0.9);
    std::vector<AutoPoint> flat{{0, 0.4, 1.0}, {1, 0.4}};
    CHECK(valueAt(flat, 0.5) == 0.4);
    // a stepped parameter ignores bends
    std::vector<AutoPoint> stepped{{0, 0, 0.8}, {1, 1, -0.5}, {2, 0}};
    std::vector<AutoPoint> plain{{0, 0}, {1, 1}, {2, 0}};
    for (double s : {0.3, 0.99, 1.5, 2.5}) CHECK(valueAt(stepped, s, 2) == valueAt(plain, s, 2));
    // splitting keeps the curve (each half its share of the bend)
    std::vector<AutoPoint> bent{{0, 0.1, 0.8}, {2, 0.9, -0.6}, {5, 0.3}};
    std::vector<AutoPoint> split = bent;
    automation::splitAt(split, 0.7);
    automation::splitAt(split, 3.1);
    automation::splitAt(split, 2.0);  // (a point there: nothing)
    automation::splitAt(split, 9.0);  // (after the last: nothing)
    CHECK(split.size() == 5);
    double worst = 0;
    for (int k = 0; k <= 600; ++k) worst = std::max(worst, std::fabs(valueAt(split, k * 0.01) - valueAt(bent, k * 0.01)));
    CHECK(worst < 1e-12);
    // setPoints keeps bends within -1…1; == sees bends
    Automation a;
    a.setPoints({{0, 0.5, 3.0}, {1, 0.5, -7.0}, {2, 0.1, NAN}});
    CHECK(a.points[0].bend == 1.0 && a.points[1].bend == -1.0 && a.points[2].bend == 0.0);
    AutoPoint p1{1, 0.5}, p2{1, 0.5, 0.25};
    CHECK(p1 != p2 && p1.bend == 0);
}

void testThinAndOverwrite() {
    std::vector<AutoPoint> line;
    for (int k = 0; k <= 100; ++k) line.push_back({k * 0.01, 0.2 + 0.5 * k / 100});
    auto thinned = automation::thin(line);
    CHECK(thinned.size() == 2 && thinned.front() == line.front() && thinned.back() == line.back());
    std::vector<AutoPoint> vee;
    for (int k = 0; k <= 100; ++k) vee.push_back({k * 0.01, std::fabs(k - 50) / 50.0});
    auto v = automation::thin(vee);
    CHECK(v.size() == 3 && v[1].t == vee[50].t);
    std::vector<AutoPoint> wobble{{0, 0.5}, {1, 0.501}, {2, 0.5}, {3, 0.7}};  // (within the tolerance: goes)
    CHECK(automation::thin(wobble).size() == 3);
    // overwrite: the run replaces its time; outside, the curve is as it was
    std::vector<AutoPoint> orig{{0, 0, 0.5}, {10, 1}};
    std::vector<AutoPoint> run{{2, 0.9}, {3, 0.9}, {4, 0.1}};
    auto out = automation::overwrite(orig, run);
    for (double s : {0.0, 1.0, 1.9, 1.998, 4.002, 6.0, 9.9, 12.0}) CHECK_NEAR(valueAt(out, s), valueAt(orig, s), 1e-12);
    CHECK_NEAR(valueAt(out, 2.5), 0.9, 1e-12);
    CHECK_NEAR(valueAt(out, 3.5), 0.5, 1e-12);
    bool ordered = true;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].t < out[i - 1].t) ordered = false;
    CHECK(ordered);
    CHECK(automation::overwrite({}, run) == run);
    CHECK(automation::overwrite(orig, {}) == orig);
    // at the very start: no point before it
    auto atZero = automation::overwrite(orig, {{0, 0.7}, {1, 0.7}});
    CHECK(atZero.front().t == 0 && atZero.front().v == 0.7);
}

// --- the recorder ---------------------------------------------------------------------------------

void testTouch() {
    Project p;
    auto track = p.addTrack(Kind::Audio);
    auto lane = laneOf(track->controls, automation::VOLUME, {{0, 0.2, 0.4}, {8, 1.0}});
    track->automation = std::vector<AutomationPtr>{lane};
    const std::vector<AutoPoint> before = lane->points.copy();
    CHECK(!automation::armedControlLane(*track, automation::VOLUME));
    lane->armed = true;
    CHECK(automation::armedControlLane(*track, automation::VOLUME) == lane);
    CHECK(!automation::armedControlLane(*track, automation::PAN));
    history::History h(p, SR);
    automation::Recorder rec;
    auto step = [&] {  // (the main window: no step while a touch goes on)
        if (!rec.busy()) h.check(SR);
    };
    rec.grabAt(lane, 2.0, 0.3, 0);
    step();
    CHECK(rec.busy() && rec.touching(*lane) && lane->touched.load());
    // the mixer plays it at the hand's value meanwhile
    double db = 0, pan = 0;
    automation::mixerValues(*track, true, SR * 5, SR, db, pan);
    CHECK_NEAR(db, automation::volumeOf(0.3), 1e-9);
    for (int k = 1; k <= 100; ++k) {
        rec.moveAt(lane, 2.0 + k * 0.02, 0.3 + 0.4 * k / 100, k * 0.01);
        step();
        if (k == 50) CHECK_NEAR(valueAt(*lane->points, 3.0), 0.5, 1e-9);  // (shown as it is written)
    }
    automation::mixerValues(*track, true, SR * 5, SR, db, pan);
    CHECK_NEAR(db, automation::volumeOf(0.7), 1e-9);
    CHECK(h.undoSteps() == 0);
    rec.releaseAt(lane);
    step();
    CHECK(!rec.busy() && !lane->touched.load());
    CHECK(h.undoSteps() == 1);  // one touch, one step
    const auto& pts = *lane->points;
    int inside = 0;
    bool outsideKept = true;
    for (auto& q : pts) {
        if (q.t >= 2.0 && q.t <= 4.0) ++inside;
        else if (q.t < 2.0 - automation::JUMP_S - 1e-12 || q.t > 4.0 + automation::JUMP_S + 1e-12)
            outsideKept = outsideKept && std::fabs(q.v - valueAt(before, q.t)) < 1e-12;
    }
    CHECK(inside == 2);  // (a straight movement: thinned to its ends)
    CHECK(outsideKept);
    for (double s : {0.0, 1.0, 1.99, 4.01, 6.0, 8.0, 9.0}) CHECK_NEAR(valueAt(pts, s), valueAt(before, s), 1e-9);
    CHECK_NEAR(valueAt(pts, 3.0), 0.5, 1e-9);
    // after the touch the mixer plays the curve again
    automation::mixerValues(*track, true, SR * 3, SR, db, pan);
    CHECK_NEAR(db, automation::volumeOf(0.5), 1e-9);
    // undo: the curve as it was
    CHECK(h.undo(SR));
    CHECK(lane->points.copy() == before);
    CHECK(h.redo(SR));
    CHECK_NEAR(valueAt(*lane->points, 3.0), 0.5, 1e-9);
    // the song loops back during a touch: both passes are written
    auto pan2 = laneOf(track->controls, automation::PAN, {});
    pan2->armed = true;
    rec.grabAt(pan2, 3.0, 0.1, 0);
    rec.moveAt(pan2, 3.5, 0.1, 0.1);
    rec.moveAt(pan2, 1.0, 0.9, 0.2);  // (looped)
    rec.moveAt(pan2, 1.5, 0.9, 0.3);
    rec.moveAt(pan2, 1.5, 0.8, 0.31);  // (the same moment heard: its latest value)
    rec.releaseAt(pan2);
    CHECK_NEAR(valueAt(*pan2->points, 3.2), 0.1, 1e-9);
    CHECK_NEAR(valueAt(*pan2->points, 1.5), 0.8, 1e-9);
    // stopping ends every touch
    rec.moveAt(lane, 5.0, 0.0, 1.0);
    CHECK(rec.busy());
    rec.tickAt(1.01, false);
    CHECK(!rec.busy() && !lane->touched.load());
    // releasing a lane not touched, or none: nothing
    rec.releaseAt(lane);
    rec.releaseAt(nullptr);
    rec.grabAt(nullptr, 1, 1, 0);
    CHECK(!rec.busy());
    // arm and disarm a track's lanes
    automation::armAll(*track, false);
    CHECK(!automation::anyArmed(*track));
    automation::armAll(*track, true);
    CHECK(automation::anyArmed(*track) && lane->armed);
    // the recorder's clock: nothing is written while stopped (no song time)
    automation::Recorder idle;
    idle.move(lane, 0.5);
    idle.grab(lane, 0.5);
    CHECK(!idle.busy());
    double now = 7.0;
    idle.songTime = [&](double& s) {
        s = now;
        return true;
    };
    idle.grab(lane, 0.25);
    now = 7.5;
    idle.move(lane, 0.75);
    idle.tick();  // (held: stays)
    CHECK(idle.busy());
    idle.release(lane);
    CHECK(!idle.busy());
    CHECK_NEAR(valueAt(*lane->points, 7.25), 0.5, 1e-9);
}

void testAudioThreadLeavesTouched() {
    Project p;
    auto track = p.addTrack(Kind::Audio);
    auto slot = builtinSlot("utility");
    track->effects = std::vector<SlotPtr>{slot};
    track->cables = chain({slot});
    auto lane = laneOf(slot, "Gain", {{0, 0}, {1, 1}});
    track->automation = std::vector<AutomationPtr>{lane};
    automation::resolve(p);
    CHECK(lane->hasTarget);
    auto fx = aload(slot->instance);
    automation::apply(*track, SR / 2, SR);
    CHECK_NEAR(fx->getParam(0), 0.5, 1e-6);
    lane->armed = true;
    automation::Recorder rec;
    rec.grabAt(lane, 0.5, 0.8, 0);
    fx->setParam(0, 0.8);  // (the user turns it)
    automation::apply(*track, SR * 3 / 4, SR);
    CHECK_NEAR(fx->getParam(0), 0.8, 1e-6);  // not pulled back to the curve (0.75)
    automation::apply(*track, SR * 9 / 10, SR);
    CHECK_NEAR(fx->getParam(0), 0.8, 1e-6);
    rec.moveAt(lane, 0.7, 0.8, 0.1);
    rec.releaseAt(lane);
    automation::apply(*track, SR * 3 / 5, SR);  // (let go: the curve sets it again)
    CHECK_NEAR(fx->getParam(0), 0.8, 1e-6);
    automation::apply(*track, SR / 4, SR);
    CHECK_NEAR(fx->getParam(0), 0.25, 1e-6);
    // a modulated parameter: its base is what is written (the modulation moves the parameter around it)
    ModCable cable;
    cable.from = nullptr;
    cable.to = slot.get();
    cable.param = "Gain";
    cable.state->base.store(0.3);
    track->modCables = std::vector<ModCable>{cable};
    CHECK_NEAR(automation::writtenValue(p, slot.get(), 0, "Gain"), 0.3, 1e-12);
    track->modCables = std::vector<ModCable>{};
    CHECK_NEAR(automation::writtenValue(p, slot.get(), 0, "Gain"), fx->getParam(0), 1e-12);
    CHECK(automation::writtenValue(p, nullptr, 0, "Gain") < 0);
}

void testWindowEditsGrouped() {
    Project p;
    auto track = p.addTrack(Kind::Audio);
    auto slot = builtinSlot("utility");
    track->effects = std::vector<SlotPtr>{slot};
    track->cables = chain({slot});
    auto lane = laneOf(slot, "Gain", {});
    track->automation = std::vector<AutomationPtr>{lane};
    lane->armed = true;
    CHECK(automation::armedLane(p, slot.get(), "Gain") == lane);
    CHECK(!automation::armedLane(p, slot.get(), "Width"));
    history::History h(p, SR);
    automation::Recorder rec;
    // a run of edits (no grab, no let go): one touch, ending TOUCH_IDLE_S after the last
    rec.moveAt(lane, 1.0, 0.2, 10.0);
    rec.moveAt(lane, 1.15, 0.3, 10.15);
    rec.moveAt(lane, 1.3, 0.4, 10.3);
    rec.tickAt(10.6, true);
    CHECK(rec.busy());
    if (!rec.busy()) h.check(SR);
    rec.tickAt(10.3 + automation::TOUCH_IDLE_S + 0.01, true);
    CHECK(!rec.busy());
    if (!rec.busy()) h.check(SR);
    CHECK(h.undoSteps() == 1);
    CHECK_NEAR(valueAt(*lane->points, 1.15), 0.3, 1e-9);
    // a later run: another touch
    rec.moveAt(lane, 3.0, 0.9, 12.0);
    rec.tickAt(12.5, true);
    h.check(SR);
    CHECK(h.undoSteps() == 2);
    // through the plugin's window: TweakWatcher (the global recorder, with a song time)
    double now = 5.0;
    automation::recorder().songTime = [&](double& s) {
        s = now;
        return true;
    };
    auto fx = aload(slot->instance);
    automation::TweakWatcher w(slot, p);
    fx->windowEdit(0, 0.6);
    w.poll();
    CHECK(automation::recorder().touching(*lane));
    now = 5.2;
    fx->windowEdit(0, 0.65);
    fx->windowEdit(1, 0.1);  // (Width: not armed, not written)
    w.poll();
    CHECK_NEAR(valueAt(*lane->points, 5.2), 0.65, 1e-9);
    automation::recorder().tickAt(automation::wallClock() + 1, true);
    CHECK(!automation::recorder().busy());
    CHECK_NEAR(valueAt(*lane->points, 5.0), 0.6, 1e-9);
    // a built-in effect's slider held: grab … let go
    now = 6.0;
    w.grab(0);
    CHECK(automation::recorder().busy());
    fx->windowEdit(0, 0.1);
    now = 6.5;
    w.writeEdits({0});
    w.letGo(0);
    CHECK(!automation::recorder().busy());
    CHECK_NEAR(valueAt(*lane->points, 6.5), 0.1, 1e-9);
    // not armed: nothing written
    lane->armed = false;
    auto was = lane->points.copy();
    now = 7.0;
    fx->windowEdit(0, 0.9);
    w.poll();
    CHECK(!automation::recorder().busy() && lane->points.copy() == was);
    // stopped: nothing either
    lane->armed = true;
    automation::recorder().songTime = [](double&) { return false; };
    fx->windowEdit(0, 0.4);
    w.poll();
    CHECK(!automation::recorder().busy());
    automation::recorder().songTime = nullptr;
}

void testShapes() {
    Timing t;  // 120 BPM, 4/4: a beat is 0.5 s, a bar 2 s
    using automation::Shape;
    auto square = automation::drawShape({}, 2, 6, Shape::Square, t, true);
    CHECK_NEAR(valueAt(square, 2.5), 1, 1e-9);
    CHECK_NEAR(valueAt(square, 3.5), 0, 1e-9);
    CHECK_NEAR(valueAt(square, 4.5), 1, 1e-9);
    CHECK_NEAR(valueAt(square, 5.5), 0, 1e-9);
    auto sine = automation::drawShape({}, 0, 2, Shape::Sine, t, false);
    CHECK_NEAR(valueAt(sine, 0.125), 1, 1e-9);
    CHECK_NEAR(valueAt(sine, 0.375), 0, 1e-9);
    CHECK_NEAR(valueAt(sine, 1.0), 0.5, 1e-9);
    auto tri = automation::drawShape({}, 0, 2, Shape::Triangle, t, true);
    CHECK_NEAR(valueAt(tri, 1.0), 1, 1e-9);
    CHECK_NEAR(valueAt(tri, 0.5), 0.5, 1e-9);
    CHECK_NEAR(valueAt(tri, 2.0), 0, 1e-9);
    auto saw = automation::drawShape({}, 0, 1, Shape::Saw, t, false);
    CHECK_NEAR(valueAt(saw, 0.25), 0.25 / (0.5 - automation::JUMP_S), 1e-9);
    CHECK_NEAR(valueAt(saw, 0.6), 0.1 / (0.5 - automation::JUMP_S), 1e-9);
    // ramps, over a range of an existing curve: the rest stays
    std::vector<AutoPoint> flat{{0, 0.5}, {8, 0.5}};
    auto up = automation::drawShape(flat, 1, 3, Shape::RampUp, t, false);
    CHECK_NEAR(valueAt(up, 2), 0.5, 1e-9);
    CHECK_NEAR(valueAt(up, 0.5), 0.5, 1e-9);
    CHECK_NEAR(valueAt(up, 3.5), 0.5, 1e-9);
    CHECK_NEAR(valueAt(up, 1.0), 0.0, 1e-9);
    auto down = automation::drawShape({}, 0, 4, Shape::RampDown, t, false);
    CHECK_NEAR(valueAt(down, 1), 0.75, 1e-9);
    // an empty range: nothing changes
    CHECK(automation::drawShape(flat, 3, 3, Shape::Sine, t, false) == flat);
    // a tempo change: the cycles follow the beats
    Timing slow;
    slow.setChange(2, 60, 4);  // (from bar 2, at 2 s: 60 BPM)
    slow.publish();
    auto slowSquare = automation::drawShape({}, 2, 6, Shape::Square, slow, false);  // (a beat: 1 s)
    CHECK_NEAR(valueAt(slowSquare, 2.25), 1, 1e-9);
    CHECK_NEAR(valueAt(slowSquare, 2.75), 0, 1e-9);
    CHECK_NEAR(valueAt(slowSquare, 5.25), 1, 1e-9);
    CHECK(std::string(automation::shapeName(Shape::Saw)) == "Saw");
}

// --- the project file -----------------------------------------------------------------------------

void testProjectFile() {
    Project p;
    auto track = p.addTrack(Kind::Audio);
    auto vol = laneOf(track->controls, automation::VOLUME, {{0, 0.2, 0.5}, {1, 0.9, -0.25}, {2, 0.4}});
    auto pan = laneOf(track->controls, automation::PAN, {{0, 0.5}, {3, 0.1}});
    track->automation = std::vector<AutomationPtr>{vol, pan};
    Json data = project_io::describe(p, SR, project_io::MasterSettings{});
    const Json& saved = data;
    const Json& lanes = saved["tracks"][size_t(0)]["automation"];
    CHECK(lanes.size() == 2);
    const Json& vp = lanes[size_t(0)]["points"];
    CHECK(vp[size_t(0)].size() == 3 && vp[size_t(1)].size() == 3 && vp[size_t(2)].size() == 2);
    CHECK(vp[size_t(0)][size_t(2)].toDouble() == 0.5);
    const Json& pp = lanes[size_t(1)]["points"];
    CHECK(pp[size_t(0)].size() == 2 && pp[size_t(1)].size() == 2);  // (no bends: as older saves)
    std::string dir = "build/test-auto-write";
    util::makeDirs(dir);
    std::string path = dir + "/bends.json", error;
    CHECK(project_io::writeJson(path, data, error));
    Project q;
    project_io::MasterSettings m;
    CHECK(project_io::loadProject(path, q, SR, m, error));
    CHECK(q.tracks.size() == 1 && q.tracks[0]->automation.size() == 2);
    if (q.tracks.size() == 1 && q.tracks[0]->automation.size() == 2) {
        CHECK(q.tracks[0]->automation[0]->points == vol->points);
        CHECK(q.tracks[0]->automation[1]->points == pan->points);
    }
    // an older file (points of two numbers) loads straight; a bend out of range is kept within -1…1; a
    // third item that isn't a number is no bend
    std::string old = dir + "/old.json";
    vol->setPoints({{0, 0.5}, {1, 0.25}, {2, 0.75}});
    track->automation = std::vector<AutomationPtr>{vol};
    std::string text = project_io::describe(p, SR, project_io::MasterSettings{}).dump();
    const std::pair<std::string, std::string> swaps[] = {{"[1.0, 0.25]", "[1.0, 0.25, 4]"}, {"[2.0, 0.75]", "[2.0, 0.75, \"x\"]"}};
    for (auto& [from, to] : swaps) {
        size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) text.replace(at, from.size(), to);
    }
    Json oldData = Json::parse(text, &error);
    CHECK(project_io::writeJson(old, oldData, error));
    Project r;
    CHECK(project_io::loadProject(old, r, SR, m, error));
    CHECK(r.tracks.size() == 1 && r.tracks[0]->automation.size() == 1);
    if (r.tracks.size() == 1 && r.tracks[0]->automation.size() == 1) {
        auto& pts = *r.tracks[0]->automation[0]->points;
        CHECK(pts.size() == 3 && pts[0].bend == 0 && pts[1].bend == 1.0 && pts[2].bend == 0);
    }
}

// --- render == playback with curved automation ----------------------------------------------------

void testRenderEqualsPlayback() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    auto a = p.addTrack(Kind::Audio);
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.resize((size_t)SR * 4 * 2);
    for (int64_t i = 0; i < SR * 4; i++) {
        buf->samples[(size_t)i * 2] = (float)(0.3 * std::sin(2 * M_PI * 220 * i / SR));
        buf->samples[(size_t)i * 2 + 1] = (float)(0.3 * std::sin(2 * M_PI * 330 * i / SR));
    }
    auto clip = makeAudioClip("tone", buf, SR, SR);
    clip->track = a;
    clip->start = 0;
    clip->prepare(SR);
    p.addClip(clip);
    auto utility = builtinSlot("utility");
    a->effects = std::vector<SlotPtr>{utility};
    a->cables = chain({utility});
    auto vol = laneOf(a->controls, automation::VOLUME, {{0.0, 50.0 / 66, 0.8}, {1.5, 62.0 / 66, -0.6}, {3.5, 40.0 / 66}});
    auto pan = laneOf(a->controls, automation::PAN, {{0.5, 0.1, -1.0}, {3.0, 0.9}});
    auto gain = laneOf(utility, "Gain", {{0.0, 0.4, 0.7}, {2.5, 0.6, -0.9}, {3.5, 0.45}});
    a->automation = std::vector<AutomationPtr>{vol, pan, gain};
    patch::ensureStates(p);
    automation::resolve(p);
    for (auto& s : p.pluginSlots()) aload(s->instance)->reset();
    const int block = config::RENDER_BLOCK;
    const int64_t start = SR * 2, end = SR * 4;  // (renderMix starts 2 s before: at 0)
    e.seek(0);
    e.play();
    std::vector<float> played, out((size_t)block * 2);
    while ((int64_t)played.size() / 2 < end + SR) {
        e.callback(nullptr, out.data(), block);
        played.insert(played.end(), out.begin(), out.end());
    }
    e.stop();
    int latency = e.latency;
    for (auto& s : p.pluginSlots()) aload(s->instance)->reset();
    auto rendered = render::renderMix(p, start, end, SR);
    CHECK(rendered && (int64_t)rendered->size() == (end - start) * 2);
    if (!rendered) return;
    int64_t differ = 0;
    double loudest = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++) {
        float x = (*rendered)[(size_t)j], y = played[(size_t)((start + latency) * 2 + j)];
        if (x != y) differ++;
        loudest = std::max(loudest, (double)std::fabs(x));
    }
    CHECK(differ == 0);
    CHECK(loudest > 0.05);
    if (differ) printf("render vs playback (bent automation): %lld samples differ\n", (long long)differ);
}

// --- the GUI --------------------------------------------------------------------------------------

template <class T>
std::vector<T*> findAll(ui::Widget* root) {
    std::vector<T*> out;
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            if (auto* t = dynamic_cast<T*>(c.get())) out.push_back(t);
            walk(c.get());
        }
    };
    walk(root);
    return out;
}

ui::Button* buttonText(ui::Widget* root, const std::string& text) {
    for (auto* b : findAll<ui::Button>(root))
        if (b->text() == text) return b;
    return nullptr;
}

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

Image paint(ui::Widget* w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    w->paintTree(p, w->rect());
    return img;
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

void testPanel() {
    Project project;
    audio::Engine engine{project};
    auto track = project.addTrack(Kind::Audio);
    track->name = "Keys";
    auto vol = laneOf(track->controls, automation::VOLUME, {{0.0, 0.8}, {10.0, 0.2}});
    auto pan = laneOf(track->controls, automation::PAN, {});
    track->automation = std::vector<AutomationPtr>{vol, pan};
    track->showAutomation = true;
    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1000, 700);
    auto* scroll = win.add<ui::ScrollArea>();
    scroll->setGeometry(0, 0, 700, 700);
    auto* timeline = scroll->makeContent<Timeline>(project, engine, scroll);
    timeline->relayout();
    MasterStrip master(engine, [] {});
    auto* panel = win.add<LanePanel>(project, *timeline, master, engine.channels, [](const TrackPtr&) {});
    panel->setGeometry(700, 0, config::PANEL_W, 700);
    panel->rebuild();
    win.showWindow();
    ui::App::get().doLayouts();
    CHECK(panel->automationFrames.count(vol) && panel->automationFrames.count(pan));
    if (!panel->automationFrames.count(vol)) return;
    LaneFrame* row = panel->automationFrames[vol];
    // the Write switch on the lane's row, with a tooltip; on: armed, the row tinted red
    ui::Button* write = buttonText(row, "Write");
    CHECK(write != nullptr);
    if (!write) return;
    CHECK(write->isCheckable() && !write->isChecked() && write->isVisible());
    CHECK(write->toolTip().find("Write") == 0 && write->toolTip().find("Volume") != std::string::npos);
    Color plain = row->color();
    CHECK(plain == theme::c("automation_lane"));
    write->setChecked(true);
    CHECK(vol->armed && !pan->armed);
    CHECK(row->color() != plain && row->color() == lane_panel::automationRowColor(*vol));
    CHECK(row->color().r > plain.r || row->color().g < plain.g);  // (towards red)
    savePng(outDir() + "/panel_write.png", paint(panel));
    // the timeline tints an armed lane red too
    Image tl = paint(timeline);
    auto lanes = timeline->lanes();
    const Lane* volLane = nullptr;
    const Lane* panLane = nullptr;
    for (auto& l : lanes) {
        if (l.automation == vol) volLane = &l;
        if (l.automation == pan) panLane = &l;
    }
    CHECK(volLane && panLane);
    if (volLane && panLane) {
        int redder = 0, x = 600;  // (along a row near the top: the armed lane's pixels are redder)
        for (; x < 1200; ++x) {
            Color armed = tl.colorAt(x, volLane->top + 6), other = tl.colorAt(x, panLane->top + 6);
            if (armed.r - armed.b > other.r - other.b) ++redder;
        }
        CHECK(redder > 500);
    }
    savePng(outDir() + "/timeline_armed.png", tl);
    // the track's right-click menu: arm all its lanes, then disarm them
    write->setChecked(false);
    CHECK(!vol->armed && row->color() == plain);
    {
        ui::Menu menu;
        panel->fillTrackMenu(menu, track);
        ui::Action* arm = actionStarting(menu, "Arm all its automation lanes");
        CHECK(arm && !arm->tooltip.empty());
        if (arm) ui::Menu::activate(arm);
        CHECK(vol->armed && pan->armed);
    }
    ui::App::get().doLayouts();
    {
        ui::Menu menu;
        panel->fillTrackMenu(menu, track);
        ui::Action* disarm = actionStarting(menu, "Disarm its automation lanes");
        CHECK(disarm != nullptr);
        if (disarm) ui::Menu::activate(disarm);
        CHECK(!vol->armed && !pan->armed);
    }
    ui::App::get().doLayouts();
    ui::App::get().collectGarbage();
    // the fader held while playing writes the Volume lane (armed), from grab to let go
    vol->armed = true;
    double now = 2.0;
    automation::recorder().songTime = [&](double& s) {
        s = now;
        return true;
    };
    history::History h(project, SR);
    VolumeSlider* slider = panel->mixers[track].slider;
    auto toValue = [](double db) { return (db - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB); };
    slider->sliderPressed.emit();
    CHECK(automation::recorder().touching(*vol) && vol->touched.load());
    // playback doesn't move the fader under the hand: it shows the hand's value
    now = 2.5;
    slider->setValue(-10);
    panel->showMixerValues(SR * 3, SR);
    CHECK(slider->value() == -10);
    now = 3.0;
    slider->setValue(-20);
    if (!automation::recorder().busy()) h.check(SR);
    slider->sliderReleased.emit();
    CHECK(!automation::recorder().busy() && !vol->touched.load());
    h.check(SR);
    CHECK(h.undoSteps() == 1);
    CHECK_NEAR(valueAt(*vol->points, 3.0), toValue(-20), 1e-9);
    CHECK_NEAR(valueAt(*vol->points, 2.5), toValue(-10), 1e-9);
    CHECK_NEAR(valueAt(*vol->points, 6.0), straightValueAt({{0.0, 0.8}, {10.0, 0.2}}, 6.0), 1e-9);
    // the pan knob held writes the Pan lane
    pan->armed = true;
    PanKnob* knob = panel->mixers[track].pan;
    now = 4.0;
    auto press = mouse(5, 5);
    knob->mousePress(press);
    CHECK(automation::recorder().touching(*pan));
    now = 4.5;
    auto drag = mouse(5 + 50, 5);
    knob->mouseMove(drag);
    knob->mouseRelease(drag);
    CHECK(!automation::recorder().busy());
    CHECK(!pan->points.empty());
    CHECK_NEAR(valueAt(*pan->points, 4.5), (knob->pan + 1) / 2, 1e-9);
    // not armed: moving the fader writes nothing
    vol->armed = false;
    auto was = vol->points.copy();
    slider->sliderPressed.emit();
    slider->setValue(-3);
    slider->sliderReleased.emit();
    CHECK(vol->points.copy() == was);
    automation::recorder().songTime = nullptr;
}

void testTimelineBends() {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
    scroll.resize(900, 420);
    Timeline& tl = *scroll.makeContent<Timeline>(project, engine, &scroll);
    auto midi = project.addTrack(Kind::Midi);
    auto lane = laneOf(std::make_shared<PluginSlot>(), "Cutoff", {{1, 0.2}, {3, 0.8}, {5, 0.3}});
    midi->automation = std::vector<AutomationPtr>{lane};
    midi->showAutomation = true;
    tl.lanesChanged();
    Lane al = tl.lanes()[1];
    CHECK(al.automation == lane);
    CHECK(tl.segmentAt(al, 2 * tl.pps) == 0 && tl.segmentAt(al, 4 * tl.pps) == 1);
    CHECK(tl.segmentAt(al, 0.5 * tl.pps) == -1 && tl.segmentAt(al, 6 * tl.pps) == -1);
    // Ctrl+drag a segment up: it bows up (no point added)
    double x = 2 * tl.pps, y = tl.automationY(al, 0.5);
    auto e = mouse(x, y, plat::ModCtrl);
    tl.mousePress(e);
    CHECK(std::holds_alternative<BendDrag>(tl.drag));
    CHECK(lane->points.size() == 3);
    e = mouse(x, y - 1, plat::ModCtrl);  // (a hand not quite still: still a click)
    tl.mouseMove(e);
    CHECK(lane->points[0].bend == 0);
    e = mouse(x, y - 20, plat::ModCtrl);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(lane->points.size() == 3);
    CHECK(lane->points[0].bend > 0.2 && lane->points[1].bend == 0);
    CHECK(valueAt(*lane->points, 2.0) > 0.5);
    // further up than the lane: a full bend at most
    e = mouse(x, y, plat::ModCtrl);
    tl.mousePress(e);
    e = mouse(x, y - 2000, plat::ModCtrl);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(lane->points[0].bend == 1.0);
    // Ctrl+drag the second segment down
    double x2 = 4 * tl.pps, y2 = tl.automationY(al, 0.55);
    e = mouse(x2, y2, plat::ModCtrl);
    tl.mousePress(e);
    e = mouse(x2, y2 + 30, plat::ModCtrl);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(lane->points[1].bend < -0.2);
    CHECK(valueAt(*lane->points, 4.0) < 0.55);
    savePng(outDir() + "/timeline_bent.png", paint(&scroll));
    // Ctrl+double-click a segment: straight again
    e = mouse(x, y, plat::ModCtrl);
    tl.mousePress(e);
    tl.mouseRelease(e);
    tl.mouseDoubleClick(e);
    CHECK(lane->points[0].bend == 0 && lane->points.size() == 3);
    // without Ctrl a double-click adds a point (Alt: off the grid, as before)
    e = mouse(4.3 * tl.pps, tl.automationY(al, 0.9), plat::ModAlt);
    tl.mousePress(e);
    tl.mouseRelease(e);
    CHECK(lane->points.size() == 3);  // (one click: none)
    tl.mouseDoubleClick(e);
    tl.mouseRelease(e);
    CHECK(lane->points.size() == 4);
    // the lane's menu: curve the segment there, and shapes over the loop
    {
        ui::Menu menu;
        tl.fillAutomationMenu(menu, al, PointF(2 * tl.pps, y));
        ui::Action* curve = actionStarting(menu, "Curve this segment");
        CHECK(curve && curve->submenu);
        if (curve && curve->submenu) {
            ui::Action* up = actionStarting(*curve->submenu, "Bow up strongly");
            CHECK(up && up->enabled && !up->tooltip.empty());
            if (up) ui::Menu::activate(up);
            CHECK(lane->points[0].bend == 1.0);
            ui::Action* straight = actionStarting(*curve->submenu, "Straighten");
            if (straight) ui::Menu::activate(straight);
            CHECK(lane->points[0].bend == 0.0);
        }
        ui::Action* shapes = actionStarting(menu, "Draw a shape");
        CHECK(shapes && shapes->submenu);
        if (shapes && shapes->submenu) {
            ui::Action* sine = actionStarting(*shapes->submenu, "Sine, a cycle per beat");
            CHECK(sine && !sine->enabled);  // (no loop, no selection yet)
        }
    }
    project.timing.loopStart = 6;
    project.timing.loopEnd = 8;
    {
        ui::Menu menu;
        tl.fillAutomationMenu(menu, al, PointF(7 * tl.pps, y));
        CHECK(!actionStarting(menu, "Curve this segment"));  // (after the last point: no segment)
        ui::Action* shapes = actionStarting(menu, "Draw a shape over the loop");
        CHECK(shapes && shapes->submenu);
        if (shapes && shapes->submenu) {
            ui::Action* square = actionStarting(*shapes->submenu, "Square, a cycle per bar");
            CHECK(square && square->enabled && !square->tooltip.empty());
            if (square) ui::Menu::activate(square);
            CHECK_NEAR(valueAt(*lane->points, 6.5), 1, 1e-9);
            CHECK_NEAR(valueAt(*lane->points, 7.5), 0, 1e-9);
            CHECK_NEAR(valueAt(*lane->points, 2.0), 0.5, 1e-9);  // (the rest as it was)
        }
    }
    savePng(outDir() + "/timeline_shape.png", paint(&scroll));
    // a stepped parameter: no curving (its curve holds each value)
    lane->hasTarget = true;
    lane->targetSteps = 2;
    lane->targetPlugin = aload(builtinSlot("utility")->instance);
    CHECK(automation::stepsOf(*lane) == 2);
    e = mouse(x, y, plat::ModCtrl);
    tl.mousePress(e);
    CHECK(!std::holds_alternative<BendDrag>(tl.drag));
    tl.mouseRelease(e);
}

}  // namespace

int main() {
    ui::App::get().initHeadless();
    theme::setSkin();
    testBends();
    testThinAndOverwrite();
    testTouch();
    testAudioThreadLeavesTouched();
    testWindowEditsGrouped();
    testShapes();
    testProjectFile();
    testRenderEqualsPlayback();
    testPanel();
    testTimelineBends();
    TEST_MAIN_END();
}
