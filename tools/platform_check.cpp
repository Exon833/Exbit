// A manual check of the platform layer (not built by default): opens a small window that draws a
// coloured rectangle following the mouse (Image + present), prints every event, and checks the
// main loop (timers, posted work, watched fds), screens, clipboard reading, an offscreen window's
// capture(), a dialog, a popup, the window icon, and (from a second X connection, so the real
// mouse and keyboard are left alone) synthetic mouse, wheel and key events, an XDND file drop and
// WM_DELETE_WINDOW. Drag files onto the window to see real drops; Esc or ✕ quits.
//
//   g++ -std=c++17 -O1 -g -Isrc -pthread tools/platform_check.cpp build/src/platform/*.o \
//       build/src/gfx/*.o -o build/platform_check -lX11 -lXext -lXrandr -lXcursor -lXi -lm
//   build/platform_check [seconds]      (default 4; 0 = until the window is closed, no self test)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

#include "platform/platform.h"

static const plat::Event::Type EvExposeType = plat::Event::Expose;  // X11 #defines Expose
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <atomic>
#include <poll.h>
#include <vector>

using namespace plat;

static const char* typeName(Event::Type t) {
    static const char* names[] = {"None", "MouseDown", "MouseUp", "MouseMove", "Wheel", "KeyDown", "KeyUp",
                                  "Enter", "Leave", "FocusIn", "FocusOut", "Resize", "Move", "Expose", "Close",
                                  "DragEnter", "DragMove", "DragLeave", "Drop"};
    return (int)t < (int)(sizeof names / sizeof names[0]) ? names[t] : "?";
}

struct Demo : WindowListener {
    std::unique_ptr<plat::Window> win;
    std::vector<Event> got;
    Image fb;
    int mx = -100, my = -100;
    int moves = 0;
    int presents = 0;
    Color colour{230, 90, 40};

    void paint() {
        Rect g = win->geometry();
        if (fb.width() != g.w || fb.height() != g.h) fb = Image(g.w, g.h);
        fb.fill(Color(32, 34, 40));
        fb.fill(Rect(10, 10, g.w - 20, 6), Color(80, 160, 255));
        fb.fill(Rect(mx - 15, my - 10, 30, 20), colour);
        win->present(fb, fb.rect());
        presents++;
    }
    void onEvent(plat::Window* w, const Event& e) override {
        got.push_back(e);
        if (e.type == Event::MouseMove) {
            moves++;
            mx = e.x;
            my = e.y;
            if (moves % 20 == 1) printf("  MouseMove %d,%d buttons %d mods %d (%d so far)\n", e.x, e.y, e.buttons, e.mods, moves);
            paint();
            return;
        }
        printf("  %s", typeName(e.type));
        switch (e.type) {
            case Event::MouseDown: case Event::MouseUp:
                printf(" %d,%d button %d buttons %d mods %d", e.x, e.y, e.button, e.buttons, e.mods);
                colour = e.type == Event::MouseDown ? Color(90, 220, 120) : Color(230, 90, 40);
                paint();
                break;
            case Event::Wheel: printf(" %d,%d dx %.1f dy %.1f mods %d", e.x, e.y, e.dx, e.dy, e.mods); break;
            case Event::KeyDown: case Event::KeyUp:
                printf(" key 0x%x mods %d%s text '%s'", e.key, e.mods, e.autoRepeat ? " (repeat)" : "", e.text.c_str());
                if (e.type == Event::KeyDown && e.key == Key_Escape) quit();
                break;
            case Event::Resize: printf(" %dx%d", e.w, e.h); paint(); break;
            case Event::Move: printf(" %d,%d", e.x, e.y); break;
            case EvExposeType: paint(); break;
            case Event::Close: printf(" (closing)"); quit(); break;
            case Event::Enter: case Event::Leave: printf(" %d,%d", e.x, e.y); break;
            case Event::DragEnter: case Event::DragMove: case Event::Drop:
                printf(" %d,%d", e.x, e.y);
                for (auto& f : e.files) printf(" [%s]", f.c_str());
                break;
            default: break;
        }
        printf("  (t=%.3f)\n", e.time);
        fflush(stdout);
    }
};

