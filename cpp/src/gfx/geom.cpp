// Colours: hex text, HSV/HSL (QColor's semantics), lighter/darker, WCAG luminance and contrast.
#include "gfx/geom.h"

#include <cstdio>

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool fuzzyNull(float v) { return std::fabs(v) <= 0.00001f; }
inline bool fuzzyEq(float a, float b) { return std::fabs(a - b) * 100000.f <= std::min(std::fabs(a), std::fabs(b)); }

// QColor keeps 16 bits per channel; 8-bit values are stored as v * 0x101 and read back rounded.
inline int to16(int v8) { return v8 * 0x101; }
inline int to8(int v16) { return (v16 - (v16 >> 8) + 0x80) >> 8; }  // qt_div_257

struct Hsv16 { int h, s, v; };  // h in 1/100 degree (65535: achromatic), s/v 0..65535

Hsv16 toHsv16(const Color& c) {
    float r = to16(c.r) / 65535.f, g = to16(c.g) / 65535.f, b = to16(c.b) / 65535.f;
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    float delta = mx - mn;
    Hsv16 o;
    o.v = (int)std::lround(mx * 65535.f);
    if (fuzzyNull(delta)) {
        o.h = 65535;
        o.s = 0;
    } else {
        float hue = 0;
        o.s = (int)std::lround(delta / mx * 65535.f);
        if (fuzzyEq(r, mx)) hue = (g - b) / delta;
        else if (fuzzyEq(g, mx)) hue = 2.f + (b - r) / delta;
        else hue = 4.f + (r - g) / delta;
        hue *= 60.f;
        if (hue < 0.f) hue += 360.f;
        o.h = (int)std::lround(hue * 100.f);
    }
    return o;
}

Color fromHsv16(const Hsv16& c, int alpha) {
    if (c.s == 0 || c.h == 65535) {
        int v = to8(c.v);
        return Color(v, v, v, alpha);
    }
    float h = c.h == 36000 ? 0.f : c.h / 6000.f;
    float s = c.s / 65535.f, v = c.v / 65535.f;
    int i = (int)h;
    float f = h - i, p = v * (1.f - s);
    float r = 0, g = 0, b = 0;
    if (i & 1) {
        float q = v * (1.f - s * f);
        switch (i) {
            case 1: r = q; g = v; b = p; break;
            case 3: r = p; g = q; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    } else {
        float t = v * (1.f - s * (1.f - f));
        switch (i) {
            case 0: r = v; g = t; b = p; break;
            case 2: r = p; g = v; b = t; break;
            default: r = t; g = p; b = v; break;
        }
    }
    auto ch = [](float x) { return to8((int)std::lround(std::clamp(x, 0.f, 1.f) * 65535.f)); };
    return Color(ch(r), ch(g), ch(b), alpha);
}

float hueOf(float r, float g, float b, float mx, float delta) {
    float hue;
    if (fuzzyEq(r, mx)) hue = (g - b) / delta;
    else if (fuzzyEq(g, mx)) hue = 2.f + (b - r) / delta;
    else hue = 4.f + (r - g) / delta;
    hue *= 60.f;
    if (hue < 0.f) hue += 360.f;
    return hue;
}

}  // namespace

Color Color::fromHex(const std::string& text, bool* ok) {
    if (ok) *ok = false;
    if (text.size() != 7 && text.size() != 9) return Color();
    if (text[0] != '#') return Color();
    int v[4] = {0, 0, 0, 255};
    for (size_t i = 1, k = 0; i < text.size(); i += 2, k++) {
        int hi = hexDigit(text[i]), lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) return Color();
        v[k] = hi * 16 + lo;
    }
    if (ok) *ok = true;
    return Color(v[0], v[1], v[2], v[3]);
}

std::string Color::hexRgb() const {
    char buf[16];
    snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

std::string Color::hex() const {
    if (a == 255) return hexRgb();
    char buf[16];
    snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
    return buf;
}

float Color::lightnessF() const {
    int mx = std::max<int>(r, std::max<int>(g, b)), mn = std::min<int>(r, std::min<int>(g, b));
    return (mx + mn) / 510.f;
}

float Color::luminance() const {
    auto ch = [](uint8_t v8) {
        double v = v8 / 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return (float)(0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b));
}

void Color::toHsv(float& h, float& s, float& v) const {
    float R = r / 255.f, G = g / 255.f, B = b / 255.f;
    float mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B));
    float delta = mx - mn;
    v = mx;
    if (fuzzyNull(delta)) {
        h = -1;
        s = 0;
        return;
    }
    s = delta / mx;
    h = hueOf(R, G, B, mx, delta);
}

