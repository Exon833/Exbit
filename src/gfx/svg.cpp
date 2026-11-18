// A small SVG renderer (see svg.h): a minimal XML reader, styles and transforms, path data, and
// drawing with the Painter.
#include "gfx/svg.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "gfx/painter.h"

namespace {

// ------------------------------------------------------------------------------------------------
// XML

struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Node>> children;
    const std::string* attr(const char* n) const {
        for (auto& a : attrs)
            if (a.first == n) return &a.second;
        return nullptr;
    }
};

std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        std::string e = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (e == "amp") cp = '&';
        else if (e == "lt") cp = '<';
        else if (e == "gt") cp = '>';
        else if (e == "quot") cp = '"';
        else if (e == "apos") cp = '\'';
        else if (!e.empty() && e[0] == '#') cp = (uint32_t)(e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? strtoul(e.c_str() + 2, nullptr, 16) : strtoul(e.c_str() + 1, nullptr, 10));
        if (!cp) {
            o += s[i];
            continue;
        }
        if (cp < 0x80) o += (char)cp;
        else o += "?";
        i = semi;
    }
    return o;
}

class XmlReader {
public:
    explicit XmlReader(const std::string& t) : s(t) {}
    std::unique_ptr<Node> parse() {
        std::unique_ptr<Node> root;
        std::vector<Node*> stack;
        while (i < s.size()) {
            if (s[i] != '<') {
                i++;
                continue;
            }
            if (s.compare(i, 4, "<!--") == 0) {
                size_t e = s.find("-->", i + 4);
                i = e == std::string::npos ? s.size() : e + 3;
            } else if (s.compare(i, 9, "<![CDATA[") == 0) {
                size_t e = s.find("]]>", i);
                i = e == std::string::npos ? s.size() : e + 3;
            } else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
                skipDecl();
            } else if (s.compare(i, 2, "</") == 0) {
                size_t e = s.find('>', i);
                i = e == std::string::npos ? s.size() : e + 1;
                if (!stack.empty()) stack.pop_back();
            } else {
                i++;
                std::unique_ptr<Node> n(new Node);
                n->name = name();
                bool selfClose = false;
                for (;;) {
                    skipWs();
                    if (i >= s.size()) break;
                    if (s[i] == '/') {
                        selfClose = true;
                        i++;
                        continue;
                    }
                    if (s[i] == '>') {
                        i++;
                        break;
                    }
                    std::string an = name();
                    if (an.empty()) {
                        i++;
                        continue;
                    }
                    skipWs();
                    std::string val;
                    if (i < s.size() && s[i] == '=') {
                        i++;
                        skipWs();
                        if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                            char q = s[i++];
                            size_t e = s.find(q, i);
                            if (e == std::string::npos) e = s.size();
                            val = decodeEntities(s.substr(i, e - i));
                            i = std::min(s.size(), e + 1);
                        } else {
                            val = name();
                        }
                    }
                    n->attrs.emplace_back(an, val);
                }
                Node* raw = n.get();
                if (stack.empty()) {
                    if (!root) root = std::move(n);
                    else continue;  // a second top-level element: ignore
                } else {
                    stack.back()->children.push_back(std::move(n));
                }
                if (!selfClose) stack.push_back(raw);
            }
        }
        return root;
    }

private:
    const std::string& s;
    size_t i = 0;
    void skipWs() {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
    }
    std::string name() {
        size_t b = i;
        while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/' &&
               s[i] != '"' && s[i] != '\'')
            i++;
        return s.substr(b, i - b);
    }
    void skipDecl() {
        int depth = 0;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '<') depth++;
            else if (c == '>' && --depth <= 0) return;
        }
    }
};

// ------------------------------------------------------------------------------------------------
// values

struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Matrix operator*(const Matrix& m) const {  // this, then m applied first (this * m)
        Matrix r;
        r.a = a * m.a + c * m.b;
        r.b = b * m.a + d * m.b;
        r.c = a * m.c + c * m.d;
        r.d = b * m.c + d * m.d;
        r.e = a * m.e + c * m.f + e;
        r.f = b * m.e + d * m.f + f;
        return r;
    }
    float scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

struct NumReader {
    const char* p;
    const char* end;
    void skipSep() {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
    }
    bool number(float& out) {
        skipSep();
        if (p >= end) return false;
        const char* q = p;
        if (*q == '+' || *q == '-') q++;
        bool digits = false;
        while (q < end && isdigit((unsigned char)*q)) q++, digits = true;
        if (q < end && *q == '.') {
            q++;
            while (q < end && isdigit((unsigned char)*q)) q++, digits = true;
        }
        if (!digits) return false;
        if (q < end && (*q == 'e' || *q == 'E')) {
            const char* r = q + 1;
            if (r < end && (*r == '+' || *r == '-')) r++;
            if (r < end && isdigit((unsigned char)*r)) {
                while (r < end && isdigit((unsigned char)*r)) r++;
                q = r;
            }
        }
        out = (float)strtod(std::string(p, q).c_str(), nullptr);
        p = q;
        return true;
    }
    bool flag(bool& out) {
        skipSep();
        if (p < end && (*p == '0' || *p == '1')) {
            out = *p == '1';
            p++;
            return true;
        }
        return false;
    }
};

