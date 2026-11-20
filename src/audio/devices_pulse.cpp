// PulseAudio (or PipeWire's PulseAudio server): the sinks and sources (libpulse's asynchronous
// API on a private main loop, briefly) and streams (libpulse-simple: a playback stream, and a
// record stream for inputs, driven by one thread). libpulse and libpulse-simple are loaded with dlopen on
// first use, never linked: the program starts where PulseAudio isn't installed (the type isn't offered).
#include <dlfcn.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "audio/devices.h"

namespace audio {
namespace detail {
void raiseThreadPriority();

namespace {

// the functions called, as pointers of the same names (they hide <pulse/*.h>'s declarations here)
#define TINYDAW_PULSE(F)                                                                                        \
    F(pa_context_connect) F(pa_context_disconnect) F(pa_context_get_server_info) F(pa_context_get_sink_info_list) \
    F(pa_context_get_source_info_list) F(pa_context_get_state) F(pa_context_new) F(pa_context_unref)            \
    F(pa_mainloop_dispatch) F(pa_mainloop_free) F(pa_mainloop_get_api) F(pa_mainloop_new) F(pa_mainloop_poll)  \
    F(pa_mainloop_prepare) F(pa_operation_unref) F(pa_simple_flush) F(pa_simple_free) F(pa_simple_get_latency)  \
    F(pa_simple_new) F(pa_simple_read) F(pa_simple_write) F(pa_strerror)
#define TINYDAW_PULSE_DEFINE(f) decltype(&::f) f = nullptr;
TINYDAW_PULSE(TINYDAW_PULSE_DEFINE)
#undef TINYDAW_PULSE_DEFINE

// both libraries and every function found (once)
bool pulseLoaded() {
    static bool ok = [] {
        void* libs[] = {dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL), dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL)};
        if (!libs[0] || !libs[1]) return false;
        auto find = [&](const char* name) {
            void* f = dlsym(libs[0], name);
            return f ? f : dlsym(libs[1], name);
        };
        bool all = true;
#define TINYDAW_PULSE_FIND(f) all = (f = reinterpret_cast<decltype(f)>(find(#f))) && all;
        TINYDAW_PULSE(TINYDAW_PULSE_FIND)
#undef TINYDAW_PULSE_FIND
        return all;
    }();
    return ok;
}

const char* kDefaultId = "default";  // the server's default sink / source (follows changes)

struct PaDevice {
    std::string name, description;
    int channels = 2;
    int rate = 48000;
};

struct PaState {
    bool ok = false;
    std::vector<PaDevice> sinks, sources;
    std::string defaultSink, defaultSource;
};

std::mutex g_mutex;
PaState g_cache;
std::chrono::steady_clock::time_point g_cachedAt;
bool g_haveCache = false;

struct Query {
    PaState st;
    int pending = 0;
    bool failed = false;
};

void onServer(pa_context*, const pa_server_info* i, void* u) {
    Query* q = (Query*)u;
    if (i) {
        q->st.defaultSink = i->default_sink_name ? i->default_sink_name : "";
        q->st.defaultSource = i->default_source_name ? i->default_source_name : "";
    }
    q->pending--;
}

void onSink(pa_context*, const pa_sink_info* i, int eol, void* u) {
    Query* q = (Query*)u;
    if (eol) {
        q->pending--;
        return;
    }
    if (!i) return;
    PaDevice d;
    d.name = i->name ? i->name : "";
    d.description = i->description ? i->description : d.name;
    d.channels = i->sample_spec.channels;
    d.rate = (int)i->sample_spec.rate;
    q->st.sinks.push_back(d);
}

void onSource(pa_context*, const pa_source_info* i, int eol, void* u) {
    Query* q = (Query*)u;
    if (eol) {
        q->pending--;
        return;
    }
    if (!i || i->monitor_of_sink != PA_INVALID_INDEX) return;  // "Monitor of …": not an input
    PaDevice d;
    d.name = i->name ? i->name : "";
    d.description = i->description ? i->description : d.name;
    d.channels = i->sample_spec.channels;
    d.rate = (int)i->sample_spec.rate;
    q->st.sources.push_back(d);
}

// asks the server for its sinks, sources and defaults (at most ~1.5 s; cached for 2 s)
PaState query() {
    if (!pulseLoaded()) return PaState();  // (not installed: not ok)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_haveCache && std::chrono::steady_clock::now() - g_cachedAt < std::chrono::seconds(2)) return g_cache;
    }
    Query q;
    pa_mainloop* ml = pa_mainloop_new();
    pa_context* c = ml ? pa_context_new(pa_mainloop_get_api(ml), "Tiny DAW") : nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    auto iterate = [&]() {
        if (pa_mainloop_prepare(ml, 50000) < 0) return false;
        if (pa_mainloop_poll(ml) < 0) return false;
        return pa_mainloop_dispatch(ml) >= 0;
    };
    if (c && pa_context_connect(c, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            pa_context_state_t s = pa_context_get_state(c);
            if (s == PA_CONTEXT_READY) {
                ready = true;
                break;
            }
            if (!PA_CONTEXT_IS_GOOD(s)) break;
            if (!iterate()) break;
        }
        if (ready) {
            q.pending = 3;
            pa_operation* ops[3] = {pa_context_get_server_info(c, onServer, &q),
                                    pa_context_get_sink_info_list(c, onSink, &q),
                                    pa_context_get_source_info_list(c, onSource, &q)};
            while (q.pending > 0 && std::chrono::steady_clock::now() < deadline)
                if (!iterate()) break;
            for (pa_operation* o : ops)
                if (o) pa_operation_unref(o);
            q.st.ok = q.pending <= 0;
        }
        pa_context_disconnect(c);
    }
    if (c) pa_context_unref(c);
    if (ml) pa_mainloop_free(ml);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache = q.st;
    g_cachedAt = std::chrono::steady_clock::now();
    g_haveCache = true;
    return q.st;
}

