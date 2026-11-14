// Tests of the patch (patch.h), the audio-thread plugin helpers (plugin_audio.h), automation
// (automation.h) and live notes (live.h).
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "audio/automation.h"
#include "audio/live.h"
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

static void testDelayLine() {
    patch::DelayLine line(3, 2);
    float l[5] = {1, 2, 3, 4, 5}, r[5] = {-1, -2, -3, -4, -5};
    float* ch[2] = {l, r};
    line.process(ch, 5);
    CHECK(l[0] == 0 && l[2] == 0 && l[3] == 1 && l[4] == 2 && r[4] == -2);
    CHECK(line.sounding);
    float z[2][2] = {{0, 0}, {0, 0}};
    float* zc[2] = {z[0], z[1]};
    line.process(zc, 2);
    CHECK(z[0][0] == 3 && z[0][1] == 4 && z[1][1] == -4);
    CHECK(line.sounding);  // 5 still held
    float y[2][3] = {};
    float* yc[2] = {y[0], y[1]};
    line.process(yc, 3);
    CHECK(y[0][0] == 5 && !line.sounding);
    line.clear();
    CHECK(!line.sounding);
    patch::DelayLine none(0, 2);
    float a[2] = {7, 8}, b[2] = {9, 10};
    float* ab[2] = {a, b};
    none.process(ab, 2);
    CHECK(a[0] == 7 && b[1] == 10);
}

static void testPlan() {
    PluginSlot s1, s2, s3;
    // IN → s1 → s2 → OUT, plus s2 → s1 (would close a loop: left out), s3 not reaching OUT
    Cables cables{std::vector<Cable>{Cable(End::in(), End::of(&s1)), Cable(End::of(&s1), End::of(&s2)),
                                     Cable(End::of(&s2), End::out()), Cable(End::of(&s2), End::of(&s1)),
                                     Cable(End::in(), End::of(&s3))}};
    auto plan = patch::makePlan(cables.get());
    CHECK(plan->order.size() == 2);
    CHECK(plan->order[0].slot == &s1 && plan->order[1].slot == &s2);
    CHECK(plan->order[1].in.size() == 1 && plan->order[1].in[0] == 0);
    CHECK(plan->order[0].in.size() == 1 && plan->order[0].in[0] == -1);  // (s2 → s1 dropped)
    CHECK(plan->out.size() == 1 && plan->out[0] == 1);
    CHECK(patch::isStraight(straight()));
    CHECK(!patch::isStraight(*cables));
    Track other;
    Cables side{std::vector<Cable>{Cable(End::in(), End::of(&s1)), Cable(End::of(&s1), End::out()),
                                   Cable(End::source(&other), End::sidechain(&s1))}};
    plan = patch::makePlan(side.get());
    CHECK(plan->leaves.size() == 1 && plan->leaves[0] == &other);
    CHECK(plan->order[0].side.size() == 1 && plan->order[0].side[0] == -2);
}

static void testRunPatch() {
    // a failing effect is switched off and passes its input on
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto fx = fakeFx(0);
    fx->params[0] = 0.5;
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    patch::PatchState st;
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* sig[2] = {l.data(), r.data()};
    plugin_audio::Transport tp;
    int late = patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK(late == 0 && l[10] == 0.5f && r[10] == 0.5f);
    fx->throwOnProcess = true;
    std::fill(l.begin(), l.end(), 1.0f);
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK(s->failed && s->error == "fake failure" && l[10] == 1.0f);
    CHECK(!plugin_audio::usable(s));
    int calls = fx->processCalls;
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK(fx->processCalls == calls);  // skipped from then on
    // one channel (a device with one output left): processed as stereo, the left one kept
    auto fx2 = fakeFx(0);
    auto s2 = slotOf(fx2);
    t->effects = std::vector<SlotPtr>{s2};
    t->cables = chain({s2});
    std::fill(l.begin(), l.end(), 0.25f);
    std::fill(r.begin(), r.end(), 9.0f);
    late = patch::runPatch(t->cables.get(), *t->effects, sig, 1, 64, SR, st, nullptr, 0, 7, tp);
    CHECK(late == 7 && l[3] == 0.25f && r[3] == 9.0f);
    // the sample rate changed: the plugin is prepared afresh
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, 44100, st, nullptr, 0, 0, tp);
    CHECK(fx2->prepares == 1 && fx2->sr == 44100);
    // states made by the GUI side
    patch::ensureStates(p);
    auto made = std::static_pointer_cast<patch::PatchState>(t->patchState);
    CHECK(made && made->plan && made->plan->cables == t->cables.get());
    patch::ensureState(*t);
    CHECK(std::static_pointer_cast<patch::PatchState>(t->patchState) == made);  // already ready: kept
    patch::freshState(*t);
    CHECK(std::static_pointer_cast<patch::PatchState>(t->patchState) != made);
    CHECK(t->rackState);
}

