// Plugins on Windows itself (plugins/wine_win.cpp, child_win.cpp, bridge_win.h, bridged.cpp; the host,
// src/winehost/host.cpp as tinydaw-plugin-host.exe): the repository's test plugins, built as Windows
// DLLs (TINYDAW_WIN_TEST_PLUGINS=<folder>: TinyTestFx.vst3, its moduleinfo.json, TinyTestSynth.dll, and
// the 32-bit TinyTestFx32.vst3 / TinyTestSynth32.dll if built), laid out as installers do in a folder of
// its own; found by the library (VST3 bundles and VST2 DLLs; the synth's kind by the kind finder, in a
// process of its own: this program again, --probe), each loaded out of process in a host of its own,
// asked, processed, its settings saved and given back, its window opened inside one of ours (a child
// window of the host's process) and closed; a host killed, and a plugin that crashes in its host
// (TINYDAW_WINE_TEST_CRASH): the program goes on, the plugin falls silent, nothing hangs; and a plugin that
// hangs for good (TINYDAW_WINE_TEST_FREEZE): our window keeps answering, the watchdog ends its host.
// The host: TINYDAW_PLUGIN_HOST (…_HOST32), else next to this program.
//
// (Windows only; Makefile: test_plugins_win, with the Windows build. TINYDAW_NO_WINDOWS=1: no windows.)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "platform/platform.h"
#include "plugins/child_win.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"
#include "plugins/wine.h"

using namespace plug;

namespace {

double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
};

std::string tempRoot, self;

bool copyFile(const std::string& from, const std::string& to) {
    std::string data;
    return util::readFile(from, data) && util::makeDirs(util::dirName(to)) && util::writeFile(to, data);
}

void pump(double seconds) {
    double until = secondsNow() + seconds;
    while (secondsNow() < until) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

// --- without plugins ---

void testCommandLine() {
    // (as CommandLineToArgvW reads them back)
    std::vector<std::string> args = {"plain", "with space", "", "quote\"in", "trailing\\", "C:\\Program Files\\X\\", "a\\\\\"b"};
    std::wstring line = child::commandLine("C:\\Some Folder\\host.exe", args);
    int n = 0;
    LPWSTR* back = CommandLineToArgvW(line.c_str(), &n);
    CHECK(back && n == (int)args.size() + 1);
    if (back && n == (int)args.size() + 1) {
        CHECK(std::wstring(back[0]) == L"C:\\Some Folder\\host.exe");
        for (size_t i = 0; i < args.size(); i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, back[i + 1], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? len - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, back[i + 1], -1, &s[0], len, nullptr, nullptr);
            if (s != args[i]) fprintf(stderr, "   argument %zu came back as [%s], not [%s]\n", i, s.c_str(), args[i].c_str());
            CHECK(s == args[i]);
        }
    }
    LocalFree(back);
}

