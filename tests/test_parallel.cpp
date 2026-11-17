// Tests of the worker pool (audio/workers.h) and of mixing on several cores (mixer.h, SEVERAL CORES):
// every job of a run is done exactly once, jobs may wait for earlier ones, and a project mixed with
// workers sounds exactly — bit for bit — as it does on one thread, whatever its routing (sidechains,
// chains of sidechains, muted tracks feeding sidechains, sends, returns sending to returns, solo, the
// master's sidechain, frozen tracks, automation, instruments) and however the buffers fall.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/patch.h"
#include "audio/render.h"
#include "audio/workers.h"
#include "check.h"
#include "config.h"
#include "effects/effects.h"
#include "model/model.h"
#include "plugins/plugin.h"
#include "core/util.h"

namespace {

const int SR = 48000;

// --- the pool ---

struct Counts {
    std::vector<std::atomic<int>> calls;
    explicit Counts(int n) : calls(n) {}
};

void testPoolRunsEachJobOnce() {
    workers::Pool pool(7);
    CHECK(pool.threads() == 7);
    bool exact = true;
    for (int round = 0; round < 3000; round++) {
        int jobs = 1 + round % 97;
        Counts counts(jobs);
        pool.run(
            jobs, [](void* ctx, int i) { static_cast<Counts*>(ctx)->calls[i].fetch_add(1); }, &counts);
        for (int i = 0; i < jobs; i++)
            if (counts.calls[i].load() != 1) exact = false;
    }
    CHECK(exact);
    // no workers: everything on the calling thread, in order
    workers::Pool none(0);
    std::vector<int> order;
    none.run(
        5, [](void* ctx, int i) { static_cast<std::vector<int>*>(ctx)->push_back(i); }, &order);
    CHECK((order == std::vector<int>{0, 1, 2, 3, 4}));
}

struct Chain {
    std::vector<std::atomic<char>> done;
    std::vector<int> seenBefore;
    explicit Chain(int n) : done(n), seenBefore(n, -1) {}
};

void testPoolJobsMayWaitForEarlierOnes() {
    // each job waits for the one before it (the worst case of a chain of sidechains): never stuck
    workers::Pool pool(5);
    for (int round = 0; round < 200; round++) {
        Chain chain(40);
        pool.run(
            40,
            [](void* ctx, int i) {
                Chain& c = *static_cast<Chain*>(ctx);
                if (i > 0)
                    while (!c.done[i - 1].load(std::memory_order_acquire)) std::this_thread::yield();
                c.seenBefore[i] = i > 0 ? (int)c.done[i - 1].load() : 1;
                c.done[i].store(1, std::memory_order_release);
            },
            &chain);
        bool all = true;
        for (int i = 0; i < 40; i++) all = all && chain.done[i].load() && chain.seenBefore[i] == 1;
        CHECK(all);
        if (!all) break;
    }
}

void testWorkersFor() {
    int cores = (int)std::thread::hardware_concurrency();
    CHECK(workers::workersFor(1) == 0);  // the audio thread alone
    CHECK(workers::workersFor(0) == std::max(0, std::min(cores - 1, workers::MAX_WORKERS)));
    CHECK(workers::workersFor(2) == std::min(1, std::max(0, cores - 1)));
    CHECK(workers::workersFor(1000) == workers::workersFor(0));
}

// --- projects ---

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

AudioPtr tone(int64_t frames, double f1, double f2, double noise = 0) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    uint32_t seed = (uint32_t)(f1 * 7 + f2);
    for (int64_t i = 0; i < frames; i++) {
        double t = (double)i / SR;
        seed = seed * 1664525u + 1013904223u;
        double n = noise * ((double)(seed >> 8) / (1 << 24) - 0.5);
        double env = std::fmod(t, 0.5) < 0.05 ? 1.0 : 0.3;  // (bursts: something for compressors to act on)
        b->samples[(size_t)i * 2] = (float)(env * 0.3 * std::sin(2 * M_PI * f1 * t) + n);
        b->samples[(size_t)i * 2 + 1] = (float)(env * 0.3 * std::sin(2 * M_PI * f2 * t) - n);
    }
    return b;
}

