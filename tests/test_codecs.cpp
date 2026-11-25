// Tests for Exbit's MP3 and Ogg Vorbis decoders (src/audio/mp3.cpp, src/audio/vorbis.cpp).
//
// No reference encoder or decoder is available, so:
//  1. the building blocks are checked against their direct formulas (IMDCTs, the polyphase
//     synthesis filterbank, the Huffman tables and decoding of hand-built bitstreams);
//  2. small encoders written here (an MP3 Layer III/Layer I encoder and an Ogg Vorbis encoder,
//     only as complete as these tests need) produce streams of known signals, which are decoded
//     and compared with the signal (SNR, level and frequency of a sine, length, alignment);
//  3. any .ogg/.oga/.mp3 files found in /usr/share/sounds (or $EXBIT_TEST_AUDIO) are decoded as
//     a sanity check, and damaged streams must not crash the decoders.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "audio/codecs.h"
#include "check.h"

// hooks defined in mp3.cpp / vorbis.cpp for these tests
void mp3TestImdct36(const float* in18, float* out36);
void mp3TestImdct12(const float* in6, float* out12);
void mp3TestSynth(const float* subbands, int slots, float* pcm);
void mp3TestSynthWindow(float* d512);
bool mp3TestHuffCode(int table, int sym, uint32_t& code, int& len);
int mp3TestHuffLinbits(int table);
int mp3TestHuffDimension(int table);
int mp3TestHuffDecode(int table, const uint8_t* buf, size_t bytes, size_t* bitpos);
void vorbisTestImdct(int n, const float* in, float* out);

namespace {

const double kPi = 3.14159265358979323846;
std::mt19937 rng(12345);
double urand(double a = -1, double b = 1) { return std::uniform_real_distribution<double>(a, b)(rng); }

// amplitude of the sinusoid at `freq` in channel `ch` of interleaved x (Hann-windowed DFT)
double toneAmplitude(const std::vector<float>& x, int channels, int ch, size_t from, size_t n, double freq, double rate) {
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
        double v = x[(from + i) * channels + ch];
        re += w * v * std::cos(2 * kPi * freq * i / rate);
        im -= w * v * std::sin(2 * kPi * freq * i / rate);
        wsum += w;
    }
    return 2 * std::sqrt(re * re + im * im) / wsum;
}

// SNR of channel ch of `got` (interleaved, starting at frame gotFrom) against ref[refFrom...]
double snrDb(const std::vector<float>& ref, int refCh, size_t refFrom, const std::vector<float>& got, int gotCh,
             size_t gotFrom, int ch, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; ++i) {
        double r = ref[(refFrom + i) * refCh + ch], g = got[(gotFrom + i) * gotCh + ch];
        s += r * r;
        e += (r - g) * (r - g);
    }
    return e > 0 ? 10 * std::log10(s / e) : 200;
}

bool finiteAndInRange(const SoundData& s) {
    for (float v : s.samples)
        if (!(v >= -1.f && v <= 1.f)) return false;
    return true;
}

// ================================================================ building blocks
void testImdcts() {
    float in[18], out[36];
    double maxErr = 0;
    for (int trial = 0; trial < 20; ++trial) {
        for (float& v : in) v = (float)urand();
        mp3TestImdct36(in, out);
        for (int i = 0; i < 36; ++i) {
            double s = 0;
            for (int k = 0; k < 18; ++k) s += in[k] * std::cos(kPi / 72 * (2 * i + 19) * (2 * k + 1));
            maxErr = std::max(maxErr, std::fabs(s - out[i]));
        }
    }
    CHECK(maxErr < 1e-4);
    float in6[6], out12[12];
    maxErr = 0;
    for (int trial = 0; trial < 20; ++trial) {
        for (float& v : in6) v = (float)urand();
        mp3TestImdct12(in6, out12);
        for (int i = 0; i < 12; ++i) {
            double s = 0;
            for (int k = 0; k < 6; ++k) s += in6[k] * std::cos(kPi / 24 * (2 * i + 7) * (2 * k + 1));
            maxErr = std::max(maxErr, std::fabs(s - out12[i]));
        }
    }
    CHECK(maxErr < 1e-5);
    // Vorbis: fast (FFT) IMDCT against the O(n^2) definition
    for (int n : {64, 128, 256, 512, 2048, 8192}) {
        std::vector<float> X(n / 2), y(n);
        for (float& v : X) v = (float)urand();
        vorbisTestImdct(n, X.data(), y.data());
        double err = 0, mag = 0;
        for (int i = 0; i < n; i += (n > 2048 ? 7 : 1)) {
            double s = 0;
            for (int k = 0; k < n / 2; ++k) s += X[k] * std::cos(2 * kPi / n * (i + 0.5 + n / 4.0) * (k + 0.5));
            err = std::max(err, std::fabs(s - y[i]));
            mag = std::max(mag, std::fabs(s));
        }
        CHECK(err < 1e-5 * std::sqrt((double)n) * std::max(mag, 1.0));
    }
}

// the ISO procedure for the analysis and synthesis filterbanks, written out directly
struct IsoSynthesis {
    std::vector<double> V = std::vector<double>(1024, 0.0);
    float D[512];
    IsoSynthesis() { mp3TestSynthWindow(D); }
    void run(const float* S, float* out) {
        for (int i = 1023; i >= 64; --i) V[i] = V[i - 64];
        for (int i = 0; i < 64; ++i) {
            double s = 0;
            for (int k = 0; k < 32; ++k) s += std::cos((16 + i) * (2 * k + 1) * kPi / 64) * S[k];
            V[i] = s;
        }
        double U[512];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 32; ++j) {
                U[64 * i + j] = V[128 * i + j];
                U[64 * i + 32 + j] = V[128 * i + 96 + j];
            }
        for (int j = 0; j < 32; ++j) {
            double s = 0;
            for (int i = 0; i < 16; ++i) s += U[j + 32 * i] * D[j + 32 * i];
            out[j] = (float)s;
        }
    }
};

struct IsoAnalysis {
    double X[512] = {0};
    float C[512];
    double M[32][64];
    IsoAnalysis() {
        float D[512];
        mp3TestSynthWindow(D);
        for (int i = 0; i < 512; ++i) C[i] = D[i] / 32;
        for (int k = 0; k < 32; ++k)
            for (int i = 0; i < 64; ++i) M[k][i] = std::cos((2 * k + 1) * (i - 16) * kPi / 64);
    }
    void run(const float* in32, float* S) {
        for (int i = 511; i >= 32; --i) X[i] = X[i - 32];
        for (int i = 0; i < 32; ++i) X[i] = in32[31 - i];
        double Y[64];
        for (int i = 0; i < 64; ++i) {
            double s = 0;
            for (int j = 0; j < 8; ++j) s += C[i + 64 * j] * X[i + 64 * j];
            Y[i] = s;
        }
        for (int k = 0; k < 32; ++k) {
            double s = 0;
            for (int i = 0; i < 64; ++i) s += M[k][i] * Y[i];
            S[k] = (float)s;
        }
    }
};

void testSynthesisFilterbank() {
    const int slots = 40;
    std::vector<float> sb(slots * 32), fast(slots * 32), direct(slots * 32);
    for (float& v : sb) v = (float)urand();
    mp3TestSynth(sb.data(), slots, fast.data());
    IsoSynthesis iso;
    for (int t = 0; t < slots; ++t) iso.run(sb.data() + 32 * t, direct.data() + 32 * t);
    double err = 0;
    for (int i = 0; i < slots * 32; ++i) err = std::max(err, (double)std::fabs(fast[i] - direct[i]));
    CHECK(err < 1e-5);
    // the window table itself: analysis + synthesis reconstruct the signal (delay 481) to the
    // MPEG filterbank's near-perfect-reconstruction accuracy; a mistyped coefficient breaks this
    IsoAnalysis ana;
    const int blocks = 64;
    std::vector<float> x(blocks * 32), y(blocks * 32), S(blocks * 32);
    for (float& v : x) v = (float)urand(-0.5, 0.5);
    for (int b = 0; b < blocks; ++b) ana.run(x.data() + 32 * b, S.data() + 32 * b);
    mp3TestSynth(S.data(), blocks, y.data());
    err = 0;
    for (int i = 600; i + 481 < blocks * 32; ++i) err = std::max(err, (double)std::fabs(y[i + 481] - x[i]));
    CHECK(err < 1e-4);
}

void testHuffmanTables() {
    const int tables[] = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24, 32};
    for (int t : tables) {
        int dim = mp3TestHuffDimension(t);
        int n = t == 32 ? 16 : dim * dim;
        // a complete prefix code: Kraft sum exactly 1, no codeword a prefix of another
        std::vector<std::pair<uint32_t, int>> codes;  // left-aligned, length
        uint64_t kraft = 0;
        bool ok = true;
        for (int i = 0; i < n; ++i) {
            int sym = t == 32 ? i : ((i / dim) << 4) | (i % dim);
            uint32_t c;
            int l;
            ok = ok && mp3TestHuffCode(t, sym, c, l) && l >= 1 && l <= 19 && c < (1u << l);
            kraft += 1ull << (32 - l);
            codes.push_back({c << (32 - l), l});
        }
        CHECK(ok);
        CHECK(kraft == (1ull << 32));
        std::sort(codes.begin(), codes.end());
        bool prefixFree = true;
        for (size_t i = 0; i + 1 < codes.size(); ++i) {
            uint32_t a = codes[i].first, b = codes[i + 1].first;
            int l = codes[i].second;
            if ((a >> (32 - l)) == (b >> (32 - l))) prefixFree = false;
        }
        CHECK(prefixFree);
    }
    CHECK(mp3TestHuffLinbits(16) == 1 && mp3TestHuffLinbits(23) == 13 && mp3TestHuffLinbits(24) == 4 &&
          mp3TestHuffLinbits(31) == 13 && mp3TestHuffLinbits(15) == 0);
    // hand-built bitstreams: random symbols from random tables, concatenated MSB first
    std::vector<uint8_t> buf(1 << 16, 0);
    size_t bits = 0;
    std::vector<std::pair<int, int>> sent;  // (table, symbol)
    const int sel[] = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 19, 24, 30, 32, 33};
    for (int i = 0; i < 3000; ++i) {
        int t = sel[rng() % (sizeof(sel) / sizeof(sel[0]))];
        int dim = mp3TestHuffDimension(t);
        int sym = (t >= 32) ? (int)(rng() % 16) : (int)(((rng() % dim) << 4) | (rng() % dim));
        uint32_t c;
        int l;
        mp3TestHuffCode(t, sym, c, l);
        for (int b = l - 1; b >= 0; --b, ++bits)
            if ((c >> b) & 1) buf[bits >> 3] |= 0x80 >> (bits & 7);
        sent.push_back({t, sym});
    }
    size_t bytes = (bits + 7) / 8;
    size_t pos = 0;
    int wrong = 0;
    for (auto& s : sent)
        if (mp3TestHuffDecode(s.first, buf.data(), bytes, &pos) != s.second) ++wrong;
    CHECK(wrong == 0);
    CHECK(pos == bits);
}

