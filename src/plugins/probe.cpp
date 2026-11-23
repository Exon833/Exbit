// The kind finder: the probe process (plugin_probe.py) and its parent side (plugin_host.py's
// KindFinder). See probe.h.
#include "plugins/probe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "core/util.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#ifndef TINYDAW_WINE_HOST  // (a Windows program: its C runtime's; not Winelib's)
#include <fcntl.h>
#include <io.h>
#endif
#ifndef TINYDAW_IN_HOST
#include "plugins/child_win.h"
#endif
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace plug {

std::string kindOf(const std::string& subcategories) {
    if (subcategories.find("Instrument") != std::string::npos)
        return INSTRUMENT;
    return subcategories.find("Fx") != std::string::npos || subcategories.find("Analyzer") != std::string::npos
               ? EFFECT
               : "";
}

bool probeFile(const std::string& path, KindList& classes, std::string& error) {
    classes.clear();
    try {
        if (isVst2Path(path)) {
            auto found = vst2Classes(path, error);
            if (found.empty()) {
                if (error.empty())
                    error = "not a VST2 plugin";
                return false;
            }
            classes.push_back({found[0].name, found[0].instrument ? INSTRUMENT : EFFECT});
            return true;
        }
        auto found = vst3Classes(path, error);
        if (found.empty() && !error.empty())
            return false;
        for (const auto& c : found) {
            std::string kind = kindOf(c.subcategories);
            if (kind.empty()) {  // (rare) create it and ask
                std::string why;
                auto plugin = Plugin::load(path, c.name, why);
                if (!plugin) {
                    error = why;
                    return false;
                }
                kind = plugin->isInstrument() ? INSTRUMENT : EFFECT;
            }
            classes.push_back({c.name, kind});
        }
        if (classes.empty()) {
            error = "no plugins in this file";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = *e.what() ? e.what() : "error";
        return false;
    }
}

static void say(const Json& entry) {
    std::string line = "\n" + std::string(PROBE_MARK) + entry.dump() + "\n";
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
}

int runProbe(const std::string& listFile) {
#if defined(_WIN32) && !defined(TINYDAW_WINE_HOST)
    _setmode(_fileno(stdout), _O_BINARY);  // ("\n" stays "\n": the finder reads lines)
#endif
    std::string text;
    if (!util::readFile(listFile, text))
        return 2;
    Json list = Json::parse(text);
    if (!list.isArray())
        return 2;
    for (const auto& item : list.items()) {
        std::string path = item.toString();
        say(Json::object({{"start", path}}));
        KindList classes;
        std::string error;
        if (probeFile(path, classes, error)) {
            Json found = Json::array();
            for (const auto& c : classes)
                found.push(Json::array({c.first, c.second}));
            say(Json::object({{"path", path}, {"classes", found}}));
        } else {
            say(Json::object({{"path", path}, {"error", error.empty() ? std::string("error") : error}}));
        }
    }
    fflush(stdout);
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);  // (no clean-up: some plugins misbehave when unloaded at exit)
#endif
}

#ifndef TINYDAW_IN_HOST  // (the plugin hosts only probe: src/winehost)
// ===================================================================================== the finder

static double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

KindFinder::KindFinder(PluginLibrary& lib, Spawner s) : library(lib), spawner(std::move(s)) {
    if (!spawner)
        spawner = spawnSelf;
    todo = library.unknownFiles();
    total_ = (int)todo.size();
    static std::atomic<int> made{0};
#ifdef TINYDAW_WINDOWS
    const char* tmp = getenv("TEMP");
    listFile = util::joinPath(tmp && *tmp ? tmp : "C:/Windows/Temp", "tinydaw-probe-" +
                                  std::to_string((long)GetCurrentProcessId()) + "-" + std::to_string(++made) + ".json");
#else
    const char* tmp = getenv("TMPDIR");
    listFile = util::joinPath(tmp && *tmp ? tmp : "/tmp",
                              "tinydaw-probe-" + std::to_string((long)getpid()) + "-" + std::to_string(++made) + ".json");
#endif
}

KindFinder::~KindFinder() {
    if (working() || pid > 0 || timerId >= 0)
        cancel();
}

int KindFinder::spawnSelf(const std::string& list, int outFd) {
#ifdef TINYDAW_WINDOWS
    // this program again, its output to outFd (its own messages, stderr, to nowhere: as on Linux)
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    if (!n || n >= MAX_PATH * 4)
        return -1;
    int len = WideCharToMultiByte(CP_UTF8, 0, exe, (int)n, nullptr, 0, nullptr, nullptr);
    std::string path(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, exe, (int)n, &path[0], len, nullptr, nullptr);
    std::string why;
    return child::spawn(path, {"--probe", list}, -1, outFd, why);
#else
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return -1;
    exe[n] = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outFd, 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    std::string arg0 = exe, arg1 = "--probe", arg2 = list;
    char* argv[] = {&arg0[0], &arg1[0], &arg2[0], nullptr};
    pid_t child = -1;
    int result = posix_spawn(&child, exe, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return result == 0 ? (int)child : -1;
#endif
}

void KindFinder::start(bool useMainLoop) {
    mainLoop = useMainLoop;
    started_ = true;
    done_ = false;
    cancelled = false;
    next();
}

bool KindFinder::poll(int timeoutMs) {
    if (!working())
        return false;
#ifdef TINYDAW_WINDOWS
    if (fd >= 0 && child::readable(fd, std::max(0, timeoutMs)) != 0 && !readAvailable()) {
        ended();
        return working();
    }
    checkTime();
#else
    if (fd >= 0) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, std::max(0, timeoutMs)) > 0 && !readAvailable()) {
            ended();
            return working();
        }
    }
    checkTime();
