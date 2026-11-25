// Reading and writing sound files, and reading MIDI files (the stand-in for soundfile and mido).
// Everything here is Exbit's own code: WAV / RF64 / AIFF / AIFF-C reading, a complete FLAC
// decoder (RFC 9639) and a FLAC encoder, WAV writing, and a Standard MIDI File reader. MP3 and Ogg
// Vorbis are decoded by audio/mp3.cpp and audio/vorbis.cpp (codecs.h); the decoder is picked by the
// file's contents, never by its name.
#include "audio/sound_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

#include "audio/codecs.h"
#include "audio/md5.h"
#include "core/os.h"
#include "core/util.h"

namespace {

const char* kNotSound = "Not a sound file this program can read";

// --- the file's bytes (mapped, so a preview of a long file only touches what it decodes) ---
struct FileBytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
    os::MappedFile map;
    std::string copy;  // when it can't be mapped
    bool open(const std::string& path, std::string& error) {
        if (map.map(path)) {
            data = map.data;
            size = map.size;
            return true;
        }
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            error = "Cannot open " + path + ": " + strerror(errno);
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            error = "Cannot open " + path + ": not a file";
            return false;
        }
        // (empty, or it can't be mapped:) read it instead
        copy.clear();
        char buf[65536];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) copy.append(buf, (size_t)n);
        ::close(fd);
        if (n < 0) {
            error = "Cannot read " + path + ": " + strerror(errno);
            return false;
        }
        data = (const uint8_t*)copy.data();
        size = copy.size();
        return true;
    }
};

inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
inline bool tag(const uint8_t* p, const char* t) { return memcmp(p, t, 4) == 0; }

int64_t frameLimit(double maxSeconds, int sr) {
    if (maxSeconds < 0 || sr <= 0) return -1;
    return (int64_t)std::ceil(maxSeconds * sr);
}

// G.711 (A-law / µ-law WAV and AIFF-C): to 16-bit values
int ulaw(uint8_t u) {
    u = (uint8_t)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}
int alaw(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else { t += 0x108; t <<= seg - 1; }
    return (a & 0x80) ? t : -t;
}

enum class Enc { Int, UInt8, Float, Ulaw, Alaw };

// interleaved samples from raw sample data: `bytes` per sample, `stride` bytes per frame
void convertSamples(const uint8_t* src, size_t frames, int channels, int bytes, size_t stride, bool bigEndian,
                    Enc enc, float* dst) {
    const double intScale = 1.0 / 2147483648.0;
    for (size_t f = 0; f < frames; f++) {
        const uint8_t* fr = src + f * stride;
        for (int c = 0; c < channels; c++) {
            const uint8_t* p = fr + (size_t)c * bytes;
            float v = 0;
            switch (enc) {
                case Enc::Int:
                case Enc::UInt8: {
                    // the sample left-justified in 32 bits: every width scales alike
                    uint32_t u = 0;
                    for (int i = 0; i < bytes; i++) {
                        uint8_t b = bigEndian ? p[i] : p[bytes - 1 - i];
                        u |= (uint32_t)b << (24 - 8 * i);
                    }
                    if (enc == Enc::UInt8) u ^= 0x80000000u;
                    v = (float)((double)(int32_t)u * intScale);
                    break;
                }
                case Enc::Float:
                    if (bytes == 4) {
                        uint32_t u = bigEndian ? be32(p) : le32(p);
                        float x;
                        memcpy(&x, &u, 4);
                        v = x;
                    } else {
                        uint64_t u = bigEndian ? be64(p) : le64(p);
                        double x;
                        memcpy(&x, &u, 8);
                        v = (float)x;
                    }
                    break;
                case Enc::Ulaw: v = (float)(ulaw(*p) / 32768.0); break;
                case Enc::Alaw: v = (float)(alaw(*p) / 32768.0); break;
            }
            *dst++ = v;
        }
    }
}

// ============================== WAV / RF64 ==============================

const char* wavFormatName(int code) {
    switch (code) {
        case 0x0002: return " (Microsoft ADPCM)";
        case 0x0011: return " (IMA ADPCM)";
        case 0x0031: return " (GSM 6.10)";
        case 0x0050: return " (MPEG)";
        case 0x0092: return " (Dolby AC3)";
        case 0x2000: return " (AC3)";
        default: return "";
    }
}

bool decodeWav(const uint8_t* d, size_t n, double maxSeconds, SoundData& out, std::string& err) {
    bool rf64 = tag(d, "RF64") || tag(d, "BW64");
    uint64_t ds64Data = 0;
    bool haveDs64 = false, haveFmt = false, haveData = false;
    int code = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t blockAlign = 0, dataOff = 0;
    uint64_t dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint8_t* c = d + pos;
        uint64_t size = le32(c + 4);
        size_t body = pos + 8;
        if (tag(c, "ds64") && size >= 24 && body + 24 <= n) {
            ds64Data = le64(d + body + 8);
            haveDs64 = true;
        } else if (tag(c, "fmt ") && size >= 14 && body + 14 <= n) {
            const uint8_t* f = d + body;
            code = le16(f);
            channels = le16(f + 2);
            rate = le32(f + 4);
            blockAlign = le16(f + 12);
            bits = size >= 16 && body + 16 <= n ? le16(f + 14) : 8;
            if (code == 0xFFFE && size >= 40 && body + 40 <= n) code = le16(f + 24);  // the sub-format GUID
            haveFmt = true;
        } else if (tag(c, "data")) {
            if (rf64 && size == 0xFFFFFFFFu && haveDs64) size = ds64Data;
            if (size == 0xFFFFFFFFu || size > n - body) size = n - body;  // streamed or cut short
            dataOff = body;
            dataLen = size;
            haveData = true;
        }
        if (size > n - body) break;
        pos = body + (size_t)size + (size & 1);  // chunks are padded to even sizes
    }
    if (!haveFmt) { err = "Unsupported WAV format: it has no format chunk"; return false; }
    if (channels <= 0 || rate == 0) { err = "Unsupported WAV format: no channels or no sample rate"; return false; }
    if (!haveData) { err = "Unsupported WAV format: it has no audio data"; return false; }
    if (code == 0x0055) {  // MP3 inside a WAV
        SoundData mp3;
        int64_t limit = maxSeconds < 0 ? -1 : frameLimit(maxSeconds, 48000);
        if (!decodeMp3(d + dataOff, (size_t)dataLen, mp3, err, limit)) return false;
        out = std::move(mp3);
        return true;
    }
    Enc enc;
    int bytes = (bits + 7) / 8;
    if (code == 1) {
        if (bits < 1 || bits > 32) { err = "Unsupported WAV format: " + std::to_string(bits) + "-bit PCM"; return false; }
        enc = bytes == 1 ? Enc::UInt8 : Enc::Int;
    } else if (code == 3) {
        if (bits != 32 && bits != 64) { err = "Unsupported WAV format: " + std::to_string(bits) + "-bit float"; return false; }
        enc = Enc::Float;
    } else if (code == 6 || code == 7) {
        enc = code == 6 ? Enc::Alaw : Enc::Ulaw;
        bytes = 1;
    } else {
        char buf[64];
        snprintf(buf, sizeof buf, "compression type 0x%04X", code);
        err = std::string("Unsupported WAV format: ") + buf + wavFormatName(code);
        return false;
    }
    size_t stride = std::max(blockAlign, (size_t)bytes * channels);
    int64_t frames = (int64_t)(dataLen / stride);
    int64_t limit = frameLimit(maxSeconds, (int)rate);
    if (limit >= 0) frames = std::min(frames, limit);
    out.sampleRate = (int)rate;
    out.channels = channels;
    out.samples.assign((size_t)frames * channels, 0.0f);
    convertSamples(d + dataOff, (size_t)frames, channels, bytes, stride, false, enc, out.samples.data());
    return true;
}

