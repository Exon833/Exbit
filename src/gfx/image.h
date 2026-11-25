// Images: premultiplied ARGB32 pixel buffers (what the windows show, cached tiles, icons,
// plugin pictures), and reading/writing them as PNG (Exbit's own inflate/deflate).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/geom.h"

class Image {
public:
    Image() = default;
    Image(int w, int h, Color fill = Color(0, 0, 0, 0));
    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return w_ <= 0 || h_ <= 0; }
    Rect rect() const { return {0, 0, w_, h_}; }
    void resize(int w, int h, Color fill = Color(0, 0, 0, 0));  // contents are lost
    void fill(Color c);
    void fill(const Rect& r, Color c);  // plain replace (no blending), clipped
    uint32_t* row(int y) { return px_.data() + (size_t)y * w_; }
    const uint32_t* row(int y) const { return px_.data() + (size_t)y * w_; }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }
    uint32_t pixel(int x, int y) const { return px_[(size_t)y * w_ + x]; }  // premultiplied 0xAARRGGBB
    void setPixel(int x, int y, uint32_t p) { px_[(size_t)y * w_ + x] = p; }
    Color colorAt(int x, int y) const { return Color::fromPremul(pixel(x, y)); }
    Image copy(const Rect& r) const;            // a part of it
    Image scaled(int w, int h) const;           // smooth (box filter down, bilinear up)
    // every painted pixel becomes `c`, keeping its coverage (alpha): to draw an icon in a skin colour
    Image tinted(Color c) const;
private:
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
};

// PNG (8-bit and 16-bit, grey/RGB/palette, with or without alpha, interlaced or not)
bool loadPng(const std::string& path, Image& out, std::string* error = nullptr);
bool decodePng(const uint8_t* data, size_t size, Image& out, std::string* error = nullptr);
bool savePng(const std::string& path, const Image& img, std::string* error = nullptr);
std::vector<uint8_t> encodePng(const Image& img);

// zlib streams (used by PNG; also handy elsewhere)
bool zlibInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
std::vector<uint8_t> zlibDeflate(const uint8_t* data, size_t size, int level = 6);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
