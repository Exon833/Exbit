// Images (premultiplied ARGB32), smooth scaling, PNG reading/writing, and zlib (RFC 1950/1951)
// inflate and deflate of our own.
#include "gfx/image.h"

#include <cstdio>
#include <cstring>

// ------------------------------------------------------------------------------------------------
// Image

Image::Image(int w, int h, Color fill) { resize(w, h, fill); }

void Image::resize(int w, int h, Color fill) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    px_.assign((size_t)w_ * h_, fill.premul());
}

void Image::fill(Color c) { std::fill(px_.begin(), px_.end(), c.premul()); }

void Image::fill(const Rect& r, Color c) {
    Rect k = r.intersected(rect());
    if (k.empty()) return;
    uint32_t p = c.premul();
    for (int y = k.y; y < k.bottom(); y++) std::fill_n(row(y) + k.x, k.w, p);
}

Image Image::copy(const Rect& r) const {
    Image out(r.w, r.h);
    Rect k = r.intersected(rect());
    for (int y = k.y; y < k.bottom(); y++)
        memcpy(out.row(y - r.y) + (k.x - r.x), row(y) + k.x, (size_t)k.w * 4);
    return out;
}

namespace {

// Weights of the source pixels for each output pixel along one axis: a box (area) filter when
// shrinking, a tent (bilinear) when enlarging.
struct Tap { int first, count; std::vector<float> w; };

std::vector<Tap> axisTaps(int src, int dst) {
    std::vector<Tap> taps(dst);
    double scale = (double)src / dst;
    for (int i = 0; i < dst; i++) {
        Tap& t = taps[i];
        if (scale > 1.0) {  // box: the source span [i*scale, (i+1)*scale)
            double a = i * scale, b = (i + 1) * scale;
            int s0 = (int)std::floor(a), s1 = std::min(src, (int)std::ceil(b));
            t.first = s0;
            t.count = s1 - s0;
            double sum = 0;
            for (int s = s0; s < s1; s++) {
                double ov = std::min(b, s + 1.0) - std::max(a, (double)s);
                t.w.push_back((float)ov);
                sum += ov;
            }
            for (float& w : t.w) w = (float)(w / sum);
        } else {
            double c = (i + 0.5) * scale - 0.5;
            int s0 = (int)std::floor(c);
            double f = c - s0;
            int a = std::clamp(s0, 0, src - 1), b = std::clamp(s0 + 1, 0, src - 1);
            if (a == b) {
                t.first = a;
                t.count = 1;
                t.w = {1.f};
            } else {
                t.first = a;
                t.count = 2;
                t.w = {(float)(1 - f), (float)f};
            }
        }
    }
    return taps;
}

}  // namespace

