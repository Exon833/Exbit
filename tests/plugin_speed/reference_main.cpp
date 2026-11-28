// plugin-speed-reference.exe: the bare reference of the plugin speed benchmark for Windows plugins, run under Wine
// (tests/test_plugin_speed.cpp starts it in its test prefix) or on Windows itself. Built with MinGW, on Exbit's own
// VST3 host built for Windows, as exbit-plugin-host.exe is (the Makefile).
//   plugin-speed-reference.exe CASE INSTRUMENT EFFECT
// prints, for each frame size, "frames N" and the time of each buffer in microseconds, one line per size.
#include <cstdio>
#include <string>

#include "audio/workers.h"
#include "direct_calls.h"
#include "speed_settings.h"

namespace audio::detail {
void raiseThreadPriority();  // (reference_thread_priority.cpp)
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s one|chain|eight INSTRUMENT EFFECT\n", argv[0]);
        return 2;
    }
    std::string error;
    audio::detail::raiseThreadPriority();  // (as the engine's audio thread)
    workers::Pool pool(workers::workersFor(0));  // (the engine's, as many workers as it would have)
    auto timesPerSize = plugin_speed::timeDirectCalls(argv[1], argv[2], argv[3], pool, error);
    if (timesPerSize.empty()) {
        fprintf(stderr, "plugin-speed-reference: %s\n", error.c_str());
        return 1;
    }
    for (size_t size = 0; size < timesPerSize.size(); size++) {
        printf("frames %d", plugin_speed::FRAME_SIZES[size]);
        for (double seconds : timesPerSize[size]) printf(" %.2f", seconds * 1e6);
        printf("\n");
    }
    fflush(stdout);
    return 0;
}
