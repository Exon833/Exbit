// Group buses, nested groups and track output routing (model.h GROUP BUSES, model/routing.h, mixer.h GROUP BUSES
// AND ROUTING), worst cases first: 64 nested groups with effects and sidechains mixed on every core while tracks move
// in and out of groups, groups nest and unnest, buses go on and off and tracks are routed into each other, all during
// playback; a project with groups that are only folders sounds exactly — bit for bit — as without them, with and
// without workers; a bus's fader, pan, effects and sends (pre and post fader); delay compensation through two nested
// buses with look-ahead (the outputs and the returns line up); track-to-track routing and the refused loops; mute and
// solo through buses; a sidechain listening to a bus; stems of buses; save, load and undo, and an old file with
// groups; the group's header row in the window and the terminal (its keys, its menu's Fold only and Output).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "audio/render.h"
#include "audio/workers.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"
#include "model/routing.h"
#include "platform/platform.h"
#include "plugins/plugin.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/lane_panel.h"
#include "ui/lanes.h"
#include "ui/main_window.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace {

const int SR = 48000;
const std::string TMP = "build/ui-samples/group-bus";

// a gain effect (parameter "Gain") that holds its sound back by `lat` frames and says so
class FakeFx : public plug::Plugin {
public:
    int lat = 0;
    double gain = 1.0;
    std::vector<float> ring[2];
    size_t rpos = 0;
    bool isVst2() const override { return false; }
    std::string name() const override { return "Fake Fx"; }
    std::string vendor() const override { return "Tests"; }
    std::string subcategories() const override { return "Fx"; }
    bool isInstrument() const override { return false; }
    bool hasSidechain() const override { return false; }
    int latency() const override { return lat; }
    double sampleRate() const override { return SR; }
    void prepare(double, int) override {}
    void reset() override { ring[0].clear(), ring[1].clear(), rpos = 0; }
    void setTransport(double, int, int, bool, int64_t, double) override {}
    void process(const float* const in[2], const float* const[2], float* const out[2], int frames) override {
        for (int c = 0; c < 2; c++)
            if ((int)ring[c].size() != lat) ring[c].assign(lat, 0.0f);
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                float x = (float)(in[c][i] * gain);
                if (lat) std::swap(x, ring[c][rpos]);
                out[c][i] = x;
            }
            if (lat) rpos = (rpos + 1) % lat;
        }
    }
    void render(const std::vector<plug::NoteMessage>&, float* const[2], int) override {}
    void notesOff() override {}
    bool holding() const override { return false; }
    std::vector<plug::ParamInfo> parameters() override {
        std::vector<plug::ParamInfo> v(1);
        v[0].id = 0, v[0].name = "Gain";
        return v;
    }
    double getParam(uint32_t) override { return gain; }
    void setParam(uint32_t, double v) override { gain = v; }
    std::string paramText(uint32_t, double v) override { return std::to_string(v); }
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

SlotPtr fakeSlot(int lat, double gain) {
    auto fx = std::make_shared<FakeFx>();
    fx->lat = lat;
    fx->gain = gain;
    auto s = std::make_shared<PluginSlot>();
    s->name = fx->name();
    s->kind = "effect";
    s->instance = fx;
    return s;
}

SlotPtr builtinSlot(const char* id, double mix = 1.0) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->mix = mix;
    s->instance = fx;
    return s;
}

void setEffects(const TrackPtr& t, std::vector<SlotPtr> slots) {
    t->effects = slots;
    t->cables = chain(slots);
}

// a compressor on `t` whose sidechain listens to `source`
void sidechainFrom(const TrackPtr& t, const TrackPtr& source) {
    auto comp = builtinSlot("compressor");
    t->effects = std::vector<SlotPtr>{comp};
    std::vector<Cable> cables = chain({comp});
    cables.push_back(Cable(End::source(source.get()), End::sidechain(comp.get())));
    t->cables = cables;
}

AudioPtr tone(int64_t frames, double f1, double f2, double noise = 0) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    uint32_t seed = (uint32_t)(f1 * 7 + f2);
    for (int64_t i = 0; i < frames; i++) {
        double t = (double)i / SR;
        seed = seed * 1664525u + 1013904223u;
        double n = noise * ((double)(seed >> 8) / (1 << 24) - 0.5);
        double env = std::fmod(t, 0.5) < 0.05 ? 1.0 : 0.3;
        b->samples[(size_t)i * 2] = (float)(env * 0.3 * std::sin(2 * M_PI * f1 * t) + n);
        b->samples[(size_t)i * 2 + 1] = (float)(env * 0.3 * std::sin(2 * M_PI * f2 * t) - n);
    }
    return b;
}

TrackPtr audioTrack(Project& p, double f1, double f2, int64_t start, double noise = 0, int64_t frames = SR * 2) {
    auto t = p.addTrack(Kind::Audio);
    auto clip = makeAudioClip("c", tone(frames, f1, f2, noise), SR, SR);
    clip->track = t;
    clip->start = start;
    clip->prepare(SR);
    p.addClip(clip);
    return t;
}

