#include "floor_round_trip.h"

#include <sys/types.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "paced_buffers.h"
#include "plugins/bridge.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace audio::detail {
void raiseThreadPriority();  // (the audio thread's priority: audio/devices.cpp)
}

namespace plugin_speed {
namespace {

constexpr int FRAMES = 64;
constexpr double SECONDS_OF_SOUND = 4;
#ifdef _WIN32
const std::string FLOOR_MEMORY = "Local\\exbit-round-trip-floor";
const std::string FLOOR_SIDE_ARGUMENT = "--floor-other-side";
#endif

// the floor's other side, as a plugin host's audio thread with nothing to do: each request done, until `quit`
void answerRoundTrips(bridge::AudioBlock* block) {
    audio::detail::raiseThreadPriority();
    uint32_t seen = block->done.load();
    for (;;) {
        bridge::waitChange(&block->request, seen, -1);
        if (block->quit.load()) return;
        seen = block->request.load(std::memory_order_acquire);
        block->done.store(seen, std::memory_order_release);
        bridge::wake(&block->done);
    }
}

}  // namespace

// the bare round trip bridge.h makes per buffer between two processes: `request` counted up and woken, the other
// side's `done` waited for
double floorRoundTrip() {
    bridge::AudioBlock* block = nullptr;
#ifdef _WIN32
    void* memory = bridge::mapShared(FLOOR_MEMORY, sizeof(bridge::AudioBlock), true);
    if (!memory) return 0;
    block = new (memory) bridge::AudioBlock();
    bridge::bindWake(&block->request, FLOOR_MEMORY + "-request", true);
    bridge::bindWake(&block->done, FLOOR_MEMORY + "-done", true);
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" " +
                           std::wstring(FLOOR_SIDE_ARGUMENT.begin(), FLOOR_SIDE_ARGUMENT.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION other{};
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &other)) return 0;
#else
    void* memory = mmap(nullptr, sizeof(bridge::AudioBlock), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 0;
    block = new (memory) bridge::AudioBlock();
    pid_t other = fork();
    if (other == 0) {
        answerRoundTrips(block);
        _exit(0);
    }
#endif
    std::vector<double> times;
    std::thread audioThread([&] {
        audio::detail::raiseThreadPriority();
        times = timePacedBuffers(FRAMES, SECONDS_OF_SOUND, [&](int) {
            uint32_t before = block->done.load(std::memory_order_acquire);
            uint32_t request = block->request.load(std::memory_order_relaxed) + 1;
            block->request.store(request, std::memory_order_release);
            bridge::wake(&block->request);
            while (block->done.load(std::memory_order_acquire) != request) bridge::waitChange(&block->done, before, 1);
        });
    });
    audioThread.join();
    block->quit = 1;
    block->request.fetch_add(1);
    bridge::wake(&block->request);
#ifdef _WIN32
    WaitForSingleObject(other.hProcess, INFINITE);
    CloseHandle(other.hProcess);
    CloseHandle(other.hThread);
    bridge::unbindWake(&block->request);
    bridge::unbindWake(&block->done);
    bridge::unmapShared(memory);
#else
    waitpid(other, nullptr, 0);
    munmap(memory, sizeof(bridge::AudioBlock));
#endif
    if (times.empty()) return 0;
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// (Windows) this run as the floor's other process, when asked by its argument
bool answerFloorRoundTrips(int argc, char** argv) {
#ifdef _WIN32
    if (argc != 2 || argv[1] != FLOOR_SIDE_ARGUMENT) return false;
    void* memory = bridge::mapShared(FLOOR_MEMORY, sizeof(bridge::AudioBlock), false);
    if (!memory) return true;
    auto* block = static_cast<bridge::AudioBlock*>(memory);
    bridge::bindWake(&block->request, FLOOR_MEMORY + "-request", false);
    bridge::bindWake(&block->done, FLOOR_MEMORY + "-done", false);
    answerRoundTrips(block);
    return true;
#else
    (void)argc, (void)argv;
    return false;
#endif
}

}  // namespace plugin_speed
