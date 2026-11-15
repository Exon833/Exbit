#include "ui/lanes.h"

namespace lanes {

std::vector<Lane> layout(const Project& project, int top) {
    std::vector<Lane> out;
    int y = top;
    GroupPtr current;
    for (auto& track : project.tracks) {
        if (track->group && track->group != current) {
            Lane l;
            l.top = y;
            l.height = config::GROUP_H;
            l.group = track->group;
            out.push_back(l);
            y += config::GROUP_H;
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
                tl.height = config::TAKE_H;
                tl.take = take;
                tl.owner = track;
                out.push_back(tl);
                y += config::TAKE_H;
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

int masterHeight() { return config::MASTER_H + scopeHeight; }

void setMasterScopeHeight(int px) { scopeHeight = px > 0 ? px : 0; }

}  // namespace lanes
