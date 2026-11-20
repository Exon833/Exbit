#include "audio/midi_effects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace midi_effects {

// --- numpy's Generator: SeedSequence (numpy/random/bit_generator.pyx) and PCG64 (pcg64.h) ---

namespace {
constexpr uint32_t INIT_A = 0x43b0d7e5, MULT_A = 0x931e8875, INIT_B = 0x8b51f9dd, MULT_B = 0x58f38ded;
constexpr uint32_t MIX_MULT_L = 0xca01f9dd, MIX_MULT_R = 0x4973f715;
constexpr int XSHIFT = 16, POOL_SIZE = 4;

uint32_t hashmix(uint32_t value, uint32_t& hashConst) {
    value ^= hashConst;
    hashConst *= MULT_A;
    value *= hashConst;
    value ^= value >> XSHIFT;
    return value;
}

uint32_t mix(uint32_t x, uint32_t y) {
    uint32_t result = MIX_MULT_L * x - MIX_MULT_R * y;
    result ^= result >> XSHIFT;
    return result;
}

const unsigned __int128 PCG_MULT = ((unsigned __int128)2549297995355413924ULL << 64) | 4865540595714422341ULL;
}  // namespace

NumpyRandom::NumpyRandom(uint64_t seed) {
    // the entropy as 32-bit words, least significant first (0: one word, 0)
    std::vector<uint32_t> entropy;
    do {
        entropy.push_back((uint32_t)(seed & 0xffffffffu));
        seed >>= 32;
    } while (seed);
    uint32_t pool[POOL_SIZE];
    uint32_t hashConst = INIT_A;
    for (int i = 0; i < POOL_SIZE; i++) pool[i] = hashmix(i < (int)entropy.size() ? entropy[i] : 0, hashConst);
    for (int src = 0; src < POOL_SIZE; src++)
        for (int dst = 0; dst < POOL_SIZE; dst++)
            if (src != dst) pool[dst] = mix(pool[dst], hashmix(pool[src], hashConst));
    for (size_t src = POOL_SIZE; src < entropy.size(); src++)
        for (int dst = 0; dst < POOL_SIZE; dst++) pool[dst] = mix(pool[dst], hashmix(entropy[src], hashConst));
    // generate_state(4, uint64)
    uint32_t words[8];
    uint32_t h = INIT_B;
    for (int i = 0; i < 8; i++) {
        uint32_t v = pool[i % POOL_SIZE];
        v ^= h;
        h *= MULT_B;
        v *= h;
        v ^= v >> XSHIFT;
        words[i] = v;
    }
    uint64_t val[4];
    for (int k = 0; k < 4; k++) val[k] = (uint64_t)words[2 * k] | ((uint64_t)words[2 * k + 1] << 32);
    unsigned __int128 initState = ((unsigned __int128)val[0] << 64) | val[1];
    unsigned __int128 initSeq = ((unsigned __int128)val[2] << 64) | val[3];
    // pcg_setseq_128_srandom_r
    state_ = 0;
    inc_ = (initSeq << 1) | 1;
    state_ = state_ * PCG_MULT + inc_;
    state_ += initState;
    state_ = state_ * PCG_MULT + inc_;
}

uint64_t NumpyRandom::next64() {
    state_ = state_ * PCG_MULT + inc_;
    uint64_t hi = (uint64_t)(state_ >> 64), lo = (uint64_t)state_;
    uint64_t x = hi ^ lo;
    unsigned rot = (unsigned)(state_ >> 122);
    return (x >> rot) | (x << ((64 - rot) & 63));
}

uint32_t NumpyRandom::next32() {
    if (hasHalf_) {
        hasHalf_ = false;
        return half_;
    }
    uint64_t next = next64();
    hasHalf_ = true;
    half_ = (uint32_t)(next >> 32);
    return (uint32_t)(next & 0xffffffffu);
}

double NumpyRandom::random() { return (double)(next64() >> 11) * (1.0 / 9007199254740992.0); }

