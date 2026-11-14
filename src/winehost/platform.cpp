// The few platform functions the plugin hosts use, for the Wine host (src/platform is the
// program's, X11 and all; the host has no main loop of its own to offer, and Windows plugins don't
// ask for one: their windows and timers run on Win32 messages).
#include "platform/platform.h"

#include <cstdlib>

#include <pthread.h>

#include "core/util.h"

namespace plat {

namespace {
const pthread_t mainThread = pthread_self();  // (set before main(), on the main thread)
}

bool isMainThread() { return pthread_equal(pthread_self(), mainThread) != 0; }
void postToMain(std::function<void()> fn) {
    if (fn)
        fn();
}
int addTimer(int, std::function<void()>, bool) { return -1; }
void removeTimer(int) {}
int watchFd(int, std::function<void()>) { return -1; }
void unwatchFd(int) {}

std::string homeDir() {
    const char* home = getenv("HOME");
    return home && *home ? home : "/";
}

std::string configDir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string dir = util::joinPath(xdg && *xdg ? xdg : util::joinPath(homeDir(), ".config"), "TinyDAW");
    util::makeDirs(dir);
    return dir;
}

}  // namespace plat
