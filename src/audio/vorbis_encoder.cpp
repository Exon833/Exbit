// Ogg Vorbis encoder (see vorbis_encoder.h), written from the Vorbis I specification and RFC 3533.
// The decoder it's checked against is ours (vorbis.cpp) and libvorbis.
//
// The stream's configuration (all in the setup header):
//   blocks 256 / 2048; modes 0 (short) and 1 (long), each with its own mapping, floor and residue;
//   floor 1, multiplier 2 (128 levels of 1.1 dB), posts in a binary-subdivision order (each predicted
//   from its neighbours), 4 posts a partition, one Huffman book for the posts' values;
//   residue type 2 over the interleaved channels, partitions of 16 (short) / 32 (long) values in 8
//   classes by their largest value: none, ±1, ±2, ±4, ±8 (vector books of 4 or 2 dimensions), and
//   cascades for more (±59, ±263, ±8168: coarse steps of 17 or 527 first, the remainder after), each
//   class and pass with a book of its own; square-polar coupling of the two channels' integer
//   residues (lossless); channels that sound alike share a floor fitted under the stricter of their
//   two thresholds.
// The psychoacoustic model, per block and channel: an FFT the block's size; partitions of a third of
// a Bark, each a masker as tonal as its spectral flatness says (for long blocks also asked of eight
// short FFTs, which see gliding tones; short blocks also ask the long view of their slot); noise is
// masked 34 dB below tones and 7 dB below noise (19 in short blocks), spread in Bark; the threshold
// in quiet; and for short blocks, no more noise than their quieter first half bears (pre-echo).
// The floor is the quantizer's step, so it's put at sqrt(12 x the allowed noise), moved by the quality.
// The time line: block centres sit 1024 apart (long), a long block may be replaced by 8 short ones
// (centres 128 apart) where there's a transient; the first block is centred on sample 0, each packet
// returns the samples between the previous block's centre and its own, so granule positions are just
// the block centres and the last page's granule is the input's length (decoders cut the rest).
#include "audio/vorbis_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <queue>
#include <thread>

#include "audio/fft.h"
#include "audio/psy.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kShort = 256, kLong = 2048;

int ilog(int64_t v) {
    int n = 0;
    while (v > 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

// ---------------------------------------------------------------- bits, LSB first
struct BitsOut {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) {
        if (bits <= 0) return;
        uint64_t m = bits >= 32 ? 0xffffffffull : ((1ull << bits) - 1);
        acc |= ((uint64_t)v & m) << n;
        n += bits;
        while (n >= 8) {
            buf.push_back((uint8_t)acc);
            acc >>= 8;
            n -= 8;
        }
    }
    void putCodeword(uint32_t code, int len) {  // a Huffman codeword: its first (most significant) bit first
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> (len - 1 - i)) & 1u) << i;
        put(r, len);
    }
    void text(const char* s) {
        for (; *s; ++s) put((uint8_t)*s, 8);
    }
    void flush() {
        if (n > 0) {
            buf.push_back((uint8_t)acc);
            acc = 0;
            n = 0;
        }
    }
};

uint32_t float32Pack(double v) {  // the spec's float format: 21-bit mantissa, exponent biased by 788
    if (v == 0) return 0;
    bool neg = v < 0;
    int e;
    double f = std::frexp(std::fabs(v), &e);
    uint32_t mant = (uint32_t)std::llround(f * (1 << 21));
    int ex = e - 21 + 788;
    if (mant >= (1u << 21)) {
        mant >>= 1;
        ++ex;
    }
    return (neg ? 0x80000000u : 0u) | ((uint32_t)ex << 21) | mant;
}

// ---------------------------------------------------------------- codebooks
struct Book {
    int dims = 1, entries = 0, lookup = 0;
    int K = 0, valueBits = 0;  // vector books: K values a dimension, -(K-1)/2 .. (K-1)/2 times `step`
    double step = 1;
    std::vector<uint64_t> counts;
    std::vector<uint8_t> lens;
    std::vector<uint32_t> codes;

    void scalar(int n) {
        dims = 1;
        entries = n;
        counts.assign(n, 0);
    }
    void vector(int d, int k, double s) {
        dims = d;
        K = k;
        step = s;
        lookup = 1;
        valueBits = ilog(k - 1);
        entries = 1;
        for (int i = 0; i < d; ++i) entries *= k;
        counts.assign(entries, 0);
    }
    void use(int entry, BitsOut* bw) {
        if (!bw) counts[entry]++;
        else bw->putCodeword(codes[entry], lens[entry]);
    }
    // Huffman code lengths from the counts (unused entries get none), then the codewords the way
    // the decoder assigns them (spec 3.2.1)
    bool build() {
        std::vector<uint64_t> c = counts;
        int used = 0;
        for (uint64_t x : c) used += x > 0;
        for (int e = 0; e < entries && used < 2; ++e)  // (a book needs two codewords at least)
            if (!c[e]) {
                c[e] = 1;
                ++used;
            }
        for (;;) {
            lens.assign(entries, 0);
            struct Node { uint64_t w; int id; };
            auto cmp = [](const Node& a, const Node& b) { return a.w > b.w || (a.w == b.w && a.id > b.id); };
            std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
            std::vector<int> parent;
            for (int e = 0; e < entries; ++e) {
                parent.push_back(-1);
                if (c[e]) pq.push({c[e], e});
            }
            while (pq.size() > 1) {
                Node a = pq.top();
                pq.pop();
                Node b = pq.top();
                pq.pop();
                int id = (int)parent.size();
                parent.push_back(-1);
                parent[a.id] = id;
                parent[b.id] = id;
                pq.push({a.w + b.w, id});
            }
            int maxLen = 0;
            for (int e = 0; e < entries; ++e) {
                if (!c[e]) continue;
                int d = 0;
                for (int p = parent[e]; p >= 0; p = parent[p]) ++d;
                lens[e] = (uint8_t)d;
                maxLen = std::max(maxLen, d);
            }
            if (maxLen <= 24) break;
            for (auto& x : c)  // too deep: flatten the statistics and try again
                if (x) x = (x >> 1) | 1;
        }
        codes.assign(entries, 0);
        uint32_t avail[33] = {0};
        bool first = true;
        for (int e = 0; e < entries; ++e) {
            int l = lens[e];
            if (!l) continue;
            uint32_t cw;
            if (first) {
                cw = 0;
                for (int j = 1; j <= l; ++j) avail[j] = 1u << (32 - j);
                first = false;
            } else {
                int z = l;
                while (z > 0 && !avail[z]) --z;
                if (z == 0) return false;
                cw = avail[z];
                avail[z] = 0;
                for (int y = z + 1; y <= l; ++y) avail[y] = cw + (1u << (32 - y));
            }
            codes[e] = cw >> (32 - l);
        }
        return true;
    }
    void write(BitsOut& b) const {
        b.put(0x564342, 24);
        b.put((uint32_t)dims, 16);
        b.put((uint32_t)entries, 24);
        b.put(0, 1);  // not ordered
        bool sparse = std::find(lens.begin(), lens.end(), 0) != lens.end();
        b.put(sparse, 1);
        for (int e = 0; e < entries; ++e) {
            if (sparse) {
                b.put(lens[e] > 0, 1);
                if (!lens[e]) continue;
            }
            b.put(lens[e] - 1u, 5);
        }
        b.put((uint32_t)lookup, 4);
        if (lookup == 1) {
            b.put(float32Pack(-(K - 1) / 2 * step), 32);
            b.put(float32Pack(step), 32);
            b.put((uint32_t)valueBits - 1, 4);
            b.put(0, 1);  // not cumulative
            for (int i = 0; i < K; ++i) b.put((uint32_t)i, valueBits);
        }
    }
};