static void testRack() {
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->usesRack = true;
    auto a = std::make_shared<FakePlugin>(), b = std::make_shared<FakePlugin>(), c = std::make_shared<FakePlugin>();
    a->instrument = b->instrument = c->instrument = true;
    a->lat = 0, b->lat = 10;
    auto sa = slotOf(a), sb = slotOf(b), sc = slotOf(c);
    sb->volumeDb = -6;
    t->rack = std::vector<SlotPtr>{sa, sb, sc};
    // a and b get the notes and are heard; c is heard but not fed
    t->rackCables = std::vector<Cable>{Cable(End::in(), End::of(sa.get())), Cable(End::in(), End::of(sb.get())),
                                       Cable(End::of(sa.get()), End::out()), Cable(End::of(sb.get()), End::out()),
                                       Cable(End::of(sc.get()), End::out())};
    CHECK(plugin_audio::playsPlugins(*t));
    CHECK(plugin_audio::instrumentLatency(*t) == 10);
    CHECK(plugin_audio::heardInstruments(*t).size() == 3);
    patch::RackState st;
    plugin_audio::Messages msgs(2);
    msgs[0].type = plugin_audio::MidiMsg::AllOff;
    msgs[1].type = plugin_audio::MidiMsg::NoteOn, msgs[1].frame = 0, msgs[1].pitch = 60, msgs[1].velocity = 100;
    std::vector<float> l(64, 0.0f), r(64, 0.0f);
    float* out[2] = {l.data(), r.data()};
    plugin_audio::Transport tp;
    patch::playInstruments(*t, msgs.data(), msgs.size(), out, 2, 64, SR, st, tp);
    // a: 0.1 from frame 0, held back 10 to line up with b; b: 0.1 * -6 dB from frame 0 (its own lateness)
    double gb = std::pow(10.0, -6.0 / 20);
    CHECK_NEAR(l[5], 0.1 * gb, 1e-6);
    CHECK_NEAR(l[10], 0.1 + 0.1 * gb, 1e-6);
    CHECK(c->received.empty() && c->notesOffCalls == 1);  // not fed: only all notes off
    CHECK(a->notesOffCalls == 1 && b->notesOffCalls == 1);
    // unwire a from OUT: it lets go of its notes, and its line is forgotten
    t->rackCables = std::vector<Cable>{Cable(End::in(), End::of(sa.get())), Cable(End::in(), End::of(sb.get())),
                                       Cable(End::of(sb.get()), End::out())};
    std::fill(l.begin(), l.end(), 0.0f);
    patch::playInstruments(*t, nullptr, 0, out, 2, 64, SR, st, tp);
    CHECK(a->notesOffCalls == 2 && st.lines.size() == 1);
    CHECK(plugin_audio::instrumentLatency(*t) == 10);
    // muted (bypass) instruments of the panel aren't heard
    sb->bypass = true;
    CHECK(plugin_audio::instrumentLatency(*t) == 0);
}

