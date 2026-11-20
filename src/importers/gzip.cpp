// gzip and raw inflate (see gzip.h).
#include "importers/gzip.h"

#include <cstring>
#include <memory>

#include "core/util.h"
#include "gfx/image.h"

namespace gzip {

namespace {

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;
    int cnt = 0;
    int extra = 0;  // bits of zeros in buf given past the end of the data (an overrun once they are used)
    void fill() {
        while (cnt <= 56) {
            if (p < end) buf |= (uint64_t)*p++ << cnt;
            else extra += 8;
            cnt += 8;
        }
    }
    uint32_t peek(int n) {
        if (cnt < n) fill();
        return (uint32_t)(buf & ((1ull << n) - 1));
    }
    void drop(int n) {
        buf >>= n;
        cnt -= n;
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        drop(n);
        return v;
    }
    bool overrun() const { return cnt < extra; }  // more consumed than there was
    void alignByte() { drop(cnt & 7); }
    // the position (in the input) of the next unread byte, once byte aligned
    const uint8_t* bytePos() const { return p - (cnt - extra) / 8; }
    void reset(const uint8_t* at) {
        p = at;
        buf = 0;
        cnt = 0;
        extra = 0;
    }
};

constexpr int FAST = 10;

// a canonical Huffman code: a FAST-bit lookup table (symbol << 4 | length), the rest decoded bit by bit
struct Huff {
    uint16_t fast[1 << FAST];
    uint16_t count[16];
    uint16_t symbols[320];
    bool build(const uint8_t* lens, int n) {
        memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[lens[i]]++;
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;  // over-subscribed
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; i++)
            if (lens[i]) symbols[offs[lens[i]]++] = (uint16_t)i;
        // the fast table: codes of up to FAST bits, bit-reversed (deflate sends them MSB first)
        memset(fast, 0, sizeof fast);
        uint32_t code = 0;
        int k = 0;
        for (int len = 1; len <= FAST; len++) {
            for (int c = 0; c < count[len]; c++, k++, code++) {
                uint32_t rev = 0;
                for (int b = 0; b < len; b++) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (uint32_t fill = rev; fill < (1u << FAST); fill += 1u << len)
                    fast[fill] = (uint16_t)(symbols[k] << 4 | len);
            }
            code <<= 1;
        }
        return true;
    }
    int decode(Bits& br) const {
        uint32_t look = br.peek(FAST);
        uint16_t e = fast[look];
        if (e) {
            br.drop(e & 15);
            return e >> 4;
        }
        // longer codes (puff's way)
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)br.get(1);
            int c = count[len];
            if (code - c < first) return symbols[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

}  // namespace

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t* used, size_t limit) {
    Bits br{data, data + size};
    static Huff* fixedLit = nullptr;
    static Huff* fixedDist = nullptr;
    static Huff fixedTables[2];
    if (!fixedLit) {  // (built once; harmless if two threads race: same contents)
        uint8_t l[288], d[30];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        for (int i = 0; i < 30; i++) d[i] = 5;
        fixedTables[0].build(l, 288);
        fixedTables[1].build(d, 30);
        fixedDist = &fixedTables[1];
        fixedLit = &fixedTables[0];
    }
    auto lit = std::make_unique<Huff>();
    auto dist = std::make_unique<Huff>();
    size_t n = out.size();
    auto room = [&](size_t more) {
        if (n + more > limit) return false;  // (more than it may hold: a zip bomb)
        if (n + more > out.size()) out.resize(std::max(std::min(out.size() * 2, limit), n + more + 65536));
        return true;
    };
    for (;;) {
        int final = (int)br.get(1);
        int type = (int)br.get(2);
        if (br.overrun()) return false;
        if (type == 0) {
            br.alignByte();
            uint32_t len = br.get(16), nlen = br.get(16);
            if (br.overrun() || (len ^ 0xFFFF) != nlen) return false;
            const uint8_t* at = br.bytePos();
            if ((size_t)(data + size - at) < len) return false;
            if (!room(len)) return false;
            memcpy(out.data() + n, at, len);
            n += len;
            br.reset(at + len);
        } else if (type == 1 || type == 2) {
            const Huff* L = fixedLit;
            const Huff* D = fixedDist;
            if (type == 2) {
                int hlit = (int)br.get(5) + 257, hdist = (int)br.get(5) + 1, hclen = (int)br.get(4) + 4;
                if (hlit > 286 || hdist > 30) return false;
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) cl[kClOrder[i]] = (uint8_t)br.get(3);
                Huff clh;
                if (!clh.build(cl, 19)) return false;
                uint8_t lens[320] = {0};
                int k = 0;
                while (k < hlit + hdist) {
                    int sym = clh.decode(br);
                    if (sym < 0 || br.overrun()) return false;
                    if (sym < 16) {
                        lens[k++] = (uint8_t)sym;
                        continue;
                    }
                    int rep, v = 0;
                    if (sym == 16) {
                        if (k == 0) return false;
                        v = lens[k - 1];
                        rep = 3 + (int)br.get(2);
                    } else if (sym == 17) {
                        rep = 3 + (int)br.get(3);
                    } else {
                        rep = 11 + (int)br.get(7);
                    }
                    if (k + rep > hlit + hdist) return false;
                    while (rep--) lens[k++] = (uint8_t)v;
                }
                if (!lens[256]) return false;
                if (!lit->build(lens, hlit) || !dist->build(lens + hlit, hdist)) return false;
                L = lit.get();
                D = dist.get();
            }
            for (;;) {
                int sym = L->decode(br);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (!room(1)) return false;
                    out[n++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 256) break;
                sym -= 257;
                if (sym >= 29) return false;
                size_t len = kLenBase[sym] + br.get(kLenExtra[sym]);
                int ds = D->decode(br);
                if (ds < 0 || ds >= 30) return false;
                size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
                if (br.overrun()) return false;
                if (d > n) return false;
                if (!room(len)) return false;
                uint8_t* o = out.data() + n;
                const uint8_t* s = o - d;
                if (d >= len) {
                    memcpy(o, s, len);
                } else {
                    for (size_t i = 0; i < len; i++) o[i] = s[i];  // (overlapping: byte by byte)
                }
                n += len;
            }
            if (br.overrun()) return false;
        } else {
            return false;
        }
        if (final) break;
    }
    out.resize(n);
    br.alignByte();
    if (used) *used = (size_t)(br.bytePos() - data);
    return true;
}

