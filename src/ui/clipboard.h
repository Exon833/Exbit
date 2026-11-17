// Copy, cut and paste clips on the timeline (Edit menu, Ctrl+C / Ctrl+X / Ctrl+V, the clip right-click
// menu), clipboard.py.
//
// Copy keeps copies of the selected clips, or with a time range selected the parts of the clips in it
// (as edits::pieceWithin cuts them), with their times from the earliest one's start (from the range's
// start) and their lanes counted from the topmost one. They share their audio with the originals, as
// split pieces do, so copying costs nothing. Paste puts them at the selection cursor, the topmost on
// the selected track and the others on the lanes below it (as the timeline shows them), replacing
// what they land on, and selects them; the cursor then moves to their end, so Ctrl+V again pastes
// right after. A clip only goes onto a track of its own kind: when the lanes don't fit, nothing is
// pasted and the status bar says why.
//
// Pasted clips are independent of the clips they were copied from, also linked MIDI clips (model.h):
// only linked clips copied TOGETHER stay linked to each other, by a link of their own (each paste a new
// one), never to the originals.
//
// One clipboard for the program (it lives as long as it runs); it is in frames, so after the sample
// rate changed it asks for a new copy instead of pasting.
//
// Ctrl+drag on a clip's name strip (Timeline: CopyDrag) drags copies of the selection instead, the
// originals staying where they are; a Ctrl+click without dragging still adds or removes a clip.
//
// THE MAIN WINDOW: its Edit menu's Cut clips (Ctrl+X) / Copy clips (Ctrl+C) / Paste clips at the cursor
// (Ctrl+V), shortcuts of the main window only (the MIDI editor copies notes with the same keys), and
// the timeline's `clipboardChosen` signal ("cut" / "copy" / "paste", from its right-click menu), call
// clipboard::use(timeline, what) and show the message it returns in the status bar for 4 s.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"

class Timeline;

namespace clipboard {

struct Copied {
    struct Item {
        ClipPtr clip;     // a copy (sharing the original's audio)
        int64_t offset;   // frames after the copied part's start
        int lane;         // lanes below the topmost
    };
    std::vector<Item> clips;
    int64_t span = 0;     // frames from the start of what was copied to its end
    int sr = 0;
};

extern std::optional<Copied> held;  // what was copied last

// keep the selection (clips, or what lies in the time range); returns how many clips were copied
int copy(Timeline& timeline);
// place what was copied at the selection cursor, from the selected track down (see the top): the new
// clips, or (none, and) `reason` why nothing was pasted
std::vector<ClipPtr> paste(Timeline& timeline, std::string& reason);
// copy, then remove what was copied (the selected clips, or what lies in the range); returns how many
int cut(Timeline& timeline);
// what = "cut" / "copy" / "paste": does it and returns what to say in the status bar (use_clipboard)
std::string use(Timeline& timeline, const std::string& what);

}  // namespace clipboard