static void testAutomation() {
    using automation::valueAt;
    std::vector<AutoPoint> pts{{0, 0}, {1, 1}, {2, 0.25}};
    CHECK_NEAR(valueAt(pts, -1), 0, 1e-12);
    CHECK_NEAR(valueAt(pts, 0.5), 0.5, 1e-12);
    CHECK_NEAR(valueAt(pts, 1.5), 0.625, 1e-12);
    CHECK_NEAR(valueAt(pts, 5), 0.25, 1e-12);
    CHECK_NEAR(valueAt(pts, 0.5, 1), 0, 1e-12);    // stepped: holds until the next point
    CHECK_NEAR(valueAt(pts, 1.0, 1), 1, 1e-12);
    CHECK_NEAR(valueAt(pts, 1.9, 1), 1, 1e-12);
    CHECK_NEAR(valueAt(pts, 2.5, 2), 0.0, 1e-12);  // 0.25 at the nearest of 0, 0.5, 1 (Python rounds half to even)
    CHECK_NEAR(automation::snap(0.3, 2), 0.5, 1e-12);
    CHECK_NEAR(automation::snap(0.3, 0), 0.3, 1e-12);
    std::vector<AutoPoint> same{{1, 0.2}, {1, 0.8}};
    CHECK_NEAR(valueAt(same, 1.0), 0.8, 1e-12);
    CHECK(automation::isPlaceholder("MIDI CC 7") && automation::isPlaceholder("P012"));
    CHECK(!automation::isPlaceholder("P01a") && !automation::isPlaceholder("Gain"));

    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto fx = fakeFx(0);
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    auto params = automation::parameters(*s);
    CHECK(params.size() == 2 && params[0].name == "Gain" && params[1].steps == 2);
    auto lane = std::make_shared<Automation>();
    lane->slot = s;
    lane->param = "Gain";
    lane->setPoints({{0, 0}, {1, 1}});
    auto mode = std::make_shared<Automation>();
    mode->slot = s;
    mode->param = "Mode";
    mode->setPoints({{0, 0}, {1, 1}});
    auto missing = std::make_shared<Automation>();
    missing->slot = s;
    missing->param = "Nothing";
    missing->setPoints({{0, 0.5}});
    t->automation = std::vector<AutomationPtr>{lane, mode, missing};
    automation::apply(*t, SR / 2, SR);  // not resolved yet: nothing set
    CHECK(fx->params[0] == 1.0);
    automation::resolve(p);
    CHECK(lane->hasTarget && lane->targetParam == 0 && mode->targetSteps == 2 && !missing->hasTarget);
    CHECK(automation::stepsOf(*mode) == 2);
    automation::apply(*t, SR / 2, SR);
    CHECK_NEAR(fx->params[0], 0.5, 1e-9);
    CHECK_NEAR(fx->params[1], 0.0, 1e-9);
    automation::apply(*t, SR * 3 / 2, SR);
    CHECK_NEAR(fx->params[0], 1.0, 1e-9);
    CHECK_NEAR(fx->params[1], 1.0, 1e-9);
    // automation applied by the mixer at the start of each buffer while playing
    p.addClip(audioClip(t, 0, 48000, 1.0f));
    mixer::MixState ms;
    std::vector<float> out(256 * 2);
    mixer::mixClips(p, out.data(), 2, 256, true, SR / 4, ms, nullptr, SR, true);
    CHECK_NEAR(fx->params[0], 0.25, 1e-9);
    CHECK_NEAR(out[100 * 2], 0.25, 1e-6);
    fx->params[0] = 0.9;  // stopped: nothing is set
    mixer::mixClips(p, out.data(), 2, 256, false, 0, ms, nullptr, SR, false);
    CHECK_NEAR(fx->params[0], 0.9, 1e-9);
    // a new instance: looked up again
    auto fx2 = fakeFx(0);
    s->instance = fx2;
    automation::resolve(p);
    CHECK(lane->targetPlugin == s->instance && lane->last < 0);
    // the tweak watcher: changes in the plugin's window, latest first; not automated ones; not presets
    Project q;
    auto u = q.addTrack(Kind::Audio);
    auto fx3 = fakeFx(0);
    auto s3 = slotOf(fx3);
    u->effects = std::vector<SlotPtr>{s3};
    fx3->windowEdit(0, 0.1);  // (before the watcher: forgotten)
    automation::TweakWatcher w(s3, q);
    fx3->windowEdit(1, 0.5);
    fx3->windowEdit(0, 0.2);
    fx3->windowEdit(2, 0.2);  // (a placeholder: not listed)
    w.poll();
    CHECK(s3->recent.size() == 2 && s3->recent[0] == "Gain" && s3->recent[1] == "Mode");
    fx3->windowEdit(1, 0.0);
    w.poll();
    CHECK(s3->recent.size() == 2 && s3->recent[0] == "Mode");
    auto auto3 = std::make_shared<Automation>();
    auto3->slot = s3;
    auto3->param = "Gain";
    auto3->setPoints({{0, 1}});
    u->automation = std::vector<AutomationPtr>{auto3};
    fx3->windowEdit(0, 0.3);
    w.poll();
    CHECK(s3->recent[0] == "Mode");  // automation moves Gain: not a tweak
    // prune: lanes of plugins the track no longer has are dropped
    u->effects = std::vector<SlotPtr>{};
    CHECK(automation::prune(*u) && u->automation.empty());
    CHECK(!automation::prune(*u));
    CHECK(automation::pluginsOf(*t).size() == 2);  // its own volume and pan, and its instrument
    CHECK(automation::pluginsOf(*t)[0] == t->controls);
}

