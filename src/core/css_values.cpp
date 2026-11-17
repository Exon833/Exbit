// Reading CSS values (css.h, css_internal.h): colours, lengths and calc(), var() substitution, the shorthands'
// expansion and the longhands' checks, and Style's typed getters.
//
// Values stay text from parsing to the computed Style (what raw() gives back); each getter reads its text
// when asked, with the node's font size for em. Callers keep computed styles, so this happens once per
// widget per stylesheet change, not per frame.
//
// Colours come out as 8-bit channels, every result rounded with std::nearbyint (halves to even), as the
// program's own colour code rounds. color-mix() of two opaque colours is computed exactly as the program's
// own mixColor() (gfx/geom.cpp) mixes them: per channel nearbyint(x + (y - x) * t), t the second colour's
// share as a float, so a skin's color-mix(in srgb, A 90%, B) is the very shade mixColor(A, B, 0.10f) drew
// before skins were CSS. With transparency it follows CSS Color 5: premultiplied by alpha. Colours inside
// color-mix() are rounded to 8 bits before mixing (as mixColor's inputs are).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/css_internal.h"

namespace css {
namespace detail {

namespace {

// the 148 CSS named colours (CSS Color 4 §6.1), sorted, for a binary search
struct Named {
    const char* name;
    uint32_t rgb;
};
const Named kNamed[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
    {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f},
    {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6},
    {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500},
    {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f},
    {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080},
    {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee},
    {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
};
static_assert(sizeof kNamed / sizeof kNamed[0] == 148, "the CSS named colours");

bool namedColor(std::string_view name, Rgba& out) {
    if (name.size() > 24) return false;
    std::string n = lower(name);
    const Named* end = kNamed + sizeof kNamed / sizeof kNamed[0];
    const Named* it = std::lower_bound(kNamed, end, n, [](const Named& a, const std::string& b) { return strcmp(a.name, b.c_str()) < 0; });
    if (it == end || n != it->name) return false;
    out = Rgba{(uint8_t)(it->rgb >> 16), (uint8_t)(it->rgb >> 8), (uint8_t)it->rgb, 255};
    return true;
}

uint8_t to8(double v) {  // a channel 0..255, rounded as the program rounds (halves to even)
    if (!(v > 0)) return 0;  // (NaN too)
    if (v >= 255) return 255;
    return (uint8_t)std::nearbyint(v);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexColor(const std::string& h, Rgba& out) {
    size_t n = h.size();
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    int d[8];
    for (size_t i = 0; i < n; i++)
        if ((d[i] = hexDigit(h[i])) < 0) return false;
    if (n <= 4) {
        out = Rgba{(uint8_t)(d[0] * 17), (uint8_t)(d[1] * 17), (uint8_t)(d[2] * 17), (uint8_t)(n == 4 ? d[3] * 17 : 255)};
    } else {
        out = Rgba{(uint8_t)(d[0] * 16 + d[1]), (uint8_t)(d[2] * 16 + d[3]), (uint8_t)(d[4] * 16 + d[5]),
                   (uint8_t)(n == 8 ? d[6] * 16 + d[7] : 255)};
    }
    return true;
}

bool single(const Tokens& t, std::pair<size_t, size_t> r) { return r.second == r.first + 1; }

// an rgb() channel (0..255), an alpha (0..1), a hue (degrees), an hsl percentage (0..100)
bool channel(const Tokens& t, std::pair<size_t, size_t> r, double& v) {
    if (!single(t, r)) return false;
    const Token& k = t[r.first];
    if (k.type == Tok::Number) v = k.num;
    else if (k.type == Tok::Percentage) v = k.num * 255 / 100;
    else if (k.type == Tok::Ident && ieq(k.text, "none")) v = 0;
    else return false;
    v = std::clamp(v, 0.0, 255.0);
    return true;
}
bool alphaValue(const Tokens& t, std::pair<size_t, size_t> r, double& v) {
    if (!single(t, r)) return false;
    const Token& k = t[r.first];
    if (k.type == Tok::Number) v = k.num;
    else if (k.type == Tok::Percentage) v = k.num / 100;
    else if (k.type == Tok::Ident && ieq(k.text, "none")) v = 0;
    else return false;
    v = std::clamp(v, 0.0, 1.0);
    return true;
}
bool hue(const Tokens& t, std::pair<size_t, size_t> r, double& deg) {
    if (!single(t, r)) return false;
    const Token& k = t[r.first];
    if (k.type == Tok::Number) deg = k.num;
    else if (k.type == Tok::Ident && ieq(k.text, "none")) deg = 0;
    else if (k.type == Tok::Dimension && k.text == "deg") deg = k.num;
    else if (k.type == Tok::Dimension && k.text == "rad") deg = k.num * 180 / 3.14159265358979323846;
    else if (k.type == Tok::Dimension && k.text == "grad") deg = k.num * 0.9;
    else if (k.type == Tok::Dimension && k.text == "turn") deg = k.num * 360;
    else return false;
    if (!std::isfinite(deg)) return false;
    deg = std::fmod(deg, 360.0);
    if (deg < 0) deg += 360;
    return true;
}
bool percentage(const Tokens& t, std::pair<size_t, size_t> r, double& v) {
    if (!single(t, r)) return false;
    const Token& k = t[r.first];
    if (k.type == Tok::Percentage || k.type == Tok::Number) v = k.num;
    else if (k.type == Tok::Ident && ieq(k.text, "none")) v = 0;
    else return false;
    v = std::clamp(v, 0.0, 100.0);
    return true;
}

// a colour function's arguments: three values and maybe an alpha, with commas (legacy) or spaces and '/'
bool colorArgs(const Tokens& t, size_t b, size_t e, std::vector<std::pair<size_t, size_t>>& vals,
               std::pair<size_t, size_t>& alpha, bool& hasAlpha) {
    auto pieces = splitTop(t, b, e, Tok::Comma);
    hasAlpha = false;
    if (pieces.size() > 1) {
        if (pieces.size() != 3 && pieces.size() != 4) return false;
        for (size_t i = 0; i < 3; i++) vals.push_back(pieces[i]);
        if (pieces.size() == 4) {
            alpha = pieces[3];
            hasAlpha = true;
        }
        return true;
    }
    auto comps = components(t, b, e);
    bool slash = comps.size() > 3 && single(t, comps[3]) && t[comps[3].first].type == Tok::Delim && t[comps[3].first].delim == '/';
    if (slash) {
        if (comps.size() != 5) return false;
        alpha = comps[4];
        hasAlpha = true;
    } else if (comps.size() != 3) {
        return false;
    }
    for (size_t i = 0; i < 3; i++) vals.push_back(comps[i]);
    return true;
}

bool colorAt(const Tokens& t, size_t b, size_t e, const Rgba* current, Rgba& out, int depth);

bool mixArgs(const Tokens& t, size_t b, size_t e, const Rgba* current, Rgba& out, int depth) {
    auto pieces = splitTop(t, b, e, Tok::Comma);
    if (pieces.size() != 3) return false;
    auto space = components(t, pieces[0].first, pieces[0].second);
    if (space.size() != 2 || !single(t, space[0]) || !single(t, space[1])) return false;
    const Token& in = t[space[0].first];
    const Token& sp = t[space[1].first];
    if (in.type != Tok::Ident || !ieq(in.text, "in") || sp.type != Tok::Ident || !ieq(sp.text, "srgb")) return false;
    Rgba c[2];
    double pct[2];
    bool given[2];
    for (int k = 0; k < 2; k++) {
        auto [pb, pe] = pieces[k + 1];
        auto comps = components(t, pb, pe);
        if (comps.empty()) return false;
        given[k] = false;
        size_t cb = pb, ce = pe;
        if (single(t, comps.front()) && t[comps.front().first].type == Tok::Percentage) {
            given[k] = true;
            pct[k] = t[comps.front().first].num;
            cb = comps.size() > 1 ? comps[1].first : pe;
        } else if (single(t, comps.back()) && t[comps.back().first].type == Tok::Percentage) {
            given[k] = true;
            pct[k] = t[comps.back().first].num;
            ce = comps.back().first;
        }
        if (!colorAt(t, cb, ce, current, c[k], depth + 1)) return false;
        if (given[k] && (pct[k] < 0 || pct[k] > 100)) return false;
    }
    // CSS Color 5 §2.1: the percentages normalised
    if (!given[0] && !given[1]) pct[0] = pct[1] = 50;
    else if (!given[0]) pct[0] = 100 - pct[1];
    else if (!given[1]) pct[1] = 100 - pct[0];
    double sum = pct[0] + pct[1];
    if (!(sum > 0)) return false;
    double alphaScale = sum < 100 ? sum / 100 : 1;
    double p = pct[0] / sum, q = pct[1] / sum;
    if (c[0].a == 255 && c[1].a == 255) {
        // as mixColor(c0, c1, t) mixes (see the top): the very same arithmetic, the same rounding
        float tf = (float)q;
        auto m = [tf](int x, int y) { return to8(std::nearbyint(x + (y - x) * (double)tf)); };
        out = Rgba{m(c[0].r, c[1].r), m(c[0].g, c[1].g), m(c[0].b, c[1].b), to8(255 * alphaScale)};
        return true;
    }
    double a0 = c[0].a / 255.0, a1 = c[1].a / 255.0;
    double a = a0 * p + a1 * q;
    auto ch = [&](int x, int y) {
        if (a <= 0) return to8(x * p + y * q);
        return to8((x * a0 * p + y * a1 * q) / a);
    };
    out = Rgba{ch(c[0].r, c[1].r), ch(c[0].g, c[1].g), ch(c[0].b, c[1].b), to8(a * alphaScale * 255)};
    return true;
}

bool colorAt(const Tokens& t, size_t b, size_t e, const Rgba* current, Rgba& out, int depth) {
    t.trim(b, e);
    if (b >= e || depth > 16) return false;
    const Token& k = t[b];
    if (e == b + 1) {
        if (k.type == Tok::Hash) return hexColor(k.text, out);
        if (k.type != Tok::Ident) return false;
        if (ieq(k.text, "transparent")) {
            out = Rgba{0, 0, 0, 0};
            return true;
        }
        if (ieq(k.text, "currentcolor")) {
            if (!current) return false;
            out = *current;
            return true;
        }
        return namedColor(k.text, out);
    }
    if (k.type != Tok::Function || k.close != e - 1) return false;
    std::string fn = lower(k.text);
    if (fn == "color-mix") return mixArgs(t, b + 1, e - 1, current, out, depth);
    std::vector<std::pair<size_t, size_t>> vals;
    std::pair<size_t, size_t> alphaR{0, 0};
    bool hasAlpha = false;
    if (fn != "rgb" && fn != "rgba" && fn != "hsl" && fn != "hsla") return false;
    if (!colorArgs(t, b + 1, e - 1, vals, alphaR, hasAlpha)) return false;
    double alpha = 1;
    if (hasAlpha && !alphaValue(t, alphaR, alpha)) return false;
    if (fn[0] == 'r') {
        double r, g, bl;
        if (!channel(t, vals[0], r) || !channel(t, vals[1], g) || !channel(t, vals[2], bl)) return false;
        out = Rgba{to8(r), to8(g), to8(bl), to8(alpha * 255)};
        return true;
    }
    double h, s, l;
    if (!hue(t, vals[0], h) || !percentage(t, vals[1], s) || !percentage(t, vals[2], l)) return false;
    s /= 100;
    l /= 100;
    // CSS Color 4 §7.1
    auto f = [&](double n) {
        double kk = std::fmod(n + h / 30, 12.0);
        double a = s * std::min(l, 1 - l);
        return (l - a * std::max(-1.0, std::min({kk - 3, 9 - kk, 1.0}))) * 255;
    };
    out = Rgba{to8(f(0)), to8(f(8)), to8(f(4)), to8(alpha * 255)};
    return true;
}

// --- calc() ---
struct Quantity {
    double v = 0;
    bool length = false;  // else a number
};

struct Calc {
    const Tokens& t;
    const LenCtx& ctx;
    bool percentIsNumber;  // number(): 50% is 0.5

    void ws(size_t& i, size_t e) const {
        while (i < e && t[i].type == Tok::Whitespace) i++;
    }
    bool term(size_t& i, size_t e, Quantity& q, int depth) const {
        ws(i, e);
        if (i >= e) return false;
        const Token& k = t[i];
        if (k.type == Tok::Number) {
            q = {k.num, false};
        } else if (k.type == Tok::Percentage) {
            if (percentIsNumber) q = {k.num / 100, false};
            else if (ctx.percent) q = {k.num * ctx.percentOf / 100, true};
            else return false;
        } else if (k.type == Tok::Dimension) {
            if (k.text == "px") q = {k.num, true};
            else if (k.text == "pt") q = {k.num * 96 / 72, true};
            else if (k.text == "em") q = {k.num * ctx.em, true};
            else return false;
        } else if (k.type == Tok::LParen || (k.type == Tok::Function && ieq(k.text, "calc"))) {
            if (depth > 32 || k.close >= e) return false;
            size_t j = i + 1;
            if (!sum(j, k.close, q, depth + 1)) return false;
            ws(j, k.close);
            if (j != k.close) return false;
            i = k.close + 1;
            return true;
        } else {
            return false;
        }
        i++;
        return true;
    }
    bool product(size_t& i, size_t e, Quantity& q, int depth) const {
        if (!term(i, e, q, depth)) return false;
        for (;;) {
            size_t j = i;
            ws(j, e);
            if (j >= e || t[j].type != Tok::Delim || (t[j].delim != '*' && t[j].delim != '/')) return true;
            char op = t[j].delim;
            j++;
            Quantity r;
            if (!term(j, e, r, depth)) return false;
            if (op == '*') {
                if (q.length && r.length) return false;
                q = {q.v * r.v, q.length || r.length};
            } else {
                if (r.length || r.v == 0) return false;
                q.v /= r.v;
            }
            i = j;
        }
    }
    bool sum(size_t& i, size_t e, Quantity& q, int depth) const {
        if (!product(i, e, q, depth)) return false;
        for (;;) {
            size_t j = i;
            ws(j, e);
            if (j >= e || t[j].type != Tok::Delim || (t[j].delim != '+' && t[j].delim != '-')) return true;
            if (j == i || j + 1 >= e || t[j + 1].type != Tok::Whitespace) return false;  // (spaces around + and -)
            char op = t[j].delim;
            j++;
            Quantity r;
            if (!product(j, e, r, depth)) return false;
            if (r.length != q.length) return false;
            q.v = op == '+' ? q.v + r.v : q.v - r.v;
            i = j;
        }
    }
    // [b, e) is exactly one value
    bool whole(size_t b, size_t e, Quantity& q) const {
        t.trim(b, e);
        if (b >= e) return false;
        const Token& k = t[b];
        if (k.type == Tok::LParen) return false;  // (a bare parenthesis isn't a value; calc() is)
        size_t i = b;
        if (!term(i, e, q, 0) || i != e) return false;
        return std::isfinite(q.v);
    }
};

std::string sideName(const std::string& prefix, const char* side) {
    // "padding" -> padding-top; "border-width" -> border-top-width
    if (prefix.compare(0, 7, "border-") == 0) return "border-" + std::string(side) + prefix.substr(6);
    return prefix + "-" + side;
}

const char* const kSides[] = {"top", "right", "bottom", "left"};
const char* const kCorners[] = {"top-left", "top-right", "bottom-right", "bottom-left"};

bool isBorderStyle(std::string_view s) {
    static const char* const styles[] = {"none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"};
    for (const char* x : styles)
        if (ieq(s, x)) return true;
    return false;
}
bool isWidthKeyword(std::string_view s) { return ieq(s, "thin") || ieq(s, "medium") || ieq(s, "thick"); }

enum class Kind { Length, LengthAuto, Width, Style, Color, Radius };

bool isKind(const Tokens& t, size_t b, size_t e, Kind kind) {
    t.trim(b, e);
    if (b >= e) return false;
    bool ident = e == b + 1 && t[b].type == Tok::Ident;
    LenCtx ctx;
    ctx.percentOf = 100;
    double px;
    switch (kind) {
        case Kind::Length:
        case Kind::Radius: return evalLength(t, b, e, ctx, px) && px >= 0;
        case Kind::LengthAuto: return (ident && ieq(t[b].text, "auto")) || evalLength(t, b, e, ctx, px);
        case Kind::Width: return (ident && isWidthKeyword(t[b].text)) || (evalLength(t, b, e, ctx, px) && px >= 0);
        case Kind::Style: return ident && isBorderStyle(t[b].text);
        case Kind::Color: {
            Rgba dummy, out;
            return evalColor(t, b, e, &dummy, out);
        }
    }
    return false;
}

std::map<std::string, std::vector<std::string>> makeLonghands() {
    std::map<std::string, std::vector<std::string>> m;
    for (const char* p : {"padding", "margin", "border-width", "border-style", "border-color"})
        for (const char* s : kSides) m[p].push_back(sideName(p, s));
    for (const char* c : kCorners) m["border-radius"].push_back(std::string("border-") + c + "-radius");
    for (const char* s : kSides) {
        std::string side = std::string("border-") + s;
        for (const char* what : {"-width", "-style", "-color"}) {
            m[side].push_back(side + what);
            m["border"].push_back(side + what);
        }
    }
    m["background"] = {"background-color"};
    return m;
}
const std::map<std::string, std::vector<std::string>>& longhandTable() {
    static const std::map<std::string, std::vector<std::string>> m = makeLonghands();
    return m;
}

void formatInto(std::string& out, double v) {
    // locale-free and short: up to 6 decimals, trailing zeros off
    if (!std::isfinite(v)) v = 0;
    if (v < 0) {
        out += '-';
        v = -v;
    }
    if (v >= 1e15) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f", v);
        out += buf;
        return;
    }
    long long scaled = (long long)std::llround(v * 1e6);
    long long whole = scaled / 1000000, frac = scaled % 1000000;
    out += std::to_string(whole);
    if (frac) {
        char buf[8];
        snprintf(buf, sizeof buf, "%06lld", frac);
        std::string f = buf;
        while (!f.empty() && f.back() == '0') f.pop_back();
        out += '.';
        out += f;
    }
}

}  // namespace

bool evalLength(const Tokens& t, size_t b, size_t e, const LenCtx& ctx, double& px) {
    t.trim(b, e);
    if (e == b + 1 && t[b].type == Tok::Number && t[b].num == 0) {
        px = 0;
        return true;
    }
    Quantity q;
    if (!Calc{t, ctx, false}.whole(b, e, q) || !q.length) return false;
    px = q.v;
    return true;
}

bool lengthText(std::string_view text, const LenCtx& ctx, double& px) {
    Tokens t;
    tokenize(text, t);
    return evalLength(t, 0, t.size(), ctx, px);
}

bool evalNumber(const Tokens& t, size_t b, size_t e, double& v) {
    LenCtx ctx;
    Quantity q;
    if (!Calc{t, ctx, true}.whole(b, e, q) || q.length) return false;
    v = q.v;
    return true;
}

bool evalColor(const Tokens& t, size_t b, size_t e, const Rgba* current, Rgba& out) {
    return colorAt(t, b, e, current, out, 0);
}

bool colorText(std::string_view text, const Rgba* current, Rgba& out) {
    if (text.size() > 4096) return false;
    Tokens t;
    tokenize(text, t);
    return evalColor(t, 0, t.size(), current, out);
}

std::string formatNumber(double v) {
    std::string s;
    formatInto(s, v);
    return s;
}

// --- var() ---
bool substitute(std::string_view text, const VarLookup& lookup, std::string& out, std::string& why, int depth) {
    const size_t kMax = 64 * 1024;
    if (depth > 32) {
        why = "var() nested too deeply";
        return false;
    }
    Tokens t;
    tokenize(text, t);
    size_t n = t.size(), b = 0, e = n;
    t.trim(b, e);
    std::string r;
    for (size_t i = b; i < e;) {
        const Token& k = t[i];
        if (k.type == Tok::Whitespace) {
            r += ' ';
            i++;
            continue;
        }
        if (i > b && t[i - 1].type != Tok::Whitespace && t[i - 1].end != k.start) r += ' ';
        if (k.type == Tok::Function && ieq(k.text, "var")) {
            size_t close = k.close;
            if (close >= n) {
                why = "an unclosed var()";
                return false;
            }
            size_t j = i + 1;
            while (j < close && t[j].type == Tok::Whitespace) j++;
            if (j >= close || t[j].type != Tok::Ident || t[j].text.compare(0, 2, "--") != 0) {
                why = "a malformed var()";
                return false;
            }
            const std::string& name = t[j].text;
            j++;
            while (j < close && t[j].type == Tok::Whitespace) j++;
            bool hasFallback = false;
            if (j < close) {
                if (t[j].type != Tok::Comma) {
                    why = "a malformed var(" + name + " ...)";
                    return false;
                }
                hasFallback = true;
                j++;
            }
            if (const std::string* v = lookup(name)) {
                r += *v;
            } else if (hasFallback) {
                std::string fb;
                if (j < close) {
                    std::string_view raw = t.src.substr(t[j].start, t[close - 1].end - t[j].start);
                    if (!substitute(raw, lookup, fb, why, depth + 1)) return false;
                }
                r += fb;
            } else {
                why = "var(" + name + ") has no value";
                return false;
            }
            i = close + 1;
        } else {
            r.append(t.src.data() + k.start, k.end - k.start);
            i++;
        }
        if (r.size() > kMax) {
            why = "a value too long after var()";
            return false;
        }
    }
    size_t s0 = r.find_first_not_of(' ');
    if (s0 == std::string::npos) r.clear();
    else r = r.substr(s0, r.find_last_not_of(' ') - s0 + 1);
    out = std::move(r);
    return true;
}

// --- properties ---
bool isShorthand(const std::string& name) { return longhandTable().count(name) != 0; }

const std::vector<std::string>& longhands(const std::string& shorthand) {
    auto& m = longhandTable();
    auto it = m.find(shorthand);
    static const std::vector<std::string> none;
    return it != m.end() ? it->second : none;
}

bool isInherited(const std::string& p) {
    return p == "color" || p == "font-family" || p == "font-size" || p == "font-weight" || p == "font-style" ||
           p == "letter-spacing";
}

std::string expand(const std::string& name, const Tokens& t, size_t b, size_t e,
                   std::vector<std::pair<std::string, std::string>>& out) {
    t.trim(b, e);
    const std::vector<std::string>& ls = longhands(name);
    std::string whole = t.text(b, e);
    if (b >= e) return "no value for '" + name + "'";
    if (e == b + 1 && t[b].type == Tok::Ident && isCssWide(t[b].text)) {
        if (name == "background") out.emplace_back("background", whole);
        for (const std::string& l : ls) out.emplace_back(l, whole);
        return "";
    }
    auto bad = [&]() { return "a bad value for '" + name + "': '" + (whole.size() > 60 ? whole.substr(0, 57) + "..." : whole) + "'"; };
    if (name == "background") {
        auto layers = splitTop(t, b, e, Tok::Comma);
        auto [lb, le] = layers.back();
        std::string color = "transparent";
        for (auto [cb, ce] : components(t, lb, le))
            if (isKind(t, cb, ce, Kind::Color)) color = t.text(cb, ce);
        out.emplace_back("background", whole);
        out.emplace_back("background-color", color);
        return "";
    }
    auto comps = components(t, b, e);
    if (name == "border" || name.compare(0, 7, "border-") == 0) {
        bool border = name == "border" || name == "border-top" || name == "border-right" || name == "border-bottom" ||
                      name == "border-left";
        if (border) {
            std::string width, style, color;
            if (comps.size() > 3) return bad();
            for (auto [cb, ce] : comps) {
                if (width.empty() && isKind(t, cb, ce, Kind::Width)) width = t.text(cb, ce);
                else if (style.empty() && isKind(t, cb, ce, Kind::Style)) style = t.text(cb, ce);
                else if (color.empty() && isKind(t, cb, ce, Kind::Color)) color = t.text(cb, ce);
                else return bad();
            }
            // (what's left out takes its initial value, as in CSS)
            if (width.empty()) width = "medium";
            if (style.empty()) style = "none";
            if (color.empty()) color = "currentcolor";
            for (size_t i = 0; i < ls.size(); i += 3) {
                out.emplace_back(ls[i], width);
                out.emplace_back(ls[i + 1], style);
                out.emplace_back(ls[i + 2], color);
            }
            return "";
        }
    }
    Kind kind = Kind::Length;
    if (name == "margin") kind = Kind::LengthAuto;
    else if (name == "border-width") kind = Kind::Width;
    else if (name == "border-style") kind = Kind::Style;
    else if (name == "border-color") kind = Kind::Color;
    else if (name == "border-radius") kind = Kind::Radius;
    if (name == "border-radius")
        for (size_t i = b; i < e; i++)
            if (t[i].type == Tok::Delim && t[i].delim == '/') return "elliptical radii ('/') are not supported";
    if (comps.empty() || comps.size() > 4) return "'" + name + "' takes 1 to 4 values";
    for (auto [cb, ce] : comps)
        if (!isKind(t, cb, ce, kind)) return bad();
    // CSS's 1-4 value rule: top right bottom left (corners: top-left top-right bottom-right bottom-left)
    static const int pick[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (size_t i = 0; i < 4; i++) {
        auto [cb, ce] = comps[pick[comps.size() - 1][i]];
        out.emplace_back(ls[i], t.text(cb, ce));
    }
    return "";
}

std::string checkValue(const std::string& p, const Tokens& t, size_t b, size_t e) {
    t.trim(b, e);
    if (b >= e) return "no value for '" + p + "'";
    if (e == b + 1 && t[b].type == Tok::Ident && isCssWide(t[b].text)) return "";
    std::string whole = t.text(b, e);
    if (whole.size() > 60) whole = whole.substr(0, 57) + "...";
    auto bad = [&]() { return "a bad value for '" + p + "': '" + whole + "'"; };
    auto ends = [&](const char* s) {
        size_t n = strlen(s);
        return p.size() >= n && p.compare(p.size() - n, n, s) == 0;
    };
    auto starts = [&](const char* s) { return p.compare(0, strlen(s), s) == 0; };
    if (p == "color" || ends("-color")) {
        if (isKind(t, b, e, Kind::Color)) return "";
        if (e == b + 1 && t[b].type == Tok::Ident) return "unknown colour '" + whole + "'";
        return "'" + whole + "' is not a colour (" + p + ")";
    }
    bool side = ends("-top") || ends("-right") || ends("-bottom") || ends("-left");
    if (starts("padding-") && side) return isKind(t, b, e, Kind::Length) ? "" : bad();
    if (starts("margin-") && side) return isKind(t, b, e, Kind::LengthAuto) ? "" : bad();
    if (starts("border-") && ends("-radius")) {
        if (components(t, b, e).size() > 1) return "elliptical radii are not supported ('" + p + "')";
        return isKind(t, b, e, Kind::Radius) ? "" : bad();
    }
    if (starts("border-") && ends("-width") && p != "border-width") return isKind(t, b, e, Kind::Width) ? "" : bad();
    if (starts("border-") && ends("-style") && p != "border-style") return isKind(t, b, e, Kind::Style) ? "" : bad();
    if (p == "font-size")
        return (e == b + 1 && t[b].type == Tok::Ident) || isKind(t, b, e, Kind::Length) ? "" : bad();
    if (p == "letter-spacing") {
        LenCtx ctx;
        double px;
        return (e == b + 1 && t[b].type == Tok::Ident && ieq(t[b].text, "normal")) || evalLength(t, b, e, ctx, px) ? "" : bad();
    }
    return "";
}

}  // namespace detail

// --- the public value helpers ---
bool parseColor(const std::string& text, Rgba& out) {
    try {
        return detail::colorText(text, nullptr, out);
    } catch (...) {
        return false;
    }
}

std::string hex(const Rgba& c) {
    char buf[12];
    if (c.a == 255) snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    else snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

// --- Style's getters ---
bool Style::has(const std::string& property) const {
    if (property.compare(0, 2, "--") == 0) return vars_.count(property) != 0;
    return props_.count(property) != 0;
}

std::string Style::raw(const std::string& property) const {
    const auto& m = property.compare(0, 2, "--") == 0 ? vars_ : props_;
    auto it = m.find(property);
    return it != m.end() ? it->second : std::string();
}

Rgba Style::color(const std::string& property, Rgba def) const {
    auto it = props_.find(property);
    if (it == props_.end()) return def;
    Rgba current, out;
    bool haveCurrent = false;
    if (property != "color") {
        auto c = props_.find("color");
        haveCurrent = c != props_.end() && detail::colorText(c->second, nullptr, current);
    }
    return detail::colorText(it->second, haveCurrent ? &current : nullptr, out) ? out : def;
}

double Style::length(const std::string& property, double def, double percentOf) const {
    auto it = props_.find(property);
    if (it == props_.end()) return def;
    const std::string& v = it->second;
    size_t n = property.size();
    if (n > 6 && property.compare(n - 6, 6, "-width") == 0 &&
        (property.compare(0, 7, "border-") == 0 || property.compare(0, 8, "outline-") == 0)) {
        if (detail::ieq(v, "thin")) return 1;
        if (detail::ieq(v, "medium")) return 3;
        if (detail::ieq(v, "thick")) return 5;
    }
    detail::LenCtx ctx;
    ctx.em = emPx_;
    ctx.percentOf = percentOf;
    double px;
    return detail::lengthText(v, ctx, px) ? px : def;
}

double Style::number(const std::string& property, double def) const {
    auto it = props_.find(property);
    if (it == props_.end()) return def;
    if (property == "font-weight") {
        if (detail::ieq(it->second, "normal")) return 400;
        if (detail::ieq(it->second, "bold")) return 700;
    }
    detail::Tokens t;
    detail::tokenize(it->second, t);
    double v;
    return detail::evalNumber(t, 0, t.size(), v) ? v : def;
}

std::string Style::keyword(const std::string& property, const std::string& def) const {
    auto it = props_.find(property);
    if (it == props_.end() || it->second.empty()) return def;
    return detail::lower(it->second);
}

std::vector<std::string> Style::list(const std::string& property) const {
    std::vector<std::string> r;
    std::string v = raw(property);
    if (v.empty()) return r;
    detail::Tokens t;
    detail::tokenize(v, t);
    for (auto [b, e] : detail::splitTop(t, 0, t.size(), detail::Tok::Comma)) {
        if (b >= e) continue;
        if (e == b + 1 && t[b].type == detail::Tok::String) r.push_back(t[b].text);
        else r.push_back(t.text(b, e));
    }
    return r;
}

Edges Style::edges(const std::string& prefix, const Edges& def) const {
    Edges r;
    r.top = length(detail::sideName(prefix, "top"), def.top);
    r.right = length(detail::sideName(prefix, "right"), def.right);
    r.bottom = length(detail::sideName(prefix, "bottom"), def.bottom);
    r.left = length(detail::sideName(prefix, "left"), def.left);
    return r;
}

Edges Style::radii(const Edges& def) const {
    Edges r;
    r.top = length("border-top-left-radius", def.top);
    r.right = length("border-top-right-radius", def.right);
    r.bottom = length("border-bottom-right-radius", def.bottom);
    r.left = length("border-bottom-left-radius", def.left);
    return r;
}

double Style::fontSizePx(double def) const {
    auto it = props_.find("font-size");
    if (it == props_.end()) return def;
    detail::LenCtx ctx;
    ctx.em = emPx_;
    ctx.percentOf = emPx_;
    double px;
    return detail::lengthText(it->second, ctx, px) ? px : def;
}

std::string unquote(const std::string& value) {
    detail::Tokens t;
    detail::tokenize(value, t);
    size_t b = 0, e = t.size();
    t.trim(b, e);
    if (e == b + 1 && t[b].type == detail::Tok::String) return t[b].text;
    return b < e ? t.text(b, e) : std::string();
}

std::optional<std::string> Style::variable(const std::string& name) const {
    auto it = vars_.find(name.compare(0, 2, "--") == 0 ? name : "--" + name);
    if (it == vars_.end()) return std::nullopt;
    return it->second;
}

}  // namespace css
