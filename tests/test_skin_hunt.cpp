// A skin, theme and scaling hunt (headless): every skin at every interface size, the main window with a realistic
// project (tests/test_layout_audit.cpp's) and the newer windows, looked at for what a user would see as a broken
// look. Reported, not failed: build/ui-samples/skin-hunt/raw.md, with pictures beside it.
//   contrast     WCAG contrast of each text-bearing widget's text against what is painted behind it (normal state:
//                sampled from the framebuffer; hover, pressed, checked, disabled: the widget's box painted in that
//                state's style over what is around it; list rows: their row box); 4.5:1 for text, 3:1 for large text
//                and one-character glyphs, disabled controls apart (WCAG exempts them: only below 1.8:1)
//   same-state   a state that looks like another (checked like unchecked, selected like not)
//   surface      texts the program draws itself (ruler, lanes, piano keys, menus, meters…) and the variables' pairs
//   doc-dark-left  colours of the types docs/skins.md lists that stay Dark's in another skin (hard-coded colours)
//   meter-rms    the level meters' RMS bar against the peak bar it is drawn in
//   glyphs       the non-ASCII characters the interface's strings use: drawn by no font here (a box), not in the
//                fonts the program ships (a computer without DejaVu / Noto draws boxes), their size and line
//                against an H (glyphs-<skin>.png: a specimen of all of them in each skin's font)
//   scaling      1 px edges and grid lines at fractional sizes, icons upscaled (blurry) at 200 % (zoom-*.png),
//                logical geometry that changes with the interface size
// HUNT_DEBUG=<text> prints what was sampled for the widgets whose description has that text.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// the realistic project, its window and the helpers of the layout audit (its main() renamed away)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define main layout_audit_main
#include "test_layout_audit.cpp"
#undef main
#pragma GCC diagnostic pop

#include "ui/midi_ports_dialog.h"
#include "ui/paint_helpers.h"
#include "ui/panel_style.h"
#include "ui/preferences.h"
#include "ui/solo_button.h"
#include "ui/record_arm.h"
#include "ui/transport_bar.h"
#include "ui/overview.h"
#include "ui/level_meter.h"
#include "ui/toolkit/style.h"

namespace hunt {

std::string HOUT = "build/ui-samples/skin-hunt";
double hElapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }

// --- colours ---
double channel(int v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double lum(Color c) { return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b); }
double contrast(Color a, Color b) {
    double x = lum(a), y = lum(b);
    if (x < y) std::swap(x, y);
    return (x + 0.05) / (y + 0.05);
}
Color over(Color top, Color base) {
    double a = top.a / 255.0;
    return Color((int)std::lround(top.r * a + base.r * (1 - a)), (int)std::lround(top.g * a + base.g * (1 - a)),
                 (int)std::lround(top.b * a + base.b * (1 - a)));
}
int dist(Color a, Color b) { return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)}); }
std::string hx(Color c) { return c.hexRgb(); }
std::string r2(double v) {
    char b[32];
    snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// --- findings: grouped by check and key, each remembering every skin / scale it showed in and its worst value ---
struct Hit {
    std::string check, key, detail, picture;
    double worst = 1e9;
    std::vector<std::string> where;
    std::map<std::string, double> per;  // each place's worst value
    std::map<std::string, std::string> perDetail;  // … and what it was
    int count = 0;
};
std::map<std::string, Hit> hits;
std::vector<std::string> hitOrder;
std::vector<std::string> hnotes;
void hit(const std::string& check, const std::string& key, const std::string& where, const std::string& detail,
         double value = 0, const std::string& picture = "") {
    std::string k = check + "|" + key;
    auto it = hits.find(k);
    if (it == hits.end()) {
        hitOrder.push_back(k);
        it = hits.emplace(k, Hit{check, key, detail, picture}).first;
    }
    Hit& h = it->second;
    h.count++;
    if (value < h.worst) {
        h.worst = value;
        h.detail = detail;
        if (!picture.empty()) h.picture = picture;
    }
    if (std::find(h.where.begin(), h.where.end(), where) == h.where.end()) h.where.push_back(where);
    if (!h.per.count(where) || value < h.per[where]) {
        h.per[where] = value;
        h.perDetail[where] = detail;
    }
}

// --- pixels ---
Rect deviceRectOf(const Widget* w) {
    Window* win = w->window();
    Point o = w->mapToWindow(Point(0, 0));
    Rect r(o.x, o.y, w->width(), w->height());
    // (cut to what its ancestors show)
    for (const Widget* p = w->parent(); p; p = p->parent()) {
        Point po = p->mapToWindow(Point(0, 0));
        r = r.intersected(Rect(po.x, po.y, p->width(), p->height()));
    }
    Rect d = win->toDeviceRect(r);
    const Image& fb = win->framebuffer();
    return d.intersected(Rect(0, 0, fb.width(), fb.height()));
}
// the most common colour in a device rect, colours taken 4 bits a channel (a gradient's band counts as one); the mean
// of that band. `alpha0`: the band is of unpainted pixels (nothing drew there)
Color modeColor(const Image& fb, const Rect& r, bool* alpha0 = nullptr) {
    std::unordered_map<uint32_t, int> n;
    std::unordered_map<uint32_t, std::array<long, 5>> sum;
    int step = r.w * r.h > 40000 ? 3 : 1;
    for (int y = r.y; y < r.bottom(); y += step)
        for (int x = r.x; x < r.right(); x += step) {
            Color c = Color::fromPremul(fb.pixel(x, y));
            uint32_t q = (uint32_t)(c.r >> 4) << 8 | (uint32_t)(c.g >> 4) << 4 | (uint32_t)(c.b >> 4) | (c.a < 128 ? 0x1000u : 0u);
            n[q]++;
            auto& s = sum[q];
            s[0] += c.r; s[1] += c.g; s[2] += c.b; s[3] += c.a; s[4]++;
        }
    uint32_t best = 0;
    int bn = -1;
    for (auto& [p, c] : n)
        if (c > bn) { bn = c; best = p; }
    if (bn <= 0) return Color();
    auto& s = sum[best];
    if (alpha0) *alpha0 = (best & 0x1000) != 0;
    return Color((int)(s[0] / s[4]), (int)(s[1] / s[4]), (int)(s[2] / s[4]));
}
// what is painted just around a rect (its parent's background there)
Color ringColor(const Image& fb, const Rect& r) {
    std::unordered_map<uint32_t, int> n;
    auto take = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < fb.width() && y < fb.height()) n[fb.pixel(x, y)]++;
    };
    for (int x = r.x; x < r.right(); x++) { take(x, r.y - 2); take(x, r.bottom() + 1); }
    for (int y = r.y; y < r.bottom(); y++) { take(r.x - 2, y); take(r.right() + 1, y); }
    uint32_t best = 0;
    int bn = -1;
    for (auto& [p, c] : n)
        if (c > bn) { bn = c; best = p; }
    Color c = Color::fromPremul(best);
    c.a = 255;
    return c;
}
struct Ink {
    Color ink;          // the pixel farthest (in contrast) from the background
    bool found = false; // a pixel near the style's text colour is there
};
Ink inkOf(const Image& fb, const Rect& r, Color bg, Color fg) {
    Ink k;
    double best = 0;
    for (int y = r.y; y < r.bottom(); y++)
        for (int x = r.x; x < r.right(); x++) {
            Color c = fb.colorAt(x, y);
            c.a = 255;
            double cr = contrast(c, bg);
            if (cr > best) { best = cr; k.ink = c; }
        }
    if (best == 0) k.ink = bg;
    // the text is in the style's colour if at least as many pixels are near it as near the farthest one
    int nearFg = 0, nearInk = 0;
    for (int y = r.y; y < r.bottom(); y++)
        for (int x = r.x; x < r.right(); x++) {
            Color c = fb.colorAt(x, y);
            if (dist(c, fg) <= 24) nearFg++;
            if (dist(c, k.ink) <= 24) nearInk++;
        }
    k.found = nearFg > 0 && (dist(fg, k.ink) <= 40 || nearFg >= nearInk);
    return k;
}

