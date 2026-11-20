#include "model/timing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "config.h"

// ---------------------------------------------------------------------------------------------
// Frac

Frac::Frac(int64_t n, int64_t d) {
    if (d < 0) n = -n, d = -d;
    if (d == 0) d = 1;
    int64_t g = std::gcd(n < 0 ? -n : n, d);
    if (g > 1) n /= g, d /= g;
    num = n;
    den = d;
}

int64_t Frac::floor() const {
    int64_t q = num / den;
    if (num % den != 0 && num < 0) q--;
    return q;
}

Frac Frac::fromDouble(double x, int64_t maxDen) {
    // the best rational approximation with a bounded denominator (continued fractions), as
    // Python's Fraction(x).limit_denominator(maxDen) gives for the floats the program uses
    if (!std::isfinite(x)) return Frac(0);
    bool negative = x < 0;
    double v = std::fabs(x);
    int64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    double rest = v;
    for (int i = 0; i < 64; i++) {
        double a = std::floor(rest);
        if (a > 1e15) break;
        int64_t ai = (int64_t)a;
        int64_t q2 = q0 + ai * q1;
        if (q2 > maxDen) {
            // the semiconvergent with the largest denominator that fits, if nearer
            int64_t k = (maxDen - q0) / q1;
            Frac b1(p0 + k * p1, q0 + k * q1), b2(p1, q1);
            double e1 = std::fabs(b1.value() - v), e2 = std::fabs(b2.value() - v);
            Frac best = e1 < e2 ? b1 : b2;
            return negative ? Frac(-best.num, best.den) : best;
        }
        int64_t p2 = p0 + ai * p1;
        p0 = p1, q0 = q1, p1 = p2, q1 = q2;
        double f = rest - a;
        if (f < 1e-12) break;
        rest = 1.0 / f;
    }
    return Frac(negative ? -p1 : p1, q1);
}

// ---------------------------------------------------------------------------------------------

namespace timing {
const std::vector<Frac> STRAIGHT_STEPS = {Frac(1, 8), Frac(1, 4), Frac(1, 2), Frac(1, 1), Frac(2, 1)};
const std::vector<Frac> THIRDS_STEPS = {Frac(1, 12), Frac(1, 6), Frac(1, 3), Frac(2, 3), Frac(4, 3)};
const std::vector<Frac>& fixedSteps(bool thirds) {
    static const std::vector<Frac> straight = [] {
        std::vector<Frac> v = STRAIGHT_STEPS;
        v.push_back(Frac(4));
        return v;
    }();
    return thirds ? THIRDS_STEPS : straight;
}
}  // namespace timing

using timing::EPSILON;

// ---------------------------------------------------------------------------------------------
// TempoMap

TempoMap::TempoMap(double bpm_, int beatsPerBar_, std::vector<TempoChange> changes_)
    : bpm(bpm_), beatsPerBar(beatsPerBar_), changes([&] {
          std::sort(changes_.begin(), changes_.end());
          return changes_;
      }()) {
    segs_.push_back(Segment{0.0, 0.0, 0, bpm, beatsPerBar});
    for (const TempoChange& c : changes) {
        Segment last = segs_.back();
        int bars = c.bar - 1 - last.bar;
        if (bars <= 0) {  // (two changes at one bar: the later one)
            segs_.back() = Segment{last.seconds, last.beat, last.bar, c.bpm, c.beatsPerBar};
            continue;
        }
        segs_.push_back(Segment{last.seconds + bars * last.beatsPerBar * last.beatS(),
                                last.beat + (double)bars * last.beatsPerBar, c.bar - 1, c.bpm, c.beatsPerBar});
    }
}

const Segment& TempoMap::segmentAt(double seconds) const {
    for (size_t i = segs_.size(); i-- > 0;)
        if (segs_[i].seconds <= seconds + 1e-9) return segs_[i];
    return segs_[0];
}

const Segment* TempoMap::following(const Segment& s) const {
    for (auto& n : segs_)
        if (n.seconds > s.seconds + 1e-9) return &n;
    return nullptr;
}

double TempoMap::beatAt(double seconds) const {
    const Segment& s = segmentAt(seconds);
    return s.beat + (seconds - s.seconds) / s.beatS();
}

double TempoMap::secondsAt(double beat) const {
    const Segment* s = &segs_[0];
    for (size_t i = segs_.size(); i-- > 0;)
        if (segs_[i].beat <= beat + 1e-9) {
            s = &segs_[i];
            break;
        }
    return s->seconds + (beat - s->beat) * s->beatS();
}

