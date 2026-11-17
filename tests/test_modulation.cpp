// Tests of modulation (audio/modulation.h, effects/modulators.cpp): the modulators' values (LFO in time
// with the song and in Hz, Random from a seed, the Level Follower, the Macro), how cables move a knob
// around its base (sums, limits, a bypassed modulator, automation and the user's own edits moving the
// base, a removed cable putting the knob back), modulation in the mixer (moved every 128 timeline
// frames whatever the buffers' size, a follower listening to another track through its sidechain,
// the same bit for bit on several cores), saving and loading, and the effects window's modulator units.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/modulation.h"
#include "audio/patch.h"
#include "audio/workers.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/model.h"
#include "model/project_io.h"
#include "ui/patch_panel.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_host.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"
#include "plugins/plugin.h"

namespace {

const int SR = 48000;

SlotPtr builtinSlot(const char* id) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    fx->prepare(SR, 4096);
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->instance = fx;
    return s;
}

effects::Effect* fxOf(const SlotPtr& s) { return dynamic_cast<effects::Effect*>(aload(s->instance).get()); }

uint32_t paramId(const SlotPtr& s, const std::string& name) {
    for (auto& p : automation::parameters(*s))
        if (p.name == name) return p.id;
    return 0;
}

// a parameter set by its value in its own units (Hz, %, an index of a choice…)
void setValue(const SlotPtr& s, const std::string& key, double value) {
    effects::Effect* e = fxOf(s);
    for (size_t i = 0; i < e->params().size(); i++)
        if (e->params()[i].key == key) aload(s->instance)->setParam((uint32_t)i, e->params()[i].toNormal(value));
}

effects::ModClock playingAt(const TempoMap& map, double seconds, int frames = 128) {
    return modulation::clockAt(map, true, (int64_t)std::llround(seconds * SR), frames, SR);
}

// --- the modulators ---

void testLfo() {
    TempoMap map(120, 4);  // a beat 0.5 s, a bar 2 s
    SlotPtr lfo = builtinSlot("mod_lfo");
    effects::Effect* m = fxOf(lfo);
    CHECK(m && m->isModulator());
    CHECK(modulation::isModulator(lfo.get()) && modulation::modulatorOf(lfo.get()) == m);
    // default: a sine over one bar, in time with the song
    CHECK_NEAR(m->modulation(playingAt(map, 0.0)), 0.0, 1e-9);
    CHECK_NEAR(m->modulation(playingAt(map, 0.5)), 1.0, 1e-9);   // a quarter of the bar
    CHECK_NEAR(m->modulation(playingAt(map, 1.5)), -1.0, 1e-9);  // three quarters
    CHECK_NEAR(m->modulation(playingAt(map, 2.5)), 1.0, 1e-9);   // the next bar the same
    CHECK_NEAR(m->lastModulation(), 1.0, 1e-9);
    // at another tempo, still a bar long (in beats)
    TempoMap slow(60, 4);
    CHECK_NEAR(m->modulation(playingAt(slow, 1.0)), 1.0, 1e-9);
    // in Hz: 2 Hz, a square, one way
    setValue(lfo, "sync", 0);
    setValue(lfo, "rate", 2.0);
    setValue(lfo, "shape", 4);
    setValue(lfo, "direction", 1);
    CHECK_NEAR(m->modulation(playingAt(map, 0.1)), 1.0, 1e-9);  // first half of a 0.5 s cycle: up
    CHECK_NEAR(m->modulation(playingAt(map, 0.3)), 0.0, 1e-9);  // second half: 0 (one way)
    // stopped: it runs on by itself, piece by piece (1/8 s pieces: a quarter cycle at 2 Hz)
    setValue(lfo, "shape", 0);
    setValue(lfo, "direction", 0);
    effects::ModClock stopped = modulation::clockAt(map, false, 0, SR / 8, SR);
    double a = m->modulation(stopped), b = m->modulation(stopped), c = m->modulation(stopped);
    CHECK_NEAR(a, 0.0, 1e-9);
    CHECK_NEAR(b, 1.0, 1e-9);
    CHECK_NEAR(c, 0.0, 1e-9);
}

