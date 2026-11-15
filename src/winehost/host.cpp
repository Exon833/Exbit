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
// Threads: the plugin's thread (Win32 messages: its window, its timers; requests from Tiny DAW
// arrive as messages, so they're served even while the plugin shows a dialog of its own; not the
// process's first thread, which only waits for it to end: see serveOnPluginThread);
// a reader thread passes the socket's requests to it; the audio thread waits on the shared memory's
// futex. All three are Win32 threads (CreateThread), as the plugin may call Windows from any.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
constexpr UINT MSG_PARAMS = WM_APP + 2;   // lParam: a ParamBatch*
constexpr UINT_PTR IDLE_TIMER = 1;
constexpr UINT MSG_DRAG = WM_APP + 3;     // (tests: TINYDAW_WINE_TEST_DRAG)
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
int windowEdits = 0;  // (tests: TINYDAW_WINE_TEST_DRAG says how many)
std::pair<uint32_t, double> lastWindowEdit{0, 0};
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
        if (block->prepareSet && block->sampleRate > 0) {  // (a new rate, asked on Tiny DAW's audio thread)
            try {
                plugin->prepare(block->sampleRate, std::max(1, std::min(block->maxBlock, MAX_FRAMES)));
            } catch (const std::exception&) {
            }
        }
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
                           m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONUP))
        fprintf(stderr, "tinydaw-wine-host: [%.3f] msg %04x to %p (%s), focus %p (%s), foreground %p\n", traceNow(), m->message,
                (void*)m->hwnd, className(m->hwnd).c_str(), (void*)GetFocus(), className(GetFocus()).c_str(),
                (void*)GetForegroundWindow());
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT WINAPI editorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // (Wine sends an embedded window WM_CLOSE when it is taken out of its embedder: the window goes
    // only when Tiny DAW closes the plugin's view, CLOSE_EDITOR, never under the plugin's feet)
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

uint64_t x11Window(HWND hwnd) { return (uint64_t)(uintptr_t)GetPropA(hwnd, "__wine_x11_whole_window"); }

void startTestDrag();
void stopTestDrag();

void closeEditor() {
    stopTestDrag();
    if (plugin && plugin->editorOpen())
        plugin->closeEditor();
    if (editorWindow)
        DestroyWindow(editorWindow);
    editorWindow = nullptr;
    forgetLayerBitmaps();  // (else they'd keep the closed window's Direct3D device)
    forgetFontLookups();   // (and its font collections)
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
        windowEdits++, lastWindowEdit = {id, v};
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
        fixWineDirectWrite();
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
        if (getenv("TINYDAW_WINE_TEST_MAIN_ENDS"))  // (tests) the main thread ends, as a plugin's crash
            ExitThread(0);                           // on it ended it: the audio thread plays on
        if (getenv("TINYDAW_WINE_TEST_CRASH")) {  // (tests) a plugin's crash: a fault in Windows code on it
            using StrlenFn = size_t(WINAPI*)(const char*);
            HMODULE crt = LoadLibraryW(u"ucrtbase.dll");
            const char* volatile nothing = nullptr;  // (not seen through by the compiler)
            if (auto f = crt ? (StrlenFn)GetProcAddress(crt, "strlen") : nullptr)
                f(nothing);
        }

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
        fixWineDirectWrite();
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
        fixWineDirect2D();  // (and if it loaded them while making its window: before that draws much)
        fixWineDxgi();
        fixWineDirectWrite();
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
        startTestDrag();
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
        if (!plugin->editorResizable() && editorWindow) {  // (it keeps its size: its window too)
            RECT now{0, 0, 0, 0};
            GetClientRect(editorWindow, &now);
            ew = now.right - now.left, eh = now.bottom - now.top;
            reply(in, w.i32(ew).i32(eh));
            break;
        }
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
    case WINDOW_RECT: {
        RECT rect{0, 0, 0, 0};
        if (editorWindow)
            GetWindowRect(editorWindow, &rect);
        reply(in, w.i32(rect.left).i32(rect.top).i32(rect.right).i32(rect.bottom));
        break;
    }
    default:
        reply(in, w);
    }
}

// --- parameter changes, gathered ---

// Tiny DAW sends a change for every knob that moves, every frame (all of them at once, from
// automation or a preset); the reader thread gathers those that wait into one batch, the latest value
// of each, and the main thread sets the batch at once: one wake-up instead of one Win32 message (and
// one trip to the wineserver) each. Order is kept: any other request closes the batch, so it
// is set before that request, and changes after it go into a new one.
struct ParamBatch {
    std::vector<std::pair<uint32_t, double>> changes;
    std::unordered_map<uint32_t, size_t> where;
};
std::mutex batchLock;
ParamBatch* openBatch = nullptr;  // (the batch changes are added to, until the main thread takes it)

