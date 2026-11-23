// (tests) a terminal emulated: what a terminal shows of the bytes tui::Screen::output sends (CUP, ED, SGR;
// characters as wide as wcwidth says, autowrap off as tui::Terminal::open sets it; writing over half of a wide
// character blanks its other half, as xterm, VTE and the Linux console do). Needs a UTF-8 LC_CTYPE (setlocale).
#pragma once
#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#include "tui/screen.h"

namespace tui_vt {
using namespace tui;

struct Vt {
    int w, h, cx = 0, cy = 0;
    std::vector<Cell> g;  // (ch 0: the right half of a wide character)
    Style cur;
    int cups = 0, sgrs = 0, clears = 0, chars = 0, overflow = 0;
    std::map<size_t, std::u32string> marks;  // (combining characters on a cell: shown with it)
    Vt(int w_, int h_) : w(w_), h(h_), g((size_t)w_ * h_) {}
    Cell& at(int x, int y) { return g[(size_t)y * w + x]; }
    void put(char32_t cp) {
        int cw = wcwidth((wchar_t)cp);
        if (cw < 0) cw = 1;
        chars++;
        if (cw == 0) {  // (combining, zero-width joiner: on the cell before, the cursor stays)
            if (cx > 0) marks[(size_t)cy * w + cx - 1] += cp;
            return;
        }
        if (cx >= w) cx = w - 1;  // (no autowrap: the last column is written over)
        overflow += cx + cw > w;  // (a wide character in the last column: half of it off the screen)
        // (half of a wide character written over: its other half blanked)
        if (at(cx, cy).ch == 0 && cx > 0) at(cx - 1, cy).ch = U' ';
        if (cx + cw < w && at(cx + cw, cy).ch == 0) at(cx + cw, cy).ch = U' ';
        at(cx, cy) = Cell{cp, cur};
        marks.erase((size_t)cy * w + cx);
        if (cw == 2 && cx + 1 < w) at(cx + 1, cy) = Cell{0, cur};
        cx = std::min(w, cx + cw);
    }
    void feed(const std::string& b) {
        for (size_t i = 0; i < b.size();) {
            unsigned char c = (unsigned char)b[i];
            if (c == 0x1b && i + 1 < b.size() && b[i + 1] == '[') {
                size_t j = i + 2;
                bool priv = j < b.size() && b[j] == '?';
                if (priv) j++;
                std::vector<int> ps{0};
                while (j < b.size() && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
                    if (b[j] == ';') ps.push_back(0);
                    else ps.back() = ps.back() * 10 + (b[j] - '0');
                    j++;
                }
                char f = j < b.size() ? b[j] : 0;
                if (!priv && f == 'H') {
                    cups++;
                    cy = std::clamp((ps[0] ? ps[0] : 1) - 1, 0, h - 1);
                    cx = std::clamp((ps.size() > 1 && ps[1] ? ps[1] : 1) - 1, 0, w - 1);
                } else if (!priv && f == 'J') {
                    clears++;
                    for (auto& cell : g) cell = Cell{U' ', cur};
                    marks.clear();
                } else if (!priv && f == 'm') {
                    sgrs++;
                    for (int p : ps) {
                        if (p == 0) cur = Style();
                        else if (p == 1) cur.attr |= Bold;
                        else if (p == 4) cur.attr |= Underline;
                        else if (p == 7) cur.attr |= Reverse;
                        else if (p >= 30 && p <= 37) cur.fg = (uint8_t)(p - 30);
                        else if (p >= 40 && p <= 47) cur.bg = (uint8_t)(p - 40);
                    }
                }
                i = j + 1;
                continue;
            }
            int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            std::u32string cps = codePoints(b.substr(i, (size_t)n));
            for (char32_t cp : cps) put(cp);
            i += (size_t)n;
        }
    }
    std::string line(int y) {
        std::string s;
        for (int x = 0; x < w; x++) {
            if (at(x, y).ch) s += utf8Of(at(x, y).ch);
            auto m = marks.find((size_t)y * w + x);
            if (m != marks.end())
                for (char32_t c : m->second) s += utf8Of(c);
        }
        size_t e = s.find_last_not_of(' ');
        return e == std::string::npos ? "" : s.substr(0, e + 1);
    }
};

// the cells that differ between what was meant and what a terminal shows (the first row that differs)
inline int mismatches(const Screen& s, Vt& vt, int& firstRow, bool styles = false) {
    int n = 0;
    firstRow = -1;
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            if (x == s.width() - 1 && y == s.height() - 1) continue;  // (never written: reported on its own)
            if (x == s.width() - 2 && y == s.height() - 1 && s.at(x + 1, y)->ch == WIDE_TAIL) continue;  // (…nor half of it)
            const Cell& a = *s.at(x, y);
            const Cell& b = vt.at(x, y);
            if (a.ch != b.ch || (styles && a.st != b.st)) {
                n++;
                if (firstRow < 0) firstRow = y;
            }
        }
    return n;
}

}  // namespace tui_vt
