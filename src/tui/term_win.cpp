// The terminal on Windows (see term.h): the console (classic conhost, or a pseudo console: Windows Terminal,
// an SSH session). Its output: the same escape sequences as on Linux, which the console understands from
// Windows 10 1607 on (ENABLE_VIRTUAL_TERMINAL_PROCESSING), written as UTF-16 (WriteConsoleW: no code page in
// the way). Its input: console input records (keys with their modifiers, size changes), turned into the
// bytes an xterm sends for them, so that KeyDecoder makes the same keys of them as on Linux; what xterm has
// no bytes for (Ctrl+Shift+letter, Ctrl+Enter…) as CSI u. The console's modes and code pages are given back
// on close(), at exit, on Ctrl+Break / the window closed / logoff, and when the program crashes.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

#include "platform/platform.h"
#include "tui/screen.h"
#include "tui/term.h"

namespace tui {

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;
DWORD g_inMode = 0, g_outMode = 0;
UINT g_inCp = 0, g_outCp = 0;
std::atomic<bool> g_raw{false};
std::atomic<bool> g_resized{false};
int g_lastW = 0, g_lastH = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
void checkFont();  // (below, with write())

const char kEnter[] = TINYDAW_TERM_ENTER;
const char kLeave[] = TINYDAW_TERM_LEAVE;

void restore() {
    if (!g_raw.exchange(false)) return;
    DWORD n = 0;
    WriteConsoleA(g_out, kLeave, sizeof kLeave - 1, &n, nullptr);
    SetConsoleMode(g_in, g_inMode);
    SetConsoleMode(g_out, g_outMode);
    if (g_outCp) SetConsoleOutputCP(g_outCp);
    if (g_inCp) SetConsoleCP(g_inCp);
}

BOOL WINAPI onCtrl(DWORD) {
    restore();
    return FALSE;  // (then what Windows does: the program ends)
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
    restore();
    return g_previousFilter ? g_previousFilter(e) : EXCEPTION_CONTINUE_SEARCH;
}

HANDLE openConsole(const wchar_t* name) {
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}



// xterm's CSI for a key: ESC [ n ; modifiers final (n 1 and no modifiers: ESC [ final)
std::string csi(int n, int m, char final) {
    if (m <= 1) return n == 1 && final != '~' ? std::string("\x1b[") + final : "\x1b[" + std::to_string(n) + final;
    return "\x1b[" + std::to_string(n) + ";" + std::to_string(m) + final;
}

std::string csiU(char32_t cp, int m) { return "\x1b[" + std::to_string((unsigned)cp) + (m > 1 ? ";" + std::to_string(m) : "") + "u"; }

wchar_t g_high = 0;  // (the first half of a character past U+FFFF, waiting for its second)

// one key record as the bytes a terminal sends for it ("" none)
std::string consoleKeyBytes(const KEY_EVENT_RECORD& k) {
    if (!k.bKeyDown) return "";
    DWORD st = k.dwControlKeyState;
    bool shift = st & SHIFT_PRESSED, ctrl = st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED),
         alt = st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
    int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    wchar_t wc = k.uChar.UnicodeChar;
    WORD vk = k.wVirtualKeyCode;
    std::string one;
    // a character: its second half joins the first; a lone half is dropped
    char32_t ch = wc;
    if (wc >= 0xD800 && wc <= 0xDBFF) {
        g_high = wc;
        return "";
    }
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (!g_high) return "";
        ch = 0x10000 + (((char32_t)g_high - 0xD800) << 10) + (wc - 0xDC00);
        g_high = 0;
        vk = 0;  // (as typed text)
    }
    if (vk == 0 || vk == VK_PACKET) {  // text put in as it is (a terminal's answers, pasted or injected characters)
        if (ch) one = utf8Of(ch);
    } else {
        switch (vk) {
        case VK_UP: one = csi(1, m, 'A'); break;
        case VK_DOWN: one = csi(1, m, 'B'); break;
        case VK_RIGHT: one = csi(1, m, 'C'); break;
        case VK_LEFT: one = csi(1, m, 'D'); break;
        case VK_HOME: one = csi(1, m, 'H'); break;
        case VK_END: one = csi(1, m, 'F'); break;
        case VK_INSERT: one = csi(2, m, '~'); break;
        case VK_DELETE: one = csi(3, m, '~'); break;
        case VK_PRIOR: one = csi(5, m, '~'); break;
        case VK_NEXT: one = csi(6, m, '~'); break;
        case VK_ESCAPE: one = "\x1b\x1b"; break;  // (ESC ESC: Escape at once, no waiting to see if a sequence follows)
        case VK_TAB: one = m == 1 ? "\t" : m == 2 ? "\x1b[Z" : csiU(9, m); break;
        case VK_RETURN: one = m == 1 ? "\r" : csiU(13, m); break;
        case VK_BACK: one = m == 1 ? "\x7f" : csiU(127, m); break;
        default:
            if (vk >= VK_F1 && vk <= VK_F12) {
                static const int n[12] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
                one = csi(n[vk - VK_F1], m, '~');
            } else if (ctrl && alt && ch >= 0x20) {
                one = utf8Of(ch);  // AltGr (Windows says Ctrl+Alt): the character it types
            } else if (ctrl && vk >= 'A' && vk <= 'Z') {
                one = shift || alt ? csiU((char32_t)(vk - 'A' + 'a'), m) : std::string(1, (char)(vk - 'A' + 1));
            } else if (ctrl && vk == VK_SPACE) {
                one = m == 5 ? std::string(1, '\0') : csiU(' ', m);
            } else if (ctrl && ch >= 0x20) {
                one = csiU(ch, m);  // (Ctrl+digit, Ctrl+punctuation)
            } else if (ctrl && ch) {
                one = std::string(1, (char)ch);  // (Ctrl+[ \ ] ^ _: their control characters)
            } else if (ch) {
                one = (alt ? "\x1b" : "") + utf8Of(ch);
            }
        }
    }
    std::string out;
    for (int i = 0; i < std::max<int>(1, k.wRepeatCount); i++) out += one;
    return out;
}

}  // namespace

