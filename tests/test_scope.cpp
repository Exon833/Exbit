// The master scope's analysis (audio/scope.h) and the master's audio kept for it (Metering::scopeOn,
// scopeLatest): spectra of known sines for every FFT size and window, the slope, fall and floor; the
// waveform's columns; the pitch of sines and a saw (and none in noise or silence); pitch follow
// keeping a held note still from picture to picture.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "audio/metering.h"
#include "audio/scope.h"
#include "check.h"

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int SR = 48000;

std::vector<float> sine(double hz, double amplitude, size_t n, int sr = SR, double phase = 0) {
    std::vector<float> out(n);
    for (size_t i = 0; i < n; i++) out[i] = (float)(amplitude * std::sin(phase + 2 * PI * hz * i / sr));
    return out;
}

void testWindows() {
    auto hann = scope::windowShape(scope::Window::Hann, 1025);
    CHECK_NEAR(hann[0], 0.0, 1e-12);
    CHECK_NEAR(hann[512], 1.0, 1e-12);
    CHECK_NEAR(hann[1024], 0.0, 1e-12);
    auto rect = scope::windowShape(scope::Window::Rectangular, 64);
    CHECK(rect.size() == 64 && rect[0] == 1.0 && rect[63] == 1.0);
    auto bh = scope::windowShape(scope::Window::BlackmanHarris, 1025);
    CHECK(bh[0] < 1e-4 && std::fabs(bh[512] - 1.0) < 1e-9);
    auto ham = scope::windowShape(scope::Window::Hamming, 1025);
    CHECK_NEAR(ham[0], 0.08, 1e-9);
    CHECK(std::string(scope::windowName(scope::Window::BlackmanHarris)) == "Blackman-Harris");
}

// the band a frequency is drawn in
int bandOf(const scope::Spectrum& s, double hz) {
    int best = 0;
    for (int b = 0; b < (int)s.levels().size(); b++)
        if (std::fabs(std::log(s.bandHz(b) / hz)) < std::fabs(std::log(s.bandHz(best) / hz))) best = b;
    return best;
}

void testSpectrum() {
    // a 1 kHz sine at half scale (-6 dB): its frequency and its level, for each size and window
    auto wave = sine(1000, 0.5, 40000);
    struct Case {
        scope::Window w;
        double tolDb;  // (scalloping: between bins a window reads a little low)
    } cases[] = {{scope::Window::Hann, 1.6},
                 {scope::Window::Hamming, 1.9},
                 {scope::Window::BlackmanHarris, 1.0},
                 {scope::Window::Rectangular, 4.0}};
    for (int size : {1024, 2048, 4096, 8192, 16384})
        for (auto& c : cases) {
            scope::Spectrum s;
            scope::SpectrumSettings set;
            set.fftSize = size;
            set.window = c.w;
            set.slope = 0;
            set.fall = 0;
            s.analyse(wave.data(), wave.size(), SR, set, 200, 0);
            double binHz = (double)SR / size;
            CHECK(std::fabs(s.peakHz() - 1000) < binHz / 2);
            double level = s.levels()[(size_t)bandOf(s, 1000)];
            CHECK(level < -6.0 + 0.6 && level > -6.0 - c.tolDb);
            CHECK(s.levels()[(size_t)bandOf(s, 8000)] < level - 30);  // (far from it: much quieter)
            if (!(level < -5.4 && level > -6.0 - c.tolDb))
                printf("  %d %s: %.2f dB\n", size, scope::windowName(c.w), level);
        }
    // the slope: 3 dB an octave about 1 kHz (4 kHz: two octaves up, +6 dB)
    auto high = sine(4000, 0.5, 8192);
    scope::Spectrum flat, tilted;
    scope::SpectrumSettings set;
    set.fall = 0;
    set.slope = 0;
    flat.analyse(high.data(), high.size(), SR, set, 200, 0);
    set.slope = 3;
    tilted.analyse(high.data(), high.size(), SR, set, 200, 0);
    int b = bandOf(flat, 4000);
    CHECK_NEAR(tilted.levels()[(size_t)b] - flat.levels()[(size_t)b], 3 * std::log2(tilted.bandHz(b) / 1000), 0.01);
    // the fall: silence after it, 0.1 s later, at 48 dB a second: 4.8 dB lower
    set.slope = 0;
    set.fall = 48;
    std::vector<float> silence(8192, 0.0f);
    scope::Spectrum falling;
    falling.analyse(high.data(), high.size(), SR, set, 200, 0);
    double before = falling.levels()[(size_t)b];
    falling.analyse(silence.data(), silence.size(), SR, set, 200, 0.1);
    CHECK_NEAR(falling.levels()[(size_t)b], before - 4.8, 0.01);
    // the floor
    set.fall = 0;
    set.floorDb = -60;
    falling.analyse(silence.data(), silence.size(), SR, set, 200, 0.1);
    bool floored = true;
    for (float v : falling.levels()) floored = floored && v == -60.0f;
    CHECK(floored);
    // fewer samples than the FFT: padded, still found
    auto little = sine(1000, 0.5, 600);
    scope::Spectrum s;
    set.fftSize = 4096;
    s.analyse(little.data(), little.size(), SR, set, 120, 0);
    CHECK(std::fabs(s.peakHz() - 1000) < 30);
    // the bands go up to 20 kHz (at 48 kHz) and never past the top of the audio range
    CHECK(s.bandHz(0) > 20 && s.bandHz(119) < 20000);
    scope::Spectrum low;
    low.analyse(little.data(), little.size(), 22050, set, 50, 0);
    CHECK(low.bandHz(49) < 11025);
}

