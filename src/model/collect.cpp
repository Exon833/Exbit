// Collect and save, and project archives (see collect.h).
#include "model/collect.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <unistd.h>

#include "core/util.h"
#include "importers/zip.h"

namespace collect {

std::set<std::string> inFolder(const std::string& jsonPath, const Project& project) {
    std::string folder = project_io::audioFolder(jsonPath);
    std::set<std::string> out;
    for (auto& c : project_io::audioHolders(project))
        if (!c->sourcePath.empty() && util::dirName(util::absolutePath(c->sourcePath)) == folder &&
            util::fileExists(c->sourcePath))
            out.insert(util::absolutePath(c->sourcePath));
    return out;
}

Summary summary(const std::string& jsonPath, const Project& project, const std::set<std::string>& before) {
    Summary s;
    s.folder = project_io::audioFolder(jsonPath);
    for (const std::string& f : inFolder(jsonPath, project)) {
        s.files++;
        s.bytes += std::max<int64_t>(0, util::fileSize(f));
        if (!before.count(f)) s.copied++;
    }
    std::set<std::string> missing;
    for (auto& c : project_io::allAudioHolders(project))
        if (c->missing) missing.insert(c->missing->path);
    s.missing = (int)missing.size();
    util::debugf("collect: %d file(s), %lld bytes in %s (%d copied now, %d missing)\n", s.files, (long long)s.bytes,
                 s.folder.c_str(), s.copied, s.missing);
    return s;
}

std::string describe(const Summary& s) {
    double mb = (double)s.bytes / (1024.0 * 1024.0);
    std::string size = mb >= 1 ? util::fixed(mb, 1) + " MB" : util::fixed((double)s.bytes / 1024.0, 1) + " KB";
    std::string text = util::format("Collected %d file%s (%s) in %s: %d copied now, the rest were there already.",
                                    s.files, s.files == 1 ? "" : "s", size.c_str(), s.folder.c_str(), s.copied);
    if (s.missing)
        text += util::format("\n\n%d sample file%s missing, so %s not collected: File → Find missing samples… looks "
                             "for %s.",
                             s.missing, s.missing == 1 ? " is" : "s are", s.missing == 1 ? "it was" : "they were",
                             s.missing == 1 ? "it" : "them");
    return text;
}

bool isArchive(const std::string& path) { return util::endsWith(util::lower(path), ".zip"); }

std::string archiveStem(const std::string& path) {
    std::string name = util::baseName(path);
    for (const char* ext : {ARCHIVE_EXTENSION, ".zip"})
        if (util::endsWith(util::lower(name), ext)) return name.substr(0, name.size() - strlen(ext));
    return util::stem(path);
}

namespace {
// what saving made in a folder of its own (and nothing else): its files, then the folders
void removeMade(const std::string& folder) {
    for (auto& e : util::listDir(folder)) {
        std::string p = util::joinPath(folder, e.name);
        if (e.isDir) removeMade(p);
        else unlink(p.c_str());
    }
    rmdir(folder.c_str());
}
}  // namespace

bool saveArchive(const std::string& zipPath, Project& project, int sr, const project_io::MasterSettings& master,
                 std::string& error, const std::function<void(int done, int total)>& progress) {
    std::string stem = archiveStem(zipPath);
    if (stem.empty()) stem = "Project";
    std::string staging = util::absolutePath(zipPath) + ".staging";
    if (util::fileExists(staging) || util::isDir(staging) || !util::makeDirs(staging)) {
        error = "Cannot make the folder " + staging + " (is it there already?)";
        return false;
    }
    std::string json = util::joinPath(staging, stem + ".json");
    // shortcut: every sample written again into the staging folder; pack the saved files directly if this is slow
    std::vector<ClipPtr> holders = project_io::audioHolders(project);
    std::map<const Clip*, std::string> kept;  // (saveProject points clips at the files it wrote: undo that)
    for (auto& c : holders) kept[c.get()] = c->sourcePath;
    auto half = [&](int base) {
        return [&progress, base](int d, int t) {
            if (progress) progress(base + (t ? d * 50 / t : 50), 100);
        };
    };
    bool ok = project_io::saveProject(json, project, sr, master, error, half(0));
    for (auto& c : holders) c->sourcePath = kept[c.get()];
    if (ok) {
        std::vector<zip::Entry> entries = {{stem + ".json", json}};
        std::string audio = project_io::audioFolder(json);
        for (auto& e : util::listDir(audio))
            if (!e.isDir) entries.push_back({stem + "_audio/" + e.name, util::joinPath(audio, e.name)});
        ok = zip::write(zipPath, entries, error, half(50));
        if (ok) util::debugf("collect: archived the project as %s (%zu files)\n", zipPath.c_str(), entries.size());
    }
    removeMade(staging);
    return ok;
}

std::string defaultFolder(const std::string& zipPath) {
    std::string dir = util::dirName(util::absolutePath(zipPath)), stem = archiveStem(zipPath);
    if (stem.empty()) stem = "Project";
    std::string folder = util::joinPath(dir, stem);
    for (int n = 2; util::fileExists(folder) || util::isDir(folder) || util::isDir(folder + ".part"); n++)
        folder = util::joinPath(dir, stem + " (" + std::to_string(n) + ")");
    return folder;
}

bool openArchive(const std::string& zipPath, const std::string& folder, std::string& jsonPath, std::string& error,
                 const std::function<void(int done, int total)>& progress) {
    std::vector<std::string> names;
    if (!zip::list(zipPath, names, error)) return false;
    std::string project;  // (a project file at its top: the one named after the archive, else the first)
    for (auto& n : names)
        if (n.find('/') == std::string::npos && util::extension(n) == ".json" &&
            (project.empty() || util::stem(n) == archiveStem(zipPath)))
            project = n;
    if (project.empty()) {
        error = "This archive holds no Tiny DAW project.";
        return false;
    }
    std::vector<std::string> files;
    if (!zip::extract(zipPath, folder, files, error, progress)) return false;
    jsonPath = util::joinPath(folder, project);
    return true;
}

}  // namespace collect
