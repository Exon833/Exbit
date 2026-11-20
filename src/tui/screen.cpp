// The terminal's picture (see screen.h)
#include "tui/screen.h"

#include <algorithm>

namespace tui {

std::string utf8Of(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

char asciiOf(char32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return (char)cp;
    switch (cp) {
    case U'─': case U'━': case U'═': return '-';
    case U'│': case U'┃': case U'║': return '|';
    case U'┌': case U'┐': case U'└': case U'┘': case U'├': case U'┤': case U'┬': case U'┴': case U'┼':
    case U'╔': case U'╗': case U'╚': case U'╝': return '+';
    case U'█': case U'▓': case U'■': return '#';
    case U'▒': case U'▌': case U'▐': return '=';
    case U'░': case U'·': case U'•': case U'▁': case U'▂': return '.';
    case U'▃': case U'▄': return '_';
    case U'▅': case U'▆': case U'▇': return '=';
    case U'▶': case U'►': case U'→': case U'▸': return '>';
    case U'◀': case U'◄': case U'←': return '<';
    case U'▲': case U'↑': return '^';
    case U'▼': case U'↓': case U'▾': return 'v';
    case U'…': return '~';
    case U'✓': case U'✔': return '*';
    case U'●': case U'◉': return 'o';
    case U'×': case U'✕': return 'x';
    default: return '?';
    }
}

std::u32string codePoints(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!n || i + n > s.size()) {
            out += U'�';
            i++;
            continue;
        }
        char32_t cp = n == 1 ? c : c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        bool ok = true;
        for (int k = 1; k < n; k++) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) ok = false;
            cp = cp << 6 | (d & 0x3F);
        }
        out += ok ? cp : U'�';
        i += ok ? n : 1;
    }
    return out;
}

int columns(const std::string& s) { return (int)codePoints(s).size(); }

std::string fit(const std::string& s, int n, bool ellipsis) {
    std::u32string cps = codePoints(s);
    if ((int)cps.size() <= n) return s;
    std::string out;
    int keep = std::max(0, ellipsis ? n - 1 : n);
    for (int i = 0; i < keep; i++) out += utf8Of(cps[i]);
    if (ellipsis && n > 0) out += utf8Of(U'…');
    return out;
}

void Screen::resize(int w, int h) {
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    cells_.assign((size_t)w_ * h_, Cell());
}

void Screen::clear(Style st) { std::fill(cells_.begin(), cells_.end(), Cell{U' ', st}); }

void Screen::set(int x, int y, char32_t ch, Style st) {
    if (Cell* c = at(x, y)) *c = Cell{ch, st};
}

int Screen::text(int x, int y, const std::string& s, Style st, int maxW) {
    if (y < 0 || y >= h_) return 0;
    int limit = maxW < 0 ? w_ - x : std::min(maxW, w_ - x);
    int n = 0;
    for (char32_t cp : codePoints(s)) {
        if (n >= limit) break;
        if (cp == U'\n' || cp == U'\t') cp = U' ';
        if (cp < 0x20) continue;
        set(x + n, y, cp, st);
        n++;
    }
    return n;
}

void Screen::fill(int x, int y, int w, int h, char32_t ch, Style st) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) set(x + i, y + j, ch, st);
}

void Screen::restyle(int x, int y, int w, int h, Style st) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (Cell* c = at(x + i, y + j)) c->st = st;
}

void Screen::hline(int x, int y, int w, Style st) { fill(x, y, w, 1, U'─', st); }
void Screen::vline(int x, int y, int h, Style st) { fill(x, y, 1, h, U'│', st); }

void Screen::box(int x, int y, int w, int h, Style st, const std::string& title) {
    if (w < 2 || h < 2) return;
    hline(x + 1, y, w - 2, st);
    hline(x + 1, y + h - 1, w - 2, st);
    vline(x, y + 1, h - 2, st);
    vline(x + w - 1, y + 1, h - 2, st);
    set(x, y, U'┌', st);
    set(x + w - 1, y, U'┐', st);
    set(x, y + h - 1, U'└', st);
    set(x + w - 1, y + h - 1, U'┘', st);
    if (!title.empty() && w > 4) text(x + 2, y, " " + fit(title, w - 6) + " ", Style{st.fg, st.bg, (uint8_t)(st.attr | Bold)});
}

namespace {
std::string sgr(Style st) {
    std::string s = "\x1b[0";
    if (st.attr & Bold) s += ";1";
    if (st.attr & Underline) s += ";4";
    if (st.attr & Reverse) s += ";7";
    if (st.fg != Default) s += ";3" + std::to_string(st.fg);
    if (st.bg != Default) s += ";4" + std::to_string(st.bg);
    return s + "m";
}
}  // namespace

std::string Screen::output(Screen& shown, bool utf8) const {
    std::string out;
    bool fresh = shown.w_ != w_ || shown.h_ != h_;
    if (fresh) {
        shown.resize(w_, h_);
        shown.cells_.assign(cells_.size(), Cell{U'\0', Style{255, 255, 255}});  // (differs from every cell)
        out += "\x1b[0m\x1b[H\x1b[2J";
    }
    Style cur{255, 255, 255};
    int cx = -1, cy = -1;  // where the terminal's cursor is (-1: not known)
    for (int y = 0; y < h_; y++) {
        for (int x = 0; x < w_; x++) {
            size_t i = (size_t)y * w_ + x;
            const Cell& c = cells_[i];
            if (c == shown.cells_[i]) continue;
            // (the bottom-right cell is never written: some terminals scroll when it is)
            if (x == w_ - 1 && y == h_ - 1) continue;
            if (cx != x || cy != y) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
            if (c.st != cur) {
                out += sgr(c.st);
                cur = c.st;
            }
            char32_t ch = c.ch < 0x20 ? U' ' : c.ch;
            if (utf8) out += utf8Of(ch);
            else out += asciiOf(ch);
            shown.cells_[i] = c;
            cx = x + 1;
            cy = y;
        }
    }
    if (!out.empty()) out += "\x1b[0m";
    return out;
}

std::string Screen::line(int y) const {
    std::string s;
    for (int x = 0; x < w_; x++) s += utf8Of(cells_[(size_t)y * w_ + x].ch);
    size_t end = s.find_last_not_of(' ');
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

std::string Screen::lines() const {
    std::string out;
    for (int y = 0; y < h_; y++) out += line(y) + "\n";
    return out;
}

}  // namespace tui
