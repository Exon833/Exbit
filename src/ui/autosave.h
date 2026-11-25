// Autosave and crash recovery (autosave.py).
//
// While the project has unsaved changes, it is written every AUTOSAVE_S to a recovery folder of this
// run (in the program's data folder: recovery/<run>/), with any audio not saved anywhere yet
// (project_io::describeRecovery / writeRecovery). The audio files are written on a worker thread, one
// autosave at a time, so the window never waits for them; the JSON is described on the GUI thread,
// where the project is changed. Saving, opening another project, New, and closing the program normally
// (also when the changes are discarded) empty the folder: what it held is saved or was given up.
//
// Each run holds a lock on a file in its folder (flock: the system lets go of it when the program ends
// however it ends), so several copies of Exbit at once each keep their own. A folder whose lock nobody holds
// belongs to a run that didn't end normally (a crash, a plugin that took the program down, a power
// cut): on start, if one holds work, Exbit offers to open it (Open it / Discard it / Not now).
// Opened, it becomes this run's folder, and its project counts as unsaved (Save puts it back where it
// came from).
#pragma once
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/json.h"
#include "model/project_io.h"

namespace ui {
class Window;
}

namespace autosave {

constexpr int AUTOSAVE_S = 60;
constexpr const char* PROJECT_FILE = "recovered.json";
constexpr const char* AUDIO_FOLDER = "recovered_audio";
constexpr const char* LOCK_FILE = "lock";

std::string recoveryDir();  // paths::dataDir("recovery")
// remove a folder and everything in it (true if it is gone)
bool removeTree(const std::string& path);

// What the recovery file says about itself
struct Header {
    double savedAt = 0;
    std::string recoveryOf;  // the project file it was a changed version of ("" an unsaved project)
    int tracks = 0, clips = 0;
};
bool readHeader(const std::string& path, Header& out);
// Recovery folders of runs that didn't end normally and hold work, newest first (folders without
// work are removed)
struct LeftBehind {
    std::string folder;
    Header header;
};
std::vector<LeftBehind> leftBehind(const std::string& root);

// What autosave needs of the main window
struct Host {
    ui::Window* window = nullptr;
    std::function<bool()> hasUnsavedChanges;
    std::function<Json()> snapshot;
    std::function<bool()> recordingActive;
    std::function<bool()> pluginsEdited;
    std::function<std::string()> projectPath;
    // the project for a recovery file (project_io::describeRecovery with the window's project, sample
    // rate and master settings)
    std::function<Json(const std::string& audioFolder, std::map<std::string, std::string>& written,
                       std::vector<project_io::PendingAudio>& toWrite)>
        describe;
    std::function<void(const std::string& text, int ms)> showMessage;
    // recovering: may the current project be replaced (saving it first if asked)? Then open the file
    std::function<bool()> confirmDiscard;
    std::function<bool(const std::string& path)> openProjectFile;
    // after opening it: the project's file is `original` ("" untitled), unsaved; the title says "(recovered)"
    std::function<void(const std::string& original)> recovered;
};

class Autosave {
public:
    // root: the recovery folder ("" = recoveryDir()); timer: autosave every AUTOSAVE_S (tests: false)
    explicit Autosave(Host host, const std::string& root = "", bool timer = true);
    ~Autosave();

    const std::string& folder() const { return folder_; }  // this run's ("" none yet)
    // this run's recovery folder, made (and locked) the first time it is needed ("" if it can't be made)
    std::string ownFolder();
    // make `folder` this run's (lock it, or take `held`: a lock on it this run already holds); the one this run
    // had, if any, goes. False if another holds it.
    bool adopt(const std::string& folder, int held = -1);
    void release(bool remove);

    // let the autosave being written finish; its error ("" none)
    std::string wait();
    // every AUTOSAVE_S: write the project if it changed since the last autosave and isn't saved
    void tick();
    // write the project to the recovery folder now (its audio on the worker thread)
    void saveNow(const Json* snapshot = nullptr);
    bool writing() const { return pending_.valid(); }
    // the project was saved, replaced or given up: what this run kept goes (the folder stays, empty)
    void clear();
    // the program ends normally: this run's folder goes
    void close();

    // on start: if a run that didn't end normally kept work, offer to open it (the newest; older ones
    // are kept for the next start). True if it was opened. The folder is held (locked) while asking, so
    // that another Exbit starting meanwhile doesn't offer it too.
    bool offerRecovery();
    // open the work kept in `folder` (as unsaved changes to `original`, if it had a file); `held`: the lock
    // on it, if this run holds it already. It becomes this run's autosave, written again at once (dated
    // now: the question about the old one doesn't come back). False if another Exbit holds it.
    bool recover(const std::string& folder, const std::string& original, int held = -1);
    // (tests and the test script: answers the question without asking: 0 open, 1 discard, 2 not now; -1 ask)
    int answer = -1;

private:
    Host host_;
    std::string root_, folder_;
    int lockFd_ = -1;
    std::map<std::string, std::string> written_;  // fingerprint -> audio file written in the folder
    std::unique_ptr<Json> last_;                  // the snapshot autosaved last
    std::future<std::string> pending_;            // the autosave being written (its error)
    int timer_ = -1;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace autosave