// a clip of `frames` frames, silent but for `value` at frame `at` (far from its edges' short fades)
ClipPtr impulseClip(const TrackPtr& t, int64_t frames, int64_t at, float value, int64_t start = 0) {
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, 0.0f);
    buf->samples[(size_t)at * 2] = value;
    buf->samples[(size_t)at * 2 + 1] = value;
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "impulse";
    c->track = t;
    c->start = start;
    c->data = buf;
    c->source = buf;
    c->sourceSr = SR;
    return c;
}

ClipPtr constantClip(const TrackPtr& t, int64_t frames, float value) {
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, value);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "dc";
    c->track = t;
    c->data = buf;
    c->source = buf;
    c->sourceSr = SR;
    return c;
}

void freshPlugins(Project& p) {
    patch::ensureStates(p);
    render::clearPatchStates(p);
    for (auto& s : p.pluginSlots())
        if (auto inst = aload(s->instance)) inst->reset();
}

// The project mixed from 0 in buffers of `sizes` (cycled), a jump half way and a stopped buffer now and then;
// with or without `pool`: everything that came out (and the last latency)
std::vector<float> mixAll(Project& p, workers::Pool* pool, const std::vector<int>& sizes, int64_t total,
                          int* latency = nullptr) {
    freshPlugins(p);
    mixer::MixState st;
    st.pool = pool;
    std::vector<float> all, buf;
    int64_t pos = 0, played = 0;
    bool restart = true, jumped = false;
    for (size_t k = 0; played < total; k++) {
        int n = sizes[k % sizes.size()];
        buf.assign((size_t)n * 2, 0.0f);
        bool stopped = k % 50 == 49;
        int lat = mixer::mixClips(p, buf.data(), 2, n, !stopped, pos, st, nullptr, SR, restart || stopped);
        if (latency) *latency = lat;
        restart = stopped;
        all.insert(all.end(), buf.begin(), buf.end());
        pos += n;
        played += n;
        if (!jumped && played > total / 2) {
            pos = SR / 3;
            restart = jumped = true;
        }
    }
    return all;
}