void testFolders() {
    // Windows' own folders, from its environment (no Wine prefixes on Windows)
    auto vst3 = defaultFolders();
    const char* common = getenv("CommonProgramFiles");
    auto slashes = [](std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    CHECK(common && !vst3.empty() && util::lower(vst3[0]) == util::lower(slashes(common) + "/VST3"));
    auto vst2 = vst2UsualFolders();
    const char* pf = getenv("ProgramFiles");
    CHECK(pf && std::find(vst2.begin(), vst2.end(), slashes(pf) + "/VstPlugins") != vst2.end());
    CHECK(std::find(vst2.begin(), vst2.end(), slashes(pf) + "/Steinberg/VstPlugins") != vst2.end());
    CHECK(wine::prefixes().empty() && wine::vst3Folders(wine::prefixes()).empty() && wine::winePath().empty());
    std::string why;
    CHECK(wine::startPrefix("C:\\x", "", why) < 0 && !why.empty());
    printf("   VST3 folders: %zu (%s…), VST2 folders: %zu\n", vst3.size(), vst3[0].c_str(), vst2.size());
}

// --- the test plugins ---

struct Found {
    PluginInfo fx, synth, fx32, synth32;
};

Found testScan(const std::string& win, bool bits32) {
    printf("== the test plugins, laid out as installed, found\n");
    Found f;
    std::string vst3 = util::joinPath(tempRoot, "VST3"), vst2 = util::joinPath(tempRoot, "VstPlugins");
    // a bundle (as installers lay them: X.vst3\Contents\x86_64-win\X.vst3) and a VST2 DLL in a folder of its own
    std::string bundle = util::joinPath(vst3, "Tiny/TinyTestFx.vst3");
    CHECK(copyFile(util::joinPath(win, "TinyTestFx.vst3"), util::joinPath(bundle, "Contents/x86_64-win/TinyTestFx.vst3")));
    CHECK(copyFile(util::joinPath(win, "TinyTestFx.moduleinfo.json"), util::joinPath(bundle, "Contents/Resources/moduleinfo.json")));
    CHECK(copyFile(util::joinPath(win, "TinyTestSynth.dll"), util::joinPath(vst2, "Tiny Test/TinyTestSynth.dll")));
    if (bits32) {
        std::string b32 = util::joinPath(vst3, "TinyTestFx32.vst3");
        CHECK(copyFile(util::joinPath(win, "TinyTestFx32.vst3"), util::joinPath(b32, "Contents/x86-win/TinyTestFx32.vst3")));
        CHECK(copyFile(util::joinPath(win, "TinyTestSynth32.dll"), util::joinPath(vst2, "Tiny Test/TinyTestSynth32.dll")));
    }
    PluginLibrary library({vst3}, util::joinPath(tempRoot, "cache.json"), {vst2});
    library.scan();
    // the kinds not known yet (the VST2 synth's), found in a process of its own: this program, --probe
    KindFinder finder(library, [](const std::string& list, int outFd) {
        std::string why;
        return child::spawn(self, {"--probe", list}, -1, outFd, why);
    });
    int unknown = (int)library.unknownFiles().size();
    double t0 = secondsNow();
    finder.start(false);
    while (finder.poll(100) && secondsNow() - t0 < 60) {
    }
    printf("   %d plugin files' kinds found in %.2f s (of %d)\n", finder.found(), secondsNow() - t0, unknown);
    CHECK(unknown >= 1 && finder.found() == unknown);
    for (const auto& p : library.plugins) {
        printf("   found %s: %s\n", p.label().c_str(), p.path.c_str());
        CHECK(wine::isWindowsPlugin(p.path));
        CHECK(p.label().find("Windows") == std::string::npos);  // (every plugin is a Windows one here)
        bool is32 = wine::is32BitPlugin(p.path);
        CHECK(p.is32Bit() == is32 && (p.label().find("32-bit") != std::string::npos) == is32);
        if (p.name == "Tiny Test Fx") (is32 ? f.fx32 : f.fx) = p;
        if (util::endsWith(p.path, "TinyTestSynth.dll")) f.synth = p;
        if (util::endsWith(p.path, "TinyTestSynth32.dll")) f.synth32 = p;
    }
    CHECK(f.fx.kind == EFFECT && util::endsWith(f.fx.path, "TinyTestFx.vst3") && f.fx.path.find("x86_64-win") != std::string::npos);
    CHECK(f.synth.kind == INSTRUMENT);
    if (bits32) CHECK(f.fx32.kind == EFFECT && f.synth32.kind == INSTRUMENT);
    // the classes in a file, asked of a host started for the purpose
    std::string error;
    auto classes = vst3Classes(f.fx.path, error);
    CHECK(!classes.empty() && classes[0].name == "Tiny Test Fx");
    return f;
}

void testFx(PluginLibrary& library, const PluginInfo& info) {
    printf("== %s, out of process\n", info.label().c_str());
    double t0 = secondsNow();
    std::unique_ptr<Plugin> p;
    try {
        p = library.load(info, 48000);
    } catch (const std::exception& e) {
        printf("   load FAILED: %s\n", e.what());
        CHECK(false);
        return;
    }
    printf("   loaded in %.2f s, in host %d\n", secondsNow() - t0, bridgedHostPid(*p));
    CHECK(isBridged(*p) && bridgedHostPid(*p) > 0 && bridgedHostPid(*p) != (int)GetCurrentProcessId());
    CHECK(p->name() == "Tiny Test Fx" && !p->isInstrument());
    // its input 64 frames late, ×1 at the default gain
    CHECK(p->latency() == 64);
    const int N = 256;
    Stereo in(N), out(N);
    in.l[10] = in.r[10] = 0.5f;
    std::thread([&] { p->process(in.in(), nullptr, out.out(), N); }).join();  // (as the audio thread)
    CHECK_NEAR(out.l[74], 0.5f, 1e-5);
    CHECK_NEAR(out.r[74], 0.5f, 1e-5);
    CHECK_NEAR(out.l[10], 0.0f, 1e-6);
    // a thousand buffers: none late
    t0 = secondsNow();
    std::thread([&] {
        for (int i = 0; i < 1000; i++) p->process(in.in(), nullptr, out.out(), N);
    }).join();
    double each = (secondsNow() - t0) / 1000;
    printf("   a buffer through the bridge: %.3f ms; late: %d\n", each * 1000, bridgedLateBuffers(*p));
    CHECK(bridgedLateBuffers(*p) == 0 && each < 0.005);
    // parameters, a change, its settings and back
    auto params = p->parameters();
    CHECK(!params.empty());
    if (!params.empty()) {
        uint32_t id = params[0].id;
        p->setParam(id, 0.25);
        settleBridged(*p);
        CHECK_NEAR(p->getParam(id), 0.25, 1e-6);
        std::thread([&] { p->process(in.in(), nullptr, out.out(), N); }).join();  // (the processor has it with a buffer)
        std::string state = stateOf(*p);
        CHECK(state.size() > 8);
        auto q = library.load(info, 48000);
        setState(*q, state);
        CHECK_NEAR(q->getParam(id), 0.25, 1e-6);
    }
}

void testSynth(PluginLibrary& library, const PluginInfo& info) {
    printf("== %s, out of process\n", info.label().c_str());
    std::unique_ptr<Plugin> p;
    try {
        p = library.load(info, 48000);
    } catch (const std::exception& e) {
        printf("   load FAILED: %s\n", e.what());
        CHECK(false);
        return;
    }
    CHECK(isBridged(*p) && p->isInstrument() && p->isVst2());
    const int N = 512;
    Stereo out(N);
    std::thread([&] { p->render({{0, true, 69, 100, 0}}, out.out(), N); }).join();
    float peak = 0;
    for (float v : out.l) peak = std::max(peak, std::fabs(v));
    printf("   a note: peak %.3f\n", peak);
    CHECK(peak > 0.2f && peak <= 0.51f);  // (a sine at the default volume, 0.5)
    CHECK(util::startsWith(p->paramText(0, 0.5), "50.0"));
}

// the plugin's window, made by its host inside one of ours
void testEditor(PluginLibrary& library, const PluginInfo& info) {
    printf("== %s's window inside ours\n", info.name.c_str());
    auto p = library.load(info, 48000);
    CHECK(p->hasEditor());
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TinyDAWTestEditor";
    RegisterClassW(&wc);
    HWND ours = CreateWindowExW(0, L"TinyDAWTestEditor", L"Tiny DAW test: a plugin's window", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                1480, 560, 420, 320, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowPos(ours, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);  // (in sight, over other programs: tests by hand)
    ShowWindow(ours, SW_SHOW);
    ShowWindow(ours, SW_SHOW);  // (again: the first may be told to hide by how this program was started, e.g. by sshd)
    pump(0.2);
    int w = 0, h = 0;
    double t0 = secondsNow();
    bool opened = p->openEditor((uintptr_t)ours, nullptr, 1.0, w, h);
    printf("   opened: %s, %dx%d, in %.2f s\n", opened ? "yes" : "NO", w, h, secondsNow() - t0);
    CHECK(opened && w > 0 && h > 0 && p->editorOpen());
    HWND plugin = GetWindow(ours, GW_CHILD);
    DWORD pid = 0;
    if (plugin) GetWindowThreadProcessId(plugin, &pid);
    CHECK(plugin && (int)pid == bridgedHostPid(*p));  // (the host's window, a child of ours)
    SetWindowPos(ours, nullptr, 0, 0, w + 40, h + 60, SWP_NOMOVE | SWP_NOZORDER);
    for (int i = 0; i < 30; i++) {  // (as the program's frames)
        p->idle();
        pump(0.03);
    }
    CHECK(plugin && (GetWindowLongW(plugin, GWL_STYLE) & WS_VISIBLE));  // (shown: in ours, shown or not)
    RECT r{};
    if (plugin) GetClientRect(plugin, &r);
    printf("   its window: %ldx%ld, visible: %d (its style %s, ours %s)\n", r.right - r.left, r.bottom - r.top,
           plugin ? (int)IsWindowVisible(plugin) : 0, plugin && (GetWindowLongW(plugin, GWL_STYLE) & WS_VISIBLE) ? "shown" : "hidden",
           IsWindowVisible(ours) ? "shown" : "hidden");
    CHECK(r.right - r.left == w && r.bottom - r.top == h);
    if (const char* shot = getenv("TINYDAW_SHOT_PAUSE")) pump(atof(shot));  // (a moment to look at it: tests by hand)
    // ours moved: the plugin's goes with it (a child window), nothing to tell the host
    SetWindowPos(ours, nullptr, 200, 150, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    p->idle();
    pump(0.1);
    POINT at{0, 0};
    if (plugin) MapWindowPoints(plugin, ours, &at, 1);
    CHECK(at.x == 0 && at.y == 0);
    t0 = secondsNow();
    p->closeEditor();
    CHECK(!p->editorOpen() && (!plugin || !IsWindow(plugin)));
    printf("   closed in %.2f s\n", secondsNow() - t0);
    // and again, then ours closed with it open (the host's window goes with ours)
    CHECK(p->openEditor((uintptr_t)ours, nullptr, 1.0, w, h));
    pump(0.2);
    p->closeEditor();
    DestroyWindow(ours);
    CHECK(!bridgedHostEnded(*p));
}

// a host that ends (killed), and a plugin that crashes in its host: the program goes on, the plugin falls
// silent (an effect lets its input through), nothing hangs, asking it things gives defaults
void testCrashes(PluginLibrary& library, const PluginInfo& info) {
    printf("== crashes: %s\n", info.name.c_str());
    for (int how = 0; how < 2; how++) {
        if (how == 1) _putenv_s("TINYDAW_WINE_TEST_CRASH", "1");  // (the host faults in Windows code, PARAM_TEXT)
        auto p = library.load(info, 48000);
        int pid = bridgedHostPid(*p);
        CHECK(pid > 0);
        double t0 = secondsNow();
        if (how == 0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
            CHECK(h && TerminateProcess(h, 9));
            if (h) CloseHandle(h);
        } else {
            p->paramText(0, 0.5);  // (crashes it: answered with a default text)
        }
        Sleep(300);
        const int N = 512;
        Stereo in(N, 0.3f), out(N);
        int threw = 0;
        std::thread([&] {
            for (int i = 0; i < 4; i++) try {
                    p->process(in.in(), nullptr, out.out(), N);
                } catch (const std::exception&) {
                    threw++;
                }
        }).join();
        CHECK(threw > 0 && bridgedHostEnded(*p));
        CHECK_NEAR(out.l[100], 0.3f, 1e-6);
        CHECK(p->paramText(0, 0.5).size() > 0);
        p->getParam(0);
        p->idle();
        CHECK(secondsNow() - t0 < 2.5);
        printf("   %s: the plugin fell silent in %.2f s; nothing hung\n", how ? "a crash in the host" : "its host killed",
               secondsNow() - t0);
        if (how == 1) _putenv_s("TINYDAW_WINE_TEST_CRASH", "");
    }
}

// a plugin that hangs for good in its host (TINYDAW_WINE_TEST_FREEZE, from a parameter's text): its own
// thread ("1"), its window open inside ours (a child window of a hung thread: ours mustn't hang with it),
// or its audio thread ("audio"). Our window keeps answering (a message sent to it from another thread;
// moved; our frames), the buffers return within their deadline; the watchdog (bridged.cpp, HANG_SECONDS
// = 10) ends the host, its window going with it, and the plugin stops: process() fails, saying so (not
// as a crash); closing, unloading and a fresh load are quick
void testHang(PluginLibrary& library, const PluginInfo& info) {
    for (const char* how : {"1", "audio"}) {
        printf("== a hang (%s): %s\n", std::string(how) == "1" ? "the plugin's thread" : "its audio thread", info.name.c_str());
        _putenv_s("TINYDAW_WINE_TEST_FREEZE", how);
        auto p = library.load(info, 48000);
        _putenv_s("TINYDAW_WINE_TEST_FREEZE", "");
        int pid = bridgedHostPid(*p);
        CHECK(pid > 0);
        HWND ours = nullptr;
        if (!getenv("TINYDAW_NO_WINDOWS")) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"TinyDAWTestHang";
            RegisterClassW(&wc);
            ours = CreateWindowExW(0, L"TinyDAWTestHang", L"Tiny DAW test: a plugin that hangs", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   1480, 560, 420, 320, nullptr, nullptr, wc.hInstance, nullptr);
            ShowWindow(ours, SW_SHOW);
            ShowWindow(ours, SW_SHOW);
            pump(0.2);
            int w = 0, h = 0;
            CHECK(p->openEditor((uintptr_t)ours, nullptr, 1.0, w, h) && GetWindow(ours, GW_CHILD));
            pump(0.2);
        }
        const int N = 512;
        std::atomic<bool> stop{false};
        std::atomic<double> longest{0}, slowestAnswer{0};
        std::atomic<int> buffers{0}, threw{0};
        std::atomic<bool> probed{false};
        std::string error;
        std::thread audio([&] {  // (as the engine's audio thread)
            Stereo in(N, 0.3f), out(N);
            while (!stop) {
                double b0 = secondsNow();
                try {
                    p->process(in.in(), nullptr, out.out(), N);
                } catch (const std::exception& e) {
                    if (!threw++) error = e.what();
                }
                longest = std::max(longest.load(), secondsNow() - b0);
                buffers++;
                Sleep(10);
            }
        });
        pump(0.3);
        std::thread prober([&] {  // (another program asking our window something: answered at once?)
            while (!stop && ours) {
                double s0 = secondsNow();
                DWORD_PTR r = 0;
                SendMessageTimeoutW(ours, WM_NULL, 0, 0, SMTO_NORMAL, 5000, &r);
                slowestAnswer = std::max(slowestAnswer.load(), secondsNow() - s0);
                Sleep(50);
            }
            probed = true;
        });
        double t0 = secondsNow();
        p->paramText(0, 0.5);  // (it hangs here)
        double text = secondsNow() - t0, slowest = 0;
        for (int i = 0; !threw && secondsNow() - t0 < 25; i++) {  // (our frames)
            double f0 = secondsNow();
            p->idle();
            double f1 = secondsNow();
            if (ours && i % 20 == 0) SetWindowPos(ours, nullptr, 1480 - i / 20, 560, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            double f2 = secondsNow();
            pump(0.016);
            double f3 = secondsNow();
            if (f3 - f0 > 0.1)
                printf("   a slow frame at %.2f s: idle() %.3f s, moved %.3f s, its messages %.3f s\n", f0 - t0, f1 - f0,
                       f2 - f1, f3 - f2);
            slowest = std::max(slowest, f3 - f0);
        }
        double stopped = secondsNow() - t0;
        stop = true;
        audio.join();
        while (!probed) pump(0.01);  // (its last message answered: not waited for here)
        prober.join();
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        bool gone = !h || WaitForSingleObject(h, 3000) == WAIT_OBJECT_0;
        if (h) CloseHandle(h);
        for (int i = 0; i < 10; i++) {  // (the program's next frames: its host found ended)
            p->idle();
            pump(0.016);
        }
        printf("   the text asked took %.2f s; stopped %.2f s later (%s); our slowest frame %.3f s, our window's slowest "
               "answer %.3f s, the slowest of %d buffers %.3f s; its host %s\n",
               text, stopped, error.c_str(), slowest, slowestAnswer.load(), buffers.load(), longest.load(),
               gone ? "ended" : "STILL THERE");
        CHECK(text < 2.5);
        CHECK(threw > 0 && stopped > 9 && stopped < 14);
        CHECK(error.find("didn't answer for 10 s") != std::string::npos);
        CHECK(slowest < 0.15 && slowestAnswer < 0.15);
        CHECK(longest < 0.1);
        CHECK(gone && !bridgedHostEnded(*p));
        double c0 = secondsNow();
        if (ours) {
            CHECK(!p->editorOpen() && !GetWindow(ours, GW_CHILD));  // (its window went with it)
            p->closeEditor();
            DestroyWindow(ours);
        }
        p.reset();
        auto q = library.load(info, 48000);  // (as a reload: a host afresh; Windows may give it the same pid)
        bool plays = false;
        std::thread([&] {
            Stereo in(N, 0.3f), out(N);
            try {
                q->process(in.in(), nullptr, out.out(), N);
                plays = true;
            } catch (const std::exception&) {
            }
        }).join();
        CHECK(q && bridgedHostPid(*q) > 0 && !bridgedHostEnded(*q) && plays);
        printf("   closed, unloaded and loaded afresh in %.2f s\n", secondsNow() - c0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--probe")  // (the kind finder's process: this program again)
        return runProbe(argv[2]);
    setvbuf(stdout, nullptr, _IONBF, 0);  // (Windows has no line buffering: _IOLBF is full)
    plat::initWithoutDisplay();  // (this is the main thread: the bridge tells offline rendering by it)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    char exe8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8, sizeof exe8, nullptr, nullptr);
    self = exe8;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string temp = tmp;
    std::replace(temp.begin(), temp.end(), '\\', '/');  // (the program's paths: core/os.h)
    tempRoot = util::joinPath(temp, "tinydaw-plugins-test-" + std::to_string((long)GetCurrentProcessId()));
    util::makeDirs(tempRoot);
    testCommandLine();
    testFolders();
    const char* dirEnv = getenv("TINYDAW_WIN_TEST_PLUGINS");
    std::string win = dirEnv && *dirEnv ? dirEnv : "build/win-test-plugins";
    if (!util::fileExists(util::joinPath(win, "TinyTestFx.vst3")) || wine::hostPath(false).empty()) {
        printf("(no test plugins in %s, or no tinydaw-plugin-host.exe: the plugins weren't tested)\n", win.c_str());
        CHECK(false);
        TEST_MAIN_END();
    }
    bool bits32 = util::fileExists(util::joinPath(win, "TinyTestFx32.vst3")) && !wine::hostPath(true).empty();
    printf("host: %s%s\n", wine::hostPath(false).c_str(), bits32 ? (" and " + wine::hostPath(true)).c_str() : " (no 32-bit one)");
    Found f = testScan(win, bits32);
    PluginLibrary library({}, util::joinPath(tempRoot, "cache2.json"), {});
    testFx(library, f.fx);
    testSynth(library, f.synth);
    if (bits32) {
        testFx(library, f.fx32);
        testSynth(library, f.synth32);
    }
    if (!getenv("TINYDAW_NO_WINDOWS")) {
        testEditor(library, f.fx);
        testEditor(library, f.synth);
        if (bits32) testEditor(library, f.fx32);
    }
    testCrashes(library, f.fx);
    testHang(library, f.fx);
    printf("the program is still here\n");
    TEST_MAIN_END();
}
