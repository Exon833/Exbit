// Windows plugins through Wine (plugins/wine.h, plugins/bridged.cpp, src/winehost): Wine prefixes,
// telling Windows plugin files apart, the library finding them; then, where Wine, the Wine host and
// Windows plugins are installed, every Windows plugin found (in the default prefix's VST3 folder,
// or TINYDAW_WINE_PLUGINS=<folder>) loaded in its host, asked, processed, its state saved and given
// back, a second instance given the state, and (unless TINYDAW_NO_WINDOWS=1) its window opened in
// an X11 window of ours and looked at; one is crashed on purpose. By default only the first effect
// and the first instrument (a minute); TINYDAW_WINE_ALL=1 tests every one, TINYDAW_WINE_ONLY=<text>
// those whose name holds it, TINYDAW_NO_WINE=1 none; TINYDAW_WINE_SHOTS=<folder> saves pictures of
// their windows.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"
#include "plugins/wine.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

using namespace plug;

namespace {

std::string tempRoot;

double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void run(const std::string& cmd) {
    int r = system(cmd.c_str());
    (void)r;
}

// --- without Wine ---

void testPrefixes() {
    std::string fake = util::joinPath(tempRoot, "prefix");
    CHECK(!wine::isPrefix(fake));
    util::makeDirs(util::joinPath(fake, "drive_c/Program Files/Common Files/VST3/Vendor"));
    util::makeDirs(util::joinPath(fake, "drive_c/Program Files/Steinberg/VstPlugins"));
    CHECK(!wine::isPrefix(fake));  // (no registry yet)
    util::writeFile(util::joinPath(fake, "system.reg"), "WINE REGISTRY Version 2\n");
    CHECK(wine::isPrefix(fake));
    CHECK(wine::prefixOf(util::joinPath(fake, "drive_c/Program Files/Common Files/VST3/Vendor/X.vst3")) == fake);
    CHECK(wine::prefixOf("/usr/lib/vst3/X.vst3").empty());
    auto v3 = wine::vst3Folders({fake, util::joinPath(tempRoot, "nothing")});
    CHECK(v3.size() == 1 && v3[0] == util::joinPath(fake, "drive_c/Program Files/Common Files/VST3"));
    auto v2 = wine::vst2UsualFolders({fake});
    CHECK(v2.size() == 1 && v2[0] == util::joinPath(fake, "drive_c/Program Files/Steinberg/VstPlugins"));
    wine::setPrefixes({fake + "/", fake});
    CHECK(wine::prefixes().size() == 1 && wine::prefixes()[0] == fake);
    CHECK(wine::prefixFor("/somewhere/else/X.dll") == fake);  // (outside every prefix: the first chosen)
    wine::setPrefixes({});
    CHECK(wine::prefixes().empty());
    CHECK(wine::looksWindows(util::joinPath(fake, "drive_c/a/b.vst3")) && wine::looksWindows("/x/Y.DLL"));
    CHECK(!wine::looksWindows("/usr/lib/vst3/X.vst3"));
}

std::string fakePe(uint16_t machine);

// a whole prefix chosen as a VST2 folder: the scan stays inside it (Wine's links lead to "/" and to
// the home folders) and leaves Windows' own DLLs alone
void testPrefixAsVst2Folder() {
    std::string prefix = util::joinPath(tempRoot, "prefix2");
    std::string outside = util::joinPath(tempRoot, "outside");
    util::makeDirs(util::joinPath(prefix, "drive_c/Program Files/VstPlugins"));
    util::makeDirs(util::joinPath(prefix, "drive_c/windows/system32"));
    util::makeDirs(util::joinPath(prefix, "drive_c/users/me"));
    util::makeDirs(util::joinPath(prefix, "dosdevices"));
    util::makeDirs(outside);
    util::writeFile(util::joinPath(prefix, "system.reg"), "WINE REGISTRY Version 2\n");
    util::writeFile(util::joinPath(prefix, "drive_c/Program Files/VstPlugins/Plug.dll"), fakePe(0x8664));
    util::writeFile(util::joinPath(prefix, "drive_c/windows/system32/kernel32.dll"), fakePe(0x8664));
    util::writeFile(util::joinPath(outside, "Elsewhere.dll"), fakePe(0x8664));
    CHECK(symlink("/", util::joinPath(prefix, "dosdevices/z:").c_str()) == 0);
    CHECK(symlink("../drive_c", util::joinPath(prefix, "dosdevices/c:").c_str()) == 0);
    CHECK(symlink(outside.c_str(), util::joinPath(prefix, "drive_c/users/me/Desktop").c_str()) == 0);
    double t0 = secondsNow();
    PluginLibrary library({}, util::joinPath(tempRoot, "cache2.json"), {prefix});
    CHECK(secondsNow() - t0 < 5);
    std::vector<std::string> looked;
    for (const auto& member : library.cache.members())
        if (member.first != LOAD_TIMES)
            looked.push_back(member.first);
    CHECK(looked.size() == 1 && util::endsWith(looked[0], "VstPlugins/Plug.dll"));
}

// a minimal PE file: "MZ", the PE header's place at 60, "PE\0\0" and the machine
std::string fakePe(uint16_t machine) {
    std::string f(256, '\0');
    f[0] = 'M', f[1] = 'Z';
    f[60] = (char)128;
    std::memcpy(&f[128], "PE\0\0", 4);
    f[132] = (char)(machine & 0xFF), f[133] = (char)(machine >> 8);
    return f;
}

void testFiles() {
    std::string dir = util::joinPath(tempRoot, "files");
    util::makeDirs(dir);
    std::string pe64 = util::joinPath(dir, "a.dll"), pe32 = util::joinPath(dir, "b.dll"), text = util::joinPath(dir, "c.dll");
    util::writeFile(pe64, fakePe(0x8664));
    util::writeFile(pe32, fakePe(0x14c));
    util::writeFile(text, "MZ not really");
    int machine = 0;
    CHECK(wine::isWindowsBinary(pe64, &machine) && machine == 0x8664);
    CHECK(wine::isWindowsBinary(pe32, &machine) && machine == 0x14c);
    CHECK(!wine::isWindowsBinary(text));
    CHECK(wine::isWindowsPlugin(pe64) && !wine::isWindowsPlugin(dir));
    CHECK(!isVst2File(pe32));  // (32-bit: can't run in the 64-bit host)
    CHECK(!isVst2File(pe64));  // (no exports: not a VST2 plugin)
    // a Windows bundle: its x86_64-win binary is the file to load
    std::string bundle = util::joinPath(dir, "W.vst3");
    util::makeDirs(util::joinPath(bundle, "Contents/x86_64-win"));
    util::writeFile(util::joinPath(bundle, "Contents/x86_64-win/W.vst3"), fakePe(0x8664));
    CHECK(loadableFile(bundle) == util::joinPath(bundle, "Contents/x86_64-win/W.vst3"));
    PluginInfo info{loadableFile(bundle), "W", "effect", ""};
    CHECK(info.label() == "W");  // (outside a prefix and not a .dll: not marked)
    PluginInfo win{"/p/drive_c/Program Files/Common Files/VST3/W.vst3", "W", "", ""};
    CHECK(win.label() == "W  (Windows)  (type not known yet)");
}

// --- the installed Windows plugins ---

struct Stereo {
    std::vector<float> l, r;
    const float* in_[2];
    float* out_[2];
    explicit Stereo(int n, float v = 0) : l((size_t)n, v), r((size_t)n, v) {}
    float* const* out() {
        out_[0] = l.data(), out_[1] = r.data();
        return out_;
    }
    const float* const* in() {
        in_[0] = l.data(), in_[1] = r.data();
        return in_;
    }
    double peak() const {
        double p = 0;
        for (size_t i = 0; i < l.size(); i++)
            p = std::max({p, (double)std::fabs(l[i]), (double)std::fabs(r[i])});
        return p;
    }
    bool finite() const {
        for (size_t i = 0; i < l.size(); i++)
            if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
                return false;
        return true;
    }
};

// plays `seconds` through the plugin in 512-frame buffers, as the audio thread does (from a thread
// of its own): a 220 Hz tone into an effect, a chord into an instrument. Returns the output's peak.
double play(Plugin& p, double seconds, bool& finite, int& late) {
    double peak = 0;
    finite = true;
    std::thread audio([&] {
        const int N = 512;
        double sr = p.sampleRate();
        int buffers = (int)(seconds * sr / N);
        Stereo in(N), out(N);
        int64_t pos = 0;
        for (int b = 0; b < buffers; b++) {
            for (int i = 0; i < N; i++)
                in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * (float)std::sin(2 * M_PI * 220 * (double)(pos + i) / sr);
            p.setTransport(120, 4, 4, true, pos);
            if (p.isInstrument()) {
                std::vector<NoteMessage> notes;
                if (b == 0)
                    for (int pitch : {48, 55, 60, 64})
                        notes.push_back({0, true, pitch, 100, 0});
                if (b == buffers - 8)
                    for (int pitch : {48, 55, 60, 64})
                        notes.push_back({0, false, pitch, 0, 0});
                p.render(notes, out.out(), N);
            } else {
                p.process(in.in(), nullptr, out.out(), N);
            }
            if (!out.finite())
                finite = false;
            peak = std::max(peak, out.peak());
            pos += N;
            std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(1e6 * N / sr / 4)));  // (a little faster than real time)
        }
    });
    audio.join();
    late = bridgedLateBuffers(p);
    return peak;
}