void gatherParam(uint32_t id, double value) {
    std::lock_guard<std::mutex> hold(batchLock);
    if (!openBatch) {
        openBatch = new ParamBatch;
        PostMessageW(messageWindow, MSG_PARAMS, 0, (LPARAM)openBatch);
    }
    auto [it, added] = openBatch->where.emplace(id, openBatch->changes.size());
    if (added)
        openBatch->changes.push_back({id, value});
    else
        openBatch->changes[it->second].second = value;
}

void closeBatch() {
    std::lock_guard<std::mutex> hold(batchLock);
    openBatch = nullptr;
}

void setBatch(ParamBatch* batch) {
    static bool setting = false;
    if (setting) {  // (a plugin that handles messages while it sets one: this batch after that one)
        PostMessageW(messageWindow, MSG_PARAMS, 0, (LPARAM)batch);
        return;
    }
    struct Setting {
        Setting() { setting = true; }
        ~Setting() { setting = false; }
    } now;
    std::unique_ptr<ParamBatch> owned(batch);
    {
        std::lock_guard<std::mutex> hold(batchLock);
        if (openBatch == batch)
            openBatch = nullptr;
    }
    if (!plugin)
        return;
    for (auto& [id, v] : batch->changes) {
        try {
            plugin->setParam(id, v);
        } catch (const std::exception&) {
        }
        values[id] = v;
    }
}

// --- (tests) a knob turned by hand, without the mouse ---

// TINYDAW_WINE_TEST_DRAG=x,y: two seconds after the plugin's window opens, its left button is pressed
// at x,y of the window and moved up and down, let go and pressed again now and then (Serum 2's WT POS
// knob turned so crashed Wine's Direct2D), as messages to the window under that point. The pointer
// never moves: the plugin's SetCursorPos and ClipCursor do nothing (plugins hide the pointer while a
// knob turns and put it back), and its GetCursorPos says where the drag is.
int dragTick = 0;
HWND dragTarget = nullptr;
POINT dragAt{0, 0};         // (in the target window)
POINT dragScreen{0, 0};     // (the same, on the screen)
void* realSetCursorPos = nullptr;
void* realGetCursorPos = nullptr;
void* realClipCursor = nullptr;

BOOL WINAPI quietSetCursorPos(int, int) { return TRUE; }
BOOL WINAPI quietClipCursor(const RECT*) { return TRUE; }
BOOL WINAPI dragGetCursorPos(POINT* p) {
    if (p)
        *p = dragScreen;
    return TRUE;
}

// every loaded module's imports of those three, pointed at the ones above
void quietPointer() {
    HMODULE user32 = LoadLibraryW(u"user32.dll");
    realSetCursorPos = (void*)GetProcAddress(user32, "SetCursorPos");
    realGetCursorPos = (void*)GetProcAddress(user32, "GetCursorPos");
    realClipCursor = (void*)GetProcAddress(user32, "ClipCursor");
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed))
        return;
    int patched = 0;
    for (DWORD m = 0; m < std::min<DWORD>(needed / sizeof(HMODULE), 1024); m++) {
        unsigned char* base = (unsigned char*)modules[m];
        uint32_t pe = *(const uint32_t*)(base + 0x3C);
        if (*(const uint16_t*)(base + pe + 24) != 0x20B)  // (PE32+ only)
            continue;
        uint32_t imports = *(const uint32_t*)(base + pe + 24 + 112 + 8);
        if (!imports)
            continue;
        for (const uint32_t* d = (const uint32_t*)(base + imports); d[3]; d += 5) {
            void** slot = (void**)(base + d[4]);
            for (; *slot; slot++) {
                void* to = *slot == realSetCursorPos   ? (void*)quietSetCursorPos
                           : *slot == realGetCursorPos ? (void*)dragGetCursorPos
                           : *slot == realClipCursor   ? (void*)quietClipCursor
                                                       : nullptr;
                if (!to)
                    continue;
                DWORD old = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = to;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                    patched++;
                }
            }
        }
    }
    fprintf(stderr, "tinydaw-wine-host: test drag: %d imports of the pointer's calls quieted\n", patched);
}

// (a thread's clock, as posted messages: a timer's come only when nothing else waits, and a plugin
// that repaints all the time has something waiting all the time)
std::atomic<bool> dragging{false};