// a style's background over `base` (gradients: the mean of their stops), for nodes that aren't painted
Color backgroundOver(const css::Style& s, Color base) {
    Color b = over(ui::style::background(s), base);
    auto gs = s.gradients();
    if (!gs.empty() && !gs[0].stops.empty()) {
        int r = 0, g = 0, bl = 0, a = 0, n = 0;
        for (auto& st : gs[0].stops) { r += st.color.r; g += st.color.g; bl += st.color.b; a += st.color.a; n++; }
        b = over(Color(r / n, g / n, bl / n, a / n), b);
    }
    return b;
}
// a widget's box in a style, painted as the toolkit paints it (gradients, images, borders), over `base`
Image boxPicture(const css::Style& s, int w, int h, float scale, Color base) {
    Image img(std::max(1, (int)std::ceil(w * scale)), std::max(1, (int)std::ceil(h * scale)), base);
    Painter p(img, scale);
    ui::style::box(p, Rect(0, 0, w, h), s);
    return img;
}
// the mean difference of two pictures (0…255)
double meanDiff(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height() || !a.width() || !a.height()) return 255;
    double s = 0;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++) s += dist(a.colorAt(x, y), b.colorAt(x, y));
    return s / (a.width() * a.height());
}

// one character (a glyph or a letter on a button: M, S, ●, ✕) or large: 3:1; else 4.5:1
bool isGlyphText(const std::string& t) {
    size_t i = 0;
    int n = 0;
    while (i < t.size()) { utf8Next(t, i); n++; }
    return n <= 2;
}
double needed(const std::string& text, const Font& f) {
    double pt = f.pointSize();
    if (pt >= 18 || (f.bold() && pt >= 14) || isGlyphText(text)) return 3.0;
    return 4.5;
}

std::string skinName, scaleName;
std::string where() { return skinName + " " + scaleName; }

// the text widgets of a window
struct TextW {
    Widget* w;
    std::string text;
    Font font;
};
void textWidgets(Widget* w, std::vector<TextW>& out) {
    if (!w->isVisible()) return;
    if (w->width() > 0 && w->height() > 0) {
        std::string t = audit::textOf(w);
        if (!t.empty()) {
            Font f;
            if (auto* l = dynamic_cast<Label*>(w)) f = l->font();
            else if (auto* b = dynamic_cast<Button*>(w)) f = b->font();
            else f = ui::style::font(w->computedStyle(), Font::ui());
            out.push_back({w, t, f});
        }
    }
    for (auto& c : w->children()) textWidgets(c.get(), out);
}

std::string ident(const Widget* w) {
    std::string d = w->type();
    if (!w->objectName.empty()) d += "#" + w->objectName;
    for (const char* p : {"toggle", "accent", "lit", "hint", "dimmed"}) {
        std::string v = w->property(p);
        if (!v.empty() && v != "false") d += "." + std::string(p) + (v == "true" ? "" : "-" + v);
    }
    std::string t = audit::textOf(w);
    if (!t.empty()) d += " \"" + audit::shortText(t, 24) + "\"";
    Window* win = w->window();
    std::string wt = win ? (win->title().empty() ? win->type() : win->title()) : "";
    if (wt.find("Tiny DAW") != std::string::npos) wt = "main";
    return audit::shortText(wt, 30) + " › " + d;
}

const Image* cropFb = nullptr;  // the framebuffer contrastWindow is looking at (for crops)
void judge(const TextW& t, const std::string& state, Color fg, Color bg, const std::string& how, const std::string& pic) {
    // (disabled controls are exempt from WCAG's minimum: reported apart, only when they can hardly be read at all)
    bool disabled = state.find("disabled") != std::string::npos || !t.w->isEnabled();
    double cr = contrast(fg, bg), need = disabled ? 1.8 : needed(t.text, t.font);
    // (the widget as painted, zoomed 3x, once per finding and place: what the numbers are about)
    std::string crop;
    if (cr + 1e-9 < need && state == "normal" && cropFb && t.w->window()) {
        crop = HOUT + "/crops/" + fileSafe(skinName + "-" + scaleName + "-" + audit::shortText(ident(t.w), 40)) + ".png";
        if (!std::filesystem::exists(crop)) {
            Rect d = deviceRectOf(t.w).adjusted(-4, -4, 4, 4).intersected(Rect(0, 0, cropFb->width(), cropFb->height()));
            Image z(d.w * 3, d.h * 3);
            for (int y = 0; y < z.height(); y++)
                for (int x = 0; x < z.width(); x++) z.row(y)[x] = cropFb->pixel(d.x + x / 3, d.y + y / 3);
            util::makeDirs(HOUT + "/crops");
            savePng(crop, z);
        }
    }
    if (cr + 1e-9 < need)
        hit(disabled ? "contrast-disabled" : "contrast", ident(t.w) + " :" + state, where(),
            r2(cr) + ":1 (needs " + r2(need) + ") text " + hx(fg) + " on " + hx(bg) + " [" + how + "], " +
                r2(t.font.pointSize()) + " pt" + (t.font.bold() ? " bold" : ""),
            cr, crop.empty() ? pic : crop);
}

css::Style withStates(const Widget* w, const std::vector<std::string>& extra) {
    std::vector<std::string> st;
    w->styleStates(st);
    for (auto& e : extra)
        if (std::find(st.begin(), st.end(), e) == st.end()) st.push_back(e);
    std::string s;
    for (auto& x : st) s += x + " ";
    panel_style::StateNode node(*w, s);
    return theme::styles().compute(node, w->parent() ? &w->parent()->computedStyle() : nullptr);
}

