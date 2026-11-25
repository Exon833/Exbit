// A reusable layout checker for Exbit's own toolkit (tests only): walks a window's widget tree and reports what
// a user would see as broken layout. Include it from any test program:
//
//     audit::Report rep;
//     audit::checkWindow(*w, rep, "main 800x600 Dark 100%");   // every check below
//     rep.write("build/ui-samples/my-audit/report.md");
//
// Each finding has a kind, the widget (its style type, #id, text) and its path from the window; findings are
// deduplicated by (kind, path, text) and remember every context (size, skin, scale) they showed in.
// Checks:
//   clipped      a visible widget reaching outside its parent's rect (cut away when painted); not for the
//                content of a ScrollArea's viewport, which is meant to be larger
//   zero-size    a visible control (not a plain container) of zero or negative width or height
//   overlap      two visible widgets placed by the same layout (row, column, form, grid) intersecting
//   text         a Label / Button / CheckBox whose single-line text is wider than its room (cut, or elided "…")
//   min-size     a widget given less than its minimumSizeHint() / minimumSize() (it draws squashed)
//   unstable     the same state laid out twice (a full relayout; a resize away and back) gives other geometry
//   unpainted    regions of the window nothing paints (drawn onto a canary colour: what still holds it)
// The TUI's screens are checked by checkScreen (wide characters, broken box borders, cut text).
#pragma once
#include <wchar.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gfx/image.h"
#include "gfx/painter.h"
#include "tui/screen.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"

namespace audit {

struct Finding {
    std::string kind, path, widget, detail;
};

class Report {
public:
    // key: kind + path + widget; the contexts it showed in, the first detail and picture
    struct Entry {
        Finding f;
        std::vector<std::string> contexts;
        std::string picture;
        int count = 0;
        std::vector<std::string> variants;  // the other widgets of the same root cause (other rows, other texts)
    };
    std::string context;   // the sweep's current context ("main 1280x800 Dark 100%")
    std::string picture;   // the picture saved for that context (if any)
    std::map<std::string, Entry> entries;
    std::vector<std::string> order;
    // deduplicated by kind and groupKey (default: the path without the children's indices, and the widget's
    // style type and #id: the same part of every row is one finding); `widget` and `detail` kept from the first
    void add(const std::string& kind, const std::string& path, const std::string& widget, const std::string& detail,
             const std::string& groupKey = "") {
        std::string g = groupKey;
        if (g.empty()) {
            g = path;
            std::string out;
            for (size_t i = 0; i < g.size(); i++) {
                if (g[i] == '[') { while (i < g.size() && g[i] != ']') i++; continue; }
                out += g[i];
            }
            g = out + "|" + widget.substr(0, widget.find(' '));
        }
        std::string key = kind + "|" + g;
        auto it = entries.find(key);
        if (it == entries.end()) {
            order.push_back(key);
            it = entries.emplace(key, Entry{Finding{kind, path, widget, detail}, {}, picture, 0}).first;
        }
        Entry& e = it->second;
        e.count++;
        if (widget != e.f.widget && e.variants.size() < 6 &&
            std::find(e.variants.begin(), e.variants.end(), widget) == e.variants.end())
            e.variants.push_back(widget);
        if (e.contexts.empty() || e.contexts.back() != context) e.contexts.push_back(context);
        if (e.picture.empty()) e.picture = picture;
    }
    int size() const { return (int)entries.size(); }
    // a plain list (the program's own report.md is written by the caller from `entries`)
    std::string text(const std::string& kindFilter = "") const {
        std::ostringstream o;
        for (auto& k : order) {
            const Entry& e = entries.at(k);
            if (!kindFilter.empty() && e.f.kind != kindFilter) continue;
            o << "- [" << e.f.kind << "] " << e.f.widget << "  at " << e.f.path << "\n    " << e.f.detail << "\n    seen "
              << e.count << "x in " << e.contexts.size() << " context(s): ";
            for (size_t i = 0; i < e.contexts.size() && i < 8; i++) o << (i ? "; " : "") << e.contexts[i];
            if (e.contexts.size() > 8) o << "; … +" << e.contexts.size() - 8;
            if (!e.picture.empty()) o << "\n    picture: " << e.picture;
            o << "\n";
        }
        return o.str();
    }
};

inline std::string rectText(const Rect& r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.w) + "x" + std::to_string(r.h);
}

