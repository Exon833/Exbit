// JACK (also PipeWire's JACK, pipewire-jack): the "JACK" driver type. libjack.so.0 is loaded with
// dlopen on first use (jack_api.h declares what we call), never linked.
//
// A JACK "device" is a JACK client owning physical audio ports, as PortAudio's JACK host API lists
// them: with jackd that is "system" (the server's sound card); with PipeWire each sound card's
// sink/source ("Built-in Audio Analog Stereo"…). Its outputs are the device's playback ports, its
// inputs its capture ports (a sink's monitor ports aren't counted). A stream is a JACK client of
// our own ("Tiny DAW") with one port per channel ("out_1"…, "in_1"…), connected to the device's
// ports when it starts. The sample rate and buffer size are the server's: what StreamConfig asks
// for there is ignored (the blocksize asked for only stands in for an impossible reported delay).
//
// Latency: PipeWire's JACK reports a playback latency of 2^32 frames (89478 s at 48 kHz) where it
// has none to tell; jackd reports 0 before the graph is set up. A reported delay outside 0-10 s, or
// 0, is replaced by the one asked for (the blocksize, else one server buffer), as the successor's
// engines do (MAX_PLAUSIBLE_LATENCY_S).
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "audio/devices.h"
#include "audio/jack_api.h"

namespace jack {
namespace {

std::mutex g_apiMutex;
const Api* g_testApi = nullptr;
bool g_loaded = false;
Api g_api{};
const Api* g_libApi = nullptr;

void quiet(const char*) {}

template <class F>
bool sym(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}

const Api* loadLibjack() {
    void* lib = dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return nullptr;  // (kept loaded for good when it is there: JACK's threads may outlive a client)
    Api a{};
    bool ok = sym(lib, "jack_client_open", a.client_open) && sym(lib, "jack_client_close", a.client_close) &&
              sym(lib, "jack_get_client_name", a.get_client_name) && sym(lib, "jack_activate", a.activate) &&
              sym(lib, "jack_deactivate", a.deactivate) &&
              sym(lib, "jack_set_process_callback", a.set_process_callback) &&
              sym(lib, "jack_set_xrun_callback", a.set_xrun_callback) && sym(lib, "jack_on_shutdown", a.on_shutdown) &&
              sym(lib, "jack_set_buffer_size_callback", a.set_buffer_size_callback) &&
              sym(lib, "jack_get_sample_rate", a.get_sample_rate) &&
              sym(lib, "jack_get_buffer_size", a.get_buffer_size) && sym(lib, "jack_port_register", a.port_register) &&
              sym(lib, "jack_port_unregister", a.port_unregister) &&
              sym(lib, "jack_port_get_buffer", a.port_get_buffer) && sym(lib, "jack_port_name", a.port_name) &&
              sym(lib, "jack_connect", a.connect) && sym(lib, "jack_get_ports", a.get_ports) &&
              sym(lib, "jack_port_get_latency_range", a.port_get_latency_range) &&
              sym(lib, "jack_set_error_function", a.set_error_function);
    if (!ok) return nullptr;
    sym(lib, "jack_on_info_shutdown", a.on_info_shutdown);  // (optional)
    sym(lib, "jack_set_info_function", a.set_info_function);
    // (MIDI and the transport: midi_jack.cpp; optional)
    sym(lib, "jack_midi_get_event_count", a.midi_get_event_count);
    sym(lib, "jack_midi_event_get", a.midi_event_get);
    sym(lib, "jack_midi_clear_buffer", a.midi_clear_buffer);
    sym(lib, "jack_midi_event_write", a.midi_event_write);
    sym(lib, "jack_last_frame_time", a.last_frame_time);
    sym(lib, "jack_frames_to_time", a.frames_to_time);
    sym(lib, "jack_get_time", a.get_time);
    sym(lib, "jack_transport_query", a.transport_query);
    sym(lib, "jack_transport_start", a.transport_start);
    sym(lib, "jack_transport_stop", a.transport_stop);
    sym(lib, "jack_transport_locate", a.transport_locate);
    sym(lib, "jack_set_timebase_callback", a.set_timebase_callback);
    sym(lib, "jack_release_timebase", a.release_timebase);
    if (!sym(lib, "jack_free", a.free_)) a.free_ = free;    // (very old libjack: plain free)
    a.set_error_function(quiet);
    if (a.set_info_function) a.set_info_function(quiet);
    g_api = a;
    return &g_api;
}

}  // namespace

const Api* api() {
    std::lock_guard<std::mutex> lock(g_apiMutex);
    if (g_testApi) return g_testApi;
    if (!g_loaded) {
        g_loaded = true;
        g_libApi = loadLibjack();
    }
    return g_libApi;
}

void setApiForTesting(const Api* a) {
    std::lock_guard<std::mutex> lock(g_apiMutex);
    g_testApi = a;
}

}  // namespace jack

