// Sound files: WAV / AIFF / FLAC reading and writing, MIDI files, MD5.
// FLAC: our encoder's files must decode losslessly, and a small independent frame builder below
// makes streams with what our encoder never writes (LPC, escape partitions, wasted bits, variable
// block sizes, 8/12/20/32-bit, odd header codes), checked sample for sample.
#include <sys/stat.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audio/file_io.h"
#include "audio/md5.h"
#include "audio/sound_file.h"
#include "check.h"

static const std::string kDir = "build/test-tmp/sound";

static std::string tmp(const std::string& name) { return kDir + "/" + name; }

static void writeBytes(const std::string& path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!b.empty()) fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static std::vector<uint8_t> readBytes(const std::string& path) {
    std::vector<uint8_t> b;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return b;
    int c;
    while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
    fclose(f);
    return b;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return (uint32_t)(s >> 32);
    }
    double uniform() { return next() / 4294967296.0; }  // 0..1
    int64_t range(int64_t lo, int64_t hi) { return lo + (int64_t)(next() % (uint64_t)(hi - lo + 1)); }
};

// a test signal: sines and a little noise, interleaved
static std::vector<float> signal(size_t frames, int channels, double amp = 0.7, uint64_t seed = 1) {
    Rng r(seed);
    std::vector<float> s(frames * channels);
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++)
            s[i * channels + c] = (float)(amp * (0.8 * std::sin(i * 0.013 * (c + 1) + c) + 0.2 * (r.uniform() * 2 - 1)));
    return s;
}

static int64_t quant(float x, int bps) {
    double full = (double)(1ll << (bps - 1));
    double v = std::nearbyint((double)x * full);
    if (v > full - 1) v = full - 1;
    if (v < -full) v = -full;
    return (int64_t)v;
}

// ---------------------------------------------------------------- MD5
static void testMd5() {
    auto md5 = [](const std::string& s) { Md5 m; m.update(s); return m.hex(); };
    CHECK(md5("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
    Md5 m;  // a million 'a's, fed in odd pieces
    std::string piece(997, 'a');
    size_t left = 1000000;
    while (left) {
        size_t n = std::min(left, piece.size());
        m.update(piece.data(), n);
        left -= n;
    }
    CHECK(m.hex() == "7707d6ae4e027c70eea2a935c2296f21");
}

// ---------------------------------------------------------------- WAV writing and reading
static void testWavRoundTrip() {
    for (int channels : {1, 2, 6}) {
        size_t frames = 12345;
        std::vector<float> s = signal(frames, channels);
        s[0] = 1.5f;   // clipped in PCM
        s[1] = -2.0f;
        std::string err;
        for (SampleFormat fmt : {SampleFormat::Pcm16, SampleFormat::Pcm24, SampleFormat::Float32}) {
            std::string path = tmp("rt_" + std::to_string(channels) + "_" + std::to_string((int)fmt) + ".wav");
            CHECK(writeWav(path, s.data(), frames, channels, 44100, fmt, err));
            SoundData d;
            CHECK(readSoundFile(path, d, err));
            CHECK(d.sampleRate == 44100);
            CHECK(d.channels == channels);
            CHECK(d.frames() == frames);
            if (d.samples.size() != s.size()) continue;
            int bps = fmt == SampleFormat::Pcm16 ? 16 : 24;
            double worst = 0;
            bool exact = true;
            for (size_t i = 0; i < s.size(); i++) {
                if (fmt == SampleFormat::Float32) {
                    exact = exact && d.samples[i] == s[i];
                } else {
                    float want = (float)(quant(s[i], bps) / (double)(1ll << (bps - 1)));
                    exact = exact && d.samples[i] == want;
                    if (i > 1) worst = std::max(worst, std::fabs((double)d.samples[i] - s[i]));
                }
            }
            CHECK(exact);
            if (fmt != SampleFormat::Float32) {
                CHECK(worst <= 0.5 / (1 << (bps - 1)) + 1e-9);  // within quantisation
                CHECK(d.samples[0] == (float)((double)((1 << (bps - 1)) - 1) / (1 << (bps - 1))));  // clipped
                CHECK(d.samples[1] == -1.0f);
            }
            auto bytes = readBytes(path);  // a plain header: RIFF, fmt (fact for float), data
            CHECK(bytes.size() > 44 && memcmp(bytes.data(), "RIFF", 4) == 0 && memcmp(bytes.data() + 8, "WAVE", 4) == 0);
            size_t dataBytes = frames * channels * (fmt == SampleFormat::Pcm16 ? 2 : fmt == SampleFormat::Pcm24 ? 3 : 4);
            CHECK(bytes.size() == (fmt == SampleFormat::Float32 ? 58 : 44) + dataBytes + (dataBytes & 1));
        }
    }
    // the start only
    std::vector<float> s = signal(48000, 2);
    std::string err;
    CHECK(writeWav(tmp("start.wav"), s.data(), 48000, 2, 48000, SampleFormat::Float32, err));
    SoundData d;
    CHECK(readSoundFileStart(tmp("start.wav"), 0.25, d, err));
    CHECK(d.frames() == 12000);
    CHECK(d.samples[23999] == s[23999]);
    // an empty file
    CHECK(writeWav(tmp("empty.wav"), nullptr, 0, 2, 48000, SampleFormat::Pcm16, err));
    CHECK(readSoundFile(tmp("empty.wav"), d, err));
    CHECK(d.frames() == 0 && d.channels == 2);
}

// byte builders
struct Bytes {
    std::vector<uint8_t> b;
    Bytes& s(const char* t) { for (; *t; t++) b.push_back((uint8_t)*t); return *this; }
    Bytes& le(uint64_t v, int n) { for (int i = 0; i < n; i++) b.push_back((uint8_t)(v >> (8 * i))); return *this; }
    Bytes& be(uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i))); return *this; }
    Bytes& raw(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); return *this; }
    Bytes& u8(int v) { b.push_back((uint8_t)v); return *this; }
};

