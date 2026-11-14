// Ogg Vorbis decoder (Tiny DAW's own), written from the Vorbis I specification and RFC 3533 (Ogg).
//
// The structure is the usual one (as in the public-domain stb_vorbis): Ogg pages are parsed and
// their CRCs checked, packets reassembled from the lacing values (also across pages), the first
// logical stream that carries Vorbis is decoded (chained streams with the same format follow on).
// Supported: codebooks (ordered, sparse, lookup types 1 and 2), floor types 0 and 1, residue types
// 0, 1 and 2, mappings with submaps and channel coupling, any block sizes with window switching.
// The IMDCT is computed with an n/4-point complex FFT. The start and end of the stream are trimmed
// by the granule positions as libvorbis does.
#include "audio/codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

int ilog(int64_t v) {
    int n = 0;
    while (v > 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

// ---------------------------------------------------------------- Ogg
struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
            t[i] = r;
        }
    }
};

uint32_t oggCrc(const uint8_t* p, size_t n, uint32_t crc) {
    static const CrcTable T;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ T.t[((crc >> 24) ^ p[i]) & 0xff];
    return crc;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct OggPage {
    size_t size = 0;  // whole page
    uint8_t flags = 0;
    int64_t granule = -1;
    uint32_t serial = 0, seq = 0;
    int nseg = 0;
    const uint8_t* lacing = nullptr;
    const uint8_t* body = nullptr;
    bool continued() const { return flags & 1; }
    bool bos() const { return flags & 2; }
    bool eos() const { return flags & 4; }
};

// a valid page (with a correct CRC) at pos?
bool readPage(const uint8_t* d, size_t size, size_t pos, OggPage& pg) {
    if (pos + 27 > size || memcmp(d + pos, "OggS", 4) != 0 || d[pos + 4] != 0) return false;
    const uint8_t* h = d + pos;
    int nseg = h[26];
    if (pos + 27 + nseg > size) return false;
    size_t body = 0;
    for (int i = 0; i < nseg; ++i) body += h[27 + i];
    size_t total = 27 + nseg + body;
    if (pos + total > size) return false;
    uint32_t crc = oggCrc(h, 22, 0);
    static const uint8_t zero[4] = {0, 0, 0, 0};
    crc = oggCrc(zero, 4, crc);
    crc = oggCrc(h + 26, total - 26, crc);
    if (crc != le32(h + 22)) return false;
    pg.size = total;
    pg.flags = h[5];
    uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = g << 8 | h[6 + i];
    pg.granule = (int64_t)g;
    pg.serial = le32(h + 14);
    pg.seq = le32(h + 18);
    pg.nseg = nseg;
    pg.lacing = h + 27;
    pg.body = h + 27 + nseg;
    return true;
}

// next valid page at or after pos (SIZE_MAX if none)
size_t nextPage(const uint8_t* d, size_t size, size_t pos, OggPage& pg) {
    while (pos + 27 <= size) {
        const void* f = memchr(d + pos, 'O', size - pos - 26);
        if (!f) return SIZE_MAX;
        pos = (const uint8_t*)f - d;
        if (readPage(d, size, pos, pg)) return pos;
        ++pos;
    }
    return SIZE_MAX;
}

// ---------------------------------------------------------------- bit reader, LSB first
// The buffer must have 8 readable (zero) bytes after `bytes`.
struct BitsLE {
    const uint8_t* d = nullptr;
    size_t bytes = 0;
    size_t pos = 0;  // bits
    bool eop = false;
    void init(const uint8_t* p, size_t n) {
        d = p;
        bytes = n;
        pos = 0;
        eop = false;
    }
    size_t left() const { return bytes * 8 - pos; }
    uint64_t peek64() const {
        size_t b = pos >> 3;
        if (b >= bytes) return 0;
        uint64_t v;
        memcpy(&v, d + b, 8);
        return v >> (pos & 7);  // 57 valid bits (zeros past the end)
    }
    uint32_t read(int n) {
        if (n <= 0) return 0;
        if ((size_t)n > left()) {
            eop = true;
            pos = bytes * 8;
            return 0;
        }
        uint32_t v = (uint32_t)(peek64() & ((n == 32) ? 0xffffffffull : ((1ull << n) - 1)));
        pos += n;
        return v;
    }
    bool skip(int n) {
        if ((size_t)n > left()) {
            eop = true;
            pos = bytes * 8;
            return false;
        }
        pos += n;
        return true;
    }
};

uint32_t bitReverse(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

float float32Unpack(uint32_t x) {
    double mantissa = (double)(x & 0x1fffff);
    if (x & 0x80000000u) mantissa = -mantissa;
    int exponent = (int)((x & 0x7fe00000u) >> 21);
    return (float)std::ldexp(mantissa, exponent - 788);
}

// ---------------------------------------------------------------- codebooks
constexpr int kFastBits = 10;

struct Codebook {
    int dims = 0, entries = 0;
    std::vector<uint8_t> lens;
    int lookupType = 0;
    std::vector<float> vq;  // entries * dims
    std::vector<int32_t> fast;  // LSB-first lookup of kFastBits bits: entry, or -1
    struct Long {
        uint32_t key;  // codeword, MSB first, left-aligned
        int32_t entry;
        uint8_t len;
    };
    std::vector<Long> longs;
    int single = -1;  // the only used entry of a one-entry book

    // assigns the codewords (spec 3.2.1) and builds the lookup structures
    bool buildHuffman() {
        uint32_t avail[33] = {0};
        bool first = true;
        int used = 0;
        for (int i = 0; i < entries; ++i)
            if (lens[i]) ++used;
        fast.assign(1 << kFastBits, -1);
        if (used == 0) return true;
        if (used == 1) {
            for (int i = 0; i < entries; ++i)
                if (lens[i]) single = i;
            return true;
        }
        for (int i = 0; i < entries; ++i) {
            int l = lens[i];
            if (!l) continue;
            uint32_t cw;  // left-aligned
            if (first) {
                cw = 0;
                for (int j = 1; j <= l; ++j) avail[j] = 1u << (32 - j);
                first = false;
            } else {
                int z = l;
                while (z > 0 && !avail[z]) --z;
                if (z == 0) return false;  // overspecified
                cw = avail[z];
                avail[z] = 0;
                for (int y = z + 1; y <= l; ++y) avail[y] = cw + (1u << (32 - y));
            }
            uint32_t code = l == 32 ? cw : cw >> (32 - l);  // MSB first
            if (l <= kFastBits) {
                uint32_t rev = bitReverse(code) >> (32 - l);  // LSB-first pattern
                for (uint32_t k = 0; k < (1u << (kFastBits - l)); ++k) fast[rev | (k << l)] = i;
            } else {
                longs.push_back({cw, i, (uint8_t)l});
            }
        }
        std::sort(longs.begin(), longs.end(), [](const Long& a, const Long& b) { return a.key < b.key; });
        return true;
    }

    // entry number, or -1 (end of packet / invalid code)
    int decode(BitsLE& br) const {
        if (single >= 0) return br.skip(lens[single]) ? single : -1;
        uint64_t bits = br.peek64();
        int e = fast[bits & ((1 << kFastBits) - 1)];
        if (e >= 0) return br.skip(lens[e]) ? e : -1;
        if (longs.empty()) {
            br.eop = true;
            return -1;
        }
        uint32_t key = bitReverse((uint32_t)bits);
        size_t lo = 0, hi = longs.size();
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (longs[mid].key <= key) lo = mid;
            else hi = mid;
        }
        const Long& L = longs[lo];
        if (L.key > key || ((key ^ L.key) >> (32 - L.len)) != 0) {
            br.eop = true;
            return -1;
        }
        return br.skip(L.len) ? L.entry : -1;
    }
    const float* vector(int entry) const { return vq.data() + (size_t)entry * dims; }
};

int64_t lookup1Values(int entries, int dims) {
    if (dims <= 0) return 0;
    int64_t r = (int64_t)std::floor(std::pow((double)entries, 1.0 / dims));
    auto powLeq = [&](int64_t b) {  // b^dims <= entries
        int64_t acc = 1;
        for (int i = 0; i < dims; ++i) {
            acc *= b;
            if (acc > entries) return false;
        }
        return true;
    };
    while (r > 0 && !powLeq(r)) --r;
    while (powLeq(r + 1)) ++r;
    return r;
}

bool readCodebook(BitsLE& br, Codebook& cb) {
    if (br.read(24) != 0x564342) return false;
    cb.dims = br.read(16);
    cb.entries = br.read(24);
    if (br.eop) return false;
    cb.lens.assign(cb.entries, 0);
    if (br.read(1)) {  // ordered
        int len = br.read(5) + 1;
        int i = 0;
        while (i < cb.entries) {
            int num = br.read(ilog(cb.entries - i));
            if (br.eop || len > 32 || i + num > cb.entries) return false;
            for (int k = 0; k < num; ++k) cb.lens[i + k] = (uint8_t)len;
            i += num;
            ++len;
        }
    } else {
        bool sparse = br.read(1);
        for (int i = 0; i < cb.entries; ++i) {
            if (sparse && !br.read(1)) continue;
            cb.lens[i] = (uint8_t)(br.read(5) + 1);
        }
    }
    if (br.eop) return false;
    cb.lookupType = br.read(4);
    if (cb.lookupType == 1 || cb.lookupType == 2) {
        float minv = float32Unpack(br.read(32));
        float delta = float32Unpack(br.read(32));
        int vbits = br.read(4) + 1;
        bool seq = br.read(1);
        if (cb.dims == 0) return false;
        int64_t lv = cb.lookupType == 1 ? lookup1Values(cb.entries, cb.dims) : (int64_t)cb.entries * cb.dims;
        if (lv <= 0 || lv > (int64_t)br.left() / vbits + 1) return false;
        std::vector<uint32_t> mult((size_t)lv);
        for (auto& m : mult) m = br.read(vbits);
        if (br.eop) return false;
        if ((int64_t)cb.entries * cb.dims > (8 << 20)) return false;  // (real books are far smaller)
        cb.vq.assign((size_t)cb.entries * cb.dims, 0.f);
        for (int e = 0; e < cb.entries; ++e) {
            if (!cb.lens[e]) continue;
            float last = 0;
            int64_t div = 1;
            for (int i = 0; i < cb.dims; ++i) {
                int64_t off = cb.lookupType == 1 ? (e / div) % lv : (int64_t)e * cb.dims + i;
                float v = mult[(size_t)off] * delta + minv + last;
                cb.vq[(size_t)e * cb.dims + i] = v;
                if (seq) last = v;
                if (cb.lookupType == 1) div *= lv;
            }
        }
    } else if (cb.lookupType != 0) {
        return false;
    }
    return cb.buildHuffman();
}

// ---------------------------------------------------------------- floors, residues, mappings
struct Floor {
    int type = 1;
    // floor 0
    int order = 0, rate = 0, barkMapSize = 0, ampBits = 0, ampOffset = 0;
    std::vector<int> books0;
    std::vector<int> map[2];  // bark map per block size
    // floor 1
    std::vector<int> partClass;
    int classDims[16] = {0}, classSub[16] = {0}, classMaster[16] = {0}, subBooks[16][8];
    int mult = 1;
    std::vector<int> X, sorted, low, high;
};

struct Residue {
    int type = 0, begin = 0, end = 0, partSize = 1, classifications = 1, classbook = 0;
    std::vector<int> books;  // [class * 8 + pass]
};

struct Mapping {
    int submaps = 1;
    std::vector<int> mag, ang;
    std::vector<int> mux;
    int floorOf[16] = {0}, residueOf[16] = {0};
};

struct Mode {
    int blockflag = 0, mapping = 0;
};

// ---------------------------------------------------------------- IMDCT (n/4-point complex FFT)
struct Imdct {
    int n = 0;
    std::vector<float> preRe, preIm, postRe, postIm, twRe, twIm;
    std::vector<int> rev;
    std::vector<float> zr, zi, u;
    void init(int size) {
        n = size;
        int M = n / 2, Q = n / 4;
        preRe.resize(Q); preIm.resize(Q); postRe.resize(Q); postIm.resize(Q);
        for (int k = 0; k < Q; ++k) {
            preRe[k] = (float)std::cos(-kPi * k / M);
            preIm[k] = (float)std::sin(-kPi * k / M);
            postRe[k] = (float)std::cos(-kPi * (k + 0.25) / M);
            postIm[k] = (float)std::sin(-kPi * (k + 0.25) / M);
        }
        twRe.resize(Q / 2 + 1); twIm.resize(Q / 2 + 1);
        for (int k = 0; k <= Q / 2; ++k) {
            twRe[k] = (float)std::cos(-2 * kPi * k / Q);
            twIm[k] = (float)std::sin(-2 * kPi * k / Q);
        }
        rev.resize(Q);
        int bits = ilog(Q) - 1;
        for (int i = 0; i < Q; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[i] = r;
        }
        zr.resize(Q); zi.resize(Q); u.resize(M);
    }
    // y[i] = sum_k X[k] cos(2 pi / n (i + 1/2 + n/4)(k + 1/2)), i < n, k < n/2
    void run(const float* X, float* y) {
        int M = n / 2, Q = n / 4;
        for (int k = 0; k < Q; ++k) {
            float a = X[2 * k], b = X[M - 1 - 2 * k];
            int r = rev[k];
            zr[r] = a * preRe[k] - b * preIm[k];
            zi[r] = a * preIm[k] + b * preRe[k];
        }
        for (int len = 2; len <= Q; len <<= 1) {
            int half = len >> 1, step = Q / len;
            for (int s = 0; s < Q; s += len) {
                for (int j = 0; j < half; ++j) {
                    float wr = twRe[j * step], wi = twIm[j * step];
                    int p = s + j, q = p + half;
                    float tr = zr[q] * wr - zi[q] * wi, ti = zr[q] * wi + zi[q] * wr;
                    zr[q] = zr[p] - tr;
                    zi[q] = zi[p] - ti;
                    zr[p] += tr;
                    zi[p] += ti;
                }
            }
        }
        for (int j = 0; j < Q; ++j) {
            float wr = zr[j] * postRe[j] - zi[j] * postIm[j];
            float wi = zr[j] * postIm[j] + zi[j] * postRe[j];
            u[2 * j] = wr;
            u[M - 1 - 2 * j] = -wi;
        }
        int h = M / 2;
        for (int i = 0; i < h; ++i) y[i] = u[i + h];
        for (int i = h; i < 3 * h; ++i) y[i] = -u[3 * h - 1 - i];
        for (int i = 3 * h; i < 2 * M; ++i) y[i] = -u[i - 3 * h];
    }
};

// ---------------------------------------------------------------- the decoder
struct VorbisDecoder {
    int channels = 0, rate = 0;
    int bs[2] = {0, 0};
    std::vector<Codebook> books;
    std::vector<Floor> floors;
    std::vector<Residue> residues;
    std::vector<Mapping> mappings;
    std::vector<Mode> modes;
    int headers = 0;  // headers seen (3 = ready)

    Imdct imdct[2];
    std::vector<float> slope[2];  // rising window halves, length bs/2
    std::vector<std::vector<float>> spec, prev, block;
    std::vector<float> floorBuf, resBuf;
    std::vector<int> floorY, floorFinal;
    std::vector<uint8_t> floorStep;
    std::vector<std::vector<uint8_t>> classif;
    int prevN = 0;
    bool havePrev = false;
    float dbTable[256];

    VorbisDecoder() {
        for (int i = 0; i < 256; ++i) dbTable[i] = (float)std::pow(10.0, (i - 255) * 7.0 / 256.0);
    }

    bool identification(BitsLE& br, std::string& error) {
        if (br.read(32) != 0) {
            error = "unsupported Vorbis version";
            return false;
        }
        channels = br.read(8);
        rate = (int)br.read(32);
        br.read(32);
        br.read(32);
        br.read(32);
        int b0 = br.read(4), b1 = br.read(4);
        bool framing = br.read(1);
        if (br.eop || channels < 1 || rate < 1 || b0 < 6 || b1 > 13 || b0 > b1 || !framing) {
            error = "invalid Vorbis identification header";
            return false;
        }
        bs[0] = 1 << b0;
        bs[1] = 1 << b1;
        return true;
    }

    bool readFloor(BitsLE& br, Floor& f) {
        f.type = br.read(16);
        int nb = (int)books.size();
        if (f.type == 0) {
            f.order = br.read(8);
            f.rate = br.read(16);
            f.barkMapSize = br.read(16);
            f.ampBits = br.read(6);
            f.ampOffset = br.read(8);
            int n = br.read(4) + 1;
            for (int i = 0; i < n; ++i) {
                int b = br.read(8);
                if (b >= nb) return false;
                f.books0.push_back(b);
            }
            if (f.order < 1 || f.barkMapSize < 1 || f.rate < 1) return false;
            for (int s = 0; s < 2; ++s) {
                int half = bs[s] / 2;
                auto bark = [](double x) { return 13.1 * std::atan(.00074 * x) + 2.24 * std::atan(.0000000185 * x * x) + .0001 * x; };
                f.map[s].resize(half);
                for (int i = 0; i < half; ++i) {
                    int v = (int)std::floor(bark((double)f.rate * i / (2.0 * half)) * f.barkMapSize / bark(.5 * f.rate));
                    f.map[s][i] = std::min(f.barkMapSize - 1, v);
                }
            }
            return !br.eop;
        }
        if (f.type != 1) return false;
        int parts = br.read(5);
        int maxClass = -1;
        f.partClass.resize(parts);
        for (int i = 0; i < parts; ++i) {
            f.partClass[i] = br.read(4);
            maxClass = std::max(maxClass, f.partClass[i]);
        }
        for (int c = 0; c <= maxClass; ++c) {
            f.classDims[c] = br.read(3) + 1;
            f.classSub[c] = br.read(2);
            if (f.classSub[c]) {
                f.classMaster[c] = br.read(8);
                if (f.classMaster[c] >= nb) return false;
            }
            for (int k = 0; k < (1 << f.classSub[c]); ++k) {
                f.subBooks[c][k] = (int)br.read(8) - 1;
                if (f.subBooks[c][k] >= nb) return false;
            }
        }
        f.mult = br.read(2) + 1;
        int rangeBits = br.read(4);
        f.X = {0, 1 << rangeBits};
        for (int i = 0; i < parts; ++i) {
            int c = f.partClass[i];
            for (int j = 0; j < f.classDims[c]; ++j) f.X.push_back(br.read(rangeBits));
        }
        if (br.eop || f.X.size() > 65 * 4) return false;
        int n = (int)f.X.size();
        f.sorted.resize(n);
        for (int i = 0; i < n; ++i) f.sorted[i] = i;
        std::stable_sort(f.sorted.begin(), f.sorted.end(), [&](int a, int b) { return f.X[a] < f.X[b]; });
        for (int i = 1; i < n; ++i)
            if (f.X[f.sorted[i]] == f.X[f.sorted[i - 1]]) return false;
        f.low.assign(n, 0);
        f.high.assign(n, 1);
        for (int i = 2; i < n; ++i) {
            int lo = -1, hi = -1;
            for (int j = 0; j < i; ++j) {
                if (f.X[j] < f.X[i] && (lo < 0 || f.X[j] > f.X[lo])) lo = j;
                if (f.X[j] > f.X[i] && (hi < 0 || f.X[j] < f.X[hi])) hi = j;
            }
            f.low[i] = lo;
            f.high[i] = hi;
        }
        return true;
    }

    bool setup(BitsLE& br, std::string& error) {
        auto fail = [&](const char* what) {
            error = std::string("invalid Vorbis setup header (") + what + ")";
            return false;
        };
        int nb = br.read(8) + 1;
        books.resize(nb);
        for (auto& b : books)
            if (!readCodebook(br, b)) return fail("codebook");
        int nt = br.read(6) + 1;
        for (int i = 0; i < nt; ++i)
            if (br.read(16) != 0) return fail("time domain");
        int nf = br.read(6) + 1;
        floors.resize(nf);
        for (auto& f : floors)
            if (!readFloor(br, f)) return fail("floor");
        int nr = br.read(6) + 1;
        residues.resize(nr);
        for (auto& r : residues) {
            r.type = br.read(16);
            if (r.type > 2) return fail("residue type");
            r.begin = br.read(24);
            r.end = br.read(24);
            r.partSize = br.read(24) + 1;
            r.classifications = br.read(6) + 1;
            r.classbook = br.read(8);
            if (r.classbook >= nb) return fail("residue classbook");
            std::vector<int> cascade(r.classifications);
            for (auto& c : cascade) {
                int lowBits = br.read(3);
                int highBits = br.read(1) ? br.read(5) : 0;
                c = highBits * 8 + lowBits;
            }
            r.books.assign(r.classifications * 8, -1);
            for (int c = 0; c < r.classifications; ++c)
                for (int j = 0; j < 8; ++j)
                    if (cascade[c] & (1 << j)) {
                        int b = br.read(8);
                        if (b >= nb || books[b].lookupType == 0) return fail("residue book");
                        r.books[c * 8 + j] = b;
                    }
            if (books[r.classbook].dims < 1) return fail("residue classbook dimensions");
        }
        int nm = br.read(6) + 1;
        mappings.resize(nm);
        for (auto& m : mappings) {
            if (br.read(16) != 0) return fail("mapping type");
            m.submaps = br.read(1) ? br.read(4) + 1 : 1;
            int steps = br.read(1) ? br.read(8) + 1 : 0;
            int bits = ilog(channels - 1);
            for (int i = 0; i < steps; ++i) {
                int a = br.read(bits), b = br.read(bits);
                if (a == b || a >= channels || b >= channels) return fail("coupling");
                m.mag.push_back(a);
                m.ang.push_back(b);
            }
            if (br.read(2) != 0) return fail("mapping reserved");
            m.mux.assign(channels, 0);
            if (m.submaps > 1)
                for (auto& x : m.mux) {
                    x = br.read(4);
                    if (x >= m.submaps) return fail("mapping mux");
                }
            for (int s = 0; s < m.submaps; ++s) {
                br.read(8);
                m.floorOf[s] = br.read(8);
                m.residueOf[s] = br.read(8);
                if (m.floorOf[s] >= nf || m.residueOf[s] >= nr) return fail("submap");
            }
        }
        int nmo = br.read(6) + 1;
        modes.resize(nmo);
        for (auto& mo : modes) {
            mo.blockflag = br.read(1);
            int wt = br.read(16), tt = br.read(16);
            mo.mapping = br.read(8);
            if (wt != 0 || tt != 0 || mo.mapping >= nm) return fail("mode");
        }
        if (!br.read(1) || br.eop) return fail("framing");
        // working buffers
        for (int s = 0; s < 2; ++s) {
            imdct[s].init(bs[s]);
            int half = bs[s] / 2;
            slope[s].resize(half);
            for (int i = 0; i < half; ++i) {
                double x = std::sin((i + 0.5) / half * kPi / 2);
                slope[s][i] = (float)std::sin(kPi / 2 * x * x);
            }
        }
        spec.assign(channels, std::vector<float>(bs[1] / 2));
        prev.assign(channels, std::vector<float>(bs[1] / 2));
        block.assign(channels, std::vector<float>(bs[1]));
        floorBuf.assign(bs[1] / 2, 0.f);
        resBuf.assign((size_t)bs[1] / 2 * channels, 0.f);
        classif.assign(channels, std::vector<uint8_t>());
        havePrev = false;
        prevN = 0;
        return true;
    }

    // header packets; false + error when they're broken
    bool header(const uint8_t* p, size_t n, std::string& error) {
        BitsLE br;
        br.init(p, n);
        int type = br.read(8);
        if (n < 7 || memcmp(p + 1, "vorbis", 6) != 0) {
            error = "invalid Vorbis header";
            return false;
        }
        br.pos = 7 * 8;
        if (headers == 0) {
            if (type != 1) {
                error = "missing Vorbis identification header";
                return false;
            }
            if (!identification(br, error)) return false;
        } else if (headers == 1) {
            if (type != 3) {
                error = "missing Vorbis comment header";
                return false;
            }
        } else {
            if (type != 5) {
                error = "missing Vorbis setup header";
                return false;
            }
            if (!setup(br, error)) return false;
        }
        ++headers;
        return true;
    }

    // ---------------- floors
    // floor 1: false if the channel is unused
    bool floor1(BitsLE& br, const Floor& f, int half, float* out) {
        if (!br.read(1)) return false;
        static const int kRange[4] = {256, 128, 86, 64};
        int range = kRange[f.mult - 1];
        int n = (int)f.X.size();
        floorY.resize(n);
        floorFinal.resize(n);
        floorStep.resize(n);
        int ybits = ilog(range - 1);
        floorY[0] = br.read(ybits);
        floorY[1] = br.read(ybits);
        int off = 2;
        for (size_t i = 0; i < f.partClass.size(); ++i) {
            int c = f.partClass[i];
            int cdim = f.classDims[c], cbits = f.classSub[c], csub = (1 << cbits) - 1;
            int cval = 0;
            if (cbits) {
                cval = books[f.classMaster[c]].decode(br);
                if (cval < 0) return false;
            }
            for (int j = 0; j < cdim; ++j) {
                int book = f.subBooks[c][cval & csub];
                cval >>= cbits;
                if (book >= 0) {
                    int v = books[book].decode(br);
                    if (v < 0) return false;
                    floorY[off + j] = v;
                } else {
                    floorY[off + j] = 0;
                }
            }
            off += cdim;
        }
        if (br.eop) return false;
        // amplitude values
        floorStep[0] = floorStep[1] = 1;
        floorFinal[0] = floorY[0];
        floorFinal[1] = floorY[1];
        for (int i = 2; i < n; ++i) {
            int lo = f.low[i], hi = f.high[i];
            int predicted = renderPoint(f.X[lo], floorFinal[lo], f.X[hi], floorFinal[hi], f.X[i]);
            int val = floorY[i];
            int highroom = range - predicted, lowroom = predicted;
            int room = (highroom < lowroom ? highroom : lowroom) * 2;
            if (val) {
                floorStep[lo] = floorStep[hi] = floorStep[i] = 1;
                if (val >= room) {
                    floorFinal[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
                } else {
                    floorFinal[i] = (val & 1) ? predicted - (val + 1) / 2 : predicted + val / 2;
                }
            } else {
                floorStep[i] = 0;
                floorFinal[i] = predicted;
            }
        }
        // curve
        int lx = 0, ly = floorFinal[f.sorted[0]] * f.mult, hx = 0, hy = 0;
        for (int k = 1; k < n; ++k) {
            int i = f.sorted[k];
            if (!floorStep[i]) continue;
            hy = floorFinal[i] * f.mult;
            hx = f.X[i];
            renderLine(lx, ly, hx, hy, out, half);
            lx = hx;
            ly = hy;
        }
        if (hx < half) renderLine(hx, hy, half, hy, out, half);
        return true;
    }

    static int renderPoint(int x0, int y0, int x1, int y1, int x) {
        int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
        if (adx == 0) return y0;
        int err = ady * (x - x0);
        int off = err / adx;
        return dy < 0 ? y0 - off : y0 + off;
    }

    void renderLine(int x0, int y0, int x1, int y1, float* v, int n) {
        int dy = y1 - y0, adx = x1 - x0;
        if (adx <= 0) return;
        int ady = std::abs(dy);
        int base = dy / adx;
        int sy = dy < 0 ? base - 1 : base + 1;
        ady -= std::abs(base) * adx;
        int x = x0, y = y0, err = 0;
        auto put = [&](int xx, int yy) {
            if (xx < n) v[xx] = dbTable[yy < 0 ? 0 : yy > 255 ? 255 : yy];
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

    bool floor0(BitsLE& br, const Floor& f, int half, int blockflag, float* out) {
        int amplitude = br.read(f.ampBits);
        if (amplitude <= 0 || br.eop) return false;
        int bn = br.read(ilog((int)f.books0.size()));
        if (br.eop || bn >= (int)f.books0.size()) return false;
        const Codebook& cb = books[f.books0[bn]];
        if (cb.dims < 1 || cb.vq.empty()) return false;
        std::vector<double> coef;
        double last = 0;
        while ((int)coef.size() < f.order) {
            int e = cb.decode(br);
            if (e < 0) return false;
            const float* v = cb.vector(e);
            for (int i = 0; i < cb.dims; ++i) coef.push_back(v[i] + last);
            last = coef.back();
        }
        std::vector<double> cosc(f.order);
        for (int i = 0; i < f.order; ++i) cosc[i] = std::cos(coef[i]);
        const std::vector<int>& map = f.map[blockflag];
        int i = 0;
        while (i < half) {
            int m = map[i];
            double w = kPi * m / f.barkMapSize;
            double cw = std::cos(w);
            double p, q;
            if (f.order & 1) {
                p = 1 - cw * cw;
                q = 0.25;
                for (int j = 0; j + 1 < f.order; j += 2) p *= 4 * (cosc[j + 1] - cw) * (cosc[j + 1] - cw);
                for (int j = 0; j < f.order; j += 2) q *= 4 * (cosc[j] - cw) * (cosc[j] - cw);
            } else {
                p = (1 - cw) / 2;
                q = (1 + cw) / 2;
                for (int j = 0; j + 1 < f.order; j += 2) {
                    p *= 4 * (cosc[j + 1] - cw) * (cosc[j + 1] - cw);
                    q *= 4 * (cosc[j] - cw) * (cosc[j] - cw);
                }
            }
            double val = std::exp(.11512925 * (amplitude * f.ampOffset / (((1 << f.ampBits) - 1) * std::sqrt(p + q)) - f.ampOffset));
            while (i < half && map[i] == m) out[i++] = (float)val;
        }
        return true;
    }

    // ---------------- residues
    void residue(BitsLE& br, const Residue& r, std::vector<float*>& vecs, std::vector<bool>& skip, int half) {
        int ch = (int)vecs.size();
        for (int c = 0; c < ch; ++c) memset(vecs[c], 0, sizeof(float) * half);
        if (r.type == 2) {
            bool any = false;
            for (int c = 0; c < ch; ++c) any = any || !skip[c];
            if (!any) return;
            size_t total = (size_t)half * ch;
            float* big = resBuf.data();
            memset(big, 0, sizeof(float) * total);
            std::vector<float*> one{big};
            std::vector<bool> noskip{false};
            decodePartitions(br, r, one, noskip, (int)total, 1);
            if (ch == 1) {
                memcpy(vecs[0], big, sizeof(float) * half);
            } else {
                for (int i = 0; i < half; ++i)
                    for (int c = 0; c < ch; ++c) vecs[c][i] = big[(size_t)i * ch + c];
            }
            return;
        }
        decodePartitions(br, r, vecs, skip, half, r.type);
    }

    // format 0 (interleaved partitions) or 1 (consecutive) into each vector
    void decodePartitions(BitsLE& br, const Residue& r, std::vector<float*>& vecs, std::vector<bool>& skip, int size, int format) {
        int begin = std::min(r.begin, size), end = std::min(r.end, size);
        int ps = r.partSize;
        int parts = (end - begin) / ps;
        if (parts <= 0) return;
        const Codebook& cbook = books[r.classbook];
        int perWord = cbook.dims;
        int ch = (int)vecs.size();
        for (int c = 0; c < ch; ++c) classif[c].assign(parts + perWord, 0);
        for (int pass = 0; pass < 8; ++pass) {
            int p = 0;
            while (p < parts) {
                if (pass == 0) {
                    for (int c = 0; c < ch; ++c) {
                        if (skip[c]) continue;
                        int temp = cbook.decode(br);
                        if (temp < 0) return;
                        for (int i = perWord - 1; i >= 0; --i) {
                            classif[c][p + i] = (uint8_t)(temp % r.classifications);
                            temp /= r.classifications;
                        }
                    }
                }
                for (int i = 0; i < perWord && p < parts; ++i, ++p) {
                    for (int c = 0; c < ch; ++c) {
                        if (skip[c]) continue;
                        int book = r.books[classif[c][p] * 8 + pass];
                        if (book < 0) continue;
                        const Codebook& cb = books[book];
                        float* v = vecs[c] + begin + p * ps;
                        int dim = cb.dims;
                        if (format == 0) {
                            int step = ps / dim;
                            for (int k = 0; k < step; ++k) {
                                int e = cb.decode(br);
                                if (e < 0) return;
                                const float* t = cb.vector(e);
                                for (int j = 0; j < dim; ++j) v[k + j * step] += t[j];
                            }
                        } else {
                            for (int k = 0; k < ps;) {
                                int e = cb.decode(br);
                                if (e < 0) return;
                                const float* t = cb.vector(e);
                                for (int j = 0; j < dim && k < ps; ++j) v[k++] += t[j];
                            }
                        }
                    }
                }
            }
        }
    }

    // ---------------- audio packets
    // decodes one audio packet; appends the finished samples (interleaved) to out; returns their count
    int audio(const uint8_t* p, size_t n, std::vector<float>& out) {
        if (headers < 3 || n == 0) return 0;
        BitsLE br;
        br.init(p, n);
        if (br.read(1) != 0) return 0;  // not an audio packet
        int modeNum = br.read(ilog((int)modes.size() - 1));
        if (br.eop || modeNum >= (int)modes.size()) return 0;
        const Mode& mode = modes[modeNum];
        int bf = mode.blockflag;
        int N = bs[bf], half = N / 2;
        int prevFlag = 0, nextFlag = 0;
        if (bf) {
            prevFlag = br.read(1);
            nextFlag = br.read(1);
        }
        if (br.eop) return 0;
        const Mapping& map = mappings[mode.mapping];
        std::vector<bool> unused(channels, false), noResidue(channels, false);
        for (int c = 0; c < channels; ++c) {
            const Floor& f = floors[map.floorOf[map.mux[c]]];
            float* fo = spec[c].data();  // floor curve kept in spec, residue in block (temporarily)
            bool ok = f.type == 1 ? floor1(br, f, half, fo) : floor0(br, f, half, bf, fo);
            unused[c] = !ok;
            noResidue[c] = !ok;
        }
        for (size_t s = 0; s < map.mag.size(); ++s)
            if (!noResidue[map.mag[s]] || !noResidue[map.ang[s]]) noResidue[map.mag[s]] = noResidue[map.ang[s]] = false;
        for (int sm = 0; sm < map.submaps; ++sm) {
            std::vector<float*> vecs;
            std::vector<bool> skip;
            for (int c = 0; c < channels; ++c)
                if (map.mux[c] == sm) {
                    vecs.push_back(block[c].data());
                    skip.push_back(noResidue[c]);
                }
            if (vecs.empty()) continue;
            residue(br, residues[map.residueOf[sm]], vecs, skip, half);
        }
        for (int s = (int)map.mag.size() - 1; s >= 0; --s) {
            float* M = block[map.mag[s]].data();
            float* A = block[map.ang[s]].data();
            for (int j = 0; j < half; ++j) {
                float m = M[j], a = A[j];
                if (m > 0) {
                    if (a > 0) { A[j] = m - a; }
                    else { A[j] = m; M[j] = m + a; }
                } else {
                    if (a > 0) { A[j] = m + a; }
                    else { A[j] = m; M[j] = m - a; }
                }
            }
        }
        // spectrum = floor * residue, IMDCT, window
        int ln = bf && !prevFlag ? bs[0] / 2 : half;  // left/right slope lengths
        int rn = bf && !nextFlag ? bs[0] / 2 : half;
        const float* ls = slope[ln == half ? bf : 0].data();
        const float* rs = slope[rn == half ? bf : 0].data();
        int lws = N / 4 - ln / 2, lwe = N / 4 + ln / 2;
        int rws = 3 * N / 4 - rn / 2, rwe = 3 * N / 4 + rn / 2;
        std::vector<float> tmp(N);
        for (int c = 0; c < channels; ++c) {
            float* sp = spec[c].data();
            float* res = block[c].data();
            if (unused[c]) {
                memset(sp, 0, sizeof(float) * half);
            } else {
                for (int i = 0; i < half; ++i) sp[i] *= res[i];
            }
            imdct[bf].run(sp, tmp.data());
            float* y = res;  // block[c] now holds the windowed block
            for (int i = 0; i < lws; ++i) y[i] = 0;
            for (int i = lws; i < lwe; ++i) y[i] = tmp[i] * ls[i - lws];
            for (int i = lwe; i < rws; ++i) y[i] = tmp[i];
            for (int i = rws; i < rwe; ++i) y[i] = tmp[i] * rs[rwe - 1 - i];
            for (int i = rwe; i < N; ++i) y[i] = 0;
        }
        int count = 0;
        if (havePrev) {
            int pn = prevN;
            count = pn / 4 + N / 4;
            size_t base = out.size();
            out.resize(base + (size_t)count * channels);
            float* o = out.data() + base;
            int shift = N / 4 - pn / 4;
            for (int c = 0; c < channels; ++c) {
                const float* pv = prev[c].data();
                const float* cv = block[c].data();
                for (int k = 0; k < count; ++k) {
                    float a = k < pn / 2 ? pv[k] : 0.f;
                    int ci = shift + k;
                    float b = ci >= 0 ? cv[ci] : 0.f;
                    o[(size_t)k * channels + c] = a + b;
                }
            }
        }
        for (int c = 0; c < channels; ++c) memcpy(prev[c].data(), block[c].data() + half, sizeof(float) * half);
        prevN = N;
        havePrev = true;
        return count;
    }
};

bool isVorbisIdent(const uint8_t* p, size_t n) { return n >= 30 && p[0] == 1 && memcmp(p + 1, "vorbis", 6) == 0; }

// first packet of a BOS page (it's always alone on the page by the spec, but be lenient)
bool firstPacket(const OggPage& pg, const uint8_t*& p, size_t& n) {
    n = 0;
    for (int i = 0; i < pg.nseg; ++i) {
        n += pg.lacing[i];
        if (pg.lacing[i] < 255) break;
    }
    p = pg.body;
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public interface
bool looksLikeVorbis(const uint8_t* data, size_t size) {
    if (size < 27 || memcmp(data, "OggS", 4) != 0) return false;
    size_t pos = 0;
    for (int i = 0; i < 16; ++i) {
        OggPage pg;
        if (!readPage(data, size, pos, pg)) {
            // a page cut short (looking at a file's beginning only): check the first packet anyway
            if (pos + 27 <= size && memcmp(data + pos, "OggS", 4) == 0) {
                size_t nseg = data[pos + 26];
                size_t bodyPos = pos + 27 + nseg;
                if ((data[pos + 5] & 2) && bodyPos + 7 <= size && data[bodyPos] == 1 && memcmp(data + bodyPos + 1, "vorbis", 6) == 0)
                    return true;
            }
            return false;
        }
        if (!pg.bos()) return false;
        const uint8_t* p;
        size_t n;
        firstPacket(pg, p, n);
        if (n >= 7 && p[0] == 1 && memcmp(p + 1, "vorbis", 6) == 0) return true;
        pos += pg.size;
    }
    return false;
}

bool decodeVorbis(const uint8_t* data, size_t size, SoundData& out, std::string& error, int64_t maxFrames) {
    out = SoundData();
    if (!data || size < 27) {
        error = "not an Ogg file (too short)";
        return false;
    }
    VorbisDecoder dec;
    bool haveStream = false, ended = false;
    uint32_t serial = 0;
    std::vector<uint8_t> packet;
    bool packetOpen = false;   // `packet` holds the start of a packet continued on the next page
    bool skipContinuation = false;
    uint32_t lastSeq = 0;
    bool firstGranuleDone = false;
    int64_t position = 0;  // granule position at the end of the output so far
    int64_t produced = 0;  // samples output (after trimming)
    int64_t streamBase = 0;  // `produced` when the current (chained) stream began
    std::vector<float>& pcm = out.samples;
    int outChannels = 0, outRate = 0;
    size_t pos = 0;
    bool stop = false;
    while (!stop) {
        OggPage pg;
        size_t at = nextPage(data, size, pos, pg);
        if (at == SIZE_MAX) break;
        pos = at + pg.size;
        if (!haveStream || ended) {
            if (!pg.bos()) continue;
            const uint8_t* p;
            size_t n;
            firstPacket(pg, p, n);
            if (!isVorbisIdent(p, n)) continue;
            if (ended) {  // a chained stream follows: go on if it has the same format
                VorbisDecoder next;
                std::string e2;
                std::vector<uint8_t> tmp(p, p + n);
                tmp.resize(n + 8, 0);
                if (!next.header(tmp.data(), n, e2) || next.channels != outChannels || next.rate != outRate) break;
                dec = VorbisDecoder();
            }
            haveStream = true;
            ended = false;
            serial = pg.serial;
            packet.clear();
            packetOpen = false;
            skipContinuation = false;
            firstGranuleDone = false;
            streamBase = produced;
            lastSeq = pg.seq;
        } else if (pg.serial != serial) {
            continue;
        } else {
            if (pg.seq != lastSeq + 1) {  // lost pages
                packet.clear();
                packetOpen = false;
                skipContinuation = pg.continued();
            }
            lastSeq = pg.seq;
        }
        if (!pg.continued() && packetOpen) {  // the continuation never came
            packet.clear();
            packetOpen = false;
        }
        if (pg.continued() && !packetOpen) skipContinuation = true;
        size_t off = 0;
        int64_t pageSamples = 0;
        bool audioOnPage = false;
        for (int i = 0; i < pg.nseg; ++i) {
            int len = pg.lacing[i];
            if (skipContinuation) {
                off += len;
                if (len < 255) skipContinuation = false;
                continue;
            }
            packet.insert(packet.end(), pg.body + off, pg.body + off + len);
            off += len;
            packetOpen = true;
            if (len == 255) continue;
            // a complete packet
            packetOpen = false;
            size_t n = packet.size();
            packet.resize(n + 8, 0);
            if (dec.headers < 3) {
                if (!dec.header(packet.data(), n, error)) {
                    if (outChannels == 0) {
                        out = SoundData();
                        return false;
                    }
                    stop = true;
                    break;
                }
                if (dec.headers == 1 && outChannels == 0) {
                    outChannels = dec.channels;
                    outRate = dec.rate;
                }
            } else {
                int got = dec.audio(packet.data(), n, pcm);
                pageSamples += got;
                produced += got;
                audioOnPage = true;
            }
            packet.clear();
        }
        if (stop) break;
        // trimming by granule position (as libvorbis)
        if (audioOnPage && pg.granule != -1) {
            if (!firstGranuleDone) {
                firstGranuleDone = true;
                int64_t streamSamples = produced - streamBase;
                if (streamSamples > pg.granule) {
                    int64_t extra = std::min(streamSamples - std::max<int64_t>(pg.granule, 0), streamSamples);
                    if (pg.eos()) {  // first and last page: the end is cut
                        pcm.resize((size_t)(produced - extra) * outChannels);
                    } else {         // else the beginning
                        auto from = pcm.begin() + (size_t)streamBase * outChannels;
                        pcm.erase(from, from + (size_t)extra * outChannels);
                    }
                    produced -= extra;
                }
                position = pg.granule;
            } else {
                position += pageSamples;
                if (position > pg.granule && pg.eos()) {
                    int64_t extra = std::min<int64_t>(position - pg.granule, pageSamples);
                    pcm.resize((size_t)(produced - extra) * outChannels);
                    produced -= extra;
                }
                position = pg.granule;
            }
        }
        if (pg.eos()) ended = true;
        if (maxFrames >= 0 && firstGranuleDone && produced >= maxFrames) break;
    }
    if (outChannels == 0 || (dec.headers < 3 && produced == 0)) {
        if (error.empty()) error = haveStream ? "incomplete Vorbis headers" : "no Vorbis stream found";
        out = SoundData();
        return false;
    }
    out.channels = outChannels;
    out.sampleRate = outRate;
    if (maxFrames >= 0 && produced > maxFrames) pcm.resize((size_t)maxFrames * outChannels);
    for (float& v : pcm) {
        if (!(v == v)) v = 0;
        v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    }
    error.clear();
    return true;
}

// ---------------------------------------------------------------- hooks for the unit tests
// (tests/test_codecs.cpp declares these itself; they're not part of the program's interface)
void vorbisTestImdct(int n, const float* in, float* out) {  // fast IMDCT, n = block size
    Imdct m;
    m.init(n);
    m.run(in, out);
}
