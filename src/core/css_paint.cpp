// Reading the CSS values that paint (css.h): gradients (background-image and the background shorthand),
// box-shadow and text-shadow lists, opacity, and backdrop-filter's blur(); Style's getters for them.
//
// GRADIENTS. linear-gradient([<angle> | to <side-or-corner>,] <stops>) and radial-gradient([<shape> ||
// <size>] [at <position>], <stops>), and their repeating- forms. Angles in deg, rad, turn, grad (or a bare
// 0). Sides: to top / right / bottom / left; corners: two of them, in any order. Shape: circle or ellipse;
// size: closest-side, closest-corner, farthest-side, farthest-corner, or explicit (a circle: one length; an
// ellipse: two lengths or percentages of the box). Position: one or two of left / center / right / top /
// bottom, lengths and percentages (two values: x then y, or keywords in either order). A colour stop is a
// colour with up to two positions (% of the gradient line, or a length), in either order; two positions make
// two stops of the same colour. At least two stops; transition hints (a position alone) are not supported.
//
// SHADOWS. none, or a comma list of: [inset] <x> <y> [<blur> [<spread>]] [<colour>], inset and the colour in
// any place around the lengths (box-shadow); text-shadow takes no inset and no spread. The blur can't be
// negative. Without a colour: the node's `color` (currentColor), black if it has none.
//
// Every length here may be px, pt, em (of the node's font size) or calc() of those; what checkValue()
// accepts when the sheet is read is what the getters read (with var() resolved first).
#include <cmath>

#include "core/css_internal.h"