bool Terminal::open(std::string& why) {
    if (open_) return true;
    if (!GetConsoleWindow() && !AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) {
        why = "the terminal UI needs a console, and none could be had (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_in = openConsole(L"CONIN$");
    g_out = openConsole(L"CONOUT$");
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE || !GetConsoleMode(g_in, &g_inMode) ||
        !GetConsoleMode(g_out, &g_outMode)) {
        why = "the terminal UI needs a console (its input and output aren't one: error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD out = g_outMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(g_out, out | DISABLE_NEWLINE_AUTO_RETURN) && !SetConsoleMode(g_out, out)) {
        why = "this console doesn't understand terminal escape sequences (Windows 10 1607 or later is needed)";
        return false;
    }
    // keys as they are: no line editing, no echo, Ctrl+C a key; size changes told; Quick Edit's selecting off
    if (!SetConsoleMode(g_in, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)) {
        SetConsoleMode(g_out, g_outMode);
        why = "can't set up the console's input (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_inCp = GetConsoleCP();
    g_outCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    g_raw = true;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        atexit(restore);
        SetConsoleCtrlHandler(onCtrl, TRUE);
        g_previousFilter = SetUnhandledExceptionFilter(onCrash);
    }
    open_ = true;
    size(g_lastW, g_lastH);
    checkFont();
    write(kEnter);
    askBackground();
    return true;
}

void Terminal::close() {
    if (!open_) return;
    unwatch();
    open_ = false;
    restore();
}

bool Terminal::size(int& w, int& h) const {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_out, &info)) {
        w = 80;
        h = 24;
        return false;
    }
    w = info.srWindow.Right - info.srWindow.Left + 1;
    h = info.srWindow.Bottom - info.srWindow.Top + 1;
    return true;
}

bool Terminal::resized() {
    int w, h;
    size(w, h);
    bool changed = w != g_lastW || h != g_lastH;
    g_lastW = w, g_lastH = h;
    return g_resized.exchange(false) || changed;
}

bool Terminal::utf8() { return true; }  // (written as UTF-16: every character reaches the console)

namespace {

// The classic console draws a character its font hasn't got as a box (it has no fallback fonts; Windows
// Terminal has): such a character is drawn as a look-alike the font has, else as ASCII (as for a terminal
// without UTF-8). A pseudo console's terminal finds its characters itself: g_fontDc none.
HDC g_fontDc = nullptr;
std::unordered_map<wchar_t, bool> g_fontHas;

void checkFont() {
    if (g_fontDc || getenv("WT_SESSION") || !IsWindowVisible(GetConsoleWindow())) return;
    CONSOLE_FONT_INFOEX f{};
    f.cbSize = sizeof f;
    if (!GetCurrentConsoleFontEx(g_out, FALSE, &f) || !f.FaceName[0]) return;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, f.FaceName);
    if (!dc || !font) {
        if (dc) DeleteDC(dc);
        return;
    }
    SelectObject(dc, font);
    g_fontDc = dc;
}