// the text a widget shows (Label, Button, CheckBox, ComboBox, LineEdit), else ""
inline std::string textOf(const ui::Widget* w) {
    if (auto* l = dynamic_cast<const ui::Label*>(w)) return l->text();
    if (auto* b = dynamic_cast<const ui::Button*>(w)) return b->text();
    if (auto* c = dynamic_cast<const ui::CheckBox*>(w)) return c->text();
    if (auto* c = dynamic_cast<const ui::ComboBox*>(w)) return c->currentText();
    if (auto* e = dynamic_cast<const ui::LineEdit*>(w)) return e->text();
    return "";
}

inline std::string shortText(const std::string& s, size_t n = 40) {
    std::string t = s;
    for (auto& c : t)
        if (c == '\n' || c == '\t') c = ' ';
    if (t.size() > n) {
        size_t cut = n;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) cut--;  // (not mid-character)
        t = t.substr(0, cut) + "…";
    }
    return t;
}

// "Label#time \"0:01.250\""
inline std::string describe(const ui::Widget* w) {
    std::string d = w->type();
    if (!w->objectName.empty()) d += "#" + w->objectName;
    std::string t = textOf(w);
    if (!t.empty()) d += " \"" + shortText(t) + "\"";
    return d;
}

// "Window > Widget#transport > Button[3]": each step its style type (#id), its index among its parent's children
inline std::string pathOf(const ui::Widget* w, bool withIndex = true) {
    std::vector<std::string> parts;
    for (const ui::Widget* p = w; p; p = p->parent()) {
        std::string s = p->type();
        if (!p->objectName.empty()) s += "#" + p->objectName;
        if (withIndex && p->parent()) s += "[" + std::to_string(p->childIndex()) + "]";
        parts.push_back(s);
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : " > ") + *it;
    return out;
}

inline bool isPlainContainer(const ui::Widget* w) { return w->type() == "Widget"; }

// inside a ScrollArea's viewport, or a window smaller than its minimum (its content may be larger: scrolled, not
// clipped)
inline bool isScrollContent(const ui::Widget* w) {
    const ui::Widget* p = w->parent();
    if (auto* win = dynamic_cast<const ui::Window*>(p); win && win->overflows()) return true;  // (a window too small: scrolls)
    if (!p || !p->parent()) return false;
    auto* sa = dynamic_cast<const ui::ScrollArea*>(p->parent());
    return sa && sa->viewport() == p;
}

// every widget a layout places (its nested layouts' too)
inline void layoutWidgets(const ui::Layout* l, std::vector<ui::Widget*>& out) {
    if (!l) return;
    if (auto* b = dynamic_cast<const ui::BoxLayout*>(l)) {
        for (auto& it : b->items) {
            if (it.widget) out.push_back(it.widget);
            if (it.layout) layoutWidgets(it.layout.get(), out);
        }
    } else if (auto* f = dynamic_cast<const ui::FormLayout*>(l)) {
        for (auto& r : f->rows) {
            if (r.label) out.push_back(r.label);
            if (r.field) out.push_back(r.field);
            if (r.fieldLayout) layoutWidgets(r.fieldLayout.get(), out);
        }
    } else if (auto* g = dynamic_cast<const ui::GridLayout*>(l)) {
        for (auto& c : g->cells)
            if (c.w) out.push_back(c.w);
    }
}

