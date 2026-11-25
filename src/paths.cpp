#include "paths.h"

#include <cstdlib>
#include <mutex>

#include "core/os.h"
#include "core/util.h"

namespace paths {

namespace {
std::mutex lock;
std::string override_;
}  // namespace

void setDataDir(const std::string& dir) {
    std::lock_guard<std::mutex> hold(lock);
    override_ = dir;
}

std::string dataDir(const std::string& sub) {
    std::string base;
    {
        std::lock_guard<std::mutex> hold(lock);
        base = override_;
    }
    if (base.empty()) {
#ifdef _WIN32
        const char* appdata = getenv("APPDATA");
        base = util::joinPath(appdata && *appdata ? os::portablePath(appdata) : util::expandUser("~"), "Exbit");
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        base = util::joinPath(xdg && *xdg ? std::string(xdg) : util::expandUser("~/.local/share"), "Exbit");
#endif
    }
    return sub.empty() ? base : util::joinPath(base, sub);
}

std::string makeDataDir(const std::string& sub) {
    std::string dir = dataDir(sub);
    if (util::isDir(dir) || util::makeDirs(dir)) return dir;
    return util::isDir(dir) ? dir : std::string();
}

}  // namespace paths
