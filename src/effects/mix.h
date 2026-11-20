// The Mix (dry / wet) fader of every effect in a patch, built-in or VST (PluginSlot.mix, saved;
// old projects 100 %): the DSP half of the successor's commit e86a408 (patch.py's DelayLine,
// native/engine.cpp's HoldBack in run_patch), for the engine to call.
//
// Below 100 % the effect's input is blended back in:  out = input·(1 − mix) + effect·mix  (in
// double precision), the input held back by the effect's own latency (plug::Plugin::latency() as it
// is now) so the two stay in time: at Mix 0 a limiter's input comes back exactly, only late by its
// look-ahead, as delay compensation expects. At Mix 1 (or more) nothing is done.
//
// Use, per effect node of a patch (one DryWet each, kept with the patch's other per-node state):
//
//     main thread, when the patch is made / the plugin prepared:  dryWet.prepare(maxBlock, plugin->latency());
//     audio thread, each buffer:
//         float mix = std::clamp((float)slot->mix, 0.f, 1.f);
//         if (DryWet::active(mix)) dryWet.keep(in, frames);        // BEFORE process() (in may be overwritten)
//         plugin->process(in, side, out, frames);
//         if (DryWet::active(mix)) dryWet.blend(out, frames, plugin->latency(), mix);
//
// keep() and blend() never allocate, unless the plugin's latency grew past what prepare() made
// room for, or a buffer is longer than maxBlock: then it grows once, as engine.cpp's HoldBack::set
// does.
#pragma once
#include <cstddef>
#include <vector>

namespace effects {

class DryWet {
public:
    static bool active(float mix) { return mix < 1.0f; }

    // (main thread) room for buffers of up to maxBlock frames and a latency of up to `latency`
    void prepare(int maxBlock, int latency = 0);
    // (audio thread) the effect's input of this buffer (2 rows of `frames`), before it is processed
    void keep(const float* const in[2], int frames);
    // (audio thread) blend the kept input, held back by `latency` frames, into the effect's output
    void blend(float* const out[2], int frames, int latency, float mix);
    // forget what is held (a seek, playback stopped, the patch rebuilt)
    void clear();

    int heldBack() const { return frames_; }  // the latency it holds the input back by now

private:
    void setLatency(int frames);
    void holdBack(float* const rows[2], int n);  // engine.cpp's HoldBack::process
    std::vector<float> dry_[2];   // the kept input (maxBlock)
    std::vector<float> ring_[2];  // the held back input (latency)
    int frames_ = 0;              // its latency now
    size_t at_ = 0;
    int kept_ = 0;
};

}  // namespace effects
