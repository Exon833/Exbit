// Tests of the sampler at Simpler's level (audio/sampler.h, model.h SamplerSettings): old settings sounding as
// before, bit for bit; each mode; the filter's response; glide timing; the LFO synced to the tempo; warping at
// another song tempo; reverse and normalise; saving, loading and undo; and the worst case: 64 voices with every
// modulation, retriggered every frame.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "audio/engine.h"
#include "audio/padkit.h"
#include "audio/sampler.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"
#include "tui/screen.h"
#include "tui/window_view.h"
#include "ui/sampler_dialog.h"

using namespace project_io;

// --- allocations counted while gCount is on (this program's own operator new) ---
static std::atomic<bool> gCount{false};
static std::atomic<long> gAllocs{0};
void* operator new(size_t n) {
    if (gCount.load(std::memory_order_relaxed)) gAllocs++;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
const int SR = 48000;

template <class F>
ClipPtr sampleOf(int64_t frames, F value) {
    auto b = std::make_shared<AudioBuffer>();
    for (int64_t i = 0; i < frames; i++) b->samples.push_back((float)value(i)), b->samples.push_back((float)(0.7 * value(i)));
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "test.wav";
    c->source = b;
    c->sourceSr = SR;
    return c;
}

// a sample with some of everything: a falling tone and noise
ClipPtr richSample(int64_t frames = 24000) {
    uint32_t seed = 1;
    return sampleOf(frames, [&](int64_t i) {
        seed = seed * 1664525u + 1013904223u;
        double noise = (double)(seed >> 8) / 16777216.0 - 0.5;
        return 0.5 * std::sin(2 * M_PI * 220 * i / SR) * std::exp(-(double)i / 20000) + 0.2 * noise;
    });
}

uint64_t fnv(const float* d, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits;
        std::memcpy(&bits, &d[i], 4);
        for (int b = 0; b < 4; b++) h = (h ^ ((bits >> (8 * b)) & 0xff)) * 1099511628211ull;
    }
    return h;
}

// the old settings' sounds: renderNote, render (with slides, pans and fine tuning), a Pad Kit's
uint64_t oldSounds() {
    ClipPtr sample = richSample();
    uint64_t h = 1469598103934665603ull;
    SamplerSettings variants[5];
    for (auto& v : variants) v.sample = sample;
    variants[1].loop = true, variants[1].start = 0.1, variants[1].end = 0.6, variants[1].attack = 0.01;
    variants[2].oneShot = true, variants[2].transpose = 3.5, variants[2].gainDb = -3;
    variants[3].decay = 0.05, variants[3].sustain = 0.3, variants[3].release = 0.4, variants[3].velocity = 1;
    variants[4].loop = true, variants[4].root = 48, variants[4].start = 0.3, variants[4].end = 0.31;
    for (const SamplerSettings& s : variants) {
        for (int pitch : {48, 60, 67, 79}) {
            std::vector<float> one = sampler::renderNote(s, pitch, 90, 0.3, SR, -1, 7);
            h = fnv(one.data(), one.size(), h);
        }
        std::vector<Note> notes;
        for (int i = 0; i < 8; i++) {
            Note n;
            n.pitch = 55 + i * 2;
            n.start = 0.1 * i;
            n.end = n.start + 0.15 + 0.03 * i;
            n.velocity = 40 + 10 * i;
            n.pan = i % 3 == 0 ? 0.0 : -0.5 + 0.2 * i;
            n.fine = i % 2 ? 13 : 0;
            n.slide = i == 3 || i == 4 || i == 6;
            notes.push_back(n);
        }
        AudioPtr all = sampler::render(s, notes, SR, 1.0);
        h = fnv(all->samples.data(), all->samples.size(), h);
    }
    PadKit kit;
    kit.pads[0] = padkit::newPad(sample, 36);
    kit.pads[1] = padkit::newPad(sample, 38);
    kit.pads[1].sound.oneShot = false, kit.pads[1].sound.loop = true, kit.pads[1].choke = 1;
    kit.pads[2] = padkit::newPad(sample, 39);
    kit.pads[2].choke = 1;
    std::vector<Note> hits;
    for (int i = 0; i < 6; i++) {
        Note n;
        n.pitch = 36 + i % 4;
        n.start = 0.07 * i;
        n.end = n.start + 0.05;
        n.velocity = 100;
        hits.push_back(n);
    }
    AudioPtr drums = padkit::render(kit, hits, SR, 0.6);
    return fnv(drums->samples.data(), drums->samples.size(), h);
}
}  // namespace

