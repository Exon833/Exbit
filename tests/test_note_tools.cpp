// model/note_tools: the MIDI editor's note tools as pure transforms, exactly (strum spacing, arpeggio orders,
// chop counts, flam grace notes that never overlap, legato ends, quantize, humanize, velocity shape, split, join,
// remove overlaps, a pitch's row), at the clip's start (nothing before 0), with bad targets, and every tool on
// 100,000 notes (fast, and the same rules kept).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "check.h"
#include "model/note_tools.h"

using namespace note_tools;

namespace {

Note note(double s, double e, int pitch, int vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

std::vector<int> all(const std::vector<Note>& ns) {
    std::vector<int> v;
    for (int i = 0; i < (int)ns.size(); ++i) v.push_back(i);
    return v;
}

// the result's notes sorted by start, then pitch
std::vector<Note> sorted(Result r) {
    std::stable_sort(r.notes.begin(), r.notes.end(), [](const Note& a, const Note& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    return r.notes;
}

// the rules every result keeps
bool sane(const std::vector<Note>& in, const Result& r) {
    if (r.notes.size() != r.from.size()) return false;
    for (size_t k = 0; k < r.notes.size(); ++k) {
        const Note& n = r.notes[k];
        if (n.start < 0 || n.end - n.start < MIN_LEN - 1e-12) return false;
        if (r.from[k] < 0 || r.from[k] >= (int)in.size()) return false;
        if (n.velocity < 1 || n.velocity > 127) return false;
    }
    return true;
}

double seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

static void testStrum() {
    std::vector<Note> chord = {note(1.0, 2.0, 64), note(1.0, 2.0, 60), note(1.01, 2.0, 72), note(1.0, 2.0, 67)};
    Params p;
    p.tool = Tool::Strum;
    p.strumS = 0.03;
    std::vector<Note> up = sorted(apply(chord, all(chord), p));
    CHECK(up.size() == 4);
    int pitches[] = {60, 64, 67, 72};
    for (int k = 0; k < 4; ++k) {
        CHECK((int)up[(size_t)k].pitch == pitches[k]);
        CHECK_NEAR(up[(size_t)k].start, 1.0 + 0.03 * k, 1e-12);  // (from the chord's first start, evenly)
        CHECK_NEAR(up[(size_t)k].end, 2.0, 1e-12);                // (the ends stay)
    }
    p.down = true;
    std::vector<Note> down = sorted(apply(chord, all(chord), p));
    CHECK((int)down[0].pitch == 72 && (int)down[3].pitch == 60);
    CHECK_NEAR(down[3].start, 1.09, 1e-12);
    // a curve: the same first and last, spacing growing (above 0)
    p.down = false;
    p.curve = 0.8;
    std::vector<Note> bent = sorted(apply(chord, all(chord), p));
    CHECK_NEAR(bent[0].start, 1.0, 1e-12);
    CHECK_NEAR(bent[3].start, 1.09, 1e-12);
    CHECK(bent[1].start - bent[0].start < bent[3].start - bent[2].start);
    // two chords apart: each strummed from its own start
    std::vector<Note> two = {note(0, 1, 60), note(0, 1, 64), note(2, 3, 60), note(2, 3, 64)};
    p.curve = 0;
    std::vector<Note> s2 = sorted(apply(two, all(two), p));
    CHECK_NEAR(s2[2].start, 2.0, 1e-12);
    CHECK_NEAR(s2[3].start, 2.03, 1e-12);
    // at the clip's start, down too: nothing before 0; a note shorter than the strum keeps a length
    std::vector<Note> edge = {note(0, 0.01, 60), note(0, 0.01, 64), note(0, 0.01, 67)};
    p.down = true;
    p.strumS = 0.5;
    Result r = apply(edge, all(edge), p);
    CHECK(sane(edge, r));
    for (const Note& n : r.notes) CHECK(n.start >= 0 && n.end > n.start);
}

static void testArpeggiate() {
    std::vector<Note> chord = {note(0, 1, 64, 90), note(0, 1, 60, 100), note(0, 1, 67, 80)};
    Params p;
    p.tool = Tool::Arpeggiate;
    p.rateS = 0.25;
    p.gate = 0.5;
    std::vector<Note> up = sorted(apply(chord, all(chord), p));
    CHECK(up.size() == 4);
    int order[] = {60, 64, 67, 60};
    for (int k = 0; k < 4 && k < (int)up.size(); ++k) {
        CHECK((int)up[(size_t)k].pitch == order[k]);
        CHECK_NEAR(up[(size_t)k].start, 0.25 * k, 1e-12);
        CHECK_NEAR(up[(size_t)k].end - up[(size_t)k].start, 0.125, 1e-12);
    }
    CHECK(up[0].velocity == 100);  // (each note its own velocity)
    p.style = ArpStyle::Down;
    std::vector<Note> down = sorted(apply(chord, all(chord), p));
    CHECK((int)down[0].pitch == 67 && (int)down[1].pitch == 64 && (int)down[2].pitch == 60 && (int)down[3].pitch == 67);
    p.style = ArpStyle::UpDown;
    std::vector<Note> longer = {note(0, 1.5, 60), note(0, 1.5, 64), note(0, 1.5, 67)};
    std::vector<Note> ud = sorted(apply(longer, all(longer), p));
    int udOrder[] = {60, 64, 67, 64, 60, 64};
    CHECK(ud.size() == 6);
    for (int k = 0; k < 6 && k < (int)ud.size(); ++k) CHECK((int)ud[(size_t)k].pitch == udOrder[k]);
    // random: the same seed, the same notes; nothing outside the chord's span
    p.style = ArpStyle::Random;
    p.seed = 7;
    std::vector<Note> a = sorted(apply(longer, all(longer), p)), b = sorted(apply(longer, all(longer), p));
    CHECK(a.size() == b.size());
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) CHECK(a[k].pitch == b[k].pitch);
    for (const Note& n : a) CHECK(n.start >= 0 && n.end <= 1.5 + 1e-12);
    // a gate of 100 %: notes touch, never overlap
    p.style = ArpStyle::Up;
    p.gate = 1.0;
    std::vector<Note> full = sorted(apply(chord, all(chord), p));
    for (size_t k = 1; k < full.size(); ++k) CHECK(full[k - 1].end <= full[k].start + 1e-12);
    // a very long chord at a tiny rate: capped
    std::vector<Note> huge = {note(0, 10000, 60), note(0, 10000, 64)};
    p.rateS = 0.001;
    CHECK((int)apply(huge, all(huge), p).notes.size() == MAX_ARP_STEPS);
}

static void testChop() {
    std::vector<Note> one = {note(1, 2, 60, 77)};
    Params p;
    p.tool = Tool::Chop;
    p.pieces = 4;
    std::vector<Note> c = sorted(apply(one, all(one), p));
    CHECK(c.size() == 4);
    for (int k = 0; k < 4 && k < (int)c.size(); ++k) {
        CHECK_NEAR(c[(size_t)k].start, 1 + 0.25 * k, 1e-12);
        CHECK_NEAR(c[(size_t)k].end, 1 + 0.25 * (k + 1), 1e-12);
        CHECK(c[(size_t)k].velocity == 77);
    }
    // by a step: pieces that long, the last one what is left
    p.chopStepS = 0.3;
    c = sorted(apply(one, all(one), p));
    CHECK(c.size() == 4);
    CHECK_NEAR(c[3].start, 1.9, 1e-9);
    CHECK_NEAR(c[3].end, 2.0, 1e-12);
    // a step that leaves a sliver: it joins the last piece
    p.chopStepS = 0.4999;
    c = sorted(apply(one, all(one), p));
    CHECK(c.size() == 2 && std::fabs(c.back().end - 2.0) < 1e-12);
    // too many: capped; a note too short to cut stays whole
    p.chopStepS = 0;
    p.pieces = 100000;
    CHECK((int)apply(one, all(one), p).notes.size() == MAX_PIECES);
    std::vector<Note> tiny = {note(0, 0.0015, 60)};
    p.pieces = 4;
    CHECK(apply(tiny, all(tiny), p).notes.size() == 1);
}

static void testFlam() {
    Params p;
    p.tool = Tool::Flam;
    p.flamS = 0.02;
    p.flamVelocity = 0.6;
    std::vector<Note> one = {note(1, 2, 60, 100)};
    std::vector<Note> f = sorted(apply(one, all(one), p));
    CHECK(f.size() == 2);
    CHECK_NEAR(f[0].start, 0.98, 1e-12);
    CHECK_NEAR(f[0].end, 1.0, 1e-12);  // (touching the note, not over it)
    CHECK(f[0].velocity == 60);
    CHECK_NEAR(f[1].start, 1.0, 1e-12);
    // the clip's start: the grace note takes what room there is; none at 0
    std::vector<Note> edge = {note(0.01, 1, 60), note(0, 1, 62)};
    f = sorted(apply(edge, all(edge), p));
    CHECK(f.size() == 3);
    CHECK(f[0].start == 0 && std::fabs(f[0].end - 0.01) < 1e-12 && (int)f[0].pitch == 60);
    for (const Note& n : f) CHECK(n.start >= 0);
    // a note of its pitch just before: the grace note starts after it (ornaments never overlap)
    std::vector<Note> close = {note(0.5, 0.99, 60), note(1.0, 2.0, 60)};
    std::vector<int> second = {1};
    f = sorted(apply(close, second, p));
    CHECK(f.size() == 3);
    CHECK_NEAR(f[1].start, 0.99, 1e-12);
    // a note of its pitch still sounding: no grace note at all
    std::vector<Note> over = {note(0.5, 1.5, 60), note(1.0, 2.0, 60)};
    CHECK(apply(over, second, p).notes.size() == 2);
}

static void testLegatoAndOverlaps() {
    std::vector<Note> ns = {note(0, 0.2, 60), note(0, 0.9, 64), note(0.5, 0.6, 62), note(2.0, 2.5, 65)};
    std::vector<Note> l = sorted(legato(ns, all(ns)));
    CHECK(l.size() == 4);
    CHECK_NEAR(l[0].end, 0.5, 1e-12);  // (a chord's notes together)
    CHECK_NEAR(l[1].end, 0.5, 1e-12);
    CHECK_NEAR(l[2].end, 2.0, 1e-12);
    CHECK_NEAR(l[3].end, 2.5, 1e-12);  // (the last keeps its end)
    // the same pitch only: a note ends where the next starts; one starting with a longer one goes; other pitches stay
    std::vector<Note> ov = {note(0, 1, 60), note(0.5, 1.5, 60), note(0.5, 0.7, 60), note(0.2, 3, 61)};
    std::vector<Note> o = sorted(removeOverlaps(ov, all(ov)));
    CHECK(o.size() == 3);
    CHECK_NEAR(o[0].end, 0.5, 1e-12);
    CHECK((int)o[1].pitch == 61 && std::fabs(o[1].end - 3) < 1e-12);
    CHECK_NEAR(o[2].start, 0.5, 1e-12);
    CHECK_NEAR(o[2].end, 1.5, 1e-12);
    // the tool too
    Params p;
    p.tool = Tool::Legato;
    CHECK(sorted(apply(ns, all(ns), p))[2].end == 2.0);
}

static void testQuantizeHumanizeShape() {
    Params p;
    p.tool = Tool::Quantize;
    p.onGrid = [](double t) { return std::nearbyint(t / 0.5) * 0.5; };
    p.stepAt = [](double) { return 0.5; };
    p.strength = 0.5;
    std::vector<Note> ns = {note(0.1, 0.3, 60), note(0.9, 1.2, 62), note(1.65, 1.8, 64)};
    std::vector<Note> q = sorted(apply(ns, all(ns), p));
    CHECK_NEAR(q[0].start, 0.05, 1e-12);
    CHECK_NEAR(q[0].end, 0.25, 1e-12);
    CHECK_NEAR(q[1].start, 0.95, 1e-12);
    CHECK_NEAR(q[2].start, 1.575, 1e-12);
    p.strength = 1;
    p.ends = true;
    for (const Note& n : apply(ns, all(ns), p).notes) {
        CHECK_NEAR(std::fmod(n.start, 0.5), 0.0, 1e-9);
        CHECK(n.end - n.start >= 0.5 - 1e-9);
    }
    // humanize: within the ranges, lengths kept, the same with the same seed, never before 0
    Params h;
    h.tool = Tool::Humanize;
    h.timingS = 0.05;
    h.velocityRange = 10;
    std::vector<Note> hs = {note(0, 0.5, 60, 1), note(1, 1.5, 62, 127), note(2, 2.25, 64, 64)};
    Result a = apply(hs, all(hs), h), b = apply(hs, all(hs), h);
    CHECK(sane(hs, a));
    for (size_t k = 0; k < a.notes.size(); ++k) {
        const Note &n = a.notes[k], &o = hs[(size_t)a.from[k]];
        CHECK(std::fabs(n.start - o.start) <= 0.05 + 1e-12);
        CHECK_NEAR(n.end - n.start, o.end - o.start, 1e-12);
        CHECK(std::fabs(n.velocity - o.velocity) <= 10);
        CHECK(n.start == b.notes[k].start && n.velocity == b.notes[k].velocity);
    }
    // velocity shape: evenly from 40 to 120 along the starts
    Params v;
    v.tool = Tool::VelocityShape;
    std::vector<Note> vs;
    for (int k = 0; k < 5; ++k) vs.push_back(note(k * 0.5, k * 0.5 + 0.25, 60));
    std::vector<Note> shaped = sorted(apply(vs, all(vs), v));
    for (int k = 0; k < 5; ++k) CHECK(shaped[(size_t)k].velocity == 40 + 20 * k);
    v.curve = 0.5;
    shaped = sorted(apply(vs, all(vs), v));
    CHECK(shaped[0].velocity == 40 && shaped[4].velocity == 120 && shaped[2].velocity > 80);  // (rises early)
}

static void testSplitJoinRow() {
    std::vector<Note> ns = {note(0, 1, 60, 90), note(0.5, 0.6, 62), note(0.4, 2, 64)};
    std::vector<Note> s = sorted(split(ns, all(ns), 0.4));
    CHECK(s.size() == 4);
    CHECK_NEAR(s[0].end, 0.4, 1e-12);
    CHECK(s[1].start == 0.4 && s[1].end == 1 && (int)s[1].pitch == 60 && s[1].velocity == 90);
    CHECK(s[2].start == 0.4 && s[2].end == 2);  // (starting at the cut: not cut)
    // join: touching and overlapping notes of a pitch become one; a gap keeps them apart
    std::vector<Note> js = {note(0, 0.5, 60, 70), note(0.5, 1, 60), note(0.9, 1.5, 60), note(2, 3, 60), note(0, 1, 61)};
    std::vector<Note> j = sorted(join(js, all(js)));
    CHECK(j.size() == 3);
    CHECK(j[0].start == 0 && j[0].end == 1.5 && j[0].velocity == 70);
    CHECK(j[2].start == 2);
    // the row
    CHECK((row(js, 60) == std::vector<int>{0, 1, 2, 3}));
    CHECK(row(js, 59).empty());
    // targets out of range or twice: ignored
    std::vector<int> bad = {-1, 0, 0, 99};
    Result r = split(ns, bad, 0.5);
    CHECK(r.notes.size() == 4 && sane(ns, r));
    CHECK(apply({}, {}, Params()).notes.empty());
}

// every tool on 100,000 notes (random, overlapping, at 0 and later): fast, and the rules kept
static void testHundredThousand() {
    std::mt19937 rng(1234);
    std::vector<Note> ns;
    ns.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        double s = (rng() % 600000) / 1000.0, len = 0.01 + (rng() % 2000) / 1000.0;
        ns.push_back(note(i % 50 == 0 ? 0.0 : s, (i % 50 == 0 ? 0.0 : s) + len, 24 + (int)(rng() % 80),
                          1 + (int)(rng() % 127)));
    }
    std::vector<int> every = all(ns);
    for (int t = 0; t < TOOL_COUNT; ++t) {
        Params p;
        p.tool = (Tool)t;
        p.onGrid = [](double x) { return std::nearbyint(x / 0.125) * 0.125; };
        p.stepAt = [](double) { return 0.125; };
        p.ends = true;
        p.pieces = 3;
        p.style = ArpStyle::Random;
        auto t0 = std::chrono::steady_clock::now();
        Result r = apply(ns, every, p);
        double took = seconds(t0);
        printf("  %s on 100000 notes: %zu notes, %.3f s\n", toolName((Tool)t), r.notes.size(), took);
        CHECK(took < 3.0);
        CHECK(sane(ns, r));
        if ((Tool)t == Tool::Chop) CHECK(r.notes.size() >= ns.size() * 2);
        if ((Tool)t == Tool::Flam) {  // no grace note overlaps a note of its pitch
            std::vector<std::vector<Note>> byPitch(128);
            for (const Note& n : ns) byPitch[(size_t)n.pitch].push_back(n);
            size_t graces = 0, bad = 0;
            for (size_t k = 0; k < r.notes.size(); ++k) {
                const Note& g = r.notes[k];
                const Note& o = ns[(size_t)r.from[k]];
                if (g.start == o.start && g.end == o.end) continue;  // (the note itself)
                graces++;
                for (const Note& n : byPitch[(size_t)g.pitch])
                    if (n.start < g.end - 1e-12 && n.end > g.start + 1e-12) bad++;
            }
            CHECK(graces > 0 && bad == 0);
        }
    }
    auto t0 = std::chrono::steady_clock::now();
    Result s = split(ns, every, 300), j = join(ns, every), o = removeOverlaps(ns, every), l = legato(ns, every);
    double took = seconds(t0);
    printf("  split, join, remove overlaps, legato on 100000 notes: %.3f s\n", took);
    CHECK(took < 4.0);
    CHECK(sane(ns, s) && sane(ns, j) && sane(ns, o) && sane(ns, l));
    CHECK(s.notes.size() > ns.size() && j.notes.size() < ns.size() && o.notes.size() <= ns.size());
    // after removing overlaps no two notes of a pitch overlap
    std::vector<Note> os = o.notes;
    std::sort(os.begin(), os.end(), [](const Note& a, const Note& b) {
        return a.pitch != b.pitch ? a.pitch < b.pitch : a.start < b.start;
    });
    size_t overlaps = 0;
    for (size_t k = 1; k < os.size(); ++k)
        if (os[k].pitch == os[k - 1].pitch && os[k - 1].end > os[k].start + 1e-12) overlaps++;
    CHECK(overlaps == 0);
}

int main() {
    testStrum();
    testArpeggiate();
    testChop();
    testFlam();
    testLegatoAndOverlaps();
    testQuantizeHumanizeShape();
    testSplitJoinRow();
    testHundredThousand();
    TEST_MAIN_END();
}
