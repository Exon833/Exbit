// Recording several tracks at once (ui/recording.h RECORDING SEVERAL TRACKS), at its worst: 32 armed tracks
// recording round a loop for a simulated hour (memory bounded, nothing allocated on the audio thread, every take
// on its frame), arming and disarming while recording (punching in and out), monitoring In / Auto / Off, low-latency
// monitoring leaving out a look-ahead plugin, MIDI round the loop (Merge and Takes), no input device, saving,
// loading and undo, and the terminal's keys.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>

#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"
#include "model/track_ops.h"
#include "platform/platform.h"
#include "plugins/plugin.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/main_window.h"
#include "ui/lane_panel.h"
#include "ui/preferences.h"
#include "ui/recording.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

// --- allocations counted while gCount is on (this program's own operator new) ---
static std::atomic<bool> gCount{false};
static std::atomic<long> gAllocs{0};
void* operator new(size_t n) {
    if (gCount.load(std::memory_order_relaxed)) gAllocs++;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

const std::string TMP = "build/ui-samples/multi_record";

// a look-ahead effect: its output is its input `lat` frames late
class LateFx : public plug::Plugin {
public:
    int lat = 64;
    int processCalls = 0;
    std::vector<float> ring[2];
    size_t pos = 0;
    bool isVst2() const override { return false; }
    std::string name() const override { return "Look-ahead"; }
    std::string vendor() const override { return "Tests"; }
    std::string subcategories() const override { return "Fx"; }
    bool isInstrument() const override { return false; }
    bool hasSidechain() const override { return false; }
    int latency() const override { return lat; }
    double sampleRate() const override { return 48000; }
    void prepare(double, int) override {}
    void reset() override {}
    void setTransport(double, int, int, bool, int64_t, double) override {}
    void process(const float* const in[2], const float* const[2], float* const out[2], int frames) override {
        processCalls++;
        for (int c = 0; c < 2; c++)
            if ((int)ring[c].size() != lat) ring[c].assign((size_t)lat, 0.0f);
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                float d = ring[c][pos];
                ring[c][pos] = in[c][i];
                out[c][i] = d;
            }
            pos = (pos + 1) % (size_t)lat;
        }
    }
    void render(const std::vector<plug::NoteMessage>&, float* const[2], int) override {}
    void notesOff() override {}
    bool holding() const override { return false; }
    std::vector<plug::ParamInfo> parameters() override { return {}; }
    double getParam(uint32_t) override { return 0; }
    void setParam(uint32_t, double) override {}
    std::string paramText(uint32_t, double) override { return ""; }
    std::vector<uint32_t> edited() override { return {}; }
    void windowEdit(uint32_t, double) override {}
    void idle() override {}
    std::string componentState() override { return ""; }
    std::string controllerState() override { return ""; }
    void setState(const std::string&, const std::string&) override {}
    bool openEditor(uintptr_t, void*, double, int&, int&) override { return false; }
    void closeEditor() override {}
    bool editorOpen() const override { return false; }
    bool hasEditor() override { return false; }
    bool editorResizable() override { return false; }
    bool requestedSize(int&, int&) override { return false; }
    void editorResized(int&, int&) override {}
};

// an offline engine at `sr` with `inputs` device inputs (the tests call callback() by hand)
void offline(audio::Engine& e, int sr, int inputs) {
    e.offline = true;
    e.sr = sr;
    e.inputs = inputs;
    e.metering->levelsOn = false;
    e.metering->loudnessOn = false;
}

TrackPtr audioTrack(Project& p, const std::string& name, int input, bool stereo = false) {
    TrackPtr t = p.addTrack(Kind::Audio);
    t->name = name;
    t->inputCh = input;
    t->inputStereo = stereo;
    return t;
}

// a buffer of the device's inputs: every channel's sample says which frame it is (frame * 8 + channel)
void fillInputs(std::vector<float>& in, int64_t first, int frames, int channels) {
    in.resize((size_t)frames * channels);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) in[(size_t)i * channels + c] = (float)((first + i) * 8 + c);
}

