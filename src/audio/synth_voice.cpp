// The built-in synth's sound (synth_patch.h SOUND): voices rendered ahead of time.
//
// A VOICE is one note, or a run of notes played by one voice: a sliding note taking over the note before
// (note_props::slides, poly), or in mono every note overlapping the one before (legato: no new attack). Its
// envelopes start with its first note and let go after its last; its pitch, level and pan glide from note to
// note. In mono a voice is cut off (in CUT_FADE frames) where the next one begins.
//
// Per voice: unison copies of both oscillators (each with its own start phase, detune and place in the
// stereo field), noise, a state-variable filter per channel, Env 1 on the level. Modulation (Env 1, Env 2,
// the LFO onto the knobs; drift; glide) is worked out every CONTROL frames; the oscillators, filter and
// Env 1 every frame.
#include <algorithm>
#include <cmath>
#include <numeric>

#include "audio/synth_patch.h"
#include "audio/workers.h"
#include "core/util.h"

namespace subsynth {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int CUT_FADE = 64;          // frames a mono voice fades out over where the next begins
constexpr double ENV_SHAPE = 5.0;     // decay and release: exponential, e^-5 (under 1 %) at their time
constexpr double FILTER_ENV_OCT = 6;  // the filter's Env 2 amount at 100 %: octaves
constexpr double DRIFT_CENTS = 18;    // drift at 100 %: the slow pitch wander's reach…
constexpr double DRIFT_OCT = 0.4;     // …the cutoff's…
constexpr double DRIFT_STATIC = 6;    // …and each unison voice's own fixed detune (cents)

uint64_t mix64(uint64_t x) {  // (splitmix64's finalizer)
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
double unit(uint64_t& state) {  // [0, 1), and the state stepped on
    state = mix64(state + 0x9e3779b97f4a7c15ULL);
    return (double)(state >> 11) * (1.0 / 9007199254740992.0);
}

// PolyBLEP: the band-limited step's residual around a wrap at t = 0 (t: the phase 0…1, dt: per frame)
inline double blep(double t, double dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1;
    }
    if (t > 1 - dt) {
        t = (t - 1) / dt;
        return t * t + t + t + 1;
    }
    return 0;
}
// PolyBLAMP: the band-limited ramp's residual (the integral of blep, in frames) around a corner at t = 0
inline double blamp(double t, double dt) {
    if (t < dt) {
        double x = 1 - t / dt;
        return x * x * x / 3;
    }
    if (t > 1 - dt) {
        double x = 1 + (t - 1) / dt;
        return x * x * x / 3;
    }
    return 0;
}
inline double wrap(double p) { return p - std::floor(p); }

// n frames of a wave into out (added at `gain`), its phase stepped on
void oscillate(int wave, double& phase, double dt, double width, float gainL, float gainR, float* l, float* r, int n) {
    double p = phase;
    for (int i = 0; i < n; i++) {
        double v;
        switch (wave) {
        case SINE: v = std::sin(2 * PI * p); break;
        case TRIANGLE: {
            v = p < 0.5 ? 4 * p - 1 : 3 - 4 * p;
            v += 4 * dt * (blamp(p, dt) - blamp(wrap(p + 0.5), dt));  // (its corners at 0 and ½)
            break;
        }
        case SAW: v = 2 * p - 1 - blep(p, dt); break;
        default: {  // pulse: up at 0, down at `width`; its DC taken away
            v = (p < width ? 1.0 : -1.0) + blep(p, dt) - blep(wrap(p - width + 1), dt) - (2 * width - 1);
            break;
        }
        }
        l[i] += (float)(v * gainL);
        r[i] += (float)(v * gainR);
        p += dt;
        if (p >= 1) p -= 1;
    }
    phase = p;
}

// Simper's trapezoidal state-variable filter (zero-delay feedback): stable for any g > 0, k > 0
struct Svf {
    double ic1 = 0, ic2 = 0;
    inline double run(double x, double a1, double a2, double a3, double k, int type) {
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return type == LOWPASS ? v2 : type == HIGHPASS ? x - k * v1 - v2 : k * v1;
    }
};

// an ADSR envelope, frame by frame: a linear attack, exponential decay and release (exactly 0 at its end)
struct Env {
    double a, d, s, r;  // frames, frames, level, frames
    int stage = 0;      // 0 attack, 1 decay / sustain, 2 release, 3 done
    double level = 0, x = 1, from = 0, dc = 0, rc = 0;
    int64_t t = 0;
    void start(double attackS, double decayS, double sustain, double releaseS, int sr) {
        a = std::max(1.0, attackS * sr), d = std::max(1.0, decayS * sr), s = sustain, r = std::max(1.0, releaseS * sr);
        dc = std::exp(-ENV_SHAPE / d), rc = std::exp(-ENV_SHAPE / r);
    }
    void letGo() {
        if (stage >= 2) return;
        stage = 2, from = level, x = 1, t = 0;
    }
    inline double next() {
        switch (stage) {
        case 0:
            level += 1.0 / a;
            if (level >= 1) level = 1, stage = 1;
            break;
        case 1: level = s + (level - s) * dc; break;
        case 2: {
            const double q = std::exp(-ENV_SHAPE);
            x *= rc;
            level = from * std::max(0.0, (x - q) / (1 - q));
            if (++t >= (int64_t)r) level = 0, stage = 3;
            break;
        }
        default: level = 0;
        }
        return level;
    }
};

double lfoWave(int shape, double p, uint64_t seed, int64_t cycle) {
    switch (shape) {
    case 0: return std::sin(2 * PI * p);
    case 1: return p < 0.5 ? 4 * p - 1 : 3 - 4 * p;
    case 2: return 2 * p - 1;
    case 3: return p < 0.5 ? 1 : -1;
    default: {
        uint64_t s = seed ^ ((uint64_t)cycle * 0x632be59bd9b4e019ULL);
        return unit(s) * 2 - 1;
    }
    }
}

struct Part {
    int64_t at;        // frames into the voice
    double pitch, gain, pan;
    double glideS;     // how long it glides from the pitch before (0: at once)
};
struct Voice {
    std::vector<Part> parts;
    int64_t start = 0;    // its first frame in the output
    int64_t gate = 1;     // frames held (then it lets go)
    int64_t cut = -1;     // a mono voice: frames until the next voice (-1: none)
    double fromPitch = 0; // the pitch its first part glides from
    double startS = 0;    // where it starts (s): a free-running LFO's phase
    uint64_t seed = 0;
};

double heardPitch(const Note& n) { return n.pitch + n.fine / 100; }

// the voice as stereo frames into l / r (resized)
void renderVoice(const Patch& P, const Voice& voice, int sr, double beatS, std::vector<float>& outL,
                 std::vector<float>& outR) {
    int64_t n = voice.gate + (int64_t)(tailS(P) * sr) + 1;
    if (voice.cut >= 0) n = std::min(n, voice.cut + CUT_FADE);
    n = std::max<int64_t>(n, 1);
    outL.assign((size_t)n, 0.0f);
    outR.assign((size_t)n, 0.0f);
    const int unison = std::clamp((int)P[UNISON], 1, MAX_UNISON);  // (a patch made in code may hold anything)
    const int ftype = std::clamp((int)P[F_TYPE], 0, 2);
    const int waves[2] = {(int)P[O1_WAVE], (int)P[O2_WAVE]};
    const int lfoShape = (int)P[LFO_SHAPE];
    const int sync = std::clamp((int)P[LFO_SYNC], 0, 11);
    const bool retrig = P[LFO_RETRIG] >= 0.5;
    uint64_t rng = mix64(voice.seed);

    // what modulates what
    double base[PARAMS];
    for (int p = 0; p < PARAMS; p++) base[p] = toNorm(p, P[p]);
    struct Route { int source, target; double depth; };
    Route routes[64];
    int nRoutes = 0;
    bool modded[PARAMS] = {};
    for (const Mod& m : P.mods)
        if (nRoutes < 64 && m.target >= 0 && m.target < PARAMS && m.source >= 0 && m.source < SOURCES) {
            routes[nRoutes++] = {m.source, m.target, m.depth};
            modded[m.target] = true;
        }
    double val[PARAMS];
    for (int p = 0; p < PARAMS; p++) val[p] = P[p];
    // (an oscillator silent with nothing raising its level isn't worked out)
    bool oscOn[2] = {P[O1_LEVEL] > 0 || modded[O1_LEVEL], P[O2_LEVEL] > 0 || modded[O2_LEVEL]};
    bool noiseOn = P[NOISE] > 0 || modded[NOISE];

    // the unison voices: start phases, places across the stereo field, fixed drift detunes
    double phase[2][MAX_UNISON], uPan[MAX_UNISON], uDrift[MAX_UNISON];
    float uL[MAX_UNISON], uR[MAX_UNISON];
    for (int u = 0; u < unison; u++) {
        for (int o = 0; o < 2; o++) phase[o][u] = unison > 1 ? unit(rng) : 0.0;
        uPan[u] = unison > 1 ? 0.8 * (2.0 * u / (unison - 1) - 1) : 0.0;
        uDrift[u] = (unit(rng) * 2 - 1) * DRIFT_STATIC * P[DRIFT];
        double l, r;
        note_props::panGains(uPan[u], l, r);
        double norm = 1 / std::sqrt((double)unison);
        uL[u] = (float)(l * norm), uR[u] = (float)(r * norm);
    }
    bool stereo = unison > 1;
    double driftRate[4], driftPhase[4];
    for (int k = 0; k < 4; k++) driftRate[k] = 0.08 + 0.35 * unit(rng), driftPhase[k] = unit(rng);
    uint32_t noise = (uint32_t)(rng >> 32) | 1u;
    uint64_t lfoSeed = mix64(rng);

    Env e1, e2;
    e1.start(P[E1_A], P[E1_D], P[E1_S], P[E1_R], sr);
    e2.start(P[E2_A], P[E2_D], P[E2_S], P[E2_R], sr);
    double lfoRate = sync > 0 ? 1.0 / (SYNC_BEATS[sync] * std::max(1e-3, beatS)) : P[LFO_RATE];
    double lfoPhase = retrig ? 0.0 : wrap(voice.startS * lfoRate);
    int64_t lfoCycle = retrig ? 0 : (int64_t)std::floor(voice.startS * lfoRate);
    double src[SOURCES] = {0, 0, 0};

    Svf fl, fr;
    size_t part = 0;
    const Part* p0 = &voice.parts[0];
    double pitchFrom = voice.fromPitch, gainFrom = p0->gain, panFrom = p0->pan;
    double pitch = voice.fromPitch, gain = p0->gain, pan = p0->pan;
    float bl[CONTROL], br[CONTROL];
    const double outBase = OUT_GAIN;

    for (int64_t i0 = 0; i0 < n; i0 += CONTROL) {
        int m = (int)std::min<int64_t>(CONTROL, n - i0);
        // --- the notes: which part, its glide ---
        while (part + 1 < voice.parts.size() && i0 >= voice.parts[part + 1].at) {
            part++;
            pitchFrom = pitch, gainFrom = gain, panFrom = pan;
        }
        const Part& pt = voice.parts[part];
        double glideF = pt.glideS * sr;
        double along = glideF >= 1 ? std::min(1.0, (double)(i0 - pt.at) / glideF) : 1.0;
        pitch = pitchFrom + (pt.pitch - pitchFrom) * along;
        gain = gainFrom + (pt.gain - gainFrom) * along;
        pan = panFrom + (pt.pan - panFrom) * along;

        // --- the modulation sources (Env 1 as it is now, Env 2 stepped a block, the LFO) ---
        src[ENV1] = e1.level;
        for (int k = 0; k < m; k++) {
            if (i0 + k == voice.gate) e2.letGo();
            e2.next();
        }
        src[ENV2] = e2.level;
        double rate = lfoRate;
        if (sync == 0 && modded[LFO_RATE]) {
            double nr = base[LFO_RATE];
            for (int r = 0; r < nRoutes; r++)
                if (routes[r].target == LFO_RATE) nr += routes[r].depth * src[routes[r].source];
            rate = fromNorm(LFO_RATE, nr);
        }
        src[LFO] = lfoWave(lfoShape, lfoPhase, lfoSeed, lfoCycle);
        lfoPhase += rate * m / sr;
        if (lfoPhase >= 1) {
            lfoCycle += (int64_t)lfoPhase;
            lfoPhase -= std::floor(lfoPhase);
        }
        if (nRoutes) {
            double norm[PARAMS];
            for (int r = 0; r < nRoutes; r++) norm[routes[r].target] = base[routes[r].target];
            for (int r = 0; r < nRoutes; r++) norm[routes[r].target] += routes[r].depth * src[routes[r].source];
            for (int r = 0; r < nRoutes; r++) {
                int t = routes[r].target;
                const ParamInfo& in = info(t);
                double nn = std::clamp(norm[t], 0.0, 1.0);  // (Int steps glide when modulated: semitones bend)
                val[t] = in.kind == ParamInfo::Log ? in.lo * std::pow(in.hi / in.lo, nn) : in.lo + nn * (in.hi - in.lo);
            }
        }
        double t = (double)(i0) / sr;
        auto drift = [&](int k) { return std::sin(2 * PI * (driftPhase[k] + driftRate[k] * t)); };
        double driftCents = P[DRIFT] > 0 ? val[DRIFT] * DRIFT_CENTS * (0.6 * drift(0) + 0.4 * drift(1)) : 0.0;
        double driftOct = P[DRIFT] > 0 ? val[DRIFT] * DRIFT_OCT * (0.6 * drift(2) + 0.4 * drift(3)) : 0.0;

        // --- the oscillators, a block ---
        std::fill(bl, bl + m, 0.0f);
        std::fill(br, br + m, 0.0f);
        for (int o = 0; o < 2; o++) {
            if (!oscOn[o]) continue;
            int b = o ? O2_WAVE : O1_WAVE;
            double level = std::clamp(val[b + 4], 0.0, 1.0);
            if (level <= 0) continue;
            double semis = pitch + 12 * P[b + 1] + val[b + 2] + (val[b + 3] + driftCents) / 100;
            double width = std::clamp(val[b + 5], 0.05, 0.95);
            for (int u = 0; u < unison; u++) {
                double cents = unison > 1 ? val[SPREAD] * (2.0 * u / (unison - 1) - 1) : 0.0;
                double f = 440.0 * std::pow(2.0, (semis - 69 + (cents + uDrift[u]) / 100) / 12);
                double dt = std::min(0.45, f / sr);
                float gl = (float)level * (stereo ? uL[u] : 1.0f), gr = (float)level * (stereo ? uR[u] : 1.0f);
                oscillate(waves[o], phase[o][u], dt, width, gl, gr, bl, br, m);
            }
        }
        if (noiseOn) {
            float level = (float)std::clamp(val[NOISE], 0.0, 1.0);
            for (int k = 0; k < m; k++) {
                noise ^= noise << 13, noise ^= noise >> 17, noise ^= noise << 5;
                float w = (float)((double)noise * (2.0 / 4294967295.0) - 1.0) * level;
                bl[k] += w, br[k] += w;
            }
        }

        // --- the filter ---
        double oct = std::log2(std::max(20.0, val[CUTOFF])) + val[KEYTRACK] * (pt.pitch - 60) / 12 +
                     val[F_ENV] * FILTER_ENV_OCT * src[ENV2] + driftOct;
        double fc = std::clamp(std::exp2(oct), 20.0, 0.45 * sr);
        double res = std::clamp(val[RESO], 0.0, 1.0);
        double g = std::tan(PI * fc / sr), k = 2 - 1.96 * res;  // (k 0.04 at full resonance: Q 25, still stable)
        double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        double comp = ftype == BANDPASS ? 1.0 : 1 / (1 + 1.5 * res);  // (its resonance's level kept in hand)
        double drive = std::clamp(val[DRIVE], 0.0, 1.0), dg = 1 + 9 * drive, dn = 1 / std::tanh(dg),
               dmix = std::min(1.0, drive * 4);

        // --- level and pan, frame by frame ---
        double nl, nr;
        note_props::panGains(pan, nl, nr);
        double out = outBase * gain * dsp::dbToGain(val[OUTPUT], -1000);
        for (int k2 = 0; k2 < m; k2++) {
            int64_t i = i0 + k2;
            if (i == voice.gate) e1.letGo();
            double env = e1.next();
            double xl = bl[k2], xr = br[k2];
            if (drive > 0) {
                xl += (std::tanh(dg * xl) * dn - xl) * dmix;
                if (stereo) xr += (std::tanh(dg * xr) * dn - xr) * dmix;
            }
            double yl = fl.run(xl, a1, a2, a3, k, ftype) * comp;
            double yr = stereo ? fr.run(xr, a1, a2, a3, k, ftype) * comp : yl;
            double level = out * env;
            if (voice.cut >= 0 && i >= voice.cut) level *= std::max(0.0, 1.0 - (double)(i - voice.cut) / CUT_FADE);
            outL[(size_t)i] = (float)(yl * level * nl);
            outR[(size_t)i] = (float)(yr * level * nr);
        }
    }
}

// the notes as voices, in note order (see the top)
std::vector<Voice> voicesOf(const Patch& P, const std::vector<Note>& notes, int sr) {
    std::vector<Voice> out;
    if (notes.empty()) return out;
    double glide = P[GLIDE];
    bool mono = P[MONO] >= 0.5;
    std::vector<int> order(notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        return notes[a].pitch < notes[b].pitch;
    });
    auto partOf = [&](const Note& note, int64_t a0, double glideS) {
        return Part{std::max<int64_t>(0, (int64_t)(note.start * sr) - a0), heardPitch(note), note.velocity / 127.0,
                    note.pan, glideS};
    };
    auto seedOf = [](const Note& n) {
        uint64_t k = mix64((uint64_t)(int64_t)std::llround(n.start * 1e6));
        return mix64(k ^ (uint64_t)(int64_t)std::llround(n.pitch * 100));
    };
    if (!mono) {
        std::vector<note_props::Slide> slid = note_props::slides(notes);
        std::vector<int> takenBy(notes.size(), -1);
        for (size_t i = 0; i < notes.size(); i++)
            if (slid[i].legato) takenBy[(size_t)slid[i].from] = (int)i;
        int prev = -1;
        for (int k : order) {
            const Note& note = notes[(size_t)k];
            int before = prev;
            prev = k;
            if (slid[(size_t)k].legato) continue;  // (played by the voice it took over)
            Voice v;
            v.start = (int64_t)(note.start * sr);
            v.startS = note.start;
            v.seed = seedOf(note);
            int from = slid[(size_t)k].from >= 0 ? slid[(size_t)k].from : glide > 0 ? before : -1;
            double firstGlide = slid[(size_t)k].from >= 0 ? std::max(glide, note_props::GLIDE_S) : glide;
            v.fromPitch = from >= 0 ? heardPitch(notes[(size_t)from]) : heardPitch(note);
            v.parts.push_back(partOf(note, v.start, from >= 0 ? firstGlide : 0.0));
            int last = k;
            for (size_t guard = 0; takenBy[(size_t)last] >= 0 && guard < notes.size(); guard++) {
                last = takenBy[(size_t)last];
                v.parts.push_back(partOf(notes[(size_t)last], v.start, std::max(glide, note_props::GLIDE_S)));
            }
            v.gate = std::max<int64_t>(1, (int64_t)((notes[(size_t)last].end - note.start) * sr));
            out.push_back(std::move(v));
        }
        return out;
    }
    // mono: a voice for each run of overlapping notes, cut off where the next begins
    double lastPitch = -1;
    for (size_t j = 0; j < order.size();) {
        const Note& first = notes[(size_t)order[j]];
        Voice v;
        v.start = (int64_t)(first.start * sr);
        v.startS = first.start;
        v.seed = seedOf(first);
        bool glides = lastPitch >= 0 && (glide > 0 || first.slide);
        v.fromPitch = glides ? lastPitch : heardPitch(first);
        v.parts.push_back(partOf(first, v.start, glides ? std::max(glide, first.slide ? note_props::GLIDE_S : 0.0) : 0.0));
        double end = first.end;
        size_t k = j + 1;
        for (; k < order.size() && notes[(size_t)order[k]].start < end; k++) {
            const Note& nn = notes[(size_t)order[k]];
            v.parts.push_back(partOf(nn, v.start, glide > 0 || nn.slide ? std::max(glide, nn.slide ? note_props::GLIDE_S : 0.0) : 0.0));
            end = std::max(end, nn.end);
        }
        v.gate = std::max<int64_t>(1, (int64_t)((end - first.start) * sr));
        if (k < order.size()) v.cut = std::max<int64_t>(1, (int64_t)(notes[(size_t)order[k]].start * sr) - v.start);
        lastPitch = v.parts.back().pitch;
        out.push_back(std::move(v));
        j = k;
    }
    return out;
}

}  // namespace

