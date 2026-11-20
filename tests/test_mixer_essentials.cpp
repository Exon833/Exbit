// The mixer essentials, worst cases first: solo, solo safe and exclusive solo (256 tracks soloed and let go every
// buffer while the mixer plays them on every core, and from another thread at once); automation in time behind an
// effect with 10,000 frames of look-ahead, sample for sample; the master's true peak (a known inter-sample peak);
// RMS meters; solo safe saved, loaded and undone; the S button (Alt-click, its right-click menu) and the terminal's
// keys (S, Shift+S) and the master row's dBTP.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/metering.h"
#include "audio/mixer.h"
#include "audio/true_peak.h"
#include "audio/workers.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "model/history.h"
#include "model/model.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "plugins/plugin.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/solo_button.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

namespace {

const int SR = 48000;
const std::string TMP = "build/ui-samples/mixer-essentials";

// a gain effect (parameter "Gain", at its input) that holds its sound back by `lat` frames and says so
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
    void reset() override { ring[0].clear(), ring[1].clear(); }
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

SlotPtr slotOf(std::shared_ptr<FakeFx> p) {
    auto s = std::make_shared<PluginSlot>();
    s->name = p->name();
    s->kind = "effect";
    s->instance = p;
    return s;
}

ClipPtr constantClip(const TrackPtr& t, int64_t frames, float value) {
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.assign((size_t)frames * 2, value);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "dc";
    c->track = t;
    c->start = 0;
    c->data = buf;
    c->source = buf;
    c->sourceSr = SR;
    return c;
}

AutomationPtr lane(SlotPtr slot, const std::string& param, std::vector<AutoPoint> pts) {
    auto a = std::make_shared<Automation>();
    a->slot = std::move(slot);
    a->param = param;
    a->setPoints(std::move(pts));
    return a;
}

}  // namespace

// --- solo, solo safe, exclusive solo: the rules ---
static void testSoloRules() {
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio), c = p.addTrack(Kind::Midi);
    c->needsInstrument = false;
    TrackPtr d = p.addTrack(Kind::Audio), r = p.addTrack(Kind::Return);
    GroupPtr g = p.groupTracks({d});
    for (auto* t : {a.get(), b.get(), c.get(), d.get(), r.get()}) CHECK(p.audible(*t));  // nothing soloed: all
    a->soloed = true;
    CHECK(p.audible(*a) && !p.audible(*b) && !p.audible(*c) && !p.audible(*d) && p.audible(*r));
    b->soloSafe = true;  // solo safe: heard while others are soloed
    CHECK(p.audible(*b) && !p.audible(*c));
    b->muted = true;  // …but mute still mutes
    CHECK(!p.audible(*b));
    b->muted = false;
    g->soloed = true;  // a group soloed: its tracks too (solo stays additive)
    CHECK(p.audible(*a) && p.audible(*d) && !p.audible(*c));
    // exclusive solo: c alone (groups let go too); b, solo safe, still heard; the return too
    p.soloOnly(c);
    CHECK(c->soloed && !a->soloed && !b->soloed && !d->soloed && !g->soloed);
    CHECK(p.audible(*c) && !p.audible(*a) && p.audible(*b) && !p.audible(*d) && p.audible(*r));
    a->soloed = c->soloed = false;  // nothing soloed: solo safe changes nothing
    for (auto* t : {a.get(), b.get(), c.get(), d.get(), r.get()}) CHECK(p.audible(*t));
}

