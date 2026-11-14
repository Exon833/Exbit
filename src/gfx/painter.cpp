// Painter: paths, the stroker, and drawing onto an Image with an exact-area anti-aliased
// rasterizer (gfx/ttf_raster.h), plus fast paths for pixel-aligned rectangles and lines.
#include "gfx/painter.h"

#include <cmath>
#include <cstring>

#include "gfx/ttf_raster.h"

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kKappa = 0.5522847498f;  // cubic approximation of a quarter circle

// premultiplied pixel * a / 255 (each channel, exact rounding)
inline uint32_t mul8(uint32_t p, uint32_t a) {
    uint32_t rb = (p & 0x00ff00ffu) * a + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ag = ((p >> 8) & 0x00ff00ffu) * a + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return rb | ag;
}
inline uint32_t over(uint32_t src, uint32_t dst) { return src + mul8(dst, 255 - (src >> 24)); }

inline float len(PointF v) { return std::sqrt(v.x * v.x + v.y * v.y); }

raster::Accumulator& accumulator() {
    static raster::Accumulator acc;
    return acc;
}

}  // namespace

// ================================================================================================
// Path

void Path::moveTo(float x, float y) {
    if (!subpaths_.empty() && subpaths_.back().segs.empty() && !subpaths_.back().closed) subpaths_.pop_back();
    Sub s;
    s.start = PointF(x, y);
    subpaths_.push_back(s);
    cur_ = start_ = PointF(x, y);
}

void Path::lineTo(float x, float y) {
    if (subpaths_.empty() || subpaths_.back().closed) moveTo(cur_.x, cur_.y);
    Seg s;
    s.kind = 0;
    s.p[0] = PointF(x, y);
    subpaths_.back().segs.push_back(s);
    cur_ = PointF(x, y);
}

void Path::quadTo(float cx, float cy, float x, float y) {
    if (subpaths_.empty() || subpaths_.back().closed) moveTo(cur_.x, cur_.y);
    Seg s;
    s.kind = 1;
    s.p[0] = PointF(cx, cy);
    s.p[1] = PointF(x, y);
    subpaths_.back().segs.push_back(s);
    cur_ = PointF(x, y);
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (subpaths_.empty() || subpaths_.back().closed) moveTo(cur_.x, cur_.y);
    Seg s;
    s.kind = 2;
    s.p[0] = PointF(c1x, c1y);
    s.p[1] = PointF(c2x, c2y);
    s.p[2] = PointF(x, y);
    subpaths_.back().segs.push_back(s);
    cur_ = PointF(x, y);
}

void Path::arcTo(float rx, float ry, float rotDeg, bool largeArc, bool sweep, float x, float y) {
    // SVG arc (endpoint parameterization, SVG 1.1 appendix F.6) as cubic segments of <= 90 degrees
    float x1 = cur_.x, y1 = cur_.y;
    if (x1 == x && y1 == y) return;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0 || ry == 0) {
        lineTo(x, y);
        return;
    }
    double phi = rotDeg * kPi / 180.0, cphi = std::cos(phi), sphi = std::sin(phi);
    double dx = (x1 - x) / 2.0, dy = (y1 - y) / 2.0;
    double x1p = cphi * dx + sphi * dy, y1p = -sphi * dx + cphi * dy;
    double lambda = (x1p * x1p) / ((double)rx * rx) + (y1p * y1p) / ((double)ry * ry);
    double Rx = rx, Ry = ry;
    if (lambda > 1) {
        double s = std::sqrt(lambda);
        Rx *= s;
        Ry *= s;
    }
    double num = Rx * Rx * Ry * Ry - Rx * Rx * y1p * y1p - Ry * Ry * x1p * x1p;
    double den = Rx * Rx * y1p * y1p + Ry * Ry * x1p * x1p;
    double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (largeArc == sweep) coef = -coef;
    double cxp = coef * Rx * y1p / Ry, cyp = -coef * Ry * x1p / Rx;
    double cx = cphi * cxp - sphi * cyp + (x1 + x) / 2.0, cy = sphi * cxp + cphi * cyp + (y1 + y) / 2.0;
    auto angle = [](double ux, double uy, double vx, double vy) {
        double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    double th1 = angle(1, 0, (x1p - cxp) / Rx, (y1p - cyp) / Ry);
    double dth = angle((x1p - cxp) / Rx, (y1p - cyp) / Ry, (-x1p - cxp) / Rx, (-y1p - cyp) / Ry);
    if (!sweep && dth > 0) dth -= 2 * kPi;
    else if (sweep && dth < 0) dth += 2 * kPi;
    int n = std::max(1, (int)std::ceil(std::fabs(dth) / (kPi / 2) - 1e-6));
    double step = dth / n;
    double t = 4.0 / 3.0 * std::tan(step / 4);
    for (int i = 0; i < n; i++) {
        double a0 = th1 + step * i, a1 = a0 + step;
        double c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        double p1x = c0 - t * s0, p1y = s0 + t * c0;
        double p2x = c1 + t * s1, p2y = s1 - t * c1;
        auto map = [&](double ux, double uy, float& ox, float& oy) {
            double X = ux * Rx, Y = uy * Ry;
            ox = (float)(cphi * X - sphi * Y + cx);
            oy = (float)(sphi * X + cphi * Y + cy);
        };
        float ax, ay, bx, by, ex, ey;
        map(p1x, p1y, ax, ay);
        map(p2x, p2y, bx, by);
        if (i == n - 1) {
            ex = x;
            ey = y;
        } else {
            map(c1, s1, ex, ey);
        }
        cubicTo(ax, ay, bx, by, ex, ey);
    }
}

