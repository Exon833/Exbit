// Tests of the mixer (mixer.h) and the engine (engine.h): clips with their edge fades, mute / solo /
// groups, output pairs, delay compensation, parallel patches, sidechains, the master patch, frozen
// tracks, the loop and the count-in, the metronome, instruments' notes, and a short real run of a
// device (silent).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

#include "audio/automation.h"
#include "audio/devices.h"
#include "audio/metronome.h"
#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "model/model.h"
#include "plugins/plugin.h"

// --- a fake plugin (tests only): a gain effect with a configurable latency (a real delay), an
// echo (a tail without latency), sidechain ducking; or an instrument playing 0.1 per held note ---
namespace {
class FakePlugin : public plug::Plugin {
public:
    bool instrument = false;
    int lat = 0;            // reported latency = the delay it applies
    bool sidechain = false; // has a sidechain input; ducks by it: out *= 1 - min(1, |side|)
    int echo = 0;           // frames: adds half its input this much later (no latency reported)
    bool throwOnProcess = false;
    double sr = 48000;
    int processCalls = 0, notesOffCalls = 0, resets = 0, prepares = 0;
    std::vector<plug::NoteMessage> received;
    std::vector<uint32_t> editedIds;
    double params[4] = {1.0, 0.0, 0.0, 0.0};
    int held = 0;
    std::vector<float> ring[2], echoRing[2];
    size_t rpos = 0, epos = 0;
    double tempo = 0;
    bool transportPlaying = false;

    bool isVst2() const override { return false; }
    std::string name() const override { return instrument ? "Fake Synth" : "Fake Fx"; }
    std::string vendor() const override { return "Tests"; }
    std::string subcategories() const override { return instrument ? "Instrument" : "Fx"; }
    bool isInstrument() const override { return instrument; }
    bool hasSidechain() const override { return sidechain; }
    int latency() const override { return lat; }
    double sampleRate() const override { return sr; }
    void prepare(double rate, int) override { sr = rate, prepares++; }
    void reset() override {
        resets++;
        held = 0;
        for (int c = 0; c < 2; c++) ring[c].clear(), echoRing[c].clear();
    }
    void setTransport(double t, int, int, bool playing, int64_t) override { tempo = t, transportPlaying = playing; }
    void process(const float* const in[2], const float* const side[2], float* const out[2], int frames) override {
        processCalls++;
        if (throwOnProcess) throw std::runtime_error("fake failure");
        for (int c = 0; c < 2; c++) {
            if ((int)ring[c].size() != lat) ring[c].assign(lat, 0.0f);
            if ((int)echoRing[c].size() != echo) echoRing[c].assign(echo, 0.0f);
        }
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                float x = (float)(in[c][i] * params[0]);
                if (side && sidechain) x *= 1.0f - std::min(1.0f, std::fabs(side[0][i]));
                if (echo) {
                    float e = echoRing[c][epos];
                    echoRing[c][epos] = x;
                    x += 0.5f * e;
                }
                if (lat) {
                    float d = ring[c][rpos];
                    ring[c][rpos] = x;
                    x = d;
                }
                out[c][i] = x;
            }
            if (lat) rpos = (rpos + 1) % lat;
            if (echo) epos = (epos + 1) % echo;
        }
    }
    void render(const std::vector<plug::NoteMessage>& notes, float* const out[2], int frames) override {
        processCalls++;
        if (throwOnProcess) throw std::runtime_error("fake synth failure");
        size_t k = 0;
        for (int i = 0; i < frames; i++) {
            for (; k < notes.size() && notes[k].frame <= i; k++) {
                received.push_back(notes[k]);
                held = std::max(0, held + (notes[k].on ? 1 : -1));
            }
            out[0][i] = out[1][i] = 0.1f * held;
        }
    }
    void notesOff() override { notesOffCalls++, held = 0; }
    bool holding() const override { return held > 0; }
    std::vector<plug::ParamInfo> parameters() override {
        std::vector<plug::ParamInfo> v(4);
        v[0].id = 0, v[0].name = "Gain";
        v[1].id = 1, v[1].name = "Mode", v[1].stepCount = 2;
        v[2].id = 2, v[2].name = "MIDI CC 1";
        v[3].id = 3, v[3].name = "P001";
        return v;
    }
    double getParam(uint32_t id) override { return id < 4 ? params[id] : 0; }
    void setParam(uint32_t id, double v) override {
        if (id < 4) params[id] = v;
    }
    std::string paramText(uint32_t, double v) override { return std::to_string(v); }
    std::vector<uint32_t> edited() override {
        auto e = editedIds;
        editedIds.clear();
        return e;
    }
    void windowEdit(uint32_t id, double v) override { setParam(id, v), editedIds.push_back(id); }
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

const int SR = 48000;

[[maybe_unused]] std::shared_ptr<FakePlugin> fakeFx(int lat = 0) {
    auto p = std::make_shared<FakePlugin>();
    p->lat = lat;
    return p;
}

SlotPtr slotOf(std::shared_ptr<FakePlugin> p) {
    auto s = std::make_shared<PluginSlot>();
    s->name = p->name();
    s->kind = p->instrument ? "instrument" : "effect";
    s->instance = p;
    return s;
}

[[maybe_unused]] ClipPtr audioClip(const TrackPtr& t, int64_t start, int64_t frames, float value) {
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, value);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "clip";
    c->track = t;
    c->start = start;
    c->data = buf;
    c->source = buf;
    c->sourceSr = SR;
    return c;
}

// mix [start, start + frames) of the project into an interleaved buffer of `channels`
[[maybe_unused]] std::vector<float> mixRange(Project& p, int64_t start, int64_t frames, int channels, mixer::MixState& st,
                            int block = 256, int* latency = nullptr) {
    std::vector<float> out((size_t)frames * channels, 0.0f);
    for (int64_t pos = start; pos < start + frames; pos += block) {
        int n = (int)std::min<int64_t>(block, start + frames - pos);
        int l = mixer::mixClips(p, out.data() + (pos - start) * channels, channels, n, true, pos, st, nullptr, SR,
                                pos == start);
        if (latency) *latency = l;
    }
    return out;
}
}  // namespace