static void testLiveNotes() {
    using plugin_audio::MidiMsg;
    live::LiveNotes n;
    CHECK(!n.active() && !n.holding());
    n.noteOn(60, 0, SR, false);  // a VST instrument: messages
    CHECK(n.active() && n.holding());
    plugin_audio::Messages m;
    n.take(256, SR, m);
    CHECK(m.size() == 1 && m[0].type == MidiMsg::NoteOn && m[0].velocity == 1 && m[0].frame == 0);
    CHECK(n.holding() && !n.active());
    n.noteOff(60);
    m.clear();
    n.take(256, SR, m);
    CHECK(m.size() == 1 && m[0].type == MidiMsg::NoteOff && !n.holding());
    // a note that stops by itself: 0.01 s = 480 frames
    n.noteOn(64, 90, SR, false, 0.01);
    m.clear();
    n.take(256, SR, m);
    CHECK(m.size() == 1 && n.holding());
    m.clear();
    n.take(256, SR, m);
    CHECK(m.size() == 1 && m[0].type == MidiMsg::NoteOff && m[0].frame == 224 && !n.holding());
    // the built-in synth: voices, faded out when let go
    n.noteOn(69, 100, SR, true);
    m.clear();
    n.take(256, SR, m);
    CHECK(m.empty() && n.active());
    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* sig[2] = {l.data(), r.data()};
    n.addVoices(sig, 2, 256, SR);
    float peak = 0;
    for (float v : l) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f && l[100] == r[100]);
    n.noteOff(69);
    m.clear();
    n.take(256, SR, m);
    int fade = (int)(synth::RELEASE_S * SR);
    for (int i = 0; i < fade / 256 + 2; i++) {
        std::fill(l.begin(), l.end(), 0.0f);
        n.addVoices(sig, 2, 256, SR);
    }
    CHECK(!n.active());
    n.collect();
    n.noteOn(70, 100, SR, true);
    n.clear();
    CHECK(!n.active() && !n.holding());
}

static void testSyncState() {
    auto fx = fakeFx(0);
    auto s = slotOf(fx);
    plugin_audio::syncState(*s, SR);
    CHECK(fx->processCalls == 1);
    auto syn = std::make_shared<FakePlugin>();
    syn->instrument = true;
    auto si = slotOf(syn);
    plugin_audio::syncState(*si, 44100);
    CHECK(syn->processCalls == 1 && syn->sr == 44100);
}

