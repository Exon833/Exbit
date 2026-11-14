// Tests of our MP3 and Ogg Vorbis encoders (audio/mp3_encoder.h, audio/vorbis_encoder.h), through our
// own decoders (readSoundFile): exact lengths, SNR on music-like signals (chords, drums, noise,
// clicks, a mix), every sample rate, mono and stereo, the spectrum kept (no band holes), no
// pre-echo before transients, edge cases, cancelling.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "audio/codecs.h"
#include "audio/fft.h"
#include "audio/mp3_encoder.h"
#include "audio/sound_file.h"
#include "audio/vorbis_encoder.h"
#include "check.h"

static const double PI = 3.14159265358979323846;
static const std::string kDir = "build/test-tmp/encoders";
static std::string tmp(const std::string& name) { return kDir + "/" + name; }

// ---------------------------------------------------------------- test signals (stereo, interleaved)
enum Kind { NOISE, CHORDS, DRUMS, CLICKS, MIX };

static std::vector<float> makeSignal(Kind kind, int sr, double seconds) {
    int n = (int)(sr * seconds);
    std::vector<float> x(2 * (size_t)n, 0.f);
    std::mt19937 rng(1234);
    std::normal_distribution<double> nd(0, 1);
    auto chords = [&](int i, double& l, double& r) {
        const double roots[] = {261.63, 220.0, 174.61, 196.0};
        double t = (double)i / sr, tt = std::fmod(t, 0.5);
        int k = (int)(t / 0.5) % 4;
        double env = std::exp(-3 * tt) * std::min(1.0, tt * 200);
        l = r = 0;
        for (double m : {1.0, 1.25, 1.5, 2.0})
            for (int h = 1; h <= 8; ++h) {
                double f = roots[k] * m * h;
                if (f > sr / 2) continue;
                double a = 0.04 / h, ph = 2 * PI * f * t;
                l += a * std::sin(ph);
                r += a * std::sin(ph + 0.3 * h);
            }
        l *= env;
        r *= env;
    };
    auto drums = [&](int i) {
        double t = (double)i / sr, beat = std::fmod(t, 0.25);
        int b = (int)(t / 0.25) % 4;
        double v = b % 2 == 0 ? 0.8 * std::sin(2 * PI * (50 + 100 * std::exp(-beat * 30)) * beat) * std::exp(-beat * 12)
                              : 0.5 * nd(rng) * std::exp(-beat * 25);
        double hh = std::fmod(t, 0.125);
        return 0.5 * (v + 0.15 * nd(rng) * std::exp(-hh * 120));
    };
    for (int i = 0; i < n; ++i) {
        double l = 0, r = 0;
        switch (kind) {
        case NOISE:
            l = 0.2 * nd(rng);
            r = 0.2 * nd(rng);
            break;
        case CHORDS:
            chords(i, l, r);
            break;
        case DRUMS:
            l = drums(i);
            r = 0.9 * l;
            break;
        case CLICKS: {
            double c = std::fmod((double)i / sr, 0.3);
            l = r = c < 0.02 ? 0.25 * nd(rng) * std::exp(-c * 300) : 0.0;
            break;
        }
        case MIX: {
            chords(i, l, r);
            double d = drums(i), bass = 0.2 * std::sin(2 * PI * 55 * i / sr);
            l = 0.7 * l + 0.5 * d + bass;
            r = 0.7 * r + 0.45 * d + bass;
            break;
        }
        }
        x[2 * (size_t)i] = (float)l;
        x[2 * (size_t)i + 1] = (float)r;
    }
    return x;
}

static std::vector<float> channelOf(const std::vector<float>& x, int c) {
    std::vector<float> m(x.size() / 2);
    for (size_t i = 0; i < m.size(); ++i) m[i] = x[2 * i + c];
    return m;
}

