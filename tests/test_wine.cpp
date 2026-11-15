// Windows plugins through Wine (plugins/wine.h, plugins/bridged.cpp, src/winehost): Wine prefixes,
// telling Windows plugin files apart, the library finding them; then, where Wine, the Wine host and
// Windows plugins are installed, every Windows plugin found (in the default prefix's VST3 folder,
// or TINYDAW_WINE_PLUGINS=<folder>) loaded in its host, asked, processed, its state saved and given
// back, a second instance given the state, and (unless TINYDAW_NO_WINDOWS=1) its window opened in
// an X11 window of ours and looked at; one is crashed on purpose. By default only the first effect
// and the first instrument (a minute); TINYDAW_WINE_ALL=1 tests every one, TINYDAW_WINE_ONLY=<text>
// those whose name holds it, TINYDAW_NO_WINE=1 none; TINYDAW_WINE_SHOTS=<folder> saves pictures of
// their windows. TINYDAW_WINE_STRESS=all|<list> runs only the bridge's stress scenarios instead
// (hosts killed and hung, many at once, odd buffers, states, rate changes, offline, windows; see
// stress() below).
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "core/util.h"
#include "platform/platform.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"
#include "plugins/wine.h"
#include "ui/wine_setup.h"
#include "model/model.h"
#include "ui/plugin_editor.h"
#include "ui/plugin_host.h"
#include "ui/toolkit/widget.h"

#define Font XFontId  // (X11's Font against Tiny DAW's)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#undef Font

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

// a fake prefix (drive_c, and a registry saying which Windows it is: "" none)
std::string fakePrefix(const std::string& dir, const std::string& arch) {
    util::makeDirs(util::joinPath(dir, "drive_c/Program Files/Common Files/VST3"));
    std::string reg = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n";
    if (!arch.empty()) reg += "#arch=" + arch + "\n";
    util::writeFile(util::joinPath(dir, "system.reg"), reg + "\n[Software] 1700000000\n");
    return dir;
}

// a fake wine program: a script answering --version with `version` (and failing otherwise)
std::string fakeWine(const std::string& file, const std::string& version) {
    util::makeDirs(util::dirName(file));
    util::writeFile(file, "#!/bin/sh\n[ \"$1\" = --version ] && echo " + version + " && exit 0\nexit 1\n");
    chmod(file.c_str(), 0755);
    return file;
}

void testPrefixArch() {
    std::string root = util::joinPath(tempRoot, "arch");
    CHECK(wine::prefixArch(fakePrefix(util::joinPath(root, "p64"), "win64")) == "win64");
    CHECK(wine::prefixArch(fakePrefix(util::joinPath(root, "p32"), "win32")) == "win32");
    CHECK(wine::prefixArch(fakePrefix(util::joinPath(root, "old"), "")) == "win64");  // (no #arch: before 32-bit-only ones)
    CHECK(wine::prefixArch(util::joinPath(root, "none")).empty());                      // (no registry yet)
    CHECK(wine::wineMajor("wine-11.22 (Staging)") == 11 && wine::wineMajor("wine-8.0") == 8);
    CHECK(wine::wineMajor("") == 0 && wine::wineMajor("nonsense") == 0);
    // a 32-bit prefix can't run the (64-bit) host, and says so
    std::string why;
    if (!wine::hostPath().empty() && !wine::winePath().empty()) {
        CHECK(!wine::availableIn(util::joinPath(root, "p32"), why) && why.find("32-bit") != std::string::npos);
        CHECK(wine::availableIn(util::joinPath(root, "p64"), why));
        if (!wine::hostPath(true).empty()) {  // (32-bit plugins: in a 32-bit prefix, and in a 64-bit one through WoW64)
            CHECK(wine::availableIn(util::joinPath(root, "p32"), why, true));
            CHECK(wine::availableIn(util::joinPath(root, "p64"), why, true));
        }
    }
}

// which wine program runs in a prefix: chosen, a Bottles bottle's runner, a Lutris game's Wine, the usual one
void testWinePrograms() {
    std::string root = util::joinPath(tempRoot, "programs");
    std::string data = util::joinPath(root, "data"), config = util::joinPath(root, "config");
    std::string oldData = getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : "";
    std::string oldConfig = getenv("XDG_CONFIG_HOME") ? getenv("XDG_CONFIG_HOME") : "";
    setenv("XDG_DATA_HOME", data.c_str(), 1);
    setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    std::string usual = wine::winePath();

    // a plain prefix: the usual Wine
    std::string plain = fakePrefix(util::joinPath(root, "plain"), "win64");
    CHECK(wine::winePathFor(plain) == usual && wine::wineSource(plain).empty());

    // a Bottles bottle (<data>/bottles/bottles/<name>, its runner in <data>/bottles/runners/<runner>)
    std::string bottle = fakePrefix(util::joinPath(data, "bottles/bottles/Music"), "win64");
    util::writeFile(util::joinPath(bottle, "bottle.yml"),
                    "Arch: win64\nName: Music\nRunner: soda-9.0-1\nParameters:\n  dxvk: true\n  Runner: not-this\n");
    std::string soda = fakeWine(util::joinPath(data, "bottles/runners/soda-9.0-1/bin/wine"), "wine-9.0");
    CHECK(wine::winePathFor(bottle) == soda);
    CHECK(wine::wineSource(bottle) == "Bottles: soda-9.0-1");
    CHECK(wine::wineVersion(soda) == "wine-9.0");
    // …whose runner is gone: the usual Wine, and why
    std::string gone = fakePrefix(util::joinPath(data, "bottles/bottles/Gone"), "win64");
    util::writeFile(util::joinPath(gone, "bottle.yml"), "Runner: 'caffe-7.20'\n");
    CHECK(wine::winePathFor(gone) == usual);
    CHECK(wine::wineSource(gone) == "Bottles: caffe-7.20 isn't installed; the usual Wine instead");
    // …a "sys-" runner: the system's Wine
    std::string sys = fakePrefix(util::joinPath(data, "bottles/bottles/Sys"), "win64");
    util::writeFile(util::joinPath(sys, "bottle.yml"), "Runner: sys-wine-11.0\n");
    if (!usual.empty()) CHECK(wine::winePathFor(sys) == usual && wine::wineSource(sys) == "Bottles: sys-wine-11.0");
    // …a runner that doesn't run here (no answer to --version): the usual Wine
    std::string broken = fakePrefix(util::joinPath(data, "bottles/bottles/Broken"), "win64");
    util::writeFile(util::joinPath(broken, "bottle.yml"), "Runner: broken-1\n");
    fakeWine(util::joinPath(data, "bottles/runners/broken-1/bin/wine"), "nonsense");
    CHECK(wine::winePathFor(broken) == usual);
    CHECK(util::startsWith(wine::wineSource(broken), "Bottles: broken-1 doesn't run outside it"));

    // a Lutris game's prefix (<config>/lutris/games/*.yml: game: prefix:, wine: version:)
    std::string game = fakePrefix(util::joinPath(root, "Games/synths"), "win64");
    std::string ge = fakeWine(util::joinPath(data, "lutris/runners/wine/wine-ge-8-26-x86_64/bin/wine"), "wine-8.26");
    util::makeDirs(util::joinPath(config, "lutris/games"));
    util::writeFile(util::joinPath(config, "lutris/games/other-1.yml"),
                    "game:\n  exe: x.exe\n  prefix: /elsewhere\nwine:\n  version: nope\n");
    util::writeFile(util::joinPath(config, "lutris/games/synths-2.yml"),
                    "game:\n  exe: drive_c/x.exe\n  prefix: \"" + game + "/\"\nsystem:\n  version: not-this\nwine:\n"
                    "  version: wine-ge-8-26-x86_64\n");
    CHECK(wine::winePathFor(game) == ge);
    CHECK(wine::wineSource(game) == "Lutris: wine-ge-8-26-x86_64");
    // …without its own version: the Wine runner's (<config>/lutris/runners/wine.yml)
    std::string game2 = fakePrefix(util::joinPath(root, "Games/fx"), "win64");
    util::writeFile(util::joinPath(config, "lutris/games/fx-3.yml"), "game:\n  prefix: " + game2 + "\n");
    util::makeDirs(util::joinPath(config, "lutris/runners"));
    util::writeFile(util::joinPath(config, "lutris/runners/wine.yml"), "wine:\n  version: wine-ge-8-26-x86_64\n");
    CHECK(wine::winePathFor(game2) == ge);

    // chosen in Options → Wine prefixes: before everything else, for that prefix only
    std::string mine = fakeWine(util::joinPath(root, "mywine/bin/wine"), "wine-10.5");
    wine::setWinePrograms({{bottle + "/", mine}, {"", mine}});
    CHECK(wine::winePathFor(bottle) == mine && wine::wineSource(bottle) == "chosen");
    std::string source;
    CHECK(wine::automaticWine(bottle, source) == soda && source == "Bottles: soda-9.0-1");  // (what Automatic would give)
    CHECK(wine::winePathFor(game) == ge);
    CHECK(wine::winePrograms().size() == 1);
    wine::setWinePrograms({});
    CHECK(wine::winePathFor(bottle) == soda);

    if (oldData.empty()) unsetenv("XDG_DATA_HOME");
    else setenv("XDG_DATA_HOME", oldData.c_str(), 1);
    if (oldConfig.empty()) unsetenv("XDG_CONFIG_HOME");
    else setenv("XDG_CONFIG_HOME", oldConfig.c_str(), 1);
}

std::string fakePe(uint16_t machine);

// Install Windows plugin…'s bookkeeping (no Wine): names of new prefixes, the VST2 folders an installer
// filled that aren't searched yet
void testSetupWithoutWine() {
    CHECK(wine_setup::validName("Plugins") && wine_setup::validName("Plugins 2"));
    CHECK(!wine_setup::validName("") && !wine_setup::validName(".hidden") && !wine_setup::validName("a/b"));
    CHECK(!wine_setup::validName(" padded") && !wine_setup::validName(".."));
    std::string p = fakePrefix(util::joinPath(tempRoot, "setup/p"), "win64");
    std::string usual = util::joinPath(p, "drive_c/Program Files/VstPlugins");
    std::string other = util::joinPath(p, "drive_c/Program Files/Steinberg/VstPlugins");
    util::makeDirs(usual);
    util::writeFile(util::joinPath(usual, "Old.dll"), fakePe(0x8664));
    auto before = wine_setup::vst2Dlls({p});
    CHECK(before.size() == 1);
    // the installer adds one beside it (in a vendor's subfolder) and one in another usual folder
    util::makeDirs(util::joinPath(usual, "Vendor"));
    util::writeFile(util::joinPath(usual, "Vendor/New.dll"), fakePe(0x8664));
    util::writeFile(util::joinPath(usual, "Vendor/readme.txt"), "not a plugin");
    util::makeDirs(other);
    util::writeFile(util::joinPath(other, "Also.dll"), fakePe(0x8664));
    auto after = wine_setup::vst2Dlls({p});
    CHECK(after.size() == 3);
    auto folders = wine_setup::newVst2Folders(before, after, {});
    CHECK(folders.size() == 2 && std::find(folders.begin(), folders.end(), usual) != folders.end() &&
          std::find(folders.begin(), folders.end(), other) != folders.end());
    folders = wine_setup::newVst2Folders(before, after, {usual});  // (one is searched already)
    CHECK(folders.size() == 1 && folders[0] == other);
    CHECK(wine_setup::newVst2Folders(after, after, {}).empty());
    std::string oldData = getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME") : "";
    setenv("XDG_DATA_HOME", "/data/here", 1);
    CHECK(wine_setup::prefixesHome() == "/data/here/wineprefixes");
    if (oldData.empty()) unsetenv("XDG_DATA_HOME");
    else setenv("XDG_DATA_HOME", oldData.c_str(), 1);
}

