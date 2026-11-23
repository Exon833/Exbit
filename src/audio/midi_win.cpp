// Windows' MIDI ports (WinMM: midiIn* / midiOut*) for midi_ports::Hub, the port lists of midi_in.h, and the output
// thread's waking and waiting (midi_out::Out) on Windows. (Linux: midi_in.cpp's ALSA sequencer, midi_jack.cpp.)
//
// Ports by their names (the device's own, as Windows shows it; a second one of the same name gets " (2)", and so
// on): remembered across sessions as ALSA's stable names are. A WinMM device is opened by one program at a time:
// one in use elsewhere "can't be opened" (the Hub says so) and is tried again when it is switched on again. WinMM
// announces nothing: ports that come or go are seen at the Hub's next look (RESCAN_S), and a device that went away
// while open is closed then. What arrives is stamped with the moment the driver gives (milliseconds since the port
// was started, on plat::now()'s clock), never later than when it reached us. Messages go out as they are sent
// (midiOutShortMsg: the Hub's output thread waits for each one's moment, on a high-resolution waitable timer).
#include <windows.h>
#include <mmsystem.h>
#include <time.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "audio/midi_in.h"
#include "audio/midi_out.h"
#include "audio/midi_ports.h"
#include "core/util.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

double monoNow() {  // plat::now()'s clock (QueryPerformanceCounter's, as steady_clock)
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// the devices' names, made unique: "Name", "Name (2)"… (their index is PortInfo::client)
std::vector<midi_in::PortInfo> listDevices(bool outputs) {
    std::vector<midi_in::PortInfo> out;
    std::map<std::string, int> seen;
    UINT n = outputs ? midiOutGetNumDevs() : midiInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        std::string name;
        if (outputs) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            name = utf8(caps.szPname);
        } else {
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            name = utf8(caps.szPname);
        }
        if (name.empty()) name = (outputs ? "MIDI output " : "MIDI input ") + std::to_string(i + 1);
        int k = ++seen[name];
        if (k > 1) name += " (" + std::to_string(k) + ")";
        midi_in::PortInfo p;
        p.name = p.stable = name;
        p.client = (int)i;
        out.push_back(p);
    }
    return out;
}

// a message's length from its status byte (WinMM gives short messages packed in one DWORD)
int messageLength(uint8_t status) {
    if (status >= 0xF8) return 1;  // (real-time: clock, start, continue, stop…)
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0: return 2;
        case 0xF0: return status == 0xF2 ? 3 : status == 0xF1 || status == 0xF3 ? 2 : 1;
        default: return 3;
    }
}

class WinmmBackend : public midi_ports::Backend {
public:
    ~WinmmBackend() override {
        std::vector<int> slots;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto& [slot, in] : ins_) slots.push_back(slot);
            for (auto& [slot, h] : outs_) slots.push_back(slot);
        }
        for (int s : slots) close(s);
    }
    const char* name() const override { return "Windows MIDI"; }
    void list(std::vector<midi_ports::Found>& out) override {
        for (bool outputs : {false, true})
            for (const midi_in::PortInfo& p : listDevices(outputs)) {
                midi_ports::Found f;
                f.key = p.stable;
                f.input = !outputs;
                f.client = p.client;
                f.full = p.name;
                out.push_back(f);
            }
    }
    bool open(const midi_ports::Found& p, int slot) override {
        std::lock_guard<std::mutex> g(lock_);
        if (p.input) {
            auto in = std::make_unique<In>();
            in->self = this;
            in->slot = slot;
            MMRESULT r = midiInOpen(&in->h, (UINT)p.client, (DWORD_PTR)&WinmmBackend::inProc, (DWORD_PTR)in.get(),
                                    CALLBACK_FUNCTION);
            if (r != MMSYSERR_NOERROR) {
                util::debugf("midi: midiInOpen \"%s\": error %u%s\n", p.key.c_str(), r,
                             r == MMSYSERR_ALLOCATED ? " (in use by another program)" : "");
                return false;
            }
            in->start = monoNow();
            midiInStart(in->h);
            ins_[slot] = std::move(in);
        } else {
            HMIDIOUT h = nullptr;
            MMRESULT r = midiOutOpen(&h, (UINT)p.client, 0, 0, CALLBACK_NULL);
            if (r != MMSYSERR_NOERROR) {
                util::debugf("midi: midiOutOpen \"%s\": error %u%s\n", p.key.c_str(), r,
                             r == MMSYSERR_ALLOCATED ? " (in use by another program)" : "");
                return false;
            }
            outs_[slot] = h;
        }
        return true;
    }
    void close(int slot) override {
        std::unique_ptr<In> in;
        HMIDIOUT out = nullptr;
        {
            std::lock_guard<std::mutex> g(lock_);
            auto i = ins_.find(slot);
            if (i != ins_.end()) in = std::move(i->second), ins_.erase(i);
            auto o = outs_.find(slot);
            if (o != outs_.end()) out = o->second, outs_.erase(o);
        }
        if (in) {  // (after midiInClose its callback is never called again: `in` may go)
            midiInStop(in->h);
            midiInReset(in->h);
            midiInClose(in->h);
        }
        if (out) {
            midiOutReset(out);
            midiOutClose(out);
        }
    }
    void send(int slot, const uint8_t* b, int n, double) override {
        if (n < 1 || n > 3) return;
        std::lock_guard<std::mutex> g(lock_);
        auto o = outs_.find(slot);
        if (o == outs_.end()) return;
        DWORD msg = b[0] | (n > 1 ? (DWORD)b[1] << 8 : 0) | (n > 2 ? (DWORD)b[2] << 16 : 0);
        midiOutShortMsg(o->second, msg);
    }
    // (tests) what a driver's callback hands over, as if it came from input `slot` started at `start`
    void fake(int slot, DWORD_PTR packed, DWORD_PTR ms, double start) {
        In in;
        in.self = this, in.slot = slot, in.start = start;
        inProc(nullptr, MIM_DATA, (DWORD_PTR)&in, packed, ms);
    }