DWORD WINAPI dragClock(LPVOID) {
    for (;;) {
        Sleep(16);
        if (dragging.load())
            PostMessageW(messageWindow, MSG_DRAG, 0, 0);
    }
}

void startTestDrag() {
    if (!getenv("TINYDAW_WINE_TEST_DRAG"))
        return;
    dragTick = 0;
    dragTarget = nullptr;
    static bool clock = false;
    if (!clock)
        clock = CreateThread(nullptr, 0, dragClock, nullptr, 0, nullptr) != nullptr;
    dragging = true;
}

void stopTestDrag() {
    dragging = false;
    dragTarget = nullptr;
}

void dragStep() {
    constexpr int START = 120, HOVER = 10, PRESSED = 240, LET_GO = 20;  // (ticks of 16 ms)
    int t = dragTick++;
    if (!editorWindow || t < START)
        return;
    t -= START;
    auto lp = [] { return (LPARAM)(((uint32_t)(uint16_t)dragAt.y << 16) | (uint16_t)dragAt.x); };
    if (!dragTarget) {
        quietPointer();
        int x = 0, y = 0;
        sscanf(getenv("TINYDAW_WINE_TEST_DRAG"), "%d,%d", &x, &y);
        POINT p{x, y};
        HWND h = editorWindow;
        for (int depth = 0; depth < 16; depth++) {  // (the deepest window under the point)
            HWND child = ChildWindowFromPointEx(h, p, CWP_SKIPINVISIBLE);
            if (!child || child == h)
                break;
            MapWindowPoints(h, child, &p, 1);
            h = child;
        }
        dragTarget = h, dragAt = p;
        fprintf(stderr, "tinydaw-wine-host: test drag at %d,%d of the window: %d,%d of %p (%s)\n", x, y, (int)p.x,
                (int)p.y, (void*)h, className(h).c_str());
    }
    int y0 = 0;
    {
        int x = 0, y = 0;
        sscanf(getenv("TINYDAW_WINE_TEST_DRAG"), "%d,%d", &x, &y);
        POINT p{x, y};
        MapWindowPoints(editorWindow, dragTarget, &p, 1);
        y0 = (int)p.y;
    }
    int cycle = HOVER + PRESSED + LET_GO, k = t % cycle;
    if (k == HOVER + PRESSED)
        fprintf(stderr, "tinydaw-wine-host: test drag: %d edits from the window so far (the last: %u = %.3f)\n",
                windowEdits, lastWindowEdit.first, lastWindowEdit.second);
    if (k < HOVER) {
        dragAt.y = y0;
        dragScreen = dragAt;
        ClientToScreen(dragTarget, &dragScreen);
        PostMessageW(dragTarget, WM_MOUSEMOVE, 0, lp());
    } else if (k == HOVER) {
        PostMessageW(dragTarget, WM_LBUTTONDOWN, MK_LBUTTON, lp());
    } else if (k < HOVER + PRESSED) {
        // up and down, fast and slow, by up to 200 pixels
        double s = (k - HOVER) * 0.05 * (1 + (t / cycle) % 3);
        dragAt.y = y0 - (int)(200 * std::sin(s) * std::sin(s * 0.37 + 1));
        dragScreen = dragAt;
        ClientToScreen(dragTarget, &dragScreen);
        PostMessageW(dragTarget, WM_MOUSEMOVE, MK_LBUTTON, lp());
    } else if (k == HOVER + PRESSED) {
        PostMessageW(dragTarget, WM_LBUTTONUP, 0, lp());
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
    if (msg == MSG_PARAMS) {
        setBatch((ParamBatch*)lp);
        return 0;
    }
    if (msg == MSG_DRAG) {
        dragStep();
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
        if (in->header.op == SET_PARAM) {
            Reader r(in->payload);
            uint32_t id = r.u32();
            double v = r.f64();
            if (r.ok()) {
                gatherParam(id, v);
                continue;
            }
        }
        closeBatch();  // (set before this request)
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

// (--classes) Tiny DAW holds the other end of our stdin, a pipe: when it lets go (done, or its
// process killed: a scan's probe that took too long), the host ends, whatever the plugin is doing
DWORD WINAPI lifelineMain(LPVOID) {
    char c[64];
    for (;;) {
        ssize_t k = read(0, c, sizeof(c));
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
    }
    quit();
}

// --classes: one JSON line per file: {"path": …, "classes": [{name, vendor, subcategories, version, instrument}]}
// or {"path": …, "error": …}
int classesMain(const std::string& listFile) {
    struct stat in {};
    if (fstat(0, &in) == 0 && S_ISFIFO(in.st_mode))
        CreateThread(nullptr, 0, lifelineMain, nullptr, 0, nullptr);
    std::string me = std::string(CLASSES_PID_MARK) + std::to_string((long)getpid()) + "\n";
    fwrite(me.data(), 1, me.size(), stdout);
    fflush(stdout);
    if (getenv("TINYDAW_WINE_TEST_HANG"))  // (tests) a plugin that hangs while it is listed
        for (;;)
            Sleep(1000);
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

// --- a plugin's crash ---

// A plugin that crashes (an exception nobody handles) on the host's main thread: Wine looks for a
// handler up the stack, but can't unwind through the host's own (Winelib) frames; its unwinder reads
// past the top of the thread's stack, faults there, and dispatches that fault the same way, until
// the stack runs out and only that thread ends: the audio thread plays on, nothing answers, the
// plugin's window stays frozen (Serum 2's WT POS knob did it, through Wine's Direct2D). Seen as it
// begins (a fault in ntdll reading just past the stack's top, which nothing does on purpose), the host
// ends at once, saying where the plugin crashed: Tiny DAW reports the plugin as crashed.
struct Fault {
    DWORD code = 0;
    void* at = nullptr;
};
Fault lastFault;  // (the latest exception other than C++ ones and ntdll's own; any thread: for the message)
HMODULE ntdllModule = nullptr;
size_t ntdllSize = 0;

std::string whereIs(void* at) {
    HMODULE module = nullptr;
    char text[600];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)at, &module) || !module) {
        std::snprintf(text, sizeof text, "%p", at);
        return text;
    }
    WCHAR name[260];
    DWORD n = GetModuleFileNameW(module, name, 260);
    std::string ascii;
    for (DWORD i = 0; i < n; i++)
        ascii += name[i] < 128 ? (char)name[i] : '?';
    size_t slash = ascii.find_last_of("\\/");
    std::snprintf(text, sizeof text, "%s+0x%llx", ascii.substr(slash == std::string::npos ? 0 : slash + 1).c_str(),
                  (unsigned long long)((const char*)at - (const char*)module));
    return text;
}

// A thread whose stack ran out (a plugin recursing without end: Wine's Direct2D did, filling Serum 2's
// paths): nothing can go on, and little stack is left to say so with. The return addresses just above
// (the frames that recursed: those near where it ran out, in the same DLL) are said, with a bare
// write(), and the host ends at once
void sayOverflowAndEnd(const void* at) {
    uint64_t seen[24];
    int n = 0;
    volatile char here = 0;
    const uint64_t* p = (const uint64_t*)((uintptr_t)&here & ~(uintptr_t)7);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);  // (not read past the stack's top)
    for (int i = 0; i < 16384 && n < 24 && (!high || (ULONG_PTR)(p + i + 1) <= high); i++) {
        uint64_t v = p[i];
        if (v + (16u << 20) < (uint64_t)(uintptr_t)at || v > (uint64_t)(uintptr_t)at + (16u << 20))
            continue;
        bool known = false;
        for (int k = 0; k < n; k++)
            known = known || seen[k] == v;
        if (!known)
            seen[n++] = v;
    }
    char text[700];
    int len = 0;
    auto put = [&](const char* t) {
        while (*t && len < (int)sizeof text - 1)
            text[len++] = *t++;
    };
    auto hex = [&](uint64_t v) {
        char d[19] = "0x";
        int k = 2;
        for (int shift = 60; shift >= 0; shift -= 4) {
            int digit = (int)((v >> shift) & 15);
            if (k > 2 || digit || shift == 0)
                d[k++] = "0123456789abcdef"[digit];
        }
        d[k] = 0;
        put(d);
    };
    put("tinydaw-wine-host: the plugin's thread ran out of stack at ");
    hex((uint64_t)(uintptr_t)at);
    put(" (returning to");
    for (int k = 0; k < n; k++) {
        put(" ");
        hex(seen[k]);
    }
    put("); the host ends\n");
    ssize_t wrote = write(2, text, (size_t)len);
    (void)wrote;
    TerminateProcess(GetCurrentProcess(), 3);
}

