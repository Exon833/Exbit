// Tests of rendering (render.h): a track for freezing and bouncing, and the whole mix.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "audio/render.h"
#include "check.h"
#include "config.h"
#include "effects/effects.h"
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

static void testRenderMix() {
    Project p;
    auto t = p.addTrack(Kind::Audio);
    p.addClip(audioClip(t, SR, SR, 1.0f));  // 1 s from 1 s
    auto out = render::renderMix(p, SR, 2 * SR, SR);
    CHECK(out && out->size() == (size_t)SR * 2);
    CHECK_NEAR((*out)[0], 1.0 / 64, 1e-6);  // exactly from `start`: the clip's fade-in
    CHECK_NEAR((*out)[1000 * 2], 1.0, 1e-6);
    // a tail of silence is cut away
    out = render::renderMix(p, SR, 2 * SR, SR, 0.0, true);
    CHECK(out && out->size() == (size_t)SR * 2);
    // the master volume
    out = render::renderMix(p, SR, 2 * SR, SR, -6.0);
    CHECK_NEAR((*out)[1000 * 2], std::pow(10.0, -6.0 / 20), 1e-5);
    // an echo rings on past the end: kept with tail, until it has died away
    auto fx = fakeFx(0);
    fx->echo = 1000;
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    out = render::renderMix(p, SR, 2 * SR, SR, 0.0, true);
    CHECK(out && out->size() == (size_t)(SR + 1000) * 2);
    CHECK(fx->resets == 2);  // plugins start afresh, and are reset afterwards (playback never hears its tail)
    out = render::renderMix(p, SR, 2 * SR, SR, 0.0, false);
    CHECK(out && out->size() == (size_t)SR * 2);
    // delay compensation: a plugin with look-ahead doesn't move the result
    fx->echo = 0;
    fx->lat = 300;
    out = render::renderMix(p, SR, 2 * SR, SR);
    CHECK(out && out->size() == (size_t)SR * 2);
    CHECK_NEAR((*out)[0], 1.0 / 64, 1e-6);
    CHECK_NEAR((*out)[(SR - 1) * 2], 1.0 / 64, 1e-6);
    // the preroll: an echo from before `start` is heard at it
    fx->lat = 0;
    fx->echo = 2000;
    p.clips = std::vector<ClipPtr>{audioClip(t, SR - 1500, 1000, 1.0f)};
    out = render::renderMix(p, SR, 2 * SR, SR);
    CHECK_NEAR((*out)[(500 + 200) * 2], 0.5, 1e-6);  // the clip's frame 200, 2000 frames later
    // muted tracks aren't rendered
    t->muted = true;
    out = render::renderMix(p, SR, 2 * SR, SR);
    CHECK_NEAR((*out)[700 * 2], 0.0, 1e-9);
    // cancelling
    t->muted = false;
    int calls = 0;
    out = render::renderMix(p, 0, 10 * SR, SR, 0.0, false, [&](double f) {
        CHECK(f >= 0 && f <= 1);
        return ++calls < 2;
    });
    CHECK(!out && calls == 2);
    // the audio thread's patch states are fresh afterwards
    auto before = t->patchState;
    out = render::renderMix(p, 0, SR, SR);
    CHECK(t->patchState && t->patchState != before);
}

