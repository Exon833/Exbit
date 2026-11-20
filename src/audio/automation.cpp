#include "audio/automation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "audio/modulation.h"
#include "config.h"

using plugin_audio::Parameter;
using plugin_audio::usable;

namespace automation {

bool isPlaceholder(const std::string& name) {
    if (name.compare(0, 7, "MIDI CC") == 0) return true;
    if (name.size() == 4 && name[0] == 'P') {
        for (int i = 1; i < 4; i++)
            if (name[i] < '0' || name[i] > '9') return false;
        return true;
    }
    return false;
}

double volumeOf(double value) { return config::MIN_DB + value * (config::MAX_DB - config::MIN_DB); }

double panOf(double value) { return value * 2 - 1; }

double controlValue(const Track& track, const std::string& name) {
    if (name == VOLUME) return (track.volumeDb - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB);
    return (track.pan + 1) / 2;
}

std::string controlText(const std::string& name, double value) {
    char buf[32];
    if (name == VOLUME) {
        double db = volumeOf(value);
        if (db <= config::MIN_DB) return "-inf dB";
        snprintf(buf, sizeof buf, "%+.1f dB", db);
        return buf;
    }
    int amount = (int)std::nearbyint(std::fabs(panOf(value)) * 100);
    if (amount == 0) return "C";
    snprintf(buf, sizeof buf, "%d %s", amount, value < 0.5 ? "L" : "R");
    return buf;
}

void mixerValues(const Track& track, bool hasStart, int64_t start, int sr, double& volumeDb, double& pan) {
    volumeDb = track.volumeDb;
    pan = track.pan;
    if (!hasStart) return;
    auto lanes = track.automation.get();
    if (lanes->empty()) return;
    const PluginSlot* controls = track.controls.get();
    for (auto& lane : *lanes) {
        if (lane->slot.get() != controls) continue;
        auto points = lane->points.get();
        bool touched = lane->touched.load(std::memory_order_acquire);  // (being written: the user's hand decides)
        if (points->empty() && !touched) continue;
        double value = touched ? lane->touchValue.load(std::memory_order_relaxed) : valueAt(*points, (double)start / sr);
        if (lane->param == VOLUME)
            volumeDb = volumeOf(value);
        else if (lane->param == PAN)
            pan = panOf(value);
    }
}

bool controlAutomated(const Track& track, const std::string& name) {
    for (auto& lane : track.automation)
        if (lane->slot == track.controls && lane->param == name && !lane->points.empty()) return true;
    return false;
}

std::vector<SlotPtr> pluginsOf(const Track& track) {
    std::vector<SlotPtr> out{track.controls};
    for (auto& s : track.instruments()) out.push_back(s);
    for (auto& s : track.effects) out.push_back(s);
    return out;
}

std::vector<Parameter> parameters(const PluginSlot& slot) {
    std::vector<Parameter> out;
    if (!usable(&slot)) return out;
    auto inst = aload(slot.instance);
    for (auto& info : inst->parameters())
        if (info.automatable && !info.readOnly && !info.hidden && !isPlaceholder(info.name))
            out.emplace_back(inst, info);
    return out;
}

TweakWatcher::TweakWatcher(SlotPtr s, Project& project) : slot(std::move(s)), project_(project) {
    for (auto& p : parameters(*slot)) names_.emplace_back(p.id, p.name);
    if (usable(slot)) aload(slot->instance)->edited();  // (forget changes from before)
}

std::set<std::string> TweakWatcher::automated() const {
    std::set<std::string> out;
    for (auto& t : project_.allTracks())
        for (auto& a : t->automation)
            if (a->slot == slot && !a->points.empty()) out.insert(a->param);
    return out;
}

void TweakWatcher::poll() {
    if (!usable(slot)) return;
    std::vector<uint32_t> edited = aload(slot->instance)->edited();
    modulation::userEdited(project_, slot.get(), edited);  // (knobs turned that modulation moves: their base)
    writeEdits(edited);  // (armed lanes while playing: automation_write.h; after the base above, which it writes)
    std::vector<std::string> changed;  // latest first, each once
    for (auto it = edited.rbegin(); it != edited.rend(); ++it)
        for (auto& n : names_)
            if (n.first == *it) {
                if (std::find(changed.begin(), changed.end(), n.second) == changed.end()) changed.push_back(n.second);
                break;
            }
    std::set<std::string> moving = automated();
    changed.erase(std::remove_if(changed.begin(), changed.end(), [&](const std::string& s) { return moving.count(s) > 0; }),
                  changed.end());
    if (!changed.empty() && (int)changed.size() <= PRESET_CHANGE) {
        std::vector<std::string> recent;
        for (auto* list : {&changed, &slot->recent})
            for (auto& s : *list)
                if (std::find(recent.begin(), recent.end(), s) == recent.end()) recent.push_back(s);
        if ((int)recent.size() > RECENT) recent.resize(RECENT);
        slot->recent = recent;
    }
}

bool prune(Track& track) {
    std::vector<SlotPtr> slots{track.controls};
    if (track.instrument) slots.push_back(track.instrument);
    for (auto& s : track.rack) slots.push_back(s);
    for (auto& s : track.effects) slots.push_back(s);
    std::vector<AutomationPtr> kept;
    for (auto& a : track.automation)
        if (std::find(slots.begin(), slots.end(), a->slot) != slots.end()) kept.push_back(a);
    if (kept.size() == track.automation.size()) return false;
    track.automation = kept;
    return true;
}

void resolve(Project& project) {
    for (auto& track : project.allTracks())
        for (auto& lane : track->automation) {
            if (!lane->slot || lane->slot->isControls()) continue;  // (the mixer applies volume and pan)
            auto instance = aload(lane->slot->instance);
            auto target = aload(lane->targetPlugin);
            if (instance && (!target || target != instance)) {
                // (the audio thread uses the fields below only while targetPlugin is its slot's instance)
                std::atomic_store(&lane->targetPlugin, std::shared_ptr<plug::Plugin>());
                lane->hasTarget = false;
                for (auto& p : parameters(*lane->slot))
                    if (p.name == lane->param) {  // (none: the plugin has no such parameter now)
                        lane->targetParam = p.id;
                        lane->targetSteps = p.steps;
                        lane->hasTarget = true;
                        break;
                    }
                lane->last = -1;
                std::atomic_store(&lane->targetPlugin, instance);
            }
        }
}

int stepsOf(const Automation& lane) {
    return aload(lane.targetPlugin) && lane.hasTarget ? lane.targetSteps : 0;
}

double snap(double value, int steps) { return steps > 0 ? std::nearbyint(value * steps) / steps : value; }

// the curve at `seconds`, with i the number of points at or before it
static double valueIn(const std::vector<AutoPoint>& points, double seconds, size_t i, int steps) {
    if (i == 0) return snap(points[0].v, steps);
    if (i == points.size() || steps > 0) return snap(points[i - 1].v, steps);
    const AutoPoint &a = points[i - 1], &b = points[i];
    if (!(b.t > a.t)) return b.v;
    if (a.bend == 0) return a.v + (b.v - a.v) * (seconds - a.t) / (b.t - a.t);  // (straight: as it always was)
    return a.v + (b.v - a.v) * bendShape((seconds - a.t) / (b.t - a.t), a.bend, b.v - a.v);
}

static size_t pointsUpTo(const std::vector<AutoPoint>& points, double seconds) {
    return std::upper_bound(points.begin(), points.end(), seconds, [](double s, const AutoPoint& p) { return s < p.t; }) -
           points.begin();
}

double valueAt(const std::vector<AutoPoint>& points, double seconds, int steps) {
    if (points.empty()) return 0;
    return valueIn(points, seconds, pointsUpTo(points, seconds), steps);
}

double valueAt(const std::vector<AutoPoint>& points, double seconds, size_t& cursor, int steps) {
    if (points.empty()) return 0;
    if (cursor == 0 || cursor > points.size() || points[cursor - 1].t > seconds) {
        cursor = pointsUpTo(points, seconds);  // (the first look, or it went back: one search)
    } else {
        while (cursor < points.size() && points[cursor].t <= seconds) ++cursor;
    }
    return valueIn(points, seconds, cursor, steps);
}

// --- curved segments (AutoPoint::bend) ---

double bendShape(double x, double bend, double rise) {
    if (bend == 0 || rise == 0) return x;
    // an exponential: f(x) = (e^(c·x) − 1) / (e^c − 1) rises from 0 to 1 whatever c is, so a segment stays
    // monotonic. c < 0 rises fast first (above the line when the segment rises): bend > 0 bows it up.
    double c = -BEND_CURVE * std::max(-1.0, std::min(1.0, bend)) * (rise > 0 ? 1 : -1);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return std::expm1(c * x) / std::expm1(c);
}

void splitAt(std::vector<AutoPoint>& points, double seconds) {
    // f restricted to [0, x0] and to [x0, 1] (rescaled) is the same curve with c·x0 and c·(1 − x0): the
    // two halves keep the shape exactly with bend·x0 and bend·(1 − x0)
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        AutoPoint &a = points[i], &b = points[i + 1];
        if (seconds == a.t || seconds == b.t) return;  // (a point there already)
        if (a.t < seconds && seconds < b.t) {
            double x0 = (seconds - a.t) / (b.t - a.t);
            AutoPoint mid{seconds, valueAt(points, seconds), a.bend * (1 - x0)};
            a.bend *= x0;
            points.insert(points.begin() + (long)i + 1, mid);
            return;
        }
    }
}