// the window, opened in an X11 window of ours (as the plugin window does): Wine's window must end
// up inside ours, mapped, at its corner, drawn (not all one colour)
void testEditor(Plugin& p, const std::string& label) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        printf("   (no X display: window not tested)\n");
        return;
    }
    CHECK(p.hasEditor());
    Window ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 40, 40, 400, 300, 0, 0, 0);
    XMapWindow(dpy, ours);
    XSync(dpy, False);
    int w = 0, h = 0;
    double t0 = secondsNow();
    bool opened = p.openEditor((uintptr_t)ours, dpy, 1.0, w, h);
    double openSeconds = secondsNow() - t0;
    CHECK(opened && w > 0 && h > 0);
    if (!opened) {
        printf("   window: did NOT open\n");
        XDestroyWindow(dpy, ours);
        XCloseDisplay(dpy);
        return;
    }
    XResizeWindow(dpy, ours, (unsigned)w, (unsigned)h);
    // frames (idle() keeps it in place) until it has drawn (some plugins take a few seconds the first time)
    auto colours = [&] {
        int distinct = 0;
        if (XImage* img = XGetImage(dpy, ours, 0, 0, (unsigned)std::min(w, 400), (unsigned)std::min(h, 300), AllPlanes, ZPixmap)) {
            std::vector<unsigned long> seen;
            for (int y = 0; y < img->height; y += 7)
                for (int x = 0; x < img->width; x += 7) {
                    unsigned long px = XGetPixel(img, x, y);
                    if (std::find(seen.begin(), seen.end(), px) == seen.end() && seen.size() < 64)
                        seen.push_back(px);
                }
            distinct = (int)seen.size();
            XDestroyImage(img);
        }
        return distinct;
    };
    double until = secondsNow() + 1.0, limit = secondsNow() + 8.0;
    int distinct = 0;
    while (secondsNow() < until || (distinct <= 4 && secondsNow() < limit)) {
        p.idle();
        int rw = 0, rh = 0;
        if (p.requestedSize(rw, rh))
            XResizeWindow(dpy, ours, (unsigned)rw, (unsigned)rh), w = rw, h = rh;
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
        }
        usleep(16000);
        distinct = colours();
    }
    double drawnSeconds = secondsNow() - t0;
    if (const char* shots = getenv("TINYDAW_WINE_SHOTS")) {  // (pictures of the windows, to look at)
        std::string file = util::joinPath(shots, util::replaceAll(label, " ", "_") + ".png");
        XSync(dpy, False);
        run("import -window " + std::to_string((unsigned long)ours) + " '" + file + "' 2>/dev/null");
    }
    Window root, parent, *children = nullptr;
    unsigned count = 0;
    XQueryTree(dpy, ours, &root, &parent, &children, &count);
    bool inside = count == 1;
    Window wineWindow = inside ? children[0] : 0;
    if (children)
        XFree(children);
    CHECK(inside);
    bool mapped = false, corner = false;
    if (wineWindow) {
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, wineWindow, &a)) {
            mapped = a.map_state == IsViewable;
            corner = a.x == 0 && a.y == 0;
        }
    }
    CHECK(mapped);
    CHECK(corner);
    CHECK(distinct > 4);
    printf("   window: %dx%d, opened in %.2f s, drawn after %.2f s, inside ours %s, mapped %s, at the corner %s, %d "
           "colours seen\n", w, h, openSeconds, drawnSeconds, inside ? "yes" : "NO", mapped ? "yes" : "NO",
           corner ? "yes" : "NO", distinct);
    if (p.editorResizable()) {
        int nw = w + 100, nh = h + 60;
        XResizeWindow(dpy, ours, (unsigned)nw, (unsigned)nh);
        p.editorResized(nw, nh);
        CHECK(nw > 0 && nh > 0);
        printf("   window: resizable, asked %dx%d, took %dx%d\n", w + 100, h + 60, nw, nh);
    }
    p.closeEditor();
    CHECK(!p.editorOpen());
    XSync(dpy, False);
    XQueryTree(dpy, ours, &root, &parent, &children, &count);
    if (children)
        XFree(children);
    CHECK(count == 0);  // (Wine's window left ours before it went)
    XDestroyWindow(dpy, ours);
    XCloseDisplay(dpy);
}