static void testRenderTrack() {
    Project p;
    auto t = p.addTrack(Kind::Audio);
    p.addClip(audioClip(t, 10000, 1000, 1.0f));
    auto r = render::renderTrack(p, t, SR);
    CHECK(r && r->start == 10000 - 64);
    CHECK(r->frames() == 1000 + 64 + 64);  // the silent pads
    CHECK_NEAR(r->audio[0], 0.0, 1e-9);
    CHECK_NEAR(r->audio[64 * 2], 1.0 / 64, 1e-6);
    CHECK_NEAR(r->audio[600 * 2], 1.0, 1e-6);
    // effects with look-ahead: moved back, in time; a tail: kept until silent, plus the pad
    auto fx = fakeFx(100);
    fx->echo = 500;
    auto s = slotOf(fx);
    t->effects = std::vector<SlotPtr>{s};
    t->cables = chain({s});
    t->volumeDb = -20;  // (its fader isn't part of the rendering)
    r = render::renderTrack(p, t, SR);
    CHECK(r && r->start == 10000 - 64);
    CHECK_NEAR(r->audio[64 * 2], 1.0 / 64, 1e-6);
    CHECK(r->frames() == 1000 + 64 + 500 + 64);
    CHECK_NEAR(r->audio[(64 + 1000 + 499) * 2], 0.5 / 64, 1e-6);
    // no clips: nothing
    auto empty = p.addTrack(Kind::Audio);
    CHECK(!render::renderTrack(p, empty, SR));
    // a sidechain listening to another (muted) track hears it as it plays, with its fader
    auto key = p.addTrack(Kind::Audio);
    key->muted = true;
    key->volumeDb = -6.0206;  // half
    p.addClip(audioClip(key, 9000, 3000, 1.0f));
    auto duck = fakeFx(0);
    duck->sidechain = true;
    auto sd = slotOf(duck);
    t->effects = std::vector<SlotPtr>{sd};
    t->cables = std::vector<Cable>{Cable(End::in(), End::of(sd.get())), Cable(End::of(sd.get()), End::out()),
                                   Cable(End::source(key.get()), End::sidechain(sd.get()))};
    auto srcs = render::sidechainSources(p, *t, SR, nullptr);
    CHECK(srcs.size() == 1 && srcs[0].first == key.get() && srcs[0].second.start == 9000 - 64);
    r = render::renderTrack(p, t, SR);
    CHECK(r);
    CHECK_NEAR(r->audio[600 * 2], 0.5, 1e-4);  // ducked by half
    // a frozen track's rendering feeds it as it is
    key->frozen = audioClip(key, 9000, 3000, 0.75f);
    r = render::renderTrack(p, t, SR);
    CHECK_NEAR(r->audio[600 * 2], 1.0 - 0.75 * 0.5, 1e-4);
    key->frozen = nullptr;
    // a MIDI track through its instrument; an instrument that fails stops the rendering
    auto m = p.addTrack(Kind::Midi);
    m->needsInstrument = false;
    auto syn = std::make_shared<FakePlugin>();
    syn->instrument = true;
    m->instrument = slotOf(syn);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->track = m;
    c->start = 2000;
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign(2 * 2000, 0.0f);
    c->data = data;
    c->events = std::make_shared<const std::vector<Clip::Event>>(std::vector<Clip::Event>{{100, 1100, 60, 100}});
    p.addClip(c);
    r = render::renderTrack(p, m, SR);
    CHECK(r && r->start == 2000 - 64);
    CHECK_NEAR(r->audio[(64 + 99) * 2], 0.0, 1e-9);
    CHECK_NEAR(r->audio[(64 + 100) * 2], 0.1, 1e-6);
    CHECK_NEAR(r->audio[(64 + 1100) * 2], 0.0, 1e-9);
    CHECK(syn->resets == 2 && syn->notesOffCalls >= 1);  // (reset before and after)
    syn->throwOnProcess = true;
    bool threw = false;
    try {
        render::renderTrack(p, m, SR);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "Fake Synth stopped: fake synth failure";
    }
    CHECK(threw);
}

// a slot holding a built-in effect (D2's), at its default settings
static SlotPtr builtinSlot(const char* id, double mix = 1.0) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->mix = mix;
    s->instance = fx;
    return s;
}

