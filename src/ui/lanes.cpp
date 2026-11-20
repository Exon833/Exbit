#include "ui/lanes.h"

#include <algorithm>

#include "model/routing.h"
#include "ui/theme.h"

namespace lanes {

namespace {
void automationLanes(std::vector<Lane>& out, int& y, const TrackPtr& track) {
    if (!track->showAutomation) return;
    for (auto& a : track->automation) {
        Lane al;
        al.top = y;
        al.height = a->height;
        al.automation = a;
        al.owner = track;
        out.push_back(al);
        y += a->height;
    }
}
}  // namespace

std::vector<Lane> layout(const Project& project, int top) {
    std::vector<Lane> out;
    int y = top;
    const int takeH = takeHeight();
    std::vector<GroupPtr> open;  // the headers above the track now: outer first (nested groups)
    for (auto& track : project.tracks) {
        std::vector<GroupPtr> path;  // its groups, outer first
        for (GroupPtr g = track->group; g && path.size() < 1000; g = std::atomic_load(&g->parent)) path.insert(path.begin(), g);
        size_t same = 0;
        while (same < open.size() && same < path.size() && open[same] == path[same]) same++;
        open.resize(same);
        bool hidden = false;  // (inside a folded group)
        for (size_t k = 0; k < same; k++) hidden = hidden || path[k]->folded;
        for (size_t k = same; k < path.size(); k++) {
            const GroupPtr& g = path[k];
            open.push_back(g);
            if (!hidden) {
                Lane l;
                l.top = y;
                l.height = headerHeight(*g);
                l.group = g;
                out.push_back(l);
                y += l.height;
                if (routing::busOn(*g)) automationLanes(out, y, g->bus);  // (its bus's, under its header)
            }
            hidden = hidden || g->folded;
        }
        if (hidden) continue;
        Lane l;
        l.top = y;
        l.height = track->height;
        l.track = track;
        out.push_back(l);
        y += track->height;
        automationLanes(out, y, track);
        if (track->showTakes) {
            for (auto& take : track->takes) {
                Lane tl;
                tl.top = y;
                tl.height = takeH;
                tl.take = take;
                tl.owner = track;
                out.push_back(tl);
                y += takeH;
            }
        }
    }
    Lane master;
    master.top = y;
    master.height = masterHeight();
    out.push_back(master);
    return out;
}

namespace {
int scopeHeight = 0;
bool tempoShown = false;
}

bool tempoLaneShown() { return tempoShown; }
void setTempoLaneShown(bool on) { tempoShown = on; }
int tempoLaneHeight() { return tempoShown ? theme::metric("--tempo-lane-height", config::TEMPO_LANE_H) : 0; }
int lanesTop() { return rulerHeight() + tempoLaneHeight(); }

int masterHeight() { return theme::metric("--master-height", config::MASTER_H) + scopeHeight; }
int groupHeight() { return theme::metric("--group-height", config::GROUP_H); }
int headerHeight(const Group& g) { return routing::busOn(g) ? std::max(groupHeight(), g.bus->height) : groupHeight(); }
int takeHeight() { return theme::metric("--take-height", config::TAKE_H); }
int rulerHeight() { return theme::metric("--ruler-height", config::RULER_H); }

void setMasterScopeHeight(int px) { scopeHeight = px > 0 ? px : 0; }

}  // namespace lanes