static void testOldSettingsIdentical() {
    uint64_t h = oldSounds();
    std::printf("old sounds: %016llx\n", (unsigned long long)h);
    CHECK(h == 0xee95e73c05319d73ull);  // (what the sampler made of them before its filter, LFO, glide, warping and modes)
}


namespace {
SamplerSettings plain(ClipPtr sample) {
    SamplerSettings s;
    s.sample = std::move(sample);
    s.attack = 0, s.decay = 0, s.sustain = 1, s.release = 0.001;
    s.velocity = 0;
    return s;
}

// the song's tempo the sampler hears (clip_hooks::currentTempoMap)
void songTempo(double bpm) {
    auto map = std::make_shared<const TempoMap>(bpm);
    clip_hooks::tempoMap = [map] { return map; };
}

// how loud the sound is around frequency hz (left channel; the mean power of the bins hz ± 40 Hz, 4 Hz apart)
double powerAt(const std::vector<float>& stereo, double hz) {
    size_t n = stereo.size() / 2;
    double sum = 0;
    int bins = 0;
    for (double f = hz - 40; f <= hz + 40; f += 4, bins++) {
        double w = 2 * M_PI * f / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; i++) {
            double s0 = stereo[i * 2] + c * s1 - s2;
            s2 = s1, s1 = s0;
        }
        sum += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return sum / bins;
}

// the frequency of a tone in frames [a, b) of the left channel (rising zero crossings)
double frequency(const std::vector<float>& stereo, int64_t a, int64_t b) {
    int64_t first = -1, last = -1, count = 0;
    for (int64_t i = std::max<int64_t>(a, 1); i < b && i < (int64_t)stereo.size() / 2; i++)
        if (stereo[(size_t)(i - 1) * 2] < 0 && stereo[(size_t)i * 2] >= 0) {
            if (first < 0) first = i;
            else count++;
            last = i;
        }
    return count > 0 ? (double)count * SR / (double)(last - first) : 0;
}

bool finite(const float* d, size_t n, float bound) {
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(d[i]) || std::fabs(d[i]) > bound) return false;
    return true;
}

ClipPtr tone(double hz, int64_t frames, double level = 0.5) {
    return sampleOf(frames, [=](int64_t i) { return level * std::sin(2 * M_PI * hz * i / SR); });
}

Note noteOf(double start, double end, int pitch, int velocity = 100) {
    Note n;
    n.start = start, n.end = end, n.pitch = pitch, n.velocity = velocity;
    return n;
}
}  // namespace

