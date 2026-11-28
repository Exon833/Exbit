// What one bridge crossing costs, measured with a plugin that does next to nothing (Tiny Test Fx, a gain), 64 frames
// at 48 kHz at real-time pace on a real-time thread, as the engine's audio thread plays it:
//   floor:      the bare wake-up and wait each way that the bridge is built on (bridge.h's wake and waitChange; on
//               Windows the events bound as bridge_win.h binds them) between two processes, nothing else;
//   in process: the plugin called directly (Linux: its Linux build; Windows: its Windows build, without the host);
//   bridged:    the plugin's Windows build through the bridge (Linux: exbit-wine-host.exe under Wine, in the test
//               prefix ~/.local/share/exbit-test-wine/speed; Windows: exbit-plugin-host.exe).
// A crossing costs bridged minus in process; what Exbit and the host add to the system's own cost is that minus the
// floor. Checked: Exbit and the host add at most HOST_SHARE_LIMIT to the floor (the rest is the system's: waking a
// thread of another process and switching to it).
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/realtime_priority.h"
#include "check.h"
#include "core/util.h"
#include "platform/platform.h"
#include "plugin_speed/floor_round_trip.h"
#include "plugin_speed/paced_buffers.h"
#include "plugins/bridge.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/wine.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace audio::detail {
void raiseThreadPriority();  // (the audio thread's priority: audio/devices.cpp)
}

namespace {

constexpr int FRAMES = 64;
constexpr double SECONDS_OF_SOUND = 4;
// seconds a crossing may cost above the floor (measured: Linux 0-0.7 µs; Windows in a VM 0-4 µs, as its floor itself
// comes out at about 9 µs or about 13 µs from run to run)
constexpr double HOST_SHARE_LIMIT = 5e-6;
// the median of per-buffer times (seconds)
double median(std::vector<double> times) {
    if (times.empty()) return 0;
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// the median time of one buffer of the plugin (its silence in, its output out); 0 if it doesn't load
double pluginBuffer(const std::string& path) {
    std::string error;
    auto plugin = plug::Plugin::load(path, "", error);
    if (!plugin) {
        printf("   %s didn't load: %s\n", path.c_str(), error.c_str());
        return 0;
    }
    plugin->prepare(48000, 4096);
    std::vector<float> sound[4];
    for (auto& channel : sound) channel.assign(FRAMES, 0.0f);
    const float* in[2] = {sound[0].data(), sound[1].data()};
    float* out[2] = {sound[2].data(), sound[3].data()};
    std::vector<double> times;
    std::thread audioThread([&] {
        audio::detail::raiseThreadPriority();
        times = plugin_speed::timePacedBuffers(FRAMES, SECONDS_OF_SOUND, [&](int) { plugin->process(in, nullptr, out, FRAMES); });
    });
    audioThread.join();
    return median(times);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (plugin_speed::answerFloorRoundTrips(argc, argv)) return 0;
    const char* winDirectory = environment::readProgramVariable("WIN_TEST_PLUGINS");
    const std::string windowsPlugin = plug::loadableFile(
        util::joinPath(util::absolutePath(winDirectory && *winDirectory ? winDirectory : "build/win-test-plugins"), "TinyTestFx.vst3"));
    if (!util::fileExists(windowsPlugin)) {
        printf("(no Windows test plugins: make win-plugins needs MinGW's x86_64-w64-mingw32-g++)\n");
        TEST_MAIN_END();
    }
#ifdef _WIN32
    const std::string inProcessPlugin = windowsPlugin;
    const char* hostVariable = "EXBIT_PLUGIN_HOST";
    const std::string host = getenv(hostVariable) ? getenv(hostVariable) : "";
#else
    const std::string inProcessPlugin = plug::loadableFile(util::joinPath(check::testDir(), "TinyTestFx.vst3"));
    const std::string prefix = util::joinPath(plat::homeDir(), ".local/share/exbit-test-wine/speed");
    std::string why;
    if (!wine::available(why) || !wine::isPrefix(prefix)) {
        printf("(no Wine, or no test prefix %s: %s)\n", prefix.c_str(), why.c_str());
        TEST_MAIN_END();
    }
    wine::setPrefixes({prefix});
#endif
    const double floor = plugin_speed::floorRoundTrip();
#ifdef _WIN32
    unsetenv(hostVariable);  // (Plugin::load then hosts it in process)
    const double inProcess = pluginBuffer(inProcessPlugin);
    setenv(hostVariable, host.c_str(), 1);
#else
    const double inProcess = pluginBuffer(inProcessPlugin);
#endif
    const double bridged = pluginBuffer(windowsPlugin);
    const double crossing = bridged - inProcess, hostShare = crossing - floor;
    printf("%d frames, median per buffer: floor %.2f µs, in process %.2f µs, bridged %.2f µs\n", FRAMES, floor * 1e6,
           inProcess * 1e6, bridged * 1e6);
    printf("a crossing: %.2f µs, of which Exbit and the host %.2f µs above the floor\n", crossing * 1e6, hostShare * 1e6);
    printf("%s\n", realtime::statusText().c_str());
    CHECK(floor > 0 && inProcess > 0 && bridged > 0);
    CHECK(hostShare <= HOST_SHARE_LIMIT);
#ifndef _WIN32
    system(("env WINEPREFIX='" + prefix + "' '" + util::joinPath(util::dirName(wine::winePath()), "wineserver") +
            "' -k 2>/dev/null").c_str());
#endif
    TEST_MAIN_END();
}
