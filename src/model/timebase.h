// The song's TIMEBASE (Timing::timebaseBeats): what a change of the tempo map does to what is placed in it.
// No GUI code.
//
//   Beats  (new projects)  the song stays locked to bars: clips start at the same beat as before, MIDI notes
//          keep their beats and lengths in beats, automation points, markers and the loop range keep their
//          beats. Audio clips keep playing at their own speed (they aren't stretched: warping is its own
//          feature, told by the `retimed` hook which clips moved, so warped clips can be prepared again).
//   Time   (project files from before the setting) everything keeps its place in seconds; only the grid, the
//          metronome, MIDI effects and swing follow the tempo.
//
// How an edit uses it: remember() the song before changing the tempo map, follow() after; follow() moves
// everything from where it was on the remembered map to the same beats on the map now. A drag keeps one
// remember() for the whole drag and calls follow() on every move: always from the positions the drag began
// with, so nothing drifts however long the drag. The moves are ordinary edits of the project's fields, so the
// history's next check makes the tempo edit and the moved clips one undo step.
#pragma once
#include <functional>
#include <vector>

#include "model/model.h"

namespace timebase {

// the song as it was before a tempo edit (empty: the timebase is Time, nothing will follow)
struct Before {
    TempoMapPtr map;
    struct ClipAt {
        ClipPtr clip;
        int64_t start, offset, length;
        double lengthS;
        std::shared_ptr<const std::vector<Note>> notes, mutedNotes;
    };
    std::vector<ClipAt> clips;
    struct TakeAt {
        TrackPtr track;
        std::vector<int64_t> starts;  // each take's (in the track's order) and its frozen rendering's (last; -1 none)
    };
    std::vector<TakeAt> takes;
    std::vector<std::pair<AutomationPtr, std::vector<AutoPoint>>> automation;
    std::vector<Marker> markers;
    double loopStart = 0, loopEnd = 0;
    bool empty() const { return !map; }
};

// what follow() needs, taken now (cheap: the notes and curves are shared, not copied)
Before remember(const Project& project);

// everything in `before` put at its beats on the project's tempo map as it is now. Returns the clips that
// moved or changed (MIDI clips are prepared again here, on every core when there are many); nothing when
// the timebase is Time or the map didn't change.
std::vector<ClipPtr> follow(Project& project, const Before& before, int sr);

// set by the warping module (or anyone): called after follow() moved clips, with them, so warped audio
// can be prepared again for the new tempo map. GUI thread.
extern std::function<void(Project& project, const std::vector<ClipPtr>& moved, int sr)> retimed;

}  // namespace timebase