static void testModes() {
    // classic, looping from the loop's start: the sample a ramp, so its value says where it plays
    const int64_t len = 4800;
    SamplerSettings s = plain(sampleOf(len, [&](int64_t i) { return 0.25 + 0.5 * (double)i / len; }));
    s.loop = true, s.loopStart = 0.5;
    std::vector<float> held = sampler::renderNote(s, 60, 127, 0.5, SR);
    CHECK((int64_t)held.size() / 2 >= SR / 2);
    float least = 1;
    for (int64_t k = len + 100; k < SR / 2 - 200; k++) least = std::min(least, held[(size_t)k * 2]);
    CHECK(least > 0.49f);  // (after the first pass: only the second half, from the loop's start)
    CHECK_NEAR(held[100 * 2], 0.25 + 0.5 * 100 / len, 1e-4);  // (the first pass from the start)
    CHECK_NEAR(sampler::loopPeriod(s, 60, SR), (double)(len - 1 - len / 2), 1.0);
    // one-shot: to the end whatever the note's length
    SamplerSettings o = plain(tone(440, SR / 2));
    o.oneShot = true;
    CHECK_NEAR((double)sampler::renderNote(o, 60, 100, 0.01, SR).size() / 2, SR / 2 - 1, 2);
    // slice: four hits at known places, silence between
    const int64_t hits[4] = {0, 12000, 24000, 36000};
    ClipPtr drums = sampleOf(SR, [&](int64_t i) {
        for (int h = 0; h < 4; h++)
            if (i >= hits[h] && i < hits[h] + 4000)
                return (0.3 + 0.1 * h) * std::sin(2 * M_PI * (200 + 100 * h) * (i - hits[h]) / SR) *
                       std::exp(-(double)(i - hits[h]) / 800);
        return 0.0;
    });
    SamplerSettings sl = plain(drums);
    sl.slice = true;
    sl.root = 36;
    std::vector<int64_t> starts = sampler::sliceStarts(sl, SR);
    CHECK(starts.size() == 4);
    for (size_t h = 0; h < 4 && h < starts.size(); h++) CHECK(std::llabs(starts[h] - hits[h]) < 600);
    std::vector<float> third = sampler::renderNote(sl, 38, 127, 0.01, SR);  // (key root + 2: the third slice)
    CHECK(!third.empty() && (int64_t)third.size() / 2 <= hits[3] - starts[2] + 1);
    if (starts.size() == 4 && !third.empty()) {
        double xy = 0, xx = 0, yy = 0;
        for (int64_t k = 0; k < 3000; k++) {
            double x = drums->source->samples[(size_t)(starts[2] + k) * 2], y = third[(size_t)k * 2];
            xy += x * y, xx += x * x, yy += y * y;
        }
        CHECK(xy / std::sqrt(xx * yy + 1e-30) > 0.999);  // (its own sound, at its own pitch)
    }
    CHECK(sampler::renderNote(sl, 35, 127, 0.1, SR).empty());  // (below the first slice: nothing)
    CHECK(sampler::renderNote(sl, 40, 127, 0.1, SR).empty());  // (past the last)
    // on a grid at the sample's tempo: 1 s at 120 BPM, quarters: every half second
    sl.sliceGrid = 4, sl.bpm = 120;
    starts = sampler::sliceStarts(sl, SR);
    CHECK(starts.size() == 2 && starts.size() == 2 && starts[1] == SR / 2);
    // a clip in slice mode: each note its slice, played to its end
    AudioPtr clip = sampler::render(sl, {noteOf(0, 0.01, 37)}, SR, 1.0);
    double later = 0;
    for (int64_t k = SR / 4; k < SR / 2 - 100; k++) later = std::max(later, (double)std::fabs(clip->samples[(size_t)k * 2]));
    CHECK(later > 0.01);  // (the second slice (hits 3 and 4) still playing a quarter second in: one-shot)
}

