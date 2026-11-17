// The editor's lanes from top to bottom (lanes.py), shared by the timeline, the lane panel and the
// overview so they always line up:
//   a header lane for each group, right above its tracks (groupHeight(): --group-height)
//   a lane for each track, as tall as the track; none for the tracks of a folded group
//   under a track showing its automation, a lane for each of its automation lanes
//   under a track showing its takes, a lane for each recorded take (takeHeight(): --take-height), latest last
//   the master lane at the bottom (masterHeight(): --master-height, and SCOPE_H more while the scope is shown)
// The heights that aren't the project's own (a track's, an automation lane's) are the skin's: :root variables of
// its stylesheets (theme::metric; the config constants when a stylesheet doesn't set them).
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
// the master lane's and row's height: --master-height (MASTER_H), and the master scope's (ui/scope_view.h) while shown
int masterHeight();
int groupHeight();   // --group-height (GROUP_H): a group's header lane and row
int takeHeight();    // --take-height (TAKE_H): a take lane and its row
int rulerHeight();   // --ruler-height (RULER_H): the timeline's ruler (the lanes start under it)
void setMasterScopeHeight(int px);  // (0: the scope hidden)

}  // namespace lanes