float parseLength(const std::string* s, float def, float percentOf = 0) {
    if (!s || s->empty()) return def;
    char* e;
    double v = strtod(s->c_str(), &e);
    if (e == s->c_str()) return def;
    std::string unit(e);
    while (!unit.empty() && isspace((unsigned char)unit.back())) unit.pop_back();
    if (unit == "pt") v *= 4.0 / 3.0;
    else if (unit == "pc") v *= 16;
    else if (unit == "mm") v *= 96 / 25.4;
    else if (unit == "cm") v *= 96 / 2.54;
    else if (unit == "in") v *= 96;
    else if (unit == "%") v = percentOf > 0 ? v * percentOf / 100 : def;
    return (float)v;
}

Matrix parseTransform(const std::string& t) {
    Matrix m;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (isspace((unsigned char)t[i]) || t[i] == ',')) i++;
        size_t b = i;
        while (i < t.size() && isalpha((unsigned char)t[i])) i++;
        std::string fn = t.substr(b, i - b);
        size_t open = t.find('(', i), close = t.find(')', i);
        if (fn.empty() || open == std::string::npos || close == std::string::npos) break;
        std::string args = t.substr(open + 1, close - open - 1);
        i = close + 1;
        NumReader nr{args.c_str(), args.c_str() + args.size()};
        std::vector<float> v;
        float x;
        while (nr.number(x)) v.push_back(x);
        Matrix k;
        if (fn == "matrix" && v.size() >= 6) {
            k.a = v[0]; k.b = v[1]; k.c = v[2]; k.d = v[3]; k.e = v[4]; k.f = v[5];
        } else if (fn == "translate" && !v.empty()) {
            k.e = v[0];
            k.f = v.size() > 1 ? v[1] : 0;
        } else if (fn == "scale" && !v.empty()) {
            k.a = v[0];
            k.d = v.size() > 1 ? v[1] : v[0];
        } else if (fn == "rotate" && !v.empty()) {
            float r = v[0] * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
            k.a = cs; k.b = sn; k.c = -sn; k.d = cs;
            if (v.size() >= 3) {
                Matrix t1, t2;
                t1.e = v[1]; t1.f = v[2];
                t2.e = -v[1]; t2.f = -v[2];
                k = t1 * k * t2;
            }
        } else if (fn == "skewX" && !v.empty()) {
            k.c = std::tan(v[0] * 3.14159265f / 180.f);
        } else if (fn == "skewY" && !v.empty()) {
            k.b = std::tan(v[0] * 3.14159265f / 180.f);
        }
        m = m * k;
    }
    return m;
}

struct Paint {
    bool none = false, current = false;
    Color color = Color(0, 0, 0);
    // url(#id) [fallback]: the element it names (a gradient); `fallback`: the colour fields hold the fallback
    std::string ref;
    bool fallback = false;
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool namedColor(const std::string& n, Color& c) {
    static const struct { const char* name; uint32_t rgb; } k[] = {
        {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"cyan", 0x00ffff}, {"aqua", 0x00ffff}, {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff},
        {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xc0c0c0}, {"maroon", 0x800000}, {"olive", 0x808000},
        {"lime", 0x00ff00}, {"teal", 0x008080}, {"navy", 0x000080}, {"purple", 0x800080}, {"orange", 0xffa500},
        {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3},
        {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"brown", 0xa52a2a}, {"pink", 0xffc0cb}, {"gold", 0xffd700},
    };
    std::string l;
    for (char ch : n) l += (char)tolower((unsigned char)ch);
    for (auto& e : k)
        if (l == e.name) {
            c = Color((int)(e.rgb >> 16), (int)((e.rgb >> 8) & 255), (int)(e.rgb & 255));
            return true;
        }
    return false;
}

bool parsePaint(std::string v, Paint& p) {
    while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && isspace((unsigned char)v[0])) v.erase(0, 1);
    if (v.empty()) return false;
    p = Paint();
    if (v == "none" || v == "transparent") {
        p.none = true;
        return true;
    }
    if (v == "currentColor") {
        p.current = true;
        return true;
    }
    if (v.compare(0, 4, "url(") == 0) {  // url(#id) [fallback]: a gradient (resolved when drawn)
        size_t close = v.find(')');
        std::string id = trim(close == std::string::npos ? v.substr(4) : v.substr(4, close - 4));
        if (id.size() >= 2 && (id[0] == '"' || id[0] == '\'') && id.back() == id[0]) id = trim(id.substr(1, id.size() - 2));
        std::string rest = close == std::string::npos ? "" : trim(v.substr(close + 1));
        Paint fb;
        if (!rest.empty() && rest.compare(0, 4, "url(") != 0 && parsePaint(rest, fb)) {
            p = fb;
            p.fallback = true;
        }
        size_t hash = id.find('#');  // (only references within the document: "#id", "file.svg#id" is not read)
        p.ref = hash == 0 ? id.substr(1) : std::string();
        if (p.ref.empty() && !p.fallback) p.none = true;
        return true;
    }
    if (v[0] == '#') {
        auto hx = [](char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10) & 15; };
        if (v.size() == 4) {
            p.color = Color(hx(v[1]) * 17, hx(v[2]) * 17, hx(v[3]) * 17);
            return true;
        }
        if (v.size() >= 7) {
            p.color = Color(hx(v[1]) * 16 + hx(v[2]), hx(v[3]) * 16 + hx(v[4]), hx(v[5]) * 16 + hx(v[6]));
            return true;
        }
        return false;
    }
    if (v.compare(0, 4, "rgb(") == 0 || v.compare(0, 5, "rgba(") == 0) {
        size_t open = v.find('(');
        std::string args = v.substr(open + 1);
        float c[4] = {0, 0, 0, 1};
        int n = 0;
        const char* q = args.c_str();
        while (n < 4 && *q && *q != ')') {
            char* e;
            double x = strtod(q, &e);
            if (e == q) {
                q++;
                continue;
            }
            if (*e == '%') {
                x = x * 2.55;
                e++;
            }
            c[n++] = (float)x;
            q = e;
        }
        p.color = Color((int)std::clamp(c[0], 0.f, 255.f), (int)std::clamp(c[1], 0.f, 255.f), (int)std::clamp(c[2], 0.f, 255.f),
                        (int)std::lround(std::clamp(c[3], 0.f, 1.f) * 255));
        return true;
    }
    return namedColor(v, p.color);
}