// --- 32 armed tracks, round a loop, for an hour ---
void testAnHourOf32Tracks() {
    const int SR = 100, IN = 8, TRACKS = 32, BLOCK = 100;  // (a low rate: an hour's frames in seconds of work)
    const int64_t HOUR = 3600LL * SR, LOOP_START = 100, LOOP_END = 400;
    Project project;
    audio::Engine engine(project);
    offline(engine, SR, IN);
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < TRACKS; i++) {
        TrackPtr t = audioTrack(project, "T" + std::to_string(i + 1), i % IN);
        t->armed = true;  // (monitoring Auto: each hears its input while armed, as it records)
        tracks.push_back(t);
    }
    patch::ensureStates(project);
    engine.setLoop(LOOP_START, LOOP_END);
    Preferences prefs;
    prefs.loopTakes = true;
    RecordingHost host;
    host.prefs = &prefs;
    host.offsetMs = [] { return 0.0; };
    Recording rec(host, engine);
    rec.tick();  // (monitoring on: the tracks are armed)
    CHECK(engine.monitoring());
    CHECK(rec.start() && rec.active() && rec.tracks().size() == (size_t)TRACKS);
    auto set = engine.recorders();
    CHECK(set && set->recorders.size() == (size_t)TRACKS);
    std::vector<float> in, out((size_t)BLOCK * 2);
    int64_t fed = 0;
    long allocs = 0;
    size_t worstMemory = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int64_t b = 0; fed < HOUR; b++) {
        fillInputs(in, fed, BLOCK, IN);
        gAllocs = 0;
        gCount = b >= 2;  // (the first buffers make the mixer's states: once)
        engine.callback(in.data(), out.data(), BLOCK);
        gCount = false;
        allocs += gAllocs;
        fed += BLOCK;
        if (b % 10 == 9) engine.idle();  // (the GUI's frames: every 10 s of audio here, the ring holds 20)
        if (b % 600 == 599)
            for (auto& r : set->recorders) worstMemory = std::max(worstMemory, r->memoryBytes());
    }
    double busy = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // the input heard through each track (its own channel), while recording
    CHECK(std::fabs(out[0]) > 0);
    engine.idle();
    for (auto& r : set->recorders) {
        CHECK(r->dropped.load() == 0);
        CHECK(r->frames() == HOUR);
        // (the take's own samples, a block at most more, and the ring: nothing grows but the take)
        size_t bound = ((size_t)HOUR + audio::Recorder::BLOCK + r->ringSamples()) * sizeof(float);
        CHECK(r->memoryBytes() <= bound);
        worstMemory = std::max(worstMemory, r->memoryBytes());
    }
    std::printf("32 tracks for an hour (simulated at %d Hz): %.2f s of work, %ld allocations on the audio thread, "
                "%.1f MB per track at most\n", SR, busy, allocs, worstMemory / 1e6);
    CHECK(allocs == 0);
    rec.finish();
    CHECK(!rec.active() && !engine.recordingAudio() && !engine.playing);
    // every pass a take on its frame: the first from 0 to the loop's end, then each the loop, at its start
    const int64_t LEN = LOOP_END - LOOP_START;
    const size_t passes = 1 + (size_t)((HOUR - LOOP_END + LEN - 1) / LEN);
    for (int i = 0; i < TRACKS; i++) {
        const TrackPtr& t = tracks[(size_t)i];
        CHECK(t->takes.size() == passes);
        if (t->takes.size() != passes) continue;
        int64_t from = 0;  // (the frame of the input each take began at)
        bool aligned = true;
        for (size_t k = 0; k < passes; k++) {
            const Clip& c = *(*t->takes)[k];
            int64_t at = k == 0 ? 0 : LOOP_START;
            int64_t frames = c.source ? c.source->frames() : 0;
            int64_t want = k == 0 ? LOOP_END : std::min(LEN, HOUR - from);
            float first = frames ? c.source->frame(0)[0] : -1, last = frames ? c.source->frame(frames - 1)[1] : -1;
            if (c.start != at || frames != want || first != (float)(from * 8 + i % IN) ||
                last != (float)((from + frames - 1) * 8 + i % IN))
                aligned = false;
            from += frames;
        }
        CHECK(aligned && from == HOUR);
    }
    // the last pass plays on each track where they overlap (it's shorter: before it, the first pass's start; after
    // it, the pass before's end)
    CHECK(project.clipsOn(tracks[0]).size() == 3);
}