// ---------------------------------------------------------------- residue classes
struct VqDef { int dims, K, step; };
const VqDef kVq[7] = {{4, 3, 1}, {4, 5, 1}, {2, 9, 1}, {2, 17, 1}, {2, 7, 17}, {1, 31, 17}, {1, 31, 527}};
constexpr int kClasses = 8;
const int kClassMax[kClasses] = {0, 1, 2, 4, 8, 59, 263, 8168};
const int kClassPasses[kClasses] = {0, 1, 1, 1, 1, 2, 2, 3};
const int kClassBooks[kClasses][3] = {{-1, -1, -1}, {0, -1, -1}, {1, -1, -1}, {2, -1, -1},
                                      {3, -1, -1},  {4, 3, -1},  {5, 3, -1},  {6, 5, 3}};
constexpr int kMaxResidue = 8168;
// each class's passes have books of their own (their statistics differ): book of (class, pass)
constexpr int kVqBooks = 11;  // 1 + 1 + 1 + 1 + 2 + 2 + 3
int vqBookOf(int c, int pass) {
    int n = 0;
    for (int k = 1; k < c; ++k) n += kClassPasses[k];
    return n + pass;
}
constexpr int kBooksPer = 2 + kVqBooks;  // per block size: floor, class words, the vector books

// ---------------------------------------------------------------- floor 1
const float* dbTable() {
    static float t[256];
    static bool init = [] {
        for (int i = 0; i < 256; ++i) t[i] = (float)std::pow(10.0, (i - 255) * 7.0 / 256.0);
        return true;
    }();
    (void)init;
    return t;
}