// ================================================================ a minimal MP3 encoder
namespace mp3enc {

struct BitsBE {
    std::vector<uint8_t> d;
    size_t n = 0;
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i, ++n) {
            if ((n & 7) == 0) d.push_back(0);
            if ((v >> i) & 1) d.back() |= 0x80 >> (n & 7);
        }
    }
    void append(const BitsBE& o) {
        for (size_t i = 0; i < o.n; ++i) put((o.d[i >> 3] >> (7 - (i & 7))) & 1, 1);
    }
};

const int kLong[3][22] = {  // 44.1, 22.05, 11.025 kHz (MPEG-1, MPEG-2, MPEG-2.5)
    {4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 20, 24, 28, 34, 42, 50, 54, 76, 158},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54}};
const int kPretab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};
bool overflow = false;  // the test encoder couldn't fit a granule into its bits

struct Params {
    int version = 0;  // 0 MPEG-1 (44.1 kHz), 1 MPEG-2 (22.05 kHz), 2 MPEG-2.5 (11.025 kHz)
    int channels = 1;
    int bitrate = 128;
    bool ms = false;          // joint stereo: mid/side
    bool intensity = false;   // joint stereo: intensity stereo from long band isStart (MPEG-1 only)
    int isStart = 13, isPos = 4;
    bool scalefactors = false;  // write nonzero scalefactors, preflag, scfsi (MPEG-1)
};

int sampleRate(const Params& p) { return p.version == 0 ? 44100 : p.version == 1 ? 22050 : 11025; }

struct Granule {
    int part23 = 0, bigValues = 0, gg = 210, sfc = 0, table[3] = {0, 0, 0}, region0 = 7, region1 = 7;
    int preflag = 0, sfScale = 0, count1Table = 0;
    int scf[22] = {0};
    int ix[576] = {0};
    BitsBE data;
};

int pairBits(int table, int x, int y) {
    int lin = mp3TestHuffLinbits(table), dim = mp3TestHuffDimension(table);
    if (!lin && (x >= dim || y >= dim)) return -1;
    if (lin && (x > 15 + (1 << lin) - 1 || y > 15 + (1 << lin) - 1)) return -1;
    uint32_t c;
    int l;
    if (!mp3TestHuffCode(table, (std::min(x, 15) << 4) | std::min(y, 15), c, l)) return -1;
    return l + (x ? 1 : 0) + (y ? 1 : 0) + (lin && x >= 15 ? lin : 0) + (lin && y >= 15 ? lin : 0);
}

void putPair(BitsBE& b, int table, int x, int y) {
    int lin = mp3TestHuffLinbits(table);
    int ax = std::abs(x), ay = std::abs(y);
    uint32_t c;
    int l;
    mp3TestHuffCode(table, (std::min(ax, 15) << 4) | std::min(ay, 15), c, l);
    b.put(c, l);
    if (lin && ax >= 15) b.put(ax - 15, lin);
    if (ax) b.put(x < 0, 1);
    if (lin && ay >= 15) b.put(ay - 15, lin);
    if (ay) b.put(y < 0, 1);
}

// Huffman-codes ix into g (tables, big_values, count1); returns the bits, writes them if out
int codeSpectrum(Granule& g, const int* widths, BitsBE* out) {
    const int* ix = g.ix;
    int last = 576;
    while (last > 0 && ix[last - 1] == 0) --last;
    int c1end = std::min(576, (last + 3) & ~3);
    int bigEnd = c1end;
    while (bigEnd >= 4 && std::abs(ix[bigEnd - 1]) <= 1 && std::abs(ix[bigEnd - 2]) <= 1 && std::abs(ix[bigEnd - 3]) <= 1 &&
           std::abs(ix[bigEnd - 4]) <= 1)
        bigEnd -= 4;
    g.bigValues = bigEnd / 2;
    int r1 = 0, r2 = 0;
    for (int b = 0; b <= g.region0; ++b) r1 += widths[b];
    r2 = r1;
    for (int b = g.region0 + 1; b <= g.region0 + g.region1 + 1; ++b) r2 += widths[b];
    int bounds[4] = {0, std::min(r1, bigEnd), std::min(r2, bigEnd), bigEnd};
    static const int cands[] = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    int total = 0;
    for (int r = 0; r < 3; ++r) {
        int best = 0, bestBits = 0;
        bool allZero = true;
        for (int i = bounds[r]; i < bounds[r + 1]; ++i) allZero = allZero && ix[i] == 0;
        if (!allZero) {
            bestBits = -1;
            for (int t : cands) {
                int s = 0;
                for (int i = bounds[r]; i < bounds[r + 1] && s >= 0; i += 2) {
                    int b = pairBits(t, std::abs(ix[i]), std::abs(ix[i + 1]));
                    s = b < 0 ? -1 : s + b;
                }
                if (s >= 0 && (bestBits < 0 || s < bestBits)) {
                    bestBits = s;
                    best = t;
                }
            }
        }
        g.table[r] = best;
        total += bestBits;
    }
    // count1: table A or B
    int bitsA = 0, bitsB = 0;
    for (int i = bigEnd; i < c1end; i += 4) {
        int q = 0, signs = 0;
        for (int k = 0; k < 4; ++k)
            if (ix[i + k]) {
                q |= 8 >> k;
                ++signs;
            }
        uint32_t c;
        int l;
        mp3TestHuffCode(32, q, c, l);
        bitsA += l + signs;
        bitsB += 4 + signs;
    }
    g.count1Table = bitsB < bitsA;
    total += std::min(bitsA, bitsB);
    if (out) {
        for (int r = 0; r < 3; ++r)
            for (int i = bounds[r]; i < bounds[r + 1]; i += 2)
                if (g.table[r]) putPair(*out, g.table[r], ix[i], ix[i + 1]);
        for (int i = bigEnd; i < c1end; i += 4) {
            int q = 0;
            for (int k = 0; k < 4; ++k)
                if (ix[i + k]) q |= 8 >> k;
            uint32_t c;
            int l;
            mp3TestHuffCode(g.count1Table ? 33 : 32, q, c, l);
            out->put(c, l);
            for (int k = 0; k < 4; ++k)
                if (ix[i + k]) out->put(ix[i + k] < 0, 1);
        }
    }
    return total;
}

// slen of MPEG-1 scalefac_compress
const int kSlen1[16] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
const int kSlen2[16] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};

// MPEG-2: only the intensity-stereo right channel has scalefactors here (scalefac_compress 6:
// intensity table 3, partitions of 7/7/7 bands with slen 0/0/3, intensity_scale 0)
const int kLsfIsSfc = 6;
int part2Bits(const Granule& g, int scfsi, bool lsf) {
    if (lsf) return g.sfc == kLsfIsSfc ? 7 * 3 : 0;
    int s1 = kSlen1[g.sfc], s2 = kSlen2[g.sfc];
    const int groupN[4] = {6, 5, 5, 5}, groupS[4] = {s1, s1, s2, s2};
    int b = 0;
    for (int k = 0; k < 4; ++k)
        if (!(scfsi & (8 >> k))) b += groupN[k] * groupS[k];
    return b;
}

void putPart2(BitsBE& out, const Granule& g, int scfsi, bool lsf) {
    if (lsf) {
        if (g.sfc == kLsfIsSfc)
            for (int b = 14; b < 21; ++b) out.put(g.scf[b], 3);
        return;
    }
    int s1 = kSlen1[g.sfc], s2 = kSlen2[g.sfc];
    int band = 0;
    const int groupN[4] = {6, 5, 5, 5}, groupS[4] = {s1, s1, s2, s2};
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < groupN[k]; ++i, ++band)
            if (!(scfsi & (8 >> k))) out.put(g.scf[band], groupS[k]);
    }
}

