#include "audio/metering.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "config.h"

namespace audio {

PeakHold::PeakHold(size_t bars, double clipDb) : clipGain_(std::pow(10.0, clipDb / 20)) { setBars(bars); }

void PeakHold::setBars(size_t bars) {
    levels_.assign(bars, 0.0);
    held_.assign(bars, 0.0);
    heldAt_.assign(bars, 0.0);
}

bool PeakHold::push(const std::vector<float>& peaks, double now) {
    bool changed = false;
    double top = 0;
    for (size_t i = 0; i < levels_.size(); i++) {
        double raw = i < peaks.size() ? peaks[i] : 0.0;
        top = std::max(top, raw);
        double level = std::max(raw, levels_[i] * config::METER_DECAY);
        if (level < 1e-4) level = 0.0;  // (idle meters settle, so they stop repainting)
        double held;
        if (raw >= held_[i]) {
            held = raw;
            heldAt_[i] = now;
        } else {
            held = now - heldAt_[i] > HOLD_S ? held_[i] * config::METER_DECAY : held_[i];
        }
        if (held < 1e-4) held = 0.0;
        if (level != levels_[i] || held != held_[i]) changed = true;
        levels_[i] = level;
        held_[i] = held;
    }
    if (top > loudest_ || (top > clipGain_ && !clipped_)) {
        loudest_ = std::max(loudest_, top);
        clipped_ = clipped_ || top > clipGain_;
        changed = true;
    }
    return changed;
}

void PeakHold::resetPeaks() {
    loudest_ = 0.0;
    clipped_ = false;
}

double PeakHold::loudestDb() const {
    return loudest_ > 0 ? 20 * std::log10(loudest_) : -std::numeric_limits<double>::infinity();
}

Metering::Metering(int sr_, int channels_, size_t ringFloats, size_t ringItems)
    : sr(sr_), channels(channels_), data_(std::max<size_t>(ringFloats, 1)), items_(std::max<size_t>(ringItems, 1)) {
    resetLoudness();
}

bool Metering::reserve(size_t floats, uint64_t& offset) {
    uint64_t head = dataHead_.load(std::memory_order_relaxed);
    uint64_t tail = dataTail_.load(std::memory_order_acquire);
    uint64_t ih = itemHead_.load(std::memory_order_relaxed), it = itemTail_.load(std::memory_order_acquire);
    if (ih - it >= items_.size() || head - tail + floats > data_.size()) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    offset = head;
    return true;
}

void Metering::publish(const Item& item, size_t floats) {
    uint64_t ih = itemHead_.load(std::memory_order_relaxed);
    items_[ih % items_.size()] = item;
    dataHead_.store(item.offset + floats, std::memory_order_release);
    itemHead_.store(ih + 1, std::memory_order_release);
}

void Metering::tap(const Track* key, const float* const* planar, int width, int first, int frames) {
    size_t floats = (size_t)width * frames;
    uint64_t offset;
    if (!reserve(floats, offset)) return;
    size_t n = data_.size();
    uint64_t at = offset;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < width; c++) data_[(at++) % n] = planar[c][i];
    publish({key, first, width, frames, false, offset}, floats);
}

void Metering::tapInterleaved(const Track* key, const float* x, int width, int first, int frames) {
    size_t floats = (size_t)width * frames;
    uint64_t offset;
    if (!reserve(floats, offset)) return;
    size_t n = data_.size(), start = (size_t)(offset % n);
    size_t firstPart = std::min(floats, n - start);
    std::copy(x, x + firstPart, data_.begin() + start);
    std::copy(x + firstPart, x + floats, data_.begin());
    publish({key, first, width, frames, false, offset}, floats);
}

void Metering::tapSilence(const Track* key, int frames) {
    uint64_t offset;
    if (!reserve(0, offset)) return;
    publish({key, 0, 0, frames, true, offset}, 0);
}

void Metering::resetLoudness() {
    loudness_.clear();
    loudness_[MASTER].reset(new loudness::LoudnessMeter(sr, channels));
    pending_.clear();
}

