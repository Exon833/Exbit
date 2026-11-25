// A check of Windows plugins' windows under Wine (not built by default): does the mouse land where
// it is in the plugin's window, wherever that window is on the screen? (Wine works out the mouse's
// place in a window from where it believes the window is; Exbit tells its host where it really
// is, plugins/bridged.cpp.)
//
// Opens a plugin's window in an X11 window of ours at one place on the screen, double-clicks (with
// XTest, a fake mouse) at a point of the plugin's window, and notes which parameters the plugin
// changed; then the same with our window elsewhere, and with it moved while open. The same point
// must change the same parameters to the same values. For FabFilter Pro-Q 4 (the default) a
// double-click in its graph makes a band at that point's frequency and gain.
//
//   g++ -std=c++17 -O1 -g -Isrc -pthread tools/wine_mouse_check.cpp \
//       $(find build/src -name '*.o' ! -path '*/src/main.o') -o build/wine_mouse_check \
//       -lX11 -lXext -lXrandr -lXcursor -lXi -lasound -lpulse-simple -lpulse -ldl -lm
//   build/wine_mouse_check ["<plugin file>" [x y]]
#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

#include "core/util.h"
#include "plugins/plugin.h"
#include "plugins/wine.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

using namespace plug;

namespace {

// XTest, from libXtst (its header isn't needed)
using FakeMotion = int (*)(Display*, int screen, int x, int y, unsigned long delay);
using FakeButton = int (*)(Display*, unsigned button, int press, unsigned long delay);
using FakeKey = int (*)(Display*, unsigned keycode, int press, unsigned long delay);
FakeMotion fakeMotion;
FakeButton fakeButton;
FakeKey fakeKey;
bool keyTrial = false;

void frames(Plugin& p, Display* dpy, double seconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        p.idle();
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
        }
        usleep(16000);
    }
}

std::map<uint32_t, double> doubleClick(Plugin& p, Display* dpy, Window ours, int x, int y) {
    Window child;
    int rx = 0, ry = 0;
    XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), x, y, &rx, &ry, &child);
    fakeMotion(dpy, -1, rx, ry, 0);
    XFlush(dpy);
    frames(p, dpy, 0.3);
    for (int i = 0; i < 2; i++) {
        fakeButton(dpy, 1, 1, 0);
        fakeButton(dpy, 1, 0, 0);
        XFlush(dpy);
        usleep(60000);
    }
    frames(p, dpy, 1.0);
    std::map<uint32_t, double> changed;
    settleBridged(p);
    for (uint32_t id : p.edited())
        changed[id] = p.getParam(id);
    fakeMotion(dpy, -1, rx + 400, ry + 300, 0);  // (off the point, so the next one is a fresh hover)
    XFlush(dpy);
    return changed;
}

void show(Plugin& p, const char* what, const std::map<uint32_t, double>& changed) {
    printf("%s: %zu parameter(s) changed\n", what, changed.size());
    auto params = p.parameters();
    for (auto& [id, v] : changed)
        for (auto& q : params)
            if (q.id == id)
                printf("   %-28s %.4f  (%s)\n", q.name.c_str(), v, p.paramText(id, v).c_str());
}

std::map<uint32_t, double> trial(const std::string& path, Display* dpy, int wx, int wy, int x, int y, bool moveWhileOpen) {
    std::string error;
    auto p = Plugin::load(path, "", error);
    if (!p) {
        printf("cannot load: %s\n", error.c_str());
        return {};
    }
    p->prepare(48000, 512);
    Window ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), wx, wy, 400, 300, 0, 0, 0);
    XSizeHints hints{};
    hints.flags = PPosition | USPosition;
    hints.x = wx, hints.y = wy;
    XSetWMNormalHints(dpy, ours, &hints);
    XMapWindow(dpy, ours);
    XSync(dpy, False);
    int w = 0, h = 0;
    if (!p->openEditor((uintptr_t)ours, dpy, 1.0, w, h)) {
        printf("the window didn't open\n");
        return {};
    }
    XResizeWindow(dpy, ours, (unsigned)w, (unsigned)h);
    XRaiseWindow(dpy, ours);
    frames(*p, dpy, 1.5);
    if (moveWhileOpen) {
        XMoveWindow(dpy, ours, wx + 260, wy + 140);
        frames(*p, dpy, 1.0);
    }
    Window child;
    int rx = 0, ry = 0;
    XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child);
    printf("-- our window at %d,%d on the screen (%dx%d)%s\n", rx, ry, w, h, moveWhileOpen ? ", moved while open" : "");
    p->edited();
    auto changed = doubleClick(*p, dpy, ours, x, y);
    show(*p, "double-click", changed);
    if (keyTrial) {  // the keyboard: Delete (the pointer over the plugin) takes the band away again
        Window c2;
        int px = 0, py = 0;
        XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), x, y, &px, &py, &c2);
        fakeMotion(dpy, -1, px, py, 0);
        fakeButton(dpy, 1, 1, 0);  // (a click on the band: it is chosen)
        fakeButton(dpy, 1, 0, 0);
        XFlush(dpy);
        frames(*p, dpy, 0.5);
        p->edited();
        unsigned key = XKeysymToKeycode(dpy, XK_Delete);
        fakeKey(dpy, key, 1, 0);
        fakeKey(dpy, key, 0, 0);
        XFlush(dpy);
        frames(*p, dpy, 1.0);
        settleBridged(*p);
        std::map<uint32_t, double> keyed;
        for (uint32_t id : p->edited())
            keyed[id] = p->getParam(id);
        show(*p, "Delete key", keyed);
    }
    p->closeEditor();
    XDestroyWindow(dpy, ours);
    XSync(dpy, False);
    return changed;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1]
                                : util::joinPath(wine::defaultPrefix(),
                                                 "drive_c/Program Files/Common Files/VST3/FabFilter/FabFilter Pro-Q 4.vst3");
    int x = argc > 3 ? atoi(argv[2]) : 420, y = argc > 3 ? atoi(argv[3]) : 220;
    void* xtst = dlopen("libXtst.so.6", RTLD_NOW);
    fakeMotion = xtst ? (FakeMotion)dlsym(xtst, "XTestFakeMotionEvent") : nullptr;
    fakeButton = xtst ? (FakeButton)dlsym(xtst, "XTestFakeButtonEvent") : nullptr;
    fakeKey = xtst ? (FakeKey)dlsym(xtst, "XTestFakeKeyEvent") : nullptr;
    keyTrial = getenv("KEYS") != nullptr;
    Display* dpy = XOpenDisplay(nullptr);
    if (!fakeMotion || !fakeButton || !dpy) {
        printf("needs an X display and libXtst.so.6\n");
        return 2;
    }
    auto a = trial(path, dpy, 60, 60, x, y, false);
    auto b = trial(path, dpy, 700, 400, x, y, false);
    auto c = trial(path, dpy, 200, 100, x, y, true);
    bool same = !a.empty() && a.size() == b.size() && a.size() == c.size();
    for (auto& [id, v] : a)
        same = same && b.count(id) && c.count(id) && std::fabs(b[id] - v) < 0.01 && std::fabs(c[id] - v) < 0.01;
    printf("%s\n", same ? "OK: the same point did the same, wherever the window was"
                        : "DIFFERENT: the mouse doesn't land where it is (or nothing changed)");
    XCloseDisplay(dpy);
    return same ? 0 : 1;
}
