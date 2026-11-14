// Tests of the time-stretcher / pitch-shifter (audio/stretch.h): lengths, pitch by the FFT peak,
// near-identity at unity, the stereo image, reversing and the cache.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/fft.h"
#include "audio/stretch.h"
#include "check.h"

static const double PI = 3.14159265358979323846;
static const int SR = 44100;

static std::shared_ptr<AudioBuffer> stereoSines(double hzL, double hzR, double seconds, double amp = 0.5) {
    auto b = std::make_shared<AudioBuffer>();
    size_t n = (size_t)(seconds * SR);
    b->samples.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        b->samples[2 * i] = (float)(amp * std::sin(2 * PI * hzL * i / SR));
        b->samples[2 * i + 1] = (float)(amp * std::sin(2 * PI * hzR * i / SR));
    }
    return b;
}

// the strongest frequency of channel `c` in the middle of `a` (Hann window, 32768-point FFT, parabolic peak)
static double peakHz(const AudioBuffer& a, int c) {
    const int N = 32768;
    fft::Plan p(N);
    std::vector<fft::cd> x(N);
    int64_t start = std::max<int64_t>(0, a.frames() / 2 - N / 2);
    for (int i = 0; i < N; ++i) {
        int64_t j = start + i;
        double v = j < a.frames() ? a.samples[(size_t)(2 * j + c)] : 0.0;
        x[i] = v * (0.5 - 0.5 * std::cos(2 * PI * i / N));
    }
    p.forward(x.data());
    int best = 1;
    for (int k = 1; k < N / 2; ++k)
        if (std::abs(x[k]) > std::abs(x[best])) best = k;
    double l = std::log(std::abs(x[best - 1]) + 1e-30), m = std::log(std::abs(x[best]) + 1e-30),
           r = std::log(std::abs(x[best + 1]) + 1e-30);
    double d = 0.5 * (l - r) / (l - 2 * m + r);
    return (best + d) * SR / N;
}

static double rms(const AudioBuffer& a, int c, int64_t from, int64_t to) {
    double s = 0;
    for (int64_t i = from; i < to; ++i) s += (double)a.samples[(size_t)(2 * i + c)] * a.samples[(size_t)(2 * i + c)];
    return std::sqrt(s / std::max<int64_t>(1, to - from));
}