int renderPoint(int x0, int y0, int x1, int y1, int x) {
    int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
    if (adx == 0) return y0;
    int err = ady * (x - x0);
    int off = err / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

void renderLine(int x0, int y0, int x1, int y1, float* v, int n) {
    const float* db = dbTable();
    int dy = y1 - y0, adx = x1 - x0;
    if (adx <= 0) return;
    int ady = std::abs(dy);
    int base = dy / adx;
    int sy = dy < 0 ? base - 1 : base + 1;
    ady -= std::abs(base) * adx;
    int x = x0, y = y0, err = 0;
    auto put = [&](int xx, int yy) {
        if (xx < n) v[xx] = db[yy < 0 ? 0 : yy > 255 ? 255 : yy];
    };
    put(x, y);
    int end = std::min(x1, n);
    for (x = x0 + 1; x < end; ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        put(x, y);
    }
}

struct FloorCfg {
    int half = 0, rangeBits = 0, classDim = 4, parts = 0;
    static constexpr int mult = 2, range = 128;
    std::vector<int> X, sorted, low, high;
    std::vector<int> nbLo, nbHi;  // the lines each post stands for
    void init(int h, int rb, const std::vector<int>& interior) {
        half = h;
        rangeBits = rb;
        X = {0, 1 << rb};
        // binary subdivision order: each new post lies between two already placed
        std::queue<std::pair<int, int>> q;
        q.push({0, (int)interior.size()});
        while (!q.empty()) {
            auto [a, b] = q.front();
            q.pop();
            if (a >= b) continue;
            int mid = (a + b) / 2;
            X.push_back(interior[mid]);
            q.push({a, mid});
            q.push({mid + 1, b});
        }
        int n = (int)X.size();
        parts = (n - 2) / classDim;
        sorted.resize(n);
        for (int i = 0; i < n; ++i) sorted[i] = i;
        std::sort(sorted.begin(), sorted.end(), [&](int a, int b) { return X[a] < X[b]; });
        low.assign(n, 0);
        high.assign(n, 1);
        for (int i = 2; i < n; ++i) {
            int lo = -1, hi = -1;
            for (int j = 0; j < i; ++j) {
                if (X[j] < X[i] && (lo < 0 || X[j] > X[lo])) lo = j;
                if (X[j] > X[i] && (hi < 0 || X[j] < X[hi])) hi = j;
            }
            low[i] = lo;
            high[i] = hi;
        }
        nbLo.assign(n, 0);
        nbHi.assign(n, 0);
        for (int k = 0; k < n; ++k) {
            int i = sorted[k];
            int prev = k > 0 ? X[sorted[k - 1]] : X[i], next = k + 1 < n ? X[sorted[k + 1]] : X[i];
            nbLo[i] = std::min(half - 1, (prev + X[i] + 1) / 2);
            nbHi[i] = std::max(nbLo[i] + 1, std::min(half, (X[i] + next + 1) / 2));
        }
    }
};

struct FloorCode {
    int final[64];
    int val[64];
    uint8_t step[64];
};

// posts fitted under the target (in floor steps of 1.1 dB, per line), coded against their predictions
void fitFloor(const FloorCfg& F, const float* target, FloorCode& fc) {
    int n = (int)F.X.size();
    int want[64] = {0};
    for (int i = 0; i < n; ++i) {
        float m = 1e9f;
        for (int k = F.nbLo[i]; k < F.nbHi[i]; ++k) m = std::min(m, target[k]);
        want[i] = std::clamp((int)std::floor(m), 0, FloorCfg::range - 1);
    }
    fc.final[0] = want[0];
    fc.final[1] = want[1];
    fc.step[0] = fc.step[1] = 1;
    fc.val[0] = fc.val[1] = 0;
    for (int i = 2; i < n; ++i) {
        int lo = F.low[i], hi = F.high[i];
        int pred = renderPoint(F.X[lo], fc.final[lo], F.X[hi], fc.final[hi], F.X[i]);
        int d = want[i] - pred;
        if (std::abs(d) <= 1) {  // close enough: the line through the neighbours (no bits)
            fc.val[i] = 0;
            fc.final[i] = pred;
            fc.step[i] = 0;
            continue;
        }
        int highroom = FloorCfg::range - pred, lowroom = pred;
        int room = std::min(highroom, lowroom) * 2;
        int v;
        if (d > 0) v = 2 * d < room ? 2 * d : d + lowroom;
        else v = 2 * -d - 1 < room ? 2 * -d - 1 : -d + highroom - 1;
        fc.val[i] = v;
        fc.final[i] = want[i];
        fc.step[i] = fc.step[lo] = fc.step[hi] = 1;
    }
}

// the curve the decoder draws from the posts
void renderFloor(const FloorCfg& F, const FloorCode& fc, float* out) {
    int n = (int)F.X.size();
    int lx = 0, ly = fc.final[F.sorted[0]] * FloorCfg::mult, hx = 0, hy = 0;
    for (int k = 1; k < n; ++k) {
        int i = F.sorted[k];
        if (!fc.step[i]) continue;
        hy = fc.final[i] * FloorCfg::mult;
        hx = F.X[i];
        renderLine(lx, ly, hx, hy, out, F.half);
        lx = hx;
        ly = hy;
    }
    if (hx < F.half) renderLine(hx, hy, F.half, hy, out, F.half);
}

// ---------------------------------------------------------------- MDCT
// X[k] = 4/N sum x[n] cos(2 pi / N (n + 1/2 + N/4)(k + 1/2)): folded into a DCT-IV of N/2 points,
// computed with an N/4-point complex FFT
struct Mdct {
    int N = 0;
    std::unique_ptr<fft::Plan> plan;
    std::vector<fft::cd> pre, post;
    void init(int n) {
        N = n;
        int M = N / 2, Q = N / 4;
        plan = std::make_unique<fft::Plan>(Q);
        pre.resize(Q);
        post.resize(Q);
        for (int k = 0; k < Q; ++k) {
            pre[k] = std::polar(1.0, -kPi * k / M);
            post[k] = std::polar(4.0 / N, -kPi * (k + 0.25) / M);
        }
    }
    void forward(const double* x, double* X) const {
        int M = N / 2, Q = N / 4, h = M / 2;  // h = N/4
        std::vector<double> r(M);
        std::vector<fft::cd> z(Q);
        for (int j = 0; j < h; ++j) r[j] = -x[3 * h - 1 - j] - x[3 * h + j];
        for (int j = h; j < M; ++j) r[j] = x[j - h] - x[3 * h - 1 - j];
        for (int n = 0; n < Q; ++n) z[n] = fft::cd(r[2 * n], r[M - 1 - 2 * n]) * pre[n];
        plan->forward(z.data());
        for (int k = 0; k < Q; ++k) {
            fft::cd t = z[k] * post[k];
            X[2 * k] = t.real();
            X[M - 1 - 2 * k] = -t.imag();
        }
    }
};

// ---------------------------------------------------------------- Ogg
uint32_t oggCrc(const uint8_t* p, size_t n) {
    static uint32_t T[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
            T[i] = r;
        }
        return true;
    }();
    (void)init;
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ T[((crc >> 24) ^ p[i]) & 0xff];
    return crc;
}

struct OggWriter {
    std::vector<uint8_t>& out;
    uint32_t serial = 0x54444157;  // "TDAW"
    uint32_t seq = 0;
    std::vector<uint8_t> body, lacing;
    int64_t granule = -1;  // of the last packet finished on the page
    bool first = true, continued = false;
    explicit OggWriter(std::vector<uint8_t>& o) : out(o) {}

    void page(bool eos, int64_t eosGranule = -1) {
        if (lacing.empty() && !eos) return;
        size_t at = out.size();
        out.insert(out.end(), {'O', 'g', 'g', 'S', 0});
        out.push_back((uint8_t)((continued ? 1 : 0) | (first ? 2 : 0) | (eos ? 4 : 0)));
        int64_t g = eos ? eosGranule : granule;
        for (int i = 0; i < 8; ++i) out.push_back((uint8_t)((uint64_t)g >> (8 * i)));
        for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(serial >> (8 * i)));
        for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(seq >> (8 * i)));
        for (int i = 0; i < 4; ++i) out.push_back(0);  // CRC, below
        out.push_back((uint8_t)lacing.size());
        out.insert(out.end(), lacing.begin(), lacing.end());
        out.insert(out.end(), body.begin(), body.end());
        uint32_t crc = oggCrc(out.data() + at, out.size() - at);
        for (int i = 0; i < 4; ++i) out[at + 22 + i] = (uint8_t)(crc >> (8 * i));
        ++seq;
        first = false;
        continued = false;
        body.clear();
        lacing.clear();
        granule = -1;
    }
    // a packet; `granuleAfter` is the stream position once it's decoded
    void packet(const std::vector<uint8_t>& p, int64_t granuleAfter) {
        size_t n = p.size(), off = 0;
        for (;;) {
            if (lacing.size() == 255) {  // page full in the middle of the packet: it goes on on the next
                page(false);
                continued = true;
            }
            size_t len = std::min<size_t>(255, n - off);
            lacing.push_back((uint8_t)len);
            body.insert(body.end(), p.begin() + off, p.begin() + off + len);
            off += len;
            if (len < 255) break;
        }
        granule = granuleAfter;
    }
};