// plain playback from 0, no jumps: everything that came out
std::vector<float> play(Project& p, workers::Pool* pool, int block, int64_t total, int* latency = nullptr) {
    freshPlugins(p);
    mixer::MixState st;
    st.pool = pool;
    std::vector<float> all, buf;
    for (int64_t pos = 0; pos < total; pos += block) {
        buf.assign((size_t)block * 2, 0.0f);
        int lat = mixer::mixClips(p, buf.data(), 2, block, true, pos, st, nullptr, SR, pos == 0);
        if (latency) *latency = lat;
        all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// --- a project of every kind of routing, with or without groups that are only folders (plain buses) ---
struct Built {
    std::vector<TrackPtr> tracks;
    TrackPtr r1, r2;
};

Built buildProject(Project& p) {
    Built b;
    for (int i = 0; i < 8; i++) {
        auto t = audioTrack(p, 110.0 * (i + 1), 165.0 * (i + 1), SR / 8 * i, 0.02 * i);
        t->pan = -0.6 + 0.15 * i;
        t->delayMs = (i % 3) - 1.0;
        if (i % 2) setEffects(t, {builtinSlot("chorus"), builtinSlot("eq")});
        else setEffects(t, {builtinSlot("delay", 0.5)});
        b.tracks.push_back(t);
    }
    auto kick = audioTrack(p, 55, 55, 0);
    kick->muted = true;  // (a ghost kick feeding a sidechain)
    auto x = audioTrack(p, 300, 450, SR / 3, 0.05);
    sidechainFrom(x, kick);
    auto y = audioTrack(p, 600, 900, SR / 5);
    sidechainFrom(y, x);
    y->automation = std::vector<AutomationPtr>{[&] {
        auto lane = std::make_shared<Automation>();
        lane->slot = y->controls;
        lane->param = automation::VOLUME;
        lane->setPoints({{0.0, 60.0 / 66}, {1.0, 40.0 / 66}, {1.5, 62.0 / 66}});
        return lane;
    }()};
    for (auto& t : {kick, x, y}) b.tracks.push_back(t);
    b.r1 = p.addTrack(Kind::Return);
    setEffects(b.r1, {builtinSlot("reverb")});
    b.r2 = p.addTrack(Kind::Return);
    setEffects(b.r2, {builtinSlot("delay")});
    for (auto& t : b.tracks)
        if (t != kick) t->sends = std::vector<Send>{{b.r1, -9.0}, {b.r2, -12.0, t == y}};
    auto limiter = builtinSlot("limiter");
    p.masterEffects = std::vector<SlotPtr>{limiter};
    p.masterCables = chain({limiter});
    return b;
}

}  // namespace

// --- groups that are only folders: the same sound, bit for bit, with and without workers ---
static void testFoldersSoundTheSame() {
    workers::Pool pool(6, false);
    const std::vector<int> sizes{512, 128, 1000, 64, 333};
    const int64_t total = SR * 2;
    Project plain;
    buildProject(plain);
    auto want = mixAll(plain, nullptr, sizes, total);
    Project grouped;
    Built b = buildProject(grouped);
    GroupPtr g1 = grouped.groupTracks({b.tracks[0], b.tracks[1], b.tracks[2], b.tracks[3]});
    GroupPtr g2 = grouped.groupTracks({b.tracks[1], b.tracks[2]});  // (inside g1: they were all in it)
    GroupPtr g3 = grouped.groupTracks({b.tracks[8], b.tracks[9], b.tracks[10]});  // (the sidechain chain)
    GroupPtr g4 = grouped.groupTracks({b.tracks[5], b.tracks[6]});
    g4->foldOnly = true;
    CHECK(g2->parent == g1 && !g1->parent && routing::target(*b.tracks[1]) == g2->bus.get());
    CHECK(routing::target(*g2->bus) == g1->bus.get() && routing::target(*g1->bus) == nullptr);
    CHECK(routing::target(*b.tracks[5]) == nullptr);  // (fold only: no bus)
    CHECK(routing::isPlain(*g1->bus) && routing::isPlain(*g3->bus));
    CHECK(grouped.groups().size() == 4 && grouped.allTracks().size() == grouped.tracks.size() + 4);
    auto got = mixAll(grouped, nullptr, sizes, total);
    CHECK(sameBits(want, got));
    auto parallel = mixAll(grouped, &pool, sizes, total);
    CHECK(sameBits(want, parallel));
    double peak = 0;
    for (float v : want) peak = std::max(peak, (double)std::fabs(v));
    CHECK(peak > 0.05);  // (not silence)
}

// --- real buses: the same with and without workers, bit for bit; a bus's fader, pan, effects and sends ---
static void testBusesOnEveryCore() {
    workers::Pool pool(7, false);
    const std::vector<int> sizes{256, 1024, 77, 512};
    Project p;
    Built b = buildProject(p);
    GroupPtr g1 = p.groupTracks({b.tracks[0], b.tracks[1], b.tracks[2]});
    GroupPtr g2 = p.groupTracks({b.tracks[1], b.tracks[2]});
    GroupPtr g3 = p.groupTracks({b.tracks[8], b.tracks[9], b.tracks[10]});
    setEffects(g1->bus, {builtinSlot("eq"), builtinSlot("limiter")});
    setEffects(g2->bus, {builtinSlot("chorus")});
    g2->bus->volumeDb = -3;
    g3->bus->pan = 0.4;
    g3->bus->sends = std::vector<Send>{{b.r1, -6.0}, {b.r2, -10.0, true}};
    sidechainFrom(b.tracks[4], g2->bus);  // (a track listening to a bus)
    CHECK(routing::setInto(b.tracks[6], b.tracks[7]));  // (and one played into another)
    auto one = mixAll(p, nullptr, sizes, SR * 2);
    auto many = mixAll(p, &pool, sizes, SR * 2);
    CHECK(sameBits(one, many));
    bool finite = true;
    for (float v : many) finite = finite && std::isfinite(v);
    CHECK(finite);
}

static void testBusFaderPanEffectsSends() {
    for (bool workersOn : {false, true}) {
        workers::Pool pool(3, false);
        Project p;
        TrackPtr a = p.addTrack(Kind::Audio);
        p.addClip(constantClip(a, SR, 0.5f));
        TrackPtr r = p.addTrack(Kind::Return);
        GroupPtr g = p.groupTracks({a});
        g->bus->volumeDb = -6;
        g->bus->pan = 0.5;
        setEffects(g->bus, {fakeSlot(0, 2.0)});
        g->bus->sends = std::vector<Send>{{r, -6.0}};
        CHECK(!routing::isPlain(*g->bus));
        auto out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        double k = dsp::dbToGain(-6, config::MIN_DB), l, rr;
        mixer::panGains(0.5, l, rr);
        double bus = 0.5 * 2 * k;  // (its effect, then its fader)
        size_t mid = (size_t)(SR / 4) * 2;
        CHECK_NEAR(out[mid], bus * l + bus * l * k, 1e-5);  // (itself, and the return its post-fader send feeds)
        CHECK_NEAR(out[mid + 1], bus * rr + bus * rr * k, 1e-5);
        // pre-fader: the return gets its sound before the fader and pan
        g->bus->sends = std::vector<Send>{{r, -6.0, true}};
        out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        CHECK_NEAR(out[mid], bus * l + 0.5 * 2 * k, 1e-5);
        CHECK_NEAR(out[mid + 1], bus * rr + 0.5 * 2 * k, 1e-5);
        // fold only: the bus is gone, the track plays on as it is
        g->foldOnly = true;
        out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        CHECK_NEAR(out[mid], 0.5, 1e-6);
        g->foldOnly = false;
        // its group muted: the bus too, and a track played into it from outside
        TrackPtr outside = p.addTrack(Kind::Audio);
        p.addClip(constantClip(outside, SR, 0.25f));
        CHECK(routing::setInto(outside, g->bus));
        g->muted = true;
        out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        CHECK_NEAR(out[mid], 0.0, 1e-9);
        g->muted = false;
        out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        CHECK_NEAR(out[mid], (0.5 + 0.25) * 2 * k * l + (0.5 + 0.25) * 2 * k, 1e-5);  // (and the pre-fader send)
    }
}

// --- delay compensation through two nested buses with look-ahead: everything lines up, outputs and returns ---
static void testDelayThroughBuses() {
    for (bool workersOn : {false, true}) {
        workers::Pool pool(4, false);
        Project p;
        TrackPtr a = p.addTrack(Kind::Audio), bt = p.addTrack(Kind::Audio), r = p.addTrack(Kind::Return);
        p.addClip(impulseClip(a, 20000, 5000, 1.0f));
        p.addClip(impulseClip(bt, 20000, 5000, 1.0f));
        GroupPtr outer = p.groupTracks({a});
        GroupPtr inner = p.groupTracks({a});  // (a was in outer: the new group goes inside it)
        CHECK(inner->parent == outer && a->group == inner);
        setEffects(inner->bus, {fakeSlot(1000, 1.0)});
        setEffects(outer->bus, {fakeSlot(500, 1.0)});
        a->sends = std::vector<Send>{{r, 0.0}};
        bt->sends = std::vector<Send>{{r, 0.0}};
        int latency = 0;
        auto out = play(p, workersOn ? &pool : nullptr, 256, 12000, &latency);
        CHECK(latency == 1500);
        int nonzero = 0;
        int64_t at = -1;
        for (size_t i = 0; i < out.size(); i += 2)
            if (out[i] != 0) nonzero++, at = (int64_t)i / 2;
        CHECK(nonzero == 1 && at == 6500);  // (a through both buses, b, and the return fed by both: together)
        if (at >= 0) CHECK_NEAR(out[(size_t)at * 2], 4.0, 1e-6);
        // the bus's own automation in time with its input: a Volume lane on the inner bus dips to silence around 5000
        // frames into the timeline (over a few buffers: the fader glides from a buffer's start to its end); the sound
        // that played there is silenced, however late its way through the buses made it
        auto lane = std::make_shared<Automation>();
        lane->slot = inner->bus->controls;
        lane->param = automation::VOLUME;
        double unity = (0.0 - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB);
        lane->setPoints({{0.0, unity}, {4690.0 / SR, unity}, {4700.0 / SR, 0.0}, {5300.0 / SR, 0.0}, {5310.0 / SR, unity}});
        inner->bus->automation = std::vector<AutomationPtr>{lane};
        out = play(p, workersOn ? &pool : nullptr, 256, 12000, &latency);
        CHECK_NEAR(out[6500 * 2], 3.0, 1e-6);  // (b, b's send and a's send: a's own sound is silenced at its bus)
    }
}

// --- track-to-track routing and the loops refused ---
static void testRoutingAndLoops() {
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio), c = p.addTrack(Kind::Audio);
    TrackPtr r = p.addTrack(Kind::Return);
    p.addClip(constantClip(a, SR, 0.25f));
    p.addClip(constantClip(b, SR, 0.5f));
    setEffects(b, {fakeSlot(0, 3.0)});
    CHECK(routing::setInto(a, b));
    CHECK(routing::target(*a) == b.get() && routing::targetName(*a) == b->name);
    auto out = play(p, nullptr, 512, SR / 2);
    CHECK_NEAR(out[(size_t)(SR / 4) * 2], (0.25 + 0.5) * 3, 1e-6);  // (into b's input, before its effects)
    CHECK(!routing::setInto(b, a));  // a loop
    CHECK(!routing::setInto(a, a));  // itself
    CHECK(!routing::setInto(a, r) && !routing::setInto(r, a));  // returns: never
    CHECK(routing::setInto(b, c) && !routing::setInto(c, a));  // a → b → c: c into a would loop
    CHECK(routing::canRoute(*c, nullptr) && !routing::canRoute(*c, a.get()) && !routing::canRoute(*c, b.get()));
    // a group's bus into a track inside it: a loop
    GroupPtr g = p.groupTracks({c});
    CHECK(!routing::setInto(g->bus, c));
    CHECK(routing::target(*c) == g->bus.get());
    // nesting: never inside itself, nor inside a group inside it
    GroupPtr h = p.groupTracks({c});  // (inside g)
    CHECK(h->parent == g && !routing::canNest(g, h) && !routing::setParent(p, g, h) && !routing::setParent(p, g, g));
    // a loop made behind the model's back (a file, an edit elsewhere): the mixer never hangs, fixLoops breaks it
    std::atomic_store(&c->into, a);
    out = play(p, nullptr, 512, SR / 4);
    bool finite = true;
    for (float v : out) finite = finite && std::isfinite(v);
    CHECK(finite);
    CHECK(routing::fixLoops(p) == 1);
    bool loops = false;
    for (auto& t : p.allTracks()) loops = loops || (routing::target(*t) && routing::reaches(*routing::target(*t), t.get()));
    CHECK(!loops);
    // a track removed: routed into it, back to the default
    for (auto& t : {a, b, c}) routing::setInto(t, nullptr);
    CHECK(routing::setInto(a, b));
    p.removeTrack(b);
    CHECK(!a->into);
}

