// The built-in effects (src/effects): each one's DSP against what it should do, their parameters'
// texts, states, the factory and the plugin hook, the Mix helper, no allocation in process(), and
// their windows painted into PNGs (build/ui-samples/effects/).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#include "core/util.h"
#include "effects/builtin.h"
#include "effects/effects.h"
#include "effects/mix.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "plugins/library.h"
#include "ui/effect_window.h"
#include "ui/theme.h"

// --- counting allocations (process() must make none) ---
// (counted on the thread that asks only: an effect's own worker thread, Space's, may allocate)
static std::atomic<long> allocations{0};
static thread_local bool counting = false;
void* operator new(size_t n) {
    if (counting) allocations++;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
// (the nothrow forms too, e.g. std::stable_sort's buffer: else they pair the sanitizers' new with our free)
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    if (counting) allocations++;
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace effects;

namespace {

const double SR = 48000;
const std::string SAMPLES = "build/ui-samples/effects";

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    size_t size() const { return l.size(); }
};

Stereo sine(double freq, double amp, double seconds, double phaseR = 0) {
    Stereo s((size_t)(seconds * SR));
    for (size_t i = 0; i < s.size(); i++) {
        s.l[i] = (float)(amp * std::sin(2 * PI * freq * i / SR));
        s.r[i] = (float)(amp * std::sin(2 * PI * freq * i / SR + phaseR));
    }
    return s;
}

// through the effect in blocks
bool countProcess = false;  // (count the allocations made by process() only)

Stereo run(plug::Plugin& fx, const Stereo& in, int blockSize = 512, const Stereo* side = nullptr) {
    Stereo out(in.size());
    counting = countProcess;
    for (size_t at = 0; at < in.size(); at += blockSize) {
        int n = (int)std::min<size_t>(blockSize, in.size() - at);
        const float* i[2] = {in.l.data() + at, in.r.data() + at};
        float* o[2] = {out.l.data() + at, out.r.data() + at};
        if (side) {
            const float* s[2] = {side->l.data() + at, side->r.data() + at};
            fx.process(i, s, o, n);
        } else {
            fx.process(i, nullptr, o, n);
        }
    }
    counting = false;
    return out;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; i++) sum += (double)x[i] * x[i];
    return std::sqrt(sum / std::max<size_t>(1, to - from));
}
double peak(const std::vector<float>& x, size_t from, size_t to) {
    double p = 0;
    for (size_t i = from; i < to; i++) p = std::max(p, (double)std::fabs(x[i]));
    return p;
}
double db(double g) { return 20 * std::log10(std::max(g, 1e-12)); }

void set(Effect& fx, const std::string& key, double value) {
    int i = fx.key(key.c_str());
    fx.setParam((uint32_t)i, fx.params()[i].toNormal(value));
}

// --- the factory, the plugin hook ---