static void testFilter() {
    // white noise through the filter: its response measured against the noise itself
    uint32_t seed = 7;
    ClipPtr noise = sampleOf(SR, [&](int64_t) {
        seed = seed * 1664525u + 1013904223u;
        return ((double)(seed >> 8) / 16777216.0 - 0.5) * 0.5;
    });
    SamplerSettings s = plain(noise);
    std::vector<float> dry = sampler::renderNote(s, 60, 127, 1.0, SR);
    auto db = [&](const std::vector<float>& wet, double hz) { return 10 * std::log10(powerAt(wet, hz) / powerAt(dry, hz)); };
    s.filter = SamplerSettings::LowPass, s.cutoff = 1000;
    std::vector<float> lp = sampler::renderNote(s, 60, 127, 1.0, SR);
    CHECK(lp.size() == dry.size());
    CHECK_NEAR(db(lp, 250), 0, 1.0);
    CHECK_NEAR(db(lp, 1000), -3, 1.5);   // (the cutoff: -3 dB)
    CHECK_NEAR(db(lp, 4000), -24, 3.0);  // (12 dB an octave, two octaves up)
    s.steep = true;
    std::vector<float> lp24 = sampler::renderNote(s, 60, 127, 1.0, SR);
    CHECK_NEAR(db(lp24, 4000), -48, 5.0);
    s.filter = SamplerSettings::HighPass, s.steep = false;
    std::vector<float> hp = sampler::renderNote(s, 60, 127, 1.0, SR);
    CHECK_NEAR(db(hp, 8000), 0, 1.0);
    CHECK_NEAR(db(hp, 250), -24, 3.0);
    s.filter = SamplerSettings::BandPass;
    std::vector<float> bp = sampler::renderNote(s, 60, 127, 1.0, SR);
    std::printf("band-pass: %.1f %.1f %.1f dB\n", db(bp, 250), db(bp, 1000), db(bp, 4000));
    CHECK(db(bp, 1000) > -1.5 && db(bp, 250) < -7 && db(bp, 4000) < -7);  // (12 dB a side, Q 0.7: -9 dB two octaves off)
    s.filter = SamplerSettings::Notch, s.resonance = 0.5;
    std::vector<float> notch = sampler::renderNote(s, 60, 127, 1.0, SR);
    std::printf("notch: %.1f %.1f dB\n", db(notch, 1000), db(notch, 250));
    CHECK(db(notch, 1000) < -8 && db(notch, 250) > -1.5);
    // key tracking: an octave up, the cutoff an octave up (the note an octave up plays the noise twice as fast)
    s.filter = SamplerSettings::LowPass, s.resonance = 0, s.cutoff = 1000, s.keyTrack = 1;
    std::vector<float> up = sampler::renderNote(s, 72, 127, 1.0, SR), upDry;
    {
        SamplerSettings d = plain(noise);
        upDry = sampler::renderNote(d, 72, 127, 1.0, SR);
    }
    CHECK_NEAR(10 * std::log10(powerAt(up, 2000) / powerAt(upDry, 2000)), -3, 1.5);
    // its envelope: bright at first (+4 octaves), down to the cutoff once it has decayed
    s.keyTrack = 0, s.cutoff = 300, s.filterEnv = 4, s.fAttack = 0, s.fDecay = 0.05, s.fSustain = 0;
    std::vector<float> swept = sampler::renderNote(s, 60, 127, 1.0, SR);
    std::vector<float> early(swept.begin(), swept.begin() + SR / 25 * 2), late(swept.begin() + SR / 2 * 2, swept.begin() + SR / 2 * 2 + SR / 25 * 2);
    CHECK(powerAt(early, 3000) > 30 * powerAt(late, 3000));
}

static void testGlide() {
    // a 440 Hz tone as A4: a note on A3, then one on A4 gliding over 0.2 s
    SamplerSettings s = plain(tone(440, SR * 2));
    s.root = 69, s.glide = 0.2;
    AudioPtr out = sampler::render(s, {noteOf(0, 0.5, 57), noteOf(0.5, 1.0, 69)}, SR, 1.0);
    const std::vector<float>& v = out->samples;
    CHECK_NEAR(frequency(v, SR / 10, SR * 4 / 10), 220, 3);
    double half = 220 * std::pow(2.0, 0.5);  // (half way: the pitch half way, in semitones)
    CHECK_NEAR(frequency(v, (int64_t)(0.59 * SR), (int64_t)(0.61 * SR)), half, half * 0.05);
    CHECK_NEAR(frequency(v, (int64_t)(0.75 * SR), (int64_t)(0.95 * SR)), 440, 3);
    // no glide: it jumps
    s.glide = 0;
    AudioPtr jump = sampler::render(s, {noteOf(0, 0.5, 57), noteOf(0.5, 1.0, 69)}, SR, 1.0);
    CHECK_NEAR(frequency(jump->samples, (int64_t)(0.52 * SR), (int64_t)(0.6 * SR)), 440, 5);
    // mono: the second note, starting while the first sounds, takes its voice over (one voice: no louder)
    s.mono = true, s.glide = 0.1;
    AudioPtr mono = sampler::render(s, {noteOf(0, 1.0, 57), noteOf(0.5, 1.0, 69)}, SR, 1.0);
    float peakAfter = 0;
    for (int64_t k = (int64_t)(0.7 * SR); k < (int64_t)(0.9 * SR); k++) peakAfter = std::max(peakAfter, std::fabs(mono->samples[(size_t)k * 2]));
    CHECK(peakAfter < 0.52f);
    CHECK_NEAR(frequency(mono->samples, (int64_t)(0.7 * SR), (int64_t)(0.95 * SR)), 440, 3);
    s.mono = false;
    AudioPtr poly = sampler::render(s, {noteOf(0, 1.0, 57), noteOf(0.5, 1.0, 69)}, SR, 1.0);
    float polyPeak = 0;
    for (int64_t k = (int64_t)(0.7 * SR); k < (int64_t)(0.9 * SR); k++) polyPeak = std::max(polyPeak, std::fabs(poly->samples[(size_t)k * 2]));
    CHECK(polyPeak > 0.6f);  // (two voices)
    // live: a note glides from the last one played (live.h glideFrom)
    std::vector<float> live = sampler::renderNote(s, 69, 127, 0.5, SR, -1, 0, 57);
    CHECK(frequency(live, 0, SR / 50) < 300);  // (it starts low…)
    CHECK_NEAR(frequency(live, SR / 4, SR / 2), 440, 3);  // (…and arrives)
}

