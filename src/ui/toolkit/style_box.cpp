// (internal to the toolkit) the boxes of style.h beyond plain ones, as CSS draws them: per-side border widths,
// colours and styles, four corner radii, gradient backgrounds, outer and inset box shadows; and text-shadow.
//
// Everything is worked out in device pixels (the painter's scale and translation applied), so shadows and
// borders are as sharp at 150 % as at 100 %. The order is CSS's: the outer shadows (the last first), the
// background colour and its gradients (the last first) under the border, the inset shadows over them, then
// the border.
//
// THE BORDER is drawn as rings, one per band of the border's width (a double border has two, groove / ridge
// and 2 px inset / outset two): each ring is the region between two rounded rectangles, filled once (exact
// area anti-aliasing), each pixel taking its side's colour: the sides meet on the line from the border box's
// corner to the padding box's corner, anti-aliased across it, so there are no seams. Boxes on whole pixels
// with square corners are drawn sharp: no anti-aliasing at all, a corner's pixels going to one side or the
// other by that line (on the line: to the top at the top-left, to the right at the top-right, to the bottom
// at the bottom corners, as the classic bevelled look has them).
//
// INSET / OUTSET / GROOVE / RIDGE take their shades from the side's colour as Chrome does (a darker shade:
// the colour's brightest channel less 0.33; a lighter one: plus 0.33, each kept if the colour is already
// near black / near white): outset's top and left lighter, bottom and right darker (inset the other way).
// With 2 px and more, the outer and inner halves differ: the classic four-tone bevel, the colour itself the
// inner light (outset: lighter / the colour / darker / darker still, from the outer top-left to the outer
// bottom-right). Groove: the outer half as inset, the inner half as outset; ridge the other way. Double:
// the outer and inner thirds (3 px and more; thinner: solid). Dashed: dashes and gaps of three times the
// width, spaced to end with a dash at each corner; dotted: dots of the width (round from 3 px, square below),
// one width apart. Sides with rounded corners have their dashes and dots follow the corners.
//
// GRADIENTS are laid on the padding box (background-origin's default) and clipped to the border box (or to
// just under the border when it is opaque, so no background shows at its outer edge); beyond the padding box
// they keep their end colours (no tiling).
//
// SHADOWS: the blur is a Gaussian (standard deviation half the blur radius, as CSS) made of three box blurs
// each way on an 8-bit mask. An outer shadow is the box grown by its spread, its corners' radii grown as CSS
// says, offset, blurred, and cut out where the box is (it never shows through a see-through box); an inset
// shadow is drawn inside the padding box, around a hole the padding box's shape shrunk by the spread and
// offset. The finished masks are kept in a small LRU cache by everything they depend on (sizes, radii,
// blur, spread, offset, the box's position to 1/16 px), so a repaint draws a kept mask: no rasterizing,
// no blurring.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

#include "ui/toolkit/style.h"

