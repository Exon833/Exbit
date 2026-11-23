// Windows plugins through Wine: what Tiny DAW (plugins/bridged.cpp) and its Wine host
// (src/winehost/host.cpp, the program tinydaw-wine-host.exe) say to each other.
//
// Each Windows plugin runs in a host process of its own, started under Wine in the plugin's Wine
// prefix (plugins/wine.h). The host is a Winelib program: Linux code that runs inside Wine, so it
// calls the plugin as any Windows program would (LoadLibrary, the Windows calling convention,
// Win32 windows) and talks to Tiny DAW through Linux means. It hosts the plugin with Tiny DAW's own
// VST3 and VST2 hosts (vst3host.cpp, vst2host.cpp, built for Windows), so the two sides speak in
// terms of plug::Plugin's methods:
//
//   - a Unix socket for everything on the main thread: requests (each with a number, answered with
//     the same number, in order), some without an answer (a parameter set, the window's position);
//     the plugin's news (parameters changed in its window, a new size or latency) is collected by
//     the host and handed over when Tiny DAW asks (IDLE, every frame, one at a time);
//   - shared memory for the audio thread (AudioBlock): Tiny DAW puts in the buffer's input, notes,
//     transport and parameter changes, wakes the host's audio thread (a futex), and waits for it to
//     be done. A host that's late leaves the buffer silent rather than holding the audio thread.
//
// Both sides are Linux code, so this header uses Linux calls only (no Win32; the 32-bit host built
// as a Windows program makes them through its Linux side, winehost/unixside.h).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <ctime>
#ifdef TINYDAW_WINE_PE
#include "winehost/unixside.h"  // (the 32-bit host as a Windows program: the same calls, made by its Linux side)
#else
#include <linux/futex.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bridge {

constexpr uint32_t VERSION = 7;  // (5: files dragged out in IDLE's news, DROP_FILES; 6: Note::kind, value; 7: beats)
constexpr int MAX_FRAMES = 8192;       // the longest buffer (prepare()'s maxBlock is capped to it)
constexpr int MAX_NOTES = 1024;        // notes per buffer
constexpr int MAX_PARAM_CHANGES = 1024;  // parameter changes per buffer (more wait for the next)

enum Op : uint32_t {
    HELLO = 0,         // host -> Tiny DAW, first: the host's process id (Wine's launcher, Tiny DAW's child, isn't it)
    // Tiny DAW -> host (answered with the same op and number, unless said otherwise)
    LOAD = 1,          // path, plugin name, shared memory name -> ok, error, the plugin's facts
    PREPARE,           // sample rate, max block -> latency (on the main thread; else AudioBlock::prepareSet)
    RESET,             // -> latency
    PARAMETERS,        // -> [ParamInfo + value]
    SET_PARAM,         // id, value (no answer)
    PARAM_TEXT,        // id, value -> text
    WINDOW_EDIT,       // id, value (no answer)
    IDLE,              // -> news: [(edited id, value)], [(changed id, value)], resize, latency, [files dragged out]
    GET_STATE,         // -> component, controller
    SET_STATE,         // component, controller -> latency
    HAS_EDITOR,        // -> bool, resizable
    OPEN_EDITOR,       // scale, screen x, y -> ok, X11 window, w, h, resizable
    SHOW_EDITOR,       // (no answer) shows the window (after Tiny DAW put it in its own)
    CLOSE_EDITOR,      // -> (nothing)
    EDITOR_RESIZED,    // w, h -> w, h
    EDITOR_PLACE,      // screen x, y (no answer): where the window really is, for the plugin's sums
                       // (its X11 window, XEmbed-embedded in Tiny DAW's, stays where it is)
    NOTES_OFF,         // (no answer)
    QUIT,              // (no answer) the host ends
    WINDOW_RECT,       // -> where Wine believes the window is on the screen: left, top, right, bottom (tests)
    DROP_FILES,        // x, y (in the plugin's window, pixels), [Unix paths] -> how they were taken ("" not)
};

// --- messages: a header (op, number, size) then the payload, written and read with Writer/Reader ---