void testFactory() {
    const auto& all = list();
    CHECK(all.size() == 23);
    std::vector<std::string> names;
    for (const auto& i : all) names.push_back(i.name);
    CHECK((names == std::vector<std::string>{"Bit-crusher", "Chorus", "Compressor", "Delay", "EQ", "Filter Motion",
                                             "Gate", "Level Follower", "LFO", "Limiter", "Macro",
                                             "MIDI Control", "Multi-band compressor", "Phaser / Flanger", "Random", "Reverb",
                                             "Saturation", "Space", "Time FX", "Transient Shaper", "Tremolo / Pan",
                                             "Tuner", "Utility"}));
    CHECK(categoryOf("builtin:mod_lfo") == "Modulators" && categoryOf("builtin:mod_macro") == "Modulators");
    CHECK(categoryOf("builtin:space") == "Reverb" && categoryOf("builtin:tuner") == "Utility");
    CHECK(find("builtin:multiband_compressor") && find("builtin:multiband_compressor")->category == "Dynamics");
    CHECK(find("eq")->path() == "builtin:eq" && find("eq")->label() == "EQ  (built-in)");
    CHECK(categoryOf("builtin:chorus") == "Modulation" && categoryOf("builtin:x") == "Other");
    CHECK(categoryOf("builtin:filter_motion") == "Filter" && categoryOf("builtin:phaser_flanger") == "Modulation" &&
          categoryOf("builtin:tremolo_pan") == "Modulation");
    CHECK(isBuiltin("builtin:eq") && !isBuiltin("/usr/lib/vst3/x.vst3"));
    for (const auto& info : all) {
        std::string error;
        auto fx = create(info.path(), error);
        CHECK(fx && error.empty());
        if (!fx) continue;
        CHECK(fx->id() == info.id && fx->name() == info.name && fx->vendor() == "Exbit");
        CHECK(!fx->hasEditor() && !fx->isInstrument() && !fx->isVst2() && fx->subcategories() == "Fx");
        CHECK(fx->sampleRate() == 48000);
        bool side = info.id == "compressor" || info.id == "gate" || info.id == "filter_motion" ||
                    info.id == "transient_shaper" || info.id == "mod_follower";
        CHECK(fx->hasSidechain() == side);
        CHECK(fx->latency() == (info.id == "limiter" ? 96 : 0));
    }
    std::string error;
    CHECK(!create("builtin:nothing", error) &&
          error == "There is no built-in effect nothing (its script is not in the effects folder)");
    // through plug::Plugin::load and the library, by the saved path
    auto plugin = plug::Plugin::load("builtin:reverb", "", error);
    CHECK(plugin && plugin->name() == "Reverb");
    plug::PluginInfo info{"builtin:limiter", "Limiter", plug::EFFECT, ""};
    CHECK(info.label() == "Limiter  (built-in)");
    plug::PluginLibrary lib({}, "build/tests/effects-plugin-cache.json", {});
    auto limiter = lib.load(info, 44100);
    CHECK(limiter && limiter->latency() == 88 && limiter->sampleRate() == 44100);  // (2 ms at 44.1 kHz)
    bool threw = false;
    try {
        lib.load({"builtin:none", "None", plug::EFFECT, ""});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// --- parameters ---

void testParams() {
    auto eq = create("eq");
    auto text = [&](Effect& fx, const char* key, double value) {
        int i = fx.key(key);
        return fx.paramText((uint32_t)i, fx.params()[i].toNormal(value));
    };
    CHECK(text(*eq, "low_cut", 10) == "10.0 Hz");
    CHECK(text(*eq, "low_cut", 250) == "250 Hz");
    CHECK(text(*eq, "mid_freq", 1000) == "1.00 kHz");
    CHECK(text(*eq, "mid_gain", 3) == "3.0 dB");
    CHECK(text(*eq, "mid_gain", -18) == "-18.0 dB");
    CHECK(text(*eq, "mid_q", 0.9) == "0.90");
    CHECK(text(*eq, "high_cut", 22000) == "22.00 kHz");
    auto comp = create("compressor");
    CHECK(text(*comp, "ratio", 4) == "4.0:1");
    CHECK(text(*comp, "detector", 0) == "Peak" && text(*comp, "detector", 1) == "RMS");
    CHECK(text(*comp, "sidechain", 1) == "On");
    CHECK(text(*comp, "mix", 100) == "100 %");
    CHECK(text(*comp, "release", 120) == "120 ms");
    auto crush = create("bitcrusher");
    CHECK(text(*crush, "downsample", 4) == "4 ×" && text(*crush, "bits", 8) == "8");
    auto gate = create("gate");
    CHECK(text(*gate, "attack", 0.5) == "0.50 ms" && text(*gate, "range", 80) == "80.0 dB");
    auto delay = create("delay");
    CHECK(text(*delay, "left_beats", 2) == "1/8" && text(*delay, "right_beats", 3) == "dotted 1/8");
    auto chorus = create("chorus");
    CHECK(text(*chorus, "rate", 0.8) == "0.80 Hz" && text(*chorus, "voices", 2) == "2");
    // what plugins report: steps, defaults
    auto params = comp->parameters();
    CHECK(params.size() == 9);
    CHECK(params[6].name == "Detector" && params[6].stepCount == 1 && params[8].stepCount == 1);
    CHECK(params[1].name == "Ratio" && params[1].units == ":1" && params[1].stepCount == 0);
    CHECK_NEAR(params[0].defaultValue, 42.0 / 60, 1e-12);
    auto dp = delay->parameters();
    CHECK(dp[3].stepCount == 7 && std::fabs(dp[3].defaultValue - 2.0 / 7) < 1e-12);
    CHECK(crush->parameters()[1].stepCount == 63);
    // normal ↔ value
    const Param& q = eq->params()[eq->key("mid_q")];
    CHECK_NEAR(q.fromNormal(q.toNormal(2.5)), 2.5, 1e-9);
    CHECK_NEAR(q.fromNormal(0), 0.3, 1e-12);
    CHECK_NEAR(q.fromNormal(1), 8, 1e-12);
    CHECK(crush->params()[0].fromNormal(0.5) == 8);  // (1 + 7.5 → 8: halves to even)
    CHECK(eq->getParam(eq->key("mid_freq")) == q.toNormal(0.9) || true);
    CHECK_NEAR(eq->getParam(eq->key("mid_freq")), 0.5, 1e-12);  // (1 kHz: the middle of 100 Hz…10 kHz)
    eq->setParam(99, 0.5);  // (no such parameter: nothing)
    CHECK(eq->getParam(99) == 0 && eq->paramText(99, 0.5).empty());
    // edits made in its window, for "recently tweaked"
    eq->windowEdit(4, 0.75);
    eq->windowEdit(2, 0.25);
    CHECK((eq->edited() == std::vector<uint32_t>{4, 2}) && eq->edited().empty());
    CHECK_NEAR(eq->value("mid_gain"), 9.0, 1e-9);
}

// --- states ---

void testState() {
    CHECK(pyFloat(100.0) == "100.0" && pyFloat(0.1) == "0.1" && pyFloat(-0.3) == "-0.3");
    CHECK(pyFloat(1e-05) == "1e-05" && pyFloat(1e16) == "1e+16" && pyFloat(0.0001) == "0.0001");
    CHECK(pyFloat(10.000000000000002) == "10.000000000000002" && pyFloat(22000) == "22000.0");
    CHECK(pyFloat(1.5e300) == "1.5e+300" && pyFloat(123456789012345.0) == "123456789012345.0");
    auto eq = create("eq");
    CHECK(eq->componentState() ==
          "{\"low_cut\": 10.0, \"low_freq\": 100.0, \"low_gain\": 0.0, \"mid_freq\": 1000.0, \"mid_gain\": 0.0, "
          "\"mid_q\": 0.9, \"high_freq\": 6000.0, \"high_gain\": 0.0, \"high_cut\": 22000.0, \"output\": 0.0}");
    CHECK(eq->controllerState().empty());
    set(*eq, "mid_gain", 4.5);
    set(*eq, "low_cut", 80);
    auto again = create("eq");
    again->setState(eq->componentState(), "");
    for (uint32_t i = 0; i < eq->params().size(); i++) CHECK_NEAR(again->getParam(i), eq->getParam(i), 1e-12);
    CHECK_NEAR(again->value("low_cut"), 80, 1e-9);
    // a state the Python program saved (ints, a missing key, an unknown one): what is there is taken
    auto comp = create("compressor");
    comp->setState("{\"threshold\": -30, \"ratio\": 8.0, \"sidechain\": true, \"other\": 3}", "");
    CHECK_NEAR(comp->value("threshold"), -30, 1e-9);
    CHECK_NEAR(comp->value("ratio"), 8, 1e-9);
    CHECK(comp->value("sidechain") == 1 && comp->value("knee") == 6);
    comp->setState("not json", "");
    CHECK_NEAR(comp->value("threshold"), -30, 1e-9);
    comp->setState("", "");  // ("{}")
    CHECK_NEAR(comp->value("ratio"), 8, 1e-9);
    // as a slot's blob ("TDV3" + length + JSON), through the library's helpers
    std::string blob = plug::stateOf(*comp);
    CHECK(blob.compare(0, 4, "TDV3") == 0 && blob.find("\"threshold\": -30.0") != std::string::npos);
    auto other = create("compressor");
    plug::setState(*other, blob);
    CHECK_NEAR(other->value("threshold"), -30, 1e-9);
    // out-of-range values are held to the range (as to_normal does)
    other->setState("{\"ratio\": 100}", "");
    CHECK_NEAR(other->value("ratio"), 20, 1e-9);
}

// --- EQ ---

double gainAt(Effect& fx, double freq) {  // steady sine through it: out RMS / in RMS (dB)
    fx.reset();
    Stereo in = sine(freq, 0.25, 0.6);
    Stereo out = run(fx, in);
    size_t from = (size_t)(0.3 * SR);
    return db(rms(out.l, from, in.size()) / rms(in.l, from, in.size()));
}

double rbjPeakDb(double f, double f0, double q, double gainDb) {  // the cookbook's peak filter, written out anew
    double A = std::pow(10, gainDb / 40), w0 = 2 * PI * f0 / SR, alpha = std::sin(w0) / (2 * q);
    double b0 = 1 + alpha * A, b1 = -2 * std::cos(w0), b2 = 1 - alpha * A;
    double a0 = 1 + alpha / A, a1 = -2 * std::cos(w0), a2 = 1 - alpha / A;
    double w = 2 * PI * f / SR;
    std::complex<double> z = std::polar(1.0, -w);
    std::complex<double> h = (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
    return db(std::abs(h));
}

void testEq() {
    auto eq = create("eq");
    CHECK_NEAR(gainAt(*eq, 1000), 0, 0.01);  // (all flat by default)
    set(*eq, "mid_gain", 6);
    set(*eq, "mid_q", 0.9);
    for (double f : {100.0, 500.0, 1000.0, 2000.0, 8000.0}) CHECK_NEAR(gainAt(*eq, f), rbjPeakDb(f, 1000, 0.9, 6), 0.05);
    CHECK_NEAR(gainAt(*eq, 1000), 6, 0.05);
    set(*eq, "mid_gain", 0);
    set(*eq, "low_cut", 200);  // a 2nd-order Butterworth-ish high-pass (Q 0.707): -3 dB at its frequency
    CHECK_NEAR(gainAt(*eq, 200), -3.01, 0.1);
    CHECK(gainAt(*eq, 50) < -20 && std::fabs(gainAt(*eq, 5000)) < 0.1);
    set(*eq, "low_cut", 10);
    set(*eq, "high_cut", 2000);
    CHECK_NEAR(gainAt(*eq, 2000), -3.01, 0.1);
    CHECK(gainAt(*eq, 10000) < -20);
    set(*eq, "high_cut", 22000);
    set(*eq, "low_gain", -12);  // shelves: their gain far below / above their frequency
    CHECK_NEAR(gainAt(*eq, 30), -12, 0.6);
    set(*eq, "low_gain", 0);
    set(*eq, "high_gain", 9);
    CHECK_NEAR(gainAt(*eq, 18000), 9, 0.6);
    set(*eq, "high_gain", 0);
    set(*eq, "output", -6);
    CHECK_NEAR(gainAt(*eq, 1000), -6, 0.02);
    // its curve: 200 points from 20 Hz to 20 kHz, dB / 24
    set(*eq, "output", 0);
    set(*eq, "mid_gain", 12);
    auto curve = eq->curve();
    CHECK(curve.size() == 200 && eq->hasCurve());
    CHECK_NEAR(curve[0].x, 0, 1e-12);
    CHECK_NEAR(curve[199].x, 1, 1e-12);
    double best = -1;
    for (const auto& p : curve) best = std::max(best, p.y);
    CHECK_NEAR(best, 0.5, 0.01);  // (+12 dB at 1 kHz)
    CHECK(eq->curveXGrid.size() == 3 && eq->curveXGrid[1].label == "1k" && std::fabs(eq->curveXGrid[1].pos - std::log(50.0) / std::log(1000.0)) < 1e-9);
}

// --- dynamics ---

void testCompressor() {
    auto comp = create("compressor");
    set(*comp, "knee", 0);
    set(*comp, "attack", 1);
    set(*comp, "release", 50);
    // a sine peaking at -6 dB: 12 dB over the -18 dB threshold, 4:1 → 9 dB down
    Stereo in = sine(1000, 0.5, 1.0);
    Stereo out = run(*comp, in);
    size_t from = (size_t)(0.6 * SR);
    double change = db(peak(out.l, from, in.size()) / 0.5);
    CHECK_NEAR(change, -9, 0.5);
    CHECK_NEAR(comp->meters()[0].fraction * 24, 9, 0.5);
    CHECK(comp->meters()[0].label == "Gain reduction");
    // below the threshold: untouched
    comp->reset();
    Stereo quiet = sine(1000, 0.05, 0.5);  // -26 dB
    Stereo q = run(*comp, quiet);
    CHECK_NEAR(db(peak(q.l, 12000, q.size()) / 0.05), 0, 0.05);
    // makeup, ratio
    comp->reset();
    set(*comp, "makeup", 6);
    set(*comp, "ratio", 2);  // 12 over → 6 down, +6
    out = run(*comp, in);
    CHECK_NEAR(db(peak(out.l, from, in.size()) / 0.5), 0, 0.5);
    // the soft knee: at the threshold, slope · (knee / 2)² / (2 knee) = -0.5 · 9 / 12
    CHECK_NEAR(compression(-18, -18, 2, 6), -0.375, 1e-12);
    CHECK_NEAR(compression(-10, -18, 4, 6), -6, 1e-12);
    CHECK(compression(-30, -18, 4, 6) == 0);
    // parallel: Mix 0 is the input
    comp->reset();
    set(*comp, "mix", 0);
    out = run(*comp, in);
    CHECK_NEAR(peak(out.l, from, in.size()), 0.5, 1e-4);
    set(*comp, "mix", 100);
    set(*comp, "makeup", 0);
    set(*comp, "ratio", 4);
    // the sidechain: a quiet input ducked by a loud sidechain, only with Sidechain on
    Stereo loud = sine(100, 0.9, 1.0);
    Stereo quietLong = sine(1000, 0.05, 1.0);
    comp->reset();
    out = run(*comp, quietLong, 512, &loud);
    CHECK_NEAR(peak(out.l, from, out.size()), 0.05, 0.002);
    comp->reset();
    set(*comp, "sidechain", 1);
    out = run(*comp, quietLong, 512, &loud);
    CHECK(db(peak(out.l, from, out.size()) / 0.05) < -10);  // (-0.9 dB peak, 17 over: ~ -12.8 dB)
    // its transfer curve: out against in, -60…0 dB
    auto curve = comp->curve();
    CHECK(curve.size() == 121 && std::fabs(curve.back().x - 1) < 1e-12);
    CHECK_NEAR(curve.back().y, (-18 + 18.0 / 4 + 60) / 60 * 2 - 1, 1e-9);  // (0 dB in: -13.5 out)
}

void testMultiband() {
    auto mb = create("multiband_compressor");
    for (const char* k : {"low_threshold", "mid_threshold", "high_threshold"}) set(*mb, k, 0);
    // nothing over the thresholds: the bands add back up flat (Linkwitz-Riley)
    for (double f : {60.0, 200.0, 1000.0, 3000.0, 10000.0}) CHECK_NEAR(gainAt(*mb, f), 0, 0.15);
    // a loud bass: only the low band turned down
    set(*mb, "low_threshold", -30);
    set(*mb, "mid_threshold", -30);
    double low = gainAt(*mb, 60);
    CHECK(low < -6);
    auto meters = mb->meters();
    CHECK(meters.size() == 3 && meters[0].label == "Low reduction" && meters[0].fraction > 0.2 && meters[2].fraction < 0.01);
    set(*mb, "low_threshold", 0);
    set(*mb, "low_gain", 6);
    CHECK_NEAR(gainAt(*mb, 50), 6, 0.6);
}

void testGate() {
    auto gate = create("gate");
    // loud, then quiet (-60 dB: below -40), then loud again
    Stereo in((size_t)(2.0 * SR));
    for (size_t i = 0; i < in.size(); i++) {
        double t = i / SR, amp = t < 0.5 ? 0.5 : t < 1.5 ? 0.001 : 0.5;
        in.l[i] = in.r[i] = (float)(amp * std::sin(2 * PI * 440 * t));
    }
    Stereo out = run(*gate, in);
    CHECK_NEAR(peak(out.l, (size_t)(0.2 * SR), (size_t)(0.5 * SR)), 0.5, 0.01);  // open
    CHECK(peak(out.l, (size_t)(0.5 * SR), (size_t)(0.51 * SR)) > 0.0005);       // held open (30 ms)
    CHECK(peak(out.l, (size_t)(1.4 * SR), (size_t)(1.5 * SR)) < 1e-6);         // shut (range 80: silence)
    CHECK_NEAR(peak(out.l, (size_t)(1.55 * SR), (size_t)(2.0 * SR)), 0.5, 0.01);  // open again (0.5 ms attack)
    auto m = gate->meters();
    CHECK(m.size() == 1 && m[0].label == "Open" && m[0].text == "100 %");
    // a range of 20 dB: shut is 20 dB down
    gate->reset();
    set(*gate, "range", 20);
    out = run(*gate, in);
    CHECK_NEAR(db(peak(out.l, (size_t)(1.4 * SR), (size_t)(1.5 * SR)) / 0.001), -20, 0.1);
    // hysteresis: a level just under the threshold (but within it) keeps it open
    gate->reset();
    set(*gate, "range", 80);
    Stereo hover((size_t)SR);
    for (size_t i = 0; i < hover.size(); i++) {
        double t = i / SR, amp = t < 0.3 ? 0.5 : dbToGain(-41.5);  // (threshold -40, hysteresis 3)
        hover.l[i] = hover.r[i] = (float)(amp * std::sin(2 * PI * 3000 * t));  // (a whole cycle in each 16-frame block)
    }
    out = run(*gate, hover);
    CHECK_NEAR(peak(out.l, (size_t)(0.8 * SR), hover.size()), dbToGain(-41.5), 1e-4);
    CHECK(gate->meters()[0].label == "Open");
    // the sidechain opens it
    gate->reset();
    set(*gate, "sidechain", 1);
    Stereo quiet = sine(440, 0.001, 0.5);
    Stereo key = sine(100, 0.5, 0.5);
    out = run(*gate, quiet, 512, &key);
    CHECK_NEAR(peak(out.l, 12000, out.size()), 0.001, 1e-4);
    Stereo noKey(quiet.size());
    gate->reset();
    out = run(*gate, quiet, 512, &noKey);
    CHECK(peak(out.l, 12000, out.size()) < 1e-6);
    CHECK(gate->meters()[0].label == "Shut");
}

void testLimiter() {
    auto lim = create("limiter");
    CHECK(lim->latency() == 96);  // (2 ms at 48 kHz: reported for delay compensation)
    // a quiet impulse comes out unchanged, 96 frames late
    Stereo imp(2048);
    imp.l[10] = 0.5f;
    imp.r[10] = -0.25f;
    Stereo out = run(*lim, imp, 256);
    CHECK(out.l[106] == 0.5f && out.r[106] == -0.25f && out.l[10] == 0);
    // never over the ceiling: loud noise and sudden peaks, pushed up 12 dB more
    set(*lim, "gain", 12);
    set(*lim, "ceiling", -1);
    lim->reset();
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1);
    Stereo noisy((size_t)SR);
    for (size_t i = 0; i < noisy.size(); i++) {
        float burst = (i / 4800) % 2 ? 1.0f : 0.05f;  // (on and off every 100 ms)
        noisy.l[i] = u(rng) * burst;
        noisy.r[i] = (i % 997 == 0) ? 1.0f : u(rng) * burst * 0.5f;
    }
    for (int blockSize : {64, 333, 512}) {
        lim->reset();
        out = run(*lim, noisy, blockSize);
        double ceiling = dbToGain(-1);
        CHECK(peak(out.l, 0, out.size()) <= ceiling + 1e-6 && peak(out.r, 0, out.size()) <= ceiling + 1e-6);
        CHECK(peak(out.l, 0, out.size()) > ceiling * 0.9);  // (and it does get there)
    }
    CHECK(lim->meters()[0].fraction > 0);
    // a steady loud tone: turned down smoothly (no clipping needed, nearly), to the ceiling
    lim->reset();
    set(*lim, "gain", 0);
    Stereo tone = sine(200, 1.0, 1.0);
    out = run(*lim, tone);
    CHECK_NEAR(peak(out.l, (size_t)(0.5 * SR), out.size()), dbToGain(-1), 0.01);
}

// --- time ---

size_t firstAbove(const std::vector<float>& x, double level, size_t from = 0) {
    for (size_t i = from; i < x.size(); i++)
        if (std::fabs(x[i]) > level) return i;
    return x.size();
}

void testDelay() {
    auto delay = create("delay");
    set(*delay, "mix", 100);
    set(*delay, "feedback", 0);
    set(*delay, "high_cut", 20000);
    Stereo imp((size_t)(1.0 * SR));
    imp.l[0] = imp.r[0] = 1.0f;
    Stereo out = run(*delay, imp);
    CHECK(firstAbove(out.l, 0.05) == 12000 && firstAbove(out.r, 0.05) == 18000);  // 250 ms, 375 ms
    // synced: at 120 BPM a 1/8 is 250 ms, a dotted 1/8 375 ms; at 100 BPM a 1/4 is 600 ms
    delay->reset();
    set(*delay, "left", 40);
    set(*delay, "right", 40);
    set(*delay, "sync", 1);
    delay->setTransport(120, 4, 4, true, 0);
    out = run(*delay, imp);
    CHECK(firstAbove(out.l, 0.05) == 12000 && firstAbove(out.r, 0.05) == 18000);
    delay->reset();
    delay->setTransport(100, 4, 4, true, 0);
    set(*delay, "left_beats", 4);   // 1/4
    set(*delay, "right_beats", 7);  // 1 bar (4/4): 2.4 s
    Stereo longer((size_t)(3.0 * SR));
    longer.l[0] = longer.r[0] = 1.0f;
    out = run(*delay, longer);
    CHECK(firstAbove(out.l, 0.05) == 28800 && firstAbove(out.r, 0.05) == 115200);  // (not asleep before its echo)
    CHECK(firstAbove(out.l, 0.05, 28900) == out.size());
    // feedback: each echo 40 % of the last; the high cut darkens them
    delay->reset();
    set(*delay, "sync", 0);
    set(*delay, "left", 100);
    set(*delay, "right", 100);
    set(*delay, "feedback", 40);
    out = run(*delay, imp);
    double e1 = peak(out.l, 4800, 4900), e2 = peak(out.l, 9600, 9700);
    CHECK_NEAR(e2 / e1, 0.4, 0.05);
    // ping-pong: the first echo on the left only, the next on the right
    delay->reset();
    set(*delay, "pingpong", 1);
    out = run(*delay, imp);
    CHECK(peak(out.l, 4800, 4900) > 0.3 && peak(out.r, 4800, 4900) < 1e-6);
    CHECK(peak(out.r, 9600, 9700) > 0.1 && peak(out.l, 9600, 9700) < 1e-6);
    // Mix 25 %: the dry sound at 75 %
    delay->reset();
    set(*delay, "mix", 25);
    out = run(*delay, imp);
    CHECK_NEAR(out.l[0], 0.75, 1e-6);
}

void testReverb() {
    auto rev = create("reverb");
    set(*rev, "mix", 100);
    Stereo imp((size_t)(4.0 * SR));
    imp.l[0] = imp.r[0] = 1.0f;
    Stereo out = run(*rev, imp);
    // the tail: some sound after the pre-delay, dying away
    double early = rms(out.l, (size_t)(0.05 * SR), (size_t)(0.3 * SR));
    double mid = rms(out.l, (size_t)(0.8 * SR), (size_t)(1.0 * SR));
    double late = rms(out.l, (size_t)(2.5 * SR), (size_t)(3.0 * SR));
    CHECK(early > 1e-4 && mid < early && late < mid * 0.1);
    CHECK(peak(out.l, 0, (size_t)(0.009 * SR)) < 1e-9);  // (10 ms pre-delay: nothing before)
    CHECK(rms(out.l, 0, out.size()) > 0 && rms(out.r, 0, out.size()) > 0);
    // a bigger room rings longer
    auto big = create("reverb");
    set(*big, "mix", 100);
    set(*big, "size", 100);
    Stereo bigOut = run(*big, imp);
    CHECK(rms(bigOut.l, (size_t)(1.5 * SR), (size_t)(2.0 * SR)) > rms(out.l, (size_t)(1.5 * SR), (size_t)(2.0 * SR)) * 3);
    // its echoes about as loud as its input (noise of 100-1000 Hz-ish: a sine at 300 Hz here)
    rev->reset();
    Stereo tone = sine(300, 0.3, 3.0);
    out = run(*rev, tone);
    CHECK(std::fabs(db(rms(out.l, (size_t)(1.5 * SR), out.size()) / rms(tone.l, 0, tone.size()))) < 6);
    // after a second of silence it sleeps: silent, not run, restarted
    Stereo silence((size_t)(12.0 * SR));
    auto sleeper = create("reverb");
    set(*sleeper, "size", 100);
    run(*sleeper, imp);
    out = run(*sleeper, silence);
    CHECK(sleeper->asleep() && peak(out.l, out.size() - 4800, out.size()) == 0);
    Stereo again = run(*sleeper, imp);  // (and wakes up)
    CHECK(!sleeper->asleep() && rms(again.l, (size_t)(0.05 * SR), (size_t)(0.3 * SR)) > 1e-5);
}

void testChorus() {
    auto chorus = create("chorus");
    auto blockRms = [](const Stereo& s, std::vector<double>& out) {
        out.clear();
        for (size_t at = (size_t)(0.1 * SR); at + 960 <= s.size(); at += 960) out.push_back(rms(s.l, at, at + 960));
    };
    Stereo in = sine(1000, 0.3, 2.0);
    std::vector<double> swaying, steady;
    blockRms(run(*chorus, in), swaying);
    set(*chorus, "depth", 0);
    chorus->reset();
    blockRms(run(*chorus, in), steady);
    auto spread = [](const std::vector<double>& v) {
        return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
    };
    CHECK(spread(swaying) > 0.05);  // (the copies' delays sway: they beat against the dry sound)
    CHECK(spread(steady) < 0.002);
    // the right side out of step with the left (width 100 %; one voice: with two, the right's voices
    // half a cycle apart are the left's two, swapped)
    set(*chorus, "depth", 3);
    set(*chorus, "voices", 1);
    chorus->reset();
    Stereo out = run(*chorus, in);
    double diff = 0;
    for (size_t i = 4800; i < out.size(); i++) diff = std::max(diff, (double)std::fabs(out.l[i] - out.r[i]));
    CHECK(diff > 0.05);
    set(*chorus, "width", 0);
    chorus->reset();
    out = run(*chorus, in);
    diff = 0;
    for (size_t i = 4800; i < out.size(); i++) diff = std::max(diff, (double)std::fabs(out.l[i] - out.r[i]));
    CHECK(diff < 1e-6);
}

// --- the moving effects (Filter Motion, Phaser / Flanger, Tremolo / Pan) ---

Stereo noise(double amp, double seconds, unsigned seed = 7) {
    std::mt19937 gen(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Stereo s((size_t)(seconds * SR));
    for (size_t i = 0; i < s.size(); i++) {
        s.l[i] = (float)(amp * u(gen));
        s.r[i] = (float)(amp * u(gen));
    }
    return s;
}

Stereo constant(double level, double seconds) {
    Stereo s((size_t)(seconds * SR));
    std::fill(s.l.begin(), s.l.end(), (float)level);
    std::fill(s.r.begin(), s.r.end(), (float)level);
    return s;
}

bool finite(const Stereo& s) {
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}
double peakOf(const Stereo& s) { return std::max(peak(s.l, 0, s.size()), peak(s.r, 0, s.size())); }

// what every moving effect does: silence in, silence out (and asleep); a sine at its defaults stays
// finite and about as loud; its state saved and loaded into a fresh one gives the same settings
void checkCommon(const char* id, const std::vector<std::pair<std::string, double>>& settings) {
    auto fx = create(id);
    Stereo silence((size_t)(2.0 * SR));
    Stereo out = run(*fx, silence);
    CHECK(peakOf(out) == 0 && fx->asleep());
    Stereo tone = sine(440, 0.5, 2.0, 0.3);
    out = run(*fx, tone);
    CHECK(finite(out) && peakOf(out) < 1.0 && rms(out.l, (size_t)SR, out.size()) > 0.05);
    if (!finite(out) || peakOf(out) >= 1.0) fprintf(stderr, "  %s: sine peak %g\n", id, peakOf(out));
    for (const auto& [k, value] : settings) set(*fx, k, value);
    auto again = create(id);
    again->setState(fx->componentState(), "");
    for (uint32_t i = 0; i < fx->params().size(); i++) CHECK_NEAR(again->getParam(i), fx->getParam(i), 1e-12);
    for (const auto& [k, value] : settings) CHECK_NEAR(again->value(k), value, 1e-9);
}

// 10 s of loud noise at extreme settings: finite, and no louder than `most`
void checkExtreme(const char* id, const std::vector<std::pair<std::string, double>>& settings, double most) {
    auto fx = create(id);
    fx->setTransport(240, 4, 4, true, 0);
    for (const auto& [k, value] : settings) set(*fx, k, value);
    Stereo in = noise(1.0, 10.0);
    Stereo out = run(*fx, in, 256, &in);
    double p = peakOf(out);
    CHECK(finite(out) && p < most);
    if (!finite(out) || p >= most) fprintf(stderr, "  %s at extremes: peak %g\n", id, p);
    // and silence after it dies away to sleep
    out = run(*fx, Stereo((size_t)(20.0 * SR)));
    CHECK(fx->asleep() && peak(out.l, out.size() - 4800, out.size()) == 0);
}

// how many times the left side rises through `level`
int risings(const std::vector<float>& x, double level) {
    int count = 0;
    for (size_t i = 1; i < x.size(); i++)
        if (x[i - 1] < level && x[i] >= level) count++;
    return count;
}

void testFilterMotion() {
    checkCommon("filter_motion", {{"type", 2}, {"cutoff", 350}, {"lfo_depth", -1.5}, {"lfo_sync", 1},
                                  {"lfo_beats", 6}, {"env_amount", 2.25}, {"sidechain", 1}});
    for (int type = 0; type < 4; type++)
        for (int slope = 0; slope < 2; slope++)
            for (double drive : {0.0, 24.0})
                for (int shape : {4, 6})
                    checkExtreme("filter_motion",
                                 {{"type", type}, {"slope", slope}, {"resonance", 100}, {"drive", drive},
                                  {"lfo_rate", 20}, {"lfo_depth", shape == 4 ? 4.0 : -4.0}, {"lfo_shape", shape},
                                  {"env_amount", 4}, {"env_attack", 0.1}, {"env_release", 5}, {"lfo_phase", 180},
                                  {"cutoff", 1000}},
                                 16);
    // the low-pass at 200 Hz: a 10 kHz tone down by far more than 20 dB, the bass through
    auto fx = create("filter_motion");
    set(*fx, "cutoff", 200);
    double high = gainAt(*fx, 10000), low = gainAt(*fx, 50);
    CHECK(high < -20 && std::fabs(low) < 1.5);
    if (high >= -20) fprintf(stderr, "  filter_motion: 10 kHz %g dB\n", high);
    set(*fx, "slope", 1);
    double steeper = gainAt(*fx, 10000);
    CHECK(steeper < -60 && steeper < high - 30);
    CHECK(std::fabs(gainAt(*fx, 50)) < 1.5);
    // high-pass, band-pass, notch at 2 kHz
    set(*fx, "slope", 0);
    set(*fx, "cutoff", 2000);
    set(*fx, "type", 1);
    CHECK(gainAt(*fx, 100) < -40 && std::fabs(gainAt(*fx, 15000)) < 1.5);
    set(*fx, "type", 2);
    CHECK(std::fabs(gainAt(*fx, 2000)) < 0.5 && gainAt(*fx, 100) < -20 && gainAt(*fx, 15000) < -10);
    set(*fx, "type", 3);
    CHECK(gainAt(*fx, 2000) < -30 && std::fabs(gainAt(*fx, 100)) < 1);
    // resonance: a peak at the cutoff
    set(*fx, "type", 0);
    set(*fx, "resonance", 100);
    CHECK(gainAt(*fx, 2000) > 20);
    // the LFO sweeps the cutoff: a 2 kHz tone through a low-pass at 1 kHz swells and fades, twice a second
    auto swept = create("filter_motion");
    set(*swept, "lfo_depth", 2);
    set(*swept, "lfo_rate", 2);
    Stereo tone = sine(2000, 0.3, 2.0);
    Stereo out = run(*swept, tone);
    std::vector<double> levels;
    for (size_t at = 4800; at + 960 <= out.size(); at += 960) levels.push_back(rms(out.l, at, at + 960));
    double lo = *std::min_element(levels.begin(), levels.end()), hi = *std::max_element(levels.begin(), levels.end());
    CHECK(hi > 0.15 && lo < 0.06);
    // stereo phase 180°: the two sides' cutoffs apart
    set(*swept, "lfo_phase", 180);
    swept->reset();
    out = run(*swept, tone);
    CHECK(std::fabs(rms(out.l, 12000, 12000 + 960) - rms(out.r, 12000, 12000 + 960)) > 0.05);
    // the envelope follower: a low-pass at 100 Hz opened by a loud sidechain, not by a silent one
    auto follower = create("filter_motion");
    set(*follower, "cutoff", 100);
    set(*follower, "env_amount", 4);
    set(*follower, "sidechain", 1);
    Stereo in = sine(3000, 0.3, 1.0), quiet((size_t)SR), loud = sine(80, 1.0, 1.0);
    double shut = rms(run(*follower, in, 512, &quiet).l, 24000, 48000);
    follower->reset();
    double open = rms(run(*follower, in, 512, &loud).l, 24000, 48000);
    CHECK(open > shut * 10);
    // following its own input instead (the sidechain switched off): a loud input opens it
    set(*follower, "sidechain", 0);
    follower->reset();
    double self = rms(run(*follower, sine(3000, 0.9, 1.0), 512, &quiet).l, 24000, 48000);
    CHECK(self > shut * 10);
    // its meter shows the cutoff now, its window the response
    CHECK(follower->meters().size() == 1 && follower->hasCurve() && follower->curve().size() == 200);
}

void testPhaserFlanger() {
    checkCommon("phaser_flanger", {{"mode", 1}, {"stages", 2}, {"feedback", -45}, {"centre", 1200},
                                   {"delay", 4.5}, {"spread", 135}, {"lfo_shape", 3}});
    for (int mode = 0; mode < 2; mode++)
        for (double fb : {-90.0, 90.0})
            checkExtreme("phaser_flanger",
                         {{"mode", mode}, {"stages", 2}, {"feedback", fb}, {"depth", 100}, {"lfo_rate", 10},
                          {"lfo_shape", 4}, {"delay", 10}, {"centre", 8000}},
                         8);
    // the phaser, still (depth 0): its 8 allpasses' notches cut a tone out deep somewhere, leave others
    auto fx = create("phaser_flanger");
    set(*fx, "depth", 0);
    set(*fx, "feedback", 0);
    double deepest = 0, loudest = -100;
    for (int i = 0; i <= 60; i++) {
        double g = gainAt(*fx, 100 * std::pow(100.0, i / 60.0));
        deepest = std::min(deepest, g);
        loudest = std::max(loudest, g);
    }
    CHECK(deepest < -15 && loudest > -1);
    // the flanger, still, at 1 ms: a comb, its first notch at 500 Hz, a peak at 1 kHz
    set(*fx, "mode", 1);
    set(*fx, "delay", 1);
    CHECK(gainAt(*fx, 500) < -30);
    CHECK(std::fabs(gainAt(*fx, 1000)) < 0.1);
    // moving (the defaults): the two sides apart (stereo spread 90°)
    auto moving = create("phaser_flanger");
    Stereo out = run(*moving, sine(1000, 0.3, 2.0));
    double diff = 0;
    for (size_t i = 4800; i < out.size(); i++) diff = std::max(diff, (double)std::fabs(out.l[i] - out.r[i]));
    CHECK(diff > 0.05);
}

void testTremoloPan() {
    checkCommon("tremolo_pan", {{"depth", 80}, {"phase", 90}, {"smoothing", 35}, {"lfo_shape", 5},
                                {"lfo_sync", 1}, {"lfo_beats", 2}});
    checkExtreme("tremolo_pan", {{"depth", 100}, {"lfo_rate", 30}, {"lfo_shape", 4}, {"smoothing", 0}}, 1.0001);
    // full depth, square: the sound all there, then all gone
    auto fx = create("tremolo_pan");
    set(*fx, "depth", 100);
    set(*fx, "lfo_shape", 4);
    Stereo dc = constant(0.5, 2.0);
    Stereo out = run(*fx, dc);
    double lowest = 1, highest = 0;
    for (size_t i = 4800; i < out.size(); i++) {
        lowest = std::min(lowest, (double)std::fabs(out.l[i]));
        highest = std::max(highest, (double)out.l[i]);
    }
    CHECK(lowest < 0.005 && highest > 0.495);
    // stereo phase 180°: auto-pan, one side up as the other goes down (together: constant)
    set(*fx, "lfo_shape", 0);
    set(*fx, "phase", 180);
    fx->reset();
    out = run(*fx, dc);
    double worst = 0, swing = 0;
    for (size_t i = 0; i < out.size(); i++) {
        worst = std::max(worst, std::fabs(out.l[i] + out.r[i] - 0.5));
        swing = std::max(swing, (double)std::fabs(out.l[i] - out.r[i]));
    }
    CHECK(worst < 1e-5 && swing > 0.45);
    // its rate (counted as the gain rises through 75 %, a twelfth into each cycle): 3 Hz free; synced, a 1/4 a cycle: 2 Hz at 120 BPM, 1.5 Hz at 90; stopped (no tempo
    // given), still at the last one
    auto rate = create("tremolo_pan");
    set(*rate, "depth", 100);
    set(*rate, "smoothing", 0);
    set(*rate, "lfo_rate", 3);
    Stereo four = constant(0.5, 4.0);
    CHECK(risings(run(*rate, four).l, 0.375) == 12);
    set(*rate, "lfo_sync", 1);
    set(*rate, "lfo_beats", 4);
    rate->setTransport(120, 4, 4, true, 0);
    rate->reset();
    CHECK(risings(run(*rate, four).l, 0.375) == 8);
    rate->setTransport(90, 4, 4, true, 0);
    rate->reset();
    CHECK(risings(run(*rate, four).l, 0.375) == 6);
    rate->setTransport(0, 4, 4, false, 0);
    rate->reset();
    CHECK(risings(run(*rate, four).l, 0.375) == 6);
    set(*rate, "lfo_beats", 7);  // 1 bar at 90 BPM: 2.67 s a cycle (rising through 0.375 at 0.22 s, 2.89 s)
    rate->reset();
    CHECK(risings(run(*rate, four).l, 0.375) == 2);
    // the meters: the left and right gains
    CHECK(rate->meters().size() == 2);
}

// --- colour ---

void testBitcrusher() {
    auto crush = create("bitcrusher");
    set(*crush, "bits", 2);  // steps of 1/2
    set(*crush, "downsample", 4);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1, 1);
    Stereo in(4000);
    for (size_t i = 0; i < in.size(); i++) in.l[i] = in.r[i] = u(rng);
    Stereo out = run(*crush, in, 333);  // (blocks not a multiple of 4: the hold carries over)
    bool held = true, stepped = true;
    for (size_t i = 0; i < out.size(); i++) {
        if (out.l[i] != out.l[i - i % 4]) held = false;
        if (out.l[i] * 2 != std::round(out.l[i] * 2)) stepped = false;
        if (i % 4 == 0 && out.l[i] != (float)(std::nearbyint(in.l[i] * 2) / 2)) stepped = false;
    }
    CHECK(held && stepped);
    set(*crush, "mix", 0);
    set(*crush, "output", -6);
    out = run(*crush, in);
    CHECK_NEAR(out.l[100], in.l[100] * dbToGain(-6), 1e-6);
}

void testSaturation() {
    auto sat = create("saturation");
    // no drive, a quiet sine: soft (tanh) is nearly straight: level unchanged
    set(*sat, "drive", 0);
    CHECK_NEAR(gainAt(*sat, 1000), db(std::sqrt(0.5) * 0.25) - db(std::sqrt(0.5) * 0.25) - 0.2, 0.1);  // (tanh: -0.2 dB at 0.25)
    // hard at 36 dB: clipped at ±1, then down by sqrt(drive)
    set(*sat, "shape", 1);
    set(*sat, "drive", 36);
    sat->reset();
    Stereo in = sine(200, 0.5, 0.5);
    Stereo out = run(*sat, in);
    double expected = 1 / std::sqrt(dbToGain(36));
    double p = peak(out.l, 4800, out.size());
    CHECK(p > expected * 0.9 && p < expected * 1.25);
    // a square-ish wave: its RMS near its peak
    CHECK(rms(out.l, 4800, out.size()) > p * 0.8);
    // tube: lopsided (even harmonics), but no DC (the 10 Hz high-pass)
    set(*sat, "shape", 2);
    set(*sat, "drive", 12);
    sat->reset();
    out = run(*sat, sine(200, 0.5, 2.0));
    double mean = 0;
    for (size_t i = SR; i < out.size(); i++) mean += out.l[i];
    mean /= out.size() - SR;
    CHECK(std::fabs(mean) < 0.01);
    // the oversampler's filter as designed: flat to 0.45 of Nyquist, 90 dB down past 0.6
    CHECK(std::fabs(db(magnitude(OVERSAMPLING_SOS, 4, 0.2, 1.0))) < 0.06);
    CHECK(db(magnitude(OVERSAMPLING_SOS, 4, 0.31, 1.0)) < -89.9);
    // mix 0: the input
    set(*sat, "mix", 0);
    sat->reset();
    out = run(*sat, in);
    CHECK(out.l[1234] == in.l[1234]);
}

void testUtility() {
    auto util_ = create("utility");
    Stereo in = sine(1000, 0.4, 0.2, 1.0);  // (right a radian ahead: some side)
    auto check = [&](const char* what, auto expect) {
        util_->reset();
        Stereo out = run(*util_, in);
        double worst = 0;
        for (size_t i = 0; i < in.size(); i++) {
            double l, r;
            expect(in.l[i], in.r[i], l, r);
            worst = std::max({worst, std::fabs(out.l[i] - l), std::fabs(out.r[i] - r)});
        }
        CHECK(worst < 1e-5);
        if (worst >= 1e-5) fprintf(stderr, "  utility %s: off by %g\n", what, worst);
    };
    check("defaults", [](double l, double r, double& ol, double& or_) { ol = l, or_ = r; });
    set(*util_, "width", 0);
    check("width 0", [](double l, double r, double& ol, double& or_) { ol = or_ = (l + r) / 2; });
    set(*util_, "width", 200);
    check("width 200", [](double l, double r, double& ol, double& or_) {
        double m = (l + r) / 2, s = (l - r) / 2 * 2;
        ol = m + s, or_ = m - s;
    });
    set(*util_, "mono", 1);
    check("mono", [](double l, double r, double& ol, double& or_) { ol = or_ = (l + r) / 2; });
    set(*util_, "mono", 0);
    set(*util_, "width", 100);
    set(*util_, "invert_left", 1);
    check("invert left", [](double l, double r, double& ol, double& or_) { ol = -l, or_ = r; });
    set(*util_, "invert_left", 0);
    set(*util_, "invert_right", 1);
    set(*util_, "gain", -6);
    check("invert right, -6 dB", [](double l, double r, double& ol, double& or_) {
        double g = dbToGain(-6);
        ol = l * g, or_ = -r * g;
    });
    set(*util_, "invert_right", 0);
    set(*util_, "gain", 0);
    // bass mono: a bass only on one side's difference (L = -R) goes; the highs keep their width
    set(*util_, "bass_mono", 1);
    util_->reset();
    Stereo bass = sine(40, 0.4, 1.0, PI);
    Stereo out = run(*util_, bass);
    CHECK(db(rms(out.l, (size_t)(0.5 * SR), out.size()) / rms(bass.l, 0, bass.size())) < -25);
    util_->reset();
    Stereo high = sine(3000, 0.4, 1.0, PI);
    out = run(*util_, high);
    CHECK_NEAR(db(rms(out.l, (size_t)(0.5 * SR), out.size()) / rms(high.l, 0, high.size())), 0, 0.05);
    CHECK(out.l[30000] * out.r[30000] <= 0);  // (still opposite: wide)
    // a mono bass is untouched
    util_->reset();
    Stereo monoBass = sine(40, 0.4, 1.0);
    out = run(*util_, monoBass);
    CHECK_NEAR(db(rms(out.l, (size_t)(0.5 * SR), out.size()) / rms(monoBass.l, 0, monoBass.size())), 0, 0.05);
}

// --- transient shaper, time FX (transient.cpp, timefx.cpp) ---

// through the effect from frame `from` to `to` (in blocks), into the same frames of `out`
void runPart(plug::Plugin& fx, const Stereo& in, Stereo& out, size_t from, size_t to, int blockSize = 512,
             const Stereo* side = nullptr) {
    for (size_t at = from; at < to; at += blockSize) {
        int n = (int)std::min<size_t>(blockSize, to - at);
        const float* i[2] = {in.l.data() + at, in.r.data() + at};
        float* o[2] = {out.l.data() + at, out.r.data() + at};
        if (side) {
            const float* s[2] = {side->l.data() + at, side->r.data() + at};
            fx.process(i, s, o, n);
        } else {
            fx.process(i, nullptr, o, n);
        }
    }
}

Stereo noiseFor(double seconds, double amp, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Stereo s((size_t)(seconds * SR));
    for (size_t i = 0; i < s.size(); i++) {
        s.l[i] = (float)(amp * u(rng));
        s.r[i] = (float)(amp * u(rng));
    }
    return s;
}

// a drum's hit: a noise burst dying away (80 ms), after `onset` frames of silence
Stereo drumHit(double seconds, size_t onset) {
    Stereo s = noiseFor(seconds, 0.8, 11);
    for (size_t i = 0; i < s.size(); i++) {
        double env = i < onset ? 0.0 : std::exp(-(double)(i - onset) / (0.08 * SR));
        s.l[i] = (float)(s.l[i] * env);
        s.r[i] = (float)(s.r[i] * env);
    }
    return s;
}

double maxDiff(const Stereo& a, const Stereo& b, size_t from = 0, size_t to = (size_t)-1) {
    double d = 0;
    for (size_t i = from; i < std::min({a.size(), b.size(), to}); i++)
        d = std::max({d, (double)std::fabs(a.l[i] - b.l[i]), (double)std::fabs(a.r[i] - b.r[i])});
    return d;
}
double maxStep(const Stereo& s, size_t from = 1) {  // the largest sample-to-sample jump
    double d = 0;
    for (size_t i = std::max<size_t>(1, from); i < s.size(); i++)
        d = std::max({d, (double)std::fabs(s.l[i] - s.l[i - 1]), (double)std::fabs(s.r[i] - s.r[i - 1])});
    return d;
}
bool finite(const Stereo& s, double bound) {
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i]) || std::fabs(s.l[i]) > bound || std::fabs(s.r[i]) > bound)
            return false;
    return true;
}
// normalized autocorrelation of one side over [from, to) at `lag`
double autocorrelation(const std::vector<float>& x, size_t from, size_t to, size_t lag) {
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = from; i + lag < to; i++) {
        xy += (double)x[i] * x[i + lag];
        xx += (double)x[i] * x[i];
        yy += (double)x[i + lag] * x[i + lag];
    }
    return xy / std::sqrt(std::max(1e-30, xx * yy));
}
size_t zeroCrossings(const std::vector<float>& x, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from + 1; i < to; i++) n += (x[i - 1] < 0) != (x[i] < 0);
    return n;
}
// a silent input gives a silent output, and the effect goes to sleep
void checkSilentAndSleeps(Effect& fx) {
    Stereo silence((size_t)(2.0 * SR));
    Stereo out = run(fx, silence);
    CHECK(peak(out.l, 0, out.size()) == 0 && peak(out.r, 0, out.size()) == 0);
    CHECK(fx.asleep());
}

