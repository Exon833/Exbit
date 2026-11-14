// Tests of loudness (loudness.h), metering (metering.h), the CPU meter (cpu.h) and the metronome's
// clicks (metronome.h).
#include <cmath>
#include <complex>
#include <map>
#include <vector>

#include "audio/cpu.h"
#include "audio/loudness.h"
#include "audio/metering.h"
#include "audio/metronome.h"
#include "check.h"
#include "config.h"
#include "model/model.h"

static const double PI = 3.14159265358979323846;

// interleaved sine: amplitude per channel (0 = silent channel)
static std::vector<float> sine(double hz, double seconds, int sr, std::vector<double> amps) {
    int ch = (int)amps.size();
    size_t n = (size_t)(seconds * sr);
    std::vector<float> x(n * ch);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) x[i * ch + c] = (float)(amps[c] * std::sin(2 * PI * hz * i / sr));
    return x;
}

static double filterGainDb(int sr, double hz) {
    auto k = loudness::kWeighting(sr);
    std::complex<double> z = std::polar(1.0, -2 * PI * hz / sr), h = 1.0;  // z^-1
    for (auto& s : k) h *= (s.b0 + s.b1 * z + s.b2 * z * z) / (1.0 + s.a1 * z + s.a2 * z * z);
    return 20 * std::log10(std::abs(h));
}

static void testKWeighting() {
    // the BS.1770 pre-filter: about +0.7 dB at 1 kHz (what makes a 1 kHz sine read its level),
    // +4 dB up high, the high-pass taking the lows away
    for (int sr : {44100, 48000, 96000}) {
        CHECK_NEAR(filterGainDb(sr, 997), 0.691, 0.05);
        CHECK_NEAR(filterGainDb(sr, 10000), 4.0, 0.3);
        CHECK(filterGainDb(sr, 20) < -5);
    }
    auto k = loudness::kWeighting(48000);  // close to BS.1770's published coefficients at 48 kHz
    CHECK_NEAR(k[0].a1, -1.69065929318241, 1e-3);
    CHECK_NEAR(k[0].a2, 0.73248077421585, 1e-3);
    CHECK_NEAR(k[1].a1, -1.99004745483398, 1e-4);
    CHECK_NEAR(k[1].a2, 0.99007225036621, 1e-4);
}

static void testLufs() {
    for (int sr : {48000, 44100}) {
        // a 0 dBFS 1 kHz sine in one channel reads -3.01 LKFS (BS.1770's reference)
        loudness::LoudnessMeter one(sr, 2);
        auto x = sine(997, 5, sr, {1.0, 0.0});
        one.add(x.data(), (int)x.size() / 2);
        double m, s, i;
        one.read(m, s, i);
        CHECK_NEAR(i, -3.01, 0.1);
        CHECK_NEAR(m, -3.01, 0.1);
        CHECK_NEAR(s, -3.01, 0.1);
        // a -20 dBFS sine in both channels reads -20 LUFS
        loudness::LoudnessMeter two(sr, 2);
        double a = std::pow(10.0, -20.0 / 20);
        x = sine(997, 5, sr, {a, a});
        for (size_t at = 0; at < x.size(); at += 2 * 1000)  // (in pieces, as it arrives)
            two.add(x.data() + at, (int)std::min<size_t>(1000, (x.size() - at) / 2));
        two.read(m, s, i);
        CHECK_NEAR(i, -20.0, 0.1);
        CHECK_NEAR(m, -20.0, 0.1);
    }
    // nothing measured yet / silence: -inf
    loudness::LoudnessMeter none(48000, 2);
    double m, s, i;
    none.read(m, s, i);
    CHECK(std::isinf(m) && m < 0 && std::isinf(i));
    none.addSilence(48000);
    none.read(m, s, i);
    CHECK(std::isinf(m) && std::isinf(i));
    // gating: a long quiet part (> 10 LU below) doesn't pull the integrated loudness down
    loudness::LoudnessMeter g(48000, 2);
    auto loud = sine(997, 3, 48000, {0.1, 0.1});
    auto quiet = sine(997, 3, 48000, {0.001, 0.001});
    g.add(loud.data(), (int)loud.size() / 2);
    g.add(quiet.data(), (int)quiet.size() / 2);
    g.read(m, s, i);
    CHECK_NEAR(i, -20.0, 0.3);
    CHECK(m < -50);  // the last 400 ms are quiet
    CHECK_NEAR(loudness::toLufs(1.0), -0.691, 1e-12);
    CHECK(std::isinf(loudness::integrated({})));
}

