#include "ui/lanes.h"

#include "ui/theme.h"

namespace lanes {

std::vector<Lane> layout(const Project& project, int top) {
    std::vector<Lane> out;
    int y = top;
    const int groupH = groupHeight(), takeH = takeHeight();
    GroupPtr current;
    for (auto& track : project.tracks) {
        if (track->group && track->group != current) {
            Lane l;
            l.top = y;
            l.height = groupH;
            l.group = track->group;
            out.push_back(l);
            y += groupH;
        }
        current = track->group;
        if (current && current->folded) continue;
        Lane l;
        l.top = y;
        l.height = track->height;
        l.track = track;
        out.push_back(l);
        y += track->height;
        if (track->showAutomation) {
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
}

int masterHeight() { return theme::metric("--master-height", config::MASTER_H) + scopeHeight; }
int groupHeight() { return theme::metric("--group-height", config::GROUP_H); }
int takeHeight() { return theme::metric("--take-height", config::TAKE_H); }
int rulerHeight() { return theme::metric("--ruler-height", config::RULER_H); }

void setMasterScopeHeight(int px) { scopeHeight = px > 0 ? px : 0; }

}  // namespace lanes