void Path::close() {
    if (subpaths_.empty() || subpaths_.back().closed) return;
    subpaths_.back().closed = true;
    cur_ = subpaths_.back().start;
}

void Path::addRect(const RectF& r) {
    moveTo(r.x, r.y);
    lineTo(r.right(), r.y);
    lineTo(r.right(), r.bottom());
    lineTo(r.x, r.bottom());
    close();
}

void Path::addRoundedRect(const RectF& r, float rx, float ry) {
    rx = std::min(std::max(0.f, rx), r.w / 2);
    ry = std::min(std::max(0.f, ry), r.h / 2);
    if (rx <= 0 || ry <= 0) {
        addRect(r);
        return;
    }
    float x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    float kx = rx * kKappa, ky = ry * kKappa;
    moveTo(x0 + rx, y0);
    lineTo(x1 - rx, y0);
    cubicTo(x1 - rx + kx, y0, x1, y0 + ry - ky, x1, y0 + ry);
    lineTo(x1, y1 - ry);
    cubicTo(x1, y1 - ry + ky, x1 - rx + kx, y1, x1 - rx, y1);
    lineTo(x0 + rx, y1);
    cubicTo(x0 + rx - kx, y1, x0, y1 - ry + ky, x0, y1 - ry);
    lineTo(x0, y0 + ry);
    cubicTo(x0, y0 + ry - ky, x0 + rx - kx, y0, x0 + rx, y0);
    close();
}

void Path::addEllipse(const RectF& r) {
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2, ry = r.h / 2;
    float kx = rx * kKappa, ky = ry * kKappa;
    moveTo(cx + rx, cy);
    cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    close();
}

void Path::addPolygon(const std::vector<PointF>& pts, bool closed) {
    if (pts.empty()) return;
    moveTo(pts[0].x, pts[0].y);
    for (size_t i = 1; i < pts.size(); i++) lineTo(pts[i].x, pts[i].y);
    if (closed) close();
}