namespace audio {
namespace detail {
namespace {

constexpr const char* CLIENT_NAME = "Tiny DAW";
constexpr double MAX_PLAUSIBLE_LATENCY_S = 10.0;
constexpr double SNAPSHOT_S = 1.0;  // the device list is looked at again after this long

double monoNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the physical audio ports of one JACK client, in JACK's order
struct Group {
    std::string client;
    std::vector<std::string> ports;
};

void addPort(std::vector<Group>& groups, const char* full) {
    std::string name = full;
    size_t colon = name.rfind(':');
    if (colon == std::string::npos) return;
    std::string client = name.substr(0, colon);
    for (auto& g : groups)
        if (g.client == client) {
            g.ports.push_back(name);
            return;
        }
    groups.push_back({client, {name}});
}

// physical audio ports grouped by client: playback ports (input = false) or capture ports (true)
std::vector<Group> physicalGroups(const jack::Api* a, jack::Client* c, bool input) {
    std::vector<Group> groups;
    unsigned long flags = jack::PortIsPhysical | (input ? jack::PortIsOutput : jack::PortIsInput);
    const char** ports = a->get_ports(c, nullptr, jack::DEFAULT_AUDIO_TYPE, flags);
    if (!ports) return groups;
    for (const char** p = ports; *p; p++) {
        if (input) {  // a sink's monitor ports (PipeWire) aren't an input to record
            const char* shortName = strrchr(*p, ':');
            if (shortName && strncmp(shortName + 1, "monitor_", 8) == 0) continue;
        }
        addPort(groups, *p);
    }
    a->free_(ports);
    // jackd's sound card first
    std::stable_sort(groups.begin(), groups.end(),
                     [](const Group& x, const Group& y) { return (x.client == "system") > (y.client == "system"); });
    return groups;
}

jack::Client* openClient(const jack::Api* a, const char* name) {
    int status = 0;
    return a->client_open(name, jack::NoStartServer, &status);
}

// what the server offers, looked at once a second at most (each look is a short-lived client)
struct Snapshot {
    bool server = false;
    int rate = 48000;
    int buffer = 1024;
    std::vector<Group> outs, ins;
    double at = -1e9;
    const jack::Api* api = nullptr;
};
std::mutex g_snapMutex;
Snapshot g_snap;

Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    const jack::Api* a = jack::api();
    double t = monoNow();
    if (a == g_snap.api && t - g_snap.at < SNAPSHOT_S) return g_snap;
    Snapshot s;
    s.api = a;
    s.at = t;
    if (a) {
        if (jack::Client* c = openClient(a, "Tiny DAW (devices)")) {
            s.server = true;
            s.rate = (int)a->get_sample_rate(c);
            s.buffer = (int)a->get_buffer_size(c);
            s.outs = physicalGroups(a, c, false);
            s.ins = physicalGroups(a, c, true);
            a->client_close(c);
        }
    }
    g_snap = s;
    return s;
}

DeviceInfo infoOf(const Group& g, const Snapshot& s, bool input) {
    DeviceInfo d;
    d.id = d.name = g.client;
    (input ? d.maxInputs : d.maxOutputs) = (int)g.ports.size();
    if (input) d.maxOutputs = 0;
    d.defaultSampleRate = s.rate;
    return d;
}

double plausible(double reported, double fallback) {
    return reported > 0.0 && reported <= MAX_PLAUSIBLE_LATENCY_S ? reported : fallback;
}

class JackStream : public Stream {
public:
    ~JackStream() override {
        stop();
        if (client_) api_->client_close(client_);
    }
    bool open(const StreamConfig& cfg, Callback cb, std::string& error);
    bool start() override;
    void stop() override;
    int sampleRate() const override { return rate_; }
    int outChannels() const override { return (int)outPorts_.size(); }
    int inChannels() const override { return (int)inPorts_.size(); }
    int blocksize() const override { return capacity_; }
    double outputLatency() const override { return latency(outPorts_, jack::PlaybackLatency); }
    double inputLatency() const override { return latency(inPorts_, jack::CaptureLatency); }
    std::string deviceName() const override { return name_; }

private:
    const jack::Api* api_ = nullptr;
    jack::Client* client_ = nullptr;
    std::vector<jack::Port*> outPorts_, inPorts_;
    std::vector<std::string> outTargets_, inSources_;  // the device's ports they connect to
    std::vector<float*> outBufs_;
    std::vector<const float*> inBufs_;
    std::vector<float> out_, in_;  // interleaved, capacity_ frames
    int rate_ = 48000, capacity_ = 1024;
    double asked_ = 0;  // the delay asked for (s): stands in for an impossible reported one
    std::string name_;
    Callback cb_;
    bool active_ = false;

