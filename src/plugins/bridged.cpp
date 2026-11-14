// A Windows plugin, run by Tiny DAW's Wine host in a process of its own (plugins/bridge.h,
// plugins/wine.h): a plug::Plugin whose every method is answered by the host.
//
// Main thread: requests over the socket, each waiting for its answer (with a time limit: a host
// that hangs makes the plugin fall silent, not the program); idle() reads the plugin's news
// without waiting (one IDLE request out at a time). Audio thread: the buffer goes through the
// shared memory; a host that's late leaves this buffer silent (an effect: its input) and the
// next buffers too, until it has caught up. Parameter values are kept here as the host last told
// them, so asking for them costs nothing.
//
// The plugin's window: the host makes a Win32 window and gives the plugin its view in it; Tiny DAW
// puts that window's X11 window (Wine's) inside its own plugin window, embedded by XEmbed, and tells
// the host where on the screen it really is (for the plugin's sums with the mouse); embedded, Wine
// takes that without moving its X11 window. (Should it move or leave anyway, it is put back.)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"
#include "plugins/bridge.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"
#include "plugins/wine.h"

#include <X11/Xlib.h>  // (last: its macros, Bool, None…, would clash with names above)

namespace plug {

namespace {

using namespace bridge;

constexpr double START_SECONDS = 60;    // Wine starting (the first time in a prefix, it sets it up)
constexpr double LOAD_SECONDS = 120;    // the plugin loading
constexpr double CALL_SECONDS = 30;     // any other request
constexpr double STATE_SECONDS = 120;   // states can be large
constexpr double TEXT_SECONDS = 2;      // a parameter's text

double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<int> made{0};

// a tiny lock for the parameter changes on their way to the audio thread (held for a moment)
class SpinLock {
public:
    void lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
        }
    }
    bool try_lock() { return !flag.test_and_set(std::memory_order_acquire); }
    void unlock() { flag.clear(std::memory_order_release); }

private:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};

// a process that isn't our child (the host: Wine's launcher started it), still there?
bool processAlive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

