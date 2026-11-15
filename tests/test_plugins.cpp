// The plugin host (plugins/): the test plugins of `make plugins` (a VST2 synth, a VST3 file with an
// effect and a single-component plugin) loaded, processed and asked; states; the plugin library's
// scan and cache; the kind finder's probe process. No windows are opened here
// (tools/plugin_editor_check.cpp opens the editors, by hand).
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "plugins/library.h"
#include "plugins/param_changes.h"
#include "plugins/plugin.h"
#include "plugins/probe.h"

using namespace plug;

namespace {

std::string testDir;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(int n, float v = 0) : l((size_t)n, v), r((size_t)n, v) {}
    float* const* out() {
        ptrs[0] = l.data();
        ptrs[1] = r.data();
        return ptrs;
    }
    const float* const* in() {
        ptrs[0] = l.data();
        ptrs[1] = r.data();
        return ptrs;
    }
    float* ptrs[2];
};

double rms(const std::vector<float>& x, size_t from = 0) {
    double s = 0;
    for (size_t i = from; i < x.size(); i++)
        s += x[i] * x[i];
    return x.size() > from ? std::sqrt(s / (double)(x.size() - from)) : 0;
}

// the frequency of a sine, from its upward zero crossings
double frequency(const std::vector<float>& x, double rate) {
    int first = -1, last = -1, count = 0;
    for (size_t i = 1; i < x.size(); i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            if (first < 0)
                first = (int)i;
            last = (int)i;
            count++;
        }
    return count > 1 ? (count - 1) * rate / (last - first) : 0;
}

bool run(const std::string& cmd) { return system(cmd.c_str()) == 0; }

std::string fxBundle() { return testDir + "/TinyTestFx.vst3"; }
std::string synthFile() { return testDir + "/TinyTestSynth.so"; }

int moduleCount(const std::string& binary, int which) {
    void* h = dlopen(binary.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!h)
        return -100;
    auto counts = (int (*)(int))dlsym(h, "TinyTestFx_counts");
    int n = counts ? counts(which) : -101;
    dlclose(h);
    return n;
}

void testPaths() {
    std::string bin = fxBundle() + "/Contents/x86_64-linux/TinyTestFx.so";
    CHECK(vst3Binary(fxBundle()) == bin);
    CHECK(vst3Binary(bin) == bin);
    CHECK(isVst3Path(fxBundle()));
    CHECK(isVst3Path(fxBundle() + "/"));
    CHECK(isVst3Path(bin));
    CHECK(!isVst3Path(synthFile()));
    CHECK(isVst2Path(synthFile()));
    CHECK(loadableFile(fxBundle()) == bin);
    CHECK(loadableFile(synthFile()) == synthFile());
    CHECK(isVst2File(synthFile()));
    CHECK(!isVst2File(bin));
    CHECK(!isVst2File(fxBundle() + "/Contents/Resources/moduleinfo.json"));
    CHECK(!isVst2File(testDir + "/nothing-here.so"));
    KindList classes;
    CHECK(readModuleinfo(fxBundle(), classes));
    CHECK(classes.size() == 2);
    if (classes.size() == 2) {
        CHECK(classes[0].first == "Tiny Test Fx" && classes[0].second == EFFECT);
        CHECK(classes[1].first == "Tiny Test Single" && classes[1].second == EFFECT);
    }
    CHECK(kindOf("Instrument|Synth") == INSTRUMENT);
    CHECK(kindOf("Fx|EQ") == EFFECT);
    CHECK(kindOf("Analyzer") == EFFECT);
    CHECK(kindOf("Tools") == "");
}

void testVst3Classes() {
    std::string error;
    auto classes = vst3Classes(fxBundle(), error);
    CHECK(error.empty());
    CHECK(classes.size() == 2);
    if (classes.size() == 2) {
        CHECK(classes[0].name == "Tiny Test Fx");
        CHECK(classes[0].subcategories == "Fx|Dynamics");
        CHECK(classes[0].vendor == "Tiny DAW Tests");  // (the class says none: the factory's)
        CHECK(classes[0].version == "1.0.0");
        CHECK(!classes[0].instrument);
        CHECK(classes[1].name == "Tiny Test Single");
    }
    std::string bin = vst3Binary(fxBundle());
    CHECK(moduleCount(bin, 0) == 1);  // ModuleEntry, once
    CHECK(moduleCount(bin, 1) == 0);  // (kept loaded)
    CHECK(moduleCount(bin, 3) == 1);  // the factory got the host context
    error.clear();
    CHECK(vst3Classes(testDir + "/Missing.vst3", error).empty());
    CHECK(!error.empty());
}

