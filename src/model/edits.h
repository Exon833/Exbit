// Editing operations on a Project (edits.py): splitting clips, cutting time ranges out of them,
// making room for a moved or placed clip, and duplicating a range or clips. Clips are never changed
// in place here: pieces are new clips (sharing the original audio) swapped into project.clips in
// one assignment, so the audio thread never sees a half-edited clip.
#pragma once
#include <vector>

#include "model/model.h"

namespace edits {
// the part of `clip` in timeline frames [lo, hi) (inside it), as a new clip sharing its audio. Its fades go
// on as the whole clip's did: a piece of a fade starts (or ends) part-way up its ramp (fadeLead, fadeTail).
ClipPtr cutPiece(const ClipPtr& clip, int64_t lo, int64_t hi);
// (left, right) pieces of clip cut at timeline `frame`; false if frame is not inside it. Inside a fade,
// the fade goes on over the cut (the pieces sound as the clip did).
bool splitClip(const ClipPtr& clip, int64_t frame, ClipPtr& left, ClipPtr& right);
// split each of `clips` at every one of `frames` inside it; returns all resulting pieces
std::vector<ClipPtr> splitClips(Project& p, const std::vector<ClipPtr>& clips, std::vector<int64_t> frames);
// make room for `clips` (just moved or placed): the parts of other clips they cover are cut away
void clearUnder(Project& p, const std::vector<ClipPtr>& clips);
// remove frames [start, end) from every clip on `tracks`
void deleteRange(Project& p, const std::vector<TrackPtr>& tracks, int64_t start, int64_t end);
// the part of clip inside [start, end) as a new clip, or null
ClipPtr pieceWithin(const ClipPtr& clip, int64_t start, int64_t end);
std::vector<ClipPtr> placeCopies(Project& p, const std::vector<ClipPtr>& copies);
std::vector<ClipPtr> duplicateRange(Project& p, const std::vector<TrackPtr>& tracks, int64_t start, int64_t end);
std::vector<ClipPtr> duplicateClips(Project& p, const std::vector<ClipPtr>& clips);

// A new clip like `clip` stretched `stretch` times its recorded length and `pitch` semitones from it: its
// window (offset, length), loop and fades follow the new length, so it plays the same part of its audio.
ClipPtr restretched(const ClipPtr& clip, double stretch, double pitch, int sr);
// A new clip like `clip` playing backwards (or forwards again): the same part of its audio, its fades
// swapped round.
ClipPtr reversedClip(const ClipPtr& clip, int sr);
// One clip of the same kind playing what `clips` (all on one track, none disabled) play, from the first's
// start to the last's end, named "<first> (joined)": audio as it comes out of the clips (their gain and
// fades), MIDI notes as they play (heardNotes: a loop's repeats, notes cut to each clip's window), and their
// switched-off notes cut to the windows too. Returns the new clip (not placed), or null if clips is empty.
ClipPtr consolidated(const std::vector<ClipPtr>& clips, int sr);
// (left, right) as new clips that overlap by up to `frames` around the middle of where left ends and right
// starts (of a gap between them, or of an overlap they already have), left fading out and right fading in
// over the overlap: each reaches as far as its audio allows. False if they can't overlap at all.
bool crossfaded(const ClipPtr& left, const ClipPtr& right, int64_t frames, ClipPtr& newLeft, ClipPtr& newRight);
}  // namespace edits
