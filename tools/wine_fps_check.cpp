// A check of how quickly Windows plugins' windows respond under Wine (not built by default): opens a
// plugin's window in an X11 window of ours (as the plugin window does), drags a control with XTest
// (a fake mouse) for a few seconds, and meanwhile photographs the area around it every few
// milliseconds: the distinct pictures per second are how often the plugin really redraws while
// being played with. Also the parameter changes it reported, and the Wine host's CPU time.
//
//   g++ -std=c++17 -O1 -g -Isrc -pthread tools/wine_fps_check.cpp \
//       $(find build/src -name '*.o' ! -path '*/src/main.o') -o build/wine_fps_check \
//       -lX11 -lXext -lXrandr -lXcursor -lXi -lasound -lpulse-simple -lpulse -ldl -lm
//   build/wine_fps_check "<plugin file>" <x> <y> [seconds]       (x, y: a knob in its window)
//   FLOAT=1 …   the plugin's window as a window of its own (not inside ours), for comparison
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/util.h"
#include "plugins/plugin.h"
#include "plugins/wine.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

using namespace plug;

namespace {

using FakeMotion = int (*)(Display*, int screen, int x, int y, unsigned long delay);
using FakeButton = int (*)(Display*, unsigned button, int press, unsigned long delay);

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double cpuSeconds(int pid) {  // utime + stime of a process
    std::string stat;
    if (!util::readFile("/proc/" + std::to_string(pid) + "/stat", stat))
        return 0;
    size_t close = stat.rfind(')');
    auto fields = util::splitWords(stat.substr(close + 2));
    if (fields.size() < 13)
        return 0;
    return (std::stod(fields[11]) + std::stod(fields[12])) / (double)sysconf(_SC_CLK_TCK);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: wine_fps_check <plugin file> <x> <y> [seconds]\n");
        return 2;
    }
    std::string path = argv[1];
    int x = atoi(argv[2]), y = atoi(argv[3]);
    double seconds = argc > 4 ? atof(argv[4]) : 3;
    void* xtst = dlopen("libXtst.so.6", RTLD_NOW);
    auto fakeMotion = xtst ? (FakeMotion)dlsym(xtst, "XTestFakeMotionEvent") : nullptr;
    auto fakeButton = xtst ? (FakeButton)dlsym(xtst, "XTestFakeButtonEvent") : nullptr;
    Display* dpy = XOpenDisplay(nullptr);
    Display* shots = XOpenDisplay(nullptr);  // (the photographer's own connection)
    if (!fakeMotion || !dpy || !shots)
        return 2;
    std::string error;
    auto p = Plugin::load(path, "", error);
    if (!p) {
        printf("cannot load: %s\n", error.c_str());
        return 1;
    }
    p->prepare(48000, 512);
    Window ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 100, 100, 400, 300, 0, 0, 0);
    XMapWindow(dpy, ours);
    XSync(dpy, False);
    int w = 0, h = 0;
    if (!p->openEditor((uintptr_t)ours, dpy, 1.0, w, h)) {
        printf("the window didn't open\n");
        return 1;
    }
    XResizeWindow(dpy, ours, (unsigned)w, (unsigned)h);
    XRaiseWindow(dpy, ours);
    // the audio thread runs meanwhile, as in the program (some plugins redraw only while processed)
    std::atomic<bool> running{true};
    std::thread audio([&] {
        std::vector<float> l(512), r(512);
        float* out[2] = {l.data(), r.data()};
        const float* in[2] = {l.data(), r.data()};
        while (running) {
            if (p->isInstrument())
                p->render({}, out, 512);
            else
                p->process(in, nullptr, out, 512);
            std::this_thread::sleep_for(std::chrono::microseconds(10667));
        }
    });
    auto frames = [&](double s) {
        double until = now() + s;
        while (now() < until) {
            p->idle();
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
            }
            usleep(16000);
        }
    };
    frames(2.5);
    if (const char* click = getenv("CLICK")) {  // CLICK="x y": a click there first (and a second's frames)
        int cx = 0, cy = 0, crx = 0, cry = 0;
        Window c;
        if (sscanf(click, "%d %d", &cx, &cy) == 2 &&
            XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), cx, cy, &crx, &cry, &c)) {
            fakeMotion(dpy, -1, crx, cry, 0);
            fakeButton(dpy, 1, 1, 0);
            fakeButton(dpy, 1, 0, 0);
            XFlush(dpy);
            frames(1.5);
        }
    }
    if (const char* shot = getenv("SHOT")) {  // SHOT=<file.png>: a picture of the window first
        XSync(dpy, False);
        std::string cmd = "import -window " + std::to_string((unsigned long)ours) + " '" + shot + "' 2>/dev/null";
        if (system(cmd.c_str()) != 0)
            printf("(no picture taken)\n");
    }
    Window child;
    int rx = 0, ry = 0;
    XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), x, y, &rx, &ry, &child);
    fakeMotion(dpy, -1, rx, ry, 0);
    XFlush(dpy);
    frames(0.3);
    p->edited();
    int pid = bridgedHostPid(*p);
    double cpu0 = cpuSeconds(pid);
    // the photographer: the area around the point, as often as it can
    std::atomic<bool> dragging{true};
    int distinct = 0, taken = 0;
    std::thread camera([&] {
        std::vector<char> last;
        int cx = std::max(0, x - 60), cy = std::max(0, y - 60);
        while (dragging) {
            XImage* img = XGetImage(shots, ours, cx, cy, 120, 120, AllPlanes, ZPixmap);
            if (img) {
                std::vector<char> now(img->data, img->data + (size_t)img->bytes_per_line * img->height);
                if (now != last)
                    distinct++, last.swap(now);
                taken++;
                XDestroyImage(img);
            }
            usleep(2000);
        }
    });
    // the drag: down, up and down again, a pixel at a time, at 120 moves a second
    double t0 = now();
    fakeButton(dpy, 1, 1, 0);
    XFlush(dpy);
    int step = 0;
    while (now() - t0 < seconds) {
        int dy = (int)(60 * std::sin((now() - t0) * 2.0));
        fakeMotion(dpy, -1, rx, ry - dy, 0);
        XFlush(dpy);
        p->idle();
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
        }
        usleep(8333);
        step++;
    }
    fakeButton(dpy, 1, 0, 0);
    XFlush(dpy);
    double elapsed = now() - t0;
    dragging = false;
    camera.join();
    double cpu = cpuSeconds(pid) - cpu0;
    frames(0.5);
    settleBridged(*p);
    auto edited = p->edited();
    printf("%s: %.1f distinct pictures a second while dragging (%d of %d photos in %.1f s), %zu parameter changes "
           "reported, Wine host CPU %.0f%%\n",
           util::baseName(path).c_str(), distinct / elapsed, distinct, taken, elapsed, edited.size(),
           100 * cpu / elapsed);
    running = false;
    audio.join();
    p->closeEditor();
    return 0;
}
