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

std::string prefixArch(const std::string& prefix) {
    // (system.reg starts with "WINE REGISTRY Version 2", a comment, then "#arch=win64")
    FILE* f = fopen(util::joinPath(prefix, "system.reg").c_str(), "rb");
    if (!f)
        return "";
    char head[4096];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    std::string text(head, n);
    size_t at = text.find("\n#arch=");
    if (at == std::string::npos)
        return "win64";  // (written by every Wine since 2.x; a prefix without it is from before 32-bit-only ones were usual)
    size_t start = at + 7, end = text.find_first_of("\r\n", start);
    return util::trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

std::vector<std::string> vst3Folders(const std::vector<std::string>& list) {
    std::vector<std::string> out;
    for (const auto& prefix : list)  // (and 32-bit plugins' own, in a 64-bit prefix)
        for (const char* v : {"drive_c/Program Files/Common Files/VST3", "drive_c/Program Files (x86)/Common Files/VST3"}) {
            std::string f = util::joinPath(prefix, v);
            if (util::isDir(f))
                out.push_back(f);
        }
    return out;
}

std::vector<std::string> vst2UsualFolders(const std::vector<std::string>& list) {
    static const char* usual[] = {"drive_c/Program Files/VstPlugins", "drive_c/Program Files/Steinberg/VstPlugins",
                                  "drive_c/Program Files/Common Files/VST2",
                                  "drive_c/Program Files/Common Files/Steinberg/VST2",
                                  // (32-bit plugins', in a 64-bit prefix)
                                  "drive_c/Program Files (x86)/VstPlugins",
                                  "drive_c/Program Files (x86)/Steinberg/VstPlugins",
                                  "drive_c/Program Files (x86)/Common Files/VST2",
                                  "drive_c/Program Files (x86)/Common Files/Steinberg/VST2"};
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

bool is32BitPlugin(const std::string& file) {
    int machine = 0;
    return isWindowsBinary(file, &machine) && machine == 0x14c;
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

std::string hostPath(bool bits32) {
    if (const char* env = getenv(bits32 ? "TINYDAW_WINE_HOST32" : "TINYDAW_WINE_HOST"))
        if (*env)
            return util::fileExists(env) ? env : "";
    std::string exe = plat::executableDir();
    for (const char* rel : {"", "..", "../..", "../lib/tinydaw"}) {
        std::string dir = *rel ? util::joinPath(exe, rel) : exe;
        std::string p = util::joinPath(dir, bits32 ? "tinydaw-wine-host-32.exe" : "tinydaw-wine-host.exe");
        if (util::fileExists(p) && util::fileExists(p + ".so"))
            return util::absolutePath(p);
    }
    return "";
}

// --- the wine program of a prefix ---

namespace {

std::map<std::string, std::string> programs;  // (setWinePrograms; under `lock`)

std::string normPrefix(const std::string& p) { return withoutSlash(util::expandUser(p)); }

std::string dataHome() {
    const char* x = getenv("XDG_DATA_HOME");
    return x && *x ? std::string(x) : util::joinPath(plat::homeDir(), ".local/share");
}

std::string configHome() {
    const char* x = getenv("XDG_CONFIG_HOME");
    return x && *x ? std::string(x) : util::joinPath(plat::homeDir(), ".config");
}

std::string unquote(std::string s) {
    s = util::trim(s);
    if (s.size() >= 2 && (s[0] == '"' || s[0] == '\'') && s.back() == s[0])
        s = s.substr(1, s.size() - 2);
    return s;
}

// a value of a small YAML file (Bottles' and Lutris' settings): `key` at the top level (section "")
// or one level inside `section:`. Only plain "key: value" lines; enough for these files.
std::string yamlValue(const std::string& text, const std::string& section, const std::string& key) {
    std::string in;  // the top-level section we are in
    for (std::string line : util::split(text, '\n')) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string t = util::trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        bool top = line[0] != ' ' && line[0] != '\t';
        size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        std::string k = unquote(t.substr(0, colon)), v = t.substr(colon + 1);
        if (top)
            in = k;
        if (k != key)
            continue;
        if ((section.empty() && top) || (!section.empty() && !top && in == section))
            return unquote(v);
    }
    return "";
}

struct Found {
    std::string wine;    // the program ("" none)
    std::string source;  // for people (wineSource)
};

// a Bottles bottle is a prefix of its own whose bottle.yml names its runner ("Runner: soda-7.0-9"), in
// <Bottles' data>/runners/<runner>/bin/wine; Bottles' data is the bottle's grandparent
// (<data>/bottles/<name>), else the usual places (installed, or as a Flatpak). "sys-…": the system's.
Found bottles(const std::string& prefix) {
    std::string yml;
    if (!util::readFile(util::joinPath(prefix, "bottle.yml"), yml))
        return {};
    std::string runner = yamlValue(yml, "", "Runner");
    if (runner.empty() || runner.find('/') != std::string::npos)
        return {};
    if (util::startsWith(runner, "sys-"))
        return {winePath(), "Bottles: " + runner};
    std::vector<std::string> roots = {util::dirName(util::dirName(prefix)), util::joinPath(dataHome(), "bottles"),
                                      util::joinPath(plat::homeDir(), ".var/app/com.usebottles.bottles/data/bottles")};
    for (const auto& root : roots) {
        std::string w = util::joinPath(root, "runners/" + runner + "/bin/wine");
        if (access(w.c_str(), X_OK) == 0)
            return {w, "Bottles: " + runner};
    }
    return {"", "Bottles: " + runner + " isn't installed"};
}

// a Lutris game's settings (<config>/lutris/games/*.yml, or <data>/lutris/games) name its prefix
// (game: prefix:) and maybe its Wine (wine: version:), else the Wine runner's own (runners/wine.yml);
// that Wine is <data>/lutris/runners/wine/<version>/bin/wine ("system": the system's)
Found lutris(const std::string& prefix) {
    std::string target = normPrefix(prefix);
    for (const auto& base : {configHome(), dataHome()}) {
        std::string games = util::joinPath(base, "lutris/games");
        for (const auto& e : util::listDir(games)) {
            if (!util::endsWith(e.name, ".yml"))
                continue;
            std::string yml;
            if (!util::readFile(util::joinPath(games, e.name), yml))
                continue;
            std::string p = yamlValue(yml, "game", "prefix");
            if (p.empty() || normPrefix(p) != target)
                continue;
            std::string version = yamlValue(yml, "wine", "version");
            if (version.empty()) {
                std::string runner;
                util::readFile(util::joinPath(configHome(), "lutris/runners/wine.yml"), runner);
                version = yamlValue(runner, "wine", "version");
            }
            if (version.empty() || version == "system")
                return {winePath(), "Lutris: the system's Wine"};
            if (version.find('/') != std::string::npos)
                return {};
            std::string w = util::joinPath(dataHome(), "lutris/runners/wine/" + version + "/bin/wine");
            if (access(w.c_str(), X_OK) == 0)
                return {w, "Lutris: " + version};
            return {"", "Lutris: " + version + " isn't installed"};
        }
    }
    return {};
}

Found automatic(const std::string& p) {
    if (p.empty())
        return {winePath(), ""};
    Found f = bottles(p);
    if (f.source.empty())
        f = lutris(p);
    if (f.source.empty())
        return {winePath(), ""};
    if (f.wine.empty() || wineVersion(f.wine).empty()) {  // (missing, or one that can't run outside its program)
        std::string why = f.wine.empty() ? f.source : f.source + " doesn't run outside it";
        return {winePath(), why + "; the usual Wine instead"};
    }
    return f;
}

Found findWine(const std::string& prefix) {
    std::string p = normPrefix(prefix);
    {
        std::lock_guard<std::mutex> hold(lock);
        auto it = programs.find(p);
        if (it != programs.end() && !it->second.empty())
            return {it->second, "chosen"};
    }
    return automatic(p);
}

}  // namespace

std::string winePathFor(const std::string& prefix) { return findWine(prefix).wine; }

std::string wineSource(const std::string& prefix) { return findWine(prefix).source; }

std::string automaticWine(const std::string& prefix, std::string& source) {
    Found f = automatic(normPrefix(prefix));
    source = f.source;
    return f.wine;
}

void setWinePrograms(const std::map<std::string, std::string>& chosen) {
    std::lock_guard<std::mutex> hold(lock);
    programs.clear();
    for (const auto& [prefix, program] : chosen)
        if (!prefix.empty() && !program.empty())
            programs[normPrefix(prefix)] = util::expandUser(program);
}

std::map<std::string, std::string> winePrograms() {
    std::lock_guard<std::mutex> hold(lock);
    return programs;
}

std::string wineVersion(const std::string& wine) {
    static std::mutex m;
    static std::map<std::string, std::string> known;
    {
        std::lock_guard<std::mutex> hold(m);
        auto it = known.find(wine);
        if (it != known.end())
            return it->second;
    }
    std::string out;
    int fds[2];
    if (executable(wine) && pipe2(fds, O_CLOEXEC) == 0) {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        std::string arg0 = wine, arg1 = "--version";
        char* argv[] = {&arg0[0], &arg1[0], nullptr};
        pid_t pid = -1;
        int r = posix_spawn(&pid, wine.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if (r == 0) {
            // (--version answers at once, without the prefix; 10 s for a slow disk, then given up)
            fcntl(fds[0], F_SETFL, O_NONBLOCK);
            for (int waited = 0; waited < 1000; waited++) {
                char buf[256];
                ssize_t n;
                while ((n = read(fds[0], buf, sizeof(buf))) > 0)
                    out.append(buf, (size_t)n);
                int status;
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
                        out.append(buf, (size_t)n);
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                        out.clear();
                    pid = -1;
                    break;
                }
                usleep(10000);
            }
            if (pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                out.clear();
            }
        }
        close(fds[0]);
    }
    out = util::trim(out);
    if (!util::startsWith(out, "wine-"))
        out.clear();
    std::lock_guard<std::mutex> hold(m);
    known[wine] = out;
    return out;
}

int wineMajor(const std::string& version) {
    if (!util::startsWith(version, "wine-"))
        return 0;
    return std::atoi(version.c_str() + 5);
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

bool availableIn(const std::string& prefix, std::string& why, bool bits32) {
    if (winePathFor(prefix).empty()) {
        std::string source = wineSource(prefix);
        why = "Windows plugins need Wine, which isn't installed (or set TINYDAW_WINE to the wine program)";
        if (!source.empty())
            why = "The Wine for " + prefix + " can't be found (" + source + ")";
        return false;
    }
    if (!bits32 && hostPath().empty()) {
        why = "Windows plugins need tinydaw-wine-host.exe next to Tiny DAW (it is built by make when winegcc is "
              "installed)";
        return false;
    }
    if (bits32 && hostPath(true).empty()) {
        why = "32-bit Windows plugins need tinydaw-wine-host-32.exe next to Tiny DAW (built by make when winegcc can "
              "build 32-bit programs: gcc-multilib and g++-multilib, and Wine's 32-bit part)";
        return false;
    }
    if (!bits32 && prefixArch(prefix) == "win32") {
        why = prefix + " is a 32-bit Wine prefix (made with WINEARCH=win32): only 32-bit plugins run there, and this "
                       "one is 64-bit. Make a 64-bit prefix (Options → Wine prefixes → New prefix…) and install the "
                       "plugin in it.";
        return false;
    }
    return true;
}

namespace {

// runs `wine <args>` in a prefix and waits for it (up to `seconds`); true if it ended well
bool runWine(const std::string& prefix, const std::vector<std::string>& args, double seconds) {
    std::string wine = winePathFor(prefix);
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

// --- setting up ---

namespace {

// the wineserver of this wine program: beside it, else on the PATH ("" none)
std::string wineserverFor(const std::string& wine) {
    std::string sibling = util::joinPath(util::dirName(wine), "wineserver");
    if (executable(sibling))
        return sibling;
    if (const char* path = getenv("PATH"))
        for (const auto& dir : util::split(path, ':'))
            if (!dir.empty() && executable(util::joinPath(dir, "wineserver")))
                return util::joinPath(dir, "wineserver");
    return "";
}

// starts `program args` with WINEPREFIX=prefix and `extra` in its environment, in `dir` ("" here);
// its stdin and stdout /dev/null, its stderr this process's (Wine's own messages)
int spawnIn(const std::string& prefix, const std::string& program, const std::vector<std::string>& args,
            const std::vector<std::string>& extra, const std::string& dir, std::string& why) {
    std::vector<std::string> env;
    for (char** e = environ; *e; e++) {
        std::string s = *e;
        bool replaced = util::startsWith(s, "WINEPREFIX=");
        for (const auto& x : extra)
            replaced = replaced || util::startsWith(s, x.substr(0, x.find('=') + 1));
        if (!replaced)
            env.push_back(s);
    }
    env.push_back("WINEPREFIX=" + prefix);
    env.insert(env.end(), extra.begin(), extra.end());
    std::vector<std::string> argv = {program};
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
    if (!dir.empty())
        posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    pid_t pid = -1;
    int r = posix_spawn(&pid, program.c_str(), &actions, &attr, a.data(), e.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (r != 0) {
        why = "Cannot start " + program + ": " + strerror(r);
        return -1;
    }
    return pid;
}

}  // namespace

int startPrefix(const std::string& dir, const std::string& wine, std::string& why) {
    if (!executable(wine)) {
        why = wine.empty() ? "Wine isn't installed (or set TINYDAW_WINE to the wine program)" : wine + " can't be run";
        return -1;
    }
    if (dir.empty() || dir[0] != '/') {
        why = "A new prefix needs a full path";
        return -1;
    }
    if (util::fileExists(dir) && (!util::isDir(dir) || !util::listDir(dir).empty())) {
        why = dir + " already exists and isn't empty";
        return -1;
    }
    if (!util::makeDirs(util::dirName(dir))) {
        why = "Cannot make " + util::dirName(dir);
        return -1;
    }
    return spawnIn(dir, wine, {"wineboot", "-u"}, {"WINEARCH=win64"}, "", why);
}

int startServerWait(const std::string& prefix, const std::string& wine, std::string& why) {
    std::string server = wineserverFor(wine);
    if (server.empty()) {
        why = "Wine's wineserver can't be found beside " + wine;
        return -1;
    }
    return spawnIn(prefix, server, {"-w"}, {}, "", why);
}

int startInstaller(const std::string& prefix, const std::string& installer, std::string& why) {
    std::string wine = winePathFor(prefix);
    if (wine.empty()) {
        availableIn(prefix, why);
        return -1;
    }
    if (!isPrefix(prefix)) {
        why = prefix + " isn't a Wine prefix (a folder holding drive_c and system.reg)";
        return -1;
    }
    std::string ext = util::extension(installer);
    if (!util::fileExists(installer) || util::isDir(installer)) {
        why = installer + " doesn't exist";
        return -1;
    }
    std::vector<std::string> args;
    if (ext == ".msi")
        args = {"msiexec", "/i", installer};
    else if (ext == ".exe" && isWindowsBinary(installer))
        args = {installer};
    else {
        why = util::baseName(installer) + " isn't a Windows installer (an .exe or .msi file)";
        return -1;
    }
    return spawnIn(prefix, wine, args, {}, util::dirName(util::absolutePath(installer)), why);
}

bool finished(int pid, int& code) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0)
        return false;
    code = r == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

void stop(int pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

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
    for (const char* app : {"tinydaw-wine-host.exe", "tinydaw-wine-host.exe.so", "tinydaw-wine-host-32.exe",
                            "tinydaw-wine-host-32.exe.so"}) {
        std::string key = std::string("AppDefaults\\\\") + app + "\\\\X11 Driver]";
        if (reg.find(key) != std::string::npos)
            continue;
        runWine(prefix, {"reg", "add", std::string("HKCU\\Software\\Wine\\AppDefaults\\") + app + "\\X11 Driver", "/v",
                         "UseEGL", "/d", "N", "/f"},
                20);
    }
}

namespace {

// is there a graphics card here with a Vulkan driver for it? (a card's PCI vendor, and its
// vendor's Vulkan driver among the installed ones)
bool hardwareVulkan() {
    static int known = -1;
    if (known >= 0)
        return known == 1;
    known = 0;
    std::vector<std::string> icds;
    for (const char* dir : {"/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d"})
        for (const auto& e : util::listDir(dir))
            icds.push_back(util::lower(e.name));
    auto have = [&](const char* start) {
        for (const auto& i : icds)
            if (util::startsWith(i, start))
                return true;
        return false;
    };
    for (const auto& card : util::listDir("/sys/class/drm")) {
        std::string vendor;
        if (!util::startsWith(card.name, "card") || card.name.find('-') != std::string::npos ||
            !util::readFile("/sys/class/drm/" + card.name + "/device/vendor", vendor))
            continue;
        vendor = util::trim(vendor);
        if ((vendor == "0x10de" && have("nvidia")) || (vendor == "0x1002" && (have("radeon") || have("amd"))) ||
            (vendor == "0x8086" && have("intel_icd")))
            known = 1;
    }
    return known == 1;
}

// does the prefix choose Wine's Direct3D renderer itself (HKCU\Software\Wine\Direct3D: renderer)?
bool prefixChoosesRenderer(const std::string& prefix) {
    std::string reg;
    util::readFile(util::joinPath(prefix, "user.reg"), reg);
    size_t at = reg.find("[Software\\\\Wine\\\\Direct3D]");
    if (at == std::string::npos)
        return false;
    size_t end = reg.find("\n[", at + 1);
    return reg.substr(at, end == std::string::npos ? std::string::npos : end - at).find("\"renderer\"") !=
           std::string::npos;
}

}  // namespace

int spawnHost(const std::string& prefix, const std::vector<std::string>& args, int outFd, std::string& why, int inFd,
              bool bits32) {
    if (!availableIn(prefix, why, bits32))
        return -1;
    preferGlx(prefix);
    // Wine's Direct3D draws on Vulkan where there is a graphics card with a Vulkan driver (it has no
    // way back to OpenGL if Vulkan fails, so only then): Direct2D plugins redraw faster with it
    // (Serum 2's OSC tab: 185 -> 150 ms) and use less processor. Not if WINE_D3D_CONFIG or the
    // prefix chooses a renderer.
    bool vulkan = !getenv("WINE_D3D_CONFIG") && hardwareVulkan() && !prefixChoosesRenderer(prefix);
    std::string wine = winePathFor(prefix), host = hostPath(bits32);
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
    // Wine's own Direct3D 11 and DXGI, also where the prefix has DXVK's put in for every program
    // (DllOverrides *d3d11, *dxgi = native): the host's fixes are made for Wine's (WaitForVBlank,
    // which JUCE 8 draws after: Quanta 2's window stayed empty with DXVK's), and Wine's Direct2D and
    // DirectComposition are made to draw through them. Not if WINEDLLOVERRIDES names them itself
    if (!util::containsNoCase(overrides, "dxgi") && !util::containsNoCase(overrides, "d3d11"))
        overrides += ";dxgi,d3d11,d3d10core=b";
    if (!prefix.empty())
        env.push_back("WINEPREFIX=" + prefix);
    env.push_back("WINEDLLOVERRIDES=" + overrides);
    if (!getenv("WINEDEBUG"))
        env.push_back("WINEDEBUG=-all");
    if (vulkan)
        env.push_back("WINE_D3D_CONFIG=renderer=vulkan");
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
    int devnull = inFd >= 0 ? -1 : open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (inFd >= 0 || devnull >= 0)
        posix_spawn_file_actions_adddup2(&actions, inFd >= 0 ? inFd : devnull, 0);
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
