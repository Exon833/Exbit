#include "model/note_tools.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>

namespace note_tools {

const char* toolName(Tool t) {
    static const char* const NAMES[TOOL_COUNT] = {"Strum",    "Arpeggiate", "Chop",     "Flam",
                                                  "Legato",   "Quantize",   "Humanize", "Velocity shape"};
    int i = (int)t;
    return i >= 0 && i < TOOL_COUNT ? NAMES[i] : "?";
}

namespace {

// the notes that aren't targets, as they are; `isTarget` marks the targets (valid indexes only, each once)
Result others(const std::vector<Note>& notes, const std::vector<int>& targets, std::vector<int>& valid) {
    std::vector<char> isTarget(notes.size(), 0);
    for (int i : targets)
        if (i >= 0 && i < (int)notes.size() && !isTarget[(size_t)i]) {
            isTarget[(size_t)i] = 1;
            valid.push_back(i);
        }
    Result r;
    r.notes.reserve(notes.size() + valid.size());
    r.from.reserve(notes.size() + valid.size());
    for (size_t i = 0; i < notes.size(); ++i)
        if (!isTarget[i]) {
            r.notes.push_back(notes[i]);
            r.from.push_back((int)i);
        }
    return r;
}

void add(Result& r, const Note& n, int from) {
    r.notes.push_back(n);
    r.from.push_back(from);
}

// targets by start (then pitch, then index)
void byStart(const std::vector<Note>& notes, std::vector<int>& idx) {
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        if (notes[a].pitch != notes[b].pitch) return notes[a].pitch < notes[b].pitch;
        return a < b;
    });
}

// targets (sorted by start) in chords: runs starting within CHORD_S of the run's first note
std::vector<std::vector<int>> chords(const std::vector<Note>& notes, const std::vector<int>& sorted) {
    std::vector<std::vector<int>> out;
    for (int i : sorted) {
        if (out.empty() || notes[i].start - notes[out.back()[0]].start > CHORD_S) out.emplace_back();
        out.back().push_back(i);
    }
    return out;
}

// 0…1 bent by curve (-1…1): 0 straight, above 0 slow then fast, below fast then slow
double bend(double frac, double curve) {
    if (curve == 0) return frac;
    return std::pow(std::max(0.0, frac), std::pow(4.0, std::clamp(curve, -1.0, 1.0)));
}

Note plain(Note n) {  // a made note: no roll, no slide
    n.repeats = 1;
    n.slide = false;
    return n;
}

void strum(const std::vector<Note>& notes, std::vector<int> idx, const Params& p, Result& r) {
    byStart(notes, idx);
    double per = std::max(0.0, p.strumS);
    for (std::vector<int>& chord : chords(notes, idx)) {
        double base = notes[chord[0]].start;
        std::stable_sort(chord.begin(), chord.end(), [&](int a, int b) {
            return p.down ? notes[a].pitch > notes[b].pitch : notes[a].pitch < notes[b].pitch;
        });
        size_t n = chord.size();
        for (size_t k = 0; k < n; ++k) {
            Note m = notes[chord[k]];
            double offset = p.curve == 0 || n < 2 ? per * (double)k
                                                  : bend((double)k / (double)(n - 1), p.curve) * per * (double)(n - 1);
            m.start = std::max(0.0, base + offset);
            m.end = std::max(m.end, m.start + MIN_LEN);
            add(r, m, chord[k]);
        }
    }
}

void arpeggiate(const std::vector<Note>& notes, std::vector<int> idx, const Params& p, Result& r) {
    byStart(notes, idx);
    double rate = std::max(MIN_LEN, p.rateS), gate = std::clamp(p.gate, 0.01, 1.0);
    std::mt19937 rng(p.seed);
    for (std::vector<int>& chord : chords(notes, idx)) {
        double base = notes[chord[0]].start, end = base;
        for (int i : chord) end = std::max(end, notes[i].end);
        std::vector<int> up;  // one note per pitch, low to high
        std::stable_sort(chord.begin(), chord.end(), [&](int a, int b) { return notes[a].pitch < notes[b].pitch; });
        for (int i : chord)
            if (up.empty() || notes[up.back()].pitch != notes[i].pitch) up.push_back(i);
        int n = (int)up.size();
        for (int s = 0; s < MAX_ARP_STEPS; ++s) {
            double t = base + rate * s;
            if (t >= end - MIN_LEN) break;
            int k = s % n;
            switch (p.style) {
            case ArpStyle::Up: break;
            case ArpStyle::Down: k = n - 1 - k; break;
            case ArpStyle::UpDown: {
                int period = std::max(1, 2 * n - 2), j = s % period;
                k = n < 2 ? 0 : j < n ? j : period - j;
                break;
            }
            case ArpStyle::Random: k = (int)(rng() % (uint32_t)n); break;
            }
            Note m = plain(notes[up[(size_t)k]]);
            m.start = t;
            m.end = std::max(t + MIN_LEN, std::min(end, t + rate * gate));
            add(r, m, up[(size_t)k]);
        }
    }
}