const PaDevice* findDevice(const PaState& st, const std::string& id, bool input) {
    const std::vector<PaDevice>& list = input ? st.sources : st.sinks;
    std::string name = id == kDefaultId ? (input ? st.defaultSource : st.defaultSink) : id;
    for (const PaDevice& d : list)
        if (d.name == name) return &d;
    return nullptr;
}

DeviceInfo toInfo(const PaDevice& d, bool input, const std::string& id, const std::string& shown) {
    DeviceInfo i;
    i.id = id;
    i.name = shown;
    i.maxOutputs = input ? 0 : d.channels;
    i.maxInputs = input ? d.channels : 0;
    i.defaultSampleRate = d.rate > 0 ? d.rate : 48000;
    return i;
}

class PulseStream : public Stream {
public:
    ~PulseStream() override {
        stop();
        if (play_) pa_simple_free(play_);
        if (rec_) pa_simple_free(rec_);
    }
    bool open(const StreamConfig& cfg, Callback cb, std::string& error);
    bool start() override;
    void stop() override;
    int sampleRate() const override { return rate_; }
    int outChannels() const override { return outCh_; }
    int inChannels() const override { return inCh_; }
    int blocksize() const override { return block_; }
    double outputLatency() const override {
        double measured = outLatency_.load();
        return play_ ? (measured > 0 ? measured : (double)block_ * 2 / rate_) : 0;
    }
    double inputLatency() const override { return rec_ ? (double)block_ / rate_ : 0; }
    std::string deviceName() const override { return name_; }

private:
    pa_simple* play_ = nullptr;
    pa_simple* rec_ = nullptr;
    int rate_ = 48000, outCh_ = 0, inCh_ = 0, block_ = 512;
    std::string name_;
    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<double> outLatency_{0};
    std::vector<float> out_, in_;
    void loop();
};

bool PulseStream::open(const StreamConfig& cfg, Callback cb, std::string& error) {
    cb_ = std::move(cb);
    PaState st = query();
    if (!st.ok) {
        error = pulseLoaded() ? "The sound server (PulseAudio / PipeWire) isn't running" : "PulseAudio isn't installed";
        return false;
    }
    block_ = cfg.blocksize > 0 ? cfg.blocksize : 512;
    const PaDevice* outDev = cfg.outputDevice.empty() ? nullptr : findDevice(st, cfg.outputDevice, false);
    const PaDevice* inDev = cfg.inputDevice.empty() ? nullptr : findDevice(st, cfg.inputDevice, true);
    if (!cfg.outputDevice.empty() && !outDev && cfg.outputDevice != kDefaultId) {
        error = "No sound server output called \"" + cfg.outputDevice + "\"";
        return false;
    }
    if (!cfg.inputDevice.empty() && !inDev && cfg.inputDevice != kDefaultId) {
        error = "No sound server input called \"" + cfg.inputDevice + "\"";
        return false;
    }
    rate_ = cfg.sampleRate > 0 ? cfg.sampleRate : outDev ? outDev->rate : inDev ? inDev->rate : 48000;
    if (rate_ <= 0) rate_ = 48000;
    int err = 0;
    if (!cfg.outputDevice.empty()) {
        outCh_ = cfg.outChannels > 0 ? cfg.outChannels : outDev ? outDev->channels : 2;
        outCh_ = std::max(1, std::min(outCh_, (int)PA_CHANNELS_MAX));
        pa_sample_spec ss{PA_SAMPLE_FLOAT32LE, (uint32_t)rate_, (uint8_t)outCh_};
        uint32_t frameBytes = (uint32_t)(outCh_ * 4);
        pa_buffer_attr attr;
        attr.maxlength = (uint32_t)-1;
        attr.tlength = (uint32_t)block_ * frameBytes * 2;  // two blocks in the server's buffer
        attr.prebuf = (uint32_t)-1;
        attr.minreq = (uint32_t)block_ * frameBytes;
        attr.fragsize = (uint32_t)-1;
        const char* dev = cfg.outputDevice == kDefaultId ? nullptr : cfg.outputDevice.c_str();
        play_ = pa_simple_new(nullptr, "Tiny DAW", PA_STREAM_PLAYBACK, dev, "Playback", &ss, nullptr, &attr, &err);
        if (!play_) {
            error = std::string("Can't open the sound server's output: ") + pa_strerror(err);
            return false;
        }
        name_ = outDev ? outDev->description : "Default output";
    }
    if (!cfg.inputDevice.empty()) {
        inCh_ = cfg.inChannels > 0 ? cfg.inChannels : inDev ? inDev->channels : 2;
        inCh_ = std::max(1, std::min(inCh_, (int)PA_CHANNELS_MAX));
        pa_sample_spec ss{PA_SAMPLE_FLOAT32LE, (uint32_t)rate_, (uint8_t)inCh_};
        pa_buffer_attr attr;
        attr.maxlength = (uint32_t)-1;
        attr.tlength = (uint32_t)-1;
        attr.prebuf = (uint32_t)-1;
        attr.minreq = (uint32_t)-1;
        attr.fragsize = (uint32_t)(block_ * inCh_ * 4);
        const char* dev = cfg.inputDevice == kDefaultId ? nullptr : cfg.inputDevice.c_str();
        rec_ = pa_simple_new(nullptr, "Tiny DAW", PA_STREAM_RECORD, dev, "Recording", &ss, nullptr, &attr, &err);
        if (!rec_) {
            error = std::string("Can't open the sound server's input: ") + pa_strerror(err);
            return false;
        }
        if (name_.empty()) name_ = inDev ? inDev->description : "Default input";
    }
    out_.assign((size_t)block_ * std::max(1, outCh_), 0.f);
    in_.assign((size_t)block_ * std::max(1, inCh_), 0.f);
    return true;
}

