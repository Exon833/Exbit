// MIDI ports on the ALSA sequencer (libasound): listed, listened to and played into (midi_ports.h), and the
// sustain pedal (midi_input.py). On Windows only the events and the pedal are here (the ports: midi_win.cpp).
#include "audio/midi_in.h"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

#ifndef _WIN32
#include "audio/alsa_api.h"
#endif
#include "audio/midi_ports.h"
#include "core/util.h"

namespace midi_in {
#ifndef _WIN32
TINYDAW_ALSA(TINYDAW_ALSA_USE)  // (loaded by openSeq, which everything here starts from)

namespace {

double monoNow() {  // plat::now()'s clock
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void quietAlsa(const char*, int, const char*, int, const char*, ...) {}

snd_seq_t* openSeq(int mode) {
    std::string why;
    if (!alsa::load(why)) return nullptr;  // (no ALSA: no MIDI inputs)
    snd_lib_error_set_handler(quietAlsa);  // (no "ALSA lib seq_hw.c: … No such file" on stderr)
    snd_seq_t* seq = nullptr;
    int r = snd_seq_open(&seq, "default", mode, SND_SEQ_NONBLOCK);
    snd_lib_error_set_handler(nullptr);
    return r < 0 ? nullptr : seq;
}

std::string portName(snd_seq_client_info_t* c, snd_seq_port_info_t* p) {
    // as RtMidi (mido's backend) names them, so the Python program's remembered names still match
    return std::string(snd_seq_client_info_get_name(c)) + ":" + snd_seq_port_info_get_name(p) + " " +
           std::to_string(snd_seq_port_info_get_client(p)) + ":" + std::to_string(snd_seq_port_info_get_port(p));
}

constexpr const char* OWN_CLIENT = "Tiny DAW";  // our own clients (any Tiny DAW's): never offered

// the inputs (readable ports) or outputs (writable ports) of the other clients
std::vector<PortInfo> listPorts(snd_seq_t* seq, bool outputs = false) {
    std::vector<PortInfo> out;
    int self = snd_seq_client_id(seq);
    const unsigned need = outputs ? SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE
                                  : SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;  // ourselves; "System" (timer, announce)
        if (std::string(snd_seq_client_info_get_name(cinfo)) == OWN_CLIENT) continue;
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            unsigned type = snd_seq_port_info_get_type(pinfo);
            if ((caps & need) != need || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
                continue;
            if (!(type & (SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_APPLICATION)))
                continue;
            PortInfo p;
            p.name = portName(cinfo, pinfo);
            p.stable = stableName(p.name);
            p.client = client;
            p.port = snd_seq_port_info_get_port(pinfo);
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace
#endif

Event Event::noteOn(int note, int velocity, int channel, double time) {
    Event e;
    e.type = velocity > 0 ? NoteOn : NoteOff;
    e.data1 = (uint8_t)note;
    e.data2 = (uint8_t)velocity;
    e.channel = (uint8_t)channel;
    e.time = time;
    return e;
}

Event Event::noteOff(int note, int channel, double time) {
    Event e;
    e.type = NoteOff;
    e.data1 = (uint8_t)note;
    e.channel = (uint8_t)channel;
    e.time = time;
    return e;
}

Event Event::control(int number, int value, int channel, double time) {
    Event e;
    e.type = Control;
    e.data1 = (uint8_t)number;
    e.data2 = (uint8_t)value;
    e.channel = (uint8_t)channel;
    e.time = time;
    return e;
}

#ifndef _WIN32  // (Windows: midi_win.cpp)
std::vector<PortInfo> ports() {
    snd_seq_t* seq = openSeq(SND_SEQ_OPEN_INPUT);
    if (!seq) return {};
    auto out = listPorts(seq);
    snd_seq_close(seq);
    return out;
}

std::vector<PortInfo> outputs() {
    snd_seq_t* seq = openSeq(SND_SEQ_OPEN_OUTPUT);
    if (!seq) return {};
    auto out = listPorts(seq, true);
    snd_seq_close(seq);
    return out;
}
#endif

std::string stableName(const std::string& port) {
    static const std::regex numbers(R"(\s+\d+:\d+$)");
    return std::regex_replace(port, numbers, "");
}

#ifndef _WIN32
// --- the ALSA sequencer's ports for midi_ports::Hub ---

namespace {

// One sequencer client per direction (each handle used by one thread: the reader's, the sender's), both "Tiny DAW":
// "MIDI in" is subscribed to every input chosen (events tell their source apart), and messages go from "MIDI out"
// straight to each output chosen (addressed: no subscription needed). The reader also hears the system's
// announcements: a port came or went.
class AlsaBackend : public midi_ports::Backend {
public:
    ~AlsaBackend() override {
        if (thread_.joinable()) {
            running_ = false;
            char c = 1;
            ssize_t w = ::write(wake_[1], &c, 1);
            (void)w;
            thread_.join();
        }
        if (in_) snd_seq_close(in_);
        if (out_) snd_seq_close(out_);
        for (int fd : wake_)
            if (fd >= 0) ::close(fd);
    }
    bool start() {
        in_ = openSeq(SND_SEQ_OPEN_INPUT);
        out_ = openSeq(SND_SEQ_OPEN_OUTPUT);
        if (!in_ || !out_) return false;
        snd_seq_set_client_name(in_, OWN_CLIENT);
        snd_seq_set_client_name(out_, OWN_CLIENT);
        inPort_ = snd_seq_create_simple_port(in_, "MIDI in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        outPort_ = snd_seq_create_simple_port(out_, "MIDI out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                              SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (inPort_ < 0 || outPort_ < 0) return false;
        snd_seq_connect_from(in_, inPort_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
        if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) < 0) return false;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    const char* name() const override { return "ALSA"; }
    void list(std::vector<midi_ports::Found>& out) override {
        for (bool outputs : {false, true})
            for (const PortInfo& p : listPorts(out_, outputs)) {
                midi_ports::Found f;
                f.key = p.stable;
                f.input = !outputs;
                f.client = p.client;
                f.port = p.port;
                f.full = p.name;
                out.push_back(f);
            }
    }
    bool open(const midi_ports::Found& p, int slot) override {
        std::lock_guard<std::mutex> g(lock_);
        if (p.input) {
            if (snd_seq_connect_from(in_, inPort_, p.client, p.port) < 0) return false;
            sources_[{p.client, p.port}] = slot;
        } else {
            dests_[slot] = {p.client, p.port};
        }
        return true;
    }
    void close(int slot) override {
        std::lock_guard<std::mutex> g(lock_);
        for (auto it = sources_.begin(); it != sources_.end(); ++it)
            if (it->second == slot) {
                snd_seq_disconnect_from(in_, inPort_, it->first.first, it->first.second);
                sources_.erase(it);
                return;
            }
        dests_.erase(slot);
    }
    void send(int slot, const uint8_t* b, int n, double) override {
        std::pair<int, int> to;
        {
            std::lock_guard<std::mutex> g(lock_);
            auto it = dests_.find(slot);
            if (it == dests_.end()) return;
            to = it->second;
        }
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, outPort_);
        snd_seq_ev_set_dest(&ev, to.first, to.second);
        snd_seq_ev_set_direct(&ev);
        uint8_t status = b[0], ch = status & 15;
        switch (status & 0xF0) {
            case 0x90: snd_seq_ev_set_noteon(&ev, ch, b[1] & 127, b[2] & 127); break;
            case 0x80: snd_seq_ev_set_noteoff(&ev, ch, b[1] & 127, b[2] & 127); break;
            case 0xB0: snd_seq_ev_set_controller(&ev, ch, b[1] & 127, b[2] & 127); break;
            case 0xE0: snd_seq_ev_set_pitchbend(&ev, ch, ((b[1] & 127) | ((b[2] & 127) << 7)) - 8192); break;
            case 0xF0:
                if (status == 0xF8) ev.type = SND_SEQ_EVENT_CLOCK;
                else if (status == 0xFA) ev.type = SND_SEQ_EVENT_START;
                else if (status == 0xFB) ev.type = SND_SEQ_EVENT_CONTINUE;
                else if (status == 0xFC) ev.type = SND_SEQ_EVENT_STOP;
                else if (status == 0xF2 && n >= 3) {
                    ev.type = SND_SEQ_EVENT_SONGPOS;
                    ev.data.control.value = (b[1] & 127) | ((b[2] & 127) << 7);
                } else {
                    return;
                }
                break;
            default: return;
        }
        snd_seq_event_output_direct(out_, &ev);
    }

private:
    snd_seq_t *in_ = nullptr, *out_ = nullptr;
    int inPort_ = -1, outPort_ = -1;
    int wake_[2] = {-1, -1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex lock_;
    std::map<std::pair<int, int>, int> sources_;  // (client, port) -> slot
    std::map<int, std::pair<int, int>> dests_;

    void loop() {
        int n = snd_seq_poll_descriptors_count(in_, POLLIN);
        std::vector<pollfd> fds((size_t)n + 1);
        snd_seq_poll_descriptors(in_, fds.data(), (unsigned)n, POLLIN);
        fds[(size_t)n] = {wake_[0], POLLIN, 0};
        while (running_) {
            for (auto& f : fds) f.revents = 0;
            int r = ::poll(fds.data(), fds.size(), 500);
            if (r < 0 && errno != EINTR) break;
            if (!running_) break;
            for (;;) {
                snd_seq_event_t* ev = nullptr;
                int got = snd_seq_event_input(in_, &ev);
                if (got == -EAGAIN || got == -EWOULDBLOCK) break;
                if (got == -ENOSPC) continue;  // (the input buffer overflowed: some events were lost)
                if (got < 0 || !ev) break;
                double now = monoNow();
                switch (ev->type) {
                    case SND_SEQ_EVENT_PORT_START:
                    case SND_SEQ_EVENT_PORT_EXIT:
                    case SND_SEQ_EVENT_CLIENT_START:
                    case SND_SEQ_EVENT_CLIENT_EXIT:
                        changed_ = true;  // (the Hub looks again)
                        continue;
                    default: break;
                }
                int slot = -1;
                {
                    std::lock_guard<std::mutex> g(lock_);
                    auto it = sources_.find({ev->source.client, ev->source.port});
                    if (it != sources_.end()) slot = it->second;
                }
                if (slot < 0) continue;
                uint8_t b[3] = {0, 0, 0};
                int len = 3;
                switch (ev->type) {
                    case SND_SEQ_EVENT_NOTEON:
                        b[0] = (uint8_t)(0x90 | (ev->data.note.channel & 15)), b[1] = ev->data.note.note & 127,
                        b[2] = ev->data.note.velocity & 127;
                        break;
                    case SND_SEQ_EVENT_NOTEOFF:
                        b[0] = (uint8_t)(0x80 | (ev->data.note.channel & 15)), b[1] = ev->data.note.note & 127;
                        break;
                    case SND_SEQ_EVENT_CONTROLLER:
                        b[0] = (uint8_t)(0xB0 | (ev->data.control.channel & 15)),
                        b[1] = (uint8_t)(ev->data.control.param & 127), b[2] = (uint8_t)(ev->data.control.value & 127);
                        break;
                    case SND_SEQ_EVENT_PITCHBEND: {
                        int v = std::max(0, std::min(16383, (int)ev->data.control.value + 8192));
                        b[0] = (uint8_t)(0xE0 | (ev->data.control.channel & 15)), b[1] = v & 127, b[2] = (uint8_t)(v >> 7);
                        break;
                    }
                    case SND_SEQ_EVENT_SONGPOS: {
                        int v = std::max(0, std::min(16383, (int)ev->data.control.value));
                        b[0] = 0xF2, b[1] = v & 127, b[2] = (uint8_t)(v >> 7);
                        break;
                    }
                    case SND_SEQ_EVENT_CLOCK: b[0] = 0xF8, len = 1; break;
                    case SND_SEQ_EVENT_START: b[0] = 0xFA, len = 1; break;
                    case SND_SEQ_EVENT_CONTINUE: b[0] = 0xFB, len = 1; break;
                    case SND_SEQ_EVENT_STOP: b[0] = 0xFC, len = 1; break;
                    default: len = 0;
                }
                if (len) deliverBytes(slot, b, len, now);
            }
        }
    }
};

}  // namespace

}  // namespace midi_in

std::unique_ptr<midi_ports::Backend> midi_ports::alsaBackend() {
    auto b = std::make_unique<midi_in::AlsaBackend>();
    if (!b->start()) {
        util::debugf("midi: no ALSA sequencer (no ALSA MIDI ports)\n");
        return nullptr;
    }
    return b;
}
#else
}  // namespace midi_in (Windows: its ports are midi_win.cpp's)
#endif

namespace midi_in {


// --- Sustain ---

void Sustain::handle(const Event& e, std::vector<Event>& out) {
    if (e.type == Event::Control && e.data1 == SUSTAIN) {
        bool down = e.data2 >= 64;
        if (down)
            pedal_ = true;
        else
            pedalUp(out, e.time);
        return;
    }
    if (e.type == Event::Control && (e.data1 == ALL_NOTES_OFF || e.data1 == ALL_SOUND_OFF)) {
        releaseAll(out, e.time);
        out.push_back(e);
        return;
    }
    if (e.type == Event::NoteOff && pedal_ && sounding_.count(e.data1)) {
        sustained_.insert(e.data1);  // (it goes on sounding while the pedal is down)
        return;
    }
    if (e.type == Event::NoteOn && sustained_.count(e.data1)) {  // struck again while the pedal holds it
        sustained_.erase(e.data1);
        out.push_back(Event::noteOff(e.data1, channel_[e.data1], e.time));
    }
    if (e.type == Event::NoteOn) {
        sounding_.insert(e.data1);
        channel_[e.data1] = e.channel;
    } else if (e.type == Event::NoteOff) {
        sounding_.erase(e.data1);
    }
    out.push_back(e);
}

void Sustain::pedalUp(std::vector<Event>& out, double time) {
    pedal_ = false;
    for (int note : sustained_) {  // every note it was holding stops now
        sounding_.erase(note);
        out.push_back(Event::noteOff(note, channel_[note], time));
    }
    sustained_.clear();
}

void Sustain::releaseAll(std::vector<Event>& out, double time) {
    pedal_ = false;
    sustained_.clear();
    for (int note : sounding_) out.push_back(Event::noteOff(note, channel_[note], time));
    sounding_.clear();
}

}  // namespace midi_in
