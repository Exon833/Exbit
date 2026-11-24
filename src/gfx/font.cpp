// Fonts: finding the system's TrueType files, reading them (cmap, metrics, glyf outlines, kern),
// rendering glyphs into cached coverage masks, laying out text, and the fallback chain for
// characters a font lacks.
#include "gfx/font.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <unordered_map>

#include "core/os.h"
#include "core/util.h"
#include "gfx/ttf_raster.h"
#include "platform/platform.h"

// ================================================================================================
// UTF-8

uint32_t utf8Next(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        i++;
        return c;
    }
    int n = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0 || c >= 0xF8 || i + n >= s.size()) {
        i++;
        return 0xFFFD;
    }
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) {
            i++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    i += n + 1;
    return cp;
}

std::u32string utf8ToU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) out.push_back(utf8Next(s, i));
    return out;
}

std::string codepointToUtf8(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out = "\xEF\xBF\xBD";
    }
    return out;
}

std::string u32ToUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) out += codepointToUtf8((uint32_t)c);
    return out;
}

size_t utf8PrevBoundary(const std::string& s, size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

size_t utf8NextBoundary(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    utf8Next(s, i);
    return i;
}

// ================================================================================================
// TrueType files

namespace {

inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline int16_t s16(const uint8_t* p) { return (int16_t)u16(p); }
inline uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string alnumLower(const std::string& s) {
    std::string o;
    for (char c : s)
        if (isalnum((unsigned char)c)) o += (char)tolower((unsigned char)c);
    return o;
}

// one font face of a file (index > 0 inside a .ttc collection), as found by the scan
struct FaceInfo {
    std::string path;
    int index = 0;
    std::string family, typoFamily, style;  // name IDs 1, 16, 2/17
    int weight = 400, width = 5;
    bool italic = false, fixedPitch = false;
    std::string display() const { return typoFamily.empty() ? family : typoFamily; }
};

std::string decodeName(const uint8_t* p, int len, int platform) {
    std::string out;
    if (platform == 0 || platform == 3) {  // UTF-16BE
        for (int i = 0; i + 1 < len; i += 2) {
            uint32_t c = u16(p + i);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
                uint32_t lo = u16(p + i + 2);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            out += codepointToUtf8(c);
        }
    } else {
        for (int i = 0; i < len; i++) out += codepointToUtf8(p[i]);  // Mac Roman ~ Latin-1 for names
    }
    return out;
}

// name ID -> best string of a name table
std::string nameOf(const uint8_t* t, size_t len, int id) {
    if (len < 6) return "";
    int count = u16(t + 2), strOff = u16(t + 4);
    int bestScore = -1;
    std::string best;
    for (int i = 0; i < count; i++) {
        size_t r = 6 + (size_t)i * 12;
        if (r + 12 > len) break;
        int plat = u16(t + r), enc = u16(t + r + 2), lang = u16(t + r + 4), nid = u16(t + r + 6);
        int l = u16(t + r + 8), off = u16(t + r + 10);
        if (nid != id) continue;
        int score;
        if (plat == 3 && (enc == 1 || enc == 10)) score = lang == 0x409 ? 4 : 3;
        else if (plat == 0) score = 2;
        else if (plat == 1 && enc == 0) score = lang == 0 ? 1 : 0;
        else continue;
        if (score > bestScore && (size_t)strOff + off + l <= len) {
            bestScore = score;
            best = decodeName(t + strOff + off, l, plat);
        }
    }
    return best;
}

bool preadAll(int fd, void* buf, size_t n, off_t at) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char*)buf + got, n - got, at + (off_t)got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// reads just the names / OS/2 / post of the faces of one file
void scanFile(const std::string& path, std::vector<FaceInfo>& out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    uint8_t hdr[12];
    std::vector<uint32_t> offsets;
    if (preadAll(fd, hdr, 12, 0)) {
        if (!memcmp(hdr, "ttcf", 4)) {
            uint32_t n = std::min<uint32_t>(u32(hdr + 8), 64);
            std::vector<uint8_t> o(n * 4);
            if (preadAll(fd, o.data(), o.size(), 12))
                for (uint32_t i = 0; i < n; i++) offsets.push_back(u32(&o[i * 4]));
        } else {
            offsets.push_back(0);
        }
    }
    for (size_t fi = 0; fi < offsets.size(); fi++) {
        uint8_t h[12];
        if (!preadAll(fd, h, 12, offsets[fi])) continue;
        uint32_t ver = u32(h);
        if (ver != 0x00010000 && memcmp(h, "true", 4) != 0) continue;  // CFF ('OTTO') outlines: skip
        int nt = std::min<int>(u16(h + 4), 64);
        std::vector<uint8_t> dir((size_t)nt * 16);
        if (!preadAll(fd, dir.data(), dir.size(), offsets[fi] + 12)) continue;
        uint32_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0, postOff = 0, postLen = 0;
        bool glyf = false;
        for (int i = 0; i < nt; i++) {
            const uint8_t* r = &dir[(size_t)i * 16];
            if (!memcmp(r, "name", 4)) nameOff = u32(r + 8), nameLen = u32(r + 12);
            else if (!memcmp(r, "OS/2", 4)) os2Off = u32(r + 8), os2Len = u32(r + 12);
            else if (!memcmp(r, "post", 4)) postOff = u32(r + 8), postLen = u32(r + 12);
            else if (!memcmp(r, "glyf", 4)) glyf = true;
        }
        if (!glyf || !nameOff || nameLen > (1u << 20)) continue;
        std::vector<uint8_t> nt8(nameLen);
        if (!preadAll(fd, nt8.data(), nameLen, nameOff)) continue;
        FaceInfo f;
        f.path = path;
        f.index = (int)fi;
        f.family = nameOf(nt8.data(), nameLen, 1);
        f.typoFamily = nameOf(nt8.data(), nameLen, 16);
        f.style = nameOf(nt8.data(), nameLen, 17);
        if (f.style.empty()) f.style = nameOf(nt8.data(), nameLen, 2);
        if (f.family.empty()) continue;
        if (os2Off && os2Len >= 64) {
            uint8_t o[64];
            if (preadAll(fd, o, 64, os2Off)) {
                f.weight = u16(o + 4);
                f.width = u16(o + 6);
                f.italic = (u16(o + 62) & 1) != 0;
            }
        }
        std::string st = lower(f.style);
        if (st.find("italic") != std::string::npos || st.find("oblique") != std::string::npos) f.italic = true;
        if (postOff && postLen >= 16) {
            uint8_t p[16];
            if (preadAll(fd, p, 16, postOff)) f.fixedPitch = u32(p + 12) != 0;
        }
        out.push_back(std::move(f));
    }
    close(fd);
}

void scanDir(const std::string& dir, std::vector<FaceInfo>& out, int depth) {
    if (depth > 8) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scanDir(p, out, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            std::string ext = n.size() > 4 ? lower(n.substr(n.size() - 4)) : "";
            if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") scanFile(p, out);
        }
    }
}

std::vector<FaceInfo>& allFaces() {
    static std::vector<FaceInfo> faces;
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
#ifdef _WIN32
        // Windows' fonts, and those installed for this user only
        const char* windir = getenv("WINDIR");
        std::vector<std::string> dirs = {util::joinPath(windir ? os::portablePath(windir) : "C:/Windows", "Fonts")};
        if (const char* local = getenv("LOCALAPPDATA"))
            dirs.push_back(os::portablePath(local) + "/Microsoft/Windows/Fonts");
#else
        std::vector<std::string> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
        if (const char* home = getenv("HOME")) {
            dirs.push_back(std::string(home) + "/.local/share/fonts");
            dirs.push_back(std::string(home) + "/.fonts");
        }
#endif
        for (const std::string& d : dirs) scanDir(d, faces, 0);
    }
    return faces;
}