int skipped = 0;
void contrastWindow(Window& win, const std::string& pic) {
    App::get().doLayouts();
    win.invalidate(win.outerRect());
    win.flush();
    const Image& fb = win.framebuffer();
    cropFb = &fb;
    float sc = win.scale();
    std::vector<TextW> tw;
    textWidgets(&win, tw);
    // everything in paint order (a widget, then its children; later siblings over earlier ones), in window coordinates
    std::vector<std::pair<Widget*, Rect>> order;
    std::function<void(Widget*)> walk = [&](Widget* w) {
        if (!w->isVisible()) return;
        Point o = w->mapToWindow(Point(0, 0));
        order.push_back({w, Rect(o.x, o.y, w->width(), w->height())});
        for (auto& c : w->children()) walk(c.get());
    };
    walk(&win);
    std::map<Widget*, size_t> place;
    for (size_t i = 0; i < order.size(); i++) place[order[i].first] = i;
    auto related = [](const Widget* a, const Widget* b) {
        for (const Widget* p = a; p; p = p->parent())
            if (p == b) return true;
        for (const Widget* p = b; p; p = p->parent())
            if (p == a) return true;
        return false;
    };
    // something painted after it over most of it (a row scrolled under the master strip)
    auto covered = [&](Widget* w) {
        size_t i = place[w];
        Rect r = order[i].second;
        for (size_t j = i + 1; j < order.size(); j++) {
            Widget* x = order[j].first;
            if (related(w, x) || audit::isPlainContainer(x)) continue;
            Rect c = order[j].second.intersected(r);
            if (c.w * c.h * 2 > r.w * r.h) return true;
        }
        return false;
    };
    for (TextW& t : tw) {
        Widget* w = t.w;
        if (!w->isShown() || covered(w)) continue;
        Rect d = deviceRectOf(w);
        if (d.w < 3 || d.h < 3) continue;
        if (d.h * 10 < (int)(w->height() * win.scale()) * 6) { skipped++; continue; }  // (mostly cut off by what holds it: a layout matter)
        const css::Style& st = w->computedStyle();
        Point o = w->mapToWindow(Point(0, 0));
        Rect inner = ui::style::contents(Rect(o.x, o.y, w->width(), w->height()), st);
        Rect di = win.toDeviceRect(inner).intersected(d);
        // (a one-glyph button, ● ■ ✕: the glyph may fill its inside; its whole box is the background)
        if (di.w < 3 || di.h < 3 || isGlyphText(t.text)) di = d;
        Color fg = ui::style::foreground(st);
        bool unpainted = false;
        Color painted = modeColor(fb, di, &unpainted);
        if (unpainted) { skipped++; continue; }
        Ink k = inkOf(fb, di, painted, fg);
        // its text not there at all, though it would show: covered by something drawn over it
        if (!k.found && contrast(k.ink, painted) < 1.3 && contrast(fg, painted) >= 1.5) { skipped++; continue; }
        if (const char* dbg = getenv("HUNT_DEBUG"))
            if (ident(w).find(dbg) != std::string::npos)
                printf("debug %s %s: device %d,%d %dx%d inner %d,%d %dx%d painted %s fg %s found %d ink %s\n", where().c_str(),
                       ident(w).c_str(), d.x, d.y, d.w, d.h, di.x, di.y, di.w, di.h, hx(painted).c_str(), hx(fg).c_str(), k.found,
                       hx(k.ink).c_str());
        Color shown = k.found ? fg : k.ink;
        judge(t, "normal", shown, painted, k.found ? "painted bg, style text" : "painted bg and text", pic);
        Color around = ringColor(fb, d);
        // the states: its box painted in each state's style, over what is around it
        Rect lin = inner.translated(-o.x, -o.y);
        auto boxBg = [&](const css::Style& s, Image* keep = nullptr) {
            Image img = boxPicture(s, w->width(), w->height(), sc, around);
            Rect ci = Rect((int)std::floor(lin.x * sc), (int)std::floor(lin.y * sc), (int)std::ceil(lin.w * sc), (int)std::ceil(lin.h * sc))
                          .intersected(Rect(0, 0, img.width(), img.height()));
            if (ci.w < 2 || ci.h < 2) ci = Rect(0, 0, img.width(), img.height());
            Color c = modeColor(img, ci);
            if (keep) *keep = std::move(img);
            return c;
        };
        Color normalBox = boxBg(st);
        bool ownPaint = dist(normalBox, painted) > 40;  // (the widget paints its background itself: states unknown)
        std::string how = ownPaint ? "box painted; the widget paints its own bg, a guess" : "box painted in that state";
        auto state = [&](const std::string& name, const css::Style& s) {
            judge(t, name, ui::style::foreground(s), boxBg(s), how, pic);
        };
        auto* b = dynamic_cast<Button*>(w);
        auto* cb = dynamic_cast<CheckBox*>(w);
        if ((b || cb) && w->isEnabled()) {
            state("hover", withStates(w, {"hover"}));
            if (b) state("pressed", withStates(w, {"hover", "pressed"}));
        }
        if (b && b->isCheckable()) {
            bool was = b->isChecked();
            Image cur, other;
            css::Style curS = withStates(w, {});
            Color curBg = boxBg(curS, &cur);
            b->setCheckedSilently(!was);
            css::Style otherS = withStates(w, {});
            state(was ? "unchecked" : "checked", otherS);
            Color otherBg = boxBg(otherS, &other);
            if (w->isEnabled()) state(was ? "unchecked:hover" : "checked:hover", withStates(w, {"hover"}));
            b->setCheckedSilently(was);
            double md = meanDiff(cur, other);
            double dfg = contrast(ui::style::foreground(curS), ui::style::foreground(otherS));
            if (md < 6 && dfg < 1.25)
                hit("same-state", ident(w) + " checked vs unchecked", where(),
                    "the two boxes differ by " + r2(md) + "/255 a pixel on average (checked " + hx(was ? curBg : otherBg) +
                        ", unchecked " + hx(was ? otherBg : curBg) + "), texts " + r2(dfg) + ":1" + (w->isEnabled() ? "" : " (it is disabled)"),
                    md, pic);
        }
        if ((b || cb || dynamic_cast<Label*>(w)) && w->isEnabled()) {
            w->setEnabled(false);
            css::Style s = w->computedStyle();
            w->setEnabled(true);
            // (a label has no box of its own: what is painted behind it)
            judge(t, "disabled", ui::style::foreground(s), b ? boxBg(s) : painted, b ? how : "painted bg", pic);
        }
    }
    // list rows: selected and hovered items, over the list as painted
    std::function<void(Widget*)> lists = [&](Widget* w) {
        if (!w->isVisible()) return;
        if ((dynamic_cast<ListWidget*>(w) || dynamic_cast<TreeWidget*>(w)) && w->isShown() && w->width() > 4 && w->height() > 4) {
            Rect d = deviceRectOf(w);
            Color painted = modeColor(fb, d);
            panel_style::Parts parts(*w);
            Font f = ui::style::font(parts.get("item"), Font::ui());
            TextW t{w, "list row text", f};
            int rowH = std::max(8, ui::style::px(parts.get("item"), "height", 22));
            auto rowBg = [&](const css::Style& s) {
                Image img = boxPicture(s, std::max(8, w->width() - 4), rowH, sc, painted);
                return modeColor(img, Rect(0, 0, img.width(), img.height()));
            };
            for (const char* s : {"", "selected", "hover", "selected focus", "disabled", "selected hover"}) {
                const css::Style& is = parts.get("item", s);
                judge(t, std::string("item:") + (*s ? s : "normal"), ui::style::foreground(is), rowBg(is), "row box painted", pic);
            }
            Color nb = rowBg(parts.get("item", "")), sb = rowBg(parts.get("item", "selected"));
            Color nf = ui::style::foreground(parts.get("item", "")), sf = ui::style::foreground(parts.get("item", "selected"));
            if (contrast(nb, sb) < 1.25 && contrast(nf, sf) < 1.25)
                hit("same-state", ident(w) + " item selected vs not", where(),
                    "selected " + hx(sb) + "/" + hx(sf) + " vs not " + hx(nb) + "/" + hx(nf), contrast(nb, sb), pic);
        }
        for (auto& c : w->children()) lists(c.get());
    };
    lists(&win);
}