// --- 256 tracks soloed and let go every buffer while playing (on every core) ---
static void testSolo256() {
    const int N = 256, BLOCK = 256;
    Project p;
    std::vector<TrackPtr> ts;
    std::vector<ClipPtr> clips;
    for (int k = 0; k < N; k++) {
        TrackPtr t = p.addTrack(Kind::Audio);
        ts.push_back(t);
        clips.push_back(constantClip(t, SR * 10, (float)(k + 1) / 1024));  // (sums exact in float)
    }
    p.clips = clips;
    TrackPtr ret = p.addTrack(Kind::Return);
    ts[0]->sends = std::vector<Send>{Send{ret, 0.0, false}};  // track 0 also reaches the return
    GroupPtr g = p.groupTracks({ts[1], ts[2], ts[3], ts[4]});

    unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    workers::Pool pool((int)cores - 1, false);
    mixer::MixState st;
    st.pool = &pool;
    std::mt19937 rng(14);
    std::vector<float> out((size_t)BLOCK * 2);
    int64_t pos = 1000;  // (past the clips' edge fades)
    bool exact = true;
    for (int buffer = 0; buffer < 300; buffer++, pos += BLOCK) {
        // a new pattern each buffer: some soloed, some solo safe, some muted, the group soloed or not
        for (auto& t : ts) {
            if (t == ret) continue;
            t->soloed = rng() % 7 == 0;
            t->soloSafe = rng() % 11 == 0;
            t->muted = rng() % 13 == 0;
        }
        g->soloed = rng() % 3 == 0;
        if (buffer % 50 == 0) p.soloOnly(ts[(size_t)(rng() % N)]);
        if (buffer % 50 == 25)
            for (auto& t : ts) t->soloed = false;  // let them all go
        double want = 0;
        for (int k = 0; k < N; k++)
            if (p.audible(*ts[(size_t)k])) want += (k + 1) / 1024.0 * (k == 0 ? 2 : 1);  // (track 0: and its return)
        std::fill(out.begin(), out.end(), 0.0f);
        mixer::mixClips(p, out.data(), 2, BLOCK, true, pos, st, nullptr, SR, buffer == 0);
        for (float v : out) exact = exact && v == (float)want;
    }
    CHECK(exact);

    // the same from another thread at once: the "audio thread" plays while this one flips solos as fast as it can
    std::atomic<bool> stop{false};
    std::atomic<int> buffers{0};
    std::atomic<bool> sane{true};
    std::thread audio([&] {
        std::vector<float> o((size_t)BLOCK * 2);
        int64_t at = 1000;
        double most = 0;
        for (int k = 0; k < N; k++) most += (k + 1) / 1024.0 * (k == 0 ? 2 : 1);
        while (!stop.load()) {
            std::fill(o.begin(), o.end(), 0.0f);
            mixer::mixClips(p, o.data(), 2, BLOCK, true, at, st, nullptr, SR);
            for (float v : o)
                if (!std::isfinite(v) || v < 0 || v > most + 1e-3) sane = false;
            at += BLOCK;
            if (at > SR * 9) at = 1000;
            buffers++;
        }
    });
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    int flips = 0;
    while (std::chrono::steady_clock::now() < until) {
        for (auto& t : ts) {
            t->soloed = !t->soloed;
            if (flips % 3 == 0) t->soloSafe = !t->soloSafe;
        }
        if (flips % 5 == 0) p.soloOnly(ts[(size_t)(flips % N)]);
        flips++;
    }
    stop = true;
    audio.join();
    util::debugf("test: %d buffers played while every solo flipped %d times", buffers.load(), flips);
    CHECK(sane.load() && buffers.load() > 50);
}