// New prefix… and Install Windows plugin… with Wine: a real prefix made (wineboot), its registry
// written once its wineserver ends; an "installer" (a program of the prefix's own) run and waited for;
// what they refuse
std::string testSetupWithWine() {
    std::string wine = wine::winePath(), why;
    std::string dir = util::joinPath(tempRoot, "made/Plugins");
    // refused: not a full path, not empty
    CHECK(wine::startPrefix("relative/p", wine, why) < 0 && !why.empty());
    util::makeDirs(util::joinPath(tempRoot, "full"));
    util::writeFile(util::joinPath(tempRoot, "full/x"), "x");
    CHECK(wine::startPrefix(util::joinPath(tempRoot, "full"), wine, why) < 0 && why.find("isn't empty") != std::string::npos);
    CHECK(wine::startPrefix(dir, "/nonexistent/wine", why) < 0);
    // stop(): a process given up on
    int sleeper = fork();
    if (sleeper == 0) {
        execlp("sleep", "sleep", "30", (char*)nullptr);
        _exit(1);
    }
    int code = 0;
    CHECK(!wine::finished(sleeper, code));
    double t0 = secondsNow();
    wine::stop(sleeper);
    CHECK(secondsNow() - t0 < 5);
    // a new prefix (no Mono or Gecko asked for: a test)
    setenv("WINEDLLOVERRIDES", "mscoree,mshtml=", 1);
    t0 = secondsNow();
    int pid = wine::startPrefix(dir, wine, why);
    CHECK(pid > 0);
    if (pid <= 0) {
        printf("  (no new prefix: %s)\n", why.c_str());
        unsetenv("WINEDLLOVERRIDES");
        return "";
    }
    while (!wine::finished(pid, code) && secondsNow() - t0 < 240) usleep(50000);
    CHECK(code == 0);
    int server = wine::startServerWait(dir, wine, why);
    CHECK(server > 0);
    while (server > 0 && !wine::finished(server, code) && secondsNow() - t0 < 300) usleep(50000);
    unsetenv("WINEDLLOVERRIDES");
    printf("  a new prefix made in %.1f s\n", secondsNow() - t0);
    CHECK(wine::isPrefix(dir));
    CHECK(wine::prefixArch(dir) == "win64");
    // an installer: run in the prefix, from its own folder, waited for (hostname.exe, one of Wine's)
    std::string exe = util::joinPath(dir, "drive_c/windows/system32/hostname.exe");
    CHECK(wine::isWindowsBinary(exe));
    pid = wine::startInstaller(dir, exe, why);
    CHECK(pid > 0);
    t0 = secondsNow();
    code = -2;
    while (pid > 0 && !wine::finished(pid, code) && secondsNow() - t0 < 120) usleep(50000);
    CHECK(code == 0);
    // refused: not an installer, missing, not a prefix
    std::string text = util::joinPath(tempRoot, "notes.exe");
    util::writeFile(text, "just text");
    CHECK(wine::startInstaller(dir, text, why) < 0 && why.find("isn't a Windows installer") != std::string::npos);
    CHECK(wine::startInstaller(dir, util::joinPath(tempRoot, "missing.msi"), why) < 0);
    CHECK(wine::startInstaller(util::joinPath(tempRoot, "full"), exe, why) < 0);
    // its wineserver ends by itself (nothing of ours runs there)
    server = wine::startServerWait(dir, wine, why);
    t0 = secondsNow();
    while (server > 0 && !wine::finished(server, code) && secondsNow() - t0 < 120) usleep(50000);
    return wine::isPrefix(dir) ? dir : "";
}

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
    // our window moved about: Wine's stays at our corner all the while (no flashes of it elsewhere),
    // and Wine knows where it really is on the screen (for the mouse)
    if (wineWindow) {
        int offCorner = 0, looks = 0, wrong = 0;
        const int places[3][2] = {{300, 200}, {80, 360}, {520, 120}};
        for (const auto& place : places) {
            XMoveWindow(dpy, ours, place[0], place[1]);
            XFlush(dpy);
            double moveUntil = secondsNow() + 0.6;
            while (secondsNow() < moveUntil) {
                p.idle();
                while (XPending(dpy)) {
                    XEvent e;
                    XNextEvent(dpy, &e);
                }
                Window r2;
                int wx = 0, wy = 0;
                unsigned ww, wh, wb, wd;
                if (XGetGeometry(dpy, wineWindow, &r2, &wx, &wy, &ww, &wh, &wb, &wd)) {
                    looks++;
                    if (wx != 0 || wy != 0)
                        offCorner++;
                }
                usleep(4000);
            }
            Window c;
            int rx = 0, ry = 0, l = 0, t = 0, rr = 0, b = 0;
            XTranslateCoordinates(dpy, ours, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &c);
            if (!bridgedWindowRect(p, l, t, rr, b) || std::abs(l - rx) > 2 || std::abs(t - ry) > 2) {
                wrong++;
                printf("   moved: ours at %d,%d on the screen, Wine believes %d,%d\n", rx, ry, l, t);
            }
        }
        CHECK(offCorner == 0);
        CHECK(wrong == 0);
        printf("   moved 3 times: Wine's window off our corner in %d of %d looks; Wine's idea of where it is %s\n",
               offCorner, looks, wrong ? "WRONG" : "right");
    }
    if (p.editorResizable()) {
        int nw = w + 100, nh = h + 60;
        XResizeWindow(dpy, ours, (unsigned)nw, (unsigned)nh);
        p.editorResized(nw, nh);
        CHECK(nw > 0 && nh > 0);
        printf("   window: resizable, asked %dx%d, took %dx%d\n", w + 100, h + 60, nw, nh);
        int bw = w, bh = h;  // (and back: plugins remember their window's size, FabFilter's in the registry)
        XResizeWindow(dpy, ours, (unsigned)bw, (unsigned)bh);
        p.editorResized(bw, bh);
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
    // (with a display: its window opened and closed, after which its settings are kept, without
    // being asked, with the next idle(); a save after the crash then gets them)
    bool kept = false;
    Display* dpy = getenv("TINYDAW_NO_WINDOWS") ? nullptr : XOpenDisplay(nullptr);
    if (dpy) {
        CHECK(!bridgedStateKnown(*p));
        Window ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 40, 40, 400, 300, 0, 0, 0);
        XMapWindow(dpy, ours);
        XSync(dpy, False);
        int w = 0, h = 0;
        if (p->hasEditor() && p->openEditor((uintptr_t)ours, dpy, 1.0, w, h)) {
            p->closeEditor();
            double t0 = secondsNow();
            while (!bridgedStateKnown(*p) && secondsNow() - t0 < 5) {
                p->idle();
                usleep(20000);
            }
            kept = bridgedStateKnown(*p);
            CHECK(kept && secondsNow() - t0 < 1.5);
            printf("   its settings kept %.2f s after its window closed\n", secondsNow() - t0);
        }
        XDestroyWindow(dpy, ours);
        XCloseDisplay(dpy);
    }
    int pid = bridgedHostPid(*p);
    CHECK(pid > 0);
    kill(pid, SIGKILL);
    usleep(300000);
    const int N = 512;
    Stereo in(N, 0.3f), out(N);
    double t0 = secondsNow();
    int threw = 0;
    std::thread([&] {
        for (int i = 0; i < 4; i++)
            try {
                p->process(in.in(), nullptr, out.out(), N);
            } catch (const std::exception&) {
                threw++;  // (the program knows: plugin_audio marks the slot "stopped after an error")
            }
    }).join();
    CHECK(secondsNow() - t0 < 1.0);
    CHECK(threw > 0);
    CHECK_NEAR(out.l[100], 0.3f, 1e-6);  // (its input, unchanged: filled before it threw)
    t0 = secondsNow();
    p->idle();
    CHECK(p->paramText(0, 0.5).size() > 0);  // (a default text)
    bool stateThrew = false;
    try {
        CHECK(stateOf(*p).size() > 8);  // (kept: the settings it last gave)
    } catch (const std::exception&) {
        stateThrew = true;  // (none known: a save keeps the project's last state rather than an empty one)
    }
    CHECK(stateThrew == !kept);
    p->getParam(0);
    CHECK(secondsNow() - t0 < 1.0);
    p.reset();
    printf("   the plugin fell silent; nothing hung\n");
}

// one buffer of the slot on a thread of its own (as the audio thread); true if it played
bool audioThreadBuffer(PluginSlot& slot) {
    const int N = 512;
    Stereo in(N, 0.3f), out(N);
    bool played = false;
    std::thread([&] { played = plugin_audio::processEffect(slot, in.in(), nullptr, out.out(), N, 48000, {}); }).join();
    return played;
}

// a crashed plugin reloaded (ui/plugin_host.h): its settings changed before the crash kept (asked for
// a little after the change, without waiting: bridged.cpp), a project saved meanwhile keeping them,
// back in a new host with them, automation following it, playing again; by itself (Options) at
// most AUTO_RELOADS times a minute
void testReload(const std::vector<std::string>& folders, const PluginInfo& info) {
    printf("== a crash, then a reload: %s\n", info.name.c_str());
    ui::App::get().initHeadless();
    Project project;
    audio::Engine engine(project);
    engine.sr = 48000;
    ui::Window main;
    PluginHost host(&main, project, engine);
    host.folders = folders;
    host.cachePath = util::joinPath(tempRoot, "reload-cache.json");
    std::vector<std::string> statuses;
    host.showStatus = [&](const std::string& t, int) { statuses.push_back(t); };
    double t0 = secondsNow();
    SlotPtr slot = host.load(info);
    CHECK(slot && slot->instance);
    if (!slot || !slot->instance)
        return;
    printf("   loaded in %.2f s\n", secondsNow() - t0);
    TrackPtr track = project.addTrack(Kind::Audio);
    track->effects = std::vector<SlotPtr>{slot};
    track->cables = chain({slot});
    const ParamInfo* probe = nullptr;
    auto params = slot->instance->parameters();
    for (const auto& q : params)
        if (q.automatable && !q.readOnly && !q.hidden && !q.isBypass && !q.isProgramChange && q.stepCount == 0) {
            probe = &q;
            break;
        }
    CHECK(probe != nullptr);
    if (!probe)
        return;
    auto lane = std::make_shared<Automation>();
    lane->slot = slot;
    lane->param = probe->name;
    track->automation = std::vector<AutomationPtr>{lane};
    automation::resolve(project);
    CHECK(aload(lane->targetPlugin) == aload(slot->instance) && lane->hasTarget);
    // a change; its settings are kept a little later (idle(), every frame), without being asked
    double to = slot->instance->getParam(probe->id) < 0.5 ? 0.8 : 0.2;
    t0 = secondsNow();
    slot->instance->setParam(probe->id, to);
    for (int i = 0; i < 10; i++) {  // (its processor has it with the next buffers, as when playing)
        plugin_audio::syncState(*slot, 48000);
        usleep(10000);
    }
    CHECK(!bridgedStateKnown(*slot->instance));
    while (!bridgedStateKnown(*slot->instance) && secondsNow() - t0 < 10) {
        host.idle({slot});
        usleep(20000);
    }
    double kept = secondsNow() - t0;
    CHECK(bridgedStateKnown(*slot->instance) && kept >= 1.9);
    printf("   its settings kept %.2f s after the change\n", kept);
    CHECK(audioThreadBuffer(*slot) || !slot->failed);
    // the crash
    int pid = bridgedHostPid(*slot->instance);
    CHECK(pid > 0);
    kill(pid, SIGKILL);
    usleep(300000);
    for (int i = 0; i < 4 && !slot->failed; i++)
        audioThreadBuffer(*slot);
    CHECK(slot->failed && bridgedHostEnded(*slot->instance) && host.canReload(slot));
    // saved meanwhile: the settings it last gave (never read before: its kept ones)
    CHECK(slot->lastRead.empty() && slot->state.empty());
    std::string saved = slot->currentState();
    CHECK(!saved.empty());
    if (!saved.empty()) {
        std::string component, controller;
        CHECK(unpackState(saved, component, controller));
    }
    host.idle({slot});
    CHECK(!statuses.empty() && statuses.back().find(info.name.substr(0, 4)) != std::string::npos &&
          statuses.back().find("Reload it") != std::string::npos);
    // reloaded by hand: a new host, its settings back, automation following, playing again
    t0 = secondsNow();
    std::string why;
    bool reloaded = host.reload(slot, why);
    printf("   reloaded in %.2f s%s%s\n", secondsNow() - t0, reloaded ? "" : ": FAILED ", why.c_str());
    CHECK(reloaded);
    if (!reloaded)
        return;
    CHECK(!slot->failed && slot->error.empty());
    int pid2 = bridgedHostPid(*slot->instance);
    CHECK(pid2 > 0 && pid2 != pid);
    printf("   \"%s\" was %.3f before the crash, %.3f after the reload (%zu bytes of settings kept)\n",
           probe->name.c_str(), to, slot->instance->getParam(probe->id), saved.size());
    CHECK(std::fabs(slot->instance->getParam(probe->id) - to) < 0.02);
    CHECK(!statuses.empty() && statuses.back() == "Reloaded " + slot->name + " (its Wine host had crashed)");
    CHECK(aload(lane->targetPlugin) == aload(slot->instance) && lane->hasTarget);
    {
        const int N = 512;
        Stereo in(N, 0.3f), out(N);
        CHECK(plugin_audio::processEffect(*slot, in.in(), nullptr, out.out(), N, 48000, {}));  // (main thread: waits)
        CHECK(!slot->failed);
    }
    // by itself: AUTO_RELOADS times within a minute, then it stays stopped
    host.autoReload = true;
    int automatic = 0;
    for (int i = 0; i < plugin_host::AUTO_RELOADS + 1; i++) {
        int now = bridgedHostPid(*slot->instance);
        kill(now, SIGKILL);
        usleep(300000);
        for (int k = 0; k < 4 && !slot->failed; k++)
            audioThreadBuffer(*slot);
        CHECK(slot->failed);
        host.idle({slot});
        if (!slot->failed && bridgedHostPid(*slot->instance) != now) {
            automatic++;
            CHECK(std::fabs(slot->instance->getParam(probe->id) - to) < 0.02);
        }
    }
    CHECK(automatic == plugin_host::AUTO_RELOADS && slot->failed);
    CHECK(!statuses.empty() && statuses.back().find("crashed again: it stays stopped") != std::string::npos);
    printf("   reloaded by itself %d times, then left stopped\n", automatic);
    host.autoReload = false;
    // stopped while its host lives on (late offline, or hung): that host is ended first, nothing waits
    CHECK(host.reload(slot, why) && !slot->failed);
    int alive = bridgedHostPid(*slot->instance);
    plugin_audio::fail(*slot, "it didn't finish a buffer in time");
    CHECK(host.canReload(slot) && !bridgedHostEnded(*slot->instance));
    t0 = secondsNow();
    CHECK(host.reload(slot, why) && !slot->failed);
    double took = secondsNow() - t0;
    printf("   stopped with its host alive: reloaded in %.2f s\n", took);
    CHECK(took < 15);
    CHECK(bridgedHostPid(*slot->instance) != alive);
    CHECK(!statuses.empty() && statuses.back() == "Reloaded " + slot->name + " (it had stopped: it didn't finish a buffer in time)");
    CHECK(std::fabs(slot->instance->getParam(probe->id) - to) < 0.02);
    track->automation = std::vector<AutomationPtr>{};
    project.retire(aload(slot->instance));
}

// --- stress (TINYDAW_WINE_STRESS=all, or a list of: many, kill, stop, buffers, state, rate,
// offline, notes, classes, editor, texts, reopen, focus, resize (every plugin: see stressResize); TINYDAW_WINE_STRESS_LONG=1 for longer loops). The effect: the first whose name
// holds TINYDAW_WINE_ONLY, else the first FabFilter one; the instrument likewise (Twin 3 is quick);
// big states from Serum 2 when it is there. Only the stress scenarios run then. ---

bool stressLong() { return getenv("TINYDAW_WINE_STRESS_LONG") != nullptr; }

bool stressWanted(const std::string& name) {
    const char* s = getenv("TINYDAW_WINE_STRESS");
    if (!s)
        return false;
    std::string list = std::string(",") + s + ",";
    return list == ",all," || list == ",1," || list.find("," + name + ",") != std::string::npos;
}

int countDir(const std::string& dir, const std::string& prefix) {
    int n = 0;
    for (const auto& f : util::listDir(dir))
        if (util::startsWith(f.name, prefix))
            n++;
    return n;
}

// what a Windows plugin may leave behind: open files, shared memory, socket files, processes
struct Leftovers {
    int fds = 0, shm = 0, sockets = 0;
    std::vector<int> processes;  // ours (by their socket's name) or our children, zombies too
    std::string describe() const {
        std::string s = std::to_string(fds) + " fds, " + std::to_string(shm) + " shm, " + std::to_string(sockets) +
                        " socket files, " + std::to_string(processes.size()) + " processes";
        for (int p : processes)
            s += " " + std::to_string(p);
        return s;
    }
};

std::string procFile(int pid, const char* what) {
    std::string text;
    util::readFile("/proc/" + std::to_string(pid) + "/" + what, text);
    return text;
}

// a process whose main thread has ended (its other threads may go on)
bool mainThreadGone(int pid) {
    std::string stat = procFile(pid, "stat");
    size_t name = stat.rfind(')');
    return name != std::string::npos && name + 2 < stat.size() && (stat[name + 2] == 'Z' || stat[name + 2] == 'X');
}

Leftovers leftovers() {
    Leftovers l;
    for (const auto& f : util::listDir("/proc/self/fd")) {
        (void)f;
        l.fds++;
    }
    std::string me = std::to_string((long)getpid());
    l.shm = countDir("/dev/shm", "tinydaw-wine-" + me + "-");
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    std::string sockDir = util::joinPath(runtime && *runtime ? runtime : "/tmp", "tinydaw-wine-" + std::to_string((long)getuid()));
    l.sockets = countDir(sockDir, me + "-");
    std::string mark = "tinydaw-wine-" + std::to_string((long)getuid()) + "/" + me + "-";
    for (const auto& e : util::listDir("/proc")) {
        const std::string& f = e.name;
        if (f.empty() || !std::isdigit((unsigned char)f[0]))
            continue;
        int pid = std::atoi(f.c_str());
        if (pid == getpid())
            continue;
        std::string cmd = procFile(pid, "cmdline");
        std::string stat = procFile(pid, "stat");
        size_t close = stat.rfind(')');
        int ppid = 0;
        if (close != std::string::npos)
            sscanf(stat.c_str() + close + 2, "%*c %d", &ppid);
        if (cmd.find(mark) != std::string::npos || ppid == getpid())
            l.processes.push_back(pid);
    }
    return l;
}