// --- mute and solo through buses ---
static void testMuteSoloThroughBuses() {
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio), c = p.addTrack(Kind::Audio);
    p.addClip(constantClip(a, SR, 0.125f));
    p.addClip(constantClip(b, SR, 0.25f));
    p.addClip(constantClip(c, SR, 0.5f));
    GroupPtr g = p.groupTracks({a, b});
    GroupPtr inner = p.groupTracks({b});
    setEffects(g->bus, {fakeSlot(0, 2.0)});
    setEffects(inner->bus, {fakeSlot(0, 2.0)});
    size_t mid = (size_t)(SR / 4) * 2;
    auto level = [&] { return play(p, nullptr, 512, SR / 2)[mid]; };
    CHECK_NEAR(level(), 0.125 * 2 + 0.25 * 4 + 0.5, 1e-6);
    b->soloed = true;  // soloing a track keeps its buses heard (and only it in them)
    CHECK_NEAR(level(), 0.25 * 4, 1e-6);
    b->soloed = false;
    g->soloed = true;  // soloing a group: everything in it, nested groups too
    CHECK_NEAR(level(), 0.125 * 2 + 0.25 * 4, 1e-6);
    CHECK(p.audible(*b) && !p.audible(*c));
    g->soloed = false;
    g->muted = true;  // muting a group: everything in it
    CHECK(!p.audible(*b) && Project::mutedIn(*inner->bus));
    CHECK_NEAR(level(), 0.5, 1e-6);
    g->muted = false;
    c->soloed = true;
    p.soloOnly(a);
    CHECK(a->soloed && !c->soloed && !g->soloed && !inner->soloed);
    CHECK_NEAR(level(), 0.125 * 2, 1e-6);
}

