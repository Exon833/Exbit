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
#include <cmath>
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

// (added) A colour for every pixel of a fill (gradients): shade() writes the n premultiplied pixels of device
// row y from device column x on (pixel centres at +0.5).
class Shader {
public:
    virtual ~Shader() = default;
    virtual void shade(int x, int y, int n, uint32_t* out) const = 0;
};

// (added) A gradient's colours along its line, worked out once: stops at positions along it (0 its start, 1
// its end; any order of equal positions is a hard stop), interpolated premultiplied (as CSS: a fade to
// transparent stays the colour, not grey). Before the first stop: its colour, after the last: the last's;
// repeating: the stops' span over and over.
struct GradientStop {
    float pos = 0;
    Color color;
};
class GradientRamp {
public:
    GradientRamp() = default;
    GradientRamp(const std::vector<GradientStop>& stops, bool repeating);  // stops in order of position
    uint32_t at(float t) const {
        if (!(t == t)) t = t0_;  // (NaN)
        float u = (t - t0_) * k_;
        if (repeating_) {
            u -= std::floor(u);
        } else {
            if (u < 0) return first_;
            if (u >= 1) return last_;
        }
        int i = (int)(u * kSize);
        return lut_[i < 0 ? 0 : i >= kSize ? kSize - 1 : i];
    }
    // its average over [t - width / 2, t + width / 2] (a pixel's footprint along the gradient): hard stops
    // anti-aliased
    uint32_t average(float t, float width) const;
    bool sharp() const { return sharp_; }  // has steps a pixel may straddle (hard stops): worth average()
    bool empty() const { return lut_.empty(); }
    bool opaque() const { return opaque_; }  // every colour of it
private:
    static constexpr int kSize = 1024;
    float t0_ = 0, k_ = 1;
    bool repeating_ = false, opaque_ = true, sharp_ = false;
    uint32_t first_ = 0, last_ = 0;
    std::vector<uint32_t> lut_;
    std::vector<float> sum_;  // (sharp) prefix sums of the entries, 4 channels each: sum_[4 i + c]
    void integral(float u, float out[4]) const;
};
// a linear gradient in device pixels: from p0 (t = 0) to p1 (t = 1)
class LinearGradientShader : public Shader {
public:
    LinearGradientShader(PointF p0, PointF p1, const GradientRamp& ramp);
    void shade(int x, int y, int n, uint32_t* out) const override;
private:
    const GradientRamp& ramp_;
    float a_ = 0, b_ = 0, c_ = 0;  // t = a x + b y + c
    float foot_ = 0;               // a pixel's width along the gradient (in t)
};
// a radial gradient in device pixels: t = 1 on the ellipse of radii rx, ry around the centre
class RadialGradientShader : public Shader {
public:
    RadialGradientShader(PointF centre, float rx, float ry, const GradientRamp& ramp);
    void shade(int x, int y, int n, uint32_t* out) const override;
private:
    const GradientRamp& ramp_;
    float cx_, cy_, kx_, ky_;
};

// (added) the coverage (0..255) of polygons (device pixels, closed) over `area`, into mask (area.w x area.h)
void coverageMask(const std::vector<std::vector<PointF>>& polys, const Rect& area, std::vector<uint8_t>& mask,
                  bool evenOdd = false);
// (added) a Gaussian blur (standard deviation sigma px) of an 8-bit mask, as three box blurs each way; what is
// beyond its edges counts as its edge pixels
void blurMask(uint8_t* mask, int w, int h, float sigma);

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

    // --- device pixels (added: style::box's borders, shadows and gradients are worked out in them) ---
    PointF toDevice(PointF p) const { return {(p.x + tx_) * scale_, (p.y + ty_) * scale_}; }
    Rect deviceClip() const { return clip_; }
    // closed polygons in device pixels (non-zero winding, or even-odd), in a colour or a shader's colours
    void fillDevice(const std::vector<std::vector<PointF>>& polys, Color c, bool evenOdd = false);
    void fillDevice(const std::vector<std::vector<PointF>>& polys, const Shader& shader, bool evenOdd = false);
    // a rectangle in device pixels (fractional edges: partly covered pixels), in a colour or a shader's
    // colours: spans, no rasterizing
    void fillDeviceRect(const RectF& r, Color c);
    void fillDeviceRect(const RectF& r, const Shader& shader);
    // an 8-bit coverage mask (w x h, stride w) with its top-left at device (x, y), in colour c
    void blendMask(int x, int y, int w, int h, const uint8_t* mask, Color c);
    // a premultiplied image with its top-left at device (x, y), its alpha times `opacity` (a widget drawn into
    // a layer for its opacity)
    void blendLayer(const Image& layer, int x, int y, float opacity);

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
    void rasterize(const std::vector<Path::Poly>& polys, Color c, bool evenOdd);  // device coordinates
    void rasterizeShaded(const std::vector<std::vector<PointF>>& polys, const Shader& s, bool evenOdd);
};
