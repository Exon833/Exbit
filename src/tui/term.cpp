// The terminal: keys from bytes, raw mode and its undoing (see term.h)
#include "tui/term.h"

#ifndef _WIN32  // (the Windows console's Terminal: term_win.cpp)
#include <fcntl.h>
#include <langinfo.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>

#include "platform/platform.h"
#include "tui/screen.h"

namespace tui {

// --- keys ---

namespace {

Key plain(int key, int mods = 0, std::string text = "") { return Key{key, mods, std::move(text)}; }

// a character typed (one UTF-8 sequence): letters as uppercase keys
Key character(const std::string& ch) {
    unsigned char c = (unsigned char)ch[0];
    if (ch.size() == 1) {
        if (c == ' ') return plain(plat::Key_Space, 0, " ");
        if (c >= 'a' && c <= 'z') return plain(c - 'a' + 'A', 0, ch);
        if (c >= 'A' && c <= 'Z') return plain(c, plat::ModShift, ch);
        return plain(c, 0, ch);
    }
    // the code point
    int cp = 0, n = (int)ch.size();
    cp = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (int i = 1; i < n; i++) cp = cp << 6 | ((unsigned char)ch[i] & 0x3F);
    return plain(cp, 0, ch);
}

// a control character (below 0x20, and DEL)
Key control(unsigned char c) {
    switch (c) {
    case 0x09: return plain(plat::Key_Tab);
    case 0x0A:
    case 0x0D: return plain(plat::Key_Return);
    case 0x08:
    case 0x7F: return plain(plat::Key_Backspace);
    case 0x1B: return plain(plat::Key_Escape);
    case 0x00: return plain(plat::Key_Space, plat::ModCtrl);
    default: break;
    }
    if (c <= 0x1A) return plain('A' + c - 1, plat::ModCtrl);
    return plain("\\]^_"[c - 0x1C], plat::ModCtrl);  // 0x1C…0x1F: Ctrl+\ ] ^ _
}

// xterm's modifier parameter (1 + shift 1, alt 2, ctrl 4, meta 8)
int modsOf(int param) {
    if (param < 2) return 0;
    int m = param - 1, out = 0;
    if (m & 1) out |= plat::ModShift;
    if (m & 2) out |= plat::ModAlt;
    if (m & 4) out |= plat::ModCtrl;
    if (m & 8) out |= plat::ModMeta;
    return out;
}

int tildeKey(int n) {
    switch (n) {
    case 1: case 7: return plat::Key_Home;
    case 2: return plat::Key_Insert;
    case 3: return plat::Key_Delete;
    case 4: case 8: return plat::Key_End;
    case 5: return plat::Key_PageUp;
    case 6: return plat::Key_PageDown;
    case 11: case 12: case 13: case 14: case 15: return plat::Key_F1 + n - 11;
    case 17: case 18: case 19: case 20: case 21: return plat::Key_F6 + n - 17;
    case 23: case 24: return plat::Key_F11 + n - 23;
    default: return 0;
    }
}

int letterKey(char f) {  // CSI or SS3 final letters
    switch (f) {
    case 'A': return plat::Key_Up;
    case 'B': return plat::Key_Down;
    case 'C': return plat::Key_Right;
    case 'D': return plat::Key_Left;
    case 'H': return plat::Key_Home;
    case 'F': return plat::Key_End;
    case 'P': return plat::Key_F1;
    case 'Q': return plat::Key_F2;
    case 'R': return plat::Key_F3;
    case 'S': return plat::Key_F4;
    case 'M': return plat::Key_Enter;  // (the keypad's, in application mode)
    default: return 0;
    }
}

// one key from the start of s: its length, 0 if s holds only part of one (-1: not a key, skip that many: -n)
int decodeOne(const std::string& s, Key& out) {
    unsigned char c = (unsigned char)s[0];
    if (c != 0x1B) {
        if (c < 0x20 || c == 0x7F) {
            out = control(c);
            return 1;
        }
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) return -1;  // (a stray continuation byte)
        if ((int)s.size() < n) return 0;
        out = character(s.substr(0, n));
        return n;
    }
    if (s.size() < 2) return 0;
    char k = s[1];
    if (k == '[') {
        if (s.size() < 3) return 0;
        if (s[2] == '[') {  // the Linux console's F1…F5: ESC [ [ A…E
            if (s.size() < 4) return 0;
            if (s[3] >= 'A' && s[3] <= 'E') {
                out = plain(plat::Key_F1 + (s[3] - 'A'));
                return 4;
            }
            return -4;
        }
        // CSI: parameters (digits ; ) then a final byte 0x40…0x7E
        size_t i = 2;
        int params[4] = {0, 0, 0, 0}, np = 0;
        bool any = false;
        while (i < s.size()) {
            char ch = s[i];
            if (ch >= '0' && ch <= '9') {
                if (np < 4) params[np] = params[np] * 10 + (ch - '0');
                any = true;
            } else if (ch == ';') {
                np++;
            } else if (ch >= 0x40 && ch <= 0x7E) {
                break;
            } else if (ch < 0x20 || ch > 0x7E) {
                return -(int)i;  // (not a sequence)
            }
            i++;
        }
        if (i >= s.size()) return 0;
        char f = s[i];
        int len = (int)i + 1;
        int count = any ? np + 1 : 0;
        if (f == '~') {
            int key = tildeKey(params[0]);
            if (!key) return -len;
            out = plain(key, count > 1 ? modsOf(params[1]) : 0);
            return len;
        }
        if (f == 'u') {  // CSI u (fixterms, kitty; the Windows console's keys): code point ; modifiers
            int cp = params[0], mods = count > 1 ? modsOf(params[1]) : 0;
            Key k = cp < 0x20 || cp == 0x7F ? control((unsigned char)cp) : character(utf8Of((char32_t)cp));
            k.mods |= mods;
            if (mods & (plat::ModCtrl | plat::ModAlt | plat::ModMeta)) k.text.clear();
            else if ((mods & plat::ModShift) && cp >= 'a' && cp <= 'z') k.text = std::string(1, (char)(cp - 32));
            out = k;
            return len;
        }
        if (f == 'Z') {
            out = plain(plat::Key_Backtab, plat::ModShift);
            return len;
        }
        int key = letterKey(f);
        if (!key) return -len;  // (a reply, or a key not known: left out)
        out = plain(key, count > 1 ? modsOf(params[1]) : 0);
        return len;
    }
    if (k == 'O') {  // SS3: ESC O letter (arrows and F1…F4 in application mode)
        if (s.size() < 3) return 0;
        int key = letterKey(s[2]);
        if (!key) return -3;
        out = plain(key);
        return 3;
    }
    if (k == 0x1B) {  // ESC ESC: Escape (some terminals' Alt+Escape)
        out = plain(plat::Key_Escape);
        return 2;
    }
    // Alt+key: ESC then the key
    std::string rest = s.substr(1);
    Key inner;
    int n = decodeOne(rest, inner);
    if (n == 0) return 0;
    if (n < 0) return n - 1;
    inner.mods |= plat::ModAlt;
    out = inner;
    return n + 1;
}

}  // namespace

