// The JACK driver type and MIDI keyboard input.
//
// JACK: libjack.so.0 is loaded (dlopen, no linking); when a JACK server runs (jackd, or PipeWire with
// pipewire-jack) a stream runs 0.5 s writing only ZEROS (never a sound) and the callbacks are counted.
// Without a server, JACK must not be offered and opening must fail with a message. Then the stream
// is run against a stand-in JACK (our own functions through jack::setApiForTesting: no sound
// anywhere) to check ports, connections, interleaving, chunking, xruns, shutdown and the latency rule.
// ALSA: a shared device keeps at least 80 ms (zeros only, if "default" opens).
// MIDI: a virtual ALSA sequencer keyboard of the test's own plays into midi_in::Input; then Sustain.
#include <alsa/asoundlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "audio/devices.h"
#include "audio/jack_api.h"
#include "audio/midi_in.h"
#include "check.h"

using namespace audio;

namespace {

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool hasDriver(const std::string& key) {
    for (auto& d : driverTypes())
        if (d.key == key) return true;
    return false;
}

// ================================================================ the real JACK
void testRealJack() {
    const jack::Api* a = jack::api();
    printf("libjack.so.0: %s\n", a ? "loaded" : "not there");
    if (!a) return;
    auto t0 = std::chrono::steady_clock::now();
    bool offered = hasDriver("JACK");
    printf("JACK server: %s (looked in %.0f ms)\n", offered ? "running" : "none", secondsSince(t0) * 1000);
    if (!offered) {
        CHECK(outputDevices("JACK").empty());
        CHECK(defaultOutput("JACK").empty());
        StreamConfig cfg;
        cfg.driverType = "JACK";
        cfg.outputDevice = "system";
        std::string err;
        auto s = Stream::open(cfg, [](const float*, float*, int) {}, err);
        CHECK(!s);
        CHECK(!err.empty());
        printf("   (opening says: %s) -- the live JACK stream test is skipped\n", err.c_str());
        return;
    }
    auto types = driverTypes();
    CHECK(!types.empty() && types[0].key == "JACK");  // offered first
    auto outs = outputDevices("JACK");
    for (auto& d : outs) printf("   out %-30s %d ch %d Hz\n", d.id.c_str(), d.maxOutputs, d.defaultSampleRate);
    for (auto& d : inputDevices("JACK")) printf("   in  %-30s %d ch\n", d.id.c_str(), d.maxInputs);
    std::string dev = defaultOutput("JACK");
    CHECK(!dev.empty());
    StreamConfig cfg;
    cfg.driverType = "JACK";
    cfg.outputDevice = dev;
    std::atomic<int> calls{0}, frames{0}, notZeroed{0}, badFrames{0};
    std::atomic<int> expect{0};
    std::string err;
    auto s = Stream::open(cfg,
                          [&](const float*, float* out, int n) {
                              calls++;
                              frames += n;
                              if (out && out[0] != 0.f) notZeroed++;
                              if (n <= 0 || (expect && n > expect)) badFrames++;
                              // writes nothing: silence
                          },
                          err);
    CHECK(s != nullptr);
    if (!s) {
        printf("   open failed: %s\n", err.c_str());
        return;
    }
    expect = s->blocksize();
    CHECK(s->outChannels() > 0);
    CHECK(s->start());
    auto t = std::chrono::steady_clock::now();
    sleepMs(500);
    s->stop();
    double ran = secondsSince(t);
    double want = ran * s->sampleRate();
    printf("   stream: %s, %d Hz, %d frames, %d outputs, latency %.1f ms; %d callbacks, %d frames in %.2f s "
           "(%.0f expected), %d xruns\n",
           s->deviceName().c_str(), s->sampleRate(), s->blocksize(), s->outChannels(), s->outputLatency() * 1000,
           calls.load(), frames.load(), ran, want, s->underflows.load());
    CHECK(calls > 0);
    CHECK(frames > want * 0.6 && frames < want * 1.4);
    CHECK(notZeroed == 0);
    CHECK(badFrames == 0);
    double lat = s->outputLatency();
    CHECK(lat > 0 && lat <= 10.0);
    CHECK(!s->lost);
    s.reset();
}

// ================================================================ a stand-in JACK server
namespace fake {
struct Port {
    std::string name, type;
    unsigned long flags = 0;
    std::vector<float> buf = std::vector<float>(8192, 0.f);
};
struct Client {
    std::string name;
    std::deque<Port> ports;
    jack::ProcessCallback process = nullptr;
    void* processArg = nullptr;
    jack::XRunCallback xrun = nullptr;
    void* xrunArg = nullptr;
    jack::InfoShutdownCallback infoShutdown = nullptr;
    void* shutdownArg = nullptr;
    std::thread thread;
    std::atomic<bool> active{false};
    int64_t frame = 0;  // frames processed
    std::atomic<int> cycles{0};
    std::atomic<int> badOut{0};
};

bool serverUp = true;
const jack::nframes_t RATE = 48000, BUFFER = 256;
std::deque<Port> physical;
std::mutex m;
std::vector<std::pair<std::string, std::string>> connections;
std::atomic<int> deactivatesAfterShutdown{0}, clients{0};
std::atomic<bool> shutDown{false};
std::atomic<int> tooBig{0};
Client* last = nullptr;  // the stream's client

void addPhysical(const char* name, unsigned long dir, const char* type = jack::DEFAULT_AUDIO_TYPE) {
    Port p;
    p.name = name;
    p.type = type;
    p.flags = dir | jack::PortIsPhysical;
    physical.push_back(p);
}

void cycleThread(Client* c) {
    while (c->active) {
        // every 4th cycle is twice the opened buffer (the server's buffer grew): the stream cuts it up
        jack::nframes_t n = (c->cycles % 4 == 3) ? 2 * BUFFER : BUFFER;
        int k = 0;
        for (auto& p : c->ports)
            if (p.flags & jack::PortIsInput) {
                for (jack::nframes_t i = 0; i < n; i++) p.buf[i] = -(float)(k * 1000 + (c->frame + i) % 1000);
                k++;
            }
        c->process(n, c->processArg);
        k = 0;
        for (auto& p : c->ports)
            if (p.flags & jack::PortIsOutput) {
                for (jack::nframes_t i = 0; i < n; i++)
                    if (p.buf[i] != (float)(k * 1000 + (c->frame + i) % 1000)) c->badOut++;
                k++;
            }
        c->frame += n;
        c->cycles++;
        sleepMs(1);
    }
}

jack::Client* clientOpen(const char* name, int options, int* status, ...) {
    if (!serverUp) {
        if (status) *status = jack::Failure | jack::ServerFailed;
        return nullptr;
    }
    CHECK(options & jack::NoStartServer);
    auto* c = new Client;
    c->name = name;
    clients++;
    if (status) *status = 0;
    return reinterpret_cast<jack::Client*>(c);
}
Client* C(jack::Client* c) { return reinterpret_cast<Client*>(c); }
void halt(Client* c) {
    if (c->active) {
        c->active = false;
        c->thread.join();
    }
}
int deactivate(jack::Client* jc) {
    if (shutDown) deactivatesAfterShutdown++;
    halt(C(jc));
    return 0;
}
int clientClose(jack::Client* jc) {
    halt(C(jc));
    if (C(jc) == last) last = nullptr;
    delete C(jc);
    clients--;
    return 0;
}
const char* clientName(jack::Client* c) { return C(c)->name.c_str(); }
int activate(jack::Client* jc) {
    Client* c = C(jc);
    CHECK(c->process != nullptr);
    last = c;
    c->active = true;
    c->thread = std::thread(cycleThread, c);
    return 0;
}
int setProcess(jack::Client* c, jack::ProcessCallback cb, void* arg) {
    C(c)->process = cb;
    C(c)->processArg = arg;
    return 0;
}
int setXrun(jack::Client* c, jack::XRunCallback cb, void* arg) {
    C(c)->xrun = cb;
    C(c)->xrunArg = arg;
    return 0;
}
void onShutdown(jack::Client*, jack::ShutdownCallback, void*) { CHECK(false); }  // (on_info_shutdown is used)
void onInfoShutdown(jack::Client* c, jack::InfoShutdownCallback cb, void* arg) {
    C(c)->infoShutdown = cb;
    C(c)->shutdownArg = arg;
}
int setBufferSize(jack::Client*, jack::BufferSizeCallback, void*) { return 0; }
jack::nframes_t sampleRate(jack::Client*) { return RATE; }
jack::nframes_t bufferSize(jack::Client*) { return BUFFER; }
jack::Port* portRegister(jack::Client* jc, const char* name, const char* type, unsigned long flags, unsigned long) {
    Client* c = C(jc);
    Port p;
    p.name = c->name + ":" + name;
    p.type = type;
    p.flags = flags;
    c->ports.push_back(p);
    return reinterpret_cast<jack::Port*>(&c->ports.back());
}
int portUnregister(jack::Client*, jack::Port*) { return 0; }
void* portBuffer(jack::Port* p, jack::nframes_t n) {
    if (n > 8192) tooBig++;  // (no CHECK on the process thread: check.h's counters aren't atomic)
    return reinterpret_cast<Port*>(p)->buf.data();
}
const char* portName(const jack::Port* p) { return reinterpret_cast<const Port*>(p)->name.c_str(); }
int connect(jack::Client*, const char* src, const char* dst) {
    std::lock_guard<std::mutex> lock(m);
    connections.push_back({src, dst});
    return 0;
}
const char** getPorts(jack::Client*, const char* namePattern, const char* typePattern, unsigned long flags) {
    CHECK(namePattern == nullptr);
    std::vector<const char*> found;
    for (auto& p : physical)
        if ((p.flags & flags) == flags && (!typePattern || p.type == typePattern)) found.push_back(p.name.c_str());
    if (found.empty()) return nullptr;
    auto** out = (const char**)malloc(sizeof(char*) * (found.size() + 1));
    for (size_t i = 0; i < found.size(); i++) out[i] = found[i];
    out[found.size()] = nullptr;
    return out;
}
void latencyRange(jack::Port*, int, jack::LatencyRange* r) { r->min = r->max = 0xFFFFFFFFu; }  // PipeWire's 2^32
void errorFunction(jack::ErrorFunction) {}

jack::Api api() {
    jack::Api a{};
    a.client_open = clientOpen;
    a.client_close = clientClose;
    a.get_client_name = clientName;
    a.activate = activate;
    a.deactivate = deactivate;
    a.set_process_callback = setProcess;
    a.set_xrun_callback = setXrun;
    a.on_shutdown = onShutdown;
    a.on_info_shutdown = onInfoShutdown;
    a.set_buffer_size_callback = setBufferSize;
    a.get_sample_rate = sampleRate;
    a.get_buffer_size = bufferSize;
    a.port_register = portRegister;
    a.port_unregister = portUnregister;
    a.port_get_buffer = portBuffer;
    a.port_name = portName;
    a.connect = connect;
    a.get_ports = getPorts;
    a.free_ = free;
    a.port_get_latency_range = latencyRange;
    a.set_error_function = errorFunction;
    a.set_info_function = errorFunction;
    return a;
}
}  // namespace fake

void testFakeJack() {
    printf("stand-in JACK server:\n");
    using namespace fake;
    addPhysical("system:playback_1", jack::PortIsInput);
    addPhysical("system:playback_2", jack::PortIsInput);
    addPhysical("system:capture_1", jack::PortIsOutput);
    addPhysical("system:capture_2", jack::PortIsOutput);
    addPhysical("system:midi_playback_1", jack::PortIsInput, "8 bit raw midi");
    addPhysical("Built-in Audio Analog Stereo:playback_FL", jack::PortIsInput);
    addPhysical("Built-in Audio Analog Stereo:playback_FR", jack::PortIsInput);
    addPhysical("Built-in Audio Analog Stereo:monitor_FL", jack::PortIsOutput);
    addPhysical("Built-in Audio Analog Stereo:monitor_FR", jack::PortIsOutput);
    addPhysical("Built-in Audio Analog Stereo:capture_MONO", jack::PortIsOutput);
    static jack::Api a = fake::api();
    jack::setApiForTesting(&a);

    // devices: the clients owning physical audio ports; "system" first; monitors aren't inputs
    auto types = driverTypes();
    CHECK(!types.empty() && types[0].key == "JACK" && types[0].label == "JACK");
    auto outs = outputDevices("JACK");
    CHECK(outs.size() == 2);
    if (outs.size() == 2) {
        CHECK(outs[0].id == "system" && outs[0].maxOutputs == 2 && outs[0].defaultSampleRate == 48000);
        CHECK(outs[1].id == "Built-in Audio Analog Stereo" && outs[1].maxOutputs == 2);
    }
    auto ins = inputDevices("JACK");
    CHECK(ins.size() == 2);
    if (ins.size() == 2) CHECK(ins[1].maxInputs == 1);
    CHECK(defaultOutput("JACK") == "system");
    CHECK(defaultInput("JACK") == "system");
    DeviceInfo info;
    CHECK(deviceInfo("JACK", "Built-in Audio Analog Stereo", info, true) && info.maxInputs == 1);
    CHECK(!deviceInfo("JACK", "nothing", info));
    CHECK(minLatency("JACK", "system") == 0);

    // a duplex stream: 2 outputs, 2 inputs, interleaved both ways, cut into the opened buffer
    StreamConfig cfg;
    cfg.driverType = "JACK";
    cfg.outputDevice = "system";
    cfg.inputDevice = "system";
    cfg.sampleRate = 44100;  // (the server's rate wins)
    cfg.blocksize = 128;     // (the server's buffer wins; this stands in for the impossible latency)
    std::atomic<int> calls{0}, badIn{0}, tooLong{0};
    int64_t counter = 0;  // (the callback's thread only)
    std::string err;
    auto s = Stream::open(cfg,
                          [&](const float* in, float* out, int n) {
                              calls++;
                              if (n > (int)BUFFER) tooLong++;
                              for (int i = 0; i < n; i++)
                                  for (int c = 0; c < 2; c++) {
                                      float v = (float)(c * 1000 + (counter + i) % 1000);
                                      if (!in || in[i * 2 + c] != -v) badIn++;
                                      out[i * 2 + c] = v;  // (to a stand-in port: nothing is heard)
                                  }
                              counter += n;
                          },
                          err);
    CHECK(s != nullptr);
    if (!s) {
        printf("   open failed: %s\n", err.c_str());
        return;
    }
    CHECK(s->sampleRate() == 48000);
    CHECK(s->blocksize() == 256);
    CHECK(s->outChannels() == 2 && s->inChannels() == 2);
    CHECK(s->deviceName() == "system");
    CHECK(s->start());
    sleepMs(150);
    CHECK(last != nullptr);
    int cycles = last ? last->cycles.load() : 0;
    CHECK(cycles > 10);
    CHECK(badIn == 0);
    CHECK(tooLong == 0);
    CHECK(last && last->badOut == 0);
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(connections.size() == 4);
        if (connections.size() == 4) {
            CHECK(connections[0].first == "Tiny DAW:out_1" && connections[0].second == "system:playback_1");
            CHECK(connections[1].first == "Tiny DAW:out_2" && connections[1].second == "system:playback_2");
            CHECK(connections[2].first == "system:capture_1" && connections[2].second == "Tiny DAW:in_1");
            CHECK(connections[3].first == "system:capture_2" && connections[3].second == "Tiny DAW:in_2");
        }
    }
    // 2^32 frames reported: the delay asked for stands in
    CHECK_NEAR(s->outputLatency(), 128.0 / 48000, 1e-9);
    CHECK_NEAR(s->inputLatency(), 128.0 / 48000, 1e-9);
    // an xrun counts as an underflow
    last->xrun(last->xrunArg);
    last->xrun(last->xrunArg);
    CHECK(s->underflows == 2);
    s->stop();
    int after = last ? last->cycles.load() : 0;
    sleepMs(20);
    CHECK(last && last->cycles == after);  // deactivated: no more callbacks
    printf("   %d cycles, %d callbacks (every 4th cycle 2 buffers long)\n", cycles, calls.load());
    CHECK(calls >= cycles + cycles / 4 - 1);
    // started again, then the server goes away: lost, and no JACK calls but closing
    CHECK(s->start());
    sleepMs(20);
    shutDown = true;
    Client* c = last;
    halt(c);  // the server stops calling…
    c->infoShutdown(jack::ClientZombie, "server quit", c->shutdownArg);  // …and says so
    CHECK(s->lost);
    s->stop();
    s.reset();
    CHECK(deactivatesAfterShutdown == 0);
    CHECK(clients == 0);
    shutDown = false;