void Metering::takePeaks(std::vector<float>& master, std::map<const Track*, std::vector<float>>& tracks) {
    master.assign(channels, 0.0f);
    tracks.clear();
    bool levels = levelsOn.load(), loud = loudnessOn.load();
    uint64_t ih = itemHead_.load(std::memory_order_acquire);
    uint64_t it = itemTail_.load(std::memory_order_relaxed);
    size_t n = data_.size();
    for (; it < ih; it++) {
        Item item = items_[it % items_.size()];
        size_t floats = (size_t)item.width * item.frames;
        if (loud) {
            Pending p;
            p.frames = item.frames, p.width = item.width, p.silent = item.silent;
            if (!item.silent) {
                p.audio.resize(floats);
                for (size_t i = 0; i < floats; i++) p.audio[i] = data_[(item.offset + i) % n];
            }
            pending_[item.key].push_back(std::move(p));
        }
        if (!item.silent && levels) {
            std::vector<float> peaks(item.width, 0.0f);
            for (size_t i = 0; i < floats; i++) {
                float v = std::fabs(data_[(item.offset + i) % n]);
                float& p = peaks[i % item.width];
                if (v > p) p = v;
            }
            if (item.key == MASTER) {
                for (int c = 0; c < item.width && c < channels; c++) master[c] = std::max(master[c], peaks[c]);
            } else {
                auto& full = tracks[item.key];
                if (full.empty()) full.assign(channels, 0.0f);
                for (int c = 0; c < item.width && item.first + c < channels; c++)
                    full[item.first + c] = std::max(full[item.first + c], peaks[c]);
            }
        }
        if (item.key == MASTER && scopeOn.load(std::memory_order_relaxed)) keepForScope(item);
        dataTail_.store(item.offset + floats, std::memory_order_release);
        itemTail_.store(it + 1, std::memory_order_release);
    }
}

void Metering::keepForScope(const Item& item) {
    size_t cap = (size_t)std::max(1.0, SCOPE_SECONDS * sr);
    if (scopeL_.size() != cap) {
        scopeL_.assign(cap, 0.0f);
        scopeR_.assign(cap, 0.0f);
        scopeFrames_ = 0;
    }
    size_t n = data_.size();
    for (int f = 0; f < item.frames; f++) {
        float l = 0.0f, r = 0.0f;
        if (!item.silent && item.width > 0) {
            uint64_t at = item.offset + (uint64_t)f * item.width;
            l = data_[at % n];
            r = item.width > 1 ? data_[(at + 1) % n] : l;
        }
        size_t i = scopeFrames_ % cap;
        scopeL_[i] = l;
        scopeR_[i] = r;
        scopeFrames_++;
    }
}

uint64_t Metering::scopeLatest(size_t n, std::vector<float>& left, std::vector<float>& right) const {
    size_t cap = scopeL_.size();
    n = std::min<size_t>({n, cap, (size_t)scopeFrames_});
    left.resize(n);
    right.resize(n);
    for (size_t k = 0; k < n; k++) {
        size_t i = (size_t)((scopeFrames_ - n + k) % cap);
        left[k] = scopeL_[i];
        right[k] = scopeR_[i];
    }
    return scopeFrames_;
}

void Metering::updateLoudness() {
    std::map<const Track*, std::vector<Pending>> pending;
    pending.swap(pending_);
    for (auto& kv : pending) {
        auto found = loudness_.find(kv.first);
        loudness::LoudnessMeter* meter = found == loudness_.end() ? nullptr : found->second.get();
        for (auto& item : kv.second) {
            if (!item.silent) {
                if (!meter || meter->channels != item.width) {
                    meter = new loudness::LoudnessMeter(sr, item.width);
                    loudness_[kv.first].reset(meter);
                }
                meter->add(item.audio.data(), item.frames);
            } else if (meter) {
                meter->addSilence(item.frames);
            }
        }
    }
}

void Metering::read(const Track* key, double& m, double& s, double& i) const {
    auto found = loudness_.find(key);
    if (found == loudness_.end() || !found->second) {
        m = s = i = -std::numeric_limits<double>::infinity();
        return;
    }
    found->second->read(m, s, i);
}

void Metering::forgetGone(const std::vector<TrackPtr>& tracks) {
    std::set<const Track*> keep;
    for (auto& t : tracks) keep.insert(t.get());
    for (auto it = loudness_.begin(); it != loudness_.end();)
        if (it->first != MASTER && !keep.count(it->first)) it = loudness_.erase(it);
        else ++it;
    for (auto it = pending_.begin(); it != pending_.end();)
        if (it->first != MASTER && !keep.count(it->first)) it = pending_.erase(it);
        else ++it;
}

}  // namespace audio