// SNR over the band below `hz` (2048-point Hann STFT), all channels
static double bandSnr(const std::vector<float>& a, const std::vector<float>& b, int ch, int sr, double hz) {
    const int N = 2048;
    fft::Plan p(N);
    std::vector<fft::cd> X(N), Y(N);
    size_t frames = a.size() / ch;
    int kmax = std::min(N / 2, (int)(hz / sr * N));
    double s = 0, e = 0;
    for (int c = 0; c < ch; ++c)
        for (size_t t = 0; t + N <= frames; t += N / 2) {
            for (int i = 0; i < N; ++i) {
                double w = 0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / N);
                size_t j = (t + i) * ch + c;
                X[i] = a[j] * w;
                Y[i] = (j < b.size() ? b[j] : 0.0f) * w;
            }
            p.forward(X.data());
            p.forward(Y.data());
            for (int k = 1; k < kmax; ++k) {
                s += std::norm(X[k]);
                e += std::norm(X[k] - Y[k]);
            }
        }
    return 10 * std::log10(s / std::max(e, 1e-30));
}

// the largest difference (dB) of third-octave band energies between a and b below `hz`, over bands
// within 40 dB of the strongest (band holes or boosts would show here)
static double spectrumDeviation(const std::vector<float>& a, const std::vector<float>& b, int ch, int sr, double hz) {
    const int N = 4096;
    fft::Plan p(N);
    std::vector<fft::cd> X(N), Y(N);
    std::vector<double> ea(64, 0.0), eb(64, 0.0);
    size_t frames = a.size() / ch;
    for (int c = 0; c < ch; ++c)
        for (size_t t = 0; t + N <= frames; t += N / 2) {
            for (int i = 0; i < N; ++i) {
                double w = 0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / N);
                size_t j = (t + i) * ch + c;
                X[i] = a[j] * w;
                Y[i] = (j < b.size() ? b[j] : 0.0f) * w;
            }
            p.forward(X.data());
            p.forward(Y.data());
            for (int k = 1; k < N / 2; ++k) {
                double f = (double)k * sr / N;
                if (f < 40 || f > hz) continue;
                int band = (int)(3 * std::log2(f / 40));
                ea[band] += std::norm(X[k]);
                eb[band] += std::norm(Y[k]);
            }
        }
    double top = *std::max_element(ea.begin(), ea.end()), worst = 0;
    for (int i = 0; i < 64; ++i)
        if (ea[i] > top * 1e-4) worst = std::max(worst, std::fabs(10 * std::log10((eb[i] + 1e-30) / ea[i])));
    return worst;
}

// how far (dB) below the clicks the decoded signal is in the 2..25 ms before each (CLICKS: every 0.3 s)
static double preEchoBelow(const std::vector<float>& d, int sr, double seconds) {
    double worst = 1e9;
    for (double t = 0.3; t < seconds - 0.05; t += 0.3) {
        int c0 = (int)std::lround(t * sr);
        double pre = 0, click = 0;
        int a = c0 - (int)(0.025 * sr), b = c0 - (int)(0.002 * sr), e = c0 + (int)(0.01 * sr);
        for (int i = a; i < b; ++i) pre += d[2 * (size_t)i] * d[2 * (size_t)i];
        for (int i = c0; i < e; ++i) click += d[2 * (size_t)i] * d[2 * (size_t)i];
        pre /= (b - a);
        click /= (e - c0);
        worst = std::min(worst, 10 * std::log10(click / std::max(pre, 1e-30)));
    }
    return worst;
}

static bool readBack(const std::string& path, SoundData& d) {
    std::string err;
    bool ok = readSoundFile(path, d, err);
    if (!ok) fprintf(stderr, "reading %s: %s\n", path.c_str(), err.c_str());
    return ok;
}

static long fileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