// --- a sidechain listening to a bus ---
static void testSidechainFromBus() {
    auto energy = [](bool busLoud) {
        Project p;
        TrackPtr x = p.addTrack(Kind::Audio), m = p.addTrack(Kind::Audio);
        auto clip = makeAudioClip("x", tone(SR, 220, 330), SR, SR);
        clip->track = x;
        clip->prepare(SR);
        p.addClip(clip);
        auto loud = makeAudioClip("m", tone(SR, 60, 60), SR, SR);
        loud->track = m;
        loud->prepare(SR);
        if (busLoud) p.addClip(loud);
        GroupPtr g = p.groupTracks({m});
        setEffects(g->bus, {fakeSlot(0, 8.0)});
        sidechainFrom(x, g->bus);
        m->muted = !busLoud;
        auto out = play(p, nullptr, 512, SR);
        // x's own share: the output minus the bus's (the bus is m's tone ×8)
        double e = 0;
        for (size_t i = SR / 2 * 2; i < out.size(); i += 2) e += (double)out[i] * out[i];
        return e;
    };
    double quiet = energy(false), loud = energy(true);
    CHECK(quiet > 0 && loud > 0 && loud != quiet);
    // a track listening to the very bus it plays into (a loop): it hears nothing there, and still plays into the bus
    Project p;
    TrackPtr m = p.addTrack(Kind::Audio);
    p.addClip(constantClip(m, SR, 0.25f));
    GroupPtr g = p.groupTracks({m});
    setEffects(g->bus, {fakeSlot(0, 2.0)});
    SlotPtr fx = fakeSlot(0, 1.0);
    std::vector<Cable> cables = chain({fx});
    cables.push_back(Cable(End::source(g->bus.get()), End::sidechain(fx.get())));
    m->effects = std::vector<SlotPtr>{fx};
    m->cables = cables;
    for (bool workersOn : {false, true}) {
        workers::Pool pool(2, false);
        auto out = play(p, workersOn ? &pool : nullptr, 512, SR / 2);
        CHECK_NEAR(out[(size_t)(SR / 4) * 2], 0.5, 1e-6);
    }
}

// --- stems: a bus's holds its tracks; a track's own sound alone ---
static void testStems() {
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    p.addClip(constantClip(a, SR / 2, 0.25f));
    p.addClip(constantClip(b, SR / 2, 0.5f));
    GroupPtr g = p.groupTracks({a});
    setEffects(g->bus, {fakeSlot(0, 2.0)});
    auto bus = render::renderMix(p, 0, SR / 2, SR, 0.0, false, nullptr, g->bus.get());
    auto own = render::renderMix(p, 0, SR / 2, SR, 0.0, false, nullptr, a.get());
    auto other = render::renderMix(p, 0, SR / 2, SR, 0.0, false, nullptr, b.get());
    CHECK(bus && own && other);
    if (bus && own && other) {
        size_t mid = (size_t)(SR / 4) * 2;
        CHECK_NEAR((*bus)[mid], 0.5, 1e-6);    // (a through its bus)
        CHECK_NEAR((*own)[mid], 0.25, 1e-6);   // (a alone)
        CHECK_NEAR((*other)[mid], 0.5, 1e-6);  // (b alone: not the bus)
    }
}

