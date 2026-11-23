// The worker pool (audio/workers.h) on its own, worst cases first: every job of every run done exactly once over
// 20000 back-to-back runs, runs after the workers fell asleep (the futex / WaitOnAddress wake), pools made and
// destroyed over and over, jobs on every thread at once and inJob(); and how long a run takes. Linux and Windows.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/workers.h"
#include "check.h"

namespace {

struct Ctx {
    std::vector<std::atomic<int>> hits;
    explicit Ctx(int n) : hits(n) {}
};

void hit(void* c, int i) { static_cast<Ctx*>(c)->hits[i]++; }

bool allOnce(Ctx& c, int times) {
    for (auto& h : c.hits)
        if (h.load() != times) return false;
    return true;
}

}  // namespace

int main() {
    int cores = workers::availableCores();
    int n = workers::workersFor(0);
    printf("%d cores: %d workers\n", cores, n);
    CHECK(cores >= 1 && n == cores - 1);

    {
        workers::Pool pool(n);
        CHECK(pool.threads() == n);
        Ctx c(64);
        auto t0 = std::chrono::steady_clock::now();
        const int runs = 20000;
        for (int r = 0; r < runs; r++) pool.run(64, hit, &c);
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / runs;
        printf("   %d runs of 64 jobs back to back: %.2f us a run\n", runs, us);
        CHECK(allOnce(c, runs));

        // after the workers went to sleep: each run wakes them
        Ctx d(16);
        double worst = 0;
        for (int r = 0; r < 50; r++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            auto a = std::chrono::steady_clock::now();
            pool.run(16, hit, &d);
            worst = std::max(worst, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count());
        }
        printf("   50 runs after sleeping: worst %.1f us\n", worst);
        CHECK(allOnce(d, 50));

        // jobs that take a while: on every thread at once
        std::atomic<int> done{0};
        auto a = std::chrono::steady_clock::now();
        pool.run(n + 1, [](void* v, int) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            static_cast<std::atomic<int>*>(v)->fetch_add(1);
        }, &done);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        printf("   %d jobs of 20 ms: %.1f ms\n", n + 1, ms);
        CHECK(done == n + 1);
        if (n > 0) CHECK(ms < 20.0 * (n + 1) * 0.75);

        // inside a job inJob() says so (nested runs then run on their own thread: no deadlock)
        std::atomic<int> inside{0};
        pool.run(4, [](void* v, int) {
            if (workers::inJob()) static_cast<std::atomic<int>*>(v)->fetch_add(1);
        }, &inside);
        CHECK(inside == 4 && !workers::inJob());
    }

    // made and destroyed over and over (some right after a run, some never used)
    for (int i = 0; i < 200; i++) {
        workers::Pool p(i % 5, i % 2 == 0);
        if (i % 3) {
            Ctx c(7);
            p.run(7, hit, &c);
            CHECK(allOnce(c, 1));
        }
    }
    {  // no workers: everything on the calling thread, in order
        workers::Pool p(0);
        Ctx c(5);
        p.run(5, hit, &c);
        CHECK(allOnce(c, 1));
    }
    TEST_MAIN_END();
}