static void testWavVariants() {
    std::string err;
    SoundData d;
    // 8-bit unsigned mono, with an odd-sized unknown chunk (padded) before fmt
    {
        Bytes w;
        w.s("RIFF").le(0, 4).s("WAVE");
        w.s("LIST").le(3, 4).s("abc").u8(0);  // odd chunk + pad byte
        w.s("fmt ").le(16, 4).le(1, 2).le(1, 2).le(8000, 4).le(8000, 4).le(1, 2).le(8, 2);
        w.s("data").le(5, 4).u8(0).u8(128).u8(255).u8(64).u8(192).u8(0);  // odd data + pad
        w.s("junk").le(2, 4).u8(1).u8(2);
        writeBytes(tmp("u8.wav"), w.b);
        CHECK(readSoundFile(tmp("u8.wav"), d, err));
        CHECK(d.sampleRate == 8000 && d.channels == 1 && d.frames() == 5);
        if (d.frames() == 5) {
            CHECK(d.samples[0] == -1.0f);
            CHECK(d.samples[1] == 0.0f);
            CHECK(d.samples[2] == 127 / 128.0f);
            CHECK(d.samples[3] == -0.5f);
            CHECK(d.samples[4] == 0.5f);
        }
    }
    // WAVE_FORMAT_EXTENSIBLE, 24-bit stereo, fmt after data
    {
        Bytes data;
        int32_t vals[4] = {8388607, -8388608, 4194304, -1};
        for (int v : vals) data.le((uint32_t)v & 0xFFFFFF, 3);
        Bytes w;
        w.s("RIFF").le(0, 4).s("WAVE");
        w.s("data").le(data.b.size(), 4).raw(data.b);
        w.s("fmt ").le(40, 4).le(0xFFFE, 2).le(2, 2).le(96000, 4).le(96000 * 6, 4).le(6, 2).le(24, 2);
        w.le(22, 2).le(24, 2).le(3, 4);  // cbSize, valid bits, channel mask
        w.le(1, 2).raw({0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
        writeBytes(tmp("ext24.wav"), w.b);
        CHECK(readSoundFile(tmp("ext24.wav"), d, err));
        CHECK(d.sampleRate == 96000 && d.channels == 2 && d.frames() == 2);
        if (d.frames() == 2) {
            CHECK(d.samples[0] == 8388607 / 8388608.0f);
            CHECK(d.samples[1] == -1.0f);
            CHECK(d.samples[2] == 0.5f);
            CHECK(d.samples[3] == -1 / 8388608.0f);
        }
    }
    // float 64, and 32-bit int
    {
        Bytes w;
        double vals[3] = {0.25, -0.75, 1.5};
        w.s("RIFF").le(0, 4).s("WAVE");
        w.s("fmt ").le(18, 4).le(3, 2).le(1, 2).le(22050, 4).le(22050 * 8, 4).le(8, 2).le(64, 2).le(0, 2);
        w.s("data").le(24, 4);
        for (double v : vals) { uint64_t u; memcpy(&u, &v, 8); w.le(u, 8); }
        writeBytes(tmp("f64.wav"), w.b);
        CHECK(readSoundFile(tmp("f64.wav"), d, err));
        CHECK(d.frames() == 3 && d.samples.size() == 3 && d.samples[0] == 0.25f && d.samples[1] == -0.75f &&
              d.samples[2] == 1.5f);  // floats are not clipped
        Bytes w2;
        w2.s("RIFF").le(0, 4).s("WAVE");
        w2.s("fmt ").le(16, 4).le(1, 2).le(1, 2).le(44100, 4).le(44100 * 4, 4).le(4, 2).le(32, 2);
        w2.s("data").le(8, 4).le(0x40000000, 4).le(0x80000000u, 4);
        writeBytes(tmp("i32.wav"), w2.b);
        CHECK(readSoundFile(tmp("i32.wav"), d, err));
        CHECK(d.frames() == 2 && d.samples.size() == 2 && d.samples[0] == 0.5f && d.samples[1] == -1.0f);
    }
    // RF64 with ds64 (data size in the ds64 chunk), 16-bit, and μ-law / A-law
    {
        Bytes w;
        w.s("RF64").le(0xFFFFFFFF, 4).s("WAVE");
        w.s("ds64").le(28, 4).le(0, 8).le(4, 8).le(2, 8).le(0, 4);
        w.s("fmt ").le(16, 4).le(1, 2).le(1, 2).le(48000, 4).le(96000, 4).le(2, 2).le(16, 2);
        w.s("data").le(0xFFFFFFFF, 4).le(16384, 2).le((uint16_t)-32768, 2);
        writeBytes(tmp("rf64.wav"), w.b);
        CHECK(readSoundFile(tmp("rf64.wav"), d, err));
        CHECK(d.frames() == 2 && d.samples.size() == 2 && d.samples[0] == 0.5f && d.samples[1] == -1.0f);
        Bytes u;
        u.s("RIFF").le(0, 4).s("WAVE");
        u.s("fmt ").le(16, 4).le(7, 2).le(1, 2).le(8000, 4).le(8000, 4).le(1, 2).le(8, 2);
        u.s("data").le(4, 4).u8(0x00).u8(0x80).u8(0xFF).u8(0x7F);
        writeBytes(tmp("ulaw.wav"), u.b);
        CHECK(readSoundFile(tmp("ulaw.wav"), d, err));
        CHECK(d.frames() == 4 && d.samples.size() == 4);
        if (d.samples.size() == 4) {
            CHECK(d.samples[0] == -32124 / 32768.0f);
            CHECK(d.samples[1] == 32124 / 32768.0f);
            CHECK(d.samples[2] == 0.0f && d.samples[3] == 0.0f);
        }
        Bytes a;
        a.s("RIFF").le(0, 4).s("WAVE");
        a.s("fmt ").le(16, 4).le(6, 2).le(1, 2).le(8000, 4).le(8000, 4).le(1, 2).le(8, 2);
        a.s("data").le(2, 4).u8(0xD5).u8(0x2A);
        writeBytes(tmp("alaw.wav"), a.b);
        CHECK(readSoundFile(tmp("alaw.wav"), d, err));
        CHECK(d.samples.size() == 2 && d.samples[0] == 8 / 32768.0f && d.samples[1] == -32256 / 32768.0f);
    }
    // a truncated data chunk: what is there is read
    {
        Bytes w;
        w.s("RIFF").le(1000, 4).s("WAVE");
        w.s("fmt ").le(16, 4).le(1, 2).le(2, 2).le(44100, 4).le(44100 * 4, 4).le(4, 2).le(16, 2);
        w.s("data").le(400, 4).le(1, 2).le(2, 2).le(3, 2).le(4, 2).le(5, 2);
        writeBytes(tmp("cut.wav"), w.b);
        CHECK(readSoundFile(tmp("cut.wav"), d, err));
        CHECK(d.frames() == 2);
    }
    // errors
    {
        Bytes w;
        w.s("RIFF").le(0, 4).s("WAVE");
        w.s("fmt ").le(20, 4).le(2, 2).le(1, 2).le(44100, 4).le(22000, 4).le(256, 2).le(4, 2).le(2, 2).le(0, 2);
        w.s("data").le(4, 4).le(0, 4);
        writeBytes(tmp("adpcm.wav"), w.b);
        CHECK(!readSoundFile(tmp("adpcm.wav"), d, err));
        CHECK(err.rfind("Unsupported WAV format: ", 0) == 0);
        CHECK(err.find("ADPCM") != std::string::npos);
        writeBytes(tmp("text.wav"), {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '\n'});
        CHECK(!readSoundFile(tmp("text.wav"), d, err));
        CHECK(err == "Not a sound file this program can read");
        writeBytes(tmp("zero.wav"), {});
        CHECK(!readSoundFile(tmp("zero.wav"), d, err));
        CHECK(err == "Not a sound file this program can read");
        CHECK(!readSoundFile(tmp("does-not-exist.wav"), d, err));
        CHECK(err.find("does-not-exist.wav") != std::string::npos);
    }
}

// ---------------------------------------------------------------- AIFF
static void ext80(Bytes& b, double rate) {  // positive whole rates
    int e = 0;
    uint64_t m = (uint64_t)rate;
    while (!(m & 0x8000000000000000ull)) { m <<= 1; e++; }
    b.be(16383 + 63 - e, 2).be(m, 8);
}

static void testAiff() {
    std::string err;
    SoundData d;
    // AIFF 16-bit stereo 44100, with an odd-sized NAME chunk before COMM
    {
        Bytes a;
        a.s("FORM").be(0, 4).s("AIFF");
        a.s("NAME").be(5, 4).s("hello").u8(0);
        a.s("COMM").be(18, 4).be(2, 2).be(3, 4).be(16, 2);
        ext80(a, 44100);
        a.s("SSND").be(8 + 12, 4).be(0, 4).be(0, 4);
        int16_t v[6] = {16384, -16384, 32767, -32768, 0, 1};
        for (int16_t x : v) a.be((uint16_t)x, 2);
        writeBytes(tmp("s16.aiff"), a.b);
        CHECK(readSoundFile(tmp("s16.aiff"), d, err));
        CHECK(d.sampleRate == 44100 && d.channels == 2 && d.frames() == 3);
        if (d.samples.size() == 6) {
            CHECK(d.samples[0] == 0.5f && d.samples[1] == -0.5f);
            CHECK(d.samples[3] == -1.0f && d.samples[5] == 1 / 32768.0f);
        }
    }
    // AIFF 24-bit mono 48000 and 8-bit (signed) with an SSND offset
    {
        Bytes a;
        a.s("FORM").be(0, 4).s("AIFF");
        a.s("COMM").be(18, 4).be(1, 2).be(2, 4).be(24, 2);
        ext80(a, 48000);
        a.s("SSND").be(8 + 4 + 6, 4).be(4, 4).be(0, 4).be(0xDEADBEEF, 4).be(0x400000, 3).be(0xC00000, 3);
        writeBytes(tmp("s24.aif"), a.b);
        CHECK(readSoundFile(tmp("s24.aif"), d, err));
        CHECK(d.sampleRate == 48000 && d.frames() == 2 && d.samples.size() == 2 && d.samples[0] == 0.5f &&
              d.samples[1] == -0.5f);
        Bytes b;
        b.s("FORM").be(0, 4).s("AIFF");
        b.s("COMM").be(18, 4).be(1, 2).be(2, 4).be(8, 2);
        ext80(b, 8000);
        b.s("SSND").be(10, 4).be(0, 4).be(0, 4).u8(0x40).u8(0x80);
        writeBytes(tmp("s8.aif"), b.b);
        CHECK(readSoundFile(tmp("s8.aif"), d, err));
        CHECK(d.samples.size() == 2 && d.samples[0] == 0.5f && d.samples[1] == -1.0f);
    }
    // AIFF-C: sowt (little-endian), fl32, fl64; an unsupported compression
    {
        auto aifc = [](const char* comp, const std::vector<uint8_t>& data, int bits, int frames) {
            Bytes a;
            a.s("FORM").be(0, 4).s("AIFC");
            a.s("FVER").be(4, 4).be(0xA2805140, 4);
            a.s("COMM").be(24, 4).be(1, 2).be(frames, 4).be(bits, 2);
            ext80(a, 22050);
            a.s(comp).u8(0).u8(0);  // compression type + empty name (pascal string, padded)
            a.s("SSND").be(8 + data.size(), 4).be(0, 4).be(0, 4).raw(data);
            return a.b;
        };
        Bytes sowt;
        sowt.le(16384, 2).le((uint16_t)-8192, 2);
        writeBytes(tmp("sowt.aifc"), aifc("sowt", sowt.b, 16, 2));
        CHECK(readSoundFile(tmp("sowt.aifc"), d, err));
        CHECK(d.sampleRate == 22050 && d.samples.size() == 2 && d.samples[0] == 0.5f && d.samples[1] == -0.25f);
        Bytes f32;
        float fv[2] = {0.125f, -3.0f};
        for (float f : fv) { uint32_t u; memcpy(&u, &f, 4); f32.be(u, 4); }
        writeBytes(tmp("fl32.aifc"), aifc("fl32", f32.b, 32, 2));
        CHECK(readSoundFile(tmp("fl32.aifc"), d, err));
        CHECK(d.samples.size() == 2 && d.samples[0] == 0.125f && d.samples[1] == -3.0f);
        Bytes f64;
        double dv[2] = {-0.0625, 0.5};
        for (double f : dv) { uint64_t u; memcpy(&u, &f, 8); f64.be(u, 8); }
        writeBytes(tmp("fl64.aifc"), aifc("fl64", f64.b, 64, 2));
        CHECK(readSoundFile(tmp("fl64.aifc"), d, err));
        CHECK(d.samples.size() == 2 && d.samples[0] == -0.0625f && d.samples[1] == 0.5f);
        writeBytes(tmp("ima4.aifc"), aifc("ima4", sowt.b, 16, 2));
        CHECK(!readSoundFile(tmp("ima4.aifc"), d, err));
        CHECK(err == "Unsupported AIFF format: compression 'ima4'");
    }
}

// ---------------------------------------------------------------- FLAC: our encoder
static std::string flacMd5(const std::vector<float>& s, int bps) {
    Md5 m;
    for (float x : s) {
        int64_t v = quant(x, bps);
        for (int b = 0; b < bps / 8; b++) {
            uint8_t byte = (uint8_t)(v >> (8 * b));
            m.update(&byte, 1);
        }
    }
    return m.hex();
}

static void testFlacRoundTrip() {
    struct Case { int channels; size_t frames; int bps; int kind; };  // kind: 0 signal, 1 silence, 2 L==R, 3 noise, 4 coarse
    std::vector<Case> cases = {{2, 44100, 16, 0}, {2, 44100, 24, 0}, {1, 4097, 16, 0}, {1, 1, 24, 0},  {2, 17, 16, 0},
                               {3, 9000, 24, 0},  {2, 5000, 16, 1},  {2, 8192, 24, 2}, {2, 6000, 16, 3}, {2, 6000, 24, 3},
                               {1, 5000, 24, 4},  {8, 300, 16, 0},   {2, 0, 16, 0},    {2, 4096, 24, 0}};
    int n = 0;
    for (auto& c : cases) {
        std::vector<float> s = signal(c.frames, c.channels, 0.9, 7 + n);
        Rng r(99 + n);
        for (size_t i = 0; i < c.frames; i++) {
            for (int ch = 0; ch < c.channels; ch++) {
                float& x = s[i * c.channels + ch];
                if (c.kind == 1) x = 0;
                if (c.kind == 2 && ch == 1) x = s[i * c.channels];
                if (c.kind == 3) x = (float)(r.uniform() * 2.2 - 1.1);                       // full scale (clipped) noise
                if (c.kind == 4) x = (float)(std::floor(x * 64) / 64);                           // wasted bits
            }
        }
        std::string path = tmp("rt" + std::to_string(n++) + ".flac");
        std::string err;
        bool wrote = writeFlac(path, s.data(), c.frames, c.channels, 44100, c.bps == 16 ? SampleFormat::Pcm16 : SampleFormat::Pcm24, err);
        CHECK(wrote);
        SoundData d;
        bool read = readSoundFile(path, d, err);
        CHECK(read);
        if (!read) { fprintf(stderr, "  %s: %s\n", path.c_str(), err.c_str()); continue; }
        CHECK(d.sampleRate == 44100 && d.channels == c.channels && d.frames() == c.frames);
        bool same = d.samples.size() == s.size();
        for (size_t i = 0; same && i < s.size(); i++)
            same = d.samples[i] == (float)(quant(s[i], c.bps) / (double)(1ll << (c.bps - 1)));
        CHECK(same);
        if (!same) fprintf(stderr, "  lossless check failed for %s\n", path.c_str());
        // STREAMINFO: total samples and the MD5 of the audio
        auto bytes = readBytes(path);
        CHECK(bytes.size() >= 42 && memcmp(bytes.data(), "fLaC", 4) == 0 && bytes[4] == 0x80);
        if (bytes.size() >= 42) {
            uint64_t total = ((uint64_t)(bytes[21] & 0x0F) << 32) | ((uint64_t)bytes[22] << 24) | (bytes[23] << 16) |
                             (bytes[24] << 8) | bytes[25];
            CHECK(total == c.frames);
            static const char* hexd = "0123456789abcdef";
            std::string md5;
            for (int i = 26; i < 42; i++) { md5 += hexd[bytes[i] >> 4]; md5 += hexd[bytes[i] & 15]; }
            CHECK(md5 == flacMd5(s, c.bps));
        }
        if (c.kind == 1) CHECK(bytes.size() < 200);  // silence: constant subframes
    }
    // compression: a smooth signal at 16 bits takes far less than its WAV
    std::vector<float> smooth(48000 * 2);
    for (size_t i = 0; i < 48000; i++) smooth[2 * i] = smooth[2 * i + 1] = (float)(0.5 * std::sin(i * 0.01));
    std::string err;
    CHECK(writeFlac(tmp("smooth.flac"), smooth.data(), 48000, 2, 48000, SampleFormat::Pcm16, err));
    CHECK(readBytes(tmp("smooth.flac")).size() < 48000 * 4 / 4);
    CHECK(!writeFlac(tmp("float.flac"), smooth.data(), 10, 2, 48000, SampleFormat::Float32, err));
    // the start only
    SoundData d;
    CHECK(readSoundFileStart(tmp("rt0.flac"), 0.5, d, err));
    CHECK(d.frames() == 22050);
    // odd sample rates in frame headers
    for (int sr : {8000, 11025, 37000, 100000, 192000, 700000}) {
        std::vector<float> s = signal(5000, 1);
        CHECK(writeFlac(tmp("sr.flac"), s.data(), 5000, 1, sr, SampleFormat::Pcm16, err));
        CHECK(readSoundFile(tmp("sr.flac"), d, err) && d.sampleRate == sr && d.frames() == 5000);
    }
}

// ---------------------------------------------------------------- FLAC: an independent frame builder
struct BW {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int n = 0;
    void put(uint64_t v, int k) {
        for (int i = k - 1; i >= 0; i--) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++n == 8) { buf.push_back((uint8_t)acc); acc = 0; n = 0; }
        }
    }
    void sput(int64_t v, int k) { put((uint64_t)v, k); }
    void unary(uint64_t q) { for (uint64_t i = 0; i < q; i++) put(0, 1); put(1, 1); }
    void align() { while (n) put(0, 1); }
};
static uint8_t tcrc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}
static uint16_t tcrc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}
static uint64_t zz(int64_t v) { return v >= 0 ? (uint64_t)v * 2 : (uint64_t)(-v) * 2 - 1; }
static int bitsFor(int64_t v) {  // two's complement width holding v
    int b = 1;
    while (v < -(1ll << (b - 1)) || v > (1ll << (b - 1)) - 1) b++;
    return b;
}

