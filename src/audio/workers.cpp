#include "audio/workers.h"

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>

namespace audio::detail {
void raiseThreadPriority();  // devices.cpp
}

namespace workers {

namespace {
constexpr int GEN_SHIFT = 40, COUNT_SHIFT = 20;
constexpr uint64_t FIELD = (1u << 20) - 1;  // the count's and next job's bits
constexpr int MAX_JOBS = (int)FIELD;

uint64_t genOf(uint64_t s) { return s >> GEN_SHIFT; }
int countOf(uint64_t s) { return (int)((s >> COUNT_SHIFT) & FIELD); }
int nextOf(uint64_t s) { return (int)(s & FIELD); }

void futexWait(std::atomic<uint32_t>* word, uint32_t value) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT_PRIVATE, value, nullptr, nullptr, 0);
}
void futexWake(std::atomic<uint32_t>* word, int count) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}
inline void pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
}  // namespace

Pool::Pool(int threads) {
    threads = std::max(0, std::min(threads, MAX_WORKERS));
    threads_.reserve(threads);
    for (int i = 0; i < threads; i++) threads_.emplace_back([this] { loop(); });
}

Pool::~Pool() {
    quit_.store(true);
    wake_.fetch_add(1);
    futexWake(&wake_, INT_MAX);
    for (auto& t : threads_) t.join();
}

bool Pool::claimAndRun() {
    uint64_t s = state_.load(std::memory_order_acquire);
    for (;;) {
        int next = nextOf(s);
        if (next >= countOf(s)) return false;
        Job fn = fn_.load(std::memory_order_relaxed);
        void* ctx = ctx_.load(std::memory_order_relaxed);
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            fn(ctx, next);
            done_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        // (s now holds the word as it is: another thread claimed that job, or a new run began)
    }
}

void Pool::run(int jobs, Job fn, void* ctx) {
    if (jobs <= 0) return;
    if (threads_.empty() || jobs == 1 || jobs > MAX_JOBS) {  // (nothing to share)
        for (int i = 0; i < jobs; i++) fn(ctx, i);
        return;
    }
    uint64_t gen = (genOf(state_.load(std::memory_order_relaxed)) + 1) & ((1ull << (64 - GEN_SHIFT)) - 1);
    fn_.store(fn, std::memory_order_relaxed);
    ctx_.store(ctx, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    state_.store((gen << GEN_SHIFT) | ((uint64_t)jobs << COUNT_SHIFT), std::memory_order_release);
    wake_.fetch_add(1, std::memory_order_release);
    futexWake(&wake_, std::min(jobs - 1, (int)threads_.size()));
    while (claimAndRun()) {
    }
    // the last jobs may still be running on workers
    for (int spins = 0; done_.load(std::memory_order_acquire) < jobs; spins++) {
        if (spins < 4096)
            pause();
        else
            sched_yield();
    }
}

void Pool::loop() {
    audio::detail::raiseThreadPriority();
    using clock = std::chrono::steady_clock;
    while (!quit_.load(std::memory_order_acquire)) {
        uint32_t seen = wake_.load(std::memory_order_acquire);
        bool worked = false;
        while (claimAndRun()) worked = true;
        if (worked) {
            // look for the next run a little while (the mixer's runs come back to back) before sleeping
            auto until = clock::now() + std::chrono::microseconds(200);
            while (wake_.load(std::memory_order_acquire) == seen && clock::now() < until && !quit_.load()) pause();
            continue;
        }
        if (wake_.load(std::memory_order_acquire) == seen && !quit_.load()) futexWait(&wake_, seen);
    }
}

namespace {
std::atomic<int> chosen{0};
}
void setChosenCores(int wanted) { chosen.store(std::max(0, wanted)); }
int chosenCores() { return chosen.load(); }

int workersFor(int wanted) {
    int cores = (int)std::thread::hardware_concurrency();
    if (cores < 1) cores = 1;
    int all = wanted <= 0 ? cores : std::min(wanted, cores);
    return std::max(0, std::min(all - 1, MAX_WORKERS));
}

}  // namespace workers