// ============================== AIFF / AIFF-C ==============================

double extended80(const uint8_t* p) {  // IEEE 754 80-bit extended (the sample rate)
    int exp = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mant = be64(p + 2);
    if (exp == 0 && mant == 0) return 0;
    double v = std::ldexp((double)mant, exp - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

bool decodeAiff(const uint8_t* d, size_t n, double maxSeconds, SoundData& out, std::string& err) {
    bool aifc = tag(d + 8, "AIFC");
    bool haveComm = false, haveData = false;
    int channels = 0, bits = 0;
    uint32_t frames = 0;
    double rate = 0;
    char comp[5] = "NONE";
    size_t dataOff = 0, dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint8_t* c = d + pos;
        size_t size = be32(c + 4);
        size_t body = pos + 8;
        size_t avail = n - body;
        if (tag(c, "COMM") && size >= 18 && avail >= 18) {
            const uint8_t* b = d + body;
            channels = be16(b);
            frames = be32(b + 2);
            bits = be16(b + 6);
            rate = extended80(b + 8);
            if (aifc && size >= 22 && avail >= 22) memcpy(comp, b + 18, 4);
            haveComm = true;
        } else if (tag(c, "SSND") && size >= 8 && avail >= 8) {
            size_t offset = be32(d + body);
            size_t len = std::min(size, avail);
            if (8 + offset <= len) {
                dataOff = body + 8 + offset;
                dataLen = len - 8 - offset;
                haveData = true;
            }
        }
        if (size > avail) break;
        pos = body + size + (size & 1);
    }
    if (!haveComm) { err = "Unsupported AIFF format: it has no COMM chunk"; return false; }
    if (channels <= 0 || rate <= 0) { err = "Unsupported AIFF format: no channels or no sample rate"; return false; }
    std::string c(comp, 4);
    Enc enc = Enc::Int;
    bool bigEndian = true;
    int bytes = (bits + 7) / 8;
    if (c == "NONE" || c == "twos" || c == "sowt") {
        if (bits < 1 || bits > 32) { err = "Unsupported AIFF format: " + std::to_string(bits) + "-bit samples"; return false; }
        bigEndian = c != "sowt";
    } else if (c == "in24" || c == "in32" || c == "42ni" || c == "42n1" || c == "23ni" || c == "23n1") {
        bytes = (c == "in24" || c[0] == '4') ? 3 : 4;  // 42n1 / 23ni: little-endian
        bigEndian = c[0] == 'i';
    } else if (c == "raw ") {
        bytes = 1;
        enc = Enc::UInt8;
    } else if (c == "fl32" || c == "FL32") {
        bytes = 4;
        enc = Enc::Float;
    } else if (c == "fl64" || c == "FL64") {
        bytes = 8;
        enc = Enc::Float;
    } else if (c == "ulaw" || c == "ULAW") {
        bytes = 1;
        enc = Enc::Ulaw;
    } else if (c == "alaw" || c == "ALAW") {
        bytes = 1;
        enc = Enc::Alaw;
    } else {
        err = "Unsupported AIFF format: compression '" + c + "'";
        return false;
    }
    int sr = (int)std::lround(rate);
    size_t stride = (size_t)bytes * channels;
    int64_t count = haveData ? (int64_t)std::min<uint64_t>(frames, dataLen / stride) : 0;
    int64_t limit = frameLimit(maxSeconds, sr);
    if (limit >= 0) count = std::min(count, limit);
    out.sampleRate = sr;
    out.channels = channels;
    out.samples.assign((size_t)count * channels, 0.0f);
    if (count) convertSamples(d + dataOff, (size_t)count, channels, bytes, stride, bigEndian, enc, out.samples.data());
    return true;
}

// ============================== FLAC (RFC 9639) ==============================

uint8_t kCrc8[256];
uint16_t kCrc16[256];
struct CrcTables {
    CrcTables() {
        for (int i = 0; i < 256; i++) {
            uint8_t c8 = (uint8_t)i;
            for (int b = 0; b < 8; b++) c8 = (uint8_t)((c8 & 0x80) ? (c8 << 1) ^ 0x07 : c8 << 1);
            kCrc8[i] = c8;
            uint16_t c16 = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++) c16 = (uint16_t)((c16 & 0x8000) ? (c16 << 1) ^ 0x8005 : c16 << 1);
            kCrc16[i] = c16;
        }
    }
} crcTables;

uint8_t crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) c = kCrc8[c ^ p[i]];
    return c;
}
uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ kCrc16[(c >> 8) ^ p[i]]);
    return c;
}

// reads bits most significant first
struct BitReader {
    const uint8_t* d;
    size_t n;
    uint64_t pos = 0;  // in bits
    uint64_t total;
    BitReader(const uint8_t* data, size_t size) : d(data), n(size), total((uint64_t)size * 8) {}
    bool over() const { return pos > total; }
    uint64_t peek64() const {
        size_t b = (size_t)(pos >> 3);
        uint64_t v = 0;
        if (b + 8 <= n) {
            memcpy(&v, d + b, 8);
            v = __builtin_bswap64(v);
        } else {
            for (int i = 0; i < 8; i++) v = (v << 8) | (b + i < n ? d[b + i] : 0);
        }
        return v;
    }
    uint32_t get(int k) {  // 0..32 bits
        if (k == 0) return 0;
        uint64_t v = peek64() << (pos & 7);
        pos += k;
        return (uint32_t)(v >> (64 - k));
    }
    uint64_t get64(int k) {
        if (k <= 32) return get(k);
        uint64_t hi = get(k - 32);
        return (hi << 32) | get(32);
    }
    int64_t sget(int k) {  // two's complement, k bits (up to 64)
        if (k == 0) return 0;
        uint64_t v = get64(k);
        return (int64_t)(v << (64 - k)) >> (64 - k);
    }
    uint64_t unary() {  // zeros before the next one
        uint64_t count = 0;
        for (;;) {
            if (pos >= total) {
                pos = total + 1;
                return count;
            }
            int off = (int)(pos & 7);
            uint64_t v = peek64() << off;
            int valid = 64 - off;
            if (v) {
                int z = __builtin_clzll(v);
                if (z < valid) {
                    pos += z + 1;
                    return count + z;
                }
            }
            count += valid;
            pos += valid;
        }
    }
    void align() { pos = (pos + 7) & ~(uint64_t)7; }
};

struct StreamInfo {
    int minBlock = 0, maxBlock = 0;
    int sampleRate = 0, channels = 0, bps = 0;
    uint64_t totalSamples = 0;
};

bool readResidual(BitReader& b, int blocksize, int order, int64_t* out) {
    int method = (int)b.get(2);
    if (method > 1) return false;
    int pbits = method ? 5 : 4;
    uint32_t escape = method ? 31 : 15;
    int porder = (int)b.get(4);
    int parts = 1 << porder;
    if (blocksize % parts) return false;
    int psize = blocksize >> porder;
    if (psize < order) return false;
    int i = order;
    for (int p = 0; p < parts; p++) {
        int count = psize - (p == 0 ? order : 0);
        uint32_t k = b.get(pbits);
        if (k == escape) {
            int nb = (int)b.get(5);
            for (int j = 0; j < count; j++) out[i++] = nb ? b.sget(nb) : 0;
        } else {
            for (int j = 0; j < count; j++) {
                uint64_t q = b.unary();
                uint64_t u = (q << k) | b.get((int)k);
                out[i++] = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
            }
        }
        if (b.over()) return false;
    }
    return true;
}