int64_t NumpyRandom::integers(int64_t low, int64_t high) {
    uint64_t rng = (uint64_t)(high - 1 - low);  // (inclusive)
    if (high - 1 <= low) return low;            // (rng 0: nothing is drawn)
    if (rng <= 0xffffffffu) {
        if (rng == 0xffffffffu) return low + (int64_t)next32();
        // buffered_bounded_lemire_uint32
        uint32_t excl = (uint32_t)rng + 1;
        uint64_t m = (uint64_t)next32() * excl;
        uint32_t leftover = (uint32_t)(m & 0xffffffffu);
        if (leftover < excl) {
            uint32_t threshold = (uint32_t)((0xffffffffu - (uint32_t)rng) % excl);
            while (leftover < threshold) {
                m = (uint64_t)next32() * excl;
                leftover = (uint32_t)(m & 0xffffffffu);
            }
        }
        return low + (int64_t)(m >> 32);
    }
    // bounded_lemire_uint64 (never needed by the effects: their ranges are small)
    uint64_t excl = rng + 1;
    unsigned __int128 m = (unsigned __int128)next64() * excl;
    uint64_t leftover = (uint64_t)m;
    if (leftover < excl) {
        uint64_t threshold = (0xffffffffffffffffULL - rng) % excl;
        while (leftover < threshold) {
            m = (unsigned __int128)next64() * excl;
            leftover = (uint64_t)m;
        }
    }
    return low + (int64_t)(m >> 64);
}

// --- tables ---

const std::vector<std::pair<std::string, std::vector<int>>> CHORDS = {
    {"Octave", {12}},           {"Fifth (power chord)", {7, 12}}, {"Major", {4, 7}},       {"Minor", {3, 7}},
    {"Major 7th", {4, 7, 11}},  {"Minor 7th", {3, 7, 10}},       {"Dominant 7th", {4, 7, 10}},
    {"Suspended 4th", {5, 7}},  {"Octave down", {-12}},
    // (new ones go last: projects keep the Chord effect's choice by its place in this list)
    {"Suspended 2nd", {2, 7}},  {"Diminished", {3, 6}},          {"Augmented", {4, 8}},
};

const std::vector<std::pair<std::string, std::vector<int>>> SCALES = {
    {"Major", {0, 2, 4, 5, 7, 9, 11}},      {"Minor", {0, 2, 3, 5, 7, 8, 10}},
    {"Dorian", {0, 2, 3, 5, 7, 9, 10}},     {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
    {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}}, {"Major pentatonic", {0, 2, 4, 7, 9}},
    {"Minor pentatonic", {0, 3, 5, 7, 10}}, {"Blues", {0, 3, 5, 6, 7, 10}},
};

