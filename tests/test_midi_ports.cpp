// MIDI in and out, worst cases first, with a fake port backend (no MIDI device needed; tests/test_jack_midi does the
// real ALSA sequencer): 16 inputs and 16 outputs streaming dense MIDI at once; MIDI clock at 300 BPM through tempo
// ramps against the exact beat times; Start / Continue / Stop / Song Position; a device unplugged while playing (no
// crash, notes off, greyed, reopened on return); External synth notes ahead by their delay (and through the loop),
// Measure, and their sound through the mixer; JACK's transport both ways against a fake JACK; MIDI clock in; the
// preferences; a track's MIDI input and External synth saved, loaded and undone.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "audio/engine.h"
#include "audio/midi_out.h"
#include "audio/midi_ports.h"
#include "audio/midi_sync.h"
#include "audio/mixer.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "ui/midi_input.h"
#include "ui/preferences.h"
#include "ui/recording.h"

using midi_in::Event;

namespace {

double monoNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

Note note(double s, double e, int pitch, int vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

// a port backend of the test's own: ports listed as the test says, what is sent kept with when it was sent
class Fake : public midi_ports::Backend {
public:
    std::vector<midi_ports::Found> ports;  // (GUI thread)
    struct Sent {
        int slot;
        uint8_t b[3];
        int n;
        double when, at;
    };
    std::mutex lock;
    std::vector<Sent> sent;
    std::map<int, std::string> opened;     // slot -> key
    bool timedSend = false;
    const char* name() const override { return "Fake"; }
    void list(std::vector<midi_ports::Found>& out) override { out.insert(out.end(), ports.begin(), ports.end()); }
    bool open(const midi_ports::Found& f, int slot) override {
        opened[slot] = f.key;
        return true;
    }
    void close(int slot) override { opened.erase(slot); }
    void send(int slot, const uint8_t* b, int n, double when) override {
        Sent s{slot, {b[0], n > 1 ? b[1] : (uint8_t)0, n > 2 ? b[2] : (uint8_t)0}, n, when, monoNow()};
        std::lock_guard<std::mutex> g(lock);
        sent.push_back(s);
    }
    bool timed() const override { return timedSend; }
    void add(const std::string& key, bool input) {
        midi_ports::Found f;
        f.key = key;
        f.input = input;
        ports.push_back(f);
    }
    void remove(const std::string& key) {
        ports.erase(std::remove_if(ports.begin(), ports.end(), [&](auto& f) { return f.key == key; }), ports.end());
        changed_ = true;  // (as ALSA announces it)
    }
    void play(int slot, std::initializer_list<uint8_t> bytes, double t) {  // (its one input thread)
        std::vector<uint8_t> b(bytes);
        deliverBytes(slot, b.data(), (int)b.size(), t);
    }
    size_t count() {
        std::lock_guard<std::mutex> g(lock);
        return sent.size();
    }
};

std::string key(int i, const char* what) { return std::string("Fake ") + what + " " + std::to_string(i); }

// ================================================================ 16 inputs and 16 outputs at once
void testDense() {
    midi_ports::Hub hub;
    auto fake = std::make_unique<Fake>();
    Fake* f = fake.get();
    for (int i = 0; i < 16; i++) f->add(key(i, "in"), true), f->add(key(i, "out"), false);
    f->add("Midi Through:Midi Through Port-0", true);
    hub.addBackend(std::move(fake));
    hub.start(false);
    int opened = 0;
    for (auto& p : hub.ports()) opened += p.input && p.open;
    CHECK(opened == 16);  // (every new input on but the Through port)
    CHECK(!hub.find("Midi Through:Midi Through Port-0", true)->on);
    for (int i = 0; i < 16; i++) hub.setOn(key(i, "out"), false, true);
    // inputs: one thread (as ALSA's reader) streams 16 ports each as full as a MIDI cable goes (3125 messages a second,
    // three times what a busy USB keyboard sends) for a second; the GUI takes them once a frame (60 a second)
    const int PER = 3125;
    std::vector<int> slots;
    for (int i = 0; i < 16; i++) slots.push_back(hub.find(key(i, "in"), true)->slot);
    std::atomic<bool> done{false};
    std::thread reader([&] {
        double t = monoNow();
        for (int k = 0; k < PER; k++) {
            for (int i = 0; i < 16; i++)
                f->play(slots[(size_t)i], {(uint8_t)(0x90 | (i & 15)), (uint8_t)(k % 128), (uint8_t)(1 + k % 127)}, monoNow());
            if (k % 32 == 31) {  // (paced: 32 messages a port each 10.24 ms)
                t += 32.0 / PER;
                while (monoNow() < t) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        done = true;
    });
    std::vector<Event> got;
    double t0 = monoNow();
    while ((!done || got.size() < (size_t)(16 * PER)) && monoNow() - t0 < 10) {
        hub.poll(got);
        sleepMs(16);
    }
    reader.join();
    hub.poll(got);
    printf("dense in: %zu events from 16 inputs (%d dropped)\n", got.size(), f->dropped());
    CHECK(got.size() + (size_t)f->dropped() == (size_t)(16 * PER));
    CHECK(f->dropped() == 0);
    std::map<int, int> next;  // each port's notes in order
    bool ordered = true;
    for (auto& e : got) {
        int& n = next[e.port];
        if (e.type != Event::NoteOn || e.data1 != n % 128) ordered = false;
        n++;
    }
    CHECK(ordered);
    // the GUI stalls (a modal dialog's long work) while 20000 messages come: those the ring can't hold are dropped
    // and counted, nothing breaks, and what was kept comes in order
    for (int k = 0; k < 20000; k++) f->play(slots[0], {0x90, (uint8_t)(k % 128), 1}, monoNow());
    got.clear();
    hub.poll(got);
    printf("   a stalled GUI: %zu kept, %d dropped\n", got.size(), f->dropped());
    CHECK(got.size() == 8192 && f->dropped() == 20000 - 8192);
    bool kept = true;
    for (size_t k = 0; k < got.size(); k++) kept = kept && got[k].data1 == k % 128;
    CHECK(kept);

    // outputs: 16 External synth tracks, 16 notes a beat each, on 16 outputs, with clock on all of them, through the
    // audio thread's scheduler at real time (512-frame buffers at 48 kHz), the output thread sending
    Project project;
    project.timing.bpm = 300;
    std::vector<TrackPtr> tracks;
    std::vector<midi_out::ExternalTrack> ext;
    const int sr = 48000;
    for (int i = 0; i < 16; i++) {
        TrackPtr t = project.addTrack(Kind::Midi);
        t->needsInstrument = false;
        t->ext.on = true;
        t->ext.port = key(i, "out");
        t->ext.channel = i + 1;
        t->ext.input = 0;
        t->ext.delayMs = 5;
        std::vector<Note> notes;
        for (int k = 0; k < 64; k++) notes.push_back(note(k * 0.0125, k * 0.0125 + 0.01, 40 + (k + i) % 48));
        ClipPtr c = makeMidiClip("dense", notes, 0.8, sr);
        c->track = t;
        c->prepare(sr);
        project.addClip(c);
        tracks.push_back(t);
        ext.push_back({t.get(), hub.find(t->ext.port, false)->slot, i});
        hub.setClock(key(i, "out"), false, true);
    }
    hub.out->setTracks(ext);
    auto map = project.timing.map();
    auto list = project.tracks.get();
    auto clips = project.clips.get();
    double start = monoNow() + 0.01;
    int64_t pos = 0;
    const int frames = 512;
    for (int b = 0; pos < sr; b++) {  // 1 s of buffers, each when its time comes
        while (monoNow() < start + (double)pos / sr) std::this_thread::sleep_for(std::chrono::microseconds(100));
        midi_out::Buffer mb;
        mb.now = start + (double)pos / sr;
        mb.outLatency = 0.01;
        mb.frames = frames;
        mb.sr = sr;
        mb.map = map.get();
        mb.tracks = list.get();
        mb.clips = clips.get();
        midi_out::Part p{0, frames, true, pos};
        hub.out->process(mb, &p, 1);
        pos += frames;
    }
    midi_out::Buffer mb;  // stop
    mb.now = start + (double)pos / sr;
    mb.frames = frames;
    mb.sr = sr;
    mb.map = map.get();
    mb.tracks = list.get();
    mb.clips = clips.get();
    midi_out::Part p{0, frames, false, 0};
    hub.out->process(mb, &p, 1);
    // what must have gone out: per output 64 notes on and off, Start, the clock (5 beats a second), Stop, the All
    // Notes Off of stopping
    size_t pulses = (size_t)std::ceil(map->beatAt((double)pos / sr - 1e-12) * 24);
    size_t want = 16 * (64 * 2 + 1 + pulses + 1 + 1);
    for (int i = 0; i < 400 && f->count() < want; i++) sleepMs(5);
    std::vector<Fake::Sent> sent;
    {
        std::lock_guard<std::mutex> g(f->lock);
        sent = f->sent;
    }
    printf("dense out: %zu messages to 16 outputs (%zu expected, %d dropped)\n", sent.size(), want, hub.out->dropped());
    CHECK(sent.size() == want);
    CHECK(hub.out->dropped() == 0);
    // each output: its moments in order; each sent close to its moment (never early)
    std::map<int, double> last;
    bool inOrder = true, never = true;
    double worst = 0, sum = 0;
    for (auto& s : sent) {
        if (s.when < last[s.slot] - 1e-9) inOrder = false;
        last[s.slot] = s.when;
        if (s.at < s.when - 0.0005) never = false;
        worst = std::max(worst, s.at - s.when);
        sum += s.at - s.when;
    }
    printf("   sent after their moments: %.3f ms on average, %.3f ms at worst\n", sum / sent.size() * 1000, worst * 1000);
    CHECK(inOrder);
    CHECK(never);
    CHECK(sum / sent.size() < 0.002);  // (a busy test machine: generous)
    CHECK(worst < 0.05);
    hub.stop();
}

// the events the scheduler made, in order
std::vector<midi_out::Event> taken(midi_out::Out& out) {
    std::vector<midi_out::Event> v;
    midi_out::Event e;
    while (out.take(e)) v.push_back(e);
    return v;
}

// ================================================================ clock at 300 BPM through tempo ramps
void testClock() {
    Project project;
    project.timing.bpm = 300;
    TempoChange c;
    c.bar = 3, c.bpm = 120, c.ramp = true;  // from bar 3, 120 BPM ramping up…
    project.timing.setChange(c);
    TempoChange d;
    d.bar = 6, d.bpm = 300;  // …to 300 at bar 6
    project.timing.setChange(d);
    auto map = project.timing.map();
    CHECK(map->hasChanges());
    midi_out::Out out;
    out.setClocks({{1, false, 0.0}, {2, false, 0.003}});
    const int sr = 44100;
    double now = 1000.0;
    int64_t pos = 0;
    std::vector<midi_out::Event> all;
    // odd buffer sizes, the latency of the device and plugins
    for (int b = 0; pos < sr * 6; b++) {
        int frames = 64 + (b * 37) % 900;
        midi_out::Buffer mb;
        mb.now = now;
        mb.outLatency = 0.02;
        mb.latency = 128;
        mb.frames = frames;
        mb.sr = sr;
        mb.map = map.get();
        midi_out::Part p{0, frames, true, pos};
        out.process(mb, &p, 1);
        for (auto& e : taken(out)) all.push_back(e);
        now += (double)frames / sr;
        pos += frames;
    }
    std::vector<double> pulses;
    int starts = 0;
    for (auto& e : all) {
        if (e.slot == 1 && e.b[0] == midi_out::CLOCK) pulses.push_back(e.when);
        if (e.slot == 1 && e.b[0] == midi_out::START) starts++;
    }
    CHECK(starts == 1);
    // pulse k is heard exactly at the k/24th beat: when = start + latencies + secondsAt(k / 24)
    double base = 1000.0 + 0.02 + 128.0 / sr;
    double worst = 0;
    for (size_t k = 0; k < pulses.size(); k++) worst = std::max(worst, std::fabs(pulses[k] - (base + map->secondsAt(k / 24.0))));
    printf("clock: %zu pulses in 6 s through 300 -> 120 -> ramp -> 300 BPM, worst %.2g s off the exact beat\n",
           pulses.size(), worst);
    CHECK(worst < 1e-6);
    CHECK(pulses.size() == (size_t)(std::ceil(map->beatAt((double)pos / sr - 1e-12) * 24)));
    // the second port: the same pulses, 3 ms later
    std::vector<double> late;
    for (auto& e : all)
        if (e.slot == 2 && e.b[0] == midi_out::CLOCK) late.push_back(e.when);
    CHECK(late.size() == pulses.size());
    bool offset = late.size() == pulses.size();
    for (size_t k = 0; k < late.size() && offset; k++) offset = std::fabs(late[k] - pulses[k] - 0.003) < 1e-9;
    CHECK(offset);
    // inside the ramp the pulses come closer together as it speeds up
    double first = map->secondsAt(map->barBeat(2)), mid = map->secondsAt(map->barBeat(4));
    size_t a = (size_t)std::llround(map->beatAt(first) * 24), m = (size_t)std::llround(map->beatAt(mid) * 24);
    CHECK(m + 10 < pulses.size() && pulses[a + 1] - pulses[a] > pulses[m + 1] - pulses[m]);
}

// ================================================================ Start / Continue / Stop / Song Position Pointer
void testTransportMessages() {
    Project project;
    project.timing.bpm = 120;  // a beat: 0.5 s; a 16th: 0.125 s
    auto map = project.timing.map();
    const int sr = 48000;
    midi_out::Out out;
    out.setClocks({{3, false, 0.0}, {4, true, 0.0}});  // port 4: Always send Start
    double now = 0;
    auto run = [&](bool moving, int64_t start, int frames = 480) {
        midi_out::Buffer mb;
        mb.now = now;
        mb.frames = frames;
        mb.sr = sr;
        mb.map = map.get();
        midi_out::Part p{0, frames, moving, start};
        out.process(mb, &p, 1);
        now += (double)frames / sr;
        return taken(out);
    };
    auto names = [](const std::vector<midi_out::Event>& v, int slot) {
        std::string s;
        for (auto& e : v) {
            if (e.slot != slot) continue;
            std::string n = e.b[0] == midi_out::START ? "start" : e.b[0] == midi_out::CONTINUE ? "continue"
                            : e.b[0] == midi_out::STOP ? "stop" : e.b[0] == midi_out::CLOCK ? "." : "";
            if (e.b[0] == midi_out::SONG_POSITION) n = "spp" + std::to_string(e.b[1] | (e.b[2] << 7));
            if (!s.empty() && n != ".") s += " ";
            s += n;
        }
        return s;
    };
    run(false, 0);
    // from the song's start: Start, the first pulse at once
    auto v = run(true, 0);
    CHECK(names(v, 3).compare(0, 6, "start.") == 0);
    CHECK(names(v, 4).compare(0, 12, "spp0 start.") == 0 || names(v, 4).compare(0, 6, "start.") == 0);
    int64_t pos = 480;
    for (int i = 0; i < 9; i++, pos += 480) run(true, pos);
    // stopped: Stop
    v = run(false, 0);
    CHECK(names(v, 3) == "stop" && names(v, 4) == "stop");
    // from 5.3 beats (2.65 s): the next 16th is 22 (5.5 beats): Song Position 22, Continue; the first pulse at 5.5
    int64_t at = (int64_t)(2.65 * sr);
    v = run(true, at, 4800);  // (0.1 s: up to 2.75 s; 5.5 beats is 2.75 s: no pulse yet)
    CHECK(names(v, 3) == "spp22 continue");
    CHECK(names(v, 4) == "spp22 start");  // (Always send Start)
    v = run(true, at + 4800, 480);
    CHECK(!v.empty() && v[0].b[0] == midi_out::CLOCK);
    if (!v.empty()) CHECK_NEAR(v[0].when, now - 480.0 / sr, 1e-9);  // (exactly at 2.75 s: this buffer's start)
    // a jump while playing (a locate, the loop): Stop, the new position, Continue
    v = run(true, sr * 4, 480);  // (8 beats: 16th 32)
    CHECK(names(v, 3).compare(0, 21, "stop spp32 continue.") == 0);
    // a jump back to the start while playing: Song Position 0 and Start
    v = run(true, 0, 480);
    CHECK(names(v, 3).compare(0, 15, "stop spp0 start") == 0);
    // a clock output turned on while playing, far into the song: its pulses start from there at once (not the whole
    // song's pulses in one buffer)
    midi_out::Out late;
    int64_t far = (int64_t)sr * 600;
    midi_out::Buffer lb;
    lb.frames = 480, lb.sr = sr, lb.map = map.get();
    midi_out::Part lp{0, 480, true, far};
    late.process(lb, &lp, 1);  // (no clock outputs yet)
    late.setClocks({{9, false, 0.0}});
    lp.start = far + 480;
    late.process(lb, &lp, 1);
    auto lv = taken(late);
    CHECK(lv.size() <= 2);  // (10 ms at 120 BPM: a pulse at most, give or take)
    // Song Position Pointer is 14 bits: far into a long song it is clamped, never wrapped
    v = run(true, (int64_t)sr * 3600, 480);
    CHECK(names(v, 3).find("spp16383") != std::string::npos);
}

// ================================================================ External synth notes ahead by their delay
void testExternalTiming() {
    Project project;
    project.timing.bpm = 120;
    const int sr = 48000;
    TrackPtr t = project.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->ext.on = true;
    t->ext.port = "Synth";
    t->ext.channel = 3;
    t->ext.input = 0;
    t->ext.delayMs = 10;  // 480 frames
    ClipPtr c = makeMidiClip("notes", {note(0, 0.1, 60), note(0.5, 0.6, 64, 90), note(1.0, 1.95, 67, 80)}, 2.0, sr);
    c->track = t;
    c->prepare(sr);
    project.addClip(c);
    auto map = project.timing.map();
    auto list = project.tracks.get();
    auto clips = project.clips.get();
    midi_out::Out out;
    out.setTracks({{t.get(), 7, 2}});
    double now = 50;
    std::vector<midi_out::Event> all;
    auto run = [&](bool moving, int64_t start, int frames, bool loops = false, int64_t ls = 0, int64_t le = 0) {
        midi_out::Buffer mb;
        mb.now = now;
        mb.outLatency = 0.03;
        mb.frames = frames;
        mb.sr = sr;
        mb.map = map.get();
        mb.tracks = list.get();
        mb.clips = clips.get();
        mb.loops = loops, mb.loopStart = ls, mb.loopEnd = le;
        midi_out::Part p{0, frames, moving, start};
        out.process(mb, &p, 1);
        now += (double)frames / sr;
        auto v = taken(out);
        all.insert(all.end(), v.begin(), v.end());
        return v;
    };
    // playing from 0: the note at 0 is in the first `delay`: sent at once (late); the others 10 ms early, exactly
    double t0 = now;
    int64_t pos = 0;
    for (; pos < sr * 2; pos += 256) run(true, pos, 256);
    std::vector<std::pair<double, std::string>> ons;
    for (auto& e : all)
        if ((e.b[0] & 0xF0) == midi_out::NOTE_ON) ons.push_back({e.when, std::to_string(e.b[1])});
    CHECK(ons.size() == 3);
    bool channel = true;
    for (auto& e : all) channel = channel && e.slot == 7 && ((e.b[0] & 0x0F) == 2 || e.b[0] >= 0xF0);
    CHECK(channel);
    if (ons.size() == 3) {
        CHECK_NEAR(ons[0].first, t0, 1e-9);                // (at once)
        CHECK_NEAR(ons[1].first, t0 + 0.5 - 0.010, 1e-9);  // 10 ms early
        CHECK_NEAR(ons[2].first, t0 + 1.0 - 0.010, 1e-9);
        CHECK(ons[1].second == "64" && ons[2].second == "67");
    }
    // stopping: the note still held (67 until 1.95 s… stopped before) gets its off, and All Notes Off
    all.clear();
    pos = sr * 1.5;
    run(true, pos, 256);  // (a jump to 1.5 s: 67 is let go there)
    all.clear();
    run(false, 0, 256);
    bool allOff = false;
    for (auto& e : all) allOff = allOff || ((e.b[0] & 0xF0) == midi_out::CONTROL && e.b[1] == 123);
    CHECK(allOff);
    // a loop from 0.4 s to 0.9 s: the note at 0.5 s comes round, and so does the one at the loop's start…
    all.clear();
    int64_t ls = (int64_t)(0.4 * sr), le = (int64_t)(0.9 * sr);
    pos = ls;
    int ons64 = 0, jumps = 0;
    double firstWrap = -1, wrapNow = -1;
    for (int i = 0; i < 400; i++) {
        int frames = 300;
        if (pos + frames > le) {  // (the engine's parts: up to the loop's end, then from its start)
            int n = (int)(le - pos);
            midi_out::Buffer mb;
            mb.now = now;
            mb.frames = frames;
            mb.sr = sr;
            mb.map = map.get();
            mb.tracks = list.get();
            mb.clips = clips.get();
            mb.loops = true, mb.loopStart = ls, mb.loopEnd = le;
            midi_out::Part parts[2] = {{0, n, true, pos}, {n, frames - n, true, ls}};
            out.process(mb, parts, 2);
            if (wrapNow < 0) wrapNow = now + (double)n / sr;
            now += (double)frames / sr;
            pos = ls + (frames - n);
            jumps++;
            for (auto& e : taken(out)) all.push_back(e);
        } else {
            run(true, pos, frames, true, ls, le);
            pos += frames;
        }
    }
    for (auto& e : all)
        if ((e.b[0] & 0xF0) == midi_out::NOTE_ON && e.b[1] == 64) {
            ons64++;
            if (ons64 == 2 && firstWrap < 0) firstWrap = e.when;
        }
    CHECK(jumps >= 2);
    CHECK(ons64 >= 3);  // (the first pass, and each time round)
    // the second time 64 sounds: 0.1 s after the loop went round, sent 10 ms early (before the jump itself)
    CHECK_NEAR(firstWrap, wrapNow + 0.1 - 0.010, 1e-6);
    // every note on gets its note off (none left hanging round the loop)
    std::map<int, int> balance;
    for (auto& e : all) {
        if ((e.b[0] & 0xF0) == midi_out::NOTE_ON) balance[e.b[1]]++;
        if ((e.b[0] & 0xF0) == midi_out::NOTE_OFF) balance[e.b[1]]--;
    }
    all.clear();
    run(false, 0, 256);
    for (auto& e : all)
        if ((e.b[0] & 0xF0) == midi_out::NOTE_OFF) balance[e.b[1]]--;
    bool balanced = true;
    for (auto& [p, n] : balance) balanced = balanced && n == 0;
    CHECK(balanced);
    // no input (heard from the synth itself): sent as heard (the device's delay later), less its delay
    t->ext.input = -1;
    t->ext.delayMs = 0;
    all.clear();
    double t1 = now;
    for (pos = 0; pos < sr; pos += 256) run(true, pos, 256);
    ons.clear();
    for (auto& e : all)
        if ((e.b[0] & 0xF0) == midi_out::NOTE_ON) ons.push_back({e.when, ""});
    CHECK(ons.size() >= 2);
    if (ons.size() >= 2) CHECK_NEAR(ons[1].first, t1 + 0.5 + 0.03, 1e-9);
    // the track no longer an External synth: its notes let go
    run(false, 0, 256);
    out.setTracks({});
    all.clear();
    run(false, 0, 256);
    CHECK(all.size() == 1 && (all[0].b[0] & 0xF0) == midi_out::CONTROL);

    // Measure: the synth's sound comes back 333 frames after its note (a stand-in synth: the test)
    midi_out::Out m;
    m.measure(5, 0, 1);
    int64_t stream = 0, noteAt = -1;
    std::vector<float> in;
    for (int b = 0; b < 200 && m.measuring() != midi_out::Out::Measuring::Done; b++) {
        const int frames = 256;
        in.assign(frames * 2, 0.f);
        for (int i = 0; i < frames; i++) in[(size_t)i * 2 + 1] = 0.004f * (float)std::sin(i * 0.3);  // (its noise)
        if (noteAt >= 0)
            for (int i = 0; i < frames; i++)
                if (stream + i >= noteAt + 333) in[(size_t)i * 2 + 1] = 0.5f;
        midi_out::Buffer mb;
        mb.now = b * 0.01;
        mb.frames = frames;
        mb.sr = sr;
        mb.in = in.data();
        mb.inChannels = 2;
        midi_out::Part p{0, frames, false, 0};
        m.process(mb, &p, 1);
        for (auto& e : taken(m))
            if ((e.b[0] & 0xF0) == midi_out::NOTE_ON && e.slot == 5) noteAt = stream;  // (sent at this buffer's start)
        stream += frames;
    }
    CHECK(m.measuring() == midi_out::Out::Measuring::Done);
    CHECK(m.measuredFrames() == 333);
    // nothing comes back: it gives up, the note let go
    midi_out::Out silent;
    silent.measure(5, 0, 1);
    bool off = false;
    for (int b = 0; b < 600 && silent.measuring() != midi_out::Out::Measuring::Failed; b++) {
        in.assign(512, 0.f);
        midi_out::Buffer mb;
        mb.frames = 256;
        mb.sr = sr;
        mb.in = in.data();
        mb.inChannels = 2;
        midi_out::Part p{0, 256, false, 0};
        silent.process(mb, &p, 1);
        for (auto& e : taken(silent)) off = off || (e.b[0] & 0xF0) == midi_out::NOTE_OFF;
    }
    CHECK(silent.measuring() == midi_out::Out::Measuring::Failed && off);
}

// the External synth's sound: the device's inputs through the track, in the engine's own callback
void testExternalSound() {
    Project project;
    audio::Engine engine(project);
    engine.offline = true;
    engine.inputs = 4;
    TrackPtr t = project.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->ext.on = true;
    t->ext.port = "Synth";
    t->ext.input = 2;  // In 3/4
    t->ext.stereo = true;
    auto out = std::make_shared<midi_out::Out>();
    engine.setMidiOut(out);
    // its sound comes back by monitoring's input path: ui/recording turns monitoring on for it, as for a track that
    // hears its input, and off once it is gone
    CHECK(mixer::usesInput(*t) && !mixer::hearsInput(*t));
    Recording rec(RecordingHost{}, engine);
    rec.tick();
    CHECK(engine.monitoring());
    const int frames = 256;
    std::vector<float> in((size_t)frames * 4), buf((size_t)frames * engine.channels);
    for (int i = 0; i < frames; i++) {
        in[(size_t)i * 4 + 2] = 0.25f;   // left
        in[(size_t)i * 4 + 3] = -0.125f; // right
        in[(size_t)i * 4 + 0] = 0.9f;    // (another input: not heard)
    }
    engine.callback(in.data(), buf.data(), frames);
    CHECK_NEAR(buf[100 * engine.channels], 0.25, 1e-6);
    CHECK_NEAR(buf[100 * engine.channels + 1], -0.125, 1e-6);
    t->ext.on = false;  // (the built-in synth again: no input heard)
    engine.callback(in.data(), buf.data(), frames);
    CHECK_NEAR(buf[100 * engine.channels], 0, 1e-9);
    t->ext.on = true;
    t->muted = true;
    engine.callback(in.data(), buf.data(), frames);
    CHECK_NEAR(buf[100 * engine.channels], 0, 1e-9);
    t->ext.input = -1;  // (heard from the synth itself: no input wanted)
    rec.tick();
    CHECK(!engine.monitoring());
    engine.setMidiOut(nullptr);
}

// ================================================================ unplugged while playing
void testUnplugged() {
    Project project;
    audio::Engine engine(project);
    TrackPtr track = project.addTrack(Kind::Midi);
    track->needsInstrument = false;
    TrackPtr ext = project.addTrack(Kind::Midi);
    ext->needsInstrument = false;
    ext->ext.on = true;
    ext->ext.port = "Fake out 0";
    ext->midiIn = "Fake in 1";  // (listening to the other keyboard only)
    midi_input::MidiInput input(engine, project, [&] { return track; }, nullptr);
    auto fake = std::make_unique<Fake>();
    Fake* f = fake.get();
    f->add("Fake in 0", true);
    f->add("Fake in 1", true);
    f->add("Fake out 0", false);
    input.hub.addBackend(std::move(fake));
    input.hub.start(false);
    input.hub.setOn("Fake out 0", false, true);
    int in0 = input.hub.find("Fake in 0", true)->slot, in1 = input.hub.find("Fake in 1", true)->slot;
    int out0 = input.hub.find("Fake out 0", false)->slot;
    std::vector<midi_in::Event> recorded;
    input.recorder = [&](const midi_in::Event& e) { recorded.push_back(e); };
    // keyboard 0 plays the selected track (its input: All); keyboard 1 the External synth (its input: that port)
    f->play(in0, {0x90, 60, 100}, monoNow());
    f->play(in0, {0xB0, 64, 127}, monoNow());  // (its pedal down)
    f->play(in1, {0x92, 48, 90}, monoNow());
    input.poll();
    auto live = engine.liveNotes(track.get());
    CHECK(live->active());
    for (int i = 0; i < 100 && f->count() < 1; i++) sleepMs(2);
    {
        std::lock_guard<std::mutex> g(f->lock);
        // (on the External synth's own channel, 1)
        CHECK(f->sent.size() == 1 && f->sent[0].slot == out0 && f->sent[0].b[0] == 0x90 && f->sent[0].b[1] == 48);
    }
    // keyboard 0 goes away while its note (held by the pedal) sounds: no crash, its notes let go, greyed
    f->remove("Fake in 0");
    input.poll();
    const midi_ports::Port* p = input.hub.find("Fake in 0", true);
    CHECK(p && !p->present && !p->open && p->on);
    CHECK(p && p->label().find("not connected") != std::string::npos);
    int offs = 0;
    for (auto& e : recorded) offs += e.type == Event::NoteOff && e.data1 == 60;
    CHECK(offs == 1);  // (the recording sees it end)
    // events from it still in flight are dropped, not played
    f->play(in0, {0x90, 61, 100}, monoNow());
    size_t before = recorded.size();
    input.poll();
    CHECK(recorded.size() == before);
    // the External synth's output goes away while a note sounds there: sending to it is skipped, no crash
    f->remove("Fake out 0");
    input.poll();
    CHECK(!input.hub.find("Fake out 0", false)->open);
    f->play(in1, {0x82, 48, 0}, monoNow());
    input.poll();
    uint8_t m[3] = {0x90, 1, 1};
    input.hub.sendNow(out0, m, 3);  // (a slot whose port is gone)
    sleepMs(20);
    // both come back: opened again by themselves, the same slots
    f->add("Fake in 0", true);
    f->add("Fake out 0", false);
    input.hub.tick(1e6);
    CHECK(input.hub.find("Fake in 0", true)->open && input.hub.find("Fake in 0", true)->slot == in0);
    CHECK(input.hub.find("Fake out 0", false)->open);
    f->play(in0, {0x90, 62, 100}, monoNow());
    input.poll();
    CHECK(!recorded.empty() && recorded.back().data1 == 62);
    input.close();
    CHECK(!input.hub.running());
}

// ================================================================ JACK's transport, against a fake JACK
struct FakeJack : midi_sync::JackTransport {
    midi_sync::JackState s;
    bool isMaster = false, otherMaster = false;
    int starts = 0, stops = 0, locates = 0;
    bool query(midi_sync::JackState& out) override {
        out = s;
        out.bbt = otherMaster;
        return true;
    }
    void start() override { s.rolling = true, starts++; }
    void stop() override { s.rolling = false, stops++; }
    void locate(int64_t frame) override { s.frame = frame, locates++; }
    bool setMaster(const Timing* t) override { return isMaster = t && !otherMaster; }
    bool master() const override { return isMaster; }
};

void testJack() {
    FakeJack jack;
    jack.s.rate = 44100;  // (JACK at another rate than the engine)
    bool playing = false;
    int64_t pos = 0;
    double tempo = 0;
    midi_sync::JackFollower::Host h;
    h.sr = 48000;
    h.playing = [&] { return playing; };
    h.pos = [&] { return pos; };
    h.play = [&] { playing = true; };
    h.stop = [&] { playing = false; };
    h.seek = [&](int64_t f) { pos = f; };
    h.tempo = [&](double b) { tempo = b; };
    midi_sync::JackFollower f;
    double now = 0;
    auto tick = [&](double dt) {
        now += dt;
        if (playing) pos += (int64_t)(dt * 48000);
        if (jack.s.rolling) jack.s.frame += (int64_t)(dt * 44100);
        f.tick(jack, h, now);
    };
    tick(0);
    CHECK(!playing && jack.starts == 0);
    // another app starts JACK at 2 s: Exbit plays from there
    jack.s.rolling = true;
    jack.s.frame = 2 * 44100;
    tick(0.016);
    CHECK(playing && std::llabs(pos - 2 * 48000) < 48000 * 0.02);
    for (int i = 0; i < 30; i++) tick(0.016);
    CHECK(jack.starts == 0 && jack.stops == 0 && jack.locates == 0);  // (nothing sent back)
    // another app locates JACK to 10 s: followed
    jack.s.frame = 10 * 44100;
    tick(0.016);
    CHECK(std::llabs(pos - 10 * 48000) < 48000 * 0.02);
    // and stops it
    jack.s.rolling = false;
    tick(0.016);
    CHECK(!playing);
    // Exbit's user moves the playhead and plays: JACK located and started
    pos = 30 * 48000;
    tick(0.016);
    CHECK(jack.locates == 1 && std::llabs(jack.s.frame - 30 * 44100) <= 1);
    playing = true;
    tick(0.016);
    CHECK(jack.starts == 1 && jack.s.rolling);
    for (int i = 0; i < 30; i++) tick(0.016);
    CHECK(jack.starts == 1 && jack.locates == 1 && playing);  // (no echo back and forth)
    // …and stops: JACK stopped
    playing = false;
    tick(0.016);
    CHECK(jack.stops == 1 && !jack.s.rolling);
    // drift while both roll: Exbit's playhead follows JACK's
    playing = true;
    tick(0.016);
    for (int i = 0; i < 5; i++) tick(0.016);
    jack.s.frame += 44100 / 4;  // (JACK 0.25 s ahead, as by another clock)
    tick(0.016);
    CHECK(std::llabs(pos - jack.s.frame * 48000 / 44100) < 48000 * 0.02);
    // another app is the timebase master: its tempo followed; Exbit isn't master
    jack.otherMaster = true;
    jack.s.bpm = 133.5;
    CHECK(!jack.setMaster(nullptr) || true);
    tick(0.016);
    CHECK(tempo == 133.5);
    CHECK(!jack.setMaster((const Timing*)&jack));  // (another app is: Exbit can't be)
    // BBT as Exbit tells it (timebase master): 300 BPM, 3/4 from bar 2
    TempoMap map(300, 4, {[] {
                     TempoChange c;
                     c.bar = 2, c.bpm = 300, c.beatsPerBar = 3;
                     return c;
                 }()});
    jack::Position p{};
    midi_sync::fillBBT(map, (uint32_t)(48000 * (4 + 1.5) * 0.2), 48000, p);  // 5.5 beats: bar 2, beat 2, half a beat
    CHECK(p.valid & jack::PositionBBT);
    CHECK(p.bar == 2 && p.beat == 2 && p.tick == 960);
    CHECK(p.beats_per_bar == 3.0f && p.beats_per_minute == 300 && p.ticks_per_beat == 1920);
    CHECK(p.bar_start_tick == 4 * 1920);
}

// ================================================================ MIDI clock in
void testClockIn() {
    midi_sync::ClockFollower f;
    std::vector<midi_sync::Action> acts;
    auto ev = [](Event::Type t, double time, int value = 0) {
        Event e;
        e.type = t;
        e.time = time;
        e.bend = (int16_t)value;
        return e;
    };
    f.event(ev(Event::Start, 0), acts);
    CHECK(acts.size() == 2 && acts[0].kind == midi_sync::Action::Locate && acts[0].beat == 0 &&
          acts[1].kind == midi_sync::Action::Play);
    CHECK(f.bpm() == 0);
    // 300 BPM with up to ±1 ms of jitter on each pulse
    double per = 60.0 / 300 / 24;
    uint32_t seed = 7;
    for (int k = 0; k < 200; k++) {
        seed = seed * 1103515245 + 12345;
        double jitter = ((int)((seed >> 8) % 2001) - 1000) * 1e-6;
        f.event(ev(Event::Clock, k * per + jitter), acts);
    }
    printf("clock in: 300 BPM with ±1 ms jitter read as %.3f BPM\n", f.bpm());
    CHECK(std::fabs(f.bpm() - 300) < 1.5);
    acts.clear();
    f.event(ev(Event::Stop, 10), acts);
    CHECK(acts.size() == 1 && acts[0].kind == midi_sync::Action::Stop && !f.running());
    acts.clear();
    f.event(ev(Event::SongPos, 11, 22), acts);  // (stopped: kept for Continue)
    CHECK(acts.empty());
    f.event(ev(Event::Continue, 12), acts);
    CHECK(acts.size() == 2 && acts[0].kind == midi_sync::Action::Locate && acts[0].beat == 5.5);
    acts.clear();
    f.event(ev(Event::SongPos, 13, 64), acts);  // (running: a locate at once)
    CHECK(acts.size() == 1 && acts[0].beat == 16);
    // Stop, then Continue without a new Song Position: on from where it stopped (no locate)
    acts.clear();
    f.event(ev(Event::Stop, 14), acts);
    acts.clear();
    f.event(ev(Event::Continue, 15), acts);
    CHECK(acts.size() == 1 && acts[0].kind == midi_sync::Action::Play);
    // a pause in the clock: the tempo starts afresh
    f.event(ev(Event::Clock, 100), acts);
    CHECK(f.bpm() == 0);
}

// ================================================================ preferences, and a track's settings saved and undone
void testSaved() {
    util::makeDirs("build/test-midi-ports");
    settings::setPath("build/test-midi-ports/settings.json");
    midi_ports::Hub hub;
    auto fake = std::make_unique<Fake>();
    fake->add("Keys", true);
    fake->add("Synth", false);
    hub.addBackend(std::move(fake));
    hub.start(false);
    int changes = 0;
    hub.changed = [&] { changes++; };
    hub.setClock("Keys", true, true);
    hub.setOn("Synth", false, true);
    hub.setClock("Synth", false, true);
    hub.setAlwaysStart("Synth", true);
    hub.setOffsetMs("Synth", -12.5);
    hub.setOffsetMs("Synth", 1e9);  // (clamped)
    hub.setOn("Gone", true, false);  // (remembered, not there)
    CHECK(changes == 6);
    Preferences p = Preferences::load();
    p.midiPorts = hub.settings().dump();
    p.sync = "MIDI clock";
    p.save();
    settings::flush();
    Preferences q = Preferences::load();
    CHECK(q.midiPorts == p.midiPorts && q.sync == "MIDI clock");
    midi_ports::Hub again;
    std::string err;
    again.load(Json::parse(q.midiPorts, &err));
    const midi_ports::Port* keys = again.find("Keys", true);
    const midi_ports::Port* synth = again.find("Synth", false);
    CHECK(keys && keys->on && keys->clock && !keys->present);
    CHECK(synth && synth->on && synth->clock && synth->alwaysStart && synth->offsetMs == midi_ports::MAX_OFFSET_MS);
    CHECK(again.find("Gone", true) && !again.find("Gone", true)->on);
    CHECK(midi_sync::modeFrom(q.sync) == midi_sync::Mode::MidiClock && midi_sync::modeFrom("nonsense") == midi_sync::Mode::Internal);
    // a broken setting reads as nothing
    midi_ports::Hub broken;
    broken.load(Json::parse("{\"in\": [1, 2], \"out\": {\"\": 3, \"X\": {\"offset_ms\": \"many\"}}}", &err));
    CHECK(broken.find("X", false) && broken.find("X", false)->offsetMs == 0);

    // a track's MIDI input and External synth: saved and loaded, undone and redone
    Project project;
    const int sr = 48000;
    history::History h(project, sr);
    TrackPtr t = project.addTrack(Kind::Midi);
    t->needsInstrument = false;
    h.check(sr);
    t->midiIn = "Keys";
    t->midiInChannel = 10;
    h.check(sr);
    t->ext.on = true;
    t->ext.port = "Synth";
    t->ext.channel = 16;
    t->ext.input = 2;
    t->ext.stereo = false;
    t->ext.delayMs = 7.5;
    h.check(sr);
    std::string path = "build/test-midi-ports/song.json";
    std::string error;
    CHECK(project_io::saveProject(path, project, sr, project_io::MasterSettings{}, error));
    Project loaded;
    project_io::MasterSettings master;
    CHECK(project_io::loadProject(path, loaded, sr, master, error));
    CHECK(loaded.tracks.size() == 1);
    if (loaded.tracks.size() == 1) {
        TrackPtr l = loaded.tracks[0];
        CHECK(l->midiIn == "Keys" && l->midiInChannel == 10);
        CHECK(l->ext == t->ext);
    }
    CHECK(h.undo(sr));
    CHECK(!t->ext.on && t->ext.port.empty() && t->midiIn == "Keys");
    CHECK(h.undo(sr));
    CHECK(t->midiIn.empty() && t->midiInChannel == 0);
    CHECK(h.redo(sr) && h.redo(sr));
    CHECK(t->ext.on && t->ext.delayMs == 7.5 && t->midiInChannel == 10);
    // older files (without these keys) load with the defaults; the track writes none of them while unset
    TrackPtr plain = project.addTrack(Kind::Midi);
    Json j = project_io::describe(project, sr, project_io::MasterSettings{});
    std::string text = j.dump();
    CHECK(text.find("midi_in") != std::string::npos);
    size_t n = 0;
    for (size_t at = text.find("external_synth"); at != std::string::npos; at = text.find("external_synth", at + 1)) n++;
    CHECK(n == 1);
    (void)plain;
}

}  // namespace

int main() {
    util::setDebug(false);
    testDense();
    testClock();
    testTransportMessages();
    testExternalTiming();
    testExternalSound();
    testUnplugged();
    testJack();
    testClockIn();
    testSaved();
    TEST_MAIN_END();
}