// the single-line text's room and need, for the controls whose text the toolkit draws (false: not checked)
inline bool textNeed(const ui::Widget* w, int& need, int& room, bool& elided) {
    elided = false;
    const css::Style& st = w->computedStyle();
    ui::style::Insets pad = ui::style::insets(st, "padding"), border = ui::style::insets(st, "border-width");
    if (auto* l = dynamic_cast<const ui::Label*>(w)) {
        if (l->text().empty() || l->text().find('\n') != std::string::npos) return false;
        if (l->sizeHint().y > ui::style::textHeight(l->font()) * 3 / 2 + pad.h() + 2 * l->margin) return false;  // (wrapped)
        need = ui::style::textWidth(l->font(), l->text());
        room = w->width() - pad.w() - 2 * l->margin;
        // a Label elides only when told to; else it is cut by its clip
        elided = l->minimumSizeHint().x < l->sizeHint().x;
        return true;
    }
    if (auto* b = dynamic_cast<const ui::Button*>(w)) {
        if (b->text().empty()) return false;
        std::vector<std::string> states;
        b->styleStates(states);
        bool icon = std::find(states.begin(), states.end(), "icon") != states.end();
        if (icon && b->style() == ui::Button::Style::Tool) return false;  // (its icon alone is drawn)
        need = ui::style::textWidth(b->font(), b->text()) + (icon ? 20 : 0);
        // as Button::paint places it: inside 3 px (autoRaise 5) or the border and padding, with 2 px more each side;
        // drawn elided with "…" when it doesn't fit
        if (b->style() == ui::Button::Style::Tool)
            room = w->width() - (b->property("autoRaise") == "true" ? 10 : 6) + 4;
        else
            room = w->width() - pad.w() - border.w() + 4 - (b->menu() ? 10 : 0);
        elided = true;
        return true;
    }
    if (auto* c = dynamic_cast<const ui::CheckBox*>(w)) {
        if (c->text().empty()) return false;
        Point h = c->sizeHint();
        need = h.x;
        room = w->width();
        return true;
    }
    return false;
}

struct Options {
    bool clipped = true, zero = true, overlap = true, text = true, minSize = true;
    // widgets (style types or #ids, matched against describe()) left out of every check: known custom painters
    std::vector<std::string> ignore;
};

inline bool ignored(const ui::Widget* w, const Options& o) {
    std::string d = describe(w);
    for (auto& s : o.ignore)
        if (d.find(s) != std::string::npos) return true;
    return false;
}

// the static checks on a laid-out tree (call App::get().doLayouts() first)
inline void checkTree(ui::Widget* w, Report& rep, const Options& opt = Options(), bool parentClipped = false) {
    for (auto& up : w->children()) {
        ui::Widget* c = up.get();
        if (!c->isVisible() || c->deleted_ || c->inFrame()) continue;
        if (dynamic_cast<ui::Window*>(c)) continue;
        bool skip = ignored(c, opt);
        Rect g = c->geometry();
        std::string path = pathOf(c), d = describe(c);
        bool zero = g.w <= 0 || g.h <= 0;
        if (!skip && opt.zero && zero && !isPlainContainer(c) && c->type() != "Separator")
            rep.add("zero-size", path, d, "visible but " + rectText(g) + " (in a parent of " + rectText(w->rect()) + ")");
        bool clipped = false;
        if (!zero && !isScrollContent(c)) {
            Rect in = g.intersected(w->rect());
            if (in.w < g.w || in.h < g.h) {
                clipped = true;
                if (!skip && opt.clipped && !parentClipped) {
                    std::string how = in.empty() ? "entirely outside" : "cut by " + std::to_string(g.w - in.w) + "x" +
                                                                          std::to_string(g.h - in.h) + " px";
                    rep.add("clipped", path, d, how + ": " + rectText(g) + " in a parent of " + rectText(w->rect()));
                }
            }
        }
        if (!skip && !zero && opt.minSize) {
            Point mh = c->minimumSizeHint(), mn = c->minimumSize();
            int mw = std::max(mh.x, mn.x), mhh = std::max(mh.y, mn.y);
            if (mw > g.w + 1 || mhh > g.h + 1)
                rep.add("min-size", path, d, "given " + std::to_string(g.w) + "x" + std::to_string(g.h) + ", needs at least " +
                                                 std::to_string(mw) + "x" + std::to_string(mhh));
        }
        if (!skip && !zero && opt.text) {
            int need = 0, room = 0;
            bool elided = false;
            if (textNeed(c, need, room, elided) && need > room)
                rep.add(elided ? "elided" : "text", path, d,
                        std::string(elided ? "elided with …: " : "text cut: ") + "needs " + std::to_string(need) +
                            " px, has " + std::to_string(room));
        }
        checkTree(c, rep, opt, parentClipped || clipped);
    }
    // siblings a layout placed overlapping
    if (opt.overlap && w->layout()) {
        std::vector<ui::Widget*> ws;
        layoutWidgets(w->layout(), ws);
        std::vector<ui::Widget*> shown;
        for (auto* x : ws)
            if (x->isVisible() && !x->geometry().empty() && x->parent() == w) shown.push_back(x);
        for (size_t i = 0; i < shown.size(); i++)
            for (size_t j = i + 1; j < shown.size(); j++) {
                Rect a = shown[i]->geometry(), b = shown[j]->geometry();
                Rect in = a.intersected(b);
                if (in.empty()) continue;
                if (ignored(shown[i], opt) || ignored(shown[j], opt)) continue;
                rep.add("overlap", pathOf(shown[i]), describe(shown[i]) + "  ×  " + describe(shown[j]),
                        rectText(a) + " and " + rectText(b) + " share " + std::to_string(in.w) + "x" + std::to_string(in.h) +
                            " px (" + pathOf(shown[j]) + ")");
            }
    }
}