void testRandomAndMacro() {
    TempoMap map(120, 4);
    SlotPtr r = builtinSlot("mod_random");
    effects::Effect* m = fxOf(r);
    // a quarter-note step: the same value within a step, the same again next time, within -1…1
    double a = m->modulation(playingAt(map, 0.1)), a2 = m->modulation(playingAt(map, 0.4));
    double b = m->modulation(playingAt(map, 0.6));
    CHECK(a == a2);
    CHECK(a != b);
    CHECK(m->modulation(playingAt(map, 0.1)) == a);
    bool inRange = true;
    double lo = 1, hi = -1;
    for (int k = 0; k < 2000; k++) {
        double v = m->modulation(playingAt(map, k * 0.5 + 0.01));
        if (v < -1 || v > 1) inRange = false;
        lo = std::min(lo, v), hi = std::max(hi, v);
    }
    CHECK(inRange && lo < -0.9 && hi > 0.9);  // (spread over the whole range)
    // another seed, other values
    SlotPtr r2 = builtinSlot("mod_random");
    setValue(r2, "seed", 7);
    CHECK(fxOf(r2)->modulation(playingAt(map, 0.1)) != a);
    // Smooth: a glide from the last step's value at the start of a step
    setValue(r, "smooth", 100);
    double start = m->modulation(playingAt(map, 0.5 + 1e-6)), end = m->modulation(playingAt(map, 1.0 - 1e-6));
    CHECK_NEAR(start, a, 1e-3);  // (the step before's value)
    CHECK_NEAR(end, b, 1e-3);
    // a Macro gives its knob
    SlotPtr macro = builtinSlot("mod_macro");
    setValue(macro, "value", 30);
    CHECK_NEAR(fxOf(macro)->modulation(playingAt(map, 0)), 0.3, 1e-9);
}

void testFollower() {
    SlotPtr f = builtinSlot("mod_follower");
    plug::Plugin* p = aload(f->instance).get();
    p->reset();
    std::vector<float> l(512), r(512), ol(512), orr(512);
    const float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    for (int i = 0; i < 512; i++) l[i] = r[i] = 0.5f * (float)std::sin(i * 0.1);
    for (int k = 0; k < 20; k++) p->process(in, nullptr, out, 512);
    double loud = fxOf(f)->modulation({});
    CHECK(loud > 0.4 && loud <= 0.5);
    bool silentOut = true;
    for (int i = 0; i < 512; i++) silentOut = silentOut && ol[i] == 0 && orr[i] == 0;
    CHECK(silentOut);  // (no sound through a modulator)
    std::fill(l.begin(), l.end(), 0.f), std::fill(r.begin(), r.end(), 0.f);
    for (int k = 0; k < 20; k++) p->process(in, nullptr, out, 512);  // 0.2 s of silence: it falls back
    double after = fxOf(f)->modulation({});
    CHECK(after < loud * 0.5);
    // listening to its sidechain
    setValue(f, "listen", 1);
    std::vector<float> sl(512, 0.8f), sr(512, 0.8f);
    const float* side[2] = {sl.data(), sr.data()};
    for (int k = 0; k < 20; k++) p->process(in, side, out, 512);
    CHECK(fxOf(f)->modulation({}) > 0.7);
}

// --- cables ---

