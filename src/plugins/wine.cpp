// Wine prefixes, Windows plugin files, and starting Tiny DAW's Wine host (see wine.h).
#include "plugins/wine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/util.h"
#include "platform/platform.h"

extern char** environ;

namespace wine {

namespace {
std::mutex lock;
std::vector<std::string> chosen;
bool chosenSet = false;

std::string withoutSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}
}  // namespace

bool isPrefix(const std::string& dir) {
    return !dir.empty() && util::isDir(util::joinPath(dir, "drive_c")) && util::fileExists(util::joinPath(dir, "system.reg"));
}

std::string defaultPrefix() {
    const char* env = getenv("WINEPREFIX");
    std::string p = env && *env ? withoutSlash(util::expandUser(env)) : util::joinPath(plat::homeDir(), ".wine");
    return isPrefix(p) ? p : "";
}

std::vector<std::string> prefixes() {
    std::lock_guard<std::mutex> hold(lock);
    if (chosenSet)
        return chosen;
    std::string d = defaultPrefix();
    return d.empty() ? std::vector<std::string>{} : std::vector<std::string>{d};
}

void setPrefixes(const std::vector<std::string>& list) {
    std::lock_guard<std::mutex> hold(lock);
    chosen.clear();
    for (const auto& p : list) {
        std::string q = withoutSlash(util::expandUser(p));
        if (!q.empty() && std::find(chosen.begin(), chosen.end(), q) == chosen.end())
            chosen.push_back(q);
    }
    chosenSet = true;
}

std::string prefixOf(const std::string& path) {
    std::string p = util::absolutePath(path);
    size_t at = p.find("/drive_c/");
    while (at != std::string::npos) {
        std::string candidate = p.substr(0, at);
        if (isPrefix(candidate))
            return candidate;
        at = p.find("/drive_c/", at + 1);
    }
    return "";
}

std::string prefixFor(const std::string& path) {
    std::string p = prefixOf(path);
    if (!p.empty())
        return p;
    for (const auto& q : prefixes())
        if (isPrefix(q))
            return q;
    return defaultPrefix();
}

std::vector<std::string> vst3Folders(const std::vector<std::string>& list) {
    std::vector<std::string> out;
    for (const auto& prefix : list) {
        std::string f = util::joinPath(prefix, "drive_c/Program Files/Common Files/VST3");
        if (util::isDir(f))
            out.push_back(f);
    }
    return out;
}

std::vector<std::string> vst2UsualFolders(const std::vector<std::string>& list) {
    static const char* usual[] = {"drive_c/Program Files/VstPlugins", "drive_c/Program Files/Steinberg/VstPlugins",
                                  "drive_c/Program Files/Common Files/VST2",
                                  "drive_c/Program Files/Common Files/Steinberg/VST2"};
    std::vector<std::string> out;
    for (const auto& prefix : list)
        for (const char* u : usual) {
            std::string f = util::joinPath(prefix, u);
            if (util::isDir(f))
                out.push_back(f);
        }
    return out;
}

bool isWindowsBinary(const std::string& file, int* machine) {
    if (machine)
        *machine = 0;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f)
        return false;
    unsigned char dos[64];
    bool ok = fread(dos, 1, sizeof(dos), f) == sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z';
    if (ok) {
        uint32_t at = dos[60] | dos[61] << 8 | dos[62] << 16 | (uint32_t)dos[63] << 24;
        unsigned char pe[6];
        ok = at < (1u << 20) && fseek(f, (long)at, SEEK_SET) == 0 && fread(pe, 1, sizeof(pe), f) == sizeof(pe) &&
             std::memcmp(pe, "PE\0\0", 4) == 0;
        if (ok && machine)
            *machine = pe[4] | pe[5] << 8;
    }
    fclose(f);
    return ok;
}

bool isWindowsPlugin(const std::string& path) {
    if (util::isDir(path))
        return false;
    return isWindowsBinary(path);
}

bool looksWindows(const std::string& path) {
    return path.find("/drive_c/") != std::string::npos || util::endsWith(util::lower(path), ".dll");
}

namespace {
bool executable(const std::string& p) { return !p.empty() && access(p.c_str(), X_OK) == 0 && !util::isDir(p); }
}  // namespace

std::string winePath() {
    if (const char* env = getenv("TINYDAW_WINE"))
        if (*env)
            return executable(env) ? env : "";
    if (const char* path = getenv("PATH"))
        for (const auto& dir : util::split(path, ':'))
            if (!dir.empty() && executable(util::joinPath(dir, "wine")))
                return util::joinPath(dir, "wine");
    for (const char* p : {"/opt/wine-staging/bin/wine", "/opt/wine-stable/bin/wine", "/opt/wine-devel/bin/wine",
                          "/usr/bin/wine", "/usr/local/bin/wine"})
        if (executable(p))
            return p;
    return "";
}

std::string hostPath() {
    if (const char* env = getenv("TINYDAW_WINE_HOST"))
        if (*env)
            return util::fileExists(env) ? env : "";
    std::string exe = plat::executableDir();
    for (const char* rel : {"", "..", "../..", "../lib/tinydaw"}) {
        std::string dir = *rel ? util::joinPath(exe, rel) : exe;
        std::string p = util::joinPath(dir, "tinydaw-wine-host.exe");
        if (util::fileExists(p) && util::fileExists(p + ".so"))
            return util::absolutePath(p);
    }
    return "";
}