// --- every type, part and state docs/skins.md lists ---
struct DocType {
    std::vector<std::string> types, parts, states;
};
std::vector<DocType> docTypes() {
    std::ifstream in("docs/skins.md");
    std::string line, text;
    bool on = false;
    std::vector<std::string> entries;
    while (std::getline(in, line)) {
        if (line.rfind("## Types", 0) == 0) { on = true; continue; }
        if (!on) continue;
        if (line.rfind("## ", 0) == 0) break;
        if (line.rfind("- **`", 0) == 0 || line.rfind("#", 0) == 0) {
            if (!text.empty()) entries.push_back(text);
            text = line.rfind("#", 0) == 0 ? "" : line;
        } else if (!text.empty()) text += " " + line;
    }
    if (!text.empty()) entries.push_back(text);
    std::vector<DocType> out;
    std::regex typeRe("\\*\\*`([A-Z][A-Za-z]+)`\\*\\*"), partRe("::([a-z][a-z-]*)"), stateRe("`:([a-z][a-z-]*)");
    for (auto& e : entries) {
        DocType d;
        std::set<std::string> seen;
        for (std::sregex_iterator i(e.begin(), e.end(), typeRe), end; i != end; ++i) d.types.push_back((*i)[1]);
        for (std::sregex_iterator i(e.begin(), e.end(), partRe), end; i != end; ++i)
            if (seen.insert("p" + (*i)[1].str()).second) d.parts.push_back((*i)[1]);
        for (std::sregex_iterator i(e.begin(), e.end(), stateRe), end; i != end; ++i)
            if (seen.insert("s" + (*i)[1].str()).second) d.states.push_back((*i)[1]);
        if (!d.types.empty()) out.push_back(d);
    }
    return out;
}
// per skin: type::part:state -> (text, background over the window)
using Looks = std::map<std::string, std::pair<Color, Color>>;
Looks lookAll(const std::vector<DocType>& docs, const Widget* owner) {
    Looks out;
    Color window = theme::c("window");
    for (auto& d : docs)
        for (auto& type : d.types) {
            std::vector<std::string> parts{""};
            for (auto& p : d.parts) parts.push_back(p);
            std::vector<std::string> states{""};
            for (auto& s : d.states) states.push_back(s);
            for (auto& s : states) {
                std::vector<std::string> st;
                if (!s.empty()) st.push_back(s);
                std::vector<css::Style> ss = paint::nodeStyles(owner, type, {}, st, parts);
                Color selfBg = backgroundOver(ss[0], window);
                for (size_t i = 0; i < parts.size(); i++) {
                    Color bg = i ? backgroundOver(ss[i], selfBg) : selfBg;
                    Color fg = ui::style::color(ss[i], "color", theme::c("window_text"));
                    out[type + (parts[i].empty() ? "" : "::" + parts[i]) + (s.empty() ? "" : ":" + s)] = {fg, bg};
                }
            }
        }
    return out;
}
// The texts the program draws on its own surfaces (not widgets: the painted check can't see them), each against
// what it is drawn on: the node's (or another part's) background over the window. Then the variable pairs the
// look is made of (text on the colour it goes on). need: 4.5 text, 3 graphics / large.
struct Surface {
    const char* what;
    const char* type;
    std::vector<std::string> classes, states;
    const char* textPart;  // "" the node
    const char* bgPart;    // "" the node
    double need;
};
void judgeSurfaces(const Widget* owner) {
    static const std::vector<Surface> list{
        {"ruler numbers", "Ruler", {}, {}, "label", "", 4.5},
        {"ruler tempo", "Ruler", {}, {}, "tempo", "", 4.5},
        {"ruler marker name", "Ruler", {}, {}, "marker", "marker", 4.5},
        {"automation / tempo lane range labels", "Lane", {"automation"}, {}, "label", "", 4.5},
        {"automation lane stepped values", "Lane", {"automation"}, {}, "value", "value", 4.5},
        {"automation lane hint", "Lane", {"automation"}, {}, "hint", "", 4.5},
        {"automation curve", "Lane", {"automation"}, {}, "curve", "", 3},
        {"Project overview caption", "OverviewCaption", {}, {}, "", "", 4.5},
        {"piano roll key names", "PianoRoll", {}, {}, "key-label", "keys", 4.5},
        {"Steps key names", "Steps", {}, {}, "key-label", "keys", 4.5},
        {"Steps add row", "Steps", {}, {}, "add-row", "row", 4.5},
        {"hint labels", "Label", {"hint"}, {}, "", "", 4.5},
        {"dimmed labels (8 pt)", "Label", {"dimmed"}, {}, "", "", 4.5},
        {"tooltips", "ToolTip", {}, {}, "", "", 4.5},
        {"status bar", "StatusBar", {}, {}, "", "", 4.5},
        {"menu items", "Menu", {}, {}, "item", "item", 4.5},
        {"menu item under the pointer", "Menu", {}, {"hover"}, "item", "item", 4.5},
        {"menu bar item under the pointer", "MenuBar", {}, {"hover"}, "item", "item", 4.5},
        {"menu bar item open", "MenuBar", {}, {"open"}, "item", "item", 4.5},
        {"drop-down list chosen row", "ComboPopup", {}, {"selected"}, "item", "item", 4.5},
        {"tabs", "TabWidget", {}, {}, "tab", "tab", 4.5},
        {"selected tab", "TabWidget", {}, {"selected"}, "tab", "tab", 4.5},
        {"group box titles", "GroupBox", {}, {}, "title", "", 4.5},
        {"meter peak number", "LevelMeter", {}, {}, "number", "number", 4.5},
        {"meter peak number, clipped", "LevelMeter", {}, {"clipped"}, "number", "number", 4.5},
        {"synth section titles", "SynthPage", {}, {}, "title", "section", 4.5},
        {"synth choices", "SynthPage", {}, {}, "choice", "choice", 4.5},
        {"synth LFO curve", "SynthPage", {}, {}, "lfo", "section", 3},
        {"master row", "LaneFrame", {"master"}, {}, "title", "", 4.5},
    };
    Color window = theme::c("window");
    for (auto& s : list) {
        std::vector<std::string> parts{"", s.textPart, s.bgPart};
        std::vector<css::Style> ss = paint::nodeStyles(owner, s.type, s.classes, s.states, parts);
        Color nodeBg = backgroundOver(ss[0], window);
        Color bg = *s.bgPart ? backgroundOver(ss[2], nodeBg) : nodeBg;
        Color fg = over(ui::style::color(ss[1], "color", theme::c("window_text")), bg);
        double cr = contrast(fg, bg);
        if (cr + 1e-9 < s.need)
            hit("surface", std::string(s.what) + " (" + s.type + (s.classes.empty() ? "" : "." + s.classes[0]) +
                               (*s.textPart ? std::string("::") + s.textPart : "") +
                               (s.states.empty() ? "" : ":" + s.states[0]) + ")",
                where(), r2(cr) + ":1 (needs " + r2(s.need) + ") " + hx(fg) + " on " + hx(bg), cr);
    }
    // the variables' pairs
    static const std::vector<std::array<const char*, 3>> pairs{
        {"window_text", "window", "4.5"}, {"text", "base", "4.5"}, {"text", "alternate_base", "4.5"},
        {"button_text", "button", "4.5"}, {"highlighted_text", "highlight", "4.5"}, {"accent_text", "accent", "4.5"},
        {"tooltip_text", "tooltip_base", "4.5"}, {"hint_text", "window", "4.5"}, {"hint_text", "base", "4.5"},
        {"dimmed_text", "window", "4.5"}, {"ruler_text", "ruler_bg", "4.5"}, {"keys_label", "keys_white", "4.5"},
        {"accent_text", "mute", "3"}, {"solo_text", "solo", "3"}, {"accent_text", "record", "3"},
        {"playhead", "lane", "3"}, {"cursor", "lane", "3"}, {"automation_line", "automation_lane", "3"},
        {"grid_bar", "lane", "1.3"}, {"sample_wave", "base", "3"}, {"selected_note", "lane", "3"},
    };
    for (auto& p : pairs) {
        Color fg = theme::c(p[0]), bg = theme::c(p[1]);
        double need = atof(p[2]), cr = contrast(over(fg, bg), bg);
        if (cr + 1e-9 < need)
            hit("variables", std::string("--") + theme::variableName(p[0]).substr(2) + " on --" + theme::variableName(p[1]).substr(2),
                where(), r2(cr) + ":1 (needs " + p[2] + ") " + hx(fg) + " on " + hx(bg), cr);
    }
    // the shades a hover makes of the accent: lighter towards its text (less contrast)
    Color ah = theme::shades().accentHover;
    double crh = contrast(theme::c("accent_text"), ah);
    if (crh < 4.5)
        hit("variables", "--accent-text on --accent-hover (a lit button under the mouse)", where(),
            r2(crh) + ":1 " + hx(theme::c("accent_text")) + " on " + hx(ah) + " (--accent alone: " +
                r2(contrast(theme::c("accent_text"), theme::c("accent"))) + ":1)",
            crh);
}