// --- save, load, undo; an old file with groups ---
static void testSaveLoadUndo() {
    util::makeDirs(TMP);
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio), c = p.addTrack(Kind::Audio);
    TrackPtr d = p.addTrack(Kind::Audio), r = p.addTrack(Kind::Return);
    GroupPtr outer = p.groupTracks({a, b});
    GroupPtr inner = p.groupTracks({b});
    GroupPtr folder = p.groupTracks({c});
    folder->foldOnly = true;
    outer->bus->volumeDb = -4.5;
    outer->bus->pan = -0.25;
    setEffects(outer->bus, {builtinSlot("eq"), builtinSlot("compressor")});
    outer->bus->sends = std::vector<Send>{{r, -7.0, true}};
    outer->bus->automation = std::vector<AutomationPtr>{[&] {
        auto lane = std::make_shared<Automation>();
        lane->slot = outer->bus->controls;
        lane->param = automation::PAN;
        lane->setPoints({{0.0, 0.2}, {1.0, 0.8}});
        return lane;
    }()};
    sidechainFrom(d, inner->bus);  // (a sidechain naming a bus)
    CHECK(routing::setInto(d, outer->bus));
    CHECK(routing::setInto(c, a));
    project_io::MasterSettings master, back;
    std::string err, path = TMP + "/buses.json";
    CHECK(project_io::saveProject(path, p, SR, master, err));
    Project q;
    CHECK(project_io::loadProject(path, q, SR, back, err));
    CHECK(q.tracks.size() == 5 && q.groups().size() == 3);
    if (q.tracks.size() == 5 && q.groups().size() == 3) {
        TrackPtr qa = q.tracks[0], qb = q.tracks[1], qc = q.tracks[2], qd = q.tracks[3];
        GroupPtr qo = qa->group, qi = qb->group;
        CHECK(qo && qi && qi->parent == qo && !qo->parent && qo->name == outer->name);
        CHECK(qo && qo->bus->volumeDb == -4.5 && qo->bus->pan == -0.25 && qo->bus->effects.size() == 2);
        CHECK(qo && qo->bus->sends.size() == 1 && qo->bus->sends[0].track == q.tracks[4] && qo->bus->sends[0].preFader);
        CHECK(qo && qo->bus->automation.size() == 1 && qo->bus->automation[0]->slot == qo->bus->controls &&
              qo->bus->automation[0]->points.size() == 2);
        CHECK(qo && qo->bus->name == qo->name && qo->bus->busOf == qo.get());
        CHECK(qc->group && qc->group->foldOnly && qc->into == qa);
        CHECK(qo && qd->into == qo->bus);
        bool listens = false;
        for (auto& cable : *qd->cables) listens = listens || (cable.first.type == End::TrackSource && cable.first.track == qi->bus.get());
        CHECK(listens);
        CHECK(qi && routing::isPlain(*qi->bus));
    }
    // undo: the bus's fader, fold only, routing and nesting, each a step with its name
    history::History h(p, SR);
    outer->bus->volumeDb = 2;
    CHECK(h.check(SR) && h.labels().back() == "Volume");
    folder->foldOnly = false;
    CHECK(h.check(SR) && h.labels().back() == "Group bus");
    CHECK(routing::setInto(c, nullptr));
    CHECK(h.check(SR) && h.labels().back() == "Track output");
    CHECK(routing::setParent(p, inner, nullptr));
    CHECK(h.check(SR));
    CHECK(h.undo(SR) && inner->parent == outer);
    CHECK(h.undo(SR) && c->into == a);
    CHECK(h.undo(SR) && folder->foldOnly);
    CHECK(h.undo(SR) && outer->bus->volumeDb == -4.5);
    CHECK(h.redo(SR) && outer->bus->volumeDb == 2);
    // an old file: groups load as plain buses, as they sounded
    std::ofstream(TMP + "/old.json") << R"({"format": "tinydaw-project", "groups": [{"name": "Drums", "muted": false}],
        "tracks": [{"name": "Kick", "kind": "audio", "group": 0}, {"name": "Snare", "kind": "audio", "group": 0},
        {"name": "Bass", "kind": "audio"}], "clips": []})";
    Project o;
    CHECK(project_io::loadProject(TMP + "/old.json", o, SR, back, err));
    CHECK(o.tracks.size() == 3 && o.groups().size() == 1);
    if (o.groups().size() == 1) {
        GroupPtr og = o.groups()[0];
        CHECK(!og->foldOnly && !og->parent && routing::isPlain(*og->bus) && og->bus->name == "Drums");
        CHECK(routing::target(*o.tracks[0]) == og->bus.get() && routing::target(*o.tracks[2]) == nullptr);
    }
    // saved again: a plain group's keys as before (no bus, parent or fold_only)
    Json again = project_io::describe(o, SR, master);
    const Json& g0 = again["groups"][size_t(0)];
    CHECK(!g0.has("bus") && !g0.has("parent") && !g0.has("fold_only"));
    // a damaged file: a group inside itself, a track routed into itself and a loop of two: cut on loading
    std::ofstream(TMP + "/loop.json") << R"({"format": "tinydaw-project",
        "groups": [{"name": "A", "parent": 1}, {"name": "B", "parent": 0}],
        "tracks": [{"name": "x", "kind": "audio", "group": 0, "output_to": 1}, {"name": "y", "kind": "audio",
        "output_to": 0}, {"name": "z", "kind": "audio", "output_to": 2}], "clips": []})";
    Project l;
    CHECK(project_io::loadProject(TMP + "/loop.json", l, SR, back, err));
    bool loops = false;
    for (auto& t : l.allTracks()) loops = loops || (routing::target(*t) && routing::reaches(*routing::target(*t), t.get()));
    CHECK(!loops && l.tracks.size() == 3 && !l.tracks[2]->into);
}