static void testLfoSync() {
    // the volume moved by a square LFO, a quarter note long: half a cycle loud, half silent
    SamplerSettings s = plain(sampleOf(SR * 3, [](int64_t) { return 0.5; }));
    s.lfoDepth = 1, s.lfoTarget = SamplerSettings::ToVolume, s.lfoShape = 4 /* square */, s.lfoSync = true, s.lfoBeats = 4;
    CHECK(sampler::followsTempo(s));
    auto firstQuiet = [&] {
        std::vector<float> out = sampler::renderNote(s, 60, 127, 2.0, SR);
        for (size_t k = 0; k < out.size() / 2; k++)
            if (out[k * 2] < 0.25f) return (double)k / SR;
        return -1.0;
    };
    songTempo(120);
    CHECK_NEAR(firstQuiet(), 0.25, 0.001);
    songTempo(60);
    CHECK_NEAR(firstQuiet(), 0.5, 0.001);
    songTempo(90);
    s.lfoBeats = 7;  // (a bar: 4 beats at 90 BPM: 2.667 s)
    CHECK_NEAR(firstQuiet(), 4 * 60.0 / 90 / 2, 0.001);
    s.lfoSync = false, s.lfoRate = 2;  // (in Hz: the tempo doesn't matter)
    CHECK_NEAR(firstQuiet(), 0.25, 0.001);
    CHECK(!sampler::followsTempo(s));
    // pan: one side down while the other is full
    s.lfoTarget = SamplerSettings::ToPan, s.lfoShape = 0;
    std::vector<float> panned = sampler::renderNote(s, 60, 127, 1.0, SR);
    int64_t k = SR / 8;  // (the sine's top: panned right)
    CHECK(panned[(size_t)k * 2] < 0.05f && panned[(size_t)k * 2 + 1] > 0.34f);  // (the right: 0.7 of the left)
    songTempo(120);
}

static void testWarp() {
    // 2 s of a sample at 120 BPM (4 beats): at 60 BPM it plays 4 s, whatever the key
    SamplerSettings s = plain(tone(330, SR * 2));
    s.warp = true, s.bpm = 120, s.oneShot = true, s.warpMode = 1;
    songTempo(60);
    CHECK(sampler::followsTempo(s));
    CHECK_NEAR((double)sampler::renderNote(s, 60, 127, 0.1, SR).size() / 2 / SR, 4.0, 0.05);
    std::vector<float> up = sampler::renderNote(s, 72, 127, 0.1, SR);
    CHECK_NEAR((double)up.size() / 2 / SR, 4.0, 0.05);
    CHECK_NEAR(frequency(up, SR, SR * 2), 660, 10);  // (an octave up: its pitch, not its length)
    songTempo(120);
    CHECK_NEAR((double)sampler::renderNote(s, 60, 127, 0.1, SR).size() / 2 / SR, 2.0, 0.05);
    songTempo(180);
    CHECK_NEAR((double)sampler::renderNote(s, 60, 127, 0.1, SR).size() / 2 / SR, 2.0 * 120 / 180, 0.05);
    songTempo(120);
}

