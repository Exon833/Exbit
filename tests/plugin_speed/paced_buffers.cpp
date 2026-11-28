#include "paced_buffers.h"

#include <chrono>
#include <thread>

#include "speed_settings.h"

#ifdef _WIN32
#include <windows.h>  // (timeBeginPeriod: sleeps of a millisecond, not Windows' default 15.6)
#endif

namespace plugin_speed {

std::vector<double> timePacedBuffers(int frames, double secondsOfSound, const std::function<void(int index)>& processBuffer) {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    using clock = std::chrono::steady_clock;
    const int bufferCount = (int)(secondsOfSound * SAMPLE_RATE / frames);
    const auto bufferPeriod = std::chrono::duration<double>((double)frames / SAMPLE_RATE);
    std::vector<double> secondsPerBuffer;
    secondsPerBuffer.reserve(bufferCount);
    const auto runStart = clock::now();
    for (int index = 0; index < bufferCount; index++) {
        std::this_thread::sleep_until(runStart + std::chrono::duration_cast<clock::duration>(bufferPeriod * index));
        const auto bufferStart = clock::now();
        processBuffer(index);
        if (index >= bufferCount / 10)
            secondsPerBuffer.push_back(std::chrono::duration<double>(clock::now() - bufferStart).count());
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return secondsPerBuffer;
}

}  // namespace plugin_speed