// quantizes xr into g with the smallest global gain whose bits fit the budget; codes g.data
void quantize(const float* xr, Granule& g, int budget, const int* widths, int scfsi, bool lsf) {
    float mul[576];
    int line = 0;
    for (int b = 0; b < 22; ++b)
        for (int k = 0; k < widths[b]; ++k, ++line)
            mul[line] = (float)std::pow(2.0, -0.5 * (1 + g.sfScale) * (g.scf[b] + (g.preflag ? kPretab[b] : 0)));
    auto tryGain = [&](int gg) {
        double step = std::pow(2.0, (gg - 210) / 4.0);
        for (int i = 0; i < 576; ++i) {
            double v = std::fabs(xr[i]) / (step * mul[i]);
            int q = (int)(std::pow(v, 0.75) + 0.4054);
            if (q > 8206) return -1;
            g.ix[i] = xr[i] < 0 ? -q : q;
        }
        int bits = codeSpectrum(g, widths, nullptr) + part2Bits(g, scfsi, lsf);
        return bits < 4096 ? bits : -1;
    };
    int lo = 0, hi = 255;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int b = tryGain(mid);
        if (b >= 0 && b <= budget) hi = mid;
        else lo = mid + 1;
    }
    for (g.gg = lo; g.gg < 255; ++g.gg) {
        int b = tryGain(g.gg);
        if (b >= 0 && b <= budget) break;
    }
    int finalBits = tryGain(g.gg);
    if (finalBits < 0 || finalBits > budget) overflow = true;
    g.data = BitsBE();
    putPart2(g.data, g, scfsi, lsf);
    codeSpectrum(g, widths, &g.data);
    g.part23 = (int)g.data.n;
}

struct Frame {
    int padding = 0;
    int mdb = 0;
    int scfsi[2] = {0, 0};
    Granule gr[2][2];
};

const int kBitrateIdx[2][16] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
                                {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}};

void putHeader(BitsBE& b, const Params& p, int padding, int layer = 3) {
    bool lsf = p.version != 0;
    int bi = 0;
    for (int i = 1; i < 15; ++i)
        if (kBitrateIdx[lsf][i] == p.bitrate) bi = i;
    if (layer == 1) bi = p.bitrate / 32;  // MPEG-1 Layer I: 32 kbps steps
    b.put(0x7FF, 11);
    b.put(p.version == 0 ? 3 : p.version == 1 ? 2 : 0, 2);
    b.put(4 - layer, 2);
    b.put(1, 1);  // no CRC
    b.put(bi, 4);
    b.put(0, 2);  // 44.1 / 22.05 / 11.025 kHz
    b.put(padding, 1);
    b.put(0, 1);
    int mode = p.channels == 1 ? 3 : (p.ms || p.intensity) ? 1 : 0;
    b.put(mode, 2);
    b.put((p.ms ? 2 : 0) | (p.intensity ? 1 : 0), 2);
    b.put(0, 4);
}

int frameSizeFor(const Params& p, int64_t frameIndex) {
    int64_t k = p.version == 0 ? 144000 : 72000;
    int64_t num = k * p.bitrate;
    int sr = sampleRate(p);
    return (int)((frameIndex + 1) * num / sr - frameIndex * num / sr);
}

struct Encoded {
    std::vector<uint8_t> bytes;
    int delay = 0;  // of the decoded signal (without gapless trimming)
};

// in: per channel signal of n samples. Returns MPEG audio frames (+ optional tags/junk)
Encoded encode(const Params& p, const std::vector<std::vector<float>>& in, size_t n, bool lameTag, bool tagsAndJunk) {
    const bool lsf = p.version != 0;
    const int nch = p.channels, ngr = lsf ? 1 : 2, spf = 576 * ngr;
    const int* widths = kLong[p.version];
    const int sideBytes = lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
    const int maxMdb = lsf ? 255 : 511;
    const int delay = 481 + 576;
    int64_t frames = (int64_t)((n + delay + spf - 1) / spf) + 1;
    std::vector<IsoAnalysis> ana(nch);
    std::vector<std::vector<float>> prevSb(nch, std::vector<float>(576, 0.f));  // [sb*18 + t]
    std::vector<Frame> fr((size_t)frames);
    std::vector<uint8_t> main;  // main data stream
    int64_t slotStart = 0, used = 0;
    double cs[8], ca[8];
    const double c[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
    for (int i = 0; i < 8; ++i) {
        cs[i] = 1 / std::sqrt(1 + c[i] * c[i]);
        ca[i] = c[i] / std::sqrt(1 + c[i] * c[i]);
    }
    std::vector<int> frameSize((size_t)frames);
    size_t sample = 0;
    for (int64_t f = 0; f < frames; ++f) {
        int bytes = frameSizeFor(p, f);
        frameSize[f] = bytes;
        int base = (p.version == 0 ? 144000 : 72000) * p.bitrate / sampleRate(p);
        fr[f].padding = bytes - base;
        int slots = bytes - 4 - sideBytes;
        int64_t start = std::max(used, slotStart - maxMdb);
        fr[f].mdb = (int)(slotStart - start);
        int64_t budget = (slotStart + slots - start) * 8;
        main.resize((size_t)start, 0);
        BitsBE frameData;
        for (int g = 0; g < ngr; ++g) {
            float xr[2][576];
            for (int ch = 0; ch < nch; ++ch) {
                float cur[576];  // [sb*18 + t]
                for (int t = 0; t < 18; ++t) {
                    float pcm[32], S[32];
                    for (int i = 0; i < 32; ++i) {
                        size_t s = sample + t * 32 + i;
                        pcm[i] = s < n ? in[ch][s] : 0.f;
                    }
                    ana[ch].run(pcm, S);
                    for (int sb = 0; sb < 32; ++sb) cur[sb * 18 + t] = ((sb & 1) && (t & 1)) ? -S[sb] : S[sb];
                }
                for (int sb = 0; sb < 32; ++sb) {
                    double z[36];
                    for (int i = 0; i < 36; ++i) {
                        double v = i < 18 ? prevSb[ch][sb * 18 + i] : cur[sb * 18 + i - 18];
                        z[i] = v * std::sin(kPi / 36 * (i + 0.5));
                    }
                    for (int k = 0; k < 18; ++k) {
                        double s = 0;
                        for (int i = 0; i < 36; ++i) s += z[i] * std::cos(kPi / 72 * (2 * i + 19) * (2 * k + 1));
                        xr[ch][sb * 18 + k] = (float)(s / 9);
                    }
                }
                for (int sb = 0; sb < 31; ++sb)
                    for (int i = 0; i < 8; ++i) {
                        float& a = xr[ch][sb * 18 + 17 - i];
                        float& b = xr[ch][(sb + 1) * 18 + i];
                        double A = a, B = b;
                        a = (float)(A * cs[i] + B * ca[i]);
                        b = (float)(B * cs[i] - A * ca[i]);
                    }
                memcpy(prevSb[ch].data(), cur, sizeof(cur));
            }
            // joint stereo
            if (nch == 2 && (p.ms || p.intensity)) {
                int isLine = 576;
                double klr = 1;  // kl + kr of the intensity position
                if (p.intensity) {
                    isLine = 0;
                    for (int b = 0; b < p.isStart; ++b) isLine += widths[b];
                    if (lsf) klr = 1 + std::pow(2.0, -0.25 * ((p.isPos + 1) / 2));
                }
                for (int i = 0; i < 576; ++i) {
                    float L = xr[0][i], R = xr[1][i];
                    if (i >= isLine) {
                        xr[0][i] = (float)((L + R) / klr);  // the decoder pans it by the intensity position
                        xr[1][i] = 0;
                    } else if (p.ms) {
                        xr[0][i] = (float)((L + R) / std::sqrt(2.0));
                        xr[1][i] = (float)((L - R) / std::sqrt(2.0));
                    }
                }
            }
            for (int ch = 0; ch < nch; ++ch) {
                Granule& G = fr[f].gr[g][ch];
                int scfsi = 0;
                if (p.scalefactors && !lsf && !(p.intensity && ch == 1)) {
                    G.sfc = 5;  // slen 1, 1
                    for (int b = 0; b < 21; ++b) G.scf[b] = (b >= 11 && (b & 1)) ? 1 : 0;
                    if (g == 0) {
                        G.preflag = 1;
                    } else {
                        G.sfScale = 1;
                        scfsi = 3;  // groups 2 and 3 copied from granule 0
                        fr[f].scfsi[ch] = scfsi;
                    }
                }
                if (p.intensity && ch == 1) {
                    G.sfc = lsf ? kLsfIsSfc : 7;  // MPEG-1: slen 1, 3: intensity positions in the upper bands
                    for (int b = p.isStart; b < 21; ++b) G.scf[b] = p.isPos;
                }
                int remaining = (ngr - g) * nch - ch;
                int64_t left = budget - (int64_t)frameData.n;
                int myBudget = (int)std::min<int64_t>(4095, left / remaining);
                quantize(xr[ch], G, myBudget, widths, scfsi, lsf);
                frameData.append(G.data);
            }
            sample += 576;
        }
        // the main data of this frame starts at `start`
        main.insert(main.end(), frameData.d.begin(), frameData.d.end());
        used = (int64_t)main.size();
        slotStart += slots;
        if (used > slotStart) used = slotStart;  // (can't happen: budget)
    }
    main.resize((size_t)slotStart, 0);
    // write the frames
    Encoded out;
    out.delay = delay;
    std::vector<uint8_t>& o = out.bytes;
    if (tagsAndJunk) {  // an ID3v2 tag whose contents look like frame headers
        const uint8_t id3[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 2, 0};  // size 256
        o.insert(o.end(), id3, id3 + 10);
        for (int i = 0; i < 256; ++i) o.push_back(i % 3 == 0 ? 0xFF : i % 3 == 1 ? 0xFB : 0x90);
    }
    if (lameTag) {
        BitsBE h;
        putHeader(h, p, 0);
        int bytes = (p.version == 0 ? 144000 : 72000) * p.bitrate / sampleRate(p);  // no padding
        std::vector<uint8_t> tf(bytes, 0);
        memcpy(tf.data(), h.d.data(), 4);
        size_t off = 4 + sideBytes;
        memcpy(tf.data() + off, "Info", 4);
        tf[off + 7] = 1;  // flags: frame count
        uint32_t nf = (uint32_t)frames;
        for (int i = 0; i < 4; ++i) tf[off + 8 + i] = (uint8_t)(nf >> (24 - 8 * i));
        uint8_t* l = tf.data() + off + 12;
        memcpy(l, "LAME3.100", 9);
        int encDelay = delay - 529;
        int64_t padding = frames * spf - (int64_t)n - encDelay;
        l[21] = (uint8_t)(encDelay >> 4);
        l[22] = (uint8_t)((encDelay & 15) << 4 | (padding >> 8));
        l[23] = (uint8_t)(padding & 255);
        o.insert(o.end(), tf.begin(), tf.end());
    }
    int64_t slot = 0;
    for (int64_t f = 0; f < frames; ++f) {
        BitsBE h;
        putHeader(h, p, fr[f].padding);
        // side info
        h.put(fr[f].mdb, lsf ? 8 : 9);
        h.put(0, lsf ? (nch == 1 ? 1 : 2) : (nch == 1 ? 5 : 3));
        if (!lsf)
            for (int ch = 0; ch < nch; ++ch) h.put(fr[f].scfsi[ch], 4);
        for (int g = 0; g < ngr; ++g)
            for (int ch = 0; ch < nch; ++ch) {
                const Granule& G = fr[f].gr[g][ch];
                h.put(G.part23, 12);
                h.put(G.bigValues, 9);
                h.put(G.gg, 8);
                h.put(G.sfc, lsf ? 9 : 4);
                h.put(0, 1);  // long blocks only
                for (int r = 0; r < 3; ++r) h.put(G.table[r], 5);
                h.put(G.region0, 4);
                h.put(G.region1, 3);
                if (!lsf) h.put(G.preflag, 1);
                h.put(G.sfScale, 1);
                h.put(G.count1Table, 1);
            }
        o.insert(o.end(), h.d.begin(), h.d.end());
        int slots = frameSize[f] - 4 - sideBytes;
        o.insert(o.end(), main.begin() + slot, main.begin() + slot + slots);
        slot += slots;
        if (tagsAndJunk && f == frames / 2) {  // junk between two frames
            for (int i = 0; i < 37; ++i) o.push_back((uint8_t)(rng() & 0x7F));
        }
    }
    if (tagsAndJunk) {  // an ID3v1 tag
        std::vector<uint8_t> tag(128, 0xFF);
        memcpy(tag.data(), "TAG", 3);
        o.insert(o.end(), tag.begin(), tag.end());
    }
    return out;
}

// Layer I (MPEG-1, 44.1 kHz, mono): fixed bit allocation
std::vector<uint8_t> encodeLayer1(const std::vector<float>& x, size_t n, int bitrate) {
    Params p;
    p.bitrate = bitrate;
    IsoAnalysis ana;
    std::vector<uint8_t> o;
    size_t frames = (n + 481) / 384 + 2;
    for (size_t f = 0; f < frames; ++f) {
        int64_t num = 12000LL * bitrate;
        int slots = (int)((f + 1) * num / 44100 - f * num / 44100);
        int base = (int)(num / 44100);
        BitsBE b;
        putHeader(b, p, slots - base, 1);
        float S[12][32];
        for (int t = 0; t < 12; ++t) {
            float pcm[32];
            for (int i = 0; i < 32; ++i) {
                size_t s = f * 384 + t * 32 + i;
                pcm[i] = s < n ? x[s] : 0.f;
            }
            ana.run(pcm, S[t]);
        }
        int nb[32];
        for (int sb = 0; sb < 32; ++sb) nb[sb] = sb < 12 ? 5 : sb < 22 ? 3 : 0;
        for (int sb = 0; sb < 32; ++sb) b.put(nb[sb] ? nb[sb] - 1 : 0, 4);
        int scf[32];
        for (int sb = 0; sb < 32; ++sb) {
            if (!nb[sb]) continue;
            double mx = 1e-9;
            for (int t = 0; t < 12; ++t) mx = std::max(mx, (double)std::fabs(S[t][sb]));
            int i = (int)std::floor(3 * (1 - std::log2(mx)));  // largest index with 2^(1-i/3) >= mx
            scf[sb] = std::max(0, std::min(62, i));
            b.put(scf[sb], 6);
        }
        for (int t = 0; t < 12; ++t)
            for (int sb = 0; sb < 32; ++sb) {
                if (!nb[sb]) continue;
                int L = (1 << nb[sb]) - 1;
                double v = S[t][sb] / (2.0 * std::pow(2.0, -scf[sb] / 3.0));
                int code = (int)std::lround((v * L + L - 1) / 2);
                b.put(std::max(0, std::min(L - 1, code)), nb[sb]);
            }
        b.d.resize(slots * 4, 0);
        o.insert(o.end(), b.d.begin(), b.d.end());
    }
    return o;
}

}  // namespace mp3enc

