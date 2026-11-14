#include "audio/record_offset.h"

#include <algorithm>
#include <cmath>

#include "core/json.h"

namespace record_offset {

std::string deviceKey(const std::string& driverType, const std::string& deviceName) {
    return driverType + "|" + deviceName;
}

namespace {
Json parsed(const std::string& text) {
    Json j = Json::parse(text.empty() ? "{}" : text);
    return j.isObject() ? j : Json::object();
}

// numpy's percentile (linear) of sorted values
double percentile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0;
    double pos = q / 100 * (double)(sorted.size() - 1);
    size_t i = (size_t)std::floor(pos);
    if (i + 1 >= sorted.size()) return sorted.back();
    double frac = pos - (double)i;
    return sorted[i] + (sorted[i + 1] - sorted[i]) * frac;
}
}  // namespace

double offsetMs(const std::string& offsetsJson, const std::string& key) {
    Json all = parsed(offsetsJson);
    const Json& v = all[key];
    return v.isNumber() ? v.toDouble() : 0.0;
}

std::string setOffsetMs(const std::string& offsetsJson, const std::string& key, double ms) {
    Json j = parsed(offsetsJson);
    j.set(key, std::nearbyint(ms * 100) / 100);
    return j.dump();
}

std::vector<int64_t> onsets(const float* audio, int64_t frames, int channels, int sr) {
    std::vector<int64_t> found;
    if (frames <= 0 || channels <= 0) return found;
    std::vector<float> level((size_t)frames);
    float top = 0;
    for (int64_t i = 0; i < frames; i++) {
        float m = 0;
        for (int c = 0; c < channels; c++) m = std::max(m, std::fabs(audio[i * channels + c]));
        level[(size_t)i] = m;
        top = std::max(top, m);
    }
    if (top <= 0) return found;
    float threshold = top / 3;
    int64_t quiet = (int64_t)(QUIET_S * sr), last = -quiet - 1;
    for (int64_t i = 0; i < frames; i++) {
        if (!(level[(size_t)i] >= threshold)) continue;
        if (i - last > quiet) found.push_back(i);
        last = i;
    }
    return found;
}

std::optional<Measurement> measure(const float* audio, int64_t frames, int channels, int64_t startFrame,
                                   const TempoMap& map, int sr) {
    double startS = (double)startFrame / sr;
    std::vector<double> found;
    for (int64_t f : onsets(audio, frames, channels, sr)) found.push_back(startS + (double)f / sr);
    if (found.empty()) return std::nullopt;
    std::vector<BeatTime> beats = map.beatTimes(std::max(0.0, found.front() - 1), found.back() + 1);
    if (beats.empty()) return std::nullopt;
    std::vector<double> errors;
    for (double t : found) {
        double nearest = beats[0].seconds;
        for (auto& b : beats)  // (np.argmin: the first of the nearest)
            if (std::fabs(b.seconds - t) < std::fabs(nearest - t)) nearest = b.seconds;
        if (std::fabs(t - nearest) <= MAX_ERROR_S) errors.push_back(t - nearest);
    }
    if ((int)errors.size() < MIN_CLICKS) return std::nullopt;
    std::sort(errors.begin(), errors.end());
    Measurement m;
    m.late = percentile(errors, 50);  // (the median)
    m.spread = percentile(errors, 90) - percentile(errors, 10);
    m.count = (int)errors.size();
    return m;
}

std::optional<Measurement> measure(const Clip& take, const Timing& timing, int sr) {
    AudioPtr data = std::atomic_load(&take.data);
    if (!data) return std::nullopt;
    int64_t from = std::max<int64_t>(0, std::min(take.offset, data->frames()));
    int64_t n = std::max<int64_t>(0, std::min(take.frames(), data->frames() - from));
    return measure(data->frame(from), n, 2, take.start, *timing.map(), sr);
}

}  // namespace record_offset