AudioPtr render(const Patch& patch, const std::vector<Note>& notes, int sr, double lengthS, double beatS) {
    double t0 = util::wallClock();
    int64_t len = (int64_t)((lengthS + std::max(synth::RELEASE_S, tailS(patch))) * sr) + 1;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.assign((size_t)len * 2, 0.0f);
    std::vector<Voice> voices = voicesOf(patch, notes, sr);
    // (voices starting outside the clip's audio: left out, as the classic voice does)
    voices.erase(std::remove_if(voices.begin(), voices.end(), [&](const Voice& v) { return v.start < 0 || v.start >= len; }),
                 voices.end());
    // Each voice rendered on its own, on every core when there are several (a batch at a time), then summed in
    // note order: the same, bit for bit, however many threads
    int threads = voices.size() >= 4 && !workers::inJob() ? workers::workersFor(workers::chosenCores()) : 0;
    threads = std::max(0, std::min(threads, (int)voices.size() - 1));
    struct Work {
        const Patch* patch;
        const std::vector<Voice>* voices;
        int sr;
        double beatS;
        size_t from;
        std::vector<std::vector<float>> l, r;
    } work{&patch, &voices, sr, beatS, 0, {}, {}};
    std::unique_ptr<workers::Pool> pool;
    if (threads > 0) pool = std::make_unique<workers::Pool>(threads, false);  // (as a render's: never outranking playback)
    size_t batch = 2 * ((size_t)threads + 1);
    work.l.resize(batch), work.r.resize(batch);
    for (size_t from = 0; from < voices.size(); from += batch) {
        size_t count = std::min(batch, voices.size() - from);
        work.from = from;
        auto job = [](void* ctx, int i) {
            Work& w = *static_cast<Work*>(ctx);
            renderVoice(*w.patch, (*w.voices)[w.from + (size_t)i], w.sr, w.beatS, w.l[(size_t)i], w.r[(size_t)i]);
        };
        if (pool) pool->run((int)count, job, &work);
        else
            for (size_t i = 0; i < count; i++) job(&work, (int)i);
        for (size_t i = 0; i < count; i++) {
            const Voice& v = voices[from + i];
            int64_t m = std::min<int64_t>((int64_t)work.l[i].size(), len - v.start);
            float* o = out->samples.data() + v.start * 2;
            const float* l = work.l[i].data();
            const float* r = work.r[i].data();
            for (int64_t k = 0; k < m; k++) o[2 * k] += l[k], o[2 * k + 1] += r[k];
        }
    }
    util::debugf("synth: %zu notes as %zu voices (patch \"%s\", unison %d, %zu modulations) rendered in %.3f s on %d "
                 "worker threads +1\n",
                 notes.size(), voices.size(), patch.name.c_str(), (int)patch[UNISON], patch.mods.size(),
                 util::wallClock() - t0, threads);
    return out;
}

std::vector<float> renderNote(const Patch& patch, int pitch, int velocity, double durationS, int sr, double beatS) {
    Note n;
    n.start = 0, n.end = std::max(1.0 / sr, durationS), n.pitch = pitch, n.velocity = velocity;
    std::vector<Voice> voices = voicesOf(patch, {n}, sr);
    std::vector<float> l, r;
    renderVoice(patch, voices[0], sr, beatS, l, r);
    std::vector<float> out(l.size() * 2);
    for (size_t i = 0; i < l.size(); i++) out[2 * i] = l[i], out[2 * i + 1] = r[i];
    return out;
}

}  // namespace subsynth