const char* const KEYS[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const std::vector<std::string> RATES = {"1/4", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32"};
const std::vector<double> RATE_BEATS = {1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};
const std::vector<std::string> STYLES = {"Up", "Down", "Up and down", "As played", "Random"};
const std::vector<std::string> GROOVE_GRIDS = {"1/8", "1/16", "1/32"};
const std::vector<double> GROOVE_BEATS = {0.5, 0.25, 0.125};

int ParamInfo::steps() const {
    if (!choices.empty()) return std::max(1, (int)choices.size() - 1);
    return scale == "int" ? (int)(high - low) : 0;
}

namespace {
ParamInfo param(const char* key, const char* name, double low, double high, double def, const char* unit = "",
                const char* scale = "linear", int decimals = -1) {
    ParamInfo p;
    p.key = key, p.name = name, p.low = low, p.high = high, p.defaultValue = def, p.unit = unit, p.scale = scale;
    p.decimals = decimals;
    return p;
}
ParamInfo choice(const char* key, const char* name, std::vector<std::string> choices, double def = 0) {
    ParamInfo p;
    p.key = key, p.name = name, p.defaultValue = def, p.choices = std::move(choices);
    return p;
}
std::vector<std::string> names(const std::vector<std::pair<std::string, std::vector<int>>>& table) {
    std::vector<std::string> out;
    for (auto& e : table) out.push_back(e.first);
    return out;
}

const std::map<std::string, std::vector<ParamInfo>>& table() {
    static const std::map<std::string, std::vector<ParamInfo>> t = {
        {"Arpeggiator",
         {choice("style", "Style", STYLES), choice("rate", "Rate", RATES, 3.0), param("gate", "Gate", 10, 200, 80, "%"),
          param("octaves", "Octaves", 1, 4, 1, "", "int")}},
        {"Chord", {choice("chord", "Chord", names(CHORDS)), param("velocity", "Added notes' velocity", 10, 100, 85, "%")}},
        {"Scale", {choice("root", "Key", std::vector<std::string>(KEYS, KEYS + 12)), choice("scale", "Scale", names(SCALES))}},
        {"Note Length",
         {choice("mode", "Mode", {"A length in beats", "A share of each note's own"}),
          param("beats", "Length", 0.0625, 8, 0.25, "beats", "log", 3), param("share", "Share", 10, 400, 100, "%")}},
        {"Velocity",
         {param("scale", "Amount", 0, 200, 100, "%"), param("offset", "Add", -127, 127, 0, "", "int"),
          param("random", "Random ±", 0, 64, 0, "", "int"), param("lowest", "Lowest", 1, 127, 1, "", "int"),
          param("highest", "Highest", 1, 127, 127, "", "int")}},
        {"Random",
         {param("chance", "Chance", 0, 100, 50, "%"), param("range", "Range", 1, 24, 12, "semitones", "int"),
          param("seed", "Seed", 1, 100, 1, "", "int")}},
        {"Pitch", {param("semitones", "Transpose", -48, 48, 0, "semitones", "int")}},
        {"Groove",
         {param("swing", "Swing", 50, 75, 60, "%", "linear", 1), choice("grid", "Grid", GROOVE_GRIDS, 1.0),
          param("accent", "Accent of the swung notes", -50, 50, 0, "%", "linear", 0),
          param("timing", "Humanize timing ±", 0, 30, 0, "ms", "linear", 1),
          param("velocity", "Humanize velocity ±", 0, 30, 0, "", "int"), param("seed", "Seed", 1, 100, 1, "", "int")}},
    };
    return t;
}

int pyRound(double x) { return (int)std::nearbyint(x); }  // Python's round(): halves to even

const Timing* timing_ = nullptr;

// seconds per beat where `seconds` is (beat_s): the tempo map's, or 120 BPM
double beatS(double seconds) { return timing_ ? timing_->beatSAt(seconds) : 0.5; }

bool inSteps(double c, const std::set<int>& steps) {
    double m = std::fmod(c, 12.0);
    if (m < 0) m += 12.0;
    return m == std::floor(m) && steps.count((int)m) > 0;
}
}  // namespace

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> k = {"Arpeggiator", "Chord", "Scale", "Note Length", "Velocity", "Random",
                                               "Pitch", "Groove"};
    return k;
}

const std::vector<ParamInfo>& params(const std::string& kind) {
    static const std::vector<ParamInfo> none;
    auto it = table().find(kind);
    return it == table().end() ? none : it->second;
}

bool isLive(const std::string& kind) { return kind != "Arpeggiator" && kind != "Note Length" && kind != "Groove"; }

std::string describe(const std::string& kind) {
    static const std::map<std::string, std::string> d = {
        {"Arpeggiator", "Plays the notes held one after another, on a beat grid (clips only)"},
        {"Chord", "Adds notes above (or below) each note: a chord"},
        {"Scale", "Moves every note to the nearest note of a scale"},
        {"Note Length", "Makes every note a set length, or a share of its own (clips only)"},
        {"Velocity", "Scales, offsets, randomises and limits how hard the notes are played"},
        {"Random", "Moves some notes up or down at random (the same each time a clip plays)"},
        {"Pitch", "Transposes every note"},
        {"Groove", "Swing: notes on the off-beats of a 1/8, 1/16 or 1/32 grid come later (those between move in "
                   "proportion), with an accent; and a little humanizing of timing and velocity (clips only)"},
    };
    auto it = d.find(kind);
    return it == d.end() ? std::string() : it->second;
}

std::set<int> scaleSteps(int root, const std::string& scale) {
    std::set<int> out;
    for (auto& s : SCALES)
        if (s.first == scale)
            for (int d : s.second) out.insert(((root + d) % 12 + 12) % 12);
    return out;
}

double nearestInScale(double pitch, const std::set<int>& steps) {
    bool found = false;
    double best = pitch;
    auto consider = [&](double c) {
        if (!inSteps(c, steps) || c < 0 || c > 127) return;
        if (!found || std::fabs(c - pitch) < std::fabs(best - pitch) ||
            (std::fabs(c - pitch) == std::fabs(best - pitch) && c < best))
            best = c, found = true;
    };
    for (int k = 0; k < 7; k++) consider(pitch - k);
    for (int k = 1; k < 7; k++) consider(pitch + k);
    return std::min(127.0, std::max(0.0, best));
}

std::vector<int> inScaleChord(int root, const std::set<int>& steps, int notes) {
    int r = (int)nearestInScale(root, steps);
    std::vector<int> above;
    for (int p = r; p < std::min(128, r + 25); p++)
        if (steps.count(p % 12)) above.push_back(p);
    std::vector<int> out;
    for (int k = 0; k < notes; k++)
        if (2 * k < (int)above.size()) out.push_back(above[2 * k]);
    return out;
}

void install(const Timing* t) {
    timing_ = t;
    clip_hooks::midiEffects = [](const Track& track, const std::vector<Note>& notes, double origin) {
        return apply(track, notes, origin);
    };
}

const Timing* timing() { return timing_; }

TimingScope::TimingScope(const Timing* t) : before_(timing_) { timing_ = t; }
TimingScope::~TimingScope() { timing_ = before_; }

// --- the effects ---

namespace {
double velocityShape(const MidiEffect& e, double velocity, int64_t k) {
    double v = velocity * e.get("scale") / 100 + e.get("offset");
    if (e.get("random") != 0) v += (NumpyRandom((uint64_t)k).random() * 2 - 1) * e.get("random");
    double r = std::min(127.0, std::max(1.0, (double)pyRound(v)));
    return std::min(e.get("highest"), std::max(e.get("lowest"), r));
}

double randomMoved(const MidiEffect& e, double pitch, int64_t key) {
    NumpyRandom rng((uint64_t)(key + 7919 * (int64_t)e.get("seed")));
    if (rng.random() * 100 >= e.get("chance")) return pitch;
    int64_t range = (int64_t)e.get("range");
    return std::min(127.0, std::max(0.0, pitch + (double)rng.integers(-range, range + 1)));
}

int64_t noteKey(const Note& n) { return (int64_t)std::nearbyint(n.start * 1000) * 131 + (int64_t)n.pitch; }

std::vector<Note> arpeggiate(const MidiEffect& e, const std::vector<Note>& notes, double origin) {
    std::vector<Note> out;
    if (notes.empty()) return out;
    double stepBeats = RATE_BEATS[std::min<size_t>(RATE_BEATS.size() - 1, (size_t)std::max(0.0, e.get("rate")))];
    const std::string& style = STYLES[std::min<size_t>(STYLES.size() - 1, (size_t)std::max(0.0, e.get("style")))];
    int octaves = (int)e.get("octaves");
    NumpyRandom rng(1);
    double first = notes[0].start, end = notes[0].end;
    for (auto& n : notes) first = std::min(first, n.start), end = std::max(end, n.end);
    first += origin;
    double t;
    if (timing_)
        t = timing_->secondsAt(std::ceil(timing_->beatAt(first) / stepBeats - 1e-9) * stepBeats) - origin;  // (on the grid)
    else
        t = std::ceil(first / (stepBeats * beatS(0)) - 1e-9) * stepBeats * beatS(0) - origin;
    int64_t index = 0;
    std::vector<Note> held;
    std::vector<std::pair<double, double>> pool;
    while (t < end) {
        double step = stepBeats * beatS(origin + t);
        held.clear();
        for (auto& n : notes)
            if (n.start <= t + 1e-9 && n.end > t + 1e-9) held.push_back(n);
        if (!held.empty()) {
            if (style == "As played")
                std::stable_sort(held.begin(), held.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
            else
                std::stable_sort(held.begin(), held.end(), [](const Note& a, const Note& b) { return a.pitch < b.pitch; });
            pool.clear();
            for (int o = 0; o < octaves; o++)
                for (auto& n : held) pool.emplace_back(n.pitch + 12 * o, n.velocity);
            if (style == "Down") {
                std::reverse(pool.begin(), pool.end());
            } else if (style == "Up and down" && pool.size() > 2) {
                size_t n = pool.size();
                for (size_t k = n - 2; k >= 1; k--) pool.push_back(pool[k]);  // pool[-2:0:-1]
            }
            std::pair<double, double> pick = style == "Random" ? pool[(size_t)rng.integers(0, (int64_t)pool.size())]
                                                               : pool[(size_t)(index % (int64_t)pool.size())];
            if (pick.first <= 127) out.push_back({t, t + step * e.get("gate") / 100, pick.first, pick.second});
            index++;
        } else {
            index = 0;
        }
        t += step;
    }
    return out;
}

// Groove: in each pair of grid steps (on the song's beats), the time from its first step to its second
// (the off-beat) is stretched to `swing` of the pair and the rest squeezed, so an off-beat note comes
// later by (swing - 50 %) of the pair and a note between the grid lines moves in proportion; notes on the
// beats stay. Each note keeps its length. The accent changes the velocity of the notes the more they sit
// on an off-beat; humanizing moves each note and its velocity by a random amount seeded by the note (and
// the seed): the same every time the clip is prepared, so renders play what playback plays.
std::string rowKey(int pitch) { return "row:" + std::to_string(pitch); }

// a note at `start` (clip time) swung: the off-beat of each pair of `grid`-beat steps at `swing` of the pair
double swungStart(double start, double origin, double grid, double swing) {
    double at = origin + start;
    double beat = timing_ ? timing_->beatAt(at) : at / beatS(0);
    double pair = 2 * grid;
    double base = std::floor(beat / pair) * pair;
    double phase = std::min(pair, std::max(0.0, beat - base));  // 0 … 2 steps
    double moved = phase <= grid ? phase * 2 * swing : pair * swing + (phase - grid) * (2 - 2 * swing);
    return (timing_ ? timing_->secondsAt(base + moved) : (base + moved) * beatS(0)) - origin;
}

std::vector<Note> groove(const MidiEffect& e, const std::vector<Note>& notes, double origin) {
    double grid = GROOVE_BEATS[std::min<size_t>(GROOVE_BEATS.size() - 1, (size_t)std::max(0.0, e.get("grid")))];
    double swing = std::min(75.0, std::max(50.0, e.get("swing"))) / 100;
    double accent = std::min(50.0, std::max(-50.0, e.get("accent"))) / 100;
    double timing = std::max(0.0, e.get("timing")) / 1000, velocity = std::max(0.0, e.get("velocity"));
    int64_t seed = (int64_t)e.get("seed");
    std::vector<Note> out = notes;
    for (auto& n : out) {
        int64_t key = noteKey(n);  // (as it was: its random amounts don't depend on the swing)
        double at = origin + n.start;
        double beat = timing_ ? timing_->beatAt(at) : at / beatS(0);
        double pair = 2 * grid;
        double base = std::floor(beat / pair) * pair;
        double phase = std::min(pair, std::max(0.0, beat - base));  // 0 … 2 steps
        double start = n.start;
        if (swing != 0.5 && !e.values.count(rowKey((int)n.pitch)))  // (a row of its own: swung by apply())
            start = swungStart(n.start, origin, grid, swing);
        double v = n.velocity;
        bool changed = false;
        if (accent != 0) {
            v *= 1 + accent * std::max(0.0, 1 - std::fabs(phase - grid) / grid);  // (1 on the off-beat, 0 on the beats)
            changed = true;
        }
        if (timing > 0 || velocity > 0) {
            NumpyRandom rng((uint64_t)(key + 104729 * seed));
            double early = rng.random() * 2 - 1, louder = rng.random() * 2 - 1;  // (both drawn: each its own)
            start += early * timing;
            if (velocity > 0) {
                v += louder * velocity;
                changed = true;
            }
        }
        start = std::max(start, std::min(n.start, 0.0));  // (never before the clip's start)
        n.end += start - n.start;
        n.start = start;
        if (changed) n.velocity = std::min(127.0, std::max(1.0, (double)pyRound(v)));
    }
    return out;
}
}  // namespace

std::vector<std::pair<double, double>> liveOne(const MidiEffect& e, double pitch, double velocity) {
    if (e.kind == "Pitch") {
        double p = pitch + e.get("semitones");
        if (p >= 0 && p <= 127) return {{p, velocity}};
        return {};
    }
    if (e.kind == "Chord") {
        size_t i = (size_t)std::max(0.0, e.get("chord"));
        const std::vector<int>& added = CHORDS[std::min(i, CHORDS.size() - 1)].second;
        std::vector<std::pair<double, double>> all{{pitch, velocity}};
        for (int a : added) all.emplace_back(pitch + a, (double)std::max(1, pyRound(velocity * e.get("velocity") / 100)));
        std::vector<std::pair<double, double>> out;
        for (auto& pv : all)
            if (pv.first >= 0 && pv.first <= 127) out.push_back(pv);
        return out;
    }
    if (e.kind == "Scale") {
        size_t s = std::min(SCALES.size() - 1, (size_t)std::max(0.0, e.get("scale")));
        return {{nearestInScale(pitch, scaleSteps((int)e.get("root"), SCALES[s].first)), velocity}};
    }
    if (e.kind == "Velocity") return {{pitch, velocityShape(e, velocity, (int64_t)(pitch * 997 + velocity))}};
    if (e.kind == "Random") {
        static NumpyRandom fresh((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
        return {{randomMoved(e, pitch, fresh.integers(0, (int64_t)1 << 30)), velocity}};
    }
    return {{pitch, velocity}};
}

std::vector<Note> process(const MidiEffect& e, const std::vector<Note>& notes, double origin) {
    if (e.kind == "Arpeggiator") return arpeggiate(e, notes, origin);
    if (e.kind == "Groove") return groove(e, notes, origin);
    if (e.kind == "Note Length") {
        std::vector<Note> out = notes;
        for (auto& n : out)
            n.end = e.get("mode") < 0.5 ? n.start + e.get("beats") * beatS(origin + n.start)
                                        : n.start + (n.end - n.start) * e.get("share") / 100;
        return out;
    }
    if (e.kind == "Velocity") {  // (the same "random" each time the clip is prepared: seeded by the note)
        std::vector<Note> out = notes;
        for (auto& n : out) n.velocity = velocityShape(e, n.velocity, noteKey(n));
        return out;
    }
    if (e.kind == "Random") {
        std::vector<Note> out = notes;
        for (auto& n : out) n.pitch = randomMoved(e, (double)(int64_t)n.pitch, noteKey(n));
        return out;
    }
    std::vector<Note> out;  // (each note becomes what liveOne() makes of it)
    for (auto& n : notes)
        for (auto& pv : liveOne(e, n.pitch, n.velocity)) {
            Note m = n;  // (keeping the note's own properties: its pan, fine tuning, slide)
            m.pitch = pv.first;
            m.velocity = pv.second;
            out.push_back(m);
        }
    return out;
}

double songSwing() { return timing_ ? std::min(100.0, std::max(0.0, timing_->swing)) : 0.0; }
int swingGrid() { return timing_ && timing_->swingGrid == 0 ? 0 : 1; }

double rowSwing(const Track& t, int pitch) {
    auto effects = t.midiEffects.get();
    std::string key = rowKey(pitch);
    for (auto& e : *effects) {
        if (e.kind != "Groove") continue;
        auto it = e.values.find(key);
        if (it != e.values.end()) return std::min(100.0, std::max(0.0, it->second));
    }
    return -1;
}

double swingOf(const Track& t, int pitch) {
    double row = rowSwing(t, pitch);
    if (row >= 0) return row;
    return t.swing >= 0 ? std::min(100.0, t.swing) : songSwing();
}

bool swings(const Track& t) {
    if ((t.swing >= 0 ? t.swing : songSwing()) > 0) return true;
    auto effects = t.midiEffects.get();
    for (auto& e : *effects)
        if (e.kind == "Groove")
            for (auto& v : e.values)
                if (v.first.compare(0, 4, "row:") == 0 && v.second > 0) return true;
    return false;
}

std::vector<Note> swung(const Track& t, std::vector<Note> notes, double origin) {
    if (!swings(t)) return notes;
    double amounts[128];  // (each pitch's, looked up once)
    for (int p = 0; p < 128; p++) amounts[p] = swingOf(t, p);
    double grid = GROOVE_BEATS[(size_t)swingGrid()];
    for (auto& n : notes) {
        double amount = amounts[std::min(127, std::max(0, (int)n.pitch))];
        if (amount <= 0) continue;
        double start = swungStart(n.start, origin, grid, 0.5 + amount / 400);
        start = std::max(start, std::min(n.start, 0.0));  // (never before the clip's start)
        n.end += start - n.start;
        n.start = start;
    }
    return notes;
}

MidiEffect withRowSwing(const MidiEffect& groove, int pitch, double amount) {
    MidiEffect e = groove;
    if (amount < 0) e.values.erase(rowKey(pitch));
    else e.values[rowKey(pitch)] = std::min(100.0, amount);
    return e;
}

std::vector<Note> apply(const Track& track, std::vector<Note> notes, double origin) {
    auto effects = track.midiEffects.get();
    bool swinging = swings(track);
    if (effects->empty() && !swinging) return notes;  // (as they are: not even sorted)
    if (swinging) notes = swung(track, std::move(notes), origin);
    for (auto& e : *effects)
        if (!notes.empty()) notes = process(e, notes, origin);
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.pitch < b.pitch;
    });
    return notes;
}

std::vector<std::pair<int, int>> live(const Track& track, int pitch, int velocity) {
    std::vector<std::pair<double, double>> pairs{{(double)pitch, (double)velocity}}, next;
    auto effects = track.midiEffects.get();
    for (auto& e : *effects) {
        if (!isLive(e.kind)) continue;
        next.clear();
        for (auto& pv : pairs)
            for (auto& q : liveOne(e, pv.first, pv.second)) next.push_back(q);
        pairs.swap(next);
    }
    std::vector<std::pair<int, int>> out;
    for (auto& pv : pairs) out.emplace_back((int)pv.first, (int)pv.second);
    return out;
}

}  // namespace midi_effects