struct Residual {
    int porder = 0;
    int method = -1;                  // -1: whatever the parameters need
    std::map<int, int> escape;        // partition -> raw bits (-1: as many as needed)
};
static void writeResidual(BW& w, const std::vector<int64_t>& res, int n, int order, const Residual& spec) {
    int parts = 1 << spec.porder, psize = n >> spec.porder;
    std::vector<int> ks(parts);
    int maxK = 0;
    for (int p = 0; p < parts; p++) {
        int s = p ? p * psize : order, e = (p + 1) * psize;
        uint64_t best = ~0ull;
        for (int k = 0; k <= 30; k++) {
            uint64_t bits = 0;
            for (int i = s; i < e; i++) bits += (zz(res[i]) >> k) + 1 + k;
            if (bits < best) { best = bits; ks[p] = k; }
        }
        if (!spec.escape.count(p)) maxK = std::max(maxK, ks[p]);
    }
    int method = spec.method >= 0 ? spec.method : (maxK > 14 ? 1 : 0);
    w.put(method, 2);
    w.put(spec.porder, 4);
    for (int p = 0; p < parts; p++) {
        int s = p ? p * psize : order, e = (p + 1) * psize;
        auto esc = spec.escape.find(p);
        if (esc != spec.escape.end()) {
            int nb = esc->second;
            if (nb < 0) { nb = 0; for (int i = s; i < e; i++) nb = std::max(nb, bitsFor(res[i])); }
            w.put(method ? 31 : 15, method ? 5 : 4);
            w.put(nb, 5);
            for (int i = s; i < e; i++) if (nb) w.sput(res[i], nb);
        } else {
            int k = ks[p];
            w.put(k, method ? 5 : 4);
            for (int i = s; i < e; i++) {
                uint64_t u = zz(res[i]);
                w.unary(u >> k);
                w.put(u & ((1ull << k) - 1), k);
            }
        }
    }
}
static void subHeader(BW& w, int type, int wasted) {
    w.put(0, 1);
    w.put(type, 6);
    if (wasted) { w.put(1, 1); w.unary(wasted - 1); } else w.put(0, 1);
}
static void subConstant(BW& w, int64_t v, int bps) { subHeader(w, 0, 0); w.sput(v, bps); }
static void subVerbatim(BW& w, const std::vector<int64_t>& x, int bps, int wasted = 0) {
    subHeader(w, 1, wasted);
    for (int64_t v : x) w.sput(v >> wasted, bps - wasted);
}
static void subFixed(BW& w, const std::vector<int64_t>& x, int bps, int order, const Residual& spec, int wasted = 0) {
    subHeader(w, 8 + order, wasted);
    int n = (int)x.size();
    std::vector<int64_t> y(n), res(n);
    for (int i = 0; i < n; i++) y[i] = x[i] >> wasted;
    for (int i = 0; i < order; i++) w.sput(y[i], bps - wasted);
    static const int c[5][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1}};
    for (int i = order; i < n; i++) {
        int64_t p = 0;
        for (int j = 0; j < order; j++) p += c[order][j] * y[i - 1 - j];
        res[i] = y[i] - p;
    }
    writeResidual(w, res, n, order, spec);
}
static void subLpc(BW& w, const std::vector<int64_t>& x, int bps, const std::vector<int>& coefs, int precision, int shift,
                   const Residual& spec, int wasted = 0) {
    int order = (int)coefs.size();
    subHeader(w, 31 + order, wasted);
    int n = (int)x.size();
    std::vector<int64_t> y(n), res(n);
    for (int i = 0; i < n; i++) y[i] = x[i] >> wasted;
    for (int i = 0; i < order; i++) w.sput(y[i], bps - wasted);
    w.put(precision - 1, 4);
    w.sput(shift, 5);
    for (int c : coefs) w.sput(c, precision);
    for (int i = order; i < n; i++) {
        int64_t sum = 0;
        for (int j = 0; j < order; j++) sum += (int64_t)coefs[j] * y[i - 1 - j];
        res[i] = y[i] - (sum >> shift);
    }
    writeResidual(w, res, n, order, spec);
}
static void utf8(BW& w, uint64_t v) {
    if (v < 0x80) { w.put(v, 8); return; }
    int bytes = 2;
    while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) bytes++;
    w.put(((0xFF00u >> bytes) & 0xFF) | (v >> (6 * (bytes - 1))), 8);
    for (int i = bytes - 2; i >= 0; i--) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
}
struct FrameSpec {
    int blocksize;
    uint64_t number = 0;
    bool variable = false;
    int bsCode = -1;           // -1: picked from the block size (6/7 when it isn't a standard size)
    int srCode = 0, srExtraBits = 0, srExtra = 0;
    int chCode = 0;
    int ssCode = 0;
};
static std::vector<uint8_t> frame(const FrameSpec& f, const std::function<void(BW&)>& subframes) {
    BW w;
    w.put(0x3FFE, 14);
    w.put(0, 1);
    w.put(f.variable ? 1 : 0, 1);
    int bs = f.bsCode, extra = 0;
    if (bs < 0) {
        bs = 7;
        extra = 16;
        if (f.blocksize == 192) { bs = 1; extra = 0; }
        for (int c = 2; c <= 5; c++) if (f.blocksize == 576 << (c - 2)) { bs = c; extra = 0; }
        for (int c = 8; c <= 15; c++) if (f.blocksize == 256 << (c - 8)) { bs = c; extra = 0; }
        if (extra && f.blocksize <= 256) { bs = 6; extra = 8; }
    } else {
        extra = bs == 6 ? 8 : bs == 7 ? 16 : 0;
    }
    w.put(bs, 4);
    w.put(f.srCode, 4);
    w.put(f.chCode, 4);
    w.put(f.ssCode, 3);
    w.put(0, 1);
    utf8(w, f.number);
    if (extra) w.put(f.blocksize - 1, extra);
    if (f.srExtraBits) w.put(f.srExtra, f.srExtraBits);
    w.put(tcrc8(w.buf.data(), w.buf.size()), 8);
    subframes(w);
    w.align();
    w.put(tcrc16(w.buf.data(), w.buf.size()), 16);
    return w.buf;
}
static std::vector<uint8_t> stream(int sr, int channels, int bps, uint64_t total, const std::vector<std::vector<uint8_t>>& frames,
                                   bool id3 = false) {
    Bytes b;
    if (id3) b.s("ID3").u8(4).u8(0).u8(0).be(0, 3).u8(20).raw(std::vector<uint8_t>(20, 0x55));
    b.s("fLaC");
    BW si;
    si.put(0x00, 8);  // STREAMINFO, not the last
    si.put(34, 24);
    si.put(16, 16);
    si.put(65535, 16);
    si.put(0, 24);
    si.put(0, 24);
    si.put(sr, 20);
    si.put(channels - 1, 3);
    si.put(bps - 1, 5);
    si.put(total, 36);
    for (int i = 0; i < 16; i++) si.put(0, 8);
    b.raw(si.buf);
    b.u8(0x80 | 1).be(7, 3).raw(std::vector<uint8_t>(7, 0));  // PADDING, the last block
    for (auto& f : frames) b.raw(f);
    return b.b;
}
static std::vector<int64_t> noise(Rng& r, int n, int bps, int64_t amp = -1) {
    if (amp < 0) amp = (1ll << (bps - 1)) - 1;
    std::vector<int64_t> x(n);
    for (int i = 0; i < n; i++) x[i] = r.range(-amp - 1 >= -(1ll << (bps - 1)) ? -amp - 1 : -amp, amp);
    return x;
}
static std::vector<int64_t> smoothSig(int n, int bps, double f, double phase = 0) {
    std::vector<int64_t> x(n);
    double amp = (double)((1ll << (bps - 1)) - 1) * 0.8;
    for (int i = 0; i < n; i++) x[i] = (int64_t)std::lround(amp * std::sin(i * f + phase));
    return x;
}
// compare a decoded stream with the expected per-channel samples
static bool sameAudio(const SoundData& d, const std::vector<std::vector<int64_t>>& chans, int bps) {
    size_t frames = chans[0].size();
    if (d.channels != (int)chans.size() || d.frames() != frames) {
        fprintf(stderr, "  got %d channels, %zu frames; want %zu, %zu\n", d.channels, d.frames(), chans.size(), frames);
        return false;
    }
    double scale = 1.0 / (double)(1ull << (bps - 1));
    for (size_t i = 0; i < frames; i++)
        for (size_t c = 0; c < chans.size(); c++)
            if (d.samples[i * chans.size() + c] != (float)((double)chans[c][i] * scale)) {
                fprintf(stderr, "  sample %zu channel %zu differs\n", i, c);
                return false;
            }
    return true;
}
static void append(std::vector<int64_t>& a, const std::vector<int64_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

static void testFlacDecoder() {
    // the CRCs the builder uses are the standard ones
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(tcrc8(check, 9) == 0xF4);
    CHECK(tcrc16(check, 9) == 0xFEE8);
    std::string err;
    SoundData d;
    Rng r(12345);

    // A: 16-bit stereo, mid/side, LPC (order 8) + FIXED with escape partitions (also 0 bits),
    //    then left/side and side/right frames, block size 1000 (16-bit code) and 4608 (code 5)
    {
        const int bps = 16;
        std::vector<std::vector<uint8_t>> frames;
        std::vector<std::vector<int64_t>> want(2);
        // frame 1: mid/side
        int n = 1000;
        std::vector<int64_t> L = smoothSig(n, bps, 0.02), R = smoothSig(n, bps, 0.021, 0.3);
        for (int i = 0; i < n; i++) { L[i] = L[i] / 2 + r.range(-50, 50); R[i] = R[i] / 2 + r.range(-50, 50); }
        for (int i = 0; i < 250; i++) R[i] = L[i] - (3 * i - 100);  // side is a straight line: order-2 residual 0
        std::vector<int64_t> M(n), S(n);
        for (int i = 0; i < n; i++) { M[i] = (L[i] + R[i]) >> 1; S[i] = L[i] - R[i]; }
        frames.push_back(frame({n, 0, false, -1, 9, 0, 0, 10, 4}, [&](BW& w) {
            subLpc(w, M, bps, {1800, -900, 200, 60, -40, 10, 5, -3}, 12, 10, Residual{3});
            Residual res{2};
            res.escape = {{0, 0}, {1, -1}, {3, 20}};
            subFixed(w, S, bps + 1, 2, res);
        }));
        append(want[0], L);
        append(want[1], R);
        // frame 2: left/side, 4608 samples; side constant
        n = 4608;
        L = noise(r, n, bps, 20000);
        R = L;
        for (auto& v : R) v -= 7;
        frames.push_back(frame({n, 1, false, -1, 9, 0, 0, 8, 4}, [&](BW& w) {
            subVerbatim(w, L, bps);
            subConstant(w, 7, bps + 1);
        }));
        append(want[0], L);
        append(want[1], R);
        // frame 3: side/right, fixed order 3 and 1, partition order 4 (method 1 forced)
        n = 2048;
        L = smoothSig(n, bps, 0.005);
        R = smoothSig(n, bps, 0.007, 1.0);
        S.assign(n, 0);
        for (int i = 0; i < n; i++) S[i] = L[i] - R[i];
        frames.push_back(frame({n, 2, false, -1, 9, 0, 0, 9, 4}, [&](BW& w) {
            Residual res{4};
            res.method = 1;
            subFixed(w, S, bps + 1, 3, res);
            subFixed(w, R, bps, 1, Residual{0});
        }));
        append(want[0], L);
        append(want[1], R);
        size_t total = want[0].size();
        writeBytes(tmp("dec_a.flac"), stream(44100, 2, bps, total, frames));
        CHECK(readSoundFile(tmp("dec_a.flac"), d, err));
        CHECK(d.sampleRate == 44100);
        CHECK(sameAudio(d, want, bps));
    }

    // B: 24-bit mono, variable block sizes (coded sample numbers), wasted bits, LPC order 32 (precision 15, shift 14),
    //    FIXED order 4 with 5-bit parameters, sample-rate codes 12/13/14 in the headers, ID3 tag in front
    {
        const int bps = 24;
        std::vector<std::vector<uint8_t>> frames;
        std::vector<std::vector<int64_t>> want(1);
        uint64_t at = 0;
        // 192 samples, verbatim with 4 wasted bits
        std::vector<int64_t> x = noise(r, 192, bps, 1 << 18);
        for (auto& v : x) v *= 16;
        frames.push_back(frame({192, at, true, -1, 12, 8, 48, 0, 6}, [&](BW& w) { subVerbatim(w, x, bps, 4); }));
        append(want[0], x);
        at += 192;
        // 576 samples, LPC order 32
        x = smoothSig(576, bps, 0.03);
        for (auto& v : x) v += r.range(-1000, 1000);
        std::vector<int> coefs(32);
        for (int i = 0; i < 32; i++) coefs[i] = (int)r.range(-16384, 16383);
        coefs[0] = 16383;
        frames.push_back(frame({576, at, true, -1, 13, 16, 48000, 0, 6},
                               [&](BW& w) { subLpc(w, x, bps, coefs, 15, 14, Residual{0}); }));
        append(want[0], x);
        at += 576;
        // 300 samples (8-bit block size code), FIXED order 4 with a big parameter (noise), 2 wasted bits
        x = noise(r, 300, bps);
        for (auto& v : x) v = v / 4 * 4;
        frames.push_back(frame({300, at, true, -1, 14, 16, 4800, 0, 6},
                               [&](BW& w) { subFixed(w, x, bps, 4, Residual{2}, 2); }));
        append(want[0], x);
        at += 300;
        // 16 samples, LPC order 16 = block size... order 12 with negative coefficients and shift 0
        x = noise(r, 16, bps, 1000);
        frames.push_back(frame({16, at, true, -1, 0, 0, 0, 0, 0},
                               [&](BW& w) { subLpc(w, x, bps, {-1, 1, -1, 0, 0, 1, 2, -2, 0, 0, 0, 1}, 3, 0, Residual{0}); }));
        append(want[0], x);
        at += 16;
        writeBytes(tmp("dec_b.flac"), stream(48000, 1, bps, 0, frames, true));
        CHECK(readSoundFile(tmp("dec_b.flac"), d, err));
        CHECK(d.sampleRate == 48000);
        CHECK(sameAudio(d, want, bps));
    }

    // C: 8-, 12- and 20-bit streams (sample size codes 1, 2, 5), 7-byte frame numbers
    for (int bps : {8, 12, 20}) {
        int ss = bps == 8 ? 1 : bps == 12 ? 2 : 5;
        std::vector<std::vector<int64_t>> want(1);
        std::vector<int64_t> x = smoothSig(1024, bps, 0.04);
        std::vector<int64_t> y = noise(r, 1024, bps);
        std::vector<std::vector<uint8_t>> frames;
        frames.push_back(frame({1024, 0, false, -1, 4, 0, 0, 0, ss}, [&](BW& w) { subFixed(w, x, bps, 2, Residual{1}); }));
        frames.push_back(frame({1024, 0xFFFFFFFFFull, true, -1, 4, 0, 0, 0, ss}, [&](BW& w) { subVerbatim(w, y, bps); }));
        append(want[0], x);
        append(want[0], y);
        writeBytes(tmp("dec_c.flac"), stream(8000, 1, bps, 2048, frames));
        CHECK(readSoundFile(tmp("dec_c.flac"), d, err));
        CHECK(sameAudio(d, want, bps));
        CHECK(d.sampleRate == 8000);
    }

    // D: 32-bit stereo, left/side with a 33-bit side, and mid/side with full-scale values
    {
        const int bps = 32;
        std::vector<std::vector<int64_t>> want(2);
        std::vector<std::vector<uint8_t>> frames;
        int n = 256;
        std::vector<int64_t> L = noise(r, n, bps), R = noise(r, n, bps);
        L[0] = INT32_MAX; R[0] = INT32_MIN;
        L[1] = INT32_MIN; R[1] = INT32_MAX;
        std::vector<int64_t> S(n), M(n);
        for (int i = 0; i < n; i++) { S[i] = L[i] - R[i]; M[i] = (L[i] + R[i]) >> 1; }
        frames.push_back(frame({n, 0, false, -1, 10, 0, 0, 8, 7}, [&](BW& w) {
            subVerbatim(w, L, bps);
            subVerbatim(w, S, bps + 1);
        }));
        frames.push_back(frame({n, 1, false, -1, 10, 0, 0, 10, 7}, [&](BW& w) {
            subFixed(w, M, bps, 1, Residual{0});
            subFixed(w, S, bps + 1, 2, Residual{2});
        }));
        append(want[0], L); append(want[0], L);
        append(want[1], R); append(want[1], R);
        writeBytes(tmp("dec_d.flac"), stream(48000, 2, bps, 0, frames));
        CHECK(readSoundFile(tmp("dec_d.flac"), d, err));
        CHECK(sameAudio(d, want, bps));
    }

    // E: 3 independent channels; garbage between frames, a frame with a broken CRC (skipped),
    //    and the stream's total cuts the last frame short
    {
        const int bps = 16;
        std::vector<std::vector<int64_t>> want(3);
        std::vector<std::vector<uint8_t>> frames;
        int n = 512;
        for (int f = 0; f < 3; f++) {
            std::vector<std::vector<int64_t>> ch = {smoothSig(n, bps, 0.01 * (f + 1)), noise(r, n, bps, 300),
                                                    std::vector<int64_t>(n, -5)};
            auto fr = frame({n, (uint64_t)f, false, -1, 9, 0, 0, 2, 4}, [&](BW& w) {
                subFixed(w, ch[0], bps, 2, Residual{2});
                subFixed(w, ch[1], bps, 0, Residual{0});
                subConstant(w, -5, bps);
            });
            if (f == 1) {
                fr[fr.size() / 2] ^= 0x10;  // damaged: skipped
            } else {
                for (int c = 0; c < 3; c++) append(want[c], ch[c]);
            }
            frames.push_back(fr);
            frames.push_back({0xFF, 0xF8, 0x00, 0x12, 0xFF, 0xFF, 0xF9});  // garbage that looks like a sync
        }
        for (auto& c : want) c.resize(c.size() - 100);
        writeBytes(tmp("dec_e.flac"), stream(44100, 3, bps, n * 2 - 100, frames));
        CHECK(readSoundFile(tmp("dec_e.flac"), d, err));
        CHECK(sameAudio(d, want, bps));
    }

    // not FLAC after all
    writeBytes(tmp("bad.flac"), {'f', 'L', 'a', 'C', 0x80, 0, 0, 2, 0, 0});
    CHECK(!readSoundFile(tmp("bad.flac"), d, err));
}

// ---------------------------------------------------------------- MIDI files
static void varlen(Bytes& b, uint32_t v) {
    uint8_t tmpb[5];
    int n = 0;
    tmpb[n++] = v & 0x7F;
    while (v >>= 7) tmpb[n++] = 0x80 | (v & 0x7F);
    while (n) b.u8(tmpb[--n]);
}
static std::vector<uint8_t> track(const Bytes& events) {
    Bytes t;
    t.s("MTrk").be(events.b.size(), 4).raw(events.b);
    return t.b;
}

static void testMidi() {
    // format 1, 480 ticks per beat. Track 0: tempo 120 BPM, at tick 960 tempo 240 BPM, ends at 1920.
    Bytes t0;
    varlen(t0, 0); t0.u8(0xFF).u8(0x51).u8(3).be(500000, 3);
    varlen(t0, 0); t0.u8(0xFF).u8(0x03).u8(5).s("Tempo");
    varlen(t0, 960); t0.u8(0xFF).u8(0x51).u8(3).be(250000, 3);
    varlen(t0, 960); t0.u8(0xFF).u8(0x2F).u8(0);
    // Track 1: running status, note_on velocity 0, the same note held twice (first in, first out),
    // a sysex, a note never released, controller and program changes; ends at tick 2400
    Bytes t1;
    varlen(t1, 0); t1.u8(0x90).u8(60).u8(100);
    varlen(t1, 0); t1.u8(0xC0).u8(5);                       // program change (1 data byte)
    varlen(t1, 480); t1.u8(0x90).u8(60).u8(90);
    varlen(t1, 480); t1.u8(60).u8(0);                       // running status: note on, velocity 0 = off
    varlen(t1, 0); t1.u8(0xF0); varlen(t1, 3); t1.u8(1).u8(2).u8(0xF7);
    varlen(t1, 0); t1.u8(0x90).u8(62).u8(70);
    varlen(t1, 240); t1.u8(0x80).u8(60).u8(64);             // note off: the second 60
    varlen(t1, 0); t1.u8(0xB0).u8(7).u8(100);
    varlen(t1, 0); t1.u8(0x80).u8(61).u8(0);                // off for a note that isn't on: ignored
    varlen(t1, 240); t1.u8(0x91).u8(64).u8(80);             // channel 2, never released
    varlen(t1, 0); t1.u8(0x82).u8(62).u8(0);                // other channel: doesn't release 62 on channel 1
    varlen(t1, 0); t1.u8(0x80).u8(62).u8(0);
    varlen(t1, 960); t1.u8(0xFF).u8(0x2F).u8(0);
    Bytes f;
    f.s("MThd").be(6, 4).be(1, 2).be(2, 2).be(480, 2).raw(track(t0)).raw(track(t1));
    writeBytes(tmp("song.mid"), f.b);
    std::vector<MidiFileNote> notes;
    double length = 0;
    std::string err;
    CHECK(readMidiFile(tmp("song.mid"), notes, length, err));
    // ticks to seconds: 0.5 s per beat to tick 960 (1.0 s), then 0.25 s per beat
    CHECK_NEAR(length, 1.75, 1e-12);
    CHECK(notes.size() == 4);
    if (notes.size() == 4) {
        CHECK(notes[0].start == 0.0 && notes[0].pitch == 60 && notes[0].velocity == 100);
        CHECK_NEAR(notes[0].end, 1.0, 1e-12);
        CHECK_NEAR(notes[1].start, 0.5, 1e-12);
        CHECK_NEAR(notes[1].end, 1.125, 1e-12);
        CHECK(notes[1].pitch == 60 && notes[1].velocity == 90);
        CHECK_NEAR(notes[2].start, 1.0, 1e-12);
        CHECK(notes[2].pitch == 62 && notes[2].velocity == 70 && notes[2].channel == 0);
        CHECK_NEAR(notes[2].end, 1.25, 1e-12);
        CHECK_NEAR(notes[3].start, 1.25, 1e-12);
        CHECK(notes[3].end == length && notes[3].pitch == 64 && notes[3].velocity == 80 && notes[3].channel == 1);
    }
    // file_io: the same as Notes, and rendered for previews
    std::vector<Note> ns;
    CHECK(file_io::loadMidi(tmp("song.mid"), ns, length, err));
    CHECK(ns.size() == 4 && ns[1].pitch == 60 && ns[1].velocity == 90);
    AudioPtr audio;
    CHECK(file_io::loadPlayable(tmp("song.mid"), 22050, audio, err));
    CHECK(audio && audio->frames() >= (int64_t)(1.75 * 22050));

    // format 0 with SMPTE time (25 fps, 40 ticks per frame: 1 ms per tick)
    Bytes t;
    varlen(t, 0); t.u8(0x99).u8(36).u8(127);
    varlen(t, 500); t.u8(0x89).u8(36).u8(0);
    varlen(t, 500); t.u8(0xFF).u8(0x2F).u8(0);
    Bytes g;
    g.s("MThd").be(6, 4).be(0, 2).be(1, 2).u8((uint8_t)-25).u8(40).raw(track(t));
    writeBytes(tmp("smpte.mid"), g.b);
    CHECK(readMidiFile(tmp("smpte.mid"), notes, length, err));
    CHECK(notes.size() == 1 && notes[0].channel == 9);
    CHECK_NEAR(length, 1.0, 1e-9);
    if (!notes.empty()) CHECK_NEAR(notes[0].end, 0.5, 1e-9);

    writeBytes(tmp("nomidi.mid"), {'R', 'I', 'F', 'F'});
    CHECK(!readMidiFile(tmp("nomidi.mid"), notes, length, err));
}

// ---------------------------------------------------------------- file_io
static void testFileIo() {
    CHECK(file_io::isMidi("a/b/Song.MID"));
    CHECK(file_io::isMidi("x.midi"));
    CHECK(!file_io::isMidi("x.wav"));
    CHECK(file_io::isSample("x.AIFF") && file_io::isSample("x.ogg") && file_io::isSample("x.mp3") &&
          file_io::isSample("y.mid") && file_io::isSample("z.flac"));
    CHECK(!file_io::isSample("x.json") && !file_io::isSample("wav"));
    // mono becomes stereo, multichannel keeps its first two
    std::vector<float> mono = signal(1000, 1), six = signal(1000, 6);
    std::string err;
    CHECK(writeWav(tmp("mono.wav"), mono.data(), 1000, 1, 32000, SampleFormat::Float32, err));
    CHECK(writeFlac(tmp("six.flac"), six.data(), 1000, 6, 32000, SampleFormat::Pcm24, err));
    AudioPtr a;
    int sr = 0;
    CHECK(file_io::loadAudio(tmp("mono.wav"), a, sr, err));
    CHECK(sr == 32000 && a && a->frames() == 1000 && a->samples[0] == mono[0] && a->samples[1] == mono[0]);
    CHECK(file_io::loadAudio(tmp("six.flac"), a, sr, err));
    CHECK(a && a->frames() == 1000 && a->samples[3] == (float)(quant(six[7], 24) / 8388608.0));
    CHECK(file_io::loadPlayable(tmp("mono.wav"), 64000, a, err));
    CHECK(a && a->frames() == 2000);
    CHECK(!file_io::loadAudio(tmp("text.wav"), a, sr, err));
    CHECK(err == "Not a sound file this program can read");
}

int main() {
    mkdir("build", 0755);
    mkdir("build/test-tmp", 0755);
    mkdir(kDir.c_str(), 0755);
    testMd5();
    testWavRoundTrip();
    testWavVariants();
    testAiff();
    testFlacRoundTrip();
    testFlacDecoder();
    testMidi();
    testFileIo();
    TEST_MAIN_END();
}