// --- the worst case: 64 nested groups with effects and sidechains, changed every way while playing ---
static void testSixtyFourNestedGroupsWhilePlaying() {
    workers::Pool pool(workers::workersFor(0), false);
    Project p;
    std::vector<TrackPtr> tracks;
    for (int i = 0; i < 96; i++) tracks.push_back(audioTrack(p, 80.0 + 13 * i, 90.0 + 11 * i, (i % 8) * 600, 0.01, SR / 2));
    TrackPtr r = p.addTrack(Kind::Return);
    setEffects(r, {builtinSlot("delay")});
    std::vector<GroupPtr> groups;  // (kept alive: the audio thread may still be reading one taken out)
    // 64 groups, each inside the one before (64 deep), a track or two in each
    for (int k = 0; k < 64; k++) {
        std::vector<TrackPtr> in(tracks.begin() + k, tracks.end());  // (all from k on: each new group inside the last)
        GroupPtr g = p.groupTracks(in);
        groups.push_back(g);
        if (k % 3 == 0) setEffects(g->bus, {builtinSlot(k % 2 ? "eq" : "compressor")});
        if (k % 5 == 0) g->bus->sends = std::vector<Send>{{r, -12.0, k % 10 == 0}};
        if (k % 7 == 0) setEffects(g->bus, {fakeSlot(k * 3, 0.9)});
    }
    CHECK(groups.back()->parent == groups[groups.size() - 2] && p.groups().size() == 64);
    for (int i = 0; i < 96; i += 9) sidechainFrom(tracks[i], groups[(i * 7) % 64]->bus);  // (tracks listening to buses)
    freshPlugins(p);
    std::atomic<bool> stop{false};
    std::atomic<int> buffers{0};
    std::atomic<bool> finite{true};
    mixer::MixState st;
    st.pool = &pool;
    std::thread audio([&] {
        std::vector<float> buf;
        int64_t pos = 0;
        while (!stop.load()) {
            int n = 64 + (buffers.load() % 7) * 100;
            buf.assign((size_t)n * 2, 0.0f);
            mixer::mixClips(p, buf.data(), 2, n, true, pos % (SR / 2), st, nullptr, SR, buffers.load() % 40 == 0);
            for (float v : buf)
                if (!std::isfinite(v)) finite = false;
            pos += n;
            buffers++;
        }
    });
    // meanwhile: tracks in and out of groups, groups nested and unnested, buses on and off, routing, mute, solo
    std::mt19937 rng(7);
    auto pick = [&](size_t n) { return (size_t)(rng() % n); };
    int refused = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < 6000; round++) {
        TrackPtr t = tracks[pick(tracks.size())];
        GroupPtr g = groups[pick(groups.size())];
        switch (round % 7) {
        case 0: t->group = pick(4) ? g : nullptr; p.gatherGroups(); routing::fixLoops(p); break;
        case 1: if (!routing::setParent(p, g, pick(3) ? groups[pick(groups.size())] : nullptr)) refused++; break;
        case 2: g->foldOnly = !g->foldOnly; routing::fixLoops(p); break;
        case 3: if (!routing::setInto(t, pick(2) ? tracks[pick(tracks.size())] : groups[pick(groups.size())]->bus)) refused++; break;
        case 4: routing::setInto(t, nullptr); g->muted = pick(5) == 0; break;
        case 5: g->soloed = pick(9) == 0; t->soloed = pick(11) == 0; break;
        case 6: g->bus->volumeDb = -(double)pick(12); g->bus->pan = (double)pick(21) / 10 - 1; break;
        }
        if (round % 50 == 0) std::this_thread::yield();
    }
    // let it play a little longer, then stop
    while (buffers.load() < 30) std::this_thread::yield();
    stop = true;
    audio.join();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("64 nested groups: %d buffers mixed while 6000 changes were made (%d loops refused) in %.2f s\n",
                buffers.load(), refused, seconds);
    CHECK(finite.load() && buffers.load() >= 30);
    // the model holds together: no loops, nothing nested in itself, the lanes laid out
    bool loops = false;
    for (auto& t : p.allTracks()) loops = loops || (routing::target(*t) && routing::reaches(*routing::target(*t), t.get()));
    CHECK(!loops);
    for (auto& g : p.groups()) CHECK(routing::canNest(g, nullptr));
    // and it still sounds the same with and without workers, bit for bit
    for (auto& g : groups) g->soloed = g->muted = false;
    for (auto& t : tracks) t->soloed = false;
    auto one = play(p, nullptr, 480, SR / 4), many = play(p, &pool, 480, SR / 4);
    CHECK(sameBits(one, many));
}

