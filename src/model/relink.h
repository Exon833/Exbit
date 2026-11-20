// Finding a project's missing samples again (no GUI code: ui/relink_dialog.h asks).
//
// A project opened with sample files that aren't where it names them (project_io.h MISSING SAMPLES) has silent
// stand-ins holding a Clip::missing: its clips, takes, samplers' and Pad Kit pads' samples. missing() lists the
// files, each with what uses it. One search through a few folders (the Explorer's first, then the one chosen)
// finds them all at once: by name (and size, when the project saved it; the nearest path when several match),
// then, for renamed files, by size and sound (decoded and compared with the fingerprint the project saved). A
// folder moved as a whole: once one file is found under a new root, the others are tried at the same place
// under it (alongside(); after "Locate…" too). apply() puts the found files in, every clip and instrument that
// used each at once; until then saving keeps naming the files where they were expected.
//
// Nothing here writes, moves or deletes any file: a search only reads folders, and apply() only reads the
// files found (decoded at once on worker threads: project_io::decodeFiles).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "model/model.h"

namespace relink {

struct Missing {
    std::string path;                // where it was expected (absolute)
    int64_t size = -1;               // bytes, if the project saved it
    std::string sound;               // its project_io::fingerprint, if saved
    std::vector<std::string> users;  // what uses it: "clip “Kick” (Drums)", "Drums: sampler", "Kit: pad 3", …
};
// the project's missing files, by path
std::vector<Missing> missing(const Project& project);

using Found = std::map<std::string, std::string>;  // expected path -> where it is now

// the files of `missing` not in `found` yet, tried relative to the new root of each file found (a folder moved:
// "/old/Samples/a.wav" found as "/new/Samples/a.wav": "/old/Loops/b.wav" tried as "/new/Loops/b.wav"); the size
// must match when known. Adds what it finds; returns how many.
int alongside(const std::vector<Missing>& missing, Found& found);

// One search of `folders` and their subfolders (each file looked at once, folders inside another searched
// once) for the files of `missing` (not in `found` yet), then alongside(). `progress(files looked at)`: false
// stops it (what was found so far is kept). Returns how many it added to `found`.
int search(const std::vector<Missing>& missing, const std::vector<std::string>& folders, Found& found,
           const std::function<bool(size_t seen)>& progress = nullptr);

// does `file` look like `m` (there, and of its size when known)?
bool fits(const Missing& m, const std::string& file);

// The found files put in: each decoded once, then every clip, take, sampler and pad sample that expected it
// holds its audio (prepared again at sr; samplers and kits replaced whole, their tracks' MIDI clips prepared
// again). Files that can't be read stay missing and are listed in `error`. Returns how many files went in.
int apply(Project& project, const Found& found, int sr, std::string& error);

}  // namespace relink
