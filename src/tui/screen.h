// The terminal's picture (exbit --tui): a grid of character cells, each a character in one of the 8
// colours every terminal has (the Linux console's: black, red, green, yellow, blue, magenta, cyan,
// white, bright with Bold) on one of them, or reversed. What is drawn never relies on colour alone:
// what is selected or focused is also reversed, bold or marked. output() gives the escape sequences
// that turn the last picture sent into this one (only the cells that changed).
//
// Characters: with a UTF-8 locale as they are (lines, blocks, arrows); else lines and blocks become
// ASCII (- | + #) and other non-ASCII characters '?'. A character takes the cells a terminal gives it
// (wcwidth: CJK and most emoji two, the cell after them holding WIDE_TAIL); zero-width ones (combining
// marks, joiners) and control characters aren't drawn. Text cut to fit ends in "…".
//
// Colours: what is drawn on the terminal's own background (Default) in a colour hard to read on it is
// drawn in a darker one on a light background (output()'s `light`): yellow, cyan, white text on white.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tui {

enum Color : uint8_t { Black, Red, Green, Yellow, Blue, Magenta, Cyan, White, Default = 9 };
enum Attr : uint8_t { Plain = 0, Bold = 1, Reverse = 2, Underline = 4 };

struct Style {
    uint8_t fg = Default, bg = Default, attr = Plain;
    bool operator==(const Style& o) const { return fg == o.fg && bg == o.bg && attr == o.attr; }
    bool operator!=(const Style& o) const { return !(*this == o); }
};
inline Style style(uint8_t fg, uint8_t bg = Default, uint8_t attr = Plain) { return Style{fg, bg, attr}; }

constexpr char32_t WIDE_TAIL = 0;  // a cell covered by the wide character before it

struct Cell {
    char32_t ch = U' ';
    Style st;
    bool operator==(const Cell& o) const { return ch == o.ch && st == o.st; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

class Screen {
public:
    Screen(int w = 80, int h = 24) { resize(w, h); }
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    void clear(Style st = Style());

    Cell* at(int x, int y) { return x >= 0 && y >= 0 && x < w_ && y < h_ ? &cells_[(size_t)y * w_ + x] : nullptr; }
    const Cell* at(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_ ? &cells_[(size_t)y * w_ + x] : nullptr; }
    void set(int x, int y, char32_t ch, Style st);
    // text (UTF-8) from x on row y, at most maxW cells (-1: to the edge); the cells used
    int text(int x, int y, const std::string& s, Style st, int maxW = -1);
    void fill(int x, int y, int w, int h, char32_t ch, Style st);
    void restyle(int x, int y, int w, int h, Style st);  // these cells' style, their characters kept
    void hline(int x, int y, int w, Style st);
    void vline(int x, int y, int h, Style st);
    void box(int x, int y, int w, int h, Style st, const std::string& title = "");

    // the escape sequences that make `shown` (the terminal's picture now) this one; shown becomes it.
    // A shown picture of another size is drawn afresh. light: the terminal's background is light
    std::string output(Screen& shown, bool utf8, bool light = false) const;
    // (tests) the characters, a line per row, trailing spaces cut
    std::string lines() const;
    std::string line(int y) const;

private:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
};

// the cells a code point takes in a terminal: 2 (wide), 1, 0 (combining, joiner), -1 (a control character)
int cellWidth(char32_t cp);
// the UTF-8 of a code point; and, without UTF-8, the ASCII it is drawn as
std::string utf8Of(char32_t cp);
char asciiOf(char32_t cp);
// the code points of a UTF-8 string (invalid bytes as U+FFFD)
std::u32string codePoints(const std::string& s);
// s cut to at most n cells ("…" at the end when cut, with ellipsis); never inside a wide character
std::string fit(const std::string& s, int n, bool ellipsis = true);
int columns(const std::string& s);  // the cells it takes

}  // namespace tui