LONG WINAPI watchFaults(EXCEPTION_POINTERS* info) {
    constexpr DWORD ACCESS_VIOLATION = 0xC0000005, CPP_EXCEPTION = 0xE06D7363, STACK_OVERFLOW = 0xC00000FD;
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    if (!r)
        return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionCode == STACK_OVERFLOW)
        sayOverflowAndEnd(r->ExceptionAddress);
    const char* at = (const char*)r->ExceptionAddress;
    bool inNtdll = ntdllModule && at >= (const char*)ntdllModule && at < (const char*)ntdllModule + ntdllSize;
    if (r->ExceptionCode == ACCESS_VIOLATION && inNtdll && r->NumberParameters >= 2) {
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        ULONG_PTR read = r->ExceptionInformation[1];
        if (high && read >= high && read < high + (1u << 24)) {
            std::fprintf(stderr, "tinydaw-wine-host: the plugin crashed (exception %08x at %s); the host ends\n",
                         (unsigned)lastFault.code, lastFault.at ? whereIs(lastFault.at).c_str() : "?");
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    if (r->ExceptionCode != CPP_EXCEPTION && !inNtdll) {
        lastFault = {r->ExceptionCode, r->ExceptionAddress};
        // (faults said once each where they happen, before anything handles them: a plugin's crash
        // ends its thread without a word when Wine's dispatcher runs out of stack first)
        static void* said[32];
        static int saidCount = 0;
        bool fault = (r->ExceptionCode & 0xF0000000) == 0xC0000000;
        if (fault && saidCount < 32 && std::find(said, said + saidCount, r->ExceptionAddress) == said + saidCount) {
            said[saidCount++] = r->ExceptionAddress;
            std::fprintf(stderr, "tinydaw-wine-host: exception %08x at %s (thread %lu, %s %p)\n", (unsigned)r->ExceptionCode,
                         whereIs(r->ExceptionAddress).c_str(), (unsigned long)GetCurrentThreadId(),
                         r->NumberParameters >= 2 && r->ExceptionInformation[0] ? "writing" : "reading",
                         r->NumberParameters >= 2 ? (void*)r->ExceptionInformation[1] : nullptr);
            std::fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void watchForCrashes() {
    ntdllModule = GetModuleHandleW(u"ntdll.dll");
    if (ntdllModule) {
        const unsigned char* base = (const unsigned char*)ntdllModule;
        uint32_t pe = *(const uint32_t*)(base + 0x3C);
        ntdllSize = *(const uint32_t*)(base + pe + 24 + 56);  // (OptionalHeader.SizeOfImage)
    }
    AddVectoredExceptionHandler(1, watchFaults);
}

// The plugin's thread (its window, its timers, Tiny DAW's requests: serveMain) isn't the process's
// first one, whose stack Wine makes 1 MB: Wine's Direct2D fills a path by recursing once for every
// triangle a side of it crosses, and the paths of a plugin's busy views (Serum 2's wavetables, as its
// WT POS knob turned) went deeper than that; Windows' own Direct2D doesn't recurse so. Its stack may
// grow to 256 MB (address space, used only as deep as it goes). And the first thread watches it: if it
// ends otherwise than by quit() (a plugin's crash on it, which Wine can't unwind through the host's
// own frames), the host ends at once, and Tiny DAW reports the plugin as crashed.
constexpr SIZE_T PLUGIN_STACK = 256u << 20;
std::string serveSocket;

DWORD WINAPI pluginThreadMain(LPVOID) { return (DWORD)serveMain(serveSocket); }

int serveOnPluginThread(const std::string& socketName) {
    serveSocket = socketName;
    HANDLE thread = CreateThread(nullptr, PLUGIN_STACK, pluginThreadMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                 nullptr);
    if (!thread)
        return serveMain(socketName);
    WaitForSingleObject(thread, INFINITE);
    std::fprintf(stderr, "tinydaw-wine-host: the plugin's thread ended (the plugin crashed%s%s); the host ends\n",
                 lastFault.at ? " at " : "", lastFault.at ? whereIs(lastFault.at).c_str() : "");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "", arg = argc > 2 ? argv[2] : "";
    watchForCrashes();
    if (mode == "--serve" && !arg.empty())
        return serveOnPluginThread(arg);
    if (mode == "--probe" && !arg.empty())
        return plug::runProbe(arg);
    if (mode == "--classes" && !arg.empty())
        return classesMain(arg);
    if (mode == "--path-test" && argc > 3)
        TerminateProcess(GetCurrentProcess(),  // (as quit(): returning from main ends in Wine's abort)
                         pathTest(atoi(argv[2]), atoi(argv[3]), argc > 4 && std::string(argv[4]) == "as-is"));
    fprintf(stderr,
            "tinydaw-wine-host.exe: Tiny DAW's host for Windows plugins (it starts it itself)\n"
            "  --serve <socket> | --probe <list.json> | --classes <list.json>\n");
    return 2;
}
