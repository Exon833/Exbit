// Points, rectangles and colours: the plain types every drawing and widget uses.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

struct Point {
    int x = 0, y = 0;
    Point() = default;
    Point(int x_, int y_) : x(x_), y(y_) {}
    Point operator+(Point o) const { return {x + o.x, y + o.y}; }
    Point operator-(Point o) const { return {x - o.x, y - o.y}; }
    bool operator==(Point o) const { return x == o.x && y == o.y; }
    bool operator!=(Point o) const { return !(*this == o); }
};

struct PointF {
    float x = 0, y = 0;
    PointF() = default;
    PointF(float x_, float y_) : x(x_), y(y_) {}
    PointF(Point p) : x((float)p.x), y((float)p.y) {}
    PointF operator+(PointF o) const { return {x + o.x, y + o.y}; }
    PointF operator-(PointF o) const { return {x - o.x, y - o.y}; }
    PointF operator*(float s) const { return {x * s, y * s}; }
};

// Integer rectangle: x, y, width, height (right() = x + w, exclusive).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
    int left() const { return x; }
    int top() const { return y; }
    int right() const { return x + w; }    // exclusive
    int bottom() const { return y + h; }   // exclusive
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    bool contains(int px, int py) const { return contains(Point(px, py)); }
    Point center() const { return {x + w / 2, y + h / 2}; }
    Rect translated(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
    Rect adjusted(int dx0, int dy0, int dx1, int dy1) const { return {x + dx0, y + dy0, w - dx0 + dx1, h - dy0 + dy1}; }
    Rect intersected(const Rect& o) const {
        int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return x1 > x0 && y1 > y0 ? Rect(x0, y0, x1 - x0, y1 - y0) : Rect();
    }
    bool intersects(const Rect& o) const { return !intersected(o).empty(); }
    Rect united(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
        int x1 = std::max(right(), o.right()), y1 = std::max(bottom(), o.bottom());
        return {x0, y0, x1 - x0, y1 - y0};
    }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    RectF() = default;
    RectF(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    RectF(const Rect& r) : x((float)r.x), y((float)r.y), w((float)r.w), h((float)r.h) {}
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(PointF p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    PointF center() const { return {x + w / 2, y + h / 2}; }
    RectF adjusted(float dx0, float dy0, float dx1, float dy1) const { return {x + dx0, y + dy0, w - dx0 + dx1, h - dy0 + dy1}; }
    RectF translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    RectF intersected(const RectF& o) const {
        float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return x1 > x0 && y1 > y0 ? RectF(x0, y0, x1 - x0, y1 - y0) : RectF();
    }
    Rect toRect() const {  // the pixels it covers (rounded outwards)
        int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        return {x0, y0, (int)std::ceil(right()) - x0, (int)std::ceil(bottom()) - y0};
    }
};

// A straight (not premultiplied) RGBA colour, 0..255 per channel.
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr Color() = default;
    constexpr Color(int r_, int g_, int b_, int a_ = 255)
        : r((uint8_t)r_), g((uint8_t)g_), b((uint8_t)b_), a((uint8_t)a_) {}

    // "#rrggbb" or "#rrggbbaa"; `ok` (if given) says whether it was one. Invalid: opaque black.
    static Color fromHex(const std::string& text, bool* ok = nullptr);
    std::string hex() const;  // "#rrggbb", or "#rrggbbaa" when not opaque (like theme.hex_of)
    std::string hexRgb() const;  // always "#rrggbb"

    Color withAlpha(int alpha) const { return Color(r, g, b, alpha); }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Color& o) const { return !(*this == o); }
    bool valid() const { return true; }

    // Qt-like helpers
    float lightnessF() const;                 // HSL lightness 0..1
    float luminance() const;                  // WCAG relative luminance 0..1
    void toHsv(float& h, float& s, float& v) const;  // h 0..360 (-1 for grey), s, v 0..1
    static Color fromHsv(float h, float s, float v, int alpha = 255);
    void toHsl(float& h, float& s, float& l) const;
    static Color fromHsl(float h, float s, float l, int alpha = 255);
    Color lighter(int percent = 150) const;   // as QColor::lighter
    Color darker(int percent = 200) const;    // as QColor::darker
    // premultiplied 0xAARRGGBB, the pixel format of Image
    uint32_t premul() const {
        uint32_t A = a;
        uint32_t R = (r * A + 127) / 255, G = (g * A + 127) / 255, B = (b * A + 127) / 255;
        return (A << 24) | (R << 16) | (G << 8) | B;
    }
    static Color fromPremul(uint32_t p) {
        int A = (int)(p >> 24);
        if (A == 0) return Color(0, 0, 0, 0);
        auto un = [A](uint32_t c) { return (int)std::min(255u, (c * 255 + A / 2) / (uint32_t)A); };
        return Color(un((p >> 16) & 255), un((p >> 8) & 255), un(p & 255), A);
    }
};

// The colour t of the way from a to b (0: a, 1: b), opaque (theme.mix).
Color mixColor(const Color& a, const Color& b, float t);
// a drawn over b (alpha blending), opaque result
Color blendOver(const Color& top, const Color& under);
// WCAG contrast ratio between two opaque colours (1..21)
float contrastRatio(const Color& a, const Color& b);
