// The terminal UI's terminal on the Windows console (src/tui/term_win.cpp): keys put in as console input
// records come out as the keys Linux's terminals give; pictures drawn at 80x24, 20x8 and after a resize are
// what the console holds; the console's modes and code pages are given back on close() and on a crash.
// Windows only. It runs in a console of its own (a classic conhost: it starts itself with CREATE_NEW_CONSOLE);
// --here runs in the console it was started in (over ssh -tt: a pseudo console, as Windows Terminal's).
// (make tests-win builds and runs it, with the program's platform layer: its main loop runs the work posted to it)
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "platform/platform.h"
#include "tui/screen.h"
#include "tui/term.h"

using namespace tui;

namespace {

HANDLE conIn() { return CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); }
HANDLE conOut() { return CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); }

INPUT_RECORD keyRec(WORD vk, wchar_t ch, DWORD state, bool down = true, WORD repeat = 1) {
    INPUT_RECORD r{};
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = down;
    r.Event.KeyEvent.wRepeatCount = repeat;
    r.Event.KeyEvent.wVirtualKeyCode = vk;
    r.Event.KeyEvent.uChar.UnicodeChar = ch;
    r.Event.KeyEvent.dwControlKeyState = state;
    return r;
}

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

std::string describe(const std::vector<Key>& ks) {
    std::string s;
    for (const Key& k : ks) s += "{" + std::to_string(k.key) + "," + std::to_string(k.mods) + ",\"" + k.text + "\"} ";
    return s;
}

// the keys these records make, through the console and the Terminal (as exbit --tui reads them)
std::vector<Key> typed(Terminal& t, std::vector<INPUT_RECORD> recs, bool withUps = true) {
    if (withUps) {
        std::vector<INPUT_RECORD> all;
        for (INPUT_RECORD r : recs) {
            all.push_back(r);
            if (r.Event.KeyEvent.wVirtualKeyCode) {
                r.Event.KeyEvent.bKeyDown = FALSE;
                all.push_back(r);
            }
        }
        recs = all;
    }
    HANDLE in = conIn();
    DWORD n = 0;
    WriteConsoleInputW(in, recs.data(), (DWORD)recs.size(), &n);
    CloseHandle(in);
    KeyDecoder d;
    d.feed(t.read(), 0);
    return d.take(0);  // (no waiting: the console's keys never leave a sequence pending)
}

void checkKeys(int line, Terminal& t, const std::vector<INPUT_RECORD>& recs, const std::vector<Key>& want) {
    std::vector<Key> got = typed(t, recs);
    CHECK(got == want);
    if (got != want) fprintf(stderr, "   line %d: got %s want %s\n", line, describe(got).c_str(), describe(want).c_str());
}

