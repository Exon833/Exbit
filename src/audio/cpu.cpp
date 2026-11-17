#include "audio/cpu.h"

#include <algorithm>
#include <cstdint>

namespace audio {

void CpuMeter::reset() {
    {
        std::lock_guard<std::mutex> g(lock_);
        busy_ = budget_ = 0;
    }
    average = -1;
}

void CpuMeter::add(double busy, double budget) {
    std::lock_guard<std::mutex> g(lock_);
    busy_ += busy;
    budget_ += budget;
}

bool CpuMeter::read(double averageSeconds, double& current, double& averageOut) {
    double busy, budget;
    {
        std::lock_guard<std::mutex> g(lock_);
        busy = busy_;
        budget = budget_;
        busy_ = budget_ = 0;
    }
    if (budget == 0) return false;
    current = busy / budget;
    double weight = std::min(1.0, budget / averageSeconds);
    average = average < 0 ? current : average + weight * (current - average);
    averageOut = average;
    return true;
}

namespace {
size_t hashOf(const Track* t) {
    auto x = (uint64_t)(uintptr_t)t;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 7);
}
void addTo(std::atomic<double>& a, double v) {
    double old = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(old, old + v, std::memory_order_relaxed)) {
    }
}
}  // namespace

const Track* const TOMBSTONE = reinterpret_cast<const Track*>(uintptr_t(1));

void TrackCpu::add(const Track* track, double seconds) {
    if (!track) return;
    Slot* slots = tables_[current_.load(std::memory_order_acquire)];
    size_t h = hashOf(track);
    for (size_t k = 0; k < 32; k++) {  // (a short probe: a full table just stops measuring)
        Slot& s = slots[(h + k) % SLOTS];
        const Track* key = s.key.load(std::memory_order_acquire);
        if (key == track) {
            addTo(s.busy, seconds);
            return;
        }
        if (!key) {
            const Track* expected = nullptr;
            if (s.key.compare_exchange_strong(expected, track, std::memory_order_acq_rel) || expected == track) {
                addTo(s.busy, seconds);
                return;
            }
        }
        // (another track's slot, or a tombstone: on)
    }
}

void TrackCpu::addBudget(double seconds) { addTo(budget_, seconds); }

std::vector<std::pair<const Track*, double>> TrackCpu::read(const std::vector<const Track*>& known) {
    if (clearNext_ >= 0) {  // (the table left at the last read: nothing adds to it any more)
        for (auto& s : tables_[clearNext_]) {
            s.key.store(nullptr, std::memory_order_relaxed);
            s.busy.store(0.0, std::memory_order_relaxed);
        }
        clearNext_ = -1;
    }
    std::vector<std::pair<const Track*, double>> out;
    double budget = budget_.exchange(0.0);
    int cur = current_.load(std::memory_order_acquire);
    size_t tombstones = 0;
    for (int t = 0; t < 2; t++)
        for (auto& s : tables_[t]) {
            const Track* key = s.key.load(std::memory_order_acquire);
            if (!key) continue;
            if (key == TOMBSTONE) {
                if (t == cur) tombstones++;
                continue;
            }
            double busy = s.busy.exchange(0.0);
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                s.key.store(TOMBSTONE, std::memory_order_release);  // (gone: never taken again in this table)
                if (t == cur) tombstones++;
                continue;
            }
            if (budget <= 0) continue;
            bool merged = false;  // (a track found in both tables, just after a move: one entry)
            for (auto& o : out)
                if (o.first == key) o.second += busy / budget, merged = true;
            if (!merged) out.emplace_back(key, busy / budget);
        }
    if (tombstones > SLOTS / 4) {  // the audio thread moves to the other (clean) table; this one is cleared next time
        current_.store(1 - cur, std::memory_order_release);
        clearNext_ = cur;
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

}  // namespace audio