// --- the window and the terminal: the group's header row ---
namespace {
void settle() {
    for (int i = 0; i < 6; i++) {
        ui::App::get().advanceTimers(0.05);
        ui::App::get().tick();
    }
}
ui::Action* actionNamed(ui::Menu& menu, const std::string& prefix) {
    for (auto& a : menu.actions())
        if (a->text.rfind(prefix, 0) == 0) return a.get();
    return nullptr;
}
}  // namespace

static void testWindowAndTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    TrackPtr a = w->timeline->addTrack(Kind::Audio), b = w->timeline->addTrack(Kind::Audio),
             c = w->timeline->addTrack(Kind::Audio);
    GroupPtr g = w->timeline->groupTracks({a, b});  // "Group these tracks": a bus
    settle();
    CHECK(g && !g->foldOnly && w->panel->groupFrames.count(g) && w->panel->meters.count(g->bus) &&
          w->panel->mixers.count(g->bus));
    auto ls = lanes::layout(w->project);
    CHECK(!ls.empty() && ls[0].group == g && ls[0].height == lanes::headerHeight(*g) && ls[0].height >= config::TRACK_H);
    // the group's menu: Fold only (no bus), Output, Put in group
    ui::Menu menu;
    w->panel->fillGroupMenu(menu, g);
    ui::Action* only = actionNamed(menu, "Fold only");
    CHECK(only && only->checkable && !only->checked);
    CHECK(actionNamed(menu, "Output: Master") && actionNamed(menu, "Put in group") && actionNamed(menu, "Effects"));
    // a track's Output: Master, the group's bus, the other tracks; a loop greyed out
    CHECK(routing::setInto(c, a));
    ui::Menu tm;
    w->panel->fillTrackMenu(tm, a);
    ui::Action* output = actionNamed(tm, "Output:");
    CHECK(output && output->submenu);
    if (output && output->submenu) {
        ui::Action* intoC = actionNamed(*output->submenu, c->name);
        CHECK(intoC && !intoC->enabled);  // (c plays into a: a into c would loop)
        ui::Action* intoB = actionNamed(*output->submenu, b->name);
        CHECK(intoB && intoB->enabled);
        CHECK(actionNamed(*output->submenu, g->name + " (its group)"));
        if (intoB) ui::Menu::activate(intoB);
        CHECK(a->into == b);
    }
    // the terminal: the group's row as a track's
    tui::Front f(*w);
    tui::Screen screen(110, 30);
    f.pane = tui::Front::Pane::Panel;
    f.laneRow = 0;  // the group's header
    f.key(tui::Key{plat::Key_Right, 0, ""});
    f.key(tui::Key{plat::Key_Right, 0, ""});
    CHECK(g->bus->volumeDb == 2);
    f.key(tui::Key{']', 0, "]"});
    CHECK_NEAR(g->bus->pan, 0.1, 1e-9);
    f.key(tui::Key{'M', 0, "m"});
    CHECK(g->muted && !a->muted);
    f.key(tui::Key{'M', 0, "m"});
    f.key(tui::Key{'S', 0, "s"});
    CHECK(g->soloed);
    f.key(tui::Key{'S', 0, "s"});
    settle();
    f.draw(screen);
    std::string all = screen.lines();
    CHECK(all.find(g->name) != std::string::npos && all.find("+2.0") != std::string::npos);
    std::ofstream(TMP + "/panel.txt") << all;
    // Fold only from its menu (F2): a plain header again, the tracks play straight on
    if (only) ui::Menu::activate(only);
    settle();
    CHECK(g->foldOnly && !w->panel->groupFrames.count(g) && w->panel->groupMeters.count(g));
    CHECK(routing::target(*b) == nullptr);
    f.draw(screen);
    CHECK(screen.lines().find("(fold only)") != std::string::npos);
    CHECK(w->history && w->history->labels().size() >= 2);
    // F1 lists the group row's keys
    f.key(tui::Key{plat::Key_F1, 0, ""});
    std::string seen;
    for (int i = 0; i < 8; i++) {
        f.draw(screen);
        seen += screen.lines();
        f.key(tui::Key{plat::Key_PageDown, 0, ""});
    }
    CHECK(seen.find("A group's row") != std::string::npos);
    delete w;
    app.paintWindows = true;
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    auto t0 = std::chrono::steady_clock::now();
    testSixtyFourNestedGroupsWhilePlaying();
    testFoldersSoundTheSame();
    testBusesOnEveryCore();
    testBusFaderPanEffectsSends();
    testDelayThroughBuses();
    testRoutingAndLoops();
    testMuteSoloThroughBuses();
    testSidechainFromBus();
    testStems();
    testSaveLoadUndo();
    testWindowAndTerminal();
    std::printf("group buses: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    TEST_MAIN_END();
}