// --- automation in time behind an effect with 10,000 frames of look-ahead ---
static void testAutomationLatency() {
    const int LAT = 10000, BLOCK = 500;
    const int64_t T1 = 20000, T0 = 30000, TF = 60000;  // (each, and each + LAT, on a buffer's start)
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    auto fx1 = std::make_shared<FakeFx>(), fx2 = std::make_shared<FakeFx>();
    fx1->lat = LAT;
    SlotPtr s1 = slotOf(fx1), s2 = slotOf(fx2);
    t->effects = std::vector<SlotPtr>{s1, s2};
    t->cables = chain({s1, s2});
    p.clips = std::vector<ClipPtr>{constantClip(t, SR * 5, 1.0f)};
    double zeroDb = (0.0 - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB);
    double minus20 = (-20.0 - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB);
    t->automation = std::vector<AutomationPtr>{
        // the late effect's own gain (at its input: nothing late there): 0.5 → 1 at T1
        lane(s1, "Gain", {{0, 0.5}, {(double)T1 / SR, 0.5}, {(double)T1 / SR, 1.0}}),
        // the effect after it (its sound 10,000 frames late): 0.5 → 1 at T0
        lane(s2, "Gain", {{0, 0.5}, {(double)T0 / SR, 0.5}, {(double)T0 / SR, 1.0}}),
        // the fader, after both: 0 dB until TF, then down to -20 dB over a second
        lane(t->controls, automation::VOLUME, {{0, zeroDb}, {(double)TF / SR, zeroDb}, {(double)TF / SR + 1, minus20}})};
    automation::resolve(p);
    CHECK(plugin_audio::inputLatency(*t, s1.get(), *p.tracks) == 0);
    CHECK(plugin_audio::inputLatency(*t, s2.get(), *p.tracks) == LAT);
    mixer::MixState st;
    const int64_t total = TF + SR + LAT;
    std::vector<float> out((size_t)total * 2, 0.0f);
    int late = -1;
    for (int64_t pos = 0; pos < total; pos += BLOCK)
        late = mixer::mixClips(p, out.data() + pos * 2, 2, BLOCK, true, pos, st, nullptr, SR, pos == 0);
    CHECK(late == LAT);
    auto heard = [&](int64_t timeline) { return out[(size_t)(timeline + LAT) * 2]; };  // (the output is LAT late)
    // sample for sample: each change lands on its own point in time, not LAT early
    CHECK(heard(T1 - 1) == 0.25f && heard(T1) == 0.5f);
    CHECK(heard(T0 - 1) == 0.5f && heard(T0) == 1.0f);
    CHECK(heard(TF - 1) == 1.0f && heard(TF + BLOCK / 2) < 1.0f);
    CHECK_NEAR(heard(TF + SR / 2), dsp::dbToGain(-10.0, config::MIN_DB), 1e-4);  // (half way down, in dB)
    bool steady = true;  // between the changes: exactly the gains, every sample
    for (int64_t i = 2000; i < T1; i++) steady = steady && heard(i) == 0.25f;
    for (int64_t i = T1; i < T0; i++) steady = steady && heard(i) == 0.5f;
    for (int64_t i = T0; i < TF; i++) steady = steady && heard(i) == 1.0f;
    CHECK(steady);
    // Pan, too, follows the sound: hard left, then centred from TF (gliding there over the buffer before, as the
    // fader glides to each buffer's end value)
    t->automation = std::vector<AutomationPtr>{
        lane(t->controls, automation::PAN, {{0, 0.0}, {(double)TF / SR, 0.0}, {(double)TF / SR, 0.5}})};
    std::fill(out.begin(), out.end(), 0.0f);
    mixer::MixState st2;
    for (int64_t pos = 0; pos < total; pos += BLOCK)
        mixer::mixClips(p, out.data() + pos * 2, 2, BLOCK, true, pos, st2, nullptr, SR, pos == 0);
    auto right = [&](int64_t timeline) { return out[(size_t)(timeline + LAT) * 2 + 1]; };
    CHECK(std::fabs(right(TF - BLOCK - 1)) < 1e-6 && std::fabs(right(TF - BLOCK)) < 1e-6);
    CHECK(right(TF - BLOCK / 2) > 0.4f && right(TF - BLOCK / 2) < 0.6f && right(TF) == 1.0f);
    CHECK(out[(size_t)(TF + LAT) * 2] == 1.0f);
}

