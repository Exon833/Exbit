#include "ui/waveform_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config.h"

namespace waveform_cache {

std::pair<int, int> tileColumns(int index, double dpr) {
    // rounding each boundary (rather than each tile's width) makes neighbouring tiles meet exactly
    if (dpr == 1.0) return {index * TILE_W, (index + 1) * TILE_W};
    return {(int)std::nearbyint(index * TILE_W * dpr), (int)std::nearbyint((index + 1) * TILE_W * dpr)};
}

Mask renderAudioTile(const Clip& clip, int firstCol, int width, int height, double pps, int sr, double dpr) {
    Mask mask((size_t)width * height, 0);
    const Peaks* peaks = clip.peaks.get();
    if (!peaks || peaks->size() == 0 || width <= 0 || height <= 0) return mask;
    const int n = (int)peaks->size();
    double blocksPerPx = sr / pps / config::PEAK_BLOCK;
    double firstBlock = (double)clip.offset / config::PEAK_BLOCK;  // a split clip starts partway into the audio
    // the block index at each column edge (width + 1 of them), clipped to the peaks
    std::vector<int> idx(width + 1);
    for (int i = 0; i <= width; ++i) {
        double v = (firstCol + i) * blocksPerPx + firstBlock;
        int k = (int)v;  // (numpy astype(int): towards zero)
        idx[i] = std::min(std::max(k, 0), n - 1);
    }
    double mid = height / 2.0, half = height / 2.0 - 3 * dpr;  // (a 3 logical-pixel margin above and below)
    const float* mins = peaks->mins.data();
    const float* maxs = peaks->maxs.data();
    const bool levels = clip.hasLevels();  // (its gain and fades shape the waveform)
    for (int c = 0; c < width; ++c) {
        // np.minimum.reduceat: blocks idx[c] up to the next column's first (at least one)
        int a = idx[c], b = std::max(idx[c + 1], a + 1);
        float lo = mins[a], hi = maxs[a];
        for (int k = a + 1; k < b; ++k) {
            lo = std::min(lo, mins[k]);
            hi = std::max(hi, maxs[k]);
        }
        if (levels) {
            float level = clip.level((int64_t)((double)(firstCol + c) * sr / pps));
            lo = std::min(1.0f, std::max(-1.0f, lo * level));
            hi = std::min(1.0f, std::max(-1.0f, hi * level));
        }
        double top = std::floor(mid - hi * half);
        double bottom = std::max(top, std::floor(mid - lo * half));
        int r0 = (int)std::max(0.0, top), r1 = (int)std::min((double)height - 1, bottom);
        for (int r = r0; r <= r1; ++r) mask[(size_t)r * width + c] = 1;
    }
    return mask;
}

Mask renderMidiTile(const Clip& clip, int firstCol, int width, int height, double pps, int sr, double dpr) {
    Mask mask((size_t)std::max(0, width) * std::max(0, height), 0);
    auto shown = clip.played ? clip.played : clip.notes;  // (repeated, when it loops)
    if (!shown || width <= 0 || height <= 0) return mask;
    double windowS = (double)clip.offset / sr, framesS = (double)clip.frames() / sr;
    std::vector<const Note*> notes;
    for (auto& n : *shown)
        if (n.end > windowS && n.start < windowS + framesS) notes.push_back(&n);
    if (notes.empty()) return mask;
    double lo = notes[0]->pitch, hi = notes[0]->pitch;
    for (auto* n : notes) {
        lo = std::min(lo, n->pitch);
        hi = std::max(hi, n->pitch);
    }
    if (hi - lo < 12) {
        double m = (lo + hi) / 2;
        lo = m - 6;
        hi = m + 6;
    }
    const int areaTop = (int)std::nearbyint(2 * dpr), areaBottom = height - (int)std::nearbyint(3 * dpr);
    double rowH = (areaBottom - areaTop) / (hi - lo + 1);
    for (auto* n : notes) {
        double a = (n->start - windowS) * pps - firstCol;
        double b = std::max((n->end - windowS) * pps - firstCol, a + dpr);
        if (!(b > 0 && a < width)) continue;
        double y = areaBottom - (n->pitch - lo + 1) * rowH;
        int r0 = std::max(areaTop, (int)y);
        int r1 = std::min(areaBottom, (int)std::ceil(y + std::max(2.0 * dpr, rowH - dpr)));
        int c0 = std::max(0, (int)a), c1 = std::min(width, (int)std::ceil(b));
        for (int r = std::max(0, r0); r < std::min(height, r1); ++r)
            if (c1 > c0) std::memset(&mask[(size_t)r * width + c0], 1, (size_t)(c1 - c0));
    }
    return mask;
}

bool TileCache::Key::operator==(const Key& o) const {
    return kind == o.kind && source == o.source && offset == o.offset && frames == o.frames && gainDb == o.gainDb &&
           fadeIn == o.fadeIn && fadeOut == o.fadeOut && fadeLead == o.fadeLead && fadeTail == o.fadeTail &&
           fadeShapes == o.fadeShapes && pps == o.pps && dpr == o.dpr && sr == o.sr && height == o.height &&
           color == o.color && index == o.index && width == o.width && envelope == o.envelope;
}

size_t TileCache::KeyHash::operator()(const Key& k) const {
    size_t h = std::hash<const void*>()(k.source);
    auto mixIn = [&h](uint64_t v) { h ^= std::hash<uint64_t>()(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    auto bits = [](double d) {
        uint64_t b;
        std::memcpy(&b, &d, sizeof b);
        return b;
    };
    uint64_t ppsBits = bits(k.pps);
    mixIn(bits(k.dpr));
    mixIn(bits(k.gainDb));
    mixIn((uint64_t)k.fadeIn ^ ((uint64_t)k.fadeOut << 21) ^ ((uint64_t)k.fadeLead << 42) ^ ((uint64_t)k.fadeTail << 7));
    mixIn((uint64_t)k.kind ^ ((uint64_t)k.fadeShapes << 8));
    mixIn((uint64_t)k.offset);
    mixIn((uint64_t)k.frames);
    mixIn(ppsBits);
    mixIn((uint64_t)k.sr);
    mixIn((uint64_t)k.height);
    mixIn(k.color);
    mixIn((uint64_t)k.index);
    mixIn((uint64_t)k.width);
    mixIn((uint64_t)(uintptr_t)k.envelope);
    return h;
}

TileCache::TileCache(size_t budgetBytes) : budget(budgetBytes) {}

const Image& TileCache::tile(const Clip& clip, int index, int width, int height, double pps, int sr, Color color,
                             double dpr) {
    std::shared_ptr<const void> source;
    if (clip.kind == Kind::Audio) source = clip.peaks;
    else if (clip.played) source = clip.played;
    else source = clip.notes;
    Key key{(int)clip.kind, source.get(), clip.offset, clip.frames(), clip.gainDb, clip.fadeIn, clip.fadeOut,
            clip.fadeLead, clip.fadeTail, (int)clip.fadeInShape + 8 * (int)clip.fadeOutShape, pps, dpr, sr, height, Color(color.r, color.g, color.b, 255).premul(), index, width, clip.gainEnvelope.get()};
    auto found = index_.find(key);
    if (found != index_.end()) {
        entries_.splice(entries_.begin(), entries_, found->second);  // most recently used
        return found->second->image;
    }
    int first = tileColumns(index, dpr).first;
    Mask mask = clip.kind == Kind::Audio ? renderAudioTile(clip, first, width, height, pps * dpr, sr, dpr)
                                         : renderMidiTile(clip, first, width, height, pps * dpr, sr, dpr);
    Image image(std::max(1, width), std::max(1, height));
    uint32_t px = key.color;  // opaque, premultiplied
    for (int y = 0; y < height; ++y) {
        uint32_t* row = image.row(y);
        const uint8_t* m = &mask[(size_t)y * width];
        for (int x = 0; x < width; ++x)
            if (m[x]) row[x] = px;
    }
    entries_.push_front(Entry{key, std::move(source), clip.gainEnvelope, std::move(image)});
    index_[key] = entries_.begin();
    bytes_ += (size_t)std::max(1, width) * std::max(1, height) * 4;
    while (bytes_ > budget && entries_.size() > 1) {
        Entry& old = entries_.back();
        bytes_ -= (size_t)old.image.width() * old.image.height() * 4;
        index_.erase(old.key);
        entries_.pop_back();
    }
    return entries_.front().image;
}

void TileCache::clear() {
    entries_.clear();
    index_.clear();
    bytes_ = 0;
}

TileCache& tiles() {
    static TileCache cache;
    return cache;
}

}  // namespace waveform_cache