void testPlugin(PluginLibrary& library, const PluginInfo& info, bool windows) {
    printf("== %s  [%s]\n", info.label().c_str(), info.path.c_str());
    fflush(stdout);
    double t0 = secondsNow();
    std::unique_ptr<Plugin> p;
    try {
        p = library.load(info, 48000);
    } catch (const std::exception& e) {
        printf("   load FAILED: %s\n", e.what());
        CHECK(false);
        return;
    }
    double loadSeconds = secondsNow() - t0;
    CHECK(p != nullptr);
    if (!p)
        return;
    printf("   loaded in %.2f s: \"%s\" by %s, %s, %s%s, latency %d\n", loadSeconds, p->name().c_str(), p->vendor().c_str(),
           p->subcategories().c_str(), p->isInstrument() ? "instrument" : "effect", p->hasSidechain() ? ", sidechain" : "",
           p->latency());
    CHECK(!p->name().empty());
    CHECK(p->isInstrument() == (info.kind == INSTRUMENT));
    CHECK(p->sampleRate() == 48000);

    // parameters
    auto params = p->parameters();
    CHECK(!params.empty());
    const ParamInfo* probe = nullptr;
    for (const auto& q : params)
        if (q.automatable && !q.readOnly && !q.hidden && !q.isBypass && !q.isProgramChange && q.stepCount == 0) {
            probe = &q;
            break;
        }
    if (probe) {
        double before = p->getParam(probe->id);
        std::string textBefore = p->paramText(probe->id, before);
        double to = before < 0.5 ? 0.75 : 0.25;
        p->setParam(probe->id, to);
        CHECK_NEAR(p->getParam(probe->id), to, 1e-9);
        settleBridged(*p);
        CHECK(std::fabs(p->getParam(probe->id) - to) < 0.02);  // (the host's value, after the plugin had it)
        std::string textAfter = p->paramText(probe->id, to);
        CHECK(!textAfter.empty());
        printf("   %zu parameters; \"%s\": %s -> %s\n", params.size(), probe->name.c_str(), textBefore.c_str(),
               textAfter.c_str());
    } else {
        printf("   %zu parameters (none continuous to try)\n", params.size());
    }
    CHECK(p->edited().empty());  // (nothing changed in its window)

    // sound
    p->prepare(48000, 512);
    bool finite = true;
    int late = 0;
    double peak = play(*p, 2.0, finite, late);
    CHECK(finite);
    if (!info.pluginName.empty() || true)
        printf("   2 s played: peak %.3f, %d late buffer%s\n", peak, late, late == 1 ? "" : "s");
    if (!p->isInstrument())
        CHECK(peak > 0.001);  // (an effect lets something through, at its first settings)
    int lateBefore = late;
    peak = play(*p, 2.0, finite, late);
    CHECK(late - lateBefore <= 2);  // (once started, it keeps up)
    if (p->isInstrument())
        printf("   instrument: a chord's peak %.3f\n", peak);

    // state: saved, given back, given to a second instance
    if (probe) {
        p->setParam(probe->id, 0.6);
        play(*p, 0.1, finite, late);  // (the processor has it with the next buffer, as with any VST3 host)
    }
    std::string state = stateOf(*p);
    CHECK(state.size() > 8);
    try {
        setState(*p, state);
        CHECK(true);
    } catch (const std::exception& e) {
        printf("   setState FAILED: %s\n", e.what());
        CHECK(false);
    }
    std::unique_ptr<Plugin> second;
    try {
        second = library.load(info, 48000);
        setState(*second, state);
        if (probe) {
            double v = second->getParam(probe->id);
            CHECK(std::fabs(v - 0.6) < 0.02);
            printf("   state: %zu bytes; a second instance given it has \"%s\" at %.3f\n", state.size(), probe->name.c_str(), v);
        } else {
            printf("   state: %zu bytes, given to a second instance\n", state.size());
        }
    } catch (const std::exception& e) {
        printf("   second instance FAILED: %s\n", e.what());
        CHECK(false);
    }
    second.reset();

    // offline, on the main thread (as rendering and freezing do): every buffer done, none late
    {
        p->reset();
        int lateBefore2 = bridgedLateBuffers(*p);
        const int N = 1024;
        Stereo in(N, 0.1f), out(N);
        bool ok = true;
        for (int b = 0; b < 20; b++) {
            if (p->isInstrument())
                p->render(b == 0 ? std::vector<NoteMessage>{{0, true, 60, 100, 0}} : std::vector<NoteMessage>{}, out.out(), N);
            else
                p->process(in.in(), nullptr, out.out(), N);
            ok = ok && out.finite();
        }
        CHECK(ok);
        CHECK(bridgedLateBuffers(*p) == lateBefore2);
    }
    // a sidechain, for those that take one
    if (p->hasSidechain() && !p->isInstrument()) {
        const int N = 512;
        Stereo in(N, 0.2f), sc(N, 0.9f), out(N);
        p->process(in.in(), sc.in(), out.out(), N);
        CHECK(out.finite());
    }

    if (windows)
        testEditor(*p, info.label());
    t0 = secondsNow();
    p.reset();
    printf("   unloaded in %.2f s\n", secondsNow() - t0);
}