// ---------------------------------------------------------------- MP3
static void testMp3() {
    std::string err;
    // every sample rate MP3 has (MPEG-1, 2 and 2.5): exact length, the band below the lowpass kept
    for (int sr : {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000}) {
        auto x = makeSignal(MIX, sr, 1.0);
        size_t n = x.size() / 2;
        std::string path = tmp("mix_" + std::to_string(sr) + ".mp3");
        CHECK(encodeMp3(path, x.data(), n, 2, sr, 320, err));
        SoundData d;
        CHECK(readBack(path, d));
        CHECK(d.sampleRate == sr && d.channels == 2);
        CHECK(d.frames() == n);
        bool lsf = sr < 32000;
        double snr = bandSnr(x, d.samples, 2, sr, lsf ? 0.35 * sr : 15000);
        printf("mp3 %5d Hz %d kbps: SNR %.1f dB\n", sr, mp3EncodedBitrate(sr, 320), snr);
        CHECK(snr > (lsf ? 18 : 35));
    }
    // music-like signals at 320 kbps
    struct Want { Kind k; const char* name; double snr; };
    for (Want w : {Want{CHORDS, "chords", 45}, Want{DRUMS, "drums", 30}, Want{NOISE, "noise", 15},
                   Want{CLICKS, "clicks", 35}, Want{MIX, "mix", 38}}) {
        auto x = makeSignal(w.k, 44100, 3.0);
        size_t n = x.size() / 2;
        std::string path = tmp(std::string(w.name) + ".mp3");
        CHECK(encodeMp3(path, x.data(), n, 2, 44100, 320, err));
        SoundData d;
        CHECK(readBack(path, d) && d.frames() == n);
        double snr = bandSnr(x, d.samples, 2, 44100, 15000);
        double dev = spectrumDeviation(x, d.samples, 2, 44100, 15000);
        printf("mp3 320 kbps %-6s: SNR (below 15 kHz) %.1f dB, spectrum within %.2f dB", w.name, snr, dev);
        CHECK(snr > w.snr);
        CHECK(dev < 1.0);
        if (w.k == CLICKS) {
            double pe = preEchoBelow(d.samples, 44100, 3.0);
            printf(", pre-echo %.1f dB below the clicks", pe);
            CHECK(pe > 35);
        }
        printf("\n");
        // a constant bitrate: the file is 320 kbps (+ the Info frame and the frames of delay)
        double kbps = fileSize(path) * 8.0 / 3.0 / 1000;
        CHECK(kbps > 320 && kbps < 335);
    }
    // bitrates: more bits, less noise
    {
        auto x = makeSignal(MIX, 44100, 2.0);
        size_t n = x.size() / 2;
        double last = 0;
        for (int kbps : {96, 128, 192, 256, 320}) {
            std::vector<uint8_t> out;
            CHECK(encodeMp3ToMemory(out, x.data(), n, 2, 44100, kbps, err));
            SoundData d;
            CHECK(decodeMp3(out.data(), out.size(), d, err) && d.frames() == n);
            double snr = bandSnr(x, d.samples, 2, 44100, 12000);
            CHECK(snr > last);
            last = snr;
            double size = (double)out.size() * 8 / 2.0 / 1000;
            CHECK(size > kbps && size < kbps * 1.06);
        }
        CHECK(mp3EncodedBitrate(44100, 300) == 320 && mp3EncodedBitrate(22050, 320) == 160);
    }
    // mono
    {
        auto x = channelOf(makeSignal(CHORDS, 48000, 1.0), 0);
        CHECK(encodeMp3(tmp("mono.mp3"), x.data(), x.size(), 1, 48000, 320, err));
        SoundData d;
        CHECK(readBack(tmp("mono.mp3"), d));
        CHECK(d.channels == 1 && d.sampleRate == 48000 && d.frames() == x.size());
        CHECK(bandSnr(x, d.samples, 1, 48000, 15000) > 45);
    }
    // rates MP3 doesn't have are resampled to one it has; more than two channels: the first two
    {
        int n = 96000;
        std::vector<float> x((size_t)n * 3);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) x[(size_t)i * 3 + c] = (float)(0.3 * std::sin(2 * PI * 1000 * (c + 1) * i / 96000.0));
        CHECK(mp3EncodedRate(96000) == 48000 && mp3EncodedRate(88200) == 44100 && mp3EncodedRate(37800) == 44100);
        CHECK(encodeMp3(tmp("r96.mp3"), x.data(), n, 3, 96000, 320, err));
        SoundData d;
        CHECK(readBack(tmp("r96.mp3"), d));
        CHECK(d.sampleRate == 48000 && d.channels == 2 && d.frames() == (size_t)n / 2);
        std::vector<float> want((size_t)n);
        for (int i = 0; i < n / 2; ++i) {
            want[2 * (size_t)i] = (float)(0.3 * std::sin(2 * PI * 1000 * i / 48000.0));
            want[2 * (size_t)i + 1] = (float)(0.3 * std::sin(2 * PI * 2000 * i / 48000.0));
        }
        CHECK(bandSnr(want, d.samples, 2, 48000, 15000) > 40);
    }
    // edge cases: nothing, a few samples, a frame's worth, silence, overloud, NaN
    for (size_t n : {(size_t)0, (size_t)1, (size_t)100, (size_t)1152, (size_t)1153, (size_t)4000}) {
        std::vector<float> x(n * 2);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(0.5 * std::sin(i * 0.01));
        std::vector<uint8_t> out;
        CHECK(encodeMp3ToMemory(out, x.data(), n, 2, 44100, 320, err));
        SoundData d;
        CHECK(decodeMp3(out.data(), out.size(), d, err) && d.frames() == n);
    }
    {
        std::vector<float> x(44100 * 2, 0.0f);
        std::vector<uint8_t> out;
        CHECK(encodeMp3ToMemory(out, x.data(), 44100, 2, 44100, 320, err));
        SoundData d;
        CHECK(decodeMp3(out.data(), out.size(), d, err) && d.frames() == 44100);
        double peak = 0;
        for (float v : d.samples) peak = std::max(peak, (double)std::fabs(v));
        CHECK(peak == 0);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (i / 200) % 2 ? 1.5f : -1.5f;  // square wave over full scale
        x[1000] = NAN;
        CHECK(encodeMp3ToMemory(out, x.data(), 44100, 2, 44100, 320, err));
        CHECK(decodeMp3(out.data(), out.size(), d, err) && d.frames() == 44100);
        CHECK(std::fabs(d.samples[44100]) > 0.9);
    }
    // the LAME-style tag: encoder delay 576, padding to the end
    {
        auto x = makeSignal(MIX, 44100, 0.5);
        std::vector<uint8_t> out;
        CHECK(encodeMp3ToMemory(out, x.data(), x.size() / 2, 2, 44100, 320, err));
        CHECK(out.size() > 200 && out[0] == 0xFF && (out[1] & 0xFE) == 0xFA);
        CHECK(memcmp(out.data() + 36, "Info", 4) == 0 && memcmp(out.data() + 156, "LAME", 4) == 0);
        int delay = out[156 + 21] << 4 | out[156 + 22] >> 4, pad = (out[156 + 22] & 15) << 8 | out[156 + 23];
        CHECK(delay == 576);
        size_t frames = (out[44] << 24 | out[45] << 16 | out[46] << 8 | out[47]);
        CHECK(frames * 1152 == x.size() / 2 + delay + pad);
    }
    // cancelling: no file
    {
        auto x = makeSignal(MIX, 44100, 3.0);
        int calls = 0;
        remove(tmp("cancel.mp3").c_str());
        CHECK(!encodeMp3(tmp("cancel.mp3"), x.data(), x.size() / 2, 2, 44100, 320, err, [&](double f) {
            ++calls;
            return f < 0.3;
        }));
        CHECK(err == "cancelled" && calls > 1 && fileSize(tmp("cancel.mp3")) < 0);
    }
    // speed: 30 s of stereo
    {
        auto x = makeSignal(MIX, 44100, 30.0);
        std::vector<uint8_t> out;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(encodeMp3ToMemory(out, x.data(), x.size() / 2, 2, 44100, 320, err));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("mp3: 30 s of stereo encoded in %.2f s (%.0fx real time)\n", secs, 30 / secs);
    }
}