void chop(const std::vector<Note>& notes, const std::vector<int>& idx, const Params& p, Result& r) {
    for (int i : idx) {
        const Note& n = notes[i];
        double len = n.end - n.start;
        int count = p.chopStepS > 0 ? (int)std::ceil(len / std::max(MIN_LEN, p.chopStepS) - 1e-9) : p.pieces;
        count = std::clamp(count, 1, std::min(MAX_PIECES, std::max(1, (int)(len / MIN_LEN))));
        // by the step: pieces that long, the last one shorter (as many as allowed: equal ones)
        double piece = p.chopStepS > 0 && count * p.chopStepS >= len - 1e-9 ? p.chopStepS : len / count;
        if (count > 1 && len - (count - 1) * piece < MIN_LEN) --count;  // (a sliver left: the last piece takes it)
        for (int k = 0; k < count; ++k) {
            Note m = k ? plain(n) : n;
            m.start = n.start + piece * k;
            m.end = k == count - 1 ? n.end : n.start + piece * (k + 1);
            add(r, m, i);
        }
    }
}

void flam(const std::vector<Note>& notes, const std::vector<int>& idx, const Params& p, Result& r) {
    // the latest end of the notes of the same pitch starting before each note (any note, not just targets):
    // a grace note starts after it, so ornaments never overlap
    std::vector<int> all(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) all[i] = (int)i;
    std::sort(all.begin(), all.end(), [&](int a, int b) {
        if (notes[a].pitch != notes[b].pitch) return notes[a].pitch < notes[b].pitch;
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        return a < b;
    });
    std::vector<double> before(notes.size(), 0.0);
    double latest = 0;
    for (size_t k = 0; k < all.size();) {
        const Note& n = notes[all[k]];
        if (k == 0 || notes[all[k - 1]].pitch != n.pitch) latest = 0;
        size_t j = k;  // (notes starting together: neither is before the other)
        while (j < all.size() && notes[all[j]].pitch == n.pitch && notes[all[j]].start == n.start) ++j;
        for (size_t q = k; q < j; ++q) before[(size_t)all[q]] = latest;
        for (size_t q = k; q < j; ++q) latest = std::max(latest, notes[all[q]].end);
        k = j;
    }
    double vel = std::clamp(p.flamVelocity, 0.0, 1.0);
    for (int i : idx) {
        const Note& n = notes[i];
        add(r, n, i);
        double gs = std::max({0.0, n.start - std::max(0.0, p.flamS), before[(size_t)i]});
        if (n.start - gs < MIN_LEN) continue;  // no room before it (the clip's start, or a note of its pitch)
        Note g = plain(n);
        g.start = gs;
        g.end = n.start;
        g.velocity = std::max(1.0, std::round(n.velocity * vel));
        add(r, g, i);
    }
}

void legatoInto(const std::vector<Note>& notes, std::vector<int> idx, Result& r) {
    byStart(notes, idx);
    // the chords' starts (notes within MIN_LEN of each other start together)
    std::vector<double> starts;
    for (int i : idx)
        if (starts.empty() || notes[i].start - starts.back() >= MIN_LEN) starts.push_back(notes[i].start);
    size_t next = 0;
    for (int i : idx) {
        Note m = notes[i];
        while (next < starts.size() && starts[next] < m.start + MIN_LEN) ++next;
        if (next < starts.size()) m.end = starts[next];
        add(r, m, i);
    }
}

void quantize(const std::vector<Note>& notes, const std::vector<int>& idx, const Params& p, Result& r) {
    for (int i : idx) {
        Note n = notes[i];
        if (!p.onGrid) {
            add(r, n, i);
            continue;
        }
        double start = std::max(0.0, n.start + (p.onGrid(n.start) - n.start) * p.strength);
        double end = n.end + start - n.start;
        if (p.ends && p.stepAt) end += (std::max(p.onGrid(end), start + p.stepAt(start)) - end) * p.strength;
        n.start = start;
        n.end = std::max(end, start + MIN_LEN);
        add(r, n, i);
    }
}

