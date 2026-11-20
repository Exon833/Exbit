// Ready-made images of clip contents (waveforms and MIDI notes), so redraws only copy pixels
// (waveform_cache.py).
//
// A clip's body is cut into tiles TILE_W (logical) pixels wide, counted from the clip's left edge.
// Each tile is rendered once straight into an image and kept in an LRU cache (about CACHE_MB). The
// cache key holds everything that changes the pixels: the clip's audio or notes (as they play: a
// loop's repeats, after the MIDI effects, without switched-off notes), its window (offset/length),
// its gain and fades and their shapes (the waveform is drawn as it plays), zoom, sample rate, lane
// height and colour.
// So zooming, cutting, recolouring, muting or resizing a lane simply asks for new tiles; moving a
// clip reuses its tiles.
//
// Tiles are rendered at the screen's own resolution (device pixels: at a 1.25x or 2x interface size
// a tile has 1.25x / 2x the pixels of its logical size), so waveforms and notes stay sharp. Tile i
// covers the device columns round(i * TILE_W * dpr) up to round((i + 1) * TILE_W * dpr) from the
// clip's left device pixel (tileColumns), so neighbouring tiles meet exactly at any scale. The
// scale is part of the cache key.
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gfx/image.h"
#include "model/model.h"

namespace waveform_cache {

constexpr int TILE_W = 256;
constexpr int CACHE_MB = 96;

// (height x width) coverage: true where the waveform / a note is
using Mask = std::vector<uint8_t>;
// (first, end) device columns of tile `index`, from the clip's left device pixel
std::pair<int, int> tileColumns(int index, double dpr);
// one vertical min..max bar per pixel column (shaped by the clip's gain and fades). firstCol, width,
// height and pps in device pixels; dpr scales the margins.
Mask renderAudioTile(const Clip& clip, int firstCol, int width, int height, double pps, int sr, double dpr = 1.0);
// mini piano roll of the notes inside the clip's window (as they play: Clip::played when there); their
// pitch range fills the height (at least an octave). A note is at least 1 logical pixel long, 2 high.
Mask renderMidiTile(const Clip& clip, int firstCol, int width, int height, double pps, int sr, double dpr = 1.0);

class TileCache {
public:
    explicit TileCache(size_t budgetBytes = (size_t)CACHE_MB << 20);
    // the image of tile `index` (width x height device pixels, starting at device column
    // tileColumns(index, dpr).first of the clip) of the clip's contents in `color`; valid until the next
    // call. pps: logical pixels per second; dpr: device pixels per logical pixel.
    const Image& tile(const Clip& clip, int index, int width, int height, double pps, int sr, Color color,
                      double dpr = 1.0);
    void clear();
    size_t bytes() const { return bytes_; }
    size_t count() const { return entries_.size(); }
    size_t budget;

private:
    struct Key {
        int kind;
        const void* source;
        int64_t offset, frames;
        double gainDb;
        int64_t fadeIn, fadeOut, fadeLead, fadeTail;
        int fadeShapes;  // (fade-in shape + 8 × fade-out shape: model.h FADE SHAPES)
        double pps, dpr;
        int sr, height;
        uint32_t color;
        int index, width;
        const void* envelope;  // (its gain envelope: clip_envelope.h)
        bool operator==(const Key& o) const;
    };
    struct KeyHash { size_t operator()(const Key& k) const; };
    struct Entry {
        Key key;
        std::shared_ptr<const void> source;  // kept alive: a recycled address must not return a stale tile
        std::shared_ptr<const void> envelope;  // (the same)
        Image image;
    };
    std::list<Entry> entries_;  // most recently used first
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    size_t bytes_ = 0;
};

TileCache& tiles();  // shared by every clip drawing (TILES)

}  // namespace waveform_cache