void testVst3Fx() {
    std::string error;
    auto p = Plugin::load(fxBundle(), "", error);
    CHECK(p != nullptr);
    if (!p) {
        fprintf(stderr, "cannot load the VST3 test plugin: %s\n", error.c_str());
        return;
    }
    CHECK(!p->isVst2());
    CHECK(p->name() == "Tiny Test Fx");
    CHECK(p->vendor() == "Tiny DAW Tests");
    CHECK(p->subcategories() == "Fx|Dynamics");
    CHECK(!p->isInstrument());
    CHECK(p->hasSidechain());
    CHECK(p->latency() == 64);
    CHECK(p->sampleRate() == 48000);
    p->prepare(44100, 512);
    CHECK(p->sampleRate() == 44100);
    CHECK(p->latency() == 64);

    // an impulse comes out 64 frames late, at gain x1
    const int N = 512;
    Stereo in(N), out(N);
    in.l[0] = in.r[0] = 1.0f;
    p->setTransport(120, 4, 4, true, 0);
    p->process(in.in(), nullptr, out.out(), N);
    CHECK_NEAR(out.l[64], 1.0, 1e-6);
    CHECK_NEAR(out.r[64], 1.0, 1e-6);
    CHECK_NEAR(out.l[0], 0.0, 1e-9);
    CHECK_NEAR(out.l[63], 0.0, 1e-9);

    // gain 0.25 (x0.5) on a steady 0.5
    p->setParam(100, 0.25);
    CHECK_NEAR(p->getParam(100), 0.25, 1e-9);  // (before it reached the window)
    Stereo steady(N, 0.5f);
    p->process(steady.in(), nullptr, out.out(), N);
    p->process(steady.in(), nullptr, out.out(), N);
    CHECK_NEAR(out.l[300], 0.25, 1e-6);
    // a loud sidechain ducks it (x0.25)
    Stereo side(N, 0.5f);
    p->process(steady.in(), side.in(), out.out(), N);
    CHECK_NEAR(out.l[300], 0.0625, 1e-6);
    CHECK_NEAR(out.r[300], 0.0625, 1e-6);
    // a silent one doesn't
    Stereo quiet(N, 0.0f);
    p->process(steady.in(), quiet.in(), out.out(), N);
    CHECK_NEAR(out.l[300], 0.25, 1e-6);
    // Duck off: no ducking
    p->setParam(101, 0.0);
    p->process(steady.in(), side.in(), out.out(), N);
    CHECK_NEAR(out.l[300], 0.25, 1e-6);
    // set again and again before a buffer: the last value plays, and is the one asked
    p->setParam(100, 1.0);
    p->setParam(101, 1.0);
    p->setParam(100, 0.0);
    p->setParam(101, 0.0);
    p->setParam(100, 0.25);
    CHECK_NEAR(p->getParam(100), 0.25, 1e-9);
    p->process(steady.in(), side.in(), out.out(), N);
    p->process(steady.in(), side.in(), out.out(), N);
    CHECK_NEAR(out.l[300], 0.25, 1e-6);
    // in place
    Stereo same(N, 0.5f);
    p->process(same.in(), nullptr, same.out(), N);
    CHECK_NEAR(same.l[300], 0.25, 1e-6);

    // parameters
    auto params = p->parameters();
    CHECK(params.size() == 4);
    if (params.size() == 4) {
        CHECK(params[0].id == 100 && params[0].name == "Gain" && params[0].units == "x");
        CHECK(params[0].stepCount == 0 && params[0].automatable && !params[0].readOnly);
        CHECK_NEAR(params[0].defaultValue, 0.5, 1e-9);
        CHECK(params[1].id == 101 && params[1].name == "Duck" && params[1].stepCount == 1);
        CHECK_NEAR(params[1].defaultValue, 1.0, 1e-9);
        CHECK(params[2].id == 102 && params[2].readOnly && !params[2].automatable);
        CHECK(!params[0].hidden && !params[0].isBypass && !params[0].isProgramChange);
    }
    CHECK(p->paramText(100, 0.25) == "0.50 x");
    CHECK(p->paramText(101, 1.0) == "On");
    CHECK(p->paramText(101, 0.0) == "Off");
    CHECK(p->paramText(999, 0.0) == "");
    p->idle();  // (the values reach the window)
    CHECK_NEAR(p->getParam(100), 0.25, 1e-9);
    CHECK_NEAR(p->getParam(101), 0.0, 1e-9);

    // the processor's messages (from this thread, as from the audio thread) reach the controller
    // directly; its output parameter changes reach it with idle()
    Stereo loud(N, 0.8f);
    p->process(loud.in(), nullptr, out.out(), N);
    p->process(loud.in(), nullptr, out.out(), N);
    CHECK_NEAR(p->getParam(102), 0.8, 1e-6);  // Meter: by IMessage
    p->idle();
    CHECK_NEAR(p->getParam(103), 0.8, 1e-6);  // Peak: by outputParameterChanges

    // edits in the window
    CHECK(p->edited().empty());
    p->windowEdit(101, 1.0);
    auto edits = p->edited();
    CHECK(edits.size() == 1 && edits[0] == 101);
    CHECK(p->edited().empty());
    CHECK_NEAR(p->getParam(101), 1.0, 1e-9);
    p->process(steady.in(), side.in(), out.out(), N);  // (Duck is on again for the processor)
    p->process(steady.in(), side.in(), out.out(), N);
    CHECK_NEAR(out.l[300], 0.0625, 1e-6);

    // state
    std::string comp = p->componentState(), ctrl = p->controllerState();
    CHECK(comp.size() == 20 && comp.compare(0, 4, "TFX1") == 0);
    CHECK(ctrl.size() == 12 && ctrl.compare(0, 4, "TFXC") == 0);
    std::string blob = stateOf(*p);
    CHECK(blob.compare(0, 4, "TDV3") == 0 && blob.size() == 8 + 20 + 12);
    std::string c2, k2;
    CHECK(unpackState(blob, c2, k2) && c2 == comp && k2 == ctrl);
    auto q = Plugin::load(fxBundle(), "Tiny Test Fx", error);
    CHECK(q != nullptr);
    if (q) {
        CHECK_NEAR(q->getParam(100), 0.5, 1e-9);
        setState(*q, blob);
        CHECK_NEAR(q->getParam(100), 0.25, 1e-9);  // (the controller follows the processor's state)
        CHECK(q->componentState() == comp);
        CHECK(q->controllerState() == ctrl);
        q->prepare(44100, 512);
        q->process(steady.in(), nullptr, out.out(), N);
        q->process(steady.in(), nullptr, out.out(), N);
        CHECK_NEAR(out.l[300], 0.25, 1e-6);
        bool threw = false;
        try {
            setState(*q, "nonsense");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // reset: the delay line starts afresh
    p->process(loud.in(), nullptr, out.out(), N);
    p->reset();
    Stereo silence(N);
    p->process(silence.in(), nullptr, out.out(), N);
    CHECK_NEAR(rms(out.l), 0.0, 1e-9);

    // longer buffers than prepared are cut up; the window
    Stereo big(1500, 0.5f), bigOut(1500);
    p->process(big.in(), nullptr, bigOut.out(), 1500);
    CHECK_NEAR(bigOut.l[1400], 0.25, 1e-6);
    CHECK(p->hasEditor());
    CHECK(!p->editorOpen());
    CHECK(!p->editorResizable());
    int w = 0, h = 0;
    CHECK(!p->requestedSize(w, h));
    CHECK(!p->holding());
    q.reset();
    p.reset();
    CHECK(moduleCount(vst3Binary(fxBundle()), 2) == 0);  // every object it made is gone
}

void testVst3Single() {
    std::string error;
    auto p = Plugin::load(vst3Binary(fxBundle()), "Tiny Test Single", error);  // (by its binary, as projects name it)
    CHECK(p != nullptr);
    if (!p) {
        fprintf(stderr, "%s\n", error.c_str());
        return;
    }
    int terminatedBefore = moduleCount(vst3Binary(fxBundle()), 4);
    CHECK(p->name() == "Tiny Test Single");
    CHECK(!p->isInstrument());
    CHECK(!p->hasSidechain());
    CHECK(p->latency() == 0);
    auto params = p->parameters();
    CHECK(params.size() == 1 && params[0].name == "Level");
    p->setParam(1, 0.5);
    const int N = 256;
    Stereo in(N, 0.8f), out(N);
    p->process(in.in(), nullptr, out.out(), N);
    CHECK_NEAR(out.l[10], 0.4, 1e-6);
    p->idle();
    CHECK_NEAR(p->getParam(1), 0.5, 1e-9);
    CHECK(p->paramText(1, 0.5) == "50 %");
    CHECK(p->controllerState().empty());  // (one object: its state is the component's)
    std::string comp = p->componentState();
    CHECK(comp.size() == 12 && comp.compare(0, 4, "TSG1") == 0);
    CHECK(!p->hasEditor());
    int w, h;
    CHECK(!p->openEditor(0, nullptr, 1.0, w, h));
    p.reset();
    CHECK(moduleCount(vst3Binary(fxBundle()), 4) == terminatedBefore + 1);  // (terminated once)
    auto missing = Plugin::load(fxBundle(), "No Such Plugin", error);
    CHECK(missing == nullptr && error.find("No Such Plugin is not in") == 0);
}

void testVst2() {
    std::string error;
    auto classes = vst2Classes(synthFile(), error);
    CHECK(classes.size() == 1);
    if (classes.size() == 1) {
        CHECK(classes[0].name == "Tiny Test Synth");
        CHECK(classes[0].vendor == "Tiny DAW Tests");
        CHECK(classes[0].instrument);
        CHECK(classes[0].subcategories == "Instrument");
        CHECK(classes[0].version == "100");
    }
    auto p = Plugin::load(synthFile(), "", error);
    CHECK(p != nullptr);
    if (!p) {
        fprintf(stderr, "cannot load the VST2 test plugin: %s\n", error.c_str());
        return;
    }
    CHECK(p->isVst2());
    CHECK(p->isInstrument());
    CHECK(!p->hasSidechain());
    CHECK(p->latency() == 16);
    CHECK(p->name() == "Tiny Test Synth");
    CHECK(p->hasEditor());
    const double rate = 48000;
    p->prepare(rate, 480);
    auto params = p->parameters();
    CHECK(params.size() == 3);
    if (params.size() == 3) {
        CHECK(params[0].name == "Volume" && params[0].units == "%" && params[0].stepCount == 0);
        CHECK(params[1].name == "Octave");
        CHECK_NEAR(params[0].defaultValue, 0.5, 1e-6);
    }
    CHECK(p->paramText(0, p->getParam(0)) == "50.0 %");
    CHECK(p->paramText(0, 0.3) == "0.30");

    // A4 for 4800 frames (ten buffers): a 440 Hz sine at 0.5
    std::vector<float> sound;
    Stereo out(480);
    p->setTransport(150, 3, 4, true, 0);
    for (int b = 0; b < 10; b++) {
        std::vector<NoteMessage> notes;
        if (b == 0)
            notes.push_back({0, true, 69, 100, 0});
        p->render(notes, out.out(), 480);
        sound.insert(sound.end(), out.l.begin(), out.l.end());
    }
    CHECK_NEAR(frequency(sound, rate), 440.0, 1.0);
    CHECK_NEAR(rms(sound), 0.5 / std::sqrt(2.0), 0.01);
    CHECK(out.l == out.r);
    CHECK(p->holding());
    CHECK_NEAR(p->getParam(2), 0.15, 1e-6);  // (it saw the tempo through audioMasterGetTime)
    p->notesOff();
    p->render({}, out.out(), 480);
    CHECK_NEAR(rms(out.l), 0.0, 1e-9);
    CHECK(!p->holding());

    // a note held across prepare() (a sample-rate change) or reset(): its note-off comes with the next
    // buffer (this synth, like FabFilter One, keeps its voices across a suspend: without it the note
    // would sound on and on, and a plugin that ignores a held key's note-on would stay silent)
    for (int how = 0; how < 2; how++) {
        p->render({{0, true, 69, 100, 0}}, out.out(), 480);
        CHECK(rms(out.l) > 0.2);
        CHECK(p->holding());
        if (how == 0)
            p->prepare(rate, 480);
        else
            p->reset();
        p->render({}, out.out(), 480);
        CHECK_NEAR(rms(out.l), 0.0, 1e-9);
        CHECK(!p->holding());
    }

    // a note in the middle of a buffer; an octave up
    p->setParam(1, 0.75);
    p->render({{240, true, 57, 90, 1}}, out.out(), 480);
    CHECK_NEAR(rms(std::vector<float>(out.l.begin(), out.l.begin() + 240)), 0.0, 1e-9);
    CHECK(rms(out.l, 240) > 0.2);
    sound.clear();
    for (int b = 0; b < 10; b++) {
        p->render({}, out.out(), 480);
        sound.insert(sound.end(), out.l.begin(), out.l.end());
    }
    CHECK_NEAR(frequency(sound, rate), 440.0, 1.0);  // (A3, an octave up)
    p->render({{100, false, 57, 0, 1}}, out.out(), 480);
    CHECK_NEAR(rms(out.l, 101), 0.0, 1e-9);
    CHECK(!p->holding());

    // edits in its window, and states
    p->windowEdit(0, 0.8);
    auto edits = p->edited();
    CHECK(edits.size() == 1 && edits[0] == 0);
    CHECK_NEAR(p->getParam(0), 0.8, 1e-6);
    std::string state = p->componentState();
    CHECK(state.compare(0, 8, "VST2CHNK") == 0 && state.size() == 8 + 12);
    CHECK(p->controllerState().empty());
    p->setParam(0, 0.2);
    CHECK_NEAR(p->getParam(0), 0.2, 1e-6);
    p->setState(state, "");
    CHECK_NEAR(p->getParam(0), 0.8, 1e-6);
    CHECK_NEAR(p->getParam(1), 0.75, 1e-6);
    std::string blob = stateOf(*p);
    auto q = Plugin::load(synthFile(), "", error);
    CHECK(q != nullptr);
    if (q) {
        setState(*q, blob);
        CHECK_NEAR(q->getParam(0), 0.8, 1e-6);
    }
    p->reset();
    CHECK(!p->holding());
    auto none = Plugin::load(testDir + "/nothing.so", "", error);
    CHECK(none == nullptr && !error.empty());
}

// JUCE's base64 (the inverse of juceDecodeBase64), to make an old state
std::string juceEncode(const std::string& bytes) {
    static const char* alphabet = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    std::string out = std::to_string(bytes.size()) + ".";
    size_t bits = bytes.size() * 8;
    for (size_t bit = 0; bit < bits; bit += 6) {
        int value = 0;
        for (int i = 0; i < 6 && bit + i < bits; i++)
            if ((unsigned char)bytes[(bit + i) >> 3] >> ((bit + i) & 7) & 1)
                value |= 1 << i;
        out += alphabet[value];
    }
    return out;
}

void testStates() {
    std::string comp("abc\0def", 7), ctrl = "XYZ";
    std::string blob = packState(comp, ctrl);
    CHECK(blob.size() == 8 + 7 + 3);
    CHECK(blob[4] == 7 && blob[5] == 0);
    std::string a, b;
    CHECK(unpackState(blob, a, b) && a == comp && b == ctrl);
    CHECK(unpackState(packState("", ""), a, b) && a.empty() && b.empty());
    CHECK(!unpackState("TDV", a, b));
    CHECK(!unpackState("random bytes", a, b));
    // an old (JUCE) state
    std::string processor;
    for (int i = 0; i < 100; i++)
        processor += (char)(i * 37);
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VST3PluginState><IComponent>" + juceEncode(processor) +
                      "</IComponent><IEditController>" + juceEncode("ui") + "</IEditController></VST3PluginState>";
    std::string juce = "VC2!";
    uint32_t n = (uint32_t)xml.size() + 1;
    for (int i = 0; i < 4; i++)
        juce += (char)((n >> (8 * i)) & 0xFF);
    juce += xml;
    juce += '\0';
    CHECK(isJuceState(juce));
    CHECK(!isJuceState(blob));
    CHECK(juceDecodeBase64(juceEncode("hello")) == "hello");
    CHECK(juceSplit(juce, a, b) && a == processor && b == "ui");
    CHECK(unpackState(juce, a, b) && a == processor && b == "ui");
    std::string empty = "VC2!" + std::string("\x20\0\0\0", 4) + "<IComponent></IComponent>       ";
    CHECK(juceSplit(empty, a, b) && a.empty() && b.empty());
}

// --- the library and the kind finder ---

std::string tempRoot;

struct Folders {
    std::string vst3, vst2, cache;
};

Folders makeFolders(const std::string& name) {
    Folders f;
    std::string root = tempRoot + "/" + name;
    run("rm -rf '" + root + "'");
    f.vst3 = root + "/vst3";
    f.vst2 = root + "/vst2";
    f.cache = root + "/config/plugin_cache.json";
    util::makeDirs(f.vst3 + "/Sub");
    util::makeDirs(f.vst2 + "/More");
    // a bundle with its moduleinfo.json, one without (its kinds are found by the probe)
    run("cp -r '" + fxBundle() + "' '" + f.vst3 + "/Sub/'");
    run("mkdir -p '" + f.vst3 + "/Other.vst3/Contents/x86_64-linux' && cp '" + vst3Binary(fxBundle()) + "' '" +
        f.vst3 + "/Other.vst3/Contents/x86_64-linux/Other.so'");
    run("mkdir -p '" + f.vst3 + "/.hidden.vst3'");  // (left out, as glob does)
    run("cp '" + synthFile() + "' '" + f.vst2 + "/More/TinyTestSynth.so'");
    run("cp '" + vst3Binary(fxBundle()) + "' '" + f.vst2 + "/NotVst2.so'");  // (a shared library, but not a VST2 plugin)
    return f;
}

const PluginInfo* find(const PluginLibrary& lib, const std::string& name) {
    for (const auto& p : lib.plugins)
        if (p.name == name)
            return &p;
    return nullptr;
}

void testLibrary() {
    Folders f = makeFolders("scan");
    PluginLibrary lib({f.vst3}, f.cache, {f.vst2});
    std::string other = f.vst3 + "/Other.vst3/Contents/x86_64-linux/Other.so";
    std::string inner = f.vst3 + "/Sub/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so";
    std::string synth = f.vst2 + "/More/TinyTestSynth.so";
    CHECK(lib.plugins.size() == 4);
    std::vector<std::string> names;
    for (const auto& p : lib.plugins)
        names.push_back(p.name);
    CHECK((names == std::vector<std::string>{"Other", "Tiny Test Fx", "Tiny Test Single", "TinyTestSynth (VST2)"}));
    if (auto* p = find(lib, "Tiny Test Fx")) {
        CHECK(p->path == inner && p->kind == EFFECT && p->pluginName == "Tiny Test Fx");
        CHECK(p->label() == "Tiny Test Fx");
    }
    if (auto* p = find(lib, "Other")) {
        CHECK(p->path == other && p->kind.empty() && p->pluginName.empty());
        CHECK(p->label() == "Other  (type not known yet)");
    }
    if (auto* p = find(lib, "TinyTestSynth (VST2)"))
        CHECK(p->path == synth && p->kind.empty());
    auto unknown = lib.unknownFiles();
    CHECK((unknown == std::vector<std::string>{other, synth}));
    CHECK(lib.ofKind(INSTRUMENT).size() == 2);  // (the unknown ones too)
    CHECK(lib.ofKind(EFFECT).size() == 4);
    // the cache remembers the VST2 folder's files, plugins or not
    std::string text;
    CHECK(util::readFile(f.cache, text));
    const Json cache = Json::parse(text);
    CHECK(cache[synth]["plugins"].size() == 1);
    CHECK(cache[synth]["plugins"][(size_t)0].str("name") == "TinyTestSynth (VST2)");
    CHECK(cache[synth]["plugins"][(size_t)0]["kind"].isNull());
    CHECK(cache[f.vst2 + "/NotVst2.so"]["plugins"].size() == 0);
    CHECK_NEAR(cache[synth].num("mtime", 0), util::fileMtime(synth), 0);

    // the kind finder, its probe forked from this test (as `tinydaw --probe <list>` would run)
    int progressCalls = 0, finishedWith = -1;
    KindFinder finder(lib, [](const std::string& list, int outFd) {
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(outFd, 1);
            runProbe(list);
            _exit(3);
        }
        return (int)pid;
    });
    CHECK(finder.total() == 2);
    finder.progress = [&](int done, int total) {
        progressCalls++;
        CHECK(done <= total && total == 2);
    };
    finder.finished = [&](int found) { finishedWith = found; };
    finder.start(false);
    for (int i = 0; i < 300 && finder.poll(100); i++) {
    }
    CHECK(!finder.working());
    CHECK(finishedWith == 2);
    CHECK(progressCalls == 2);
    // the file with two plugins is listed once per plugin; the VST2 one keeps its name
    names.clear();
    for (const auto& p : lib.plugins)
        names.push_back(p.name + "|" + p.kind);
    CHECK((names == std::vector<std::string>{"Tiny Test Fx|effect", "Tiny Test Fx|effect", "Tiny Test Single|effect",
                                             "Tiny Test Single|effect", "TinyTestSynth (VST2)|instrument"}));
    CHECK(lib.unknownFiles().empty());
    CHECK(lib.ofKind(INSTRUMENT).size() == 1);

    // a new library reads it all from the cache
    PluginLibrary again({f.vst3}, f.cache, {f.vst2});
    auto byPath = [](std::vector<PluginInfo> l) {  // (equal names keep the order they were found in)
        std::sort(l.begin(), l.end(), [](const PluginInfo& a, const PluginInfo& b) {
            return a.path + "|" + a.name < b.path + "|" + b.name;
        });
        return l;
    };
    CHECK(byPath(again.plugins) == byPath(lib.plugins));
    CHECK(again.unknownFiles().empty());

    // loading: a file of several plugins needs a choice; the kind and the time are remembered
    PluginInfo several{inner, "TinyTestFx", "", ""};
    bool choice = false;
    try {
        again.load(several);
    } catch (const PluginChoiceNeeded& e) {
        choice = true;
        CHECK((e.names == std::vector<std::string>{"Tiny Test Fx", "Tiny Test Single"}));
        CHECK(std::string(e.what()) == "Tiny Test Fx, Tiny Test Single");
    }
    CHECK(choice);
    PluginInfo fx{inner, "Tiny Test Fx", EFFECT, "Tiny Test Fx"};
    CHECK(again.lastLoadSeconds(fx) < 0);
    auto plugin = again.load(fx, 44100);
    CHECK(plugin && plugin->sampleRate() == 44100);
    CHECK(again.lastLoadSeconds(fx) >= 0);
    PluginInfo synthInfo = *find(again, "TinyTestSynth (VST2)");
    auto s = again.load(synthInfo);
    CHECK(s && s->isInstrument());
    bool failed = false;
    try {
        again.load(PluginInfo{f.vst2 + "/NotVst2.so", "NotVst2", "", ""});
    } catch (const PluginChoiceNeeded&) {
    } catch (const std::runtime_error&) {
        failed = true;
    }
    CHECK(failed);
    PluginLibrary third({f.vst3}, f.cache, {f.vst2});
    CHECK(third.lastLoadSeconds(fx) >= 0);
    CHECK(PluginLibrary::loadKey(fx) == inner + "|Tiny Test Fx");
}

void testFinderFailures() {
    Folders f = makeFolders("fail");
    std::string other = f.vst3 + "/Other.vst3/Contents/x86_64-linux/Other.so";
    std::string synth = f.vst2 + "/More/TinyTestSynth.so";
    // the first probe process "crashes" while looking at the first file, the next one works
    {
        PluginLibrary lib({f.vst3}, f.cache, {f.vst2});
        int calls = 0;
        KindFinder finder(lib, [&](const std::string& list, int outFd) {
            fflush(stdout);
            int call = calls++;
            pid_t pid = fork();
            if (pid == 0) {
                dup2(outFd, 1);
                if (call == 0) {
                    std::string line = "plugin chatter\n" + std::string(PROBE_MARK) + "{\"start\": \"" + other + "\"}\n";
                    if (write(1, line.data(), line.size()) < 0)
                        _exit(5);
                    _exit(9);  // (as if it crashed)
                }
                runProbe(list);
                _exit(3);
            }
            return (int)pid;
        });
        int found = -1;
        finder.finished = [&](int n) { found = n; };
        finder.start(false);
        for (int i = 0; i < 300 && finder.poll(100); i++) {
        }
        CHECK(calls == 2);
        CHECK(found == 1);
        CHECK(lib.probeFailedBefore(other));
        CHECK(!lib.probeFailedBefore(synth));
        CHECK(lib.cache[other].str("probe_failed") == "crashed while being looked at");
        CHECK(find(lib, "Other") && find(lib, "Other")->kind.empty());
        CHECK((lib.unknownFiles() == std::vector<std::string>{}));  // (not tried again until it changes)
    }
    // a plugin that hangs is given up on
    run("rm -f '" + f.cache + "'");
    {
        PluginLibrary lib({f.vst3}, f.cache, {f.vst2});
        KindFinder finder(lib, [&](const std::string& list, int outFd) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                dup2(outFd, 1);
                std::string text;
                util::readFile(list, text);
                const Json all = Json::parse(text);
                Json first = all[(size_t)0];
                std::string line = "\n" + std::string(PROBE_MARK) + Json::object({{"start", first}}).dump() + "\n";
                if (write(1, line.data(), line.size()) < 0)
                    _exit(5);
                sleep(20);  // (hangs)
                _exit(0);
            }
            return (int)pid;
        });
        finder.timeoutSeconds = 1;
        int found = -1;
        finder.finished = [&](int n) { found = n; };
        finder.start(false);
        for (int i = 0; i < 100 && finder.poll(100); i++) {
        }
        CHECK(!finder.working());
        CHECK(found == 0);
        CHECK(lib.cache[other].str("probe_failed") == "took over 1 s");
        CHECK(lib.cache[synth].str("probe_failed") == "took over 1 s");  // (the next process hung on it too)
        CHECK(lib.unknownFiles().empty());
    }
    // nothing to do: finished at once
    {
        PluginLibrary lib({}, f.cache, {});
        KindFinder finder(lib);
        int found = -1;
        finder.finished = [&](int n) { found = n; };
        finder.start(false);
        CHECK(found == 0 && !finder.working());
    }
}