std::vector<std::vector<float>> testSignal(int rate, size_t n, int channels, double f0, bool quietGap) {
    std::vector<std::vector<float>> x(channels, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i) {
        double t = (double)i / rate;
        // smooth fades (5 ms) at the start and around the quiet gap: the test encoders use long
        // blocks only, which code clicks badly (pre-echo)
        auto ramp = [](double u) { return u <= 0 ? 0.0 : u >= 1 ? 1.0 : 0.5 - 0.5 * std::cos(kPi * u); };
        double env = ramp(t / 0.005);
        if (quietGap) env *= std::max(ramp((0.45 - t) / 0.005), ramp((t - 0.55) / 0.005));
        double a = 0.4 * std::sin(2 * kPi * f0 * t) + 0.1 * std::sin(2 * kPi * 2.7 * f0 * t + 0.3);
        x[0][i] = (float)(env * a);
        if (channels > 1) x[1][i] = (float)(env * (0.25 * std::sin(2 * kPi * f0 * t + 1.0) + 0.12 * std::sin(2 * kPi * 1.9 * f0 * t)));
    }
    return x;
}

std::vector<float> interleave(const std::vector<std::vector<float>>& x) {
    size_t n = x[0].size(), c = x.size();
    std::vector<float> o(n * c);
    for (size_t i = 0; i < n; ++i)
        for (size_t k = 0; k < c; ++k) o[i * c + k] = x[k][i];
    return o;
}

void testMp3Roundtrip(const mp3enc::Params& p, double minSnr, const char* what) {
    int rate = mp3enc::sampleRate(p);
    size_t n = (size_t)(rate * 1.0) + 123;
    auto x = testSignal(rate, n, p.channels, 440, true);
    if (p.intensity) {  // right = scaled left above the intensity boundary (8 kHz tone), plus a 3.2 kHz tone in the left only
        for (size_t i = 0; i < n; ++i) {
            double t = (double)i / rate;
            double hi = 0.1 * std::sin(2 * kPi * (p.version ? 6000 : 8000) * t);
            // left / right: MPEG-1 tan(pos pi/12); MPEG-2 (intensity_scale 0) 2^(-(pos+1)/2 / 4) for odd pos
            double ratio = p.version ? std::pow(2.0, -0.25 * ((p.isPos + 1) / 2)) : std::tan(p.isPos * kPi / 12);
            if (p.version && !(p.isPos & 1)) ratio = 1 / std::pow(2.0, -0.25 * (p.isPos / 2));
            // a tone in the left only in the band below the intensity boundary keeps the boundary there
            double below = p.version ? 2957 : 3158;
            x[0][i] = (float)(0.3 * std::sin(2 * kPi * 440 * t) + 0.15 * std::sin(2 * kPi * below * t) + hi);
            x[1][i] = (float)(0.2 * std::sin(2 * kPi * 660 * t) + hi / ratio);
        }
    }
    mp3enc::overflow = false;
    mp3enc::Encoded e = mp3enc::encode(p, x, n, true, true);
    CHECK(!mp3enc::overflow);
    SoundData s;
    std::string err;
    bool ok = decodeMp3(e.bytes.data(), e.bytes.size(), s, err);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  %s: %s\n", what, err.c_str());
        return;
    }
    CHECK(looksLikeMp3(e.bytes.data(), e.bytes.size()));
    CHECK(!looksLikeVorbis(e.bytes.data(), e.bytes.size()));
    CHECK(s.sampleRate == rate);
    CHECK(s.channels == p.channels);
    CHECK(s.frames() == n);  // gapless: the LAME tag's delay and padding were applied
    CHECK(finiteAndInRange(s));
    auto ref = interleave(x);
    for (int c = 0; c < p.channels; ++c) {
        double snr = snrDb(ref, p.channels, 0, s.samples, s.channels, 0, c, std::min(n, s.frames()));
        if (getenv("CODEC_VERBOSE") || snr < minSnr) fprintf(stderr, "  %s: channel %d SNR %.1f dB\n", what, c, snr);
        CHECK(snr >= minSnr);
    }
    double a = toneAmplitude(s.samples, s.channels, 0, 0, std::min<size_t>(s.frames(), rate / 3), 440, rate);
    CHECK_NEAR(a, p.intensity ? 0.3 : 0.4, 0.02);
    // previews: the first frames only, identical to the full decode
    SoundData part;
    CHECK(decodeMp3(e.bytes.data(), e.bytes.size(), part, err, 5000));
    CHECK(part.frames() == 5000);
    bool same = part.samples.size() == 5000u * s.channels;
    for (size_t i = 0; same && i < part.samples.size(); ++i) same = part.samples[i] == s.samples[i];
    CHECK(same);
}