// --- arming and disarming while recording: punching in and out ---
void testPunchInAndOut() {
    const int SR = 1000, IN = 4, BLOCK = 100;
    Project project;
    audio::Engine engine(project);
    offline(engine, SR, IN);
    TrackPtr a = audioTrack(project, "A", 0), b = audioTrack(project, "B", 2, true), keys = project.addTrack(Kind::Midi);
    keys->name = "Keys";
    keys->needsInstrument = false;
    patch::ensureStates(project);
    a->armed = true;
    std::string said;
    RecordingHost host;
    host.showMessage = [&](const std::string& t, int) { said = t; };
    host.midiInputReady = [] { return true; };
    std::vector<std::pair<TrackPtr, int64_t>> shown;
    host.showRecording = [&](const std::vector<std::pair<TrackPtr, int64_t>>& s) { shown = s; };
    Recording rec(host, engine);
    CHECK(rec.start() && rec.tracks().size() == 1 && shown.size() == 1);
    CHECK(said.find("onto A") != std::string::npos);
    std::vector<float> in, out((size_t)BLOCK * 2);
    int64_t fed = 0;
    auto run = [&](int64_t frames) {
        for (int64_t end = fed + frames; fed < end; fed += BLOCK) {
            fillInputs(in, fed, BLOCK, IN);
            engine.callback(in.data(), out.data(), BLOCK);
        }
        engine.idle();
    };
    run(SR);  // 1 s of A
    rec.setArmed({b, keys}, true);  // B punched in (a stereo pair), Keys too
    CHECK(b->armed && rec.tracks().size() == 3 && shown.size() == 3);
    CHECK(said.find("Punched in") != std::string::npos);
    run(SR);
    rec.setArmed({a}, false);  // A punched out: its 2 s placed now
    CHECK(!a->armed && rec.tracks().size() == 2);
    auto clipsA = project.clipsOn(a);
    CHECK(clipsA.size() == 1 && clipsA[0]->source && clipsA[0]->source->frames() == 2 * SR && clipsA[0]->start == 0);
    rec.setArmed({b}, false);
    rec.setArmed({b}, true);  // (out and in again: a second take)
    run(SR / 2);
    rec.setArmed({a, b, keys}, false);  // nothing armed: the transport goes on
    CHECK(rec.active() && engine.playing && rec.tracks().empty());
    run(SR / 2);
    CHECK(!rec.finish());
    auto clipsB = project.clipsOn(b);
    CHECK(b->takes.size() == 2);
    if (b->takes.size() == 2) {
        const Clip& first = *(*b->takes)[0];
        CHECK(first.start == SR && first.source->frames() == SR);  // (from where it was punched in, to where out)
        CHECK(first.source->frame(0)[0] == (float)(SR * 8 + 2) && first.source->frame(0)[1] == (float)(SR * 8 + 3));
        CHECK((*b->takes)[1]->source->frames() == SR / 2);
    }
    CHECK(project.clipsOn(keys).empty());  // (nothing played on Keys: no take)
    // an armed track whose input the device lacks is left out (and said); one with None too
    TrackPtr c = audioTrack(project, "C", 7), d = audioTrack(project, "D", -1);
    a->armed = c->armed = d->armed = true;
    CHECK(rec.start() && rec.tracks().size() == 1);
    CHECK(said.find("left out") != std::string::npos && said.find("C's In 8") != std::string::npos &&
          said.find("D has no input") != std::string::npos);
    rec.finish();
    // recording MIDI alone, then an audio track armed: it punches in too
    a->armed = c->armed = d->armed = false;
    keys->armed = true;
    CHECK(rec.start() && rec.recordingMidi() && !engine.recordingAudio());
    run(SR / 2);
    rec.setArmed({a}, true);
    CHECK(engine.recordingAudio() && rec.tracks().size() == 2);
    run(SR / 2);
    size_t before = a->takes.size();
    rec.finish();
    CHECK(a->takes.size() == before + 1 && (*a->takes)[before]->source->frames() == SR / 2);
}