namespace css {
namespace detail {

namespace {

const double kPi = 3.14159265358979323846;

bool one(std::pair<size_t, size_t> r) { return r.second == r.first + 1; }

bool identIs(const Tokens& t, std::pair<size_t, size_t> r, const char* name) {
    return one(r) && t[r.first].type == Tok::Ident && ieq(t[r.first].text, name);
}

// an angle in degrees: 45deg, 1.2rad, 0.25turn, 100grad, or 0
bool angleAt(const Tokens& t, std::pair<size_t, size_t> r, double& deg) {
    if (!one(r)) return false;
    const Token& k = t[r.first];
    if (k.type == Tok::Number && k.num == 0) deg = 0;
    else if (k.type != Tok::Dimension) return false;
    else if (k.text == "deg") deg = k.num;
    else if (k.text == "rad") deg = k.num * 180 / kPi;
    else if (k.text == "turn") deg = k.num * 360;
    else if (k.text == "grad") deg = k.num * 0.9;
    else return false;
    return std::isfinite(deg);
}

// a length (px) or a percentage (a fraction: 50% -> 0.5)
bool lengthPercent(const Tokens& t, std::pair<size_t, size_t> r, double em, double& v, bool& percent) {
    if (one(r) && t[r.first].type == Tok::Percentage) {
        v = t[r.first].num / 100;
        percent = true;
        return std::isfinite(v);
    }
    LenCtx ctx;
    ctx.em = em;
    ctx.percent = false;
    percent = false;
    return evalLength(t, r.first, r.second, ctx, v) && std::isfinite(v);
}

bool lengthOnly(const Tokens& t, std::pair<size_t, size_t> r, double em, double& v) {
    LenCtx ctx;
    ctx.em = em;
    ctx.percent = false;
    return evalLength(t, r.first, r.second, ctx, v) && std::isfinite(v);
}

// <position> after `at`: one or two values
bool positionAt(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& c, size_t from, double em,
                Style::Gradient& g) {
    size_t n = c.size() - from;
    if (n < 1 || n > 2) return false;
    // each value: a keyword with its axis (0 either: center, 1 x, 2 y) and fraction, or a length / percentage
    struct V {
        int axis = 0;
        double v = 0.5;
        bool px = false;
        bool keyword = false;
    } v[2];
    for (size_t i = 0; i < n; i++) {
        auto r = c[from + i];
        if (identIs(t, r, "center")) v[i] = {0, 0.5, false, true};
        else if (identIs(t, r, "left")) v[i] = {1, 0, false, true};
        else if (identIs(t, r, "right")) v[i] = {1, 1, false, true};
        else if (identIs(t, r, "top")) v[i] = {2, 0, false, true};
        else if (identIs(t, r, "bottom")) v[i] = {2, 1, false, true};
        else {
            bool pct;
            double x;
            if (!lengthPercent(t, r, em, x, pct)) return false;
            v[i] = {0, x, !pct, false};
        }
    }
    V x{1, 0.5, false, true}, y{2, 0.5, false, true};
    if (n == 1) {
        if (v[0].axis == 2) y = v[0];
        else x = v[0];
    } else {
        bool swap = v[0].axis == 2 || v[1].axis == 1;
        if (swap && (!v[0].keyword || !v[1].keyword)) return false;  // (a length goes x then y)
        V a = swap ? v[1] : v[0], b = swap ? v[0] : v[1];
        if (a.axis == 2 || b.axis == 1) return false;  // two of one axis
        x = a;
        y = b;
    }
    g.centerX = x.v;
    g.centerY = y.v;
    g.centerXPx = x.px;
    g.centerYPx = y.px;
    return true;
}

// the colour stops of [args from `first`]
bool stopsAt(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& args, size_t first,
             const Rgba* current, double em, Style::Gradient& g, std::string& why) {
    for (size_t i = first; i < args.size(); i++) {
        auto comps = components(t, args[i].first, args[i].second);
        if (comps.empty()) {
            why = "an empty colour stop";
            return false;
        }
        Rgba color;
        bool haveColor = false;
        double pos[2];
        bool px[2];
        int npos = 0;
        for (auto r : comps) {
            bool pct;
            double v;
            if (!haveColor && evalColor(t, r.first, r.second, current, color)) {
                haveColor = true;
            } else if (lengthPercent(t, r, em, v, pct)) {
                if (npos == 2) {
                    why = "a colour stop has at most two positions";
                    return false;
                }
                pos[npos] = v;
                px[npos++] = !pct;
            } else {
                why = "'" + t.text(r.first, r.second) + "' is not a colour or a position";
                return false;
            }
        }
        if (!haveColor) {
            why = "transition hints (a position alone) are not supported";
            return false;
        }
        Style::ColorStop s;
        s.color = color;
        if (npos == 0) {
            g.stops.push_back(s);
        }
        for (int k = 0; k < npos; k++) {
            s.position = pos[k];
            s.px = px[k];
            g.stops.push_back(s);
        }
    }
    if (g.stops.size() < 2) {
        why = "a gradient needs two colour stops";
        return false;
    }
    return true;
}

bool linearAt(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& args, const Rgba* current,
              double em, Style::Gradient& g, std::string& why) {
    size_t first = 0;
    auto comps = components(t, args[0].first, args[0].second);
    double deg;
    if (comps.size() == 1 && angleAt(t, comps[0], deg)) {
        g.angle = deg;
        first = 1;
    } else if (!comps.empty() && identIs(t, comps[0], "to")) {
        int hx = 0, vy = 0;
        if (comps.size() < 2 || comps.size() > 3) {
            why = "'to' takes a side or a corner";
            return false;
        }
        for (size_t i = 1; i < comps.size(); i++) {
            if (identIs(t, comps[i], "left") && !hx) hx = -1;
            else if (identIs(t, comps[i], "right") && !hx) hx = 1;
            else if (identIs(t, comps[i], "top") && !vy) vy = -1;
            else if (identIs(t, comps[i], "bottom") && !vy) vy = 1;
            else {
                why = "'" + t.text(comps[i].first, comps[i].second) + "' is not a side (to top, to right…)";
                return false;
            }
        }
        if (hx && vy) {
            g.toCorner = true;
            g.cornerX = hx;
            g.cornerY = vy;
            g.angle = hx > 0 ? (vy > 0 ? 135 : 45) : (vy > 0 ? 225 : 315);  // (a square box's)
        } else {
            g.angle = vy < 0 ? 0 : hx > 0 ? 90 : vy > 0 ? 180 : 270;
        }
        first = 1;
    }
    g.angle = std::fmod(g.angle, 360.0);
    if (g.angle < 0) g.angle += 360;
    return stopsAt(t, args, first, current, em, g, why);
}

bool radialAt(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& args, const Rgba* current,
              double em, Style::Gradient& g, std::string& why) {
    auto comps = components(t, args[0].first, args[0].second);
    auto isConfig = [&](std::pair<size_t, size_t> r) {
        static const char* const words[] = {"circle", "ellipse", "closest-side", "closest-corner", "farthest-side",
                                            "farthest-corner", "at"};
        for (const char* w : words)
            if (identIs(t, r, w)) return true;
        double v;
        bool pct;
        return lengthPercent(t, r, em, v, pct);
    };
    size_t first = 0;
    if (!comps.empty() && isConfig(comps[0])) {
        first = 1;
        bool shape = false, sizeKw = false;
        int nExplicit = 0;
        double ex[2] = {0, 0};
        bool exPct[2] = {false, false};
        for (size_t i = 0; i < comps.size(); i++) {
            auto r = comps[i];
            if (identIs(t, r, "at")) {
                if (!positionAt(t, comps, i + 1, em, g)) {
                    why = "a bad position after 'at'";
                    return false;
                }
                break;
            }
            if ((identIs(t, r, "circle") || identIs(t, r, "ellipse")) && !shape) {
                shape = true;
                g.circle = identIs(t, r, "circle");
                continue;
            }
            Style::Gradient::Size s = Style::Gradient::FarthestCorner;
            bool kw = true;
            if (identIs(t, r, "closest-side")) s = Style::Gradient::ClosestSide;
            else if (identIs(t, r, "closest-corner")) s = Style::Gradient::ClosestCorner;
            else if (identIs(t, r, "farthest-side")) s = Style::Gradient::FarthestSide;
            else if (identIs(t, r, "farthest-corner")) s = Style::Gradient::FarthestCorner;
            else kw = false;
            if (kw) {
                if (sizeKw || nExplicit) {
                    why = "a radial gradient has one size";
                    return false;
                }
                sizeKw = true;
                g.size = s;
                continue;
            }
            double v;
            bool pct;
            if (!sizeKw && nExplicit < 2 && lengthPercent(t, r, em, v, pct) && v >= 0) {
                ex[nExplicit] = v;
                exPct[nExplicit++] = pct;
                continue;
            }
            why = "'" + t.text(r.first, r.second) + "' is not a shape, a size or 'at'";
            return false;
        }
        if (nExplicit) {
            if (!shape) g.circle = nExplicit == 1;
            if (g.circle && (nExplicit != 1 || exPct[0])) {
                why = "a circle's size is one length";
                return false;
            }
            if (!g.circle && nExplicit != 2) {
                why = "an ellipse's size is two lengths or percentages";
                return false;
            }
            g.size = Style::Gradient::Explicit;
            g.radiusX = ex[0];
            g.radiusY = g.circle ? ex[0] : ex[1];
            g.radiusXPercent = exPct[0];
            g.radiusYPercent = g.circle ? false : exPct[1];
        }
    }
    return stopsAt(t, args, first, current, em, g, why);
}

}  // namespace

bool isGradientFunction(const Token& k) {
    if (k.type != Tok::Function) return false;
    std::string n = lower(k.text);
    return n == "linear-gradient" || n == "radial-gradient" || n == "repeating-linear-gradient" ||
           n == "repeating-radial-gradient";
}

bool parseGradient(const Tokens& t, size_t b, size_t e, const Rgba* current, double em, Style::Gradient& g,
                   std::string& why) {
    t.trim(b, e);
    if (b >= e || !isGradientFunction(t[b]) || t[b].close != e - 1) {
        why = "not a gradient";
        return false;
    }
    std::string n = lower(t[b].text);
    g = Style::Gradient();
    g.repeating = n.compare(0, 10, "repeating-") == 0;
    g.kind = n.find("radial") != std::string::npos ? Style::Gradient::Radial : Style::Gradient::Linear;
    auto args = splitTop(t, b + 1, e - 1, Tok::Comma);
    if (args.empty() || args[0].first >= args[0].second) {
        why = "an empty " + n + "()";
        return false;
    }
    bool ok = g.kind == Style::Gradient::Linear ? linearAt(t, args, current, em, g, why)
                                                : radialAt(t, args, current, em, g, why);
    if (!ok) why = n + "(): " + why;
    return ok;
}

bool parseImages(const Tokens& t, size_t b, size_t e, const Rgba* current, double em,
                 std::vector<Style::Gradient>* out, std::string& why) {
    t.trim(b, e);
    if (b >= e) {
        why = "no value";
        return false;
    }
    if (e == b + 1 && t[b].type == Tok::Ident && ieq(t[b].text, "none")) return true;
    for (auto [lb, le] : splitTop(t, b, e, Tok::Comma)) {
        if (le == lb + 1 && t[lb].type == Tok::Ident && ieq(t[lb].text, "none")) continue;  // (a layer without one)
        if (lb < le && (t[lb].type == Tok::Url || (t[lb].type == Tok::Function && ieq(t[lb].text, "url")))) {
            why = "url() images are not supported (gradients are)";
            return false;
        }
        Style::Gradient g;
        if (!parseGradient(t, lb, le, current, em, g, why)) return false;
        if (out) out->push_back(std::move(g));
    }
    return true;
}

bool parseShadows(const Tokens& t, size_t b, size_t e, bool box, const Rgba* current, double em,
                  std::vector<Style::Shadow>* out, std::string& why) {
    t.trim(b, e);
    if (b >= e) {
        why = "no value";
        return false;
    }
    if (e == b + 1 && t[b].type == Tok::Ident && ieq(t[b].text, "none")) return true;
    for (auto [sb, se] : splitTop(t, b, e, Tok::Comma)) {
        auto comps = components(t, sb, se);
        if (comps.empty()) {
            why = "an empty shadow";
            return false;
        }
        Style::Shadow s;
        s.color = current ? *current : Rgba{0, 0, 0, 255};
        bool haveColor = false, haveInset = false;
        double len[4];
        int n = 0;
        bool lengthsDone = false;  // (the lengths come together)
        for (auto r : comps) {
            double v;
            if (box && !haveInset && identIs(t, r, "inset")) {
                haveInset = true;
                s.inset = true;
                if (n) lengthsDone = true;
            } else if (!lengthsDone && lengthOnly(t, r, em, v)) {
                if (n == (box ? 4 : 3)) {
                    why = std::string(box ? "a box shadow" : "a text shadow") + " has at most " + (box ? "4" : "3") +
                          " lengths";
                    return false;
                }
                len[n++] = v;
            } else if (!haveColor && evalColor(t, r.first, r.second, current, s.color)) {
                haveColor = true;
                if (n) lengthsDone = true;
            } else {
                why = "'" + t.text(r.first, r.second) + "' is not part of a shadow";
                return false;
            }
        }
        if (n < 2) {
            why = "a shadow needs its x and y offsets";
            return false;
        }
        s.x = len[0];
        s.y = len[1];
        s.blur = n > 2 ? len[2] : 0;
        s.spread = n > 3 ? len[3] : 0;
        if (s.blur < 0) {
            why = "a shadow's blur can't be negative";
            return false;
        }
        if (out) out->push_back(s);
    }
    return true;
}

bool parseBackdropBlur(const Tokens& t, size_t b, size_t e, double em, double* px, std::string& why) {
    t.trim(b, e);
    if (px) *px = 0;
    if (e == b + 1 && t[b].type == Tok::Ident && ieq(t[b].text, "none")) return true;
    if (b >= e || t[b].type != Tok::Function || t[b].close != e - 1) {
        why = "none or blur(<length>)";
        return false;
    }
    if (!ieq(t[b].text, "blur")) {
        why = "only blur() is supported";
        return false;
    }
    double v = 0;
    size_t ab = b + 1, ae = e - 1;
    t.trim(ab, ae);
    if (ab < ae && !lengthOnly(t, {ab, ae}, em, v)) {  // (blur() alone: 0)
        why = "blur() takes a length";
        return false;
    }
    if (v < 0) {
        why = "a blur can't be negative";
        return false;
    }
    if (px) *px = v;
    return true;
}

}  // namespace detail

namespace {
// the node's `color`, for currentColor
bool currentOf(const std::map<std::string, std::string>& props, Rgba& out) {
    auto c = props.find("color");
    return c != props.end() && detail::colorText(c->second, nullptr, out);
}
}  // namespace

std::vector<Style::Gradient> Style::gradients(const std::string& property) const {
    std::vector<Gradient> r;
    auto it = props_.find(property);
    if (it == props_.end()) return r;
    Rgba current;
    bool have = currentOf(props_, current);
    detail::Tokens t;
    detail::tokenize(it->second, t);
    std::string why;
    if (!detail::parseImages(t, 0, t.size(), have ? &current : nullptr, emPx_, &r, why)) r.clear();
    return r;
}

std::vector<Style::Shadow> Style::shadows(const std::string& property) const {
    std::vector<Shadow> r;
    auto it = props_.find(property);
    if (it == props_.end()) return r;
    Rgba current{0, 0, 0, 255};
    currentOf(props_, current);
    detail::Tokens t;
    detail::tokenize(it->second, t);
    std::string why;
    if (!detail::parseShadows(t, 0, t.size(), property != "text-shadow", &current, emPx_, &r, why)) r.clear();
    return r;
}

double Style::backdropBlur() const {
    auto it = props_.find("backdrop-filter");
    if (it == props_.end()) return 0;
    detail::Tokens t;
    detail::tokenize(it->second, t);
    double px = 0;
    std::string why;
    return detail::parseBackdropBlur(t, 0, t.size(), emPx_, &px, why) ? px : 0;
}

bool declaresOnNodes(const Engine& engine, const std::string& property) {
    for (const SheetPtr& sheet : engine.sheets()) {
        if (!sheet) continue;
        for (const detail::Rule& rule : sheet->rules) {
            bool node = false;
            for (const detail::Selector& s : rule.selectors) node = node || s.part.empty();
            if (!node) continue;
            for (const detail::Decl& d : rule.decls)
                if (d.name == property) return true;
        }
    }
    return false;
}

}  // namespace css