// a host that crashes: the plugin falls silent (an effect lets its input through), nothing hangs,
// and asking it things gives defaults
void testCrash(PluginLibrary& library, const PluginInfo& info) {
    printf("== a crash: %s\n", info.name.c_str());
    std::unique_ptr<Plugin> p;
    try {
        p = library.load(info, 48000);
    } catch (const std::exception& e) {
        printf("   load FAILED: %s\n", e.what());
        CHECK(false);
        return;
    }
    int pid = bridgedHostPid(*p);
    CHECK(pid > 0);
    kill(pid, SIGKILL);
    usleep(300000);
    const int N = 512;
    Stereo in(N, 0.3f), out(N);
    double t0 = secondsNow();
    std::thread([&] {
        for (int i = 0; i < 4; i++)
            p->process(in.in(), nullptr, out.out(), N);
    }).join();
    CHECK(secondsNow() - t0 < 1.0);
    CHECK_NEAR(out.l[100], 0.3f, 1e-6);  // (its input, unchanged)
    t0 = secondsNow();
    p->idle();
    CHECK(p->paramText(0, 0.5).size() > 0);  // (a default text)
    std::string state = stateOf(*p);
    p->getParam(0);
    CHECK(secondsNow() - t0 < 1.0);
    p.reset();
    printf("   the plugin fell silent; nothing hung\n");
}