// ------------------------------------------------------------------------------------------------
// a loaded font

struct TtfFont {
    std::string path;
    int index = 0;
    std::vector<uint8_t> data;
    uint32_t glyf = 0, glyfLen = 0, loca = 0, locaLen = 0, hmtx = 0, hmtxLen = 0, cmapSub = 0;
    int cmapFormat = 0;
    int upem = 2048, numGlyphs = 0, locFormat = 0, numHM = 0;
    int asc = 0, desc = 0, gap = 0;  // font units, desc > 0 below the baseline
    int xHeight = 0, capHeight = 0;  // font units (0: unknown)
    std::unordered_map<uint32_t, int16_t> kern;  // 'kern' table pairs
    std::vector<uint32_t> gposPairs;              // GPOS 'kern' feature: PairPos subtables (absolute offsets)
    mutable std::unordered_map<uint32_t, int16_t> gposCache;
    std::unordered_map<uint32_t, int> cmapCache;

    struct Pt { float x, y; bool on; };

    bool load(const std::string& p, int idx) {
        path = p;
        index = idx;
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 12) { fclose(f); return false; }
        data.resize((size_t)n);
        bool ok = fread(data.data(), 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        if (!ok) return false;
        uint32_t base = 0;
        if (!memcmp(data.data(), "ttcf", 4)) {
            uint32_t cnt = u32(&data[8]);
            if ((uint32_t)idx >= cnt || 12 + 4 * (size_t)idx + 4 > data.size()) return false;
            base = u32(&data[12 + 4 * idx]);
        }
        if (base + 12 > data.size()) return false;
        int nt = u16(&data[base + 4]);
        uint32_t head = 0, hhea = 0, maxp = 0, cmap = 0, kernT = 0, os2 = 0, kernLen = 0, cmapLen = 0, os2Len = 0;
        uint32_t gpos = 0, gposLen = 0;
        for (int i = 0; i < nt; i++) {
            size_t r = base + 12 + (size_t)i * 16;
            if (r + 16 > data.size()) return false;
            const uint8_t* t = &data[r];
            uint32_t off = u32(t + 8), len = u32(t + 12);
            if ((uint64_t)off + len > data.size()) continue;
            if (!memcmp(t, "head", 4)) head = off;
            else if (!memcmp(t, "hhea", 4)) hhea = off;
            else if (!memcmp(t, "maxp", 4)) maxp = off;
            else if (!memcmp(t, "cmap", 4)) cmap = off, cmapLen = len;
            else if (!memcmp(t, "loca", 4)) loca = off, locaLen = len;
            else if (!memcmp(t, "glyf", 4)) glyf = off, glyfLen = len;
            else if (!memcmp(t, "hmtx", 4)) hmtx = off, hmtxLen = len;
            else if (!memcmp(t, "kern", 4)) kernT = off, kernLen = len;
            else if (!memcmp(t, "GPOS", 4)) gpos = off, gposLen = len;
            else if (!memcmp(t, "OS/2", 4)) os2 = off, os2Len = len;
        }
        if (!head || !hhea || !maxp || !cmap || !loca || !glyf || !hmtx) return false;
        upem = u16(&data[head + 18]);
        if (upem < 16 || upem > 16384) upem = 2048;
        locFormat = s16(&data[head + 50]);
        numGlyphs = u16(&data[maxp + 4]);
        asc = s16(&data[hhea + 4]);
        desc = -s16(&data[hhea + 6]);
        gap = s16(&data[hhea + 8]);
        numHM = u16(&data[hhea + 34]);
        if ((asc == 0 && desc == 0) && os2 && os2Len >= 78) {
            asc = u16(&data[os2 + 74]);
            desc = u16(&data[os2 + 76]);
            gap = 0;
        }
        if (gap < 0) gap = 0;
        if (os2 && os2Len >= 90 && u16(&data[os2]) >= 2) {
            xHeight = s16(&data[os2 + 86]);
            capHeight = s16(&data[os2 + 88]);
        }
        // cmap: a Unicode subtable, full repertoire (format 12) preferred
        int nsub = u16(&data[cmap + 2]);
        int best = -1;
        for (int i = 0; i < nsub; i++) {
            size_t r = cmap + 4 + (size_t)i * 8;
            if (r + 8 > cmap + cmapLen) break;
            int plat = u16(&data[r]), enc = u16(&data[r + 2]);
            uint32_t off = cmap + u32(&data[r + 4]);
            if (off + 4 > data.size()) continue;
            int fmt = u16(&data[off]);
            int score = -1;
            if (fmt == 12 && ((plat == 3 && enc == 10) || plat == 0)) score = 5;
            else if (fmt == 4 && ((plat == 3 && enc == 1) || plat == 0)) score = 4;
            else if (fmt == 6 && ((plat == 3 && enc == 1) || plat == 0)) score = 3;
            else if (fmt == 4 && plat == 3 && enc == 0) score = 2;  // symbol fonts
            if (score > best) {
                best = score;
                cmapSub = off;
                cmapFormat = fmt;
            }
        }
        if (best < 0) return false;
        auto topOf = [&](uint32_t cp) {  // yMax of a letter
            uint32_t off, len;
            int g = lookup(cp);
            return g > 0 && glyphRange(g, off, len) && len >= 10 ? (int)s16(&data[off + 8]) : 0;
        };
        if (xHeight <= 0) xHeight = topOf('x');
        if (capHeight <= 0) capHeight = topOf('H');
        if (kernT && kernLen >= 4 && u16(&data[kernT]) == 0) {
            int ntab = u16(&data[kernT + 2]);
            size_t p0 = kernT + 4;
            for (int t = 0; t < ntab && p0 + 6 <= kernT + kernLen; t++) {
                int length = u16(&data[p0 + 2]), cov = u16(&data[p0 + 4]);
                if ((cov >> 8) == 0 && (cov & 1) && !(cov & 4) && !(cov & 2)) {
                    int np = u16(&data[p0 + 6]);
                    for (int k = 0; k < np; k++) {
                        size_t q = p0 + 14 + (size_t)k * 6;
                        if (q + 6 > data.size()) break;
                        kern[(uint32_t)u16(&data[q]) << 16 | u16(&data[q + 2])] = s16(&data[q + 4]);
                    }
                }
                if (length < 6) break;
                p0 += length;
            }
        }
        if (kern.empty() && gpos && gposLen >= 10) readGposKerning(gpos, gposLen);
        return true;
    }

    // the PairPos subtables of the lookups of every 'kern' feature (any script)
    void readGposKerning(uint32_t g, uint32_t len) {
        auto in = [&](size_t o, size_t n) { return o + n <= (size_t)g + len && o + n <= data.size(); };
        uint32_t featList = g + u16(&data[g + 6]), lookupList = g + u16(&data[g + 8]);
        if (!in(featList, 2) || !in(lookupList, 2)) return;
        std::vector<int> lookups;
        int nf = u16(&data[featList]);
        for (int i = 0; i < nf; i++) {
            size_t r = featList + 2 + (size_t)i * 6;
            if (!in(r, 6)) break;
            if (memcmp(&data[r], "kern", 4) != 0) continue;
            size_t ft = featList + u16(&data[r + 4]);
            if (!in(ft, 4)) continue;
            int n = u16(&data[ft + 2]);
            for (int k = 0; k < n && in(ft + 4 + k * 2, 2); k++) lookups.push_back(u16(&data[ft + 4 + k * 2]));
        }
        std::sort(lookups.begin(), lookups.end());
        lookups.erase(std::unique(lookups.begin(), lookups.end()), lookups.end());
        int nl = u16(&data[lookupList]);
        for (int li : lookups) {
            if (li >= nl || !in(lookupList + 2 + li * 2, 2)) continue;
            size_t lt = lookupList + u16(&data[lookupList + 2 + li * 2]);
            if (!in(lt, 6)) continue;
            int type = u16(&data[lt]), nsub = u16(&data[lt + 4]);
            for (int k = 0; k < nsub && in(lt + 6 + k * 2, 2); k++) {
                size_t st = lt + u16(&data[lt + 6 + k * 2]);
                if (type == 9 && in(st, 8) && u16(&data[st + 2]) == 2) st += u32(&data[st + 4]);
                else if (type != 2) continue;
                if (in(st, 10)) gposPairs.push_back((uint32_t)st);
            }
        }
    }