// after unloading: the processes end (Tiny DAW gives them 3 s, away from the main thread), and
// nothing is left over (what `before` didn't have)
bool cleanAfter(const Leftovers& before, double seconds = 6) {
    double until = secondsNow() + seconds;
    Leftovers now = leftovers();
    while (secondsNow() < until && (!now.processes.empty() || now.fds > before.fds || now.shm > before.shm ||
                                    now.sockets > before.sockets)) {
        usleep(100000);
        now = leftovers();
    }
    bool clean = now.processes.empty() && now.fds <= before.fds && now.shm <= before.shm && now.sockets <= before.sockets;
    if (!clean) {
        printf("   LEFT OVER: before %s; now %s\n", before.describe().c_str(), now.describe().c_str());
        for (int p : now.processes)
            printf("     %d: %s (%s)\n", p, util::replaceAll(procFile(p, "cmdline"), std::string(1, '\0'), " ").c_str(),
                   util::trim(procFile(p, "stat")).substr(0, 60).c_str());
        for (int p : now.processes)
            kill(p, SIGKILL);  // (not left running: they are ours)
    }
    return clean;
}

std::unique_ptr<Plugin> loadOrFail(PluginLibrary& library, const PluginInfo& info) {
    try {
        auto p = library.load(info, 48000);
        CHECK(p != nullptr);
        return p;
    } catch (const std::exception& e) {
        printf("   load FAILED: %s\n", e.what());
        CHECK(false);
        return nullptr;
    }
}

void tone(Stereo& s, int64_t pos, double sr) {
    for (size_t i = 0; i < s.l.size(); i++)
        s.l[i] = s.r[i] = 0.25f * (float)std::sin(2 * M_PI * 220 * (double)(pos + (int64_t)i) / sr);
}

// one buffer, through an effect or an instrument (a chord held from frame 0 of the first)
void oneBuffer(Plugin& p, Stereo& in, Stereo& out, int64_t pos, int frames) {
    p.setTransport(120, 4, 4, true, pos);
    if (p.isInstrument()) {
        std::vector<NoteMessage> notes;
        if (pos == 0)
            for (int pitch : {48, 55, 60, 64})
                notes.push_back({0, true, pitch, 100, 0});
        p.render(notes, out.out(), frames);
    } else {
        p.process(in.in(), nullptr, out.out(), frames);
    }
}

// an audio thread of ours: buffers of N, real-time paced, until `stop`; the longest process()
struct AudioLoop {
    Plugin& p;
    int N;
    std::atomic<bool> stop{false};
    std::atomic<int> buffers{0}, threw{0};
    std::atomic<double> longest{0}, peak{0};
    std::atomic<bool> finite{true}, inputBack{false};
    std::string error;
    std::function<void(int)> each;  // (the audio thread, before buffer b)
    std::thread t;
    AudioLoop(Plugin& plugin, int n) : p(plugin), N(n) {}
    void start() {
        t = std::thread([this] {
            Stereo in(N), out(N);
            int64_t pos = 0;
            double sr = p.sampleRate();
            for (int b = 0; !stop; b++) {
                if (each)
                    each(b);
                tone(in, pos, sr);
                double t0 = secondsNow();
                try {
                    oneBuffer(p, in, out, pos, N);
                } catch (const std::exception& e) {
                    if (!threw++)
                        error = e.what();
                }
                longest = std::max(longest.load(), secondsNow() - t0);
                if (!out.finite())
                    finite = false;
                peak = std::max(peak.load(), out.peak());
                inputBack = !p.isInstrument() && std::memcmp(out.l.data(), in.l.data(), sizeof(float) * (size_t)N) == 0;
                buffers++;
                pos += N;
                std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(1e6 * N / sr)));
            }
        });
    }
    void finish() {
        stop = true;
        if (t.joinable())
            t.join();
    }
    ~AudioLoop() { finish(); }
};

// many instances at once (8 hosts), all processed by one audio thread, as the engine does; then
// unloaded, and loaded and unloaded in a loop: nothing left over
void stressMany(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: many instances of %s\n", info.name.c_str());
    Leftovers before = leftovers();
    const int COUNT = 8;
    std::vector<std::unique_ptr<Plugin>> ps;
    double t0 = secondsNow();
    for (int i = 0; i < COUNT; i++)
        if (auto p = loadOrFail(library, info))
            ps.push_back(std::move(p));
    printf("   %zu loaded in %.2f s\n", ps.size(), secondsNow() - t0);
    CHECK((int)ps.size() == COUNT);
    for (auto& p : ps)
        p->prepare(48000, 512);
    std::vector<int> pids;
    for (auto& p : ps)
        pids.push_back(bridgedHostPid(*p));
    std::sort(pids.begin(), pids.end());
    CHECK(std::unique(pids.begin(), pids.end()) == pids.end() && pids.front() > 0);  // (a host each)
    const int N = 512;
    std::vector<double> peaks(ps.size(), 0);
    double longest = 0;
    bool finite = true;
    std::thread([&] {
        Stereo in(N), out(N);
        for (int b = 0; b < 48000 * 2 / N; b++) {
            double s0 = secondsNow();
            for (size_t i = 0; i < ps.size(); i++) {
                tone(in, (int64_t)b * N, 48000);
                oneBuffer(*ps[i], in, out, (int64_t)b * N, N);
                finite = finite && out.finite();
                peaks[i] = std::max(peaks[i], out.peak());
            }
            longest = std::max(longest, secondsNow() - s0);
            std::this_thread::sleep_for(std::chrono::microseconds(1000000 * N / 48000 / 2));
        }
    }).join();
    int late = 0;
    for (auto& p : ps)
        late += bridgedLateBuffers(*p);
    double lowest = *std::min_element(peaks.begin(), peaks.end());
    printf("   2 s through all %d: lowest peak %.3f, longest round of all %.1f ms, %d late buffers\n", COUNT, lowest,
           1000 * longest, late);
    CHECK(finite);
    CHECK(info.kind == INSTRUMENT || lowest > 0.001);
    CHECK(late <= COUNT);  // (a buffer or so each, at the start)
    t0 = secondsNow();
    ps.clear();
    double unload = secondsNow() - t0;
    printf("   all unloaded in %.2f s\n", unload);
    CHECK(unload < 2);
    CHECK(cleanAfter(before));
    int loops = stressLong() ? 20 : 5;
    t0 = secondsNow();
    for (int i = 0; i < loops; i++) {
        auto p = loadOrFail(library, info);
        if (!p)
            break;
        p->prepare(48000, 512);
        Stereo in(N, 0.1f), out(N);
        std::thread([&] { oneBuffer(*p, in, out, 0, N); }).join();
        if (i % 2)
            stateOf(*p);
    }
    printf("   loaded and unloaded %d times in %.1f s\n", loops, secondsNow() - t0);
    CHECK(cleanAfter(before));
}

// a host killed (SIGKILL) mid-buffer and mid-request: Tiny DAW's side returns at once, doesn't
// crash, knows (process() throws, as for any plugin that fails: the slot shows it stopped), and
// nothing is left over
void stressKill(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s killed mid-buffer and mid-request\n", info.name.c_str());
    Leftovers before = leftovers();
    {  // mid-buffer, with an audio thread going
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 512);
        AudioLoop audio(*p, 512);
        audio.start();
        usleep(500000);
        int pid = bridgedHostPid(*p);
        double longestBefore = audio.longest;
        kill(pid, SIGKILL);
        int at = audio.buffers;
        while (audio.buffers < at + 40)
            usleep(5000);
        audio.finish();
        printf("   killed mid-buffer: longest buffer %.1f ms (before the kill %.1f ms); after it, %s; process() threw %d "
               "times%s%s\n", 1000 * audio.longest, 1000 * longestBefore,
               audio.inputBack ? "the input comes back unchanged" : "NOT the input", audio.threw.load(),
               audio.error.empty() ? "" : ": ", audio.error.c_str());
        CHECK(audio.longest < 0.25);
        CHECK(audio.finite);
        CHECK(audio.threw > 0);  // (the program knows it stopped: plugin_audio marks the slot failed)
        double t0 = secondsNow();
        p->idle();
        p->paramText(0, 0.5);
        bool stateThrew = false;
        try {
            stateOf(*p);
        } catch (const std::exception&) {
            stateThrew = true;
        }
        CHECK(stateThrew);  // (its state isn't known: a save keeps the project's last one)
        try {
            setState(*p, packState("x", ""));
        } catch (const std::exception&) {
        }
        p->parameters();
        p->prepare(44100, 512);
        p->reset();
        CHECK(!p->hasEditor() || true);
        double asked = secondsNow() - t0;
        printf("   asked things of it afterwards: %.2f s\n", asked);
        CHECK(asked < 1);
        t0 = secondsNow();
        p.reset();
        CHECK(secondsNow() - t0 < 1);
    }
    CHECK(cleanAfter(before));
    // mid-request: while its state is being saved and given (a big one, being sent), and while
    // asked for its parameters; the killer waits a little longer each time
    const char* what[] = {"saving its state", "being given a big state", "listing its parameters", "preparing"};
    for (int k = 0; k < 4; k++) {
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        int pid = bridgedHostPid(*p);
        std::string state = stateOf(*p);
        std::string big = packState(std::string(64u << 20, 'x'), "");
        std::atomic<bool> killed{false};
        double killedAt = 0;
        std::thread killer([&] {
            usleep(k == 1 ? 150000 : 20000);
            killedAt = secondsNow();
            kill(pid, SIGKILL);
            killed = true;
        });
        double longestAfter = 0;
        int calls = 0;
        bool threw = false;
        double t0 = secondsNow();
        while (secondsNow() - t0 < 1.5) {
            double c0 = secondsNow();
            try {
                if (k == 0)
                    stateOf(*p);
                else if (k == 1)
                    setState(*p, big);
                else if (k == 2) {
                    p->parameters();
                    setState(*p, state);  // (parameters() asks again after this)
                } else
                    p->prepare(calls % 2 ? 44100 : 48000, 512);
            } catch (const std::exception&) {
                threw = true;
            }
            calls++;
            if (killed)
                longestAfter = std::max(longestAfter, secondsNow() - std::max(c0, killedAt));
        }
        killer.join();
        printf("   killed while %s: %d calls, the longest after the kill %.2f s%s\n", what[k], calls, longestAfter,
               threw ? ", setState threw" : "");
        CHECK(longestAfter < 1);
        double u0 = secondsNow();
        p.reset();
        CHECK(secondsNow() - u0 < 1);
    }
    CHECK(cleanAfter(before));
}

// a host that hangs (SIGSTOP): how long the audio thread and the main thread are held. The audio
// thread: a few buffers' time once, then not at all; the main thread: a message the host takes
// nothing of waits SEND_SECONDS (2 s) once, then the host counts as hung and nothing waits for it;
// a request, its time limit once. Watchdog threads let it go on (SIGCONT) or kill it after a
// while, so nothing here can hang for good.
void stressStop(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s hangs (SIGSTOP)\n", info.name.c_str());
    Leftovers before = leftovers();
    auto timed = [](const char* what, const std::function<void()>& f) {
        double t0 = secondsNow();
        try {
            f();
        } catch (const std::exception& e) {
        }
        double s = secondsNow() - t0;
        printf("   main thread: %s held it %.2f s\n", what, s);
        return s;
    };
    // a watchdog: `action` on the host after `seconds` unless `ok` is set first
    struct Watchdog {
        std::atomic<bool> ok{false}, fired{false};
        std::thread t;
        Watchdog(int pid, int sig, double seconds) {
            t = std::thread([this, pid, sig, seconds] {
                double until = secondsNow() + seconds;
                while (!ok && secondsNow() < until)
                    usleep(10000);
                if (!ok) {
                    fired = true;
                    kill(pid, sig);
                }
            });
        }
        bool done() {
            ok = true;
            if (t.joinable())
                t.join();
            return fired;
        }
        ~Watchdog() { done(); }
    };
    {  // the audio thread, idle(), a flood of parameter changes (a knob dragged: messages the
       // stopped host doesn't read, the socket fills), then requests; then unloaded, hung
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 512);
        auto params = p->parameters();
        uint32_t id = params.empty() ? 0 : params[0].id;
        int pid = bridgedHostPid(*p);
        AudioLoop audio(*p, 512);
        audio.start();
        usleep(300000);
        kill(pid, SIGSTOP);
        usleep(300000);
        printf("   audio thread: longest buffer %.1f ms while it hangs\n", 1000 * audio.longest);
        CHECK(audio.longest < 0.06);  // (4 buffers' time at most)
        CHECK(timed("idle()", [&] { p->idle(); }) < 0.1);
        CHECK(timed("idle() again", [&] { p->idle(); }) < 0.1);
        CHECK(timed("getParam()", [&] { p->getParam(id); }) < 0.1);
        bool resumed;
        double longest = 0, spent;
        int waited = -1;
        {
            Watchdog dog(pid, SIGCONT, 8);
            double s0 = secondsNow();
            for (int i = 0; i < 100000 && !dog.fired; i++) {
                double c0 = secondsNow();
                p->windowEdit(id, (i % 100) / 100.0);  // (one message each: setParam()'s are gathered into one)
                double c = secondsNow() - c0;
                if (c > longest)
                    longest = c, waited = i;
            }
            spent = secondsNow() - s0;
            resumed = dog.done();
        }
        printf("   main thread: 100000 window edits first: %s in %.2f s; the longest, number %d, %.2f s\n",
               resumed ? "BLOCKED until the host was let go on" : "returned", spent, waited, longest);
        CHECK(!resumed);
        CHECK(longest < 2.5);
        CHECK(spent < 4);
        if (resumed)
            kill(pid, SIGSTOP);
        CHECK(timed("paramText()", [&] { p->paramText(id, 0.5); }) < 2.5);
        CHECK(timed("paramText() again", [&] { p->paramText(id, 0.5); }) < 0.5);  // (known hung: not waited for)
        CHECK(timed("parameters()+setState()", [&] {
                  setState(*p, packState("x", ""));
              }) < 2.5);
        double u0 = secondsNow();
        {
            Watchdog dog(pid, SIGKILL, 8);
            audio.finish();
            p.reset();
            dog.done();
        }
        double unload = secondsNow() - u0;
        printf("   unloaded, hung, in %.2f s\n", unload);
        CHECK(unload < 1);
    }
    CHECK(cleanAfter(before));
    {  // a big state (64 MB) given to a hung host: cut short, the host can't read the rest: it is
       // killed; setState returns, and later buffers say it ended
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 512);
        int pid = bridgedHostPid(*p);
        kill(pid, SIGSTOP);
        bool threw = false;
        double t0 = secondsNow();
        {
            Watchdog dog(pid, SIGKILL, 10);
            try {
                setState(*p, packState(std::string(64u << 20, 'x'), ""));
            } catch (const std::exception&) {
                threw = true;
            }
            dog.done();
        }
        double given = secondsNow() - t0;
        usleep(500000);
        int processThrew = 0;
        std::thread([&] {
            Stereo in(512, 0.3f), out(512);
            for (int i = 0; i < 4; i++)
                try {
                    p->process(in.in(), nullptr, out.out(), 512);
                } catch (const std::exception&) {
                    processThrew++;
                }
        }).join();
        bool gone = kill(pid, 0) != 0;
        printf("   a 64 MB state given to it, hung: setState %s in %.2f s; the host %s; process() threw %d of 4\n",
               threw ? "threw" : "returned", given, gone ? "is gone" : "is STILL THERE", processThrew);
        CHECK(given < 3);
        CHECK(threw);
        CHECK(gone);
        CHECK(processThrew > 0);
        p.reset();
    }
    CHECK(cleanAfter(before));
    {  // hung for a while, then going on: what was changed meanwhile reaches it (the last value),
       // and it answers again
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 512);
        const ParamInfo* probe = nullptr;
        auto params = p->parameters();
        for (const auto& q : params)
            if (q.automatable && !q.readOnly && !q.hidden && !q.isBypass && !q.isProgramChange && q.stepCount == 0) {
                probe = &q;
                break;
            }
        CHECK(probe != nullptr);
        if (!probe)
            return;
        uint32_t id = probe->id;
        int pid = bridgedHostPid(*p);
        std::string goodState = stateOf(*p);
        kill(pid, SIGSTOP);
        double hungText = timed("paramText() (hung)", [&] { p->paramText(id, 0.5); });
        CHECK(hungText < 2.5);
        {  // let go on soon after: a state save, though it counts as hung, still asks (and gets it)
            std::thread later([pid] {
                usleep(300000);
                kill(pid, SIGCONT);
            });
            std::string got;
            try {
                got = stateOf(*p);
            } catch (const std::exception& e) {
                printf("   stateOf() after it went on: threw %s\n", e.what());
            }
            later.join();
            printf("   stateOf() of a hung host let go on 0.3 s later: %s\n",
                   got == goodState ? "its real state" : got.empty() ? "NONE" : "DIFFERENT");
            CHECK(got == goodState);
            kill(pid, SIGSTOP);
            usleep(100000);
        }
        for (double v : {0.2, 0.4, 0.7})
            p->setParam(id, v);
        kill(pid, SIGCONT);
        double until = secondsNow() + 1;
        while (secondsNow() < until) {
            p->idle();
            usleep(16000);
        }
        settleBridged(*p);
        bool finite = true;
        int late = 0;
        play(*p, 0.2, finite, late);  // (the processor has it with the next buffers)
        std::string text;
        double again = timed("paramText() after it went on", [&] { text = p->paramText(id, 0.5); });
        CHECK(again < 0.5);
        CHECK(!text.empty() && text != util::fixed(0.5, 2));  // (the plugin's text, not the fallback)
        auto second = loadOrFail(library, info);
        if (second) {
            setState(*second, stateOf(*p));
            double v = second->getParam(id);
            printf("   set to 0.2, 0.4, 0.7 while it hung; after it went on, its state has \"%s\" at %.3f; its text "
                   "\"%s\"\n", probe->name.c_str(), v, text.c_str());
            CHECK(std::fabs(v - 0.7) < 0.02);
        }
    }
    CHECK(cleanAfter(before));
}