void testCables() {
    TempoMap map(120, 4);
    auto track = std::make_shared<Track>("T", Kind::Audio);
    SlotPtr macro = builtinSlot("mod_macro"), macro2 = builtinSlot("mod_macro"), util = builtinSlot("utility");
    track->effects = std::vector<SlotPtr>{macro, macro2, util};
    plug::Plugin* u = aload(util->instance).get();
    uint32_t w = paramId(util, "Width");
    u->setParam(w, 0.25);  // the base
    ModCable c;
    c.from = macro.get(), c.to = util.get(), c.param = "Width", c.depth = 0.5;
    track->modCables = std::vector<ModCable>{c};
    modulation::resolve(*track);
    CHECK(c.state->hasTarget && c.state->paramId == w);
    CHECK_NEAR(c.state->base, 0.25, 1e-12);
    setValue(macro, "value", 60);
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 0.25 + 0.5 * 0.6, 1e-9);
    // two cables to one knob add up, kept within 0…1
    ModCable c2;
    c2.from = macro2.get(), c2.to = util.get(), c2.param = "Width", c2.depth = 1.0;
    track->modCables = std::vector<ModCable>{c, c2};
    modulation::resolve(*track);
    setValue(macro2, "value", 20);
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 0.25 + 0.3 + 0.2, 1e-9);
    setValue(macro2, "value", 100);
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 1.0, 1e-12);
    // a bypassed modulator moves nothing
    macro2->bypass = true;
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 0.25 + 0.3, 1e-9);
    macro2->bypass = false;
    // an automation lane on it moves its base (not the knob itself)
    CHECK(modulation::takeAutomation(*track, util.get(), w, 0.1));
    CHECK(!modulation::takeAutomation(*track, util.get(), paramId(util, "Gain"), 0.1));
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 1.0, 1e-9);  // (0.1 + 0.3 + 1.0, kept within 0…1)
    setValue(macro2, "value", 0);
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 0.1 + 0.3, 1e-9);
    // the user turns the knob in its window: the base follows; the modulation's own value doesn't count
    Project project;
    project.tracks = std::vector<TrackPtr>{track};
    u->setParam(w, 0.05);
    modulation::userEdited(project, util.get(), {w});
    CHECK_NEAR(c.state->base, 0.05, 1e-12);
    modulation::apply(*track->modCables, playingAt(map, 0));
    CHECK_NEAR(u->getParam(w), 0.05 + 0.3, 1e-9);
    modulation::userEdited(project, util.get(), {w});  // (the value now is the modulation's: base unchanged)
    CHECK_NEAR(c.state->base, 0.05, 1e-12);
    // a cable removed: the knob back at its base
    modulation::release(c);
    CHECK_NEAR(u->getParam(w), 0.05, 1e-12);
    // cables whose ends are gone are dropped
    track->effects = std::vector<SlotPtr>{macro, util};
    CHECK(modulation::kept(*track).size() == 1);
    // targets: everything but the modulator itself
    auto targets = modulation::targetsFor(*track, macro.get());
    CHECK(targets.size() == 1 && targets[0] == util);
    // steps every 128 timeline frames
    CHECK(modulation::toNextStep(true, 0, 0, 512) == 128);
    CHECK(modulation::toNextStep(true, 100, 0, 512) == 28);
    CHECK(modulation::toNextStep(true, 100, 0, 10) == 10);
    CHECK(modulation::toNextStep(false, 5000, 0, 512) == 128);
}

// --- in the mixer ---

AudioPtr tone(int64_t frames, double f) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    for (int64_t i = 0; i < frames; i++)
        b->samples[(size_t)i * 2] = b->samples[(size_t)i * 2 + 1] = (float)(0.4 * std::sin(2 * M_PI * f * i / SR));
    return b;
}

TrackPtr audioTrack(Project& p, double f, double seconds) {
    auto t = p.addTrack(Kind::Audio);
    auto clip = makeAudioClip("c", tone((int64_t)(seconds * SR), f), SR, SR);
    clip->track = t;
    clip->prepare(SR);
    p.addClip(clip);
    return t;
}

// a pad (an LFO moving its Utility's gain), and a kick whose loudness ducks the pad's second Utility
// through a Level Follower listening to the kick on its sidechain
void buildProject(Project& p) {
    auto kick = audioTrack(p, 60, 4);
    auto pad = audioTrack(p, 440, 4);
    SlotPtr lfo = builtinSlot("mod_lfo"), gain = builtinSlot("utility"), follower = builtinSlot("mod_follower"),
            duck = builtinSlot("utility");
    setValue(lfo, "length", 3);  // 1/4: a beat-long cycle
    setValue(follower, "listen", 1);
    pad->effects = std::vector<SlotPtr>{lfo, gain, follower, duck};
    std::vector<Cable> cables = chain({gain, duck});
    cables.push_back(Cable(End::source(kick.get()), End::sidechain(follower.get())));
    pad->cables = cables;
    ModCable a, b;
    a.from = lfo.get(), a.to = gain.get(), a.param = "Gain", a.depth = 0.2;
    b.from = follower.get(), b.to = duck.get(), b.param = "Gain", b.depth = -0.5;
    pad->modCables = std::vector<ModCable>{a, b};
    modulation::resolve(*pad);
    patch::ensureStates(p);
}

std::vector<float> mixAll(Project& p, workers::Pool* pool, const std::vector<int>& sizes, int64_t total) {
    mixer::MixState st;
    st.pool = pool;
    std::vector<float> all, buf;
    int64_t pos = 0;
    for (size_t k = 0; pos < total; k++) {
        int n = sizes[k % sizes.size()];
        buf.assign((size_t)n * 2, 0.0f);
        mixer::mixClips(p, buf.data(), 2, n, true, pos, st, nullptr, SR, k == 0);
        all.insert(all.end(), buf.begin(), buf.end());
        pos += n;
    }
    all.resize((size_t)total * 2);
    return all;
}

int64_t differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int64_t d = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) d++;
    return d;
}

double rms(const std::vector<float>& a, int64_t from, int64_t to) {
    double s = 0;
    for (int64_t i = from; i < to; i++) s += (double)a[(size_t)i * 2] * a[(size_t)i * 2];
    return std::sqrt(s / std::max<int64_t>(1, to - from));
}