// every widget's geometry in window coordinates (for the stability check)
inline void geometries(ui::Widget* w, std::map<ui::Widget*, Rect>& out) {
    for (auto& up : w->children()) {
        ui::Widget* c = up.get();
        if (!c->isVisible() || dynamic_cast<ui::Window*>(c)) continue;
        Point at = c->mapToWindow(Point(0, 0));
        out[c] = Rect(at.x, at.y, c->width(), c->height());
        geometries(c, out);
    }
}

inline void compareGeometries(const std::map<ui::Widget*, Rect>& a, const std::map<ui::Widget*, Rect>& b,
                              Report& rep, const std::string& how) {
    for (auto& [w, r] : a) {
        auto it = b.find(w);
        if (it == b.end() || !ui::Widget::alive(w)) continue;
        const Rect& s = it->second;
        if (r.x != s.x || r.y != s.y || r.w != s.w || r.h != s.h) {
            // only the outermost widget that moved (its children follow it)
            ui::Widget* p = w->parent();
            auto pa = a.find(p), pb = b.find(p);
            if (pa != a.end() && pb != b.end()) {
                const Rect &x = pa->second, &y = pb->second;
                if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h) continue;
            }
            rep.add("unstable", pathOf(w), describe(w), how + ": " + rectText(r) + " then " + rectText(s));
        }
    }
}

// the window laid out again (all of it), and resized away and back: the same geometry each time?
inline void checkStability(ui::Window& win, Report& rep) {
    ui::App& app = ui::App::get();
    app.doLayouts();
    std::map<ui::Widget*, Rect> first, again, back;
    geometries(&win, first);
    win.relayout();
    app.doLayouts();
    geometries(&win, again);
    compareGeometries(first, again, rep, "laid out again");
    int w = win.width(), h = win.height();
    win.resizeWindow(w + 37, h + 23);
    app.doLayouts();
    win.resizeWindow(w, h);
    app.doLayouts();
    geometries(&win, back);
    compareGeometries(first, back, rep, "resized away and back");
}