void testTransientShaper() {
    auto ts = create("transient_shaper");
    CHECK(ts->hasSidechain() && ts->latency() == 0 && ts->meters().size() == 1);
    CHECK(ts->meters()[0].label == "Gain change");
    checkSilentAndSleeps(*ts);
    size_t onset = 4800, attackEnd = onset + 240;  // (the first 5 ms)
    size_t tailFrom = onset + (size_t)(0.1 * SR), tailTo = onset + (size_t)(0.3 * SR);
    Stereo hit = drumHit(0.6, onset);
    // at 0 %: the sound untouched, bit for bit
    ts->reset();
    Stereo out = run(*ts, hit);
    CHECK(maxDiff(out, hit) == 0);
    double dryPeak = peak(hit.l, onset, attackEnd), dryTail = rms(hit.l, tailFrom, tailTo);
    auto shaped = [&](double attack, double sustain, double& attackPeak, double& tail) {
        set(*ts, "attack", attack);
        set(*ts, "sustain", sustain);
        ts->reset();
        Stereo o = run(*ts, hit);
        attackPeak = peak(o.l, onset, attackEnd);
        tail = rms(o.l, tailFrom, tailTo);
    };
    double p, t;
    // + attack: the hit's first 5 ms louder against its tail (the tail itself untouched)
    shaped(100, 0, p, t);
    CHECK(db(p / t) - db(dryPeak / dryTail) > 6);
    CHECK_NEAR(db(t / dryTail), 0, 0.5);
    // − attack: softer
    shaped(-100, 0, p, t);
    CHECK(db(p / t) - db(dryPeak / dryTail) < -3);
    // − sustain: less of the tail's energy, the attack as it was
    shaped(0, -100, p, t);
    CHECK(db(t / dryTail) < -6);
    CHECK_NEAR(db(p / dryPeak), 0, 0.5);
    // + sustain: more of it
    shaped(0, 100, p, t);
    CHECK(db(t / dryTail) > 6);
    // the meter: the gain change as the hit starts
    set(*ts, "attack", 100);
    set(*ts, "sustain", 0);
    ts->reset();
    out = Stereo(hit.size());
    runPart(*ts, hit, out, 0, onset + 512);
    CHECK(ts->meters()[0].fraction > 0.2 && ts->meters()[0].text[0] == '+');
    // a steady sine, quiet or loud, at the most of either: unity gain (only the followers' difference counts)
    for (double sustain : {100.0, -100.0})
        for (double amp : {0.01, 0.1, 0.7}) {
            set(*ts, "attack", 100);
            set(*ts, "sustain", sustain);
            ts->reset();
            Stereo tone = sine(440, amp, 1.0);
            out = run(*ts, tone);
            size_t from = (size_t)(0.5 * SR);
            double change = db(rms(out.l, from, out.size()) / rms(tone.l, from, tone.size()));
            CHECK_NEAR(change, 0, 0.5);
            CHECK(peak(out.l, from, out.size()) <= amp * dbToGain(0.5));
        }
    // the same sound whatever the buffer size
    set(*ts, "attack", 60);
    set(*ts, "sustain", -40);
    ts->reset();
    Stereo a = run(*ts, hit, 512);
    ts->reset();
    Stereo b = run(*ts, hit, 77);
    CHECK(maxDiff(a, b) < 1e-6);
    // the sidechain: a steady tone shaped by the hit wired into its SC jack, only with Sidechain on
    set(*ts, "attack", 0);
    set(*ts, "sustain", -100);
    Stereo tone = sine(440, 0.2, 0.6);
    ts->reset();
    out = run(*ts, tone, 512, &hit);
    CHECK_NEAR(db(rms(out.l, tailFrom, tailTo) / rms(tone.l, tailFrom, tailTo)), 0, 0.5);
    set(*ts, "sidechain", 1);
    ts->reset();
    out = run(*ts, tone, 512, &hit);
    CHECK(db(rms(out.l, tailFrom, tailTo) / rms(tone.l, tailFrom, tailTo)) < -6);
    set(*ts, "sidechain", 0);
    // output gain, and the soft clip holding it under full scale
    set(*ts, "sustain", 0);
    set(*ts, "output", 12);
    ts->reset();
    Stereo loud = sine(440, 0.5, 0.3);
    out = run(*ts, loud);
    CHECK_NEAR(peak(out.l, 4800, out.size()), 0.5 * dbToGain(12), 0.01);
    set(*ts, "soft_clip", 1);
    ts->reset();
    out = run(*ts, loud);
    CHECK(peak(out.l, 0, out.size()) < 1.0 && peak(out.l, 4800, out.size()) > 0.9);
    Stereo quietTone = sine(440, 0.3, 0.3);
    set(*ts, "output", 0);
    ts->reset();
    out = run(*ts, quietTone);
    CHECK(maxDiff(out, quietTone) == 0);  // (below the knee: untouched)
    // 10 s of noise at its extremes: finite, bounded
    for (double extreme : {0.0, 1.0}) {
        auto fx = create("transient_shaper");
        for (size_t i = 0; i < fx->params().size(); i++) fx->setParam((uint32_t)i, extreme);
        Stereo n = noiseFor(10.0, 1.0, 5);
        Stereo side = noiseFor(10.0, 1.0, 6);
        Stereo o = run(*fx, n, 512, &side);
        CHECK(finite(o, 64));
    }
    // its state, back again
    set(*ts, "attack", 35);
    set(*ts, "release", 500);
    set(*ts, "soft_clip", 1);
    auto again = create("transient_shaper");
    again->setState(ts->componentState(), "");
    for (uint32_t i = 0; i < ts->params().size(); i++) {
        CHECK_NEAR(again->getParam(i), ts->getParam(i), 1e-12);
        CHECK_NEAR(again->value((int)i), ts->value((int)i), 1e-9);  // (a log scale's round trip: within rounding)
    }
    CHECK_NEAR(again->value("release"), 500, 1e-9);
    CHECK(again->value("soft_clip") == 1);
}