void testInTheMixer() {
    const int64_t total = SR * 2;
    Project p1, p2, p3, p4;
    buildProject(p1);
    buildProject(p2);
    buildProject(p3);
    buildProject(p4);
    auto a = mixAll(p1, nullptr, {256}, total);
    auto b = mixAll(p2, nullptr, {100, 333, 1024, 7}, total);  // (other buffers: the knobs move at the same frames)
    int64_t d1 = differing(a, b);
    CHECK(d1 == 0);
    if (d1) printf("other buffer sizes: %lld samples differ\n", (long long)d1);
    workers::Pool pool(5);
    auto c = mixAll(p3, &pool, {256}, total);  // (on several cores: the same buffers)
    int64_t d2 = differing(a, c);
    CHECK(d2 == 0);
    if (d2) printf("several cores: %lld samples differ\n", (long long)d2);
    // the modulation is heard: the pad's level changes over the LFO's beat-long cycle
    auto d = mixAll(p4, nullptr, {256}, total);
    double r1 = rms(d, SR / 8 - 2000, SR / 8 + 2000), r2 = rms(d, SR * 3 / 8 - 2000, SR * 3 / 8 + 2000);
    CHECK(std::fabs(r1 - r2) > 0.01);
    // the follower hears the kick through its sidechain
    CHECK(fxOf(p4.tracks[1]->effects[2])->lastModulation() > 0.2);
    // without the modulation cables the level stays put
    Project p5;
    buildProject(p5);
    for (auto& m : p5.tracks[1]->modCables) modulation::release(m);
    p5.tracks[1]->modCables = std::vector<ModCable>{};
    auto e = mixAll(p5, nullptr, {256}, total);
    double e1 = rms(e, SR / 8 - 2000, SR / 8 + 2000), e2 = rms(e, SR * 3 / 8 - 2000, SR * 3 / 8 + 2000);
    CHECK(std::fabs(e1 - e2) < 0.002);
}

void testSaving() {
    Project p;
    buildProject(p);
    TrackPtr pad = p.tracks[1];
    pad->modCables[0].state->base = 0.42;
    project_io::MasterSettings master;
    std::string text = project_io::describe(p, SR, master).dump();
    CHECK(text.find("\"modulation\"") != std::string::npos);
    // a track without modulation saves no such key (older files unchanged)
    Project plain;
    audioTrack(plain, 220, 1);
    CHECK(project_io::describe(plain, SR, master).dump().find("modulation") == std::string::npos);
    // saved and loaded: the same cables, depths and bases
    std::string folder = "build/test-tmp/modulation";
    util::makeDirs(folder);
    std::string path = folder + "/m.json", error;
    CHECK(project_io::saveProject(path, p, SR, master, error));
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, error));
    CHECK(q.tracks.size() == 2);
    if (q.tracks.size() == 2) {
        TrackPtr t = q.tracks[1];
        CHECK(t->modCables.size() == 2);
        if (t->modCables.size() == 2 && t->effects.size() == 4) {
            const ModCable& x = t->modCables[0];
            CHECK(x.from == t->effects[0].get() && x.to == t->effects[1].get() && x.param == "Gain" && x.depth == 0.2);
            CHECK_NEAR(x.state->base, 0.42, 1e-12);
            CHECK(t->modCables[1].depth == -0.5 && t->modCables[1].to == t->effects[3].get());
        }
    }
}

// --- the effects window ---