void KeyDecoder::feed(const std::string& bytes, double now) {
    if (bytes.empty()) return;
    if (buf_.empty()) since_ = now;
    buf_ += bytes;
}

std::vector<Key> KeyDecoder::take(double now) {
    std::vector<Key> out;
    while (!buf_.empty()) {
        Key k;
        int n = decodeOne(buf_, k);
        if (n == 0) {
            if (now - since_ < ESC_WAIT_S) break;
            // never finished: an Escape (ESC alone), the rest as it comes
            if ((unsigned char)buf_[0] == 0x1B) out.push_back(plain(plat::Key_Escape));
            buf_.erase(0, 1);
            continue;
        }
        if (n > 0) out.push_back(k);
        buf_.erase(0, (size_t)(n > 0 ? n : -n));
    }
    since_ = now;
    return out;
}

// --- the terminal (its own on Linux and on Windows: what both do) ---

void Terminal::askBackground() {
    // its background asked (OSC 11), and DA1 after it, which every terminal answers: that answer ends the wait
    // for one that doesn't know OSC 11. Keys typed meanwhile are kept for read()
    write("\x1b]11;?\x1b\\\x1b[c");
    std::string got;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (;;) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0 || !waitInput(left)) break;
        got += read();
        size_t da = got.find("\x1b[?");
        if (da != std::string::npos && got.find('c', da) != std::string::npos) break;
    }
    int l = lightFromReply(got);
    light_ = l >= 0 ? l == 1 : lightFromColorFgBg(getenv("COLORFGBG")) == 1;
    // (the answers taken out: what is left was typed)
    for (const char* start : {"\x1b]11;", "\x1b[?"}) {
        size_t a = got.find(start);
        if (a == std::string::npos) continue;
        size_t b = start[1] == ']' ? got.find_first_of("\x07\x1b", a + 5) : got.find('c', a);
        if (b == std::string::npos) b = got.size() - 1;
        else if (got[b] == '\x1b') b++;  // (ESC \ ends it)
        got.erase(a, b + 1 - a);
    }
    typed_ = got;
}