bool decompress(const uint8_t* data, size_t size, std::string& out, std::string& error) {
    out.clear();
    size_t at = 0;
    std::vector<uint8_t> buffer;
    bool any = false;
    while (at < size) {
        const uint8_t* m = data + at;
        size_t left = size - at;
        if (any && left < 18) break;  // (trailing zeros some tools add)
        if (!isGzip(m, left) || left < 18) {
            if (any) break;
            error = "not a gzip file";
            return false;
        }
        if (m[2] != 8) {
            error = "unknown gzip compression method";
            return false;
        }
        int flags = m[3];
        size_t p = 10;
        if (flags & 4) {  // FEXTRA
            if (p + 2 > left) goto cut;
            size_t xlen = m[p] | (m[p + 1] << 8);
            p += 2 + xlen;
        }
        if (flags & 8) {  // FNAME
            while (p < left && m[p]) p++;
            p++;
        }
        if (flags & 16) {  // FCOMMENT
            while (p < left && m[p]) p++;
            p++;
        }
        if (flags & 2) p += 2;  // FHCRC
        if (p >= left) goto cut;
        {
            size_t before = buffer.size(), used = 0;
            if (!inflateRaw(m + p, left - p, buffer, &used)) {
                error = "the compressed data is damaged";
                return false;
            }
            p += used;
            if (p + 8 > left) goto cut;
            uint32_t crc = m[p] | (m[p + 1] << 8) | (m[p + 2] << 16) | ((uint32_t)m[p + 3] << 24);
            uint32_t isize = m[p + 4] | (m[p + 5] << 8) | (m[p + 6] << 16) | ((uint32_t)m[p + 7] << 24);
            size_t got = buffer.size() - before;
            if (crc32(buffer.data() + before, got) != crc || (uint32_t)got != isize) {
                error = "the compressed data is damaged (wrong checksum)";
                return false;
            }
            at += p + 8;
            any = true;
        }
    }
    out.assign((const char*)buffer.data(), buffer.size());
    return true;
cut:
    error = "the file is cut short";
    return false;
}

bool readFile(const std::string& path, std::string& out, std::string& error) {
    std::string raw;
    if (!util::readFile(path, raw)) {
        error = "cannot read " + path;
        return false;
    }
    return decompress((const uint8_t*)raw.data(), raw.size(), out, error);
}

std::vector<uint8_t> compress(const uint8_t* data, size_t size, int level) {
    std::vector<uint8_t> z = zlibDeflate(data, size, level);  // 2-byte header, deflate, 4-byte adler32
    std::vector<uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};  // (no name, no time, Unix)
    if (z.size() >= 6) out.insert(out.end(), z.begin() + 2, z.end() - 4);
    uint32_t crc = crc32(data, size), n = (uint32_t)size;
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)(crc >> (8 * i)));
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)(n >> (8 * i)));
    return out;
}

}  // namespace gzip