void testColumns() {
    std::vector<float> ramp(1000);
    for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (float)i / 1000.0f;
    std::vector<scope::Column> cols;
    scope::columns(ramp.data(), ramp.size(), 10, cols);
    CHECK(cols.size() == 10);
    CHECK_NEAR(cols[0].lo, 0.0, 1e-6);
    CHECK_NEAR(cols[0].hi, 0.099, 1e-6);
    CHECK_NEAR(cols[9].hi, 0.999, 1e-6);
    // fewer samples than columns: each column one sample
    scope::columns(ramp.data(), 4, 8, cols);
    CHECK(cols.size() == 8 && cols[7].lo == cols[7].hi);
    scope::columns(nullptr, 0, 5, cols);
    CHECK(cols.size() == 5 && cols[0].hi == 0);
}

void testPitch() {
    auto a3 = sine(220, 0.6, 8192);
    auto p = scope::detectPitch(a3.data(), a3.size(), SR);
    CHECK(std::fabs(p.hz - 220) < 220 * 0.003 && p.clarity > 0.8);
    CHECK(scope::noteName(p.hz) == "A3");
    // a saw at 440 (all harmonics): still its fundamental, not an octave up
    std::vector<float> saw(8192);
    for (size_t i = 0; i < saw.size(); i++) saw[i] = (float)(0.5 * (2 * std::fmod(440.0 * i / SR, 1.0) - 1));
    p = scope::detectPitch(saw.data(), saw.size(), SR);
    CHECK(std::fabs(p.hz - 440) < 440 * 0.005);
    CHECK(scope::noteName(p.hz) == "A4");
    // low: 55 Hz
    auto a1 = sine(55, 0.5, 12000);
    p = scope::detectPitch(a1.data(), a1.size(), SR);
    CHECK(std::fabs(p.hz - 55) < 55 * 0.005 && scope::noteName(p.hz) == "A1");
    // noise and silence: none
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> noise(8192);
    for (auto& v : noise) v = u(rng);
    CHECK(scope::detectPitch(noise.data(), noise.size(), SR).hz == 0);
    std::vector<float> silence(8192, 0.0f);
    CHECK(scope::detectPitch(silence.data(), silence.size(), SR).hz == 0);
    CHECK(scope::detectPitch(a3.data(), 50, SR).hz == 0);  // (too little heard)
    CHECK(scope::noteName(261.63) == "C4" && scope::noteName(277.18) == "C#4" && scope::noteName(0).empty());
}

// pitch follow: pictures taken at odd moments of a held note start at the same point of its wave
void testFollow() {
    auto wave = sine(220, 0.5, 60000);
    for (size_t i = 0; i < wave.size(); i++)  // (a harmonic too: two rising crossings a period)
        wave[i] += (float)(0.3 * std::sin(2 * PI * 660 * i / SR + 1.0));
    double period = SR / 220.0;
    size_t length = (size_t)std::lround(2 * period);
    std::vector<float> previous;
    double firstPhase = -1;
    bool steady = true;
    for (size_t end = 9000; end < 60000; end += 3713) {  // (frames at odd moments)
        size_t start = scope::followStart(wave.data(), end, length, period, previous);
        CHECK(start + length <= end && start + (size_t)period + length >= end);
        CHECK(wave[start - 1] < 0 && wave[start] >= 0);
        double phase = std::fmod(start, period) / period;
        if (firstPhase < 0) firstPhase = phase;
        double off = std::fabs(phase - firstPhase);
        off = std::min(off, 1 - off);
        steady = steady && off < 0.01;
        previous.assign(wave.begin() + (long)start, wave.begin() + (long)(start + length));
    }
    CHECK(steady);
    // no period, or no crossing: the newest `length`
    CHECK(scope::followStart(wave.data(), 9000, 500, 0, {}) == 8500);
    std::vector<float> flat(4000, 0.25f);
    CHECK(scope::followStart(flat.data(), flat.size(), 1000, 200, {}) == 3000);
    CHECK(scope::followStart(flat.data(), 500, 1000, 200, {}) == 0);
}

