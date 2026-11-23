// The terminal's picture (see screen.h)
#include "tui/screen.h"

#include <algorithm>
#ifdef _WIN32
#include <iterator>

#include "tui/cell_width_table.h"
#else
#include <cwchar>
#include <locale.h>
#endif

namespace tui {

int cellWidth(char32_t cp) {
    if (cp < 0x7F) return cp >= 0x20 ? 1 : -1;
    if (cp < 0xA0) return -1;  // (DEL, the C1 controls: a terminal would act on them)
    if (cp < 0x300) return 1;
#ifdef _WIN32
    // (no wcwidth on Windows: glibc's table, as Windows Terminal and Linux's terminals count them)
    const CellWidthRange* end = std::end(CELL_WIDTH_RANGES);
    const CellWidthRange* r = std::lower_bound(std::begin(CELL_WIDTH_RANGES), end, cp,
                                               [](const CellWidthRange& a, char32_t c) { return a.last < c; });
    return r != end && r->first <= cp ? r->width : 1;
#else
    // as the terminal counts them: the C library's table (in a UTF-8 locale of this thread's own, whatever the
    // program's locale is)
    static locale_t utf8 = [] {
        for (const char* name : {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"})
            if (locale_t l = newlocale(LC_CTYPE_MASK, name, (locale_t)0)) return l;
        return (locale_t)0;
    }();
    if (utf8) {
        locale_t was = uselocale(utf8);
        int w = wcwidth((wchar_t)cp);
        uselocale(was);
        return w;
    }
    // shortcut: no UTF-8 locale installed: the main wide and zero-width ranges only (a complete table if it matters)
    if ((cp >= 0x300 && cp < 0x370) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
#endif
}

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

namespace {
int widthOf(char32_t cp) {  // (a character not drawn as itself: as U+FFFD, one cell)
    int w = cellWidth(cp);
    return w < 0 ? 1 : w;
}
}  // namespace

int columns(const std::string& s) {
    int n = 0;
    for (char32_t cp : codePoints(s)) n += widthOf(cp);
    return n;
}

std::string fit(const std::string& s, int n, bool ellipsis) {
    if (columns(s) <= n) return s;
    std::u32string out;
    int room = std::max(0, ellipsis ? n - 1 : n), used = 0;
    for (char32_t cp : codePoints(s)) {
        int w = widthOf(cp);
        if (used + w > room) break;
        out += cp;
        used += w;
    }
    while (!out.empty() && out.back() == U'‍') out.pop_back();  // (no joiner left dangling)
    if (ellipsis && n > 0) out += U'…';
    std::string bytes;
    for (char32_t cp : out) bytes += utf8Of(cp);
    return bytes;
}

void Screen::resize(int w, int h) {
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    cells_.assign((size_t)w_ * h_, Cell());
}

void Screen::clear(Style st) { std::fill(cells_.begin(), cells_.end(), Cell{U' ', st}); }

void Screen::set(int x, int y, char32_t ch, Style st) {
    Cell* c = at(x, y);
    if (!c) return;
    int w = cellWidth(ch);
    Cell* next = at(x + 1, y);
    if (w < 1 || (w == 2 && !next)) ch = U' ', w = 1;  // (a control or zero-width character alone; a wide one at the edge)
    // (a wide character's halves never apart: one written over, the other becomes a space)
    if (c->ch == WIDE_TAIL && x > 0) at(x - 1, y)->ch = U' ';
    if (next && next->ch == WIDE_TAIL && w == 1) next->ch = U' ';
    *c = Cell{ch, st};
    if (w == 2) {
        if (Cell* after = at(x + 2, y); after && after->ch == WIDE_TAIL) after->ch = U' ';
        *next = Cell{WIDE_TAIL, st};
    }
}

int Screen::text(int x, int y, const std::string& s, Style st, int maxW) {
    if (y < 0 || y >= h_) return 0;
    int limit = maxW < 0 ? w_ - x : std::min(maxW, w_ - x);
    if (limit <= 0) return 0;
    std::u32string cps = codePoints(s);
    int total = 0, ink = 0;  // (ink: up to the last character that isn't a space)
    for (char32_t& cp : cps) {
        if (cp == U'\n' || cp == U'\t') cp = U' ';
        if (cellWidth(cp) < 0) cp = U'�';
        total += widthOf(cp);
        if (cp != U' ') ink = total;
    }
    bool cut = ink > limit;  // (cut: "…" after what fits)
    int room = cut ? limit - 1 : limit, n = 0;
    for (char32_t cp : cps) {
        int w = widthOf(cp);
        if (w == 0) continue;
        if (n + w > room) break;
        set(x + n, y, cp, st);
        n += w;
    }
    if (cut) {
        set(x + n++, y, U'…', st);
        while (n < limit) set(x + n++, y, U' ', st);  // (a wide character that didn't fit: its cell blank)
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
// the style a cell is sent in: text hard to read on a light background (the terminal's own), darker there
Style onGround(Style st, bool light) {
    if (!light || st.bg != Default || (st.attr & Reverse)) return st;
    if (st.fg == White) st.fg = Default;
    else if (st.fg == Yellow) st.fg = Magenta;
    else if (st.fg == Cyan) st.fg = Blue;
    return st;
}

std::string sgr(Style st) {
    std::string s = "\x1b[0";
    if (st.attr & Bold) s += ";1";
    if (st.attr & Underline) s += ";4";
    if (st.attr & Reverse) s += ";7";
    // (only the 8 colours: 38 or 48 would start an extended colour's parameters)
    if (st.fg < 8) s += ";3" + std::to_string(st.fg);
    if (st.bg < 8) s += ";4" + std::to_string(st.bg);
    return s + "m";
}
}  // namespace

std::string Screen::output(Screen& shown, bool utf8, bool light) const {
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
            // a wide character's two cells are sent as one (the tail with its head); not into the bottom-right cell
            bool head = x + 1 < w_ && cells_[i + 1].ch == WIDE_TAIL && cellWidth(c.ch) == 2 && !(y == h_ - 1 && x + 2 == w_);
            if (c.ch == WIDE_TAIL && x > 0 && cellWidth(cells_[i - 1].ch) == 2) continue;
            if (c == shown.cells_[i] && (!head || cells_[i + 1] == shown.cells_[i + 1])) continue;
            // (the bottom-right cell is never written: some terminals scroll when it is)
            if (x == w_ - 1 && y == h_ - 1) continue;
            if (cx != x || cy != y) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
            Style st = onGround(c.st, light);
            if (st != cur) {
                out += sgr(st);
                cur = st;
            }
            // (anything else as wide as it isn't meant to be, a control character: a space)
            char32_t ch = cellWidth(c.ch) != (head ? 2 : 1) ? U' ' : c.ch;
            if (utf8) out += utf8Of(ch);
            else out += asciiOf(ch);
            shown.cells_[i] = c;
            cx = x + 1;
            if (head) {
                if (!utf8 || ch == U' ') out += ' ';  // (sent as one ASCII character: a space in the tail's cell)
                shown.cells_[i + 1] = cells_[i + 1];
                cx++;
            }
            cy = y;
        }
    }
    if (!out.empty()) out += "\x1b[0m";
    return out;
}

std::string Screen::line(int y) const {
    std::string s;
    for (int x = 0; x < w_; x++)
        if (cells_[(size_t)y * w_ + x].ch != WIDE_TAIL) s += utf8Of(cells_[(size_t)y * w_ + x].ch);
    size_t end = s.find_last_not_of(' ');
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

std::string Screen::lines() const {
    std::string out;
    for (int y = 0; y < h_; y++) out += line(y) + "\n";
    return out;
}

}  // namespace tui