    // outputs only, the second device, its 2 channels; no blocksize asked: one server buffer stands in
    cfg.outputDevice = "Built-in Audio Analog Stereo";
    cfg.inputDevice.clear();
    cfg.blocksize = 0;
    connections.clear();
    std::atomic<int> withInput{0};
    s = Stream::open(cfg, [&](const float* in, float*, int) { withInput += in != nullptr; }, err);
    CHECK(s && s->outChannels() == 2 && s->inChannels() == 0);
    if (s) {
        CHECK_NEAR(s->outputLatency(), 256.0 / 48000, 1e-9);
        CHECK(s->inputLatency() == 0);
        CHECK(s->start());
        sleepMs(10);
        s->stop();
        CHECK(connections.size() == 2 && connections[1].second == "Built-in Audio Analog Stereo:playback_FR");
        CHECK(withInput == 0);
    }
    s.reset();
    // a device that isn't there
    cfg.outputDevice = "Elsewhere";
    s = Stream::open(cfg, [](const float*, float*, int) {}, err);
    CHECK(!s && err.find("Elsewhere") != std::string::npos);
    s.reset();
    CHECK(clients == 0);
    CHECK(tooBig == 0);

    // no server: not offered, opening fails
    serverUp = false;
    sleepMs(1100);  // (the device list is looked at again after a second)
    CHECK(!hasDriver("JACK"));
    CHECK(outputDevices("JACK").empty());
    cfg.outputDevice = "system";
    s = Stream::open(cfg, [](const float*, float*, int) {}, err);
    CHECK(!s && !err.empty());
    jack::setApiForTesting(nullptr);
}

