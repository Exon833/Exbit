// JACK MIDI ports and JACK's transport (midi_ports.h, midi_sync.h): a JACK client of its own, "Tiny DAW MIDI",
// made while a JACK server runs (jackd, or PipeWire's JACK) and only when something needs it (a JACK port chosen,
// Sync: JACK, the MIDI ports window listing them). libjack is loaded at run time (jack_api.h).
//
// Its process callback (JACK's real-time thread) reads its input ports (each message timed by its frame in the
// cycle) and writes its output ports: what the Hub's output thread handed over (a lock-free ring), each message at
// the frame of the next cycle its moment falls on (sample-accurate; one already late goes at the cycle's start).
// No allocation and no locks there. A port is unregistered only after the callback is known not to be using it.
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "audio/jack_api.h"
#include "audio/midi_out.h"
#include "audio/midi_ports.h"
#include "audio/midi_sync.h"
#include "core/spsc.h"
#include "core/util.h"

namespace midi_ports {
namespace {

constexpr int MAX_PORTS = 64;
constexpr int PENDING = 4096;
constexpr const char* CLIENT = "Tiny DAW MIDI";

double monoNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

class JackBackend : public Backend, public midi_sync::JackTransport {
public:
    JackBackend() : outQ_(8192) {}
    ~JackBackend() override {
        if (!c_) return;
        setMaster(nullptr);
        a_->deactivate(c_);
        a_->client_close(c_);
    }
    bool begin() {
        a_ = jack::api();
        if (!a_ || !a_->hasMidi() || !a_->last_frame_time || !a_->frames_to_time || !a_->get_time) return false;
        int status = 0;
        c_ = a_->client_open(CLIENT, jack::NoStartServer, &status);
        if (!c_) return false;
        rate_ = std::max(1, (int)a_->get_sample_rate(c_));
        a_->set_process_callback(c_, process, this);
        if (a_->activate(c_) != 0) {
            a_->client_close(c_);
            c_ = nullptr;
            return false;
        }
        return true;
    }
    const char* name() const override { return "JACK"; }
    void list(std::vector<Found>& out) override {
        for (bool input : {true, false}) {
            // (an input of ours reads a port that outputs MIDI, and the other way round)
            const char** names = a_->get_ports(c_, nullptr, jack::DEFAULT_MIDI_TYPE, input ? jack::PortIsOutput : jack::PortIsInput);
            if (!names) continue;
            for (const char** n = names; *n; n++) {
                if (std::strstr(*n, "Tiny DAW")) continue;  // (our own, and their bridges)
                Found f;
                f.key = std::string(JACK_PREFIX) + *n;
                f.input = input;
                f.full = *n;
                out.push_back(f);
            }
            a_->free_(names);
        }
    }
    bool open(const Found& f, int slot) override {
        for (auto& p : ports_) {
            if (p.port.load()) continue;
            std::string name = (f.input ? "in_" : "out_") + std::to_string(slot);
            jack::Port* port = a_->port_register(c_, name.c_str(), jack::DEFAULT_MIDI_TYPE,
                                                 f.input ? jack::PortIsInput : jack::PortIsOutput, 0);
            if (!port) return false;
            const char* mine = a_->port_name(port);
            int r = f.input ? a_->connect(c_, f.full.c_str(), mine) : a_->connect(c_, mine, f.full.c_str());
            if (r != 0 && r != 17 /* EEXIST */) {
                a_->port_unregister(c_, port);
                return false;
            }
            p.input = f.input;
            p.slot = slot;
            p.port = port;  // (the callback uses it from now on)
            return true;
        }
        return false;
    }
    void close(int slot) override {
        for (auto& p : ports_) {
            jack::Port* port = p.port.load();
            if (!port || p.slot != slot) continue;
            p.port = nullptr;
            while (inProcess_.load()) std::this_thread::yield();  // (the callback may hold it this very moment)
            a_->port_unregister(c_, port);
        }
    }
    void send(int slot, const uint8_t* b, int n, double when) override {
        midi_out::Event e;
        e.when = when;
        e.slot = (int16_t)slot;
        e.n = (uint8_t)std::min(n, 3);
        std::memcpy(e.b, b, e.n);
        outQ_.push(e);
    }
    bool timed() const override { return true; }