// --- true peak: 4x oversampled (ITU-R BS.1770-4) ---
static void testTruePeak() {
    const double PI = 3.14159265358979323846;
    // phase 0 is the sample itself; every phase passes DC unchanged
    for (auto& phase : audio::TruePeak::coefficients()) {
        double sum = 0;
        for (float c : phase) sum += c;
        CHECK_NEAR(sum, 1.0, 1e-6);
    }
    CHECK(audio::TruePeak::coefficients()[0][audio::TruePeak::TAPS / 2 - 1] == 1.0f);
    // a sine at a quarter of the rate, 45° off: every sample at ±0.707 (-3.01 dBFS), its peaks between them at 1.0
    std::vector<float> x((size_t)SR * 2);
    float samplePeak = 0;
    for (int i = 0; i < SR; i++) {
        // (faded in over 10 ms: a sine starting at full scale would overshoot between its first samples)
        float v = (float)(std::sin(PI / 2 * i + PI / 4) * (i < 480 ? 0.5 - 0.5 * std::cos(PI * i / 480) : 1.0));
        x[(size_t)i * 2] = x[(size_t)i * 2 + 1] = v;
        samplePeak = std::max(samplePeak, std::fabs(v));
    }
    CHECK_NEAR(20 * std::log10(samplePeak), -3.01, 0.01);
    audio::TruePeak tp(2);
    CHECK(std::isinf(tp.peakDb()));
    tp.add(x.data(), SR, 2);
    CHECK_NEAR(tp.peakDb(), 0.0, 0.05);
    // in buffers of every size (1 frame up): the same
    audio::TruePeak pieces(2);
    for (int at = 0, n = 1; at < SR; at += n, n = n % 97 + 1) pieces.add(x.data() + at * 2, std::min(n, SR - at), 2);
    CHECK_NEAR(pieces.peakDb(), tp.peakDb(), 1e-6);
    // a 997 Hz sine at -6 dBFS: true peak = its peak (within the interpolator's ripple)
    audio::TruePeak sine(1);
    std::vector<float> m(SR);
    for (int i = 0; i < SR; i++) m[(size_t)i] = 0.5f * (float)std::sin(2 * PI * 997 * i / SR);
    sine.add(m.data(), SR, 1);
    CHECK_NEAR(sine.peakDb(), -6.02, 0.1);
    // through the metering, as the master plays it: reset by Play (resetLoudness)
    audio::Metering mt(SR, 2);
    for (int at = 0; at < SR; at += 512) mt.tapInterleaved(audio::MASTER, x.data() + at * 2, 2, 0, std::min(512, SR - at));
    std::vector<float> peaks;
    std::map<const Track*, std::vector<float>> tracks, squares;
    mt.takePeaks(peaks, tracks, &squares);
    mt.updateLoudness();
    CHECK_NEAR(mt.truePeakDb(), 0.0, 0.05);
    CHECK_NEAR(20 * std::log10(peaks[0]), -3.01, 0.01);  // (the sample peak, for the bars)
    CHECK(squares.count(audio::MASTER) && std::fabs(squares[audio::MASTER][0] - 0.5f) < 5e-3);  // (sine: RMS² = ½; less its fade in)
    mt.resetLoudness();
    CHECK(std::isinf(mt.truePeakDb()));
    // the worst: full-scale noise in odd buffers, then silence: finite, and at least the sample peak
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> uni(-1, 1);
    std::vector<float> noise(4096 * 2);
    for (auto& v : noise) v = uni(rng);
    float np = 0;
    for (float v : noise) np = std::max(np, std::fabs(v));
    for (int at = 0, n = 7; at < 4096; at += n) mt.tapInterleaved(audio::MASTER, noise.data() + at * 2, 2, 0, std::min(n, 4096 - at));
    mt.tapSilence(audio::MASTER, 1000);
    mt.takePeaks(peaks, tracks);
    mt.updateLoudness();
    CHECK(std::isfinite(mt.truePeakDb()) && mt.truePeakDb() >= 20 * std::log10(np) - 1e-4);
}

