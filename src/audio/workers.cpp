#include "audio/workers.h"

#ifdef _WIN32
#include <windows.h>  // (WaitOnAddress: Windows' futex)
#else
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

#ifdef _WIN32
void futexWait(std::atomic<uint32_t>* word, uint32_t value) { WaitOnAddress(word, &value, sizeof value, INFINITE); }
void futexWake(std::atomic<uint32_t>* word, int count) {
    if (count == 1) WakeByAddressSingle(word);
    else WakeByAddressAll(word);
}
#else
void futexWait(std::atomic<uint32_t>* word, uint32_t value) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT_PRIVATE, value, nullptr, nullptr, 0);
}
void futexWake(std::atomic<uint32_t>* word, int count) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}
#endif
inline void pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
}  // namespace

Pool::Pool(int threads, bool realtime) : realtime_(realtime) {
    threads = std::max(0, threads);
    threads_.reserve(threads);
    for (int i = 0; i < threads; i++) {
        try {
            threads_.emplace_back([this] { loop(); });
        } catch (...) {  // (out of threads: go on with those made)
            break;
        }
    }
}

Pool::~Pool() {
    quit_.store(true);
    wake_.fetch_add(1);
    futexWake(&wake_, INT_MAX);
    for (auto& t : threads_) t.join();
}

namespace {
thread_local int tlJobs = 0;  // (inJob: the pool jobs this thread is running, nested)
}
bool inJob() { return tlJobs > 0; }

bool Pool::claimAndRun() {
    uint64_t s = state_.load(std::memory_order_acquire);
    for (;;) {
        int next = nextOf(s);
        if (next >= countOf(s)) return false;
        Job fn = fn_.load(std::memory_order_relaxed);
        void* ctx = ctx_.load(std::memory_order_relaxed);
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            tlJobs++;
            fn(ctx, next);
            tlJobs--;
            done_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        // (s now holds the word as it is: another thread claimed that job, or a new run began)
    }
}

void Pool::run(int jobs, Job fn, void* ctx) {
    if (jobs <= 0) return;
    if (threads_.empty() || jobs == 1 || jobs > MAX_JOBS) {  // (nothing to share)
        tlJobs++;
        for (int i = 0; i < jobs; i++) fn(ctx, i);
        tlJobs--;
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
    if (realtime_) audio::detail::raiseThreadPriority();
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

int availableCores() {
#ifdef _WIN32
    DWORD_PTR mine = 0, sys = 0;  // (the processors this process may run on; up to 64, one processor group)
    if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &sys) && mine) {
        int n = __builtin_popcountll((unsigned long long)mine);
        return std::max(n, (int)std::thread::hardware_concurrency());
    }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
#endif
    return std::max(1, (int)std::thread::hardware_concurrency());
}

int workersFor(int wanted) {
    int cores = availableCores();
    int all = wanted <= 0 ? cores : std::min(wanted, cores);
    return std::max(0, all - 1);
}

}  // namespace workers