// --- every ● toggled at once, over and over, while 32 tracks record at the real rate ---
void testArmStorm() {
    const int SR = 48000, IN = 2, TRACKS = 32, BLOCK = 256;
    Project project;
    audio::Engine engine(project);
    offline(engine, SR, IN);
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < TRACKS; i++) {
        TrackPtr t = audioTrack(project, "S" + std::to_string(i + 1), 0, i % 2 == 1);
        t->armed = true;
        tracks.push_back(t);
    }
    patch::ensureStates(project);
    Recording rec(RecordingHost{}, engine);
    rec.tick();
    CHECK(rec.start());
    std::vector<float> in, out((size_t)BLOCK * 2);
    int64_t fed = 0;
    long allocs = 0;
    const int ROUNDS = 20;
    auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < ROUNDS; round++) {
        for (int b = 0; b < 20; b++, fed += BLOCK) {  // (about a tenth of a second each round)
            fillInputs(in, fed, BLOCK, IN);
            gAllocs = 0;
            gCount = b > 1;
            engine.callback(in.data(), out.data(), BLOCK);
            gCount = false;
            allocs += gAllocs;
            if (b % 4 == 3) engine.idle();
        }
        rec.setArmed(tracks, false);  // all at once out…
        engine.callback(in.data(), out.data(), BLOCK);
        rec.setArmed(tracks, true);   // …and in again
    }
    rec.finish();
    double busy = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double audio = (double)fed / SR;
    std::printf("32 tracks at %d Hz, every ● toggled %d times: %.2f s of work for %.2f s of audio, %ld allocations "
                "on the audio thread\n", SR, ROUNDS, busy, audio, allocs);
    CHECK(allocs == 0);
    CHECK(busy < audio * 4);  // (with the takes placed each time; the audio thread's share is far less)
    bool all = true;
    for (auto& t : tracks) all = all && t->takes.size() == (size_t)ROUNDS;  // (a take each round; the last punch-in, with nothing yet: none)
    CHECK(all);
    CHECK(!engine.recordingAudio() && !rec.active());
}

// --- monitoring In / Auto / Off; a recorded track's clips muted ---
void testMonitoring() {
    const int SR = 1000, BLOCK = 100;
    Project project;
    audio::Engine engine(project);
    offline(engine, SR, 2);
    TrackPtr t = audioTrack(project, "Vox", 1);
    auto source = std::make_shared<AudioBuffer>();
    source->samples.assign((size_t)SR * 10 * 2, 0.25f);
    ClipPtr clip = makeAudioClip("clip", source, SR, SR);
    clip->track = t;
    clip->start = 0;
    project.addClip(clip);
    patch::ensureStates(project);
    std::string error;
    CHECK(engine.setMonitoring(true, error) && engine.monitoring());
    std::vector<float> in((size_t)BLOCK * 2), out((size_t)BLOCK * 2);
    for (int i = 0; i < BLOCK; i++) in[(size_t)i * 2] = 0.1f, in[(size_t)i * 2 + 1] = 0.5f;  // (In 2: 0.5)
    auto heard = [&] {
        engine.callback(in.data(), out.data(), BLOCK);
        return out[(size_t)(BLOCK / 2) * 2];  // (the left output, mid-buffer)
    };
    t->monitor = Track::Monitor::Auto;
    CHECK(std::fabs(heard()) < 1e-6);  // (not armed)
    t->armed = true;
    CHECK(mixer::hearsInput(*t) && std::fabs(heard() - 0.5f) < 1e-6);
    t->monitor = Track::Monitor::Off;
    CHECK(!mixer::hearsInput(*t) && std::fabs(heard()) < 1e-6);
    t->armed = false;
    t->monitor = Track::Monitor::In;
    CHECK(std::fabs(heard() - 0.5f) < 1e-6);
    t->inputStereo = false, t->inputCh = 0;  // (In 1, mono: on both sides)
    CHECK(std::fabs(heard() - 0.1f) < 1e-6 && std::fabs(out[(size_t)(BLOCK / 2) * 2 + 1] - 0.1f) < 1e-6);
    t->muted = true;
    CHECK(std::fabs(heard()) < 1e-6);
    t->muted = false;
    t->inputCh = 5;  // (an input the device lacks: silence, nothing read past its channels)
    CHECK(std::fabs(heard()) < 1e-6);
    t->inputCh = 1;
    // playing: the clip and the input; recording onto it: the input alone (its clips muted)
    engine.seek(SR);
    engine.play();
    CHECK(std::fabs(heard() - 0.75f) < 1e-5);
    engine.stop();
    Recording rec(RecordingHost{}, engine);
    t->armed = true;
    CHECK(rec.start());
    CHECK(std::fabs(heard() - 0.5f) < 1e-5);
    rec.finish();
    // monitoring through the track's fader
    t->volumeDb = -6.0206;
    CHECK(std::fabs(heard() - 0.25f) < 1e-3);
    CHECK(engine.setMonitoring(false, error) && !engine.monitoring() && std::fabs(heard()) < 1e-6);
}