    // index of glyph g in a Coverage table, or -1
    int coverageIndex(size_t c, int gid) const {
        if (c + 4 > data.size()) return -1;
        int fmt = u16(&data[c]), n = u16(&data[c + 2]);
        if (fmt == 1) {
            if (c + 4 + (size_t)n * 2 > data.size()) return -1;
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2, v = u16(&data[c + 4 + mid * 2]);
                if (v == gid) return mid;
                if (v < gid) lo = mid + 1;
                else hi = mid - 1;
            }
        } else if (fmt == 2) {
            if (c + 4 + (size_t)n * 6 > data.size()) return -1;
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                const uint8_t* r = &data[c + 4 + mid * 6];
                if (gid < u16(r)) hi = mid - 1;
                else if (gid > u16(r + 2)) lo = mid + 1;
                else return u16(r + 4) + gid - u16(r);
            }
        }
        return -1;
    }

    int classOf(size_t c, int gid) const {
        if (c + 6 > data.size()) return 0;
        int fmt = u16(&data[c]);
        if (fmt == 1) {
            int start = u16(&data[c + 2]), n = u16(&data[c + 4]);
            if (gid < start || gid >= start + n || c + 6 + (size_t)n * 2 > data.size()) return 0;
            return u16(&data[c + 6 + (gid - start) * 2]);
        }
        if (fmt == 2) {
            int n = u16(&data[c + 2]);
            if (c + 4 + (size_t)n * 6 > data.size()) return 0;
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                const uint8_t* r = &data[c + 4 + mid * 6];
                if (gid < u16(r)) hi = mid - 1;
                else if (gid > u16(r + 2)) lo = mid + 1;
                else return u16(r + 4);
            }
        }
        return 0;
    }

    static int valueSize(int vf) { return 2 * __builtin_popcount(vf & 0xFF); }
    static int xAdvanceAt(int vf) { return (vf & 4) ? 2 * __builtin_popcount(vf & 3) : -1; }

    int gposKerning(int l, int r) const {
        uint32_t key = (uint32_t)l << 16 | (uint32_t)r;
        auto it = gposCache.find(key);
        if (it != gposCache.end()) return it->second;
        int total = 0;
        for (uint32_t st : gposPairs) {
            if (st + 10 > data.size()) continue;
            int fmt = u16(&data[st]);
            int ci = coverageIndex(st + u16(&data[st + 2]), l);
            if (ci < 0) continue;
            int vf1 = u16(&data[st + 4]), vf2 = u16(&data[st + 6]);
            int xa = xAdvanceAt(vf1);
            int recSize = valueSize(vf1) + valueSize(vf2);
            if (fmt == 1) {
                int nsets = u16(&data[st + 8]);
                if (ci >= nsets || st + 10 + (size_t)ci * 2 + 2 > data.size()) continue;
                size_t ps = st + u16(&data[st + 10 + ci * 2]);
                if (ps + 2 > data.size()) continue;
                int n = u16(&data[ps]);
                size_t stride = 2 + recSize;
                if (ps + 2 + n * stride > data.size()) continue;
                int lo = 0, hi = n - 1;
                while (lo <= hi) {
                    int mid = (lo + hi) / 2;
                    const uint8_t* rec = &data[ps + 2 + mid * stride];
                    int g2 = u16(rec);
                    if (g2 == r) {
                        if (xa >= 0) total += s16(rec + 2 + xa);
                        break;
                    }
                    if (g2 < r) lo = mid + 1;
                    else hi = mid - 1;
                }
                if (lo <= hi) break;  // found: this lookup's subtables end here
            } else if (fmt == 2) {
                if (st + 16 > data.size()) continue;
                int c1 = classOf(st + u16(&data[st + 8]), l), c2 = classOf(st + u16(&data[st + 10]), r);
                int n1 = u16(&data[st + 12]), n2 = u16(&data[st + 14]);
                if (c1 >= n1 || c2 >= n2) continue;
                size_t rec = st + 16 + ((size_t)c1 * n2 + c2) * recSize;
                if (rec + recSize > data.size()) continue;
                if (xa >= 0) total += s16(&data[rec + xa]);
                break;
            }
        }
        gposCache[key] = (int16_t)total;
        return total;
    }

    int glyphIndex(uint32_t cp) {
        auto it = cmapCache.find(cp);
        if (it != cmapCache.end()) return it->second;
        int g = lookup(cp);
        cmapCache[cp] = g;
        return g;
    }

    int lookup(uint32_t cp) const {
        const uint8_t* t = &data[cmapSub];
        size_t avail = data.size() - cmapSub;
        if (cmapFormat == 4) {
            if (cp > 0xFFFF || avail < 14) return 0;
            int segX2 = u16(t + 6);
            if ((size_t)14 + segX2 * 4 + 2 > avail) return 0;
            const uint8_t* ends = t + 14;
            const uint8_t* starts = ends + segX2 + 2;
            const uint8_t* deltas = starts + segX2;
            const uint8_t* ranges = deltas + segX2;
            int lo = 0, hi = segX2 / 2 - 1;
            while (lo <= hi) {  // first segment whose end >= cp
                int mid = (lo + hi) / 2;
                if (u16(ends + mid * 2) < cp) lo = mid + 1;
                else hi = mid - 1;
            }
            if (lo >= segX2 / 2) return 0;
            int seg = lo;
            uint32_t start = u16(starts + seg * 2);
            if (cp < start) return 0;
            int delta = s16(deltas + seg * 2);
            int ro = u16(ranges + seg * 2);
            if (ro == 0) return (int)((cp + delta) & 0xFFFF);
            size_t gi = (size_t)(ranges + seg * 2 - t) + ro + (cp - start) * 2;
            if (gi + 2 > avail) return 0;
            int g = u16(t + gi);
            return g ? (int)((g + delta) & 0xFFFF) : 0;
        }
        if (cmapFormat == 12) {
            if (avail < 16) return 0;
            uint32_t n = u32(t + 12);
            if (16 + (size_t)n * 12 > avail) return 0;
            int lo = 0, hi = (int)n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                const uint8_t* g = t + 16 + (size_t)mid * 12;
                uint32_t s = u32(g), e = u32(g + 4);
                if (cp < s) hi = mid - 1;
                else if (cp > e) lo = mid + 1;
                else return (int)(u32(g + 8) + (cp - s));
            }
            return 0;
        }
        if (cmapFormat == 6) {
            uint32_t first = u16(t + 6), cnt = u16(t + 8);
            if (cp < first || cp >= first + cnt || 10 + (size_t)cnt * 2 > avail) return 0;
            return u16(t + 10 + (cp - first) * 2);
        }
        return 0;
    }

    int advance(int gid) const {
        if (numHM <= 0) return 0;
        int i = std::min(gid, numHM - 1);
        size_t o = hmtx + (size_t)i * 4;
        if (o + 2 > hmtx + hmtxLen) return 0;
        return u16(&data[o]);
    }

    int kerning(int l, int r) const {
        if (!gposPairs.empty()) return gposKerning(l, r);
        if (kern.empty()) return 0;
        auto it = kern.find((uint32_t)l << 16 | (uint32_t)r);
        return it == kern.end() ? 0 : it->second;
    }

    bool glyphRange(int gid, uint32_t& off, uint32_t& len) const {
        if (gid < 0 || gid >= numGlyphs) return false;
        uint32_t a, b;
        if (locFormat == 0) {
            if (loca + (size_t)gid * 2 + 4 > loca + locaLen) return false;
            a = u16(&data[loca + gid * 2]) * 2u;
            b = u16(&data[loca + gid * 2 + 2]) * 2u;
        } else {
            if (loca + (size_t)gid * 4 + 8 > loca + locaLen) return false;
            a = u32(&data[loca + gid * 4]);
            b = u32(&data[loca + gid * 4 + 4]);
        }
        if (b < a || b > glyfLen) return false;
        off = glyf + a;
        len = b - a;
        return true;
    }

    // contours in font units (y up)
    bool outline(int gid, std::vector<std::vector<Pt>>& contours, int depth = 0) const {
        if (depth > 8) return false;
        uint32_t off, len;
        if (!glyphRange(gid, off, len)) return false;
        if (len < 10) return true;  // empty glyph (space)
        const uint8_t* g = &data[off];
        const uint8_t* end = g + len;
        int nc = s16(g);
        if (nc >= 0) {
            const uint8_t* p = g + 10;
            if (p + nc * 2 + 2 > end) return false;
            std::vector<int> ends(nc);
            for (int i = 0; i < nc; i++) ends[i] = u16(p + i * 2);
            int npts = nc ? ends[nc - 1] + 1 : 0;
            p += nc * 2;
            int il = u16(p);
            p += 2 + il;
            std::vector<uint8_t> flags(npts);
            for (int i = 0; i < npts;) {
                if (p >= end) return false;
                uint8_t f = *p++;
                flags[i++] = f;
                if (f & 8) {
                    if (p >= end) return false;
                    int rep = *p++;
                    while (rep-- && i < npts) flags[i++] = f;
                }
            }
            std::vector<Pt> pts(npts);
            int v = 0;
            for (int i = 0; i < npts; i++) {
                uint8_t f = flags[i];
                if (f & 2) {
                    if (p >= end) return false;
                    v += (f & 16) ? *p : -*p;
                    p++;
                } else if (!(f & 16)) {
                    if (p + 2 > end) return false;
                    v += s16(p);
                    p += 2;
                }
                pts[i].x = (float)v;
                pts[i].on = f & 1;
            }
            v = 0;
            for (int i = 0; i < npts; i++) {
                uint8_t f = flags[i];
                if (f & 4) {
                    if (p >= end) return false;
                    v += (f & 32) ? *p : -*p;
                    p++;
                } else if (!(f & 32)) {
                    if (p + 2 > end) return false;
                    v += s16(p);
                    p += 2;
                }
                pts[i].y = (float)v;
            }
            int s = 0;
            for (int c = 0; c < nc; c++) {
                int e = ends[c];
                if (e < s || e >= npts) return false;
                contours.emplace_back(pts.begin() + s, pts.begin() + e + 1);
                s = e + 1;
            }
            return true;
        }
        // composite
        const uint8_t* p = g + 10;
        for (;;) {
            if (p + 4 > end) return false;
            int flags = u16(p), child = u16(p + 2);
            p += 4;
            float dx = 0, dy = 0;
            int a1, a2;
            if (flags & 1) {
                if (p + 4 > end) return false;
                a1 = (flags & 2) ? s16(p) : u16(p);
                a2 = (flags & 2) ? s16(p + 2) : u16(p + 2);
                p += 4;
            } else {
                if (p + 2 > end) return false;
                a1 = (flags & 2) ? (int8_t)p[0] : p[0];
                a2 = (flags & 2) ? (int8_t)p[1] : p[1];
                p += 2;
            }
            float m[4] = {1, 0, 0, 1};
            auto f2 = [](const uint8_t* q) { return s16(q) / 16384.f; };
            if (flags & 8) {
                if (p + 2 > end) return false;
                m[0] = m[3] = f2(p);
                p += 2;
            } else if (flags & 0x40) {
                if (p + 4 > end) return false;
                m[0] = f2(p);
                m[3] = f2(p + 2);
                p += 4;
            } else if (flags & 0x80) {
                if (p + 8 > end) return false;
                m[0] = f2(p); m[1] = f2(p + 2); m[2] = f2(p + 4); m[3] = f2(p + 6);
                p += 8;
            }
            std::vector<std::vector<Pt>> sub;
            if (!outline(child, sub, depth + 1)) return false;
            for (auto& c : sub)
                for (Pt& q : c) {
                    float x = q.x, y = q.y;
                    q.x = m[0] * x + m[2] * y;
                    q.y = m[1] * x + m[3] * y;
                }
            if (flags & 2) {
                dx = (float)a1;
                dy = (float)a2;
            } else {  // point matching: parent point a1 meets child point a2
                const Pt* pp = nullptr;
                const Pt* cp = nullptr;
                int k = 0;
                for (auto& c : contours)
                    for (auto& q : c)
                        if (k++ == a1) pp = &q;
                k = 0;
                for (auto& c : sub)
                    for (auto& q : c)
                        if (k++ == a2) cp = &q;
                if (pp && cp) {
                    dx = pp->x - cp->x;
                    dy = pp->y - cp->y;
                }
            }
            for (auto& c : sub) {
                for (Pt& q : c) {
                    q.x += dx;
                    q.y += dy;
                }
                contours.push_back(std::move(c));
            }
            if (!(flags & 0x20)) break;
        }
        return true;
    }
};