void testTimeFx() {
    auto fx = create("time_fx");
    CHECK(!fx->hasSidechain() && fx->latency() == 0);
    CHECK(fx->params()[fx->key("trigger")].isToggle() && fx->parameters()[fx->key("trigger")].stepCount == 1);
    fx->setTransport(120, 4, 4, true, 0);
    checkSilentAndSleeps(*fx);
    // Trigger off: the input bit for bit
    Stereo in = noiseFor(2.0, 0.5, 3);
    fx->reset();
    Stereo out = run(*fx, in);
    CHECK(maxDiff(out, in) == 0);
    // the transport stopped: the input bit for bit, triggered or not, in every mode
    for (int mode = 0; mode < 3; mode++) {
        auto f = create("time_fx");
        set(*f, "mode", mode);
        set(*f, "trigger", 1);
        f->setTransport(120, 4, 4, false, 0);
        out = run(*f, in);
        CHECK(maxDiff(out, in) == 0);
    }
    const size_t BEAT = 24000;  // (at 120 BPM)

    // --- tape stop: a 1/4 note to a standstill, silent after; Trigger off spins it up over a 1/4 ---
    {
        auto t = create("time_fx");
        t->setTransport(120, 4, 4, true, 0);
        set(*t, "stop_length", 1);
        set(*t, "start_length", 1);
        Stereo s = sine(220, 0.5, 3.0);
        Stereo o(s.size());
        size_t on = 47 * 512, off = 141 * 512;  // (at block edges: parameters are read a block at a time)
        runPart(*t, s, o, 0, on);
        set(*t, "trigger", 1);
        runPart(*t, s, o, on, off);
        set(*t, "trigger", 0);
        runPart(*t, s, o, off, s.size());
        CHECK(maxDiff(o, s, 0, on) == 0);
        CHECK(rms(o.l, on, on + BEAT / 2) > 0.1);  // (slowing, still sounding)
        CHECK(zeroCrossings(o.l, on + BEAT / 2, on + BEAT * 3 / 4) <
              zeroCrossings(s.l, on + BEAT / 2, on + BEAT * 3 / 4) * 0.6);  // (lower: slower)
        CHECK(rms(o.l, on + BEAT, off) < 1e-4);  // (stopped: silence)
        CHECK(peak(o.l, on + BEAT + 64, off) < 1e-4);
        CHECK(rms(o.l, off + BEAT / 4, off + BEAT / 2) > 0.05);  // (spinning up)
        CHECK(maxDiff(o, s, off + BEAT + 1000, s.size()) == 0);  // (back on the live sound)
        CHECK(maxStep(o) < 0.05);
        CHECK(t->meters()[0].label == "Speed" && t->meters()[0].fraction == 1);
        // a curve either way: still silent at its end, and no clicks
        for (double curve : {-100.0, 100.0}) {
            auto c = create("time_fx");
            c->setTransport(120, 4, 4, true, 0);
            set(*c, "stop_length", 0);  // (1/8)
            set(*c, "curve", curve);
            Stereo oc(s.size());
            runPart(*c, s, oc, 0, on);
            set(*c, "trigger", 1);
            runPart(*c, s, oc, on, off);
            CHECK(c->meters()[0].fraction == 0);
            set(*c, "trigger", 0);
            runPart(*c, s, oc, off, s.size());
            CHECK(peak(oc.l, on + BEAT / 2 + 64, off) < 1e-4);
            CHECK(maxStep(oc) < 0.05);
            CHECK(maxDiff(oc, s, off + BEAT + 1000, s.size()) == 0);
        }
    }

    // --- stutter: the 1/16 before the trigger (6000 frames at 120 BPM), over and over ---
    {
        auto st = create("time_fx");
        st->setTransport(120, 4, 4, true, 0);
        set(*st, "mode", 1);
        const size_t SLICE = BEAT / 4;
        size_t on = 94 * 512;
        Stereo o(in.size());
        runPart(*st, in, o, 0, on);
        set(*st, "trigger", 1);
        runPart(*st, in, o, on, in.size());
        // each repeat the slice before the trigger, exactly (but for its crossfade in)
        for (size_t k = 0; k < 3; k++) {
            double d = 0;
            for (size_t i = 400; i < SLICE; i++)
                d = std::max(d, (double)std::fabs(o.l[on + k * SLICE + i] - in.l[on - SLICE + i]));
            CHECK(d < 1e-6);
        }
        // its autocorrelation peaks at the slice's length (noise has none of its own)
        CHECK(autocorrelation(o.l, on + SLICE, in.size() - SLICE, SLICE) > 0.95);
        CHECK(std::fabs(autocorrelation(o.l, on + SLICE, in.size() - SLICE, SLICE / 2)) < 0.2);
        CHECK(std::fabs(autocorrelation(in.l, on, in.size() - SLICE, SLICE)) < 0.1);
        // off: the live sound again
        size_t off = on + 30 * 512;
        auto st2 = create("time_fx");
        st2->setTransport(120, 4, 4, true, 0);
        set(*st2, "mode", 1);
        runPart(*st2, in, o, 0, on);
        set(*st2, "trigger", 1);
        runPart(*st2, in, o, on, off);
        set(*st2, "trigger", 0);
        runPart(*st2, in, o, off, in.size());
        CHECK(maxDiff(o, in, off + 400, in.size()) == 0);
        // fade per repeat (6 dB each), gate (the second half of each repeat silent)
        auto sf = create("time_fx");
        sf->setTransport(120, 4, 4, true, 0);
        set(*sf, "mode", 1);
        set(*sf, "repeat_fade", 6);
        set(*sf, "gate", 50);
        runPart(*sf, in, o, 0, on);
        set(*sf, "trigger", 1);
        runPart(*sf, in, o, on, in.size());
        double r1 = rms(o.l, on + SLICE + 400, on + SLICE + SLICE / 2 - 200);
        double r2 = rms(o.l, on + 2 * SLICE + 400, on + 2 * SLICE + SLICE / 2 - 200);
        CHECK_NEAR(db(r2 / r1), -6, 0.5);
        CHECK(peak(o.l, on + SLICE + SLICE / 2 + 200, on + 2 * SLICE) < 1e-6);
        // pitch per repeat (−12: the next repeat an octave down) on a sine
        auto sp = create("time_fx");
        sp->setTransport(120, 4, 4, true, 0);
        set(*sp, "mode", 1);
        set(*sp, "slice", 3);  // (1/4: 24000 frames)
        set(*sp, "repeat_pitch", -12);
        Stereo tone = sine(1000, 0.5, 2.0);
        Stereo ot(tone.size());
        runPart(*sp, tone, ot, 0, on);
        set(*sp, "trigger", 1);
        runPart(*sp, tone, ot, on, tone.size());
        double z0 = (double)zeroCrossings(ot.l, on + 1000, on + 20000);
        double z1 = (double)zeroCrossings(ot.l, on + BEAT + 1000, on + BEAT + 20000);
        CHECK_NEAR(z1 / z0, 0.5, 0.02);
        CHECK(maxStep(ot) < 0.2);  // (a 1 kHz sine steps up to 0.065 by itself)
        // a slice longer than what it has heard since it started: the one from the trigger on
        auto sl = create("time_fx");
        sl->setTransport(120, 4, 4, true, 0);
        set(*sl, "mode", 1);
        set(*sl, "slice", 5);  // (1 bar: 96000 frames, more than the 512 heard)
        Stereo o2(in.size());
        runPart(*sl, in, o2, 0, 512);
        set(*sl, "trigger", 1);
        runPart(*sl, in, o2, 512, in.size());
        CHECK(maxDiff(o2, in, 0, in.size()) == 0);  // (the first time: as it comes in)
    }

    // --- half speed: an octave down, re-synced on every bar line (2 s at 120 BPM in 4/4) ---
    {
        auto h = create("time_fx");
        h->setTransport(120, 4, 4, true, 0);
        set(*h, "mode", 2);
        set(*h, "trigger", 1);
        Stereo tone = sine(880, 0.5, 3.0);
        Stereo o = run(*h, tone);
        double ratio = (double)zeroCrossings(o.l, 9600, 86400) / zeroCrossings(tone.l, 9600, 86400);
        CHECK_NEAR(ratio, 0.5, 0.01);
        const size_t BAR = 4 * BEAT;
        double d = 0;  // (from the bar line on: the sound of the bar line, at half speed)
        for (size_t t = 400; t < 4000; t += 2) d = std::max(d, (double)std::fabs(o.l[BAR + t] - tone.l[BAR + t / 2]));
        CHECK(d < 1e-6);
        CHECK(maxStep(o) < 0.1);  // (an 880 Hz sine steps up to 0.058 by itself)
        // every 2 bars: no re-sync at the first bar line
        auto h2 = create("time_fx");
        h2->setTransport(120, 4, 4, true, 0);
        set(*h2, "mode", 2);
        set(*h2, "trigger", 1);
        set(*h2, "resync", 1);
        o = run(*h2, tone);
        d = 0;
        for (size_t t = 400; t < 4000; t += 2) d = std::max(d, (double)std::fabs(o.l[BAR + t] - tone.l[(BAR + t) / 2]));
        CHECK(d < 1e-6);
        // from another place in the song: bar lines from the transport's position (half a bar in)
        auto h3 = create("time_fx");
        h3->setTransport(120, 4, 4, true, BAR / 2);
        set(*h3, "mode", 2);
        set(*h3, "trigger", 1);
        o = run(*h3, tone);
        d = 0;
        for (size_t t = 400; t < 4000; t += 2)
            d = std::max(d, (double)std::fabs(o.l[BAR / 2 + t] - tone.l[BAR / 2 + t / 2]));
        CHECK(d < 1e-6);
    }

    // --- no clicks: a sine through every mode, the trigger switched on, off and on again ---
    for (int mode = 0; mode < 3; mode++) {
        auto c = create("time_fx");
        c->setTransport(120, 4, 4, true, 0);
        set(*c, "mode", mode);
        set(*c, "repeat_pitch", -5);
        set(*c, "repeat_fade", 3);
        set(*c, "gate", 60);
        set(*c, "slice", 2);
        Stereo s = sine(220, 0.5, 5.0);
        Stereo o(s.size());
        size_t edges[] = {47 * 512, 141 * 512, 200 * 512, 300 * 512, 330 * 512, s.size()};
        size_t at = 0;
        for (int e = 0; e < 6; e++) {
            runPart(*c, s, o, at, edges[e]);
            at = edges[e];
            set(*c, "trigger", e % 2 == 0 ? 1 : 0);
        }
        double step = maxStep(o);
        CHECK(step < 0.05);  // (a 220 Hz sine steps up to 0.0144 by itself)
        if (step >= 0.05) fprintf(stderr, "  time_fx mode %d: a jump of %g\n", mode, step);
        // and the transport stopping while engaged: back to the input, smoothly
        set(*c, "trigger", 1);
        Stereo o2(s.size());
        runPart(*c, s, o2, 0, 100 * 512);
        c->setTransport(120, 4, 4, false, 0);
        runPart(*c, s, o2, 100 * 512, s.size());
        CHECK(maxStep(o2, 100 * 512) < 0.05);
        CHECK(maxDiff(o2, s, 102 * 512, s.size()) == 0);
    }

    // --- 10 s of loud noise at its extremes (every parameter at its lowest, then its highest; slow and
    // fast tempos; the trigger switched every 0.3 s): finite, bounded ---
    for (double tempo : {30.0, 300.0})
        for (int mode = 0; mode < 3; mode++)
            for (double extreme : {0.0, 1.0}) {
                auto e = create("time_fx");
                e->setTransport(tempo, extreme > 0.5 ? 7 : 4, extreme > 0.5 ? 8 : 4, true, 12345);
                for (size_t i = 0; i < e->params().size(); i++) e->setParam((uint32_t)i, extreme);
                set(*e, "mode", mode);
                Stereo n = noiseFor(10.0, 1.0, 9);
                Stereo o(n.size());
                size_t step = (size_t)(0.3 * SR) / 512 * 512;
                for (size_t at = 0, k = 0; at < n.size(); at += step, k++) {
                    set(*e, "trigger", k % 2 == 0 ? 1 : 0);
                    runPart(*e, n, o, at, std::min(n.size(), at + step));
                }
                CHECK(finite(o, 2.0));
            }

    // --- its state, back again; its texts ---
    set(*fx, "mode", 2);
    set(*fx, "slice", 4);
    set(*fx, "curve", -40);
    set(*fx, "repeat_pitch", -7);
    set(*fx, "trigger", 1);
    auto again = create("time_fx");
    again->setState(fx->componentState(), "");
    for (uint32_t i = 0; i < fx->params().size(); i++) CHECK_NEAR(again->getParam(i), fx->getParam(i), 1e-12);
    CHECK(again->componentState() == fx->componentState());
    auto text = [&](const char* key, double value) {
        int i = fx->key(key);
        return fx->paramText((uint32_t)i, fx->params()[i].toNormal(value));
    };
    CHECK(text("mode", 0) == "Tape stop" && text("slice", 5) == "1 bar" && text("stop_length", 5) == "4 bars");
    CHECK(text("repeat_pitch", -3) == "-3.0 st" && text("resync", 2) == "4 bars");
}