// ================================================================ ALSA's shared devices
void testAlsaShared() {
    CHECK(minLatency("ALSA", "default") == SHARED_ALSA_MIN_LATENCY);
    CHECK(minLatency("ALSA", "pipewire") == SHARED_ALSA_MIN_LATENCY);
    CHECK(minLatency("ALSA", "hw:CARD=PCH,DEV=0") == 0);
    CHECK(minLatency("ALSA", "plughw:CARD=PCH,DEV=0") == 0);
    CHECK(minLatency("PulseAudio", "x") == 0);
    CHECK(plausibleLatency(4294967295.0 / 48000, 0.005) == 0.005);
    CHECK(plausibleLatency(0.02, 0.005) == 0.02);
    std::string def = defaultOutput("ALSA");
    printf("ALSA default output: \"%s\"\n", def.c_str());
    CHECK(!def.empty());
    if (def.compare(0, 3, "hw:") == 0 || def.compare(0, 7, "plughw:") == 0) return;
    StreamConfig cfg;
    cfg.driverType = "ALSA";
    cfg.outputDevice = def;
    cfg.blocksize = 256;
    std::atomic<int> calls{0};
    std::string err;
    auto s = Stream::open(cfg, [&](const float*, float*, int) { calls++; /* zeros: silence */ }, err);
    if (!s) {
        printf("   (\"%s\" doesn't open: %s)\n", def.c_str(), err.c_str());
        return;
    }
    printf("   \"%s\" at 256 frames: %d-frame periods, %.1f ms delay\n", def.c_str(), s->blocksize(),
           s->outputLatency() * 1000);
    CHECK(s->outputLatency() >= SHARED_ALSA_MIN_LATENCY - 0.002);
    CHECK(s->start());
    sleepMs(200);
    s->stop();
    CHECK(calls > 0);
}

