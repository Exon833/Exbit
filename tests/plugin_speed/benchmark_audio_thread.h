// The plugin speed benchmark's audio thread and worker pool, made once for the whole benchmark as the engine makes
// its own once (audio/engine.cpp), so that rtkit, which answers 25 requests per 20 s per user, grants each of them
// real time once instead of being asked again for every run (and refusing most of them).
#pragma once
#include <functional>

namespace workers {
class Pool;
}

namespace plugin_speed {

// runs job(pool) on the benchmark's audio thread (real time, started on the first call) with its worker pool (the
// engine's: as many workers as it would have), and returns when the job has
void runOnAudioThread(const std::function<void(workers::Pool& pool)>& job);

}  // namespace plugin_speed
