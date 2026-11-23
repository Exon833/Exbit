// The terminal (tinydaw --tui): raw keyboard input decoded into keys, and output. Nothing but the
// terminal's own escape sequences (ECMA-48 / VT100, as the Linux console and xterm-likes understand
// them): no terminfo, no curses, so it runs on a bare text console.
//
// KEYS. Terminals send bytes: a character, a control character (Ctrl+A is 1), or an escape sequence
// (ESC [ A for Up). KeyDecoder turns them into keys as the toolkit knows them (plat::Key, letters
// uppercase, plat::Modifier; text: what it types). What a terminal can't send isn't there: the Linux
// console sends Shift+arrows, Ctrl+arrows and Ctrl+Shift+letters as the plain keys, and nothing when a
// key is let go. Alt+key comes as ESC then the key; a lone ESC is the Escape key once nothing has
// followed it for ESC_WAIT_S.
#pragma once
#include <string>
#include <vector>

namespace tui {

struct Key {
    int key = 0;       // plat::Key (letters uppercase, other characters as their code point)
    int mods = 0;      // plat::Modifier
    std::string text;  // what it types (UTF-8), if anything
    bool operator==(const Key& o) const { return key == o.key && mods == o.mods && text == o.text; }
};

constexpr double ESC_WAIT_S = 0.05;  // a lone ESC this long: the Escape key (not the start of a sequence)

class KeyDecoder {
public:
    void feed(const std::string& bytes, double now);
    // the keys decoded so far; a sequence cut short stays pending until more comes, or (an ESC alone, or a
    // sequence never finished) until ESC_WAIT_S after it came
    std::vector<Key> take(double now);
    bool pending() const { return !buf_.empty(); }

private:
    std::string buf_;
    double since_ = 0;  // when the pending bytes came
};

// the standard input and output, while they are a terminal
class Terminal {
public:
    ~Terminal() { close(); }
    // raw input (no echo, no line editing, no signals from keys: Ctrl+C, Ctrl+S, Ctrl+Z… are keys), the
    // cursor hidden, the screen of its own where the terminal has one; restored by close(), at exit, and
    // when the program is ended by a signal or crashes. False and why if stdin or stdout isn't a terminal.
    bool open(std::string& why);
    void close();
    bool isOpen() const { return open_; }
    bool size(int& w, int& h) const;  // columns and rows
    bool resized();                   // the window changed size since last asked (SIGWINCH)
    static bool utf8();               // the locale's characters are UTF-8 (else: ASCII only)
    // its background is light (a light theme): asked of it when opened (OSC 11), else told by COLORFGBG; dark
    // when neither says
    bool light() const { return light_; }
    void write(const std::string& s);
    std::string read();               // what has been typed (without waiting)

private:
    bool open_ = false, light_ = false;
    std::string typed_;  // (keys typed while the background was asked: read() gives them first)
};

// (tests) a background's lightness from the terminal's answer to OSC 11 ("ESC ] 11 ; rgb:ffff/ffff/ffff BEL") or
// from COLORFGBG ("0;15": the text's colour, then the background's): 1 light, 0 dark, -1 not told
int lightFromReply(const std::string& reply);
int lightFromColorFgBg(const char* value);

}  // namespace tui