bool readSubframe(BitReader& b, int bps, int blocksize, int64_t* out) {
    if (b.get(1)) return false;
    int type = (int)b.get(6);
    int wasted = 0;
    if (b.get(1)) wasted = (int)b.unary() + 1;
    if (wasted >= bps) return false;
    bps -= wasted;
    if (type == 0) {  // CONSTANT
        int64_t v = b.sget(bps);
        std::fill(out, out + blocksize, v);
    } else if (type == 1) {  // VERBATIM
        for (int i = 0; i < blocksize; i++) out[i] = b.sget(bps);
    } else if (type >= 8 && type <= 12) {  // FIXED
        int order = type - 8;
        if (order > blocksize) return false;
        for (int i = 0; i < order; i++) out[i] = b.sget(bps);
        if (!readResidual(b, blocksize, order, out)) return false;
        // (wrapping arithmetic: a damaged frame can't overflow; its CRC rejects it afterwards)
        uint64_t* u = (uint64_t*)out;
        switch (order) {
            case 1: for (int i = 1; i < blocksize; i++) u[i] += u[i - 1]; break;
            case 2: for (int i = 2; i < blocksize; i++) u[i] += 2 * u[i - 1] - u[i - 2]; break;
            case 3: for (int i = 3; i < blocksize; i++) u[i] += 3 * u[i - 1] - 3 * u[i - 2] + u[i - 3]; break;
            case 4:
                for (int i = 4; i < blocksize; i++) u[i] += 4 * u[i - 1] - 6 * u[i - 2] + 4 * u[i - 3] - u[i - 4];
                break;
            default: break;
        }
    } else if (type >= 32) {  // LPC
        int order = type - 31;
        if (order > blocksize) return false;
        for (int i = 0; i < order; i++) out[i] = b.sget(bps);
        int precision = (int)b.get(4);
        if (precision == 15) return false;
        precision += 1;
        int shift = (int)b.sget(5);
        int64_t coef[32];
        for (int i = 0; i < order; i++) coef[i] = b.sget(precision);
        if (!readResidual(b, blocksize, order, out)) return false;
        for (int i = order; i < blocksize; i++) {
            uint64_t sum = 0;  // (wrapping, as above)
            for (int j = 0; j < order; j++) sum += (uint64_t)coef[j] * (uint64_t)out[i - 1 - j];
            uint64_t pred = shift >= 0 ? (uint64_t)((int64_t)sum >> shift) : sum << -shift;
            out[i] = (int64_t)((uint64_t)out[i] + pred);
        }
    } else {
        return false;  // reserved
    }
    if (wasted)
        for (int i = 0; i < blocksize; i++) out[i] = (int64_t)((uint64_t)out[i] << wasted);
    return !b.over();
}