// ------------------------------------------------------------------------------------------------
// registry of loaded files and resolved faces

std::map<std::pair<std::string, int>, std::unique_ptr<TtfFont>>& loadedFonts() {
    static std::map<std::pair<std::string, int>, std::unique_ptr<TtfFont>> m;
    return m;
}

TtfFont* loadFont(const FaceInfo& fi) {
    auto& m = loadedFonts();
    auto key = std::make_pair(fi.path, fi.index);
    auto it = m.find(key);
    if (it != m.end()) return it->second.get();
    std::unique_ptr<TtfFont> f(new TtfFont);
    if (!f->load(fi.path, fi.index)) f.reset();
    TtfFont* r = f.get();
    m[key] = std::move(f);
    return r;
}

// the faces added from files (Font::addFamily), named as they were added (a deque: what points at them stays)
std::deque<FaceInfo>& addedFaces() {
    static std::deque<FaceInfo> faces;
    return faces;
}

// the faces of a family (those added first; else matched on the name table, then on the file name)
std::vector<const FaceInfo*> familyFaces(const std::string& family) {
    std::vector<const FaceInfo*> out;
    std::string want = lower(family);
    for (const FaceInfo& f : addedFaces())
        if (lower(f.family) == want) out.push_back(&f);
    if (!out.empty()) return out;
    for (const FaceInfo& f : allFaces())
        if (lower(f.family) == want || lower(f.typoFamily) == want) out.push_back(&f);
    if (out.empty()) {
        std::string w = alnumLower(family);
        if (w.empty()) return out;
        for (const FaceInfo& f : allFaces()) {
            size_t slash = f.path.find_last_of('/');
            std::string base = f.path.substr(slash + 1);
            base = alnumLower(base.substr(0, base.size() - 4));
            if (base == w || base == w + "regular" || base == w + "bold" || base == w + "book") out.push_back(&f);
        }
    }
    return out;
}