// --- glyphs ---
// a font file's cmap (format 4 and 12): does it map the codepoint?
struct Cmap {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    bool has(uint32_t cp) const {
        for (auto& r : ranges)
            if (cp >= r.first && cp <= r.second) return true;
        return false;
    }
};
Cmap readCmap(const std::string& path) {
    Cmap c;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? (d[o] << 8 | d[o + 1]) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? (u16(o) << 16 | u16(o + 2)) : 0; };
    if (d.size() < 12) return c;
    int n = u16(4);
    size_t cmap = 0;
    for (int i = 0; i < n; i++)
        if (u32(12 + i * 16) == 0x636d6170) cmap = u32(12 + i * 16 + 8);
    if (!cmap) return c;
    int tables = u16(cmap + 2);
    size_t best = 0;
    int bestFmt = 0;
    for (int i = 0; i < tables; i++) {
        uint32_t pid = u16(cmap + 4 + i * 8), eid = u16(cmap + 6 + i * 8);
        size_t off = cmap + u32(cmap + 8 + i * 8);
        int fmt = u16(off);
        if ((pid == 3 && (eid == 1 || eid == 10)) || pid == 0)
            if ((fmt == 12 && bestFmt != 12) || (fmt == 4 && !bestFmt)) { best = off; bestFmt = fmt; }
    }
    if (bestFmt == 4) {
        int segs = u16(best + 6) / 2;
        size_t ends = best + 14, starts = ends + segs * 2 + 2;
        for (int i = 0; i < segs; i++)
            if (u16(starts + i * 2) <= u16(ends + i * 2) && u16(starts + i * 2) != 0xFFFF)
                c.ranges.push_back({u16(starts + i * 2), u16(ends + i * 2)});
        // (a segment mapping to glyph 0 still counts: rare in real fonts)
    } else if (bestFmt == 12) {
        uint32_t groups = u32(best + 12);
        for (uint32_t i = 0; i < groups && best + 16 + i * 12 + 12 <= d.size(); i++)
            c.ranges.push_back({u32(best + 16 + i * 12), u32(best + 20 + i * 12)});
    }
    return c;
}

// the non-ASCII characters in the string literals of src/ui (what the interface can show), with a file:line each
std::map<uint32_t, std::string> uiCharacters() {
    std::map<uint32_t, std::string> out;
    for (auto& e : std::filesystem::recursive_directory_iterator("src/ui")) {
        if (e.path().extension() != ".cpp" && e.path().extension() != ".h") continue;
        std::ifstream in(e.path());
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            n++;
            // each string literal's bytes (\xHH escapes undone: "\xe2\x96\xbe" is ▾), then its characters
            std::vector<std::string> lits;
            std::string cur;
            bool inStr = false;
            for (size_t i = 0; i < line.size(); i++) {
                char c = line[i];
                if (!inStr) {
                    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
                    if (c == '\'' && i + 2 < line.size()) { i += line[i + 1] == '\\' ? 3 : 2; continue; }
                    if (c == '"') { inStr = true; cur.clear(); }
                    continue;
                }
                if (c == '"') { inStr = false; lits.push_back(cur); continue; }
                if (c == '\\' && i + 1 < line.size()) {
                    if (line[i + 1] == 'x') {
                        size_t j = i + 2;
                        int v = 0, k = 0;
                        while (j < line.size() && k < 2 && isxdigit((unsigned char)line[j])) { v = v * 16 + std::stoi(std::string(1, line[j]), nullptr, 16); j++; k++; }
                        cur += (char)v;
                        i = j - 1;
                    } else {
                        cur += line[i + 1];
                        i++;
                    }
                    continue;
                }
                cur += c;
            }
            for (auto& s : lits)
                for (size_t i = 0; i < s.size();) {
                    size_t at = i;
                    uint32_t cp = utf8Next(s, i);
                    if (i == at) i++;
                    if (cp >= 0x80 && cp != 0xFFFD && !out.count(cp)) out[cp] = e.path().string() + ":" + std::to_string(n);
                }
        }
    }
    return out;
}

// a glyph's ink: top and bottom (y down, from the baseline) of its mask's covered rows
bool inkRows(const Font& f, uint32_t cp, int& top, int& bottom) {
    std::vector<Font::Run> runs;
    f.layout(codepointToUtf8(cp), 0, runs);
    if (runs.empty() || !runs[0].glyph) return false;
    const Font::Glyph& g = *runs[0].glyph;
    top = 1 << 20;
    bottom = -(1 << 20);
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++)
            if (g.mask[y * g.w + x] > 40) { top = std::min(top, g.top + y); bottom = std::max(bottom, g.top + y); }
    return top <= bottom;
}

void sweepGlyphs() {
    auto chars = uiCharacters();
    // the fonts the program ships, and which skins use which (their @font-face)
    std::vector<std::pair<std::string, Cmap>> shipped;
    for (auto& e : std::filesystem::directory_iterator("data/skins/fonts"))
        if (e.path().extension() == ".ttf") shipped.push_back({e.path().filename().string(), readCmap(e.path().string())});
    std::string list;
    for (auto& s : shipped) list += s.first + " (" + std::to_string(s.second.ranges.size()) + " ranges) ";
    hnotes.push_back("shipped fonts: " + list);
    hnotes.push_back(std::to_string(chars.size()) + " non-ASCII characters in src/ui's strings");
    std::string notShipped, tofu;
    for (const skin::Skin& s : skin::library()) {
        skin::apply(s);
        skinName = s.name;
        Font ui = theme::uiFont();
        Font mono = theme::monoFont(9);
        int Htop = 0, Hbot = 0;
        inkRows(ui, 'H', Htop, Hbot);
        double hMid = (Htop + Hbot) / 2.0, hH = Hbot - Htop + 1;
        // the symbols beside text most: their ink's height against an H's and how far their middle is off the H's
        {
            std::string n = s.name + " (" + (ui.family().empty() ? "the system's ui font" : ui.family()) + " " + r2(ui.pixelSize()) +
                            " px; H " + std::to_string((int)hH) + " px tall):";
            for (uint32_t cp : {0x25BEu, 0x25B8u, 0x2715u, 0x25CFu, 0x2295u, 0x2669u, 0x2922u, 0x27F2u, 0x2B07u, 0x23F8u}) {
                int t = 0, b = 0;
                if (!ui.hasGlyph(cp) || !inkRows(ui, cp, t, b)) continue;
                n += " " + codepointToUtf8(cp) + " " + r2((b - t + 1) / hH) + "x " + (((t + b) / 2.0 - hMid) >= 0 ? "+" : "") +
                     r2((t + b) / 2.0 - hMid);
            }
            hnotes.push_back("glyph sizes: " + n);
        }
        // a specimen: every symbol the interface uses, between H's, in the skin's interface font (boxes and jumps show)
        {
            std::string line = "H";
            for (auto& [cp, at] : chars)
                if (cp >= 0x2000 || cp == 0xB7 || cp == 0xD7) line += codepointToUtf8(cp) + "H";
            Window spec(plat::WindowOptions{});
            spec.resizeWindow(1100, 40);
            auto* l = spec.add<Label>(line);
            l->setGeometry(Rect(4, 4, 1092, 32));
            spec.showWindow();
            spec.invalidate(spec.outerRect());
            spec.flush();
            // (two halves, each zoomed 3x with its pixels kept)
            const Image& fb = spec.framebuffer();
            int half = fb.width() / 2;
            Image z(half * 3, fb.height() * 6);
            for (int y = 0; y < z.height(); y++)
                for (int x = 0; x < z.width(); x++) {
                    int part = y / (fb.height() * 3);
                    z.row(y)[x] = fb.pixel(std::min(fb.width() - 1, part * half + x / 3), (y % (fb.height() * 3)) / 3);
                }
            savePng(HOUT + "/glyphs-" + fileSafe(s.name) + ".png", z);
            spec.hideWindow();
        }
        for (auto& [cp, at] : chars) {
            if (cp < 0x2000 && cp != 0xB7 && cp != 0xD7) continue;  // (letters, accents: any text font has them)
            std::string ch = codepointToUtf8(cp);
            if (!ui.hasGlyph(cp))
                hit("glyph-missing", ch + " U+" + util::format("%04X", cp), skinName,
                    "no font on this computer draws it (a hollow box) in " + (ui.family().empty() ? "the ui font" : ui.family()) +
                        "; first used at " + at);
            int t = 0, b = 0;
            // the symbols drawn beside text on one line: their ink's middle against an H's
            if (ui.hasGlyph(cp) && inkRows(ui, cp, t, b)) {
                double mid = (t + b) / 2.0, h = b - t + 1;
                bool arrowish = (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x25A0 && cp <= 0x25FF) || (cp >= 0x2700 && cp <= 0x27BF) ||
                                (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2600 && cp <= 0x26FF) || cp >= 0x1F000 ||
                                cp == 0x2715 || cp == 0x2295;
                if (arrowish && (std::fabs(mid - hMid) > std::max(2.0, hH * 0.25) || h > hH * 1.6))
                    hit("glyph-baseline", ch + " U+" + util::format("%04X", cp) + " in " + s.name, skinName,
                        "ink rows " + std::to_string(t) + ".." + std::to_string(b) + " vs H " + std::to_string(Htop) + ".." +
                            std::to_string(Hbot) + " (" + r2(ui.pixelSize()) + " px " + (ui.family().empty() ? "ui" : ui.family()) +
                            "): off the line by " + r2(mid - hMid) + " px, " + r2(h / hH) + "x as tall; used at " + at,
                        -std::fabs(mid - hMid));
            }
        }
        (void)mono;
    }
    for (auto& [cp, at] : chars) {
        if (cp < 0x2000 && cp != 0xB7 && cp != 0xD7) continue;
        bool any = false;
        for (auto& s : shipped) any = any || s.second.has(cp);
        if (!any) notShipped += codepointToUtf8(cp) + " ";
    }
    hit("glyph-unshipped", "symbols no shipped font has", "all skins",
        "these characters are drawn only if the computer has a fallback font (DejaVu Sans, Noto Sans Symbols…; "
        "src/gfx/font.cpp kFallbackFamilies); the fonts in data/skins/fonts have none of them: " + notShipped);
    skin::apply(skin::find("Dark"));
}