// one frame at d[0..]: its size in bytes, or 0 if this isn't a (good) frame
size_t readFlacFrame(const uint8_t* d, size_t n, const StreamInfo& si, std::vector<std::vector<int64_t>>& chans,
                     int& blocksize) {
    BitReader b(d, n);
    if (b.get(14) != 0x3FFE || b.get(1)) return 0;
    b.get(1);  // blocking strategy (fixed / variable): frames are read in order either way
    int bsCode = (int)b.get(4), srCode = (int)b.get(4), chCode = (int)b.get(4), ssCode = (int)b.get(3);
    if (b.get(1)) return 0;
    uint32_t first = b.get(8);  // the coded frame / sample number (UTF-8-like)
    int extra;
    if (!(first & 0x80)) extra = 0;
    else if ((first & 0xE0) == 0xC0) extra = 1;
    else if ((first & 0xF0) == 0xE0) extra = 2;
    else if ((first & 0xF8) == 0xF0) extra = 3;
    else if ((first & 0xFC) == 0xF8) extra = 4;
    else if ((first & 0xFE) == 0xFC) extra = 5;
    else if (first == 0xFE) extra = 6;
    else return 0;
    for (int i = 0; i < extra; i++)
        if ((b.get(8) & 0xC0) != 0x80) return 0;
    if (bsCode == 0) return 0;
    else if (bsCode == 1) blocksize = 192;
    else if (bsCode <= 5) blocksize = 576 << (bsCode - 2);
    else if (bsCode == 6) blocksize = (int)b.get(8) + 1;
    else if (bsCode == 7) blocksize = (int)b.get(16) + 1;
    else blocksize = 256 << (bsCode - 8);
    if (srCode == 12) b.get(8);
    else if (srCode == 13 || srCode == 14) b.get(16);
    else if (srCode == 15) return 0;
    int channels;
    if (chCode < 8) channels = chCode + 1;
    else if (chCode <= 10) channels = 2;
    else return 0;
    if (si.channels && channels != si.channels) return 0;
    static const int kSizes[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    int bps = ssCode == 0 ? si.bps : kSizes[ssCode];
    if (bps == 0 || (si.bps && bps != si.bps)) return 0;
    size_t headerLen = (size_t)(b.pos >> 3);
    if (headerLen + 1 > n) return 0;
    if (b.get(8) != crc8(d, headerLen)) return 0;
    if ((int)chans.size() < channels) chans.resize(channels);
    for (int c = 0; c < channels; c++) {
        if ((int)chans[c].size() < blocksize) chans[c].resize(blocksize);
        bool side = (chCode == 8 && c == 1) || (chCode == 9 && c == 0) || (chCode == 10 && c == 1);
        if (!readSubframe(b, bps + (side ? 1 : 0), blocksize, chans[c].data())) return 0;
    }
    b.align();
    size_t bodyLen = (size_t)(b.pos >> 3);
    if (bodyLen + 2 > n) return 0;
    if (b.get(16) != crc16(d, bodyLen)) return 0;
    if (chCode >= 8) {
        int64_t* a = chans[0].data();
        int64_t* s = chans[1].data();
        for (int i = 0; i < blocksize; i++) {
            uint64_t x = (uint64_t)a[i], y = (uint64_t)s[i];
            if (chCode == 8) {          // left / side
                s[i] = (int64_t)(x - y);
            } else if (chCode == 9) {   // side / right
                a[i] = (int64_t)(x + y);
            } else {                    // mid / side
                uint64_t mid = (x << 1) | (y & 1);
                a[i] = (int64_t)(mid + y) >> 1;
                s[i] = (int64_t)(mid - y) >> 1;
            }
        }
    }
    return bodyLen + 2;
}

size_t id3Size(const uint8_t* d, size_t n) {  // an ID3v2 tag at the start: its size, else 0
    if (n < 10 || memcmp(d, "ID3", 3) != 0) return 0;
    if ((d[6] | d[7] | d[8] | d[9]) & 0x80) return 0;
    size_t size = ((size_t)d[6] << 21) | ((size_t)d[7] << 14) | ((size_t)d[8] << 7) | d[9];
    size += 10 + ((d[5] & 0x10) ? 10 : 0);
    return std::min(size, n);
}

bool decodeFlac(const uint8_t* d, size_t n, double maxSeconds, SoundData& out, std::string& err) {
    size_t pos = id3Size(d, n);
    if (pos + 4 > n || !tag(d + pos, "fLaC")) { err = kNotSound; return false; }
    pos += 4;
    StreamInfo si;
    bool haveInfo = false;
    for (;;) {
        if (pos + 4 > n) { err = "Damaged FLAC file: its header is cut short"; return false; }
        bool last = d[pos] & 0x80;
        int type = d[pos] & 0x7F;
        size_t len = ((size_t)d[pos + 1] << 16) | ((size_t)d[pos + 2] << 8) | d[pos + 3];
        size_t body = pos + 4;
        if (len > n - body) { err = "Damaged FLAC file: its header is cut short"; return false; }
        if (type == 0 && len >= 34) {
            BitReader b(d + body, len);
            si.minBlock = (int)b.get(16);
            si.maxBlock = (int)b.get(16);
            b.get(24);
            b.get(24);
            si.sampleRate = (int)b.get(20);
            si.channels = (int)b.get(3) + 1;
            si.bps = (int)b.get(5) + 1;
            si.totalSamples = b.get64(36);
            haveInfo = true;
        }
        pos = body + len;
        if (last) break;
    }
    if (!haveInfo || si.sampleRate == 0) { err = "Damaged FLAC file: it has no stream information"; return false; }
    if (si.bps < 4 || si.bps > 32) { err = "Unsupported FLAC format: " + std::to_string(si.bps) + "-bit samples"; return false; }
    int64_t limit = frameLimit(maxSeconds, si.sampleRate);
    if (si.totalSamples && (limit < 0 || (uint64_t)limit > si.totalSamples)) limit = (int64_t)si.totalSamples;
    out.sampleRate = si.sampleRate;
    out.channels = si.channels;
    out.samples.clear();
    if (limit >= 0) out.samples.reserve(std::min((size_t)limit * si.channels, n * 8));  // (a bogus total can't eat memory)
    const double scale = 1.0 / (double)(1ull << (si.bps - 1));
    std::vector<std::vector<int64_t>> chans(si.channels);
    int64_t done = 0;
    while (pos + 2 <= n && (limit < 0 || done < limit)) {
        if (d[pos] == 0xFF && (d[pos + 1] & 0xFE) == 0xF8) {
            int blocksize = 0;
            size_t len = readFlacFrame(d + pos, n - pos, si, chans, blocksize);
            if (len) {
                int64_t take = blocksize;
                if (limit >= 0) take = std::min<int64_t>(take, limit - done);
                size_t at = out.samples.size();
                out.samples.resize(at + (size_t)take * si.channels);
                float* o = out.samples.data() + at;
                for (int64_t i = 0; i < take; i++)
                    for (int c = 0; c < si.channels; c++) *o++ = (float)((double)chans[c][i] * scale);
                done += take;
                pos += len;
                continue;
            }
        }
        pos++;  // not a frame here: look for the next one
    }
    return true;
}

// ============================== Ogg (for previews: its sample rate) ==============================

int vorbisRate(const uint8_t* d, size_t n) {
    if (n < 28 || !tag(d, "OggS")) return 0;
    size_t segs = d[26];
    size_t p = 27 + segs;
    if (p + 16 > n || d[p] != 1 || memcmp(d + p + 1, "vorbis", 6) != 0) return 0;
    return (int)le32(d + p + 12);
}

bool decodeBytes(const uint8_t* d, size_t n, double maxSeconds, SoundData& out, std::string& err) {
    out = SoundData();
    if (n >= 12 && (tag(d, "RIFF") || tag(d, "RF64") || tag(d, "BW64")) && tag(d + 8, "WAVE"))
        return decodeWav(d, n, maxSeconds, out, err);
    if (n >= 12 && tag(d, "FORM") && (tag(d + 8, "AIFF") || tag(d + 8, "AIFC")))
        return decodeAiff(d, n, maxSeconds, out, err);
    size_t skip = id3Size(d, n);
    if (n >= skip + 4 && tag(d + skip, "fLaC")) return decodeFlac(d, n, maxSeconds, out, err);
    if (n >= 4 && tag(d, "OggS")) {
        if (!looksLikeVorbis(d, n)) {
            err = "Unsupported Ogg format: only Ogg Vorbis can be read";
            return false;
        }
        int sr = vorbisRate(d, n);
        int64_t limit = maxSeconds < 0 ? -1 : frameLimit(maxSeconds, sr > 0 ? sr : 192000);
        if (!decodeVorbis(d, n, out, err, limit)) return false;
    } else if (n >= 4 && (skip > 0 || looksLikeMp3(d, n))) {
        if (!looksLikeMp3(d, n)) { err = kNotSound; return false; }
        int64_t limit = maxSeconds < 0 ? -1 : frameLimit(maxSeconds, 48000);  // (MP3 rates are at most 48 kHz)
        if (!decodeMp3(d, n, out, err, limit)) return false;
    } else {
        err = kNotSound;
        return false;
    }
    if (maxSeconds >= 0 && out.channels > 0) {  // only as much as asked for
        int64_t want = frameLimit(maxSeconds, out.sampleRate);
        if (want >= 0 && (size_t)want < out.frames()) out.samples.resize((size_t)want * out.channels);
    }
    return true;
}

bool readFile(const std::string& path, double maxSeconds, SoundData& out, std::string& error) {
    FileBytes file;
    if (!file.open(path, error)) return false;
    if (!decodeBytes(file.data, file.size, maxSeconds, out, error)) return false;
    if (out.channels <= 0 || out.sampleRate <= 0) {
        error = kNotSound;
        return false;
    }
    return true;
}

// ============================== writing ==============================

struct Writer {
    FILE* f = nullptr;
    std::string path;
    bool ok = true;
    bool open(const std::string& p, std::string& err) {
        path = p;
        f = fopen(p.c_str(), "wb");
        if (!f) {
            err = "Cannot write " + p + ": " + strerror(errno);
            return false;
        }
        return true;
    }
    void write(const void* data, size_t n) {
        if (ok && n && fwrite(data, 1, n, f) != n) ok = false;
    }
    void write(const std::string& s) { write(s.data(), s.size()); }
    bool close(std::string& err) {
        int e = errno;
        if (fclose(f) != 0) ok = false;
        f = nullptr;
        if (!ok) {
            err = "Cannot write " + path + ": " + strerror(e ? e : EIO);
            ::remove(path.c_str());
        }
        return ok;
    }
};

void putLe(std::string& s, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) s += (char)(uint8_t)(v >> (8 * i));
}

int64_t quantize(float x, int bps) {  // to a bps-bit integer, clipped
    double full = (double)(1ll << (bps - 1));
    double v = (double)x * full;
    if (!(v == v)) return 0;  // NaN
    v = std::nearbyint(v);
    if (v > full - 1) return (int64_t)full - 1;
    if (v < -full) return -(int64_t)full;
    return (int64_t)v;
}

// ---- the FLAC encoder ----

struct BitWriter {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int nacc = 0;
    void put(uint64_t v, int k) {  // 0..32 bits
        if (k == 0) return;
        acc = (acc << k) | (v & ((1ull << k) - 1));
        nacc += k;
        while (nacc >= 8) {
            nacc -= 8;
            buf.push_back((uint8_t)(acc >> nacc));
        }
        acc &= (1ull << nacc) - 1;
    }
    void put64(uint64_t v, int k) {
        if (k > 32) {
            put(v >> 32, k - 32);
            put(v & 0xFFFFFFFFull, 32);
        } else {
            put(v, k);
        }
    }
    void putSigned(int64_t v, int k) { put64((uint64_t)v & (k >= 64 ? ~0ull : (1ull << k) - 1), k); }
    void unary(uint64_t q) {
        while (q >= 32) {
            put(0, 32);
            q -= 32;
        }
        put(1, (int)q + 1);
    }
    void align() {
        if (nacc) put(0, 8 - nacc);
    }
};