    double latency(const std::vector<jack::Port*>& ports, int mode) const;
    static int process(jack::nframes_t frames, void* arg);
    static int xrun(void* arg);
    static void shutdown(void* arg);
    static void infoShutdown(int, const char*, void* arg) { shutdown(arg); }
    void run(int frames);
};

bool JackStream::open(const StreamConfig& cfg, Callback cb, std::string& error) {
    api_ = jack::api();
    if (!api_) {
        error = "JACK isn't installed (libjack.so.0 can't be loaded)";
        return false;
    }
    client_ = openClient(api_, CLIENT_NAME);
    if (!client_) {
        error = "No JACK server is running (start JACK, or PipeWire with pipewire-jack)";
        return false;
    }
    cb_ = std::move(cb);
    rate_ = std::max(1, (int)api_->get_sample_rate(client_));
    capacity_ = std::max(1, (int)api_->get_buffer_size(client_));
    asked_ = (cfg.blocksize > 0 ? cfg.blocksize : capacity_) / (double)rate_;
    auto findGroup = [&](const std::string& id, bool input, Group& out) {
        for (auto& g : physicalGroups(api_, client_, input))
            if (g.client == id) {
                out = g;
                return true;
            }
        return false;
    };
    auto addPorts = [&](const std::string& device, int asked, bool input) {
        Group g;
        if (!findGroup(device, input, g)) {
            error = std::string("No JACK ") + (input ? "input" : "output") + " device \"" + device + "\"";
            return false;
        }
        int n = asked > 0 ? asked : (int)g.ports.size();
        for (int i = 0; i < n; i++) {
            std::string name = (input ? "in_" : "out_") + std::to_string(i + 1);
            jack::Port* p = api_->port_register(client_, name.c_str(), jack::DEFAULT_AUDIO_TYPE,
                                                input ? jack::PortIsInput : jack::PortIsOutput, 0);
            if (!p) {
                error = "JACK won't make the port \"" + name + "\"";
                return false;
            }
            (input ? inPorts_ : outPorts_).push_back(p);
            if (i < (int)g.ports.size()) (input ? inSources_ : outTargets_).push_back(g.ports[i]);
        }
        return true;
    };
    if (!cfg.outputDevice.empty() && !addPorts(cfg.outputDevice, cfg.outChannels, false)) return false;
    if (!cfg.inputDevice.empty() && !addPorts(cfg.inputDevice, cfg.inChannels, true)) return false;
    name_ = cfg.outputDevice.empty() ? cfg.inputDevice : cfg.outputDevice;
    outBufs_.assign(outPorts_.size(), nullptr);
    inBufs_.assign(inPorts_.size(), nullptr);
    out_.assign((size_t)capacity_ * std::max<size_t>(1, outPorts_.size()), 0.f);
    in_.assign((size_t)capacity_ * std::max<size_t>(1, inPorts_.size()), 0.f);
    api_->set_process_callback(client_, process, this);
    api_->set_xrun_callback(client_, xrun, this);
    if (api_->on_info_shutdown)
        api_->on_info_shutdown(client_, infoShutdown, this);
    else
        api_->on_shutdown(client_, shutdown, this);
    return true;
}

bool JackStream::start() {
    if (active_) return true;
    if (lost) return false;
    underflows = 0;
    if (api_->activate(client_) != 0) return false;
    active_ = true;
    // (connections that fail, e.g. a port gone meanwhile, leave that channel unconnected)
    for (size_t i = 0; i < outTargets_.size(); i++)
        api_->connect(client_, api_->port_name(outPorts_[i]), outTargets_[i].c_str());
    for (size_t i = 0; i < inSources_.size(); i++)
        api_->connect(client_, inSources_[i].c_str(), api_->port_name(inPorts_[i]));
    return true;
}

void JackStream::stop() {
    if (!active_) return;
    active_ = false;
    if (!lost) api_->deactivate(client_);  // (waits for the process callback to finish)
}

double JackStream::latency(const std::vector<jack::Port*>& ports, int mode) const {
    if (ports.empty()) return 0;
    jack::LatencyRange r{0, 0};
    if (!lost) api_->port_get_latency_range(ports[0], mode, &r);
    return plausible((double)r.max / rate_, asked_);
}

int JackStream::process(jack::nframes_t frames, void* arg) {
    auto* s = static_cast<JackStream*>(arg);
    const jack::Api* a = s->api_;
    for (size_t c = 0; c < s->outPorts_.size(); c++) s->outBufs_[c] = (float*)a->port_get_buffer(s->outPorts_[c], frames);
    for (size_t c = 0; c < s->inPorts_.size(); c++)
        s->inBufs_[c] = (const float*)a->port_get_buffer(s->inPorts_[c], frames);
    // the server's buffer may have grown since we opened: our callback takes at most capacity_ frames
    int done = 0;
    while (done < (int)frames) {
        int n = std::min(s->capacity_, (int)frames - done);
        s->run(n);
        int oc = (int)s->outPorts_.size();
        for (int c = 0; c < oc; c++) {
            float* dst = s->outBufs_[c];
            if (!dst) continue;
            dst += done;
            const float* src = s->out_.data() + c;
            for (int i = 0; i < n; i++) dst[i] = src[(size_t)i * oc];
        }
        done += n;
        for (auto& b : s->inBufs_)
            if (b) b += n;
    }
    return 0;
}

// one callback of n frames: the inputs' next n frames (inBufs_) interleaved in, out_ filled
void JackStream::run(int n) {
    int oc = (int)outPorts_.size(), ic = (int)inPorts_.size();
    for (int c = 0; c < ic; c++) {
        const float* src = inBufs_[c];
        float* dst = in_.data() + c;
        if (src)
            for (int i = 0; i < n; i++) dst[(size_t)i * ic] = src[i];
        else
            for (int i = 0; i < n; i++) dst[(size_t)i * ic] = 0.f;
    }
    std::fill(out_.begin(), out_.begin() + (size_t)n * std::max(1, oc), 0.f);
    try {
        cb_(ic ? in_.data() : nullptr, out_.data(), n);
    } catch (...) {
        std::fill(out_.begin(), out_.begin() + (size_t)n * std::max(1, oc), 0.f);
    }
}

int JackStream::xrun(void* arg) {
    static_cast<JackStream*>(arg)->underflows++;
    return 0;
}

void JackStream::shutdown(void* arg) {
    // the server quit (or threw us out): no JACK calls any more but closing the client
    static_cast<JackStream*>(arg)->lost = true;
}

}  // namespace

bool jackAvailable() { return snapshot().server; }

std::vector<DeviceInfo> jackDevices(bool input) {
    Snapshot s = snapshot();
    std::vector<DeviceInfo> out;
    for (auto& g : input ? s.ins : s.outs) out.push_back(infoOf(g, s, input));
    return out;
}

bool jackDeviceInfo(const std::string& id, DeviceInfo& out, bool input) {
    Snapshot s = snapshot();
    for (auto& g : input ? s.ins : s.outs)
        if (g.client == id) {
            out = infoOf(g, s, input);
            return true;
        }
    return false;
}

std::string jackDefault(bool input) {
    Snapshot s = snapshot();
    auto& groups = input ? s.ins : s.outs;
    return groups.empty() ? "" : groups[0].client;
}

std::unique_ptr<Stream> jackOpen(const StreamConfig& cfg, Callback cb, std::string& error) {
    std::unique_ptr<JackStream> s(new JackStream());
    if (!s->open(cfg, std::move(cb), error)) return nullptr;
    return s;
}

}  // namespace detail
}  // namespace audio