int main() {
    auto src = stereoSines(440, 660, 2.0);
    AudioPtr s = src;
    const int64_t n = src->frames();

    // unity: the very same audio
    CHECK(stretch::process(s, 1.0, 0.0) == s);
    CHECK(stretch::stretched(s, 1.0, 0.0) == s);

    // lengths: round(frames * factor), at the same pitch
    for (double f : {0.5, 0.75, 1.3333, 1.5, 2.0, 3.0}) {
        AudioPtr r = stretch::process(s, f, 0.0);
        CHECK(r->frames() == (int64_t)std::nearbyint(n * f));
        CHECK_NEAR(peakHz(*r, 0), 440.0, 0.5);
        CHECK_NEAR(peakHz(*r, 1), 660.0, 0.5);  // (each channel keeps its own sound: the stereo image)
        // level held in the middle (no dips from the overlap-add weighting)
        int64_t a = r->frames() / 4, e = 3 * r->frames() / 4;
        CHECK_NEAR(rms(*r, 0, a, e), 0.5 / std::sqrt(2.0), 0.03);
    }

    // pitch: semitones and cents at the same length
    for (double st : {12.0, -12.0, 7.0, -5.0, 0.5, -0.25, 3.33}) {
        AudioPtr r = stretch::process(s, 1.0, st);
        CHECK(r->frames() == n);
        double want = 440.0 * std::pow(2.0, st / 12);
        double got = peakHz(*r, 0);
        CHECK_NEAR(1200 * std::log2(got / want), 0.0, 2.0);  // within 2 cents
        CHECK_NEAR(1200 * std::log2(peakHz(*r, 1) / (660.0 * std::pow(2.0, st / 12))), 0.0, 2.0);
    }
    // both at once
    {
        AudioPtr r = stretch::process(s, 1.5, -3.0);
        CHECK(r->frames() == (int64_t)std::nearbyint(n * 1.5));
        CHECK_NEAR(1200 * std::log2(peakHz(*r, 0) / (440.0 * std::pow(2.0, -3.0 / 12))), 0.0, 2.0);
    }

    // just off unity the vocoder runs (hop 512 in and out) and gives the audio back almost exactly (not
    // bit-exactly: the phases advance by the factor while the frames land on whole samples)
    {
        double f = 1.0 + 2e-6;
        std::vector<float> r = stretch::timeStretch(src->samples.data(), n, 2, f);
        CHECK((int64_t)(r.size() / 2) == (int64_t)std::nearbyint(n * f));
        double err = 0, sig = 0;
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                double d = r[(size_t)(2 * i + c)] - src->samples[(size_t)(2 * i + c)];
                err += d * d;
                sig += (double)src->samples[(size_t)(2 * i + c)] * src->samples[(size_t)(2 * i + c)];
            }
        double snr = 10 * std::log10(sig / std::max(err, 1e-30));
        printf("near-unity SNR %.1f dB\n", snr);
        CHECK(snr > 35);
    }

    // a mono signal and a 3-channel one through the generic function
    {
        std::vector<float> mono(SR);
        for (int i = 0; i < SR; ++i) mono[i] = (float)std::sin(2 * PI * 1000 * i / SR);
        std::vector<float> r = stretch::stretch(mono.data(), SR, 1, 0.8, 2.0);
        CHECK((int64_t)r.size() == (int64_t)std::nearbyint(SR * 0.8));
        std::vector<float> three(SR * 3);
        for (int i = 0; i < SR; ++i)
            for (int c = 0; c < 3; ++c) three[3 * i + c] = (float)(0.25 * std::sin(2 * PI * 300 * (c + 1) * i / SR));
        std::vector<float> r3 = stretch::stretch(three.data(), SR, 3, 1.25, 0.0);
        CHECK(r3.size() == (size_t)std::nearbyint(SR * 1.25) * 3);
        for (int c = 0; c < 3; ++c) {  // each channel at its own level
            double e = 0;
            size_t m = r3.size() / 3;
            for (size_t i = m / 4; i < 3 * m / 4; ++i) e += r3[3 * i + c] * r3[3 * i + c];
            CHECK_NEAR(std::sqrt(e / (m / 2)), 0.25 / std::sqrt(2.0), 0.02);
        }
    }

    // short and empty audio
    {
        auto tiny = std::make_shared<AudioBuffer>();
        tiny->samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
        AudioPtr r = stretch::process(tiny, 2.0, 0.0);
        CHECK(r->frames() == 6);
        auto empty = std::make_shared<AudioBuffer>();
        CHECK(stretch::process(empty, 2.0, 5.0)->frames() == 0);
    }

    // reverse, and the cache
    {
        AudioPtr rev = stretch::stretched(s, 1.0, 0.0, true);
        CHECK(rev != s && rev->frames() == n);
        CHECK(rev->samples[0] == src->samples[(size_t)(2 * (n - 1))]);
        CHECK(rev->samples[1] == src->samples[(size_t)(2 * (n - 1) + 1)]);
        AudioPtr a = stretch::stretched(s, 1.25, 1.0);
        AudioPtr b = stretch::stretched(s, 1.25, 1.0);
        CHECK(a == b);
        AudioPtr c = stretch::stretched(s, 1.25, 1.0, true);
        CHECK(c != a && c->frames() == a->frames());
        CHECK(c->samples[0] == a->samples[a->samples.size() - 2]);
    }

    // speed: a minute of stereo stretched by 1.1 (a 4-minute song takes about 2 s at -O2)
    {
        auto song = stereoSines(220, 330, 60.0, 0.3);
        auto t0 = std::chrono::steady_clock::now();
        AudioPtr r = stretch::process(song, 1.1, 0.0);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("1 minute of stereo stretched in %.2f s\n", secs);
        CHECK(r->frames() == (int64_t)std::nearbyint(song->frames() * 1.1));
    }
    TEST_MAIN_END();
}