// ================================================================ MIDI
using midi_in::Event;

struct Keyboard {  // a virtual MIDI keyboard: an ALSA sequencer client with one readable port
    snd_seq_t* seq = nullptr;
    int port = -1;
    bool open() {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return false;
        snd_seq_set_client_name(seq, "TinyDAW test keyboard");
        port = snd_seq_create_simple_port(seq, "keys", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        return port >= 0;
    }
    void send(snd_seq_event_t& ev) {
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    }
    void note(int n, int v) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_noteon(&ev, 0, n, v);
        send(ev);
    }
    void off(int n) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_noteoff(&ev, 0, n, 64);
        send(ev);
    }
    void cc(int c, int v) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_controller(&ev, 0, c, v);
        send(ev);
    }
    void bend(int v) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_pitchbend(&ev, 0, v);
        send(ev);
    }
    void close() {
        if (seq) snd_seq_close(seq);
        seq = nullptr;
    }
};

std::string describe(const Event& e) {
    switch (e.type) {
        case Event::NoteOn: return "on" + std::to_string(e.data1) + "/" + std::to_string(e.data2);
        case Event::NoteOff: return "off" + std::to_string(e.data1);
        case Event::Control: return "cc" + std::to_string(e.data1) + "=" + std::to_string(e.data2);
        default: return "bend" + std::to_string(e.bend);
    }
}