struct Style {
    Paint fill, stroke;
    Color current = Color(0, 0, 0);
    float strokeWidth = 1, fillOpacity = 1, strokeOpacity = 1, opacity = 1;
    LineCap cap = LineCap::Flat;
    LineJoin join = LineJoin::Miter;
    bool evenOdd = false, hidden = false;
    Style() { stroke.none = true; }
};

void applyProperty(Style& st, const std::string& k, const std::string& v, bool& displayNone) {
    auto num = [&](float def) {
        char* e;
        double x = strtod(v.c_str(), &e);
        return e == v.c_str() ? def : (float)x;
    };
    if (k == "fill") parsePaint(v, st.fill);
    else if (k == "stroke") parsePaint(v, st.stroke);
    else if (k == "color") {
        Paint p;
        if (parsePaint(v, p) && !p.none && !p.current) st.current = p.color;
    } else if (k == "stroke-width") st.strokeWidth = std::max(0.f, num(1));
    else if (k == "fill-opacity") st.fillOpacity = std::clamp(num(1), 0.f, 1.f);
    else if (k == "stroke-opacity") st.strokeOpacity = std::clamp(num(1), 0.f, 1.f);
    else if (k == "opacity") st.opacity *= std::clamp(num(1), 0.f, 1.f);
    else if (k == "fill-rule") st.evenOdd = v.find("evenodd") != std::string::npos;
    else if (k == "stroke-linecap") st.cap = v.find("round") != std::string::npos ? LineCap::Round : v.find("square") != std::string::npos ? LineCap::Square : LineCap::Flat;
    else if (k == "stroke-linejoin") st.join = v.find("round") != std::string::npos ? LineJoin::Round : v.find("bevel") != std::string::npos ? LineJoin::Bevel : LineJoin::Miter;
    else if (k == "display") displayNone = v.find("none") != std::string::npos;
    else if (k == "visibility") st.hidden = v.find("hidden") != std::string::npos || v.find("collapse") != std::string::npos;
}

// the element's own presentation attributes, then its style="…" (which wins)
void applyStyle(const Node& n, Style& st, bool& displayNone) {
    static const char* props[] = {"fill", "stroke", "color", "stroke-width", "fill-opacity", "stroke-opacity", "opacity",
                                  "fill-rule", "stroke-linecap", "stroke-linejoin", "display", "visibility"};
    for (const char* p : props)
        if (const std::string* v = n.attr(p)) applyProperty(st, p, trim(*v), displayNone);
    if (const std::string* s = n.attr("style")) {
        size_t i = 0;
        while (i < s->size()) {
            size_t semi = s->find(';', i);
            if (semi == std::string::npos) semi = s->size();
            std::string decl = s->substr(i, semi - i);
            size_t colon = decl.find(':');
            if (colon != std::string::npos) applyProperty(st, trim(decl.substr(0, colon)), trim(decl.substr(colon + 1)), displayNone);
            i = semi + 1;
        }
    }
}