static void testClipsAndFades() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 1000, 0.5f));
    p.addClip(audioClip(b, 0, 1000, 0.25f));
    mixer::MixState st;
    auto out = mixRange(p, 0, 1024, 2, st);
    CHECK_NEAR(out[0], 0.75 / 64, 1e-6);       // the 64-frame fade at the clip's start
    CHECK_NEAR(out[1], 0.75 / 64, 1e-6);
    CHECK_NEAR(out[62 * 2], 0.75 * 63 / 64, 1e-6);
    CHECK_NEAR(out[100 * 2], 0.75, 1e-6);
    CHECK_NEAR(out[999 * 2], 0.75 / 64, 1e-6);  // ... and at its end
    CHECK_NEAR(out[1000 * 2], 0.0, 1e-9);
    // a clip window: offset and length
    Project q;
    auto t = q.addTrack(Kind::Audio);
    auto buf = std::make_shared<AudioBuffer>();
    for (int i = 0; i < 1000; i++) buf->samples.push_back((float)i), buf->samples.push_back((float)-i);
    auto c = audioClip(t, 500, 10, 0);
    c->data = buf;
    c->offset = 200;
    c->length = 300;
    q.addClip(c);
    mixer::MixState st2;
    auto o = mixRange(q, 0, 1024, 2, st2);
    CHECK_NEAR(o[499 * 2], 0, 1e-9);
    CHECK_NEAR(o[600 * 2], 300.0, 1e-3);   // data frame 200 + 100
    CHECK_NEAR(o[600 * 2 + 1], -300.0, 1e-3);
    CHECK_NEAR(o[500 * 2], 200.0 / 64, 1e-4);
    CHECK_NEAR(o[800 * 2], 0, 1e-9);
    // disabled clips are silent
    c->disabled = true;
    o = mixRange(q, 0, 1024, 2, st2);
    CHECK_NEAR(o[600 * 2], 0, 1e-9);
}

static void testMuteSoloGroups() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 1000, 0.5f));
    p.addClip(audioClip(b, 0, 1000, 0.25f));
    mixer::MixState st;
    auto at100 = [&]() { return mixRange(p, 0, 256, 2, st)[100 * 2]; };
    a->muted = true;
    CHECK_NEAR(at100(), 0.25, 1e-6);
    a->muted = false;
    b->soloed = true;
    CHECK_NEAR(at100(), 0.25, 1e-6);
    a->soloed = true;
    CHECK_NEAR(at100(), 0.75, 1e-6);
    a->soloed = b->soloed = false;
    auto g = p.groupTracks({a});
    g->muted = true;
    CHECK_NEAR(at100(), 0.25, 1e-6);
    g->muted = false;
    g->soloed = true;
    CHECK_NEAR(at100(), 0.5, 1e-6);
    g->soloed = false;
    b->volumeDb = -6.0;
    CHECK_NEAR(at100(), 0.5 + 0.25 * std::pow(10.0, -6.0 / 20), 1e-5);
    b->volumeDb = config::MIN_DB;
    CHECK_NEAR(at100(), 0.5, 1e-6);
}

static void testOutputPairs() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 1000, 0.5f));
    p.addClip(audioClip(b, 0, 1000, 0.25f));
    b->output = 2;
    mixer::MixState st;
    auto o = mixRange(p, 0, 256, 4, st);
    CHECK_NEAR(o[100 * 4 + 0], 0.5, 1e-6);
    CHECK_NEAR(o[100 * 4 + 1], 0.5, 1e-6);
    CHECK_NEAR(o[100 * 4 + 2], 0.25, 1e-6);
    CHECK_NEAR(o[100 * 4 + 3], 0.25, 1e-6);
    mixer::MixState st2;
    o = mixRange(p, 0, 256, 2, st2);  // the device lacks outputs 3/4: on 1/2
    CHECK_NEAR(o[100 * 2], 0.75, 1e-6);
    mixer::MixState st3;
    o = mixRange(p, 0, 256, 3, st3);  // only output 3: the left channel
    CHECK_NEAR(o[100 * 3 + 2], 0.25, 1e-6);
    CHECK_NEAR(o[100 * 3 + 0], 0.5, 1e-6);
}

static void testDelayCompensation() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    auto fx = fakeFx(100);
    auto s = slotOf(fx);
    a->effects = std::vector<SlotPtr>{s};
    a->cables = chain({s});
    p.addClip(audioClip(a, 1000, 2000, 1.0f));
    p.addClip(audioClip(b, 1000, 2000, 1.0f));
    CHECK(plugin_audio::trackLatency(*a, *p.tracks) == 100);
    CHECK(plugin_audio::trackLatency(*b, *p.tracks) == 0);
    mixer::MixState st;
    int lat = -1;
    auto o = mixRange(p, 0, 2048, 2, st, 256, &lat);
    CHECK(lat == 100);
    CHECK_NEAR(o[1099 * 2], 0.0, 1e-9);      // both come out 100 frames late, together
    CHECK_NEAR(o[1100 * 2], 2.0 / 64, 1e-6);
    CHECK_NEAR(o[1163 * 2], 2.0, 1e-6);
    // without compensation the other track is early
    Project q;
    auto c = q.addTrack(Kind::Audio), d = q.addTrack(Kind::Audio);
    auto s2 = slotOf(fakeFx(100));
    c->effects = std::vector<SlotPtr>{s2};
    c->cables = chain({s2});
    q.addClip(audioClip(c, 1000, 2000, 1.0f));
    q.addClip(audioClip(d, 1000, 2000, 1.0f));
    mixer::MixState none;
    none.compensate = false;
    o = mixRange(q, 0, 2048, 2, none, 256, &lat);
    CHECK(lat == 0);
    CHECK_NEAR(o[1000 * 2], 1.0 / 64, 1e-6);
    CHECK_NEAR(o[1100 * 2], 1.0 + 1.0 / 64, 1e-6);  // (d in full, c's first frame)
    // a bypassed effect neither delays nor processes
    s->bypass = true;
    CHECK(plugin_audio::trackLatency(*a, *p.tracks) == 0);
    s->bypass = false;
    // a slot named by the cables that is no longer among the track's effects is never touched
    a->effects = std::vector<SlotPtr>{};
    CHECK(plugin_audio::trackLatency(*a, *p.tracks) == 0);
}

static void testParallelPatch() {
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto s = slotOf(fakeFx(64));
    t->effects = std::vector<SlotPtr>{s};
    t->cables = std::vector<Cable>{Cable(End::in(), End::out()), Cable(End::in(), End::of(s.get())),
                                   Cable(End::of(s.get()), End::out())};
    p.addClip(audioClip(t, 1000, 2000, 1.0f));
    patch::ensureStates(p);
    mixer::MixState st;
    int lat = -1;
    auto o = mixRange(p, 0, 2048, 2, st, 256, &lat);
    CHECK(lat == 64);
    CHECK_NEAR(o[1000 * 2], 0.0, 1e-9);  // the dry path is held back to line up with the effect
    CHECK_NEAR(o[1063 * 2], 0.0, 1e-9);
    CHECK_NEAR(o[1064 * 2], 2.0 / 64, 1e-6);
    CHECK_NEAR(o[1500 * 2], 2.0, 1e-6);
    // an effect whose output doesn't reach OUT isn't processed
    auto fx2 = fakeFx(0);
    auto s2 = slotOf(fx2);
    t->effects = std::vector<SlotPtr>{s, s2};
    t->cables = std::vector<Cable>{Cable(End::in(), End::out()), Cable(End::in(), End::of(s2.get()))};
    o = mixRange(p, 0, 2048, 2, st, 256, &lat);
    CHECK(fx2->processCalls == 0);
    CHECK(lat == 0);
    CHECK_NEAR(o[1500 * 2], 1.0, 1e-6);
    // nothing wired to OUT: silent
    t->cables = std::vector<Cable>{Cable(End::in(), End::of(s.get()))};
    o = mixRange(p, 0, 2048, 2, st, 256, &lat);
    CHECK_NEAR(o[1500 * 2], 0.0, 1e-9);
}

