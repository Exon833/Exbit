// The editor's lanes from top to bottom (lanes.py), shared by the timeline, the lane panel and the
// overview so they always line up:
//   a header lane for each group, right above its tracks (GROUP_H tall)
//   a lane for each track, as tall as the track; none for the tracks of a folded group
//   under a track showing its automation, a lane for each of its automation lanes
//   under a track showing its takes, a lane for each recorded take (TAKE_H), latest last
//   the master lane at the bottom (MASTER_H)
#pragma once
#include <vector>

#include "model/model.h"

struct Lane {
    int top = 0;
    int height = 0;
    TrackPtr track;            // a track's lane
    GroupPtr group;            // a group's header lane
    AutomationPtr automation;  // an automation lane of the track `owner`
    ClipPtr take;              // a recorded take (a clip) of the track `owner`
    TrackPtr owner;
    // none of these: the master lane
    int bottom() const { return top + height; }
    bool isMaster() const { return !track && !group && !automation && !take; }
};

namespace lanes {
// [Lane], starting at y = top; the last one is the master lane
std::vector<Lane> layout(const Project& project, int top = 0);
}  // namespace lanes
