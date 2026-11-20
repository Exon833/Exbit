// Automation that follows clips (Edit → Automation follows clips, also in the transport's Grid menu):
// Timing::automationFollows, on by default, saved with the project and undoable. No GUI code.
//
// While it is on, moving clips (dragged, nudged, moved to another track), copying, cutting and pasting
// them, duplicating clips and Duplicate range take the automation of their tracks within their time span
// along (Bitwig's way; off, the automation stays locked to the timeline). Points are moved or copied; the
// curve outside the span keeps its value at the span's edges: points are added there so nothing else
// changes shape (a jump between the outside and what is laid in is EDGE_S wide, as song_time's are).
// A span moved away leaves a straight line between the curve's values at its edges.
//
// Onto another track a lane goes only if that track has the same parameter: its own Volume or Pan
// (`controls`), or a plugin of the same kind and file with that parameter (a lane is made for it there if
// it has none yet); otherwise that lane's automation stays where it was.
//
// The curve functions also serve an automation lane's own range copy, paste and clear (ui/clipboard.h).
#pragma once
#include <vector>

#include "model/model.h"

namespace auto_follow {

constexpr double EDGE_S = 1e-4;  // (as song_time::EDGE_S) a jump at a span's edge: its two points this far apart

// --- a curve (points by time; empty: no automation) ---
// the curve over [start, start + length] seconds, from 0 s: a point at either end (and those inside); empty
// if the curve has no points
std::vector<AutoPoint> cut(const std::vector<AutoPoint>& points, double start, double length);
// the curve with the span emptied: a point at either end keeps the curve's value there, none between
std::vector<AutoPoint> cleared(const std::vector<AutoPoint>& points, double start, double length);
// `piece` (from cut(): 0 … length s) laid over the curve at `at`, replacing what was there; outside it the
// curve keeps its shape (an empty curve: just the piece). An empty piece changes nothing.
std::vector<AutoPoint> pasted(const std::vector<AutoPoint>& points, const std::vector<AutoPoint>& piece, double at,
                              double length);

// --- a track's lanes ---
struct Piece {
    AutomationPtr lane;              // where it was cut from (its slot and parameter)
    std::vector<AutoPoint> points;   // from 0 s (cut())
};
// the curves of the track's lanes with points over the span
std::vector<Piece> take(const Track& track, double start, double length);
// the target's plugin slot for a lane of `slot` (on `from`): the slot itself on its own track, the target's
// controls for Volume / Pan, else its first plugin of the same kind and file; null if it has none
SlotPtr matching(const Track& target, const Track& from, const SlotPtr& slot);
// the pieces laid on `target` at `at` (lanes made for those its slots have); how many were laid. `added`:
// lanes were made (the timeline lays out again)
int lay(Track& target, const Track& from, const std::vector<Piece>& pieces, double at, double length, bool& added);

// --- clips moved or copied ---
struct Move {
    TrackPtr from, to;
    double start = 0, length = 0;  // where it was, in seconds
    double at = 0;                 // where it went
};
// a clip's move: from `from` at frame `start` to where it is now
Move moveOf(const Clip& clip, const TrackPtr& from, int64_t start, int sr);
// When automation follows clips (else nothing): each span's automation goes (copies: is copied) where its
// clip went. Every piece is taken before any is cleared or laid, so moves that overlap don't disturb each
// other. True if lanes were made.
bool carry(Project& project, const std::vector<Move>& moves, bool copies);
// the span's automation cleared on every lane of the track that has points (a cut)
void clear(Track& track, double start, double length);

}  // namespace auto_follow