static void testSidechain() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 4000, 0.5f));
    p.addClip(audioClip(b, 0, 4000, 0.5f));
    auto fx = fakeFx(0);
    fx->sidechain = true;
    auto s = slotOf(fx);
    a->effects = std::vector<SlotPtr>{s};
    mixer::MixState st;
    CHECK_NEAR(mixRange(p, 0, 2048, 2, st)[1000 * 2], 1.0, 1e-6);
    a->cables = std::vector<Cable>{Cable(End::in(), End::of(s.get())), Cable(End::of(s.get()), End::out()),
                                   Cable(End::source(b.get()), End::sidechain(s.get()))};
    b->muted = true;  // a muted "ghost" track still feeds the sidechain
    mixer::feedingOrder(*p.tracks, *p.masterCables, st);
    CHECK(st.order.size() == 2 && st.order[0] == 1 && st.order[1] == 0);
    CHECK(st.feeder[1] && !st.feeder[0]);
    auto o = mixRange(p, 0, 2048, 2, st);
    CHECK_NEAR(o[1000 * 2], 0.25, 1e-6);  // 0.5 ducked by half
    CHECK_NEAR(o[0], 0.5 / 64 * (1 - 0.5 / 64), 1e-6);
    // the listening track is as late as the one it listens to
    auto s2 = slotOf(fakeFx(30));
    b->effects = std::vector<SlotPtr>{s2};
    b->cables = chain({s2});
    CHECK(plugin_audio::trackLatency(*a, *p.tracks) == 30);
    // a track that is gone counts as silence
    Project q;
    auto c = q.addTrack(Kind::Audio);
    q.addClip(audioClip(c, 0, 4000, 0.5f));
    auto fxq = fakeFx(0);
    fxq->sidechain = true;
    auto sq = slotOf(fxq);
    c->effects = std::vector<SlotPtr>{sq};
    c->cables = std::vector<Cable>{Cable(End::in(), End::of(sq.get())), Cable(End::of(sq.get()), End::out()),
                                   Cable(End::source(b.get()), End::sidechain(sq.get()))};
    mixer::MixState st2;
    CHECK_NEAR(mixRange(q, 0, 2048, 2, st2)[1000 * 2], 0.5, 1e-6);
}

static void testMasterPatch() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 4000, 1.0f));
    p.addClip(audioClip(b, 0, 4000, 1.0f));
    b->output = 2;
    auto fx = fakeFx(10);
    fx->params[0] = 0.5;
    auto s = slotOf(fx);
    p.masterEffects = std::vector<SlotPtr>{s};
    p.masterCables = chain({s});
    mixer::MixState st;
    int lat = -1;
    auto o = mixRange(p, 0, 1024, 4, st, 256, &lat);
    CHECK(lat == 10);
    CHECK_NEAR(o[9 * 4], 0.0, 1e-9);
    CHECK_NEAR(o[10 * 4], 0.5 / 64, 1e-6);
    CHECK_NEAR(o[500 * 4], 0.5, 1e-6);
    CHECK_NEAR(o[9 * 4 + 2], 0.0, 1e-9);  // outputs 3/4 are held back as long as the master effects
    CHECK_NEAR(o[10 * 4 + 2], 1.0 / 64, 1e-6);
    CHECK_NEAR(o[500 * 4 + 2], 1.0, 1e-6);
}

static void testFrozen() {
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto fx = fakeFx(50);
    fx->params[0] = 0;
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    p.addClip(audioClip(t, 0, 4000, 1.0f));
    auto f = audioClip(t, 0, 4000, 0.3f);
    t->frozen = f;
    CHECK(plugin_audio::trackLatency(*t, *p.tracks) == 0);
    mixer::MixState st;
    auto o = mixRange(p, 0, 1024, 2, st);
    CHECK_NEAR(o[100 * 2], 0.3, 1e-6);
    CHECK(fx->processCalls == 0);
    t->frozen = nullptr;  // (the frozen clip refers to its track)
}

static void testAdvance() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    std::vector<audio::Engine::Part> parts;
    e.setLoop(1000, 1500);
    e.seek(1400);
    e.play();
    e.advance(256, parts);
    CHECK(parts.size() == 2);
    CHECK(parts[0].offset == 0 && parts[0].frames == 100 && parts[0].hasStart && parts[0].start == 1400);
    CHECK(parts[1].offset == 100 && parts[1].frames == 156 && parts[1].start == 1000);
    CHECK(e.pos == 1156);
    // a loop's end right at the end of the buffer jumps too
    e.seek(1244);
    e.advance(256, parts);
    CHECK(parts.size() == 1 && parts[0].start == 1244 && parts[0].frames == 256);
    CHECK(e.pos == 1000);
    // playback that starts after the loop's end just carries on
    e.seek(2000);
    e.advance(256, parts);
    CHECK(parts.size() == 1 && e.pos == 2256);
    // count-in: the transport waits, then starts on the frame
    e.clearLoop();
    e.seek(5000);
    e.countIn = 300;
    e.advance(256, parts);
    CHECK(parts.size() == 1 && !parts[0].hasStart && parts[0].frames == 256);
    CHECK(e.pos == 5000 && e.countIn == 44);
    e.advance(256, parts);
    CHECK(parts.size() == 2 && !parts[0].hasStart && parts[0].frames == 44);
    CHECK(parts[1].hasStart && parts[1].offset == 44 && parts[1].frames == 212 && parts[1].start == 5000);
    CHECK(e.pos == 5212 && e.countIn == 0);
    e.stop();
}

static void testCallback() {
    Project p;
    auto t = p.addTrack(Kind::Audio);
    p.addClip(audioClip(t, 0, 10000, 0.5f));
    audio::Engine e(p);
    e.offline = true;
    std::vector<float> out(512 * 2);
    e.callback(nullptr, out.data(), 512);  // stopped: nothing plays
    CHECK(e.pos == 0);
    CHECK_NEAR(out[200], 0, 1e-9);
    e.play();
    e.callback(nullptr, out.data(), 512);
    CHECK(e.pos == 512);
    CHECK_NEAR(out[100 * 2], 0.5, 1e-6);
    std::vector<float> master;
    std::map<const Track*, std::vector<float>> tracks;
    e.metering->takePeaks(master, tracks);
    CHECK_NEAR(master[0], 0.5, 1e-6);
    CHECK(tracks.count(t.get()) == 1);
    e.masterGain = 0.5;
    e.callback(nullptr, out.data(), 512);
    CHECK_NEAR(out[100 * 2], 0.25, 1e-6);
    e.masterMute = true;
    e.callback(nullptr, out.data(), 512);
    CHECK_NEAR(out[100 * 2], 0, 1e-9);
    e.masterMute = false;
    e.masterGain = 1.0;
    CHECK(e.heardPos() == 1536);
    e.stop();
    CHECK(e.pos == 1536 && !e.playing);
    CHECK(e.lastError().empty());
}