std::vector<Path::Poly> Path::flatten(float tolerance) const {
    std::vector<Poly> out;
    tolerance = std::max(tolerance, 0.01f);
    for (const Sub& s : subpaths_) {
        Poly p;
        p.closed = s.closed;
        p.pts.push_back(s.start);
        PointF c = s.start;
        for (const Seg& g : s.segs) {
            if (g.kind == 0) {
                p.pts.push_back(g.p[0]);
                c = g.p[0];
            } else if (g.kind == 1) {
                PointF dd = c - g.p[0] * 2 + g.p[1];
                int n = std::max(1, (int)std::ceil(std::sqrt(0.25f * len(dd) / tolerance)));
                n = std::min(n, 500);
                for (int i = 1; i <= n; i++) {
                    float t = (float)i / n, u = 1 - t;
                    p.pts.push_back(PointF(u * u * c.x + 2 * u * t * g.p[0].x + t * t * g.p[1].x,
                                           u * u * c.y + 2 * u * t * g.p[0].y + t * t * g.p[1].y));
                }
                c = g.p[1];
            } else {
                PointF d1 = c - g.p[0] * 2 + g.p[1], d2 = g.p[0] - g.p[1] * 2 + g.p[2];
                float m = std::max(len(d1), len(d2));
                int n = std::max(1, (int)std::ceil(std::sqrt(0.75f * m / tolerance)));
                n = std::min(n, 500);
                for (int i = 1; i <= n; i++) {
                    float t = (float)i / n, u = 1 - t;
                    float a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
                    p.pts.push_back(PointF(a * c.x + b * g.p[0].x + cc * g.p[1].x + d * g.p[2].x,
                                           a * c.y + b * g.p[0].y + cc * g.p[1].y + d * g.p[2].y));
                }
                c = g.p[2];
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

void Path::transform(float a, float b, float c, float d, float e, float f) {
    auto tr = [&](PointF& p) {
        float x = p.x, y = p.y;
        p.x = a * x + c * y + e;
        p.y = b * x + d * y + f;
    };
    for (Sub& s : subpaths_) {
        tr(s.start);
        for (Seg& g : s.segs) {
            int n = g.kind == 0 ? 1 : g.kind == 1 ? 2 : 3;
            for (int i = 0; i < n; i++) tr(g.p[i]);
        }
    }
    tr(cur_);
    tr(start_);
}

RectF Path::bounds() const {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    auto add = [&](PointF p) {
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    };
    for (const Sub& s : subpaths_) {
        add(s.start);
        for (const Seg& g : s.segs) {
            int n = g.kind == 0 ? 1 : g.kind == 1 ? 2 : 3;
            for (int i = 0; i < n; i++) add(g.p[i]);
        }
    }
    if (x0 > x1) return RectF();
    return RectF(x0, y0, x1 - x0, y1 - y0);
}

// ================================================================================================
// Stroker: a stroke is the union of pieces that all wind the same way (a quad per segment, a
// wedge per join, caps), so the accumulator's min(1, |sum|) fills their union.

namespace {

struct Stroker {
    float hw;
    LineCap cap;
    LineJoin join;
    float miterLimit = 4.f;
    std::vector<Path::Poly>& out;

    void piece(std::vector<PointF>&& pts) {
        if (pts.size() < 3) return;
        double area = 0;
        for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
            area += (double)pts[j].x * pts[i].y - (double)pts[i].x * pts[j].y;
        if (std::fabs(area) < 1e-9) return;
        if (area < 0) std::reverse(pts.begin(), pts.end());
        Path::Poly p;
        p.closed = true;
        p.pts = std::move(pts);
        out.push_back(std::move(p));
    }

    int arcSteps(float sweep) const {
        float r = std::max(hw, 0.05f);
        float tol = 0.1f;
        float step = r > tol ? 2.f * std::acos(1.f - tol / r) : kPi / 2;
        return std::max(2, std::min(256, (int)std::ceil(std::fabs(sweep) / step)));
    }

    void fan(PointF c, float a0, float sweep) {
        int n = arcSteps(sweep);
        std::vector<PointF> pts;
        pts.reserve(n + 2);
        pts.push_back(c);
        for (int i = 0; i <= n; i++) {
            float a = a0 + sweep * i / n;
            pts.push_back(PointF(c.x + hw * std::cos(a), c.y + hw * std::sin(a)));
        }
        piece(std::move(pts));
    }

    void dot(PointF p) {
        if (cap == LineCap::Round) {
            fan(p, 0, 2 * kPi);
        } else if (cap == LineCap::Square) {
            piece({PointF(p.x - hw, p.y - hw), PointF(p.x + hw, p.y - hw), PointF(p.x + hw, p.y + hw),
                   PointF(p.x - hw, p.y + hw)});
        }
    }

    void joinAt(PointF p, PointF d1, PointF d2) {
        float cross = d1.x * d2.y - d1.y * d2.x, dt = d1.x * d2.x + d1.y * d2.y;
        if (std::fabs(cross) < 1e-5f && dt > 0) return;
        PointF n1(-d1.y, d1.x), n2(-d2.y, d2.x);
        if (std::fabs(cross) < 1e-5f) {  // a U-turn
            if (join == LineJoin::Round) fan(p, std::atan2(n1.y, n1.x), -kPi);
            return;
        }
        float s = cross > 0 ? -1.f : 1.f;
        PointF o1 = p + n1 * (s * hw), o2 = p + n2 * (s * hw);
        if (join == LineJoin::Round) {
            float a1 = std::atan2(o1.y - p.y, o1.x - p.x), a2 = std::atan2(o2.y - p.y, o2.x - p.x);
            float sw = a2 - a1;
            while (sw > kPi) sw -= 2 * kPi;
            while (sw < -kPi) sw += 2 * kPi;
            fan(p, a1, sw);
            return;
        }
        if (join == LineJoin::Miter) {
            float cosHalf = std::sqrt(std::max(0.f, (1 + dt) / 2));
            if (cosHalf * miterLimit >= 1.f) {
                float k = s * hw / (1 + dt);
                PointF tip = p + (n1 + n2) * k;
                piece({p, o1, tip, o2});
                return;
            }
        }
        piece({p, o1, o2});
    }

    void polyline(const Path::Poly& in) {
        std::vector<PointF> pts;
        pts.reserve(in.pts.size());
        for (PointF q : in.pts) {
            if (!std::isfinite(q.x) || !std::isfinite(q.y)) continue;
            if (!pts.empty() && std::fabs(q.x - pts.back().x) < 1e-4f && std::fabs(q.y - pts.back().y) < 1e-4f) continue;
            pts.push_back(q);
        }
        bool closed = in.closed;
        if (closed && pts.size() > 1 && std::fabs(pts[0].x - pts.back().x) < 1e-4f &&
            std::fabs(pts[0].y - pts.back().y) < 1e-4f)
            pts.pop_back();
        size_t n = pts.size();
        if (n == 0) return;
        if (n == 1) {
            dot(pts[0]);
            return;
        }
        if (n == 2) closed = false;
        size_t segs = closed ? n : n - 1;
        std::vector<PointF> dirs(segs);
        for (size_t i = 0; i < segs; i++) {
            PointF v = pts[(i + 1) % n] - pts[i];
            float l = len(v);
            dirs[i] = PointF(v.x / l, v.y / l);
        }
        for (size_t i = 0; i < segs; i++) {
            PointF a = pts[i], b = pts[(i + 1) % n], d = dirs[i];
            if (!closed && cap == LineCap::Square) {
                if (i == 0) a = a - d * hw;
                if (i == segs - 1) b = b + d * hw;
            }
            PointF nn(-d.y * hw, d.x * hw);
            piece({a + nn, b + nn, b - nn, a - nn});
        }
        for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
            size_t prev = (i + segs - 1) % segs;
            joinAt(pts[i], dirs[prev], dirs[i]);
        }
        if (!closed && cap == LineCap::Round) {
            PointF d0 = dirs[0], d1 = dirs[segs - 1];
            fan(pts[0], std::atan2(d0.x, -d0.y), kPi);
            fan(pts[n - 1], std::atan2(d1.x, -d1.y), -kPi);
        }
    }
};

void strokeOutline(const std::vector<Path::Poly>& lines, float width, LineCap cap, LineJoin join,
                   std::vector<Path::Poly>& out) {
    if (!(width > 0)) return;
    Stroker s{width / 2, cap, join, 4.f, out};
    for (const Path::Poly& p : lines) s.polyline(p);
}

}  // namespace

// ================================================================================================
// Painter

Painter::Painter(Image& target) : img_(&target) { clip_ = target.rect(); }

Painter::Painter(Image& target, float scale) : img_(&target) {
    clip_ = target.rect();
    scale_ = scale > 0 && std::isfinite(scale) ? scale : 1.f;
}

// While a scaled painter hands a call on in device pixels: no translation, no scale (put back after)
struct Painter::Unscaled {
    Painter* p;
    float tx, ty, s;
    explicit Unscaled(Painter* painter) : p(painter), tx(painter->tx_), ty(painter->ty_), s(painter->scale_) {
        p->tx_ = p->ty_ = 0;
        p->scale_ = 1;
    }
    ~Unscaled() {
        p->tx_ = tx;
        p->ty_ = ty;
        p->scale_ = s;
    }
};

void Painter::save() { stack_.push_back({tx_, ty_, clip_, opacity_}); }

void Painter::restore() {
    if (stack_.empty()) return;
    State s = stack_.back();
    stack_.pop_back();
    tx_ = s.tx;
    ty_ = s.ty;
    clip_ = s.clip;
    opacity_ = s.opacity;
}

void Painter::translate(float dx, float dy) {
    tx_ += dx;
    ty_ += dy;
}

static inline int iround(float v) { return (int)std::floor(v + 0.5f); }

Rect Painter::devRect(const Rect& r) const {
    // the edges rounded to device pixels, so rectangles side by side still meet exactly
    float s = scale_;
    int x0 = iround((r.x + tx_) * s), y0 = iround((r.y + ty_) * s);
    int x1 = iround((r.x + r.w + tx_) * s), y1 = iround((r.y + r.h + ty_) * s);
    return Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

void Painter::toDevice(std::vector<Path::Poly>& polys) const {
    float s = scale_;
    for (Path::Poly& p : polys)
        for (PointF& q : p.pts) {
            q.x = (q.x + tx_) * s;
            q.y = (q.y + ty_) * s;
        }
}

void Painter::setClip(const Rect& r) {
    if (scale_ != 1) {
        clip_ = clip_.intersected(devRect(r));
        return;
    }
    clip_ = clip_.intersected(r.translated(iround(tx_), iround(ty_)));
}

void Painter::resetClip() { clip_ = img_->rect(); }

Rect Painter::clip() const {
    if (scale_ != 1) {  // (every logical pixel the device clip touches)
        if (clip_.empty()) return Rect();
        int x0 = (int)std::floor(clip_.x / scale_ - tx_), y0 = (int)std::floor(clip_.y / scale_ - ty_);
        int x1 = (int)std::ceil(clip_.right() / scale_ - tx_), y1 = (int)std::ceil(clip_.bottom() / scale_ - ty_);
        return Rect(x0, y0, x1 - x0, y1 - y0);
    }
    return clip_.translated(-iround(tx_), -iround(ty_));
}

void Painter::setOpacity(float o) {
    float base = stack_.empty() ? 1.f : stack_.back().opacity;
    opacity_ = std::clamp(base * o, 0.f, 1.f);
}

static inline uint32_t premulWith(Color c, float opacity) {
    if (opacity < 1.f) c.a = (uint8_t)std::lround(c.a * opacity);
    return c.premul();
}

// --- pixel-aligned fast paths ---

void Painter::fillRect(const Rect& r, Color c) {
    if (scale_ != 1) {
        Rect d = devRect(r);
        Unscaled u(this);
        fillRect(d, c);
        return;
    }
    uint32_t p = premulWith(c, opacity_);
    if (!(p >> 24)) return;
    Rect d = r.translated(iround(tx_), iround(ty_)).intersected(clip_);
    if (d.empty()) return;
    if ((p >> 24) == 255) {
        for (int y = d.y; y < d.bottom(); y++) std::fill_n(img_->row(y) + d.x, d.w, p);
        return;
    }
    uint32_t inv = 255 - (p >> 24);
    for (int y = d.y; y < d.bottom(); y++) {
        uint32_t* q = img_->row(y) + d.x;
        for (int i = 0; i < d.w; i++) q[i] = p + mul8(q[i], inv);
    }
}

void Painter::drawHLine(int x0, int x1, int y, Color c) {
    if (x1 < x0) std::swap(x0, x1);
    fillRect(Rect(x0, y, x1 - x0, 1), c);
}

void Painter::drawVLine(int x, int y0, int y1, Color c) {
    if (y1 < y0) std::swap(y0, y1);
    fillRect(Rect(x, y0, 1, y1 - y0), c);
}

void Painter::blendSpan(int x, int y, int len, uint32_t premulColor) {
    if (scale_ != 1) {  // (the span's logical pixels, as device rectangles)
        Rect d = devRect(Rect(x, y, len, 1));
        Unscaled u(this);
        for (int row = d.y; row < d.bottom(); row++) blendSpan(d.x, row, d.w, premulColor);
        return;
    }
    x += iround(tx_);
    y += iround(ty_);
    if (y < clip_.y || y >= clip_.bottom()) return;
    int x0 = std::max(x, clip_.x), x1 = std::min(x + len, clip_.right());
    if (x1 <= x0) return;
    uint32_t p = opacity_ < 1.f ? mul8(premulColor, (uint32_t)std::lround(opacity_ * 255)) : premulColor;
    uint32_t a = p >> 24;
    uint32_t* q = img_->row(y);
    if (a == 255) {
        std::fill(q + x0, q + x1, p);
    } else if (p) {
        uint32_t inv = 255 - a;
        for (int i = x0; i < x1; i++) q[i] = p + mul8(q[i], inv);
    }
}

void Painter::blendPixel(int x, int y, uint32_t premulColor, int coverage) {
    if (scale_ != 1) {
        Rect d = devRect(Rect(x, y, 1, 1));
        Unscaled u(this);
        for (int row = d.y; row < d.bottom(); row++)
            for (int col = d.x; col < d.right(); col++) blendPixel(col, row, premulColor, coverage);
        return;
    }
    x += iround(tx_);
    y += iround(ty_);
    if (!clip_.contains(x, y) || coverage <= 0) return;
    uint32_t k = (uint32_t)std::min(255, coverage);
    if (opacity_ < 1.f) k = (k * (uint32_t)std::lround(opacity_ * 255) + 127) / 255;
    uint32_t s = mul8(premulColor, k);
    uint32_t* q = img_->row(y) + x;
    *q = over(s, *q);
}

// --- the rasterizer ---

void Painter::rasterize(const std::vector<Path::Poly>& polys, Color c) { rasterize(polys, c, false); }

void Painter::rasterize(const std::vector<Path::Poly>& polys, Color c, bool evenOdd) {
    uint32_t p = premulWith(c, opacity_);
    if (!(p >> 24) || polys.empty()) return;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (const Path::Poly& q : polys)
        for (PointF pt : q.pts) {
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) continue;
            bx0 = std::min(bx0, pt.x); by0 = std::min(by0, pt.y);
            bx1 = std::max(bx1, pt.x); by1 = std::max(by1, pt.y);
        }
    if (bx0 > bx1) return;
    bx0 = std::max(bx0, -1e7f); by0 = std::max(by0, -1e7f);
    bx1 = std::min(bx1, 1e7f); by1 = std::min(by1, 1e7f);
    Rect bounds((int)std::floor(bx0), (int)std::floor(by0), 0, 0);
    bounds.w = (int)std::ceil(bx1) - bounds.x + 1;
    bounds.h = (int)std::ceil(by1) - bounds.y + 1;
    Rect reg = bounds.intersected(clip_);
    if (reg.empty()) return;
    raster::Accumulator& acc = accumulator();
    acc.begin(reg.w, reg.h);
    float ox = (float)reg.x, oy = (float)reg.y;
    for (const Path::Poly& q : polys) {
        size_t n = q.pts.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; i++) {
            PointF a = q.pts[i], b = q.pts[(i + 1) % n];  // fills always close their polygons
            acc.line(a.x - ox, a.y - oy, b.x - ox, b.y - oy);
        }
    }
    bool opaque = (p >> 24) == 255;
    for (int y = 0; y < reg.h; y++) {
        uint32_t* row = img_->row(reg.y + y) + reg.x;
        acc.readRow(y, evenOdd, [&](int x, int n, const uint8_t* cov, int solid) {
            uint32_t* q = row + x;
            if (!cov) {
                uint32_t s = solid == 255 ? p : mul8(p, (uint32_t)solid);
                if ((s >> 24) == 255) {
                    std::fill_n(q, n, s);
                } else {
                    uint32_t inv = 255 - (s >> 24);
                    for (int i = 0; i < n; i++) q[i] = s + mul8(q[i], inv);
                }
                return;
            }
            for (int i = 0; i < n; i++) {
                uint32_t k = cov[i];
                if (k == 255) {
                    q[i] = opaque ? p : over(p, q[i]);
                } else {
                    q[i] = over(mul8(p, k), q[i]);
                }
            }
        });
    }
}

static void translatePolys(std::vector<Path::Poly>& polys, float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    for (Path::Poly& p : polys)
        for (PointF& q : p.pts) {
            q.x += dx;
            q.y += dy;
        }
}

// --- filling ---

void Painter::fillRect(const RectF& r, Color c) {
    if (r.empty()) return;
    if (scale_ != 1) {
        float lx0 = r.x + tx_, ly0 = r.y + ty_, lx1 = r.right() + tx_, ly1 = r.bottom() + ty_;
        if (lx0 == std::floor(lx0) && ly0 == std::floor(ly0) && lx1 == std::floor(lx1) && ly1 == std::floor(ly1) &&
            std::fabs(lx0) < 1e7f && std::fabs(ly0) < 1e7f && std::fabs(lx1) < 1e7f && std::fabs(ly1) < 1e7f) {
            // whole logical pixels (1 px lines among them): sharp, as the integer rectangles
            float sx = tx_, sy = ty_;
            tx_ = ty_ = 0;
            fillRect(Rect((int)lx0, (int)ly0, (int)lx1 - (int)lx0, (int)ly1 - (int)ly0), c);
            tx_ = sx;
            ty_ = sy;
            return;
        }
        RectF d(lx0 * scale_, ly0 * scale_, (lx1 - lx0) * scale_, (ly1 - ly0) * scale_);
        Unscaled u(this);
        fillRect(d, c);
        return;
    }
    float x0 = r.x + tx_, y0 = r.y + ty_, x1 = r.right() + tx_, y1 = r.bottom() + ty_;
    if (x0 == std::floor(x0) && y0 == std::floor(y0) && x1 == std::floor(x1) && y1 == std::floor(y1) &&
        std::fabs(x0) < 1e8f && std::fabs(y0) < 1e8f && std::fabs(x1) < 1e8f && std::fabs(y1) < 1e8f) {
        float sx = tx_, sy = ty_;
        tx_ = ty_ = 0;  // fillRect(Rect) adds the (rounded) translation: already in
        fillRect(Rect((int)x0, (int)y0, (int)x1 - (int)x0, (int)y1 - (int)y0), c);
        tx_ = sx;
        ty_ = sy;
        return;
    }
    std::vector<Path::Poly> polys(1);
    polys[0].closed = true;
    polys[0].pts = {PointF(x0, y0), PointF(x1, y0), PointF(x1, y1), PointF(x0, y1)};
    rasterize(polys, c, false);
}

void Painter::fillRoundedRect(const RectF& r, float radius, Color c) {
    if (r.empty()) return;
    if (radius <= 0) {
        fillRect(r, c);
        return;
    }
    Path p;
    p.addRoundedRect(r, radius, radius);
    fillPath(p, c);
}

void Painter::fillEllipse(const RectF& r, Color c) {
    if (r.empty()) return;
    Path p;
    p.addEllipse(r);
    fillPath(p, c);
}

void Painter::fillPolygon(const std::vector<PointF>& pts, Color c) {
    if (pts.size() < 3) return;
    std::vector<Path::Poly> polys(1);
    polys[0].closed = true;
    polys[0].pts = pts;
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, false);
}

void Painter::fillPath(const Path& p, Color c) { fillPath(p, c, false); }

void Painter::fillPath(const Path& p, Color c, bool evenOdd) {
    if (p.empty()) return;
    std::vector<Path::Poly> polys = p.flatten(0.03f);
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, evenOdd);
}

