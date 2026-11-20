// Inserting and deleting time in the whole song (Edit → Insert time… / Delete time), song_time.py.
// No GUI code.
//
// insertTime opens a gap: everything from `at` on moves later by `length` (clips crossing `at` are
// split there). deleteTime takes a stretch out: what lies in it goes and everything after it moves
// earlier (clips crossing its edges are cut there). Every track is changed, whatever lanes the range
// covered. What moves:
//
// - clips, and recorded takes (not placed: they move when they start after the edit point);
// - automation curves, which keep their shape on both sides (a point is added at the edit point);
// - markers (those inside deleted time go);
// - the loop range;
// - tempo and time-signature changes (steps and ramps), which are kept by bar: they move by whole bars when the time
//   inserted or deleted is whole bars (both ends on bar lines), else they stay where they are (the
//   note returned says so). Inserted bars hold the tempo before them, and a ramp the edit cuts goes on
//   after it from the tempo it had there (deleted bars: the ramp before the cut goes to that tempo).
//
// A frozen track's rendering can't be cut: an edit that would cut through one isn't made (the
// functions return false with the reason in `error`, Python's FrozenInTheWay).
#pragma once
#include <string>
#include <vector>

#include "model/model.h"

namespace song_time {

constexpr double EDGE_S = 1e-4;  // where a curve jumps (deleted time), its two points this far apart

// "" if the edit may be made, else why not ("Audio 1 is frozen: unfreeze it first (right-click its row)").
// span: deleting [spanStart, spanEnd) (spanEnd > spanStart), which also may not take away any of a frozen
// track's rendering or clips
std::string checkFrozen(const Project& project, const std::vector<int64_t>& frames, int64_t spanStart = 0,
                        int64_t spanEnd = 0);
// (first bar, how many bars) when startS and endS both fall on bar lines (in bars and beats), else false
bool wholeBars(const Timing& timing, double startS, double endS, int& first, int& count);

// Open a gap of `length` frames at frame `at` in the whole song. `note`: a remark for the status bar
// ("" if none). False (and `error`) if it isn't made.
bool insertTime(Project& project, int64_t at, int64_t length, int sr, std::string& note, std::string& error);
// Take frames [start, end) out of the whole song.
bool deleteTime(Project& project, int64_t start, int64_t end, int sr, std::string& note, std::string& error);

// the parts (public for tests)
void moveAutomation(Project& project, double atS, double shiftS, double cutTo = -1);  // cutTo < 0: inserting
void moveMarkers(Project& project, double atS, double shiftS, double cutTo = -1);
void moveLoop(Timing& timing, double atS, double shiftS, double cutTo = -1);
std::string moveTempo(Timing& timing, double atS, double lengthS, bool insert);

}  // namespace song_time