Image Image::scaled(int w, int h) const {
    if (w <= 0 || h <= 0) return Image();
    if (empty()) return Image(w, h);
    if (w == w_ && h == h_) return *this;
    std::vector<Tap> tx = axisTaps(w_, w), ty = axisTaps(h_, h);
    // horizontal pass into floats (4 channels), then vertical
    std::vector<float> tmp((size_t)w * h_ * 4);
    for (int y = 0; y < h_; y++) {
        const uint32_t* s = row(y);
        float* d = &tmp[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            const Tap& t = tx[x];
            float a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < t.count; k++) {
                uint32_t p = s[t.first + k];
                float wt = t.w[k];
                a += wt * (p >> 24);
                r += wt * ((p >> 16) & 255);
                g += wt * ((p >> 8) & 255);
                b += wt * (p & 255);
            }
            d[x * 4] = a; d[x * 4 + 1] = r; d[x * 4 + 2] = g; d[x * 4 + 3] = b;
        }
    }
    Image out(w, h);
    for (int y = 0; y < h; y++) {
        const Tap& t = ty[y];
        uint32_t* d = out.row(y);
        for (int x = 0; x < w; x++) {
            float c[4] = {0, 0, 0, 0};
            for (int k = 0; k < t.count; k++) {
                const float* s = &tmp[((size_t)(t.first + k) * w + x) * 4];
                float wt = t.w[k];
                c[0] += wt * s[0]; c[1] += wt * s[1]; c[2] += wt * s[2]; c[3] += wt * s[3];
            }
            uint32_t A = (uint32_t)std::min(255.f, c[0] + 0.5f);
            uint32_t R = std::min(A, (uint32_t)std::min(255.f, c[1] + 0.5f));
            uint32_t G = std::min(A, (uint32_t)std::min(255.f, c[2] + 0.5f));
            uint32_t B = std::min(A, (uint32_t)std::min(255.f, c[3] + 0.5f));
            d[x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    return out;
}

Image Image::tinted(Color c) const {
    Image out(w_, h_);
    uint32_t r = c.r, g = c.g, b = c.b;
    for (size_t i = 0; i < px_.size(); i++) {
        uint32_t a = ((px_[i] >> 24) * c.a + 127) / 255;
        if (!a) continue;
        out.px_[i] = (a << 24) | (((r * a + 127) / 255) << 16) | (((g * a + 127) / 255) << 8) | ((b * a + 127) / 255);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// crc32 / adler32

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    while (size) {
        size_t n = std::min<size_t>(size, 5552);
        size -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

// ------------------------------------------------------------------------------------------------
// inflate

namespace {

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    int padding = 0;  // zero bytes added past the end of the data
    void refill() {
        while (cnt <= 56) {
            if (p < end) buf |= (uint64_t)*p++ << cnt;
            else padding++;
            cnt += 8;
        }
    }
    bool overrun() const { return padding * 8 > cnt; }  // read bits that aren't there
    uint32_t peek(int n) {
        if (cnt < n) refill();
        return (uint32_t)(buf & ((1ull << n) - 1));
    }
    void drop(int n) { buf >>= n; cnt -= n; }
    uint32_t get(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        drop(n);
        return v;
    }
    void alignByte() { drop(cnt & 7); }
};

// canonical Huffman decoding table: entry = symbol << 4 | length (one level, 2^maxLen entries)
struct Huff {
    std::vector<uint16_t> table;
    int bits = 0;
    bool build(const uint8_t* lens, int n) {
        int count[16] = {0};
        int maxLen = 0;
        for (int i = 0; i < n; i++) {
            count[lens[i]]++;
            maxLen = std::max<int>(maxLen, lens[i]);
        }
        count[0] = 0;
        if (maxLen == 0) {  // no codes (allowed for distances): everything decodes as invalid
            bits = 1;
            table.assign(2, 0);
            return true;
        }
        int left = 1;
        for (int l = 1; l <= 15; l++) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false;  // over-subscribed
        }
        int next[16];
        int code = 0;
        for (int l = 1; l <= 15; l++) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        bits = maxLen;
        table.assign((size_t)1 << bits, 0);
        for (int s = 0; s < n; s++) {
            int l = lens[s];
            if (!l) continue;
            int c = next[l]++;
            int rev = 0;  // bit-reverse the code (deflate sends codes MSB first)
            for (int k = 0; k < l; k++) rev |= ((c >> k) & 1) << (l - 1 - k);
            for (int k = rev; k < (1 << bits); k += 1 << l) table[k] = (uint16_t)(s << 4 | l);
        }
        return true;
    }
    int decode(BitReader& br) const {
        uint16_t e = table[br.peek(bits)];
        int l = e & 15;
        if (!l) return -1;
        br.drop(l);
        return e >> 4;
    }
};

}  // namespace

// checkAdler false: PNG data whose deflate stream is complete is accepted even with a wrong or
// missing checksum (as libpng and browsers do; such files exist)
static bool inflateImpl(const uint8_t* data, size_t size, std::vector<uint8_t>& out, bool checkAdler) {
    out.clear();
    if (size < 2) return false;
    if ((data[0] & 15) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
    BitReader br{data + 2, data + size};
    Huff lit, dist;
    Huff fixedLit, fixedDist;
    bool fixedBuilt = false;
    for (;;) {
        int final = br.get(1);
        int type = br.get(2);
        if (br.overrun()) return false;
        if (type == 0) {
            br.alignByte();
            uint32_t len = br.get(16), nlen = br.get(16);
            if ((len ^ 0xFFFF) != nlen) return false;
            // the bit buffer holds bytes already read ahead: give them back
            if (br.overrun()) return false;
            br.p -= (br.cnt / 8 - br.padding);
            br.buf = 0;
            br.cnt = 0;
            br.padding = 0;
            if ((size_t)(br.end - br.p) < len) return false;
            out.insert(out.end(), br.p, br.p + len);
            br.p += len;
        } else if (type == 1 || type == 2) {
            const Huff* L;
            const Huff* D;
            if (type == 1) {
                if (!fixedBuilt) {
                    uint8_t l[288], d[30];
                    for (int i = 0; i < 144; i++) l[i] = 8;
                    for (int i = 144; i < 256; i++) l[i] = 9;
                    for (int i = 256; i < 280; i++) l[i] = 7;
                    for (int i = 280; i < 288; i++) l[i] = 8;
                    for (int i = 0; i < 30; i++) d[i] = 5;
                    fixedLit.build(l, 288);
                    fixedDist.build(d, 30);
                    fixedBuilt = true;
                }
                L = &fixedLit;
                D = &fixedDist;
            } else {
                int hlit = br.get(5) + 257, hdist = br.get(5) + 1, hclen = br.get(4) + 4;
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) cl[kClOrder[i]] = (uint8_t)br.get(3);
                Huff clh;
                if (!clh.build(cl, 19)) return false;
                uint8_t lens[320] = {0};
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = clh.decode(br);
                    if (sym < 0 || br.overrun()) return false;
                    if (sym < 16) {
                        lens[n++] = (uint8_t)sym;
                    } else {
                        int rep, val = 0;
                        if (sym == 16) {
                            if (n == 0) return false;
                            val = lens[n - 1];
                            rep = 3 + br.get(2);
                        } else if (sym == 17) {
                            rep = 3 + br.get(3);
                        } else {
                            rep = 11 + br.get(7);
                        }
                        if (n + rep > hlit + hdist) return false;
                        while (rep--) lens[n++] = (uint8_t)val;
                    }
                }
                if (!lens[256]) return false;
                if (!lit.build(lens, hlit) || !dist.build(lens + hlit, hdist)) return false;
                L = &lit;
                D = &dist;
            }
            for (;;) {
                int sym = L->decode(br);
                if (sym < 0 || br.overrun()) return false;
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int len = kLenBase[sym] + br.get(kLenExtra[sym]);
                    int ds = D->decode(br);
                    if (ds < 0 || ds >= 30) return false;
                    size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
                    if (d > out.size()) return false;
                    size_t from = out.size() - d;
                    out.resize(out.size() + len);
                    uint8_t* o = out.data() + out.size() - len;
                    const uint8_t* s = out.data() + from;
                    for (int i = 0; i < len; i++) o[i] = s[i];  // may overlap: byte by byte
                }
            }
        } else {
            return false;
        }
        if (final) break;
    }
    if (!checkAdler) return true;
    // adler32 trailer (after the deflate stream, byte aligned)
    br.alignByte();
    uint32_t a = 0;
    for (int i = 0; i < 4; i++) a = (a << 8) | br.get(8);
    if (br.overrun()) return false;
    return a == adler32(out.data(), out.size());
}

bool zlibInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    return inflateImpl(data, size, out, true);
}