// --- outlines ---

void Painter::drawRect(const Rect& r, Color c, int width) {
    if (r.empty() || width <= 0) return;
    if (width * 2 >= r.w || width * 2 >= r.h) {
        fillRect(r, c);
        return;
    }
    fillRect(Rect(r.x, r.y, r.w, width), c);
    fillRect(Rect(r.x, r.bottom() - width, r.w, width), c);
    fillRect(Rect(r.x, r.y + width, width, r.h - 2 * width), c);
    fillRect(Rect(r.right() - width, r.y + width, width, r.h - 2 * width), c);
}

void Painter::strokeRect(const RectF& r, Color c, float width) {
    if (width <= 0) return;
    float hw = width / 2;
    // the outer rectangle minus the inner one (opposite windings)
    RectF o(r.x - hw, r.y - hw, r.w + width, r.h + width);
    RectF in(r.x + hw, r.y + hw, r.w - width, r.h - width);
    float x0 = o.x + tx_, y0 = o.y + ty_, x1 = o.right() + tx_, y1 = o.bottom() + ty_;
    bool aligned = x0 == std::floor(x0) && y0 == std::floor(y0) && x1 == std::floor(x1) && y1 == std::floor(y1) &&
                   width == std::floor(width);
    if (aligned) {  // (scaled: drawRect's rectangles are scaled)
        float sx = tx_, sy = ty_;
        tx_ = ty_ = 0;
        drawRect(Rect((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)), c, (int)width);
        tx_ = sx;
        ty_ = sy;
        return;
    }
    std::vector<Path::Poly> polys(2);
    polys[0].closed = polys[1].closed = true;
    polys[0].pts = {PointF(o.x, o.y), PointF(o.right(), o.y), PointF(o.right(), o.bottom()), PointF(o.x, o.bottom())};
    if (!in.empty())
        polys[1].pts = {PointF(in.x, in.y), PointF(in.x, in.bottom()), PointF(in.right(), in.bottom()), PointF(in.right(), in.y)};
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, false);
}

