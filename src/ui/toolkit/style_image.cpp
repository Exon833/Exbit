// (internal to the toolkit) the images the stylesheets name (style_image.h): files read and decoded once (kept
// by path, read again when the file's time or size changes: a skin reloaded after one of its images was saved
// draws the new one), images made from them at the sizes drawn (an SVG drawn at device pixels, so it is as sharp
// at 150 % as at 100 %; a PNG at its size times the interface size, nearest-neighbour or smooth), kept in an LRU
// cache by everything they depend on; tiled backgrounds' colours (TileShader); and -tiny-glyph (style::glyph).
//
// THREADS: the GUI's, as all drawing; the caches are behind a mutex all the same (a stylesheet may be parsed,
// and its files checked through imageFileOk, on another thread).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>

#include "core/util.h"
#include "gfx/svg.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/style_image.h"

namespace ui {
namespace style {

namespace {

std::mutex& cacheLock() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<const ImageSource>>& sources() {
    static std::map<std::string, std::shared_ptr<const ImageSource>> s;
    return s;
}
struct Tile {
    std::string key;
    std::shared_ptr<const Image> image;
};
struct Tiles {
    std::list<Tile> list;  // the most recently used first
    size_t bytes = 0;
    int made = 0;
};
Tiles& tiles() {
    static Tiles t;
    return t;
}
const size_t kTileBytes = 16u << 20;  // (the images kept at their sizes: what doesn't fit goes, oldest first)
const int kMaxSide = 8192;           // (an image bigger than that across is not made)

std::shared_ptr<const ImageSource> load(const std::string& path, double mtime, int64_t size) {
    auto s = std::make_shared<ImageSource>();
    s->path = path;
    s->mtime = mtime;
    s->size = size;
    std::string data;
    if (size < 0 || !util::readFile(path, data)) {
        s->why = "the file is missing or can't be read";
        return s;
    }
    static const unsigned char kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (data.size() >= 8 && memcmp(data.data(), kPng, 8) == 0) {
        std::string err;
        if (decodePng((const uint8_t*)data.data(), data.size(), s->bitmap, &err) && !s->bitmap.empty()) {
            s->ok = true;
            s->w = (float)s->bitmap.width();
            s->h = (float)s->bitmap.height();
            s->ratio = s->w / s->h;
        } else {
            s->why = "not a PNG that can be read" + (err.empty() ? std::string() : " (" + err + ")");
        }
        return s;
    }
    float w = 0, h = 0, ratio = 0;
    if (svgSize(data, w, h, ratio)) {
        s->ok = true;
        s->svg = true;
        s->svgText = std::move(data);
        s->w = w;
        s->h = h;
        s->ratio = ratio;
        return s;
    }
    s->why = "not a PNG or an SVG image";
    return s;
}

// a premultiplied image scaled to w x h, each pixel the source's nearest (pixel centres)
Image nearest(const Image& src, int w, int h) {
    Image out(w, h);
    int sw = src.width(), sh = src.height();
    std::vector<int> xs((size_t)w);
    for (int x = 0; x < w; x++) xs[(size_t)x] = std::min(sw - 1, (int)(((int64_t)x * 2 + 1) * sw / (2 * (int64_t)w)));
    for (int y = 0; y < h; y++) {
        int sy = std::min(sh - 1, (int)(((int64_t)y * 2 + 1) * sh / (2 * (int64_t)h)));
        const uint32_t* s = src.row(sy);
        uint32_t* d = out.row(y);
        for (int x = 0; x < w; x++) d[x] = s[xs[(size_t)x]];
    }
    return out;
}

char tintKey(Color c) { return c.a ? 't' : 'n'; }

}  // namespace

std::shared_ptr<const ImageSource> imageSource(const std::string& path) {
    double mtime = util::fileMtime(path);
    int64_t size = util::fileSize(path);
    {
        std::lock_guard<std::mutex> lock(cacheLock());
        auto it = sources().find(path);
        if (it != sources().end() && it->second->mtime == mtime && it->second->size == size) return it->second;
    }
    std::shared_ptr<const ImageSource> s = load(path, mtime, size);  // (read without the lock held)
    std::lock_guard<std::mutex> lock(cacheLock());
    sources()[path] = s;
    return s;
}

std::shared_ptr<const Image> imageTile(const ImageSource& src, int w, int h, bool pix, Color tint) {
    if (!src.ok || w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide) return nullptr;
    char num[96];
    snprintf(num, sizeof num, "|%.6f|%lld|%dx%d|%d|%c%02x%02x%02x%02x", src.mtime, (long long)src.size, w, h, (int)pix,
             tintKey(tint), tint.r, tint.g, tint.b, tint.a);
    std::string key = src.path + num;
    Tiles& t = tiles();
    {
        std::lock_guard<std::mutex> lock(cacheLock());
        for (auto it = t.list.begin(); it != t.list.end(); ++it)
            if (it->key == key) {
                if (it != t.list.begin()) t.list.splice(t.list.begin(), t.list, it);
                return t.list.front().image;
            }
    }
    Image img;
    if (src.svg) {
        img = tint.a ? renderSvgTinted(src.svgText, w, h, tint) : renderSvg(src.svgText, w, h);
    } else {
        const Image& b = src.bitmap;
        img = b.width() == w && b.height() == h ? b : pix ? nearest(b, w, h) : b.scaled(w, h);
        if (tint.a) img = img.tinted(tint);
    }
    if (img.empty()) return nullptr;
    auto made = std::make_shared<const Image>(std::move(img));
    std::lock_guard<std::mutex> lock(cacheLock());
    t.made++;
    t.list.push_front(Tile{key, made});
    t.bytes += (size_t)w * h * 4;
    while (t.bytes > kTileBytes && t.list.size() > 1) {
        const Image& old = *t.list.back().image;
        t.bytes -= (size_t)old.width() * old.height() * 4;
        t.list.pop_back();
    }
    return made;
}

bool TileShader::at(int v, int o, float period, int size, bool repeat, int& t) {
    int d = v - o;
    if (!repeat) {
        if (d < 0 || d >= size) return false;
        t = d;
        return true;
    }
    if (period == (float)size) {
        t = d % size;
        if (t < 0) t += size;
        return true;
    }
    // (space: the k-th tile starts at o + k * period, on a whole pixel)
    int k = (int)std::floor((d + 0.5f) / period);
    int start = (int)std::floor(k * period + 0.5f);
    t = d - start;
    return t >= 0 && t < size;
}

void TileShader::shade(int x, int y, int n, uint32_t* out) const {
    int tw = tile_.width(), th = tile_.height();
    int ty;
    if (tw <= 0 || th <= 0 || !at(y, oy_, py_, th, ry_, ty)) {
        std::fill_n(out, n, 0u);
        return;
    }
    const uint32_t* row = tile_.row(ty);
    if (rx_ && px_ == (float)tw) {  // (the usual: tiles side by side, one modulo a row)
        int tx = (x - ox_) % tw;
        if (tx < 0) tx += tw;
        for (int i = 0; i < n; i++) {
            out[i] = row[tx];
            if (++tx == tw) tx = 0;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        int tx;
        out[i] = at(x + i, ox_, px_, tw, rx_, tx) ? row[tx] : 0u;
    }
}

bool pixelated(const css::Style& s) {
    if (!s.has("image-rendering")) return false;
    std::string k = s.keyword("image-rendering", "auto");
    return k == "pixelated" || k == "crisp-edges" || k == "optimizespeed";
}

std::shared_ptr<const GlyphPaint> glyphPaint(const css::Style& s) {
    std::string url = s.url("-tiny-glyph");
    if (url.empty()) return nullptr;
    auto g = std::make_shared<GlyphPaint>();
    g->src = imageSource(url);
    if (!g->src->ok) return nullptr;  // (reported when the stylesheet was read: the control's own glyph)
    // (one or two lengths, each px; em of the node's font)
    auto lengths = [&](const char* property, float& a, float& b, float autoValue) {
        std::vector<std::string> v = util::splitWords(s.raw(property));
        auto len = [&](const std::string& text) {
            if (util::lower(text) == "auto") return autoValue;
            css::Style one;
            one.emPx_ = s.emPx_;
            one.props_["x"] = text;
            return (float)one.length("x", autoValue);
        };
        if (!v.empty()) a = b = len(v[0]);
        if (v.size() > 1) b = len(v[1]);
    };
    if (s.has("-tiny-glyph-size")) {
        lengths("-tiny-glyph-size", g->w, g->h, -1);
        if (g->w < 0 && g->w != -1) g->w = -1;
        if (g->h < 0 && g->h != -1) g->h = -1;
    }
    if (s.has("-tiny-glyph-offset")) lengths("-tiny-glyph-offset", g->dx, g->dy, 0);
    if (s.keyword("-tiny-glyph-tint", "none") == "color") {
        g->tint = foreground(s);
        g->hidden = g->tint.a == 0;  // (in a transparent colour: not seen)
    }
    g->pixelated = pixelated(s);
    return g;
}

bool glyph(Painter& p, const RectF& area, const css::Style& s) {
    const BoxLook& b = boxLook(s);
    if (!b.glyph) return false;
    const GlyphPaint& g = *b.glyph;
    const ImageSource& src = *g.src;
    if (g.hidden) return true;
    // its size (logical px): -tiny-glyph-size, else its own, else as big as fits in the area (its proportions kept)
    float w = g.w, h = g.h;
    float ratio = src.ratio > 0 ? src.ratio : 1;
    if (w < 0 && h < 0) {
        w = src.w;
        h = src.h;
        if (!(w > 0) || !(h > 0)) {
            w = std::min(area.w, area.h * ratio);
            h = w / ratio;
        }
    } else if (w < 0) {
        w = h * ratio;
    } else if (h < 0) {
        h = w / ratio;
    }
    float sc = p.scale();
    int tw = (int)std::lround(w * sc), th = (int)std::lround(h * sc);
    if (tw <= 0 || th <= 0) return true;
    if (!g.tile || g.tw != tw || g.th != th) {
        g.tile = imageTile(src, tw, th, g.pixelated, g.tint);
        g.tw = tw;
        g.th = th;
    }
    if (!g.tile) return true;
    PointF a = p.toDevice(PointF(area.x, area.y));
    int x = (int)std::floor(a.x + (area.w * sc - (float)tw) / 2 + 1e-3f) + (int)std::lround(g.dx * sc);
    int y = (int)std::floor(a.y + (area.h * sc - (float)th) / 2 + 1e-3f) + (int)std::lround(g.dy * sc);
    p.blendLayer(*g.tile, x, y, 1.f);
    return true;
}

bool imageFileOk(const std::string& path, std::string& why) {
    std::shared_ptr<const ImageSource> s = imageSource(path);
    if (!s->ok) why = s->why;
    return s->ok;
}

int imagesMade() {
    std::lock_guard<std::mutex> lock(cacheLock());
    return tiles().made;
}

void clearImageCaches() {
    std::lock_guard<std::mutex> lock(cacheLock());
    sources().clear();
    tiles().list.clear();
    tiles().bytes = 0;
}

}  // namespace style
}  // namespace ui