void testMp3Encoders() {
    mp3enc::Params p;
    testMp3Roundtrip(p, 20, "MPEG-1 mono 128 kbps");
    p.scalefactors = true;
    testMp3Roundtrip(p, 20, "MPEG-1 mono, scalefactors");
    p = mp3enc::Params();
    p.channels = 2;
    p.bitrate = 192;
    testMp3Roundtrip(p, 20, "MPEG-1 stereo");
    p.ms = true;
    testMp3Roundtrip(p, 20, "MPEG-1 MS stereo");
    p.intensity = true;
    p.bitrate = 256;
    testMp3Roundtrip(p, 15, "MPEG-1 MS + intensity stereo");
    p = mp3enc::Params();
    p.version = 1;
    p.bitrate = 64;
    testMp3Roundtrip(p, 20, "MPEG-2 22.05 kHz mono");
    p.channels = 2;
    p.ms = true;
    p.bitrate = 96;
    testMp3Roundtrip(p, 18, "MPEG-2 22.05 kHz MS stereo");
    p.intensity = true;
    p.isStart = 14;
    p.isPos = 3;
    p.bitrate = 128;
    testMp3Roundtrip(p, 15, "MPEG-2 22.05 kHz MS + intensity stereo");
    p = mp3enc::Params();
    p.version = 2;
    p.bitrate = 32;
    testMp3Roundtrip(p, 18, "MPEG-2.5 11.025 kHz mono");

    // Layer I
    size_t n = 30000;
    auto x = testSignal(44100, n, 1, 440, false);
    auto bytes = mp3enc::encodeLayer1(x[0], n, 192);
    SoundData s;
    std::string err;
    CHECK(decodeMp3(bytes.data(), bytes.size(), s, err));
    CHECK(s.channels == 1 && s.sampleRate == 44100);
    if (s.frames() > n + 481) {
        double snr = snrDb(x[0], 1, 0, s.samples, 1, 481, 0, n);
        CHECK(snr > 15);
        CHECK_NEAR(toneAmplitude(s.samples, 1, 0, 481, 20000, 440, 44100), 0.4, 0.02);
    } else {
        CHECK(false);
    }
}

// ================================================================ a minimal Ogg Vorbis encoder
namespace vorbisenc {

struct BitsLE {
    std::vector<uint8_t> d;
    size_t n = 0;
    void put(uint32_t v, int bits) {
        for (int i = 0; i < bits; ++i, ++n) {
            if ((n & 7) == 0) d.push_back(0);
            if ((v >> i) & 1) d.back() |= 1 << (n & 7);
        }
    }
    void putCode(uint32_t code, int len) {  // Huffman codewords go MSB first
        for (int i = len - 1; i >= 0; --i) put((code >> i) & 1, 1);
    }
};

uint32_t floatPack(double v) {
    if (v == 0) return 0;
    uint32_t sign = v < 0 ? 0x80000000u : 0;
    double m = std::fabs(v);
    int e = 0;
    while (m < (1 << 20)) {
        m *= 2;
        --e;
    }
    while (m >= (1 << 21)) {
        m /= 2;
        ++e;
    }
    uint32_t mant = (uint32_t)std::floor(m + 0.5);
    if (mant >= (1u << 21)) {
        mant >>= 1;
        ++e;
    }
    return sign | (uint32_t)(e + 788) << 21 | mant;
}
double floatUnpack(uint32_t x) {
    double m = x & 0x1fffff;
    if (x & 0x80000000u) m = -m;
    return std::ldexp(m, (int)((x >> 21) & 0x3ff) - 788);
}

struct Book {
    int dims = 1, entries = 0;
    std::vector<int> lens;
    std::vector<uint32_t> codes;  // MSB first
    bool ordered = false, sparse = false;
    int lookup = 0;
    uint32_t minPacked = 0, deltaPacked = 0;
    int valueBits = 0;
    std::vector<int> mults;
    // codewords by the specification's rule: in entry order, the lowest available codeword
    void assign() {
        codes.assign(entries, 0);
        std::vector<std::pair<uint32_t, int>> taken;
        for (int e = 0; e < entries; ++e) {
            int l = lens[e];
            if (!l) continue;
            for (uint32_t c = 0; c < (1u << l); ++c) {
                bool clash = false;
                for (auto& t : taken) {
                    int m = std::min(l, t.second);
                    if ((c >> (l - m)) == (t.first >> (t.second - m))) {
                        clash = true;
                        break;
                    }
                }
                if (!clash) {
                    codes[e] = c;
                    taken.push_back({c, l});
                    break;
                }
            }
        }
    }
    void write(BitsLE& b) const {
        b.put(0x564342, 24);
        b.put(dims, 16);
        b.put(entries, 24);
        b.put(ordered, 1);
        if (ordered) {
            int cur = lens[0];
            b.put(cur - 1, 5);
            int i = 0;
            while (i < entries) {
                int num = 0;
                while (i + num < entries && lens[i + num] == cur) ++num;
                int bits = 0;
                for (int v = entries - i; v > 0; v >>= 1) ++bits;
                b.put(num, bits);
                i += num;
                ++cur;
            }
        } else {
            b.put(sparse, 1);
            for (int e = 0; e < entries; ++e) {
                if (sparse) {
                    b.put(lens[e] > 0, 1);
                    if (!lens[e]) continue;
                }
                b.put(lens[e] - 1, 5);
            }
        }
        b.put(lookup, 4);
        if (lookup) {
            b.put(minPacked, 32);
            b.put(deltaPacked, 32);
            b.put(valueBits - 1, 4);
            b.put(0, 1);  // sequence_p
            for (int m : mults) b.put(m, valueBits);
        }
    }
    void code(BitsLE& b, int e) const { b.putCode(codes[e], lens[e]); }
};

struct Floor1 {
    int rangeBits = 7;
    std::vector<int> X;  // [0] = 0, [1] = 1 << rangeBits, then 2 per partition
    int parts() const { return ((int)X.size() - 2) / 2; }
};

const int kMult = 2, kRange = 128;  // floor1 multiplier 2

struct Params {
    int channels = 1;
    int residueType = 1;  // 0, 1 (mono) or 2 (any)
    int rate = 44100;
    int startDelay = 0;   // samples encoded before the signal; removed by the granule positions
    size_t pageLimit = 600;
};

double dbValue(int idx) { return std::pow(10.0, (idx - 255) * 7.0 / 256.0); }

struct Encoder {
    Params P;
    Book books[5];
    Floor1 floors[2];
    float fineMin = 0, fineDelta = 0;
    std::vector<std::vector<float>> cosTab[2];

    explicit Encoder(const Params& p) : P(p) {
        // book 0: residue classes (2), ordered
        books[0].entries = 2;
        books[0].lens = {1, 1};
        books[0].ordered = true;
        // book 1: residue pass 0, 2-dimensional, values -15..15 (lookup type 1)
        Book& b1 = books[1];
        b1.dims = 2;
        b1.entries = 31 * 31;
        b1.lens.assign(b1.entries, 10);
        std::vector<int> order(b1.entries);
        for (int i = 0; i < b1.entries; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [](int a, int b) {
            return std::abs(a % 31 - 15) + std::abs(a / 31 - 15) < std::abs(b % 31 - 15) + std::abs(b / 31 - 15);
        });
        for (int i = 0; i < 63; ++i) b1.lens[order[i]] = 9;  // 63 * 2^-9 + 898 * 2^-10 = 1
        b1.lookup = 1;
        b1.minPacked = floatPack(-15);
        b1.deltaPacked = floatPack(1);
        b1.valueBits = 5;
        for (int i = 0; i < 31; ++i) b1.mults.push_back(i);
        // book 2: residue pass 1, 4-dimensional, values -1/3, 0, 1/3 (lookup type 2), sparse
        Book& b2 = books[2];
        b2.dims = 4;
        b2.entries = 81;
        b2.sparse = true;
        b2.lens.assign(81, 7);
        for (int i = 0; i < 47; ++i) b2.lens[(40 + i * 7) % 81] = 6;  // 47 * 2^-6 + 34 * 2^-7 = 1
        b2.lookup = 2;
        b2.minPacked = floatPack(-1.0 / 3);
        b2.deltaPacked = floatPack(1.0 / 3);
        b2.valueBits = 2;
        for (int e = 0; e < 81; ++e)
            for (int i = 0, v = e; i < 4; ++i, v /= 3) b2.mults.push_back(v % 3);
        fineMin = (float)floatUnpack(b2.minPacked);
        fineDelta = (float)floatUnpack(b2.deltaPacked);
        // book 3: floor1 amplitudes 0..127, ordered
        books[3].entries = 128;
        books[3].lens.assign(128, 7);
        books[3].ordered = true;
        // book 4: floor1 class master (2 subclass bits), ordered with three lengths
        books[4].entries = 4;
        books[4].lens = {1, 2, 3, 3};
        books[4].ordered = true;
        for (auto& b : books) b.assign();
        floors[0].rangeBits = 7;
        floors[0].X = {0, 128, 4, 12, 20, 32, 48, 64, 88, 108};
        floors[1].rangeBits = 10;
        floors[1].X = {0, 1024, 32, 96, 160, 256, 384, 512, 700, 860};
        for (int s = 0; s < 2; ++s) {
            int n = s ? 2048 : 256;
            cosTab[s].assign(n / 2, std::vector<float>(n));
            for (int k = 0; k < n / 2; ++k)
                for (int i = 0; i < n; ++i) cosTab[s][k][i] = (float)std::cos(2 * kPi / n * (i + 0.5 + n / 4.0) * (k + 0.5));
        }
    }