void Painter::strokeRoundedRect(const RectF& r, float radius, Color c, float width) {
    if (radius <= 0) {
        strokeRect(r, c, width);
        return;
    }
    Path p;
    p.addRoundedRect(r, radius, radius);
    strokePath(p, c, width, LineCap::Flat, LineJoin::Miter);
}

void Painter::strokeEllipse(const RectF& r, Color c, float width) {
    Path p;
    p.addEllipse(r);
    strokePath(p, c, width, LineCap::Flat, LineJoin::Round);
}

void Painter::drawLine(PointF a, PointF b, Color c, float width, LineCap cap) {
    if (width <= 0) return;
    // axis-aligned lines that cover whole pixels: the fast path
    if (a.y == b.y || a.x == b.x) {
        float hw = width / 2;
        float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
        float ext = cap == LineCap::Square ? hw : 0;
        RectF box;
        if (a.y == b.y) {
            if (x0 == x1 && cap == LineCap::Flat) return;
            box = RectF(x0 - ext, a.y - hw, x1 - x0 + 2 * ext, width);
        } else {
            if (y0 == y1 && cap == LineCap::Flat) return;
            box = RectF(a.x - hw, y0 - ext, width, y1 - y0 + 2 * ext);
        }
        if (cap != LineCap::Round) {
            fillRect(box, c);
            return;
        }
    }
    std::vector<Path::Poly> lines(1);
    lines[0].pts = {a, b};
    std::vector<Path::Poly> polys;
    strokeOutline(lines, width, cap, LineJoin::Miter, polys);
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, false);
}

