// A tap on an effect's sound for its window (the EQ's analyzer, the tuner): the audio thread copies
// what passes (both sides mixed to one) into a ring of the newest samples, and the window's thread
// reads the newest of them as it repaints, to work out what it shows there (an FFT, a pitch).
//
// Nothing locks and nothing is allocated after allocate(): the ring's samples are atomics, the
// count written so far tells the reader where the newest are. And nothing is copied while no one
// looks: the audio thread copies only within WATCH_S of the last read (a window open on the effect
// reads every 50 ms; once it is closed the copying stops by itself).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace effects {

class SoundTap {
public:
    static constexpr double WATCH_S = 0.5;

    // (main thread) room for the newest `atLeast` samples (rounded up to a power of two) at `rate`
    void allocate(size_t atLeast, double rate) {
        size_t size = 1;
        while (size < atLeast) size <<= 1;
        if (size != size_) {
            ring_ = std::make_unique<std::atomic<float>[]>(size);
            size_ = size;
        }
        for (size_t i = 0; i < size_; i++) ring_[i].store(0.0f, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        watchFrames_ = (uint64_t)std::max(1.0, WATCH_S * rate);
        lastRead_ = 0;
    }
    size_t size() const { return size_; }

    // (audio thread) the frames that went by: copied ((left + right) / 2) while someone looks
    void write(const float* const* x, int n) {
        uint64_t seen = seen_.load(std::memory_order_relaxed) + (uint64_t)std::max(n, 0);
        seen_.store(seen, std::memory_order_relaxed);
        if (!size_ || !watching(seen)) return;
        uint64_t at = written_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++)
            ring_[(size_t)((at + (uint64_t)i) & (size_ - 1))].store(0.5f * (x[0][i] + x[1][i]),
                                                                   std::memory_order_relaxed);
        written_.store(at + (uint64_t)std::max(n, 0), std::memory_order_release);
    }

    // (the window's thread) the newest n samples (n ≤ size(); silence before the first), oldest
    // first, into out; and from now on the audio thread copies for WATCH_S more. Returns how many
    // samples came in since the last read (0: nothing new).
    uint64_t read(float* out, size_t n) {
        asked_.store(true, std::memory_order_relaxed);
        askedAt_.store(seen_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        n = std::min(n, size_);
        uint64_t end = written_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; i++) {
            uint64_t k = end - n + i;  // (wraps below 0 before the first: read as silence)
            out[i] = end >= n - i ? ring_[(size_t)(k & (size_ - 1))].load(std::memory_order_relaxed) : 0.0f;
        }
        uint64_t fresh = end - lastRead_;
        lastRead_ = end;
        return fresh;
    }

    // is the audio thread copying (has someone read within WATCH_S)?
    bool watched() const { return watching(seen_.load(std::memory_order_relaxed)); }
    // samples copied so far (tests: none while no one looks)
    uint64_t written() const { return written_.load(std::memory_order_acquire); }

private:
    bool watching(uint64_t seen) const {
        return asked_.load(std::memory_order_relaxed) &&
               seen - askedAt_.load(std::memory_order_relaxed) < watchFrames_;
    }
    std::unique_ptr<std::atomic<float>[]> ring_;
    size_t size_ = 0;
    std::atomic<uint64_t> written_{0}, seen_{0}, askedAt_{0};
    std::atomic<bool> asked_{false};
    uint64_t watchFrames_ = 24000;
    uint64_t lastRead_ = 0;  // (the reader's)
};

}  // namespace effects