static void testReverseNormalise() {
    const int64_t len = 4800;
    SamplerSettings s = plain(sampleOf(len, [&](int64_t i) { return 0.25 * (double)i / len; }));
    s.oneShot = true;
    s.reverse = true;
    std::vector<float> back = sampler::renderNote(s, 60, 127, 0.01, SR);
    CHECK(back.size() / 2 >= (size_t)len - 2);
    CHECK_NEAR(back[0], 0.25 * (len - 1) / len, 1e-4);  // (from its end)
    CHECK(back[1000 * 2] > back[2000 * 2]);           // (going down)
    s.normalise = true;
    std::vector<float> loud = sampler::renderNote(s, 60, 127, 0.01, SR);
    CHECK_NEAR(loud[0], 1.0, 1e-4);  // (its loudest sample at full level)
    s.reverse = false;
    std::vector<float> fwd = sampler::renderNote(s, 60, 127, 0.01, SR);
    CHECK_NEAR(fwd[(size_t)(len - 1000) * 2], (double)(len - 1000) / (len - 1), 1e-3);
}

// the worst case: 64 voices at once, every modulation, a note started every frame
static void testWorstCase() {
    ClipPtr sample = richSample(SR / 2);
    for (int target = 0; target < 4; target++) {
        SamplerSettings s = plain(sample);
        s.attack = 0.001, s.decay = 0.01, s.sustain = 0.5, s.release = 0.01;
        s.velocity = 1;
        s.loop = true, s.loopStart = 0.4, s.start = 0.1, s.end = 0.9;
        s.reverse = true, s.normalise = true;
        s.filter = 1 + target, s.steep = true, s.cutoff = 800, s.resonance = 1, s.filterEnv = 8, s.keyTrack = 1;
        s.fAttack = 0, s.fDecay = 0.005, s.fSustain = 0.2, s.fRelease = 0.01;
        s.lfoDepth = 1, s.lfoTarget = target, s.lfoShape = 6 - target, s.lfoRate = 40;
        s.glide = 0.01;
        std::vector<Note> notes;
        for (int k = 0; k < 1500; k++)  // (a note every frame, each 64 frames long: 64 sounding at once)
            notes.push_back(noteOf((double)k / SR, (double)(k + 64) / SR, k % 128, 1 + k % 127));
        AudioPtr out = sampler::render(s, notes, SR, 0.1);
        CHECK(out && finite(out->samples.data(), out->samples.size(), 64.0f));
    }
    // played live: 64 voices on the audio thread, a new one every frame, nothing allocated there
    auto settings = std::make_shared<SamplerSettings>(plain(sample));
    SamplerSettings& s = *settings;
    s.filter = SamplerSettings::LowPass, s.resonance = 0.9, s.filterEnv = 3, s.lfoDepth = 1, s.lfoTarget = SamplerSettings::ToFilter;
    s.release = 0.005;
    Track t;
    t.sampler = settings;
    live::NoteRender render = sampler::noteRenderer(t);
    live::LiveNotes notes;
    plugin_audio::Messages msgs;
    msgs.reserve(4096);
    float left[1], right[1];
    float* const signal[2] = {left, right};
    long allocs = 0;
    bool fine = true;
    for (int k = 0; k < 800; k++) {
        notes.noteOn(36 + k % 64, 100, SR, true, 64.0 / SR, render.render ? &render : nullptr);
        left[0] = right[0] = 0;
        gAllocs = 0;
        gCount = true;
        notes.take(1, SR, msgs);
        notes.addVoices(signal, 2, 1, SR);
        gCount = false;
        allocs += gAllocs;
        msgs.clear();
        fine = fine && finite(left, 1, 64.0f) && finite(right, 1, 64.0f);
    }
    CHECK(allocs == 0);
    CHECK(fine);
}