// ------------------------------------------------------------------------------------------------
// deflate: LZ77 with hash chains (lazy matching) and per-block choice of stored / fixed / dynamic
// Huffman codes.

namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint64_t buf = 0;
    int cnt = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t v, int n) {
        buf |= (uint64_t)v << cnt;
        cnt += n;
        while (cnt >= 8) {
            out.push_back((uint8_t)buf);
            buf >>= 8;
            cnt -= 8;
        }
    }
    void putRev(uint32_t code, int n) {  // Huffman code, MSB first
        uint32_t r = 0;
        for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
        put(r, n);
    }
    void flushByte() {
        if (cnt) put(0, 8 - cnt);
    }
};

// Huffman code lengths limited to maxBits (JPEG Annex K.3 adjustment), then canonical codes.
void huffLengths(const uint32_t* freq, int n, int maxBits, uint8_t* lens) {
    for (int i = 0; i < n; i++) lens[i] = 0;
    std::vector<int> syms;
    for (int i = 0; i < n; i++)
        if (freq[i]) syms.push_back(i);
    if (syms.empty()) return;
    if (syms.size() == 1) {
        lens[syms[0]] = 1;
        return;
    }
    // build the tree with two queues (leaves sorted by frequency, internal nodes in order)
    std::sort(syms.begin(), syms.end(), [&](int a, int b) { return freq[a] != freq[b] ? freq[a] < freq[b] : a < b; });
    int m = (int)syms.size();
    std::vector<uint64_t> w(2 * m);
    std::vector<int> parent(2 * m, -1);
    for (int i = 0; i < m; i++) w[i] = freq[syms[i]];
    int leaf = 0, node = m, nodesEnd = m;
    auto pick = [&]() {
        if (leaf < m && (node >= nodesEnd || w[leaf] <= w[node])) return leaf++;
        return node++;
    };
    for (int k = 0; k < m - 1; k++) {
        int a = pick(), b = pick();
        w[nodesEnd] = w[a] + w[b];
        parent[a] = parent[b] = nodesEnd;
        nodesEnd++;
    }
    int root = nodesEnd - 1;
    std::vector<int> depth(2 * m, 0);
    for (int i = root - 1; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
    int blCount[64] = {0};
    int maxLen = 0;
    for (int i = 0; i < m; i++) {
        blCount[depth[i]]++;
        maxLen = std::max(maxLen, depth[i]);
    }
    for (int i = maxLen; i > maxBits; i--) {
        while (blCount[i] > 0) {
            int j = i - 2;
            while (blCount[j] == 0) j--;
            blCount[i] -= 2;
            blCount[i - 1]++;
            blCount[j + 1] += 2;
            blCount[j]--;
        }
    }
    // the most frequent symbols get the shortest codes
    int idx = m - 1;
    for (int l = 1; l <= maxBits; l++)
        for (int k = 0; k < blCount[l]; k++) lens[syms[idx--]] = (uint8_t)l;
}

void canonicalCodes(const uint8_t* lens, int n, uint16_t* codes) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int code = 0;
    for (int l = 1; l <= 15; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (int i = 0; i < n; i++) codes[i] = lens[i] ? (uint16_t)next[lens[i]]++ : 0;
}

int lenSymbol(int len) {  // 3..258 -> 0..28
    int s = 0;
    while (s < 28 && kLenBase[s + 1] <= len) s++;
    return s;
}
int distSymbol(int d) {
    int s = 0;
    while (s < 29 && kDistBase[s + 1] <= d) s++;
    return s;
}

struct LzSym { uint16_t litOrLen; uint16_t dist; };  // dist 0: literal

void encodeBlock(BitWriter& bw, const uint8_t* data, size_t start, size_t end, const std::vector<LzSym>& syms,
                 bool final) {
    static uint8_t lenSym[259];
    static bool tables = false;
    static uint8_t distSymSmall[512], distSymLarge[256];
    if (!tables) {
        for (int l = 3; l <= 258; l++) lenSym[l] = (uint8_t)lenSymbol(l);
        for (int d = 1; d <= 256; d++) distSymSmall[d] = (uint8_t)distSymbol(d);
        for (int k = 0; k < 256; k++) distSymLarge[k] = (uint8_t)distSymbol(k * 128 + 1);
        tables = true;
    }
    auto dsym = [&](int d) { return d <= 256 ? distSymSmall[d] : distSymLarge[(d - 1) >> 7]; };

    uint32_t lf[286] = {0}, df[30] = {0};
    for (const LzSym& s : syms) {
        if (!s.dist) lf[s.litOrLen]++;
        else {
            lf[257 + lenSym[s.litOrLen]]++;
            df[dsym(s.dist)]++;
        }
    }
    lf[256] = 1;
    uint8_t ll[286], dl[30];
    huffLengths(lf, 286, 15, ll);
    huffLengths(df, 30, 15, dl);
    int nd = 0;
    for (int i = 0; i < 30; i++)
        if (dl[i]) nd++;
    if (nd == 0) dl[0] = dl[1] = 1;  // at least one distance code
    else if (nd == 1) {  // some decoders dislike a lone 1-bit code: add a second
        for (int i = 0; i < 30; i++)
            if (!dl[i]) { dl[i] = 1; break; }
    }
    int hlit = 286;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    int hdist = 30;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;
    // run-length encode the code lengths
    uint8_t all[316];
    memcpy(all, ll, hlit);
    memcpy(all + hlit, dl, hdist);
    int total = hlit + hdist;
    std::vector<std::pair<uint8_t, uint8_t>> rle;  // (symbol, extra value)
    for (int i = 0; i < total;) {
        int v = all[i], run = 1;
        while (i + run < total && all[i + run] == v) run++;
        int r = run;
        if (v == 0) {
            while (r >= 11) { int k = std::min(r, 138); rle.push_back({18, (uint8_t)(k - 11)}); r -= k; }
            if (r >= 3) { rle.push_back({17, (uint8_t)(r - 3)}); r = 0; }
            while (r--) rle.push_back({0, 0});
        } else {
            rle.push_back({(uint8_t)v, 0});
            r--;
            while (r >= 3) { int k = std::min(r, 6); rle.push_back({16, (uint8_t)(k - 3)}); r -= k; }
            while (r--) rle.push_back({(uint8_t)v, 0});
        }
        i += run;
    }
    uint32_t cf[19] = {0};
    for (auto& e : rle) cf[e.first]++;
    uint8_t cl[19];
    huffLengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && !cl[kClOrder[hclen - 1]]) hclen--;

    // sizes in bits of the three ways
    static uint8_t fixedL[288];
    if (!fixedL[0]) {
        for (int k = 0; k < 288; k++) fixedL[k] = k < 144 ? 8 : k < 256 ? 9 : k < 280 ? 7 : 8;
    }
    uint64_t extraBits = 0;
    for (int s = 0; s < 29; s++) extraBits += (uint64_t)lf[257 + s] * kLenExtra[s];
    for (int s = 0; s < 30; s++) extraBits += (uint64_t)df[s] * kDistExtra[s];
    uint64_t dynBits = 14 + 3 * (uint64_t)hclen + extraBits, fixBits = extraBits;
    for (auto& e : rle) dynBits += cl[e.first] + (e.first == 16 ? 2 : e.first == 17 ? 3 : e.first == 18 ? 7 : 0);
    for (int i = 0; i < 286; i++) {
        dynBits += (uint64_t)lf[i] * ll[i];
        fixBits += (uint64_t)lf[i] * fixedL[i];
    }
    for (int i = 0; i < 30; i++) {
        dynBits += (uint64_t)df[i] * dl[i];
        fixBits += (uint64_t)df[i] * 5;
    }
    uint64_t storedBits = (end - start) * 8 + ((end - start + 65534) / 65535) * 40 + 8;

    if (storedBits <= dynBits && storedBits <= fixBits) {
        size_t pos = start;
        do {
            size_t n = std::min<size_t>(65535, end - pos);
            bool last = final && pos + n >= end;
            bw.put(last ? 1 : 0, 1);
            bw.put(0, 2);
            bw.flushByte();
            bw.put((uint32_t)n, 16);
            bw.put((uint32_t)(n ^ 0xFFFF), 16);
            for (size_t i = 0; i < n; i++) bw.out.push_back(data[pos + i]);
            pos += n;
        } while (pos < end);
        return;
    }
    uint8_t useL[288], useD[30];
    uint16_t lc[288], dc[30];
    bw.put(final ? 1 : 0, 1);
    if (fixBits <= dynBits) {
        bw.put(1, 2);
        memcpy(useL, fixedL, 288);
        for (int i = 0; i < 30; i++) useD[i] = 5;
        canonicalCodes(useL, 288, lc);
        canonicalCodes(useD, 30, dc);
    } else {
        bw.put(2, 2);
        bw.put(hlit - 257, 5);
        bw.put(hdist - 1, 5);
        bw.put(hclen - 4, 4);
        for (int i = 0; i < hclen; i++) bw.put(cl[kClOrder[i]], 3);
        uint16_t cc[19];
        canonicalCodes(cl, 19, cc);
        for (auto& e : rle) {
            bw.putRev(cc[e.first], cl[e.first]);
            if (e.first == 16) bw.put(e.second, 2);
            else if (e.first == 17) bw.put(e.second, 3);
            else if (e.first == 18) bw.put(e.second, 7);
        }
        memset(useL, 0, sizeof useL);
        memcpy(useL, ll, 286);
        memcpy(useD, dl, 30);
        canonicalCodes(useL, 288, lc);
        canonicalCodes(useD, 30, dc);
    }
    for (const LzSym& s : syms) {
        if (!s.dist) {
            bw.putRev(lc[s.litOrLen], useL[s.litOrLen]);
        } else {
            int ls = lenSym[s.litOrLen];
            bw.putRev(lc[257 + ls], useL[257 + ls]);
            if (kLenExtra[ls]) bw.put(s.litOrLen - kLenBase[ls], kLenExtra[ls]);
            int ds = dsym(s.dist);
            bw.putRev(dc[ds], useD[ds]);
            if (kDistExtra[ds]) bw.put(s.dist - kDistBase[ds], kDistExtra[ds]);
        }
    }
    bw.putRev(lc[256], useL[256]);
}

}  // namespace