void testInstalled() {
    if (getenv("TINYDAW_NO_WINE")) {
        printf("(TINYDAW_NO_WINE: the installed Windows plugins aren't tested)\n");
        return;
    }
    std::string why;
    if (!wine::available(why)) {
        printf("(no Windows plugins tested: %s)\n", why.c_str());
        return;
    }
    std::vector<std::string> folders;
    if (const char* f = getenv("TINYDAW_WINE_PLUGINS"))
        folders.push_back(f);
    else
        folders = wine::vst3Folders({wine::defaultPrefix()});
    if (folders.empty()) {
        printf("(no Wine prefix with a VST3 folder: no Windows plugins tested)\n");
        return;
    }
    PluginLibrary library(folders, util::joinPath(tempRoot, "cache.json"), {});
    std::vector<PluginInfo> windows;
    for (const auto& p : library.plugins)
        if (wine::isWindowsPlugin(p.path))
            windows.push_back(p);
    printf("%zu Windows plugins found in %s\n", windows.size(), folders[0].c_str());
    CHECK(!windows.empty());
    // their kinds, from moduleinfo.json or the probe (through the host)
    auto unknown = library.unknownFiles();
    if (!unknown.empty()) {
        double t0 = secondsNow();
        KindFinder finder(library, [](const std::string& list, int outFd) {  // (as `tinydaw --probe <list>`)
            fflush(stdout);
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                dup2(outFd, 1);
                runProbe(list);
                _exit(3);
            }
            return (int)pid;
        });
        finder.start(false);
        while (finder.poll(200)) {
        }
        printf("the kinds of %zu plugin files found in %.1f s (%d found)\n", unknown.size(), secondsNow() - t0,
               finder.found());
        CHECK(finder.found() == (int)unknown.size());
    }
    windows.clear();
    for (const auto& p : library.plugins)
        if (wine::isWindowsPlugin(p.path))
            windows.push_back(p);
    for (const auto& p : windows)
        CHECK(!p.kind.empty());
    const char* only = getenv("TINYDAW_WINE_ONLY");
    bool editors = !getenv("TINYDAW_NO_WINDOWS") && getenv("DISPLAY");
    // every one with TINYDAW_WINE_ALL=1 (or TINYDAW_WINE_ONLY); else the first effect and the first
    // instrument (the whole lot takes minutes)
    bool all = getenv("TINYDAW_WINE_ALL") != nullptr;
    bool effectDone = false, instrumentDone = false;
    for (const auto& p : windows) {
        if (only && !util::containsNoCase(p.name, only))
            continue;
        if (!only && !all) {
            bool& done = p.kind == INSTRUMENT ? instrumentDone : effectDone;
            if (done)
                continue;
            done = true;
        }
        testPlugin(library, p, editors);
    }
    for (const auto& p : windows)
        if (p.kind == EFFECT && (!only || util::containsNoCase(p.name, only))) {
            testCrash(library, p);
            break;
        }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    char tmpl[] = "/tmp/tinydaw-wine-test-XXXXXX";
    tempRoot = mkdtemp(tmpl);
    testPrefixes();
    testFiles();
    testPrefixAsVst2Folder();
    testInstalled();
    run("rm -rf '" + tempRoot + "'");
    TEST_MAIN_END();
}