// very long and odd buffers: 1, 3, 5000, 8192, 16384 frames (more than the shared memory holds:
// done in parts), the audio thread and the main thread; finite, sounding, none late
void stressBuffers(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s with odd and long buffers\n", info.name.c_str());
    Leftovers before = leftovers();
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    p->prepare(48000, 16384);
    for (int N : {1, 3, 5000, 8192, 16384, 0}) {
        int count = N == 0 ? 10 : std::max(4, (int)(48000 * 0.5 / std::max(N, 1)));
        if (N == 1)
            count = 4800;
        bool finite = true;
        double peak = 0, longest = 0;
        int lateBefore = bridgedLateBuffers(*p);
        std::thread([&] {
            Stereo in(std::max(N, 1)), out(std::max(N, 1));
            int64_t pos = 0;
            for (int b = 0; b < count; b++) {
                tone(in, pos, 48000);
                double t0 = secondsNow();
                oneBuffer(*p, in, out, pos, N);
                longest = std::max(longest, secondsNow() - t0);
                finite = finite && out.finite();
                if (N > 0)
                    peak = std::max(peak, out.peak());
                pos += N;
            }
        }).join();
        int late = bridgedLateBuffers(*p) - lateBefore;
        printf("   %5d frames x %d (audio thread): peak %.3f, longest %.2f ms, %d late\n", N, count, peak, 1000 * longest, late);
        CHECK(finite);
        CHECK(late <= 1);
        if (N > 0 && !p->isInstrument())
            CHECK(peak > 0.001);
    }
    // the main thread, offline, 16384 and 1
    for (int N : {16384, 1}) {
        Stereo in(N, 0.1f), out(N);
        int lateBefore = bridgedLateBuffers(*p);
        bool finite = true;
        for (int b = 0; b < (N == 1 ? 2000 : 8); b++) {
            tone(in, (int64_t)b * N, 48000);
            oneBuffer(*p, in, out, (int64_t)b * N, N);
            finite = finite && out.finite();
        }
        printf("   %5d frames on the main thread: %d late\n", N, bridgedLateBuffers(*p) - lateBefore);
        CHECK(finite);
        CHECK(bridgedLateBuffers(*p) == lateBefore);
    }
    // a long buffer in parts is the same as in short ones (a fresh start each, same input)
    if (!p->isInstrument()) {
        const int N = 16384;
        Stereo in(N), whole(N), parts(N);
        tone(in, 0, 48000);
        p->reset();
        p->process(in.in(), nullptr, whole.out(), N);
        p->reset();
        for (int at = 0; at < N; at += 512) {
            const float* i2[2] = {in.l.data() + at, in.r.data() + at};
            float* o2[2] = {parts.l.data() + at, parts.r.data() + at};
            p->process(i2, nullptr, o2, 512);
        }
        double diff = 0;
        for (int i = 0; i < N; i++)
            diff = std::max(diff, (double)std::fabs(whole.l[(size_t)i] - parts.l[(size_t)i]));
        printf("   16384 frames at once vs in 512s: largest difference %.6f\n", diff);
        CHECK(diff < 1e-3);
    }
    p.reset();
    CHECK(cleanAfter(before));
}

// state save/restore: round trips equal; a big state (Serum 2's); nonsense states refused, the
// host still there afterwards
void stressState(PluginLibrary& library, const PluginInfo& info, const PluginInfo* big) {
    printf("== stress: %s's state\n", info.name.c_str());
    Leftovers before = leftovers();
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    std::string s1 = stateOf(*p);
    setState(*p, s1);
    std::string s2 = stateOf(*p);
    printf("   %zu bytes; given back and saved again: %s\n", s1.size(), s1 == s2 ? "the same" : "DIFFERENT");
    CHECK(s1 == s2);
    auto second = loadOrFail(library, info);
    if (second) {
        setState(*second, s1);
        CHECK(stateOf(*second) == s1);
        second.reset();
    }
    // nonsense: empty, cut short, garbage of 64 MB; each refused (or taken), the host alive
    std::string component, controller;
    unpackState(s1, component, controller);
    const std::pair<const char*, std::string> bad[] = {
        {"empty", packState("", "")},
        {"cut short", packState(component.substr(0, component.size() / 2), "")},
        {"64 MB of garbage", packState(std::string(64u << 20, '\x5a'), std::string(1000, '\x01'))},
    };
    for (const auto& [name, s] : bad) {
        double t0 = secondsNow();
        std::string how = "taken";
        try {
            setState(*p, s);
        } catch (const std::exception& e) {
            how = std::string("refused: ") + e.what();
        }
        printf("   %s: %s (%.2f s)\n", name, how.c_str(), secondsNow() - t0);
        int pid = bridgedHostPid(*p);
        CHECK(pid > 0 && kill(pid, 0) == 0);
        setState(*p, s1);  // (and it still takes a good one)
        CHECK(stateOf(*p) == s1);
    }
    p.reset();
    if (big) {
        auto b = loadOrFail(library, *big);
        if (b) {
            double t0 = secondsNow();
            std::string bs = stateOf(*b);
            double saved = secondsNow() - t0;
            t0 = secondsNow();
            setState(*b, bs);
            double given = secondsNow() - t0;
            std::string bs2 = stateOf(*b);
            printf("   %s: %zu bytes, saved in %.2f s, given back in %.2f s, saved again %s\n", big->name.c_str(), bs.size(),
                   saved, given, bs == bs2 ? "the same" : "DIFFERENT");
            CHECK(bs.size() > 1000);
            CHECK(bs == bs2);
            auto b2 = loadOrFail(library, *big);
            if (b2) {
                setState(*b2, bs);
                std::string bs3 = stateOf(*b2);
                printf("   a second %s given it saves %s\n", big->name.c_str(), bs3 == bs ? "the same" : "DIFFERENT");
                CHECK(bs3 == bs);
            }
        }
    }
    CHECK(cleanAfter(before));
}

// sample-rate changes while it plays: as plugin_audio::ensureRate does, on the audio thread (the
// engine's rate changed), while the main thread goes on with the GUI's calls (idle, getParam,
// paramText); and then on the main thread between buffers
void stressRate(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s's sample rate changed while it plays\n", info.name.c_str());
    Leftovers before = leftovers();
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    p->prepare(48000, 512);
    auto params = p->parameters();
    std::atomic<double> prepareSeconds{0};
    std::atomic<int> rates{0}, silentAfter{0}, worstSilent{0}, threw{0};
    std::atomic<bool> finite{true}, stop{false}, soundedBefore{false};
    std::atomic<int> firstSound{-1};
    int lateAtStart = bridgedLateBuffers(*p);
    std::atomic<int> latencyLow{1 << 30}, latencyHigh{-1};
    std::string error;
    // the audio thread: 512-frame buffers; every 25th a new rate (as ensureRate); after each, how
    // many buffers stay silent (an instrument is given its chord again, as a prepare starts afresh)
    std::thread audio([&] {
        const int N = 512;
        Stereo in(N), out(N);
        int64_t pos = 0;
        int silentRun = -1;  // (-1: sounding since the last change)
        for (int b = 0; !stop; b++) {
            bool changed = false;
            if (b > 0 && b % 25 == 0) {
                double t0 = secondsNow();
                int sr = (b / 25) % 3 == 0 ? 48000 : (b / 25) % 3 == 1 ? 44100 : 96000;
                try {  // (an instrument's chord still held: its note-offs come with the next buffer, F15)
                    if (p->sampleRate() != sr)
                        p->prepare(sr);
                } catch (const std::exception& e) {
                    if (!threw++)
                        error = e.what();
                }
                prepareSeconds = std::max(prepareSeconds.load(), secondsNow() - t0);
                rates++;
                changed = true;
                silentRun = 0;
            }
            double sr = p->sampleRate();
            tone(in, pos, sr);
            p->setTransport(120, 4, 4, true, pos);
            try {
                if (p->isInstrument()) {
                    std::vector<NoteMessage> notes;
                    if (b == 0 || changed)
                        for (int pitch : {48, 55, 60, 64})
                            notes.push_back({0, true, pitch, 100, 0});
                    p->render(notes, out.out(), N);
                } else {
                    p->process(in.in(), nullptr, out.out(), N);
                }
            } catch (const std::exception& e) {
                if (!threw++)
                    error = e.what();
            }
            if (!out.finite())
                finite = false;
            if (rates == 0 && out.peak() > 0.001 && !soundedBefore) {
                soundedBefore = true;
                firstSound = b;  // (buffers from the first chord to its first sound: the plugin's own start)
            }
            if (silentRun >= 0 && soundedBefore) {
                if (out.peak() > 0.001) {
                    worstSilent = std::max(worstSilent.load(), silentRun);
                    silentRun = -1;
                } else if (++silentRun > 20) {
                    silentAfter++;  // (still silent 20 buffers after the change)
                    silentRun = -1;
                }
            }
            int l = p->latency();
            latencyLow = std::min(latencyLow.load(), l);
            latencyHigh = std::max(latencyHigh.load(), l);
            pos += N;
            std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(1e6 * N / sr)));
        }
    });
    double t0 = secondsNow();
    int gui = 0;
    while (secondsNow() - t0 < 3) {  // (the GUI, meanwhile)
        p->idle();
        for (const auto& q : params)
            p->getParam(q.id);
        if (!params.empty())
            p->paramText(params[(size_t)gui % params.size()].id, 0.5);
        gui++;
        usleep(16000);
    }
    stop = true;
    audio.join();
    int lateChanging = bridgedLateBuffers(*p) - lateAtStart;
    printf("   %d rate changes on the audio thread, the longest held it %.1f ms; silent afterwards for at most %d buffers, "
           "%d changes after which it stayed silent%s (from the start: %d); %d late buffers (the host preparing); latency "
           "%d..%d; %d GUI frames%s%s\n", rates.load(), 1000 * prepareSeconds, worstSilent.load(), silentAfter.load(),
           soundedBefore ? "" : " (SILENT before the first change too: not judged)", firstSound.load(), lateChanging,
           latencyLow.load(),
           latencyHigh.load(), gui,
           threw ? "; THREW: " : "", error.c_str());
    CHECK(finite);
    CHECK(threw == 0);
    CHECK(rates >= 8);
    CHECK(prepareSeconds < 0.05);  // (the audio thread must not wait on the host's main thread for long)
    CHECK(silentAfter == 0);
    // (a fresh start takes what the first one took, plus the buffers the host was late for while the
    // plugin prepared on its audio thread: Serum 2 takes ~150 ms at a new rate, silent meanwhile, the
    // rest of the program playing on, where in-process it would hold the whole audio thread)
    CHECK(worstSilent <= std::max(0, firstSound.load()) + 8 + lateChanging);
    CHECK(latencyLow >= 0 && latencyHigh < 96000);
    // the rate changed while the host is hung (or just busy on its main thread: drawing its window):
    // the audio thread (plugin_audio::ensureRate) must not wait for it; a watchdog lets it go on after 5 s
    {
        p->prepare(48000, 512);
        int pid = bridgedHostPid(*p);
        kill(pid, SIGSTOP);
        std::atomic<bool> done{false};
        std::thread watchdog([&] {
            double until = secondsNow() + 5;
            while (!done && secondsNow() < until)
                usleep(10000);
            kill(pid, SIGCONT);
        });
        double held = 0;
        std::thread([&] {
            Stereo in(512), out(512);
            double t0 = secondsNow();
            if (p->sampleRate() != 44100)
                p->prepare(44100);  // (as ensureRate does)
            oneBuffer(*p, in, out, 0, 512);
            held = secondsNow() - t0;
        }).join();
        done = true;
        watchdog.join();
        printf("   the rate changed on the audio thread while the host hangs: the audio thread held %.2f s%s\n", held,
               held > 4.9 ? " (until the host was let go on)" : "");
        CHECK(held < 0.1);
    }
    // and on the main thread, between buffers: it plays at the new rate, its chord played again (an
    // instrument's notes still held in the first three rounds, let go of first in the last two: F15, a
    // note held across prepare() left FabFilter One silent for that key until it got the note-off)
    int round = 0;
    for (int sr : {44100, 96000, 22050, 192000, 48000}) {
        if (p->isInstrument() && round++ >= 3) {
            p->notesOff();
            Stereo out(512);
            std::thread([&] { p->render({}, out.out(), 512); }).join();
            printf("   (notes let go of first)\n");
        }
        p->prepare(sr, 512);
        CHECK(p->sampleRate() == sr);
        Stereo in(512), out(512);
        bool finite = true;
        double peak = 0;
        int firstSound = -1;
        std::thread([&] {
            for (int b = 0; b < 40; b++) {
                tone(in, (int64_t)b * 512, sr);
                oneBuffer(*p, in, out, (int64_t)b * 512, 512);
                finite = finite && out.finite();
                peak = std::max(peak, out.peak());
                if (firstSound < 0 && out.peak() > 0.001)
                    firstSound = b;
            }
        }).join();
        printf("   at %d Hz: peak %.3f, sounding from buffer %d\n", sr, peak, firstSound);
        CHECK(finite);
        CHECK(peak > 0.001);
    }
    p.reset();
    CHECK(cleanAfter(before));
}