namespace ui {
namespace style {

enum class Line : uint8_t { Solid, Dashed, Dotted, Double, Groove, Ridge, Inset, Outset };

struct BoxPaint {
    float width[4] = {0, 0, 0, 0};  // top right bottom left (logical px)
    Color color[4];
    Line line[4] = {Line::Solid, Line::Solid, Line::Solid, Line::Solid};
    float radius[4] = {0, 0, 0, 0};  // top-left top-right bottom-right bottom-left (before Options → Rounded corners)
    Color bg;
    // the bands of each side's width: from f0 to f1 (fractions of it), in a colour
    struct Band {
        float f0, f1;
        Color c;
    };
    std::vector<Band> bands[4];
    std::vector<float> breaks;  // every band's ends, sorted, once each
    bool opaqueSide[4] = {false, false, false, false};  // all of its width one opaque band (or bands)
    struct Layer {
        css::Style::Gradient g;
        std::shared_ptr<GradientRamp> ramp;  // made once when its stops don't depend on the box's size
    };
    std::vector<Layer> layers;                // background-image's, the first on top
    std::vector<css::Style::Shadow> shadows;  // box-shadow's, the first on top
};

namespace {

const float kPi = 3.14159265358979f;

// --- shades for inset / outset / groove / ridge (Chrome's Color::Dark / Light and their thresholds) ---
int diffSquared(Color a, int r, int g, int b) {
    int dr = a.r - r, dg = a.g - g, db = a.b - b;
    return dr * dr + dg * dg + db * db;
}
Color darker(Color c) {
    if (diffSquared(c, 0, 0, 0) <= diffSquared(Color(0x20, 0x20, 0x20), 0, 0, 0)) return c;  // (dark enough)
    float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f, v = std::max(r, std::max(g, b));
    float k = std::max(0.f, (v - 0.33f) / v);
    auto ch = [k](float x) { return std::clamp((int)std::lround(x * k * 255), 0, 255); };
    return Color(ch(r), ch(g), ch(b), c.a);
}
Color lighter(Color c) {
    if (diffSquared(c, 255, 255, 255) <= diffSquared(Color(0xeb, 0xeb, 0xeb), 255, 255, 255)) return c;  // (light enough)
    float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f, v = std::max(r, std::max(g, b));
    if (v <= 0) return Color(0x54, 0x54, 0x54, c.a);
    float k = std::min(1.f, v + 0.33f) / v;
    auto ch = [k](float x) { return std::clamp((int)std::lround(x * k * 255), 0, 255); };
    return Color(ch(r), ch(g), ch(b), c.a);
}

Line lineOf(const std::string& k) {
    if (k == "dashed") return Line::Dashed;
    if (k == "dotted") return Line::Dotted;
    if (k == "double") return Line::Double;
    if (k == "groove") return Line::Groove;
    if (k == "ridge") return Line::Ridge;
    if (k == "inset") return Line::Inset;
    if (k == "outset") return Line::Outset;
    return Line::Solid;  // (solid, and not set: the program's boxes had a solid border before border-style)
}

// a side's bands (side: 0 top, 1 right, 2 bottom, 3 left)
void makeBands(BoxPaint& b, int side) {
    std::vector<BoxPaint::Band>& v = b.bands[side];
    float w = b.width[side];
    Color c = b.color[side];
    if (w <= 0 || c.a == 0) return;
    bool topLeft = side == 0 || side == 3;
    switch (b.line[side]) {
        case Line::Solid: v.push_back({0, 1, c}); break;
        case Line::Dashed:
        case Line::Dotted: break;  // (drawn apart: not rings)
        case Line::Double:
            if (w >= 3) {
                v.push_back({0, 1.f / 3, c});
                v.push_back({2.f / 3, 1, c});
            } else {
                v.push_back({0, 1, c});
            }
            break;
        case Line::Inset:
        case Line::Outset: {
            bool outset = b.line[side] == Line::Outset;
            if (w >= 2) {
                // the four tones, from the outer top-left to the outer bottom-right:
                // outset: lighter, the colour | darker, darker still; inset: darker, darker still | the colour, lighter
                Color outer, inner;
                if (outset) {
                    outer = topLeft ? lighter(c) : darker(darker(c));
                    inner = topLeft ? c : darker(c);
                } else {
                    outer = topLeft ? darker(c) : lighter(c);
                    inner = topLeft ? darker(darker(c)) : c;
                }
                v.push_back({0, 0.5f, outer});
                v.push_back({0.5f, 1, inner});
            } else {
                v.push_back({0, 1, topLeft == outset ? lighter(c) : darker(c)});
            }
            break;
        }
        case Line::Groove:
        case Line::Ridge: {
            bool darkOutside = topLeft == (b.line[side] == Line::Groove);
            v.push_back({0, 0.5f, darkOutside ? darker(c) : lighter(c)});
            v.push_back({0.5f, 1, darkOutside ? lighter(c) : darker(c)});
            break;
        }
    }
    float covered = 0;
    bool opaque = !v.empty();
    for (const BoxPaint::Band& band : v) {
        opaque = opaque && band.c.a == 255 && band.f0 <= covered + 1e-6f;
        covered = std::max(covered, band.f1);
    }
    b.opaqueSide[side] = opaque && covered >= 1 - 1e-6f;
}

// --- geometry, in device pixels ---
struct RoundRect {
    float L = 0, T = 0, R = 0, B = 0;
    float rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0};  // top-left top-right bottom-right bottom-left
    bool empty() const { return !(R - L > 0) || !(B - T > 0); }
    bool square() const {
        for (int i = 0; i < 4; i++)
            if (rx[i] > 0 && ry[i] > 0) return false;
        return true;
    }
    // shrunk by d (top right bottom left), its radii by as much (CSS's inner border edge)
    RoundRect inset(const float d[4]) const {
        RoundRect r;
        r.L = L + d[3];
        r.T = T + d[0];
        r.R = R - d[1];
        r.B = B - d[2];
        const int hx[4] = {3, 1, 1, 3}, vy[4] = {0, 0, 2, 2};  // each corner's sides (horizontal radius: left/right)
        for (int i = 0; i < 4; i++) {
            r.rx[i] = std::max(0.f, rx[i] - d[hx[i]]);
            r.ry[i] = std::max(0.f, ry[i] - d[vy[i]]);
            if (r.rx[i] <= 0 || r.ry[i] <= 0) r.rx[i] = r.ry[i] = 0;
        }
        return r;
    }
    // moved and grown by `spread` (negative: shrunk), its radii as CSS grows a shadow's
    RoundRect shadow(float dx, float dy, float spread) const {
        RoundRect r;
        r.L = L + dx - spread;
        r.T = T + dy - spread;
        r.R = R + dx + spread;
        r.B = B + dy + spread;
        auto grow = [spread](float v) {
            if (spread < 0) return std::max(0.f, v + spread);
            if (v <= 0 || spread == 0) return v;
            float ratio = v / spread;
            return ratio >= 1 ? v + spread : v + spread * (1 + (ratio - 1) * (ratio - 1) * (ratio - 1));
        };
        for (int i = 0; i < 4; i++) {
            r.rx[i] = grow(rx[i]);
            r.ry[i] = grow(ry[i]);
            if (r.rx[i] <= 0 || r.ry[i] <= 0) r.rx[i] = r.ry[i] = 0;
        }
        return r;
    }
    // its outline, clockwise (screen coordinates); square corners are one point
    std::vector<PointF> polygon() const {
        std::vector<PointF> out;
        if (empty()) return out;
        // (radii that don't fit are scaled down together, as CSS does)
        float f = 1, w = R - L, h = B - T;
        auto fit = [&](float len, float a, float b) {
            if (a + b > len && a + b > 0) f = std::min(f, len / (a + b));
        };
        fit(w, rx[0], rx[1]);
        fit(w, rx[3], rx[2]);
        fit(h, ry[0], ry[3]);
        fit(h, ry[1], ry[2]);
        const float cxs[4] = {L, R, R, L}, cys[4] = {T, T, B, B}, sx[4] = {1, -1, -1, 1}, sy[4] = {1, 1, -1, -1};
        // each corner's quarter ellipse from (cos, sin) of its start angle, turned step by step (no trigonometry
        // per point): within 0.04 px of the true curve
        const float c0[4] = {-1, 0, 1, 0}, s0[4] = {0, -1, 0, 1};
        out.reserve(64);
        for (int i = 0; i < 4; i++) {
            float ax = rx[i] * f, ay = ry[i] * f;
            if (ax <= 0 || ay <= 0) {
                out.push_back(PointF(cxs[i], cys[i]));
                continue;
            }
            float cx = cxs[i] + sx[i] * ax, cy = cys[i] + sy[i] * ay, r = std::max(ax, ay);
            int n = std::clamp((int)std::ceil(2.78f * std::sqrt(r)), 2, 128);
            float d = 0.5f * kPi / n, cd = std::cos(d), sd = std::sin(d);
            float c = c0[i], s = s0[i];
            for (int k = 0; k <= n; k++) {
                out.push_back(PointF(cx + ax * c, cy + ay * s));
                float nc = c * cd - s * sd;
                s = s * cd + c * sd;
                c = nc;
            }
            out.back() = PointF(cx + ax * (i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 0 : -1),
                                cy + ay * (i == 0 ? -1 : i == 1 ? 0 : i == 2 ? 1 : 0));  // (exactly on the side)
        }
        return out;
    }
};

std::vector<PointF> reversed(std::vector<PointF> v) {
    std::reverse(v.begin(), v.end());
    return v;
}

// a colour or a shader's colours
struct Fill {
    Color c;
    const Shader* shader = nullptr;
};
void fillPolys(Painter& p, const std::vector<std::vector<PointF>>& polys, const Fill& f) {
    if (f.shader) p.fillDevice(polys, *f.shader);
    else p.fillDevice(polys, f.c);
}
void fillRect(Painter& p, const RectF& r, const Fill& f) {
    if (f.shader) p.fillDeviceRect(r, *f.shader);
    else p.fillDeviceRect(r, f.c);
}
// the part of a polygon above y = c (keep: y <= c), or below it (keep: y >= c)
std::vector<PointF> clipAt(const std::vector<PointF>& in, float c, bool above) {
    std::vector<PointF> out;
    size_t n = in.size();
    auto inside = [&](PointF q) { return above ? q.y <= c : q.y >= c; };
    for (size_t i = 0; i < n; i++) {
        PointF a = in[i], b = in[(i + 1) % n];
        bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            float t = (c - a.y) / (b.y - a.y);
            out.push_back(PointF(a.x + (b.x - a.x) * t, c));
        }
    }
    return out;
}
// rows [y0, y1) between a shape's (or two shapes') corners, where they are straight-sided: drawn as spans
bool straightRows(const RoundRect& o, float& y0, float& y1) {
    y0 = std::ceil(o.T + std::max(o.ry[0], o.ry[1]));
    y1 = std::floor(o.B - std::max(o.ry[2], o.ry[3]));
    return y1 - y0 >= 2;
}

// a rounded rectangle filled: its corners' rows rasterized, the rows between as spans
void fillShape(Painter& p, const RoundRect& o, const Fill& f) {
    if (o.empty()) return;
    float y0, y1;
    if (!straightRows(o, y0, y1)) {
        fillPolys(p, {o.polygon()}, f);
        return;
    }
    std::vector<PointF> poly = o.polygon();
    if (y0 > o.T) fillPolys(p, {clipAt(poly, y0, true)}, f);
    fillRect(p, RectF(o.L, y0, o.R - o.L, y1 - y0), f);
    if (y1 < o.B) fillPolys(p, {clipAt(poly, y1, false)}, f);
}

// the ring between two rounded rectangles (inner inside outer): the same, its sides' strips as spans
void fillRing(Painter& p, const RoundRect& outer, const RoundRect& inner, const Fill& f) {
    if (outer.empty()) return;
    if (inner.empty()) {
        fillShape(p, outer, f);
        return;
    }
    float a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    bool straight = straightRows(outer, a0, a1) && straightRows(inner, b0, b1);
    float y0 = std::max(a0, b0), y1 = std::min(a1, b1);
    std::vector<PointF> po = outer.polygon(), pi = reversed(inner.polygon());
    if (!straight || y1 - y0 < 2) {
        fillPolys(p, {po, pi}, f);
        return;
    }
    fillPolys(p, {clipAt(po, y0, true), clipAt(pi, y0, true)}, f);
    if (inner.L > outer.L) fillRect(p, RectF(outer.L, y0, inner.L - outer.L, y1 - y0), f);
    if (outer.R > inner.R) fillRect(p, RectF(inner.R, y0, outer.R - inner.R, y1 - y0), f);
    fillPolys(p, {clipAt(po, y1, false), clipAt(pi, y1, false)}, f);
}

bool whole(float v) { return v == std::floor(v); }

struct Geo {
    RoundRect border;  // the border box
    float w[4] = {0, 0, 0, 0};
    bool crisp = false;
    float scale = 1;
};

Geo geometry(Painter& p, const RectF& r, const BoxPaint& b) {
    Geo g;
    float s = g.scale = p.scale();
    PointF t = p.translation();
    float lx0 = r.x + t.x, ly0 = r.y + t.y, lx1 = r.right() + t.x, ly1 = r.bottom() + t.y;
    bool square = !theme::rounded || (b.radius[0] <= 0 && b.radius[1] <= 0 && b.radius[2] <= 0 && b.radius[3] <= 0);
    bool aligned = whole(lx0) && whole(ly0) && whole(lx1) && whole(ly1);
    for (int i = 0; i < 4; i++) aligned = aligned && whole(b.width[i]);
    g.crisp = square && aligned;
    RoundRect& o = g.border;
    if (g.crisp) {  // (as the painter places whole rectangles: edges rounded to device pixels)
        auto rd = [s](float v) { return std::floor(v * s + 0.5f); };
        o.L = rd(lx0);
        o.T = rd(ly0);
        o.R = rd(lx1);
        o.B = rd(ly1);
        for (int i = 0; i < 4; i++) g.w[i] = b.width[i] > 0 ? std::max(1.f, rd(b.width[i])) : 0;
    } else {
        o.L = lx0 * s;
        o.T = ly0 * s;
        o.R = lx1 * s;
        o.B = ly1 * s;
        for (int i = 0; i < 4; i++) g.w[i] = b.width[i] * s;
    }
    // borders wider than the box: narrowed to fit
    float W = o.R - o.L, H = o.B - o.T;
    if (g.w[1] + g.w[3] > W && W >= 0) {
        float k = g.w[1] + g.w[3] > 0 ? W / (g.w[1] + g.w[3]) : 0;
        g.w[1] *= k;
        g.w[3] *= k;
        if (g.crisp) g.w[1] = W - (g.w[3] = std::floor(g.w[3]));
    }
    if (g.w[0] + g.w[2] > H && H >= 0) {
        float k = g.w[0] + g.w[2] > 0 ? H / (g.w[0] + g.w[2]) : 0;
        g.w[0] *= k;
        g.w[2] *= k;
        if (g.crisp) g.w[2] = H - (g.w[0] = std::floor(g.w[0]));
    }
    if (theme::rounded)
        for (int i = 0; i < 4; i++) o.rx[i] = o.ry[i] = std::max(0.f, b.radius[i] * s);
    // radii that don't fit: scaled down together (CSS)
    float f = 1;
    auto fit = [&](float len, float a, float c) {
        if (a + c > len && a + c > 0) f = std::min(f, len / (a + c));
    };
    fit(W, o.rx[0], o.rx[1]);
    fit(W, o.rx[3], o.rx[2]);
    fit(H, o.ry[0], o.ry[3]);
    fit(H, o.ry[1], o.ry[2]);
    for (int i = 0; i < 4; i++) {
        o.rx[i] *= f;
        o.ry[i] *= f;
    }
    return g;
}

// where a band's edge is, across a side `w` wide (crisp: whole pixels)
float bandOffset(float f, float w, bool crisp) {
    if (!crisp || w <= 0) return f * w;
    if (f <= 0) return 0;
    if (f >= 1) return w;
    if (std::fabs(f - 0.5f) < 1e-4f) return std::floor(w / 2);
    float third = std::max(1.f, std::floor(w / 3 + 0.5f));
    return f < 0.5f ? third : w - third;
}

// --- the border's colours: each pixel its side's, the sides meeting on the corners' lines ---
class RingShader : public Shader {
public:
    RingShader(const Geo& g, const uint32_t colors[4]) : crisp_(g.crisp) {
        const RoundRect& o = g.border;
        for (int i = 0; i < 4; i++) col_[i] = colors[i];
        cx_ = (o.L + o.R) / 2;
        cy_ = (o.T + o.B) / 2;
        // corners: tl tr br bl; each from the border box's corner to the padding box's
        const float px[4] = {o.L, o.R, o.R, o.L}, py[4] = {o.T, o.T, o.B, o.B};
        const float qx[4] = {o.L + g.w[3], o.R - g.w[1], o.R - g.w[1], o.L + g.w[3]};
        const float qy[4] = {o.T + g.w[0], o.T + g.w[0], o.B - g.w[2], o.B - g.w[2]};
        const int hs[4] = {0, 0, 2, 2}, vs[4] = {3, 1, 1, 3};
        const float sx[4] = {1, -1, -1, 1}, sy[4] = {1, 1, -1, -1};
        const float tie[4] = {1, 0, 1, 1};  // (on the line: top, right, bottom, bottom)
        for (int i = 0; i < 4; i++) {
            Corner& c = corner_[i];
            c.px = px[i];
            c.py = py[i];
            c.h = hs[i];
            c.v = vs[i];
            c.tie = tie[i];
            float dx = qx[i] - px[i], dy = qy[i] - py[i];
            float nx = -dy, ny = dx;
            if (dx == 0 && dy == 0) {
                nx = sx[i];
                ny = -sy[i];
            }
            // positive towards the horizontal side (along it from the corner), negative towards the vertical one
            if (nx * sx[i] < ny * sy[i]) {
                nx = -nx;
                ny = -ny;
            }
            float len = std::sqrt(nx * nx + ny * ny);
            c.nx = nx;
            c.ny = ny;
            c.inv = len > 0 ? 1 / len : 0;
        }
    }
    void shade(int x, int y, int n, uint32_t* out) const override {
        float py = y + 0.5f;
        for (int i = 0; i < n; i++) {
            float px = x + i + 0.5f;
            const Corner& c = corner_[py < cy_ ? (px < cx_ ? 0 : 1) : (px < cx_ ? 3 : 2)];
            float d = (px - c.px) * c.nx + (py - c.py) * c.ny;
            float wh;
            if (crisp_) wh = d > 0 ? 1.f : d < 0 ? 0.f : c.tie;
            else wh = std::clamp(0.5f + d * c.inv, 0.f, 1.f);
            uint32_t a = col_[c.v], b = col_[c.h];
            if (wh >= 1 || a == b) out[i] = b;
            else if (wh <= 0) out[i] = a;
            else {
                uint32_t k = (uint32_t)(wh * 256 + 0.5f), ik = 256 - k;
                uint32_t rb = (((a & 0x00ff00ffu) * ik + (b & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
                uint32_t ag = ((((a >> 8) & 0x00ff00ffu) * ik + ((b >> 8) & 0x00ff00ffu) * k)) & 0xff00ff00u;
                out[i] = rb | ag;
            }
        }
    }

private:
    struct Corner {
        float px, py, nx, ny, inv, tie;
        int h, v;
    };
    Corner corner_[4];
    uint32_t col_[4];
    float cx_, cy_;
    bool crisp_;
};

// --- gradients ---
// CSS's colour stop fix-up: a missing first / last position is 0 / 1; none before an earlier one; missing
// ones spread evenly between their neighbours. `lineLen`: what px positions are of (device px), `s` the scale.
std::vector<GradientStop> fixStops(const std::vector<css::Style::ColorStop>& in, float lineLen, float s) {
    size_t n = in.size();
    std::vector<float> pos(n);
    std::vector<bool> given(n);
    for (size_t i = 0; i < n; i++) {
        given[i] = in[i].px || in[i].position != -1;
        pos[i] = !given[i] ? 0 : in[i].px ? (lineLen > 0 ? (float)in[i].position * s / lineLen : 0) : (float)in[i].position;
    }
    if (!given[0]) {
        pos[0] = 0;
        given[0] = true;
    }
    if (!given[n - 1]) {
        pos[n - 1] = 1;
        given[n - 1] = true;
    }
    float most = pos[0];
    for (size_t i = 0; i < n; i++)
        if (given[i]) {
            pos[i] = std::max(pos[i], most);
            most = pos[i];
        }
    for (size_t i = 1; i < n;) {
        if (given[i]) {
            i++;
            continue;
        }
        size_t j = i;
        while (!given[j]) j++;
        float a = pos[i - 1], b = pos[j];
        for (size_t k = i; k < j; k++) pos[k] = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j;
    }
    std::vector<GradientStop> out(n);
    for (size_t i = 0; i < n; i++) out[i] = {pos[i], toColor(in[i].color)};
    return out;
}

bool sizeIndependent(const css::Style::Gradient& g) {
    for (const auto& s : g.stops)
        if (s.px) return false;
    return true;
}

void paintGradient(Painter& p, const RoundRect& clip, const RoundRect& box,
                   const BoxPaint::Layer& layer, float s) {
    const css::Style::Gradient& g = layer.g;
    float W = box.R - box.L, H = box.B - box.T;
    if (!(W > 0) || !(H > 0)) return;
    GradientRamp local;
    auto ramp = [&](float lineLen) -> const GradientRamp& {
        if (layer.ramp) return *layer.ramp;
        local = GradientRamp(fixStops(g.stops, lineLen, s), g.repeating);
        return local;
    };
    if (g.kind == css::Style::Gradient::Linear) {
        float dx, dy;
        if (g.toCorner) {  // perpendicular to the diagonal between the other two corners
            dx = g.cornerX * H;
            dy = g.cornerY * W;
            float l = std::sqrt(dx * dx + dy * dy);
            dx /= l;
            dy /= l;
        } else {
            float a = (float)g.angle * kPi / 180;
            dx = std::sin(a);
            dy = -std::cos(a);
        }
        float len = std::fabs(W * dx) + std::fabs(H * dy);
        PointF c((box.L + box.R) / 2, (box.T + box.B) / 2);
        PointF p0(c.x - dx * len / 2, c.y - dy * len / 2), p1(c.x + dx * len / 2, c.y + dy * len / 2);
        const GradientRamp& r = ramp(len);
        LinearGradientShader sh(p0, p1, r);
        fillShape(p, clip, Fill{Color(), &sh});
        return;
    }
    float cx = box.L + (float)(g.centerXPx ? g.centerX * s : g.centerX * W);
    float cy = box.T + (float)(g.centerYPx ? g.centerY * s : g.centerY * H);
    float l = cx - box.L, rgt = box.R - cx, t = cy - box.T, btm = box.B - cy;
    float sideNearX = std::min(std::fabs(l), std::fabs(rgt)), sideFarX = std::max(std::fabs(l), std::fabs(rgt));
    float sideNearY = std::min(std::fabs(t), std::fabs(btm)), sideFarY = std::max(std::fabs(t), std::fabs(btm));
    float rx = 0, ry = 0;
    using G = css::Style::Gradient;
    if (g.size == G::Explicit) {
        rx = (float)(g.radiusXPercent ? g.radiusX * W : g.radiusX * s);
        ry = (float)(g.radiusYPercent ? g.radiusY * H : g.radiusY * s);
        if (g.circle) ry = rx;
    } else if (g.circle) {
        float nearC = 1e30f, farC = 0;
        for (float x : {l, rgt})
            for (float y : {t, btm}) {
                float d = std::sqrt(x * x + y * y);
                nearC = std::min(nearC, d);
                farC = std::max(farC, d);
            }
        rx = g.size == G::ClosestSide    ? std::min(sideNearX, sideNearY)
             : g.size == G::FarthestSide ? std::max(sideFarX, sideFarY)
             : g.size == G::ClosestCorner ? nearC
                                          : farC;
        ry = rx;
    } else {
        bool closest = g.size == G::ClosestSide || g.size == G::ClosestCorner;
        float sx = closest ? sideNearX : sideFarX, sy = closest ? sideNearY : sideFarY;
        rx = sx;
        ry = sy;
        if (g.size == G::ClosestCorner || g.size == G::FarthestCorner) {
            // through the corner, with the side's ellipse's proportions
            float k = sx > 0 && sy > 0 ? std::sqrt(2.f) : 1.f;
            rx = sx * k;
            ry = sy * k;
        }
    }
    const GradientRamp& r = ramp(rx);
    RadialGradientShader sh(PointF(cx, cy), rx, ry, r);
    fillShape(p, clip, Fill{Color(), &sh});
}

// --- the shadows' masks, kept ---
struct Mask {
    std::vector<int32_t> key;
    int x = 0, y = 0, w = 0, h = 0;  // where, from the box's whole-pixel origin
    std::vector<uint8_t> a;
};
struct MaskCache {
    std::list<Mask> list;  // the most recently used first
    size_t bytes = 0;
    int made = 0;  // (masks made: blurred, not found)
};
MaskCache& masks() {
    static MaskCache c;
    return c;
}
const size_t kMaxMasks = 48, kMaxMaskBytes = 24u << 20;

int32_t q16(float v) { return (int32_t)std::lround(v * 16); }
void keyOf(std::vector<int32_t>& k, const RoundRect& r, float ox, float oy) {
    k.push_back(q16(r.L - ox));
    k.push_back(q16(r.T - oy));
    k.push_back(q16(r.R - ox));
    k.push_back(q16(r.B - oy));
    for (int i = 0; i < 4; i++) {
        k.push_back(q16(r.rx[i]));
        k.push_back(q16(r.ry[i]));
    }
}

const Mask* findMask(const std::vector<int32_t>& key) {
    MaskCache& c = masks();
    for (auto it = c.list.begin(); it != c.list.end(); ++it)
        if (it->key == key) {
            if (it != c.list.begin()) c.list.splice(c.list.begin(), c.list, it);
            return &c.list.front();
        }
    return nullptr;
}
const Mask& keepMask(Mask&& m) {
    MaskCache& c = masks();
    c.made++;
    c.bytes += m.a.size();
    c.list.push_front(std::move(m));
    while (c.list.size() > 1 && (c.list.size() > kMaxMasks || c.bytes > kMaxMaskBytes)) {
        c.bytes -= c.list.back().a.size();
        c.list.pop_back();
    }
    return c.list.front();
}

Rect boundsOf(const RoundRect& r, int margin) {
    int x0 = (int)std::floor(r.L) - margin, y0 = (int)std::floor(r.T) - margin;
    int x1 = (int)std::ceil(r.R) + margin, y1 = (int)std::ceil(r.B) + margin;
    return Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

const int kMaxMaskSide = 4096;

// an outer shadow: `shape` blurred, cut out where `box` is
void outerShadow(Painter& p, const RoundRect& box, const RoundRect& shape, float sigma, Color c) {
    if (shape.empty() || c.a == 0) return;
    float ox = std::floor(box.L), oy = std::floor(box.T);
    std::vector<int32_t> key{0, q16(sigma)};
    keyOf(key, box, ox, oy);
    keyOf(key, shape, ox, oy);
    const Mask* m = findMask(key);
    if (!m) {
        int margin = sigma > 0 ? (int)std::ceil(3 * sigma) + 1 : 1;
        Rect area = boundsOf(shape, margin);
        if (area.w > kMaxMaskSide || area.h > kMaxMaskSide) return;
        Mask n;
        n.key = key;
        std::vector<uint8_t> cut;
        coverageMask({shape.polygon()}, area, n.a);
        blurMask(n.a.data(), area.w, area.h, sigma);
        coverageMask({box.polygon()}, area, cut);
        for (size_t i = 0; i < n.a.size(); i++) n.a[i] = (uint8_t)((n.a[i] * (255 - cut[i]) + 127) / 255);
        n.x = area.x - (int)ox;
        n.y = area.y - (int)oy;
        n.w = area.w;
        n.h = area.h;
        m = &keepMask(std::move(n));
    }
    p.blendMask((int)ox + m->x, (int)oy + m->y, m->w, m->h, m->a.data(), c);
}

// an inset shadow: inside `pad`, around `hole` blurred
void insetShadow(Painter& p, const RoundRect& pad, const RoundRect& hole, float sigma, Color c) {
    if (pad.empty() || c.a == 0) return;
    float ox = std::floor(pad.L), oy = std::floor(pad.T);
    std::vector<int32_t> key{1, q16(sigma)};
    keyOf(key, pad, ox, oy);
    keyOf(key, hole, ox, oy);
    const Mask* m = findMask(key);
    if (!m) {
        int margin = sigma > 0 ? (int)std::ceil(3 * sigma) + 1 : 1;
        Rect area = boundsOf(pad, 0), big = boundsOf(pad, margin);
        if (big.w > kMaxMaskSide || big.h > kMaxMaskSide) return;
        std::vector<uint8_t> a, inside;
        if (hole.empty()) a.assign((size_t)big.w * big.h, 0);
        else coverageMask({hole.polygon()}, big, a);
        for (uint8_t& v : a) v = (uint8_t)(255 - v);
        blurMask(a.data(), big.w, big.h, sigma);
        coverageMask({pad.polygon()}, area, inside);
        Mask n;
        n.key = key;
        n.a.resize((size_t)area.w * area.h);
        for (int y = 0; y < area.h; y++)
            for (int x = 0; x < area.w; x++) {
                size_t i = (size_t)y * area.w + x;
                uint8_t v = a[(size_t)(y + area.y - big.y) * big.w + (x + area.x - big.x)];
                n.a[i] = (uint8_t)((v * inside[i] + 127) / 255);
            }
        n.x = area.x - (int)ox;
        n.y = area.y - (int)oy;
        n.w = area.w;
        n.h = area.h;
        m = &keepMask(std::move(n));
    }
    p.blendMask((int)ox + m->x, (int)oy + m->y, m->w, m->h, m->a.data(), c);
}

// --- dashed and dotted sides ---
// a piece of a polyline from arc length a to b
std::vector<PointF> piece(const std::vector<PointF>& line, const std::vector<float>& at, float a, float b) {
    std::vector<PointF> out;
    for (size_t i = 0; i + 1 < line.size(); i++) {
        float s0 = at[i], s1 = at[i + 1];
        if (s1 <= a || s0 >= b || s1 <= s0) continue;
        auto lerp = [&](float s) {
            float u = (s - s0) / (s1 - s0);
            return PointF(line[i].x + (line[i + 1].x - line[i].x) * u, line[i].y + (line[i + 1].y - line[i].y) * u);
        };
        if (out.empty()) out.push_back(lerp(std::max(a, s0)));
        out.push_back(lerp(std::min(b, s1)));
    }
    return out;
}
// a polyline as a band `w` wide around it (one polygon)
std::vector<PointF> thick(const std::vector<PointF>& pts, float w) {
    std::vector<PointF> left, right;
    size_t n = pts.size();
    if (n < 2) return {};
    for (size_t i = 0; i < n; i++) {
        PointF d0 = i > 0 ? pts[i] - pts[i - 1] : pts[1] - pts[0];
        PointF d1 = i + 1 < n ? pts[i + 1] - pts[i] : pts[n - 1] - pts[n - 2];
        auto unit = [](PointF v) {
            float l = std::sqrt(v.x * v.x + v.y * v.y);
            return l > 0 ? PointF(v.x / l, v.y / l) : PointF(0, 0);
        };
        PointF d = unit(unit(d0) + unit(d1));
        PointF nrm(-d.y * w / 2, d.x * w / 2);
        left.push_back(pts[i] + nrm);
        right.push_back(pts[i] - nrm);
    }
    left.insert(left.end(), right.rbegin(), right.rend());
    return left;
}

void dashSide(Painter& p, const Geo& g, int side, Line line, Color c) {
    float w = g.w[side];
    if (w <= 0 || c.a == 0) return;
    const RoundRect& o = g.border;
    // the side's centre line: from the middle of one corner to the middle of the next
    std::vector<PointF> path;
    float half[4] = {g.w[0] / 2, g.w[1] / 2, g.w[2] / 2, g.w[3] / 2};
    RoundRect mid = o.inset(half);
    bool straight = o.square();
    if (straight) {
        // top / bottom from corner to corner, left / right between them
        if (side == 0) path = {PointF(o.L, o.T + half[0]), PointF(o.R, o.T + half[0])};
        else if (side == 2) path = {PointF(o.L, o.B - half[2]), PointF(o.R, o.B - half[2])};
        else if (side == 1) path = {PointF(o.R - half[1], o.T + g.w[0]), PointF(o.R - half[1], o.B - g.w[2])};
        else path = {PointF(o.L + half[3], o.T + g.w[0]), PointF(o.L + half[3], o.B - g.w[2])};
    } else {
        // corners: tl tr br bl; a side runs from its first corner's middle to its second's
        const int c0[4] = {0, 1, 2, 3}, c1[4] = {1, 2, 3, 0};
        const float cxs[4] = {mid.L, mid.R, mid.R, mid.L}, cys[4] = {mid.T, mid.T, mid.B, mid.B};
        const float sx[4] = {1, -1, -1, 1}, sy[4] = {1, 1, -1, -1};
        const float a0[4] = {kPi, 1.5f * kPi, 0, 0.5f * kPi};
        auto arc = [&](int k, float from, float to) {
            float ax = mid.rx[k], ay = mid.ry[k];
            if (ax <= 0 || ay <= 0) {
                path.push_back(PointF(cxs[k], cys[k]));
                return;
            }
            float cx = cxs[k] + sx[k] * ax, cy = cys[k] + sy[k] * ay;
            int n = std::clamp((int)std::ceil(std::max(ax, ay) * 0.6f), 2, 48);
            for (int i = 0; i <= n; i++) {
                float t = a0[k] + kPi / 2 * (from + (to - from) * i / n);
                path.push_back(PointF(cx + ax * std::cos(t), cy + ay * std::sin(t)));
            }
        };
        arc(c0[side], 0.5f, 1);
        arc(c1[side], 0, 0.5f);
    }
    std::vector<float> at(path.size(), 0);
    for (size_t i = 1; i < path.size(); i++) {
        PointF d = path[i] - path[i - 1];
        at[i] = at[i - 1] + std::sqrt(d.x * d.x + d.y * d.y);
    }
    float len = at.back();
    if (len <= 0) return;
    std::vector<std::vector<PointF>> polys;
    bool horizontal = side == 0 || side == 2;
    // a straight crisp side: whole-pixel rectangles along it
    auto rectPiece = [&](float a, float b) {
        if (g.crisp) {
            a = std::floor(a + 0.5f);
            b = std::floor(b + 0.5f);
        }
        if (b <= a) return;
        float across0 = horizontal ? path[0].y - w / 2 : path[0].x - w / 2;
        if (horizontal) polys.push_back({PointF(path[0].x + a, across0), PointF(path[0].x + b, across0),
                                         PointF(path[0].x + b, across0 + w), PointF(path[0].x + a, across0 + w)});
        else polys.push_back({PointF(across0, path[0].y + a), PointF(across0 + w, path[0].y + a),
                              PointF(across0 + w, path[0].y + b), PointF(across0, path[0].y + b)});
    };
    if (line == Line::Dashed && straight) {
        // a dash at each end (the corners), gaps evened out between
        float dash = 3 * w, gap = 3 * w;
        int n = std::max(1, (int)std::lround((len + gap) / (dash + gap)));
        float space = n > 1 ? (len - n * dash) / (n - 1) : 0;
        if (n == 1) dash = len;
        for (int i = 0; i < n; i++) {
            float a = i * (dash + space);
            rectPiece(a, a + dash);
        }
    } else if (line == Line::Dashed) {
        // from corner middle to corner middle: half a dash at each end (with the next side's half, one dash
        // across the corner), whole ones evenly between
        float dash = 3 * w, gap = 3 * w;
        int n = std::max(1, (int)std::lround(len / (dash + gap)));
        float period = len / n;
        dash = std::min(dash, period * 0.6f);
        for (int i = 0; i <= n; i++) {
            float a = std::max(0.f, i * period - dash / 2), b = std::min(len, i * period + dash / 2);
            if (b > a) polys.push_back(thick(piece(path, at, a, b), w));
        }
    } else {
        // straight: a dot at each end, evenly between; around a rounded corner: one on the corner's middle (the
        // side's start; its end is the next side's start)
        int n = straight ? std::max(1, (int)std::lround((len + w) / (2 * w))) : std::max(1, (int)std::lround(len / (2 * w)));
        float step = straight ? (n > 1 ? (len - w) / (n - 1) : 0) : len / n;
        bool round = w >= 3;
        for (int i = 0; i < n; i++) {
            float s = !straight ? i * step : n > 1 ? w / 2 + i * step : len / 2;
            if (straight && !round) {
                rectPiece(s - w / 2, s + w / 2);
                continue;
            }
            std::vector<PointF> one = piece(path, at, std::max(0.f, s - 0.01f), std::min(len, s + 0.01f));
            if (one.empty()) continue;
            PointF ctr = one.front();
            std::vector<PointF> dot;
            if (round) {
                int k = std::clamp((int)std::ceil(w * 2), 8, 64);
                for (int j = 0; j < k; j++) {
                    float t = 2 * kPi * j / k;
                    dot.push_back(PointF(ctr.x + w / 2 * std::cos(t), ctr.y + w / 2 * std::sin(t)));
                }
            } else {
                dot = {PointF(ctr.x - w / 2, ctr.y - w / 2), PointF(ctr.x + w / 2, ctr.y - w / 2),
                       PointF(ctr.x + w / 2, ctr.y + w / 2), PointF(ctr.x - w / 2, ctr.y + w / 2)};
            }
            polys.push_back(std::move(dot));
        }
    }
    p.fillDevice(polys, c);
}

// --- text-shadow's blurred masks, kept ---
struct TextMask {
    std::string key;
    int dx = 0, dy = 0, w = 0, h = 0;  // from the text's whole-pixel origin
    std::vector<uint8_t> a;
};
std::list<TextMask>& textMasks() {
    static std::list<TextMask> l;
    return l;
}

}  // namespace

std::shared_ptr<const BoxPaint> boxPaint(const css::Style& s, const BoxLook& look) {
    auto b = std::make_shared<BoxPaint>();
    static const char* const sides[4] = {"top", "right", "bottom", "left"};
    css::Edges w = s.edges("border-width", css::Edges());
    b->width[0] = (float)std::max(0.0, w.top);
    b->width[1] = (float)std::max(0.0, w.right);
    b->width[2] = (float)std::max(0.0, w.bottom);
    b->width[3] = (float)std::max(0.0, w.left);
    Color fg = foreground(s);
    for (int i = 0; i < 4; i++) {
        std::string side = std::string("border-") + sides[i];
        b->color[i] = color(s, side + "-color", fg);
        b->line[i] = lineOf(s.keyword(side + "-style", "solid"));
    }
    css::Edges r = s.radii(css::Edges());
    b->radius[0] = (float)std::max(0.0, r.top);
    b->radius[1] = (float)std::max(0.0, r.right);
    b->radius[2] = (float)std::max(0.0, r.bottom);
    b->radius[3] = (float)std::max(0.0, r.left);
    b->bg = look.bg;
    if (s.has("background-image"))
        for (css::Style::Gradient& g : s.gradients("background-image")) b->layers.push_back({std::move(g), nullptr});
    if (s.has("box-shadow")) b->shadows = s.shadows("box-shadow");

    // a plain box (the inline fast path draws it: exactly as before there was more)?
    bool plain = b->layers.empty() && b->shadows.empty();
    for (int i = 1; i < 4; i++) plain = plain && b->radius[i] == b->radius[0];
    plain = plain && b->radius[0] == look.radius;
    // (the plain path draws every side in border-top-color, solid; or, fill only, no border at all)
    const int insetOf[4] = {look.border.top, look.border.right, look.border.bottom, look.border.left};
    for (int i = 0; i < 4; i++) {
        if (insetOf[i] <= 0) continue;
        if (look.fillOnly) {
            if (b->color[i].a && (b->line[i] != Line::Solid || b->color[i] != look.bg)) plain = false;
        } else if (b->line[i] != Line::Solid || b->color[i] != look.edge) {
            plain = false;
        }
    }
    // (the plain path fills the whole box with the border's colour and the background over it: right only for an
    // opaque background; under a see-through one the border would show through, so the border is drawn as a ring)
    if (!look.fillOnly && look.bg.a < 255) plain = false;
    if (plain) return nullptr;

    for (int i = 0; i < 4; i++) {
        makeBands(*b, i);
        for (const BoxPaint::Band& band : b->bands[i]) {
            b->breaks.push_back(band.f0);
            b->breaks.push_back(band.f1);
        }
    }
    std::sort(b->breaks.begin(), b->breaks.end());
    b->breaks.erase(std::unique(b->breaks.begin(), b->breaks.end(), [](float x, float y) { return std::fabs(x - y) < 1e-6f; }),
                    b->breaks.end());
    for (BoxPaint::Layer& l : b->layers)
        if (sizeIndependent(l.g)) l.ramp = std::make_shared<GradientRamp>(fixStops(l.g.stops, 1, 1), l.g.repeating);
    return b;
}

void drawBox(Painter& p, const RectF& r, const BoxLook& look) {
    const BoxPaint& b = *look.more;
    if (r.empty()) return;
    // (a repaint wholly inside it, clear of its border and corners, where only its flat background is drawn: that
    // fill alone, as below draws it there. A meter repainting in a bevelled track row, a cursor in a framed
    // timeline: no ring worked out each frame)
    if (b.layers.empty() && b.shadows.empty()) {
        float most = 0;
        for (int i = 0; i < 4; i++) most = std::max(most, b.width[i] + (theme::rounded ? b.radius[i] : 0.f));
        RectF in = r.adjusted(most + 1, most + 1, -most - 1, -most - 1);  // (+1: what anti-aliasing reaches)
        Rect c = p.clip();
        bool inside = c.x >= in.x && c.y >= in.y && c.right() <= in.right() && c.bottom() <= in.bottom();
        if (!c.empty() && !in.empty() && inside) {
            if (b.bg.a) p.fillRect(c, b.bg);
            return;
        }
    }
    Geo g = geometry(p, r, b);
    const RoundRect& o = g.border;
    float s = g.scale;
    RoundRect pad = o.inset(g.w);

    // 1. the outer shadows, the last first
    for (size_t i = b.shadows.size(); i-- > 0;) {
        const css::Style::Shadow& sh = b.shadows[i];
        if (sh.inset) continue;
        outerShadow(p, o, o.shadow((float)sh.x * s, (float)sh.y * s, (float)sh.spread * s), (float)sh.blur * s / 2,
                    toColor(sh.color));
    }

    // 2. the background: under an opaque border only to just under it (no background at its outer edge)
    float under[4];
    for (int i = 0; i < 4; i++) {
        float w = g.w[i];
        under[i] = !b.opaqueSide[i] || w <= 0 ? 0 : g.crisp ? w : w >= 2 ? w - 1 : w / 2;
    }
    RoundRect bgShape = o.inset(under);
    bool coveredByImage = false;
    for (const BoxPaint::Layer& l : b.layers)
        if (l.ramp && l.ramp->opaque()) coveredByImage = true;
    if (b.bg.a && !coveredByImage) fillShape(p, bgShape, Fill{b.bg});
    for (size_t i = b.layers.size(); i-- > 0;)
        if (!bgShape.empty()) paintGradient(p, bgShape, pad, b.layers[i], s);

    // 3. the inset shadows, over the background, inside the padding box
    for (size_t i = b.shadows.size(); i-- > 0;) {
        const css::Style::Shadow& sh = b.shadows[i];
        if (!sh.inset) continue;
        insetShadow(p, pad, pad.shadow((float)sh.x * s, (float)sh.y * s, -(float)sh.spread * s), (float)sh.blur * s / 2,
                    toColor(sh.color));
    }

    // 4. the border: a ring per band
    for (size_t k = 0; k + 1 < b.breaks.size(); k++) {
        float fa = b.breaks[k], fb = b.breaks[k + 1];
        uint32_t cols[4];
        bool any = false, moves = false;
        float da[4], db[4];
        for (int i = 0; i < 4; i++) {
            Color c(0, 0, 0, 0);
            for (const BoxPaint::Band& band : b.bands[i])
                if (band.f0 <= fa + 1e-6f && fb <= band.f1 + 1e-6f) c = band.c;
            cols[i] = c.premul();
            any = any || c.a;
            da[i] = bandOffset(fa, g.w[i], g.crisp);
            db[i] = bandOffset(fb, g.w[i], g.crisp);
            moves = moves || (db[i] > da[i] && c.a);
        }
        if (!any || !moves) continue;
        RoundRect outer = o.inset(da), inner = o.inset(db);
        if (cols[0] == cols[1] && cols[1] == cols[2] && cols[2] == cols[3]) {
            fillRing(p, outer, inner, Fill{Color::fromPremul(cols[0])});
        } else {
            RingShader sh(g, cols);
            fillRing(p, outer, inner, Fill{Color(), &sh});
        }
    }

    // 5. dashed and dotted sides
    for (int i = 0; i < 4; i++)
        if (b.line[i] == Line::Dashed || b.line[i] == Line::Dotted) dashSide(p, g, i, b.line[i], b.color[i]);
}

namespace {

// text-shadow's shadows of some text: `draw` draws it moved by (dx, dy) in a colour; `bounds` (logical) is
// where it is drawn, `key` says what it is (its text, font, layout) for the kept blurred masks
using TextDraw = std::function<void(Painter&, float dx, float dy, Color)>;
void shadowsOf(Painter& p, const RectF& bounds, const std::string& key, const std::vector<css::Style::Shadow>& shadows,
               const TextDraw& draw) {
    float sc = p.scale();
    for (size_t i = shadows.size(); i-- > 0;) {
        const css::Style::Shadow& sh = shadows[i];
        Color c = toColor(sh.color);
        if (!c.a) continue;
        if (sh.blur <= 0) {
            draw(p, (float)sh.x, (float)sh.y, c);
            continue;
        }
        // blurred: the text into a mask, blurred, kept for the next repaint
        float sigma = (float)sh.blur * sc / 2;
        int margin = (int)std::ceil(3 * sigma) + 2;
        PointF d0 = p.toDevice(PointF(bounds.x + (float)sh.x, bounds.y + (float)sh.y));
        int ox = (int)std::floor(d0.x) - margin, oy = (int)std::floor(d0.y) - margin;
        int tw = (int)std::ceil(bounds.w * sc) + 2 * margin + 2, th = (int)std::ceil(bounds.h * sc) + 2 * margin + 2;
        if (tw > kMaxMaskSide || th > kMaxMaskSide || tw <= 0 || th <= 0) continue;
        char num[64];
        snprintf(num, sizeof num, "|%d|%d|%d|%.4f", (int)std::lround(sigma * 16), (int)std::lround((d0.x - std::floor(d0.x)) * 16),
                 (int)std::lround((d0.y - std::floor(d0.y)) * 16), sc);
        std::string k = key + num;
        std::list<TextMask>& cache = textMasks();
        auto it = std::find_if(cache.begin(), cache.end(), [&](const TextMask& m) { return m.key == k; });
        if (it == cache.end()) {
            TextMask m;
            m.key = k;
            m.dx = ox - (int)std::floor(d0.x);
            m.dy = oy - (int)std::floor(d0.y);
            m.w = tw;
            m.h = th;
            Image layer(tw, th);
            Painter lp(layer, sc);
            PointF t = p.translation();
            lp.translate(t.x - ox / sc, t.y - oy / sc);
            draw(lp, (float)sh.x, (float)sh.y, Color(255, 255, 255));
            m.a.resize((size_t)tw * th);
            for (int yy = 0; yy < th; yy++)
                for (int xx = 0; xx < tw; xx++) m.a[(size_t)yy * tw + xx] = (uint8_t)(layer.pixel(xx, yy) >> 24);
            blurMask(m.a.data(), tw, th, sigma);
            cache.push_front(std::move(m));
            if (cache.size() > 32) cache.pop_back();
            it = cache.begin();
        } else if (it != cache.begin()) {
            cache.splice(cache.begin(), cache, it);
            it = cache.begin();
        }
        p.blendMask((int)std::floor(d0.x) + it->dx, (int)std::floor(d0.y) + it->dy, it->w, it->h, it->a.data(), c);
    }
}

std::string fontKey(const Font& f) {
    char b[64];
    snprintf(b, sizeof b, "|%.3f|%d|", f.pixelSize(), (int)f.bold());
    return f.family() + b;
}

}  // namespace

void textShadows(Painter& p, const RectF& r, int align, const std::string& s, const Font& f,
                 const std::vector<css::Style::Shadow>& shadows, bool elide) {
    if (s.empty() || shadows.empty()) return;
    std::string shown = elide ? f.elided(s, r.w) : s;
    if (shown.empty()) return;
    // where Painter::drawText puts it
    float w = f.width(shown), asc = f.ascent(), desc = f.descent();
    float x = r.x, y;
    if (align & AlignRight) x = r.right() - w;
    else if (align & AlignHCenter) x = r.x + (r.w - w) / 2;
    if (align & AlignBottom) y = r.bottom() - desc;
    else if (align & AlignVCenter) y = r.y + (r.h - (asc + desc)) / 2 + asc;
    else y = r.y + asc;
    float pad = std::ceil(f.pixelSize() * 0.25f) + 1;  // (glyphs reaching past their advance)
    RectF bounds(x - pad, y - asc - pad, w + 2 * pad, asc + desc + 2 * pad);
    shadowsOf(p, bounds, "1|" + fontKey(f) + shown, shadows, [&](Painter& q, float dx, float dy, Color c) {
        q.drawText(PointF(x + dx, y + dy), shown, f, c);
    });
}

void textBlockShadows(Painter& p, const RectF& r, int align, const std::string& s, const Font& f,
                      const std::vector<css::Style::Shadow>& shadows, bool wrap) {
    if (s.empty() || shadows.empty()) return;
    float pad = std::ceil(f.pixelSize() * 0.5f) + 1;
    RectF bounds(r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad);
    char lay[64];
    snprintf(lay, sizeof lay, "%.2f|%.2f|%d|%d|", r.w, r.h, align, (int)wrap);
    shadowsOf(p, bounds, "2|" + fontKey(f) + lay + s, shadows, [&](Painter& q, float dx, float dy, Color c) {
        q.drawTextBlock(r.translated(dx, dy), align, s, f, c, wrap);
    });
}

int shadowMasksMade() { return masks().made; }
void clearBoxCaches() {
    masks().list.clear();
    masks().bytes = 0;
    textMasks().clear();
}

}  // namespace style
}  // namespace ui