struct SubPlan {
    enum { Constant, Verbatim, Fixed } kind = Verbatim;
    int order = 0;
    int wasted = 0;
    uint64_t bits = 0;
    int porder = 0;
    int method = 0;
    std::vector<int> params;
};

inline uint64_t zigzag(int64_t r) { return ((uint64_t)r << 1) ^ (uint64_t)(r >> 63); }

void fixedResidual(const int64_t* y, int n, int order, int64_t* r) {
    for (int i = order; i < n; i++) {
        int64_t p;
        switch (order) {
            case 0: p = 0; break;
            case 1: p = y[i - 1]; break;
            case 2: p = 2 * y[i - 1] - y[i - 2]; break;
            case 3: p = 3 * y[i - 1] - 3 * y[i - 2] + y[i - 3]; break;
            default: p = 4 * y[i - 1] - 6 * y[i - 2] + 4 * y[i - 3] - y[i - 4]; break;
        }
        r[i] = y[i] - p;
    }
}

// the best Rice partitioning of residual r[order..n): its size in bits (estimated from the
// partition sums, an upper bound within a bit or so per sample), the partition order and parameters
uint64_t planRice(const int64_t* r, int n, int order, std::vector<uint64_t>& prefix, int& bestP, int& method,
                  std::vector<int>& params) {
    prefix.assign((size_t)n + 1, 0);
    for (int i = order; i < n; i++) prefix[i + 1] = prefix[i] + zigzag(r[i]);
    uint64_t best = std::numeric_limits<uint64_t>::max();
    std::vector<int> ks;
    for (int p = 0; p <= 8; p++) {
        int parts = 1 << p;
        if (n % parts) break;
        int psize = n >> p;
        if (p > 0 && psize <= order) break;
        uint64_t bits = 2 + 4;
        int maxK = 0;
        ks.assign(parts, 0);
        for (int q = 0; q < parts; q++) {
            int start = q == 0 ? order : q * psize, end = (q + 1) * psize;
            uint64_t cnt = (uint64_t)(end - start);
            uint64_t s = prefix[end] - prefix[start];
            int bestK = 0;
            uint64_t bestBits = std::numeric_limits<uint64_t>::max();
            if (cnt) {
                uint64_t mean = s / cnt;
                int k0 = mean ? 63 - __builtin_clzll(mean) : 0;
                for (int k = std::max(0, k0 - 1); k <= std::min(30, k0 + 1); k++) {
                    uint64_t est = cnt * (uint64_t)(k + 1) + (s >> k) - (k ? cnt / 2 : 0);
                    if (est < bestBits) {
                        bestBits = est;
                        bestK = k;
                    }
                }
            } else {
                bestBits = 0;
            }
            ks[q] = bestK;
            maxK = std::max(maxK, bestK);
            bits += bestBits;
        }
        bits += (uint64_t)parts * (maxK > 14 ? 5 : 4);
        if (bits < best) {
            best = bits;
            bestP = p;
            method = maxK > 14 ? 1 : 0;
            params = ks;
        }
    }
    return best;
}

SubPlan planSubframe(const int64_t* x, int n, int bps, std::vector<int64_t>& y, std::vector<int64_t>& r,
                     std::vector<uint64_t>& prefix) {
    SubPlan plan;
    bool same = true;
    uint64_t any = 0;
    for (int i = 0; i < n; i++) {
        if (x[i] != x[0]) same = false;
        any |= (uint64_t)x[i];
    }
    if (same) {
        plan.kind = SubPlan::Constant;
        plan.bits = 8 + bps;
        return plan;
    }
    int wasted = any ? std::min(__builtin_ctzll(any), bps - 1) : 0;
    int ebps = bps - wasted;
    y.resize(n);
    for (int i = 0; i < n; i++) y[i] = x[i] >> wasted;
    plan.wasted = wasted;
    plan.kind = SubPlan::Verbatim;
    plan.bits = 8 + wasted + (uint64_t)n * ebps;
    r.resize(n);
    for (int order = 0; order <= std::min(4, n); order++) {
        fixedResidual(y.data(), n, order, r.data());
        int p = 0, method = 0;
        std::vector<int> params;
        uint64_t rice = planRice(r.data(), n, order, prefix, p, method, params);
        if (rice == std::numeric_limits<uint64_t>::max()) continue;
        uint64_t bits = 8 + wasted + (uint64_t)order * ebps + rice;
        if (bits < plan.bits) {
            plan.kind = SubPlan::Fixed;
            plan.bits = bits;
            plan.order = order;
            plan.porder = p;
            plan.method = method;
            plan.params = std::move(params);
        }
    }
    return plan;
}

void writeSubframe(BitWriter& w, const int64_t* x, int n, int bps, const SubPlan& plan, std::vector<int64_t>& y,
                   std::vector<int64_t>& r) {
    w.put(0, 1);
    if (plan.kind == SubPlan::Constant) {
        w.put(0, 6);
        w.put(0, 1);
        w.putSigned(x[0], bps);
        return;
    }
    w.put(plan.kind == SubPlan::Verbatim ? 1 : 8 + plan.order, 6);
    if (plan.wasted) {
        w.put(1, 1);
        w.unary(plan.wasted - 1);
    } else {
        w.put(0, 1);
    }
    int ebps = bps - plan.wasted;
    y.resize(n);
    for (int i = 0; i < n; i++) y[i] = x[i] >> plan.wasted;
    if (plan.kind == SubPlan::Verbatim) {
        for (int i = 0; i < n; i++) w.putSigned(y[i], ebps);
        return;
    }
    for (int i = 0; i < plan.order; i++) w.putSigned(y[i], ebps);
    r.resize(n);
    fixedResidual(y.data(), n, plan.order, r.data());
    w.put(plan.method, 2);
    w.put(plan.porder, 4);
    int parts = 1 << plan.porder, psize = n >> plan.porder;
    for (int q = 0; q < parts; q++) {
        int k = plan.params[q];
        w.put(k, plan.method ? 5 : 4);
        int start = q == 0 ? plan.order : q * psize, end = (q + 1) * psize;
        for (int i = start; i < end; i++) {
            uint64_t u = zigzag(r[i]);
            w.unary(u >> k);
            w.put(u & ((1ull << k) - 1), k);
        }
    }
}

void putUtf8(BitWriter& w, uint64_t v) {
    if (v < 0x80) {
        w.put(v, 8);
        return;
    }
    int bytes = 2;
    while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) bytes++;
    uint32_t first = ((0xFF00u >> bytes) & 0xFF) | (uint32_t)(v >> (6 * (bytes - 1)));
    w.put(first & 0xFF, 8);
    for (int i = bytes - 2; i >= 0; i--) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
}

int sampleRateCode(int sr, int& extraBits, int& extraValue) {
    static const int kRates[] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    extraBits = 0;
    for (int i = 1; i < 12; i++)
        if (kRates[i] == sr) return i;
    if (sr % 1000 == 0 && sr / 1000 <= 255) { extraBits = 8; extraValue = sr / 1000; return 12; }
    if (sr <= 65535) { extraBits = 16; extraValue = sr; return 13; }
    if (sr % 10 == 0 && sr / 10 <= 65535) { extraBits = 16; extraValue = sr / 10; return 14; }
    return 0;  // from STREAMINFO
}