std::vector<uint8_t> zlibDeflate(const uint8_t* data, size_t size, int level) {
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 64);
    out.push_back(0x78);
    out.push_back(level <= 0 ? 0x01 : level >= 7 ? 0xDA : 0x9C);
    BitWriter bw(out);
    level = std::clamp(level, 0, 9);
    const int WIN = 32768, HBITS = 15, HSIZE = 1 << HBITS;
    int maxChain = level == 0 ? 0 : level <= 3 ? 8 : level <= 6 ? 64 : 512;
    int niceLen = level <= 3 ? 32 : level <= 6 ? 128 : 258;
    std::vector<int32_t> head(level ? HSIZE : 1, -1), prev(level ? WIN : 1, -1);
    auto hashAt = [&](size_t i) {
        return ((uint32_t)data[i] * 2654435761u ^ ((uint32_t)data[i + 1] << 8) * 40503u ^ (uint32_t)data[i + 2] * 0x9E3779B1u) >> (32 - HBITS) & (HSIZE - 1);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= size || !maxChain) return;
        uint32_t h = hashAt(i);
        prev[i & (WIN - 1)] = head[h];
        head[h] = (int32_t)i;
    };
    auto findMatch = [&](size_t i, int& bestLen, int& bestDist) {
        bestLen = 0;
        bestDist = 0;
        if (i + 2 >= size || !maxChain) return;
        int32_t cand = head[hashAt(i)];
        int chain = maxChain;
        size_t maxLen = std::min<size_t>(258, size - i);
        while (cand >= 0 && chain-- > 0 && (size_t)bestLen < maxLen) {
            size_t d = i - (size_t)cand;
            if (d > (size_t)WIN - 262 || d == 0) break;
            const uint8_t* a = data + i;
            const uint8_t* b = data + cand;
            if (b[bestLen] == a[bestLen] && b[0] == a[0]) {
                size_t l = 0;
                while (l < maxLen && a[l] == b[l]) l++;
                if ((int)l > bestLen) {
                    bestLen = (int)l;
                    bestDist = (int)d;
                    if (l >= (size_t)niceLen) break;
                }
            }
            int32_t nx = prev[cand & (WIN - 1)];
            if (nx >= cand) break;
            cand = nx;
        }
        if (bestLen < 3) bestLen = 0;
    };

    std::vector<LzSym> syms;
    syms.reserve(1 << 15);
    size_t blockStart = 0, i = 0;
    const size_t BLOCK_SYMS = 1 << 14;
    if (size == 0) {
        bw.put(1, 1);
        bw.put(1, 2);
        bw.put(0, 7);  // end of block (fixed code 256 = 7 zero bits)
    }
    while (i < size) {
        int len, dist;
        findMatch(i, len, dist);
        if (len && level >= 4 && len < niceLen && i + 1 < size) {  // lazy: a longer match one later?
            insert(i);
            int len2, dist2;
            findMatch(i + 1, len2, dist2);
            if (len2 > len) {
                syms.push_back({data[i], 0});
                i++;
                len = len2;
                dist = dist2;
                insert(i);
            }
            syms.push_back({(uint16_t)len, (uint16_t)dist});
            for (int k = 1; k < len; k++) insert(i + k);
            i += len;
        } else if (len) {
            syms.push_back({(uint16_t)len, (uint16_t)dist});
            for (int k = 0; k < len; k++) insert(i + k);
            i += len;
        } else {
            insert(i);
            syms.push_back({data[i], 0});
            i++;
        }
        if (syms.size() >= BLOCK_SYMS || i >= size) {
            encodeBlock(bw, data, blockStart, i, syms, i >= size);
            syms.clear();
            blockStart = i;
        }
    }
    bw.flushByte();
    uint32_t a = adler32(data, size);
    for (int k = 3; k >= 0; k--) out.push_back((uint8_t)(a >> (k * 8)));
    return out;
}