bool fontHas(wchar_t c) {
    auto it = g_fontHas.find(c);
    if (it != g_fontHas.end()) return it->second;
    WORD glyph = 0xFFFF;
    bool has = GetGlyphIndicesW(g_fontDc, &c, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS) == 1 && glyph != 0xFFFF;
    return g_fontHas[c] = has;
}

// what the console's font draws for a character of one cell
wchar_t drawable(char32_t cp) {
    if (cp < 0x10000 && fontHas((wchar_t)cp)) return (wchar_t)cp;
    static const char32_t like[][2] = {{U'▶', U'►'}, {U'▸', U'►'}, {U'◀', U'◄'}, {U'◂', U'◄'}, {U'▾', U'▼'},
                                       {U'▴', U'▲'}, {U'✓', U'√'}, {U'✔', U'√'}, {U'◉', U'●'}, {U'✕', U'×'},
                                       {U'▮', U'█'}, {U'▇', U'█'}, {U'▆', U'█'}, {U'▅', U'▄'}, {U'▃', U'▄'},
                                       {U'▂', U'_'}, {U'▁', U'_'}, {U'♩', U'♪'}, {U'↗', U'/'}, {U'↳', U'└'}, {U'⇤', U'«'},
                                       {U'⇥', U'»'}};
    for (const auto& l : like)
        if (l[0] == cp && fontHas((wchar_t)l[1])) return (wchar_t)l[1];
    return (wchar_t)asciiOf(cp);
}

}  // namespace

void Terminal::write(const std::string& s) {
    if (s.empty()) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    // U+FE0E ("as text", after a symbol that has an emoji form: text_symbols.h) left out: a classic console, and
    // Windows 10's pseudo consoles (ssh, editors' terminals), give it a cell of its own; Windows Terminal ignores it
    w.erase(std::remove(w.begin(), w.end(), L'\xFE0E'), w.end());
    if (g_fontDc) {
        std::wstring d;
        d.reserve(w.size());
        for (size_t i = 0; i < w.size(); i++) {
            char32_t cp = w[i];
            bool pair = cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF;
            if (pair) cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            if (cp >= 0x80 && cellWidth(cp) == 1) {
                d += drawable(cp);
            } else {
                d += w[i];
                if (pair) d += w[i + 1];
            }
            i += pair;
        }
        w.swap(d);
    }
    size_t done = 0;
    while (done < w.size()) {
        size_t chunk = std::min<size_t>(w.size() - done, 16384);  // (old consoles take only so much at once)
        if (done + chunk < w.size() && w[done + chunk - 1] >= 0xD800 && w[done + chunk - 1] <= 0xDBFF) chunk--;
        DWORD wrote = 0;
        if (!WriteConsoleW(g_out, w.data() + done, (DWORD)chunk, &wrote, nullptr) || !wrote) return;
        done += wrote;
    }
}

std::string Terminal::read() {
    std::string out;
    out.swap(typed_);
    DWORD count = 0;
    while (GetNumberOfConsoleInputEvents(g_in, &count) && count) {
        INPUT_RECORD recs[128];
        DWORD got = 0;
        if (!ReadConsoleInputW(g_in, recs, std::min<DWORD>(count, 128), &got) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (recs[i].EventType == KEY_EVENT) out += consoleKeyBytes(recs[i].Event.KeyEvent);
            else if (recs[i].EventType == WINDOW_BUFFER_SIZE_EVENT) g_resized = true;
        }
    }
    return out;
}

bool Terminal::waitInput(int ms) { return WaitForSingleObject(g_in, (DWORD)ms) == WAIT_OBJECT_0; }

namespace {
struct Watch {
    std::atomic<bool> stop{false}, posted{false};
    std::function<void()> fn;
    std::thread thread;
};
std::shared_ptr<Watch> g_watch;
}  // namespace

// (no fd to watch: a thread waits for the console's input and has the main thread read it)
void Terminal::watch(std::function<void()> fn) {
    unwatch();
    auto w = std::make_shared<Watch>();
    w->fn = std::move(fn);
    w->thread = std::thread([w] {
        while (!w->stop) {
            if (w->posted) {
                Sleep(1);
                continue;
            }
            if (WaitForSingleObject(g_in, 50) != WAIT_OBJECT_0 || w->stop) continue;
            w->posted = true;
            plat::postToMain([w] {
                if (!w->stop) w->fn();
                w->posted = false;
            });
        }
    });
    g_watch = w;
    watch_ = 1;
}

void Terminal::unwatch() {
    if (!g_watch) return;
    g_watch->stop = true;
    if (g_watch->thread.joinable()) g_watch->thread.join();
    g_watch.reset();
    watch_ = -1;
}

}  // namespace tui
