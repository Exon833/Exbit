// Export MIDI clips as a standard MIDI file (.mid), for other programs: right-click MIDI clips → Export as
// MIDI file…, or File → Export selected MIDI clips… (midi_export.py).
//
// One clip: its notes. Several: one file, with a MIDI track per Exbit track (the clips on one track
// together), in time with each other. The file starts at the bar the earliest clip begins in, so its bars
// (and time-signature changes) are the song's. The notes are written as the clips play them (loops
// repeated, cut to the clip's window, switched-off notes left out: Clip::heardNotes), with the song's
// tempo and time signature (file_io::saveMidi), so they land on the same beats elsewhere. Not the
// track's MIDI effects: those are Exbit's, applied as it plays.
//
// THE MAIN WINDOW: File → "Export selected MIDI clips…" and the timeline's clip menu "Export as MIDI
// file…" call midi_export::exportMidi(this, clips, projectPath, project, engine.sr, statusFn).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "audio/file_io.h"
#include "model/model.h"

namespace ui { class Window; }

namespace midi_export {

struct Parts {
    double start = 0;                     // song seconds the file starts at (the earliest clip's bar)
    std::vector<file_io::MidiPart> parts; // a part per track, in the tracks' order of first appearance
};
// the parts of `clips` (MIDI clips)
Parts partsOf(const std::vector<ClipPtr>& clips, int sr, const Timing& timing);
// a name to offer for the file: the clip's (one clip), else the project's
std::string fileName(const std::vector<ClipPtr>& clips, const std::string& projectPath);
// write the parts to `path`; false and `error` if it can't. `notes`: how many were written
bool writeFile(const std::string& path, const std::vector<ClipPtr>& clips, int sr, const Timing& timing,
               std::string& error, size_t* notes = nullptr);
// ask where, then write `clips` (the MIDI ones among them) as a MIDI file; its path, or "" if none
std::string exportMidi(ui::Window* window, const std::vector<ClipPtr>& clips, const std::string& projectPath,
                       const Project& project, int sr, std::function<void(const std::string&, int)> status);

}  // namespace midi_export