static void testPreview() {
    Project p;
    audio::Engine e(p);
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign(200, 1.0f);  // 100 frames
    e.preview(data);
    std::vector<float> out(256 * 2);
    e.callback(nullptr, out.data(), 256);
    CHECK_NEAR(out[0], config::PREVIEW_GAIN, 1e-6);
    CHECK_NEAR(out[99 * 2 + 1], config::PREVIEW_GAIN, 1e-6);
    CHECK_NEAR(out[100 * 2], 0, 1e-9);
    CHECK(!e.previewVoice());  // done: forgotten
    e.preview(data, 0, true, 1.0);
    e.callback(nullptr, out.data(), 256);
    CHECK_NEAR(out[150 * 2], 1.0, 1e-6);  // looping: over and over
    CHECK(e.previewVoice() && e.previewVoice()->pos == 56);
    e.stopPreview();
    e.idle();
}

static void testMetronome() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    e.setMetronome(metronome::MetronomeSettings());
    auto clicks = metronome::renderClicks(metronome::MetronomeSettings(), e.sr);
    e.seek(0);
    e.play();
    std::vector<float> out(512 * 2);
    e.callback(nullptr, out.data(), 512);
    CHECK_NEAR(out[100 * 2], clicks.bar[100], 1e-6);  // the bar's click at frame 0
    CHECK_NEAR(out[100 * 2 + 1], clicks.bar[100], 1e-6);
    // the next beat: 120 BPM at 48 kHz = 24000 frames
    e.stop();
    e.seek(24000 - 100);
    e.play();
    e.callback(nullptr, out.data(), 512);
    CHECK_NEAR(out[99 * 2], 0, 1e-9);
    CHECK_NEAR(out[150 * 2], clicks.beat[50], 1e-6);
    // never before where playback started
    e.stop();
    e.seek(24000 + 50);
    e.play();
    e.callback(nullptr, out.data(), 512);
    CHECK_NEAR(out[0], 0, 1e-9);
    e.stop();
    e.setMetronome(std::nullopt);
}

static void testInstrumentNotes() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto synth = std::make_shared<FakePlugin>();
    synth->instrument = true;
    t->instrument = slotOf(synth);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->track = t;
    c->start = 1000;
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign(2000, 0.0f);  // 1000 frames
    c->data = data;
    c->events = std::make_shared<const std::vector<Clip::Event>>(
        std::vector<Clip::Event>{{100, 300, 60, 100}, {300, 400, 62, 0}, {1500, 1600, 64, 100}});
    using plugin_audio::MidiMsg;
    auto m = plugin_audio::noteMessages({c}, true, 1000, 256, true);
    CHECK(m.size() == 2 && m[0].type == MidiMsg::AllOff && m[1].type == MidiMsg::NoteOn && m[1].frame == 100 &&
          m[1].pitch == 60);
    m = plugin_audio::noteMessages({c}, true, 1256, 256, false);
    // at 300: the note-off of 60 before the note-on of 62 (velocity 0 played as 1)
    CHECK(m.size() == 3 && m[0].type == MidiMsg::NoteOff && m[0].frame == 44 && m[0].pitch == 60);
    CHECK(m[1].type == MidiMsg::NoteOn && m[1].frame == 44 && m[1].pitch == 62 && m[1].velocity == 1);
    CHECK(m[2].type == MidiMsg::NoteOff && m[2].frame == 144 && m[2].pitch == 62);
    c->length = 200;  // notes are cut at the end of the clip's window
    m = plugin_audio::noteMessages({c}, true, 1000, 256, false);
    CHECK(m.size() == 2 && m[0].frame == 100 && m[1].type == MidiMsg::NoteOff && m[1].frame == 200);
    m = plugin_audio::noteMessages({c}, false, 0, 256, true);  // stopped: only all notes off
    CHECK(m.size() == 1 && m[0].type == MidiMsg::AllOff);
    c->length = -1;
    p.addClip(c);
    // through the mixer
    mixer::MixState st;
    std::vector<float> out(256 * 2);
    CHECK(plugin_audio::playsPlugins(*t));
    mixer::mixClips(p, out.data(), 2, 256, true, 1000, st, nullptr, SR, true);
    CHECK(synth->notesOffCalls == 1);
    CHECK(synth->received.size() == 1 && synth->received[0].frame == 100 && synth->received[0].on);
    CHECK_NEAR(out[99 * 2], 0, 1e-9);
    CHECK_NEAR(out[200 * 2], 0.1, 1e-6);
    std::fill(out.begin(), out.end(), 0.0f);
    mixer::mixClips(p, out.data(), 2, 256, true, 1256, st, nullptr, SR, false);
    CHECK(synth->received.size() == 4 && !synth->received[1].on && synth->received[1].frame == 44);
    CHECK(synth->notesOffCalls == 1);
    // stopped with a note held: let go
    synth->held = 1;
    std::fill(out.begin(), out.end(), 0.0f);
    mixer::mixClips(p, out.data(), 2, 256, false, 0, st, nullptr, SR, false);
    CHECK(synth->notesOffCalls == 2);
    // the engine: all notes off when playback starts or jumps; plugins paused: none called
    audio::Engine e(p);
    e.offline = true;
    e.seek(1000);
    e.play();
    int offs = synth->notesOffCalls;
    e.callback(nullptr, out.data(), 256);
    CHECK(synth->notesOffCalls == offs + 1);
    e.callback(nullptr, out.data(), 256);
    CHECK(synth->notesOffCalls == offs + 1);
    e.seek(1000);
    e.callback(nullptr, out.data(), 256);
    CHECK(synth->notesOffCalls == offs + 2);
    {
        audio::Engine::PluginsPaused paused(e);
        CHECK(e.pluginsPaused);
        int calls = synth->processCalls;
        e.callback(nullptr, out.data(), 256);
        CHECK(synth->processCalls == calls);
        CHECK_NEAR(out[10], 0, 1e-9);
    }
    CHECK(!e.pluginsPaused);
    e.callback(nullptr, out.data(), 256);
    CHECK(synth->notesOffCalls == offs + 3);  // (their notes were missed)
    // a failing instrument is switched off, never the stream
    synth->throwOnProcess = true;
    e.callback(nullptr, out.data(), 256);
    CHECK(t->instrument->failed && t->instrument->error == "fake synth failure");
    CHECK(!plugin_audio::playsPlugins(*t));
    e.stop();
}

static void testLiveNotesThroughEngine() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;  // the built-in synth
    audio::Engine e(p);
    auto notes = e.liveNotes(t.get());
    CHECK(e.liveNotes(t.get()) == notes);
    notes->noteOn(69, 100, e.sr, true);
    std::vector<float> out(256 * 2);
    e.callback(nullptr, out.data(), 256);  // stopped, but live notes play
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);
    notes->noteOff(69);
    for (int i = 0; i < 40; i++) e.callback(nullptr, out.data(), 256);
    peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(peak == 0.0f);  // faded out and finished
    e.idle();
}

