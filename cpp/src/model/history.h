// Undo and redo for the whole project (history.py). No GUI code.
//
// Edits change the project's objects in place (a clip's start, a track's volume…) and replace its
// lists (project.clips, track.effects…), so a step is a snapshot of every object's data fields (the
// shared audio and notes are never changed in place, so they are shared, not copied) plus the
// project's own lists. Undo writes the fields back into the SAME objects, so everything holding on to
// them (the audio thread, open windows) stays valid.
//
// The program calls check() now and then (the main window, while no mouse button is held, so a drag
// is one step): when the project differs from the last snapshot, that snapshot becomes an undo step.
// Whatever changes the project is undoable without each edit having to say so.
//
// Left out, as they are how the project is looked at rather than what it is (VIEW_FIELDS): lane
// heights, shown or folded lanes, the grid view (beats, thirds, the grid step, snapping) and whether
// it loops. The loop range is kept: inserting or deleting time moves it with the music, and undoing
// that puts it back. Runtime fields (patch states, automation targets, plugin instances, errors,
// recently tweaked parameters, plugin states) aren't kept either. A looping audio clip's audio (its
// loop written out, as long as the clip) isn't kept: it follows from the clip's other fields, so
// restore() builds it again rather than holding a copy of a long loop for every step. Plugins' own
// settings (changed in their windows) aren't part of it; a plugin removed and brought back by undo is
// the same loaded plugin (its slot keeps it). A change of sample rate (another audio device) starts
// the history afresh: the clips' prepared audio belongs to the rate.
#pragma once
#include <memory>
#include <vector>

#include "model/model.h"

namespace history {

constexpr int MAX_STEPS = 100;

struct SnapshotData;  // (history.cpp)

// the project as it was at one moment (its objects' data fields and its lists)
class Snapshot {
public:
    explicit Snapshot(const Project& project);
    ~Snapshot();
    Snapshot(Snapshot&&) noexcept;
    Snapshot& operator=(Snapshot&&) noexcept;
    bool matches(const Project& project) const;  // is the project still as this snapshot says?
    void restore(Project& project, int sr) const;  // put it back (into the same objects)

private:
    std::unique_ptr<SnapshotData> d_;
};

class History {
public:
    History(Project& project, int sr);
    // start afresh from the project as it is now (a project opened or made, another sample rate)
    void reset(int sr);
    // note a change since the last look as a step to undo; true if there was one
    bool check(int sr);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    int undoSteps() const { return (int)undo_.size(); }
    int redoSteps() const { return (int)redo_.size(); }
    // back to how the project was before the last change; false if there is nothing to undo
    bool undo(int sr);
    bool redo(int sr);

private:
    Project& project_;
    int sr_ = 0;
    std::vector<Snapshot> undo_, redo_;
    std::unique_ptr<Snapshot> current_;
};

}  // namespace history