// the parameter changes on their way (plugins/param_changes.h): the last value of each wins, in the
// order first changed; emptied in place, keeping their room (the audio thread never allocates); and
// thousands of parameters at once (Serum 2: 2,622) found by their ids, not searched one by one
void testParamChanges() {
    IdIndex index;
    CHECK(index.find(7) == IdIndex::NONE);  // (empty, no room yet)
    index.reserve(100);
    size_t room = index.room();
    CHECK(room >= 100);
    for (uint32_t i = 0; i < 100; i++)
        index.insert(i * 4096u, (int32_t)i);  // (ids alike in their low bits)
    bool all = true;
    for (uint32_t i = 0; i < 100; i++)
        all = all && index.find(i * 4096u) == (int32_t)i;
    CHECK(all);
    CHECK(index.find(1) == IdIndex::NONE);
    index.clear();
    CHECK(index.size() == 0);
    CHECK(index.find(0) == IdIndex::NONE);
    CHECK(index.find(4096) == IdIndex::NONE);
    CHECK(index.room() == room);  // (emptied in place)
    for (uint32_t i = 0; i < 300; i++)  // (more than its room: it grows, keeping what it has)
        index.insert(0x9E000000u + i * 7u, (int32_t)i);
    all = true;
    for (uint32_t i = 0; i < 300; i++)
        all = all && index.find(0x9E000000u + i * 7u) == (int32_t)i;
    CHECK(all);
    CHECK(index.room() >= 300);

    LatestChanges latest;
    latest.set(5, 0.1);
    latest.set(3, 0.2);
    latest.set(5, 0.3);  // (the last of each, where it was first changed)
    latest.set(9, 0.4);
    CHECK(latest.size() == 3);
    CHECK(latest.items()[0].id == 5 && latest.items()[0].value == 0.3);
    CHECK(latest.items()[1].id == 3 && latest.items()[1].value == 0.2);
    CHECK(latest.items()[2].id == 9 && latest.items()[2].value == 0.4);
    double v = 0;
    CHECK(latest.find(3, v) && v == 0.2);
    CHECK(!latest.find(4, v));
    latest.dropFirst(1);  // (the first sent: the others kept, in order, still found)
    CHECK(latest.size() == 2 && latest.items()[0].id == 3 && latest.items()[1].id == 9);
    CHECK(!latest.find(5, v));
    CHECK(latest.find(9, v) && v == 0.4);
    latest.set(5, 0.5);
    CHECK(latest.size() == 3 && latest.items()[2].id == 5);
    latest.dropFirst(10);
    CHECK(latest.empty() && !latest.find(9, v));

    // a queue between threads: every parameter of a big plugin, changed each frame
    const uint32_t COUNT = 2622;
    ChangeQueue queue;
    queue.reserve(COUNT + 8);
    size_t queueRoom = queue.room();
    CHECK(queueRoom >= COUNT);
    std::vector<ParamValueChange> taken;
    taken.reserve(queueRoom);
    const ParamValueChange* takenAt = taken.data();
    bool ordered = true, latestWon = true, found = true;
    for (int frame = 0; frame < 3; frame++) {
        for (int pass = 0; pass < 2; pass++)  // (twice: the second values win)
            for (uint32_t i = 0; i < COUNT; i++)
                queue.add(0x40000000u + i * 2654435761u, frame + pass + i / (double)COUNT);
        for (uint32_t i = 0; i < COUNT; i += 97)
            found = found && queue.find(0x40000000u + i * 2654435761u, v) && v == frame + 1 + i / (double)COUNT;
        CHECK(queue.take(taken, false));
        CHECK(taken.size() == COUNT);
        for (uint32_t i = 0; i < COUNT && i < taken.size(); i++) {
            ordered = ordered && taken[i].id == 0x40000000u + i * 2654435761u;
            latestWon = latestWon && taken[i].value == frame + 1 + i / (double)COUNT;
        }
        CHECK(!queue.find(0x40000000u, v));  // (taken: empty)
        CHECK(queue.take(taken, true) && taken.empty());
    }
    CHECK(ordered);
    CHECK(latestWon);
    CHECK(found);
    CHECK(queue.room() == queueRoom);  // (it kept its room: nothing allocated frame after frame)
    CHECK(taken.data() == takenAt);    // (nor what it was taken into)
}

}  // namespace

int main() {
    testDir = util::absolutePath(check::testDir());
    if (!util::fileExists(synthFile()) || !util::isDir(fxBundle())) {
        fprintf(stderr, "the test plugins are missing in %s (make plugins)\n", testDir.c_str());
        CHECK(false);
        TEST_MAIN_END();
    }
    char tmpl[] = "/tmp/tinydaw-plugin-test-XXXXXX";
    tempRoot = mkdtemp(tmpl);
    testParamChanges();
    testPaths();
    testVst3Classes();
    testVst3Fx();
    testVst3Single();
    testVst2();
    testStates();
    testLibrary();
    testFinderFailures();
    run("rm -rf '" + tempRoot + "'");
    TEST_MAIN_END();
}
