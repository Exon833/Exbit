// MIDI keyboard input on the ALSA sequencer (libasound), and the sustain pedal (midi_input.py).
#include "audio/midi_in.h"

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <regex>

namespace midi_in {

namespace {

double monoNow() {  // plat::now()'s clock
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void quietAlsa(const char*, int, const char*, int, const char*, ...) {}

snd_seq_t* openSeq(int mode) {
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

std::vector<PortInfo> listPorts(snd_seq_t* seq) {
    std::vector<PortInfo> out;
    int self = snd_seq_client_id(seq);
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;  // ourselves; "System" (timer, announce)
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            unsigned type = snd_seq_port_info_get_type(pinfo);
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                    (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ) ||
                (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
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

std::vector<PortInfo> ports() {
    snd_seq_t* seq = openSeq(SND_SEQ_OPEN_INPUT);
    if (!seq) return {};
    auto out = listPorts(seq);
    snd_seq_close(seq);
    return out;
}

std::string stableName(const std::string& port) {
    static const std::regex numbers(R"(\s+\d+:\d+$)");
    return std::regex_replace(port, numbers, "");
}

// --- Input ---

struct Input::Seq {
    snd_seq_t* seq = nullptr;
    int port = -1;
    int srcClient = -1, srcPort = -1;
    int wake[2] = {-1, -1};
    ~Seq() {
        if (seq) snd_seq_close(seq);
        for (int fd : wake)
            if (fd >= 0) ::close(fd);
    }
};

Input::Input() : ring_(RING) {}

Input::~Input() { close(); }

bool Input::open(const std::string& name) {
    close();
    name_ = name;
    lost_ = false;
    if (name.empty()) return true;
    auto s = std::make_unique<Seq>();
    s->seq = openSeq(SND_SEQ_OPEN_INPUT);
    if (!s->seq) return false;
    const PortInfo* found = nullptr;
    auto list = listPorts(s->seq);
    for (auto& p : list)
        if (p.stable == name || p.name == name) {
            found = &p;
            break;
        }
    if (!found) return false;
    snd_seq_set_client_name(s->seq, "Tiny DAW");
    s->port = snd_seq_create_simple_port(s->seq, "MIDI in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (s->port < 0) return false;
    if (snd_seq_connect_from(s->seq, s->port, found->client, found->port) < 0) return false;
    // the system's announcements: tells us when the keyboard goes away
    snd_seq_connect_from(s->seq, s->port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    s->srcClient = found->client;
    s->srcPort = found->port;
    if (pipe2(s->wake, O_NONBLOCK | O_CLOEXEC) < 0) return false;
    seq_ = std::move(s);
    connected_ = true;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void Input::close() {
    if (thread_.joinable()) {
        running_ = false;
        char c = 1;
        ssize_t w = ::write(seq_->wake[1], &c, 1);
        (void)w;
        thread_.join();
    }
    seq_.reset();
    connected_ = false;
}

size_t Input::poll(std::vector<Event>& out) {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint64_t h = head_.load(std::memory_order_acquire);
    size_t n = 0;
    for (; t != h; t++, n++) out.push_back(ring_[t % RING]);
    tail_.store(t, std::memory_order_release);
    return n;
}

void Input::deliver(const Event& e) {
    if (callback_) callback_(e);
    if (!queue_) return;
    uint64_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= RING) {
        dropped_++;
        return;
    }
    ring_[h % RING] = e;
    head_.store(h + 1, std::memory_order_release);
}

void Input::loop() {
    snd_seq_t* seq = seq_->seq;
    int n = snd_seq_poll_descriptors_count(seq, POLLIN);
    std::vector<pollfd> fds((size_t)n + 1);
    snd_seq_poll_descriptors(seq, fds.data(), (unsigned)n, POLLIN);
    fds[(size_t)n] = {seq_->wake[0], POLLIN, 0};
    while (running_) {
        for (auto& f : fds) f.revents = 0;
        int r = ::poll(fds.data(), fds.size(), 500);
        if (r < 0 && errno != EINTR) break;
        if (!running_) break;
        bool any = false;
        for (;;) {
            snd_seq_event_t* ev = nullptr;
            int got = snd_seq_event_input(seq, &ev);
            if (got == -EAGAIN || got == -EWOULDBLOCK) break;
            if (got == -ENOSPC) continue;  // (the input buffer overflowed: some events were lost)
            if (got < 0 || !ev) break;
            double now = monoNow();
            Event e;
            e.time = now;
            bool keep = true;
            switch (ev->type) {
                case SND_SEQ_EVENT_NOTEON:
                    e = Event::noteOn(ev->data.note.note & 127, ev->data.note.velocity & 127, ev->data.note.channel & 15,
                                      now);
                    break;
                case SND_SEQ_EVENT_NOTEOFF:
                    e = Event::noteOff(ev->data.note.note & 127, ev->data.note.channel & 15, now);
                    break;
                case SND_SEQ_EVENT_CONTROLLER:
                    e = Event::control((int)(ev->data.control.param & 127), (int)(ev->data.control.value & 127),
                                       ev->data.control.channel & 15, now);
                    break;
                case SND_SEQ_EVENT_PITCHBEND:
                    e.type = Event::PitchBend;
                    e.channel = ev->data.control.channel & 15;
                    e.bend = (int16_t)std::max(-8192, std::min(8191, (int)ev->data.control.value));
                    break;
                case SND_SEQ_EVENT_PORT_EXIT:
                case SND_SEQ_EVENT_CLIENT_EXIT:
                    if (ev->data.addr.client == seq_->srcClient &&
                        (ev->type == SND_SEQ_EVENT_CLIENT_EXIT || ev->data.addr.port == seq_->srcPort))
                        lost_ = true;
                    keep = false;
                    break;
                default:
                    keep = false;
            }
            if (keep) {
                deliver(e);
                any = true;
            }
        }
        if (any && notify_) notify_();
    }
}

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
