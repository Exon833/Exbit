// Edits made to many clips at once (the timeline's slip, crop and fades for every selected clip). No GUI code.
//
// SLIP. A clip's contents move inside it while the clip stays where it is (its start and length kept):
//   an audio clip   its window onto its audio moves (Clip::offset), kept inside the audio: slipping past
//                   either end of it stops there. A gain envelope sits on frames of the audio
//                   (clip_envelope.h), so it travels with the sound with nothing to change.
//   a looping one   (Clip::loopingAudio) the loop's phase moves: the window's start wraps around the loop (into
//                   it: a lead-in before the loop's start, left by trimming, isn't kept), so it never reaches an
//                   end; its looped audio is built further when the window needs it.
//   a MIDI clip     its notes (and switched-off notes) move by as much: a note moved before the clip's start or
//                   past its end is kept but not heard (slipping back brings it back). A looping MIDI clip's
//                   notes wrap around its pass (lengthS), as the loop's phase.
//   linked clips    (model.h) share their notes: slipping one slips them all (each prepared again).
// Every clip is moved from how it was when the slip began (SlipStart), so a drag going back and forth never
// drifts. The audio thread reads what changes: scalars in place, audio swapped in whole (prepared on copies,
// on every core when there are many: workers.h).
//
// CROP. The clips are cut to a time range: the parts outside it are removed (edits::pieceWithin), clips
// already inside it stay as they are, those not reaching it are left alone. One assignment of project.clips.
//
// FADES. A fade-in and a fade-out of a length and shape for every audio clip given (the lengths are frames;
// a clip shorter than both plays them shortened, as Clip::fades does).
#pragma once
#include <map>
#include <memory>
#include <vector>

#include "model/model.h"

namespace clip_edits {

// a clip as it was when a slip began
struct SlipStart {
    ClipPtr clip;
    int64_t offset = 0, length = 0;
    std::shared_ptr<const std::vector<Note>> notes, mutedNotes;
};
// the clips as they are now (frozen tracks' left out; of linked clips, the first one only: it slips the others)
std::vector<SlipStart> slipStart(const Project& p, const std::vector<ClipPtr>& clips);
// every clip's contents `frames` later than at `from` (negative: earlier), each kept to its audio's ends
void slipFrom(Project& p, const std::vector<SlipStart>& from, int64_t frames, int sr);
// the two together: one slip
void slip(Project& p, const std::vector<ClipPtr>& clips, int64_t frames, int sr);
// the slips (frames, as slipFrom's) an audio clip's contents can take from `s`: lo ≤ 0 ≤ hi (hi: as far later
// as its audio's start allows, lo: as far earlier as its end allows); a looping audio clip or a MIDI clip: unlimited
void slipRoom(const SlipStart& s, int64_t& lo, int64_t& hi);

// the clips cut to [start, end) frames: {clip: its piece} for those that were cut (frozen tracks' left alone)
std::map<ClipPtr, ClipPtr> crop(Project& p, const std::vector<ClipPtr>& clips, int64_t start, int64_t end);

struct Fades {
    int64_t in = 0, out = 0;  // frames (0: none)
    FadeShape inShape = FadeShape::Linear, outShape = FadeShape::Linear;
};
// the audio clips among `clips` (frozen tracks' left alone) fade in and out so; how many were changed
int setFades(const std::vector<ClipPtr>& clips, const Fades& f);

}  // namespace clip_edits