struct Header {
    uint32_t op;
    uint32_t seq;
    uint32_t size;
};

class Writer {
public:
    std::string data;
    Writer& u8(uint8_t v) { data += (char)v; return *this; }
    Writer& u32(uint32_t v) { return raw(&v, 4); }
    Writer& i32(int32_t v) { return raw(&v, 4); }
    Writer& i64(int64_t v) { return raw(&v, 8); }
    Writer& f64(double v) { return raw(&v, 8); }
    Writer& b(bool v) { return u8(v ? 1 : 0); }
    Writer& str(const std::string& s) {
        u32((uint32_t)s.size());
        data += s;
        return *this;
    }
    Writer& raw(const void* p, size_t n) {
        data.append((const char*)p, n);
        return *this;
    }
};

class Reader {
public:
    explicit Reader(const std::string& d) : data(d) {}
    bool ok() const { return good; }
    uint8_t u8() {
        uint8_t v = 0;
        get(&v, 1);
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        get(&v, 4);
        return v;
    }
    int32_t i32() {
        int32_t v = 0;
        get(&v, 4);
        return v;
    }
    int64_t i64() {
        int64_t v = 0;
        get(&v, 8);
        return v;
    }
    double f64() {
        double v = 0;
        get(&v, 8);
        return v;
    }
    bool b() { return u8() != 0; }
    std::string str() {
        uint32_t n = u32();
        if (!good || n > data.size() - at) {
            good = false;
            return "";
        }
        std::string s = data.substr(at, n);
        at += n;
        return s;
    }

private:
    const std::string& data;
    size_t at = 0;
    bool good = true;
    void get(void* p, size_t n) {
        if (!good || n > data.size() - at) {
            good = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, data.data() + at, n);
        at += n;
    }
};

