#include "model/auto_follow.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "audio/automation.h"
#include "audio/workers.h"
#include "core/util.h"

namespace auto_follow {

namespace {

constexpr double SAME_V = 1e-9;  // values this close are the same (no jump at a span's edge)

// the curve with points added at `a` and `b` (curved segments keep their shape: automation::splitAt)
std::vector<AutoPoint> splitTwice(const std::vector<AutoPoint>& points, double a, double b) {
    std::vector<AutoPoint> s = points;
    automation::splitAt(s, a);
    automation::splitAt(s, b);
    return s;
}

// the bend of the point at exactly `t` in s (the segment on from there), 0 if none
double bendAt(const std::vector<AutoPoint>& s, double t) {
    double bend = 0;
    for (auto& p : s)
        if (p.t == t) bend = p.bend;
    return bend;
}

// fn(i) for i = 0 … n-1 on every thread the user lets the program use (the GUI thread waits for them all)
template <class Fn>
void forEach(int n, Fn fn) {
    if (n <= 1) {
        for (int i = 0; i < n; i++) fn(i);
        return;
    }
    static workers::Pool pool(workers::workersFor(workers::chosenCores()), false);  // (not real-time: not playback)
    pool.run(n, [](void* ctx, int i) { (*static_cast<Fn*>(ctx))(i); }, &fn);
}

}  // namespace

std::vector<AutoPoint> cut(const std::vector<AutoPoint>& points, double start, double length) {
    if (points.empty() || !(length > 0)) return {};
    double end = start + length;
    std::vector<AutoPoint> s = splitTwice(points, start, end);
    std::vector<AutoPoint> out{{0.0, automation::valueAt(points, start), bendAt(s, start)}};
    for (auto& p : s)
        if (p.t > start && p.t < end) out.push_back({p.t - start, p.v, p.bend});
    out.push_back({length, automation::valueAt(points, end), 0.0});
    return out;
}

std::vector<AutoPoint> cleared(const std::vector<AutoPoint>& points, double start, double length) {
    if (points.empty() || !(length > 0)) return points;
    double end = start + length;
    std::vector<AutoPoint> s = splitTwice(points, start, end);
    std::vector<AutoPoint> out;
    out.reserve(s.size() + 2);
    for (auto& p : s)
        if (p.t < start) out.push_back(p);
    out.push_back({start, automation::valueAt(points, start), 0.0});  // (a straight line across the span)
    out.push_back({end, automation::valueAt(points, end), bendAt(s, end)});
    for (auto& p : s)
        if (p.t > end) out.push_back(p);
    return out;
}

std::vector<AutoPoint> pasted(const std::vector<AutoPoint>& points, const std::vector<AutoPoint>& piece, double at,
                              double length) {
    if (piece.empty() || !(length > 0)) return points;
    std::vector<AutoPoint> out;
    out.reserve(points.size() + piece.size() + 4);
    if (points.empty()) {  // (nothing around it to keep)
        for (auto& p : piece) out.push_back({at + p.t, p.v, p.bend});
        return out;
    }
    double end = at + length;
    bool jumpIn = at > 0 && std::fabs(automation::valueAt(points, at) - piece.front().v) > SAME_V;
    bool jumpOut = std::fabs(automation::valueAt(points, end) - piece.back().v) > SAME_V;
    // the outside curve exactly as it was up to `before` and from `after` (a jump: EDGE_S from the piece)
    double before = jumpIn ? std::max(0.0, at - EDGE_S) : at, after = jumpOut ? end + EDGE_S : end;
    std::vector<AutoPoint> s = splitTwice(points, before, after);
    for (auto& p : s)
        if (p.t < before) out.push_back(p);
    if (jumpIn) out.push_back({before, automation::valueAt(points, before), 0.0});
    for (auto& p : piece) out.push_back({at + p.t, p.v, p.bend});
    if (jumpOut) out.push_back({after, automation::valueAt(points, after), bendAt(s, after)});
    else out.back().bend = bendAt(s, end);  // (the curve on from the piece's end, as it was)
    for (auto& p : s)
        if (p.t > after) out.push_back(p);
    return out;
}

std::vector<Piece> take(const Track& track, double start, double length) {
    std::vector<Piece> out;
    for (auto& lane : track.automation)
        if (!lane->points.empty()) out.push_back({lane, cut(*lane->points, start, length)});
    return out;
}

SlotPtr matching(const Track& target, const Track& from, const SlotPtr& slot) {
    if (!slot) return nullptr;
    if (&target == &from) return slot;
    if (slot->isControls()) return target.controls;
    for (auto& s : automation::pluginsOf(target))
        if (s && !s->isControls() && s->kind == slot->kind && s->path == slot->path && s->pluginName == slot->pluginName)
            return s;
    return nullptr;
}

namespace {

// the target's lane for a piece (made if its slot has none yet, into `lanes`); null: no such parameter there
AutomationPtr laneFor(const Track& target, const Track& from, const Piece& piece, std::vector<AutomationPtr>& lanes,
                      bool& made) {
    if (std::find(lanes.begin(), lanes.end(), piece.lane) != lanes.end()) return piece.lane;  // (its own track)
    SlotPtr slot = matching(target, from, piece.lane->slot);
    if (!slot) return nullptr;
    for (auto& l : lanes)
        if (l->slot == slot && l->param == piece.lane->param) return l;
    auto lane = std::make_shared<Automation>();
    lane->slot = slot;
    lane->param = piece.lane->param;
    lanes.push_back(lane);
    made = true;
    return lane;
}

}  // namespace

int lay(Track& target, const Track& from, const std::vector<Piece>& pieces, double at, double length, bool& added) {
    std::vector<AutomationPtr> lanes = target.automation.copy();
    bool made = false;
    int n = 0;
    for (auto& piece : pieces)
        if (AutomationPtr lane = laneFor(target, from, piece, lanes, made)) {
            lane->setPoints(pasted(*lane->points, piece.points, at, length));
            n++;
        }
    if (made) {
        target.automation = lanes;
        target.showAutomation = true;
        added = true;
    }
    return n;
}

Move moveOf(const Clip& clip, const TrackPtr& from, int64_t start, int sr) {
    Move m;
    m.from = from;
    m.to = clip.track;
    m.start = (double)start / sr;
    m.length = (double)clip.frames() / sr;
    m.at = (double)clip.start / sr;
    return m;
}

bool carry(Project& project, const std::vector<Move>& moves, bool copies) {
    if (!project.timing.automationFollows) return false;
    std::vector<const Move*> real;
    for (auto& m : moves)
        if (m.from && m.to && m.length > 0 && (m.from != m.to || m.start != m.at)) real.push_back(&m);
    if (real.empty()) return false;
    // every piece first (on every thread: each move reads its own track's lanes)
    std::vector<std::vector<Piece>> pieces(real.size());
    forEach((int)real.size(), [&](int i) { pieces[(size_t)i] = take(*real[(size_t)i]->from, real[(size_t)i]->start, real[(size_t)i]->length); });
    // what goes where, lane by lane (lanes made here, on this thread): a lane's edits in order on one thread
    struct Edit {
        const std::vector<AutoPoint>* piece;  // null: clear the span
        double at, length;
    };
    std::map<AutomationPtr, std::vector<Edit>> clears, lays;
    std::vector<AutomationPtr> order;  // (the lanes, in the order first met)
    bool added = false;
    std::map<Track*, std::vector<AutomationPtr>> newLanes;
    int laid = 0, stayed = 0;
    for (size_t i = 0; i < real.size(); i++) {
        const Move& m = *real[i];
        auto it = newLanes.find(m.to.get());
        if (it == newLanes.end()) it = newLanes.emplace(m.to.get(), m.to->automation.copy()).first;
        for (auto& piece : pieces[i]) {
            bool made = false;
            AutomationPtr target = laneFor(*m.to, *m.from, piece, it->second, made);
            if (!target) {
                stayed++;  // (no such parameter there: it stays where it was)
                continue;
            }
            added = added || made;
            if (!copies) {
                if (!clears.count(piece.lane) && !lays.count(piece.lane)) order.push_back(piece.lane);
                clears[piece.lane].push_back({nullptr, m.start, m.length});
            }
            if (!clears.count(target) && !lays.count(target)) order.push_back(target);
            lays[target].push_back({&piece.points, m.at, m.length});
            laid++;
        }
    }
    for (auto& [track, lanes] : newLanes)
        if (lanes.size() != track->automation.size()) {
            track->automation = lanes;
            track->showAutomation = true;
        }
    // every lane on a thread of its own: its spans cleared first (all of them), then the pieces laid
    forEach((int)order.size(), [&](int i) {
        Automation& lane = *order[(size_t)i];
        std::vector<AutoPoint> pts = lane.points.copy();
        auto c = clears.find(order[(size_t)i]);
        if (c != clears.end())
            for (auto& e : c->second) pts = cleared(pts, e.at, e.length);
        auto l = lays.find(order[(size_t)i]);
        if (l != lays.end())
            for (auto& e : l->second) pts = pasted(pts, *e.piece, e.at, e.length);
        lane.setPoints(std::move(pts));
    });
    util::debugf("automation follows clips: %zu %s, %d curve pieces %s, %d stayed (no such parameter there), %zu lanes "
                 "changed%s\n",
                 real.size(), real.size() == 1 ? "span" : "spans", laid, copies ? "copied" : "moved", stayed,
                 order.size(), added ? " (lanes made)" : "");
    return added;
}

void clear(Track& track, double start, double length) {
    for (auto& lane : track.automation)
        if (!lane->points.empty()) lane->setPoints(cleared(*lane->points, start, length));
}

}  // namespace auto_follow