// ---------------------------------------------------------------- the encoder
struct BlockPos {
    int64_t centre = 0;
    bool isLong = true, prevLong = true, nextLong = true;
};

struct BlockOut {
    bool used[2] = {false, false};
    FloorCode floor[2];
    std::vector<int> res;       // the coupled residue, channels interleaved (type 2)
    std::vector<uint8_t> cls;   // class of each partition
};

class Encoder {
public:
    int sr = 44100, nch = 2;
    double quality = 6;
    FloorCfg floors[2];  // [0 short, 1 long]
    Book books[2 * kBooksPer];  // per block size b: kBooksPer b + 0 floor, + 1 class words, + 2... vector books
    int partSize[2] = {16, 32};
    std::vector<double> slope[2];  // rising window halves: 128 and 1024 long
    Mdct mdct[2];
    std::unique_ptr<fft::Plan> fftPlan[2];
    std::vector<double> hann[2];
    psy::Partitions part[2];
    std::vector<int> sfmLo[2], sfmHi[2];  // the bins each partition's flatness is measured over
    std::vector<int> shortToLong, longToShort;  // partitions at the same Bark
    // The model's constants (tuned on test signals against libvorbis at -q 6 for bitrate and SNR):
    double offsetDb = 0;                            // set by the quality (setup)
    double tonalDb = 34, noiseDb = 7, zSlope = 0;  // noise masked this far (+ zSlope z) below a tone / noise
    double sfmNoise = -2, sfmTone = -5;            // spectral flatness (dB) taken as noise .. as a tone
    double shortExtraDb = 12;  // short blocks: their noise maskers count for less (coarse; transients)
    int cutoffLine[2] = {kShort / 2, kLong / 2};

    void setup(int rate, int channels, double q);
    bool encode(const float* in, int64_t frames, int stride, std::vector<uint8_t>& out, std::string& error,
                const std::function<bool(double)>& progress);

private:
    const float* src = nullptr;
    int64_t srcFrames = 0;
    int stride = 1;
    float sample(int c, int64_t i) const {
        if (i < 0 || i >= srcFrames) return 0.0f;
        float v = src[i * stride + c];
        return v == v ? std::clamp(v, -8.0f, 8.0f) : 0.0f;
    }
    std::vector<BlockPos> layout(int64_t frames);
    void tonality(const double* pw, int bf, double* tb) const;
    void longTonality(const double* x, const double* pw, double* tb) const;
    void allowedNoise(const double* x, int bf, int c, int64_t centre, double* allowed) const;
    void analyze(const BlockPos& b, BlockOut& o) const;
    void emit(const BlockPos& b, const BlockOut& o, BitsOut* bw);
    void headers(BitsOut& id, BitsOut& comment, BitsOut& setup) const;
};

void Encoder::setup(int rate, int channels, double q) {
    sr = rate;
    nch = channels >= 2 ? 2 : 1;
    quality = std::clamp(q, -1.0, 10.0);
    // the noise allowed, against the model's threshold: lower for higher quality
    offsetDb = 2.5 * (quality - 6.0) + 0.25;
    // floor posts (lines of the half block): denser low, about a third of an octave high up
    floors[0].init(kShort / 2, 7, {1, 2, 3, 4, 6, 8, 10, 12, 15, 18, 22, 26, 31, 37, 44, 52, 62, 74, 88, 106});
    floors[1].init(kLong / 2, 10, {2,   4,   6,   8,   11,  14,  18,  22,  27,  32,  38,  45,  52,  60,
                                   68,  78,  88,  100, 112, 126, 140, 156, 172, 192, 212, 236, 260, 288,
                                   320, 352, 388, 428, 472, 520, 576, 640, 704, 776, 856, 944});
    for (int b = 0; b < 2; ++b) {
        Book* bk = books + kBooksPer * b;
        bk[0].scalar(FloorCfg::range);
        bk[1].scalar(kClasses * kClasses);
        bk[1].dims = 2;  // (class words: two partitions each)
        for (int c = 1; c < kClasses; ++c)
            for (int j = 0; j < kClassPasses[c]; ++j) {
                const VqDef& d = kVq[kClassBooks[c][j]];
                bk[2 + vqBookOf(c, j)].vector(d.dims, d.K, d.step);
            }
        int N = b ? kLong : kShort, half = N / 2;
        slope[b].resize(half);
        for (int i = 0; i < half; ++i) {
            double x = std::sin((i + 0.5) / half * kPi / 2);
            slope[b][i] = std::sin(kPi / 2 * x * x);
        }
        mdct[b].init(N);
        fftPlan[b] = std::make_unique<fft::Plan>(N);
        hann[b].resize(N);
        for (int i = 0; i < N; ++i) hann[b][i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N);
        part[b].build(N, sr);
        if (b == 1) {
            shortToLong.assign(part[0].n, 0);
            for (int q = 0; q < part[0].n; ++q) {
                int best = 0;
                for (int p = 0; p < part[1].n; ++p)
                    if (std::fabs(part[1].z[p] - part[0].z[q]) < std::fabs(part[1].z[best] - part[0].z[q])) best = p;
                shortToLong[q] = best;
            }
            longToShort.assign(part[1].n, 0);
            for (int p = 0; p < part[1].n; ++p) {
                int best = 0;
                for (int q = 0; q < part[0].n; ++q)
                    if (std::fabs(part[0].z[q] - part[1].z[p]) < std::fabs(part[0].z[best] - part[1].z[p])) best = q;
                longToShort[p] = best;
            }
        }
        sfmLo[b].resize(part[b].n);
        sfmHi[b].resize(part[b].n);
        for (int q = 0; q < part[b].n; ++q) {  // at least 9 bins
            int lo = part[b].first[q], hi = part[b].last[q];
            while (hi - lo < 9) {
                if (lo > 1) --lo;
                if (hi - lo < 9 && hi < N / 2 + 1) ++hi;
            }
            sfmLo[b][q] = lo;
            sfmHi[b][q] = hi;
        }
        // nothing above 20 kHz (and the top 2 % of the band); lower for low qualities (as libvorbis)
        double cut = std::min(std::min(20000.0, 14000.0 + 1000.0 * std::max(quality, 0.0)), 0.49 * sr);
        cutoffLine[b] = std::min(half, (int)std::ceil(cut / (sr / 2.0) * half));
    }
}