// --- scaling ---
// a 1 px edge's cross-section: blended pixels (neither the edge's colour nor the inside's or the outside's)
int blendedEdge(const Image& fb, const Window& win, const Widget* w, Color edge) {
    Point o = w->mapToWindow(Point(0, 0));
    double s = win.scale();
    int y = (int)std::lround((o.y + w->height() / 2.0) * s);
    int x0 = (int)std::floor(o.x * s) - 1, x1 = (int)std::ceil((o.x + 2) * s) + 1;
    if (y < 0 || y >= fb.height() || x0 < 0 || x1 >= fb.width()) return 0;
    Color out = fb.colorAt(x0, y), in = fb.colorAt(x1, y);
    int n = 0;
    for (int x = x0; x <= x1; x++) {
        Color c = fb.colorAt(x, y);
        if (dist(c, edge) > 10 && dist(c, out) > 10 && dist(c, in) > 10) n++;
    }
    return n;
}
// how sharp a drawing's edges are: the mean of the largest 3 % of the steps between neighbouring pixels. A drawing
// made at the screen's pixels keeps its steps at any size; one made at 100 % and stretched to 200 % halves them
// (each step spread over two pixels)
double sharpness(const Image& fb, const Rect& d) {
    std::vector<int> steps;
    for (int y = d.y; y < d.bottom() - 1; y++)
        for (int x = d.x; x < d.right() - 1; x++) {
            Color c = fb.colorAt(x, y);
            steps.push_back(dist(c, fb.colorAt(x + 1, y)));
            steps.push_back(dist(c, fb.colorAt(x, y + 1)));
        }
    if (steps.size() < 40) return -1;
    std::sort(steps.rbegin(), steps.rend());
    size_t n = std::max<size_t>(5, steps.size() * 3 / 100);
    double s = 0;
    for (size_t i = 0; i < n; i++) s += steps[i];
    return s / n;
}

void collect(Widget* w, std::vector<Widget*>& out) {
    if (!w->isVisible()) return;
    out.push_back(w);
    for (auto& c : w->children()) collect(c.get(), out);
}

std::string pictureOf(Window& w, const std::string& name) {
    std::string f = HOUT + "/" + fileSafe(name) + ".png";
    w.invalidate(w.outerRect());
    w.flush();
    savePng(f, w.framebuffer());
    return f;
}

// a picture shrunk by an integer factor (box filter), for the contact sheet
Image shrink(const Image& src, int k) {
    Image out(std::max(1, src.width() / k), std::max(1, src.height() / k));
    for (int y = 0; y < out.height(); y++)
        for (int x = 0; x < out.width(); x++) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int yy = 0; yy < k; yy++)
                for (int xx = 0; xx < k; xx++) {
                    Color c = src.colorAt(x * k + xx, y * k + yy);
                    r += c.r; g += c.g; b += c.b; n++;
                }
            out.row(y)[x] = 0xFF000000u | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
        }
    return out;
}
void paste(Image& dst, const Image& src, int ox, int oy) {
    for (int y = 0; y < src.height() && oy + y < dst.height(); y++)
        for (int x = 0; x < src.width() && ox + x < dst.width(); x++) dst.row(oy + y)[ox + x] = src.pixel(x, y);
}

std::function<void(Window*)> onModal;  // a modal dialog opened: looked at inside its loop, before it is closed

// the Tier 1 parts on the main window: a soloed-safe track, armed tracks with monitoring, RMS meters running
void dressRig(Rig& r) {
    r.tracks[0]->armed = true;
    r.tracks[1]->armed = true;
    r.tracks[1]->monitor = Track::Monitor::In;
    r.tracks[2]->soloSafe = true;
    r.tracks[0]->soloed = true;
    r.tracks[4]->muted = true;
    r.tracks[5]->sampler = std::make_shared<SamplerSettings>();
    r.tracks[6]->padKit = std::make_shared<PadKit>();
    r.w->timeline->tracksChanged.emit();
    r.w->panel->rebuild();
    settle(2);
}

using Opener = std::function<void(Rig&)>;
std::vector<std::pair<std::string, Opener>> tier1() {
    return {
        {"Synth", [](Rig& r) { synth_dialog::openSynth(r.tracks[4]); }},
        {"Sampler", [](Rig& r) { sampler_dialog::openSampler(r.tracks[5]); }},
        {"Warp", [](Rig& r) {
             r.w->timeline->selection.selectOnly(r.firstAudio);
             r.w->timeline->warpSelected(r.firstAudio->start + SR);
         }},
        {"Slice", [](Rig& r) { r.w->timeline->sliceToPadKit(r.firstAudio); }},
        {"MIDI ports", [](Rig& r) { r.w->showMidiPorts(); }},
        {"External synth", [](Rig& r) {
             auto* d = new midi_ports_dialog::ExternalSynthDialog(r.w, r.tracks[6]);
             d->showWindow();
         }},
        {"Shortcuts", [](Rig& r) { r.w->showShortcuts(); }},
        {"Palette", [](Rig& r) { r.w->showPalette(); }},
        {"Missing samples", [](Rig& r) {
             r.firstAudio->missing = std::make_shared<MissingFile>(MissingFile{"/nowhere/Drums 1.wav", 1234, ""});
             project_safety::showMissing(*r.w, true);
         }},
        {"Pad Kit", [](Rig& r) { pad_kit_dialog::openPadKit(r.tracks[6]); }},
        {"MIDI editor", [](Rig& r) { r.w->openMidiEditor(r.firstMidi); }},
    };
}

