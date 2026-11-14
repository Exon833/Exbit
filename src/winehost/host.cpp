// tinydaw-wine-host.exe: one Windows plugin, for Tiny DAW, under Wine (see plugins/bridge.h).
//
// A Winelib program: Linux code built with g++ and linked by winegcc against Wine's DLLs, so it
// loads Windows plugins as any Windows program does and talks to Tiny DAW through a Unix socket and
// shared memory. The plugin is hosted by Tiny DAW's own VST3 and VST2 hosts, built for Windows.
//
//   tinydaw-wine-host.exe --serve <socket>     serve one plugin to Tiny DAW (it listens on the
//                                              Unix socket <socket>, a path; the host connects)
//   tinydaw-wine-host.exe --probe <list.json>  the kinds of these plugin files (plugins/probe.h)
//   tinydaw-wine-host.exe --classes <list.json>  the plugins in these files, as JSON lines
//
// Threads: the main thread is the plugin's (Win32 messages: its window, its timers; requests from
// Tiny DAW arrive as messages, so they're served even while the plugin shows a dialog of its own);
// a reader thread passes the socket's requests to it; the audio thread waits on the shared memory's
// futex. All three are Win32 threads (CreateThread), as the plugin may call Windows from any.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/json.h"
#include "core/util.h"
#include "plugins/bridge.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"
#include "winehost/d2dfix.h"

using namespace bridge;

namespace {

constexpr UINT MSG_REQUEST = WM_APP + 1;  // lParam: an Incoming*
constexpr UINT_PTR IDLE_TIMER = 1;
constexpr UINT IDLE_MS = 15;
constexpr int VALUE_SWEEP_TICKS = 8;  // every so many idle ticks, every parameter's value is compared

struct Incoming {
    Header header;
    std::string payload;
};

int sock = -1;
HWND messageWindow = nullptr;
std::unique_ptr<plug::Plugin> plugin;
AudioBlock* block = nullptr;
HANDLE audioThread = nullptr;

// --- the plugin's news, collected every idle tick, handed over at IDLE ---
std::vector<std::pair<uint32_t, double>> editedNews;
std::map<uint32_t, double> values;   // the values Tiny DAW was last told
std::map<uint32_t, double> changedNews;
bool resizeNews = false;
int newsW = 0, newsH = 0;
int latencyNow = 0;
int ticks = 0;

// --- the window ---
HWND editorWindow = nullptr;
const char16_t EDITOR_CLASS[] = u"TinyDAWPluginWindow";

[[noreturn]] void quit() {
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);  // (no clean-up: some plugins misbehave when unloaded at exit)
    _exit(0);
}

void reply(const Incoming& in, const Writer& w) { sendMessage(sock, in.header.op, in.header.seq, w.data); }

// --- the audio thread ---

void realtime() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    sched_param sp{};
    int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
    sp.sched_priority = std::min(hi, lo + 69);  // (a little below Tiny DAW's own audio thread, which waits for this one)
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        sp.sched_priority = lo + 9;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
}

bool tracing = getenv("TINYDAW_WINE_TRACE") != nullptr;

double traceNow() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

DWORD WINAPI audioMain(LPVOID) {
    realtime();
    if (tracing)
        fprintf(stderr, "tinydaw-wine-host: audio thread started\n");
    std::vector<plug::NoteMessage> notes;
    notes.reserve(MAX_NOTES);
    // (from the last buffer done, not the last asked for: a buffer may be asked for before this
    // thread first looks, and it must still be done)
    uint32_t seen = block->done.load(std::memory_order_acquire);
    for (;;) {
        if (tracing) {
            while (!waitChange(&block->request, seen, 1.0))
                fprintf(stderr, "tinydaw-wine-host: waiting: request %u, done %u, seen %u, block %p\n",
                        block->request.load(), block->done.load(), seen, (void*)block);
        } else {
            waitChange(&block->request, seen, -1);
        }
        if (block->quit.load())
            return 0;
        seen = block->request.load(std::memory_order_acquire);
        double started = tracing ? traceNow() : 0;
        if (tracing && seen < 4)
            fprintf(stderr, "tinydaw-wine-host: buffer %u begins\n", seen);
        int frames = std::max(0, std::min(block->frames, MAX_FRAMES));
        int count = std::max(0, std::min(block->paramCount, MAX_PARAM_CHANGES));
        for (int i = 0; i < count; i++)
            plugin->setParam(block->params[i].id, block->params[i].value);
        if (block->notesOff)
            plugin->notesOff();
        if (block->transportSet)
            plugin->setTransport(block->tempo, block->numerator, block->denominator, block->playing != 0,
                                 block->position);
        float* out[2] = {block->out[0], block->out[1]};
        if (block->op == RENDER) {
            notes.clear();
            int n = std::max(0, std::min(block->noteCount, MAX_NOTES));
            for (int i = 0; i < n; i++) {
                const Note& m = block->notes[i];
                notes.push_back({m.frame, m.on != 0, m.pitch, m.velocity, m.channel});
            }
            plugin->render(notes, out, frames);
        } else {
            const float* in[2] = {block->in[0], block->in[1]};
            const float* sc[2] = {block->sidechain[0], block->sidechain[1]};
            plugin->process(in, block->hasSidechain ? sc : nullptr, out, frames);
        }
        block->holding = plugin->holding() ? 1 : 0;
        block->latency = plugin->latency();
        if (tracing && (seen < 4 || traceNow() - started > 0.005))
            fprintf(stderr, "tinydaw-wine-host: buffer %u took %.1f ms\n", seen, 1000 * (traceNow() - started));
        block->done.store(seen, std::memory_order_release);
        wake(&block->done);
    }
}