// the window painted onto a canary colour: regions left holding it (nothing painted there). Returns the picture
// (device pixels) with them marked; regions of at least minArea logical px² are reported.
inline Image checkUnpainted(ui::Window& win, Report& rep, int minArea = 1500) {
    const Color canary(1, 254, 3);
    float s = win.scale();
    int W = std::max(1, (int)std::lround(win.width() * s)), H = std::max(1, (int)std::lround(win.height() * s));
    Image img(W, H);
    img.fill(canary);
    {
        Painter p(img, s);
        p.setClip(win.rect());
        win.paintTree(p, win.rect());
    }
    // cells of 4 logical px, unpainted when nearly all their pixels are the canary
    int cell = std::max(1, (int)std::lround(4 * s));
    int cw = (W + cell - 1) / cell, ch = (H + cell - 1) / cell;
    std::vector<char> bad((size_t)cw * ch, 0);
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int n = 0, k = 0;
            for (int y = cy * cell; y < std::min(H, (cy + 1) * cell); y++)
                for (int x = cx * cell; x < std::min(W, (cx + 1) * cell); x++, n++)
                    if (img.colorAt(x, y) == canary) k++;
            bad[(size_t)cy * cw + cx] = n && k * 10 >= n * 9;
        }
    std::vector<int> seen((size_t)cw * ch, 0);
    for (int i = 0; i < cw * ch; i++) {
        if (!bad[i] || seen[i]) continue;
        std::vector<int> stack{i};
        seen[i] = 1;
        int n = 0, x0 = cw, y0 = ch, x1 = 0, y1 = 0;
        while (!stack.empty()) {
            int j = stack.back();
            stack.pop_back();
            n++;
            int x = j % cw, y = j / cw;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& d : nb) {
                int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
                int k = ny * cw + nx;
                if (bad[k] && !seen[k]) seen[k] = 1, stack.push_back(k);
            }
        }
        double cellLogical = cell / s;
        double area = n * cellLogical * cellLogical;
        if (area < minArea) continue;
        Rect r((int)(x0 * cellLogical), (int)(y0 * cellLogical), (int)((x1 - x0 + 1) * cellLogical),
                   (int)((y1 - y0 + 1) * cellLogical));
        // the widget under its middle (what should have painted it)
        ui::Widget* under = win.childAt(Point(r.x + r.w / 2, r.y + r.h / 2));
        // the window's background showing between controls, under a menu bar, status bar, label, list… is how
        // the skins draw them (see-through): only a custom-painted widget leaving its own area bare is reported
        static const std::set<std::string> seeThrough{"Widget", "Label", "PushButton", "ToolButton", "Button", "LineEdit",
                                                      "ListWidget", "TreeWidget", "MenuBar", "StatusBar", "ComboBox",
                                                      "CheckBox", "SpinBox", "GroupBox", "TabWidget", "ScrollArea",
                                                      "Separator", "ButtonBox", "Dialog", "Window", "Slider"};
        if (!under || seeThrough.count(under->type())) continue;
        std::string who = describe(under);
        // dedup by the widget under it and the region's rough shape
        rep.add("unpainted", under ? pathOf(under, false) : "Window", who,
                std::to_string((int)area) + " px² unpainted, bounds " + rectText(r) + " of " +
                    std::to_string(win.width()) + "x" + std::to_string(win.height()));
    }
    return img;
}

// every check on a shown window (laid out first)
inline Image checkWindow(ui::Window& win, Report& rep, const Options& opt = Options(), bool stability = true,
                         bool unpainted = true) {
    ui::App::get().doLayouts();
    checkTree(&win, rep, opt);
    if (stability) checkStability(win, rep);
    if (unpainted) return checkUnpainted(win, rep);
    return Image();
}

// --- the terminal's picture ---
inline int cellWidth(char32_t ch) {
    static bool locale = [] { return std::setlocale(LC_CTYPE, "C.UTF-8") || std::setlocale(LC_CTYPE, "en_US.UTF-8"); }();
    (void)locale;
    return ::wcwidth((wchar_t)ch);
}

inline bool isBoxChar(char32_t c) { return c >= 0x2500 && c <= 0x257F; }