#endif
    return working();
}

void KindFinder::next() {
    if (timerId >= 0 && mainLoop) {  // (the one-shot that called this)
        timerId = -1;
    }
    if (cancelled)
        return;
    if (todo.empty()) {
        stopProcess();
        util::removeFile(listFile);
        done_ = true;
        if (finished)
            finished(found_);
        return;
    }
    Json list = Json::array();
    for (const auto& f : todo)
        list.push(Json(f));
    util::writeFile(listFile, list.dump());
    int fds[2];
#ifdef TINYDAW_WINDOWS
    if (!child::pipe(fds)) {
#else
    if (pipe2(fds, O_CLOEXEC) != 0) {
#endif
        done_ = true;
        if (finished)
            finished(found_);
        return;
    }
    pid = spawner(listFile, fds[1]);
    close(fds[1]);
    if (pid <= 0) {
        close(fds[0]);
        pid = -1;
        done_ = true;
        if (finished)
            finished(found_);
        return;
    }
    fd = fds[0];
    current.clear();
    buffer.clear();
    deadline = 0;
#ifdef TINYDAW_WINDOWS
    // (a pipe can't be watched by the main loop on Windows: looked at by a timer, its time checked with it)
    if (mainLoop)
        timerId = plat::addTimer(50, [this] {
            if (fd >= 0 && child::readable(fd, 0) != 0 && !readAvailable())
                ended();
            else
                checkTime();
        }, true);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (mainLoop) {
        watchId = plat::watchFd(fd, [this] {
            if (!readAvailable())
                ended();
        });
        timerId = plat::addTimer(250, [this] { checkTime(); }, true);
    }
#endif
}

bool KindFinder::readAvailable() {
#ifdef TINYDAW_WINDOWS
    if (fd < 0)
        return false;
    bool open = true;
    char chunk[4096];
    for (;;) {  // (what has come, without waiting: read only what the pipe holds)
        int ready = child::readable(fd, 0);
        if (ready < 0) {
            open = false;  // (the end: its writers are gone)
            break;
        }
        if (ready == 0)
            break;
        int n = _read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            open = false;
            break;
        }
        buffer.append(chunk, (size_t)n);
    }
    read();
    return open;
#else
    if (fd < 0)
        return false;
    bool open = true;
    char chunk[4096];
    while (true) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buffer.append(chunk, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        open = false;  // (the end, or an error)
        break;
    }
    read();
    return open;
#endif
}