void testWindow() {
    Project project;
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(1280, 800);
    ui::App::get().mainWindow = &main;
    PluginHost host(&main, project, engine);
    TrackPtr t = project.addTrack(Kind::Audio);
    SlotPtr lfo = builtinSlot("mod_lfo"), util = builtinSlot("utility"), macro = builtinSlot("mod_macro");
    t->effects = std::vector<SlotPtr>{lfo, util, macro};
    t->cables = chain({util});
    int changes = 0;
    auto* fx = new PluginsDialog(&main, host, "Effects: T", project, t, [&] { ++changes; });
    fx->open();
    PatchPanel* panel = fx->panel;
    CHECK(panel->units().size() == 3);
    if (panel->units().size() == 3) {
        CHECK(panel->units()[0].modulator && !panel->units()[1].modulator && panel->units()[2].modulator);
        CHECK(panel->units()[0].status == "Moves nothing" && panel->units()[0].idle && !panel->units()[0].mix);
    }
    CHECK(panel->modLinks().empty());
    ModCable c;
    c.from = lfo.get(), c.to = util.get(), c.param = "Width", c.depth = 0.5;
    fx->setModCables({c});
    CHECK(changes == 1);
    CHECK(panel->units()[0].status == "Moves 1 knob" && !panel->units()[0].idle);
    CHECK(panel->modLinks().size() == 1);
    if (panel->modLinks().size() == 1)
        CHECK(panel->modLinks()[0].from == 0 && panel->modLinks()[0].to == 1 && panel->modLinks()[0].label == "Width +50 %");
    // the modulator's menu lists its cable
    ui::Menu menu;
    fx->fillModulatorMenu(menu, lfo);
    bool listed = false;
    for (auto& a : menu.actions())
        if (a->text.find("Utility: Width") != std::string::npos) listed = true;
    CHECK(listed);
    // removing the moved effect takes its cable with it, and the knob goes back to its base
    modulation::resolve(*t);
    double base = c.state->base;
    modulation::apply(*t->modCables, modulation::clockAt(*project.timing.audioMap(), true, SR / 4, 128, SR));
    CHECK(aload(util->instance)->getParam(paramId(util, "Width")) != base);
    fx->setPatch(std::vector<SlotPtr>{lfo, macro}, std::nullopt);
    CHECK(t->modCables.empty());
    CHECK(panel->modLinks().empty());
    CHECK_NEAR(aload(util->instance)->getParam(paramId(util, "Width")), base, 1e-12);
    fx->close();
}

// a plugin (the VST3 test effect, or TINYDAW_MOD_PLUGIN / TINYDAW_MOD_PARAM: e.g. a Windows one through
// Wine, by hand) has a knob moved by a Macro through the mixer: its parameter and its sound follow
void testPluginKnob() {
    std::string path = util::absolutePath(check::testDir()) + "/TinyTestFx.vst3", name = "Gain";
    if (const char* e = getenv("TINYDAW_MOD_PLUGIN")) path = e;
    if (const char* e = getenv("TINYDAW_MOD_PARAM")) name = e;
    std::string error;
    auto loaded = plug::Plugin::load(path, "", error);
    if (!loaded) {
        printf("(skipped: %s: %s)\n", path.c_str(), error.c_str());
        return;
    }
    loaded->prepare(SR, 4096);
    Project p;
    auto t = audioTrack(p, 330, 2);
    auto fx = std::make_shared<PluginSlot>();
    fx->path = path, fx->name = loaded->name(), fx->kind = "effect";
    fx->instance = std::shared_ptr<plug::Plugin>(loaded.release());
    SlotPtr macro = builtinSlot("mod_macro");
    t->effects = std::vector<SlotPtr>{macro, fx};
    t->cables = chain({fx});
    uint32_t id = paramId(fx, name);
    plug::Plugin* plugin = aload(fx->instance).get();
    plugin->setParam(id, 0.2);
    ModCable c;
    c.from = macro.get(), c.to = fx.get(), c.param = name, c.depth = 0.6;
    t->modCables = std::vector<ModCable>{c};
    modulation::resolve(*t);
    patch::ensureStates(p);
    CHECK(c.state->hasTarget);
    CHECK_NEAR(c.state->base, 0.2, 1e-6);
    mixer::MixState st;
    std::vector<float> buf(512 * 2);
    auto level = [&](int64_t at) {
        double peak = 0;
        for (int k = 0; k < 8; k++) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            mixer::mixClips(p, buf.data(), 2, 512, true, at + k * 512, st, nullptr, SR, k == 0);
            if (k >= 4)
                for (float x : buf) peak = std::max(peak, (double)std::fabs(x));
        }
        return peak;
    };
    setValue(macro, "value", 0);
    double quiet = level(0);
    CHECK_NEAR(plugin->getParam(id), 0.2, 1e-6);
    setValue(macro, "value", 100);
    double loud = level(SR / 2);
    CHECK_NEAR(plugin->getParam(id), 0.8, 1e-6);
    printf("%s: %s moved 0.2 -> 0.8, peak %.3f -> %.3f\n", fx->name.c_str(), name.c_str(), quiet, loud);
    CHECK(loud != quiet);
    modulation::release(c);
    CHECK_NEAR(plugin->getParam(id), 0.2, 1e-6);
}

}  // namespace

int main() {
    testLfo();
    testRandomAndMacro();
    testFollower();
    testCables();
    testInTheMixer();
    testSaving();
    testPluginKnob();
    testWindow();
    TEST_MAIN_END();
}