bool PulseStream::start() {
    if (running_) return true;
    underflows = 0;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void PulseStream::stop() {
    if (!running_ && !thread_.joinable()) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    int err;
    if (play_) pa_simple_flush(play_, &err);  // drop what is still queued
    if (rec_) pa_simple_flush(rec_, &err);
}

void PulseStream::loop() {
    raiseThreadPriority();
    int err = 0;
    int n = 0;
    while (running_) {
        if (rec_) {
            if (pa_simple_read(rec_, in_.data(), in_.size() * sizeof(float), &err) < 0) {
                lost = true;
                break;
            }
        }
        std::fill(out_.begin(), out_.end(), 0.f);
        try {
            cb_(rec_ ? in_.data() : nullptr, out_.data(), block_);
        } catch (...) {
            std::fill(out_.begin(), out_.end(), 0.f);
        }
        if (play_) {
            if (pa_simple_write(play_, out_.data(), out_.size() * sizeof(float), &err) < 0) {
                lost = true;
                break;
            }
            if ((n++ & 15) == 0) {
                pa_usec_t us = pa_simple_get_latency(play_, &err);
                if (us != (pa_usec_t)-1 && us > 0) outLatency_ = us / 1e6;
            }
        }
    }
}

}  // namespace

bool pulseAvailable() { return query().ok; }

std::vector<DeviceInfo> pulseDevices(bool input) {
    PaState st = query();
    std::vector<DeviceInfo> out;
    if (!st.ok) return out;
    const std::vector<PaDevice>& list = input ? st.sources : st.sinks;
    if (list.empty()) return out;
    const PaDevice* def = findDevice(st, kDefaultId, input);
    const PaDevice& d0 = def ? *def : list[0];
    out.push_back(toInfo(d0, input, kDefaultId, "Default (" + d0.description + ")"));
    for (const PaDevice& d : list) out.push_back(toInfo(d, input, d.name, d.description));
    return out;
}

bool pulseDeviceInfo(const std::string& id, DeviceInfo& out, bool input) {
    PaState st = query();
    if (!st.ok) return false;
    const PaDevice* d = findDevice(st, id, input);
    if (!d && id == kDefaultId) {
        const std::vector<PaDevice>& list = input ? st.sources : st.sinks;
        if (!list.empty()) d = &list[0];
    }
    if (!d) return false;
    out = toInfo(*d, input, id, id == kDefaultId ? "Default (" + d->description + ")" : d->description);
    return true;
}

std::string pulseDefault(bool input) {
    PaState st = query();
    if (!st.ok || (input ? st.sources : st.sinks).empty()) return "";
    return kDefaultId;
}

std::unique_ptr<Stream> pulseOpen(const StreamConfig& cfg, Callback cb, std::string& error) {
    std::unique_ptr<PulseStream> s(new PulseStream());
    if (!s->open(cfg, std::move(cb), error)) return nullptr;
    return s;
}

}  // namespace detail
}  // namespace audio