static void testRealDevice() {
    // open the ALSA "default" device, play silence for half a second, check the playhead moves
    Project p;
    audio::Engine e(p);
    std::string error;
    // (ALSA's "default" PCM; where the device list doesn't offer it, the sound server's default output)
    if (!e.open("ALSA", "default", 512, false, error)) {
        std::string pulse = audio::defaultOutput("PulseAudio");
        if (pulse.empty() || !e.open("PulseAudio", pulse, 512, false, error)) {
            printf("(no default device to run: %s)\n", error.c_str());
            return;
        }
    }
    e.masterMute = true;  // (nothing plays anyway: no clips)
    e.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int64_t pos = e.pos;
    e.stop();
    printf("(ran %s at %d Hz, %d outputs: %lld frames in 0.5 s)\n", e.deviceName().c_str(), e.sr, e.channels,
           (long long)pos);
    CHECK(pos > e.sr / 10);
    double cur, avg;
    CHECK(e.cpu.read(config::CPU_AVERAGE_SECONDS, cur, avg));
    e.close();
    CHECK(!e.isOpen());
}

// --- the successor's mixer: returns and sends, pan, track delay, clip gain and fades, looping clips,
// volume / pan automation, held notes let go, live held notes, meters, metronome, MIDI takes ---

// a clip of `frames` silent frames but 1.0 at frame `at` (both channels)
static ClipPtr impulseClip(const TrackPtr& t, int64_t start, int64_t frames, int64_t at, float value = 1.0f) {
    auto c = audioClip(t, start, frames, 0.0f);
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, 0.0f);
    buf->samples[(size_t)at * 2] = buf->samples[(size_t)at * 2 + 1] = value;
    c->data = c->source = buf;
    return c;
}

// frames (left channel) where out is louder than 0.5
static std::vector<int64_t> peaksAt(const std::vector<float>& out, int channels, int channel = 0) {
    std::vector<int64_t> at;
    for (size_t i = 0; i < out.size() / channels; i++)
        if (std::fabs(out[i * channels + channel]) > 0.5f) at.push_back((int64_t)i);
    return at;
}

static void testReturnsAndSends() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    auto r = p.addTrack(Kind::Return);
    auto later = p.addTrack(Kind::Audio);  // (added after the return: above it)
    CHECK(p.tracks[3] == r && p.tracks[2] == later && r->name == "Return A");
    p.addClip(audioClip(a, 0, 4000, 0.5f));
    p.addClip(audioClip(b, 0, 4000, 0.25f));
    a->sends = std::vector<Send>{{r, -6.0}};
    const double g6 = std::pow(10.0, -6.0 / 20);
    mixer::MixState st;
    std::vector<float> out;
    auto at = [&](int ch = 0) {
        out = mixRange(p, 0, 1024, 2, st);
        return out[500 * 2 + ch];
    };
    CHECK_NEAR(at(), 0.75 + 0.5 * g6, 1e-5);
    a->volumeDb = -6.0;  // post-fader
    CHECK_NEAR(at(), 0.5 * g6 + 0.25 + 0.5 * g6 * g6, 1e-5);
    a->volumeDb = 0;
    a->muted = true;  // a muted track sends nothing
    CHECK_NEAR(at(), 0.25, 1e-5);
    a->muted = false;
    a->soloed = true;  // soloing leaves the returns playing
    CHECK_NEAR(at(), 0.5 + 0.5 * g6, 1e-5);
    a->soloed = false;
    b->soloed = true;  // (a isn't heard, so it sends nothing)
    CHECK_NEAR(at(), 0.25, 1e-5);
    b->soloed = false;
    r->muted = true;
    CHECK_NEAR(at(), 0.75, 1e-5);
    r->muted = false;
    a->sends = std::vector<Send>{{r, (double)config::MIN_DB}};  // silent send
    CHECK_NEAR(at(), 0.75, 1e-5);
    // through the return's effects, fader and pan; the send comes after a's pan
    a->sends = std::vector<Send>{{r, 0.0}};
    b->sends = std::vector<Send>{{r, 0.0}};
    auto fx = fakeFx(0);
    fx->params[0] = 2.0;
    auto s = slotOf(fx);
    r->effects = std::vector<SlotPtr>{s};
    r->cables = chain({s});
    patch::ensureStates(p);
    r->volumeDb = -6.0;
    CHECK_NEAR(at(), 0.75 + 0.75 * 2 * g6, 1e-5);  // (two tracks into one return add up)
    a->pan = 1.0;
    CHECK_NEAR(at(0), 0.25 + 0.25 * 2 * g6, 1e-5);
    CHECK_NEAR(at(1), 0.75 + 0.75 * 2 * g6, 1e-5);
    r->pan = -1.0;  // the return's own pan
    CHECK_NEAR(at(1), 0.75, 1e-5);
    // a return's own effects' look-ahead isn't made up for: it comes that much after the tracks
    Project q;
    auto t = q.addTrack(Kind::Audio);
    auto ret = q.addTrack(Kind::Return);
    q.addClip(impulseClip(t, 0, 8000, 1000));
    t->sends = std::vector<Send>{{ret, 0.0}};
    auto look = fakeFx(100);
    auto ls = slotOf(look);
    ret->effects = std::vector<SlotPtr>{ls};
    ret->cables = chain({ls});
    mixer::MixState st2;
    int latency = 0;
    out = mixRange(q, 0, 4096, 2, st2, 256, &latency);
    CHECK(latency == 100);  // (the latest track: the return)
    CHECK((peaksAt(out, 2) == std::vector<int64_t>{1100, 1200}));
    // a stem (only): the track's own sound, though it still sends into the return
    std::vector<float> stem(1024 * 2, 0.0f);
    mixer::MixState st3;
    mixer::mixClips(p, stem.data(), 2, 1024, true, 0, st3, nullptr, SR, true, true, nullptr, b.get());
    CHECK_NEAR(stem[500 * 2], 0.25, 1e-5);
    std::fill(stem.begin(), stem.end(), 0.0f);
    mixer::mixClips(p, stem.data(), 2, 1024, true, 0, st3, nullptr, SR, true, true, nullptr, r.get());
    CHECK_NEAR(stem[500 * 2 + 0], 0.25 * 2 * g6, 1e-5);  // (the return panned hard left: its right side silent)
    CHECK_NEAR(stem[500 * 2 + 1], 0.0, 1e-5);
    r->pan = 0;
    std::fill(stem.begin(), stem.end(), 0.0f);
    mixer::mixClips(p, stem.data(), 2, 1024, true, 0, st3, nullptr, SR, true, true, nullptr, r.get());
    CHECK_NEAR(stem[500 * 2 + 0], 0.25 * 2 * g6, 1e-5);
    CHECK_NEAR(stem[500 * 2 + 1], 0.75 * 2 * g6, 1e-5);
}

