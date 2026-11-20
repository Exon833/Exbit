// A lock-free ring between one producer thread and one consumer thread (audio thread → MIDI thread, MIDI
// thread → GUI): no allocation after construction, no locks, so a real-time thread may push into it.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

template <class T>
class Spsc {
public:
    explicit Spsc(size_t capacity) : items_(capacity) {}
    // (producer) false when full: the item is dropped (counted)
    bool push(const T& v) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= items_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        items_[h % items_.size()] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    // (consumer) the oldest item, if any
    bool pop(T& out) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = items_[t % items_.size()];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool empty() const { return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire); }
    int dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::vector<T> items_;
    std::atomic<uint64_t> head_{0}, tail_{0};
    std::atomic<int> dropped_{0};
};