// --- low-latency monitoring: a look-ahead plugin on the recorded track left out while recording ---
void testLowLatency() {
    const int SR = 1000, BLOCK = 100;
    Project project;
    audio::Engine engine(project);
    offline(engine, SR, 1);
    TrackPtr t = audioTrack(project, "Gtr", 0);
    auto fx = std::make_shared<LateFx>();
    auto slot = std::make_shared<PluginSlot>();
    slot->name = "Look-ahead";
    slot->kind = "effect";
    slot->instance = fx;
    t->effects = std::vector<SlotPtr>{slot};
    t->cables = chain({slot});
    t->armed = true;
    patch::ensureStates(project);
    std::string error;
    CHECK(engine.setMonitoring(true, error));
    std::vector<float> in((size_t)BLOCK, 0.5f), out((size_t)BLOCK * 2);
    engine.callback(in.data(), out.data(), BLOCK);
    CHECK(engine.latency.load() == 64 && fx->processCalls == 1 && out[0] == 0.0f);  // (held back by it)
    Preferences prefs;
    prefs.lowLatencyMonitoring = true;
    std::string said;
    RecordingHost host;
    host.prefs = &prefs;
    host.showMessage = [&](const std::string& s, int) { said = s; };
    Recording rec(host, engine);
    CHECK(rec.start());
    CHECK(slot->lowLatencyBypass.load() && rec.lowLatencyBypassed().size() == 1 && !slot->bypass);
    CHECK(said.find("Low-latency monitoring: 1 effect") != std::string::npos);
    engine.callback(in.data(), out.data(), BLOCK);
    engine.callback(in.data(), out.data(), BLOCK);
    CHECK(engine.latency.load() == 0 && fx->processCalls == 1);  // (left out: not processed, nothing held back)
    CHECK(std::fabs(out[0] - 0.5f) < 1e-6);                      // (heard at once)
    rec.finish();
    CHECK(!slot->lowLatencyBypass.load() && rec.lowLatencyBypassed().empty());
    engine.callback(in.data(), out.data(), BLOCK);
    CHECK(engine.latency.load() == 64 && fx->processCalls == 2);
    // off: it stays in while recording
    prefs.lowLatencyMonitoring = false;
    CHECK(rec.start() && !slot->lowLatencyBypass.load());
    rec.finish();
}

