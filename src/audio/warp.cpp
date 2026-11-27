// Warping (see warp.h): the markers' beat map, BEATS (pieces placed at their warped times) and the vocoder of
// TONES and COMPLEX (a time-varying, batch-parallel phase vocoder with identity phase locking).
#include "audio/warp.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <deque>
#include <functional>
#include <mutex>

#include "audio/fft.h"
#include "audio/stretch.h"
#include "audio/workers.h"
#include "core/util.h"

namespace warp {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
inline double princarg(double a) { return a - kTwoPi * std::nearbyint(a / kTwoPi); }

const char* const MODE_NAMES[] = {"Beats", "Tones", "Complex"};
const char* const PRESERVE_NAMES[] = {"transients", "1/4", "1/8", "1/16"};

}  // namespace

const char* modeName(Mode m) { return MODE_NAMES[std::max(0, std::min(2, (int)m))]; }

Mode modeFromName(const std::string& name) {
    for (int i = 0; i < 3; i++)
        if (util::lower(name) == util::lower(MODE_NAMES[i])) return (Mode)i;
    return Mode::Beats;
}

const char* preserveName(int p) { return PRESERVE_NAMES[std::max(0, std::min(3, p))]; }

int preserveFromName(const std::string& name) {
    for (int i = 0; i < 4; i++)
        if (name == PRESERVE_NAMES[i]) return i;
    return TRANSIENTS;
}

// --- markers ---

Markers ordered(Markers m) {
    std::stable_sort(m.begin(), m.end(), [](const Marker& a, const Marker& b) { return a.frame < b.frame; });
    Markers out;
    for (auto& k : m) {
        if (!std::isfinite(k.frame) || !std::isfinite(k.beat)) continue;
        if (!out.empty() && (k.frame <= out.back().frame || k.beat <= out.back().beat)) continue;
        out.push_back(k);
    }
    return out;
}

Markers straight(int64_t frames, int sourceSr, double bpm) {
    if (bpm <= 0) bpm = DEFAULT_BPM;
    if (frames <= 0 || sourceSr <= 0) return {Marker{0, 0}};
    return {Marker{0, 0}, Marker{(double)frames, (double)frames / sourceSr * bpm / 60.0}};
}

BeatMap::BeatMap(Markers markers, double bpm, int sourceSr)
    : m_(ordered(std::move(markers))), fpb_(std::max(1, sourceSr) * 60.0 / (bpm > 0 ? bpm : DEFAULT_BPM)) {}

double BeatMap::frameAt(double beat) const {
    if (m_.empty()) return beat * fpb_;
    if (m_.size() == 1) return m_[0].frame + (beat - m_[0].beat) * fpb_;
    auto it = std::upper_bound(m_.begin(), m_.end(), beat, [](double b, const Marker& k) { return b < k.beat; });
    size_t i = (size_t)std::max<ptrdiff_t>(0, std::min<ptrdiff_t>((ptrdiff_t)m_.size() - 2, (it - m_.begin()) - 1));
    const Marker &a = m_[i], &b = m_[i + 1];
    return a.frame + (beat - a.beat) * (b.frame - a.frame) / (b.beat - a.beat);
}

double BeatMap::beatAt(double frame) const {
    if (m_.empty()) return frame / fpb_;
    if (m_.size() == 1) return m_[0].beat + (frame - m_[0].frame) / fpb_;
    auto it = std::upper_bound(m_.begin(), m_.end(), frame, [](double f, const Marker& k) { return f < k.frame; });
    size_t i = (size_t)std::max<ptrdiff_t>(0, std::min<ptrdiff_t>((ptrdiff_t)m_.size() - 2, (it - m_.begin()) - 1));
    const Marker &a = m_[i], &b = m_[i + 1];
    return a.beat + (frame - a.frame) * (b.beat - a.beat) / (b.frame - a.frame);
}