// all of n bytes (false: the other side is gone)
inline bool sendAll(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

inline bool recvAll(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        ssize_t k = ::recv(fd, c, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

// as sendAll, but giving up when the other side takes nothing for `seconds` (0: whatever fits now;
// errno ETIMEDOUT): a reader that has stopped. `sent` gets how much went (a message cut short
// leaves the stream unreadable)
inline bool sendAllWithin(int fd, const void* p, size_t n, double seconds, size_t& sent) {
    const char* c = (const char*)p;
    sent = 0;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd q{fd, POLLOUT, 0};
            int r;
            do
                r = ::poll(&q, 1, (int)(seconds * 1000));
            while (r < 0 && errno == EINTR);
            if (r == 0)
                errno = ETIMEDOUT;
            if (r <= 0)
                return false;
            continue;
        }
        if (k <= 0)
            return false;
        c += k;
        n -= (size_t)k;
        sent += (size_t)k;
    }
    return true;
}

inline std::string framed(uint32_t op, uint32_t seq, const std::string& payload) {
    Header h{op, seq, (uint32_t)payload.size()};
    std::string all((const char*)&h, sizeof(h));
    all += payload;
    return all;
}

inline bool sendMessage(int fd, uint32_t op, uint32_t seq, const std::string& payload) {
    std::string all = framed(op, seq, payload);
    return sendAll(fd, all.data(), all.size());
}

// (--classes) the host's first line: this, then its process id (so that it can be ended, hung or not)
constexpr const char* CLASSES_PID_MARK = "\x1eTINYDAW-PID ";

constexpr uint32_t MAX_MESSAGE = 512u << 20;  // (a plugin state can be large; anything past this is nonsense)

inline bool recvMessage(int fd, Header& h, std::string& payload) {
    if (!recvAll(fd, &h, sizeof(h)) || h.size > MAX_MESSAGE)
        return false;
    payload.resize(h.size);
    return h.size == 0 || recvAll(fd, &payload[0], h.size);
}

// is a message waiting (or the other side gone)? waits up to ms
inline bool readable(int fd, int ms) {
    pollfd p{fd, POLLIN, 0};
    int r;
    do
        r = ::poll(&p, 1, ms);
    while (r < 0 && errno == EINTR);
    return r > 0;
}

// --- the audio thread's shared memory ---

// plug::NoteMessage: a note (kind 0), the bend wheel (1: value -8192..8191) or a control change (2:
// pitch the control number, value 0..127)
struct Note {
    int32_t frame, on, pitch, velocity, channel;
    int32_t kind, value;
};

struct ParamChange {
    uint32_t id;
    uint32_t pad;
    double value;
};

enum AudioOp : uint32_t { PROCESS = 1, RENDER = 2 };

struct AudioBlock {
    // the futex words: Tiny DAW counts `request` up and wakes the host; the host sets `done` to it
    std::atomic<uint32_t> request;
    std::atomic<uint32_t> done;
    std::atomic<uint32_t> quit;  // the host's audio thread ends
    uint32_t op;
    int32_t frames;
    int32_t hasSidechain;
    int32_t notesOff;      // note-offs for every held note first
    // transport (setTransport)
    int32_t transportSet;
    int32_t numerator, denominator, playing;
    alignas(8) double tempo;  // (alignas: the same layout for the 32-bit host, whose ABI aligns these to 4)
    alignas(8) int64_t position;
    alignas(8) double beats;  // the song's beats at `position` (< 0: none given)
    // a new sample rate, asked for on the audio thread (prepare() there is no request: the host's
    // audio thread prepares the plugin before this buffer)
    int32_t prepareSet;
    int32_t maxBlock;
    alignas(8) double sampleRate;
    // in
    int32_t noteCount;
    int32_t paramCount;
    Note notes[MAX_NOTES];
    ParamChange params[MAX_PARAM_CHANGES];
    // out
    int32_t holding;
    int32_t latency;
    float in[2][MAX_FRAMES];
    float sidechain[2][MAX_FRAMES];
    float out[2][MAX_FRAMES];
};

// (the same layout in Tiny DAW, the 64-bit host and the 32-bit one, which share it)
static_assert(sizeof(ParamChange) == 16 && offsetof(ParamChange, value) == 8, "ParamChange");
static_assert(offsetof(AudioBlock, tempo) == 48 && offsetof(AudioBlock, position) == 56, "AudioBlock");
static_assert(offsetof(AudioBlock, beats) == 64, "AudioBlock");
static_assert(offsetof(AudioBlock, sampleRate) == 80 && offsetof(AudioBlock, notes) == 96, "AudioBlock");
static_assert(sizeof(Note) == 28, "Note");
static_assert(offsetof(AudioBlock, params) == 28768 && offsetof(AudioBlock, in) == 45160, "AudioBlock");
static_assert(sizeof(AudioBlock) == 241768, "AudioBlock");

inline long futex(std::atomic<uint32_t>* word, int op, uint32_t value, const timespec* timeout = nullptr) {
    static_assert(sizeof(std::atomic<uint32_t>) == 4, "a futex is 32 bits");
#ifdef TINYDAW_WINE_PE
    return unixside::futex(word, op, value, timeout);
#else
    return ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), op, value, timeout, nullptr, 0);
#endif
}

inline void wake(std::atomic<uint32_t>* word) { futex(word, FUTEX_WAKE, INT_MAX); }

// waits while *word == value, up to `seconds` (< 0: forever); true if it changed
inline bool waitChange(std::atomic<uint32_t>* word, uint32_t value, double seconds) {
    timespec end{};
    if (seconds >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        long ns = end.tv_nsec + (long)((seconds - (long)seconds) * 1e9);
        end.tv_sec += (long)seconds + ns / 1000000000L;
        end.tv_nsec = ns % 1000000000L;
    }
    while (word->load(std::memory_order_acquire) == value) {
        timespec left{}, *wait = nullptr;
        if (seconds >= 0) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            long s = end.tv_sec - now.tv_sec, ns = end.tv_nsec - now.tv_nsec;
            if (ns < 0)
                s--, ns += 1000000000L;
            if (s < 0)
                return false;
            left = {s, ns};
            wait = &left;
        }
        futex(word, FUTEX_WAIT, value, wait);
    }
    return true;
}

}  // namespace bridge