private:
    struct In {
        WinmmBackend* self = nullptr;
        HMIDIIN h = nullptr;
        int slot = -1;
        double start = 0;
    };
    std::mutex lock_;     // the open ports (GUI thread, the output thread)
    std::mutex deliver_;  // (WinMM may call back on more than one thread: the ring takes one at a time)
    std::map<int, std::unique_ptr<In>> ins_;
    std::map<int, HMIDIOUT> outs_;

    static void CALLBACK inProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
        if (msg != MIM_DATA) return;  // (MIM_LONGDATA, SysEx: not used; MIM_OPEN / MIM_CLOSE)
        In* in = (In*)inst;
        uint8_t b[3] = {(uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0x7F), (uint8_t)((p1 >> 16) & 0x7F)};
        if (b[0] < 0x80) return;  // (no status byte: WinMM resolves running status itself)
        double now = monoNow(), t = in->start + (double)p2 * 1e-3;
        if (!(t <= now && t > now - 1.0)) t = now;  // (a driver's stamp that can't be: when it reached us)
        std::lock_guard<std::mutex> g(in->self->deliver_);
        in->self->deliverBytes(in->slot, b, messageLength(b[0]), t);
    }
};

}  // namespace

// --- midi_in.h's port lists ---

namespace midi_in {
std::vector<PortInfo> ports() { return listDevices(false); }
std::vector<PortInfo> outputs() { return listDevices(true); }
}  // namespace midi_in

// --- midi_ports.h's backends ---

namespace midi_ports {
std::unique_ptr<Backend> winmmBackend() {
    auto b = std::make_unique<WinmmBackend>();
    util::debugf("midi: Windows MIDI: %u inputs, %u outputs\n", midiInGetNumDevs(), midiOutGetNumDevs());
    return b;
}
std::unique_ptr<Backend> jackBackend() { return nullptr; }  // (no JACK on Windows)
// (tests) a WinMM callback's message, through the backend's own parsing: `b` must be winmmBackend()'s
void winmmFake(Backend* b, int slot, uint32_t packed, uint32_t ms, double start) {
    static_cast<WinmmBackend*>(b)->fake(slot, packed, ms, start);
}
}  // namespace midi_ports

// --- midi_out::Out's waking (an auto-reset event) and waiting (a high-resolution waitable timer) ---

namespace midi_out {

void Out::openWake() {
    wake_ = (intptr_t)CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) {  // (before Windows 10 1803: an ordinary timer, at the 1 ms the system's timer then gives)
        t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        static bool once = (timeBeginPeriod(1), true);
        (void)once;
        util::debugf("midi: no high-resolution timer: 1 ms timing for MIDI out\n");
    }
    timer_ = (intptr_t)t;
}

void Out::closeWake() {
    if (wake_) CloseHandle((HANDLE)wake_);
    if (timer_) CloseHandle((HANDLE)timer_);
}

void Out::wake() {
    if (wake_) SetEvent((HANDLE)wake_);
}

void Out::wait(double seconds) {
    HANDLE ev = (HANDLE)wake_, tm = (HANDLE)timer_;
    if (!ev) return;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)(seconds * 1e7));  // (relative, in 100 ns)
    if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) {
        HANDLE both[2] = {ev, tm};
        WaitForMultipleObjects(2, both, FALSE, INFINITE);
    } else {
        WaitForSingleObject(ev, (DWORD)std::ceil(seconds * 1000));
    }
}

}  // namespace midi_out