struct Quiet : WindowListener {
    void onEvent(plat::Window*, const Event&) override {}
};


// --- synthetic input from a second X connection (doesn't touch the real mouse or keyboard) ---
struct Sender {
    Display* d = nullptr;
    ::Window src = 0, target = 0;
    std::string uris = "file:///tmp/a%20b.wav\r\nfile://localhost/home/x/c%C3%A9.mid\r\n";
    std::atomic<bool> statusAccepted{false}, finished{false}, stop{false};
    std::thread pumper;  // a drag source answers on its own (we wait for it inside our loop)
    Atom at(const char* n) { return XInternAtom(d, n, False); }
    void open(::Window t) {
        d = XOpenDisplay(nullptr);
        target = t;
        src = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
        XFlush(d);
        pumper = std::thread([this] {
            while (!stop) {
                pollfd p{ConnectionNumber(d), POLLIN, 0};
                poll(&p, 1, 20);
                pump();
            }
        });
    }
    ~Sender() {
        stop = true;
        if (pumper.joinable()) pumper.join();
        if (d) XCloseDisplay(d);
    }
    void pump() {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == SelectionRequest) {
                XSelectionRequestEvent& r = ev.xselectionrequest;
                XEvent rep{};
                rep.xselection.type = SelectionNotify;
                rep.xselection.requestor = r.requestor;
                rep.xselection.selection = r.selection;
                rep.xselection.target = r.target;
                rep.xselection.time = r.time;
                rep.xselection.property = r.property;
                XChangeProperty(d, r.requestor, r.property, r.target, 8, PropModeReplace,
                                (const unsigned char*)uris.data(), (int)uris.size());
                XSendEvent(d, r.requestor, False, 0, &rep);
                XFlush(d);
            } else if (ev.type == ClientMessage) {
                if (ev.xclient.message_type == at("XdndStatus")) statusAccepted = ev.xclient.data.l[1] & 1;
                if (ev.xclient.message_type == at("XdndFinished")) finished = ev.xclient.data.l[1] & 1;
            }
        }
    }
    void send(XEvent& ev, long mask) {
        XSendEvent(d, target, False, mask, &ev);
        XFlush(d);
    }
    void pointer(int type, int x, int y, unsigned button, unsigned state) {
        XEvent ev{};
        ev.type = type;
        XButtonEvent& b = ev.xbutton;  // MotionNotify shares the layout up to `state`
        b.window = target;
        b.root = DefaultRootWindow(d);
        b.x = x;
        b.y = y;
        b.state = state;
        b.same_screen = True;
        b.time = CurrentTime;
        if (type == MotionNotify) ev.xmotion.is_hint = NotifyNormal;
        else b.button = button;
        send(ev, type == MotionNotify ? PointerMotionMask : type == ButtonPress ? ButtonPressMask : ButtonReleaseMask);
    }
    void key(int type, KeySym ks, unsigned state) {
        XEvent ev{};
        ev.type = type;
        XKeyEvent& k = ev.xkey;
        k.display = d;
        k.window = target;
        k.root = DefaultRootWindow(d);
        k.keycode = XKeysymToKeycode(d, ks);
        k.state = state;
        k.same_screen = True;
        k.time = CurrentTime;
        send(ev, type == KeyPress ? KeyPressMask : KeyReleaseMask);
    }
    void client(Atom type, long l0, long l1, long l2, long l3, long l4) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = target;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = l0;
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[3] = l3;
        ev.xclient.data.l[4] = l4;
        send(ev, 0);
    }
};

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 4;  // the synthetic events are checked when > 0
    init(argc, argv);
    if (!hasDisplay()) {
        printf("no display\n");
        return 1;
    }
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) fails++;
    };

    Rect prim = primaryScreenGeometry();
    Point cp = cursorPos();
    Rect near = screenGeometry(cp);
    printf("primary screen (available) %d,%d %dx%d; cursor %d,%d on %d,%d %dx%d; buttons %d mods %d\n", prim.x, prim.y,
           prim.w, prim.h, cp.x, cp.y, near.x, near.y, near.w, near.h, heldButtons(), heldModifiers());
    printf("configDir %s\nhomeDir %s\nexecutableDir %s\ndataDir %s\n", configDir().c_str(), homeDir().c_str(),
           executableDir().c_str(), dataDir().c_str());

    // --- timers: 10 ms repeating, its ticks shouldn't drift ---
    {
        int ticks = 0, single = 0, selfRemoved = 0;
        double t0 = now();
        int id = addTimer(10, [&] { ticks++; });
        addTimer(50, [&] { single++; }, false);
        int self = 0;
        self = addTimer(5, [&] { selfRemoved++; removeTimer(self); });
        while (now() - t0 < 0.5) processEvents(-1);
        removeTimer(id);
        printf("timer: %d ticks of 10 ms in 0.5 s, one-shot ran %d, self-removing ran %d\n", ticks, single, selfRemoved);
        check(ticks >= 48 && ticks <= 51, "10 ms timer accurate");
        check(single == 1, "one-shot timer once");
        check(selfRemoved == 1, "timer removed from its own callback");
    }
    // --- postToMain from another thread, watched fd ---
    {
        bool posted = false, onMain = false;
        std::thread th([&] { postToMain([&] { posted = true; onMain = isMainThread(); }); });
        double t0 = now();
        while (!posted && now() - t0 < 1) processEvents(200);
        th.join();
        check(posted && onMain, "postToMain from a thread runs on the main thread");
        int p[2];
        if (pipe(p) == 0) {
            int got = 0;
            int wid = 0;
            wid = watchFd(p[0], [&] {
                char c;
                if (read(p[0], &c, 1) == 1) got++;
                unwatchFd(wid);
            });
            if (write(p[1], "x", 1) != 1) {}
            t0 = now();
            while (!got && now() - t0 < 1) processEvents(200);
            check(got == 1, "watched fd callback (unwatched from inside it)");
            close(p[0]);
            close(p[1]);
        }
    }
    // --- clipboard (reading only: the user's clipboard is left alone) ---
    {
        double t0 = now();
        std::string text = clipboardText();
        printf("clipboard: %zu bytes in %.0f ms: '%.40s'\n", text.size(), (now() - t0) * 1000, text.c_str());
    }

    // --- an offscreen window: present a pattern, capture it back ---
    {
        Quiet q;
        WindowOptions o;
        o.title = "offscreen";
        o.w = 120;
        o.h = 80;
        o.offscreen = true;
        auto off = plat::Window::create(o, &q);
        off->show();
        Image pat(120, 80, Color(10, 200, 30));
        pat.fill(Rect(20, 20, 40, 30), Color(250, 10, 120));
        double t0 = now();
        while (now() - t0 < 0.3) processEvents(20);
        off->present(pat, pat.rect());
        t0 = now();
        while (now() - t0 < 0.1) processEvents(20);
        Image shot;
        bool ok = off->capture(shot);
        printf("offscreen capture: %s %dx%d", ok ? "yes" : "no", shot.width(), shot.height());
        if (ok && shot.width() >= 120)
            printf(" pixel(5,5)=%08x pixel(30,30)=%08x", shot.pixel(5, 5), shot.pixel(30, 30));
        printf(" geometry %d,%d\n", off->geometry().x, off->geometry().y);
        check(ok && shot.width() == 120 && (shot.pixel(5, 5) & 0xFFFFFF) == 0x0AC81E &&
                  (shot.pixel(30, 30) & 0xFFFFFF) == 0xFA0A78,
              "offscreen window captured");
    }

    // --- the visible demo window ---
    Demo demo;
    WindowOptions o;
    o.title = "Exbit platform check — ✓ UTF-8";
    o.w = 320;
    o.h = 200;
    o.minW = 200;
    o.minH = 120;
    o.acceptsFiles = true;
    o.iconSvg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><circle cx='8' cy='8' r='7' fill='#e05a28'/></svg>";
    demo.win = plat::Window::create(o, &demo);
    demo.win->setCursor(plat::Cursor::Cross);
    demo.win->show();
    double t0 = now();
    // animate the rectangle so present() is exercised even without a mouse
    int anim = addTimer(16, [&] {
        double t = now() - t0;
        if (demo.moves == 0) {
            demo.mx = 160 + (int)(120 * std::cos(t * 3));
            demo.my = 100 + (int)(60 * std::sin(t * 3));
            demo.paint();
        }
    });
    if (seconds > 0) addTimer((int)(seconds * 1000), [] { quit(); }, false);
    Sender snd;
    if (seconds > 0) snd.open((::Window)demo.win->nativeHandle());
    int step = 0;
    Demo dlg, pop;
    addTimer(120, [&] {
        if (!snd.d) return;  // no self test
        Rect g = demo.win->geometry();
        switch (step++) {
            case 1: {
                WindowOptions d;
                d.title = "dialog";
                d.w = 180;
                d.h = 90;
                d.dialog = true;
                d.resizable = false;
                d.transientFor = demo.win.get();
                dlg.win = plat::Window::create(d, &dlg);
                dlg.win->show();
                break;
            }
            case 2: {
                Rect dg = dlg.win->geometry();
                printf("dialog at %d,%d %dx%d (centred over the window: %d,%d)\n", dg.x, dg.y, dg.w, dg.h,
                       g.x + (g.w - 180) / 2, g.y + (g.h - 90) / 2);
                check(std::abs(dg.x - (g.x + (g.w - 180) / 2)) <= 2 && std::abs(dg.y - (g.y + (g.h - 90) / 2)) <= 2,
                      "dialog centred over its parent");
                dlg.win.reset();
                WindowOptions o2;
                o2.popup = true;
                o2.w = 100;
                o2.h = 50;
                o2.x = g.x + 20;
                o2.y = g.y + 40;
                pop.win = plat::Window::create(o2, &pop);
                pop.win->show();
                break;
            }
            case 3: {
                Rect pg = pop.win->geometry();
                printf("popup at %d,%d focus %d\n", pg.x, pg.y, pop.win->hasFocus());
                check(pg.x == g.x + 20 && pg.y == g.y + 40, "popup where asked");
                check(pop.win->hasFocus(), "popup took the focus");
                pop.win.reset();
                demo.win->resize(360, 230);
                break;
            }
            case 4: {
                snd.pointer(MotionNotify, 50, 60, 0, 0);
                Atom type;
                int format;
                unsigned long n = 0, after;
                unsigned char* data = nullptr;
                XGetWindowProperty(snd.d, snd.target, snd.at("_NET_WM_ICON"), 0, 1 << 20, False, XA_CARDINAL, &type,
                                   &format, &n, &after, &data);
                long* l = (long*)data;
                printf("_NET_WM_ICON: %lu values, first %ldx%ld, centre pixel %08lx\n", n, n ? l[0] : 0, n ? l[1] : 0,
                       n > 2 + 8 * 16 + 8 ? (unsigned long)l[2 + 8 * 16 + 8] & 0xFFFFFFFFul : 0);
                check(n > 2 && l[0] == 16 && ((unsigned long)l[2 + 8 * 16 + 8] & 0xFFFFFFFFul) == 0xFFE05A28ul, "window icon from SVG");
                if (data) XFree(data);
                break;
            }
            case 5: snd.pointer(ButtonPress, 51, 61, 1, 0); break;
            case 6: snd.pointer(ButtonRelease, 51, 61, 1, Button1Mask); break;
            case 7: snd.pointer(ButtonPress, 51, 61, 4, ControlMask); snd.pointer(ButtonRelease, 51, 61, 4, ControlMask);
                    snd.pointer(ButtonPress, 51, 61, 7, 0); break;
            case 8: snd.key(KeyPress, XK_a, ShiftMask); snd.key(KeyRelease, XK_a, ShiftMask);
                    snd.key(KeyPress, XK_F5, ControlMask); snd.key(KeyRelease, XK_F5, ControlMask); break;
            case 9: XSetSelectionOwner(snd.d, snd.at("XdndSelection"), snd.src, CurrentTime);
                    snd.client(snd.at("XdndEnter"), (long)snd.src, 5L << 24, (long)snd.at("text/uri-list"), 0, 0); break;
            case 10: snd.client(snd.at("XdndPosition"), (long)snd.src, 0, ((long)(g.x + 100) << 16) | (g.y + 80), CurrentTime,
                                (long)snd.at("XdndActionCopy"));
                     break;
            case 11: snd.client(snd.at("XdndDrop"), (long)snd.src, 0, CurrentTime, 0, 0); break;
            case 12: {
                Image shot;
                bool ok = demo.win->capture(shot);
                Rect f = demo.win->frameGeometry();
                printf("window geometry %d,%d %dx%d frame %d,%d %dx%d focus %d; capture %s %dx%d centre pixel %08x\n",
                       g.x, g.y, g.w, g.h, f.x, f.y, f.w, f.h, demo.win->hasFocus(), ok ? "yes" : "no", shot.width(),
                       shot.height(), ok ? shot.pixel(shot.width() / 2, shot.height() / 2) : 0);
                double t1 = now();
                std::string text = clipboardText();
                printf("clipboard (focused): %zu bytes in %.0f ms\n", text.size(), (now() - t1) * 1000);
                break;
            }
            case 13: {
                XEvent ev{};
                snd.client(snd.at("WM_PROTOCOLS"), (long)snd.at("WM_DELETE_WINDOW"), CurrentTime, 0, 0, 0);
                (void)ev;
                break;
            }
            default: break;
        }
    });
    run();
    removeTimer(anim);
    printf("presents: %d in %.1f s (%.0f/s), mouse moves %d\n", demo.presents, now() - t0, demo.presents / (now() - t0),
           demo.moves);
    if (seconds > 0) {
        auto find = [&](Event::Type t, int nth = 0) -> const Event* {
            for (auto& e : demo.got)
                if (e.type == t && nth-- == 0) return &e;
            return nullptr;
        };
        const Event* e;
        e = find(Event::MouseMove);
        check(e && e->x == 50 && e->y == 60, "MouseMove");
        e = find(Event::MouseDown);
        check(e && e->button == LeftButton && e->buttons == LeftButton && e->x == 51, "MouseDown");
        e = find(Event::MouseUp);
        check(e && e->button == LeftButton && e->buttons == 0, "MouseUp");
        e = find(Event::Wheel);
        check(e && e->dy == 1 && e->dx == 0 && e->mods == ModCtrl, "Wheel up with Ctrl");
        e = find(Event::Wheel, 1);
        check(e && e->dx == -1 && e->dy == 0, "Wheel right (button 7)");
        check(!find(Event::Wheel, 2), "no wheel event for releases");
        e = find(Event::KeyDown);
        check(e && e->key == 'A' && e->text == "A" && e->mods == ModShift && !e->autoRepeat, "KeyDown Shift+A");
        e = find(Event::KeyUp);
        check(e && e->key == 'A', "KeyUp A");
        e = find(Event::KeyDown, 1);
        check(e && e->key == Key_F5 && e->mods == ModCtrl && e->text.empty(), "KeyDown Ctrl+F5");
        e = find(Event::DragEnter);
        check(e && e->x == 100 && e->y == 80 && e->files.size() == 2, "DragEnter with files");
        e = find(Event::Drop);
        check(e && e->files.size() == 2 && e->files[0] == "/tmp/a b.wav" && e->files[1] == "/home/x/c\xc3\xa9.mid",
              "Drop: files decoded");
        check(snd.statusAccepted && snd.finished, "XdndStatus accepted, XdndFinished sent");
        check(find(Event::Close) != nullptr, "WM_DELETE_WINDOW gives Close");
        e = find(Event::Resize);
        check(e && e->w == 360 && e->h == 230, "resize() gives Resize");
        check(demo.fb.width() == 360, "painted at the new size");
    }
    demo.win.reset();
    shutdown();
    printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