Color Color::fromHsv(float h, float s, float v, int alpha) {
    s = std::clamp(s, 0.f, 1.f);
    v = std::clamp(v, 0.f, 1.f);
    Hsv16 c;
    if (h < 0) {
        c.h = 65535;
    } else {
        h = std::fmod(h, 360.f);
        c.h = (int)std::lround(h * 100.f);
    }
    c.s = (int)std::lround(s * 65535.f);
    c.v = (int)std::lround(v * 65535.f);
    return fromHsv16(c, alpha);
}

void Color::toHsl(float& h, float& s, float& l) const {
    float R = r / 255.f, G = g / 255.f, B = b / 255.f;
    float mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B));
    float delta = mx - mn, delta2 = mx + mn;
    l = 0.5f * delta2;
    if (fuzzyNull(delta)) {
        h = -1;
        s = 0;
        return;
    }
    s = l < 0.5f ? delta / delta2 : delta / (2.f - delta2);
    h = hueOf(R, G, B, mx, delta);
}

Color Color::fromHsl(float h, float s, float l, int alpha) {
    s = std::clamp(s, 0.f, 1.f);
    l = std::clamp(l, 0.f, 1.f);
    auto ch = [](float x) { return (int)std::lround(std::clamp(x, 0.f, 1.f) * 255.f); };
    if (s == 0 || h < 0) {
        int v = ch(l);
        return Color(v, v, v, alpha);
    }
    h = std::fmod(h, 360.f) / 360.f;
    float temp2 = l < 0.5f ? l * (1.f + s) : l + s - l * s;
    float temp1 = 2.f * l - temp2;
    float t3[3] = {h + 1.f / 3.f, h, h - 1.f / 3.f};
    float out[3];
    for (int i = 0; i < 3; i++) {
        float t = t3[i];
        if (t < 0) t += 1.f;
        else if (t > 1) t -= 1.f;
        if (6.f * t < 1.f) out[i] = temp1 + (temp2 - temp1) * 6.f * t;
        else if (2.f * t < 1.f) out[i] = temp2;
        else if (3.f * t < 2.f) out[i] = temp1 + (temp2 - temp1) * (2.f / 3.f - t) * 6.f;
        else out[i] = temp1;
    }
    return Color(ch(out[0]), ch(out[1]), ch(out[2]), alpha);
}

Color Color::lighter(int factor) const {
    if (factor <= 0) return *this;
    if (factor < 100) return darker(10000 / factor);
    Hsv16 c = toHsv16(*this);
    int s = c.s;
    unsigned v = (unsigned)c.v;
    v = (unsigned)factor * v / 100;
    if (v > 65535) {
        s -= (int)(v - 65535);
        if (s < 0) s = 0;
        v = 65535;
    }
    c.s = s;
    c.v = (int)v;
    return fromHsv16(c, a);
}

Color Color::darker(int factor) const {
    if (factor <= 0) return *this;
    if (factor < 100) return lighter(10000 / factor);
    Hsv16 c = toHsv16(*this);
    c.v = c.v * 100 / factor;
    return fromHsv16(c, a);
}

Color mixColor(const Color& a, const Color& b, float t) {
    auto m = [t](int x, int y) {
        double v = std::nearbyint(x + (y - x) * (double)t);  // Python's round(): halves to even
        return (int)std::clamp(v, 0.0, 255.0);
    };
    return Color(m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255);
}

Color blendOver(const Color& top, const Color& under) {
    float ta = top.a / 255.f, ua = under.a / 255.f;
    float oa = ta + ua * (1 - ta);
    if (oa <= 0) return Color(0, 0, 0, 255);
    auto ch = [&](int t, int u) {
        return (int)std::lround(std::clamp((t * ta + u * ua * (1 - ta)) / oa, 0.f, 255.f));
    };
    return Color(ch(top.r, under.r), ch(top.g, under.g), ch(top.b, under.b), 255);
}

float contrastRatio(const Color& a, const Color& b) {
    float la = a.luminance(), lb = b.luminance();
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
