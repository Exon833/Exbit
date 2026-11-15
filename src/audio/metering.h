// Level and loudness metering, done off the audio thread (metering.py).
//
// The audio callback only hands over a copy of what each track and the master played (tap /
// tapSilence: a quick memory copy into a preallocated lock-free single-producer/single-consumer
// ring). The GUI thread then
//   - every frame, takes per-channel peaks for the level meters (takePeaks), and
//   - a few times a second, K-filters everything that arrived since for the LUFS meters
//     in one batch per meter (updateLoudness), much cheaper than filtering each buffer.
// Either can be switched off; then the callback does not even copy for it.
//
// Keys: a track is keyed by its Track*, the master by MASTER (nullptr). The GUI should call
// forgetGone() when tracks are removed, so a new track never inherits a removed one's meter.
// If the GUI stops draining for long, the ring fills and further taps are dropped (`dropped`).
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "audio/loudness.h"
#include "model/model.h"

namespace audio {

constexpr const Track* MASTER = nullptr;  // key of the master signal; tracks are keyed by their Track*

// What a level meter shows besides its bars (level_meter.py): each bar's peak held for HOLD_S (then it
// falls), the loudest peak since Play (dBFS), and a clip light once the sound went over clipDb (a
// track's: 0 dB; the master's: -0.1 dB) until it is cleared: by Play (Metering::peakResets changes) or a
// click on the meter (resetPeaks()). GUI thread; `now` is a clock in seconds.
class PeakHold {
public:
    static constexpr double HOLD_S = 1.5;
    static constexpr double CLIP_DB = 0.0, MASTER_CLIP_DB = -0.1;
    explicit PeakHold(size_t bars = 2, double clipDb = CLIP_DB);
    void setBars(size_t bars);  // (forgets the bars' levels)
    // new peaks, one per bar (missing: silence); true if anything shown changed
    bool push(const std::vector<float>& peaks, double now);
    void resetPeaks();
    const std::vector<double>& levels() const { return levels_; }  // falling back slowly (METER_DECAY)
    const std::vector<double>& held() const { return held_; }
    double loudest() const { return loudest_; }  // gain; 0 = nothing yet
    double loudestDb() const;                    // dBFS (-inf: nothing yet)
    bool clipped() const { return clipped_; }

private:
    std::vector<double> levels_, held_, heldAt_;
    double clipGain_;
    double loudest_ = 0.0;
    bool clipped_ = false;
};

class Metering {
public:
    // ringFloats: room for the audio waiting to be measured (floats); ringItems: for that many taps
    Metering(int sr, int channels, size_t ringFloats = (size_t)1 << 21, size_t ringItems = 16384);
    const int sr, channels;
    std::atomic<bool> levelsOn{true};    // peak meters
    std::atomic<bool> loudnessOn{true};  // LUFS
    std::atomic<bool> scopeOn{false};    // the master scope (ui/scope_view.h): the master's last SCOPE_SECONDS kept
    std::atomic<int64_t> dropped{0};     // taps that found the ring full
    bool active() const {
        return levelsOn.load(std::memory_order_relaxed) || loudnessOn.load(std::memory_order_relaxed) ||
               scopeOn.load(std::memory_order_relaxed);
    }
    static constexpr double SCOPE_SECONDS = 2.0;

    // --- audio thread ---
    // what `key` played on outputs [first, first + width): planar channel pointers
    void tap(const Track* key, const float* const* planar, int width, int first, int frames);
    // the same from interleaved audio (`width` floats per frame)
    void tapInterleaved(const Track* key, const float* interleaved, int width, int first, int frames);
    void tapSilence(const Track* key, int frames);

    // --- GUI thread ---
    void resetLoudness();  // start the LUFS measurements afresh (e.g. when playback starts)
    // Play was pressed (Engine::play calls it): the meters' loudest peaks and clip lights start again
    // (PeakHold::resetPeaks when this count changes)
    void resetPeaks() { peakResets.fetch_add(1); }
    std::atomic<uint64_t> peakResets{0};
    // Drain what was played since the last call: master peaks and {track: peaks}, one value per
    // output channel. Also queues the audio for the next updateLoudness().
    void takePeaks(std::vector<float>& master, std::map<const Track*, std::vector<float>>& tracks);
    void updateLoudness();  // measure everything queued since the last call: one filter run per meter
    // (momentary, short-term, integrated) LUFS of a track or MASTER; -inf if never heard
    void read(const Track* key, double& momentary, double& shortTerm, double& integrated) const;
    // drop the meters of tracks no longer in `tracks`
    void forgetGone(const std::vector<TrackPtr>& tracks);
    // (scopeOn) the master's newest `n` frames (at most SCOPE_SECONDS' worth), oldest first, of its outputs
    // 1 and 2 (a single output: the same in both); takePeaks keeps them. Fewer if fewer were heard since
    // the scope was switched on. Returns how many frames were heard in all (for following the newest).
    uint64_t scopeLatest(size_t n, std::vector<float>& left, std::vector<float>& right) const;
    void clearScope() { scopeFrames_ = 0; }  // (GUI thread) forget what was kept (the scope shown again)

private:
    struct Item {
        const Track* key;
        int first, width, frames;
        bool silent;
        uint64_t offset;  // where its audio starts in the float ring (monotonic)
    };
    struct Pending {      // a block of audio, or a silent frame count
        std::vector<float> audio;
        int width = 0, frames = 0;
        bool silent = false;
    };
    bool reserve(size_t floats, uint64_t& offset);
    void publish(const Item& item, size_t floats);

    std::vector<float> data_;
    std::vector<Item> items_;
    std::atomic<uint64_t> dataHead_{0}, dataTail_{0};  // floats written / consumed
    std::atomic<uint64_t> itemHead_{0}, itemTail_{0};
    std::map<const Track*, std::unique_ptr<loudness::LoudnessMeter>> loudness_;
    std::map<const Track*, std::vector<Pending>> pending_;
    // (GUI thread) the master's last SCOPE_SECONDS, as rings, and how many frames went into them
    std::vector<float> scopeL_, scopeR_;
    uint64_t scopeFrames_ = 0;
    void keepForScope(const Item& item);
};

}  // namespace audio