void apply(Track& track, int64_t start, int sr, const plugin_audio::TrackList* tracks) {
    auto lanes = track.automation.get();
    for (auto& lane : *lanes) {
        if (lane->slot && lane->slot->isControls()) continue;  // (the mixer applies volume and pan: mixerValues())
        auto points = lane->points.get();
        auto target = aload(lane->targetPlugin);
        if (lane->touched.load(std::memory_order_acquire)) {
            lane->last = -1;  // (being written: the user moves it; once let go, the curve sets it again)
            continue;
        }
        if (points->empty() || !target || !lane->slot || !lane->hasTarget || target != aload(lane->slot->instance))
            continue;
        int64_t late = tracks ? plugin_audio::inputLatency(track, lane->slot.get(), *tracks) : 0;
        double value = valueAt(*points, (double)(start - late) / sr, lane->targetSteps);
        if (lane->last < 0 || std::fabs(value - lane->last) > 1e-6) {
            if (modulation::takeAutomation(track, lane->slot.get(), lane->targetParam, value)) {
                lane->last = value;  // (modulated: the lane moves its base, the modulation sets it)
                continue;
            }
            try {
                target->setParam(lane->targetParam, value);
                lane->last = value;
            } catch (...) {
                lane->hasTarget = false;  // (a parameter that refuses: leave it alone from now on)
            }
        }
    }
}

}  // namespace automation
