// Keeping a project whole: File → Collect and save, and project archives (no GUI code: main_window asks).
//
// COLLECT AND SAVE. A saved project keeps every sample it uses in its own audio folder ("<name>_audio" beside
// it: project_io.h), which is the project's samples folder: saving copies in whatever isn't there yet (clips,
// takes, samplers' and Pad Kit pads' samples, frozen renderings, from anywhere), one file per sound (identical
// audio once, a different sound with a taken name under a new name), and leaves those already there as they
// are. Nothing is moved: the originals stay where they were, untouched. inFolder() before saving and summary()
// after tell what it did: how many files and bytes the folder holds for the project, how many were copied in
// now, and how many samples are still missing (they can't be copied: relink.h finds them).
//
// ARCHIVES (".exbit.zip", zip.h): the project file and every sample in one file, to send or keep. Inside:
// "<name>.json" and "<name>_audio/…", as a saved project is laid out, so an archive unpacks into a folder
// that opens as a project. saveArchive() saves the project into a folder of its own beside the archive
// ("<archive>.staging", removed again; the project itself is left as it was), then packs it. openArchive()
// unpacks into a new folder (defaultFolder(): beside the archive, named after it) and gives the project file.
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "model/project_io.h"

namespace collect {

// the files of `project` already in the audio folder of the project file at `jsonPath`
std::set<std::string> inFolder(const std::string& jsonPath, const Project& project);

struct Summary {
    std::string folder;   // the audio folder
    int files = 0;        // files there the project uses
    int64_t bytes = 0;    // their size
    int copied = 0;       // of which copied in by this save (not in `before`)
    int missing = 0;      // missing files (relink.h), not collected
};
// after saving to `jsonPath`; `before`: inFolder() before it
Summary summary(const std::string& jsonPath, const Project& project, const std::set<std::string>& before);
// "Collected 12 files (34.5 MB) in …: 3 copied now." (+ the missing ones)
std::string describe(const Summary& s);

constexpr const char* ARCHIVE_EXTENSION = ".exbit.zip";
bool isArchive(const std::string& path);  // its name ends in .exbit.zip (or .zip: the program's old name's too)
std::string archiveStem(const std::string& path);  // "a/Song.exbit.zip" -> "Song" (an old name's archive too)
// the file chooser's patterns for archives: "*.exbit.zip", and the extension of the program's old name
std::string archivePatterns();

// the project and all its samples as one archive at `zipPath` (the project itself unchanged)
bool saveArchive(const std::string& zipPath, Project& project, int sr, const project_io::MasterSettings& master,
                 std::string& error, const std::function<void(int done, int total)>& progress = nullptr);
// where to unpack it: beside it, named after it ("Song", or "Song (2)"… when that's taken)
std::string defaultFolder(const std::string& zipPath);
// unpacked into `folder` (new); `jsonPath`: the project file in it
bool openArchive(const std::string& zipPath, const std::string& folder, std::string& jsonPath, std::string& error,
                 const std::function<void(int done, int total)>& progress = nullptr);

}  // namespace collect