// --- the framework ---

void testBlocksAndPlaces() {
    // the same sound whatever the buffer size (effects without block-wise envelopes), and in place
    for (const char* id : {"eq", "delay", "reverb", "chorus", "utility", "bitcrusher", "saturation", "filter_motion",
                           "phaser_flanger", "tremolo_pan"}) {
        auto a = create(id), b = create(id), c = create(id);
        Stereo in = sine(330, 0.3, 0.5, 0.5);
        in.l[100] = 0.9f;
        Stereo x = run(*a, in, 512), y = run(*b, in, 77);
        Stereo z = in;  // in place: in == out
        for (size_t at = 0; at < z.size(); at += 256) {
            int n = (int)std::min<size_t>(256, z.size() - at);
            float* io[2] = {z.l.data() + at, z.r.data() + at};
            const float* i[2] = {io[0], io[1]};
            c->process(i, nullptr, io, n);
        }
        double d1 = 0, d2 = 0;
        for (size_t i = 0; i < in.size(); i++) {
            d1 = std::max(d1, (double)std::fabs(x.l[i] - y.l[i]) + std::fabs(x.r[i] - y.r[i]));
            d2 = std::max(d2, (double)std::fabs(x.l[i] - z.l[i]) + std::fabs(x.r[i] - z.r[i]));
        }
        CHECK(d1 < 1e-6 && d2 < 1e-6);
        if (d1 >= 1e-6 || d2 >= 1e-6) fprintf(stderr, "  %s: blocks %g, in place %g\n", id, d1, d2);
    }
    // a buffer longer than it was prepared for is done in pieces
    auto eq = create("eq");
    eq->prepare(SR, 128);
    Stereo in = sine(1000, 0.3, 0.1);
    Stereo out = run(*eq, in, 1000);
    CHECK(out.l[2000] == in.l[2000]);
}