// path data
void parsePathData(const std::string& d, Path& path) {
    NumReader r{d.c_str(), d.c_str() + d.size()};
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0;  // current point, subpath start
    float lcx = 0, lcy = 0;                // last control point (for S / T)
    char last = 0;
    for (;;) {
        r.skipSep();
        if (r.p >= r.end) break;
        if (isalpha((unsigned char)*r.p)) {
            cmd = *r.p++;
        } else if (!cmd) {
            break;
        }
        bool rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        float v[7];
        auto need = [&](int n) {
            for (int k = 0; k < n; k++)
                if (!r.number(v[k])) return false;
            return true;
        };
        if (C == 'Z') {
            path.close();
            cx = sx;
            cy = sy;
            last = 'Z';
            cmd = 0;  // a number after Z without a command is an error; stop treating as implicit
            continue;
        }
        if (C == 'M') {
            if (!need(2)) break;
            if (rel) { v[0] += cx; v[1] += cy; }
            path.moveTo(v[0], v[1]);
            cx = sx = v[0];
            cy = sy = v[1];
            cmd = rel ? 'l' : 'L';  // following pairs are line-tos
            last = 'M';
            continue;
        }
        if (C == 'L') {
            if (!need(2)) break;
            if (rel) { v[0] += cx; v[1] += cy; }
            path.lineTo(v[0], v[1]);
            cx = v[0]; cy = v[1];
        } else if (C == 'H') {
            if (!need(1)) break;
            cx = rel ? cx + v[0] : v[0];
            path.lineTo(cx, cy);
        } else if (C == 'V') {
            if (!need(1)) break;
            cy = rel ? cy + v[0] : v[0];
            path.lineTo(cx, cy);
        } else if (C == 'C') {
            if (!need(6)) break;
            if (rel) for (int k = 0; k < 6; k += 2) { v[k] += cx; v[k + 1] += cy; }
            path.cubicTo(v[0], v[1], v[2], v[3], v[4], v[5]);
            lcx = v[2]; lcy = v[3];
            cx = v[4]; cy = v[5];
        } else if (C == 'S') {
            if (!need(4)) break;
            if (rel) for (int k = 0; k < 4; k += 2) { v[k] += cx; v[k + 1] += cy; }
            float c1x = cx, c1y = cy;
            if (last == 'C' || last == 'S') { c1x = 2 * cx - lcx; c1y = 2 * cy - lcy; }
            path.cubicTo(c1x, c1y, v[0], v[1], v[2], v[3]);
            lcx = v[0]; lcy = v[1];
            cx = v[2]; cy = v[3];
        } else if (C == 'Q') {
            if (!need(4)) break;
            if (rel) for (int k = 0; k < 4; k += 2) { v[k] += cx; v[k + 1] += cy; }
            path.quadTo(v[0], v[1], v[2], v[3]);
            lcx = v[0]; lcy = v[1];
            cx = v[2]; cy = v[3];
        } else if (C == 'T') {
            if (!need(2)) break;
            if (rel) { v[0] += cx; v[1] += cy; }
            float qx = cx, qy = cy;
            if (last == 'Q' || last == 'T') { qx = 2 * cx - lcx; qy = 2 * cy - lcy; }
            path.quadTo(qx, qy, v[0], v[1]);
            lcx = qx; lcy = qy;
            cx = v[0]; cy = v[1];
        } else if (C == 'A') {
            bool large, sweep;
            if (!r.number(v[0]) || !r.number(v[1]) || !r.number(v[2]) || !r.flag(large) || !r.flag(sweep) ||
                !r.number(v[5]) || !r.number(v[6]))
                break;
            if (rel) { v[5] += cx; v[6] += cy; }
            path.arcTo(v[0], v[1], v[2], large, sweep, v[5], v[6]);
            cx = v[5]; cy = v[6];
        } else {
            break;  // unknown command
        }
        last = C;
    }
}

std::vector<float> numbers(const std::string* s) {
    std::vector<float> v;
    if (!s) return v;
    NumReader r{s->c_str(), s->c_str() + s->size()};
    float x;
    while (r.number(x)) v.push_back(x);
    return v;
}

// an element's name without the "svg:" prefix ("" for another namespace's elements: inkscape:, sodipodi:…)
std::string localName(const Node& n) {
    size_t colon = n.name.find(':');
    if (colon == std::string::npos) return n.name;
    return n.name.compare(0, colon, "svg") == 0 ? n.name.substr(colon + 1) : std::string();
}

// a presentation property of an element: its style="…" declaration (the last one), else its attribute; false:
// neither
bool property(const Node& n, const char* name, std::string& out) {
    if (const std::string* s = n.attr("style")) {
        bool found = false;
        size_t i = 0;
        while (i < s->size()) {
            size_t semi = s->find(';', i);
            if (semi == std::string::npos) semi = s->size();
            std::string decl = s->substr(i, semi - i);
            size_t colon = decl.find(':');
            if (colon != std::string::npos && trim(decl.substr(0, colon)) == name) {
                out = trim(decl.substr(colon + 1));
                found = true;
            }
            i = semi + 1;
        }
        if (found) return true;
    }
    if (const std::string* v = n.attr(name)) {
        out = trim(*v);
        return true;
    }
    return false;
}

// preserveAspectRatio="[defer] <align> [meet | slice]": the viewBox (vx, vy, vw, vh) onto a w x h viewport
// (null or unreadable: xMidYMid meet)
Matrix viewBoxMatrix(float vx, float vy, float vw, float vh, float w, float h, const std::string* par) {
    int ax = 1, ay = 1;  // 0 min, 1 mid, 2 max
    bool none = false, slice = false;
    if (par) {
        std::string text = trim(*par);
        if (text.compare(0, 5, "defer") == 0) text = trim(text.substr(5));
        size_t sp = text.find_first_of(" \t\r\n,");
        std::string align = text.substr(0, sp), mode = sp == std::string::npos ? "" : trim(text.substr(sp + 1));
        if (align == "none") {
            none = true;
        } else if (align.size() == 8 && align[0] == 'x' && align[4] == 'Y') {
            auto part = [](const std::string& p) { return p == "Min" ? 0 : p == "Max" ? 2 : 1; };
            ax = part(align.substr(1, 3));
            ay = part(align.substr(5, 3));
        }
        slice = mode == "slice";
    }
    Matrix m;
    if (none) {  // (stretched to the viewport, each way on its own)
        m.a = w / vw;
        m.d = h / vh;
        m.e = -vx * m.a;
        m.f = -vy * m.d;
        return m;
    }
    float s = slice ? std::max(w / vw, h / vh) : std::min(w / vw, h / vh);
    m.a = m.d = s;
    m.e = (ax == 0 ? 0.f : ax == 1 ? (w - vw * s) / 2 : w - vw * s) - vx * s;
    m.f = (ay == 0 ? 0.f : ay == 1 ? (h - vh * s) / 2 : h - vh * s) - vy * s;
    return m;
}