// --- RMS: mean squares from the metering, averaged over RMS_S by the meter ---
static void testRms() {
    audio::Metering mt(SR, 4);
    Track t;
    std::vector<float> half((size_t)SR / 10 * 2, 0.5f);
    float* ch[2] = {half.data(), half.data() + SR / 10};
    mt.tap(&t, ch, 2, 2, SR / 10);  // on outputs 3/4
    mt.tapSilence(&t, SR / 10);     // then as long silent: the mean square halves
    std::vector<float> peaks;
    std::map<const Track*, std::vector<float>> tracks, squares;
    mt.takePeaks(peaks, tracks, &squares);
    CHECK(squares.count(&t) && squares[&t].size() == 4);
    CHECK(squares[&t][0] == 0 && std::fabs(squares[&t][2] - 0.125f) < 1e-6 && std::fabs(squares[&t][3] - 0.125f) < 1e-6);
    CHECK(!squares.count(audio::MASTER));  // (nothing heard there)
    LevelMeter m(4);
    m.showChannels({2, 3});
    level_meter::testNow = 100;
    std::vector<float> ms{0, 0, 0.25f, 0.25f};  // RMS 0.5
    m.pushRms(&ms);  // the first: at once
    CHECK_NEAR(m.rms()[0], 0.5, 1e-9);
    level_meter::testNow = 100 + level_meter::RMS_S;  // silence for RMS_S: down to e^-1 of the power
    m.pushRms(nullptr);
    CHECK_NEAR(m.rms()[1], 0.5 * std::sqrt(std::exp(-1.0)), 1e-6);
    for (int i = 0; i < 200; i++) {  // settles to nothing (stops repainting)
        level_meter::testNow += 0.05;
        m.pushRms(nullptr);
    }
    CHECK(m.rms()[0] == 0 && m.rms()[1] == 0);
    level_meter::testNow = -1;
}

// --- solo safe: saved, loaded (old files without it: off), undone ---
static void testSoloSafeSaved() {
    util::makeDirs(TMP);
    Project p;
    TrackPtr a = p.addTrack(Kind::Audio), b = p.addTrack(Kind::Audio);
    history::History h(p, SR);
    a->soloSafe = true;
    CHECK(h.check(SR));
    CHECK(h.labels().back() == "Solo safe");
    CHECK(h.undo(SR) && !a->soloSafe);
    CHECK(h.redo(SR) && a->soloSafe);
    project_io::MasterSettings master, back;
    std::string err, path = TMP + "/solo.json";
    CHECK(project_io::saveProject(path, p, SR, master, err));
    Project q;
    CHECK(project_io::loadProject(path, q, SR, back, err));
    CHECK(q.tracks.size() == 2 && q.tracks[0]->soloSafe && !q.tracks[1]->soloSafe);
    std::ofstream(TMP + "/old.json") << R"({"format": "tinydaw-project", "tracks": [{"name": "Old", "kind": "audio",
        "soloed": true}], "clips": []})";
    Project o;
    CHECK(project_io::loadProject(TMP + "/old.json", o, SR, back, err));
    CHECK(o.tracks.size() == 1 && o.tracks[0]->soloed && !o.tracks[0]->soloSafe);
}