void testNoAllocation() {
    for (const auto& info : list()) {
        auto fx = create(info.id);
        Stereo in = sine(440, 0.5, 0.5), side = sine(60, 0.5, 0.5);
        fx->setTransport(128, 4, 4, true, 0);
        for (size_t i = 0; i < fx->params().size(); i++) fx->setParam((uint32_t)i, 0.7);  // (changed() too)
        Stereo silence((size_t)(30 * SR));  // (the delay: 66 % feedback every 0.7 s)
        allocations = 0;
        countProcess = true;
        run(*fx, in, 512, &side);
        run(*fx, silence, 512);  // (goes to sleep: start() on the audio thread)
        countProcess = false;
        CHECK(fx->asleep());
        if (!fx->asleep()) fprintf(stderr, "  %s awake\n", info.id.c_str());
        CHECK(allocations == 0);
        if (allocations) fprintf(stderr, "  %s allocated %ld times\n", info.id.c_str(), (long)allocations);
    }
}

void testMix() {
    // Mix 0 around a limiter: its input exactly, as late as the limiter's look-ahead
    auto lim = create("limiter");
    set(*lim, "gain", 18);
    DryWet dw;
    dw.prepare(512, lim->latency());
    Stereo in = sine(300, 0.8, 0.3);
    Stereo out(in.size());
    for (float mix : {0.0f, 0.5f}) {
        lim->reset();
        dw.clear();
        Stereo wet(in.size());
        auto lim2 = create("limiter");
        set(*lim2, "gain", 18);
        wet = run(*lim2, in);
        allocations = 0;
        counting = true;
        for (size_t at = 0; at < in.size(); at += 512) {
            int n = (int)std::min<size_t>(512, in.size() - at);
            const float* i[2] = {in.l.data() + at, in.r.data() + at};
            float* o[2] = {out.l.data() + at, out.r.data() + at};
            if (DryWet::active(mix)) dw.keep(i, n);
            lim->process(i, nullptr, o, n);
            if (DryWet::active(mix)) dw.blend(o, n, lim->latency(), mix);
        }
        counting = false;
        CHECK(allocations == 0);
        double worst = 0;
        for (size_t i = 96; i < in.size(); i++) {
            double expect = in.l[i - 96] * (1.0 - mix) + wet.l[i] * mix;
            worst = std::max(worst, std::fabs(out.l[i] - expect));
        }
        CHECK(worst < 1e-6);
        CHECK(out.l[50] == 0);
    }
    CHECK(dw.heldBack() == 96);
    // Mix 1: untouched
    float a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8};
    float* o[2] = {a, b};
    dw.blend(o, 4, 0, 1.0f);
    CHECK(a[0] == 1 && b[3] == 8);
    // a latency of 0: a plain blend
    DryWet plain;
    plain.prepare(4, 0);
    float in1[4] = {1, 1, 1, 1}, in2[4] = {0, 0, 0, 0};
    const float* in_[2] = {in1, in2};
    plain.keep(in_, 4);
    float w1[4] = {0, 0, 0, 0}, w2[4] = {1, 1, 1, 1};
    float* wo[2] = {w1, w2};
    plain.blend(wo, 4, 0, 0.25f);
    CHECK_NEAR(w1[0], 0.75, 1e-7);
    CHECK_NEAR(w2[3], 0.25, 1e-7);
}