// offline rendering on the main thread (RENDER: an instrument): every buffer done, none late; the
// host killed in the middle: the rest returns at once; the host hung: how long the main thread is held
void stressOffline(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s rendered offline on the main thread\n", info.name.c_str());
    Leftovers before = leftovers();
    for (int round = 0; round < 3; round++) {
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 1024);
        int pid = bridgedHostPid(*p);
        const int N = 1024, buffers = 48000 * 10 / N;
        Stereo in(N), out(N);
        double peak = 0, longest = 0, t0 = secondsNow();
        bool finite = true;
        int lateBefore = bridgedLateBuffers(*p);
        std::atomic<bool> resumed{false};
        std::thread helper;
        int threw = 0;
        for (int b = 0; b < buffers; b++) {
            if (b == buffers / 2 && round == 1)
                kill(pid, SIGKILL);
            if (b == buffers / 2 && round == 2) {
                kill(pid, SIGSTOP);
                helper = std::thread([&] {
                    usleep(3000000);
                    kill(pid, SIGCONT);
                    resumed = true;
                });
            }
            double b0 = secondsNow();
            tone(in, (int64_t)b * N, 48000);
            try {
                oneBuffer(*p, in, out, (int64_t)b * N, N);
            } catch (const std::exception&) {
                threw++;  // (the host killed: the program is told)
            }
            longest = std::max(longest, secondsNow() - b0);
            finite = finite && out.finite();
            if (b < buffers / 2 || round == 0)
                peak = std::max(peak, out.peak());
        }
        if (helper.joinable())
            helper.join();
        double spent = secondsNow() - t0;
        int late = bridgedLateBuffers(*p) - lateBefore;
        const char* what[] = {"10 s rendered", "10 s rendered, the host killed half way", "10 s rendered, the host hung for 3 s half way"};
        printf("   %s: in %.2f s, peak %.3f, longest buffer %.2f s, %d late, %d buffers threw\n", what[round], spent, peak,
               longest, late, threw);
        CHECK(round == 1 ? threw > 0 : threw == 0);
        CHECK(finite);
        CHECK(peak > 0.001);
        if (round == 0)
            CHECK(late == 0);
        if (round == 1)
            CHECK(longest < 1);
        p.reset();
    }
    CHECK(cleanAfter(before));
}

// the window opened and closed again and again; the plugin unloaded with its window open; the
// host killed with its window open (no X errors, our window empty afterwards)
int xErrors = 0;
int countXError(Display*, XErrorEvent* e) {
    xErrors++;
    char text[128];
    XGetErrorText(e->display, e->error_code, text, sizeof text);
    printf("   X error: %s (request %d)\n", text, e->request_code);
    return 0;
}

void stressEditor(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s's window opened and closed\n", info.name.c_str());
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        printf("   (no X display)\n");
        return;
    }
    auto oldHandler = XSetErrorHandler(countXError);
    Leftovers before = leftovers();
    auto children = [&](Window w) {
        Window root, parent, *kids = nullptr;
        unsigned count = 0;
        XQueryTree(dpy, w, &root, &parent, &kids, &count);
        if (kids)
            XFree(kids);
        return (int)count;
    };
    auto frames = [&](Plugin& p, double seconds) {
        double until = secondsNow() + seconds;
        while (secondsNow() < until) {
            p.idle();
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
            }
            usleep(16000);
        }
    };
    for (int round = 0; round < 3; round++) {
        auto p = loadOrFail(library, info);
        if (!p)
            break;
        Window ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 60, 60, 400, 300, 0, 0, 0);
        XMapWindow(dpy, ours);
        XSync(dpy, False);
        int opens = round == 0 ? (stressLong() ? 20 : 6) : 1, opened = 0, wrong = 0;
        double longestOpen = 0;
        for (int i = 0; i < opens; i++) {
            int w = 0, h = 0;
            double t0 = secondsNow();
            bool ok = p->openEditor((uintptr_t)ours, dpy, 1.0, w, h);
            longestOpen = std::max(longestOpen, secondsNow() - t0);
            if (ok)
                opened++, XResizeWindow(dpy, ours, (unsigned)w, (unsigned)h);
            frames(*p, i % 2 ? 0.05 : 0.4);  // (closed at once, or after it drew)
            if (children(ours) != 1)
                wrong++;
            if (round == 0 || i < opens - 1) {
                p->closeEditor();
                XSync(dpy, False);
                if (children(ours) != 0)
                    wrong++;
            }
        }
        XSync(dpy, False);
        if (round == 0) {
            printf("   opened and closed %d times: %d opened, the longest open %.2f s, %d times wrong\n", opens, opened,
                   longestOpen, wrong);
            CHECK(opened == opens && wrong == 0);
            p.reset();
        } else if (round == 1) {
            double t0 = secondsNow();
            p.reset();  // (unloaded with its window open)
            XSync(dpy, False);
            printf("   unloaded with its window open in %.2f s; our window has %d children after\n", secondsNow() - t0,
                   children(ours));
            CHECK(secondsNow() - t0 < 2);
            CHECK(children(ours) == 0);
        } else {
            kill(bridgedHostPid(*p), SIGKILL);
            usleep(300000);
            double t0 = secondsNow();
            frames(*p, 0.2);
            p->closeEditor();
            CHECK(!p->editorOpen());
            p.reset();
            XSync(dpy, False);
            usleep(300000);
            XSync(dpy, False);
            printf("   its host killed with the window open: closed and unloaded in %.2f s; our window has %d children "
                   "after\n", secondsNow() - t0, children(ours));
            CHECK(secondsNow() - t0 < 2);
            CHECK(children(ours) == 0);
        }
        XDestroyWindow(dpy, ours);
        XSync(dpy, False);
    }
    printf("   %d X errors\n", xErrors);
    CHECK(xErrors == 0);
    XSetErrorHandler(oldHandler);
    XCloseDisplay(dpy);
    CHECK(cleanAfter(before));
}

// (F04) a note-off sent while the host is late (its buffers given up on) reaches the plugin later:
// no note left hanging. An instrument; the host stopped for a few buffers around the note-off.
void stressNotes(PluginLibrary& library, const PluginInfo& info) {
    printf("== stress: %s's notes while its host is late\n", info.name.c_str());
    Leftovers before = leftovers();
    for (int round = 0; round < 2; round++) {
        auto p = loadOrFail(library, info);
        if (!p)
            return;
        p->prepare(48000, 512);
        int pid = bridgedHostPid(*p);
        const int N = 512;
        double tail = 0;
        int lateBefore = bridgedLateBuffers(*p);
        std::thread([&] {
            Stereo out(N);
            for (int b = 0; b < 48000 * 3 / N; b++) {
                std::vector<NoteMessage> notes;
                if (b == 0)
                    notes.push_back({0, true, 60, 100, 0});
                if (round == 1 && b == 20)
                    kill(pid, SIGSTOP);  // (this buffer times out; the next ones find it still busy)
                if (b == 22)
                    notes.push_back({0, false, 60, 0, 0});
                p->setTransport(120, 4, 4, true, (int64_t)b * N);
                p->render(notes, out.out(), N);
                if (round == 1 && b == 24)
                    kill(pid, SIGCONT);
                if (b >= 48000 * 3 / N - 30)
                    tail = std::max(tail, out.peak());
                std::this_thread::sleep_for(std::chrono::microseconds(1000000 * N / 48000));
            }
        }).join();
        int late = bridgedLateBuffers(*p) - lateBefore;
        printf("   %s: %d late; 2.5 s after the note-off: %s, the last 0.3 s's peak %.4f\n",
               round ? "host stopped around the note-off" : "on time", late, p->holding() ? "a note HELD" : "nothing held",
               tail);
        if (round == 1)
            CHECK(late >= 2);  // (the note-off's buffer really was one it was late for)
        CHECK(!p->holding());
        CHECK(tail < 0.001);
        p.reset();
    }
    CHECK(cleanAfter(before));
}

// (F06) the Wine host listing a file's plugins (--classes) for the scan's probe process: the probe
// process given up on (KindFinder kills it after PROBE_TIMEOUT_S) while the host hangs (the host's
// test hook TINYDAW_WINE_TEST_HANG: stuck as a plugin loading; a SIGSTOPped one couldn't see its
// lifeline go); the host must not stay behind
void stressClasses(const PluginInfo& info) {
    printf("== stress: the host listing %s's plugins, hung, its probe process killed\n", info.name.c_str());
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        setenv("TINYDAW_WINE_TEST_HANG", "1", 1);  // (the host hangs as a plugin stuck loading: its lifeline still runs)
        std::string error;
        bridgedClasses(info.path, error);
        _exit(0);
    }
    std::string mark = "tinydaw-classes-" + std::to_string((long)child) + "-";
    auto hostsOf = [&] {
        std::vector<int> found;
        for (const auto& e : util::listDir("/proc")) {
            if (e.name.empty() || !std::isdigit((unsigned char)e.name[0]))
                continue;
            int pid = std::atoi(e.name.c_str());
            if (pid != child && procFile(pid, "cmdline").find(mark) != std::string::npos)
                found.push_back(pid);
        }
        return found;
    };
    std::vector<int> hosts;
    double until = secondsNow() + 30;
    int host = -1;
    while (host < 0 && secondsNow() < until) {
        hosts = hostsOf();
        for (int pid : hosts)
            if (util::trim(procFile(pid, "comm")) == "tinydaw-wine-ho")
                host = pid;
        usleep(20000);
    }
    CHECK(host > 0);
    usleep(1000000);
    CHECK(host > 0 && kill(host, 0) == 0);  // (hung, still there)
    kill(child, SIGKILL);  // (as KindFinder does after PROBE_TIMEOUT_S)
    int status;
    waitpid(child, &status, 0);
    double k0 = secondsNow();
    std::vector<int> left = hostsOf();
    while (!left.empty() && secondsNow() - k0 < 5) {
        usleep(100000);
        left = hostsOf();
    }
    printf("   after its probe process was killed: %s\n",
           left.empty() ? ("its Wine processes gone in " + util::fixed(secondsNow() - k0, 1) + " s").c_str()
                        : (std::to_string(left.size()) + " of its Wine processes still there after 5 s (killed now)").c_str());
    for (int pid : left)
        printf("     %d: %s\n", pid, util::replaceAll(procFile(pid, "cmdline"), std::string(1, '\0'), " ").c_str());
    CHECK(left.empty());
    for (int pid : left)
        kill(pid, SIGKILL);
}

// a window of ours with the plugin's window in it, idled (as the plugin window is every frame)
struct OpenWindow {
    Display* dpy;
    Plugin& p;
    Window ours = 0;
    bool opened = false;
    OpenWindow(Display* d, Plugin& plugin) : dpy(d), p(plugin) {
        ours = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 60, 60, 400, 300, 0, 0, 0);
        XMapWindow(dpy, ours);
        XSync(dpy, False);
        int w = 0, h = 0;
        opened = p.openEditor((uintptr_t)ours, dpy, 1.0, w, h);
        if (opened)
            XResizeWindow(dpy, ours, (unsigned)w, (unsigned)h);
    }
    void frames(double seconds) {
        double until = secondsNow() + seconds;
        do {
            p.idle();
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
            }
            usleep(16000);
        } while (secondsNow() < until);
    }
    ~OpenWindow() {
        p.closeEditor();
        XDestroyWindow(dpy, ours);
        XSync(dpy, False);
    }
};

// (F07) 15 parameter texts in a row (as a stepped automation lane's labels in one timeline
// paint): with the window closed, and open and drawing
void stressTexts(PluginLibrary& library, const PluginInfo& info, Display* dpy) {
    printf("== stress: 15 parameter texts of %s, its window closed and open\n", info.name.c_str());
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    auto params = p->parameters();
    uint32_t id = params.empty() ? 0 : params[0].id;
    for (const auto& q : params)
        if (q.stepCount >= 2 && q.stepCount <= 16 && !q.hidden) {
            id = q.id;
            break;
        }
    auto fifteen = [&] {
        double t0 = secondsNow(), longest = 0;
        for (int k = 0; k < 15; k++) {
            double c0 = secondsNow();
            p->paramText(id, k / 14.0);
            longest = std::max(longest, secondsNow() - c0);
        }
        return std::make_pair(secondsNow() - t0, longest);
    };
    fifteen();
    auto closed = fifteen();
    std::pair<double, double> open{0, 0}, worst{0, 0};
    {
        OpenWindow w(dpy, *p);
        CHECK(w.opened);
        w.frames(1.0);
        for (int round = 0; round < 5; round++) {  // (one paint each, a frame apart)
            open = fifteen();
            if (open.first > worst.first)
                worst = open;
            w.frames(0.05);
        }
    }
    printf("   closed: %.1f ms (longest %.1f ms); open: the worst of 5 paints %.1f ms (longest call %.1f ms)\n",
           1000 * closed.first, 1000 * closed.second, 1000 * worst.first, 1000 * worst.second);
    CHECK(worst.first < 0.016);  // (a frame)
}