// ------------------------------------------------------------------------------------------------
// PNG

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
}

inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

bool unfilter(uint8_t* rows, int h, size_t stride, int bpp) {
    std::vector<uint8_t> zero(stride, 0);
    for (int y = 0; y < h; y++) {
        uint8_t* r = rows + (size_t)y * (stride + 1);
        int f = r[0];
        uint8_t* cur = r + 1;
        const uint8_t* up = y ? rows + (size_t)(y - 1) * (stride + 1) + 1 : zero.data();
        switch (f) {
            case 0: break;
            case 1: for (size_t i = bpp; i < stride; i++) cur[i] += cur[i - bpp]; break;
            case 2: for (size_t i = 0; i < stride; i++) cur[i] += up[i]; break;
            case 3:
                for (size_t i = 0; i < stride; i++) cur[i] += (uint8_t)(((i >= (size_t)bpp ? cur[i - bpp] : 0) + up[i]) >> 1);
                break;
            case 4:
                for (size_t i = 0; i < stride; i++)
                    cur[i] += paeth(i >= (size_t)bpp ? cur[i - bpp] : 0, up[i], i >= (size_t)bpp ? up[i - bpp] : 0);
                break;
            default: return false;
        }
    }
    return true;
}

}  // namespace

bool decodePng(const uint8_t* data, size_t size, Image& out, std::string* error) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || memcmp(data, sig, 8) != 0) return fail(error, "not a PNG file");
    size_t pos = 8;
    uint32_t W = 0, H = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat;
    uint8_t pal[256][4];
    int palSize = 0;
    for (int i = 0; i < 256; i++) pal[i][0] = pal[i][1] = pal[i][2] = 0, pal[i][3] = 255;
    bool hasKey = false;
    uint16_t key[3] = {0, 0, 0};
    bool seenIhdr = false;
    while (pos + 12 <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t* type = data + pos + 4;
        const uint8_t* d = data + pos + 8;
        if (len > size - pos - 12) return fail(error, "truncated PNG");
        if (!memcmp(type, "IHDR", 4)) {
            if (len < 13) return fail(error, "bad IHDR");
            W = be32(d);
            H = be32(d + 4);
            depth = d[8];
            ctype = d[9];
            interlace = d[12];
            seenIhdr = true;
            if (d[10] != 0 || d[11] != 0) return fail(error, "unknown PNG compression");
        } else if (!memcmp(type, "PLTE", 4)) {
            palSize = std::min<int>(256, len / 3);
            for (int i = 0; i < palSize; i++) {
                pal[i][0] = d[i * 3];
                pal[i][1] = d[i * 3 + 1];
                pal[i][2] = d[i * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (ctype == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = d[i];
            } else if (ctype == 0 && len >= 2) {
                hasKey = true;
                key[0] = (uint16_t)(d[0] << 8 | d[1]);
            } else if (ctype == 2 && len >= 6) {
                hasKey = true;
                for (int k = 0; k < 3; k++) key[k] = (uint16_t)(d[k * 2] << 8 | d[k * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), d, d + len);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!seenIhdr || W == 0 || H == 0 || W > 30000 || H > 30000) return fail(error, "bad PNG size");
    int channels;
    switch (ctype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail(error, "bad PNG colour type");
    }
    if (!(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) return fail(error, "bad PNG bit depth");
    if (ctype == 3 && depth == 16) return fail(error, "bad PNG bit depth");
    if ((ctype == 2 || ctype == 4 || ctype == 6) && depth < 8) return fail(error, "bad PNG bit depth");
    std::vector<uint8_t> raw;
    if (!inflateImpl(idat.data(), idat.size(), raw, false)) return fail(error, "corrupt PNG data");
    int bitsPerPixel = channels * depth;
    int bpp = std::max(1, bitsPerPixel / 8);
    out.resize((int)W, (int)H);

    auto sample = [&](const uint8_t* r, uint32_t x, int c) -> uint32_t {  // c-th channel of pixel x
        if (depth == 8) return r[x * channels + c];
        if (depth == 16) return (uint32_t)r[(x * channels + c) * 2] << 8 | r[(x * channels + c) * 2 + 1];
        uint32_t bit = x * depth;  // channels == 1 here
        return (r[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    };
    auto to8 = [&](uint32_t v) -> uint32_t {
        if (depth == 16) return (v * 255 + 32767) / 65535;
        if (depth == 8) return v;
        return v * 255 / ((1u << depth) - 1);
    };
    auto putRow = [&](const uint8_t* r, uint32_t n, uint32_t y0, uint32_t x0, uint32_t dx) {
        uint32_t* dst = out.row((int)y0);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t R, G, B, A = 255;
            if (ctype == 3) {
                uint32_t idx = sample(r, i, 0);
                const uint8_t* p = pal[idx & 255];
                R = p[0]; G = p[1]; B = p[2]; A = p[3];
            } else if (ctype == 0 || ctype == 4) {
                uint32_t v = sample(r, i, 0);
                if (ctype == 0 && hasKey && v == key[0]) A = 0;
                R = G = B = to8(v);
                if (ctype == 4) A = to8(sample(r, i, 1));
            } else {
                uint32_t v0 = sample(r, i, 0), v1 = sample(r, i, 1), v2 = sample(r, i, 2);
                if (ctype == 2 && hasKey && v0 == key[0] && v1 == key[1] && v2 == key[2]) A = 0;
                R = to8(v0); G = to8(v1); B = to8(v2);
                if (ctype == 6) A = to8(sample(r, i, 3));
            }
            dst[x0 + i * dx] = Color((int)R, (int)G, (int)B, (int)A).premul();
        }
    };

    size_t off = 0;
    if (!interlace) {
        size_t stride = ((size_t)W * bitsPerPixel + 7) / 8;
        if (raw.size() < (stride + 1) * H) return fail(error, "truncated PNG data");
        if (!unfilter(raw.data(), (int)H, stride, bpp)) return fail(error, "bad PNG filter");
        for (uint32_t y = 0; y < H; y++) putRow(raw.data() + y * (stride + 1) + 1, W, y, 0, 1);
    } else {
        static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dxs[7] = {8, 8, 4, 4, 2, 2, 1}, dys[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; p++) {
            uint32_t pw = W > (uint32_t)sx[p] ? (W - sx[p] + dxs[p] - 1) / dxs[p] : 0;
            uint32_t ph = H > (uint32_t)sy[p] ? (H - sy[p] + dys[p] - 1) / dys[p] : 0;
            if (!pw || !ph) continue;
            size_t stride = ((size_t)pw * bitsPerPixel + 7) / 8;
            if (raw.size() < off + (stride + 1) * ph) return fail(error, "truncated PNG data");
            if (!unfilter(raw.data() + off, (int)ph, stride, bpp)) return fail(error, "bad PNG filter");
            for (uint32_t y = 0; y < ph; y++)
                putRow(raw.data() + off + y * (stride + 1) + 1, pw, sy[p] + y * dys[p], sx[p], dxs[p]);
            off += (stride + 1) * ph;
        }
    }
    return true;
}

static bool readFile(const std::string& path, std::vector<uint8_t>& buf) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    buf.resize((size_t)n);
    bool ok = n == 0 || fread(buf.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

bool loadPng(const std::string& path, Image& out, std::string* error) {
    std::vector<uint8_t> buf;
    if (!readFile(path, buf)) return fail(error, "cannot read the file");
    return decodePng(buf.data(), buf.size(), out, error);
}

std::vector<uint8_t> encodePng(const Image& img) {
    int W = img.width(), H = img.height();
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        uint32_t n = (uint32_t)d.size();
        for (int k = 3; k >= 0; k--) png.push_back((uint8_t)(n >> (k * 8)));
        size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), d.begin(), d.end());
        uint32_t c = crc32(png.data() + start, png.size() - start);
        for (int k = 3; k >= 0; k--) png.push_back((uint8_t)(c >> (k * 8)));
    };
    std::vector<uint8_t> ihdr(13, 0);
    for (int k = 0; k < 4; k++) {
        ihdr[k] = (uint8_t)((uint32_t)W >> ((3 - k) * 8));
        ihdr[4 + k] = (uint8_t)((uint32_t)H >> ((3 - k) * 8));
    }
    ihdr[8] = 8;
    ihdr[9] = 6;  // RGBA
    chunk("IHDR", ihdr);
    size_t stride = (size_t)W * 4;
    std::vector<uint8_t> raw((stride + 1) * H);
    std::vector<uint8_t> cur(stride), prevRow(stride, 0), cand(stride), best(stride);
    for (int y = 0; y < H; y++) {
        const uint32_t* s = img.row(y);
        for (int x = 0; x < W; x++) {
            Color c = Color::fromPremul(s[x]);
            cur[x * 4] = c.r; cur[x * 4 + 1] = c.g; cur[x * 4 + 2] = c.b; cur[x * 4 + 3] = c.a;
        }
        // the filter whose output has the smallest sum of absolute (signed) values
        uint64_t bestScore = ~0ull;
        int bestF = 0;
        for (int f = 0; f < 5; f++) {
            uint64_t score = 0;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= 4 ? cur[i - 4] : 0, b = prevRow[i], c = i >= 4 ? prevRow[i - 4] : 0;
                int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paeth(a, b, c);
                uint8_t v = (uint8_t)(cur[i] - pred);
                cand[i] = v;
                score += v < 128 ? v : 256 - v;
            }
            if (score < bestScore) {
                bestScore = score;
                bestF = f;
                best.swap(cand);
            }
        }
        raw[y * (stride + 1)] = (uint8_t)bestF;
        memcpy(&raw[y * (stride + 1) + 1], best.data(), stride);
        prevRow.swap(cur);
        cur.resize(stride);
    }
    chunk("IDAT", zlibDeflate(raw.data(), raw.size(), 6));
    chunk("IEND", {});
    return png;
}

bool savePng(const std::string& path, const Image& img, std::string* error) {
    std::vector<uint8_t> png = encodePng(img);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return fail(error, "cannot write the file");
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    ok = fclose(f) == 0 && ok;
    return ok || fail(error, "cannot write the file");
}