void testKeys(Terminal& t) {
    const DWORD S = SHIFT_PRESSED, C = LEFT_CTRL_PRESSED, A = LEFT_ALT_PRESSED;
    checkKeys(__LINE__, t, {keyRec('A', L'a', 0)}, {key('A', 0, "a")});
    checkKeys(__LINE__, t, {keyRec('A', L'A', S)}, {key('A', plat::ModShift, "A")});
    checkKeys(__LINE__, t, {keyRec('A', 1, C)}, {key('A', plat::ModCtrl)});
    checkKeys(__LINE__, t, {keyRec('C', 3, C)}, {key('C', plat::ModCtrl)});  // (Ctrl+C: a key; the program goes on)
    checkKeys(__LINE__, t, {keyRec('S', 19, RIGHT_CTRL_PRESSED | S)}, {key('S', plat::ModCtrl | plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec('F', L'f', A)}, {key('F', plat::ModAlt, "f")});
    checkKeys(__LINE__, t, {keyRec('Q', L'@', C | RIGHT_ALT_PRESSED)}, {key('@', 0, "@")});  // AltGr+Q (German): @
    checkKeys(__LINE__, t, {keyRec('2', L'@', S)}, {key('@', 0, "@")});
    checkKeys(__LINE__, t, {keyRec(VK_SPACE, L' ', 0)}, {key(plat::Key_Space, 0, " ")});
    checkKeys(__LINE__, t, {keyRec(VK_UP, 0, 0)}, {key(plat::Key_Up)});
    checkKeys(__LINE__, t, {keyRec(VK_RIGHT, 0, C)}, {key(plat::Key_Right, plat::ModCtrl)});
    checkKeys(__LINE__, t, {keyRec(VK_DOWN, 0, S)}, {key(plat::Key_Down, plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec(VK_LEFT, 0, A | S)}, {key(plat::Key_Left, plat::ModAlt | plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec(VK_HOME, 0, ENHANCED_KEY), keyRec(VK_END, 0, C)}, {key(plat::Key_Home), key(plat::Key_End, plat::ModCtrl)});
    checkKeys(__LINE__, t, {keyRec(VK_INSERT, 0, 0), keyRec(VK_DELETE, 0, 0), keyRec(VK_PRIOR, 0, 0), keyRec(VK_NEXT, 0, S)},
               {key(plat::Key_Insert), key(plat::Key_Delete), key(plat::Key_PageUp), key(plat::Key_PageDown, plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec(VK_F1, 0, 0), keyRec(VK_F5, 0, 0), keyRec(VK_F12, 0, 0), keyRec(VK_F3, 0, S)},
               {key(plat::Key_F1), key(plat::Key_F5), key(plat::Key_F12), key(plat::Key_F3, plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec(VK_ESCAPE, 27, 0)}, {key(plat::Key_Escape)});  // (at once)
    checkKeys(__LINE__, t, {keyRec(VK_ESCAPE, 27, 0), keyRec('A', L'a', 0)}, {key(plat::Key_Escape), key('A', 0, "a")});  // (not Alt+A)
    checkKeys(__LINE__, t, {keyRec(VK_RETURN, L'\r', 0), keyRec(VK_RETURN, L'\n', C)}, {key(plat::Key_Return), key(plat::Key_Return, plat::ModCtrl)});
    checkKeys(__LINE__, t, {keyRec(VK_TAB, L'\t', 0), keyRec(VK_TAB, L'\t', S)}, {key(plat::Key_Tab), key(plat::Key_Backtab, plat::ModShift)});
    checkKeys(__LINE__, t, {keyRec(VK_BACK, 8, 0)}, {key(plat::Key_Backspace)});
    checkKeys(__LINE__, t, {keyRec('X', L'x', 0, true, 3)}, {key('X', 0, "x"), key('X', 0, "x"), key('X', 0, "x")});
    checkKeys(__LINE__, t, {keyRec(VK_SHIFT, 0, S), keyRec(VK_CONTROL, 0, C)}, {});  // (modifiers alone: nothing)
    // é and a character past U+FFFF (🎵, typed as its two UTF-16 halves)
    checkKeys(__LINE__, t, {keyRec(0, 0xE9, 0)}, {key(0xE9, 0, "\xc3\xa9")});
    checkKeys(__LINE__, t, {keyRec(VK_PACKET, 0xD83C, 0), keyRec(VK_PACKET, 0xDFB5, 0)}, {key(0x1F3B5, 0, "\xf0\x9f\x8e\xb5")});
    // a terminal's answer put in as input (no key): left out, what follows comes through
    std::vector<INPUT_RECORD> reply;
    for (char c : std::string("\x1b[?1;0c")) reply.push_back(keyRec(0, (wchar_t)c, 0));
    reply.push_back(keyRec('Q', L'q', 0));
    checkKeys(__LINE__, t, reply, {key('Q', 0, "q")});
}

void windowCells(HANDLE out, int& w, int& h) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(out, &info);
    w = info.srWindow.Right - info.srWindow.Left + 1;
    h = info.srWindow.Bottom - info.srWindow.Top + 1;
}

// the console made w x h cells: its buffer and window set (the normal screen), else (the screen of a program's
// own, which follows the window) its window dragged to that size, as a person would
void setSize(int w, int h) {
    HANDLE out = conOut();
    SMALL_RECT tiny{0, 0, 1, 1};
    SetConsoleWindowInfo(out, TRUE, &tiny);
    SetConsoleScreenBufferSize(out, COORD{(SHORT)w, (SHORT)h});
    SMALL_RECT all{0, 0, (SHORT)(w - 1), (SHORT)(h - 1)};
    SetConsoleWindowInfo(out, TRUE, &all);
    int cw, ch;
    windowCells(out, cw, ch);
    HWND win = GetConsoleWindow();
    for (int i = 0; i < 4 && (cw != w || ch != h) && win; i++) {
        CONSOLE_FONT_INFO f{};
        GetCurrentConsoleFont(out, FALSE, &f);
        RECT wr, cr;
        GetWindowRect(win, &wr);
        GetClientRect(win, &cr);
        int fx = f.dwFontSize.X ? f.dwFontSize.X : 8, fy = f.dwFontSize.Y ? f.dwFontSize.Y : 16;
        int nw = (wr.right - wr.left) + (w - cw) * fx, nh = (wr.bottom - wr.top) + (h - ch) * fy;
        SetWindowPos(win, nullptr, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(100);
        windowCells(out, cw, ch);
    }
    if (cw != w || ch != h) fprintf(stderr, "   the console couldn't be made %dx%d (it is %dx%d)\n", w, h, cw, ch);
    CloseHandle(out);
}

// a row of what the console shows (the cell after a wide character left out, trailing spaces cut)
std::string consoleLine(int y, int w) {
    HANDLE out = conOut();
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(out, &info);
    std::vector<CHAR_INFO> cells((size_t)w);
    SMALL_RECT r{info.srWindow.Left, (SHORT)(info.srWindow.Top + y), (SHORT)(info.srWindow.Left + w - 1), (SHORT)(info.srWindow.Top + y)};
    ReadConsoleOutputW(out, cells.data(), COORD{(SHORT)w, 1}, COORD{0, 0}, &r);
    CloseHandle(out);
    std::wstring ws;
    for (const CHAR_INFO& c : cells)
        if (!(c.Attributes & COMMON_LVB_TRAILING_BYTE)) ws += c.Char.UnicodeChar;
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &s[0], n, nullptr, nullptr);
    size_t end = s.find_last_not_of(' ');
    return end == std::string::npos ? "" : s.substr(0, end + 1);
}

// draws a picture of this size, and checks the console shows it
void testPicture(Terminal& t, int w, int h, bool exact) {
    int tw = 0, th = 0;
    CHECK(t.size(tw, th));
    if (exact) CHECK(tw == w && th == h);
    if (tw != w || th != h) fprintf(stderr, "   the console is %dx%d (wanted %dx%d)\n", tw, th, w, h);
    Screen s(tw, th), shown(1, 1);
    s.clear();
    s.box(0, 0, tw, th, style(Cyan), "Exbit");
    s.text(1, 1, "héllo ─│┼█ ▶ 日本", style(Yellow, Default, Bold), tw - 2);
    // symbols with an emoji form: one cell each, the next one where it was (U+FE0E, sent after © and ™ by
    // Screen::output, is no cell of its own)
    s.text(1, 2, "©x™x⏮x✔x", Style(), tw - 2);
    s.text(1, th - 2, std::to_string(tw) + "x" + std::to_string(th), style(Default, Blue, Reverse), tw - 2);
    t.write(s.output(shown, Terminal::utf8()));
    Sleep(50);
    s.set(tw - 1, th - 1, U' ', Style());  // (never written: see Screen::output)
    int bad = 0;
    for (int y = 0; y < th; y++) {
        std::string got = consoleLine(y, tw), want = s.line(y);
        // (a classic console whose font has no ▶ is given its look-alike ►: term_win.cpp)
        size_t tri = want.find("▶");
        if (got != want && tri != std::string::npos) want.replace(tri, std::string("▶").size(), "►");
        for (const char* like : {"⇤«", "✓√"}) {  // (…and these theirs)
            std::string sym(like, 3), alt(like + 3);
            size_t at = want.find(sym);
            if (got != want && at != std::string::npos) want.replace(at, sym.size(), alt);
        }
        if (got != want && bad++ < 4) fprintf(stderr, "   %dx%d row %d: got \"%s\" want \"%s\"\n", tw, th, y, got.c_str(), want.c_str());
    }
    CHECK(bad == 0);
}

struct Modes {
    DWORD in = 0, out = 0;
    UINT cp = 0, outCp = 0;
    bool operator==(const Modes& o) const { return in == o.in && out == o.out && cp == o.cp && outCp == o.outCp; }
};
Modes modes() {
    Modes m;
    HANDLE in = conIn(), out = conOut();
    GetConsoleMode(in, &m.in);
    GetConsoleMode(out, &m.out);
    m.cp = GetConsoleCP();
    m.outCp = GetConsoleOutputCP();
    CloseHandle(in);
    CloseHandle(out);
    return m;
}

int runTests(bool here) {
    plat::initWithoutDisplay();  // (the main loop: what the input's watch posts runs in processEvents)
    if (!here) setSize(80, 24);
    Modes before = modes();
    {
        Terminal t;
        std::string why;
        CHECK(t.open(why));
        if (!t.isOpen()) {
            fprintf(stderr, "can't open the terminal: %s\n", why.c_str());
            return 1;
        }
        Modes on = modes();
        CHECK(on.in == (ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS));
        CHECK(on.out & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        CHECK(on.cp == CP_UTF8 && on.outCp == CP_UTF8);
        t.read();  // (anything left of the console's answers)
        testKeys(t);
        // the input watched: something typed has the main thread told
        int told = 0;
        t.watch([&] { told++; t.read(); });
        HANDLE in = conIn();
        INPUT_RECORD r = keyRec('A', L'a', 0);
        DWORD n = 0;
        WriteConsoleInputW(in, &r, 1, &n);
        CloseHandle(in);
        for (int i = 0; i < 100 && !told; i++) plat::processEvents(10);
        t.unwatch();
        CHECK(told == 1);
        testPicture(t, 80, 24, !here);
        t.resized();
        if (!here) {
            // 20x8, and a resize: told by resized(), the new size from size(), drawn right
            setSize(20, 8);
            t.read();
            CHECK(t.resized());
            CHECK(!t.resized());
            testPicture(t, 20, 8, true);
            setSize(100, 30);
            t.read();
            CHECK(t.resized());
            testPicture(t, 100, 30, true);
        }
        t.close();
        CHECK(modes() == before);
    }
    // a crash (in a child sharing this console) gives the console back as it was
    if (!here) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" --crash";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        CHECK(CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CHECK(code == (DWORD)EXCEPTION_ACCESS_VIOLATION);
        CHECK(modes() == before);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    fprintf(stderr, "%d passed, %d failed\n", check::passes(), check::failures());
    return check::failures() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--crash") {
        Terminal t;
        std::string why;
        if (!t.open(why)) return 2;
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
        *(volatile int*)nullptr = 1;
        return 3;
    }
    if (mode == "--here") return runTests(true);
    if (mode == "--demo") {  // (by hand: the keys typed and the size, drawn as exbit --tui draws; Ctrl+Q ends it)
        Terminal t;
        std::string why;
        if (!t.open(why)) return fprintf(stderr, "%s\n", why.c_str()), 1;
        KeyDecoder d;
        Screen shown(1, 1);
        std::vector<std::string> seen;
        for (bool quit = false; !quit;) {
            int w, h;
            t.size(w, h);
            Screen s(w, h);
            s.box(0, 0, w, h, style(Cyan), "Exbit console check");
            s.text(2, 1, std::to_string(w) + "x" + std::to_string(h) + "  héllo ─│┼█ ▶ 日本 🎵", style(Yellow, Default, Bold), w - 4);
            s.text(2, 2, "type keys; Ctrl+Q ends", style(Default, Blue, Reverse), w - 4);
            s.text(2, 3, "…●▼▸♪♩▶▲↗·▾█─→×✕◀░▄│»¦▮■▌▇▆▅▃▂▁═┘└┐┌┃✔✓◉◄►▓▒▐╝╚╗╔║┼┴┬┤├━⏮↳↓", Style(), w - 4);
            for (int i = 0; i < (int)seen.size() && 4 + i < h - 1; i++) s.text(2, 4 + i, seen[seen.size() - 1 - i], Style(), w - 4);
            if (t.resized()) shown = Screen(1, 1);
            t.write("\x1b[?2026h" + s.output(shown, Terminal::utf8(), t.light()) + "\x1b[?2026l");
            WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 100);
            d.feed(t.read(), GetTickCount() / 1000.0);
            for (const Key& k : d.take(GetTickCount() / 1000.0 + 1)) {
                if (k.key == 'Q' && k.mods == plat::ModCtrl) quit = true;
                seen.push_back("key " + std::to_string(k.key) + " mods " + std::to_string(k.mods) + " text \"" + k.text + "\"");
            }
        }
        t.close();
        return 0;
    }
    if (mode == "--child") {
        freopen(argv[2], "w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return runTests(false);
    }
    // in a console of its own (a classic conhost window), its findings written to a file
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string log = std::string(tmp) + "test_tui_console_win.log";
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string cmd = "\"" + std::string(self) + "\" --child \"" + log + "\"";
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "can't start the test in a console of its own (error %lu)\n", GetLastError());
        return 1;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 9);
        fprintf(stderr, "the test in its own console didn't end\n");
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (FILE* f = fopen(log.c_str(), "r")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) fputs(buf, stdout);
        fclose(f);
    }
    printf("(in a console of its own: exit %lu)\n", code);
    return (int)code;
}