void KindFinder::read() {
    size_t end;
    while ((end = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        size_t mark = line.find(PROBE_MARK);
        if (mark == std::string::npos)
            continue;
        std::string error;
        Json entry = Json::parse(line.substr(mark + std::strlen(PROBE_MARK)), &error);
        if (error.empty() && entry.isObject())
            note(entry);
        if (cancelled)
            return;
    }
}

void KindFinder::note(const Json& entry) {
    if (entry.has("start")) {
        current = entry.str("start");
        deadline = secondsNow() + timeoutSeconds;
        return;
    }
    std::string path = entry.str("path");
    if (std::find(todo.begin(), todo.end(), path) == todo.end())
        return;
    const Json& classes = entry["classes"];
    if (classes.isArray() && classes.size() > 0) {
        KindList list;
        for (const auto& c : classes.items())
            list.push_back({c[(size_t)0].toString(), c[(size_t)1].toString()});
        library.learn(path, list);
        found_++;
    } else {
        std::string why = entry.str("error");
        library.probeFailed(path, why.empty() ? "unknown" : why);
    }
    forget(path);
}

void KindFinder::forget(const std::string& path) {
    todo.erase(std::remove(todo.begin(), todo.end(), path), todo.end());
    current.clear();
    deadline = 0;
    if (progress)
        progress(total_ - (int)todo.size(), total_);
}

void KindFinder::checkTime() {
#ifdef TINYDAW_WINDOWS
    if (deadline > 0 && secondsNow() > deadline) {
        tooLong();
        return;
    }
    int code;
    if (pid > 0 && child::finished(pid, code)) {  // (ended, its output maybe still held open by a child of a plugin's)
        pid = -1;
        readAvailable();
        ended();
    }
#else
    if (deadline > 0 && secondsNow() > deadline) {
        tooLong();
        return;
    }
    // (the process ended but its output stays open, e.g. a child of a plugin's holds it)
    if (pid > 0) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            pid = -1;
            readAvailable();
            ended();
        }
    }
#endif
}

void KindFinder::tooLong() {
    if (!current.empty() && std::find(todo.begin(), todo.end(), current) != todo.end()) {
        library.probeFailed(current, "took over " + std::to_string(timeoutSeconds) + " s");
        forget(current);
    }
    stopProcess();  // (ended() goes on with the rest)
    deadline = 0;
}

void KindFinder::ended() {
    // the process ended: done, crashed or stopped. A plugin it was looking at is to blame.
    if (fd >= 0)
        readAvailable();
    if (cancelled)
        return;
    if (!current.empty() && std::find(todo.begin(), todo.end(), current) != todo.end()) {
        library.probeFailed(current, "crashed while being looked at");
        forget(current);
    }
    closePipe();
#ifdef TINYDAW_WINDOWS
    int code;
    if (pid > 0 && !child::finished(pid, code))
        child::kill(pid);  // (its output ended: it is ending, or it closed it and is stopped)
    pid = -1;
#else
    if (pid > 0) {  // (its output ended: it is ending, or it closed it and is stopped)
        int status;
        if (waitpid(pid, &status, WNOHANG) != pid) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        pid = -1;
    }
#endif
    if (mainLoop)
        timerId = plat::addTimer(0, [this] { next(); }, false);
    else
        next();
}

void KindFinder::stopProcess() {
#ifdef TINYDAW_WINDOWS
    if (pid > 0) {
        child::kill(pid);  // (and waited for: ended() then finds it gone)
        pid = -1;
    }
#else
    if (pid > 0)
        kill(pid, SIGKILL);
#endif
}

void KindFinder::closePipe() {
    if (mainLoop) {
        if (watchId >= 0)
            plat::unwatchFd(watchId);
        if (timerId >= 0)
            plat::removeTimer(timerId);
    }
    watchId = timerId = -1;
#ifdef TINYDAW_WINDOWS
    if (fd >= 0)
        _close(fd);
#else
    if (fd >= 0)
        close(fd);
#endif
    fd = -1;
}

void KindFinder::cancel() {
    cancelled = true;
    todo.clear();
    deadline = 0;
    closePipe();
#ifdef TINYDAW_WINDOWS
    if (pid > 0) {
        child::kill(pid);
        pid = -1;
    }
#else
    if (pid > 0) {
        kill(pid, SIGKILL);
        int status;
        waitpid(pid, &status, 0);
        pid = -1;
    }
#endif
    util::removeFile(listFile);
    done_ = true;
}

#endif  // TINYDAW_IN_HOST

}  // namespace plug