double TempoMap::barSeconds(int bar) const {
    const Segment* s = &segs_[0];
    for (size_t i = segs_.size(); i-- > 0;)
        if (segs_[i].bar <= bar) {
            s = &segs_[i];
            break;
        }
    return s->seconds + (double)(bar - s->bar) * s->beatsPerBar * s->beatS();
}

int TempoMap::barAt(double seconds) const {
    const Segment& s = segmentAt(seconds);
    return s.bar + (int)((seconds - s.seconds) / s.beatS() / s.beatsPerBar + EPSILON);
}

void TempoMap::beatTimes(double t0, double t1, std::vector<BeatTime>& out) const {
    out.clear();
    for (size_t i = 0; i < segs_.size(); i++) {
        const Segment& s = segs_[i];
        double end = i + 1 < segs_.size() ? segs_[i + 1].seconds : std::numeric_limits<double>::infinity();
        if (end <= t0 || s.seconds > t1) continue;
        double b = s.beatS();
        int64_t k0 = std::max<int64_t>(0, (int64_t)std::ceil((std::max(t0, s.seconds) - s.seconds) / b - 1e-9));
        int64_t k1 = (int64_t)std::floor((std::min(t1, end) - s.seconds) / b + 1e-9) + 1;
        for (int64_t k = k0; k < k1; k++) {
            double at = s.seconds + (double)k * b;
            if (at < end - 1e-9 && t0 - 1e-9 <= at && at <= t1) out.push_back({at, k % s.beatsPerBar == 0});
        }
    }
}

std::vector<BeatTime> TempoMap::beatTimes(double t0, double t1) const {
    std::vector<BeatTime> out;
    beatTimes(t0, t1, out);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Timing

Timing::Timing() : bpm(config::DEFAULT_BPM), beatsPerBar(config::DEFAULT_BEATS_PER_BAR) {}

Timing::Timing(const Timing& o)
    : bpm(o.bpm), beatsPerBar(o.beatsPerBar), beats(o.beats), thirds(o.thirds), loopStart(o.loopStart),
      loopEnd(o.loopEnd), looping(o.looping), changes(o.changes), grid(o.grid), snapOn(o.snapOn), key(o.key),
      scale(o.scale), swing(o.swing), swingGrid(o.swingGrid), map_(std::atomic_load(&o.map_)) {}

Timing& Timing::operator=(const Timing& o) {
    if (this == &o) return *this;
    bpm = o.bpm;
    beatsPerBar = o.beatsPerBar;
    beats = o.beats;
    thirds = o.thirds;
    loopStart = o.loopStart;
    loopEnd = o.loopEnd;
    looping = o.looping;
    changes = o.changes;
    grid = o.grid;
    snapOn = o.snapOn;
    key = o.key;
    scale = o.scale;
    swing = o.swing;
    swingGrid = o.swingGrid;
    publish();  // (the audio thread hears the new tempo map at once)
    return *this;
}

bool Timing::loopRange(double& start, double& end) const {
    if (loopEnd - loopStart >= config::LOOP_MIN_S) {
        start = loopStart;
        end = loopEnd;
        return true;
    }
    return false;
}

TempoMapPtr Timing::map() const {
    TempoMapPtr m = std::atomic_load(&map_);
    if (m && m->bpm == bpm && m->beatsPerBar == beatsPerBar && m->changes.size() == changes.size()) {
        bool same = true;
        std::vector<TempoChange> sorted = changes;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size() && same; i++) same = sorted[i] == m->changes[i];
        if (same) return m;
    }
    m = std::make_shared<const TempoMap>(bpm, beatsPerBar, changes);
    std::atomic_store(&map_, m);
    return m;
}

TempoMapPtr Timing::audioMap() const {
    TempoMapPtr m = std::atomic_load(&map_);
    if (m) return m;
    static const TempoMapPtr fallback = std::make_shared<const TempoMap>();  // (never published yet)
    return fallback;
}

void Timing::setChange(int bar, double bpm_, int beatsPerBar_) {
    removeChange(bar);
    changes.push_back(TempoChange{bar, bpm_, beatsPerBar_});
    std::sort(changes.begin(), changes.end());
}

void Timing::removeChange(int bar) {
    changes.erase(std::remove_if(changes.begin(), changes.end(), [bar](const TempoChange& c) { return c.bar == bar; }),
                  changes.end());
}

