#include "audio/dsp.h"

#include <algorithm>

namespace dsp {

std::shared_ptr<AudioBuffer> toStereo(const float* in, size_t frames, int channels) {
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize(frames * 2);
    float* o = out->samples.data();
    if (channels <= 0) {
        std::fill(out->samples.begin(), out->samples.end(), 0.0f);
    } else if (channels == 1) {
        for (size_t i = 0; i < frames; i++) o[2 * i] = o[2 * i + 1] = in[i];
    } else {
        for (size_t i = 0; i < frames; i++) {
            o[2 * i] = in[i * channels];
            o[2 * i + 1] = in[i * channels + 1];
        }
    }
    return out;
}

AudioPtr resample(const AudioPtr& data, int srcSr, int dstSr) {
    if (!data || srcSr == dstSr || srcSr <= 0 || dstSr <= 0) return data;
    int64_t len = data->frames();
    int64_t n = (int64_t)std::nearbyint((double)len * dstSr / srcSr);
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize((size_t)std::max<int64_t>(0, n) * 2);
    if (len == 0 || n <= 0) return out;
    // np.linspace(0, len - 1, n) and np.interp
    double step = n > 1 ? (double)(len - 1) / (double)(n - 1) : 0.0;
    const float* s = data->samples.data();
    float* o = out->samples.data();
    for (int64_t i = 0; i < n; i++) {
        double x = i * step;
        int64_t j = (int64_t)x;
        if (j >= len - 1) {
            o[2 * i] = s[2 * (len - 1)];
            o[2 * i + 1] = s[2 * (len - 1) + 1];
            continue;
        }
        float t = (float)(x - (double)j);
        o[2 * i] = s[2 * j] + (s[2 * j + 2] - s[2 * j]) * t;
        o[2 * i + 1] = s[2 * j + 1] + (s[2 * j + 3] - s[2 * j + 1]) * t;
    }
    return out;
}

PeaksPtr waveformPeaks(const AudioBuffer& data, int block) {
    auto p = std::make_shared<Peaks>();
    int64_t len = data.frames();
    int64_t blocks = (len + block - 1) / block;
    p->mins.resize((size_t)blocks);
    p->maxs.resize((size_t)blocks);
    const float* s = data.samples.data();
    for (int64_t b = 0; b < blocks; b++) {
        float lo = 1e9f, hi = -1e9f;
        int64_t a = b * block, e = std::min(len, a + block);
        for (int64_t i = a; i < e; i++) {
            float m = (s[2 * i] + s[2 * i + 1]) * 0.5f;
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        if (e - a < block) {  // np.pad with zeros
            lo = std::min(lo, 0.0f);
            hi = std::max(hi, 0.0f);
        }
        p->mins[(size_t)b] = std::clamp(lo, -1.0f, 1.0f);
        p->maxs[(size_t)b] = std::clamp(hi, -1.0f, 1.0f);
    }
    return p;
}

}  // namespace dsp