TrackPtr audioTrack(Project& p, double f1, double f2, int64_t start, double noise = 0) {
    auto t = p.addTrack(Kind::Audio);
    auto clip = makeAudioClip("c", tone(SR * 4, f1, f2, noise), SR, SR);
    clip->track = t;
    clip->start = start;
    clip->prepare(SR);
    p.addClip(clip);
    return t;
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

// What the scenarios vary.
struct Options {
    bool returnToReturn = false;  // a return sending to another (the returns then go one after the other)
    bool solo = false;
    bool masterSidechain = false;
};

void buildProject(Project& p, const Options& o) {
    // plain tracks with effects
    for (int i = 0; i < 6; i++) {
        auto t = audioTrack(p, 110.0 * (i + 1), 165.0 * (i + 1), SR / 8 * i, 0.02 * i);
        t->pan = -0.6 + 0.2 * i;
        t->delayMs = (i % 3) - 1.0;
        if (i % 2)
            setEffects(t, {builtinSlot("chorus"), builtinSlot("eq")});
        else
            setEffects(t, {builtinSlot("delay", 0.5)});
    }
    // a muted "ghost" kick feeding a sidechain; a chain of listeners: c listens to b, b to the kick
    auto kick = audioTrack(p, 55, 55, 0);
    kick->muted = true;
    auto b = audioTrack(p, 300, 450, SR / 3, 0.05);
    sidechainFrom(b, kick);
    auto c = audioTrack(p, 600, 900, SR / 5);
    sidechainFrom(c, b);
    // volume automation
    auto lane = std::make_shared<Automation>();
    lane->slot = c->controls;
    lane->param = automation::VOLUME;
    lane->setPoints({{0.0, 60.0 / 66}, {1.0, 40.0 / 66}, {2.5, 62.0 / 66}});
    c->automation = std::vector<AutomationPtr>{lane};
    // the built-in synth
    auto syn = p.addTrack(Kind::Midi);
    syn->needsInstrument = false;
    auto mc = makeMidiClip("synth", {{0.0, 0.4, 60, 100}, {0.5, 1.2, 67, 90}, {1.5, 2.5, 64, 80}}, 3.0, SR);
    mc->track = syn;
    mc->start = SR / 4;
    mc->prepare(SR);
    p.addClip(mc);
    // a frozen track
    auto f = p.addTrack(Kind::Audio);
    auto frozen = makeAudioClip("frozen", tone(SR * 2, 220, 330), SR, SR);
    frozen->start = SR / 2;
    f->frozen = frozen;
    f->pan = 0.5;
    // returns with sends
    auto r1 = p.addTrack(Kind::Return);
    setEffects(r1, {builtinSlot("reverb")});
    auto r2 = p.addTrack(Kind::Return);
    setEffects(r2, {builtinSlot("delay")});
    for (auto& t : *p.tracks)
        if (t->kind != Kind::Return && t != kick) t->sends = std::vector<Send>{{r1, -9.0}, {r2, -12.0}};
    if (o.returnToReturn) r1->sends = std::vector<Send>{{r2, -6.0}};
    if (o.solo) {
        b->soloed = true;
        syn->soloed = true;
    }
    // the master: a limiter (look-ahead), and maybe a compressor listening to a track
    if (o.masterSidechain) {
        auto comp = builtinSlot("compressor");
        auto limiter = builtinSlot("limiter");
        p.masterEffects = std::vector<SlotPtr>{comp, limiter};
        std::vector<Cable> cables = chain({comp, limiter});
        cables.push_back(Cable(End::source(syn.get()), End::sidechain(comp.get())));
        p.masterCables = cables;
    } else {
        auto limiter = builtinSlot("limiter");
        p.masterEffects = std::vector<SlotPtr>{limiter};
        p.masterCables = chain({limiter});
    }
    patch::ensureStates(p);
    for (auto& s : p.pluginSlots()) aload(s->instance)->reset();
}

// The project mixed in buffers of `sizes` (cycled), playing from 0 with a jump half way and a few
// stopped buffers, with or without `pool`: everything that came out.
std::vector<float> mixAll(Project& p, workers::Pool* pool, const std::vector<int>& sizes, int64_t total) {
    mixer::MixState st;
    st.pool = pool;
    std::vector<float> all;
    std::vector<float> buf;
    int64_t pos = 0, played = 0;
    bool restart = true;
    bool jumped = false;
    for (size_t k = 0; played < total; k++) {
        int n = sizes[k % sizes.size()];
        buf.assign((size_t)n * 2, 0.0f);
        bool stopped = k % 50 == 49;  // (now and then the transport stops for a buffer: plugins only)
        mixer::mixClips(p, buf.data(), 2, n, !stopped, pos, st, nullptr, SR, restart || stopped);
        restart = stopped;
        all.insert(all.end(), buf.begin(), buf.end());
        pos += n;
        played += n;
        if (!jumped && played > total / 2) {  // (the playhead moved: delay lines cleared, notes off)
            pos = SR / 3;
            restart = jumped = true;
        }
    }
    return all;
}

int64_t differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int64_t d = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) d++;
    return d;
}