int blockSizeCode(int n, int& extraBits) {
    extraBits = 0;
    if (n == 192) return 1;
    for (int c = 2; c <= 5; c++)
        if (n == 576 << (c - 2)) return c;
    for (int c = 8; c <= 15; c++)
        if (n == 256 << (c - 8)) return c;
    if (n <= 256) { extraBits = 8; return 6; }
    extraBits = 16;
    return 7;
}

void encodeFlacFrame(BitWriter& w, uint64_t frameNumber, const std::vector<std::vector<int64_t>>& chans, int n,
                     int bps, int sr, std::vector<int64_t>& y, std::vector<int64_t>& r, std::vector<uint64_t>& prefix) {
    int channels = (int)chans.size();
    // the channel layout: independent, or (stereo) left/side, side/right or mid/side, whichever is smallest
    int chCode = channels - 1;
    std::vector<const int64_t*> sig;
    std::vector<int> sigBps;
    std::vector<SubPlan> plans;
    std::vector<int64_t> mid, side;
    if (channels == 2) {
        const int64_t* L = chans[0].data();
        const int64_t* R = chans[1].data();
        mid.resize(n);
        side.resize(n);
        for (int i = 0; i < n; i++) {
            mid[i] = (L[i] + R[i]) >> 1;
            side[i] = L[i] - R[i];
        }
        SubPlan pl = planSubframe(L, n, bps, y, r, prefix);
        SubPlan pr = planSubframe(R, n, bps, y, r, prefix);
        SubPlan pm = planSubframe(mid.data(), n, bps, y, r, prefix);
        SubPlan ps = planSubframe(side.data(), n, bps + 1, y, r, prefix);
        uint64_t costs[4] = {pl.bits + pr.bits, pl.bits + ps.bits, ps.bits + pr.bits, pm.bits + ps.bits};
        int best = (int)(std::min_element(costs, costs + 4) - costs);
        switch (best) {
            case 0: sig = {L, R}; sigBps = {bps, bps}; plans = {pl, pr}; chCode = 1; break;
            case 1: sig = {L, side.data()}; sigBps = {bps, bps + 1}; plans = {pl, ps}; chCode = 8; break;
            case 2: sig = {side.data(), R}; sigBps = {bps + 1, bps}; plans = {ps, pr}; chCode = 9; break;
            default: sig = {mid.data(), side.data()}; sigBps = {bps, bps + 1}; plans = {pm, ps}; chCode = 10; break;
        }
    } else {
        for (int c = 0; c < channels; c++) {
            sig.push_back(chans[c].data());
            sigBps.push_back(bps);
            plans.push_back(planSubframe(chans[c].data(), n, bps, y, r, prefix));
        }
    }
    size_t start = w.buf.size();
    int bsExtra, srExtraBits, srExtra = 0;
    int bsCode = blockSizeCode(n, bsExtra);
    int srCode = sampleRateCode(sr, srExtraBits, srExtra);
    int ssCode = bps == 8 ? 1 : bps == 12 ? 2 : bps == 16 ? 4 : bps == 20 ? 5 : bps == 24 ? 6 : bps == 32 ? 7 : 0;
    w.put(0x3FFE, 14);
    w.put(0, 1);  // reserved
    w.put(0, 1);  // fixed block size
    w.put(bsCode, 4);
    w.put(srCode, 4);
    w.put(chCode, 4);
    w.put(ssCode, 3);
    w.put(0, 1);
    putUtf8(w, frameNumber);
    if (bsExtra) w.put(n - 1, bsExtra);
    if (srExtraBits) w.put(srExtra, srExtraBits);
    w.put(crc8(w.buf.data() + start, w.buf.size() - start), 8);
    for (size_t c = 0; c < sig.size(); c++) writeSubframe(w, sig[c], n, sigBps[c], plans[c], y, r);
    w.align();
    w.put(crc16(w.buf.data() + start, w.buf.size() - start), 16);
}

// ============================== MIDI files ==============================

uint32_t readVarLen(const uint8_t* d, size_t end, size_t& p, bool& ok) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (p >= end) { ok = false; return v; }
        uint8_t b = d[p++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    return v;
}

struct MidiEvent {
    int64_t tick;
    uint8_t status;  // 0x80 / 0x90 with channel; 0xFF = set tempo; 0 = anything else
    uint8_t a, b;
    uint32_t tempo;
};

}  // namespace

// ============================== the API ==============================

bool readSoundFile(const std::string& path, SoundData& out, std::string& error) {
    return readFile(path, -1.0, out, error);
}

bool readSoundFileStart(const std::string& path, double maxSeconds, SoundData& out, std::string& error) {
    return readFile(path, std::max(0.0, maxSeconds), out, error);
}

bool writeWav(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
              SampleFormat format, std::string& error) {
    if (channels <= 0 || channels > 65535 || sampleRate <= 0) {
        error = "Cannot write " + path + ": no channels or no sample rate";
        return false;
    }
    int bytes = format == SampleFormat::Pcm16 ? 2 : format == SampleFormat::Pcm24 ? 3 : 4;
    bool isFloat = format == SampleFormat::Float32;
    uint64_t blockAlign = (uint64_t)bytes * channels;
    uint64_t dataBytes = blockAlign * frames;
    uint64_t pad = dataBytes & 1;
    size_t fmtSize = isFloat ? 18 : 16;
    uint64_t headerAfterRiff = 4 + (8 + fmtSize) + (isFloat ? 12 : 0) + 8;  // "WAVE", fmt, fact, data header
    bool rf64 = dataBytes + pad + headerAfterRiff + 36 > 0xFFFFFFFFull;
    std::string h;
    h += rf64 ? "RF64" : "RIFF";
    uint64_t riffSize = headerAfterRiff + dataBytes + pad + (rf64 ? 36 : 0);
    putLe(h, rf64 ? 0xFFFFFFFFull : riffSize, 4);
    h += "WAVE";
    if (rf64) {
        h += "ds64";
        putLe(h, 28, 4);
        putLe(h, riffSize, 8);
        putLe(h, dataBytes, 8);
        putLe(h, frames, 8);
        putLe(h, 0, 4);
    }
    h += "fmt ";
    putLe(h, fmtSize, 4);
    putLe(h, isFloat ? 3 : 1, 2);
    putLe(h, channels, 2);
    putLe(h, sampleRate, 4);
    putLe(h, (uint64_t)sampleRate * blockAlign, 4);
    putLe(h, blockAlign, 2);
    putLe(h, bytes * 8, 2);
    if (isFloat) {
        putLe(h, 0, 2);  // cbSize
        h += "fact";
        putLe(h, 4, 4);
        putLe(h, std::min<uint64_t>(frames, 0xFFFFFFFFull), 4);
    }
    h += "data";
    putLe(h, rf64 ? 0xFFFFFFFFull : dataBytes, 4);

    Writer w;
    if (!w.open(path, error)) return false;
    w.write(h);
    std::vector<uint8_t> buf;
    const size_t chunk = 16384;
    for (size_t f0 = 0; f0 < frames && w.ok; f0 += chunk) {
        size_t nf = std::min(chunk, frames - f0);
        buf.resize(nf * blockAlign);
        uint8_t* o = buf.data();
        const float* in = interleaved + f0 * channels;
        for (size_t i = 0; i < nf * channels; i++) {
            if (isFloat) {
                memcpy(o, &in[i], 4);  // (little-endian machines)
                o += 4;
            } else {
                int64_t v = quantize(in[i], bytes * 8);
                for (int b = 0; b < bytes; b++) *o++ = (uint8_t)(v >> (8 * b));
            }
        }
        w.write(buf.data(), buf.size());
    }
    if (pad) w.write("\0", 1);
    return w.close(error);
}

