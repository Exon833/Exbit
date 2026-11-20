#include "model/clip_envelope.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/automation.h"
#include "config.h"

namespace clip_envelope {

double dbOf(double v) { return config::MIN_DB + v * (MAX_DB - config::MIN_DB); }

double valueOf(double db) {
    if (!(db > config::MIN_DB)) return 0;
    return std::min(1.0, (db - config::MIN_DB) / (MAX_DB - config::MIN_DB));
}

double unity() { return valueOf(0); }

float gainOf(double v) { return v <= 0 ? 0.0f : (float)std::pow(10.0, dbOf(v) / 20); }

std::string dbText(double v) {
    if (v <= 0) return "-inf dB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.1f dB", dbOf(v));
    return buf;
}

Points make(std::vector<AutoPoint> points) {
    if (points.empty()) return nullptr;
    for (auto& p : points) {  // (as Automation::setPoints keeps a lane's)
        p.t = std::isfinite(p.t) ? std::max(0.0, p.t) : 0.0;
        p.v = std::isfinite(p.v) ? std::min(1.0, std::max(0.0, p.v)) : unity();
        p.bend = std::isfinite(p.bend) ? std::min(1.0, std::max(-1.0, p.bend)) : 0.0;
    }
    std::stable_sort(points.begin(), points.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
    return std::make_shared<const std::vector<AutoPoint>>(std::move(points));
}

int64_t audioFrame(const Clip& clip, int64_t frame) {
    // (looped(): its audio up to the loop's end, then the loop again and again)
    int64_t end = clip.loopFrom + clip.loopFor;
    if (clip.loopingAudio() && clip.loopFor > 0 && frame >= end) return clip.loopFrom + (frame - end) % clip.loopFor;
    return frame;
}

std::vector<int64_t> framesOf(const Clip& clip, double t, int64_t lo, int64_t hi) {
    std::vector<int64_t> out;
    int64_t f = (int64_t)std::nearbyint(t), end = clip.loopFrom + clip.loopFor;
    bool looping = clip.loopingAudio() && clip.loopFor > 0;
    if ((!looping || f < end) && lo <= f && f < hi) out.push_back(f);
    if (!looping || f < clip.loopFrom || f >= end) return out;
    int64_t into = end + (f - clip.loopFrom);  // (its first repeat)
    int64_t k = lo > into ? (lo - into + clip.loopFor - 1) / clip.loopFor : 0;
    for (int64_t at = into + k * clip.loopFor; at < hi; at += clip.loopFor) out.push_back(at);
    return out;
}

double valueAt(const Clip& clip, int64_t frame) {
    auto envelope = std::atomic_load(&clip.gainEnvelope);
    if (!envelope || envelope->empty()) return unity();
    return automation::valueAt(*envelope, (double)audioFrame(clip, frame));
}

float Walk::gainAtStep(int64_t frame) {
    return gainOf(automation::valueAt(points, (double)audioFrame(clip, frame), cursor));
}

float Walk::gain(int64_t frame) {
    if (points.empty()) return 1.0f;
    int64_t c = frame >= 0 ? frame / STEP : -((-frame + STEP - 1) / STEP);  // (floor)
    if (c != cell) {
        g0 = c == cell + 1 ? g1 : gainAtStep(c * STEP);
        g1 = gainAtStep((c + 1) * STEP);
        cell = c;
    }
    return g0 + (g1 - g0) * (float)(frame - c * STEP) / STEP;
}

std::vector<AutoPoint> scaled(const std::vector<AutoPoint>& points, double k) {
    std::vector<AutoPoint> out = points;
    for (auto& p : out) p.t *= k;
    return out;
}

std::vector<AutoPoint> mirrored(const std::vector<AutoPoint>& points, int64_t frames) {
    // frame t is heard at frames - 1 - t; a segment's bend stays as it is, on its new first point (bendShape:
    // 1 - f(1 - x) with c is f with -c, and the rise turns round too)
    std::vector<AutoPoint> out;
    out.reserve(points.size());
    for (size_t j = points.size(); j-- > 0;) {
        AutoPoint p = points[j];
        p.t = std::max(0.0, (double)(frames - 1) - p.t);
        p.bend = j > 0 ? points[j - 1].bend : 0.0;
        out.push_back(p);
    }
    return out;
}

}  // namespace clip_envelope