// ---------------------------------------------------------------- Ogg Vorbis
static void testVorbis() {
    std::string err;
    // any sample rate: exact length, SNR
    for (int sr : {44100, 48000, 32000, 22050, 16000, 8000, 96000}) {
        auto x = makeSignal(MIX, sr, 1.0);
        size_t n = x.size() / 2;
        std::string path = tmp("mix_" + std::to_string(sr) + ".ogg");
        CHECK(encodeVorbis(path, x.data(), n, 2, sr, 6, err));
        SoundData d;
        CHECK(readBack(path, d));
        CHECK(d.sampleRate == sr && d.channels == 2);
        CHECK(d.frames() == n);
        double snr = bandSnr(x, d.samples, 2, sr, std::min(15000.0, 0.4 * sr));
        printf("ogg %5d Hz q6: SNR %.1f dB, %.0f kbps\n", sr, snr, fileSize(path) * 8.0 / 1000);
        CHECK(snr > 22);
    }
    struct Want { Kind k; const char* name; double snr; };
    for (Want w : {Want{CHORDS, "chords", 24}, Want{DRUMS, "drums", 18}, Want{NOISE, "noise", 11},
                   Want{CLICKS, "clicks", 14}, Want{MIX, "mix", 26}}) {
        auto x = makeSignal(w.k, 44100, 3.0);
        size_t n = x.size() / 2;
        std::string path = tmp(std::string(w.name) + ".ogg");
        CHECK(encodeVorbis(path, x.data(), n, 2, 44100, 6, err));
        SoundData d;
        CHECK(readBack(path, d) && d.frames() == n);
        double snr = bandSnr(x, d.samples, 2, 44100, 15000);
        double dev = spectrumDeviation(x, d.samples, 2, 44100, 15000);
        printf("ogg q6 %-6s: %.0f kbps, SNR (below 15 kHz) %.1f dB, spectrum within %.2f dB", w.name,
               fileSize(path) * 8.0 / 3.0 / 1000, snr, dev);
        CHECK(snr > w.snr);
        CHECK(dev < 1.5);
        if (w.k == CLICKS) {
            double pe = preEchoBelow(d.samples, 44100, 3.0);
            printf(", pre-echo %.1f dB below the clicks", pe);
            CHECK(pe > 30);
        }
        printf("\n");
    }
    // quality: more, bigger and cleaner
    {
        auto x = makeSignal(MIX, 44100, 2.0);
        size_t n = x.size() / 2, lastSize = 0;
        double lastSnr = 0;
        for (double q : {0.0, 3.0, 6.0, 10.0}) {
            std::vector<uint8_t> out;
            CHECK(encodeVorbisToMemory(out, x.data(), n, 2, 44100, q, err));
            SoundData d;
            CHECK(decodeVorbis(out.data(), out.size(), d, err) && d.frames() == n);
            double snr = bandSnr(x, d.samples, 2, 44100, 12000);
            CHECK(out.size() > lastSize && snr > lastSnr);
            lastSize = out.size();
            lastSnr = snr;
        }
    }
    // mono
    {
        auto x = channelOf(makeSignal(CHORDS, 44100, 1.0), 1);
        CHECK(encodeVorbis(tmp("mono.ogg"), x.data(), x.size(), 1, 44100, 6, err));
        SoundData d;
        CHECK(readBack(tmp("mono.ogg"), d));
        CHECK(d.channels == 1 && d.frames() == x.size());
        CHECK(bandSnr(x, d.samples, 1, 44100, 15000) > 24);
    }
    // one channel silent, the other not (an unused channel next to a coupled one)
    {
        auto x = makeSignal(CHORDS, 44100, 1.0);
        for (size_t i = 1; i < x.size(); i += 2) x[i] = 0;
        std::vector<uint8_t> out;
        CHECK(encodeVorbisToMemory(out, x.data(), x.size() / 2, 2, 44100, 6, err));
        SoundData d;
        CHECK(decodeVorbis(out.data(), out.size(), d, err) && d.frames() == x.size() / 2);
        double right = 0;
        for (size_t i = 1; i < d.samples.size(); i += 2) right = std::max(right, (double)std::fabs(d.samples[i]));
        CHECK(right < 1e-3);
        CHECK(bandSnr(x, d.samples, 2, 44100, 15000) > 24);
    }
    // edge cases
    for (size_t n : {(size_t)0, (size_t)1, (size_t)100, (size_t)1024, (size_t)1025, (size_t)5000}) {
        std::vector<float> x(n * 2);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(0.5 * std::sin(i * 0.01));
        std::vector<uint8_t> out;
        CHECK(encodeVorbisToMemory(out, x.data(), n, 2, 44100, 6, err));
        SoundData d;
        CHECK(decodeVorbis(out.data(), out.size(), d, err));
        CHECK(d.frames() == n);
    }
    {
        std::vector<float> x(44100 * 2, 0.0f);
        std::vector<uint8_t> out;
        CHECK(encodeVorbisToMemory(out, x.data(), 44100, 2, 44100, 6, err));
        SoundData d;
        CHECK(decodeVorbis(out.data(), out.size(), d, err) && d.frames() == 44100);
        double peak = 0;
        for (float v : d.samples) peak = std::max(peak, (double)std::fabs(v));
        CHECK(peak == 0);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (i / 200) % 2 ? 1.5f : -1.5f;
        x[1000] = NAN;
        CHECK(encodeVorbisToMemory(out, x.data(), 44100, 2, 44100, 6, err));
        CHECK(decodeVorbis(out.data(), out.size(), d, err) && d.frames() == 44100);
        CHECK(std::fabs(d.samples[44100]) > 0.9);
    }
    // the stream: Ogg pages from the identification header on; the last page ends the stream at the length
    {
        auto x = makeSignal(MIX, 44100, 1.0);
        std::vector<uint8_t> out;
        CHECK(encodeVorbisToMemory(out, x.data(), x.size() / 2, 2, 44100, 6, err));
        CHECK(looksLikeVorbis(out.data(), out.size()));
        size_t pos = 0, last = 0;
        int pages = 0;
        while (pos + 27 <= out.size() && memcmp(out.data() + pos, "OggS", 4) == 0) {
            last = pos;
            int nseg = out[pos + 26];
            size_t body = 0;
            for (int i = 0; i < nseg; ++i) body += out[pos + 27 + i];
            pos += 27 + nseg + body;
            ++pages;
        }
        CHECK(pos == out.size() && pages > 3);
        CHECK(out[5] == 2 && (out[last + 5] & 4));  // BOS first, EOS last
        int64_t g = 0;
        for (int i = 7; i >= 0; --i) g = g << 8 | out[last + 6 + i];
        CHECK(g == (int64_t)(x.size() / 2));
    }
    // cancelling: no file
    {
        auto x = makeSignal(MIX, 44100, 3.0);
        remove(tmp("cancel.ogg").c_str());
        CHECK(!encodeVorbis(tmp("cancel.ogg"), x.data(), x.size() / 2, 2, 44100, 6, err, [](double f) { return f < 0.6; }));
        CHECK(err == "cancelled" && fileSize(tmp("cancel.ogg")) < 0);
    }
    // speed: 30 s of stereo
    {
        auto x = makeSignal(MIX, 44100, 30.0);
        std::vector<uint8_t> out;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(encodeVorbisToMemory(out, x.data(), x.size() / 2, 2, 44100, 6, err));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("ogg: 30 s of stereo encoded in %.2f s (%.0fx real time), %.0f kbps\n", secs, 30 / secs,
               out.size() * 8.0 / 30 / 1000);
    }
}

int main() {
    mkdir("build", 0755);
    mkdir("build/test-tmp", 0755);
    mkdir(kDir.c_str(), 0755);
    testMp3();
    testVorbis();
    TEST_MAIN_END();
}