// Wine's launcher (our child) has ended: reaped; its exit status, or -1 if it's still running
int launcherEnded(int launcher) {
    int status;
    if (launcher <= 0 || waitpid(launcher, &status, WNOHANG) != launcher)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

// the host goes: given graceSeconds to end by itself, then killed; Wine's launcher reaped
void endProcesses(int host, int& launcher, double graceSeconds) {
    double until = secondsNow() + graceSeconds;
    while (host > 0 && processAlive(host) && secondsNow() < until)
        usleep(10000);
    if (host > 0 && processAlive(host))
        kill(host, SIGKILL);
    while (launcher > 0 && launcherEnded(launcher) < 0 && secondsNow() < until)
        usleep(10000);
    if (launcher > 0 && launcherEnded(launcher) < 0) {
        kill(launcher, SIGKILL);
        int status;
        waitpid(launcher, &status, 0);
    }
    launcher = -1;
}

class BridgedPlugin final : public Plugin {
public:
    BridgedPlugin(const std::string& path, const std::string& wanted) {
        std::string why;
        if (!wine::available(why))
            throw std::runtime_error(why);
        int n = ++made;
        std::string tag = std::to_string((long)getpid()) + "-" + std::to_string(n);
        shmName = "/tinydaw-wine-" + tag;
        int fd = shm_open(shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 || ftruncate(fd, sizeof(AudioBlock)) != 0) {
            if (fd >= 0)
                close(fd);
            shm_unlink(shmName.c_str());
            throw std::runtime_error("Cannot make the shared memory for a Windows plugin");
        }
        void* m = mmap(nullptr, sizeof(AudioBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            shm_unlink(shmName.c_str());
            throw std::runtime_error("Cannot map the shared memory for a Windows plugin");
        }
        block = new (m) AudioBlock();
        try {
            start(path, wanted, tag);
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~BridgedPlugin() override { shutdown(); }

    bool isVst2() const override { return vst2_; }
    std::string name() const override { return name_; }
    std::string vendor() const override { return vendor_; }
    std::string subcategories() const override { return subcategories_; }
    bool isInstrument() const override { return instrument_; }
    bool hasSidechain() const override { return sidechain_; }
    int latency() const override { return latency_.load(); }
    double sampleRate() const override { return sampleRate_; }

    void prepare(double sr, int maxBlock) override {
        sampleRate_ = sr;
        std::string answer;
        if (call(PREPARE, Writer().f64(sr).i32(std::min(maxBlock, MAX_FRAMES)), answer, CALL_SECONDS)) {
            Reader r(answer);
            int l = r.i32();
            if (r.ok())
                latency_ = l;
        }
    }

    void reset() override {
        std::string answer;
        if (call(RESET, Writer(), answer, CALL_SECONDS)) {
            Reader r(answer);
            int l = r.i32();
            if (r.ok())
                latency_ = l;
        }
    }

    // --- audio (audio thread) ---

    void setTransport(double tempo, int numerator, int denominator, bool playing, int64_t positionFrames) override {
        t_tempo = tempo, t_num = numerator, t_den = denominator, t_playing = playing, t_position = positionFrames;
    }

    void process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) override {
        int done = 0;
        while (done < frames) {
            int n = std::min(frames - done, MAX_FRAMES);
            const float* i2[2] = {in[0] + done, in[1] + done};
            const float* s2[2] = {sidechain ? sidechain[0] + done : nullptr, sidechain ? sidechain[1] + done : nullptr};
            float* o2[2] = {out[0] + done, out[1] + done};
            if (!run(PROCESS, i2, sidechain ? s2 : nullptr, nullptr, o2, n, done))
                for (int c = 0; c < 2; c++)
                    if (o2[c] != i2[c])
                        std::memcpy(o2[c], i2[c], sizeof(float) * (size_t)n);  // (late: the input, unchanged)
            done += n;
            t_position += n;  // (a long buffer goes in parts)
        }
    }

    void render(const std::vector<NoteMessage>& notes, float* const out[2], int frames) override {
        int done = 0;
        while (done < frames) {
            int n = std::min(frames - done, MAX_FRAMES);
            float* o2[2] = {out[0] + done, out[1] + done};
            if (!run(RENDER, nullptr, nullptr, &notes, o2, n, done))
                for (int c = 0; c < 2; c++)
                    std::memset(o2[c], 0, sizeof(float) * (size_t)n);  // (late: silence)
            done += n;
            t_position += n;
        }
    }

    void notesOff() override { notesOffWanted = true; }
    bool holding() const override { return holding_.load(); }

    // --- parameters ---

    std::vector<ParamInfo> parameters() override {
        if (!paramsKnown)
            fetchParameters();
        return params;
    }

    double getParam(uint32_t id) override {
        if (!paramsKnown)
            fetchParameters();
        auto it = values.find(id);
        if (it != values.end())
            return it->second;
        for (const auto& p : params)
            if (p.id == id)
                return p.defaultValue;
        return 0;
    }

    void setParam(uint32_t id, double value) override {
        if (plat::isMainThread()) {
            values[id] = value;
            post(SET_PARAM, Writer().u32(id).f64(value));  // (the plugin and its window, even when nothing plays)
            if (!processedOnMain)
                return;
        }
        std::lock_guard<SpinLock> hold(changesLock);  // (to the processor, with the next buffer)
        if (changeCount < MAX_PARAM_CHANGES)
            changes[changeCount++] = {id, 0, value};
    }

    std::string paramText(uint32_t id, double value) override {
        std::string answer;
        if (!call(PARAM_TEXT, Writer().u32(id).f64(value), answer, TEXT_SECONDS))
            return util::fixed(value, 2);
        Reader r(answer);
        std::string text = r.str();
        return r.ok() ? text : util::fixed(value, 2);
    }

    std::vector<uint32_t> edited() override {
        std::vector<uint32_t> out;
        out.swap(editedIds);
        return out;
    }

    void windowEdit(uint32_t id, double value) override { post(WINDOW_EDIT, Writer().u32(id).f64(value)); }

    void idle() override {
        std::lock_guard<std::recursive_mutex> hold(callLock);
        if (dead)
            return;
        readWaiting(0);
        if (!idleSeq && !dead) {
            idleSeq = ++seq;
            if (!sendMessage(sock, IDLE, idleSeq, ""))
                died();
        }
        keepEditorInPlace();
    }

    // (tests) wait until the host has answered the IDLE asked for (news read in)
    void settle(double seconds = 5) {
        double until = secondsNow() + seconds;
        idle();
        while (idleSeq && !dead && secondsNow() < until) {
            std::lock_guard<std::recursive_mutex> hold(callLock);
            readWaiting(20);
        }
    }

    // --- state ---

    std::string componentState() override {
        std::string answer;
        stateFetched = false;
        if (!call(GET_STATE, Writer(), answer, STATE_SECONDS))
            return "";
        Reader r(answer);
        std::string component = r.str();
        fetchedController = r.str();
        stateFetched = r.ok();
        return r.ok() ? component : "";
    }

    std::string controllerState() override {
        if (stateFetched) {
            stateFetched = false;
            return fetchedController;
        }
        componentState();
        stateFetched = false;
        return fetchedController;
    }

    void setState(const std::string& component, const std::string& controller) override {
        std::string answer;
        if (!call(SET_STATE, Writer().str(component).str(controller), answer, STATE_SECONDS))
            throw std::runtime_error(name_ + " didn't take its settings (its Wine host doesn't answer)");
        Reader r(answer);
        bool ok = r.b();
        std::string error = r.str();
        int l = r.i32();
        if (r.ok())
            latency_ = l;
        paramsKnown = false;  // (values, maybe names, are new)
        if (r.ok() && !ok)
            throw std::invalid_argument(error.empty() ? "unknown plugin state" : error);
    }

    // --- the window ---

    bool openEditor(uintptr_t parent, void* display, double scale, int& w, int& h) override {
        closeEditor();
        if (!display || !parent)
            return false;
        std::string answer;
        Window child;
        int sx = 0, sy = 0;  // (where our window is on the screen: the plugin's is made there)
        XTranslateCoordinates((Display*)display, (Window)parent, DefaultRootWindow((Display*)display), 0, 0, &sx, &sy, &child);
        if (!call(OPEN_EDITOR, Writer().f64(scale).i32(sx).i32(sy), answer, CALL_SECONDS))
            return false;
        Reader r(answer);
        bool ok = r.b();
        uint64_t xid = (uint64_t)r.i64();
        int pw = r.i32(), ph = r.i32();
        bool resizable = r.b();
        if (!r.ok() || !ok || !xid)
            return false;
        dpy = (Display*)display;
        parentWindow = (Window)parent;
        wineWindow = (Window)xid;
        resizable_ = resizable;
        editorKnown = 1;
        XReparentWindow(dpy, wineWindow, parentWindow, 0, 0);
        XMapWindow(dpy, wineWindow);
        embed();
        XSync(dpy, False);
        post(SHOW_EDITOR, Writer());
        placedX = placedY = INT32_MIN;
        keepEditorInPlace();
        w = pw > 0 ? pw : 640;
        h = ph > 0 ? ph : 400;
        return true;
    }

    void closeEditor() override {
        if (!wineWindow)
            return;
        Window going = wineWindow;
        wineWindow = 0;
        std::string answer;
        // the host closes the plugin's view and its window (gone from ours when this returns); if
        // it doesn't answer, Wine's window leaves ours anyway (if ours went first, it would take
        // Wine's with it)
        if (!call(CLOSE_EDITOR, Writer(), answer, CALL_SECONDS) && dpy) {
            XUnmapWindow(dpy, going);
            XReparentWindow(dpy, going, DefaultRootWindow(dpy), -32000, -32000);
            XSync(dpy, False);
        }
        resizeWanted = false;
    }

    bool editorOpen() const override { return wineWindow != 0; }

    bool hasEditor() override {
        if (editorKnown < 0) {
            std::string answer;
            if (!call(HAS_EDITOR, Writer(), answer, CALL_SECONDS))
                return false;
            Reader r(answer);
            bool has = r.b();
            bool resizable = r.b();
            if (!r.ok())
                return false;
            editorKnown = has ? 1 : 0;
            resizable_ = resizable;
        }
        return editorKnown == 1;
    }

    bool editorResizable() override { return resizable_; }

    bool requestedSize(int& w, int& h) override {
        if (!resizeWanted)
            return false;
        resizeWanted = false;
        w = wantedW;
        h = wantedH;
        return true;
    }

    void editorResized(int& w, int& h) override {
        if (!wineWindow)
            return;
        std::string answer;
        if (!call(EDITOR_RESIZED, Writer().i32(w).i32(h), answer, CALL_SECONDS))
            return;
        Reader r(answer);
        int nw = r.i32(), nh = r.i32();
        if (r.ok() && nw > 0 && nh > 0)
            w = nw, h = nh;
        keepEditorInPlace();
    }

    // (tests) where Wine believes the window is on the screen
    bool windowRect(int& left, int& top, int& right, int& bottom) {
        std::string answer;
        if (!call(WINDOW_RECT, Writer(), answer, CALL_SECONDS))
            return false;
        Reader r(answer);
        left = r.i32(), top = r.i32(), right = r.i32(), bottom = r.i32();
        return r.ok();
    }

    bool hostAlive() const { return !dead; }
    int hostPid() const { return pid; }
    int lateBuffers() const { return late_.load(); }

private:
    // --- the host process ---
    int pid = -1, launcher = -1, sock = -1;  // (pid: the host's; launcher: Wine's, which started it)
    std::string shmName, socketPath;
    AudioBlock* block = nullptr;
    std::recursive_mutex callLock;
    uint32_t seq = 0, idleSeq = 0;
    std::atomic<bool> dead{false};

    // --- facts ---
    bool vst2_ = false, instrument_ = false, sidechain_ = false;
    std::string name_, vendor_, subcategories_;
    std::atomic<int> latency_{0};
    double sampleRate_ = 48000;

    // --- parameters (main thread) ---
    bool paramsKnown = false;
    std::vector<ParamInfo> params;
    std::map<uint32_t, double> values;
    std::vector<uint32_t> editedIds;
    std::string fetchedController;
    bool stateFetched = false;

    // --- audio thread ---
    SpinLock changesLock;
    ParamChange changes[MAX_PARAM_CHANGES];
    int changeCount = 0;
    std::atomic<bool> notesOffWanted{false}, holding_{false};
    std::atomic<int> late_{0};
    bool pending = false;  // a buffer the host hasn't finished
    uint32_t pendingRequest = 0;
    std::atomic<bool> processedOnMain{false};  // (offline rendering: parameter changes go with the buffers)
    double t_tempo = 120;
    int t_num = 4, t_den = 4;
    bool t_playing = false;
    int64_t t_position = 0;

    // --- the window ---
    Display* dpy = nullptr;
    Window parentWindow = 0, wineWindow = 0;
    int editorKnown = -1;
    bool resizable_ = false;
    bool resizeWanted = false;
    int wantedW = 0, wantedH = 0;
    int placedX = INT32_MIN, placedY = INT32_MIN;

    void start(const std::string& path, const std::string& wanted, const std::string& tag) {
        // the socket: a file in a folder only we can enter ($XDG_RUNTIME_DIR, else the temporary folder)
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        const char* tmp = getenv("TMPDIR");
        std::string dir = util::joinPath(runtime && *runtime ? runtime : (tmp && *tmp ? tmp : "/tmp"),
                                         "tinydaw-wine-" + std::to_string((long)getuid()));
        mkdir(dir.c_str(), 0700);
        socketPath = util::joinPath(dir, tag + ".sock");
        unlink(socketPath.c_str());
        int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socketPath.size() >= sizeof(addr.sun_path)) {
            if (listener >= 0)
                close(listener);
            throw std::runtime_error("The folder for a Windows plugin's socket has too long a name: " + dir);
        }
        std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
        if (listener < 0 || bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
            if (listener >= 0)
                close(listener);
            throw std::runtime_error("Cannot make the socket for a Windows plugin (" + socketPath + ": " + strerror(errno) + ")");
        }
        const std::string& socketName = socketPath;
        std::string why;
        launcher = wine::spawnHost(wine::prefixFor(path), {"--serve", socketName}, -1, why);
        if (launcher < 0) {
            close(listener);
            throw std::runtime_error(why);
        }
        double until = secondsNow() + START_SECONDS;
        bool launcherDone = false;
        while (sock < 0) {
            if (readable(listener, 100)) {
                sock = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                break;
            }
            // (Wine's launcher ends as the host starts, with some status of its own; once it has
            // ended, the host has a while to connect, else Wine couldn't start it)
            if (!launcherDone && launcherEnded(launcher) >= 0) {
                launcherDone = true;
                launcher = -1;
                until = std::min(until, secondsNow() + 15);
            }
            if (secondsNow() > until) {
                close(listener);
                throw std::runtime_error(launcherDone ? "Wine couldn't start the plugin host for " + util::baseName(path) +
                                                            " (try running winecfg once for the prefix " +
                                                            wine::prefixFor(path) + ")"
                                                      : "Wine took too long to start the plugin host for " +
                                                            util::baseName(path));
            }
        }
        close(listener);
        unlink(socketPath.c_str());  // (connected: the name isn't needed any more)
        if (sock < 0)
            throw std::runtime_error("The plugin host for " + util::baseName(path) + " didn't connect");
        Header h;
        std::string hello;
        if (!readable(sock, (int)(START_SECONDS * 1000)) || !recvMessage(sock, h, hello) || h.op != HELLO)
            throw std::runtime_error("The plugin host for " + util::baseName(path) + " didn't say hello");
        Reader hr(hello);
        pid = hr.i32();
        if (hr.u32() != VERSION)
            throw std::runtime_error("tinydaw-wine-host.exe is of another version than Tiny DAW (make builds both)");
        std::string answer;
        if (!call(LOAD, Writer().str(path).str(wanted).str(shmName).f64(sampleRate_), answer, LOAD_SECONDS))
            throw std::runtime_error(dead ? util::baseName(path) + " crashed its Wine host while loading"
                                          : util::baseName(path) + " took too long to load");
        shm_unlink(shmName.c_str());  // (the host has it open; nothing is left behind)
        shmName.clear();
        Reader r(answer);
        bool ok = r.b();
        std::string error = r.str();
        if (!r.ok() || !ok)
            throw std::runtime_error(error.empty() ? "Cannot load " + path : error);
        vst2_ = r.b();
        name_ = r.str();
        vendor_ = r.str();
        subcategories_ = r.str();
        instrument_ = r.b();
        sidechain_ = r.b();
        latency_ = r.i32();
    }

    void shutdown() {
        if (wineWindow)
            closeEditor();
        if (block)
            block->quit = 1, block->request.fetch_add(1), wake(&block->request);
        if (sock >= 0) {
            if (!dead)
                sendMessage(sock, QUIT, ++seq, "");
            close(sock);
            sock = -1;
        }
        // (it ends by itself when told, or when the socket closes; it is waited for, and killed if
        // it doesn't, away from the main thread)
        int host = pid, wineLauncher = launcher;
        std::thread([host, wineLauncher]() mutable { endProcesses(host, wineLauncher, 3); }).detach();
        pid = -1;
        launcher = -1;
        if (block) {
            block->~AudioBlock();
            munmap(block, sizeof(AudioBlock));
            block = nullptr;
        }
        if (!shmName.empty())
            shm_unlink(shmName.c_str());
        if (!socketPath.empty())
            unlink(socketPath.c_str());
    }

    void died() {
        dead = true;
        if (wineWindow && dpy) {
            wineWindow = 0;  // (Wine's window went with the host)
        }
    }

    // a request and its answer (false: no answer in time, or the host is gone)
    bool call(Op op, const Writer& w, std::string& answer, double seconds) {
        std::lock_guard<std::recursive_mutex> hold(callLock);
        if (dead || sock < 0)
            return false;
        uint32_t mine = ++seq;
        if (!sendMessage(sock, op, mine, w.data)) {
            died();
            return false;
        }
        double until = secondsNow() + seconds;
        for (;;) {
            double left = until - secondsNow();
            if (left <= 0)
                return false;
            if (!readable(sock, (int)std::min(1000.0, left * 1000) + 1))
                continue;
            Header h;
            std::string payload;
            if (!recvMessage(sock, h, payload)) {
                died();
                return false;
            }
            if (h.seq == mine) {
                answer.swap(payload);
                return true;
            }
            news(h, payload);
        }
    }

    void post(Op op, const Writer& w) {
        std::lock_guard<std::recursive_mutex> hold(callLock);
        if (dead || sock < 0)
            return;
        if (!sendMessage(sock, op, ++seq, w.data))
            died();
    }

    // reads what has come (waiting up to ms for the first)
    void readWaiting(int ms) {
        while (!dead && readable(sock, ms)) {
            ms = 0;
            Header h;
            std::string payload;
            if (!recvMessage(sock, h, payload)) {
                died();
                return;
            }
            news(h, payload);
        }
    }

    // an answer nobody waits for: IDLE's news (others are late answers, dropped)
    void news(const Header& h, const std::string& payload) {
        if (h.op != IDLE || h.seq != idleSeq)
            return;
        idleSeq = 0;
        Reader r(payload);
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && r.ok(); i++) {
            uint32_t id = r.u32();
            double v = r.f64();
            if (!r.ok())
                break;
            editedIds.push_back(id);
            values[id] = v;
        }
        n = r.u32();
        for (uint32_t i = 0; i < n && r.ok(); i++) {
            uint32_t id = r.u32();
            double v = r.f64();
            if (r.ok())
                values[id] = v;
        }
        bool resize = r.b();
        int w = r.i32(), hh = r.i32();
        int l = r.i32();
        if (!r.ok())
            return;
        if (resize && w > 0 && hh > 0)
            resizeWanted = true, wantedW = w, wantedH = hh;
        latency_ = l;
    }

    void fetchParameters() {
        std::string answer;
        if (!call(PARAMETERS, Writer(), answer, CALL_SECONDS))
            return;
        Reader r(answer);
        uint32_t n = r.u32();
        std::vector<ParamInfo> list;
        std::map<uint32_t, double> vals;
        for (uint32_t i = 0; i < n && r.ok(); i++) {
            ParamInfo p;
            p.id = r.u32();
            p.name = r.str();
            p.units = r.str();
            p.defaultValue = r.f64();
            p.stepCount = r.i32();
            p.automatable = r.b();
            p.readOnly = r.b();
            p.hidden = r.b();
            p.isBypass = r.b();
            p.isProgramChange = r.b();
            double v = r.f64();
            if (!r.ok())
                break;
            list.push_back(p);
            vals[p.id] = v;
        }
        if (!r.ok())
            return;
        params.swap(list);
        values.swap(vals);
        paramsKnown = true;
    }

    // --- the audio thread's buffer ---

    bool run(AudioOp op, const float* const* in, const float* const* sidechain, const std::vector<NoteMessage>* notes,
             float* const* out, int frames, int offset) {
        if (dead || !block)
            return false;
        bool onMain = plat::isMainThread();
        if (onMain)
            processedOnMain = true;
        if (pending) {
            if (block->done.load(std::memory_order_acquire) != pendingRequest) {
                late_++;
                return false;
            }
            pending = false;
        }
        block->op = op;
        block->frames = frames;
        block->hasSidechain = sidechain && sidechain[0] && sidechain[1];
        block->notesOff = notesOffWanted.exchange(false) ? 1 : 0;
        block->transportSet = 1;
        block->tempo = t_tempo;
        block->numerator = t_num;
        block->denominator = t_den;
        block->playing = t_playing;
        block->position = t_position;
        block->paramCount = 0;
        if (changesLock.try_lock()) {
            std::memcpy(block->params, changes, sizeof(ParamChange) * (size_t)changeCount);
            block->paramCount = changeCount;
            changeCount = 0;
            changesLock.unlock();
        }
        block->noteCount = 0;
        if (notes)
            for (const auto& m : *notes) {
                if (m.frame < offset || m.frame >= offset + frames)
                    continue;
                if (block->noteCount >= MAX_NOTES)
                    break;
                block->notes[block->noteCount++] = {m.frame - offset, m.on ? 1 : 0, m.pitch, m.velocity, m.channel};
            }
        if (in)
            for (int c = 0; c < 2; c++)
                std::memcpy(block->in[c], in[c], sizeof(float) * (size_t)frames);
        if (block->hasSidechain)
            for (int c = 0; c < 2; c++)
                std::memcpy(block->sidechain[c], sidechain[c], sizeof(float) * (size_t)frames);
        uint32_t before = block->done.load(std::memory_order_acquire);
        uint32_t request = block->request.load(std::memory_order_relaxed) + 1;
        block->request.store(request, std::memory_order_release);
        wake(&block->request);
        // (real time: a few buffers' time at most; offline, on the main thread: as long as it takes)
        double limit = onMain ? CALL_SECONDS : std::max(0.02, 4.0 * frames / std::max(1.0, sampleRate_));
        if (!waitChange(&block->done, before, limit) || block->done.load(std::memory_order_acquire) != request) {
            pending = true;
            pendingRequest = request;
            late_++;
            if (!processAlive(pid))
                dead = true;  // (it crashed)
            return false;
        }
        for (int c = 0; c < 2; c++)
            std::memcpy(out[c], block->out[c], sizeof(float) * (size_t)frames);
        holding_ = block->holding != 0;
        latency_ = block->latency;
        return true;
    }

    // XEmbed: Wine's window is told it is embedded in ours (XEMBED_EMBEDDED_NOTIFY). Wine then
    // follows ours as its parent, and a change of place only changes where Windows thinks the
    // window is: its X11 window stays at our corner (else Wine moved it, to its screen place inside
    // ours, every frame while ours was being moved: flashes of the plugin inside itself).
    void embed() {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = wineWindow;
        e.xclient.message_type = XInternAtom(dpy, "_XEMBED", False);
        e.xclient.format = 32;
        e.xclient.data.l[0] = CurrentTime;
        e.xclient.data.l[1] = 0;  // (XEMBED_EMBEDDED_NOTIFY)
        e.xclient.data.l[2] = 0;
        e.xclient.data.l[3] = (long)parentWindow;
        e.xclient.data.l[4] = 0;  // (protocol version)
        XSendEvent(dpy, wineWindow, False, NoEventMask, &e);
    }

    void keepEditorInPlace() {
        if (!wineWindow || !dpy)
            return;
        // where our window's inside is on the screen: the host tells Wine (for the plugin's sums with
        // the mouse: GetCursorPos against its window's place). Its window being XEmbed-embedded in
        // ours, Wine takes the place without moving its X11 window.
        Window child;
        int x = 0, y = 0;
        if (XTranslateCoordinates(dpy, parentWindow, DefaultRootWindow(dpy), 0, 0, &x, &y, &child) &&
            (x != placedX || y != placedY)) {
            placedX = x, placedY = y;
            post(EDITOR_PLACE, Writer().i32(x).i32(y));
        }
        // and Wine's window stays in ours, at its corner
        Window root, parent, *children = nullptr;
        unsigned count = 0;
        if (XQueryTree(dpy, wineWindow, &root, &parent, &children, &count)) {
            if (children)
                XFree(children);
            if (parent != parentWindow) {
                XReparentWindow(dpy, wineWindow, parentWindow, 0, 0);
                XMapWindow(dpy, wineWindow);
                embed();
            } else {
                int wx, wy;
                unsigned ww, wh, border, depth;
                if (XGetGeometry(dpy, wineWindow, &root, &wx, &wy, &ww, &wh, &border, &depth) && (wx != 0 || wy != 0))
                    XMoveWindow(dpy, wineWindow, 0, 0);
            }
        }
        XFlush(dpy);
    }
};

}  // namespace