// --- MIDI round the loop: Merge and Takes ---
void testMidiLoop() {
    Project project;
    audio::Engine engine(project);
    const int sr = engine.sr;
    TrackPtr keys = project.addTrack(Kind::Midi);
    keys->name = "Keys";
    keys->needsInstrument = false;
    Preferences prefs;
    RecordingHost host;
    host.prefs = &prefs;
    std::string said;
    host.showMessage = [&](const std::string& s, int) { said = s; };
    audio::LoopRange loop{sr, 2 * sr};  // 1 s … 2 s
    auto s = [&](double seconds) { return seconds * sr; };
    auto play = [&](Recording& rec) {
        rec.beginMidiTake(keys, sr, loop);
        rec.midiNoteAt(60, 100, s(1.25));  // pass 1
        rec.midiNoteAt(60, 0, s(1.5));
        rec.midiNoteAt(62, 90, s(1.9));    // (held over the loop's end: cut there)
        rec.follow(s(1.99));
        rec.follow(s(1.02));               // pass 2
        rec.midiNoteAt(62, 0, s(1.1));
        rec.midiNoteAt(64, 100, s(1.5));
        rec.midiNoteAt(64, 0, s(1.6));
        rec.follow(s(1.95));
        rec.follow(s(1.05));               // pass 3: nothing played
        rec.follow(s(1.9));
        rec.follow(s(1.01));               // pass 4
        rec.midiNoteAt(67, 100, s(1.75));
        rec.midiNoteAt(67, 0, s(1.8));
        return rec.finishMidiAtFrame(s(1.85));
    };
    // Merge: every pass's notes in one clip over the loop
    prefs.midiLoopTakes = false;
    Recording merge(host, engine);
    ClipPtr clip = play(merge);
    CHECK(clip && clip->start == sr && std::fabs(clip->lengthS - 1.0) < 1e-9);
    if (clip && clip->notes && clip->notes->size() == 4) {
        const auto& n = *clip->notes;
        CHECK(n[0].pitch == 60 && std::fabs(n[0].start - 0.25) < 1e-6 && std::fabs(n[0].end - 0.5) < 1e-6);
        CHECK(n[1].pitch == 64 && std::fabs(n[1].start - 0.5) < 1e-6 && std::fabs(n[1].end - 0.6) < 1e-6);
        CHECK(n[2].pitch == 67 && std::fabs(n[2].start - 0.75) < 1e-6 && std::fabs(n[2].end - 0.8) < 1e-6);
        CHECK(n[3].pitch == 62 && std::fabs(n[3].start - 0.9) < 1e-6 && std::fabs(n[3].end - 1.0) < 1e-6);
    } else {
        CHECK(false);
    }
    CHECK(keys->takes.size() == 1 && said.find("merged") != std::string::npos);
    CHECK(project.clipsOn(keys).size() == 1);
    // Takes: each pass a take (the silent one left out); the last plays
    prefs.midiLoopTakes = true;
    project.removeClips(project.clipsOn(keys));
    keys->takes = std::vector<ClipPtr>{};
    Recording takes(host, engine);
    ClipPtr last = play(takes);
    CHECK(keys->takes.size() == 3);
    if (keys->takes.size() == 3) {
        const Clip& p1 = *(*keys->takes)[0];
        const Clip& p2 = *(*keys->takes)[1];
        const Clip& p4 = *(*keys->takes)[2];
        CHECK(p1.start == sr && p2.start == sr && p4.start == sr);
        CHECK(p1.notes->size() == 2 && (*p1.notes)[1].pitch == 62 && std::fabs((*p1.notes)[1].end - 1.0) < 1e-6);
        CHECK(p2.notes->size() == 1 && (*p2.notes)[0].pitch == 64 && std::fabs((*p2.notes)[0].start - 0.5) < 1e-6);
        CHECK(p4.notes->size() == 1 && (*p4.notes)[0].pitch == 67 && std::fabs(p4.lengthS - 0.85) < 1e-6);
    }
    CHECK(last && last->notes && last->notes->size() == 1 && (*last->notes)[0].pitch == 67);
    CHECK(said.find("3 takes round the loop") != std::string::npos);
    // starting before the loop: Merge's clip from there
    prefs.midiLoopTakes = false;
    project.removeClips(project.clipsOn(keys));
    Recording early(host, engine);
    early.beginMidiTake(keys, sr / 2, loop);
    early.midiNoteAt(48, 100, s(0.75));
    early.midiNoteAt(48, 0, s(0.8));
    early.follow(s(1.9));
    early.follow(s(1.0));
    early.midiNoteAt(50, 100, s(1.2));
    early.midiNoteAt(50, 0, s(1.3));
    ClipPtr whole = early.finishMidiAtFrame(s(1.4));
    CHECK(whole && whole->start == sr / 2 && std::fabs(whole->lengthS - 1.5) < 1e-9 && whole->notes->size() == 2);
    if (whole && whole->notes->size() == 2) CHECK(std::fabs((*whole->notes)[1].start - 0.7) < 1e-6);
    // the engine goes round the loop while recording MIDI only with loop recording on
    engine.offline = true;
    engine.setLoop(100, 300);
    std::vector<audio::Engine::Part> parts;
    engine.recordingMidi = true;
    engine.seek(250);
    engine.play();
    engine.advance(100, parts);
    CHECK(parts.size() == 1 && engine.pos.load() == 350);  // (on past the loop, as before)
    engine.stop();
    engine.loopRecording = true;
    engine.seek(250);
    engine.play();
    engine.advance(100, parts);
    CHECK(parts.size() == 2 && engine.pos.load() == 150);  // (round it)
    engine.stop();
    engine.recordingMidi = false;
    engine.loopRecording = false;
}

