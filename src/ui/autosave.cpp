// Autosave and crash recovery (autosave.py). See autosave.h.
#include "ui/autosave.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "core/util.h"
#include "paths.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

namespace autosave {

std::string recoveryDir() { return paths::dataDir("recovery"); }

bool removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        for (auto& e : util::listDir(path)) removeTree(util::joinPath(path, e.name));
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

namespace {

// a lock on <folder>/lock: held (by this run or another) or not. Returns the fd holding it, or -1.
int lockFolder(const std::string& folder) {
    std::string path = util::joinPath(folder, LOCK_FILE);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    std::string pid = std::to_string(getpid()) + "\n";  // (who holds it, for people looking)
    if (ftruncate(fd, 0) == 0) {
        ssize_t n = write(fd, pid.data(), pid.size());
        (void)n;
    }
    return fd;
}

void unlockFolder(int fd) {
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    ::close(fd);
}

std::string plural(int n, const char* word) { return std::to_string(n) + " " + word + (n != 1 ? "s" : ""); }

}  // namespace

bool readHeader(const std::string& path, Header& out) {
    std::string text;
    if (!util::readFile(path, text)) return false;
    std::string error;
    Json data = Json::parse(text, &error);
    if (!error.empty() || !data.isObject()) return false;
    out.savedAt = data["saved_at"].isNumber() && data["saved_at"].toDouble() > 0 ? data["saved_at"].toDouble()
                                                                                 : util::fileMtime(path);
    out.recoveryOf = data["recovery_of"].isString() ? data["recovery_of"].toString() : "";
    out.tracks = (int)data["tracks"].size();
    out.clips = (int)data["clips"].size();
    return true;
}

std::vector<LeftBehind> leftBehind(const std::string& root) {
    std::vector<LeftBehind> found;
    if (!util::isDir(root)) return found;
    for (auto& e : util::listDir(root)) {
        std::string folder = util::joinPath(root, e.name);
        if (!e.isDir) continue;
        int fd = lockFolder(folder);
        if (fd < 0) continue;  // (another Exbit running now holds it, or it can't be looked at)
        unlockFolder(fd);
        Header h;
        if (!readHeader(util::joinPath(folder, PROJECT_FILE), h)) removeTree(folder);  // (nothing in it worth keeping)
        else found.push_back({folder, h});
    }
    std::sort(found.begin(), found.end(),
              [](const LeftBehind& a, const LeftBehind& b) { return a.header.savedAt > b.header.savedAt; });
    return found;
}

// --------------------------------------------------------------------------------------------

Autosave::Autosave(Host host, const std::string& root, bool timer)
    : host_(std::move(host)), root_(root.empty() ? recoveryDir() : root) {
    if (timer) {
        std::weak_ptr<bool> alive = alive_;
        timer_ = ui::App::get().every(AUTOSAVE_S * 1000, [this, alive] {
            if (!alive.expired()) tick();
        });
    }
}

Autosave::~Autosave() {
    *alive_ = false;
    if (timer_ >= 0) ui::App::get().cancelTimer(timer_);
    wait();
    unlockFolder(lockFd_);  // (the folder stays: not a normal close() — e.g. tests)
}

// --- this run's folder ---

std::string Autosave::ownFolder() {
    if (folder_.empty()) {
        char stamp[64];
        std::time_t t = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", std::localtime(&t));
        std::string folder = util::joinPath(root_, std::string(stamp) + " " + std::to_string(getpid()));
        if (!util::makeDirs(folder) || !adopt(folder)) return "";
    }
    return folder_;
}

bool Autosave::adopt(const std::string& folder, int held) {
    if (folder == folder_) return true;
    int fd = held >= 0 ? held : lockFolder(folder);
    if (fd < 0) return false;  // (in use by another Exbit)
    if (!folder_.empty()) release(true);
    folder_ = folder;
    lockFd_ = fd;
    written_.clear();
    return true;
}

void Autosave::release(bool remove) {
    if (folder_.empty()) return;
    wait();
    unlockFolder(lockFd_);
    lockFd_ = -1;
    if (remove) removeTree(folder_);
    folder_.clear();
}

// --- autosaving ---

std::string Autosave::wait() {
    if (!pending_.valid()) return "";
    try {
        return pending_.get();
    } catch (const std::exception& e) {
        return e.what();
    }
}

void Autosave::tick() {
    if (pending_.valid()) {
        if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;  // (the last one is still being written: next time)
        std::string error = wait();
        if (!error.empty() && host_.showMessage) host_.showMessage("Autosave failed: " + error, 8000);
    }
    if ((host_.recordingActive && host_.recordingActive()) || !host_.hasUnsavedChanges()) return;
    Json snap = host_.snapshot();
    if (last_ && *last_ == snap && !(host_.pluginsEdited && host_.pluginsEdited())) return;
    saveNow(&snap);
}

void Autosave::saveNow(const Json* snapshot) {
    std::string folder = ownFolder();
    if (folder.empty()) {
        if (host_.showMessage) host_.showMessage("Autosave failed: can't make a folder in " + root_, 8000);
        return;
    }
    wait();
    std::vector<project_io::PendingAudio> toWrite;
    Json data = host_.describe(util::joinPath(folder, AUDIO_FOLDER), written_, toWrite);
    data.set("saved_at", util::wallClock());
    std::string of = host_.projectPath ? host_.projectPath() : "";
    data.set("recovery_of", of.empty() ? Json() : Json(of));
    last_ = std::make_unique<Json>(snapshot ? *snapshot : host_.snapshot());
    std::string path = util::joinPath(folder, PROJECT_FILE);
    pending_ = std::async(std::launch::async, [path, data, toWrite]() -> std::string {
        std::string error;
        if (!project_io::writeRecovery(path, data, toWrite, error)) return error.empty() ? "couldn't write it" : error;
        return "";
    });
}

void Autosave::clear() {
    wait();
    last_.reset();
    if (folder_.empty()) return;
    for (auto& e : util::listDir(folder_)) {
        if (util::startsWith(e.name, LOCK_FILE)) continue;  // (the lock)
        removeTree(util::joinPath(folder_, e.name));
    }
    written_.clear();
}

void Autosave::close() {
    if (timer_ >= 0) {
        ui::App::get().cancelTimer(timer_);
        timer_ = -1;
    }
    release(true);
}

// --- after a crash ---

namespace {
// "Recover your work?" with Open it / Discard it / Not now: 0, 1, 2
int askRecovery(ui::Window* parent, const std::string& text, const std::string& more) {
    ui::Dialog d(parent, "Recover your work?", 440, 160);
    d.setMinimumWindowSize(0, 0);
    d.addStyleClass("recovery");  // (default.css: Dialog.recovery, its room; ::text, ::buttons)
    auto* col = ui::styleRoom(ui::vbox(&d, 14, 10), d.computedStyle());
    auto* lab = d.add<ui::Label>(text);
    lab->setWordWrap(true);
    lab->setAlignment(AlignLeft | AlignTop);
    lab->setMinimumWidth(std::max(0, ui::style::px(d.computedStyle("text"), "min-width", 420)));
    col->addWidget(lab);
    auto* info = d.add<ui::Label>(more);
    info->setWordWrap(true);
    info->setAlignment(AlignLeft | AlignTop);
    col->addWidget(info);
    col->addSpacing(std::max(0, ui::style::px(d.computedStyle("buttons"), "margin-top", 6)));
    col->addStretch(1);
    auto* box = d.add<ui::ButtonBox>(0);
    int chosen = 2;
    ui::Button* open = box->addButton("Open it", 1);
    ui::Button* discard = box->addButton("Discard it", 2);
    ui::Button* later = box->addButton("Not now", 0);
    open->clicked.connect([&] { chosen = 0; d.accept(); });
    discard->clicked.connect([&] { chosen = 1; d.accept(); });
    later->clicked.connect([&] { chosen = 2; d.reject(); });
    open->setDefault(true);
    open->setFocus();
    col->addWidget(box);
    d.adjustSize();
    d.exec();
    return chosen;
}
}  // namespace

bool Autosave::offerRecovery() {
    std::vector<LeftBehind> found = leftBehind(root_);
    // the newest one no other Exbit is offering or has opened meanwhile: held while asking
    size_t at = 0;
    int fd = -1;
    for (; at < found.size(); at++)
        if ((fd = lockFolder(found[at].folder)) >= 0) break;
    if (fd < 0) return false;
    const LeftBehind& newest = found[at];
    util::debugf("autosave: offering %s (%zu kept in all)\n", newest.folder.c_str(), found.size());
    char when[64];
    std::time_t t = (std::time_t)newest.header.savedAt;
    std::strftime(when, sizeof when, "%d %B %Y, %H:%M", std::localtime(&t));
    const std::string& of = newest.header.recoveryOf;
    std::string what = !of.empty() ? "your changes to " + util::baseName(of) : "an unsaved project";
    std::string text = "Exbit didn't close normally last time. It kept " + what + ", as it was at " + when + " (" +
                       plural(newest.header.tracks, "track") + ", " + plural(newest.header.clips, "clip") + ").";
    std::string more = "Open it now? You can then save it.";
    if (found.size() > 1) {
        size_t n = found.size() - 1;
        more += "\n\n" + std::to_string(n) + " older one" + (n > 1 ? "s are" : " is") + " also kept, for after this one.";
    }
    int chosen = answer >= 0 ? answer : askRecovery(host_.window, text, more);
    util::debugf("autosave: %s: %s\n", newest.folder.c_str(), chosen == 0 ? "open it" : chosen == 1 ? "discard it" : "not now");
    if (chosen == 0) return recover(newest.folder, of, fd);
    unlockFolder(fd);
    if (chosen == 1 && !removeTree(newest.folder)) {
        // (something in it can't be removed now, e.g. a file another program holds open on Windows): its project
        // file at least goes, so that it isn't offered again; the rest goes at a later start (leftBehind)
        bool gone = util::removeFile(util::joinPath(newest.folder, PROJECT_FILE));
        util::debugf("autosave: %s not removed whole; its project file %s\n", newest.folder.c_str(),
                     gone ? "removed" : "couldn't be removed either");
    }
    return false;
}

bool Autosave::recover(const std::string& folder, const std::string& original, int held) {
    int fd = held >= 0 ? held : lockFolder(folder);
    if (fd < 0) {
        util::debugf("autosave: %s is held by another Exbit: not opened\n", folder.c_str());
        if (host_.showMessage) host_.showMessage("That work is open in another Exbit", 8000);
        return false;
    }
    if ((host_.confirmDiscard && !host_.confirmDiscard()) ||
        !host_.openProjectFile(util::joinPath(folder, PROJECT_FILE))) {
        unlockFolder(fd);
        return false;
    }
    adopt(folder, fd);  // (its audio files are where the recovered clips' audio comes from)
    std::string path = !original.empty() && util::isDir(util::dirName(original)) ? original : "";
    if (host_.recovered) host_.recovered(path);
    saveNow();  // (this run's autosave now, dated now: if this run doesn't end normally either, it is what's offered)
    return true;
}

}  // namespace autosave