bool available(std::string& why) {
    if (winePath().empty()) {
        why = "Windows plugins need Wine, which isn't installed (or set TINYDAW_WINE to the wine program)";
        return false;
    }
    if (hostPath().empty()) {
        why = "Windows plugins need tinydaw-wine-host.exe next to Tiny DAW (it is built by make when winegcc is "
              "installed)";
        return false;
    }
    return true;
}

namespace {

// runs `wine <args>` in a prefix and waits for it (up to `seconds`); true if it ended well
bool runWine(const std::string& prefix, const std::vector<std::string>& args, double seconds) {
    std::string wine = winePath();
    if (wine.empty())
        return false;
    std::vector<std::string> env;
    for (char** e = environ; *e; e++)
        if (!util::startsWith(*e, "WINEPREFIX=") && !util::startsWith(*e, "WINEDEBUG="))
            env.push_back(*e);
    env.push_back("WINEPREFIX=" + prefix);
    env.push_back("WINEDEBUG=-all");
    std::vector<std::string> argv = {wine};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> a, e;
    for (auto& s : argv)
        a.push_back(&s[0]);
    a.push_back(nullptr);
    for (auto& s : env)
        e.push_back(&s[0]);
    e.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    int r = posix_spawn(&pid, wine.c_str(), &actions, nullptr, a.data(), e.data());
    posix_spawn_file_actions_destroy(&actions);
    if (r != 0)
        return false;
    for (int waited = 0; waited < (int)(seconds * 100); waited++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    return false;
}

}  // namespace

// OpenGL through GLX, not EGL, for the host: its own settings in the prefix
// (HKCU\Software\Wine\AppDefaults\tinydaw-wine-host.exe[.so]\X11 Driver: UseEGL = N; the rest of
// the prefix keeps its own). Wine 11 starts OpenGL through EGL, which NVIDIA's drivers before 560
// can't give X11 windows under XWayland: every plugin's drawing (OpenGL, and Direct3D and Direct2D
// on top of it) then falls back to Mesa's software rasterizer, llvmpipe, at several CPU cores and a
// fraction of the frame rate. Through GLX it is the graphics card's, with any driver. Set before
// the first host starts in a prefix (Wine reads it as a program starts), once; a value already
// there (the user's) is left alone; TINYDAW_WINE_EGL=1: not set.
void preferGlx(const std::string& prefix) {
    static std::mutex once;
    static std::vector<std::string> done;
    std::lock_guard<std::mutex> hold(once);
    if (prefix.empty() || getenv("TINYDAW_WINE_EGL") || std::find(done.begin(), done.end(), prefix) != done.end())
        return;
    done.push_back(prefix);
    std::string reg;
    util::readFile(util::joinPath(prefix, "user.reg"), reg);
    for (const char* app : {"tinydaw-wine-host.exe", "tinydaw-wine-host.exe.so"}) {
        std::string key = std::string("AppDefaults\\\\") + app + "\\\\X11 Driver]";
        if (reg.find(key) != std::string::npos)
            continue;
        runWine(prefix, {"reg", "add", std::string("HKCU\\Software\\Wine\\AppDefaults\\") + app + "\\X11 Driver", "/v",
                         "UseEGL", "/d", "N", "/f"},
                20);
    }
}

int spawnHost(const std::string& prefix, const std::vector<std::string>& args, int outFd, std::string& why) {
    if (!available(why))
        return -1;
    preferGlx(prefix);
    std::string wine = winePath(), host = hostPath();
    std::vector<std::string> env;
    std::string overrides = "winedbg.exe=d";  // (a crashing plugin ends its host, with no Wine crash dialog)
    for (char** e = environ; *e; e++) {
        std::string s = *e;
        if (util::startsWith(s, "WINEPREFIX="))
            continue;
        if (util::startsWith(s, "WINEDLLOVERRIDES=")) {
            overrides = s.substr(17) + ";" + overrides;
            continue;
        }
        env.push_back(s);
    }
    if (!prefix.empty())
        env.push_back("WINEPREFIX=" + prefix);
    env.push_back("WINEDLLOVERRIDES=" + overrides);
    if (!getenv("WINEDEBUG"))
        env.push_back("WINEDEBUG=-all");
    std::vector<std::string> argv = {wine, host};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> a, e;
    for (auto& s : argv)
        a.push_back(&s[0]);
    a.push_back(nullptr);
    for (auto& s : env)
        e.push_back(&s[0]);
    e.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0)
        posix_spawn_file_actions_adddup2(&actions, devnull, 0);
    if (outFd >= 0)
        posix_spawn_file_actions_adddup2(&actions, outFd, 1);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    pid_t pid = -1;
    int r = posix_spawn(&pid, wine.c_str(), &actions, &attr, a.data(), e.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (devnull >= 0)
        close(devnull);
    if (r != 0) {
        why = "Cannot start Wine (" + wine + "): " + strerror(r);
        return -1;
    }
    return pid;
}

}  // namespace wine
