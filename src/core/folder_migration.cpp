#include "core/folder_migration.h"

#include <cerrno>
#include <cstring>
#include <mutex>

#include "core/util.h"

namespace folder_migration {

bool copyFolderContents(const std::string& from, const std::string& to) {
    if (!util::isDir(to) && !util::makeDirs(to)) return false;
    bool copiedAll = true;
    util::walkFiles(from, [&](const std::string& path) {
        std::string target = util::joinPath(to, path.substr(from.size() + 1));
        std::string targetFolder = util::dirName(target);
        std::string contents;
        copiedAll = util::readFile(path, contents) && (util::isDir(targetFolder) || util::makeDirs(targetFolder)) &&
                    util::writeFile(target, contents);
        if (!copiedAll) util::debugf("folders: couldn't copy %s to %s\n", path.c_str(), target.c_str());
        return copiedAll;
    });
    return copiedAll;
}

std::string useNewFolderOrMoveOldOne(const std::string& newFolder, const std::string& oldFolder) {
    static std::mutex oneAtATime;  // (two threads asking at once: one moves it, the other then finds it moved)
    std::lock_guard<std::mutex> hold(oneAtATime);
    if (util::isDir(newFolder) || !util::isDir(oldFolder)) return newFolder;
    if (util::renameFile(oldFolder, newFolder)) {
        util::debugf("folders: %s (the program's old name) moved to %s\n", oldFolder.c_str(), newFolder.c_str());
        return newFolder;
    }
    util::debugf("folders: couldn't move %s to %s (%s): copying it\n", oldFolder.c_str(), newFolder.c_str(),
                 strerror(errno));
    // copied beside the new folder first, then renamed: a copy cut short never passes for a finished one
    std::string unfinished = newFolder + ".copying";
    if (copyFolderContents(oldFolder, unfinished) && util::renameFile(unfinished, newFolder)) {
        util::debugf("folders: %s copied to %s (the old folder kept)\n", oldFolder.c_str(), newFolder.c_str());
        return newFolder;
    }
    util::debugf("folders: couldn't copy %s to %s either: the old folder is used as it is\n", oldFolder.c_str(),
                 newFolder.c_str());
    return oldFolder;
}

}  // namespace folder_migration