    std::vector<uint8_t> identHeader() const {
        BitsLE b;
        b.put(1, 8);
        for (const char* c = "vorbis"; *c; ++c) b.put(*c, 8);
        b.put(0, 32);
        b.put(P.channels, 8);
        b.put(P.rate, 32);
        b.put(0, 32);
        b.put(128000, 32);
        b.put(0, 32);
        b.put(8, 4);   // 256
        b.put(11, 4);  // 2048
        b.put(1, 1);
        return b.d;
    }
    std::vector<uint8_t> commentHeader() const {
        BitsLE b;
        b.put(3, 8);
        for (const char* c = "vorbis"; *c; ++c) b.put(*c, 8);
        const char* vendor = "Exbit test encoder";
        b.put((uint32_t)strlen(vendor), 32);
        for (const char* c = vendor; *c; ++c) b.put(*c, 8);
        b.put(0, 32);
        b.put(1, 1);
        return b.d;
    }
    std::vector<uint8_t> setupHeader() const {
        BitsLE b;
        b.put(5, 8);
        for (const char* c = "vorbis"; *c; ++c) b.put(*c, 8);
        b.put(5 - 1, 8);
        for (auto& bk : books) bk.write(b);
        b.put(0, 6);   // one time-domain transform
        b.put(0, 16);
        b.put(2 - 1, 6);  // two floors
        for (auto& f : floors) {
            b.put(1, 16);
            b.put(f.parts(), 5);
            for (int i = 0; i < f.parts(); ++i) b.put(0, 4);  // all class 0
            b.put(2 - 1, 3);  // class 0: dimensions 2
            b.put(1, 2);      // 1 subclass bit
            b.put(4, 8);      // master book
            b.put(0, 8);      // subclass 0: no book
            b.put(3 + 1, 8);  // subclass 1: book 3
            b.put(kMult - 1, 2);
            b.put(f.rangeBits, 4);
            for (size_t i = 2; i < f.X.size(); ++i) b.put(f.X[i], f.rangeBits);
        }
        b.put(1 - 1, 6);  // one residue
        b.put(P.residueType, 16);
        b.put(0, 24);
        b.put(P.residueType == 2 ? 1024 * P.channels : 1024, 24);
        b.put(32 - 1, 24);
        b.put(2 - 1, 6);
        b.put(0, 8);  // classbook
        b.put(0, 3);  // class 0: no passes
        b.put(0, 1);
        b.put(3, 3);  // class 1: passes 0 and 1
        b.put(0, 1);
        b.put(1, 8);
        b.put(2, 8);
        b.put(2 - 1, 6);  // two mappings (short, long)
        for (int m = 0; m < 2; ++m) {
            b.put(0, 16);
            b.put(0, 1);  // one submap
            if (P.channels == 2) {
                b.put(1, 1);
                b.put(0, 8);  // one coupling step: magnitude 0, angle 1
                b.put(0, 1);
                b.put(1, 1);
            } else {
                b.put(0, 1);
            }
            b.put(0, 2);
            b.put(0, 8);
            b.put(m, 8);  // floor
            b.put(0, 8);  // residue
        }
        b.put(2 - 1, 6);  // two modes
        for (int m = 0; m < 2; ++m) {
            b.put(m, 1);
            b.put(0, 16);
            b.put(0, 16);
            b.put(m, 8);
        }
        b.put(1, 1);
        return b.d;
    }

    // floor1 curve the decoder will compute from the codes (the spec's algorithm, written again)
    static int renderPoint(int x0, int y0, int x1, int y1, int x) {
        int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
        int off = ady * (x - x0) / adx;
        return dy < 0 ? y0 - off : y0 + off;
    }
    static void renderLine(int x0, int y0, int x1, int y1, std::vector<float>& v) {
        int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
        int base = dy / adx, sy = dy < 0 ? base - 1 : base + 1, y = y0, err = 0;
        ady -= std::abs(base) * adx;
        if (x0 < (int)v.size()) v[x0] = (float)dbValue(y);
        for (int x = x0 + 1; x < x1 && x < (int)v.size(); ++x) {
            err += ady;
            if (err >= adx) {
                err -= adx;
                y += sy;
            } else {
                y += base;
            }
            v[x] = (float)dbValue(y);
        }
    }

    // codes a floor following |X| (returns false: channel unused); curve = the decoded floor
    bool floor(BitsLE& b, const Floor1& f, const std::vector<float>& X, std::vector<float>& curve) {
        int half = (int)X.size();
        double peak = 0;
        for (float v : X) peak = std::max(peak, (double)std::fabs(v));
        if (peak < 1e-7) {
            b.put(0, 1);
            return false;
        }
        b.put(1, 1);
        int n = (int)f.X.size();
        int win = half / 32;
        std::vector<int> want(n);
        for (int i = 0; i < n; ++i) {
            double a = 0;
            for (int k = std::max(0, f.X[i] - win); k <= std::min(half - 1, f.X[i] + win); ++k) a = std::max(a, (double)std::fabs(X[k]));
            double target = std::max(a / 6, peak * 1e-4);
            double idx = 255 + std::log10(target) * 256 / 7;
            want[i] = std::max(0, std::min(kRange - 1, (int)std::ceil(idx / kMult)));
        }
        std::vector<int> fin(n), code(n), step(n, 0);
        fin[0] = code[0] = want[0];
        fin[1] = code[1] = want[1];
        step[0] = step[1] = 1;
        for (int i = 2; i < n; ++i) {
            int lo = -1, hi = -1;
            for (int j = 0; j < i; ++j) {
                if (f.X[j] < f.X[i] && (lo < 0 || f.X[j] > f.X[lo])) lo = j;
                if (f.X[j] > f.X[i] && (hi < 0 || f.X[j] < f.X[hi])) hi = j;
            }
            int pred = renderPoint(f.X[lo], fin[lo], f.X[hi], fin[hi], f.X[i]);
            int highroom = kRange - pred, lowroom = pred, room = std::min(highroom, lowroom) * 2;
            int delta = want[i] - pred, val = 0;
            while (true) {
                val = delta > 0 ? 2 * delta : delta < 0 ? -2 * delta - 1 : 0;
                if (val < room || delta == 0) break;
                delta += delta > 0 ? -1 : 1;
            }
            code[i] = val;
            if (val) {
                step[lo] = step[hi] = step[i] = 1;
                fin[i] = (val & 1) ? pred - (val + 1) / 2 : pred + val / 2;
            } else {
                fin[i] = pred;
            }
        }
        b.put(code[0], 7);
        b.put(code[1], 7);
        for (int p = 0; p < f.parts(); ++p) {
            int c0 = code[2 + 2 * p], c1 = code[3 + 2 * p];
            int cval = (c0 ? 1 : 0) | (c1 ? 2 : 0);
            books[4].code(b, cval);
            if (c0) books[3].code(b, c0);
            if (c1) books[3].code(b, c1);
        }
        // the curve
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int c) { return f.X[a] < f.X[c]; });
        curve.assign(half, 0.f);
        int lx = 0, ly = fin[order[0]] * kMult, hx = 0, hy = 0;
        for (int k = 1; k < n; ++k) {
            int i = order[k];
            if (!step[i]) continue;
            hy = fin[i] * kMult;
            hx = f.X[i];
            renderLine(lx, ly, hx, hy, curve);
            lx = hx;
            ly = hy;
        }
        if (hx < half) renderLine(hx, hy, half, hy, curve);
        return true;
    }

    // value v -> pass 0 integer (|q| <= 15) + pass 1 fine step (-1, 0, 1) * 1/3
    void split(double v, int& q, int& fine) const {
        q = (int)std::lround(v);
        q = std::max(-15, std::min(15, q));
        double r = v - q;
        fine = (int)std::lround((r - fineMin) / fineDelta) - 1;
        fine = std::max(-1, std::min(1, fine));
    }

    // residue vectors -> bitstream (classes per partition, two passes), in the decoder's order
    void residue(BitsLE& b, std::vector<std::vector<float>>& vecs, const std::vector<bool>& skip, int size) {
        const int ps = 32;
        int parts = size / ps;
        int ch = (int)vecs.size();
        std::vector<std::vector<int>> q(ch, std::vector<int>(size)), fine(ch, std::vector<int>(size));
        std::vector<std::vector<int>> cls(ch, std::vector<int>(parts, 0));
        for (int c = 0; c < ch; ++c) {
            for (int i = 0; i < size; ++i) split(vecs[c][i], q[c][i], fine[c][i]);
            for (int p = 0; p < parts; ++p)
                for (int i = p * ps; i < (p + 1) * ps; ++i)
                    if (q[c][i] || fine[c][i]) cls[c][p] = 1;
        }
        auto pos = [&](int dim, int k, int j) {  // position in the partition of entry k, component j
            return P.residueType == 0 ? k + j * (ps / dim) : k * dim + j;
        };
        for (int pass = 0; pass < 2; ++pass)
            for (int p = 0; p < parts; ++p) {
                if (pass == 0)
                    for (int c = 0; c < ch; ++c)
                        if (!skip[c]) books[0].code(b, cls[c][p]);
                for (int c = 0; c < ch; ++c) {
                    if (skip[c] || !cls[c][p]) continue;
                    int base = p * ps;
                    if (pass == 0) {
                        for (int k = 0; k < ps / 2; ++k) {
                            int a = q[c][base + pos(2, k, 0)], d = q[c][base + pos(2, k, 1)];
                            books[1].code(b, (a + 15) + 31 * (d + 15));
                        }
                    } else {
                        for (int k = 0; k < ps / 4; ++k) {
                            int e = 0;
                            for (int j = 3; j >= 0; --j) e = e * 3 + fine[c][base + pos(4, k, j)] + 1;
                            books[2].code(b, e);
                        }
                    }
                }
            }
    }

    static float slope(int i, int len) {
        double x = std::sin((i + 0.5) / len * kPi / 2);
        return (float)std::sin(kPi / 2 * x * x);
    }
    static std::vector<float> window(int n, int ln, int rn) {
        std::vector<float> w(n, 0.f);
        int lws = n / 4 - ln / 2, rws = 3 * n / 4 - rn / 2;
        for (int i = 0; i < n; ++i) {
            if (i < lws) w[i] = 0;
            else if (i < lws + ln) w[i] = slope(i - lws, ln);
            else if (i < rws) w[i] = 1;
            else if (i < rws + rn) w[i] = slope(rws + rn - 1 - i, rn);
        }
        return w;
    }

    struct Packet {
        std::vector<uint8_t> data;
        int64_t granule = -1;
    };

    // x: per channel; returns packets (the three headers first) with granule positions
    std::vector<Packet> encode(const std::vector<std::vector<float>>& x, size_t n, const std::vector<bool>& longBlock) {
        std::vector<Packet> out;
        out.push_back({identHeader(), 0});
        out.push_back({commentHeader(), 0});
        out.push_back({setupHeader(), 0});
        int D = P.startDelay;
        auto sampleAt = [&](int c, int64_t t) -> double {
            int64_t i = t - D;
            return (i >= 0 && i < (int64_t)n) ? x[c][(size_t)i] : 0.0;
        };
        int64_t s = 0, center = 0, prevN = 0;
        for (size_t j = 0; j < longBlock.size(); ++j) {
            int N = longBlock[j] ? 2048 : 256, half = N / 2;
            if (j == 0) {
                s = -N / 2;
            } else {
                s = s + 3 * prevN / 4 - N / 4;
            }
            bool prevLong = j == 0 || longBlock[j - 1], nextLong = j + 1 >= longBlock.size() || longBlock[j + 1];
            int ln = longBlock[j] && !prevLong ? 128 : half, rn = longBlock[j] && !nextLong ? 128 : half;
            std::vector<float> w = window(N, ln, rn);
            BitsLE b;
            b.put(0, 1);
            b.put(longBlock[j] ? 1 : 0, 1);
            if (longBlock[j]) {
                b.put(prevLong, 1);
                b.put(nextLong, 1);
            }
            int C = P.channels;
            std::vector<std::vector<float>> X(C, std::vector<float>(half)), curve(C), res(C, std::vector<float>(half));
            std::vector<bool> unused(C);
            const auto& tab = cosTab[longBlock[j] ? 1 : 0];
            for (int c = 0; c < C; ++c) {
                std::vector<double> z(N);
                for (int i = 0; i < N; ++i) z[i] = w[i] * sampleAt(c, s + i);
                for (int k = 0; k < half; ++k) {
                    double acc = 0;
                    const float* ct = tab[k].data();
                    for (int i = 0; i < N; ++i) acc += z[i] * ct[i];
                    X[c][k] = (float)(acc * 4.0 / N);
                }
            }
            for (int c = 0; c < C; ++c) {
                unused[c] = !floor(b, floors[longBlock[j] ? 1 : 0], X[c], curve[c]);
                for (int k = 0; k < half; ++k) res[c][k] = unused[c] ? 0.f : X[c][k] / curve[c][k];
            }
            if (C == 1) {
                std::vector<std::vector<float>> v{res[0]};
                residue(b, v, {unused[0]}, half);
            } else {
                // values on the residue grid (q + fine/3), then square-polar coupling (exact on the grid)
                for (int c = 0; c < C; ++c)
                    for (int k = 0; k < half; ++k) {
                        int q, f;
                        split(res[c][k], q, f);
                        res[c][k] = q + fineMin + fineDelta * (f + 1);
                    }
                std::vector<float> big((size_t)half * 2);
                for (int k = 0; k < half; ++k) {
                    double L = res[0][k], R = res[1][k], m, a;
                    if (L > R) {
                        if (L > 0) { m = L; a = L - R; }
                        else { m = R; a = R - L; }
                    } else {
                        if (R > 0) { m = R; a = L - R; }
                        else { m = L; a = R - L; }
                    }
                    big[2 * k] = (float)m;
                    big[2 * k + 1] = (float)a;
                }
                std::vector<std::vector<float>> v{big};
                residue(b, v, {unused[0] && unused[1]}, half * 2);
            }
            Packet pk;
            pk.data = b.d;
            if (pk.data.empty()) pk.data.push_back(0);
            int64_t newCenter = s + N / 2;
            if (j > 0) center = newCenter;
            else center = 0;
            pk.granule = std::max<int64_t>(0, center - D);  // a page's granule = that of its last packet
            out.push_back(pk);
            prevN = N;
        }
        return out;
    }
};