static void testEffectMix() {
    // Mix 50 % on an effect doubling its input with 10 frames of look-ahead: the input comes back, held back
    // as long, so the two are in time: 0.5 × x + 0.5 × 2x
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto fx = fakeFx(10);
    fx->params[0] = 2.0;
    auto s = slotOf(fx);
    s->mix = 0.5;
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    patch::PatchState st;
    plugin_audio::Transport tp;
    std::vector<float> l(64, 0.0f), r(64, 0.0f);
    l[5] = r[5] = 1.0f;
    float* sig[2] = {l.data(), r.data()};
    int late = patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK(late == 10);
    CHECK_NEAR(l[15], 1.5, 1e-6);
    CHECK_NEAR(l[5], 0.0, 1e-9);
    CHECK_NEAR(r[15], 1.5, 1e-6);
    // Mix 0: only the input, late by the look-ahead (delay compensation expects that)
    s->mix = 0.0;
    std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
    l[40] = 1.0f;
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK_NEAR(l[50], 1.0, 1e-6);
    CHECK_NEAR(l[40], 0.0, 1e-9);
    // 100 %: the effect alone (nothing blended)
    s->mix = 1.0;
    std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
    l[20] = 1.0f;
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK_NEAR(l[30], 2.0, 1e-6);
    // a bypassed effect passes its input, whatever its Mix
    s->mix = 0.3;
    s->bypass = true;
    std::fill(l.begin(), l.end(), 0.5f);
    patch::runPatch(t->cables.get(), *t->effects, sig, 2, 64, SR, st, nullptr, 0, 0, tp);
    CHECK(l[30] == 0.5f);
}

static void testHeldMessagesAndTransport() {
    using plugin_audio::MidiMsg;
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->start = 0;
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign(4000 * 2, 0.0f);
    c->data = data;
    c->events = std::make_shared<const std::vector<Clip::Event>>(std::vector<Clip::Event>{{10, 3000, 60, 100}});
    uint16_t held[128] = {};
    plugin_audio::Messages out;
    std::vector<plugin_audio::NoteEvent> ev;
    const Clip* clips[1] = {c.get()};
    plugin_audio::noteMessages(clips, 1, true, 0, 256, true, out, ev, held);
    CHECK(held[60] == 1 && out.size() == 2);
    CHECK(plugin_audio::soundsOn(*c, 60, 256) && !plugin_audio::soundsOn(*c, 61, 256));
    plugin_audio::noteMessages(clips, 1, true, 256, 256, false, out, ev, held);
    CHECK(out.empty() && held[60] == 1);
    c->length = 400;  // trimmed while it sounds: its note ends at the new end (an off in this buffer)
    plugin_audio::noteMessages(clips, 1, true, 256, 256, false, out, ev, held);
    CHECK(out.size() == 1 && out[0].type == MidiMsg::NoteOff && out[0].frame == 144 && held[60] == 0);
    c->length = -1;
    held[60] = 1;
    plugin_audio::noteMessages(clips, 0, true, 512, 256, false, out, ev, held);  // its clip is gone
    CHECK(out.size() == 1 && out[0].type == MidiMsg::NoteOff && out[0].frame == 0 && out[0].pitch == 60 && !held[60]);
    held[61] = 2;
    plugin_audio::noteMessages(clips, 1, false, 0, 256, false, out, ev, held);  // stopped: forgotten
    CHECK(!held[61]);
    // tempo-synced plugins are told the tempo where they play (the tempo map)
    TempoMap map(120, 4, {TempoChange{2, 90, 3}});
    auto tp = plugin_audio::Transport::at(map, true, 3 * SR, SR);
    CHECK(tp.tempo == 90 && tp.numerator == 3 && tp.playing && tp.position == 3 * SR);
    tp = plugin_audio::Transport::at(map, true, SR, SR);
    CHECK(tp.tempo == 120 && tp.numerator == 4);
    Project p;
    auto t = p.addTrack(Kind::Audio);
    auto fx = fakeFx(0);
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    p.timing.setChange(2, 90, 3);
    p.timing.publish();
    mixer::MixState ms;
    std::vector<float> buf(256 * 2);
    mixer::mixClips(p, buf.data(), 2, 256, true, 3 * SR, ms, nullptr, SR, true);
    CHECK(fx->tempo == 90 && fx->transportPlaying);
}

int main() {
    testDelayLine();
    testPlan();
    testRunPatch();
    testRack();
    testAutomation();
    testLiveNotes();
    testSyncState();
    testEffectMix();
    testHeldMessagesAndTransport();
    TEST_MAIN_END();
}