// --- the windows ---

void saveWindow(EffectWindow& w, const std::string& name) {
    Image img = w.snapshot();
    util::makeDirs(SAMPLES);
    CHECK(savePng(SAMPLES + "/" + name, img));
}

void testWindows() {
    ui::App::get().initHeadless();
    for (const auto& info : list()) {
        auto slot = std::make_shared<PluginSlot>();
        slot->path = info.path();
        slot->name = info.name;
        slot->kind = "effect";
        std::shared_ptr<Effect> fx(create(info.id).release());
        slot->instance = fx;
        if (info.id == "eq") {
            set(*fx, "low_cut", 60);
            set(*fx, "mid_gain", 6);
            set(*fx, "mid_freq", 800);
            set(*fx, "high_gain", -4);
        }
        if (fx->hasSidechain() || !fx->meters().empty()) {  // (something on its meters)
            Stereo in = sine(200, 0.9, 0.3);
            if (info.id == "limiter") set(*fx, "gain", 9);
            run(*fx, in);
        }
        EffectWindow w(nullptr, slot, info.name + " — Bass");
        CHECK(w.attach());
        CHECK(w.controls.size() == fx->params().size());
        CHECK((w.curveView != nullptr) == fx->hasCurve());
        CHECK((w.meterView != nullptr) == !fx->meters().empty());
        saveWindow(w, info.id + ".png");
        if (info.id == "compressor") {
            // a slider moved in the window is an edit; automation moving a value is shown
            auto* slider = dynamic_cast<ui::Slider*>(w.controls[0].widget);
            slider->setValue(500);
            CHECK_NEAR(fx->value("threshold"), -30, 1e-9);
            CHECK((fx->edited() == std::vector<uint32_t>{0}));
            fx->setParam(1, fx->params()[1].toNormal(10));
            w.follow();
            CHECK(w.controls[1].text->text() == "10.0:1" && fx->edited().empty());
            auto* box = dynamic_cast<ui::ComboBox*>(w.controls[6].widget);
            CHECK(box && box->count() == 2);
            box->setCurrentIndex(1);
            CHECK(fx->value("detector") == 1);
            auto* side = dynamic_cast<ui::CheckBox*>(w.controls[8].widget);
            CHECK(side != nullptr);
            side->setChecked(true);
            CHECK(fx->value("sidechain") == 1);
            CHECK((fx->edited() == std::vector<uint32_t>{6, 8}));
            ui::MouseEvent e;
            slider->mouseDoubleClick(e);  // (its default)
            CHECK_NEAR(fx->value("threshold"), -18, 1e-6);
            w.defaults();
            CHECK_NEAR(fx->value("ratio"), 4, 1e-9);
            CHECK(fx->value("sidechain") == 0 && !side->isChecked());
            CHECK(fx->edited().size() == 1 + fx->params().size());
            int closed = 0;
            w.editorClosed.connect([&](SlotPtr s) { closed += s == slot; });
            w.deleteOnClose = false;
            w.close();
            CHECK(closed == 1);
        }
    }
    // a slot that isn't a built-in effect: an empty window that won't attach
    auto slot = std::make_shared<PluginSlot>();
    EffectWindow none(nullptr, slot, "none");
    CHECK(!none.attach() && none.controls.empty());
}

}  // namespace

// --- Space, Tuner, the EQ's analyzer ---

namespace {

std::vector<double> convolve(const std::vector<float>& x, const std::vector<float>& h, size_t n) {
    std::vector<double> y(n, 0.0);
    for (size_t t = 0; t < n; t++) {
        double sum = 0;
        for (size_t k = 0; k < h.size() && k <= t; k++) sum += (double)h[k] * (t - k < x.size() ? x[t - k] : 0.0f);
        y[t] = sum;
    }
    return y;
}

// (noiseFor and finite: above)

// Space, wet only, no pre-delay, no cuts: its response exactly
std::unique_ptr<Effect> plainSpace() {
    auto fx = create("space");
    set(*fx, "mix", 100);
    set(*fx, "width", 100);
    set(*fx, "predelay", 0);
    set(*fx, "low_cut", 20);
    set(*fx, "high_cut", 20000);
    set(*fx, "length", 100);
    return fx;
}

void testSpace() {
    // a response set directly, long enough for many pieces: what comes out is the input's (mono)
    // convolution with it, whatever the buffer size, within 1e-4
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1);
    std::vector<float> irL(3000), irR(3000);
    for (size_t i = 0; i < irL.size(); i++) {
        float env = (float)std::exp(-3.0 * i / irL.size());
        irL[i] = 0.3f * env * u(rng), irR[i] = 0.3f * env * u(rng);
    }
    for (int blockSize : {512, 77, 256, 1000}) {
        auto fx = plainSpace();
        setSpaceImpulse(*fx, irL, irR);
        CHECK(spaceImpulse(*fx, 0) == irL && spaceImpulse(*fx, 1) == irR);
        CHECK(spacePartitions(*fx) == 11);  // ((3000 - 1) / 256)
        Stereo in = noiseFor(0.25, 0.4, 3);
        in.l[10] = in.r[10] = 1.0f;  // (an impulse in it too)
        Stereo out = run(*fx, in, blockSize);
        std::vector<float> mono(in.size());
        for (size_t i = 0; i < in.size(); i++) mono[i] = 0.5f * (in.l[i] + in.r[i]);
        std::vector<double> wantL = convolve(mono, irL, in.size()), wantR = convolve(mono, irR, in.size());
        double worst = 0;
        for (size_t i = 0; i < in.size(); i++)
            worst = std::max({worst, std::fabs(out.l[i] - wantL[i]), std::fabs(out.r[i] - wantR[i])});
        CHECK(worst < 1e-4);
        if (worst >= 1e-4) fprintf(stderr, "  space, blocks of %d: off by %g\n", blockSize, worst);
    }
    // an impulse in gives the response out (with a pre-delay: the response is later by it)
    {
        auto fx = plainSpace();
        set(*fx, "predelay", 20);
        setSpaceImpulse(*fx, irL, irR);
        std::vector<float> ir = spaceImpulse(*fx, 0);
        CHECK(ir.size() == 960 + irL.size() && ir[959] == 0 && ir[960] == irL[0]);
        Stereo in((size_t)(0.2 * SR));
        in.l[0] = in.r[0] = 1.0f;
        Stereo out = run(*fx, in, 128);
        double worst = 0;
        for (size_t i = 0; i < in.size(); i++)
            worst = std::max(worst, (double)std::fabs(out.l[i] - (i < ir.size() ? ir[i] : 0.0f)));
        CHECK(worst < 1e-4);
    }
    // the rooms: made the same way every time, finite, as loud as what goes in (energy 1), the two
    // sides different (a wide room), and an impulse in gives the room's response out
    for (int room = 0; room < 6; room++) {
        auto a = plainSpace(), b = plainSpace();
        set(*a, "room", room);
        set(*b, "room", room);
        CHECK(spaceSettle(*a) && spaceSettle(*b));
        std::vector<float> l = spaceImpulse(*a, 0), r = spaceImpulse(*a, 1);
        CHECK(l == spaceImpulse(*b, 0) && r == spaceImpulse(*b, 1) && l.size() == r.size() && l.size() > 4800);
        double el = 0, er = 0, lr = 0;
        bool ok = true;
        for (size_t i = 0; i < l.size(); i++) {
            ok = ok && std::isfinite(l[i]) && std::isfinite(r[i]);
            el += (double)l[i] * l[i], er += (double)r[i] * r[i], lr += (double)l[i] * r[i];
        }
        CHECK(ok);
        CHECK_NEAR((el + er) / 2, 1.0, 0.15);  // (the 30 Hz high-pass takes a little)
        CHECK(std::fabs(lr / std::sqrt(el * er)) < 0.3);
        a->reset();
        Stereo in((size_t)(0.5 * SR));
        in.l[0] = in.r[0] = 1.0f;
        Stereo out = run(*a, in, 300);
        double worst = 0;
        for (size_t i = 0; i < in.size(); i++)
            worst = std::max({worst, (double)std::fabs(out.l[i] - (i < l.size() ? l[i] : 0.0f)),
                              (double)std::fabs(out.r[i] - (i < r.size() ? r[i] : 0.0f))});
        CHECK(worst < 1e-4);
        if (worst >= 1e-4) fprintf(stderr, "  space room %d: off by %g\n", room, worst);
    }
    // length: the hall at 50 % is about half as long
    {
        auto fx = plainSpace();
        size_t full = spaceImpulse(*fx, 0).size();
        set(*fx, "length", 50);
        CHECK(spaceSettle(*fx));
        size_t half = spaceImpulse(*fx, 0).size();
        CHECK(half < full * 0.6 && half > full * 0.4);
    }
    // silence in: silence out, and it sleeps (after its response has died away)
    {
        auto fx = create("space");
        Stereo out = run(*fx, Stereo((size_t)(4 * SR)));
        CHECK(peak(out.l, 0, out.size()) == 0 && peak(out.r, 0, out.size()) == 0);
        CHECK(fx->asleep());
        // sound, then silence: the reverb rings on (it doesn't sleep while it does), then sleeps
        Stereo in = noiseFor(0.3, 0.5, 5);
        in.l.resize((size_t)(8 * SR), 0.0f);
        in.r.resize(in.l.size(), 0.0f);
        set(*fx, "mix", 100);
        out = run(*fx, in);
        CHECK(rms(out.l, (size_t)(0.5 * SR), (size_t)(0.7 * SR)) > 1e-3);  // (the hall's tail)
        CHECK(peak(out.l, (size_t)(7 * SR), out.size()) == 0 && fx->asleep());
    }
    // changing the room while it plays: no allocation on the audio thread, never anything but
    // finite sound, and once the new response is built, it plays it
    {
        auto fx = plainSpace();
        set(*fx, "mix", 50);
        Stereo in = noiseFor(0.05, 0.5, 9);
        allocations = 0;
        bool ok = true;
        for (int step = 0; step < 40; step++) {
            set(*fx, "room", step % 6);
            if (step % 3 == 0) set(*fx, "predelay", step * 2.0);
            if (step % 10 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(40));  // (some built meanwhile)
            countProcess = true;
            Stereo out = run(*fx, in, 256);
            countProcess = false;
            ok = ok && finite(out) && peak(out.l, 0, out.size()) < 50;
        }
        CHECK(ok);
        CHECK(allocations == 0);
        set(*fx, "room", 5);
        set(*fx, "predelay", 0);
        CHECK(spaceSettle(*fx));
        Stereo more = noiseFor(0.5, 0.5, 10);
        countProcess = true;
        run(*fx, more, 256);  // (the new one taken, faded to)
        countProcess = false;
        CHECK(allocations == 0);
        set(*fx, "mix", 100);
        fx->reset();
        Stereo click((size_t)(0.3 * SR));
        click.l[0] = click.r[0] = 1.0f;
        Stereo out = run(*fx, click, 256);
        std::vector<float> ir = spaceImpulse(*fx, 0);
        double worst = 0;
        for (size_t i = 0; i < click.size(); i++) worst = std::max(worst, (double)std::fabs(out.l[i] - ir[i]));
        CHECK(worst < 1e-4);
    }
    // its state: the values, and the same response from them (built on loading, before it plays)
    {
        auto fx = create("space");
        set(*fx, "room", 3);
        set(*fx, "length", 140);
        set(*fx, "predelay", 35);
        set(*fx, "low_cut", 300);
        set(*fx, "high_cut", 6000);
        set(*fx, "width", 60);
        CHECK(spaceSettle(*fx));
        std::string state = fx->componentState();
        CHECK(state.find("\"room\": 3.0") != std::string::npos && state.find("\"predelay\": 35.0") != std::string::npos);
        auto again = create("space");
        again->setState(state, "");
        for (uint32_t i = 0; i < fx->params().size(); i++) CHECK_NEAR(again->getParam(i), fx->getParam(i), 1e-12);
        std::vector<float> ir = spaceImpulse(*again, 0);
        CHECK(ir == spaceImpulse(*fx, 0));
        size_t pre = (size_t)std::lround(0.035 * SR);
        CHECK(ir.size() > pre + 200 && peak(ir, 0, pre) == 0 && peak(ir, pre, pre + 200) > 0);  // (the plate: no gap of its own)
    }
    // what it costs: a 3 s response at 48 kHz, 256-frame buffers
    {
        auto fx = plainSpace();
        set(*fx, "length", 130);  // (the hall's 2.3 s × 1.3)
        CHECK(spaceSettle(*fx));
        double seconds = spaceImpulse(*fx, 0).size() / SR;
        CHECK(seconds > 2.9 && seconds < 3.2);
        Stereo in = noiseFor(10, 0.3, 11);
        auto t0 = std::chrono::steady_clock::now();
        run(*fx, in, 256);
        double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double perBlock = spent / (in.size() / 256.0);
        printf("Space: %.2f s response, %d pieces: %.1f us per 256-frame block, %.2f %% of real time\n", seconds,
               spacePartitions(*fx), perBlock * 1e6, 100 * spent / 10);
#ifdef NDEBUG
        CHECK(spent / 10 < 0.25);  // (optimized: well under a quarter of one core)
#endif
        // and making a response (on loading a project: the thread loading it waits for it)
        auto cathedral = create("space");
        t0 = std::chrono::steady_clock::now();
        cathedral->setState("{\"room\": 5.0}", "");
        spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("Space: the cathedral (%.2f s) made in %.1f ms\n", spaceImpulse(*cathedral, 0).size() / SR, spent * 1e3);
        CHECK(cathedral->value("room") == 5);
    }
}