struct OggWriter {
    uint32_t serial;
    uint32_t seq = 0;
    std::vector<std::vector<uint8_t>> pages;
    explicit OggWriter(uint32_t s) : serial(s) {}

    static uint32_t crc(const uint8_t* p, size_t n) {
        uint32_t c = 0;
        for (size_t i = 0; i < n; ++i) {
            c ^= (uint32_t)p[i] << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
        }
        return c;
    }
    void page(const std::vector<uint8_t>& body, const std::vector<uint8_t>& lacing, int64_t granule, uint8_t flags) {
        std::vector<uint8_t> p = {'O', 'g', 'g', 'S', 0, flags};
        for (int i = 0; i < 8; ++i) p.push_back((uint8_t)((uint64_t)granule >> (8 * i)));
        for (int i = 0; i < 4; ++i) p.push_back((uint8_t)(serial >> (8 * i)));
        for (int i = 0; i < 4; ++i) p.push_back((uint8_t)(seq >> (8 * i)));
        for (int i = 0; i < 4; ++i) p.push_back(0);
        p.push_back((uint8_t)lacing.size());
        p.insert(p.end(), lacing.begin(), lacing.end());
        p.insert(p.end(), body.begin(), body.end());
        uint32_t c = crc(p.data(), p.size());
        for (int i = 0; i < 4; ++i) p[22 + i] = (uint8_t)(c >> (8 * i));
        pages.push_back(p);
        ++seq;
    }
    // packets onto pages: the first alone (BOS), the other headers flushed before the audio
    void write(const std::vector<Encoder::Packet>& packets, size_t limit, int headerPackets) {
        std::vector<uint8_t> body, lacing;
        bool continued = false;
        int64_t granule = -1;
        bool first = true;
        auto flush = [&](bool eos, bool midPacket) {
            uint8_t flags = (continued ? 1 : 0) | (first ? 2 : 0) | (eos ? 4 : 0);
            page(body, lacing, granule, flags);
            body.clear();
            lacing.clear();
            granule = -1;
            first = false;
            continued = midPacket;
        };
        for (size_t i = 0; i < packets.size(); ++i) {
            const auto& d = packets[i].data;
            size_t off = 0;
            while (true) {
                size_t seg = std::min<size_t>(255, d.size() - off);
                if (lacing.size() == 255 || (body.size() >= limit && !lacing.empty())) flush(false, off > 0);
                lacing.push_back((uint8_t)seg);
                body.insert(body.end(), d.begin() + off, d.begin() + off + seg);
                off += seg;
                if (seg < 255) break;
            }
            granule = packets[i].granule;
            if (i == 0 || (int)i == headerPackets - 1) flush(false, false);
            else if (i + 1 == packets.size()) flush(true, false);
        }
    }
};

}  // namespace vorbisenc

std::vector<uint8_t> concatPages(const std::vector<std::vector<uint8_t>>& pages) {
    std::vector<uint8_t> o;
    for (auto& p : pages) o.insert(o.end(), p.begin(), p.end());
    return o;
}

// encodes x (per channel) into an Ogg Vorbis stream; block plan: long with a run of short blocks
std::vector<uint8_t> makeVorbis(const vorbisenc::Params& p, const std::vector<std::vector<float>>& x, size_t n,
                                uint32_t serial, std::vector<std::vector<uint8_t>>* pagesOut = nullptr) {
    vorbisenc::Encoder enc(p);
    std::vector<bool> plan;
    int64_t covered = -1024;  // center of the last block
    int64_t s = 0, prevN = 0;
    while (covered < (int64_t)n + p.startDelay) {
        size_t j = plan.size();
        bool lng = !(j >= 6 && j < 14);
        int N = lng ? 2048 : 256;
        s = j == 0 ? -N / 2 : s + 3 * prevN / 4 - N / 4;
        covered = s + N / 2;
        prevN = N;
        plan.push_back(lng);
    }
    auto packets = enc.encode(x, n, plan);
    packets.back().granule = (int64_t)n;  // the end: cut by the last granule position
    vorbisenc::OggWriter w(serial);
    w.write(packets, p.pageLimit, 3);
    if (pagesOut) *pagesOut = w.pages;
    return concatPages(w.pages);
}

void testVorbisRoundtrip(const vorbisenc::Params& p, double minSnr, const char* what) {
    size_t n = (size_t)(p.rate * 0.9) + 77;
    auto x = testSignal(p.rate, n, p.channels, 440, true);
    std::vector<uint8_t> file = makeVorbis(p, x, n, 0x1234567);
    SoundData s;
    std::string err;
    bool ok = decodeVorbis(file.data(), file.size(), s, err);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  %s: %s\n", what, err.c_str());
        return;
    }
    CHECK(looksLikeVorbis(file.data(), file.size()));
    CHECK(!looksLikeMp3(file.data(), file.size()));
    CHECK(s.sampleRate == p.rate);
    CHECK(s.channels == p.channels);
    CHECK(s.frames() == n);
    CHECK(finiteAndInRange(s));
    auto ref = interleave(x);
    for (int c = 0; c < p.channels; ++c) {
        double snr = snrDb(ref, p.channels, 0, s.samples, s.channels, 0, c, std::min(n, s.frames()));
        if (getenv("CODEC_VERBOSE") || snr < minSnr) fprintf(stderr, "  %s: channel %d SNR %.1f dB\n", what, c, snr);
        CHECK(snr >= minSnr);
    }
    CHECK_NEAR(toneAmplitude(s.samples, s.channels, 0, 0, std::min<size_t>(s.frames(), 15000), 440, p.rate), 0.4, 0.02);
    SoundData part;
    CHECK(decodeVorbis(file.data(), file.size(), part, err, 3000));
    CHECK(part.frames() == 3000);
    bool same = part.samples.size() == 3000u * s.channels;
    for (size_t i = 0; same && i < part.samples.size(); ++i) same = part.samples[i] == s.samples[i];
    CHECK(same);
}