const TempoChange* Timing::changeAtBar(int bar) const {
    for (auto& c : changes)
        if (c.bar == bar) return &c;
    return nullptr;
}

// --- the grid ---

std::optional<Frac> Timing::fixedStep() const {
    if (!(grid > 0)) return std::nullopt;
    Frac step = Frac::fromDouble(grid, 96);
    const std::vector<Frac>& steps = timing::fixedSteps(thirds);
    if (std::find(steps.begin(), steps.end(), step) != steps.end()) return step;
    // (thirds switched since: the nearest of theirs)
    Frac best = steps[0];
    double gap = std::fabs((steps[0] - step).value());
    for (auto& s : steps) {
        double g = std::fabs((s - step).value());
        if (g < gap) best = s, gap = g;
    }
    return best;
}

Frac Timing::adaptiveStep(double pps, double seconds) const {
    Segment s = segmentAt(seconds);
    double beatPx = pps * s.beatS();
    Frac bar(s.beatsPerBar);
    for (const Frac& step : thirds ? timing::THIRDS_STEPS : timing::STRAIGHT_STEPS)
        if (step < bar && (bar / step).den == 1 && step.value() * beatPx >= config::GRID_MIN_PX) return step;
    Frac step = bar;
    for (int i = 0; i < 60 && step.value() * beatPx < config::GRID_MIN_PX; i++) step = step * Frac(2);
    return step;
}

Frac Timing::gridStep(double pps, double seconds, bool drawing) const {
    Frac adaptive = adaptiveStep(pps, seconds);
    std::optional<Frac> fixed = fixedStep();
    if (!fixed) return adaptive;
    Frac step = *fixed;
    if (step == timing::WHOLE_BAR && !thirds) step = Frac(segmentAt(seconds).beatsPerBar);
    return drawing ? std::max(step, adaptive) : step;
}

void Timing::stepGrid(bool finer, double pps, double seconds) {
    const std::vector<Frac>& steps = timing::fixedSteps(thirds);
    std::optional<Frac> fixed = fixedStep();
    Frac now;
    if (fixed) {
        now = *fixed;
    } else {
        now = pps > 0 ? adaptiveStep(pps, seconds) : Frac(1, thirds ? 3 : 4);
        if (std::find(steps.begin(), steps.end(), now) == steps.end() &&
            now >= Frac(segmentAt(seconds).beatsPerBar) && !thirds)
            now = timing::WHOLE_BAR;  // (a bar or more shown: the coarsest is "1 bar")
    }
    std::vector<Frac> below, above;
    for (auto& s : steps) {
        if (s < now) below.push_back(s);
        if (s > now) above.push_back(s);
    }
    Frac chosen = finer ? (below.empty() ? steps.front() : below.back()) : (above.empty() ? steps.back() : above.front());
    grid = chosen.value();
}

std::string Timing::gridName() const {
    std::optional<Frac> step = fixedStep();
    if (!step) return "follows the zoom";
    Frac whole = *step / Frac(4);  // (in whole notes: a beat is a quarter)
    if (thirds) return "1/" + std::to_string((int)(whole.den * 2 / 3)) + " triplets";
    return *step == Frac(4) ? "1 bar" : "1/" + std::to_string(whole.den);
}

double Timing::snap(double seconds, double pps, bool floor) const {
    seconds = std::max(0.0, seconds);
    TempoMapPtr m = map();
    const Segment& s = m->segmentAt(seconds);
    double step = gridStep(pps, seconds).value();
    double bar = s.beatsPerBar;
    double at;
    double ratio = bar / step;
    if (step < bar && ratio - std::floor(ratio) > 1e-9) {  // (a step that doesn't fit a bar: counted from its bar line)
        double beats_ = (seconds - s.seconds) / s.beatS();
        double barStart = std::floor(beats_ / bar + 1e-9) * bar;
        double within = beats_ - barStart;
        double low = std::floor(within / step + 1e-9) * step;
        double high = std::min(low + step, bar);
        at = s.seconds + (barStart + (floor || within - low <= high - within ? low : high)) * s.beatS();
    } else {
        double steps = (seconds - s.seconds) / s.beatS() / step;
        // Python's round() rounds halves to even; std::nearbyint does the same in the default mode
        at = s.seconds + (floor ? std::floor(steps + 1e-9) : std::nearbyint(steps)) * step * s.beatS();
    }
    const Segment* next = m->following(s);
    if (next && !floor && std::fabs(next->seconds - seconds) < std::fabs(at - seconds))
        at = next->seconds;  // (the next segment's first bar is nearer)
    return std::max(0.0, at);
}