bool sameTempo(const TempoMap& a, const TempoMap& b) {
    return &a == &b || (a.bpm == b.bpm && a.beatsPerBar == b.beatsPerBar && a.beatUnit == b.beatUnit &&
                         a.changes == b.changes && a.ramp == b.ramp);
}

// --- rendering ---

namespace {

// a job worked out: where things are, in WORK frames (the result before it is resampled to the engine's rate:
// the source's rate times the pitch ratio, so a piece played at its own speed there comes out higher or lower)
struct Ctx {
    const AudioBuffer& src;
    int64_t n;      // source frames
    int S;          // source rate
    double rate;    // work frames per second
    const TempoMap& map;
    double origin, b0;  // song seconds and beat of work frame 0
    BeatMap bm;
    int64_t lw;     // work frames
    double srcAt(double y) const { return bm.frameAt(map.beatAt(origin + y / rate) - b0); }
    double workAt(double f) const { return (map.secondsAt(b0 + bm.beatAt(f)) - origin) * rate; }
    float at(int64_t p, int c) const { return p >= 0 && p < n ? src.samples[(size_t)(p * 2 + c)] : 0.0f; }
    // the same, the audio mirrored beyond its start and end (the vocoder's reading: a frame reading past either
    // sees no step from silence, which turned phases make a click when the audio starts or ends mid-wave)
    float mirrored(int64_t p, int c) const {
        if (p < 0) p = -p;
        if (p >= n) p = 2 * (n - 1) - p;
        return at(std::max<int64_t>(0, std::min<int64_t>(n - 1, p)), c);  // (a source shorter than half a frame)
    }
};

// fn(0 … jobs-1), on the pool's threads and this one (or here alone)
template <class F>
void parallel(workers::Pool* pool, int jobs, F& fn) {
    if (jobs <= 0) return;
    if (!pool || jobs == 1) {
        for (int i = 0; i < jobs; i++) fn(i);
        return;
    }
    pool->run(jobs, [](void* ctx, int i) { (*static_cast<F*>(ctx))(i); }, &fn);
}

// --- BEATS ---
void beats(const Ctx& c, const Job& j, float* out, workers::Pool* pool) {
    const int64_t N = c.n;
    const int S = c.S;
    std::vector<int64_t> cuts{0};
    if (j.preserve == TRANSIENTS) {
        auto t = transients(j.source, S);
        cuts.insert(cuts.end(), t->begin(), t->end());
    } else {
        double step = j.preserve == QUARTERS ? 1.0 : j.preserve == EIGHTHS ? 0.5 : 0.25;
        double end = c.bm.beatAt((double)N);
        for (double b = std::ceil(c.bm.beatAt(0) / step) * step; b < end; b += step) cuts.push_back(std::llround(c.bm.frameAt(b)));
    }
    for (auto& m : c.bm.markers()) cuts.push_back(std::llround(m.frame));
    std::sort(cuts.begin(), cuts.end());
    const int64_t F = std::max<int64_t>(8, std::llround(0.004 * S));        // the crossfade between pieces
    const int64_t gap = std::max<int64_t>(2 * F, std::llround(0.02 * S));   // pieces at least this long
    std::vector<int64_t> pieces;
    for (int64_t cut : cuts) {
        if (cut < 0 || cut >= N) continue;
        if (!pieces.empty() && cut - pieces.back() < gap) continue;
        pieces.push_back(cut);
    }
    if (pieces.empty()) return;
    const int n = (int)pieces.size();
    std::vector<int64_t> o((size_t)n + 1);
    for (int i = 0; i < n; i++) o[(size_t)i] = std::llround(c.workAt((double)pieces[(size_t)i]));
    o[(size_t)n] = std::max(c.lw, o[(size_t)n - 1]);
    for (int i = 1; i <= n; i++) o[(size_t)i] = std::max(o[(size_t)i], o[(size_t)i - 1]);
    const int64_t tailMax = std::max<int64_t>(8, std::llround(0.05 * S));
    auto piece = [&](int i) {
        int64_t s0 = pieces[(size_t)i], s1 = i + 1 < n ? pieces[(size_t)i + 1] : N;
        int64_t o0 = o[(size_t)i], len = o[(size_t)i + 1] - o0, slen = s1 - s0;
        if (len <= 0) return;
        int64_t pre = i > 0 ? std::min(F, len) : 0;     // fading in just before its onset…
        int64_t fo = i + 1 < n ? std::min(F, len) : 0;  // …and out just before the next one's
        bool looping = len > slen && slen >= 8;         // slowed down: a loop of its tail fills the gap
        int64_t tl = std::max<int64_t>(4, std::min(tailMax, slen)), cf = std::max<int64_t>(1, tl / 4), period = tl - cf;
        int64_t a = s1 - tl;
        for (int64_t d = -pre; d < len; d++) {
            int64_t y = o0 + d;
            if (y < 0) continue;
            if (y >= c.lw) break;
            if (d >= slen && !looping) break;
            double g = 1;
            if (d < 0) g = (double)(d + pre) / (double)pre;
            else if (fo && d >= len - fo) g = (double)(len - d) / (double)fo;
            int64_t v = s0 + d;
            float l, r;
            if (!looping || v < s1 - cf) {
                l = c.at(v, 0);
                r = c.at(v, 1);
            } else {
                int64_t w = (v - (s1 - cf)) % period;
                if (w < cf) {  // (each pass's end crossfaded into the next pass's start)
                    float x = (float)w / (float)cf;
                    l = (1 - x) * c.at(s1 - cf + w, 0) + x * c.at(a + w, 0);
                    r = (1 - x) * c.at(s1 - cf + w, 1) + x * c.at(a + w, 1);
                } else {
                    l = c.at(a + w, 0);
                    r = c.at(a + w, 1);
                }
                if (d >= slen) g *= std::max(0.0, 1.0 - (double)(d - slen) / (double)(len - slen));
            }
            out[2 * y] += (float)(g * l);
            out[2 * y + 1] += (float)(g * r);
        }
    };
    // blocks of pieces, the even ones then the odd ones: neighbours (whose crossfades overlap) never at once
    // shortcut: a block is assumed to span more than a crossfade (true below ~1000 times the speed)
    const int BLOCK = 256, blocks = (n + BLOCK - 1) / BLOCK;
    for (int parity = 0; parity < 2; parity++) {
        auto block = [&](int b) {
            int k = 2 * b + parity;
            for (int i = k * BLOCK; i < std::min(n, (k + 1) * BLOCK); i++) piece(i);
        };
        parallel(pool, (blocks - parity + 1) / 2, block);
    }
    util::debugf("warp: Beats (%s): %d pieces, crossfades of %lld frames\n", preserveName(j.preserve), n, (long long)F);
}

// --- the vocoder (TONES, COMPLEX) ---

struct Band {
    int n, hop;
    int side;  // 0: every frequency; 1: the bass (below ~150-300 Hz); 2: the rest
};

constexpr double LOW_HZ = 150, HIGH_HZ = 300;  // where COMPLEX's bass band hands over
constexpr int RAMP = 32;                       // frames over which a frame's reading fades at an onset
// the least window weight an output frame is divided by (a whole one is 1.5: Hann squared, a quarter apart). Only
// at an onset, where the frames before it have faded out and the ones after it not yet in, is there less: there
// the sum is quieter for a few frames instead of what little the frames put there blown up into a click.
constexpr float MIN_WEIGHT = 0.1f;

// `srcCentre`: the source frame each work frame is read around; `onsets`: frames no frame reads across (each
// reads only between the onsets around its centre); `resetUntil[i]`: frames centred from onset i up to here
// start from their own phases (as does each first frame after an onset). Added into `out` (lw work frames).
void vocode(const Ctx& c, const std::function<double(double)>& srcCentre, const Band& band,
            const std::vector<int64_t>& onsets, const std::vector<int64_t>& resetUntil, float* out,
            workers::Pool* pool) {
    const int N = band.n, half = N / 2, nb = half + 1, hop = band.hop, lag = N / 8;
    std::vector<double> win((size_t)N);
    for (int i = 0; i < N; i++) win[(size_t)i] = 0.5 - 0.5 * std::cos(kTwoPi * i / N);  // (periodic Hann)
    std::vector<double> bw((size_t)nb, 1.0);  // each bin's share in this band
    if (band.side) {
        for (int b = 0; b < nb; b++) {
            double hz = (double)b * c.S / N;
            double low = hz <= LOW_HZ ? 1.0 : hz >= HIGH_HZ ? 0.0 : (HIGH_HZ - hz) / (HIGH_HZ - LOW_HZ);
            bw[(size_t)b] = band.side == 1 ? low : 1.0 - low;
        }
    }
    fft::Plan plan(N);
    const int64_t kFirst = -(half / hop), kLast = (c.lw + half) / hop + 1;
    struct Frame {
        int64_t x = 0;
        int zone = -1;
        std::vector<fft::cd> L, R;
        std::vector<double> ph, mag, om, th, oa, ob;
        std::vector<float> mask;
    };
    const int threads = pool ? pool->threads() + 1 : 1;
    const int B = (int)std::min<int64_t>(kLast - kFirst + 1, std::max(32, 8 * threads));
    std::vector<Frame> fr((size_t)B);
    for (auto& f : fr) {
        f.L.resize((size_t)nb);
        f.R.resize((size_t)nb);
        for (auto* v : {&f.ph, &f.mag, &f.om, &f.th}) v->resize((size_t)nb);
        f.oa.resize((size_t)N);
        f.ob.resize((size_t)N);
        f.mask.resize((size_t)N);
    }
    const int64_t NONE_BEFORE = LLONG_MIN / 4, NONE_AFTER = LLONG_MAX / 4;
    auto maskAt = [](int64_t p, int64_t zl, int64_t zr) -> float {
        if (p < zl || p >= zr) return 0.0f;
        float m = 1.0f;
        if (p - zl < RAMP) m = ((float)(p - zl) + 0.5f) / RAMP;
        if (zr - p <= RAMP) m = std::min(m, ((float)(zr - p) - 0.5f) / RAMP);
        return m;
    };
    // the sums of the frames put back (and of their windows' weights) for the output still open: from `base`
    const int64_t span = (int64_t)B * hop + N;
    std::vector<float> acc((size_t)span * 2, 0.0f), wsum((size_t)span, 0.0f);
    int64_t base = kFirst * hop - half;
    auto flush = [&](int64_t upTo) {  // output frames before upTo are complete: into `out`
        int64_t count = std::min(span, std::max<int64_t>(0, upTo - base));
        for (int64_t i = 0; i < count; i++) {
            int64_t p = base + i;
            if (p < 0 || p >= c.lw) continue;
            float w = std::max(wsum[(size_t)i], MIN_WEIGHT);
            out[2 * p] += acc[(size_t)(2 * i)] / w;
            out[2 * p + 1] += acc[(size_t)(2 * i + 1)] / w;
        }
        std::copy(acc.begin() + 2 * count, acc.end(), acc.begin());
        std::fill(acc.end() - 2 * count, acc.end(), 0.0f);
        std::copy(wsum.begin() + count, wsum.end(), wsum.begin());
        std::fill(wsum.end() - count, wsum.end(), 0.0f);
        base += count;
    };
    std::vector<double> theta((size_t)nb, 0.0);
    std::vector<int> peaks, owner((size_t)nb);
    bool first = true;
    int prevZone = INT_MIN;
    for (int64_t k0 = kFirst; k0 <= kLast; k0 += B) {
        const int cnt = (int)std::min<int64_t>(B, kLast - k0 + 1);
        auto analyse = [&](int i) {
            Frame& f = fr[(size_t)i];
            double y = (double)((k0 + i) * hop);
            f.x = std::llround(srcCentre(y));
            int zone = (int)(std::upper_bound(onsets.begin(), onsets.end(), f.x) - onsets.begin()) - 1;
            f.zone = zone;
            int64_t zl = zone >= 0 ? onsets[(size_t)zone] : NONE_BEFORE;
            int64_t zr = zone + 1 < (int)onsets.size() ? onsets[(size_t)zone + 1] : NONE_AFTER;
            thread_local std::vector<double> a, b, ref;
            thread_local std::vector<fft::cd> work, refSpec;
            a.resize((size_t)N);
            b.resize((size_t)N);
            ref.resize((size_t)N);
            refSpec.resize((size_t)nb);
            for (int j = 0; j < N; j++) {
                int64_t p = f.x - half + j, q = p - lag;
                float m = maskAt(p, zl, zr), mq = maskAt(q, zl, zr);
                f.mask[(size_t)j] = m;
                double w = win[(size_t)j];
                a[(size_t)j] = c.mirrored(p, 0) * w * m;
                b[(size_t)j] = c.mirrored(p, 1) * w * m;
                ref[(size_t)j] = ((double)c.mirrored(q, 0) + c.mirrored(q, 1)) * w * mq;
            }
            fft::realPair(plan, a.data(), b.data(), f.L.data(), f.R.data(), work);
            fft::realPair(plan, ref.data(), nullptr, refSpec.data(), nullptr, work);
            for (int k = 0; k < nb; k++) {
                fft::cd mono = f.L[(size_t)k] + f.R[(size_t)k];
                f.mag[(size_t)k] = std::abs(mono);
                f.ph[(size_t)k] = std::arg(mono);
                // its instantaneous frequency (radians per frame), from how far its phase turned since `lag` before
                double expected = kTwoPi * k * lag / N;
                f.om[(size_t)k] = (expected + princarg(f.ph[(size_t)k] - std::arg(refSpec[(size_t)k]) - expected)) / lag;
            }
        };
        parallel(pool, cnt, analyse);
        for (int i = 0; i < cnt; i++) {  // the phases, carried on in order
            Frame& f = fr[(size_t)i];
            bool reset = first || f.zone != prevZone ||
                         (f.zone >= 0 && f.zone < (int)resetUntil.size() && f.x <= resetUntil[(size_t)f.zone]);
            if (reset) {
                theta = f.ph;
            } else {
                peaks.clear();
                for (int k = 1; k < nb - 1; k++)
                    if (f.mag[(size_t)k] > f.mag[(size_t)k - 1] && f.mag[(size_t)k] >= f.mag[(size_t)k + 1]) peaks.push_back(k);
                if (peaks.empty()) {
                    for (int k = 0; k < nb; k++) theta[(size_t)k] += f.om[(size_t)k] * hop;
                } else {  // each bin follows the peak it lies under (identity phase locking)
                    size_t o = 0;
                    for (int k = 0; k < nb; k++) {
                        while (o + 1 < peaks.size() && (peaks[o] + peaks[o + 1]) / 2 <= k) ++o;
                        owner[(size_t)k] = (int)o;
                    }
                    for (int p : peaks) theta[(size_t)p] += f.om[(size_t)p] * hop;
                    for (int k = 0; k < nb; k++) {
                        int p = peaks[(size_t)owner[(size_t)k]];
                        if (k != p) theta[(size_t)k] = theta[(size_t)p] + f.ph[(size_t)k] - f.ph[(size_t)p];
                    }
                }
            }
            for (auto& t : theta) t = princarg(t);  // (kept small: no precision lost over hours)
            f.th = theta;
            first = false;
            prevZone = f.zone;
        }
        auto synth = [&](int i) {
            Frame& f = fr[(size_t)i];
            thread_local std::vector<fft::cd> A, Bv, work;
            A.resize((size_t)nb);
            Bv.resize((size_t)nb);
            for (int k = 0; k < nb; k++) {
                fft::cd turn = std::polar(bw[(size_t)k], f.th[(size_t)k] - f.ph[(size_t)k]);
                A[(size_t)k] = f.L[(size_t)k] * turn;
                Bv[(size_t)k] = f.R[(size_t)k] * turn;
            }
            fft::realPairInverse(plan, A.data(), Bv.data(), f.oa.data(), f.ob.data(), work);
        };
        parallel(pool, cnt, synth);
        for (int i = 0; i < cnt; i++) {  // put back together
            const Frame& f = fr[(size_t)i];
            int64_t at = (k0 + i) * hop - half - base;
            for (int j = 0; j < N; j++) {
                // put back through the window and the mask it was read with, and weighed as it was: a frame's
                // phases turned spread its audio over the whole frame, beyond the mask too, and that must not
                // count where its weight says it has none (divided by next to nothing, it clicked)
                double w = win[(size_t)j] * f.mask[(size_t)j];
                acc[(size_t)(2 * (at + j))] += (float)(f.oa[(size_t)j] * w);
                acc[(size_t)(2 * (at + j) + 1)] += (float)(f.ob[(size_t)j] * w);
                wsum[(size_t)(at + j)] += (float)(w * w);
            }
        }
        flush((k0 + cnt) * hop - half);
    }
    flush(base + span);
}

// COMPLEX's reading of the source: around each onset at its own speed (PRE frames before, POST after: whole
// frames of the short window fit there, so the attack is read and put back where it was), the warping's speeding
// up or slowing down taken up between onsets, following the warping's own curve there
struct Anchored {
    const Ctx& c;
    std::vector<double> ay, ax;  // (work frame, source frame) in threes per onset: PRE before, at, POST after
    double at(double y) const {
        if (ay.empty()) return c.srcAt(y);
        size_t j = (size_t)(std::upper_bound(ay.begin(), ay.end(), y) - ay.begin());
        if (j == 0) {  // before the first onset: from the warping's own reading at 0 to the first anchor
            double t = ay[0] > 0 ? std::max(0.0, std::min(1.0, y / ay[0])) : 1.0;
            return c.srcAt(y) + t * (ax[0] - c.srcAt(ay[0]));
        }
        if (j == ay.size()) {  // after the last one: back to the warping's own reading at the end
            double ye = ay.back(), t = (double)c.lw > ye ? std::max(0.0, std::min(1.0, ((double)c.lw - y) / ((double)c.lw - ye))) : 1.0;
            return c.srcAt(y) + t * (ax.back() - c.srcAt(ye));
        }
        size_t i = j - 1;
        double ya = ay[i], yb = ay[i + 1], xa = ax[i], xb = ax[i + 1];
        if (yb <= ya) return xb;
        double t = (y - ya) / (yb - ya);
        if (i % 3 != 2) return xa + t * (xb - xa);  // (around an onset: its own speed)
        double ca = xa - c.srcAt(ya), cb = xb - c.srcAt(yb);
        return c.srcAt(y) + ca + t * (cb - ca);
    }
};

}  // namespace

