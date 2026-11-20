// Commit to notes (the MIDI effects window's "Commit to notes ▾"): what a MIDI track's effects make of its
// clips (arpeggiator, chord, groove… and the song's and the track's swing) written into notes, as FL's Burn
// MIDI, Cubase's Freeze MIDI modifiers or Live's groove Commit do. No GUI code.
//
// The notes written are exactly the notes the clip plays (Clip::played): its loop laid out (the clip then
// doesn't loop: its notes already repeat), each note's chance and repeats decided, then the track's MIDI
// effects. So the clip sounds the same before and after, on every instrument.
//
// WHAT. One clip (or some) of the track, or every MIDI clip on it. WHERE. Into the same clips, or into new
// clips on a new MIDI track right below (a copy of the track with its instrument and effects plugins, but no
// MIDI effects, playing straight): the originals stay as they were, switched off (Clip::disabled) so they
// don't play twice; switch them on again from their menu.
// AFTER. Into the same clips: when every MIDI clip of the track was committed, the effects that were on
// are bypassed (MidiEffect::bypass, shown in the window) and the track plays straight (Track::swing 0) if it
// swung; when only some were, those clips are marked effectsCommitted (prepared without the track's effects)
// so the others keep playing through them.
// LINKED CLIPS. A committed clip leaves its link first (links::makeUnique): its partners keep their notes
// and keep playing through their own track's effects (the notes written depend on where the clip is: a
// groove or an arpeggiator follow the song's beats).
// It is all done in one go on the GUI thread, so the undo history (history.h) takes it as one step. The
// notes are worked out on every processor core (workers::Pool) and the clips are prepared again on them too
// (song_scale::reprepare).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"

namespace midi_commit {

// the notes `c` plays now (Clip::played as prepare() makes it, without rendering), as notes of its own:
// times from its start, every note plain but for its slide, pan and fine tuning
std::vector<Note> playedNotes(const Clip& c, int sr);

struct Result {
    int clips = 0;      // clips committed
    TrackPtr track;     // the new track (into a new track), else null
    int bypassed = 0;   // effects bypassed
    std::string error;  // why nothing was done ("" when it was)
};

// loads plugin slots (the main window's plugin host): the names of those that couldn't be
using Loader = std::function<std::vector<std::string>(const std::vector<SlotPtr>&)>;

// Commit `clips` of MIDI track `t` (empty: every MIDI clip on it) into their own notes, or (newTrack) into
// clips on a new track below (its plugins loaded with `load`, if given). See the top.
Result commit(Project& p, int sr, const TrackPtr& t, std::vector<ClipPtr> clips, bool newTrack,
              const Loader& load = nullptr);

}  // namespace midi_commit