// knobs turned while the plugin plays with its window open (Serum 2's WT POS: Wine's Direct2D
// crashed its window's thread, and closing its window then held Tiny DAW's main thread until it was
// killed). First one knob, 60 times a second (WT POS where there is one); then the worst case: every
// knob and switch at once, each frame (the read-only, hidden and program-change ones left alone). The
// host keeps answering, the main thread is never held long, the sound stays numbers, and the window
// closes promptly
void stressKnob(PluginLibrary& library, const PluginInfo& info, Display* dpy) {
    printf("== stress: the knobs of %s turned while it plays, its window open\n", info.name.c_str());
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    p->prepare(48000, 512);
    auto params = p->parameters();
    std::vector<ParamInfo> all;
    for (const auto& q : params)
        if (!q.readOnly && !q.hidden && !q.isProgramChange)
            all.push_back(q);
    if (all.empty()) {
        printf("   (no parameters)\n");
        return;
    }
    const ParamInfo* one = nullptr;
    for (const auto& q : all)
        if (!one && util::containsNoCase(q.name, "wt") && util::containsNoCase(q.name, "pos"))
            one = &q;
    for (const auto& q : all)
        if (!one && q.stepCount == 0)
            one = &q;
    if (!one)
        one = &all[0];
    // (a hung host's text is the value itself, at once: an answer is any other)
    uint32_t asked = one->id;
    auto answers = [&] { return p->paramText(asked, 0.37) != util::fixed(0.37, 2); };
    bool answeredBefore = answers();
    AudioLoop audio(*p, 512);
    audio.start();
    double worst = 0;
    int pid = bridgedHostPid(*p);
    bool answeredOne = false, answeredAll = false, recovered = true;
    double closing = 0, behind = 0;
    {
        OpenWindow w(dpy, *p);
        CHECK(w.opened);
        w.frames(2.0);
        auto frame = [&](const std::function<void()>& change) {
            double t0 = secondsNow();
            change();
            p->idle();
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
            }
            worst = std::max(worst, secondsNow() - t0);
            usleep(16000);
        };
        int turns = stressLong() ? 1800 : 300;
        for (int k = 0; k < turns; k++)
            frame([&] { p->setParam(one->id, 0.5 + 0.5 * std::sin(k * 0.07)); });
        answeredOne = answers();
        int frames = stressLong() ? 900 : 180;
        for (int k = 0; k < frames; k++)
            frame([&] {
                for (size_t i = 0; i < all.size(); i++) {
                    const ParamInfo& q = all[i];
                    double v = q.stepCount > 0 ? (double)(((size_t)k + i) % (size_t)(q.stepCount + 1)) / q.stepCount
                                               : 0.5 + 0.5 * std::sin(k * 0.07 + (double)i * 0.37);
                    p->setParam(q.id, v);
                }
            });
        answeredAll = answers();
        if (!answeredAll) {  // (stuck for good, or only behind for a while: so many changes)
            recovered = false;
            double since = secondsNow();
            while (!recovered && secondsNow() - since < 20) {
                w.frames(0.25);
                recovered = answers();
            }
            behind = secondsNow() - since;
        }
        w.frames(0.5);
        double t0 = secondsNow();
        w.p.closeEditor();
        closing = secondsNow() - t0;
    }
    audio.finish();
    bool alive = pid > 0 && kill(pid, 0) == 0 && !mainThreadGone(pid);
    printf("   \"%s\" turned 60 times a second, then all %zu knobs and switches at once: the longest main-thread "
           "step %.1f ms; answering after one %s, after all %s%s; the window closed in %.2f s; %d buffers, "
           "longest %.1f ms, %d threw, %s; the host %s\n",
           one->name.c_str(), all.size(), 1000 * worst, answeredOne ? "yes" : "NO", answeredAll ? "yes" : "NO",
           answeredAll ? "" : recovered ? (" (" + util::fixed(behind, 1) + " s behind)").c_str() : " (not again in 20 s)", closing, audio.buffers.load(), 1000 * audio.longest.load(),
           audio.threw.load(), audio.finite ? "numbers" : "NOT NUMBERS", alive ? "alive" : "GONE");
    CHECK(answeredBefore);
    CHECK(answeredOne);
    CHECK(recovered);  // (all at once, every frame, a plugin may fall behind for a while, not for good)
    CHECK(worst < 0.5);
    CHECK(closing < 2);
    CHECK(audio.threw == 0);
    CHECK(audio.finite);
    CHECK(alive);
}

// Serum 2's WT POS knob turned by hand, its window open, while it plays (TINYDAW_WINE_TEST_DRAG: the
// host presses the window's left button there and moves it up and down, as messages; the pointer
// never moves). Wine's Direct2D recursed without end filling a path of its wavetable view, ~30 s
// into this, every time: the plugin crashed. Now it plays on, its window answers, and closes
void stressDrag(PluginLibrary& library, const PluginInfo& info, Display* dpy) {
    printf("== stress: %s's WT POS turned by hand for 45 s while it plays\n", info.name.c_str());
    setenv("TINYDAW_WINE_TEST_DRAG", "101,293", 1);  // (Serum 2's WT POS of oscillator A; read as the host starts)
    auto p = loadOrFail(library, info);
    unsetenv("TINYDAW_WINE_TEST_DRAG");
    if (!p)
        return;
    p->prepare(48000, 512);
    int pid = bridgedHostPid(*p);
    AudioLoop audio(*p, 512);
    audio.start();
    double closing = 0;
    bool answering = false;
    {
        OpenWindow w(dpy, *p);
        CHECK(w.opened);
        w.frames(45.0);
        auto params = p->parameters();
        answering = !params.empty() && p->paramText(params[0].id, 0.37) != util::fixed(0.37, 2);
        double t0 = secondsNow();
        w.p.closeEditor();
        closing = secondsNow() - t0;
    }
    audio.finish();
    bool alive = pid > 0 && kill(pid, 0) == 0 && !mainThreadGone(pid);
    printf("   the host %s, answering %s; the window closed in %.2f s; %d buffers, %d threw, %s\n",
           alive ? "alive" : "GONE", answering ? "yes" : "NO", closing, audio.buffers.load(), audio.threw.load(),
           audio.finite ? "numbers" : "NOT NUMBERS");
    CHECK(alive);
    CHECK(answering);
    CHECK(closing < 2);
    CHECK(audio.threw == 0);
    CHECK(audio.finite);
}

// a host whose main thread ends while its process goes on (as when a plugin crashed on it and Wine
// couldn't unwind the host's frames; TINYDAW_WINE_TEST_MAIN_ENDS ends it on a parameter's text):
// found out within a second, the plugin reported as crashed, nothing held long, nothing left over.
// And a plugin's crash itself, in Windows code on the main thread (TINYDAW_WINE_TEST_CRASH: Serum 2
// in Wine's Direct2D): the host ends at once, saying where
void stressMainEnds(PluginLibrary& library, const PluginInfo& info, const char* hook) {
    printf("== stress: %s's host %s\n", info.name.c_str(),
           std::string(hook) == "TINYDAW_WINE_TEST_CRASH" ? "crashes on its main thread" : "loses its main thread");
    Leftovers before = leftovers();
    setenv(hook, "1", 1);
    auto p = loadOrFail(library, info);
    unsetenv(hook);
    if (!p)
        return;
    p->prepare(48000, 512);
    auto params = p->parameters();
    uint32_t id = params.empty() ? 0 : params[0].id;
    AudioLoop audio(*p, 512);
    audio.start();
    usleep(300000);
    double t0 = secondsNow();
    p->paramText(id, 0.5);  // (its main thread ends here)
    double text = secondsNow() - t0;
    t0 = secondsNow();
    p->paramText(id, 0.6);
    double second = secondsNow() - t0;
    t0 = secondsNow();
    while (!audio.threw && secondsNow() - t0 < 3)
        usleep(10000);
    double reported = secondsNow() - t0;
    audio.finish();
    printf("   the text asked as it ended took %.2f s, the next %.2f s; process() threw %.2f s later (%s)\n", text,
           second, reported, audio.threw ? audio.error.c_str() : "it didn't");
    CHECK(text < 1.5);
    CHECK(second < 0.1);
    CHECK(audio.threw > 0);
    p.reset();
    CHECK(cleanAfter(before));
}

// (F09) the window opened and closed many times in one host: the host's memory doesn't grow
// with each (Direct2D plugins: layer bitmaps, devices)
long hostRssKb(int pid) {
    std::string status = procFile(pid, "status");
    size_t at = status.find("VmRSS:");
    return at == std::string::npos ? -1 : std::atol(status.c_str() + at + 6);
}

void stressReopen(PluginLibrary& library, const PluginInfo& info, Display* dpy) {
    int rounds = stressLong() ? 20 : 8;
    printf("== stress: %s's window opened and closed %d times: the host's memory\n", info.name.c_str(), rounds);
    auto p = loadOrFail(library, info);
    if (!p)
        return;
    int pid = bridgedHostPid(*p);
    std::vector<long> rss;
    for (int i = 0; i < rounds; i++) {
        {
            OpenWindow w(dpy, *p);
            CHECK(w.opened);
            w.frames(0.8);
        }
        double until = secondsNow() + 0.3;
        while (secondsNow() < until) {
            p->idle();
            usleep(16000);
        }
        rss.push_back(hostRssKb(pid));
    }
    std::string list;
    for (long kb : rss)
        list += " " + std::to_string(kb / 1024);
    double perRound = rounds > 3 ? (double)(rss.back() - rss[2]) / (rounds - 3) / 1024 : 0;
    printf("   the host's memory after each close (MB):%s; %.1f MB more per round after the 3rd\n", list.c_str(), perRound);
    // (Quanta 2, JUCE 8 on Direct2D, still keeps ~9 MB per open after F09's fix, linearly: every layer
    // bitmap and Direct2D / D3D11 device is freed, so it is Wine's (wined3d on Vulkan) or JUCE's own;
    // the limit catches the 108 MB per open that the emulated layers leaked)
    CHECK(perRound < 20);
}