std::string join(const std::vector<Event>& v) {
    std::string s;
    for (auto& e : v) s += (s.empty() ? "" : " ") + describe(e);
    return s;
}

void testSustainLogic() {
    midi_in::Sustain sus;
    std::vector<Event> out;
    auto feed = [&](const Event& e) { sus.handle(e, out); };
    // no pedal: everything passes
    feed(Event::noteOn(60, 100));
    feed(Event::noteOff(60));
    CHECK(join(out) == "on60/100 off60");
    out.clear();
    // pedal down: keys let go keep sounding; pedal up lets them go (sorted)
    feed(Event::noteOn(64, 90));
    feed(Event::noteOn(62, 90));
    feed(Event::control(64, 127));
    CHECK(sus.pedal());
    feed(Event::noteOff(64));
    feed(Event::noteOff(62));
    CHECK(join(out) == "on64/90 on62/90");
    CHECK(sus.sounding(64) && sus.sounding(62));
    feed(Event::noteOn(67, 50));  // struck while the pedal is down, still held when it lifts: sounds on
    feed(Event::control(64, 0));
    CHECK(join(out) == "on64/90 on62/90 on67/50 off62 off64");
    CHECK(!sus.pedal() && sus.sounding(67) && !sus.sounding(64));
    feed(Event::noteOff(67));
    out.clear();
    // a key let go before the pedal went down isn't held; one struck again restarts
    feed(Event::control(64, 100));
    feed(Event::noteOff(70));  // (not sounding: passes on)
    feed(Event::noteOn(60, 80));
    feed(Event::noteOff(60));
    feed(Event::noteOn(60, 81));  // again while held by the pedal: let go first, then struck
    CHECK(join(out) == "off70 on60/80 off60 on60/81");
    // all notes off: every note sounding (held by the pedal or a key) lets go, pedal too, then the CC
    feed(Event::noteOn(48, 70));
    feed(Event::noteOff(60));  // (sustained)
    out.clear();
    feed(Event::control(123, 0));
    CHECK(join(out) == "off48 off60 cc123=0");
    CHECK(!sus.pedal() && !sus.sounding(48) && !sus.sounding(60));
    out.clear();
    // other controls and bends pass unchanged; the pedal's own aren't passed on
    feed(Event::control(1, 33));
    feed(Event::control(64, 127));
    Event b;
    b.type = Event::PitchBend;
    b.bend = -300;
    feed(b);
    CHECK(join(out) == "cc1=33 bend-300");
    // releaseAll (closing the input)
    feed(Event::noteOn(50, 10));
    feed(Event::noteOff(50));
    out.clear();
    sus.releaseAll(out, 5.0);
    CHECK(join(out) == "off50");
    CHECK(!out.empty() && out[0].time == 5.0);
    CHECK(!sus.pedal());
    // a note-on of velocity 0 is a note-off
    CHECK(Event::noteOn(60, 0).type == Event::NoteOff);
    CHECK(midi_in::stableName("Keystation 49:Keystation 49 MIDI 1 20:0") == "Keystation 49:Keystation 49 MIDI 1");
    CHECK(midi_in::stableName("No numbers") == "No numbers");
}

