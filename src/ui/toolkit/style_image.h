// (internal to the toolkit) the image files the stylesheets name in url()s (background-image layers, -tiny-glyph),
// read and decoded once, and drawn at the sizes asked for, kept (style_image.cpp).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "core/css.h"
#include "gfx/image.h"
#include "gfx/painter.h"

namespace ui {
namespace style {

// a PNG or an SVG file, read and decoded
struct ImageSource {
    std::string path;
    double mtime = 0;  // (when it was read: read again when its time or size changes)
    int64_t size = -1;
    bool ok = false;
    std::string why;  // (not ok) what is wrong with it
    bool svg = false;
    std::string svgText;  // (svg)
    Image bitmap;         // (PNG)
    float w = 0, h = 0;   // its own size in px (an SVG's width / height; 0: not given)
    float ratio = 0;      // its proportions, width / height (0: none)
};
// the file, read (kept while its time and size stay the same); never null
std::shared_ptr<const ImageSource> imageSource(const std::string& path);
// it at w x h device pixels: an SVG drawn at that size, a PNG scaled to it (nearest-neighbour when `pixelated`,
// else smooth; as it is at its own size), every painted pixel in `tint` when tint.a > 0 (its coverage kept). Kept
// by (file, its time, size, pixelated, tint) in a cache of a few MB. Null if it can't be made.
std::shared_ptr<const Image> imageTile(const ImageSource& src, int w, int h, bool pixelated, Color tint);

// The colours of a tiled image, in device pixels: a tile's top-left at (ox, oy), the next one every periodX /
// periodY px (wider than the tile: gaps, CSS's `space`), along x / y only when repeatX / repeatY (else one).
class TileShader : public Shader {
public:
    TileShader(const Image& tile, int ox, int oy, float periodX, float periodY, bool repeatX, bool repeatY)
        : tile_(tile), ox_(ox), oy_(oy), px_(periodX), py_(periodY), rx_(repeatX), ry_(repeatY) {}
    void shade(int x, int y, int n, uint32_t* out) const override;
private:
    // the tile's pixel along one axis at device v (false: none there)
    static bool at(int v, int o, float period, int size, bool repeat, int& t);
    const Image& tile_;
    int ox_, oy_;
    float px_, py_;
    bool rx_, ry_;
};

// -tiny-glyph: the image, how big and where (style::glyph)
struct GlyphPaint {
    std::shared_ptr<const ImageSource> src;
    float w = -1, h = -1;    // -tiny-glyph-size (px; -1: auto)
    float dx = 0, dy = 0;    // -tiny-glyph-offset (px)
    Color tint = Color(0, 0, 0, 0);  // (a = 0: as it is)
    bool hidden = false;     // (tinted in a transparent colour)
    bool pixelated = false;  // image-rendering: pixelated / crisp-edges
    // (the image at the size last drawn)
    mutable int tw = -1, th = -1;
    mutable std::shared_ptr<const Image> tile;
};
// is image-rendering pixelated (or crisp-edges, optimizeSpeed)?
bool pixelated(const css::Style& s);

}  // namespace style
}  // namespace ui