// wide characters (two terminal columns, but one cell here: what follows is pushed right), zero-width ones taking a
// cell, replacement characters, and boxes whose borders something was drawn over
inline void checkScreen(const tui::Screen& s, Report& rep, const std::string& where) {
    rep.context = where;
    for (int y = 0; y < s.height(); y++) {
        for (int x = 0; x < s.width(); x++) {
            char32_t ch = s.at(x, y)->ch;
            if (ch < 0x80) continue;
            int cw = cellWidth(ch);
            std::string u = tui::utf8Of(ch);
            char hex[16];
            snprintf(hex, sizeof hex, "U+%04X", (unsigned)ch);
            std::string line = s.line(y);
            if (cw == 2)
                rep.add("tui-wide", where, u + " " + hex,
                        "a double-width character in one cell: the rest of row " + std::to_string(y) +
                            " is shifted right by one in a real terminal: \"" + shortText(line, 100) + "\"",
                        "wide " + u);
            else if (cw == 0 || cw < 0)
                rep.add("tui-zero-width", where, u + " " + hex,
                        std::string(cw == 0 ? "a zero-width" : "a non-printable") + " character in a cell (row " +
                            std::to_string(y) + "): the row is shifted left: \"" + shortText(line, 100) + "\"",
                        "zero " + u);
            else if (ch == 0xFFFD)
                rep.add("tui-replacement", where, u, "U+FFFD at row " + std::to_string(y));
        }
    }
    // text running into a box's right side or the screen's right edge (two letters against it: cut mid-word,
    // without an ellipsis); grouped by the kind of screen (the context without its size)
    std::string kind = where;
    for (size_t i = 0; i + 1 < kind.size(); i++)
        if (isdigit((unsigned char)kind[i]) && kind.find('x', i) != std::string::npos) {
            size_t e = kind.find(' ', i);
            kind = kind.substr(0, i) + (e == std::string::npos ? "" : kind.substr(e));
            break;
        }
    auto letter = [](char32_t c) { return c < 0x80 ? (bool)isalnum((int)c) : (c >= 0xC0 && c < 0x2000); };
    for (int y = 0; y < s.height(); y++)
        for (int x = 2; x <= s.width(); x++) {
            bool edge = x == s.width();
            if (!edge && s.at(x, y)->ch != U'│') continue;
            if (!edge && x + 1 < s.width() && s.at(x + 1, y)->ch == U'│') continue;
            if (!edge) {  // (a box's side, ┐ above it: a pane's divider cuts what scrolls under it, as meant)
                int t = y;
                while (t > 0 && s.at(x, t)->ch == U'│') t--;
                if (s.at(x, t)->ch != U'┐') continue;
            }
            // (a box keeps a space inside its side: a word against that space is as cut)
            int end = !edge && x >= 3 && s.at(x - 1, y)->ch == U' ' ? x - 1 : x;
            if (letter(s.at(end - 1, y)->ch) && letter(s.at(end - 2, y)->ch)) {
                // the word that was cut
                int b = end - 1;
                while (b > 0 && s.at(b - 1, y)->ch != U' ' && !isBoxChar(s.at(b - 1, y)->ch)) b--;
                std::string word;
                for (int i = b; i < end; i++) word += tui::utf8Of(s.at(i, y)->ch);
                rep.add("tui-cut", where, "\"" + word + "\" at " + (edge ? "the screen's edge" : "a box's side"),
                        "text cut mid-word without … at column " + std::to_string(x) + ", row " + std::to_string(y) + ": \"" +
                            shortText(s.line(y), 140) + "\"",
                        "cut|" + word);
            }
        }
    // boxes: ┌ … ┐ on a row, └ … ┘ below; their sides should be unbroken
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            if (s.at(x, y)->ch != U'┌') continue;
            int x2 = x + 1;
            while (x2 < s.width() && s.at(x2, y)->ch != U'┐' && s.at(x2, y)->ch != U'┌') x2++;
            if (x2 >= s.width() || s.at(x2, y)->ch != U'┐') continue;
            int y2 = y + 1;
            while (y2 < s.height() && s.at(x, y2)->ch != U'└') y2++;
            if (y2 >= s.height()) {
                rep.add("tui-box", where, "box at " + std::to_string(x) + "," + std::to_string(y),
                        "its bottom-left corner └ is missing (cut by the screen's bottom or drawn over)");
                continue;
            }
            if (s.at(x2, y2)->ch != U'┘')
                rep.add("tui-box", where, "box at " + std::to_string(x) + "," + std::to_string(y),
                        "its bottom-right corner is '" + tui::utf8Of(s.at(x2, y2)->ch) + "', not ┘ (row " + std::to_string(y2) + ")");
            for (int yy = y + 1; yy < y2; yy++) {
                char32_t r = s.at(x2, yy)->ch;
                if (r != U'│' && !isBoxChar(r)) {
                    rep.add("tui-box", where, "box at " + std::to_string(x) + "," + std::to_string(y),
                            "its right side is broken at row " + std::to_string(yy) + " by '" + tui::utf8Of(r) + "': \"" +
                                shortText(s.line(yy), 120) + "\"");
                    break;
                }
            }
            for (int xx = x + 1; xx < x2; xx++) {
                char32_t b = s.at(xx, y2)->ch;
                if (b != U'─' && !isBoxChar(b)) {
                    rep.add("tui-box", where, "box at " + std::to_string(x) + "," + std::to_string(y),
                            "its bottom border is broken at column " + std::to_string(xx) + " by '" + tui::utf8Of(b) + "'");
                    break;
                }
            }
        }
}

}  // namespace audit
