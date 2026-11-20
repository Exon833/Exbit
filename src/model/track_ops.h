// Track operations of the track panel: duplicating tracks and moving them (up, down, or dragged onto another
// row). No GUI code (the panel: ui/lane_panel.h, TRACK SELECTION).
//
// DUPLICATE. A copy goes right below its track, in its group (a return's among the returns). It is named as new
// tracks are ("Audio 7": Project::tracksMade) when its track has such a name, else "<name> 2" ("Bass 2", then
// "Bass 3"…: the first number no track has). It has all of its track but the recorded takes: fader, pan, mute,
// solo, output, delay, sends, colour, MIDI effects, built-in sampler and Pad Kit (shared: they are never changed
// in place), its effects and instruments as NEW slots holding its plugins' settings now (a plugin instance is never
// shared: copyOf() leaves them unloaded; load them, as projects are, before add()), their places in the patch
// space, its cables and modulation cables pointing at the copy's own units (a cable from another track stays
// from that track), and its automation lanes on the copy's own units. A frozen track's copy is frozen too (a copy
// of its rendering). With clips, each of the track's clips gets a copy on the new track (copyClip); LINKED MIDI
// clips stay linked: the copy's clip plays the same notes and follows edits made to any of them.
// Everything is built before the copy joins the project (the audio thread sees it whole), so it is one undo step.
//
// MOVE. step(): tracks one lane up (-1) or down (1), as the lanes show them: in a group a track swaps with its
// neighbour, and at the group's edge it steps out (above or below the group); a track next to a group steps into
// it at that end, or over all of it when it is folded (nested groups: one level at a time). Returns stay below the
// other tracks, among themselves.
// Several tracks move together keeping their order; none moves past the top or bottom, nor past another of them
// that cannot move. moveTo(): tracks put above or below another one, into its group (dragging a row onto
// another). Each is one undo step (history.h: "Move tracks").
#pragma once
#include <functional>
#include <vector>

#include "model/model.h"

namespace track_ops {

// a slot holding `slot`'s plugin and its settings now, not loaded (instance null)
SlotPtr freshCopy(const SlotPtr& slot);
// a track's plugins: its instrument, its instrument patch panel's, its effects
std::vector<SlotPtr> slotsOf(const Track& track);
// a copy of `track` not in any project yet (see DUPLICATE); its slots (slotsOf) are unloaded
TrackPtr copyOf(const Track& track);
// `copy` (copyOf(*original)) into the project right below `original`, named; with copies of original's clips
void add(Project& project, const TrackPtr& original, const TrackPtr& copy, bool withClips);
// copyOf + load(its slots) + add: the copy
TrackPtr duplicate(Project& project, const TrackPtr& track, bool withClips,
                   const std::function<void(const std::vector<SlotPtr>&)>& load = nullptr);
// the name a copy of a track named `name` gets (see DUPLICATE); counts the project's new tracks when it is one
// of their names
std::string copyName(Project& project, const Track& track);

// see MOVE: false if none of them could move
bool step(Project& project, const std::vector<TrackPtr>& tracks, int dir);
// `tracks` (in the project's order) above `target` (or below it), in its group; false if nothing moved
bool moveTo(Project& project, const std::vector<TrackPtr>& tracks, const TrackPtr& target, bool below);

// `cables` with `slot` wired in at the end: what went to OUT goes into it, and it goes to OUT
std::vector<Cable> wiredAtEnd(const std::vector<Cable>& cables, PluginSlot* slot);

}  // namespace track_ops