void Painter::drawPolyline(const std::vector<PointF>& pts, Color c, float width, LineCap cap, LineJoin join) {
    if (pts.size() < 2) return;
    std::vector<Path::Poly> lines(1);
    lines[0].pts = pts;
    std::vector<Path::Poly> polys;
    strokeOutline(lines, width, cap, join, polys);
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, false);
}

void Painter::strokePath(const Path& p, Color c, float width, LineCap cap, LineJoin join) {
    if (p.empty()) return;
    std::vector<Path::Poly> lines = p.flatten(0.03f), polys;
    strokeOutline(lines, width, cap, join, polys);
    if (scale_ != 1) toDevice(polys);
    else translatePolys(polys, tx_, ty_);
    rasterize(polys, c, false);
}

void Painter::drawDashedLine(PointF a, PointF b, Color c, float width, float dash, float gap) {
    PointF v = b - a;
    float L = len(v);
    if (L <= 0 || dash <= 0) return;
    if (gap <= 0) {
        drawLine(a, b, c, width);
        return;
    }
    PointF d(v.x / L, v.y / L);
    bool axis = a.x == b.x || a.y == b.y;
    std::vector<Path::Poly> lines;
    for (float t = 0; t < L; t += dash + gap) {
        float e = std::min(L, t + dash);
        if (axis) {
            drawLine(a + d * t, a + d * e, c, width);
        } else {
            Path::Poly p;
            p.pts = {a + d * t, a + d * e};
            lines.push_back(p);
        }
    }
    if (!lines.empty()) {
        std::vector<Path::Poly> polys;
        strokeOutline(lines, width, LineCap::Flat, LineJoin::Miter, polys);
        if (scale_ != 1) toDevice(polys);
        else translatePolys(polys, tx_, ty_);
        rasterize(polys, c, false);
    }
}