void testMidiInput() {
    Keyboard kb;
    if (!kb.open()) {
        printf("MIDI: no ALSA sequencer here: the live MIDI test is skipped\n");
        return;
    }
    auto list = midi_in::ports();
    const midi_in::PortInfo* mine = nullptr;
    for (auto& p : list) {
        printf("   MIDI port \"%s\" (remembered as \"%s\")\n", p.name.c_str(), p.stable.c_str());
        CHECK(p.name.compare(0, 7, "System:") != 0);
        if (p.stable == "TinyDAW test keyboard:keys") mine = &p;
    }
    CHECK(mine != nullptr);
    midi_in::Input in;
    CHECK(!in.open("No such keyboard"));
    CHECK(!in.isOpen() && in.name() == "No such keyboard");
    CHECK(in.open(""));
    std::atomic<int> called{0}, notified{0};
    in.setCallback([&](const Event&) { called++; });
    in.setNotify([&] { notified++; });
    CHECK(in.open("TinyDAW test keyboard:keys"));
    CHECK(in.isOpen());
    for (auto& p : midi_in::ports()) CHECK(p.name.compare(0, 9, "Tiny DAW:") != 0);  // our own isn't offered
    double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    kb.note(60, 100);
    kb.note(64, 90);
    kb.cc(64, 127);
    kb.off(60);
    kb.off(64);
    kb.note(60, 80);  // struck again while held
    kb.cc(64, 0);
    kb.bend(1000);
    kb.note(67, 0);  // (velocity 0: a note-off)
    kb.note(67, 55);
    kb.cc(123, 0);
    std::vector<Event> got;
    for (int i = 0; i < 200 && got.size() < 11; i++) {
        sleepMs(5);
        in.poll(got);
    }
    double t1 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    printf("   received: %s\n", join(got).c_str());
    CHECK(join(got) == "on60/100 on64/90 cc64=127 off60 off64 on60/80 cc64=0 bend1000 off67 on67/55 cc123=0");
    CHECK(called == 11);
    CHECK(notified >= 1);
    bool ordered = true, inTime = true;
    for (size_t i = 0; i < got.size(); i++) {
        if (i && got[i].time < got[i - 1].time) ordered = false;
        if (got[i].time < t0 - 0.001 || got[i].time > t1 + 0.001) inTime = false;
    }
    CHECK(ordered);
    CHECK(inTime);
    // through the pedal, as the GUI's handle() does
    midi_in::Sustain sus;
    std::vector<Event> played;
    for (auto& e : got) sus.handle(e, played);
    printf("   played:   %s\n", join(played).c_str());
    CHECK(join(played) == "on60/100 on64/90 off60 on60/80 off64 bend1000 off67 on67/55 off60 off67 cc123=0");
    // the keyboard goes away
    CHECK(!in.lost());
    kb.close();
    for (int i = 0; i < 100 && !in.lost(); i++) sleepMs(5);
    CHECK(in.lost());
    in.close();
    CHECK(!in.isOpen());
    std::vector<Event> none;
    CHECK(in.poll(none) == 0);
    CHECK(in.dropped() == 0);
}

}  // namespace

int main() {
    testRealJack();
    testFakeJack();
    testAlsaShared();
    testSustainLogic();
    testMidiInput();
    TEST_MAIN_END();
}