bool writeFlac(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
               SampleFormat format, std::string& error) {
    if (format == SampleFormat::Float32) {
        error = "Cannot write " + path + ": FLAC files are 16-bit or 24-bit";
        return false;
    }
    if (channels < 1 || channels > 8) {
        error = "Cannot write " + path + ": FLAC files have 1 to 8 channels";
        return false;
    }
    if (sampleRate < 1 || sampleRate > 1048575) {
        error = "Cannot write " + path + ": unsupported sample rate for FLAC";
        return false;
    }
    const int bps = format == SampleFormat::Pcm16 ? 16 : 24;
    const int blockSize = 4096;
    Writer w;
    if (!w.open(path, error)) return false;
    w.write("fLaC", 4);
    uint8_t info[38] = {0};
    w.write(info, sizeof info);  // STREAMINFO, written once everything is known

    Md5 md5;
    uint32_t minFrame = 0xFFFFFF, maxFrame = 0;
    std::vector<std::vector<int64_t>> chans(channels);
    std::vector<int64_t> y, r;
    std::vector<uint64_t> prefix;
    std::vector<uint8_t> md5Bytes;
    BitWriter bw;
    uint64_t frameNumber = 0;
    for (size_t f0 = 0; f0 < frames && w.ok; f0 += blockSize, frameNumber++) {
        int n = (int)std::min<size_t>(blockSize, frames - f0);
        md5Bytes.resize((size_t)n * channels * (bps / 8));
        uint8_t* mb = md5Bytes.data();
        for (int c = 0; c < channels; c++) chans[c].resize(n);
        const float* in = interleaved + f0 * channels;
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < channels; c++) {
                int64_t v = quantize(in[(size_t)i * channels + c], bps);
                chans[c][i] = v;
                for (int b = 0; b < bps / 8; b++) *mb++ = (uint8_t)(v >> (8 * b));
            }
        }
        md5.update(md5Bytes.data(), md5Bytes.size());
        bw.buf.clear();
        encodeFlacFrame(bw, frameNumber, chans, n, bps, sampleRate, y, r, prefix);
        minFrame = std::min<uint32_t>(minFrame, (uint32_t)bw.buf.size());
        maxFrame = std::max<uint32_t>(maxFrame, (uint32_t)bw.buf.size());
        w.write(bw.buf.data(), bw.buf.size());
    }
    if (frames == 0) minFrame = 0;
    BitWriter si;
    si.put(0x80, 8);  // the last metadata block, STREAMINFO
    si.put(34, 24);
    si.put(blockSize, 16);
    si.put(blockSize, 16);
    si.put(minFrame, 24);
    si.put(maxFrame, 24);
    si.put(sampleRate, 20);
    si.put(channels - 1, 3);
    si.put(bps - 1, 5);
    si.put64(frames, 36);
    uint8_t digest[16];
    md5.digest(digest);
    for (uint8_t b : digest) si.put(b, 8);
    if (w.ok && fseek(w.f, 4, SEEK_SET) != 0) w.ok = false;
    w.write(si.buf.data(), si.buf.size());
    return w.close(error);
}

// beats: times in beats (ticks / ticks per beat) rather than seconds
static bool readMidi(const std::string& path, std::vector<MidiFileNote>& notes, double& lengthSeconds,
                     std::string& error, bool beats, std::vector<MidiFileSig>* sigs = nullptr) {
    notes.clear();
    std::vector<std::pair<int64_t, MidiFileSig>> sigTicks;  // (time_signature events, at their ticks)
    lengthSeconds = 0;
    FileBytes file;
    if (!file.open(path, error)) return false;
    const uint8_t* d = file.data;
    size_t n = file.size;
    size_t pos = 0;
    // RIFF MIDI ("RMID"): the Standard MIDI File is its "data" chunk
    if (n >= 20 && tag(d, "RIFF") && tag(d + 8, "RMID")) {
        size_t p = 12;
        while (p + 8 <= n) {
            size_t size = le32(d + p + 4);
            if (tag(d + p, "data")) {
                pos = p + 8;
                n = std::min(n, pos + size);
                break;
            }
            p += 8 + size + (size & 1);
        }
    }
    if (pos + 14 > n || !tag(d + pos, "MThd")) {
        error = "Not a MIDI file";
        return false;
    }
    size_t hlen = be32(d + pos + 4);
    if (hlen < 6 || hlen > n - pos - 8) {
        error = "Damaged MIDI file";
        return false;
    }
    int ntracks = be16(d + pos + 10);
    uint16_t division = be16(d + pos + 12);
    // seconds per tick: from the tempo (ticks per beat), or fixed (SMPTE time)
    double smpteTick = 0;
    int tpb = division;
    if (division & 0x8000) {
        int fps = -(int8_t)(division >> 8);
        int perFrame = division & 0xFF;
        double rate = fps == 29 ? 29.97 : fps;
        if (rate <= 0 || perFrame == 0) {
            error = "Damaged MIDI file";
            return false;
        }
        smpteTick = 1.0 / (rate * perFrame);
    } else if (tpb == 0) {
        error = "Damaged MIDI file";
        return false;
    }
    pos += 8 + hlen;

    // every track's events, at absolute ticks (end-of-track markers only count for the length)
    std::vector<MidiEvent> events;
    int64_t lastTick = 0;
    int tracksRead = 0;
    while (pos + 8 <= n && tracksRead < ntracks) {
        size_t size = be32(d + pos + 4);
        size_t body = pos + 8;
        size_t end = body + std::min(size, n - body);
        if (!tag(d + pos, "MTrk")) {
            pos = end;
            continue;
        }
        tracksRead++;
        size_t p = body;
        int64_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        while (p < end && ok) {
            tick += readVarLen(d, end, p, ok);
            if (!ok || p >= end) break;
            uint8_t status = d[p];
            if (status < 0x80) {
                if (!running) { ok = false; break; }  // running status with nothing before it
                status = running;
            } else {
                p++;
            }
            MidiEvent ev{tick, 0, 0, 0, 0};
            if (status == 0xFF) {  // meta
                if (p >= end) break;
                uint8_t type = d[p++];
                uint32_t len = readVarLen(d, end, p, ok);
                if (!ok || len > end - p) break;
                if (type == 0x2F) {  // end of track
                    lastTick = std::max(lastTick, tick);
                    p += len;
                    continue;
                }
                if (type == 0x51 && len >= 3) {
                    ev.status = 0xFF;
                    ev.tempo = ((uint32_t)d[p] << 16) | ((uint32_t)d[p + 1] << 8) | d[p + 2];
                }
                if (type == 0x58 && len >= 2 && sigs) {  // time_signature: numerator, the denominator's power of two
                    int num = d[p], power = d[p + 1];
                    if (power > 5 || num < 1) {
                        util::debugf("%s: a time signature %d/2^%d at tick %lld: read as 4/4\n", path.c_str(), num, power,
                                     (long long)tick);
                        num = 4, power = 2;
                    }
                    sigTicks.push_back({tick, MidiFileSig{0, num, 1 << power}});
                }
                p += len;
            } else if (status == 0xF0 || status == 0xF7) {  // sysex
                uint32_t len = readVarLen(d, end, p, ok);
                if (!ok || len > end - p) break;
                p += len;
            } else if (status >= 0xF0) {  // system common / real time
                int len = status == 0xF2 ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
                if (p + len > end) break;
                p += len;
            } else {
                running = status;
                int hi = status & 0xF0;
                int len = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                if (p + len > end) break;
                if (hi == 0x80 || hi == 0x90) {
                    ev.status = status;
                    ev.a = d[p] & 0x7F;
                    ev.b = d[p + 1] & 0x7F;
                }
                p += len;
            }
            lastTick = std::max(lastTick, tick);
            events.push_back(ev);
        }
        pos = end;
    }
    if (tracksRead == 0 && ntracks > 0) {
        error = "Damaged MIDI file: it has no tracks";
        return false;
    }
    // the tracks merged (mido.merge_tracks: a stable sort by time), played with the tempo map
    std::stable_sort(events.begin(), events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.tick < b.tick; });
    uint32_t tempo = 500000;
    double now = 0;
    int64_t prev = 0;
    auto advance = [&](int64_t tick) {
        int64_t delta = tick - prev;
        prev = tick;
        if (beats) now = (double)tick / (smpteTick > 0 || tpb <= 0 ? 480.0 : (double)tpb);
        else if (delta > 0) now += smpteTick > 0 ? delta * smpteTick : (double)delta * ((double)tempo * 1e-6 / tpb);
    };
    struct Held { double start; int velocity; };
    std::map<std::pair<int, int>, std::vector<Held>> held;  // (channel, note) -> held notes, first first
    std::vector<std::pair<int, int>> heldOrder;
    for (const MidiEvent& ev : events) {
        advance(ev.tick);
        if (ev.status == 0xFF) {
            tempo = ev.tempo;
            continue;
        }
        int hi = ev.status & 0xF0;
        if (hi != 0x80 && hi != 0x90) continue;
        std::pair<int, int> key(ev.status & 0x0F, ev.a);
        if (hi == 0x90 && ev.b > 0) {
            auto it = held.find(key);
            if (it == held.end()) heldOrder.push_back(key);
            held[key].push_back({now, ev.b});
        } else {
            auto it = held.find(key);
            if (it != held.end() && !it->second.empty()) {
                Held h = it->second.front();
                it->second.erase(it->second.begin());
                notes.push_back({h.start, now, ev.a, h.velocity, key.first});
            }
        }
    }
    advance(std::max(lastTick, prev));
    for (auto& key : heldOrder)  // notes never released end with the file
        for (auto& h : held[key]) notes.push_back({h.start, now, key.second, h.velocity, key.first});
    std::stable_sort(notes.begin(), notes.end(), [](const MidiFileNote& a, const MidiFileNote& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end < b.end;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.velocity < b.velocity;
    });
    lengthSeconds = now;
    if (sigs) {
        std::stable_sort(sigTicks.begin(), sigTicks.end(),
                         [](const std::pair<int64_t, MidiFileSig>& a, const std::pair<int64_t, MidiFileSig>& b) {
                             return a.first < b.first;
                         });
        sigs->clear();
        for (auto& s : sigTicks) {
            s.second.beat = (double)s.first / (smpteTick > 0 || tpb <= 0 ? 480.0 : (double)tpb);
            sigs->push_back(s.second);
        }
    }
    return true;
}