// --- the window ---

std::string className(HWND h) {
    char name[128] = {0};
    if (h)
        GetClassNameA(h, name, sizeof(name) - 1);
    return name;
}

// (TINYDAW_WINE_TRACE=1) which window the keys go to
LRESULT WINAPI traceHook(int code, WPARAM wp, LPARAM lp) {
    const MSG* m = (const MSG*)lp;
    if (code >= 0 && m && (m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_CHAR ||
                           m->message == WM_LBUTTONDOWN))
        fprintf(stderr, "tinydaw-wine-host: msg %04x to %p (%s), focus %p (%s), foreground %p\n", m->message,
                (void*)m->hwnd, className(m->hwnd).c_str(), (void*)GetFocus(), className(GetFocus()).c_str(),
                (void*)GetForegroundWindow());
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT WINAPI editorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, msg, wp, lp); }

uint64_t x11Window(HWND hwnd) { return (uint64_t)(uintptr_t)GetPropA(hwnd, "__wine_x11_whole_window"); }

void closeEditor() {
    if (plugin && plugin->editorOpen())
        plugin->closeEditor();
    if (editorWindow)
        DestroyWindow(editorWindow);
    editorWindow = nullptr;
}

void resizeEditorWindow(int w, int h) {
    if (editorWindow)
        SetWindowPos(editorWindow, nullptr, 0, 0, std::max(1, w), std::max(1, h),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// --- the plugin's news ---

void collectNews() {
    if (!plugin)
        return;
    plugin->idle();
    for (uint32_t id : plugin->edited()) {
        double v = plugin->getParam(id);
        editedNews.push_back({id, v});
        values[id] = v;
        changedNews.erase(id);
    }
    int w = 0, h = 0;
    if (plugin->requestedSize(w, h) && w > 0 && h > 0) {
        resizeEditorWindow(w, h);  // (the plugin's window grows at once; Tiny DAW's follows)
        resizeNews = true, newsW = w, newsH = h;
    }
    if (++ticks % VALUE_SWEEP_TICKS == 0)
        for (auto& [id, last] : values) {
            double v = plugin->getParam(id);
            if (v != last) {
                last = v;
                changedNews[id] = v;
            }
        }
}

void writeParameters(Writer& w) {
    auto params = plugin->parameters();
    values.clear();
    w.u32((uint32_t)params.size());
    for (const auto& p : params) {
        double v = plugin->getParam(p.id);
        values[p.id] = v;
        w.u32(p.id).str(p.name).str(p.units).f64(p.defaultValue).i32(p.stepCount);
        w.b(p.automatable).b(p.readOnly).b(p.hidden).b(p.isBypass).b(p.isProgramChange).f64(v);
    }
    changedNews.clear();
}

// --- the requests ---

bool openSharedMemory(const std::string& name) {
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0)
        return false;
    void* m = mmap(nullptr, sizeof(AudioBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return false;
    block = (AudioBlock*)m;
    return true;
}

void load(const Incoming& in) {
    Reader r(in.payload);
    std::string path = r.str(), name = r.str(), shm = r.str();
    double sampleRate = r.f64();
    Writer w;
    std::string error;
    try {
        if (!openSharedMemory(shm))
            error = "The Wine host cannot reach Tiny DAW's audio memory";
        else
            plugin = plug::Plugin::load(path, name, error);
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (plugin) {
        fixWineDirect2D();  // (before it draws anything)
        fixWineDxgi();
        try {
            plugin->prepare(sampleRate);
        } catch (const std::exception&) {
        }
        latencyNow = plugin->latency();
        audioThread = CreateThread(nullptr, 0, audioMain, nullptr, 0, nullptr);
        w.b(true).str("");
        w.b(plugin->isVst2()).str(plugin->name()).str(plugin->vendor()).str(plugin->subcategories());
        w.b(plugin->isInstrument()).b(plugin->hasSidechain()).i32(plugin->latency());
    } else {
        w.b(false).str(error.empty() ? "Cannot load " + path : error);
    }
    reply(in, w);
}

void serve(const Incoming& in) {
    Reader r(in.payload);
    Writer w;
    if (in.header.op == LOAD) {
        load(in);
        return;
    }
    if (in.header.op == QUIT)
        quit();
    if (!plugin) {
        reply(in, w);
        return;
    }
    switch (in.header.op) {
    case PREPARE: {
        double sr = r.f64();
        int maxBlock = std::min(r.i32(), MAX_FRAMES);
        plugin->prepare(sr, maxBlock);
        latencyNow = plugin->latency();
        reply(in, w.i32(latencyNow));
        break;
    }
    case RESET:
        plugin->reset();
        latencyNow = plugin->latency();
        reply(in, w.i32(latencyNow));
        break;
    case PARAMETERS:
        writeParameters(w);
        reply(in, w);
        break;
    case SET_PARAM: {
        uint32_t id = r.u32();
        double v = r.f64();
        plugin->setParam(id, v);
        values[id] = v;
        break;
    }
    case PARAM_TEXT: {
        uint32_t id = r.u32();
        double v = r.f64();
        reply(in, w.str(plugin->paramText(id, v)));
        break;
    }
    case WINDOW_EDIT: {
        uint32_t id = r.u32();
        double v = r.f64();
        plugin->windowEdit(id, v);
        break;
    }
    case IDLE: {
        collectNews();
        w.u32((uint32_t)editedNews.size());
        for (auto& [id, v] : editedNews)
            w.u32(id).f64(v);
        w.u32((uint32_t)changedNews.size());
        for (auto& [id, v] : changedNews)
            w.u32(id).f64(v);
        w.b(resizeNews).i32(newsW).i32(newsH);
        w.i32(plugin->latency());
        editedNews.clear();
        changedNews.clear();
        resizeNews = false;
        reply(in, w);
        break;
    }
    case GET_STATE: {
        std::string component = plugin->componentState();
        std::string controller = plugin->controllerState();
        reply(in, w.str(component).str(controller));
        break;
    }
    case SET_STATE: {
        std::string component = r.str(), controller = r.str();
        bool ok = true;
        std::string error;
        try {
            plugin->setState(component, controller);
        } catch (const std::exception& e) {
            ok = false;
            error = e.what();
        }
        latencyNow = plugin->latency();
        reply(in, w.b(ok).str(error).i32(latencyNow));
        break;
    }
    case HAS_EDITOR: {
        bool has = plugin->hasEditor();
        reply(in, w.b(has).b(has && plugin->editorResizable()));
        break;
    }
    case OPEN_EDITOR: {
        double scale = r.f64();
        int sx = r.i32(), sy = r.i32();  // where Tiny DAW's window is on the screen
        if (!r.ok())
            sx = sy = 0;
        fixWineDirect2D();  // (if the plugin loaded them only now)
        fixWineDxgi();
        closeEditor();
        // a borderless popup, not shown, where Tiny DAW's window is (plugins look at where their
        // window is when they make it: JUCE 8 picks the screen it repaints with then, once)
        editorWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, (LPCWSTR)EDITOR_CLASS, u"Tiny DAW plugin",
                                       WS_POPUP | WS_CLIPCHILDREN, sx, sy, 640, 400, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
        int pw = 0, ph = 0;
        bool ok = editorWindow && plugin->openEditor((uintptr_t)editorWindow, nullptr, scale, pw, ph);
        if (!ok) {
            closeEditor();
            reply(in, w.b(false));
            break;
        }
        resizeEditorWindow(pw, ph);
        uint64_t xid = x11Window(editorWindow);
        if (!xid) {  // (Wine makes the X11 window when it is first shown: shown off every screen, not to be seen)
            SetWindowPos(editorWindow, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(editorWindow, SW_SHOWNA);
            for (int i = 0; i < 200 && !(xid = x11Window(editorWindow)); i++) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                Sleep(5);
            }
        }
        if (!xid) {
            closeEditor();
            reply(in, w.b(false));
            break;
        }
        reply(in, w.b(true).i64((int64_t)xid).i32(pw).i32(ph).b(plugin->editorResizable()));
        break;
    }
    case SHOW_EDITOR:
        if (editorWindow)
            ShowWindow(editorWindow, SW_SHOWNA);
        break;
    case CLOSE_EDITOR:
        closeEditor();
        reply(in, w);
        break;
    case EDITOR_RESIZED: {
        int ew = r.i32(), eh = r.i32();
        plugin->editorResized(ew, eh);
        resizeEditorWindow(ew, eh);
        reply(in, w.i32(ew).i32(eh));
        break;
    }
    case EDITOR_PLACE: {
        int x = r.i32(), y = r.i32();
        if (editorWindow)
            SetWindowPos(editorWindow, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (tracing && editorWindow)
            fprintf(stderr, "tinydaw-wine-host: the window is at %d,%d, on monitor %p\n", x, y,
                    (void*)MonitorFromWindow(editorWindow, MONITOR_DEFAULTTONULL));
        break;
    }
    case NOTES_OFF:
        plugin->notesOff();
        break;
    default:
        reply(in, w);
    }
}

LRESULT WINAPI messageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == MSG_REQUEST) {
        std::unique_ptr<Incoming> in((Incoming*)lp);
        try {
            serve(*in);
        } catch (const std::exception& e) {
            // (an answer of the right kind but empty: Tiny DAW reads defaults)
            if (in->header.op != SET_PARAM && in->header.op != WINDOW_EDIT && in->header.op != SHOW_EDITOR &&
                in->header.op != EDITOR_PLACE && in->header.op != NOTES_OFF)
                reply(*in, Writer());
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == IDLE_TIMER) {
        try {
            collectNews();
        } catch (const std::exception&) {
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI readerMain(LPVOID) {
    for (;;) {
        auto in = std::make_unique<Incoming>();
        // Tiny DAW let go of the plugin (or is gone): the host ends at once, from here (the main
        // thread may be busy in the plugin)
        if (!recvMessage(sock, in->header, in->payload) || in->header.op == QUIT)
            quit();
        PostMessageW(messageWindow, MSG_REQUEST, 0, (LPARAM)in.release());
    }
}

int serveMain(const std::string& socketName) {
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketName.c_str(), sizeof(addr.sun_path) - 1);
    if (sock < 0 || connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "tinydaw-wine-host: cannot reach Tiny DAW (%s: %s)\n", socketName.c_str(), strerror(errno));
        return 1;
    }
    sendMessage(sock, HELLO, 0, Writer().i32((int32_t)getpid()).u32(VERSION).data);
    OleInitialize(nullptr);
    HINSTANCE self = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = messageProc;
    wc.hInstance = self;
    wc.lpszClassName = u"TinyDAWHost";
    RegisterClassExW(&wc);
    WNDCLASSEXW ec{};
    ec.cbSize = sizeof(ec);
    ec.lpfnWndProc = editorProc;
    ec.hInstance = self;
    ec.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ec.lpszClassName = (LPCWSTR)EDITOR_CLASS;
    RegisterClassExW(&ec);
    messageWindow = CreateWindowExW(0, u"TinyDAWHost", u"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, self, nullptr);
    SetTimer(messageWindow, IDLE_TIMER, IDLE_MS, nullptr);
    if (tracing)
        SetWindowsHookExW(WH_GETMESSAGE, traceHook, nullptr, GetCurrentThreadId());
    CreateThread(nullptr, 0, readerMain, nullptr, 0, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    quit();
}

// --classes: one JSON line per file: {"path": …, "classes": [{name, vendor, subcategories, version, instrument}]}
// or {"path": …, "error": …}
int classesMain(const std::string& listFile) {
    std::string text;
    if (!util::readFile(listFile, text))
        return 2;
    Json list = Json::parse(text);
    for (const auto& item : list.items()) {
        std::string path = item.toString(), error;
        std::vector<plug::ClassInfo> classes;
        try {
            classes = plug::isVst3Path(path) ? plug::vst3Classes(path, error) : plug::vst2Classes(path, error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        Json found = Json::array();
        for (const auto& c : classes)
            found.push(Json::object({{"name", c.name},
                                     {"vendor", c.vendor},
                                     {"subcategories", c.subcategories},
                                     {"version", c.version},
                                     {"instrument", c.instrument}}));
        Json entry = Json::object({{"path", path}, {"classes", found}});
        if (classes.empty())
            entry.set("error", error.empty() ? std::string("no plugins in this file") : error);
        std::string line = std::string(plug::PROBE_MARK) + entry.dump() + "\n";
        fwrite(line.data(), 1, line.size(), stdout);
        fflush(stdout);
    }
    quit();
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "", arg = argc > 2 ? argv[2] : "";
    if (mode == "--serve" && !arg.empty())
        return serveMain(arg);
    if (mode == "--probe" && !arg.empty())
        return plug::runProbe(arg);
    if (mode == "--classes" && !arg.empty())
        return classesMain(arg);
    fprintf(stderr,
            "tinydaw-wine-host.exe: Tiny DAW's host for Windows plugins (it starts it itself)\n"
            "  --serve <socket> | --probe <list.json> | --classes <list.json>\n");
    return 2;
}
