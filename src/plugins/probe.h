// Finding out whether plugins are instruments or effects, without the user loading each one
// (daw/audio/plugin_probe.py, and ui/plugin_host.py's KindFinder).
//
// Run as its own process (`tinydaw --probe <list.json>`: main.cpp calls runProbe()), so a plugin
// that crashes or hangs while it is looked at can't take the program down with it. The list is the
// plugin files whose kind isn't known yet (a JSON array of paths). For each, one line goes to stdout
// as it starts ({"start": path}) and one when it is done ({"path": path, "classes": [[name, kind],
// ...]} or {"path": path, "error": text}); each line begins with PROBE_MARK, so whatever plugins
// print themselves is told apart. If the process dies, the last "start" without a result names the
// plugin to blame.
//
// VST3: the plugin file's factory lists its plugins with their sub-categories ("Instrument|Synth",
// "Fx|EQ"…), read without creating any of them; only a plugin whose sub-categories say neither is
// created to ask it. VST2: the plugin is created (its entry point run) and asked; nothing is
// processed and no window opens.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "plugins/library.h"

namespace plug {

constexpr const char* PROBE_MARK = "\x1eTINYDAW ";
constexpr int PROBE_TIMEOUT_S = 30;  // a plugin taking longer than this to be looked at is given up on

// INSTRUMENT, EFFECT, or "" if a plugin's VST3 sub-categories don't say
std::string kindOf(const std::string& subcategories);
// the (name, kind) of the plugins in one file; false and `error` if it can't tell
bool probeFile(const std::string& path, KindList& classes, std::string& error);
// what the probe process does: looks at each file of the list, printing its lines; ends the process
// itself (_exit(0), no clean-up: some plugins misbehave when unloaded at exit). Returns (2) only if
// the list can't be read.
int runProbe(const std::string& listFile);

// Finds the kinds of a library's plugins not known yet (library.unknownFiles()) with the probe in a
// process of its own, and tells the library (learn / probeFailed). A plugin that crashes the
// process, or takes over PROBE_TIMEOUT_S, is given up on and the process goes on with the next ones.
// `progress(done, total)` after each plugin; `finished(found)` at the end (the last thing it does:
// the finder may be destroyed from there only through plat::postToMain).
//
// In the program: start() and the main loop does the rest (it watches the process's output with
// plat::watchFd and checks the time with a plat timer). Without the main loop (tests): start(false),
// then call poll() until it returns false.
class KindFinder {
public:
    // starts the probe process for `listFile`, its stdout going to `outFd`; returns its pid (-1: failed).
    // The default runs this program again: /proc/self/exe --probe <listFile>.
    using Spawner = std::function<int(const std::string& listFile, int outFd)>;

    explicit KindFinder(PluginLibrary& library, Spawner spawner = {});
    ~KindFinder();  // cancels

    std::function<void(int done, int total)> progress;
    std::function<void(int found)> finished;
    int timeoutSeconds = PROBE_TIMEOUT_S;

    int total() const { return total_; }
    int found() const { return found_; }
    bool working() const { return started_ && !done_; }

    // look at the next plugins (in a new process); finished() at once if there are none
    void start(bool useMainLoop = true);
    // (without the main loop) waits up to timeoutMs for output, reads it, gives up on a plugin taking
    // too long, goes on after the process ended; false once finished
    bool poll(int timeoutMs = 100);
    // stop at once (the library is being replaced, or the program closes); finished() isn't called
    void cancel();

    static int spawnSelf(const std::string& listFile, int outFd);  // the default Spawner

private:
    PluginLibrary& library;
    Spawner spawner;
    std::vector<std::string> todo;
    int total_ = 0, found_ = 0;
    std::string current, buffer, listFile;
    int pid = -1, fd = -1, watchId = -1, timerId = -1;
    double deadline = 0;
    bool mainLoop = false, started_ = false, done_ = false, cancelled = false;

    void next();
    void read();
    bool readAvailable();  // false at the end of the output
    void note(const Json& entry);
    void forget(const std::string& path);
    void checkTime();
    void tooLong();
    void ended();
    void stopProcess();
    void closePipe();
};

}  // namespace plug