void testVorbisEncoders() {
    vorbisenc::Params p;
    testVorbisRoundtrip(p, 25, "mono, residue 1");
    p.residueType = 0;
    testVorbisRoundtrip(p, 25, "mono, residue 0");
    p.residueType = 2;
    p.startDelay = 300;  // the first page's granule position cuts 300 samples
    testVorbisRoundtrip(p, 25, "mono, residue 2, start trimmed");
    p.channels = 2;
    p.startDelay = 0;
    p.rate = 48000;
    p.pageLimit = 200;  // many packets span pages
    testVorbisRoundtrip(p, 25, "stereo, coupled, residue 2");

    // multiplexed with another logical stream, junk between pages, then a chained second stream
    vorbisenc::Params q;
    q.channels = 2;
    q.residueType = 2;
    size_t n1 = 20000, n2 = 15000;
    auto x1 = testSignal(q.rate, n1, 2, 440, false);
    auto x2 = testSignal(q.rate, n2, 2, 660, false);
    std::vector<std::vector<uint8_t>> pages1, pages2;
    makeVorbis(q, x1, n1, 111, &pages1);
    makeVorbis(q, x2, n2, 222, &pages2);
    vorbisenc::OggWriter other(999);
    std::vector<vorbisenc::Encoder::Packet> otherPackets;
    for (int i = 0; i < 6; ++i) {
        vorbisenc::Encoder::Packet pk;
        pk.data.assign(300 + 40 * i, (uint8_t)(0x80 + i));
        pk.granule = i;
        otherPackets.push_back(pk);
    }
    other.write(otherPackets, 400, 1);
    std::vector<std::vector<uint8_t>> all;
    all.push_back(other.pages[0]);  // BOS pages first: a non-Vorbis stream, then ours
    all.push_back(pages1[0]);
    size_t oi = 1;
    for (size_t i = 1; i < pages1.size(); ++i) {
        all.push_back(pages1[i]);
        if (i % 3 == 0 && oi < other.pages.size()) all.push_back(other.pages[oi++]);
        if (i == 5) all.push_back(std::vector<uint8_t>{'O', 'g', 'g', 'S', 0, 0, 1, 2, 3, 'j', 'u', 'n', 'k'});
    }
    while (oi < other.pages.size()) all.push_back(other.pages[oi++]);
    for (auto& pg : pages2) all.push_back(pg);
    std::vector<uint8_t> file = concatPages(all);
    SoundData s;
    std::string err;
    CHECK(decodeVorbis(file.data(), file.size(), s, err));
    CHECK(s.channels == 2);
    CHECK(s.frames() == n1 + n2);
    if (s.frames() == n1 + n2) {
        auto r1 = interleave(x1), r2 = interleave(x2);
        CHECK(snrDb(r1, 2, 0, s.samples, 2, 0, 0, n1) > 25);
        CHECK(snrDb(r2, 2, 0, s.samples, 2, n1, 1, n2) > 18);
        // the same samples as each stream decoded on its own
        SoundData a, b;
        std::vector<uint8_t> f1 = concatPages(pages1), f2 = concatPages(pages2);
        CHECK(decodeVorbis(f1.data(), f1.size(), a, err) && decodeVorbis(f2.data(), f2.size(), b, err));
        bool same = a.frames() == n1 && b.frames() == n2;
        for (size_t i = 0; same && i < n1 * 2; ++i) same = a.samples[i] == s.samples[i];
        for (size_t i = 0; same && i < n2 * 2; ++i) same = b.samples[i] == s.samples[n1 * 2 + i];
        CHECK(same);
    }
}

// ================================================================ real files on this machine
int64_t lastGranule(const std::vector<uint8_t>& d) {
    for (size_t i = d.size() >= 27 ? d.size() - 27 : 0; i > 0; --i)
        if (memcmp(d.data() + i, "OggS", 4) == 0) {
            uint64_t g = 0;
            for (int k = 7; k >= 0; --k) g = g << 8 | d[i + 6 + k];
            return (int64_t)g;
        }
    return -1;
}

void findFiles(const std::string& dir, std::vector<std::string>& out, int depth) {
    if (depth > 4 || out.size() >= 40) return;
    DIR* dp = opendir(dir.c_str());
    if (!dp) return;
    while (dirent* e = readdir(dp)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            findFiles(path, out, depth + 1);
        } else if (st.st_size < (64 << 20)) {
            std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
            for (auto& ch : ext) ch = (char)tolower(ch);
            if (ext == ".ogg" || ext == ".oga" || ext == ".mp3" || ext == ".mp2") out.push_back(path);
        }
        if (out.size() >= 40) break;
    }
    closedir(dp);
}

bool readFile(const std::string& path, std::vector<uint8_t>& d) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    d.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return true;
}

void testSystemFiles() {
    std::vector<std::string> files;
    findFiles("/usr/share/sounds", files, 0);
    if (const char* extra = getenv("EXBIT_TEST_AUDIO")) findFiles(extra, files, 0);
    int decoded = 0;
    for (auto& path : files) {
        std::vector<uint8_t> d;
        if (!readFile(path, d)) continue;
        bool ogg = looksLikeVorbis(d.data(), d.size());
        bool mp3 = !ogg && looksLikeMp3(d.data(), d.size());
        if (!ogg && !mp3) continue;  // e.g. Ogg Opus/FLAC
        SoundData s;
        std::string err;
        bool ok = ogg ? decodeVorbis(d.data(), d.size(), s, err) : decodeMp3(d.data(), d.size(), s, err);
        if (!ok) fprintf(stderr, "  %s: %s\n", path.c_str(), err.c_str());
        CHECK(ok);
        if (!ok) continue;
        ++decoded;
        CHECK(s.channels >= 1 && s.channels <= 8);
        CHECK(s.sampleRate >= 8000 && s.sampleRate <= 192000);
        CHECK(s.frames() > 0);
        CHECK(finiteAndInRange(s));
        if (ogg) {
            int64_t g = lastGranule(d);
            if (std::llabs((int64_t)s.frames() - g) > 0) fprintf(stderr, "  %s: %zu frames, last granule %lld\n", path.c_str(), s.frames(), (long long)g);
            CHECK(std::llabs((int64_t)s.frames() - g) <= 0);
        }
        // a preview of the start is the start of the full decode
        SoundData part;
        CHECK(ogg ? decodeVorbis(d.data(), d.size(), part, err, 1000) : decodeMp3(d.data(), d.size(), part, err, 1000));
        CHECK(part.frames() == std::min<size_t>(1000, s.frames()));
    }
    printf("decoded %d sound files from this machine\n", decoded);
}

// ================================================================ damaged streams
void testDamage() {
    mp3enc::Params p;
    p.channels = 2;
    p.ms = true;
    size_t n = 20000;
    auto x = testSignal(44100, n, 2, 440, false);
    std::vector<uint8_t> mp3 = mp3enc::encode(p, x, n, true, false).bytes;
    vorbisenc::Params q;
    q.channels = 2;
    q.residueType = 2;
    std::vector<uint8_t> ogg = makeVorbis(q, x, n, 77);
    int survived = 0;
    for (int trial = 0; trial < 120; ++trial) {
        for (int kind = 0; kind < 2; ++kind) {
            std::vector<uint8_t> d = kind ? ogg : mp3;
            int mode = trial % 3;
            if (mode == 0) {  // random byte damage
                for (int k = 0; k < 1 + trial % 20; ++k) d[rng() % d.size()] = (uint8_t)rng();
            } else if (mode == 1) {  // truncated
                d.resize(rng() % d.size());
            } else {  // random bytes overwritten in a run
                size_t at = rng() % d.size(), len = std::min(d.size() - at, (size_t)(rng() % 600));
                for (size_t i = 0; i < len; ++i) d[at + i] = (uint8_t)rng();
            }
            if (kind == 1 && mode == 0 && trial % 2) {  // also corrupt bytes with valid CRCs: rewrite them
                // (pages with a bad CRC are skipped, so recompute the CRCs to reach the decoder)
                for (size_t i = 0; i + 27 < d.size(); ++i) {
                    if (memcmp(d.data() + i, "OggS", 4) != 0) continue;
                    size_t nseg = d[i + 26], body = 0;
                    if (i + 27 + nseg > d.size()) break;
                    for (size_t k = 0; k < nseg; ++k) body += d[i + 27 + k];
                    size_t total = 27 + nseg + body;
                    if (i + total > d.size()) break;
                    for (int k = 0; k < 4; ++k) d[i + 22 + k] = 0;
                    uint32_t c = vorbisenc::OggWriter::crc(d.data() + i, total);
                    for (int k = 0; k < 4; ++k) d[i + 22 + k] = (uint8_t)(c >> (8 * k));
                    i += total - 1;
                }
            }
            SoundData s;
            std::string err;
            bool ok = kind ? decodeVorbis(d.data(), d.size(), s, err) : decodeMp3(d.data(), d.size(), s, err);
            if (!ok || finiteAndInRange(s)) ++survived;
        }
    }
    CHECK(survived == 240);
    // garbage that is neither
    std::vector<uint8_t> junk(5000);
    for (auto& b : junk) b = (uint8_t)rng();
    SoundData s;
    std::string err;
    CHECK(!decodeVorbis(junk.data(), junk.size(), s, err));
    CHECK(!err.empty());
    CHECK(!looksLikeVorbis(junk.data(), junk.size()));
    const uint8_t tiny[3] = {0xFF, 0xFB, 0x90};
    CHECK(!decodeMp3(tiny, 3, s, err));
}

}  // namespace

int main() {
    testImdcts();
    testSynthesisFilterbank();
    testHuffmanTables();
    testMp3Encoders();
    testVorbisEncoders();
    testSystemFiles();
    testDamage();
    TEST_MAIN_END();
}