// the best face of a family for normal or bold text (upright, normal width)
const FaceInfo* pickStyle(const std::vector<const FaceInfo*>& faces, bool bold) {
    const FaceInfo* best = nullptr;
    int bestScore = 1 << 30;
    int target = bold ? 700 : 400;
    for (const FaceInfo* f : faces) {
        int score = (f->italic ? 100000 : 0) + std::abs(f->width - 5) * 1000 + std::abs(f->weight - target);
        if (!bold && f->weight > 400) score += 1;  // prefer Regular over Medium
        std::string st = lower(f->style);
        if (st == (bold ? "bold" : "regular") || (!bold && st == "book")) score -= 5;
        if (score < bestScore) {
            bestScore = score;
            best = f;
        }
    }
    return best;
}

// (the last ones: Windows' own)
const char* const kUiFamilies[] = {"DejaVu Sans", "Noto Sans", "Liberation Sans", "Cantarell", "Droid Sans",
                                   "FreeSans", "Ubuntu", "Open Sans", "Segoe UI", "Tahoma", "Arial"};
const char* const kMonoFamilies[] = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Hack",
                                     "Droid Sans Mono", "FreeMono", "Ubuntu Mono", "Noto Mono", "Consolas",
                                     "Cascadia Mono", "Courier New"};
const char* const kFallbackFamilies[] = {"DejaVu Sans", "DejaVu Sans Mono", "Noto Sans Symbols", "Noto Sans Symbols2",
                                         "Noto Sans Math", "Symbola", "FreeSerif", "Noto Sans", "DejaVu Serif",
                                         "Liberation Sans", "Noto Music", "Droid Sans Fallback", "Segoe UI Symbol",
                                         "Segoe UI", "Cambria Math", "Segoe UI Historic", "Arial", "Microsoft YaHei",
                                         "Yu Gothic", "Malgun Gothic", "Nirmala UI"};

bool isMonoName(const std::string& l) {
    static const char* names[] = {"consolas", "courier new", "courier", "monospace", "mono", "menlo", "monaco",
                                  "lucida console", "cascadia mono", "cascadia code", "sf mono"};
    for (const char* n : names)
        if (l == n) return true;
    return false;
}

std::vector<const FaceInfo*> defaultFamily(bool mono) {
    for (const char* f : mono ? kMonoFamilies : kUiFamilies) {
        auto v = familyFaces(f);
        if (!v.empty()) return v;
    }
    // any sans (or monospaced) face
    std::vector<const FaceInfo*> out;
    for (const FaceInfo& f : allFaces()) {
        std::string l = lower(f.display());
        bool m = f.fixedPitch || l.find("mono") != std::string::npos;
        if (mono ? m : (l.find("sans") != std::string::npos && !m)) {
            if (out.empty() || out[0]->display() == f.display()) out.push_back(&f);
        }
    }
    if (out.empty() && !allFaces().empty()) out.push_back(&allFaces()[0]);
    return out;
}

}  // namespace

// ================================================================================================
// FontFace: a family + boldness resolved to a file, with the fallback chain

class FontFace {
public:
    TtfFont* primary = nullptr;
    bool synthBold = false;   // embolden the outlines (no Bold file)
    bool wantBold = false;
    struct Mapped { TtfFont* font; int gid; bool synth; };  // gid < 0: nobody has it (box)
    std::unordered_map<uint32_t, Mapped> map;
    Mapped ascii[128];
    bool asciiReady[128] = {};

    Mapped lookup(uint32_t cp) {
        if (cp < 128) {
            if (!asciiReady[cp]) {
                ascii[cp] = lookupSlow(cp);
                asciiReady[cp] = true;
            }
            return ascii[cp];
        }
        return lookupSlow(cp);
    }
    Mapped lookupSlow(uint32_t cp);
};

namespace {

std::string g_shippedDir;  // "" : the program's data/fonts
std::vector<TtfFont*> g_fallbacks;
bool g_fallbacksReady = false;

// the fonts shipped with the program first (data/fonts: the interface's symbols, there on any machine), then the
// system's
std::vector<TtfFont*>& fallbackFonts(bool& ready) {
    std::vector<TtfFont*>& v = g_fallbacks;
    ready = g_fallbacksReady;
    if (!g_fallbacksReady) {
        g_fallbacksReady = true;
        std::vector<FaceInfo> shipped;
        std::string dir = g_shippedDir.empty() ? util::joinPath(plat::dataDir(), "fonts") : g_shippedDir;
        scanDir(dir, shipped, 0);
        for (const FaceInfo& fi : shipped)
            if (TtfFont* f = loadFont(fi)) v.push_back(f);
        if (util::debug()) fprintf(stderr, "font: %zu shipped fallback font(s) in %s\n", v.size(), dir.c_str());
        for (const char* fam : kFallbackFamilies) {
            auto faces = familyFaces(fam);
            if (const FaceInfo* fi = pickStyle(faces, false))
                if (TtfFont* f = loadFont(*fi)) v.push_back(f);
        }
    }
    return v;
}

}  // namespace

FontFace::Mapped FontFace::lookupSlow(uint32_t cp) {
    auto it = map.find(cp);
    if (it != map.end()) return it->second;
    Mapped m{primary, -1, synthBold};
    if (primary) {
        int g = primary->glyphIndex(cp);
        if (g > 0) m.gid = g;
    }
    if (m.gid < 0) {
        bool ready;
        for (TtfFont* f : fallbackFonts(ready)) {
            if (f == primary) continue;
            int g = f->glyphIndex(cp);
            if (g > 0) {
                m = Mapped{f, g, wantBold};
                break;
            }
        }
    }
    map[cp] = m;
    return m;
}

