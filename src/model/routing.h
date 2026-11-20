// Where each track's sound goes: group buses, nested groups, and a track's output routed into another track (FL
// Studio's free routing). No GUI or audio-device code.
//
// A track's TARGET (target()): the track or bus it plays into, or null (the master). In order:
//   - a return track: the master, always (its input is what is sent to it);
//   - Track::into, when set: that track or group bus (summed into its input, before its effects);
//   - a group's bus: the bus of the nearest group around that group that isn't "fold only";
//   - any other track: the bus of its group, or of the nearest group around it that isn't "fold only".
// Following targets never loops: setInto() and setParent() refuse what would make a loop, and fixLoops()
// breaks any a file (or an edit elsewhere) brings, by clearing `into` (the mixer also never sums a track into
// one processed before it).
//
// A PLAIN bus (isPlain) changes nothing: no effects wired in, its fader at 0 dB and centred, no automation of
// them, no sends, no delay, not muted or soloed. The mixer skips it (its tracks go straight on), so groups
// that are just folders sound exactly as they did before groups were buses, and cost nothing.
//
// The audio thread reads targets on its snapshot: `into` and Group::parent are swapped with std::atomic_store.
#pragma once
#include <string>
#include <vector>

#include "model/model.h"

namespace routing {

// where t's sound goes (see the top): a track (or group bus), or null: the master
Track* target(const Track& t);
// does `from`'s sound reach `to`, following targets (from itself counts)?
bool reaches(const Track& from, const Track* to);
// could t play into `to` (null: its default) without a loop? (not a return, not itself, nothing feeding back)
bool canRoute(const Track& t, const Track* to);
// t's sound into `to` (null: its default); false (nothing changed) if it would make a loop
bool setInto(const TrackPtr& t, const TrackPtr& to);
// could g go inside `parent` (null: at the top)? (not itself, nor a group inside it)
bool canNest(const GroupPtr& g, const GroupPtr& parent);
// g inside `parent` (null: at the top); false if that would be a loop. Then fixLoops().
bool setParent(Project& p, const GroupPtr& g, const GroupPtr& parent);
// break routing loops (a file, regrouping): `into` cleared where a loop is found; how many were cleared
int fixLoops(Project& p);
// a bus that changes nothing (see the top); false for anything but a group's bus
bool isPlain(const Track& bus);
// the group's bus is on (not fold only)
inline bool busOn(const Group& g) { return !g.foldOnly; }
// the name shown for where t plays: "Master", a group's name, or a track's
std::string targetName(const Track& t);
// the groups t is in (a bus: those around its group), innermost first
std::vector<Group*> groupsOf(const Track& t);
// the project's tracks inside g, in groups nested in it too (in the project's order)
std::vector<TrackPtr> inside(const Project& p, const Group* g);

}  // namespace routing