static void testPanLaw() {
    double l, r;
    mixer::panGains(0, l, r);
    CHECK(l == 1.0 && r == 1.0);
    mixer::panGains(0.5, l, r);
    CHECK_NEAR(l, std::cos(3.14159265358979 / 4), 1e-12);
    CHECK(r == 1.0);
    mixer::panGains(-1, l, r);
    CHECK(l == 1.0);
    CHECK_NEAR(r, 0.0, 1e-12);
    Project p;
    auto a = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, 4000, 0.5f));
    mixer::MixState st;
    a->pan = 0.5;
    auto out = mixRange(p, 0, 1024, 2, st);
    CHECK_NEAR(out[500 * 2], 0.5 * std::cos(3.14159265358979 / 4), 1e-6);  // after the fader: 0.3536
    CHECK_NEAR(out[500 * 2 + 1], 0.5, 1e-6);
    a->pan = -0.25;
    a->volumeDb = -6;
    out = mixRange(p, 0, 1024, 2, st);
    CHECK_NEAR(out[500 * 2], 0.5 * std::pow(10.0, -6.0 / 20), 1e-6);
    CHECK_NEAR(out[500 * 2 + 1], 0.5 * std::pow(10.0, -6.0 / 20) * std::cos(3.14159265358979 / 8), 1e-6);
    // a mono output (one channel left): no pan
    a->pan = 1.0, a->volumeDb = 0;
    mixer::MixState st1;
    out = mixRange(p, 0, 1024, 1, st1);
    CHECK_NEAR(out[500], 0.5, 1e-6);
}

static void testTrackDelay() {
    Project p;
    auto a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(impulseClip(a, 0, 8000, 1000));
    p.addClip(impulseClip(b, 0, 8000, 1000, -1.0f));
    CHECK(mixer::delayFrames(*a, SR) == 0);
    a->delayMs = 10;  // 480 frames later
    mixer::MixState st;
    int latency = -1;
    auto out = mixRange(p, 0, 4096, 2, st, 256, &latency);
    CHECK(latency == 0);
    CHECK((peaksAt(out, 2) == std::vector<int64_t>{1000, 1480}));
    CHECK(out[1000 * 2] < 0 && out[1480 * 2] > 0);  // (b's at 1000, a's 10 ms later)
    a->delayMs = -5;  // 240 frames earlier: the others are held back
    mixer::MixState st2;
    out = mixRange(p, 0, 4096, 2, st2, 256, &latency);
    CHECK(latency == 240);
    CHECK((peaksAt(out, 2) == std::vector<int64_t>{1000, 1240}));
    CHECK(out[1000 * 2] > 0 && out[1240 * 2] < 0);
    // with plugin delay compensation: b's effect holds it back 100
    a->delayMs = 10;
    auto fx = fakeFx(100);
    auto s = slotOf(fx);
    b->effects = std::vector<SlotPtr>{s};
    b->cables = chain({s});
    mixer::MixState st3;
    out = mixRange(p, 0, 4096, 2, st3, 256, &latency);
    CHECK(latency == 100);
    CHECK((peaksAt(out, 2) == std::vector<int64_t>{1100, 1580}));
    // a return's delay: only later (as its own hold-back)
    Project q;
    auto t = q.addTrack(Kind::Audio);
    auto ret = q.addTrack(Kind::Return);
    q.addClip(impulseClip(t, 0, 8000, 1000));
    t->sends = std::vector<Send>{{ret, 0.0}};
    t->volumeDb = config::MIN_DB;  // (only what it sends is heard... a silent fader sends silence)
    t->volumeDb = 0, t->muted = false;
    ret->delayMs = 5;
    mixer::MixState st4;
    out = mixRange(q, 0, 4096, 2, st4, 256, &latency);
    CHECK(latency == 0);
    CHECK((peaksAt(out, 2) == std::vector<int64_t>{1000, 1240}));
}

static void testClipLevels() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto c = audioClip(a, 0, 10000, 0.5f);
    c->gainDb = -6;
    c->fadeIn = 2000;
    c->fadeOut = 1000;
    p.addClip(c);
    mixer::MixState st;
    auto out = mixRange(p, 0, 10240, 2, st, 1024);
    for (int64_t pos : {100, 999, 1999, 5000, 9000, 9500, 9900})
        CHECK_NEAR(out[pos * 2], 0.5 * c->level(pos), 1e-6);
    CHECK_NEAR(out[999 * 2], 0.5 * std::pow(10.0, -6.0 / 20) * 1000.0 / 2000.0, 1e-6);
    CHECK_NEAR(out[9500 * 2], 0.5 * std::pow(10.0, -6.0 / 20) * 500.0 / 1000.0, 1e-6);
    // before the track's effects: a gain effect doubles the faded sound
    auto fx = fakeFx(0);
    fx->params[0] = 2.0;
    auto s = slotOf(fx);
    a->effects = std::vector<SlotPtr>{s};
    a->cables = chain({s});
    out = mixRange(p, 0, 10240, 2, st, 1024);
    CHECK_NEAR(out[999 * 2], 2 * 0.5 * std::pow(10.0, -6.0 / 20) * 0.5, 1e-6);
    // a piece cut from inside the fade-in goes on with the ramp (fadeLead)
    c->fadeLead = 1000;
    out = mixRange(p, 0, 10240, 2, st, 1024);
    CHECK_NEAR(out[500 * 2], 2 * 0.5 * std::pow(10.0, -6.0 / 20) * 1501.0 / 2000.0, 1e-6);
}

static void testLoopingClip() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    auto src = std::make_shared<AudioBuffer>();
    for (int i = 0; i < 2000; i++) src->samples.push_back((float)i / 2000), src->samples.push_back((float)-i / 2000);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->track = a;
    c->source = src;
    c->sourceSr = SR;
    c->loop = true;
    c->loopStart = 1000.0 / SR;
    c->loopLength = 500.0 / SR;
    c->length = 3000;  // longer than its audio: the loop repeats
    c->prepare(SR);
    p.addClip(c);
    const int seam = (int)std::nearbyint(config::LOOP_SEAM_S * SR);  // 144
    CHECK(c->baseFrames == 2000 && c->data->frames() >= 3000);
    mixer::MixState st;
    auto out = mixRange(p, 0, 4096, 2, st, 512);
    auto v = [&](int64_t f) { return out[f * 2]; };
    CHECK_NEAR(v(1200), 1200.0 / 2000, 1e-6);  // (the first pass: the audio as it is)
    // each repeat begins with a crossfade from what followed the loop (frames 1500…) into its start
    CHECK_NEAR(v(1500), 1500.0 / 2000, 1e-6);
    double ramp = 72.0 / (seam - 1);
    CHECK_NEAR(v(1572), (1072.0 * ramp + 1572.0 * (1 - ramp)) / 2000, 1e-5);
    CHECK_NEAR(v(1500 + seam), (1000.0 + seam) / 2000, 1e-6);
    CHECK_NEAR(v(2000 + 10), v(1500 + 10), 1e-6);  // (every pass the same)
    CHECK_NEAR(v(2500 + 300), (1300.0) / 2000, 1e-6);
    CHECK_NEAR(v(3000), 0.0, 1e-9);  // (the window ends)
}