namespace {

struct Defaults {
    std::string ui;
    float uiPx = 12;  // (9 pt)
    std::string mono = "Consolas";
};
Defaults& defaults() {
    static Defaults d;
    return d;
}

// the faces resolved so far, by family (lower case) and boldness (Fonts point at them: they stay)
std::map<std::pair<std::string, bool>, std::unique_ptr<FontFace>>& resolvedFaces() {
    static std::map<std::pair<std::string, bool>, std::unique_ptr<FontFace>> faces;
    return faces;
}

// the file (and fallbacks) of a family, into `face`
void fillFace(FontFace* face, const std::string& family, bool bold) {
    std::string l = lower(family);
    std::vector<const FaceInfo*> cands;
    if (!l.empty() && l != "sans" && l != "sans-serif" && l != "sans serif") cands = familyFaces(family);
    if (cands.empty()) {
        if (l == "serif") cands = familyFaces("DejaVu Serif");
        if (cands.empty()) cands = defaultFamily(isMonoName(l));
    }
    face->primary = nullptr;  // (again, when a family was added: what it had found before is forgotten)
    face->synthBold = false;
    face->map.clear();
    std::fill(std::begin(face->asciiReady), std::end(face->asciiReady), false);
    face->wantBold = bold;
    const FaceInfo* fi = pickStyle(cands, bold);
    if (fi) {
        face->primary = loadFont(*fi);
        face->synthBold = bold && fi->weight < 600;
        if (!face->primary) {  // unreadable file: another of the family, or the default
            for (const FaceInfo* c : cands)
                if ((face->primary = loadFont(*c))) break;
            if (!face->primary)
                for (const FaceInfo* c : defaultFamily(false))
                    if ((face->primary = loadFont(*c))) break;
        }
    }
    if (!face->primary) {  // no font on the system at all: the shipped one (it has ASCII and Latin-1)
        bool ready;
        std::vector<TtfFont*>& fb = fallbackFonts(ready);
        if (!fb.empty()) face->primary = fb[0];
        face->synthBold = bold;
    }
    if (util::debug())
        fprintf(stderr, "font: \"%s\"%s -> %s%s\n", family.c_str(), bold ? " bold" : "",
                face->primary ? face->primary->path.c_str() : "(none)", face->synthBold ? " (synthetic bold)" : "");
}

FontFace* resolveFace(const std::string& family, bool bold) {
    auto& faces = resolvedFaces();
    auto key = std::make_pair(lower(family), bold);
    auto it = faces.find(key);
    if (it != faces.end()) return it->second.get();
    std::unique_ptr<FontFace> face(new FontFace);
    fillFace(face.get(), family, bold);
    FontFace* r = face.get();
    faces[key] = std::move(face);
    return r;
}

// ------------------------------------------------------------------------------------------------
// glyph rendering and the cache

struct GlyphKey {
    const TtfFont* font;
    uint32_t sizeBits;
    int gid;
    uint8_t phase;  // subpixel x offset in quarters
    uint8_t bold;
    bool operator==(const GlyphKey& o) const {
        return font == o.font && sizeBits == o.sizeBits && gid == o.gid && phase == o.phase && bold == o.bold;
    }
};
struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        size_t h = std::hash<const void*>()(k.font);
        h ^= (size_t)k.sizeBits * 0x9E3779B97F4A7C15ull;
        h ^= ((size_t)k.gid << 3 | k.phase << 1 | k.bold) * 0xC2B2AE3D27D4EB4Full;
        return h ^ (h >> 29);
    }
};
struct CacheEntry {
    std::unique_ptr<Font::Glyph> glyph;
    uint64_t lastUse = 0;
};

std::unordered_map<GlyphKey, CacheEntry, GlyphKeyHash>& glyphCache() {
    static std::unordered_map<GlyphKey, CacheEntry, GlyphKeyHash> c;
    return c;
}
uint64_t g_useClock = 0;
const size_t kCacheLimit = 8192;

void trimCache() {
    auto& c = glyphCache();
    if (c.size() <= kCacheLimit) return;
    std::vector<uint64_t> uses;
    uses.reserve(c.size());
    for (auto& e : c) uses.push_back(e.second.lastUse);
    size_t drop = c.size() - kCacheLimit * 3 / 4;
    std::nth_element(uses.begin(), uses.begin() + drop, uses.end());
    uint64_t cut = uses[drop];
    for (auto it = c.begin(); it != c.end();) {
        if (it->second.lastUse < cut) it = c.erase(it);
        else ++it;
    }
}

float emboldenStrength(float px) { return px / 24.f; }  // total extra width, as FreeType's

// coverage curve: a light gamma lift so thin stems keep their weight on dark and light backgrounds
const uint8_t* coverageCurve() {
    static uint8_t lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)std::lround(255.0 * std::pow(i / 255.0, 1 / 1.2));
        ready = true;
    }
    return lut;
}

// An outline's edge, in the mask's pixels (y down).
struct Edge {
    float x0, y0, x1, y1;
};

// Dropout control for an outline drawn without anti-aliasing (font-smooth: never). `mask` holds the pixels
// the outline covers at least half of (each 0 or 255); a stroke thinner than a pixel can cover none of its
// pixels by half (a minus sign, an apostrophe, the thin parts of an S or of a serif) and would vanish or break.
// So, as TrueType's monochrome rasterizers do: along each scanline through the pixels' centres (each row's,
// then each column's), every span the outline holds inside (non-zero winding) that has none of the pixels it
// passes through on gets the pixel nearest its middle turned on. A stub (a span at the tip of a part of the
// outline: it reaches the scanline on one side but not the other) is left out when the pixel next to it on the
// side the part goes on was on before this pass: the outer edge of a round stroke drawn in the next row or
// column doesn't grow a bump (and a bar two pixels long still gets both). So every stroke keeps a pixel in each row and each column it crosses; what was already
// on stays as it is.
void dropoutControl(const std::vector<Edge>& edges, int w, int h, uint8_t* mask) {
    struct Crossing {
        float at;
        int dir;
    };
    struct Span {
        float from, to;
    };
    std::vector<Crossing> xs;
    std::vector<std::vector<Span>> spans;
    std::vector<uint8_t> was;  // (the mask before the pass)
    for (int pass = 0; pass < 2; pass++) {
        bool rows = pass == 0;  // rows: scanlines across y, spans along x; then the columns
        int lines = rows ? h : w, len = rows ? w : h;
        spans.assign((size_t)lines, {});
        for (int j = 0; j < lines; j++) {  // each scanline's inside spans
            float c = j + 0.5f;
            xs.clear();
            for (const Edge& e : edges) {
                float a0 = rows ? e.y0 : e.x0, a1 = rows ? e.y1 : e.x1;  // across the scanline
                if (a0 == a1) continue;
                float lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
                if (c < lo || c >= hi) continue;  // (half open: a vertex two edges share counts once)
                float b0 = rows ? e.x0 : e.y0, b1 = rows ? e.x1 : e.y1;  // along it
                xs.push_back({b0 + (b1 - b0) * ((c - a0) / (a1 - a0)), a1 > a0 ? 1 : -1});
            }
            std::sort(xs.begin(), xs.end(), [](const Crossing& a, const Crossing& b) { return a.at < b.at; });
            int wind = 0;
            float start = 0;
            for (const Crossing& x : xs) {
                int before = wind;
                wind += x.dir;
                if (before == 0 && wind != 0) start = x.at;
                else if (before != 0 && wind == 0 && x.at > start) spans[(size_t)j].push_back({start, x.at});
            }
        }
        auto index = [&](int line, int i) { return rows ? (size_t)line * w + i : (size_t)i * w + line; };
        was.assign(mask, mask + (size_t)w * h);
        // does the part of the outline a span is in reach the scanline k too? (a span there that overlaps it)
        auto reaches = [&](int k, const Span& s) {
            if (k < 0 || k >= lines) return false;
            for (const Span& o : spans[(size_t)k])
                if (o.from <= s.to + 0.5f && o.to >= s.from - 0.5f) return true;
            return false;
        };
        for (int j = 0; j < lines; j++)
            for (const Span& s : spans[(size_t)j]) {
                int i0 = std::max(0, (int)std::floor(s.from)), i1 = std::min(len - 1, (int)std::ceil(s.to) - 1);
                bool any = false;
                for (int i = i0; i <= i1 && !any; i++) any = mask[index(j, i)] != 0;
                if (any) continue;
                int i = std::max(0, std::min(len - 1, (int)std::floor((s.from + s.to) * 0.5f)));
                bool before = reaches(j - 1, s), after = reaches(j + 1, s);
                if (before != after && was[index(before ? j - 1 : j + 1, i)]) continue;  // (a stub, drawn next to it)
                mask[index(j, i)] = 255;
            }
    }
}

