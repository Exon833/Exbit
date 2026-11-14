// An effect's Mix (dry / wet): see mix.h.
#include "effects/mix.h"

#include <algorithm>

namespace effects {

void DryWet::prepare(int maxBlock, int latency) {
    for (int c = 0; c < 2; c++) {
        dry_[c].assign(std::max(1, maxBlock), 0.0f);
        if ((int)ring_[c].capacity() < latency) ring_[c].reserve(latency);
    }
    kept_ = 0;
    setLatency(std::max(0, latency));
}

void DryWet::setLatency(int frames) {  // (HoldBack::set: a new length starts silent)
    frames_ = frames;
    for (auto& r : ring_) r.assign(std::max(0, frames), 0.0f);  // (no allocation within the capacity)
    at_ = 0;
}

void DryWet::clear() {
    for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.0f);
    at_ = 0;
}

void DryWet::keep(const float* const in[2], int frames) {
    if ((int)dry_[0].size() < frames)  // (not prepared for buffers this long: grows once)
        for (auto& d : dry_) d.resize(frames);
    for (int c = 0; c < 2; c++) std::copy(in[c], in[c] + frames, dry_[c].data());
    kept_ = frames;
}

void DryWet::holdBack(float* const rows[2], int n) {
    if (!frames_) return;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            float value = rows[c][i];
            rows[c][i] = ring_[c][at_];
            ring_[c][at_] = value;
        }
        at_ = (at_ + 1) % (size_t)frames_;
    }
}

void DryWet::blend(float* const out[2], int frames, int latency, float mix) {
    if (!active(mix)) return;
    if (latency != frames_) setLatency(std::max(0, latency));
    int n = std::min(frames, kept_);
    float* rows[2] = {dry_[0].data(), dry_[1].data()};
    holdBack(rows, n);
    double m = std::max(0.0, (double)mix);
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n; i++) out[c][i] = (float)((double)rows[c][i] * (1.0 - m) + (double)out[c][i] * m);
    kept_ = 0;
}

}  // namespace effects