// a stereo test signal: two sines and a little of a third, `frames` long
static AudioPtr tone(int64_t frames, double f1, double f2) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    for (int64_t i = 0; i < frames; i++) {
        double t = (double)i / SR;
        b->samples[(size_t)i * 2] = (float)(0.3 * std::sin(2 * M_PI * f1 * t) + 0.05 * std::sin(2 * M_PI * 3 * f2 * t));
        b->samples[(size_t)i * 2 + 1] = (float)(0.3 * std::sin(2 * M_PI * f2 * t));
    }
    return b;
}

static void testRenderEqualsPlayback() {
    // A project using every feature of the mixer: rendering it gives exactly what playback plays.
    Project p;
    audio::Engine e(p);  // (first: it installs the sampler and the MIDI effects for clips being prepared)
    e.offline = true;
    p.timing.setChange(3, 100, 3);  // (a tempo change: the arpeggiator, the metronome, tempo-synced effects)
    p.timing.publish();
    // an audio track: clip gain, fades, a loop; its pan, delay, a send, volume automation, an effect with Mix
    auto a = p.addTrack(Kind::Audio);
    auto clip = makeAudioClip("loop", tone(SR * 3 / 2, 220, 330), SR, SR);
    clip->track = a;
    clip->start = SR / 2;
    clip->gainDb = -3;
    clip->fadeIn = 4000;
    clip->fadeOut = 6000;
    clip->loop = true;
    clip->loopStart = 0.2;
    clip->loopLength = 0.3;
    clip->length = SR * 5 / 2;
    clip->prepare(SR);
    p.addClip(clip);
    a->pan = 0.3;
    a->delayMs = 7;
    auto echo = builtinSlot("delay", 0.5);
    a->effects = std::vector<SlotPtr>{echo};
    a->cables = chain({echo});
    auto lane = std::make_shared<Automation>();
    lane->slot = a->controls;
    lane->param = automation::VOLUME;
    lane->setPoints({{0.0, 60.0 / 66}, {2.0, 50.0 / 66}, {4.0, 62.0 / 66}});
    auto panLane = std::make_shared<Automation>();
    panLane->slot = a->controls;
    panLane->param = automation::PAN;
    panLane->setPoints({{1.0, 0.2}, {3.0, 0.9}});
    a->automation = std::vector<AutomationPtr>{lane, panLane};
    // a MIDI track playing the built-in sampler through MIDI effects (Chord, then an arpeggiator)
    auto m = p.addTrack(Kind::Midi);
    m->needsInstrument = false;
    auto sample = makeAudioClip("sample", tone(SR / 2, 440, 440), SR, SR);
    auto settings = std::make_shared<SamplerSettings>();
    settings->sample = sample;
    settings->root = 69;
    settings->loop = true;
    settings->release = 0.1;
    m->sampler = settings;
    MidiEffect chord, arp;
    MidiEffect::make("Chord", {{"chord", 2}}, chord);
    MidiEffect::make("Arpeggiator", {{"rate", 1}}, arp);
    m->midiEffects = std::vector<MidiEffect>{chord, arp};
    auto mc = makeMidiClip("notes", {{0.0, 1.5, 60, 100}, {2.0, 3.0, 65, 80}}, 4.0, SR);
    mc->track = m;
    mc->start = SR;
    mc->prepare(SR);
    p.addClip(mc);
    CHECK(mc->played && mc->played->size() > 6);  // (the arpeggiator made the notes)
    m->pan = -0.4;
    m->delayMs = -4;
    // the built-in synth with Note Length
    auto syn = p.addTrack(Kind::Midi);
    syn->needsInstrument = false;
    MidiEffect len;
    MidiEffect::make("Note Length", {{"beats", 0.5}}, len);
    syn->midiEffects = std::vector<MidiEffect>{len};
    auto sc = makeMidiClip("synth", {{0.0, 0.1, 72, 110}, {1.0, 1.1, 76, 90}}, 2.0, SR);
    sc->track = syn;
    sc->start = SR * 2;
    sc->prepare(SR);
    p.addClip(sc);
    syn->volumeDb = -4;
    // a frozen track: its rendering, with its fader and pan still
    auto f = p.addTrack(Kind::Audio);
    auto frozen = makeAudioClip("frozen", tone(SR, 110, 165), SR, SR);
    frozen->start = SR * 3;
    f->frozen = frozen;
    f->pan = -0.8;
    // a return with a reverb, fed by two tracks; and a limiter (look-ahead) on the master
    auto r = p.addTrack(Kind::Return);
    auto verb = builtinSlot("reverb");
    r->effects = std::vector<SlotPtr>{verb};
    r->cables = chain({verb});
    r->volumeDb = -2;
    a->sends = std::vector<Send>{{r, -6.0}};
    m->sends = std::vector<Send>{{r, -3.0}};
    auto limiter = builtinSlot("limiter");
    p.masterEffects = std::vector<SlotPtr>{limiter};
    p.masterCables = chain({limiter});
    patch::ensureStates(p);
    for (auto& s : p.pluginSlots()) aload(s->instance)->reset();
    // playback: from 0, in the render's blocks
    const int block = config::RENDER_BLOCK;
    const int64_t start = SR * 2, end = SR * 6;  // (renderMix starts 2 s before: at 0)
    e.seek(0);
    e.play();
    std::vector<float> played;
    std::vector<float> buf((size_t)block * 2);
    while ((int64_t)played.size() / 2 < end + SR) {
        e.callback(nullptr, buf.data(), block);
        played.insert(played.end(), buf.begin(), buf.end());
    }
    e.stop();
    int latency = e.latency;
    CHECK(latency > 0);  // (the limiter's look-ahead, and m's -4 ms)
    auto rendered = render::renderMix(p, start, end, SR);
    CHECK(rendered && (int64_t)rendered->size() == (end - start) * 2);
    if (!rendered) return;
    int64_t differ = 0;
    double loudest = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++) {
        float x = (*rendered)[(size_t)j], y = played[(size_t)((start + latency) * 2 + j)];
        if (x != y) differ++;
        loudest = std::max(loudest, (double)std::fabs(x));
    }
    CHECK(differ == 0);
    CHECK(loudest > 0.05);
    if (differ) printf("render vs playback: %lld samples differ\n", (long long)differ);
    // a stem: one track's sound alone (no master effects)
    auto stem = render::renderMix(p, start, end, SR, 0.0, false, nullptr, m.get());
    CHECK(stem && stem->size() == rendered->size());
    // a bounce keeps the mix (fader, pan, output, delay, sends, volume / pan automation)
    auto bounced = p.addTrack(Kind::Audio);
    render::keepMix(*a, *bounced);
    CHECK(bounced->pan == 0.3 && bounced->delayMs == 7 && bounced->sends == a->sends);
    CHECK(bounced->automation.size() == 2 && bounced->automation[0]->slot == bounced->controls &&
          bounced->automation[0]->points == lane->points);
    // a track's rendering (freeze) has its clip gain and fades, not its fader, pan or volume lanes
    auto plain = p.addTrack(Kind::Audio);
    auto pc = audioClip(plain, SR, SR, 0.5f);
    pc->gainDb = -6;
    pc->fadeIn = 1000;
    p.addClip(pc);
    plain->pan = 1.0;
    plain->volumeDb = -10;
    auto frozenR = render::renderTrack(p, plain, SR);
    CHECK(frozenR && frozenR->start == SR - config::EDGE_FADE_FRAMES);
    if (frozenR) {
        int64_t k = config::EDGE_FADE_FRAMES + 499;
        CHECK_NEAR(frozenR->audio[(size_t)k * 2], 0.5 * pc->level(499), 1e-6);
        CHECK_NEAR(frozenR->audio[(size_t)k * 2 + 1], 0.5 * pc->level(499), 1e-6);
    }
}

int main() {
    testRenderMix();
    testRenderTrack();
    testRenderEqualsPlayback();
    TEST_MAIN_END();
}