static void testMetering() {
    audio::Metering mt(48000, 4, 4096, 16);
    Track t1, t2;
    std::vector<float> l(256, 0.5f), r(256, -0.25f);
    const float* ch[2] = {l.data(), r.data()};
    mt.tap(&t1, ch, 2, 2, 256);  // on outputs 3/4
    std::vector<float> master(256 * 4, 0.1f);
    master[7] = -0.9f;
    mt.tapInterleaved(audio::MASTER, master.data(), 4, 0, 256);
    mt.tapSilence(&t2, 256);
    std::vector<float> mp;
    std::map<const Track*, std::vector<float>> tp;
    mt.takePeaks(mp, tp);
    CHECK(mp.size() == 4 && std::fabs(mp[3] - 0.9f) < 1e-6 && std::fabs(mp[0] - 0.1f) < 1e-6);
    CHECK(tp.size() == 1 && tp[&t1].size() == 4);
    CHECK(tp[&t1][0] == 0 && std::fabs(tp[&t1][2] - 0.5f) < 1e-6 && std::fabs(tp[&t1][3] - 0.25f) < 1e-6);
    mt.takePeaks(mp, tp);
    CHECK(tp.empty() && mp[0] == 0);
    // the ring is full: taps are dropped, never waited for
    for (int k = 0; k < 20; k++) mt.tap(&t1, ch, 2, 0, 256);
    CHECK(mt.dropped > 0);
    mt.takePeaks(mp, tp);
    // LUFS of a track: measured in batches
    audio::Metering m2(48000, 2);
    double a = std::pow(10.0, -20.0 / 20);
    auto x = sine(997, 3, 48000, {a, a});
    std::vector<float> L(512), R(512);
    for (size_t at = 0; at + 512 <= x.size() / 2; at += 512) {
        for (int k = 0; k < 512; k++) L[k] = x[(at + k) * 2], R[k] = x[(at + k) * 2 + 1];
        const float* c2[2] = {L.data(), R.data()};
        m2.tap(&t1, c2, 2, 0, 512);
        if (at % (512 * 16) == 0) {
            m2.takePeaks(mp, tp);
            m2.updateLoudness();
        }
    }
    m2.takePeaks(mp, tp);
    m2.updateLoudness();
    double mo, st, in;
    m2.read(&t1, mo, st, in);
    CHECK_NEAR(in, -20.0, 0.1);
    m2.read(&t2, mo, st, in);  // never heard
    CHECK(std::isinf(mo) && std::isinf(in));
    m2.read(audio::MASTER, mo, st, in);
    CHECK(std::isinf(in));
    m2.forgetGone({});
    m2.read(&t1, mo, st, in);
    CHECK(std::isinf(in));
    // switched off: nothing queued for loudness
    m2.loudnessOn = false;
    m2.levelsOn = false;
    CHECK(!m2.active());
}

static void testCpu() {
    audio::CpuMeter cpu;
    double cur, avg;
    CHECK(!cpu.read(5, cur, avg));
    cpu.add(0.01, 0.02);
    cpu.add(0.01, 0.02);
    CHECK(cpu.read(5, cur, avg));
    CHECK_NEAR(cur, 0.5, 1e-12);
    CHECK_NEAR(avg, 0.5, 1e-12);
    cpu.add(0.03, 0.04);
    CHECK(cpu.read(5, cur, avg));
    CHECK_NEAR(cur, 0.75, 1e-12);
    CHECK_NEAR(avg, 0.5 + 0.04 / 5 * 0.25, 1e-12);
    cpu.add(0.5, 10);  // a long stretch: the average follows at once (weight 1)
    CHECK(cpu.read(5, cur, avg));
    CHECK_NEAR(avg, 0.05, 1e-12);
    CHECK(!cpu.read(5, cur, avg));
    cpu.reset();
    CHECK(cpu.average < 0);
}

static void testClicks() {
    metronome::Click c;  // 1 ms attack, 10 ms sustain, 50 ms release at -8 dB
    auto x = metronome::renderClick(c, 48000);
    CHECK(x.size() == 48 + 480 + 2400);
    double level = std::pow(10.0, -8.0 / 20);
    CHECK_NEAR(x[0], 0.0, 1e-9);  // rises from silence
    CHECK_NEAR(x[12], std::sin(2 * PI * 1000 * 12 / 48000.0) * level * 12 / 48, 1e-6);  // 1000 Hz: 48 frames a period
    CHECK_NEAR(x[48 + 12], level, 1e-6);  // sustain at its level (a sine's peak)
    CHECK_NEAR(x.back(), 0.0, 1e-6);  // falls back to silence
    c.waveform = "square";
    x = metronome::renderClick(c, 48000);
    CHECK_NEAR(x[100], 0.6 * level, 1e-6);
    c.waveform = "noise";
    auto n1 = metronome::renderClick(c, 48000), n2 = metronome::renderClick(c, 48000);
    CHECK(n1 == n2 && n1[100] != 0);  // the same noise every time
    c.attackMs = c.sustainMs = c.releaseMs = 0;
    CHECK(metronome::renderClick(c, 48000).empty());
    auto clicks = metronome::renderClicks(metronome::MetronomeSettings(), 48000);
    CHECK(clicks.bar.size() == 48 + 720 + 3360 && clicks.beat.size() == 2928);
    // placement: bars every 4 beats of 24000 frames (120 BPM); beats before `earliest` are left out
    Timing timing;
    std::vector<float> out(200 * 2, 0.0f);
    metronome::addClicks(out.data(), 2, 200, 96000 - 100, clicks, timing, 48000);
    CHECK(out[99 * 2] == 0 && out[101 * 2] == clicks.bar[1] && out[101 * 2 + 1] == clicks.bar[1]);
    std::fill(out.begin(), out.end(), 0.0f);
    metronome::addClicks(out.data(), 2, 200, 24000 + 1000, clicks, timing, 48000, 0.5);  // a click from before
    CHECK_NEAR(out[0], clicks.beat[1000] * 0.5, 1e-7);
    std::fill(out.begin(), out.end(), 0.0f);
    metronome::addClicks(out.data(), 2, 200, 24000 + 1000, clicks, timing, 48000, 1.0, 24001);
    CHECK(out[0] == 0);
    timing.beatsPerBar = 3;
    std::fill(out.begin(), out.end(), 0.0f);
    metronome::addClicks(out.data(), 2, 200, 72000 - 100, clicks, timing, 48000);
    CHECK(out[101 * 2] == clicks.bar[1]);  // beat 3 starts a bar in 3/4
}

int main() {
    testKWeighting();
    testLufs();
    testMetering();
    testCpu();
    testClicks();
    TEST_MAIN_END();
}