void humanize(const std::vector<Note>& notes, const std::vector<int>& idx, const Params& p, Result& r) {
    std::mt19937 rng(p.seed);
    auto unit = [&] { return (double)rng() / (double)std::mt19937::max() * 2.0 - 1.0; };  // -1…1
    double range = std::max(0.0, p.timingS), vrange = std::max(0.0, p.velocityRange);
    for (int i : idx) {
        Note n = notes[i];
        double dt = unit() * range, dv = std::round(unit() * vrange);
        double start = std::max(0.0, n.start + dt);
        n.end += start - n.start;
        n.start = start;
        n.velocity = std::clamp(n.velocity + dv, 1.0, 127.0);
        add(r, n, i);
    }
}

void velocityShape(const std::vector<Note>& notes, std::vector<int> idx, const Params& p, Result& r) {
    byStart(notes, idx);
    if (idx.empty()) return;
    double first = notes[idx.front()].start, last = notes[idx.back()].start;
    size_t n = idx.size();
    for (size_t k = 0; k < n; ++k) {
        Note m = notes[idx[k]];
        double frac = last > first ? (m.start - first) / (last - first) : n > 1 ? (double)k / (double)(n - 1) : 0.0;
        m.velocity = std::clamp(std::round(p.from + (p.to - p.from) * bend(frac, -p.curve)), 1.0, 127.0);
        add(r, m, idx[k]);
    }
}

}  // namespace

Result apply(const std::vector<Note>& notes, const std::vector<int>& targets, const Params& p) {
    std::vector<int> idx;
    Result r = others(notes, targets, idx);
    switch (p.tool) {
    case Tool::Strum: strum(notes, idx, p, r); break;
    case Tool::Arpeggiate: arpeggiate(notes, idx, p, r); break;
    case Tool::Chop: chop(notes, idx, p, r); break;
    case Tool::Flam: flam(notes, idx, p, r); break;
    case Tool::Legato: legatoInto(notes, idx, r); break;
    case Tool::Quantize: quantize(notes, idx, p, r); break;
    case Tool::Humanize: humanize(notes, idx, p, r); break;
    case Tool::VelocityShape: velocityShape(notes, idx, p, r); break;
    }
    return r;
}

Result split(const std::vector<Note>& notes, const std::vector<int>& targets, double at) {
    std::vector<int> idx;
    Result r = others(notes, targets, idx);
    for (int i : idx) {
        const Note& n = notes[i];
        if (n.start + MIN_LEN <= at && at <= n.end - MIN_LEN) {
            Note a = n, b = plain(n);
            a.end = at;
            b.start = at;
            add(r, a, i);
            add(r, b, i);
        } else {
            add(r, n, i);
        }
    }
    return r;
}

Result join(const std::vector<Note>& notes, const std::vector<int>& targets) {
    std::vector<int> idx;
    Result r = others(notes, targets, idx);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (notes[a].pitch != notes[b].pitch) return notes[a].pitch < notes[b].pitch;
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        return a < b;
    });
    for (size_t k = 0; k < idx.size();) {
        Note m = notes[idx[k]];
        size_t j = k + 1;
        while (j < idx.size() && notes[idx[j]].pitch == m.pitch && notes[idx[j]].start <= m.end + 1e-9)
            m.end = std::max(m.end, notes[idx[j++]].end);
        add(r, m, idx[k]);
        k = j;
    }
    return r;
}

Result removeOverlaps(const std::vector<Note>& notes, const std::vector<int>& targets) {
    std::vector<int> idx;
    Result r = others(notes, targets, idx);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {  // by pitch, start, the longest first
        if (notes[a].pitch != notes[b].pitch) return notes[a].pitch < notes[b].pitch;
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        if (notes[a].end != notes[b].end) return notes[a].end > notes[b].end;
        return a < b;
    });
    for (size_t k = 0; k < idx.size();) {
        Note m = notes[idx[k]];
        size_t j = k + 1;  // (those starting with it: gone)
        while (j < idx.size() && notes[idx[j]].pitch == m.pitch && notes[idx[j]].start < m.start + MIN_LEN) ++j;
        if (j < idx.size() && notes[idx[j]].pitch == m.pitch) m.end = std::min(m.end, notes[idx[j]].start);
        add(r, m, idx[k]);
        k = j;
    }
    return r;
}

Result legato(const std::vector<Note>& notes, const std::vector<int>& targets) {
    Params p;
    p.tool = Tool::Legato;
    return apply(notes, targets, p);
}

std::vector<int> row(const std::vector<Note>& notes, int pitch) {
    std::vector<int> out;
    for (size_t i = 0; i < notes.size(); ++i)
        if ((int)std::lround(notes[i].pitch) == pitch) out.push_back((int)i);
    return out;
}

}  // namespace note_tools
