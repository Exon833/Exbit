#include "audio/automation_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

#include "audio/automation.h"
#include "plugins/plugin.h"

namespace automation {

namespace {
constexpr double LOOPED_BACK_S = 0.05;  // the song time going back further than this during a touch: it looped
constexpr int MAX_CYCLES = 20000;       // (a shape over hours, a cycle per beat: enough)

double clamp01(double v) { return std::isfinite(v) ? std::min(1.0, std::max(0.0, v)) : 0.0; }
}  // namespace

// --- armed lanes ---------------------------------------------------------------------------------

AutomationPtr armedControlLane(const Track& track, const std::string& param) {
    for (auto& lane : track.automation)
        if (lane->armed && lane->slot == track.controls && lane->param == param) return lane;
    return nullptr;
}

AutomationPtr armedLane(const Project& project, const PluginSlot* slot, const std::string& param) {
    if (!slot) return nullptr;
    for (auto& track : project.allTracks())
        for (auto& lane : track->automation)
            if (lane->armed && lane->slot.get() == slot && lane->param == param) return lane;
    return nullptr;
}

double writtenValue(const Project& project, const PluginSlot* slot, uint32_t id, const std::string& param) {
    if (!slot) return -1;
    for (auto& track : project.allTracks())  // (modulated: the base, which the user moves: modulation::userEdited)
        for (auto& cable : track->modCables)
            if (cable.to == slot && cable.param == param && cable.state) {
                double base = cable.state->base.load(std::memory_order_relaxed);
                if (base >= 0) return base;
            }
    auto instance = std::atomic_load(&slot->instance);
    if (!instance) return -1;
    try {
        return instance->getParam(id);
    } catch (...) {
        return -1;
    }
}

void armAll(Track& track, bool on) {
    for (auto& lane : track.automation) lane->armed = on;
}

bool anyArmed(const Track& track) {
    for (auto& lane : track.automation)
        if (lane->armed) return true;
    return false;
}

// --- thinning and writing a run into a curve -------------------------------------------------------

std::vector<AutoPoint> thin(const std::vector<AutoPoint>& points, double tolerance) {
    const size_t n = points.size();
    if (n <= 2) return points;
    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> todo{{0, n - 1}};
    while (!todo.empty()) {
        auto [i, j] = todo.back();
        todo.pop_back();
        if (j <= i + 1) continue;
        const AutoPoint &a = points[i], &b = points[j];
        size_t worst = i;
        double worstErr = -1;
        for (size_t k = i + 1; k < j; ++k) {
            const AutoPoint& p = points[k];
            double err;
            if (b.t > a.t) {
                err = std::fabs(p.v - (a.v + (b.v - a.v) * (p.t - a.t) / (b.t - a.t)));
            } else {  // (a jump: on it if between its two values)
                double lo = std::min(a.v, b.v), hi = std::max(a.v, b.v);
                err = p.v < lo ? lo - p.v : p.v > hi ? p.v - hi : 0;
            }
            if (err > worstErr) {
                worstErr = err;
                worst = k;
            }
        }
        if (worstErr > tolerance) {
            keep[worst] = 1;
            todo.push_back({i, worst});
            todo.push_back({worst, j});
        }
    }
    std::vector<AutoPoint> out;
    for (size_t k = 0; k < n; ++k)
        if (keep[k]) out.push_back(points[k]);
    return out;
}

std::vector<AutoPoint> overwrite(const std::vector<AutoPoint>& points, const std::vector<AutoPoint>& run) {
    if (run.empty()) return points;
    std::vector<AutoPoint> written;
    for (auto& p : run) written.push_back({std::max(0.0, p.t), clamp01(p.v), 0.0});
    if (points.empty()) return written;
    double lead = written.front().t - JUMP_S, tail = written.back().t + JUMP_S;
    std::vector<AutoPoint> pts = points;
    if (lead > 0) splitAt(pts, lead);  // (the curve before the run stays exactly as it was)
    splitAt(pts, tail);
    std::vector<AutoPoint> out;
    for (auto& p : pts)
        if (p.t < lead) out.push_back(p);
    if (lead > 0) out.push_back({lead, valueAt(pts, lead), 0.0});
    out.insert(out.end(), written.begin(), written.end());
    AutoPoint back{tail, valueAt(pts, tail), 0.0};  // (then back to the curve: the point there keeps its bend)
    for (auto& p : pts)
        if (p.t == tail) back = p;
    out.push_back(back);
    for (auto& p : pts)
        if (p.t > tail) out.push_back(p);
    return out;
}

// --- the recorder --------------------------------------------------------------------------------

Recorder& recorder() {
    static Recorder r;
    return r;
}

double wallClock() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Recorder::Touch* Recorder::find(const Automation* lane) {
    for (auto& t : touches_)
        if (t.lane.get() == lane) return &t;
    return nullptr;
}

bool Recorder::touching(const Automation& lane) const {
    for (auto& t : touches_)
        if (t.lane.get() == &lane) return true;
    return false;
}

void Recorder::add(Touch& t, double seconds, double value) {
    value = clamp01(value);
    seconds = std::max(0.0, seconds);
    t.lane->touchValue.store(value, std::memory_order_relaxed);
    if (!t.run.empty()) {
        double last = t.run.back().t;
        if (seconds < last - LOOPED_BACK_S) {  // the song looped: that pass is written, another begins
            t.before = overwrite(t.before, thin(t.run));
            t.run.clear();
        } else if (seconds <= last) {  // (the same moment, as heard: its latest value)
            t.run.back().v = value;
            t.lane->points = overwrite(t.before, t.run);
            ++writes_;
            return;
        }
    }
    t.run.push_back({seconds, value, 0.0});
    t.lane->points = overwrite(t.before, t.run);  // (shown as it is written; thinned at the end)
    ++writes_;
}

void Recorder::grabAt(const AutomationPtr& lane, double seconds, double value, double wall) {
    if (!lane) return;
    Touch* t = find(lane.get());
    if (!t) {
        touches_.push_back(Touch{lane, lane->points.copy(), {}, true, wall});
        t = &touches_.back();
        lane->touchValue.store(clamp01(value), std::memory_order_relaxed);
        lane->touched.store(true, std::memory_order_release);  // (the audio thread leaves the control alone now)
    }
    t->held = true;
    t->lastWall = wall;
    add(*t, seconds, value);
}

void Recorder::moveAt(const AutomationPtr& lane, double seconds, double value, double wall) {
    if (!lane) return;
    Touch* t = find(lane.get());
    if (!t) {
        touches_.push_back(Touch{lane, lane->points.copy(), {}, false, wall});
        t = &touches_.back();
        lane->touchValue.store(clamp01(value), std::memory_order_relaxed);
        lane->touched.store(true, std::memory_order_release);
    }
    t->lastWall = wall;
    add(*t, seconds, value);
}

void Recorder::releaseAt(const AutomationPtr& lane) {
    if (!lane) return;
    for (size_t i = 0; i < touches_.size(); ++i)
        if (touches_[i].lane == lane) {
            end(i);
            return;
        }
}

void Recorder::end(size_t index) {
    Touch t = std::move(touches_[index]);
    touches_.erase(touches_.begin() + (long)index);
    if (!t.run.empty()) t.lane->points = overwrite(t.before, thin(t.run));
    t.lane->touched.store(false, std::memory_order_release);  // (the curve plays it again)
    ++writes_;
}

void Recorder::tickAt(double wall, bool playing) {
    if (!playing) {
        releaseAll();
        return;
    }
    for (size_t i = touches_.size(); i-- > 0;)
        if (!touches_[i].held && wall - touches_[i].lastWall >= TOUCH_IDLE_S) end(i);
}

void Recorder::releaseAll() {
    while (!touches_.empty()) end(touches_.size() - 1);
}

void Recorder::grab(const AutomationPtr& lane, double value) {
    double seconds;
    if (lane && songTime && songTime(seconds)) grabAt(lane, seconds, value, wallClock());
}

void Recorder::move(const AutomationPtr& lane, double value) {
    double seconds;
    if (lane && songTime && songTime(seconds)) moveAt(lane, seconds, value, wallClock());
}

void Recorder::release(const AutomationPtr& lane) { releaseAt(lane); }

void Recorder::tick() {
    if (touches_.empty()) return;
    double seconds;
    tickAt(wallClock(), songTime && songTime(seconds));
}

// --- a plugin's (or built-in effect's) window: TweakWatcher (automation.h) --------------------------

const std::string* TweakWatcher::nameOf(uint32_t id) const {
    for (auto& n : names_)
        if (n.first == id) return &n.second;
    return nullptr;
}

void TweakWatcher::writeEdits(const std::vector<uint32_t>& ids) {
    if (ids.empty() || !slot) return;
    std::set<uint32_t> seen;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {  // (each once: its value now)
        if (!seen.insert(*it).second) continue;
        const std::string* name = nameOf(*it);
        if (!name) continue;
        AutomationPtr lane = armedLane(project_, slot.get(), *name);
        if (!lane) continue;
        double value = writtenValue(project_, slot.get(), *it, *name);
        if (value >= 0) recorder().move(lane, value);
    }
}

void TweakWatcher::grab(uint32_t id) {
    const std::string* name = nameOf(id);
    AutomationPtr lane = name ? armedLane(project_, slot.get(), *name) : nullptr;
    if (!lane) return;
    double value = writtenValue(project_, slot.get(), id, *name);
    if (value >= 0) recorder().grab(lane, value);
}

void TweakWatcher::letGo(uint32_t id) {
    const std::string* name = nameOf(id);
    AutomationPtr lane = name ? armedLane(project_, slot.get(), *name) : nullptr;
    if (lane) recorder().release(lane);
}

// --- shapes --------------------------------------------------------------------------------------

const char* shapeName(Shape shape) {
    switch (shape) {
        case Shape::RampUp: return "Ramp up";
        case Shape::RampDown: return "Ramp down";
        case Shape::Sine: return "Sine";
        case Shape::Triangle: return "Triangle";
        case Shape::Saw: return "Saw";
        case Shape::Square: return "Square";
    }
    return "";
}

namespace {
struct Corner {
    double phase, value;
    bool early;  // JUMP_S before the phase (the top of a jump)
};

std::vector<Corner> cornersOf(Shape shape) {
    switch (shape) {
        case Shape::Sine: {
            std::vector<Corner> out;
            for (int k = 0; k < SINE_POINTS; ++k)
                out.push_back({(double)k / SINE_POINTS, 0.5 + 0.5 * std::sin(2 * M_PI * k / SINE_POINTS), false});
            return out;
        }
        case Shape::Triangle: return {{0, 0, false}, {0.5, 1, false}};
        case Shape::Saw: return {{0, 0, false}, {1, 1, true}};
        case Shape::Square: return {{0, 1, false}, {0.5, 1, true}, {0.5, 0, false}, {1, 0, true}};
        default: return {};
    }
}

// the value just before phase f (0 < f <= 1) of a cycle
double valueBefore(Shape shape, double f) {
    switch (shape) {
        case Shape::Sine: return 0.5 + 0.5 * std::sin(2 * M_PI * f);
        case Shape::Triangle: return f <= 0.5 ? 2 * f : 2 - 2 * f;
        case Shape::Saw: return f;
        case Shape::Square: return f <= 0.5 ? 1 : 0;
        default: return 0;
    }
}
}  // namespace

std::vector<AutoPoint> drawShape(const std::vector<AutoPoint>& points, double start, double end, Shape shape,
                                 const Timing& timing, bool perBar) {
    start = std::max(0.0, start);
    if (!(end > start + 2 * JUMP_S)) return points;
    std::vector<AutoPoint> run;
    if (shape == Shape::RampUp || shape == Shape::RampDown) {
        bool up = shape == Shape::RampUp;
        run = {{start, up ? 0.0 : 1.0, 0.0}, {end, up ? 1.0 : 0.0, 0.0}};
        return overwrite(points, run);
    }
    double b0 = timing.beatAt(start), b1 = timing.beatAt(end);
    double len = perBar ? std::max(1, timing.beatsPerBarAt(start)) : 1.0;
    std::vector<Corner> corners = cornersOf(shape);
    bool done = false;
    for (int c = 0; c < MAX_CYCLES && !done; ++c) {
        double cycle = b0 + c * len;
        if (cycle >= b1 - 1e-9) break;
        for (auto& k : corners) {
            double s = timing.secondsAt(cycle + k.phase * len) - (k.early ? JUMP_S : 0.0);
            if (s >= end - 1e-9) {
                done = true;
                break;
            }
            if (!run.empty() && s <= run.back().t) continue;
            run.push_back({s, k.value, 0.0});
        }
    }
    double phase = (b1 - b0) / len;
    double f = phase - std::floor(phase);
    if (f < 1e-9) f = 1;
    run.push_back({end, valueBefore(shape, f), 0.0});
    return overwrite(points, run);
}

}  // namespace automation