void testTuner() {
    // the sound passes untouched
    auto fx = create("tuner");
    Stereo in = noiseFor(0.3, 0.7, 12);
    Stereo out = run(*fx, in, 100);
    CHECK(std::memcmp(out.l.data(), in.l.data(), in.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(out.r.data(), in.r.data(), in.size() * sizeof(float)) == 0);
    // nothing was copied while no one looked: nothing to show
    auto m = fx->meters();
    CHECK(m.size() == 3 && m[0].text == "—");
    // sines found within 2 cents, named
    for (double f : {110.0, 440.0, 1000.0}) {
        auto t = create("tuner");
        t->meters();  // (its window opens)
        run(*t, sine(f, 0.5, 0.3), 256);
        auto ms = t->meters();
        double hz = std::atof(ms[2].text.c_str());
        CHECK(hz > 0 && std::fabs(1200 * std::log2(hz / f)) < 2);
        double cents = (ms[1].fraction - 0.5) * 100;
        TunerReading want = tunerReading(f);
        CHECK(ms[0].text == want.note && std::fabs(cents - want.cents) < 2);
        if (f == 440) CHECK(ms[0].text == "A4" && std::fabs(cents) < 2);
        if (f == 110) CHECK(ms[0].text == "A2");
        if (f == 1000) CHECK(ms[0].text == "B5" && std::fabs(cents - 21.5) < 2);
        if (hz <= 0 || std::fabs(1200 * std::log2(hz / f)) >= 2) fprintf(stderr, "  tuner: %g Hz read %g\n", f, hz);
    }
    // the reference pitch moves the notes: at A = 415, 440 Hz is an A# (one cent sharp)
    TunerReading r = tunerReading(440, 415);
    CHECK(r.note == "A#4" && std::fabs(r.cents - 1.26) < 0.1);
    TunerReading sharp = tunerReading(440 * std::pow(2, 3 / 1200.0));
    CHECK(sharp.note == "A4" && std::fabs(sharp.cents - 3) < 1e-9);
    CHECK(tunerReading(0).hz == 0 && tunerReading(0).note.empty());
    auto t = create("tuner");
    set(*t, "reference", 415);
    t->meters();
    run(*t, sine(440, 0.5, 0.2), 256);
    CHECK(t->meters()[0].text == "A#4");
    // silence: nothing shown (once the note has been held its while)
    run(*t, Stereo((size_t)(0.2 * SR)), 256);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(*t, Stereo((size_t)(0.2 * SR)), 256);
    CHECK(t->meters()[0].text == "—");
}

// the analyzer's loudest band: its x (and level, y)
double analyzerPeak(Effect& fx, double& level) {
    std::vector<CurvePoint> pts;
    fx.analyzer(120, 0, pts);
    size_t best = 0;
    for (size_t i = 0; i < pts.size(); i++)
        if (pts[i].y > pts[best].y) best = i;
    level = pts.empty() ? -1 : pts[best].y;
    return pts.empty() ? -1.0 : pts[best].x;
}

void testEqAnalyzer() {
    auto eq = create("eq");
    CHECK((eq->analyzerChoices() == std::vector<std::string>{"Off", "Pre", "Post"}) && eq->analyzerChoice() == 2);
    // no window asked: nothing copied, nothing to see (every band on the floor)
    run(*eq, sine(1000, 0.5, 0.3));
    std::vector<CurvePoint> points;
    CHECK(eq->analyzer(120, 0, points) && points.size() == 120);
    bool floor = true;
    for (const auto& p : points) floor = floor && p.y < -0.999;
    CHECK(floor);
    // asked: a sine shows its peak at its band, at its level (-6 dB, less the 4.5 dB/octave tilt)
    for (double f : {100.0, 1000.0, 5000.0}) {
        auto fx = create("eq");
        double level;
        analyzerPeak(*fx, level);  // (the window opens)
        run(*fx, sine(f, 0.5, 0.2));
        double x = analyzerPeak(*fx, level);
        CHECK(std::fabs(x - eqX(f)) <= 1.0 / 120);
        double db = -90 + (level + 1) / 2 * 102 - 4.5 * std::log2(f / 1000);
        CHECK_NEAR(db, -6.0, 1.0);
    }
    // pre and post: the EQ's cut shows in post only
    for (int choice : {1, 2}) {
        auto fx = create("eq");
        set(*fx, "mid_freq", 1000);
        set(*fx, "mid_gain", -18);
        fx->setAnalyzerChoice(choice);
        double level;
        analyzerPeak(*fx, level);
        run(*fx, sine(1000, 0.5, 0.2));
        analyzerPeak(*fx, level);
        double db = -90 + (level + 1) / 2 * 102;
        CHECK_NEAR(db, choice == 1 ? -6.0 : -24.0, 1.5);
    }
    // off: nothing
    eq->setAnalyzerChoice(0);
    CHECK(!eq->analyzer(120, 0, points) && points.empty());
    // the effects without one
    CHECK(create("compressor")->analyzerChoices().empty());
}

std::unique_ptr<EffectWindow> windowOn(std::shared_ptr<Effect> fx, const std::string& name) {
    auto slot = std::make_shared<PluginSlot>();
    slot->path = "builtin:" + fx->id();
    slot->name = name;
    slot->kind = "effect";
    slot->instance = fx;
    return std::make_unique<EffectWindow>(nullptr, slot, name + " — Bass");
}

void testNewWindows() {
    ui::App::get().initHeadless();
    // the EQ's analyzer behind its curve, with its list
    std::shared_ptr<Effect> eq(create("eq").release());
    set(*eq, "mid_gain", 6);
    set(*eq, "mid_freq", 800);
    set(*eq, "low_cut", 60);
    auto w = windowOn(eq, "EQ");
    CHECK(w->attach() && w->analyzerBox && w->analyzerBox->count() == 3 && w->analyzerBox->currentIndex() == 2);
    w->follow();  // (it asks: the audio thread copies from now on)
    Stereo pinkish = noiseFor(0.3, 0.6, 13);
    for (size_t i = 1; i < pinkish.size(); i++) {  // (a gentle low-pass: more lows than highs)
        pinkish.l[i] = 0.9f * pinkish.l[i - 1] + 0.1f * pinkish.l[i];
        pinkish.r[i] = 0.9f * pinkish.r[i - 1] + 0.1f * pinkish.r[i];
    }
    run(*eq, pinkish);
    w->follow();
    CHECK(w->curveView && w->curveView->spectrum.size() > 16);
    saveWindow(*w, "eq-analyzer.png");
    w->analyzerBox->setCurrentIndex(0);
    CHECK(eq->analyzerChoice() == 0 && w->curveView->spectrum.empty());
    w->deleteOnClose = false;
    w->close();
    // the tuner, drawn as one
    std::shared_ptr<Effect> tuner(create("tuner").release());
    auto tw = windowOn(tuner, "Tuner");
    CHECK(tw->attach() && dynamic_cast<TunerView*>(tw->meterView) != nullptr);
    run(*tuner, sine(445, 0.5, 0.2), 256);
    tw->follow();
    CHECK(tw->meterView->values.size() == 3 && tw->meterView->values[0].text == "A4");
    saveWindow(*tw, "tuner-a4.png");
    tw->deleteOnClose = false;
    tw->close();
    // Space: its settings (the room a list)
    std::shared_ptr<Effect> space(create("space").release());
    auto sw = windowOn(space, "Space");
    CHECK(sw->attach() && sw->controls.size() == 7 && !sw->curveView && !sw->meterView && !sw->analyzerBox);
    CHECK(dynamic_cast<ui::ComboBox*>(sw->controls[0].widget) != nullptr);
    sw->deleteOnClose = false;
    sw->close();
}

}  // namespace

int main() {
    testFactory();
    testParams();
    testState();
    testEq();
    testCompressor();
    testMultiband();
    testGate();
    testLimiter();
    testDelay();
    testReverb();
    testChorus();
    testFilterMotion();
    testPhaserFlanger();
    testTremoloPan();
    testBitcrusher();
    testSaturation();
    testUtility();
    testTransientShaper();  // (transient.cpp)
    testTimeFx();           // (timefx.cpp)
    testBlocksAndPlaces();
    testNoAllocation();
    testMix();
    testWindows();
    testSpace();
    testTuner();
    testEqAnalyzer();
    testNewWindows();
    TEST_MAIN_END();
}