// --- images ---

void Painter::drawImage(Point at, const Image& img) {
    if (img.empty()) return;
    if (scale_ != 1) {
        drawImage(RectF((float)at.x, (float)at.y, (float)img.width(), (float)img.height()), img, img.rect());
        return;
    }
    int dx = at.x + iround(tx_), dy = at.y + iround(ty_);
    Rect d = Rect(dx, dy, img.width(), img.height()).intersected(clip_);
    if (d.empty()) return;
    uint32_t op = (uint32_t)std::lround(opacity_ * 255);
    if (!op) return;
    for (int y = d.y; y < d.bottom(); y++) {
        const uint32_t* s = img.row(y - dy) + (d.x - dx);
        uint32_t* q = img_->row(y) + d.x;
        if (op == 255) {
            for (int i = 0; i < d.w; i++) {
                uint32_t p = s[i], a = p >> 24;
                if (a == 255) q[i] = p;
                else if (a) q[i] = over(p, q[i]);
            }
        } else {
            for (int i = 0; i < d.w; i++) {
                uint32_t p = s[i];
                if (p >> 24) q[i] = over(mul8(p, op), q[i]);
            }
        }
    }
}

void Painter::drawImage(const RectF& dst, const Image& img) { drawImage(dst, img, img.rect()); }

void Painter::drawImage(const RectF& dst, const Image& img, const Rect& srcIn) {
    Rect src = srcIn.intersected(img.rect());
    if (src.empty() || dst.empty()) return;
    if (scale_ != 1) {
        float s = scale_, lx0 = dst.x + tx_, ly0 = dst.y + ty_, lx1 = dst.right() + tx_, ly1 = dst.bottom() + ty_;
        RectF d(lx0 * s, ly0 * s, dst.w * s, dst.h * s);
        // (edges on whole device pixels when they are on whole logical ones: no blurred seams)
        if (lx0 == std::floor(lx0) && ly0 == std::floor(ly0) && lx1 == std::floor(lx1) && ly1 == std::floor(ly1)) {
            int x0 = iround(lx0 * s), y0 = iround(ly0 * s), x1 = iround(lx1 * s), y1 = iround(ly1 * s);
            d = RectF((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0));
        }
        Unscaled u(this);
        drawImage(d, img, src);
        return;
    }
    float x0 = dst.x + tx_, y0 = dst.y + ty_;
    // 1:1 at whole pixels: a plain copy
    if (dst.w == src.w && dst.h == src.h && x0 == std::floor(x0) && y0 == std::floor(y0)) {
        float sx = tx_, sy = ty_;
        tx_ = ty_ = 0;
        Rect keep = clip_;
        clip_ = clip_.intersected(Rect((int)x0, (int)y0, src.w, src.h));
        drawImage(Point((int)x0 - src.x, (int)y0 - src.y), img);
        clip_ = keep;
        tx_ = sx;
        ty_ = sy;
        return;
    }
    // much smaller: shrink with the box filter first, then sample
    const Image* source = &img;
    Image shrunk;
    Rect s = src;
    if (dst.w < src.w * 0.5f || dst.h < src.h * 0.5f) {
        int w = std::max(1, (int)std::ceil(dst.w)), h = std::max(1, (int)std::ceil(dst.h));
        shrunk = img.copy(src).scaled(std::min(w, src.w), std::min(h, src.h));
        source = &shrunk;
        s = shrunk.rect();
    }
    Rect dev = RectF(x0, y0, dst.w, dst.h).toRect().intersected(clip_);
    if (dev.empty()) return;
    float kx = s.w / dst.w, ky = s.h / dst.h;
    uint32_t op = (uint32_t)std::lround(opacity_ * 255);
    for (int y = dev.y; y < dev.bottom(); y++) {
        // vertical edge coverage
        float cy = std::min((float)(y + 1), y0 + dst.h) - std::max((float)y, y0);
        if (cy <= 0) continue;
        float sy = s.y + (y + 0.5f - y0) * ky - 0.5f;
        sy = std::clamp(sy, (float)s.y, (float)(s.bottom() - 1));
        int iy = (int)sy;
        int iy1 = std::min(iy + 1, s.bottom() - 1);
        uint32_t fy = (uint32_t)((sy - iy) * 256);
        const uint32_t* r0 = source->row(iy);
        const uint32_t* r1 = source->row(iy1);
        uint32_t* q = img_->row(y);
        for (int x = dev.x; x < dev.right(); x++) {
            float cx = std::min((float)(x + 1), x0 + dst.w) - std::max((float)x, x0);
            if (cx <= 0) continue;
            float sx = s.x + (x + 0.5f - x0) * kx - 0.5f;
            sx = std::clamp(sx, (float)s.x, (float)(s.right() - 1));
            int ix = (int)sx;
            int ix1 = std::min(ix + 1, s.right() - 1);
            uint32_t fx = (uint32_t)((sx - ix) * 256);
            auto lerp = [](uint32_t a, uint32_t b, uint32_t f) {  // f 0..256
                uint32_t rb = ((a & 0x00ff00ff) * (256 - f) + (b & 0x00ff00ff) * f) >> 8 & 0x00ff00ff;
                uint32_t ag = (((a >> 8) & 0x00ff00ff) * (256 - f) + ((b >> 8) & 0x00ff00ff) * f) & 0xff00ff00;
                return rb | ag;
            };
            uint32_t p = lerp(lerp(r0[ix], r0[ix1], fx), lerp(r1[ix], r1[ix1], fx), fy);
            uint32_t k = (uint32_t)(cx * cy * op + 0.5f);
            if (k < 255) p = mul8(p, k);
            if (p >> 24) q[x] = over(p, q[x]);
        }
    }
}