// `aliased`: drawn without anti-aliasing (each pixel 0 or 255; dropoutControl)
void renderGlyph(const TtfFont& f, int gid, float px, int phase, bool bold, bool aliased, Font::Glyph& out) {
    std::vector<std::vector<TtfFont::Pt>> contours;
    if (!f.outline(gid, contours) || contours.empty()) return;
    float scale = px / f.upem;
    float sx = phase * 0.25f;
    // light vertical hinting: heights are stretched a little (piecewise linearly) so that the
    // x-height and the cap height land on whole pixels: crisp tops of the letters, as FreeType's
    // light hinting does. Widths are unchanged.
    float xh = f.xHeight * scale, cap = f.capHeight * scale;
    bool hint = f.xHeight > 0 && px <= 40;
    float rx = hint ? std::max(1.f, std::round(xh)) : xh;
    bool hintCap = hint && f.capHeight > f.xHeight && cap - xh >= 1.f;
    float rc = hintCap ? std::max(rx + 1, std::round(cap)) : cap;
    for (auto& c : contours)
        for (auto& p : c) {
            p.x = p.x * scale + sx;
            float y = p.y * scale;
            if (hint) {
                if (y <= xh) y = y * rx / xh;
                else if (hintCap && y <= cap) y = rx + (y - xh) * (rc - rx) / (cap - xh);
                else if (hintCap) y = y * rc / cap;
                else y = y * rx / xh;
            }
            p.y = -y;
        }
    if (bold) {
        // move every point outwards along its corner's bisector by half the strength
        float d = emboldenStrength(px) / 2;
        double area = 0;
        for (auto& c : contours)
            for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) area += (double)c[j].x * c[i].y - (double)c[i].x * c[j].y;
        float orient = area > 0 ? 1.f : -1.f;
        for (auto& c : contours) {
            size_t n = c.size();
            if (n < 3) continue;
            std::vector<TtfFont::Pt> o = c;
            for (size_t i = 0; i < n; i++) {
                const auto& a = c[(i + n - 1) % n];
                const auto& b = c[i];
                const auto& e = c[(i + 1) % n];
                float ix = b.x - a.x, iy = b.y - a.y, ox = e.x - b.x, oy = e.y - b.y;
                float il = std::sqrt(ix * ix + iy * iy), ol = std::sqrt(ox * ox + oy * oy);
                if (il < 1e-6f || ol < 1e-6f) continue;
                ix /= il; iy /= il; ox /= ol; oy /= ol;
                // outward normals (y down; orientation from the outline's total area)
                float n1x = -iy * orient, n1y = ix * orient, n2x = -oy * orient, n2y = ox * orient;
                float dot = n1x * n2x + n1y * n2y;
                float k = d / std::max(0.25f, 1 + dot);
                o[i].x = b.x - (n1x + n2x) * k;
                o[i].y = b.y - (n1y + n2y) * k;
            }
            c = std::move(o);
        }
    }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (auto& c : contours)
        for (auto& p : c) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
    int ix0 = (int)std::floor(x0), iy0 = (int)std::floor(y0), ix1 = (int)std::ceil(x1), iy1 = (int)std::ceil(y1);
    int w = ix1 - ix0, h = iy1 - iy0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    static raster::Accumulator acc;
    std::vector<Edge> edges;  // (aliased: the edges too, for dropoutControl)
    acc.begin(w, h);
    auto edge = [&](float ax, float ay, float bx, float by) {
        if (aliased) edges.push_back({ax, ay, bx, by});
        acc.line(ax, ay, bx, by);
    };
    float ox = (float)ix0, oy = (float)iy0;
    const float tol = 0.1f;
    for (auto& c : contours) {
        size_t n = c.size();
        if (n < 2) continue;
        // start on an on-curve point (or the midpoint of two off-curve ones)
        size_t first = 0;
        while (first < n && !c[first].on) first++;
        float sxp, syp;
        std::vector<TtfFont::Pt> pts;
        if (first == n) {
            sxp = (c[0].x + c[1].x) / 2;
            syp = (c[0].y + c[1].y) / 2;
            pts.push_back({sxp, syp, true});
            for (size_t k = 1; k <= n; k++) pts.push_back(c[k % n]);
        } else {
            for (size_t k = 0; k <= n; k++) pts.push_back(c[(first + k) % n]);
        }
        float cx = pts[0].x - ox, cy = pts[0].y - oy;
        float startX = cx, startY = cy;
        bool haveCtrl = false;
        float qx = 0, qy = 0;
        auto lineTo = [&](float x, float y) {
            edge(cx, cy, x, y);
            cx = x;
            cy = y;
        };
        auto quadTo = [&](float ctx, float cty, float x, float y) {
            float ddx = cx - 2 * ctx + x, ddy = cy - 2 * cty + y;
            float dd = std::sqrt(ddx * ddx + ddy * ddy);
            int steps = std::max(1, std::min(64, (int)std::ceil(std::sqrt(0.25f * dd / tol))));
            float px0 = cx, py0 = cy;
            for (int s = 1; s <= steps; s++) {
                float t = (float)s / steps, u = 1 - t;
                float nx = u * u * px0 + 2 * u * t * ctx + t * t * x, ny = u * u * py0 + 2 * u * t * cty + t * t * y;
                edge(cx, cy, nx, ny);
                cx = nx;
                cy = ny;
            }
        };
        for (size_t k = 1; k < pts.size(); k++) {
            float x = pts[k].x - ox, y = pts[k].y - oy;
            if (pts[k].on) {
                if (haveCtrl) quadTo(qx, qy, x, y);
                else lineTo(x, y);
                haveCtrl = false;
            } else {
                if (haveCtrl) {
                    float mx = (qx + x) / 2, my = (qy + y) / 2;
                    quadTo(qx, qy, mx, my);
                }
                qx = x;
                qy = y;
                haveCtrl = true;
            }
        }
        if (haveCtrl) quadTo(qx, qy, startX, startY);
        else if (cx != startX || cy != startY) lineTo(startX, startY);
    }
    out.w = w;
    out.h = h;
    out.left = ix0;
    out.top = iy0;
    out.mask.resize((size_t)w * h);
    acc.readMask(out.mask.data());
    if (aliased) {  // (on where the outline covers half of a pixel or more, then dropout control)
        for (uint8_t& v : out.mask) v = v >= 128 ? 255 : 0;
        dropoutControl(edges, w, h, out.mask.data());
        return;
    }
    const uint8_t* lut = coverageCurve();
    for (uint8_t& v : out.mask) v = lut[v];
}

// a hollow box for characters no font has
void boxGlyph(float px, Font::Glyph& out) {
    int w = std::max(3, (int)std::lround(px * 0.5f)), h = std::max(4, (int)std::lround(px * 0.72f));
    int t = std::max(1, (int)std::lround(px / 14));
    out.w = w;
    out.h = h;
    out.left = std::max(1, (int)std::lround(px * 0.08f));
    out.top = -h;
    out.mask.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (x < t || y < t || x >= w - t || y >= h - t) out.mask[(size_t)y * w + x] = 255;
}

float boxAdvance(float px) { return std::lround(px * 0.5f) + 2 * std::max(1.f, (float)std::lround(px * 0.08f)); }

const Font::Glyph* cachedGlyph(const FontFace::Mapped& m, float px, int phase, bool aliased) {
    GlyphKey k{m.font, 0, m.gid, (uint8_t)phase, (uint8_t)((m.synth ? 1 : 0) | (aliased ? 2 : 0))};
    memcpy(&k.sizeBits, &px, 4);
    auto& c = glyphCache();
    auto it = c.find(k);
    if (it == c.end()) {
        CacheEntry e;
        e.glyph.reset(new Font::Glyph);
        if (m.gid < 0) boxGlyph(px, *e.glyph);
        else if (m.font) renderGlyph(*m.font, m.gid, px, phase, m.synth, aliased, *e.glyph);
        it = c.emplace(k, std::move(e)).first;
    }
    it->second.lastUse = ++g_useClock;
    return it->second.glyph.get();
}

inline bool zeroWidth(uint32_t cp) {
    return cp < 32 || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x2060 ||
           cp == 0xFEFF || (cp >= 0xE0100 && cp <= 0xE01EF);
}