// block centres: 1024 apart; a long block replaced by eight short ones where there's a transient
std::vector<BlockPos> Encoder::layout(int64_t frames) {
    const int seg = 128;
    int64_t slots = frames / 1024 + 2;
    int64_t nseg = (slots + 1) * 1024 / seg + 2;
    std::vector<uint8_t> attack((size_t)nseg, 0);
    for (int c = 0; c < nch; ++c) {
        std::vector<double> e((size_t)nseg, 0.0);
        float prev = 0;
        for (int64_t s = 0; s < nseg; ++s) {
            double acc = 0;
            for (int i = 0; i < seg; ++i) {
                float x = sample(c, s * seg + i);
                double d = x - prev;
                prev = x;
                acc += d * d;
            }
            e[(size_t)s] = acc;
        }
        for (int64_t s = 4; s < nseg; ++s) {  // (the piece just before is skipped: attacks may take ~5 ms)
            double before = std::max(std::max(e[(size_t)s - 2], e[(size_t)s - 3]), e[(size_t)s - 4]);
            if (e[(size_t)s] > 6.0 * before && e[(size_t)s] > seg * 1e-6) attack[(size_t)s] = 1;
        }
    }
    std::vector<BlockPos> blocks;
    blocks.push_back({0, true, true, true});
    for (int64_t m = 1; blocks.back().centre < frames; ++m) {
        // a transient at t is in long blocks floor(t / 1024) (late in it: its noise would spread over
        // all of it before t) and the next one (early, harmless unless 768 or more into the slot)
        bool isShort = false;
        for (int64_t t = 1024 * m - 256; t < 1024 * m + 1024; t += seg)
            if (t >= 0 && t / seg < nseg && attack[(size_t)(t / seg)]) isShort = true;
        if (!isShort) {
            blocks.push_back({1024 * m, true, true, true});
            continue;
        }
        for (int i = 0; i < 8 && blocks.back().centre < frames; ++i) blocks.push_back({1024 * m - 448 + 128 * i, false, true, true});
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        blocks[i].prevLong = i == 0 || blocks[i - 1].isLong;
        blocks[i].nextLong = i + 1 == blocks.size() || blocks[i + 1].isLong;
    }
    return blocks;
}

// The allowed noise per MDCT line (in MDCT energy) of one channel's block (x: the block's samples):
// the energy per partition spread in Bark, lowered by 5.5 dB where the spectrum is noise-like and by
// 14.5 + z dB where it's tonal (by its flatness: geometric over arithmetic mean around the partition,
// as Johnston's model), and the threshold in quiet.
// how tonal each partition of a power spectrum is: its flatness (geometric over arithmetic mean
// around it) from that of noise (0) to that of tones (1)
void Encoder::tonality(const double* pw, int bf, double* tb) const {
    const psy::Partitions& P = part[bf];
    const int nb = P.bins;
    std::vector<double> lg(nb);
    for (int j = 0; j < nb; ++j) lg[j] = std::log(pw[j] + 1e-12);
    for (int p = 0; p < P.n; ++p) {
        double am = 0, gm = 0;
        int lo = sfmLo[bf][p], hi = sfmHi[bf][p];
        for (int j = lo; j < hi; ++j) {
            am += pw[j];
            gm += lg[j];
        }
        am /= hi - lo;
        gm = std::exp(gm / (hi - lo));
        double sfmDb = am > 1e-12 ? 10 * std::log10(std::max(gm / am, 1e-10)) : 0.0;
        tb[p] = std::clamp((sfmDb - sfmNoise) / (sfmTone - sfmNoise), 0.0, 1.0);
    }
}

// the tonality of a long block (x: its samples, pw: its power spectrum): tones that move (glides,
// vibrato) smear in a long FFT and look like noise, so short FFTs over the block's middle are asked
// too, and the more tonal of the two counts
void Encoder::longTonality(const double* x, const double* pw, double* tb) const {
    const psy::Partitions& P = part[1];
    const psy::Partitions& S = part[0];
    tonality(pw, 1, tb);
    std::vector<double> sa(kShort), spw(S.bins), stb(S.n), avg(S.n, 0.0);
    std::vector<fft::cd> SX(S.bins), work;
    for (int w = 0; w < 8; ++w) {
        const double* xs = x + kLong / 4 + 128 * w - kShort / 2 + 64;
        for (int i = 0; i < kShort; ++i) sa[i] = xs[i] * hann[0][i];
        fft::realPair(*fftPlan[0], sa.data(), nullptr, SX.data(), nullptr, work);
        for (int j = 0; j < S.bins; ++j) spw[j] = std::norm(SX[j]);
        tonality(spw.data(), 0, stb.data());
        for (int q = 0; q < S.n; ++q) avg[q] += stb[q] / 8;
    }
    for (int p = 0; p < P.n; ++p) tb[p] = std::max(tb[p], avg[longToShort[p]]);
}

