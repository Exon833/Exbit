// Painter: Tiny DAW's own 2D renderer onto an Image (the stand-in for QPainter).
//
// Everything is anti-aliased (coverage-based scanline rasterizer, non-zero winding) except the
// fast paths for pixel-aligned rectangles and lines. Coordinates are in pixels; a pixel's centre is
// at (x + 0.5, y + 0.5), so fillRect(Rect(0,0,10,1)) fills exactly the top row's first 10 pixels,
// and a 1 px line from (0, 4.5) to (10, 4.5) covers row 4 exactly (as Qt does for a cosmetic pen).
// drawRect(r) with width 1 outlines the outermost pixels of r (inside it).
//
// State (save/restore): translation, clip rectangle, opacity.
//
// Scale (the interface size, Options → Interface size): a Painter made with a scale draws in logical
// pixels onto a target `scale` times bigger. Every coordinate, size, line width, font size and
// image is multiplied by it; pixel-aligned rectangles stay pixel-aligned (their edges are rounded to
// device pixels, so neighbours still meet exactly). At scale 1 nothing differs from a plain Painter.
#pragma once
#include <string>
#include <vector>

#include "gfx/font.h"
#include "gfx/geom.h"
#include "gfx/image.h"

enum Align {
    AlignLeft = 0x1,
    AlignRight = 0x2,
    AlignHCenter = 0x4,
    AlignTop = 0x20,
    AlignBottom = 0x40,
    AlignVCenter = 0x80,
    AlignCenter = AlignHCenter | AlignVCenter,
};

// A vector path (moveTo / lineTo / curves). Curves are flattened when filled or stroked.
class Path {
public:
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void arcTo(float rx, float ry, float xAxisRotationDeg, bool largeArc, bool sweep, float x, float y);  // SVG arc
    void close();
    void addRect(const RectF& r);
    void addRoundedRect(const RectF& r, float rx, float ry);
    void addEllipse(const RectF& r);
    void addPolygon(const std::vector<PointF>& pts, bool closed = true);
    bool empty() const { return subpaths_.empty(); }
    PointF current() const { return cur_; }
    // polylines (each one subpath; `closed` flags); tolerance in px
    struct Poly { std::vector<PointF> pts; bool closed = false; };
    std::vector<Poly> flatten(float tolerance = 0.25f) const;
    void transform(float a, float b, float c, float d, float e, float f);  // x' = a x + c y + e, y' = b x + d y + f
    RectF bounds() const;
private:
    struct Seg { int kind; PointF p[3]; };  // 0 line, 1 quad, 2 cubic (p = control points…, end)
    struct Sub { PointF start; std::vector<Seg> segs; bool closed = false; };
    std::vector<Sub> subpaths_;
    PointF cur_, start_;
};

enum class LineCap { Flat, Square, Round };
enum class LineJoin { Miter, Bevel, Round };

class Painter {
public:
    explicit Painter(Image& target);
    Painter(Image& target, float scale);  // (added) logical pixels × scale = the target's pixels
    Image& target() { return *img_; }
    float scale() const { return scale_; }  // (added) 1 unless made with a scale

    // --- state ---
    void save();
    void restore();
    void translate(float dx, float dy);
    PointF translation() const { return {tx_, ty_}; }
    void setClip(const Rect& r);       // intersected with the current clip (r in current coordinates)
    void resetClip();                  // the whole image
    Rect clip() const;                 // current clip, in current coordinates
    void setOpacity(float o);          // multiplies every colour's alpha (0..1), relative to the saved one
    float opacity() const { return opacity_; }

    // --- filling ---
    void fillRect(const Rect& r, Color c);    // fast, pixel-aligned
    void fillRect(const RectF& r, Color c);   // anti-aliased edges
    void fillRoundedRect(const RectF& r, float radius, Color c);
    void fillEllipse(const RectF& r, Color c);
    void fillPolygon(const std::vector<PointF>& pts, Color c);
    void fillPath(const Path& p, Color c);
    void fillPath(const Path& p, Color c, bool evenOdd);  // (added) evenOdd: SVG fill-rule="evenodd"
    // a vertical gradient is never used by the flat skins; not provided

    // --- outlines ---
    void drawRect(const Rect& r, Color c, int width = 1);   // inside r
    void strokeRect(const RectF& r, Color c, float width = 1);  // centred on r's edges
    void strokeRoundedRect(const RectF& r, float radius, Color c, float width = 1);  // centred on the edges
    void strokeEllipse(const RectF& r, Color c, float width = 1);
    void drawLine(PointF a, PointF b, Color c, float width = 1, LineCap cap = LineCap::Flat);
    void drawHLine(int x0, int x1, int y, Color c);  // pixels x0..x1-1 of row y (fast)
    void drawVLine(int x, int y0, int y1, Color c);  // pixels y0..y1-1 of column x (fast)
    void drawPolyline(const std::vector<PointF>& pts, Color c, float width = 1,
                      LineCap cap = LineCap::Flat, LineJoin join = LineJoin::Miter);
    void strokePath(const Path& p, Color c, float width = 1, LineCap cap = LineCap::Flat,
                    LineJoin join = LineJoin::Miter);
    void drawDashedLine(PointF a, PointF b, Color c, float width, float dash, float gap);

    // --- images ---
    void drawImage(Point at, const Image& img);                       // 1:1, blended
    void drawImage(const RectF& dst, const Image& img);               // scaled (bilinear) to dst
    void drawImage(const RectF& dst, const Image& img, const Rect& src);

    // --- text (UTF-8) ---
    // baseline starts at `at`; returns the advance width
    float drawText(PointF at, const std::string& text, const Font& font, Color c);
    // in a box: alignment flags (Align…); elide: cut with "…" to fit the width
    void drawText(const RectF& box, int align, const std::string& text, const Font& font, Color c,
                  bool elide = false);
    // several lines (split at '\n', and wrapped at spaces when `wrap`), aligned in the box
    void drawTextBlock(const RectF& box, int align, const std::string& text, const Font& font, Color c,
                       bool wrap = true);

    // pixel blending with the current clip/opacity (used by the rasterizer and by callers that
    // write coverage directly, e.g. waveform tiles). x, y in current coordinates (the translation,
    // rounded to whole pixels, is added); premulColor is premultiplied 0xAARRGGBB.
    void blendSpan(int x, int y, int len, uint32_t premulColor);
    void blendPixel(int x, int y, uint32_t premulColor, int coverage /*0..255*/);

private:
    struct State { float tx, ty; Rect clip; float opacity; };
    struct Unscaled;  // (painter.cpp) device coordinates for the duration of a call
    Image* img_;
    float scale_ = 1;
    Rect devRect(const Rect& r) const;  // a rectangle in current (logical) coordinates, in device pixels
    void toDevice(std::vector<Path::Poly>& polys) const;  // translated and scaled
    float tx_ = 0, ty_ = 0;
    Rect clip_;
    float opacity_ = 1;
    std::vector<State> stack_;
    void rasterize(const std::vector<Path::Poly>& polys, Color c);  // device coordinates
    void rasterize(const std::vector<Path::Poly>& polys, Color c, bool evenOdd);
};