static void testVolumePanAutomation() {
    Project p;
    auto a = p.addTrack(Kind::Audio);
    p.addClip(audioClip(a, 0, SR * 2, 0.5f));
    auto lane = std::make_shared<Automation>();
    lane->slot = a->controls;
    lane->param = automation::VOLUME;
    const double zero = 60.0 / 66.0;  // (0 dB on the fader's range)
    lane->setPoints({{0.0, zero}, {1.0, 0.0}});
    a->automation = std::vector<AutomationPtr>{lane};
    CHECK(automation::controlAutomated(*a, automation::VOLUME) && !automation::controlAutomated(*a, automation::PAN));
    CHECK(automation::prune(*a) == false && a->automation.size() == 1);  // (its own lanes stay)
    double db, pan;
    automation::mixerValues(*a, false, SR / 2, SR, db, pan);
    CHECK(db == 0 && pan == 0);  // stopped: the fader
    automation::mixerValues(*a, true, SR / 2, SR, db, pan);
    CHECK_NEAR(db, -30, 1e-9);  // (the GUI's fader shows this while playing)
    CHECK(automation::controlText(automation::VOLUME, 0.0) == "-inf dB");
    CHECK(automation::controlText(automation::VOLUME, zero) == "+0.0 dB");
    CHECK(automation::controlText(automation::PAN, 0.5) == "C");
    CHECK(automation::controlText(automation::PAN, 0.2) == "60 L");
    mixer::MixState st;
    auto out = mixRange(p, 0, 16384, 2, st, 1024);
    auto gainAt = [&](int64_t f) { return dsp::dbToGain(automation::volumeOf(zero * (1 - (double)f / SR)), config::MIN_DB); };
    CHECK_NEAR(out[2048 * 2], 0.5 * gainAt(2048), 1e-6);
    double g0 = gainAt(2048), g1 = gainAt(3072);  // (a straight line across each buffer)
    CHECK_NEAR(out[(2048 + 512) * 2], 0.5 * (float)(g0 + (g1 - g0) * 0.5), 1e-6);
    // frozen tracks follow it too
    auto frozen = audioClip(nullptr, 0, SR * 2, 0.25f);
    a->frozen = frozen;
    mixer::MixState st2;
    out = mixRange(p, 0, 4096, 2, st2, 1024);
    CHECK_NEAR(out[2048 * 2], 0.25 * gainAt(2048), 1e-6);
    a->frozen = nullptr;
    // pan
    auto panLane = std::make_shared<Automation>();
    panLane->slot = a->controls;
    panLane->param = automation::PAN;
    panLane->setPoints({{0.0, 0.0}});  // hard left
    a->automation = std::vector<AutomationPtr>{panLane};
    out = mixRange(p, 0, 1024, 2, st, 1024);
    CHECK_NEAR(out[500 * 2], 0.5, 1e-6);
    CHECK_NEAR(out[500 * 2 + 1], 0.0, 1e-6);
    CHECK(automation::pluginsOf(*a)[0] == a->controls);
    CHECK(automation::parameters(*a->controls).empty());
}

static void testHeldNotesLetGo() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto synth = std::make_shared<FakePlugin>();
    synth->instrument = true;
    t->instrument = slotOf(synth);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->track = t;
    c->start = 0;
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign(20000 * 2, 0.0f);
    c->data = data;
    c->events = std::make_shared<const std::vector<Clip::Event>>(std::vector<Clip::Event>{{10, 15000, 60, 100}});
    p.addClip(c);
    mixer::MixState st;
    std::vector<float> out(256 * 2);
    mixer::mixClips(p, out.data(), 2, 256, true, 0, st, nullptr, SR, true);
    CHECK(synth->held == 1);
    mixer::mixClips(p, out.data(), 2, 256, true, 256, st, nullptr, SR, false);
    CHECK(synth->held == 1);  // (it plays on)
    p.removeClips({c});  // deleted while it sounds: let go at once
    mixer::mixClips(p, out.data(), 2, 256, true, 512, st, nullptr, SR, false);
    CHECK(synth->held == 0);
    CHECK(!synth->received.empty() && !synth->received.back().on && synth->received.back().frame == 0);
}

static void testLiveHeldAndOneShot() {
    // a built-in synth note held down sustains until let go (a seamless loop), not just HOLD_S
    live::LiveNotes notes;
    CHECK(notes.noteOn(69, 100, SR, true));
    std::vector<float> l(512), r(512);
    float* sig[2] = {l.data(), r.data()};
    plugin_audio::Messages msgs;
    float lastPeak = 0;
    for (int b = 0; b < SR * 6 / 512; b++) {
        std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
        notes.take(512, SR, msgs);
        notes.addVoices(sig, 2, 512, SR);
        lastPeak = 0;
        for (float v : l) lastPeak = std::max(lastPeak, std::fabs(v));
    }
    CHECK(lastPeak > 0.05f);  // after 6 s, still sounding
    CHECK(notes.active());
    notes.noteOff(69);
    for (int b = 0; b < 20; b++) {
        std::fill(l.begin(), l.end(), 0.0f);
        notes.take(512, SR, msgs);
        notes.addVoices(sig, 2, 512, SR);
    }
    CHECK(!notes.active());
    // loopable: the loop is a whole number of periods, and its seam crossfaded
    std::vector<float> sine(SR);
    for (int i = 0; i < SR; i++) sine[i] = (float)std::sin(2 * 3.14159265358979 * 100.0 * i / SR);  // period 480
    int64_t loopStart;
    auto looped = live::loopable(sine, 1, SR, SR, 480.0, loopStart);
    CHECK(loopStart >= 0 && (SR - loopStart) % 480 == 0 && (int64_t)looped.size() == SR);
    // a one-shot sampler note plays to its end: its note-off is ignored
    live::NoteRender one;
    one.render = [](int, int, double, int) { return std::vector<float>(1000 * 2, 0.5f); };
    one.channels = 2;
    one.oneShot = true;
    live::LiveNotes shots;
    shots.setRenderer(one);
    shots.noteOn(60, 100, SR, true);
    CHECK(shots.noteOff(60));  // (ignored)
    std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
    shots.take(512, SR, msgs);
    shots.addVoices(sig, 2, 512, SR);
    CHECK_NEAR(l[511], 0.5, 1e-6);  // (not faded: the key's release didn't stop it)
    std::fill(l.begin(), l.end(), 0.0f);
    shots.take(512, SR, msgs);
    shots.addVoices(sig, 2, 512, SR);
    CHECK_NEAR(l[400], 0.5, 1e-6);   // frame 912
    CHECK_NEAR(l[500], 0.0, 1e-9);   // past its 1000 frames
    shots.take(512, SR, msgs);
    shots.addVoices(sig, 2, 512, SR);
    CHECK(!shots.active());
}

