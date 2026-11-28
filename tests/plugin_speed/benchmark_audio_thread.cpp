#include "benchmark_audio_thread.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include "audio/workers.h"

namespace audio::detail {
void raiseThreadPriority();  // (the audio thread's priority: audio/devices.cpp)
}

namespace plugin_speed {

void runOnAudioThread(const std::function<void(workers::Pool& pool)>& job) {
    // (never destroyed: the thread still waits on them while the program ends)
    static std::mutex& lock = *new std::mutex;
    static std::condition_variable& changed = *new std::condition_variable;
    static const std::function<void(workers::Pool&)>* waitingJob = nullptr;  // (null: none, or done)
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {  // (left running until the program ends, as an engine's audio thread runs while it plays)
            audio::detail::raiseThreadPriority();
            workers::Pool pool(workers::workersFor(0));
            std::unique_lock<std::mutex> hold(lock);
            for (;;) {
                changed.wait(hold, [] { return waitingJob != nullptr; });
                (*waitingJob)(pool);
                waitingJob = nullptr;
                changed.notify_all();
            }
        }).detach();
    });
    std::unique_lock<std::mutex> hold(lock);
    waitingJob = &job;
    changed.notify_all();
    changed.wait(hold, [] { return waitingJob == nullptr; });
}

}  // namespace plugin_speed