// The RMS bar inside a level meter's peak bar: a meter at -6 dB peaks, RMS at -20 dB (a usual mix), and at -8 dB
// (a loud one): is the thin RMS bar along the middle anything but the peak bar's own colour?
void sweepMeters() {
    for (const skin::Skin& s : skin::library()) {
        skin::apply(s);
        skinName = s.name;
        scaleName = "100%";
        for (double rmsDb : {-20.0, -8.0}) {
            Window win(plat::WindowOptions{});
            win.resizeWindow(240, 30);
            auto* m = win.add<LevelMeter>(2);
            m->setGeometry(Rect(4, 4, 232, 22));
            win.showWindow();
            std::vector<float> peaks{0.5f, 0.5f}, ms{(float)std::pow(10.0, rmsDb / 10), (float)std::pow(10.0, rmsDb / 10)};
            m->push(peaks);
            m->pushRms(&ms);
            win.invalidate(win.outerRect());
            win.flush();
            const Image& fb = win.framebuffer();
            auto bars = m->barRects();
            if (bars.empty()) continue;
            int y0 = bars[0].first + 4, th = bars[0].second;
            // a column a third of the way into the RMS bar: its middle row (RMS) against its top row (the peak bar)
            int x = 4 + std::max(1, (int)(m->barsW() * 0.1));
            Color mid = fb.colorAt(x, y0 + th / 2), top = fb.colorAt(x, y0);
            double cr = contrast(mid, top);
            std::string d = "RMS at " + r2(rmsDb) + " dB: its bar " + hx(mid) + " inside the peak bar " + hx(top) + " (" + r2(cr) +
                            ":1; " + std::to_string(th) + " px bar, the RMS a third of it)";
            if (dist(mid, top) <= 8) {  // (the same colour: nothing shows it)
                std::string f = HOUT + "/meter-" + fileSafe(s.name) + "-rms" + std::to_string((int)rmsDb) + ".png";
                Image z(fb.width() * 4, fb.height() * 4);
                for (int yy = 0; yy < z.height(); yy++)
                    for (int xx = 0; xx < z.width(); xx++) z.row(yy)[xx] = fb.pixel(xx / 4, yy / 4);
                savePng(f, z);
                hit("meter-rms", "the RMS bar inside the peak bar, RMS " + r2(rmsDb) + " dB", s.name, d, cr, f);
            } else
                hnotes.push_back("meter " + s.name + ": " + d);
            win.hideWindow();
        }
    }
    skin::apply(skin::find("Dark"));
}

void sweepSkins(const std::vector<DocType>& docs) {
    App::get().setScale(1);
    scaleName = "100%";
    Rig r = makeMain(1280, 800);
    dressRig(r);
    Looks dark;
    Image sheet(3 * 427 + 4 * 8, 3 * 267 + 4 * 8, Color(40, 40, 40));
    int idx = 0;
    std::vector<Window*> keep;
    for (const skin::Skin& s : skin::library()) {
        r.w->chooseSkin(s);
        settle(2);
        skinName = s.name;
        std::string pic = pictureOf(*r.w, s.name + " 100 main");
        paste(sheet, shrink(r.w->framebuffer(), 3), 8 + (idx % 3) * (427 + 8), 8 + (idx / 3) * (267 + 8));
        idx++;
        contrastWindow(*r.w, pic);
        // the rows further down (the MIDI tracks: their instrument buttons, a muted track, the return)
        r.w->timeline->scroll->scrollTo(0, 1 << 20);
        settle(1);
        contrastWindow(*r.w, pictureOf(*r.w, s.name + " 100 main scrolled down"));
        r.w->timeline->scroll->scrollTo(0, 0);
        settle(1);
        // the newer windows
        for (auto& [name, open] : tier1()) {
            std::vector<Window*> before = shownOthers(r.w);
            std::string label = name, skinNow = s.name;
            onModal = [&, label, skinNow](Window* win) {
                std::string wp = pictureOf(*win, skinNow + " 100 " + label);
                contrastWindow(*win, wp);
            };
            open(r);
            onModal = nullptr;
            settle(2);
            for (Window* win : shownOthers(r.w))
                if (std::find(before.begin(), before.end(), win) == before.end() && !win->isPopup()) {
                    std::string wp = pictureOf(*win, s.name + " 100 " + name);
                    contrastWindow(*win, wp);
                    win->hideWindow();
                }
        }
        App::get().closeAllPopups();
        // the documented types
        Looks mine = lookAll(docs, r.w);
        if (s.name == "Dark") dark = mine;
        bool sameWindow = theme::c("window") == Color::fromHex("#2b2d31");
        judgeSurfaces(r.w);
        for (auto& [key, fb] : mine) {
            auto [fg, bg] = fb;
            if (!sameWindow && s.name != "Dark" && dark.count(key)) {
                auto [dfg, dbg] = dark[key];
                // (white and black are the same in most skins on purpose; transparent is no colour)
                auto plain = [](Color c) { return c.a == 0 || c.hexRgb() == "#ffffff" || c.hexRgb() == "#000000"; };
                bool bgSame = bg == dbg && !plain(bg) && dist(bg, theme::c("window")) > 30,
                     fgSame = fg == dfg && !plain(fg) && dist(fg, theme::c("window_text")) > 30;
                if (bgSame || fgSame)
                    hit("doc-dark-left", key, s.name,
                        std::string(bgSame ? "background " + hx(bg) : "") + (bgSame && fgSame ? " and " : "") +
                            (fgSame ? "text " + hx(fg) : "") + " stays Dark's in this skin (window " + hx(theme::c("window")) + ")");
            }
        }
        if (elapsed() > 22) { hnotes.push_back("skins after " + s.name + " skipped (time)"); break; }
    }
    savePng(HOUT + "/contact-sheet-100.png", sheet);
    r.w->chooseSkin(skin::find("Dark"));
    closeMain(r);
}