void Encoder::allowedNoise(const double* x, int bf, int c, int64_t centre, double* allowed) const {
    const int N = bf ? kLong : kShort, nb = N / 2 + 1;
    const psy::Partitions& P = part[bf];
    std::vector<double> a(N), pw(nb), e(P.n, 0.0), tb(P.n);
    std::vector<fft::cd> X(nb), work;
    for (int i = 0; i < N; ++i) a[i] = x[i] * hann[bf][i];
    fft::realPair(*fftPlan[bf], a.data(), nullptr, X.data(), nullptr, work);
    for (int j = 0; j < nb; ++j) {
        pw[j] = std::norm(X[j]);
        e[P.partOf[j]] += pw[j];
    }
    if (bf) {
        longTonality(x, pw.data(), tb.data());
    } else {
        // short blocks are too coarse in frequency to tell tones: the long view of their slot (the
        // 2048 samples around it) is asked too
        tonality(pw.data(), 0, tb.data());
        int64_t m = (centre + 512) >> 10;
        std::vector<double> lx(kLong), la(kLong), lpw(kLong / 2 + 1), ltb(part[1].n);
        std::vector<fft::cd> LX(kLong / 2 + 1);
        for (int i = 0; i < kLong; ++i) lx[i] = sample(c, 1024 * m - kLong / 2 + i);
        for (int i = 0; i < kLong; ++i) la[i] = lx[i] * hann[1][i];
        fft::realPair(*fftPlan[1], la.data(), nullptr, LX.data(), nullptr, work);
        for (int j = 0; j <= kLong / 2; ++j) lpw[j] = std::norm(LX[j]);
        longTonality(lx.data(), lpw.data(), ltb.data());
        for (int q = 0; q < P.n; ++q) tb[q] = std::max(tb[q], ltb[shortToLong[q]]);
    }
    // each partition as a masker: how far below it noise is masked (more for tones)
    std::vector<double> me(P.n);
    for (int p = 0; p < P.n; ++p) {
        double t = tb[p];
        double snr = t * (tonalDb + zSlope * P.z[p]) + (1 - t) * (noiseDb + (bf ? 0.0 : shortExtraDb));
        me[p] = e[p] * std::pow(10.0, -snr / 10);
    }
    std::vector<double> thrBin(nb);
    for (int q = 0; q < P.n; ++q) {
        double ecb = 0;
        const double* sp = &P.spread[(size_t)q * P.n];
        for (int p = P.spreadLo[q]; p < P.spreadHi[q]; ++p) ecb += sp[p] * me[p];
        double thr = std::max(ecb * P.rnorm[q], P.athE[q]);
        double perBin = thr / (P.last[q] - P.first[q]);
        for (int j = P.first[q]; j < P.last[q]; ++j) thrBin[j] = perBin;
    }
    // FFT (Hann) energy to MDCT energy (4/N scale, power-complementary window): 32 / (3 N^2)
    double k = 32.0 / (3.0 * N * N) * std::pow(10.0, -offsetDb / 10);
    if (!bf) {  // pre-echo: the noise spreads over the whole block, so not much above its quieter first half
        double e1 = 0, e2 = 0;
        for (int i = 0; i < N / 2; ++i) e1 += a[i] * a[i];
        for (int i = N / 2; i < N; ++i) e2 += a[i] * a[i];
        if (e2 > 0) k *= std::clamp(4.0 * e1 / e2, 1e-3, 1.0);
    }
    for (int i = 0; i < N / 2; ++i) allowed[i] = thrBin[i] * k;
}

void Encoder::analyze(const BlockPos& b, BlockOut& o) const {
    const int bf = b.isLong, N = bf ? kLong : kShort, half = N / 2;
    const FloorCfg& F = floors[bf];
    // the window (as the decoder's): short slopes next to short blocks
    int ln = bf && !b.prevLong ? kShort / 2 : half, rn = bf && !b.nextLong ? kShort / 2 : half;
    const std::vector<double>& ls = slope[ln == half ? bf : 0];
    const std::vector<double>& rs = slope[rn == half ? bf : 0];
    int lws = N / 4 - ln / 2, lwe = N / 4 + ln / 2, rws = 3 * N / 4 - rn / 2, rwe = 3 * N / 4 + rn / 2;
    std::vector<double> x(N), xw(N), spec[2], allowed[2];
    double peak[2] = {0, 0};
    for (int c = 0; c < nch; ++c) {
        for (int i = 0; i < N; ++i) x[i] = sample(c, b.centre - half + i);
        for (int i = 0; i < N; ++i) {
            double w = i < lws ? 0.0 : i < lwe ? ls[i - lws] : i < rws ? 1.0 : i < rwe ? rs[rwe - 1 - i] : 0.0;
            xw[i] = x[i] * w;
        }
        spec[c].resize(half);
        mdct[bf].forward(xw.data(), spec[c].data());
        for (int i = cutoffLine[bf]; i < half; ++i) spec[c][i] = 0;
        for (int i = 0; i < half; ++i) peak[c] = std::max(peak[c], std::fabs(spec[c][i]));
        allowed[c].resize(half);
        allowedNoise(x.data(), bf, c, b.centre, allowed[c].data());
    }
    // channels that sound alike go under the stricter of their two thresholds with one floor (their
    // residues then line up for the coupling); different ones keep their own
    bool share = nch == 2 && peak[0] > 0 && peak[1] > 0;
    if (share) {
        double d = 0;
        for (int i = 0; i < half; ++i) d += std::fabs(std::log10((allowed[0][i] + 1e-30) / (allowed[1][i] + 1e-30)));
        share = 10 * d / half < 3.0;  // (within 3 dB on average)
    }
    if (share)
        for (int i = 0; i < half; ++i) allowed[0][i] = allowed[1][i] = std::min(allowed[0][i], allowed[1][i]);
    std::vector<float> target(half), curve(half);
    std::vector<int> q[2];
    for (int c = 0; c < nch; ++c) {
        q[c].assign(half, 0);
        if (c == 1 && share) {
            o.floor[1] = o.floor[0];
        } else {
            for (int i = 0; i < half; ++i) {
                // the floor is the quantizer's step: its noise is about step^2 / 12
                double f = std::sqrt(12.0 * allowed[c][i]);
                double a = std::fabs(spec[c][i]);
                if (share) a = std::max(a, std::fabs(spec[1][i]));
                f = std::max(f, a / (kMaxResidue - 0.5));  // (the residue's range)
                target[i] = (float)((255.0 + std::log10(std::max(f, 1e-30)) * 256.0 / 7.0) / 2.0);
            }
            fitFloor(F, target.data(), o.floor[c]);
        }
        renderFloor(F, o.floor[c], curve.data());
        bool any = false;
        for (int i = 0; i < half; ++i) {
            int v = (int)std::lround(spec[c][i] / curve[i]);
            v = std::clamp(v, -kMaxResidue, kMaxResidue);
            q[c][i] = v;
            any = any || v != 0;
        }
        o.used[c] = any;
    }
    // coupling (square polar, on the integers: lossless) and interleaving
    o.res.assign((size_t)half * nch, 0);
    if (nch == 1) {
        for (int i = 0; i < half; ++i) o.res[i] = q[0][i];
    } else {
        for (int i = 0; i < half; ++i) {
            int l = q[0][i], r = q[1][i], m, a;
            if (std::abs(l) > std::abs(r)) {
                m = l;
                a = l > 0 ? l - r : r - l;
            } else {
                m = r;
                a = r > 0 ? l - r : r - l;
            }
            if (std::abs(a) > kMaxResidue) {  // (only when both are near the limit: keep it codable)
                a = std::clamp(a, -kMaxResidue, kMaxResidue);
            }
            o.res[2 * i] = m;
            o.res[2 * i + 1] = a;
        }
    }
    int ps = partSize[bf];
    int parts = (int)o.res.size() / ps;
    o.cls.assign(parts, 0);
    for (int p = 0; p < parts; ++p) {
        int m = 0;
        for (int i = 0; i < ps; ++i) m = std::max(m, std::abs(o.res[(size_t)p * ps + i]));
        int c = 0;
        while (kClassMax[c] < m) ++c;
        o.cls[p] = (uint8_t)c;
    }
}