// --- no input device, no audio device: said, nothing breaks ---
void testNoInputDevice() {
    Project project;
    audio::Engine engine(project);
    offline(engine, 1000, 0);  // (a device without inputs)
    TrackPtr t = audioTrack(project, "Vox", 0);
    t->armed = true;
    RecordingHost host;
    std::string said;
    host.showMessage = [&](const std::string& s, int) { said = s; };
    Recording rec(host, engine);
    static std::string seen;
    seen.clear();
    ui::App::get().singleShot(30, [] {
        ui::Window* top = ui::App::get().modalWindow();
        if (!top) return;
        std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
            if (auto* l = dynamic_cast<ui::Label*>(w)) seen += l->text() + "\n";
            for (auto& c : w->children()) walk(c.get());
        };
        walk(top);
        if (auto* d = dynamic_cast<ui::Dialog*>(top)) d->reject();
    });
    CHECK(!rec.start() && !rec.active() && !engine.playing && !engine.recordingAudio());
    CHECK(seen.find("None of the armed tracks can be recorded") != std::string::npos &&
          seen.find("no inputs") != std::string::npos);
    // monitoring without any audio device open: said once, not tried again every frame
    Project p2;
    audio::Engine closed(p2);
    TrackPtr m = audioTrack(p2, "Mic", 0);
    m->monitor = Track::Monitor::In;
    RecordingHost h2;
    int told = 0;
    h2.showMessage = [&](const std::string& s, int) {
        if (s.find("Monitoring: the input can't be heard") == 0) told++;
    };
    Recording rec2(h2, closed);
    for (int i = 0; i < 10; i++) rec2.tick();
    CHECK(told == 1 && !closed.monitoring());
    m->monitor = Track::Monitor::Off;
    rec2.tick();
    m->monitor = Track::Monitor::In;
    rec2.tick();
    CHECK(told == 2);
    // a device that fails mid-way: closing it stops monitoring and recording without a crash
    std::string error;
    CHECK(engine.setMonitoring(true, error));
    engine.close();
    CHECK(!engine.monitoring() && !engine.recordingAudio());
}

// --- saved, loaded, undone ---
void testSaveLoadUndo() {
    const int SR = 48000;
    Project project;
    TrackPtr a = project.addTrack(Kind::Audio), b = project.addTrack(Kind::Audio), k = project.addTrack(Kind::Midi);
    a->name = "A", b->name = "B", k->name = "K";
    a->armed = true;
    a->monitor = Track::Monitor::In;
    a->inputCh = 2, a->inputStereo = true;
    b->monitor = Track::Monitor::Off;
    b->inputCh = -1;
    k->armed = true;
    std::string path = TMP + "/armed.json", error;
    project_io::MasterSettings master;
    CHECK(project_io::saveProject(path, project, SR, master, error));
    std::string text;
    CHECK(util::readFile(path, text));
    CHECK(text.find("\"input\": \"3/4\"") != std::string::npos && text.find("\"input\": \"none\"") != std::string::npos);
    Project loaded;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, loaded, SR, m2, error));
    CHECK(loaded.tracks.size() == 3);
    if (loaded.tracks.size() == 3) {
        const Track& la = *loaded.tracks[0];
        const Track& lb = *loaded.tracks[1];
        const Track& lk = *loaded.tracks[2];
        CHECK(la.armed && la.monitor == Track::Monitor::In && la.inputCh == 2 && la.inputStereo);
        CHECK(!lb.armed && lb.monitor == Track::Monitor::Off && lb.inputCh == -1 && !lb.inputStereo);
        CHECK(lk.armed && lk.monitor == Track::Monitor::Auto && lk.inputCh == 0);
    }
    // an older file (none of them): the defaults; and they're only written when set
    Project fresh;
    fresh.addTrack(Kind::Audio);
    CHECK(project_io::saveProject(path, fresh, SR, master, error) && util::readFile(path, text));
    CHECK(text.find("\"armed\"") == std::string::npos && text.find("\"monitor\"") == std::string::npos &&
          text.find("\"input\"") == std::string::npos);
    // undo: monitoring and the input are steps; arming isn't (as how the project is worked)
    history::History h(project, SR);
    a->monitor = Track::Monitor::Auto;
    CHECK(h.check(SR) && h.labels().back() == "Monitoring");
    b->inputCh = 0;
    CHECK(h.check(SR) && h.labels().back() == "Track input");
    a->armed = false;
    CHECK(!h.check(SR));
    CHECK(h.undo(SR) && b->inputCh == -1);
    CHECK(h.undo(SR) && a->monitor == Track::Monitor::In && !a->armed);
    // a copy of a track keeps its monitoring and input, not its arming
    a->armed = true;
    TrackPtr copy = track_ops::copyOf(*a);
    CHECK(!copy->armed && copy->monitor == a->monitor && copy->inputCh == 2 && copy->inputStereo);
}