int lightFromReply(const std::string& reply) {
    size_t at = reply.find("\x1b]11;rgb:");
    if (at == std::string::npos) return -1;
    double sum = 0;
    const double weight[3] = {0.299, 0.587, 0.114};
    const char* p = reply.c_str() + at + 9;
    for (int i = 0; i < 3; i++) {
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        int digits = (int)(end - p);
        if (digits < 1 || digits > 4) return -1;
        sum += weight[i] * (double)v / (double)((1ul << (4 * digits)) - 1);
        p = end + (i < 2 ? 1 : 0);
        if (i < 2 && *end != '/') return -1;
    }
    return sum > 0.5 ? 1 : 0;
}

int lightFromColorFgBg(const char* value) {
    if (!value || !*value) return -1;
    const char* last = strrchr(value, ';');
    if (!last || !last[1]) return -1;
    if (!isdigit((unsigned char)last[1])) return -1;  // ("default")
    int bg = atoi(last + 1);
    return bg == 7 || bg == 15 ? 1 : 0;
}

#ifndef _WIN32
// --- the terminal: Linux's ---

namespace {

struct termios g_saved;
std::atomic<bool> g_raw{false};
std::atomic<bool> g_resized{false};

// (also from signal handlers: write() and tcsetattr() only)
const char kEnter[] = TINYDAW_TERM_ENTER;
const char kLeave[] = TINYDAW_TERM_LEAVE;

void restore() {
    if (!g_raw.exchange(false)) return;
    ssize_t r = ::write(1, kLeave, sizeof kLeave - 1);
    (void)r;
    tcsetattr(0, TCSAFLUSH, &g_saved);
}

void onSignal(int sig) {
    restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

void onWinch(int) { g_resized = true; }

}  // namespace

bool Terminal::open(std::string& why) {
    if (open_) return true;
    if (!isatty(0) || !isatty(1)) {
        why = "the terminal UI needs a terminal (its input and output aren't one)";
        return false;
    }
    if (tcgetattr(0, &g_saved) != 0) {
        why = std::string("can't read the terminal's settings: ") + strerror(errno);
        return false;
    }
    struct termios raw = g_saved;
    raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) != 0) {
        why = std::string("can't set up the terminal: ") + strerror(errno);
        return false;
    }
    g_raw = true;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        atexit(restore);
        for (int sig : {SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) signal(sig, onSignal);
        signal(SIGWINCH, onWinch);
    }
    open_ = true;
    write(kEnter);
    askBackground();
    return true;
}

bool Terminal::waitInput(int ms) {
    struct pollfd p {0, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
}

void Terminal::close() {
    if (!open_) return;
    open_ = false;
    restore();
}

bool Terminal::size(int& w, int& h) const {
    struct winsize ws {};
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) {
        w = 80;
        h = 24;
        return false;
    }
    w = ws.ws_col;
    h = ws.ws_row;
    return true;
}

bool Terminal::resized() { return g_resized.exchange(false); }

bool Terminal::utf8() {
    static bool on = [] {
        setlocale(LC_CTYPE, "");
        const char* cs = nl_langinfo(CODESET);
        return cs && (strcmp(cs, "UTF-8") == 0 || strcmp(cs, "utf8") == 0);
    }();
    return on;
}

void Terminal::write(const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t n = ::write(1, s.data() + done, s.size() - done);
        if (n > 0) {
            done += (size_t)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            return;
        }
    }
}

std::string Terminal::read() {
    std::string out;
    out.swap(typed_);
    char buf[512];
    for (;;) {
        ssize_t n = ::read(0, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, (size_t)n);
            if (n < (ssize_t)sizeof buf) break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    return out;
}

void Terminal::watch(std::function<void()> fn) { watch_ = plat::watchFd(0, std::move(fn)); }

void Terminal::unwatch() {
    if (watch_ >= 0) plat::unwatchFd(watch_);
    watch_ = -1;
}
#endif  // !_WIN32

}  // namespace tui