std::vector<GridLine> Timing::lines(double t0, double t1, double pps) const {
    std::vector<GridLine> found;
    for (auto& line : linesBetween(t0, t1, pps))
        if (t0 - 1e-9 <= line.seconds && line.seconds <= t1 + 1e-9) found.push_back(std::move(line));
    return found;
}

std::vector<GridLine> Timing::linesBetween(double t0, double t1, double pps) const {
    std::vector<GridLine> found;
    TempoMapPtr m = map();
    const std::vector<Segment>& segs = m->segments();
    const double inf = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < segs.size(); i++) {
        const Segment& s = segs[i];
        double end = i + 1 < segs.size() ? segs[i + 1].seconds : inf;
        if (end < t0 || s.seconds > t1) continue;
        Frac step = gridStep(pps, s.seconds, true);
        int barBeats = s.beatsPerBar;
        Frac bar(barBeats);
        double beatPx = pps * s.beatS();
        int64_t every = 1;  // label every this many bars
        while ((double)every * barBeats * beatPx < config::BAR_LABEL_MIN_PX && every < (int64_t(1) << 40)) every *= 2;
        bool labelBeats = beatPx >= config::BEAT_LABEL_MIN_PX && (Frac(1) / step).den == 1;
        double stepV = step.value();
        int64_t first = std::max<int64_t>(0, (int64_t)std::floor((std::max(t0, s.seconds) - s.seconds) / s.beatS() / stepV));
        int64_t last = (int64_t)std::ceil((std::min(t1, end < inf ? end : t1) - s.seconds) / s.beatS() / stepV) + 1;
        std::vector<Frac> positions;
        if (step < bar && (bar / step).den != 1) {  // (a step that doesn't fit a bar: from each bar line)
            int64_t perBar = (bar / step).floor() + 1;  // ceil(bar / step) (never a whole number here)
            int64_t b0 = (Frac(first) * step / bar).floor(), b1 = (Frac(last) * step / bar).floor() + 2;
            for (int64_t b = b0; b < b1; b++)
                for (int64_t j = 0; j < perBar; j++) positions.push_back(Frac(b) * bar + Frac(j) * step);
        } else {
            for (int64_t k = first; k < last; k++) positions.push_back(Frac(k) * step);
        }
        for (const Frac& pos : positions) {
            double seconds = s.seconds + pos.value() * s.beatS();
            if (seconds >= end - 1e-9) break;
            int64_t barIndex = (pos / bar).floor();
            Frac beat = pos - Frac(barIndex) * bar;
            int64_t barNumber = s.bar + barIndex;
            GridLine line{seconds, timing::FINE, std::string()};
            if (beat.num == 0) {
                line.strength = timing::BAR;
                if (barNumber % every == 0) line.label = std::to_string(barNumber + 1);
            } else if (beat.den == 1) {
                line.strength = timing::BEAT;
                if (labelBeats) line.label = std::to_string(barNumber + 1) + "." + std::to_string(beat.num + 1);
            }
            found.push_back(std::move(line));
        }
    }
    return found;
}

void Timing::position(double seconds, int& bar, int& beat, int& part) const {
    Segment s = segmentAt(seconds);
    double beats_ = (seconds - s.seconds) / s.beatS() + EPSILON;
    double b = std::floor(beats_ / s.beatsPerBar);
    double within = beats_ - b * s.beatsPerBar;  // Python's divmod (floor)
    bar = s.bar + (int)b + 1;
    beat = (int)std::floor(within) + 1;
    double frac = within - std::floor(within);
    part = (int)(frac * (thirds ? 3 : 4)) + 1;
}

double TapTempo::tap(double now) {
    if (!taps_.empty() && now - taps_.back() > config::TAP_RESET_S) taps_.clear();
    taps_.push_back(now);
    if ((int)taps_.size() > config::TAP_COUNT) taps_.erase(taps_.begin(), taps_.end() - config::TAP_COUNT);
    if (taps_.size() < 2 || taps_.back() <= taps_.front()) return 0;
    double bpm = 60.0 * (taps_.size() - 1) / (taps_.back() - taps_.front());
    bpm = std::min(std::max(bpm, config::MIN_BPM), config::MAX_BPM);
    return std::nearbyint(bpm * 100) / 100;
}