// --- text ---

float Painter::drawText(PointF at, const std::string& text, const Font& font, Color c) {
    if (text.empty()) return 0;
    if (scale_ != 1) {  // the font at the device size, from the device position
        float s = scale_;
        Font big = font.withPixelSize(font.pixelSize() * s);
        PointF d((at.x + tx_) * s, (at.y + ty_) * s);
        Unscaled u(this);
        return drawText(d, text, big, c) / s;
    }
    std::vector<Font::Run> runs;
    float ox = at.x + tx_;
    float adv = font.layout(text, ox, runs);
    uint32_t p = premulWith(c, opacity_);
    if (!(p >> 24)) return adv;
    int by = (int)std::floor(at.y + ty_ + 0.5f);
    bool opaque = (p >> 24) == 255;
    for (const Font::Run& r : runs) {
        const Font::Glyph* g = r.glyph;
        if (!g || g->w <= 0 || g->h <= 0) continue;
        int gx = (int)std::floor(r.x) + g->left, gy = by + g->top;
        Rect gr = Rect(gx, gy, g->w, g->h).intersected(clip_);
        if (gr.empty()) continue;
        for (int y = gr.y; y < gr.bottom(); y++) {
            const uint8_t* m = g->mask.data() + (size_t)(y - gy) * g->w + (gr.x - gx);
            uint32_t* q = img_->row(y) + gr.x;
            for (int i = 0; i < gr.w; i++) {
                uint32_t k = m[i];
                if (!k) continue;
                if (k == 255) q[i] = opaque ? p : over(p, q[i]);
                else q[i] = over(mul8(p, k), q[i]);
            }
        }
    }
    return adv;
}

void Painter::drawText(const RectF& box, int align, const std::string& text, const Font& font, Color c, bool elide) {
    std::string shown = elide ? font.elided(text, box.w) : text;
    if (shown.empty()) return;
    float w = font.width(shown);
    float x = box.x;
    if (align & AlignRight) x = box.right() - w;
    else if (align & AlignHCenter) x = box.x + (box.w - w) / 2;
    float asc = font.ascent(), desc = font.descent();
    float y;
    if (align & AlignBottom) y = box.bottom() - desc;
    else if (align & AlignVCenter) y = box.y + (box.h - (asc + desc)) / 2 + asc;
    else y = box.y + asc;
    drawText(PointF(x, y), shown, font, c);
}

// the lines of a text block: split at '\n', wrapped at spaces to `maxWidth` when wrap is on
static std::vector<std::string> blockLines(const std::string& text, const Font& font, float maxWidth, bool wrap) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!para.empty() && para.back() == '\r') para.pop_back();
        if (!wrap || font.width(para) <= maxWidth) {
            lines.push_back(para);
        } else {
            std::string line;
            size_t i = 0;
            while (i <= para.size()) {
                size_t sp = para.find(' ', i);
                if (sp == std::string::npos) sp = para.size();
                std::string word = para.substr(i, sp - i);
                std::string cand = line.empty() ? word : line + " " + word;
                if (!line.empty() && font.width(cand) > maxWidth) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = cand;
                }
                i = sp + 1;
            }
            lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

void Painter::drawTextBlock(const RectF& box, int align, const std::string& text, const Font& font, Color c, bool wrap) {
    std::vector<std::string> lines = blockLines(text, font, box.w, wrap);
    float ls = font.lineSpacing(), asc = font.ascent(), desc = font.descent();
    float total = (lines.size() - 1) * ls + asc + desc;
    float y;
    if (align & AlignBottom) y = box.bottom() - total;
    else if (align & AlignVCenter) y = box.y + (box.h - total) / 2;
    else y = box.y;
    for (const std::string& line : lines) {
        if (!line.empty()) {
            float w = font.width(line);
            float x = box.x;
            if (align & AlignRight) x = box.right() - w;
            else if (align & AlignHCenter) x = box.x + (box.w - w) / 2;
            drawText(PointF(x, y + asc), line, font, c);
        }
        y += ls;
    }
}