double loudest(const std::vector<float>& a) {
    double m = 0;
    for (float x : a) m = std::max(m, (double)std::fabs(x));
    return m;
}

void testParallelMixIsBitIdentical() {
    workers::Pool pool(7);
    const int64_t total = SR * 3;
    struct Case {
        const char* name;
        Options o;
        std::vector<int> sizes;
    };
    std::vector<Case> cases = {
        {"plain, 256-frame buffers", {}, {256}},
        {"uneven buffers", {}, {64, 1000, 333, 7, 512}},
        {"a return sending to a return", {true, false, false}, {256, 480}},
        {"solo", {false, true, false}, {128}},
        {"the master's sidechain", {false, false, true}, {256, 100}},
        {"everything", {true, true, true}, {512, 31}},
    };
    for (auto& c : cases) {
        Project one, many;
        buildProject(one, c.o);
        buildProject(many, c.o);
        auto a = mixAll(one, nullptr, c.sizes, total);
        auto b = mixAll(many, &pool, c.sizes, total);
        int64_t d = differing(a, b);
        CHECK(d == 0);
        CHECK(loudest(a) > 0.01);  // (something played)
        if (d) printf("%s: %lld samples differ\n", c.name, (long long)d);
    }
}

// a tap: told on which thread, and how often it heard of the tracks' work
struct RecordingTap : mixer::TrackTap {
    int works = 0;
    std::thread::id thread = std::this_thread::get_id();
    bool otherThread = false, negative = false;
    void silent(const Track*, int) override { here(); }
    void sound(const Track*, const float* const[2], int, int, int) override { here(); }
    void work(const Track*, double seconds) override {
        works++;
        if (seconds < 0) negative = true;
        here();
    }
    void here() {
        if (std::this_thread::get_id() != thread) otherThread = true;
    }
};

void testTapAndWorkOnCallingThread() {
    workers::Pool pool(4);
    Project p;
    buildProject(p, {});
    mixer::MixState st;
    st.pool = &pool;
    RecordingTap tap;
    std::vector<float> buf(512 * 2);
    for (int k = 0; k < 20; k++) mixer::mixClips(p, buf.data(), 2, 512, true, k * 512, st, &tap, SR, k == 0);
    CHECK(!tap.otherThread);
    CHECK(!tap.negative);
    CHECK(tap.works == 20 * (int)p.tracks.size());  // (told how long each track took, every buffer)
}

void testRenderSameWithAnyCores() {
    // a render (its own pool, render.cpp) is the same with all cores or one
    Project p1, p2;
    buildProject(p1, {true, false, true});
    buildProject(p2, {true, false, true});
    workers::setChosenCores(1);
    auto one = render::renderMix(p1, 0, SR * 2, SR);
    workers::setChosenCores(0);
    auto all = render::renderMix(p2, 0, SR * 2, SR);
    CHECK(one && all);
    if (one && all) {
        CHECK(differing(*one, *all) == 0);
        CHECK(loudest(*one) > 0.01);
    }
}