bool inverse(const Matrix& m, Matrix& out) {
    double det = (double)m.a * m.d - (double)m.b * m.c;
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    double k = 1.0 / det;
    out.a = (float)(m.d * k);
    out.b = (float)(-m.b * k);
    out.c = (float)(-m.c * k);
    out.d = (float)(m.a * k);
    out.e = (float)((m.c * (double)m.f - m.d * (double)m.e) * k);
    out.f = (float)((m.b * (double)m.e - m.a * (double)m.f) * k);
    return true;
}

// premultiplied pixel * a / 255 (as the Painter blends)
inline uint32_t mul8(uint32_t p, uint32_t a) {
    uint32_t rb = (p & 0x00ff00ffu) * a + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ag = ((p >> 8) & 0x00ff00ffu) * a + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return rb | ag;
}

// A gradient paint server (linearGradient / radialGradient, its href chain followed) made ready for one
// element: its colour at each device pixel
struct GradientPaint : public Shader {
    bool radial = false;
    int spread = 0;  // 0 pad, 1 reflect, 2 repeat
    GradientRamp ramp;
    Matrix inv;  // device px -> gradient space
    float x1 = 0, y1 = 0, x2 = 1, y2 = 0;                           // linear
    float cx = .5f, cy = .5f, r = .5f, fx = .5f, fy = .5f, fr = 0;  // radial
    float ka = 0, kb = 0, kc = 0, foot = 0;                         // (linear) t = ka x + kb y + kc, device px

    void prepare() {
        if (!radial) {
            float dx = x2 - x1, dy = y2 - y1, l2 = dx * dx + dy * dy;
            float ga = dx / l2, gb = dy / l2, gc = -(x1 * ga + y1 * gb);  // (t in gradient space)
            ka = ga * inv.a + gb * inv.b;
            kb = ga * inv.c + gb * inv.d;
            kc = ga * inv.e + gb * inv.f + gc;
            if (std::fabs(ka) < 1e-7f * std::fabs(kb)) ka = 0;  // (vertical or horizontal: exactly)
            if (std::fabs(kb) < 1e-7f * std::fabs(ka)) kb = 0;
            foot = std::sqrt(ka * ka + kb * kb);
            return;
        }
        // SVG 1.1: a focal point outside the end circle is moved onto it (just inside)
        float wx = fx - cx, wy = fy - cy, d = std::sqrt(wx * wx + wy * wy);
        if (d > r * 0.999f && d > 0) {
            fx = cx + wx * (r * 0.999f / d);
            fy = cy + wy * (r * 0.999f / d);
        }
        if (fr >= r) fr = 0;
    }
    float radialT(float gx, float gy) const {
        float dx = gx - fx, dy = gy - fy, a = dx * dx + dy * dy;
        float dist = std::sqrt(a);
        float edge;  // from the focal point to the end circle, through this point
        if (fx == cx && fy == cy) {
            edge = r;
        } else {
            if (a < 1e-12f) return 0;
            float wx = fx - cx, wy = fy - cy;
            float b = 2 * (wx * dx + wy * dy), c = wx * wx + wy * wy - r * r;
            float disc = std::max(0.f, b * b - 4 * a * c);
            edge = (-b + std::sqrt(disc)) / (2 * a) * dist;
        }
        float span = edge - fr;
        return span > 1e-9f ? (dist - fr) / span : 1.f;
    }
    float spreadT(float t) const {
        if (spread == 2) return t - std::floor(t);
        if (spread == 1) {
            float u = t - 2 * std::floor(t * 0.5f);
            return u > 1 ? 2 - u : u;
        }
        return t;
    }
    void shade(int x, int y, int n, uint32_t* out) const override {
        float py = y + 0.5f;
        if (!radial) {
            float t = ka * (x + 0.5f) + kb * py + kc;
            if (spread == 0 && ramp.sharp())  // (hard stops: averaged over the pixel)
                for (int i = 0; i < n; i++) out[i] = ramp.average(t + ka * i, foot);
            else
                for (int i = 0; i < n; i++) out[i] = ramp.at(spreadT(t + ka * i));
            return;
        }
        for (int i = 0; i < n; i++) {
            float px = x + i + 0.5f;
            float gx = inv.a * px + inv.c * py + inv.e, gy = inv.b * px + inv.d * py + inv.f;
            out[i] = ramp.at(spreadT(radialT(gx, gy)));
        }
    }
};

struct Renderer {
    Painter& p;
    float viewportW = 0, viewportH = 0;  // (the root's viewBox: userSpaceOnUse percentages)
    std::map<std::string, const Node*> ids;

    void collectIds(const Node& n, int depth) {
        if (depth > 64) return;
        if (const std::string* id = n.attr("id"))
            if (!id->empty()) ids.emplace(*id, &n);  // (the first of an id counts)
        for (auto& c : n.children) collectIds(*c, depth + 1);
    }