static void testSaveLoadUndo() {
    const std::string dir = "build/test-tmp/sampler-simpler";
    util::makeDirs(dir);
    Project p;
    TrackPtr t = p.addTrack(Kind::Midi);
    t->name = "Sampler";
    auto s = std::make_shared<SamplerSettings>(plain(richSample()));
    SamplerSettings& v = *s;
    v.loop = true, v.loopStart = 0.3, v.slice = true, v.sliceGrid = 16, v.sensitivity = 0.7, v.reverse = true;
    v.normalise = true, v.filter = SamplerSettings::HighPass, v.steep = true, v.cutoff = 1234, v.resonance = 0.4;
    v.filterEnv = -2.5, v.keyTrack = 0.5, v.fAttack = 0.01, v.fDecay = 0.2, v.fSustain = 0.3, v.fRelease = 0.4;
    v.lfoShape = 3, v.lfoSync = true, v.lfoRate = 7, v.lfoBeats = 2, v.lfoDepth = 0.6, v.lfoTarget = SamplerSettings::ToPan;
    v.glide = 0.08, v.mono = true, v.warp = true, v.bpm = 97, v.warpMode = 2;
    t->sampler = s;
    std::string error, path = dir + "/simpler.tdaw";
    MasterSettings master;
    CHECK(saveProject(path, p, SR, master, error));
    Project q;
    CHECK(loadProject(path, q, SR, master, error));
    CHECK(q.tracks.size() == 1 && q.tracks[0]->sampler);
    if (q.tracks.size() == 1 && q.tracks[0]->sampler) {
        SamplerSettings back = *q.tracks[0]->sampler;
        CHECK(back.sample && back.sample->source && back.sample->source->frames() == v.sample->source->frames());
        back.sample = v.sample;
        CHECK(back == v);
    }
    // an old sampler (none of them set) is written as it always was: no new keys, and it reads back the same
    std::string text;
    {
        Project old;
        TrackPtr o = old.addTrack(Kind::Midi);
        o->sampler = std::make_shared<SamplerSettings>(plain(richSample()));
        CHECK(saveProject(dir + "/old.tdaw", old, SR, master, error));
        CHECK(util::readFile(dir + "/old.tdaw", text));
        Project again;
        CHECK(loadProject(dir + "/old.tdaw", again, SR, master, error));
        SamplerSettings back = *again.tracks[0]->sampler;
        back.sample = o->sampler->sample;
        CHECK(back == *o->sampler);
    }
    for (const char* key : {"\"cutoff\"", "\"lfo_depth\"", "\"warp\"", "\"slice\"", "\"glide\"", "\"reverse\""})
        CHECK(text.find(key) == std::string::npos);
    CHECK(text.find("\"one_shot\"") != std::string::npos);
    // undo and redo: the settings replaced whole are a step
    history::History steps(p, SR);
    SamplerPtr before = t->sampler;
    auto after = std::make_shared<SamplerSettings>(*before);
    after->cutoff = 500, after->lfoDepth = 0.1;
    std::atomic_store(&t->sampler, SamplerPtr(after));
    CHECK(steps.check(SR));
    CHECK(steps.undo(SR));
    CHECK(t->sampler && t->sampler->cutoff == 1234 && t->sampler->lfoDepth == 0.6);
    CHECK(steps.redo(SR));
    CHECK(t->sampler && t->sampler->cutoff == 500 && t->sampler->lfoDepth == 0.1);
}