// --- the terminal's keys (tui/app.cpp): Shift+R arms, H the monitoring, Shift+H the input ---
void testTuiKeys() {
    using namespace tui;
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    std::vector<Key> queue;
    size_t at = 0;
    auto step = [&](const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            f.key(queue[at++]);
        }
    };
    app.nestedLoop = [&](const std::function<bool()>& done) { step(done); };
    Screen screen(110, 30);
    auto keys = [&](std::vector<Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) app.advanceTimers(0.05), app.tick();
        f.draw(screen);
    };
    keys({Key{plat::Key_Tab}, Key{'A', 0, "a"}, Key{'A', 0, "a"}});  // (the track panel; two audio tracks)
    CHECK(f.pane == Front::Pane::Panel && w->project.tracks.size() == 2);
    TrackPtr second = w->project.tracks.size() == 2 ? w->project.tracks[1] : nullptr;
    if (!second) return;
    f.setCurrent(second);
    keys({Key{'R', plat::ModShift, "R"}});
    CHECK(second->armed && screen.lines().find("●") != std::string::npos);
    keys({Key{'H', 0, "h"}});
    if (second->monitor != Track::Monitor::In || screen.lines().find("●» Audio 2") == std::string::npos)
        std::printf("monitor %d\n%s\n", (int)second->monitor, screen.lines().c_str());
    CHECK(second->monitor == Track::Monitor::In && screen.lines().find("●» Audio 2") != std::string::npos);
    keys({Key{'H', 0, "h"}});
    CHECK(second->monitor == Track::Monitor::Off);
    keys({Key{'H', 0, "h"}});
    CHECK(second->monitor == Track::Monitor::Auto);
    // Shift+H: the input, from a menu (no device here: None alone)
    keys({Key{'H', plat::ModShift, "H"}});
    CHECK(f.menuOpen());
    keys({Key{plat::Key_Return}});
    CHECK(!f.menuOpen() && second->inputCh == -1);
    // the first track armed too (Shift+R adds), then the second disarmed
    f.setCurrent(w->project.tracks[0]);
    keys({Key{'R', plat::ModShift, "R"}});
    CHECK(w->project.tracks[0]->armed && second->armed);
    f.setCurrent(second);
    keys({Key{'R', plat::ModShift, "R"}});
    CHECK(!second->armed && w->project.tracks[0]->armed);
    // the F1 list and the status line say so
    keys({Key{plat::Key_F1}});
    std::string help = screen.lines();
    keys({Key{plat::Key_Escape}});
    bool listed = help.find("Shift+R") != std::string::npos;
    for (int i = 0; i < 12 && !listed; i++) {  // (the list scrolls)
        keys({Key{plat::Key_F1}, Key{plat::Key_PageDown}});
        listed = screen.lines().find("Shift+R") != std::string::npos;
        keys({Key{plat::Key_Escape}});
    }
    CHECK(listed);
    f.draw(screen);
    CHECK(screen.lines().find("Shift+R: arm") != std::string::npos);
    w->close();
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testAnHourOf32Tracks();
    testPunchInAndOut();
    testArmStorm();
    testMonitoring();
    testLowLatency();
    testMidiLoop();
    testNoInputDevice();
    testSaveLoadUndo();
    testTuiKeys();
    TEST_MAIN_END();
}