// advance of a mapped glyph in px (unrounded)
inline float advanceOf(const FontFace::Mapped& m, float px) {
    if (m.gid < 0 || !m.font) return boxAdvance(px);
    float a = m.font->advance(m.gid) * px / m.font->upem;
    if (m.synth) a += emboldenStrength(px);
    return a;
}

// walks the text: calls fn(byteIndex, mapped, penX) for each visible character, returns the total
template <class F>
float walk(FontFace* face, float px, const std::string& s, F&& fn) {
    float pen = 0;
    TtfFont* prevFont = nullptr;
    int prevGid = -1;
    for (size_t i = 0; i < s.size();) {
        size_t at = i;
        uint32_t cp = utf8Next(s, i);
        if (zeroWidth(cp)) {
            if (cp == '\t') {
                FontFace::Mapped sp = face->lookup(' ');
                pen += 4 * advanceOf(sp, px);
            }
            prevGid = -1;
            continue;
        }
        FontFace::Mapped m = face->lookup(cp);
        if (m.font && m.font == prevFont && prevGid >= 0 && m.gid >= 0)
            pen += m.font->kerning(prevGid, m.gid) * px / m.font->upem;
        fn(at, m, pen);
        pen += advanceOf(m, px);
        prevFont = m.font;
        prevGid = m.gid;
    }
    return pen;
}

}  // namespace

// ================================================================================================
// Font

Font::Font() : family_(defaults().ui), size_(defaults().uiPx), bold_(false) {}

Font::Font(const std::string& family, float pixelSize, bool bold)
    : family_(family), size_(pixelSize > 0 ? pixelSize : 12.f), bold_(bold) {}

Font Font::fromPoints(const std::string& family, float points, bool bold) {
    return Font(family, points * 4.f / 3.f, bold);
}

FontFace* Font::face() const {
    if (!face_) const_cast<Font*>(this)->face_ = resolveFace(family_, bold_);
    return face_;
}

float Font::ascent() const {
    FontFace* f = face();
    if (!f->primary) return std::ceil(size_ * 0.93f);
    return std::ceil(f->primary->asc * size_ / f->primary->upem - 0.01f);
}

float Font::descent() const {
    FontFace* f = face();
    if (!f->primary) return std::ceil(size_ * 0.24f);
    return std::ceil(f->primary->desc * size_ / f->primary->upem - 0.01f);
}

float Font::lineSpacing() const {
    FontFace* f = face();
    float lead = f->primary ? std::round(f->primary->gap * size_ / f->primary->upem) : 0;
    return ascent() + descent() + lead;
}

float Font::width(const std::string& utf8) const {
    return walk(face(), size_, utf8, [](size_t, const FontFace::Mapped&, float) {});
}

float Font::charWidth(uint32_t cp) const {
    if (zeroWidth(cp)) return cp == '\t' ? 4 * charWidth(' ') : 0;
    return advanceOf(face()->lookup(cp), size_);
}

std::string Font::elided(const std::string& utf8, float maxWidth) const {
    if (width(utf8) <= maxWidth) return utf8;
    const std::string dots = "\xE2\x80\xA6";
    float dw = width(dots);
    if (dw > maxWidth) return "";
    // the longest prefix whose characters end before maxWidth - dw
    size_t cut = 0;
    float limit = maxWidth - dw;
    FontFace* f = face();
    walk(f, size_, utf8, [&](size_t at, const FontFace::Mapped& m, float pen) {
        if (pen + advanceOf(m, size_) <= limit + 0.001f) cut = utf8NextBoundary(utf8, at);
    });
    // walk visits characters in order; cut ends up at the last one that fits (all before it fit too)
    std::string head = utf8.substr(0, cut);
    while (!head.empty() && head.back() == ' ') head.pop_back();
    return head + dots;
}

size_t Font::hitTest(const std::string& utf8, float x) const {
    size_t best = 0;
    float bestD = std::fabs(x);
    float total = walk(face(), size_, utf8, [&](size_t at, const FontFace::Mapped&, float pen) {
        float d = std::fabs(x - pen);
        if (d < bestD) {
            bestD = d;
            best = at;
        }
    });
    if (std::fabs(x - total) < bestD) best = utf8.size();
    return best;
}

bool Font::hasGlyph(uint32_t cp) const {
    if (zeroWidth(cp)) return true;
    return face()->lookup(cp).gid >= 0;
}

float Font::layout(const std::string& utf8, float startX, std::vector<Run>& out) const {
    trimCache();
    out.clear();
    out.reserve(utf8.size());
    float px = size_;
    bool aliased = aliased_;
    return walk(face(), px, utf8, [&](size_t, const FontFace::Mapped& m, float pen) {
        // (aliased: each glyph on a whole pixel, so a letter looks the same wherever it is)
        float x = aliased ? std::floor(startX + pen + 0.5f) : std::floor((startX + pen) * 4.f + 0.5f) / 4.f;
        float ix = std::floor(x);
        int phase = (int)((x - ix) * 4.f + 0.5f) & 3;
        const Glyph* g = cachedGlyph(m, px, phase, aliased);
        out.push_back(Run{g, x});
    });
}

Font Font::ui() {
    const Defaults& d = defaults();
    return Font(d.ui, d.uiPx > 0 ? d.uiPx : 12.f);
}

Font Font::mono() {
    const Defaults& d = defaults();
    return Font(d.mono.empty() ? "Consolas" : d.mono, d.uiPx > 0 ? d.uiPx : 12.f);
}

void Font::setDefaults(const std::string& uiFamily, float uiPixels, const std::string& monoFamily) {
    Defaults& d = defaults();
    d.ui = uiFamily;
    d.uiPx = uiPixels > 0 ? uiPixels : 12.f;
    d.mono = monoFamily.empty() ? "Consolas" : monoFamily;
}

bool Font::addFamily(const std::string& family, const std::string& path, bool bold, bool italic) {
    if (family.empty() || path.empty()) return false;
    std::deque<FaceInfo>& added = addedFaces();
    for (const FaceInfo& f : added)
        if (f.path == path && f.family == family && (f.weight >= 600) == bold && f.italic == italic) return true;
    std::vector<FaceInfo> found;
    scanFile(path, found);
    if (found.empty()) return false;
    for (FaceInfo& f : found) {  // (as the stylesheet says it is)
        f.family = family;
        f.typoFamily.clear();
        f.weight = bold ? 700 : 400;
        f.width = 5;
        f.italic = italic;
        f.style = bold ? (italic ? "Bold Italic" : "Bold") : (italic ? "Italic" : "Regular");
        added.push_back(std::move(f));
    }
    // what was found for that name before (another font, or the default) is looked up again
    auto& faces = resolvedFaces();
    for (bool b : {false, true}) {
        auto it = faces.find(std::make_pair(lower(family), b));
        if (it != faces.end()) fillFace(it->second.get(), family, b);
    }
    return true;
}

void Font::setShippedFolder(const std::string& dir) {
    g_shippedDir = dir;
    g_fallbacks.clear();
    g_fallbacksReady = false;
    for (auto& f : resolvedFaces()) fillFace(f.second.get(), f.first.first, f.first.second);  // (looked up again)
}

std::string Font::glyphFile(uint32_t cp) const {
    FontFace::Mapped m = face()->lookup(cp);
    return m.font && m.gid >= 0 ? m.font->path : std::string();
}

std::vector<std::string> Font::families() {
    std::vector<std::string> out;
    for (const FaceInfo& f : addedFaces()) out.push_back(f.family);
    for (const FaceInfo& f : allFaces()) out.push_back(f.display());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