double seconds(const Job& j) {
    if (!j.source || j.source->frames() == 0 || j.sourceSr <= 0) return 0;
    TempoMapPtr map = j.map ? j.map : std::make_shared<const TempoMap>();
    BeatMap bm(j.markers, j.bpm, j.sourceSr);
    double b0 = map->beatAt(j.origin);
    return std::max(0.0, map->secondsAt(b0 + bm.beatAt((double)j.source->frames())) - j.origin);
}

AudioPtr render(const Job& j) {
    auto result = std::make_shared<AudioBuffer>();
    if (!j.source || j.source->frames() == 0 || j.sr <= 0 || j.sourceSr <= 0) return result;
    auto started = std::chrono::steady_clock::now();
    TempoMapPtr map = j.map ? j.map : std::make_shared<const TempoMap>();
    const double r = std::pow(2.0, j.semitones / 12.0);
    const double T = std::min(seconds(j), 48 * 3600.0);  // (never more than two days of audio)
    if (!(T > 0)) return result;
    Ctx c{*j.source, j.source->frames(), j.sourceSr, j.sourceSr * r, *map, j.origin, map->beatAt(j.origin),
          BeatMap(j.markers, j.bpm, j.sourceSr), 0};
    c.lw = std::max<int64_t>(1, std::llround(T * c.rate));
    std::vector<float> work((size_t)c.lw * 2, 0.0f);
    std::unique_ptr<workers::Pool> pool;
    int threads = workers::inJob() ? 0 : workers::workersFor(workers::chosenCores());
    if (threads > 0 && c.lw > 2 * (int64_t)j.sourceSr) pool = std::make_unique<workers::Pool>(threads, false);
    auto plain = [&c](double y) { return c.srcAt(y); };
    if (j.mode == Mode::Beats) {
        beats(c, j, work.data(), pool.get());
    } else if (j.mode == Mode::Tones) {
        vocode(c, plain, Band{stretch::N_FFT, stretch::HOP, 0}, {}, {}, work.data(), pool.get());
    } else {
        // the onsets (at least two short frames apart), and the room each gets at its own speed
        const Band low{2 * stretch::N_FFT, 2 * stretch::HOP, 1}, high{stretch::N_FFT, stretch::HOP, 2};
        const double PRE = high.n / 2, POST = high.n * 3 / 4;
        std::vector<int64_t> on;
        for (int64_t f : *transients(j.source, j.sourceSr))
            if (f > 0 && f < c.n && (on.empty() || f - on.back() >= 2 * high.hop)) on.push_back(f);
        size_t m = on.size();
        std::vector<double> o(m), pre(m, PRE), post(m, POST);
        for (size_t i = 0; i < m; i++) o[i] = c.workAt((double)on[i]);
        for (size_t i = 0; i <= m; i++) {  // (each gap between onsets keeps a fifth for the speed change)
            double roomO = i == 0 ? (m ? o[0] : 0) : i == m ? (double)c.lw - o[m - 1] : o[i] - o[i - 1];
            double roomS = i == 0 ? (m ? (double)on[0] : 0) : i == m ? (double)(c.n - on[m - 1]) : (double)(on[i] - on[i - 1]);
            double room = 0.8 * std::max(0.0, std::min(roomO, roomS));
            double want = (i > 0 ? POST : 0) + (i < m ? PRE : 0);
            double k = want > room ? room / want : 1.0;
            if (i > 0) post[i - 1] = POST * k;
            if (i < m) pre[i] = PRE * k;
        }
        Anchored anchored{c, {}, {}};
        std::vector<int64_t> resetHigh(m), resetLow(m);
        for (size_t i = 0; i < m; i++) {
            double x = (double)on[i];
            anchored.ay.insert(anchored.ay.end(), {o[i] - pre[i], o[i], o[i] + post[i]});
            anchored.ax.insert(anchored.ax.end(), {x - pre[i], x, x + post[i]});
            resetHigh[i] = on[i] + (int64_t)post[i] - high.n / 2;
            resetLow[i] = on[i] + (int64_t)post[i] - low.n / 2;
        }
        auto read = [&anchored](double y) { return anchored.at(y); };
        vocode(c, read, low, on, resetLow, work.data(), pool.get());
        vocode(c, read, high, on, resetHigh, work.data(), pool.get());
        util::debugf("warp: Complex: %zu onsets kept (phase reset, read at their own speed)\n", m);
    }
    int64_t frames = std::max<int64_t>(1, std::llround(T * j.sr));
    if (frames == c.lw) result->samples = std::move(work);
    else result->samples = stretch::resampleTo(work.data(), c.lw, 2, frames);
    if (j.reverse) {
        float* s = result->samples.data();
        for (int64_t i = 0, k = result->frames() - 1; i < k; ++i, --k) {
            std::swap(s[2 * i], s[2 * k]);
            std::swap(s[2 * i + 1], s[2 * k + 1]);
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    util::debugf("warp: %s, %.2f s of audio at %d Hz -> %.2f s (%zu markers, %+.2f semitones, %d threads) in %.0f ms\n",
                 modeName(j.mode), (double)c.n / j.sourceSr, j.sourceSr, T, c.bm.markers().size(), j.semitones,
                 pool ? pool->threads() + 1 : 1, ms);
    return result;
}

// --- kept renderings and transients ---

namespace {

struct Entry {
    Job job;  // (its source held weakly below: a freed source's address may come back, then it's no hit)
    const AudioBuffer* key;
    std::weak_ptr<const AudioBuffer> source;
    AudioPtr result;
};

std::mutex g_mutex;
std::deque<Entry> g_cache;  // oldest first

bool sameJob(const Job& a, const Job& b) {
    if (a.sourceSr != b.sourceSr || a.markers != b.markers || a.bpm != b.bpm || a.mode != b.mode ||
        (a.mode == Mode::Beats && a.preserve != b.preserve) || a.semitones != b.semitones || a.reverse != b.reverse ||
        a.sr != b.sr)
        return false;
    TempoMap fallback;
    const TempoMap& ma = a.map ? *a.map : fallback;
    const TempoMap& mb = b.map ? *b.map : fallback;
    if (!sameTempo(ma, mb)) return false;
    // one steady tempo: where it plays changes nothing
    return !ma.hasChanges() || std::fabs(a.origin - b.origin) < 0.5 / std::max(1, a.sr);
}

struct Transients {
    const AudioBuffer* key;
    std::weak_ptr<const AudioBuffer> source;
    int sr;
    std::shared_ptr<const std::vector<int64_t>> found;
};
std::mutex t_mutex;
std::deque<Transients> t_cache;

}  // namespace

AudioPtr rendered(const Job& job) {
    if (!job.source) return job.source;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
            if (it->key == job.source.get() && sameJob(it->job, job)) {
                if (it->source.lock() == job.source) return it->result;
                g_cache.erase(it);
                break;
            }
        }
    }
    AudioPtr result = render(job);
    Entry e{job, job.source.get(), job.source, result};
    e.job.source = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    while ((int)g_cache.size() >= CACHE) g_cache.pop_front();
    g_cache.push_back(std::move(e));
    return result;
}

std::shared_ptr<const std::vector<int64_t>> transients(const AudioPtr& source, int sourceSr) {
    static const auto none = std::make_shared<const std::vector<int64_t>>();
    if (!source || source->frames() == 0) return none;
    {
        std::lock_guard<std::mutex> lock(t_mutex);
        for (auto& t : t_cache)
            if (t.key == source.get() && t.sr == sourceSr && t.source.lock() == source) return t.found;
    }
    auto started = std::chrono::steady_clock::now();
    dsp::Onsets onsets(source->samples.data(), source->frames(), sourceSr);
    auto found = std::make_shared<const std::vector<int64_t>>(onsets.find(0.5));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    util::debugf("warp: %zu transients in %.2f s of audio (%.0f ms)\n", found->size(), (double)source->frames() / sourceSr, ms);
    std::lock_guard<std::mutex> lock(t_mutex);
    while ((int)t_cache.size() >= CACHE) t_cache.pop_front();
    t_cache.push_back(Transients{source.get(), source, sourceSr, found});
    return found;
}

}  // namespace warp