bool readMidiFile(const std::string& path, std::vector<MidiFileNote>& notes, double& lengthSeconds,
                  std::string& error) {
    return readMidi(path, notes, lengthSeconds, error, false);
}

bool readMidiFileBeats(const std::string& path, std::vector<MidiFileNote>& notes, double& lengthBeats,
                       std::string& error, std::vector<MidiFileSig>* sigs) {
    return readMidi(path, notes, lengthBeats, error, true, sigs);
}

bool readMidiFileSignatures(const std::string& path, std::vector<MidiFileSig>& sigs, std::string& error) {
    std::vector<MidiFileNote> notes;
    double length = 0;
    return readMidi(path, notes, length, error, true, &sigs);
}

// ============================== writing MIDI files ==============================

namespace midi_out {
MidiOutEvent noteOn(int64_t tick, int pitch, int velocity, int channel) {
    return {tick, 1, {(uint8_t)(0x90 | (channel & 15)), (uint8_t)(pitch & 127), (uint8_t)(velocity & 127)}};
}
MidiOutEvent noteOff(int64_t tick, int pitch, int channel) {
    return {tick, 0, {(uint8_t)(0x80 | (channel & 15)), (uint8_t)(pitch & 127), 0}};
}
MidiOutEvent tempo(int64_t tick, double bpm) {
    uint32_t us = (uint32_t)std::lround(60e6 / bpm);  // mido.bpm2tempo
    return {tick, 0, {0xFF, 0x51, 3, (uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us}};
}
MidiOutEvent timeSignature(int64_t tick, int numerator, int denominator) {
    int power = 0;
    while ((1 << power) < denominator && power < 7) power++;
    // (mido's defaults: 24 MIDI clocks per click, 8 32nd notes per beat)
    return {tick, 1, {0xFF, 0x58, 4, (uint8_t)numerator, (uint8_t)power, 24, 8}};
}
}  // namespace midi_out

static void putVarLen(std::string& out, uint64_t v) {
    uint8_t bytes[10];
    int n = 0;
    bytes[n++] = v & 0x7F;
    while (v >>= 7) bytes[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) out += (char)bytes[n];
}

static void putBe(std::string& out, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) out += (char)((v >> (8 * i)) & 0xFF);
}

bool writeMidiFile(const std::string& path, int ticksPerBeat, const std::vector<MidiOutTrack>& tracks,
                   std::string& error) {
    if (ticksPerBeat <= 0 || ticksPerBeat > 0x7FFF) {
        error = "Cannot write " + path + ": ticks per beat out of range";
        return false;
    }
    std::string file = "MThd";
    putBe(file, 6, 4);
    putBe(file, 1, 2);  // type 1
    putBe(file, tracks.size(), 2);
    putBe(file, (uint64_t)ticksPerBeat, 2);
    for (const MidiOutTrack& track : tracks) {
        std::string body;
        body += '\0';  // the name, at tick 0
        body += (char)0xFF;
        body += (char)0x03;
        putVarLen(body, track.name.size());
        body += track.name;
        std::vector<const MidiOutEvent*> events;
        for (auto& e : track.events) events.push_back(&e);
        std::stable_sort(events.begin(), events.end(), [](const MidiOutEvent* a, const MidiOutEvent* b) {
            return a->tick != b->tick ? a->tick < b->tick : a->order < b->order;
        });
        int64_t now = 0;
        for (const MidiOutEvent* e : events) {
            int64_t at = std::max(now, e->tick);
            putVarLen(body, (uint64_t)(at - now));
            now = at;
            body.append(e->bytes.begin(), e->bytes.end());
        }
        body += '\0';  // end of track
        body += (char)0xFF;
        body += (char)0x2F;
        body += '\0';
        file += "MTrk";
        putBe(file, body.size(), 4);
        file += body;
    }
    Writer w;
    if (!w.open(path, error)) return false;
    w.write(file);
    return w.close(error);
}