// --- the window's S buttons and the terminal's keys ---
namespace {
template <class T>
std::vector<T*> findAll(ui::Widget* root) {
    std::vector<T*> out;
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            if (auto* t = dynamic_cast<T*>(c.get())) out.push_back(t);
            walk(c.get());
        }
    };
    walk(root);
    return out;
}
SoloButton* soloOf(MainWindow& w, const TrackPtr& t) {
    auto found = findAll<SoloButton>(w.panel->frames[t]);
    return found.size() == 1 ? found[0] : nullptr;
}
void click(ui::Widget* b, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point(5, 5);
    e.button = plat::LeftButton;
    e.mods = mods;
    b->mousePress(e);
    b->mouseRelease(e);
}
void settle() {
    for (int i = 0; i < 6; i++) {
        ui::App::get().advanceTimers(0.05);
        ui::App::get().tick();
    }
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
    settle();
    CHECK(w->project.tracks.size() == 3);
    // a click solos (additive), Alt-click solos one alone
    click(soloOf(*w, a));
    click(soloOf(*w, b));
    CHECK(a->soloed && b->soloed && !c->soloed);
    click(soloOf(*w, c), plat::ModAlt);
    settle();  // (the rows built again)
    CHECK(!a->soloed && !b->soloed && c->soloed);
    CHECK(soloOf(*w, a) && !soloOf(*w, a)->isChecked() && soloOf(*w, c)->isChecked());
    click(soloOf(*w, c), plat::ModAlt);  // Alt-click on the one soloed: it stays the only one
    settle();
    CHECK(c->soloed && soloOf(*w, c)->isChecked());
    // right-click: Solo safe (a lock on its S), undoable
    ui::Menu menu;
    soloOf(*w, a)->fillMenu(menu);
    ui::Action* safe = nullptr;
    ui::Action* only = nullptr;
    for (auto& owned : menu.actions()) {
        ui::Action* act = owned.get();
        if (act->text == "Solo safe") safe = act;
        if (act->text == "Solo only this track") only = act;
    }
    CHECK(safe && only && safe->checkable && !safe->checked);
    if (safe) ui::Menu::activate(safe);
    CHECK(a->soloSafe && w->project.audible(*a) && !w->project.audible(*b));
    CHECK(soloOf(*w, a)->toolTip().find("Solo safe") != std::string::npos);
    settle();
    CHECK(w->history && !w->history->labels().empty() && w->history->labels().back() == "Solo safe");
    if (only) ui::Menu::activate(only);  // exclusive solo from the menu
    settle();
    CHECK(a->soloed && !c->soloed);

    // the master strip: the true peak next to the LUFS
    CHECK(w->master->truePeak->text() == "TP\n–");
    w->master->showTruePeak(-0.34);
    CHECK(w->master->truePeak->text() == "TP\n-0.3" && w->master->truePeak->toolTip().find("dBTP") != std::string::npos);

    // the terminal: S solo, Shift+S solo safe; the master row's true peak in dBTP
    tui::Front f(*w);
    tui::Screen screen(100, 30);
    f.pane = tui::Front::Pane::Panel;
    f.laneRow = 1;  // b
    f.key(tui::Key{'S', 0, "s"});
    CHECK(b->soloed);
    f.key(tui::Key{'S', plat::ModShift, "S"});
    CHECK(b->soloSafe && b->soloed);
    f.key(tui::Key{'S', 0, "s"});
    CHECK(!b->soloed && b->soloSafe);
    f.key(tui::Key{'S', plat::ModShift, "S"});
    CHECK(!b->soloSafe);
    f.laneRow = 2;  // c: solo safe shows as an s
    f.key(tui::Key{'S', plat::ModShift, "S"});
    settle();
    f.draw(screen);
    std::string all = screen.lines();
    CHECK(all.find("dBTP") != std::string::npos);
    std::ofstream(TMP + "/panel.txt") << all;
    // F1 lists the key
    f.key(tui::Key{plat::Key_F1, 0, ""});
    std::string seen;  // (taller than 30 rows: scrolled through with PgDn)
    for (int i = 0; i < 6; i++) {
        f.draw(screen);
        seen += screen.lines();
        f.key(tui::Key{plat::Key_PageDown, 0, ""});
    }
    CHECK(seen.find("solo safe") != std::string::npos && f.helpShown);
    delete w;
    app.paintWindows = true;
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testSoloRules();
    testSolo256();
    testAutomationLatency();
    testTruePeak();
    testRms();
    testSoloSafeSaved();
    testWindowAndTerminal();
    TEST_MAIN_END();
}