// every interface size: geometry against 100 %, blended 1 px edges, soft icons; a picture of each skin at 150 %
void sweepScales() {
    std::map<std::string, Rect> at100;
    std::map<std::string, double> soft100;
    for (int size : prefs::interfaceSizes()) {
        if (elapsed() > 27) { hnotes.push_back("interface sizes from " + std::to_string(size) + " % skipped (time)"); break; }
        App::get().setScale(size / 100.f);
        scaleName = std::to_string(size) + "%";
        skinName = "Dark";
        Rig r = makeMain(1280, 800);
        dressRig(r);
        r.w->invalidate(r.w->outerRect());
        r.w->flush();
        const Image& fb = r.w->framebuffer();
        std::string pic = pictureOf(*r.w, "Dark " + std::to_string(size) + " main");
        std::vector<Widget*> all;
        collect(r.w, all);
        int edges = 0, blended = 0;
        std::string example;
        for (Widget* w : all) {
            if (!w->isShown()) continue;
            std::string path = audit::pathOf(w);
            Rect g(w->mapToWindow(Point(0, 0)).x, w->mapToWindow(Point(0, 0)).y, w->width(), w->height());
            if (size == 100) at100[path] = g;
            else if (at100.count(path)) {
                Rect a = at100[path];
                int dw = std::abs(a.w - g.w), dh = std::abs(a.h - g.h);
                if ((dw >= 3 && dw * 10 > a.w) || (dh >= 3 && dh * 10 > a.h))
                    hit("scale-geometry", audit::describe(w) + " at " + audit::pathOf(w, false), scaleName,
                        "logical size " + std::to_string(a.w) + "x" + std::to_string(a.h) + " at 100 % but " +
                            std::to_string(g.w) + "x" + std::to_string(g.h) + " at " + scaleName,
                        -(double)std::max(dw, dh), pic);
            }
            // 1 px solid edges
            const css::Style& st = w->computedStyle();
            if (st.length("border-left-width", 0) == 1 && st.keyword("border-left-style", "none") == "solid" && w->width() > 6 &&
                w->height() > 6) {
                Color edge = ui::style::color(st, "border-left-color");
                if (edge.a == 255) {
                    edges++;
                    int n = blendedEdge(fb, *r.w, w, edge);
                    if (n > 0) {
                        blended++;
                        if (example.empty()) example = audit::describe(w);
                    }
                }
            }
            // drawings that should be sharp: icon buttons, the solo-safe lock, the monitor speaker, the overview
            bool iconic = dynamic_cast<IconButton*>(w) || dynamic_cast<SoloButton*>(w) || dynamic_cast<MonitorButton*>(w) ||
                          dynamic_cast<Overview*>(w) || (dynamic_cast<Button*>(w) && w->property("icon") != "");
            std::vector<std::string> sts;
            w->styleStates(sts);
            if (dynamic_cast<Button*>(w) && std::find(sts.begin(), sts.end(), "icon") != sts.end()) iconic = true;
            if (iconic) {
                // the drawing alone, not the button's box: the middle (the icon), or the solo-safe lock's corner
                Point o = w->mapToWindow(Point(0, 0));
                Rect area(o.x, o.y, w->width(), w->height());
                if (!dynamic_cast<Overview*>(w)) {
                    int m = std::min(w->width(), w->height()) * 3 / 10;
                    area = area.adjusted(m, m, -m, -m);
                }
                if (auto* sb = dynamic_cast<SoloButton*>(w)) {
                    if (!sb->track->soloSafe) continue;
                    int px = std::max(6, w->height() * 2 / 5);
                    area = Rect(o.x + w->width() - px - 2, o.y + 2, px, px);
                }
                Rect d = r.w->toDeviceRect(area).intersected(deviceRectOf(w));
                if (d.w > 4 && d.h > 4) {
                    // (a zoomed picture of it, pixels kept: 100 % at 4x, 200 % at 2x, the same size to compare)
                    if ((size == 100 || size == 200) && !dynamic_cast<Overview*>(w)) {
                        std::string tip = audit::shortText(w->toolTip(), 14);
                        std::string f = HOUT + "/zoom-" + fileSafe(w->type() + "-" + tip) + "-" + std::to_string(size) + ".png";
                        if (!std::filesystem::exists(f)) {
                            Rect full = deviceRectOf(w);
                            int k = size == 100 ? 8 : 4;
                            Image z(full.w * k, full.h * k);
                            for (int y = 0; y < z.height(); y++)
                                for (int x = 0; x < z.width(); x++) z.row(y)[x] = fb.pixel(full.x + x / k, full.y + y / k);
                            savePng(f, z);
                        }
                    }
                    double sh = sharpness(fb, d);
                    if (size == 100) soft100[path] = sh;
                    else if (size == 200 && sh >= 0 && soft100.count(path) && soft100[path] > 20) {
                        double ratio = sh / soft100[path];
                        std::string what = w->type() + (w->objectName.empty() ? "" : "#" + w->objectName);
                        std::string tip = w->toolTip();
                        if (!tip.empty()) what += " (" + audit::shortText(tip, 30) + ")";
                        hit(ratio < 0.75 ? "scale-blurry" : "scale-sharp", what, scaleName,
                            "its sharpest steps between pixels: " + r2(soft100[path]) + " at 100 %, " + r2(sh) + " at 200 % (" + r2(ratio) +
                                "x; ~1 is drawn at the screen's pixels, ~0.5 a 100 % drawing stretched)",
                            ratio, pic);
                    }
                }
            }
        }
        // the timeline's 1 px grid lines, on a row below the last track (nothing but the grid there)
        {
            Timeline* tl = r.w->timeline;
            ui::ScrollArea* sa = tl->scroll;
            sa->scrollTo(1 << 20, 0);  // (past the last clip: lanes with nothing but the grid)
            settle(1);
            r.w->invalidate(r.w->outerRect());
            r.w->flush();
            Rect vis = deviceRectOf(sa->viewport());
            int y = vis.y + vis.h / 2;
            int x0 = vis.x + 2, x1 = vis.right() - 2;
            Color bg = modeColor(fb, Rect(x0, y, std::max(1, x1 - x0), 1));
            int runs = 0, width = 0;
            double peak = 0;
            for (int x = x0; x < x1;) {
                if (dist(fb.colorAt(x, y), bg) <= 3) { x++; continue; }
                int w0 = x, top = 0;
                while (x < x1 && dist(fb.colorAt(x, y), bg) > 3) top = std::max(top, dist(fb.colorAt(x, y), bg)), x++;
                runs++;
                width += x - w0;
                peak += top;
            }
            if (runs) {
                double mw = (double)width / runs, mp = peak / runs;
                static double peak100 = 0;
                if (size == 100) peak100 = mp;
                std::string d = std::to_string(runs) + " lines, " + r2(mw) + " device px wide on average (" + r2(mw / r.w->scale()) +
                                " logical), their colour " + r2(mp) + " from the lane's (at 100 %: " + r2(peak100) + ")";
                if (size != 100 && (mp < peak100 * 0.8 || std::fabs(mw - std::round(mw)) > 0.2))
                    hit("scale-lines", "timeline grid lines (Timeline::grid…, stroke-width 1px)", scaleName, d, mp / std::max(1.0, peak100), pic);
                else
                    hnotes.push_back(scaleName + ": timeline grid: " + d);
            }
        }
        if (size % 100 && edges)
            hit("scale-edges", "1 px solid borders blended", scaleName,
                std::to_string(blended) + " of " + std::to_string(edges) + " widgets with a 1 px solid border have blended pixels across it (e.g. " +
                    example + ")",
                -(double)blended / edges, pic);
        else if (edges)
            hnotes.push_back(scaleName + ": " + std::to_string(blended) + " of " + std::to_string(edges) + " 1 px borders blended");
        // each skin at 150 % (a picture, and its contrast at that size)
        if (size == 150) {
            for (const skin::Skin& s : skin::library()) {
                r.w->chooseSkin(s);
                settle(1);
                skinName = s.name;
                std::string p = pictureOf(*r.w, s.name + " 150 main");
                contrastWindow(*r.w, p);
            }
            r.w->chooseSkin(skin::find("Dark"));
        }
        closeMain(r);
    }
    App::get().setScale(1);
}

void writeRaw() {
    std::ofstream o(HOUT + "/raw.md");
    o << "# Skin hunt: raw findings (tests/test_skin_hunt.cpp)\n\n";
    for (auto& n : hnotes) o << "- " << n << "\n";
    o << "\n";
    std::map<std::string, std::vector<const Hit*>> byCheck;
    for (auto& k : hitOrder) byCheck[hits[k].check].push_back(&hits[k]);
    for (auto& [check, v] : byCheck) {
        std::sort(v.begin(), v.end(), [](const Hit* a, const Hit* b) { return a->worst < b->worst; });
        o << "## " << check << " (" << v.size() << ")\n\n";
        for (const Hit* h : v) {
            o << "- **" << h->key << "**: " << h->detail << "\n  - seen " << h->count << "x in: ";
            for (size_t i = 0; i < h->where.size() && i < 12; i++) o << (i ? "; " : "") << h->where[i] << " " << r2(h->per.at(h->where[i]));
            if (h->where.size() > 12) o << "; … +" << h->where.size() - 12;
            if (!h->picture.empty()) o << "\n  - picture: " << h->picture;
            o << "\n";
        }
        o << "\n";
    }
}

}  // namespace hunt

#ifndef SKIN_HUNT_NO_MAIN  // (tests/test_contrast.cpp includes the hunt for its helpers)
int main() {
    using namespace hunt;
    plat::initWithoutDisplay();
    skin::setBuiltinDir("data/skins");
    paths::setDataDir(util::absolutePath(HOUT + "/tmp/data"));
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(HOUT + "/tmp");
    for (auto& e : std::filesystem::directory_iterator(HOUT))
        if (e.is_regular_file() && e.path().extension() == ".png") std::filesystem::remove(e.path());
    std::filesystem::remove_all(HOUT + "/crops");
    settings::setPath(HOUT + "/tmp/settings.json");
    // (modal dialogs opened by the sweeps: closed at once)
    App::get().nestedLoop = [](const std::function<bool()>& done) {
        settle(2);
        for (Window* w : std::vector<Window*>(App::get().windows()))
            if (w->isWindowShown() && w->isModal() && !w->isPopup()) {
                if (onModal) onModal(w);
                w->close();
            }
        App::get().closeAllPopups();
    };
    auto timed = [](const char* name, const std::function<void()>& fn) {
        double t0 = hElapsed();
        fn();
        printf("skin hunt: %s done in %.1f s (%zu findings so far)\n", name, hElapsed() - t0, hits.size());
        fflush(stdout);
    };
    std::vector<DocType> docs = docTypes();
    hnotes.push_back(std::to_string(docs.size()) + " type entries read from docs/skins.md");
    timed("glyphs", sweepGlyphs);
    timed("meters", sweepMeters);
    timed("skins", [&] { sweepSkins(docs); });
    timed("scales", sweepScales);
    writeRaw();
    printf("skin hunt: %zu distinct findings in %.1f s: %s/raw.md\n", hits.size(), hElapsed(), HOUT.c_str());
    CHECK(true);
    TEST_MAIN_END();
}
#endif