std::unique_ptr<Plugin> loadBridged(const std::string& path, const std::string& name, std::string& error) {
    try {
        return std::unique_ptr<Plugin>(new BridgedPlugin(path, name));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

// (tests) the host's news read in; false if `plugin` isn't a Windows one
bool settleBridged(Plugin& plugin, double seconds) {
    auto* b = dynamic_cast<BridgedPlugin*>(&plugin);
    if (!b)
        return false;
    b->settle(seconds);
    return true;
}

bool bridgedWindowRect(Plugin& plugin, int& left, int& top, int& right, int& bottom) {
    auto* b = dynamic_cast<BridgedPlugin*>(&plugin);
    return b && b->windowRect(left, top, right, bottom);
}

int bridgedHostPid(Plugin& plugin) {
    auto* b = dynamic_cast<BridgedPlugin*>(&plugin);
    return b ? b->hostPid() : -1;
}

int bridgedLateBuffers(Plugin& plugin) {
    auto* b = dynamic_cast<BridgedPlugin*>(&plugin);
    return b ? b->lateBuffers() : 0;
}

std::vector<ClassInfo> bridgedClasses(const std::string& path, std::string& error) {
    std::vector<ClassInfo> result;
    static std::atomic<int> lists{0};
    const char* tmp = getenv("TMPDIR");
    std::string list = util::joinPath(tmp && *tmp ? tmp : "/tmp", "tinydaw-classes-" + std::to_string((long)getpid()) +
                                                                       "-" + std::to_string(++lists) + ".json");
    if (!util::writeFile(list, Json::array({Json(path)}).dump())) {
        error = "Cannot write " + list;
        return result;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        util::removeFile(list);
        error = "Cannot start the Wine host";
        return result;
    }
    int pid = wine::spawnHost(wine::prefixFor(path), {"--classes", list}, fds[1], error);
    close(fds[1]);
    std::string output;
    double until = secondsNow() + START_SECONDS + LOAD_SECONDS;
    char chunk[4096];
    while (pid > 0 && secondsNow() < until) {
        if (!readable(fds[0], 200))
            continue;
        ssize_t k = read(fds[0], chunk, sizeof(chunk));
        if (k <= 0)
            break;
        output.append(chunk, (size_t)k);
    }
    close(fds[0]);
    int launcherPid = pid;
    endProcesses(-1, launcherPid, 1);
    util::removeFile(list);
    if (pid < 0)
        return result;
    size_t at = output.rfind(PROBE_MARK);
    if (at == std::string::npos) {
        error = util::baseName(path) + " crashed or hung its Wine host";
        return result;
    }
    std::string line = output.substr(at + std::strlen(PROBE_MARK));
    line = line.substr(0, line.find('\n'));
    Json entry = Json::parse(line);
    for (const auto& c : entry["classes"].items()) {
        ClassInfo info;
        info.name = c.str("name");
        info.vendor = c.str("vendor");
        info.subcategories = c.str("subcategories");
        info.version = c.str("version");
        info.instrument = c["instrument"].toBool(false);
        result.push_back(info);
    }
    if (result.empty())
        error = entry.str("error");
    return result;
}

}  // namespace plug
