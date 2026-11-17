// Worker threads that share the audio thread's work: the tracks of one buffer are processed on several
// processor cores at once (mixer.cpp), so a song with many tracks of plugins doesn't have to fit on one.
//
// A Pool holds `threads` threads, started once (when the audio device opens, or a render begins) and
// stopped when the Pool goes. run(jobs, fn, ctx) calls fn(ctx, i) for i = 0 … jobs-1, spread over the
// workers AND the calling thread, and returns when every call has returned. It never allocates, locks a
// mutex or makes a system call except to wake sleeping workers (one futex wake) — so the audio thread
// can call it.
//
// The calling thread takes jobs too: with no worker free (or none at all) it does them all itself, in
// order, so a run always finishes. Jobs may wait for earlier jobs of the same run (a track waiting for
// the track its sidechain listens to: mixer.cpp spins on a flag): jobs are claimed in index order, so a
// job only ever waits for one that has been claimed already, by a thread that is running it.
//
// How a worker can't take a job of the wrong run: the run's generation, its job count and the next job
// to claim are one 64-bit atomic word. A worker claims a job by compare-and-swap on that word, so a
// claim made against a finished run (another one published since) fails. fn and ctx are read after the
// word and before the claim; they can't change while a job of their run is still unclaimed, because a
// new run is published only once every job of the last one is done.
//
// The engine's workers are real-time threads (SCHED_FIFO) when the system allows it, like the audio
// thread (a render's pool isn't: Pool(n, false)). After a
// run they keep looking for the next one for a short while (the mixer makes two or three runs per
// buffer, back to back), then sleep on a futex until woken.
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace workers {

using Job = void (*)(void* ctx, int index);

class Pool {
public:
    // threads 0: no workers (run() does everything on the calling thread). realtime: the workers are made
    // real-time threads as the audio thread is (the engine's pool); renders' aren't, so a render never
    // outranks playback. Fewer threads than asked if the system can't make more.
    explicit Pool(int threads, bool realtime = true);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    int threads() const { return (int)threads_.size(); }
    // fn(ctx, 0 … jobs-1), on the workers and the calling thread; returns when all have returned.
    // One run at a time (the caller's: a Pool belongs to one engine or one render).
    void run(int jobs, Job fn, void* ctx);

private:
    void loop();
    bool claimAndRun();  // took (and did) a job of the current run?

    // generation (bits 40…63) | job count (bits 20…39) | next job (bits 0…19)
    std::atomic<uint64_t> state_{0};
    std::atomic<Job> fn_{nullptr};
    std::atomic<void*> ctx_{nullptr};
    std::atomic<int> done_{0};
    std::atomic<uint32_t> wake_{0};  // futex word: changed (and woken) when a run is published
    std::atomic<bool> quit_{false};
    bool realtime_ = true;
    std::vector<std::thread> threads_;
};

// How many workers to use for `wanted` (the user's choice: 0 = automatic, 1 = none (the audio thread
// alone), n = that many cores in all): the cores this program may use less one (the audio thread),
// at most MAX_WORKERS.
constexpr int MAX_WORKERS = 15;
int workersFor(int wanted);
// the processor cores this program may run on (its CPU affinity: taskset, cgroups), at least 1
int availableCores();
// the user's choice (Options → Audio properties → Processing cores), for renders, which make a Pool of
// their own while they run (the engine's is the audio thread's). GUI thread.
void setChosenCores(int wanted);
int chosenCores();

}  // namespace workers