    // The gradient `ref` names, for an element (bbox: its geometry's bounds in user space; ctm: user space to
    // device px): false if `ref` names no gradient. `solid`: it is one colour (one stop, a line of no length, a
    // radius of 0), `solidColor`; `none`: nothing is drawn (no stops, a bounding box with no width or height).
    bool gradient(const std::string& ref, const RectF& bbox, const Matrix& ctm, Color current, GradientPaint& g,
                  bool& solid, Color& solidColor, bool& none) {
        solid = none = false;
        auto it = ids.find(ref);
        if (it == ids.end()) return false;
        std::string kind = localName(*it->second);
        if (kind != "linearGradient" && kind != "radialGradient") return false;
        g.radial = kind == "radialGradient";
        // the href chain: each attribute and the stops come from the first gradient of it that has them
        std::vector<const Node*> chain;
        for (const Node* n = it->second; n && chain.size() < 16;) {
            if (std::find(chain.begin(), chain.end(), n) != chain.end()) break;  // (a loop)
            std::string k = localName(*n);
            if (k != "linearGradient" && k != "radialGradient") break;
            chain.push_back(n);
            const std::string* h = n->attr("href");
            if (!h) h = n->attr("xlink:href");
            if (!h || h->empty() || (*h)[0] != '#') break;
            auto next = ids.find(h->substr(1));
            n = next == ids.end() ? nullptr : next->second;
        }
        auto common = [&](const char* name) -> const std::string* {
            for (const Node* n : chain)
                if (const std::string* v = n->attr(name)) return v;
            return nullptr;
        };
        auto own = [&](const char* name) -> const std::string* {  // (only from gradients of the same kind)
            for (const Node* n : chain)
                if (localName(*n) == kind)
                    if (const std::string* v = n->attr(name)) return v;
            return nullptr;
        };
        const std::string* units = common("gradientUnits");
        bool user = units && trim(*units) == "userSpaceOnUse";
        if (const std::string* sm = common("spreadMethod")) {
            std::string v = trim(*sm);
            g.spread = v == "reflect" ? 1 : v == "repeat" ? 2 : 0;
        }
        Matrix gt;
        if (const std::string* t = common("gradientTransform")) gt = parseTransform(*t);
        // stops: offset (a number or %, clamped to 0…1, never before the one before), stop-color, stop-opacity
        std::vector<GradientStop> stops;
        for (const Node* n : chain) {
            for (auto& c : n->children) {
                if (localName(*c) != "stop") continue;
                GradientStop s;
                float o = 0;
                if (const std::string* off = c->attr("offset")) {
                    std::string v = trim(*off);
                    char* e;
                    double x = strtod(v.c_str(), &e);
                    if (e != v.c_str()) o = (float)(*e == '%' ? x / 100 : x);
                }
                if (!(o == o)) o = 0;
                o = std::clamp(o, 0.f, 1.f);
                if (!stops.empty()) o = std::max(o, stops.back().pos);
                s.pos = o;
                std::string v;
                Paint col;
                s.color = Color(0, 0, 0);
                if (property(*c, "stop-color", v) && parsePaint(v, col) && col.ref.empty() && !col.fallback) {
                    if (col.none) s.color = Color(0, 0, 0, 0);
                    else s.color = col.current ? current : col.color;
                }
                if (property(*c, "stop-opacity", v)) {
                    char* e;
                    double x = strtod(v.c_str(), &e);
                    if (e != v.c_str()) {
                        if (*e == '%') x /= 100;
                        if (!(x == x)) x = 1;
                        s.color.a = (uint8_t)std::lround(s.color.a * std::clamp(x, 0.0, 1.0));
                    }
                }
                stops.push_back(s);
            }
            if (!stops.empty()) break;
        }
        if (stops.empty()) {
            none = true;
            return true;
        }
        if (stops.size() == 1) {
            solid = true;
            solidColor = stops[0].color;
            return true;
        }
        // where: fractions of the bounding box (objectBoundingBox: numbers or %), or user space lengths
        // (userSpaceOnUse: % of the viewport; a radius's of its diagonal / sqrt 2)
        float diag = std::sqrt((viewportW * viewportW + viewportH * viewportH) / 2);
        auto coord = [&](const char* name, float defFraction, float viewport) {
            const std::string* v = own(name);
            if (user) return parseLength(v, defFraction * viewport, viewport);
            return parseLength(v, defFraction, 1);
        };
        Color last = stops.back().color;
        if (!g.radial) {
            g.x1 = coord("x1", 0, viewportW);
            g.y1 = coord("y1", 0, viewportH);
            g.x2 = coord("x2", 1, viewportW);
            g.y2 = coord("y2", 0, viewportH);
            if (g.x1 == g.x2 && g.y1 == g.y2) {  // (the area in the last stop's colour)
                solid = true;
                solidColor = last;
                return true;
            }
        } else {
            g.cx = coord("cx", .5f, viewportW);
            g.cy = coord("cy", .5f, viewportH);
            g.r = coord("r", .5f, diag);
            g.fx = own("fx") ? coord("fx", .5f, viewportW) : g.cx;
            g.fy = own("fy") ? coord("fy", .5f, viewportH) : g.cy;
            g.fr = std::max(0.f, coord("fr", 0, diag));
            if (!(g.r > 0)) {  // (0: the last stop's colour; negative: an error, nothing)
                if (g.r == 0) {
                    solid = true;
                    solidColor = last;
                } else {
                    none = true;
                }
                return true;
            }
        }
        Matrix toUser = gt;
        if (!user) {
            if (!(bbox.w > 0) || !(bbox.h > 0)) {
                none = true;
                return true;
            }
            Matrix box;
            box.a = bbox.w;
            box.d = bbox.h;
            box.e = bbox.x;
            box.f = bbox.y;
            toUser = box * gt;
        }
        if (!inverse(ctm * toUser, g.inv)) {
            solid = true;
            solidColor = last;
            return true;
        }
        g.ramp = GradientRamp(stops, false);
        g.prepare();
        return true;
    }