// the master's audio, kept for the scope while it is on
void testMeteringKeeps() {
    audio::Metering m(SR, 2);
    std::vector<float> l(512), r(512);
    for (int i = 0; i < 512; i++) l[i] = (float)i / 512, r[i] = -(float)i / 512;
    std::vector<float> inter(1024);
    for (int i = 0; i < 512; i++) inter[2 * i] = l[i], inter[2 * i + 1] = r[i];
    std::vector<float> master, gotL, gotR;
    std::map<const Track*, std::vector<float>> tracks;
    m.levelsOn = false;
    m.loudnessOn = false;
    CHECK(!m.active());
    m.scopeOn = true;
    CHECK(m.active());  // (the audio thread copies for the scope alone)
    m.tapInterleaved(audio::MASTER, inter.data(), 2, 0, 512);
    m.tapSilence(audio::MASTER, 100);
    m.takePeaks(master, tracks);
    uint64_t heard = m.scopeLatest(612, gotL, gotR);
    CHECK(heard == 612 && gotL.size() == 612);
    CHECK(gotL[0] == l[0] && gotL[511] == l[511] && gotR[300] == r[300] && gotL[600] == 0.0f);
    m.scopeLatest(10, gotL, gotR);  // (the newest)
    CHECK(gotL.size() == 10 && gotL[9] == 0.0f);
    m.scopeLatest(100000, gotL, gotR);  // (no more than was heard)
    CHECK(gotL.size() == 612);
    // a single output: the same in both
    float mono[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    m.tapInterleaved(audio::MASTER, mono, 1, 0, 4);
    m.takePeaks(master, tracks);
    m.scopeLatest(4, gotL, gotR);
    CHECK(gotL[3] == 0.4f && gotR[3] == 0.4f);
    // off: nothing more kept; cleared: nothing
    m.scopeOn = false;
    m.tapInterleaved(audio::MASTER, inter.data(), 2, 0, 512);
    m.takePeaks(master, tracks);
    CHECK(m.scopeLatest(100000, gotL, gotR) == 616);
    m.clearScope();
    CHECK(m.scopeLatest(100000, gotL, gotR) == 0 && gotL.empty());
    // more than SCOPE_SECONDS: the newest kept
    m.scopeOn = true;
    std::vector<float> chunk(2 * 4800, 0.0f);
    for (int k = 0; k < 25; k++) {
        chunk[0] = (float)k;
        m.tapInterleaved(audio::MASTER, chunk.data(), 2, 0, 4800);
        m.takePeaks(master, tracks);
    }
    m.scopeLatest((size_t)(audio::Metering::SCOPE_SECONDS * SR), gotL, gotR);
    CHECK(gotL.size() == 96000 && gotL[0] == 5.0f);  // (the oldest kept: chunk 5 of 0…24)
}

}  // namespace

// what a picture costs at its most (a 16384-point spectrum of both channels, 120 bands each; pitch
// follow's detection), against the 16.7 ms a 60 Hz screen gives a frame
void testCost() {
    auto wave = sine(220, 0.5, 96000);
    scope::Spectrum l, r;
    scope::SpectrumSettings set;
    set.fftSize = 16384;
    auto t0 = std::chrono::steady_clock::now();
    const int N = 20;
    for (int i = 0; i < N; i++) {
        l.analyse(wave.data(), wave.size(), SR, set, 120, 0.016);
        r.analyse(wave.data(), wave.size(), SR, set, 120, 0.016);
    }
    double spectrumMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) scope::detectPitch(wave.data(), wave.size(), SR);
    double pitchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
    printf("a picture: spectrum 16384 x2 %.2f ms, pitch %.2f ms\n", spectrumMs, pitchMs);
#ifdef NDEBUG  // (the optimised build, as the program is: the sanitizers' unoptimised one is many times slower)
    CHECK(spectrumMs < 8 && pitchMs < 4);
#endif
}

int main() {
    testWindows();
    testCost();
    testSpectrum();
    testColumns();
    testPitch();
    testFollow();
    testMeteringKeeps();
    TEST_MAIN_END();
}