static void testMeters() {
    audio::PeakHold m(2);
    CHECK(m.push({0.5f, 0.2f}, 0.0));
    CHECK(m.held()[0] == 0.5 && m.loudest() == 0.5 && !m.clipped());
    CHECK_NEAR(m.loudestDb(), 20 * std::log10(0.5), 1e-9);
    m.push({0.1f, 0.1f}, 1.0);
    CHECK_NEAR(m.held()[0], 0.5, 1e-12);  // held for 1.5 s
    CHECK_NEAR(m.levels()[0], 0.5 * config::METER_DECAY, 1e-9);
    m.push({0.1f, 0.1f}, 2.0);
    CHECK_NEAR(m.held()[0], 0.5 * config::METER_DECAY, 1e-9);  // then it falls
    m.push({1.2f, 0.0f}, 3.0);
    CHECK(m.clipped() && m.loudest() > 1.19);
    m.push({0.0f, 0.0f}, 4.0);
    CHECK(m.clipped());  // (stays lit)
    m.resetPeaks();
    CHECK(!m.clipped() && m.loudest() == 0);
    audio::PeakHold master(2, audio::PeakHold::MASTER_CLIP_DB);
    master.push({0.995f, 0.0f}, 0.0);  // -0.04 dB: over the master's -0.1 dB
    CHECK(master.clipped());
    audio::PeakHold track(2);
    track.push({0.995f, 0.0f}, 0.0);
    CHECK(!track.clipped());
    // Play clears them (the engine counts it)
    Project p;
    audio::Engine e(p);
    e.offline = true;
    uint64_t before = e.metering->peakResets;
    e.play();
    CHECK(e.metering->peakResets == before + 1);
    e.stop();
}

static void testMetronomeTempoMap() {
    Project p;
    p.timing.setChange(2, 60, 3);  // bar 2 (at 2 s): 60 BPM in 3/4
    p.timing.publish();
    metronome::Clicks clicks;
    clicks.bar = {1.0f};
    clicks.beat = {0.5f};
    std::vector<float> out((size_t)SR * 6 * 2, 0.0f);
    for (int64_t pos = 0; pos < SR * 6; pos += 1000)
        metronome::addClicks(out.data() + pos * 2, 2, (int)std::min<int64_t>(1000, SR * 6 - pos), pos, clicks, p.timing, SR);
    std::vector<std::pair<int64_t, float>> found;
    for (int64_t i = 0; i < SR * 6; i++)
        if (out[i * 2] != 0) found.emplace_back(i, out[i * 2]);
    std::vector<std::pair<int64_t, float>> expected = {{0, 1.0f},       {SR / 2, 0.5f}, {SR, 0.5f},     {SR * 3 / 2, 0.5f},
                                                       {SR * 2, 1.0f},  {SR * 3, 0.5f}, {SR * 4, 0.5f}, {SR * 5, 1.0f}};
    CHECK(found == expected);
    // the count-in clicks on the song's beats when recording starts off a beat (0.25 s, 120 BPM 4/4)
    Project q;
    metronome::MetronomeSettings ms;
    int64_t frames = 0;
    AudioPtr count = metronome::countIn(ms, q.timing, 1, SR, 0.25, frames);
    CHECK(frames == 96000 && count && count->frames() == 96000);
    auto bar = metronome::renderClick(ms.bar, SR), beat = metronome::renderClick(ms.beat, SR);
    auto starts = [&](int64_t at, const std::vector<float>& click) {
        return std::fabs(count->frame(at + 5)[0] - click[5]) < 1e-6 && count->frame(at - 1)[0] == 0.0f;
    };
    CHECK(starts(12000, beat) && starts(36000, beat) && starts(60000, beat) && starts(84000, bar));
    CHECK(!metronome::countIn(ms, q.timing, 0, SR, 0, frames) && frames == 0);
}

static void testMidiTakesAndRecording() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    e.setLoop(0, 1024);
    e.seek(0);
    std::string error;
    CHECK(e.startMidiRecording(0, nullptr, error));
    CHECK(e.recordingMidi && e.playing);
    std::vector<float> out(512 * 2);
    for (int i = 0; i < 4; i++) e.callback(nullptr, out.data(), 512);
    CHECK(e.pos == 2048);  // (it didn't loop)
    double heard = -1;
    CHECK(e.heardNow(heard));
    CHECK(heard >= 1536 && heard < 1536 + SR);
    e.finishMidiRecording();
    CHECK(!e.recordingMidi && !e.playing && !e.heardNow(heard));
    // the take's notes
    audio::MidiTake take(48000, SR);
    take.event(true, 60, 100, 50000);
    take.event(true, 62, 90, 30000);   // (during the count-in: from the take's start)
    take.event(false, 60, 0, 60000);
    take.event(false, 64, 0, 60000);   // (never on: ignored)
    double length = 0;
    auto notes = take.finish(80000, length);
    CHECK(notes.size() == 2);
    CHECK_NEAR(notes[0].start, 0.0, 1e-12);  // 62, held to the end
    CHECK_NEAR(notes[0].end, 32000.0 / SR, 1e-12);
    CHECK(notes[0].pitch == 62 && notes[0].velocity == 90);
    CHECK_NEAR(notes[1].start, 2000.0 / SR, 1e-12);
    CHECK_NEAR(notes[1].end, 12000.0 / SR, 1e-12);
    CHECK_NEAR(length, 32000.0 / SR, 1e-12);
    // every take kept with its track
    auto t = p.addTrack(Kind::Audio);
    auto clip = audioClip(t, 0, 100, 0.1f);
    CHECK(!audio::keepTake(*t, clip) && t->takes.size() == 1 && !t->showTakes);
    CHECK(audio::keepTake(*t, clip) && t->takes.size() == 2 && t->showTakes);
    CHECK(t->takes[0] != clip && !t->takes[0]->track);
    // the recording offset moves audio takes earlier
    audio::Recorder rec({0}, 0.01, SR, 1.0, 5.0);
    rec.begin(10000, 0);
    std::vector<float> got;
    CHECK(rec.audio(got) == 10000 - 480 - 240);
    CHECK(rec.offsetMs == 5.0);
}

int main() {
    testClipsAndFades();
    testMuteSoloGroups();
    testOutputPairs();
    testDelayCompensation();
    testParallelPatch();
    testSidechain();
    testMasterPatch();
    testFrozen();
    testAdvance();
    testCallback();
    testPreview();
    testMetronome();
    testInstrumentNotes();
    testLiveNotesThroughEngine();
    testReturnsAndSends();
    testPanLaw();
    testTrackDelay();
    testClipLevels();
    testLoopingClip();
    testVolumePanAutomation();
    testHeldNotesLetGo();
    testLiveHeldAndOneShot();
    testMeters();
    testMetronomeTempoMap();
    testMidiTakesAndRecording();
    testRealDevice();
    TEST_MAIN_END();
}