    // a fill's or stroke's paint for the element: a colour, a gradient (`shaded`, color.a: its opacity), or none
    struct Resolved {
        bool none = true, shaded = false;
        Color color;
        GradientPaint grad;
    };
    void resolve(const Paint& pt, const Style& st, float opacity, const Path& userPath, const Matrix& ctm,
                 Resolved& out) {
        out.none = true;
        out.shaded = false;
        if (pt.none && pt.ref.empty()) return;
        Color c;
        if (!pt.ref.empty()) {
            RectF box;
            std::vector<Path::Poly> polys = userPath.flatten(0.03f);
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (auto& poly : polys)
                for (auto& q : poly.pts) {
                    x0 = std::min(x0, q.x);
                    y0 = std::min(y0, q.y);
                    x1 = std::max(x1, q.x);
                    y1 = std::max(y1, q.y);
                }
            if (x0 <= x1) box = RectF(x0, y0, x1 - x0, y1 - y0);
            bool solid = false, none = false;
            Color sc;
            if (gradient(pt.ref, box, ctm, st.current, out.grad, solid, sc, none)) {
                if (none) return;
                if (!solid) {
                    out.shaded = true;
                    out.color = Color(0, 0, 0, (int)std::lround(255 * opacity * st.opacity));
                    out.none = out.color.a == 0;
                    return;
                }
                c = sc;
            } else if (pt.fallback && !pt.none) {  // (not a gradient: the fallback colour, else nothing)
                c = pt.current ? st.current : pt.color;
            } else {
                return;
            }
        } else {
            c = pt.current ? st.current : pt.color;
        }
        c.a = (uint8_t)std::lround(c.a * opacity * st.opacity);
        out.color = c;
        out.none = c.a == 0;
    }

    // a gradient's colours where `cover` (a scratch image the target's size) is painted, its alpha the coverage,
    // at `alpha` (0..255)
    void shadeCoverage(const Image& cover, const Shader& sh, int alpha) {
        Image& dst = p.target();
        int w = std::min(dst.width(), cover.width()), h = std::min(dst.height(), cover.height());
        std::vector<uint32_t> buf((size_t)std::max(0, w));
        for (int y = 0; y < h; y++) {
            const uint32_t* c = cover.row(y);
            int x0 = 0;
            while (x0 < w && !(c[x0] >> 24)) x0++;
            if (x0 == w) continue;
            int x1 = w;
            while (x1 > x0 && !(c[x1 - 1] >> 24)) x1--;
            sh.shade(x0, y, x1 - x0, buf.data());
            uint32_t* d = dst.row(y);
            for (int x = x0; x < x1; x++) {
                uint32_t k = c[x] >> 24;
                if (!k) continue;
                if (alpha < 255) k = (k * (uint32_t)alpha + 127) / 255;
                uint32_t s = buf[(size_t)(x - x0)];
                if (k < 255) s = mul8(s, k);
                if ((s >> 24) == 255) d[x] = s;
                else if (s) d[x] = s + mul8(d[x], 255 - (s >> 24));
            }
        }
    }

    void node(const Node& n, const Style& parent, const Matrix& ctm, int depth) {
        if (depth > 64) return;
        std::string name = localName(n);
        if (name.empty()) return;  // inkscape:, sodipodi:… elements
        if (name == "defs" || name == "metadata" || name == "title" || name == "desc" || name == "style" ||
            name == "clipPath" || name == "mask" || name == "symbol" || name == "marker" || name == "pattern" ||
            name == "linearGradient" || name == "radialGradient" || name == "text" || name == "filter")
            return;
        Style st = parent;
        st.opacity = 1;
        bool displayNone = false;
        applyStyle(n, st, displayNone);
        if (displayNone) return;
        st.opacity *= parent.opacity;  // group opacity, approximated by multiplying into the children
        Matrix m = ctm;
        if (const std::string* t = n.attr("transform")) m = ctm * parseTransform(*t);
        if (name == "g" || name == "svg" || name == "a" || name == "switch") {
            if (name == "svg" && depth > 0) {
                Matrix k;
                k.e = parseLength(n.attr("x"), 0);
                k.f = parseLength(n.attr("y"), 0);
                m = m * k;
                // its own viewBox onto its width x height (preserveAspectRatio)
                std::vector<float> vb = numbers(n.attr("viewBox"));
                float w = parseLength(n.attr("width"), viewportW, viewportW);
                float h = parseLength(n.attr("height"), viewportH, viewportH);
                if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0 && w > 0 && h > 0)
                    m = m * viewBoxMatrix(vb[0], vb[1], vb[2], vb[3], w, h, n.attr("preserveAspectRatio"));
            }
            for (auto& c : n.children) node(*c, st, m, depth + 1);
            return;
        }
        Path path;
        if (name == "path") {
            if (const std::string* d = n.attr("d")) parsePathData(*d, path);
        } else if (name == "rect") {
            float x = parseLength(n.attr("x"), 0), y = parseLength(n.attr("y"), 0);
            float w = parseLength(n.attr("width"), 0), h = parseLength(n.attr("height"), 0);
            if (w <= 0 || h <= 0) return;
            const std::string* rxs = n.attr("rx");
            const std::string* rys = n.attr("ry");
            float rx = parseLength(rxs, -1), ry = parseLength(rys, -1);
            if (rx < 0) rx = ry;
            if (ry < 0) ry = rx;
            if (rx > 0 && ry > 0) path.addRoundedRect(RectF(x, y, w, h), rx, ry);
            else path.addRect(RectF(x, y, w, h));
        } else if (name == "circle") {
            float cx = parseLength(n.attr("cx"), 0), cy = parseLength(n.attr("cy"), 0), r = parseLength(n.attr("r"), 0);
            if (r <= 0) return;
            path.addEllipse(RectF(cx - r, cy - r, 2 * r, 2 * r));
        } else if (name == "ellipse") {
            float cx = parseLength(n.attr("cx"), 0), cy = parseLength(n.attr("cy"), 0);
            float rx = parseLength(n.attr("rx"), 0), ry = parseLength(n.attr("ry"), 0);
            if (rx <= 0 || ry <= 0) return;
            path.addEllipse(RectF(cx - rx, cy - ry, 2 * rx, 2 * ry));
        } else if (name == "line") {
            path.moveTo(parseLength(n.attr("x1"), 0), parseLength(n.attr("y1"), 0));
            path.lineTo(parseLength(n.attr("x2"), 0), parseLength(n.attr("y2"), 0));
        } else if (name == "polyline" || name == "polygon") {
            std::vector<float> v = numbers(n.attr("points"));
            if (v.size() < 4) return;
            path.moveTo(v[0], v[1]);
            for (size_t k = 2; k + 1 < v.size(); k += 2) path.lineTo(v[k], v[k + 1]);
            if (name == "polygon") path.close();
        } else {
            return;
        }
        if (path.empty() || st.hidden) return;
        Path user;
        if (!st.fill.ref.empty() || !st.stroke.ref.empty()) user = path;  // (for its bounding box in user space)
        path.transform(m.a, m.b, m.c, m.d, m.e, m.f);
        Resolved paint;
        if (name != "line") {
            resolve(st.fill, st, st.fillOpacity, user, m, paint);
            if (!paint.none && !paint.shaded) {
                p.fillPath(path, paint.color, st.evenOdd);
            } else if (!paint.none) {
                std::vector<std::vector<PointF>> polys;
                for (auto& poly : path.flatten(0.03f)) polys.push_back(std::move(poly.pts));
                p.save();
                p.setOpacity(paint.color.a / 255.f);
                p.fillDevice(polys, paint.grad, st.evenOdd);
                p.restore();
            }
        }
        if (st.strokeWidth > 0) {
            resolve(st.stroke, st, st.strokeOpacity, user, m, paint);
            if (!paint.none && !paint.shaded) {
                p.strokePath(path, paint.color, st.strokeWidth * m.scale(), st.cap, st.join);
            } else if (!paint.none) {  // (its coverage drawn on its own first, then the gradient's colours there)
                Image cover(p.target().width(), p.target().height());
                Painter cp(cover);
                cp.strokePath(path, Color(255, 255, 255), st.strokeWidth * m.scale(), st.cap, st.join);
                shadeCoverage(cover, paint.grad, paint.color.a);
            }
        }
    }
};

}  // namespace