    // --- the transport ---
    bool query(midi_sync::JackState& out) override {
        if (!a_->hasTransport()) return false;
        jack::Position pos{};
        int state = a_->transport_query(c_, &pos);
        out.rolling = state == jack::TransportRolling || state == jack::TransportLooping;
        out.frame = pos.frame;
        out.rate = pos.frame_rate ? (int)pos.frame_rate : rate_;
        out.bbt = (pos.valid & jack::PositionBBT) && !master_;
        out.bpm = out.bbt ? pos.beats_per_minute : 0;
        return true;
    }
    void start() override {
        if (a_->transport_start) a_->transport_start(c_);
    }
    void stop() override {
        if (a_->transport_stop) a_->transport_stop(c_);
    }
    void locate(int64_t frame) override {
        if (a_->transport_locate) a_->transport_locate(c_, (jack::nframes_t)std::max<int64_t>(0, frame));
    }
    bool setMaster(const Timing* timing) override {
        if (!timing) {
            if (master_ && a_->release_timebase) a_->release_timebase(c_);
            master_ = false;
            timing_ = nullptr;
            return true;
        }
        timing_ = timing;
        if (master_) return true;
        if (!a_->set_timebase_callback) return false;
        master_ = a_->set_timebase_callback(c_, 1, timebase, this) == 0;  // (conditional: not over another app)
        util::debugf("sync: Tiny DAW %s JACK's timebase master\n", master_ ? "is" : "can't be (another app is)");
        return master_;
    }
    bool master() const override { return master_; }

private:
    struct Slot {
        std::atomic<jack::Port*> port{nullptr};
        int slot = -1;
        bool input = true;
    };
    const jack::Api* a_ = nullptr;
    jack::Client* c_ = nullptr;
    int rate_ = 48000;
    Slot ports_[MAX_PORTS];
    std::atomic<int> inProcess_{0};
    Spsc<midi_out::Event> outQ_;
    midi_out::Event pending_[PENDING];
    int pendingN_ = 0;
    bool master_ = false;
    std::atomic<const Timing*> timing_{nullptr};

    static void timebase(int, jack::nframes_t, jack::Position* pos, int, void* arg) {
        auto* self = static_cast<JackBackend*>(arg);
        const Timing* t = self->timing_.load();
        if (!t || !pos) return;
        TempoMapPtr map = t->audioMap();
        if (map) midi_sync::fillBBT(*map, pos->frame, pos->frame_rate ? pos->frame_rate : (uint32_t)self->rate_, *pos);
    }

    static int process(jack::nframes_t frames, void* arg) {
        auto* self = static_cast<JackBackend*>(arg);
        self->inProcess_++;
        self->run(frames);
        self->inProcess_--;
        return 0;
    }

    void run(jack::nframes_t frames) {
        const double offset = monoNow() - (double)a_->get_time() / 1e6;  // (JACK's clock to ours)
        const double cycle = (double)a_->frames_to_time(c_, a_->last_frame_time(c_)) / 1e6 + offset;
        for (auto& p : ports_) {
            jack::Port* port = p.port.load();
            if (!port) continue;
            void* buf = a_->port_get_buffer(port, frames);
            if (!buf) continue;
            if (p.input) {
                uint32_t n = a_->midi_get_event_count(buf);
                for (uint32_t i = 0; i < n; i++) {
                    jack::MidiEvent ev{};
                    if (a_->midi_event_get(&ev, buf, i) == 0 && ev.size >= 1)
                        deliverBytes(p.slot, ev.buffer, (int)std::min<size_t>(ev.size, 3), cycle + (double)ev.time / rate_);
                }
            } else {
                a_->midi_clear_buffer(buf);
            }
        }
        midi_out::Event e;
        while (pendingN_ < PENDING && outQ_.pop(e)) pending_[pendingN_++] = e;
        for (int i = 1; i < pendingN_; i++)  // (by moment, keeping their order otherwise: an insertion sort, no allocation)
            for (int j = i; j > 0 && pending_[j].when < pending_[j - 1].when; j--) std::swap(pending_[j], pending_[j - 1]);
        const double heard = cycle + (double)frames / rate_;  // (what this cycle writes comes out a period later)
        int kept = 0;
        for (int i = 0; i < pendingN_; i++) {
            const midi_out::Event& m = pending_[i];
            double at = (m.when - heard) * rate_;
            if (at >= frames) {  // (a later cycle's)
                pending_[kept++] = m;
                continue;
            }
            for (auto& p : ports_) {
                jack::Port* port = p.port.load();
                if (!port || p.input || p.slot != m.slot) continue;
                void* buf = a_->port_get_buffer(port, frames);
                if (buf) a_->midi_event_write(buf, (jack::nframes_t)std::max(0.0, at), m.b, m.n);
            }
        }
        pendingN_ = kept;
    }
};

}  // namespace

std::unique_ptr<Backend> jackBackend() {
    auto b = std::make_unique<JackBackend>();
    if (!b->begin()) {
        util::debugf("midi: no JACK server (no JACK MIDI ports or transport)\n");
        return nullptr;
    }
    util::debugf("midi: JACK client \"%s\" made (JACK MIDI ports, the transport)\n", CLIENT);
    return b;
}

}  // namespace midi_ports