void testParallelIsFaster() {
    // with many busy tracks several cores should help (reported; checked only loosely, as a loaded
    // machine may differ)
    if (std::thread::hardware_concurrency() < 4) return;
    auto build = [](Project& p) {
        for (int i = 0; i < 24; i++) {
            auto t = audioTrack(p, 100.0 + i * 20, 150.0 + i * 20, 0, 0.05);
            setEffects(t, {builtinSlot("reverb"), builtinSlot("chorus"), builtinSlot("multiband_compressor")});
        }
        patch::ensureStates(p);
    };
    Project p1, p2;
    build(p1);
    build(p2);
    workers::Pool pool(workers::workersFor(0));
    auto time = [](Project& p, workers::Pool* pool) {
        auto t0 = std::chrono::steady_clock::now();
        auto out = mixAll(p, pool, {256}, SR * 2);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    double single = time(p1, nullptr), multi = time(p2, &pool);
    printf("24 busy tracks, 2 s of sound: one thread %.3f s, %d threads %.3f s (%.1fx)\n", single,
           pool.threads() + 1, multi, single / multi);
    CHECK(multi < single);
}

// a plugin file loaded into a slot, ready to play ("" if it can't be)
SlotPtr pluginSlot(const std::string& path, const char* kind) {
    std::string error;
    auto p = plug::Plugin::load(path, "", error);
    if (!p) {
        printf("(skipped: %s: %s)\n", path.c_str(), error.c_str());
        return nullptr;
    }
    p->prepare(SR, 4096);
    auto s = std::make_shared<PluginSlot>();
    s->path = path;
    s->name = p->name();
    s->kind = kind;
    s->instance = std::shared_ptr<plug::Plugin>(p.release());
    return s;
}

void buildPluginProject(Project& p, const std::string& fx, const std::string& synth) {
    for (int i = 0; i < 4; i++) {
        auto t = audioTrack(p, 200.0 + 50 * i, 300.0 + 50 * i, SR / 10 * i, 0.03);
        if (auto s = pluginSlot(fx, "effect")) setEffects(t, {s, builtinSlot("chorus")});
    }
    for (int i = 0; i < 3; i++) {
        auto t = p.addTrack(Kind::Midi);
        t->needsInstrument = false;
        if (auto s = pluginSlot(synth, "instrument")) t->instrument = s;
        auto mc = makeMidiClip("m", {{0.0, 0.3, 60.0 + i * 4, 100}, {0.4, 1.4, 67.0 + i, 90}, {1.5, 2.0, 55, 70}}, 2.5,
                               SR);
        mc->track = t;
        mc->start = SR / 5 * i;
        mc->prepare(SR);
        p.addClip(mc);
    }
    patch::ensureStates(p);
    for (auto& s : p.pluginSlots())
        if (auto inst = aload(s->instance)) inst->reset();
}

void testPluginsOnWorkers() {
    // real plugins (the test VST3 effect and VST2 synth) processed on worker threads sound the same as on one
    std::string dir = util::absolutePath(check::testDir());
    std::string fx = dir + "/TinyTestFx.vst3", synth = dir + "/TinyTestSynth.so";
    // TINYDAW_PARALLEL_FX / _SYNTH: other plugins to try (e.g. Windows ones through Wine), by hand
    if (const char* e = getenv("TINYDAW_PARALLEL_FX")) fx = e;
    if (const char* e = getenv("TINYDAW_PARALLEL_SYNTH")) synth = e;
    Project one, many;
    buildPluginProject(one, fx, synth);
    buildPluginProject(many, fx, synth);
    int loaded = 0;
    for (auto& s : many.pluginSlots())
        if (aload(s->instance) && !s->isControls() && s->path.rfind("builtin:", 0) != 0) loaded++;
    CHECK(loaded == 7);
    workers::Pool pool(6);
    auto a = mixAll(one, nullptr, {256, 300}, SR * 2);
    auto b = mixAll(many, &pool, {256, 300}, SR * 2);
    int64_t d = differing(a, b);
    bool strict = !getenv("TINYDAW_PARALLEL_FX") && !getenv("TINYDAW_PARALLEL_SYNTH");
    if (strict) CHECK(d == 0);  // (other plugins needn't be deterministic: only reported)
    printf("plugins on workers: %lld of %zu samples differ, loudest %.3f\n", (long long)d, a.size(), loudest(b));
    CHECK(loudest(b) > 0.01);
    for (auto& s : many.pluginSlots()) CHECK(!s->failed.load());
}

}  // namespace

int main() {
    Project host;
    audio::Engine e(host);  // (installs the sampler and MIDI effects for clips being prepared)
    testPoolRunsEachJobOnce();
    testPoolJobsMayWaitForEarlierOnes();
    testWorkersFor();
    testParallelMixIsBitIdentical();
    testTapAndWorkOnCallingThread();
    testRenderSameWithAnyCores();
    testPluginsOnWorkers();
    testParallelIsFaster();
    TEST_MAIN_END();
}