Image renderSvg(const std::string& svgText, int w, int h) {
    if (w <= 0 || h <= 0) return Image();
    XmlReader xr(svgText);
    std::unique_ptr<Node> root = xr.parse();
    if (!root) return Image();
    std::string rn = root->name;
    size_t colon = rn.find(':');
    if (colon != std::string::npos) rn = rn.substr(colon + 1);
    if (rn != "svg") return Image();
    float vx = 0, vy = 0, vw = 0, vh = 0;
    std::vector<float> vb = numbers(root->attr("viewBox"));
    bool viewBox = vb.size() == 4 && vb[2] > 0 && vb[3] > 0;
    if (viewBox) {
        vx = vb[0]; vy = vb[1]; vw = vb[2]; vh = vb[3];
    } else {
        vw = parseLength(root->attr("width"), (float)w, 0);
        vh = parseLength(root->attr("height"), (float)h, 0);
        if (vw <= 0 || vh <= 0) { vw = (float)w; vh = (float)h; }
    }
    // preserveAspectRatio, with a viewBox (default xMidYMid meet: as big as fits, centred; none: stretched)
    Matrix m = viewBoxMatrix(vx, vy, vw, vh, (float)w, (float)h, viewBox ? root->attr("preserveAspectRatio") : nullptr);
    Image img(w, h);
    Painter p(img);
    Renderer r{p, vw, vh, {}};
    r.collectIds(*root, 0);
    Style st;
    r.node(*root, st, m, 0);
    return img;
}

Image renderSvgFile(const std::string& path, int w, int h) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return Image();
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return renderSvg(text, w, h);
}

Image renderSvgTinted(const std::string& svgText, int w, int h, Color tint) {
    Image img = renderSvg(svgText, w, h);
    return img.empty() ? img : img.tinted(tint);
}

bool svgSize(const std::string& svgText, float& w, float& h, float& ratio) {
    w = h = ratio = 0;
    XmlReader xr(svgText);
    std::unique_ptr<Node> root = xr.parse();
    if (!root) return false;
    std::string rn = root->name;
    size_t colon = rn.find(':');
    if (colon != std::string::npos) rn = rn.substr(colon + 1);
    if (rn != "svg") return false;
    w = parseLength(root->attr("width"), 0, 0);
    h = parseLength(root->attr("height"), 0, 0);
    if (!std::isfinite(w) || w < 0) w = 0;
    if (!std::isfinite(h) || h < 0) h = 0;
    std::vector<float> vb = numbers(root->attr("viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) ratio = vb[2] / vb[3];
    else if (w > 0 && h > 0) ratio = w / h;
    // one of width / height given: the other from the proportions
    if (ratio > 0 && w > 0 && h <= 0) h = w / ratio;
    else if (ratio > 0 && h > 0 && w <= 0) w = h * ratio;
    return true;
}