// one audio packet (bw null: count the codebooks' symbols instead)
void Encoder::emit(const BlockPos& b, const BlockOut& o, BitsOut* bw) {
    const int bf = b.isLong;
    const FloorCfg& F = floors[bf];
    Book* bk = books + kBooksPer * bf;
    if (bw) {
        bw->put(0, 1);
        bw->put((uint32_t)bf, 1);  // mode 0 short, 1 long
        if (bf) {
            bw->put(b.prevLong, 1);
            bw->put(b.nextLong, 1);
        }
    }
    int n = (int)F.X.size();
    for (int c = 0; c < nch; ++c) {
        if (bw) bw->put(o.used[c], 1);
        if (!o.used[c]) continue;
        if (bw) {
            bw->put((uint32_t)o.floor[c].final[0], 7);
            bw->put((uint32_t)o.floor[c].final[1], 7);
        }
        for (int i = 2; i < n; ++i) bk[0].use(o.floor[c].val[i], bw);
    }
    bool any = false;
    for (int c = 0; c < nch; ++c) any = any || o.used[c];
    if (!any) return;
    const int ps = partSize[bf], parts = (int)o.cls.size();
    const int perWord = 2;
    std::vector<int> idx((size_t)ps * 3);
    for (int pass = 0; pass < 3; ++pass) {  // (no class has more than three passes)
        int p = 0;
        while (p < parts) {
            if (pass == 0) {
                int word = 0;
                for (int i = 0; i < perWord; ++i) word = word * kClasses + (p + i < parts ? o.cls[p + i] : 0);
                bk[1].use(word, bw);
            }
            for (int i = 0; i < perWord && p < parts; ++i, ++p) {
                int c = o.cls[p];
                if (pass >= kClassPasses[c]) continue;
                // the values split over the passes: coarse steps first
                const int* v = o.res.data() + (size_t)p * ps;
                for (int k = 0; k < ps; ++k) {
                    int rem = v[k];
                    for (int j = 0; j <= pass; ++j) {
                        const VqDef& d = kVq[kClassBooks[c][j]];
                        int h = (d.K - 1) / 2;
                        int qv = std::clamp((int)std::lround((double)rem / d.step), -h, h);
                        rem -= qv * d.step;
                        if (j == pass) idx[k] = qv + h;
                    }
                }
                int vb = kClassBooks[c][pass];
                const VqDef& d = kVq[vb];
                for (int k = 0; k < ps; k += d.dims) {
                    int e = 0, mul = 1;
                    for (int t = 0; t < d.dims; ++t) {
                        e += idx[k + t] * mul;
                        mul *= d.K;
                    }
                    bk[2 + vqBookOf(c, pass)].use(e, bw);
                }
            }
        }
    }
}