// the window, and the window in the terminal: its controls a list, its waveform a sketch
static void testWindowAndTerminal() {
    Project project;
    audio::Engine engine{project};
    ui::Window main;
    main.resizeWindow(1000, 700);
    sampler_dialog::Host host;
    host.window = &main;
    host.project = &project;
    host.engine = &engine;
    sampler_dialog::setHost(host);
    TrackPtr t = project.addTrack(Kind::Midi);
    t->name = "Keys";
    t->sampler = std::make_shared<SamplerSettings>(plain(richSample()));
    sampler_dialog::SamplerDialog* d = sampler_dialog::openSampler(t);
    CHECK(d != nullptr);
    if (!d) return;
    ui::App::get().doLayouts();
    // the window: a change in a box is the track's settings, replaced whole
    SamplerPtr first = t->sampler;
    d->controls.filter->setCurrentIndex(2);  // Low-pass 24 dB
    CHECK(t->sampler != first && t->sampler->filter == SamplerSettings::LowPass && t->sampler->steep);
    CHECK(d->controls.cutoff->isEnabled() && d->filterEnv->isEnabled());
    d->mode->setCurrentIndex(2);  // Slice
    CHECK(t->sampler->slice && !t->sampler->oneShot && d->sliceGrid->isEnabled() && !d->loop->isEnabled());
    d->sliceGrid->setCurrentIndex(2);  // 1/8 notes
    CHECK(t->sampler->sliceGrid == 8 && !d->waveform->slices.empty());
    d->mode->setCurrentIndex(0);
    d->loop->setChecked(true);
    d->lfoSync->setChecked(true);
    CHECK(t->sampler->lfoSync && d->lfoBeats->isEnabled() && !d->lfoRate->isEnabled());
    // dragging the loop's handle
    CHECK(d->waveform->loopStart >= 0);
    ui::MouseEvent e;
    e.button = plat::LeftButton;
    e.pos = Point(d->waveform->width() / 2, 30);
    d->waveform->loopStart = 0.45;  // (nearest the press)
    d->waveform->mousePress(e);
    d->waveform->mouseRelease(e);
    CHECK_NEAR(t->sampler->loopStart, 0.5, 0.01);
    CHECK_NEAR(d->loopStart->value(), 50, 1);
    // the terminal: the controls a list, a row each, the focused one current
    tui::Screen screen(130, 50);
    auto lines = [&] {
        screen.clear();
        tui::drawWindow(screen, *d, 0, 0, 130, 50);
        std::vector<std::string> out;
        for (int y = 0; y < 50; y++) {
            std::string line;
            for (int x = 0; x < 130; x++) {
                char32_t c = screen.at(x, y)->ch;
                line += c < 128 ? (char)c : c == U'█' ? '#' : c >= U'▁' && c <= U'▇' ? '=' : '?';
            }
            out.push_back(line);
        }
        return out;
    };
    auto find = [](const std::vector<std::string>& ls, const std::string& what) {
        for (const std::string& l : ls)
            if (l.find(what) != std::string::npos) return true;
        return false;
    };
    std::vector<std::string> shown = lines();
    CHECK(find(shown, "Mode"));
    CHECK(find(shown, "[") && find(shown, "]") && find(shown, "|"));  // (the sketch: start, end, the loop)
    std::vector<std::string> rows;
    int current = -1;
    d->page->listRows(rows, current);
    CHECK(rows.size() == 37);
    bool cutoffRow = false;
    for (const std::string& r : rows) cutoffRow = cutoffRow || (r.rfind("Cutoff", 0) == 0 && r.find("20000 Hz") != std::string::npos);
    CHECK(cutoffRow);
    // Tab to the cutoff's row, Down: a step down (it is at its top)
    auto focused = [&] {
        rows.clear();
        d->page->listRows(rows, current);
        return current >= 0 ? rows[(size_t)current] : std::string();
    };
    for (int i = 0; i < 80 && focused().rfind("Cutoff", 0) != 0; i++) tui::sendKey(*d, tui::Key{plat::Key_Tab, 0, ""});
    CHECK(focused().rfind("Cutoff", 0) == 0);
    tui::sendKey(*d, tui::Key{plat::Key_Down, 0, ""});
    CHECK_NEAR(t->sampler->cutoff, 20000 - 25, 1e-9);
    shown = lines();
    if (std::getenv("SHOW_TUI"))
        for (auto& l : shown) std::printf("%s\n", l.c_str());
    CHECK(find(shown, "19975 Hz"));
    tui::sendKey(*d, tui::Key{plat::Key_Tab, 0, ""});  // (Tab: the next row of the list)
    CHECK(focused().rfind("Resonance", 0) == 0);
    for (int i = 0; i < 80 && focused().rfind("LFO sync", 0) != 0; i++) tui::sendKey(*d, tui::Key{plat::Key_Tab, 0, ""});
    tui::sendKey(*d, tui::Key{plat::Key_Space, 0, " "});  // (a check box: Space)
    CHECK(!t->sampler->lfoSync);
    d->close();
    ui::App::get().collectGarbage();
}

int main() {
    ui::App::get().initHeadless();
    songTempo(120);
    testOldSettingsIdentical();
    testModes();
    testFilter();
    testGlide();
    testLfoSync();
    testWarp();
    testReverseNormalise();
    testWorstCase();
    testSaveLoadUndo();
    testWindowAndTerminal();
    TEST_MAIN_END();
}