// (F10) a plat window embedding an XEmbed client (as Tiny DAW's plugin window embeds Wine's):
// the client's XEMBED_REQUEST_FOCUS gives it the X focus; the focus leaving our window tells it
// XEMBED_WINDOW_DEACTIVATE; our window focused again gives it back. A child window of our own on a
// second connection stands for Wine's; the requests are synthetic (XSendEvent: no XTest, no mouse),
// and the focus the user had is given back at once (the whole thing well under a second).
void stressFocus() {
    printf("== stress: XEmbed focus requests answered by a plat window\n");
    plat::init(0, nullptr);
    if (!plat::hasDisplay()) {
        printf("   (no display)\n");
        return;
    }
    struct Quiet : plat::WindowListener {
        void onEvent(plat::Window*, const plat::Event&) override {}
    } listener;
    plat::WindowOptions opts;
    opts.title = "Tiny DAW test (XEmbed focus)";
    opts.w = 200, opts.h = 150;
    opts.offscreen = true;
    auto win = plat::Window::create(opts, &listener);
    win->show();
    auto pump = [] {
        for (int i = 0; i < 6; i++)
            plat::processEvents(10);
    };
    pump();
    Display* client = XOpenDisplay(nullptr);
    if (!client) {
        printf("   (no second display connection)\n");
        return;
    }
    Window ours = (Window)win->nativeHandle();
    Window wine = XCreateSimpleWindow(client, ours, 0, 0, 100, 100, 0, 0, 0);
    XMapWindow(client, wine);
    XSetWindowAttributes a{};
    a.override_redirect = True;
    Window other = XCreateWindow(client, DefaultRootWindow(client), -3000, -3000, 10, 10, 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWOverrideRedirect, &a);
    XMapWindow(client, other);
    XSync(client, False);
    Window had = 0;
    int hadRevert = 0;
    XGetInputFocus(client, &had, &hadRevert);
    Atom xembed = XInternAtom(client, "_XEMBED", False);
    auto focusNow = [&] {
        XSync(client, False);
        Window f = 0;
        int r = 0;
        XGetInputFocus(client, &f, &r);
        return f;
    };
    auto name = [&](Window f) {
        return f == wine ? "the embedded window" : f == ours ? "OUR window" : f == other ? "the other window" : "elsewhere";
    };
    // our window has the focus (as after a click on it); the embedded one asks for it
    XSetInputFocus(client, ours, RevertToParent, CurrentTime);
    XSync(client, False);
    pump();
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = ours;
    e.xclient.message_type = xembed;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = 3;  // XEMBED_REQUEST_FOCUS
    XSendEvent(client, ours, False, NoEventMask, &e);
    XSync(client, False);
    pump();
    Window afterRequest = focusNow();
    // the focus goes elsewhere: the embedded window is told it is no longer active
    XSetInputFocus(client, other, RevertToParent, CurrentTime);
    XSync(client, False);
    pump();
    XSync(client, False);
    bool deactivated = false;
    while (XPending(client)) {
        XEvent got;
        XNextEvent(client, &got);
        if (got.type == ClientMessage && got.xclient.window == wine && got.xclient.message_type == xembed &&
            got.xclient.data.l[1] == 2)  // XEMBED_WINDOW_DEACTIVATE
            deactivated = true;
    }
    // our window focused again: the embedded one gets the keys back
    XSetInputFocus(client, ours, RevertToParent, CurrentTime);
    XSync(client, False);
    pump();
    Window afterBack = focusNow();
    // (the focus back where the user had it)
    if (had > PointerRoot)
        XSetInputFocus(client, had, hadRevert, CurrentTime);
    else
        XSetInputFocus(client, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(client, False);
    printf("   after REQUEST_FOCUS the focus is on %s; the focus going elsewhere %s it; our window focused again: on %s\n",
           name(afterRequest), deactivated ? "deactivated" : "did NOT deactivate", name(afterBack));
    CHECK(afterRequest == wine);
    CHECK(deactivated);
    CHECK(afterBack == wine);
    XDestroyWindow(client, other);
    XDestroyWindow(client, wine);
    XSync(client, False);
    XCloseDisplay(client);
    win.reset();
    pump();
}

// --- resizing (TINYDAW_WINE_STRESS=resize: every Windows plugin, or those of TINYDAW_WINE_ONLY) ---
// Tiny DAW's own plugin window (EditorWindow, as PluginHost opens it) with the plugin's in it, sized
// as a window manager does when the edge is dragged (the window's X11 size changed from outside:
// ConfigureNotify -> EditorWindow::resized -> EDITOR_RESIZED -> the plugin's answer -> fit). Sizes
// asked: bigger, smaller than any plugin takes, odd ones, a fast drag (many in a row, no frame
// between) and a drag at 60 Hz with a knob turning; then the size it had, the window closed and
// opened again (at that size). Each time: the size is one the plugin takes again unchanged and not
// under the least it took; our window and Wine's (embedded) end at it; it is drawn there (not left
// black); the host answers, no main-thread step is long, the sound goes on (an instrument plays a
// chord all along). Plugins that can't be resized keep their size. The plugin's size is put back
// before its window closes (FabFilter's remember theirs in the prefix's registry).

// the Wine window in ours (0: none or more than one)
Window embeddedChild(Display* dpy, Window ours) {
    Window root, parent, *kids = nullptr, child = 0;
    unsigned count = 0;
    if (XQueryTree(dpy, ours, &root, &parent, &kids, &count) && count == 1)
        child = kids[0];
    if (kids)
        XFree(kids);
    return child;
}

bool windowSize(Display* dpy, Window w, int& width, int& height, int* x = nullptr, int* y = nullptr) {
    Window root;
    int wx = 0, wy = 0;
    unsigned ww = 0, wh = 0, b = 0, d = 0;
    if (!w || !XGetGeometry(dpy, w, &root, &wx, &wy, &ww, &wh, &b, &d))
        return false;
    width = (int)ww, height = (int)wh;
    if (x)
        *x = wx;
    if (y)
        *y = wy;
    return true;
}

// what our window shows: distinct colours over all of it, and the share of pure black in the part
// that is new (outside oldW x oldH; all of it when it didn't grow)
struct Drawn {
    int distinct = 0;
    double blackNew = 1;  // (0..1)
    bool ok() const { return distinct > 4 && blackNew < 0.5; }
};
Drawn looked(Display* dpy, Window ours, int w, int h, int oldW, int oldH) {
    Drawn d;
    XImage* img = XGetImage(dpy, ours, 0, 0, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
    if (!img)
        return d;
    std::vector<unsigned long> seen;
    int fresh = 0, black = 0;
    bool grew = w > oldW || h > oldH;
    for (int y = 2; y < img->height; y += 5)
        for (int x = 2; x < img->width; x += 5) {
            unsigned long px = XGetPixel(img, x, y) & 0xffffff;
            if (seen.size() < 64 && std::find(seen.begin(), seen.end(), px) == seen.end())
                seen.push_back(px);
            if (!grew || x >= oldW || y >= oldH) {
                fresh++;
                if (px == 0)
                    black++;
            }
        }
    XDestroyImage(img);
    d.distinct = (int)seen.size();
    d.blackNew = fresh ? (double)black / fresh : 0;
    return d;
}

struct ResizeRig {
    Display* dpy;  // (ours, to look: not the one Tiny DAW's windows are on)
    SlotPtr slot;
    Plugin& p;
    EditorWindow* win = nullptr;
    Window ours = 0;
    double worst = 0;          // the longest main-thread step (events handled, a frame)
    std::string worstWhat, phase;
    std::vector<std::pair<std::string, double>> longest;  // (each phase's longest step)
    std::function<void()> eachFrame;
    ResizeRig(Display* d, SlotPtr s) : dpy(d), slot(std::move(s)), p(*slot->instance) {}

    bool open() {
        win = new EditorWindow(nullptr, slot, "Tiny DAW test (resize)");
        if (!win->attach()) {
            win->close();
            win = nullptr;
            ui::App::get().processEvents();
            return false;
        }
        win->showWindow();
        ours = (Window)win->native()->nativeHandle();
        return true;
    }
    // frames as the program's: its events (a Resize asks the plugin), then the window's idle()
    void step(const char* what) {
        double t0 = secondsNow();
        ui::App::get().processEvents();
        if (win)
            win->idle();
        double took = secondsNow() - t0;
        if (took > worst)
            worst = took, worstWhat = phase + ": " + what;
        if (longest.empty() || longest.back().first != phase)
            longest.push_back({phase, 0.0});
        longest.back().second = std::max(longest.back().second, took);
        if (eachFrame)
            eachFrame();
    }
    void frames(double seconds, const char* what = "a frame") {
        double until = secondsNow() + seconds;
        do {
            step(what);
            usleep(16000);
        } while (secondsNow() < until);
    }
    // the edge dragged to w x h, as a window manager does it
    void drag(int w, int h) {
        XResizeWindow(dpy, ours, (unsigned)std::max(1, w), (unsigned)std::max(1, h));
        XFlush(dpy);
    }
    // frames until our window, Wine's and EditorWindow agree on a size for a while (or `seconds` passed)
    bool settle(int& w, int& h, double seconds = 4) {
        if (!win) {
            w = h = 0;
            return false;
        }
        double until = secondsNow() + seconds, sameSince = 0;
        int lw = -1, lh = -1;
        while (secondsNow() < until) {
            step("settling");
            XSync(dpy, False);
            int ow = 0, oh = 0, cw = 0, ch = 0;
            bool both = windowSize(dpy, ours, ow, oh) && windowSize(dpy, embeddedChild(dpy, ours), cw, ch);
            bool agree = both && ow == cw && oh == ch && ow == win->width() && oh == win->height();
            if (agree && ow == lw && oh == lh) {
                if (secondsNow() - sameSince > 0.25) {
                    w = ow, h = oh;
                    return true;
                }
            } else {
                sameSince = secondsNow();
                lw = agree ? ow : -1, lh = agree ? oh : -1;
            }
            usleep(16000);
        }
        w = win->width(), h = win->height();
        return false;
    }
    // drawn at w x h (it may take the plugin a few frames)
    Drawn drawn(int w, int h, int oldW, int oldH, double seconds = 3) {
        double until = secondsNow() + seconds;
        Drawn d;
        if (!win)
            return d;
        do {
            frames(0.05);
            XSync(dpy, False);
            d = looked(dpy, ours, w, h, oldW, oldH);
        } while (!d.ok() && secondsNow() < until);
        return d;
    }
    void shoot(const std::string& name) {
        if (const char* shots = getenv("TINYDAW_WINE_SHOTS")) {
            XSync(dpy, False);
            run("import -window " + std::to_string((unsigned long)ours) + " '" +
                util::joinPath(shots, util::replaceAll(name, " ", "_") + ".png") + "' 2>/dev/null");
        }
    }
    void close() {
        if (!win)
            return;
        win->close();
        win = nullptr;
        ours = 0;
        for (int i = 0; i < 5; i++) {
            ui::App::get().processEvents();
            usleep(10000);
        }
    }
    ~ResizeRig() { close(); }
};

void stressResize(PluginLibrary& library, const PluginInfo& info, Display* dpy) {
    printf("== stress: %s's window resized\n", info.name.c_str());
    std::shared_ptr<Plugin> p(loadOrFail(library, info).release());
    if (!p)
        return;
    p->prepare(48000, 512);
    auto slot = std::make_shared<PluginSlot>();
    slot->name = info.name;
    slot->path = info.path;
    slot->instance = p;
    std::vector<ParamInfo> knobs;
    for (const auto& q : p->parameters())
        if (!q.readOnly && !q.hidden && !q.isProgramChange && !q.isBypass && q.stepCount == 0)
            knobs.push_back(q);
    // (a hung host's text is the value itself, at once: a knob whose own text differs tells them apart)
    const ParamInfo* told = nullptr;
    for (const auto& q : knobs)
        if (!told && p->paramText(q.id, 0.37) != util::fixed(0.37, 2))
            told = &q;
    auto answers = [&] { return !told || p->paramText(told->id, 0.37) != util::fixed(0.37, 2); };
    AudioLoop audio(*p, 512);
    audio.start();
    int lateBefore = bridgedLateBuffers(*p);
    int pid = bridgedHostPid(*p);
    ResizeRig rig(dpy, slot);
    if (!rig.open()) {
        printf("   window: did NOT open\n");
        CHECK(false);
        return;
    }
    std::string label = util::replaceAll(info.name, " ", "_");
    rig.phase = "opening";
    int w0 = 0, h0 = 0;
    bool settled = rig.settle(w0, h0);
    Drawn first = rig.drawn(w0, h0, w0, h0, 8);
    bool resizable = rig.win->resizable;
    printf("   opened at %dx%d (%s), drawn %s (%d colours), %s\n", w0, h0, settled ? "settled" : "NOT SETTLED",
           first.ok() ? "yes" : "NO", first.distinct, resizable ? "resizable" : "not resizable");
    CHECK(settled);
    CHECK(first.ok());
    rig.shoot(label + "-opened");
    int failures = 0, minW = 1 << 30, minH = 1 << 30;
    std::vector<std::pair<int, int>> answered;
    // one size asked (the edge dragged there) and what came of it
    auto ask = [&](const std::string& what, int w, int h) {
        if (!rig.win) {
            printf("   %-22s: no window\n", what.c_str());
            failures++;
            return std::make_pair(0, 0);
        }
        int oldW = rig.win->width(), oldH = rig.win->height();
        rig.phase = what;
        rig.drag(w, h);
        int aw = 0, ah = 0;
        bool ok = rig.settle(aw, ah);
        int cx = -1, cy = -1, cw = 0, ch = 0;
        windowSize(dpy, embeddedChild(dpy, rig.ours), cw, ch, &cx, &cy);
        Drawn d = rig.drawn(aw, ah, oldW, oldH);
        // the size it took, given back to it, is taken unchanged
        int again = aw, againH = ah;
        if (resizable)
            p->editorResized(again, againH);
        bool stable = again == aw && againH == ah;
        bool talking = answers();
        bool fine = ok && aw > 0 && ah > 0 && cx == 0 && cy == 0 && d.ok() && stable && talking;
        if (!resizable)
            fine = fine && aw == w0 && ah == h0;
        if (!fine)
            failures++;
        answered.push_back({aw, ah});
        printf("   %-22s asked %4dx%-4d -> %4dx%-4d%s Wine's %dx%d at %d,%d; drawn %s (%d colours, %.0f%% of the new "
               "part black)%s%s\n", what.c_str(), w, h, aw, ah, ok ? "" : " (NOT SETTLED)", cw, ch, cx, cy,
               d.ok() ? "yes" : "NO", d.distinct, 100 * d.blackNew,
               stable ? "" : (" ; given back it took " + std::to_string(again) + "x" + std::to_string(againH)).c_str(),
               talking ? "" : " ; the host DOESN'T ANSWER");
        if (!ok) {
            int ow = 0, oh = 0;
            windowSize(dpy, rig.ours, ow, oh);
            Window root, parent, *kids = nullptr;
            unsigned count = 0;
            XQueryTree(dpy, rig.ours, &root, &parent, &kids, &count);
            if (kids)
                XFree(kids);
            printf("   (ours %dx%d, EditorWindow %dx%d, %u windows in ours, the plugin's window %s)\n", ow, oh,
                   rig.win->width(), rig.win->height(), count, p->editorOpen() ? "open" : "CLOSED");
        }
        rig.shoot(label + "-" + util::replaceAll(what, " ", "_"));
        return std::make_pair(aw, ah);
    };
    double peakBefore = audio.peak.exchange(0);
    if (resizable) {
        ask("bigger", w0 * 13 / 10, h0 * 13 / 10);
        auto least = ask("very small", 80, 60);  // (under any plugin's least)
        minW = least.first, minH = least.second;
        ask("odd", w0 + 37, h0 + 13);
        ask("narrow and tall", std::max(minW, w0 * 3 / 4) + 1, h0 * 5 / 4 + 1);
        // a fast drag: 30 sizes in a row, none waited for
        for (int k = 1; k <= 30; k++)
            rig.drag(w0 + 9 * k, h0 + 5 * k);
        ask("fast drag (30 sizes)", w0 + 9 * 31, h0 + 5 * 31);
        // a drag at 60 Hz, back to near its size, a knob turning meanwhile
        int turns = 0;
        rig.eachFrame = [&] {
            if (!knobs.empty())
                p->setParam(knobs[(size_t)turns % std::min<size_t>(knobs.size(), 4)].id, 0.5 + 0.4 * std::sin(turns * 0.2));
            turns++;
        };
        rig.phase = "60 Hz drag, knobs";
        for (int k = 40; k >= 1; k--) {
            rig.drag(w0 + 7 * k, h0 + 4 * k);
            rig.step("a drag frame");
            usleep(16000);
        }
        ask("60 Hz drag, knobs", w0 + 7, h0 + 4);
        rig.eachFrame = nullptr;
        for (auto [aw, ah] : answered)
            if (aw < minW || ah < minH) {
                printf("   %dx%d is under the least it took (%dx%d)\n", aw, ah, minW, minH);
                failures++;
            }
        // the size it had
        auto back = ask("its own size again", w0, h0);
        if (back.first != w0 || back.second != h0)
            failures++;
    } else {
        // the window manager is told it can't be resized (min = max); asked anyway, it stays
        ask("bigger (not resizable)", w0 + 100, h0 + 60);
        // (Tiny DAW doesn't ask such a plugin; asked anyway, through the bridge: what it answers)
        int nw = w0 + 100, nh = h0 + 60, cw = 0, ch = 0;
        p->editorResized(nw, nh);
        rig.frames(0.3);
        windowSize(dpy, embeddedChild(dpy, rig.ours), cw, ch);
        printf("   asked %dx%d through the bridge: answered %dx%d, Wine's window %dx%d%s\n", w0 + 100, h0 + 60, nw, nh,
               cw, ch, nw == w0 && nh == h0 && cw == w0 && ch == h0 ? "" : " (NOT REFUSED: see the report)");
        if (cw != w0 || ch != h0) {  // (its own size back)
            nw = w0, nh = h0;
            p->editorResized(nw, nh);
            rig.frames(0.3);
        }
    }
    double peakDuring = audio.peak.load();
    // opened again: at the size it had when closed (asked bigger first, then its own again)
    int reW = 0, reH = 0, backW = 0, backH = 0;
    bool reopened = false, backOpened = false;
    if (resizable) {
        auto big = ask("bigger, to reopen", w0 + 60, h0 + 40);
        rig.close();
        reopened = rig.open() && rig.settle(reW, reH);
        Drawn d = rig.drawn(reW, reH, reW, reH, 8);
        printf("   closed at %dx%d and opened again: at %dx%d, drawn %s\n", big.first, big.second, reW, reH,
               d.ok() ? "yes" : "NO");
        CHECK(reopened && d.ok());
        if (reW != big.first || reH != big.second)
            printf("   (it didn't open at the size it was closed at)\n");
        ask("its own size, to close", w0, h0);
        rig.close();
        backOpened = rig.open() && rig.settle(backW, backH);
        printf("   closed at %dx%d and opened again: at %dx%d\n", w0, h0, backW, backH);
        CHECK(backOpened && backW == w0 && backH == h0);
        if (backW != w0 || backH != h0) {  // (put back at all costs: the plugin remembers it)
            ask("its own size (again)", w0, h0);
        }
    }
    double closeT0 = secondsNow();
    rig.close();
    double closing = secondsNow() - closeT0;
    audio.finish();
    int late = bridgedLateBuffers(*p) - lateBefore;
    bool alive = pid > 0 && kill(pid, 0) == 0 && !mainThreadGone(pid);
    printf("   %d of %zu sizes wrong; the least it took %dx%d; the longest main-thread step %.0f ms (%s); closed in "
           "%.2f s; %d buffers, longest %.1f ms, %d late, %d threw, %s, peak %.3f before, %.3f while resized; the host "
           "%s\n", failures, answered.size(), minW == 1 << 30 ? 0 : minW, minH == 1 << 30 ? 0 : minH, 1000 * rig.worst,
           rig.worstWhat.c_str(), closing, audio.buffers.load(), 1000 * audio.longest.load(), late, audio.threw.load(),
           audio.finite ? "numbers" : "NOT NUMBERS", peakBefore, peakDuring, alive ? "alive" : "GONE");
    std::string steps;
    for (const auto& [phase, t] : rig.longest)
        steps += (steps.empty() ? "" : ", ") + phase + " " + std::to_string((int)(1000 * t));
    printf("   the longest step (ms) of each: %s\n", steps.c_str());
    CHECK(failures == 0);
    CHECK(rig.worst < 0.25);  // (30 sizes in a row, each asked at once, held it ~0.5 s)
    CHECK(closing < 2);
    CHECK(audio.threw == 0);
    CHECK(audio.finite);
    if (peakBefore > 1e-4)
        CHECK(peakDuring > 1e-4);  // (it sounded all along; Quanta 2 has no sound before a sample is loaded)
    CHECK(alive);
    slot->instance.reset();
    p.reset();
}

void stress(PluginLibrary& library, const std::vector<PluginInfo>& windows) {
    const char* only = getenv("TINYDAW_WINE_ONLY");
    const PluginInfo *effect = nullptr, *instrument = nullptr, *big = nullptr;
    for (const auto& p : windows) {
        bool fits = only ? util::containsNoCase(p.name, only) : util::containsNoCase(p.path, "FabFilter");
        if (fits && p.kind == EFFECT && !effect)
            effect = &p;
        if (fits && p.kind == INSTRUMENT && !instrument)
            instrument = &p;
        if (util::containsNoCase(p.name, "Serum 2") && p.kind == INSTRUMENT)
            big = &p;
    }
    if (!instrument)  // (TINYDAW_WINE_ONLY named an effect)
        for (const auto& p : windows)
            if (p.kind == INSTRUMENT && util::containsNoCase(p.path, "FabFilter"))
                instrument = &p;
    printf("stress: the effect %s, the instrument %s, the big state %s\n", effect ? effect->name.c_str() : "(none)",
           instrument ? instrument->name.c_str() : "(none)", big ? big->name.c_str() : "(none)");
    const PluginInfo* first = effect ? effect : instrument;
    if (!first)
        return;
    if (stressWanted("many"))
        stressMany(library, *first);
    if (stressWanted("kill"))
        stressKill(library, *first);
    if (stressWanted("stop"))
        stressStop(library, *first);
    if (stressWanted("buffers")) {
        stressBuffers(library, *first);
        if (instrument && instrument != first)
            stressBuffers(library, *instrument);
    }
    if (stressWanted("state"))
        stressState(library, *first, big);
    if (stressWanted("rate")) {
        stressRate(library, *first);
        if (instrument && instrument != first)  // (an instrument too: render after the rate changed)
            stressRate(library, *instrument);
    }
    if (stressWanted("offline") && instrument)
        stressOffline(library, *instrument);
    if (stressWanted("notes") && instrument)
        stressNotes(library, *instrument);
    if (stressWanted("classes")) {
        stressClasses(*first);
        for (const PluginInfo* info : {first, big}) {  // (and a normal listing still works, as quick as loading)
            if (!info)
                continue;
            std::string error;
            double t0 = secondsNow();
            auto classes = bridgedClasses(info->path, error);
            std::string names;
            for (const auto& c : classes)
                names += (names.empty() ? "" : ", ") + c.name;
            printf("   listed %s in %.2f s: %s%s\n", util::baseName(info->path).c_str(), secondsNow() - t0,
                   names.empty() ? "NOTHING" : names.c_str(), error.empty() ? "" : (" (" + error + ")").c_str());
            CHECK(!classes.empty());
            CHECK(secondsNow() - t0 < 20);
        }
        usleep(1500000);
        int left = 0;
        for (const auto& e : util::listDir("/proc"))
            if (!e.name.empty() && std::isdigit((unsigned char)e.name[0]) &&
                procFile(std::atoi(e.name.c_str()), "cmdline").find("tinydaw-classes-" + std::to_string((long)getpid()) + "-") !=
                    std::string::npos)
                left++;
        printf("   Wine processes of those listings left 1.5 s after: %d\n", left);
        CHECK(left == 0);
    }
    bool windowsOk = !getenv("TINYDAW_NO_WINDOWS") && getenv("DISPLAY");
    if (stressWanted("mainends")) {
        stressMainEnds(library, *first, "TINYDAW_WINE_TEST_MAIN_ENDS");
        stressMainEnds(library, *first, "TINYDAW_WINE_TEST_CRASH");
    }
    if (windowsOk && stressWanted("drag"))
        if (Display* dpy = XOpenDisplay(nullptr)) {
            for (const auto& p : windows)
                if (p.name == "Serum 2")
                    stressDrag(library, p, dpy);
            XCloseDisplay(dpy);
        }
    if (windowsOk && stressWanted("knob"))
        if (Display* dpy = XOpenDisplay(nullptr)) {
            for (const auto& p : windows)
                if (only ? util::containsNoCase(p.name, only)
                         : p.name == "Serum 2" || p.name == "Quanta 2" || util::containsNoCase(p.name, "Pro-Q 4"))
                    stressKnob(library, p, dpy);
            XCloseDisplay(dpy);
        }
    if (windowsOk && (stressWanted("texts") || stressWanted("reopen"))) {
        if (Display* dpy = XOpenDisplay(nullptr)) {
            for (const auto& p : windows) {
                bool named = only ? util::containsNoCase(p.name, only)
                                  : p.name == "Serum 2" || p.name == "Quanta 2" || util::containsNoCase(p.name, "Pro-Q 4");
                if (named && stressWanted("texts"))
                    stressTexts(library, p, dpy);
                if (named && stressWanted("reopen"))
                    stressReopen(library, p, dpy);
            }
            XCloseDisplay(dpy);
        }
    }
    if (stressWanted("editor") && windowsOk)
        stressEditor(library, *first);
    if (stressWanted("resize") && windowsOk) {
        ui::App::get().init(0, nullptr);
        Display* dpy = XOpenDisplay(nullptr);
        // (with no monitor, as XWayland has while the screens are off, Wine 11's wineserver divides by
        // the monitor's width as Wine windows move or the pointer passes, and dies: SIGFPE in
        // map_point_raw_to_virt; every Wine process of the prefix then waits for good)
        int monitors = 0;
        if (dpy)
            if (XRRMonitorInfo* m = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &monitors))
                XRRFreeMonitors(m);
        if (dpy && monitors <= 0) {
            printf("== stress: resizing NOT tested: the X server has no monitor (screens off?): Wine's server would crash\n");
        } else if (dpy && plat::hasDisplay()) {
            for (const auto& p : windows)
                if (!only || util::containsNoCase(p.name, only))
                    stressResize(library, p, dpy);
        }
        if (dpy)
            XCloseDisplay(dpy);
    }
}

// the kinds of the library's plugin files not known yet, from the probe (through the host), as
// `tinydaw --probe <list>` does; how many were found
int findKinds(PluginLibrary& library) {
    auto unknown = library.unknownFiles();
    if (unknown.empty()) return 0;
    double t0 = secondsNow();
    KindFinder finder(library, [](const std::string& list, int outFd) {
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
    return finder.found();
}

// the Windows builds of the test plugins (make win-plugins), installed in a new prefix by the test
// installer as Install Windows plugin… runs it, then found, loaded through the Wine host and tested
// as any installed plugin, and with what they are known to do
void testWindowsTestPlugins(const std::string& prefix) {
    const char* dirEnv = getenv("TINYDAW_WIN_TEST_PLUGINS");  // (make test sets it; else the release build's)
    std::string win = util::absolutePath(dirEnv && *dirEnv ? dirEnv : "build/win-test-plugins");
    std::string installer = util::joinPath(win, "TinyTestInstaller.exe");
    if (!util::fileExists(installer)) {
        printf("(no Windows test plugins: make win-plugins needs MinGW's x86_64-w64-mingw32-g++)\n");
        return;
    }
    // the 32-bit ones too where they were built and the 32-bit host is there
    bool bits32 = util::fileExists(util::joinPath(win, "TinyTestFx32.vst3")) && !wine::hostPath(true).empty();
    printf("== the Windows test plugins%s, installed in %s\n", bits32 ? " (64- and 32-bit)" : "", prefix.c_str());
    auto before = wine_setup::vst2Dlls({prefix});
    std::string why;
    int code = -2;
    int pid = wine::startInstaller(prefix, installer, why);
    CHECK(pid > 0);
    double t0 = secondsNow();
    while (pid > 0 && !wine::finished(pid, code) && secondsNow() - t0 < 120) usleep(50000);
    CHECK(code == 0);
    std::string c = util::joinPath(prefix, "drive_c");
    std::string vst2 = c + "/Program Files/VstPlugins", vst2x86 = c + "/Program Files (x86)/VstPlugins";
    CHECK(util::fileExists(c + "/Program Files/Common Files/VST3/TinyTestFx.vst3/Contents/x86_64-win/TinyTestFx.vst3"));
    CHECK(util::fileExists(c + "/Program Files/Common Files/VST3/TinyTestFx.vst3/Contents/Resources/moduleinfo.json"));
    CHECK(util::fileExists(vst2 + "/Tiny Test/TinyTestSynth.dll"));
    if (bits32) {
        CHECK(util::fileExists(c + "/Program Files (x86)/Common Files/VST3/TinyTestFx32.vst3/Contents/x86-win/TinyTestFx32.vst3"));
        CHECK(util::fileExists(vst2x86 + "/Tiny Test/TinyTestSynth32.dll"));
    }
    // Install Windows plugin… would offer the VST2 folders it filled
    auto folders = wine_setup::newVst2Folders(before, wine_setup::vst2Dlls({prefix}), {});
    CHECK(folders.size() == (bits32 ? 2u : 1u) && std::find(folders.begin(), folders.end(), vst2) != folders.end());
    if (bits32) CHECK(std::find(folders.begin(), folders.end(), vst2x86) != folders.end());
    PluginLibrary library(wine::vst3Folders({prefix}), util::joinPath(tempRoot, "cache-win.json"), folders);
    findKinds(library);
    std::vector<PluginInfo> found;
    for (const auto& p : library.plugins)
        if (wine::isWindowsPlugin(p.path)) found.push_back(p);
    const PluginInfo *fx = nullptr, *synth = nullptr, *fx32 = nullptr, *synth32 = nullptr;
    for (const auto& p : found) {
        printf("   found %s%s: %s\n", p.label().c_str(), wine::is32BitPlugin(p.path) ? " [32-bit]" : "", p.path.c_str());
        if (p.name == "Tiny Test Fx") (util::endsWith(p.path, "TinyTestFx32.vst3") ? fx32 : fx) = &p;
        if (util::endsWith(p.path, "TinyTestSynth.dll")) synth = &p;
        if (util::endsWith(p.path, "TinyTestSynth32.dll")) synth32 = &p;
    }
    CHECK(fx && fx->kind == EFFECT);
    CHECK(synth && synth->kind == INSTRUMENT);
    if (bits32) {
        CHECK(fx32 && fx32->kind == EFFECT && wine::is32BitPlugin(fx32->path));
        CHECK(synth32 && synth32->kind == INSTRUMENT && wine::is32BitPlugin(synth32->path));
    }
    bool editors = !getenv("TINYDAW_NO_WINDOWS") && getenv("DISPLAY");
    for (const auto& p : found) testPlugin(library, p, editors && p.name != "Tiny Test Single");  // (it has no window)
    // what they are known to do, through the bridge: the effect is its input 64 frames late (×1 at the
    // default gain), the synth sounds while a note is held
    auto exactFx = [&](const PluginInfo& info) {
        auto p = library.load(info, 48000);
        CHECK(p->latency() == 64);
        const int N = 256;
        Stereo in(N), out(N);
        in.l[10] = in.r[10] = 0.5f;
        p->process(in.in(), nullptr, out.out(), N);
        CHECK_NEAR(out.l[74], 0.5f, 1e-5);
        CHECK_NEAR(out.r[74], 0.5f, 1e-5);
        CHECK_NEAR(out.l[10], 0.0f, 1e-6);
        CHECK(bridgedHostPid(*p) > 0);
    };
    auto exactSynth = [&](const PluginInfo& info) {
        auto p = library.load(info, 48000);
        CHECK(p->isInstrument());
        const int N = 512;
        Stereo out(N);
        p->render({{0, true, 69, 100, 0}}, out.out(), N);
        float peak = 0;
        for (float v : out.l) peak = std::max(peak, std::fabs(v));
        CHECK(peak > 0.2f && peak <= 0.51f);  // (a sine at the default volume, 0.5)
        CHECK(util::startsWith(p->paramText(0, 0.5), "50.0"));  // (its display, then its label)
    };
    for (const PluginInfo* p : {fx, fx32})
        if (p) exactFx(*p);
    for (const PluginInfo* p : {synth, synth32})
        if (p) exactSynth(*p);
    if (fx) testCrash(library, *fx);
    if (fx32) testCrash(library, *fx32);
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
    // the wine program chosen for the plugin's prefix is the one that runs its host (a script that
    // notes it ran, then runs the usual one)
    if (!windows.empty() && !wine::prefixOf(windows[0].path).empty()) {
        std::string log = util::joinPath(tempRoot, "chosen-wine.log");
        std::string wrapper = util::joinPath(tempRoot, "chosen/bin/wine");
        util::makeDirs(util::dirName(wrapper));
        util::writeFile(wrapper, "#!/bin/sh\necho \"$@\" >> '" + log + "'\nexec '" + wine::winePath() + "' \"$@\"\n");
        chmod(wrapper.c_str(), 0755);
        wine::setWinePrograms({{wine::prefixOf(windows[0].path), wrapper}});
        std::string error, ran;
        auto classes = bridgedClasses(windows[0].path, error);
        wine::setWinePrograms({});
        util::readFile(log, ran);
        CHECK(ran.find("--classes") != std::string::npos);
        CHECK(!classes.empty());
        if (classes.empty()) printf("  (through the chosen wine: %s)\n", error.c_str());
    }
    // their kinds, from moduleinfo.json or the probe (through the host)
    findKinds(library);
    windows.clear();
    for (const auto& p : library.plugins)
        if (wine::isWindowsPlugin(p.path))
            windows.push_back(p);
    for (const auto& p : windows) {
        CHECK(!p.kind.empty());
        if (p.kind.empty())
            printf("   kind NOT found: %s [%s]\n", p.name.c_str(), p.path.c_str());
    }
    if (getenv("TINYDAW_WINE_STRESS")) {
        stress(library, windows);
        return;
    }
    const char* only = getenv("TINYDAW_WINE_ONLY");
    bool editors =!getenv("TINYDAW_NO_WINDOWS") && getenv("DISPLAY");
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
            testReload(folders, p);
            break;
        }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    plat::isMainThread();  // (this is the main thread: the bridge tells offline rendering by it)
    char tmpl[] = "/tmp/tinydaw-wine-test-XXXXXX";
    tempRoot = mkdtemp(tmpl);
    testPrefixes();
    testPrefixArch();
    testWinePrograms();
    testSetupWithoutWine();
    if (!getenv("TINYDAW_NO_WINE") && !wine::winePath().empty() && !wine::hostPath().empty()) {
        std::string made = testSetupWithWine();
        if (!made.empty()) testWindowsTestPlugins(made);
    }
    testFiles();
    testPrefixAsVst2Folder();
    if (stressWanted("focus") && !getenv("TINYDAW_NO_WINDOWS") && getenv("DISPLAY"))
        stressFocus();
    testInstalled();
    run("rm -rf '" + tempRoot + "'");
    TEST_MAIN_END();
}