void Encoder::headers(BitsOut& id, BitsOut& comment, BitsOut& setup) const {
    id.put(1, 8);
    id.text("vorbis");
    id.put(0, 32);
    id.put((uint32_t)nch, 8);
    id.put((uint32_t)sr, 32);
    id.put(0, 32);
    double nominal = (quality < 4 ? 64 + 16 * (quality + 1) : 128 + 32 * (quality - 4)) * 1000 * nch / 2;
    id.put((uint32_t)nominal, 32);
    id.put(0, 32);
    id.put(8, 4);   // 256
    id.put(11, 4);  // 2048
    id.put(1, 1);
    id.flush();

    comment.put(3, 8);
    comment.text("vorbis");
    const char* vendor = "Exbit";
    comment.put((uint32_t)strlen(vendor), 32);
    comment.text(vendor);
    comment.put(0, 32);
    comment.put(1, 1);
    comment.flush();

    setup.put(5, 8);
    setup.text("vorbis");
    setup.put(2 * kBooksPer - 1, 8);
    for (const Book& b : books) b.write(setup);
    setup.put(0, 6);  // time domain transforms: one placeholder
    setup.put(0, 16);
    setup.put(2 - 1, 6);  // floors
    for (int f = 0; f < 2; ++f) {
        const FloorCfg& F = floors[f];
        setup.put(1, 16);
        setup.put((uint32_t)F.parts, 5);
        for (int p = 0; p < F.parts; ++p) setup.put(0, 4);  // all partitions in class 0
        setup.put((uint32_t)F.classDim - 1, 3);
        setup.put(0, 2);  // no subclasses
        setup.put((uint32_t)(kBooksPer * f + 0) + 1, 8);
        setup.put(FloorCfg::mult - 1, 2);
        setup.put((uint32_t)F.rangeBits, 4);
        for (size_t i = 2; i < F.X.size(); ++i) setup.put((uint32_t)F.X[i], F.rangeBits);
    }
    setup.put(2 - 1, 6);  // residues
    for (int r = 0; r < 2; ++r) {
        int half = (r ? kLong : kShort) / 2;
        setup.put(2, 16);
        setup.put(0, 24);
        setup.put((uint32_t)(half * nch), 24);
        setup.put((uint32_t)partSize[r] - 1, 24);
        setup.put(kClasses - 1, 6);
        setup.put((uint32_t)(kBooksPer * r + 1), 8);
        for (int c = 0; c < kClasses; ++c) {
            int bits = (1 << kClassPasses[c]) - 1;
            setup.put((uint32_t)(bits & 7), 3);
            setup.put(bits > 7, 1);
            if (bits > 7) setup.put((uint32_t)(bits >> 3), 5);
        }
        for (int c = 0; c < kClasses; ++c)
            for (int j = 0; j < kClassPasses[c]; ++j) setup.put((uint32_t)(kBooksPer * r + 2 + vqBookOf(c, j)), 8);
    }
    setup.put(2 - 1, 6);  // mappings
    for (int m = 0; m < 2; ++m) {
        setup.put(0, 16);
        setup.put(0, 1);  // one submap
        if (nch == 2) {
            setup.put(1, 1);
            setup.put(0, 8);  // one coupling step: magnitude 0, angle 1
            setup.put(0, 1);
            setup.put(1, 1);
        } else {
            setup.put(0, 1);
        }
        setup.put(0, 2);
        setup.put(0, 8);
        setup.put((uint32_t)m, 8);  // floor
        setup.put((uint32_t)m, 8);  // residue
    }
    setup.put(2 - 1, 6);  // modes
    for (int m = 0; m < 2; ++m) {
        setup.put((uint32_t)m, 1);
        setup.put(0, 16);
        setup.put(0, 16);
        setup.put((uint32_t)m, 8);
    }
    setup.put(1, 1);
    setup.flush();
}

bool Encoder::encode(const float* in, int64_t frames, int inStride, std::vector<uint8_t>& out, std::string& error,
                     const std::function<bool(double)>& progress) {
    src = in;
    srcFrames = frames;
    stride = inStride;
    std::vector<BlockPos> blocks = layout(frames);
    const size_t nb = blocks.size();
    // the blocks are analyzed on several threads, a chunk at a time, and coded in order
    const int threads = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    const size_t chunk = 64 * (size_t)threads;
    std::vector<BlockOut> outs(std::min(chunk, nb));
    auto analyzeChunk = [&](size_t from, size_t n) {
        if (threads == 1 || n < 8) {
            for (size_t k = 0; k < n; ++k) analyze(blocks[from + k], outs[k]);
            return;
        }
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (size_t k = (size_t)t; k < n; k += (size_t)threads) analyze(blocks[from + k], outs[k]);
            });
        for (auto& th : pool) th.join();
    };
    // pass 1: the statistics of every codebook
    for (size_t i = 0; i < nb; i += chunk) {
        if (progress && !progress(0.5 * i / nb)) {
            error = "cancelled";
            return false;
        }
        size_t n = std::min(chunk, nb - i);
        analyzeChunk(i, n);
        for (size_t k = 0; k < n; ++k) emit(blocks[i + k], outs[k], nullptr);
    }
    for (Book& b : books)
        if (!b.build()) {
            error = "Vorbis encoder: codebook construction failed";
            return false;
        }
    // pass 2: the stream (the same analysis again)
    out.clear();
    OggWriter ogg(out);
    BitsOut id, comment, setupHdr;
    headers(id, comment, setupHdr);
    ogg.packet(id.buf, 0);
    ogg.page(false);
    ogg.packet(comment.buf, 0);
    ogg.packet(setupHdr.buf, 0);
    ogg.page(false);
    for (size_t i = 0; i < nb; i += chunk) {
        if (progress && !progress(0.5 + 0.5 * i / nb)) {
            error = "cancelled";
            return false;
        }
        size_t n = std::min(chunk, nb - i);
        analyzeChunk(i, n);
        for (size_t k = 0; k < n; ++k) {
            BitsOut bw;
            emit(blocks[i + k], outs[k], &bw);
            bw.flush();
            if (bw.buf.empty()) bw.buf.push_back(0);
            bool last = i + k + 1 == nb;
            ogg.packet(bw.buf, std::min<int64_t>(blocks[i + k].centre, frames));
            if (last) ogg.page(true, frames);
            else if (ogg.body.size() >= 4096) ogg.page(false);
        }
    }
    if (progress && !progress(1.0)) {
        error = "cancelled";
        return false;
    }
    return true;
}

}  // namespace

bool encodeVorbisToMemory(std::vector<uint8_t>& out, const float* interleaved, size_t frames, int channels,
                          int sampleRate, double quality, std::string& error, std::function<bool(double)> progress) {
    out.clear();
    if (channels <= 0 || sampleRate <= 0 || (!interleaved && frames)) {
        error = "Vorbis encoder: no audio";
        return false;
    }
    auto enc = std::make_unique<Encoder>();
    enc->setup(sampleRate, channels, quality);
    return enc->encode(interleaved, (int64_t)frames, channels, out, error, progress);
}

bool encodeVorbis(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
                  double quality, std::string& error, std::function<bool(double)> progress) {
    std::vector<uint8_t> data;
    if (!encodeVorbisToMemory(data, interleaved, frames, channels, sampleRate, quality, error, progress)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        error = "can't write " + path;
        return false;
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        error = "can't write " + path;
        remove(path.c_str());
    }
    return ok;
}
