// The Drawn synth (Patch::drawn, ui/wave_canvas.h): its sound (the drawn cycle at each key's pitch, band-limited:
// no partial folds back; its DC taken away), its files, then its window headless (Instrument ▾ → Drawn synth…,
// drawing with the mouse and the keys, undo, back to the built-in synth; every skin pictured) and in the
// terminal; and the worst case: every key at once on the harshest drawings, many tracks drawn on at once.
// Pictures go to build/ui-samples/drawn-synth/.
#include <cmath>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "audio/fft.h"
#include "audio/render.h"
#include "audio/synth_patch.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "paths.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/instrument_menu.h"
#include "ui/main_window.h"
#include "ui/plugin_host.h"
#include "ui/sampler_dialog.h"
#include "ui/skin.h"
#include "ui/synth_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/wave_canvas.h"

using namespace subsynth;

namespace {
const int SR = 48000;
const std::string OUT = "build/ui-samples/drawn-synth";
const std::string TMP = "build/test-tmp/drawn-synth";

Patch drawnOf(std::vector<float> wave) {
    Patch p = drawnPatch();
    p.drawn = std::make_shared<const std::vector<float>>(fitWave(wave));
    return p;
}

// the left channel's power spectrum of n frames from `at` (Hann window); bin k: k * SR / n Hz
std::vector<double> spectrum(const AudioBuffer& a, int64_t at, int n) {
    std::vector<fft::cd> x((size_t)n);
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);
        x[(size_t)i] = a.frame(at + i)[0] * w;
    }
    fft::Plan(n).forward(x.data());
    std::vector<double> p((size_t)n / 2);
    for (int k = 0; k < n / 2; k++) p[(size_t)k] = std::norm(x[(size_t)k]);
    return p;
}

// the share of the power within a few bins of a harmonic of f0 (1: nothing folded back)
double harmonicShare(const std::vector<double>& p, double f0, int n) {
    double all = 0, near = 0;
    for (size_t k = 1; k < p.size(); k++) {
        double f = (double)k * SR / n;
        double h = std::round(f / f0);
        all += p[k];
        if (h >= 1 && std::fabs(f - h * f0) <= 4.0 * SR / n) near += p[k];
    }
    return all > 0 ? near / all : 1;
}

double peakOf(const AudioBuffer& a) {
    double m = 0;
    for (float v : a.samples) m = std::max(m, (double)std::fabs(v));
    return m;
}

bool finite(const AudioBuffer& a) {
    for (float v : a.samples)
        if (!std::isfinite(v)) return false;
    return true;
}

std::vector<float> square() {
    std::vector<float> w((size_t)WAVE_POINTS);
    for (int i = 0; i < WAVE_POINTS; i++) w[(size_t)i] = i < WAVE_POINTS / 2 ? 1.0f : -1.0f;
    return w;
}
}  // namespace

// --- the sound ---

static void testSound() {
    // the starting sine: at the key's pitch (A4: 440 Hz), nothing else
    AudioPtr a = subsynth::render(drawnPatch(), {{0.0, 1.0, 69, 127}}, SR, 1.0);
    CHECK(a && finite(*a));
    if (!a) return;
    const int N = 8192;
    std::vector<double> p = spectrum(*a, SR / 4, N);
    size_t top = (size_t)(std::max_element(p.begin(), p.end()) - p.begin());
    CHECK_NEAR((double)top * SR / N, 440.0, 2.0 * SR / N);
    CHECK(harmonicShare(p, 440, N) > 0.999);
    CHECK(peakOf(*a) > 0.05);

    // a square drawn, played high (C7, 2093 Hz): only its own harmonics, none folded back past Nyquist
    double f0 = 440.0 * std::pow(2.0, (96 - 69) / 12.0);
    AudioPtr sq = subsynth::render(drawnOf(square()), {{0.0, 1.0, 96, 127}}, SR, 1.0);
    CHECK(sq && finite(*sq));
    if (sq) {
        double share = harmonicShare(spectrum(*sq, SR / 4, N), f0, N);
        printf("   a drawn square at %.0f Hz: %.4f of its power on its harmonics\n", f0, share);
        CHECK(share > 0.995);
    }
    // THE LEVEL DRAWN IS THE LEVEL HEARD: drawn to the very top, a note at full velocity peaks at 0 dBFS (sine,
    // square, a drawing all above the centre line whose DC is taken away); half as high, -6 dB; at any pitch
    auto peakDb = [](const AudioPtr& out) { return out ? 20 * std::log10(std::max(1e-9, peakOf(*out))) : -999.0; };
    std::vector<float> upper((size_t)WAVE_POINTS, 0.0f);
    for (int i = WAVE_POINTS / 4; i < WAVE_POINTS / 2; i++) upper[(size_t)i] = 1.0f;
    std::vector<float> half = square();
    for (float& v : half) v *= 0.5f;
    for (int pitch : {36, 60, 84}) {
        double sineDb = peakDb(subsynth::render(drawnPatch(), {{0.0, 1.0, (double)pitch, 127}}, SR, 1.0));
        double squareDb = peakDb(subsynth::render(drawnOf(square()), {{0.0, 1.0, (double)pitch, 127}}, SR, 1.0));
        double upperDb = peakDb(subsynth::render(drawnOf(upper), {{0.0, 1.0, (double)pitch, 127}}, SR, 1.0));
        double halfDb = peakDb(subsynth::render(drawnOf(half), {{0.0, 1.0, (double)pitch, 127}}, SR, 1.0));
        printf("   key %d at full velocity: sine %.2f dB, square %.2f dB, above the centre %.2f dB, half %.2f dB\n", pitch,
               sineDb, squareDb, upperDb, halfDb);
        CHECK(std::fabs(sineDb) < 0.1);
        CHECK(std::fabs(squareDb) < 0.1);
        CHECK(std::fabs(upperDb) < 0.1);
        CHECK(std::fabs(halfDb + 6.02) < 0.1);
    }
    // and through the whole mix (a track at 0 dB, the master at 0 dB): its top reaches the master at 0 dBFS
    {
        Project pr;
        TrackPtr t = pr.addTrack(Kind::Midi);
        t->needsInstrument = false;
        std::atomic_store(&t->synth, PatchPtr(std::make_shared<const Patch>(drawnOf(square()))));
        ClipPtr c = makeMidiClip("Held", {{0.0, 1.0, 60, 127}}, 1.0, SR);
        c->track = t;
        pr.addClip(c);
        c->prepare(SR);
        auto mix = ::render::renderMix(pr, 0, SR, SR);
        CHECK(mix.has_value());
        if (mix) {
            double m = 0;
            for (float v : *mix) m = std::max(m, (double)std::fabs(v));
            printf("   through the mix: %.2f dBFS on the master\n", 20 * std::log10(std::max(1e-9, m)));
            CHECK(std::fabs(20 * std::log10(std::max(1e-9, m))) < 0.1);
        }
    }
    // velocity scales it, as on every instrument (64 of 127: about -6 dB)
    CHECK(std::fabs(peakDb(subsynth::render(drawnPatch(), {{0.0, 1.0, 60, 64}}, SR, 1.0)) - 20 * std::log10(64 / 127.0)) < 0.1);

    // a drawing off centre: its DC taken away (a flat line is silence)
    AudioPtr flat = subsynth::render(drawnOf(std::vector<float>((size_t)WAVE_POINTS, 0.6f)), {{0.0, 0.5, 60, 127}}, SR, 0.5);
    CHECK(flat && peakOf(*flat) < 1e-4);

    // the same notes render the same, bit for bit
    AudioPtr again = subsynth::render(drawnOf(square()), {{0.0, 1.0, 96, 127}}, SR, 1.0);
    CHECK(sq && again && sq->samples == again->samples);
}

// --- its files (projects and presets: toJson / fromJson) ---

static void testFiles() {
    Patch p = drawnOf(square());
    Patch back = fromJson(toJson(p));
    CHECK(back.drawn && back.drawn->size() == (size_t)WAVE_POINTS);
    CHECK(back == p);  // (5 decimals kept: a drawing's values come back as they were)
    CHECK(fromJson(toJson(Patch())).drawn == nullptr);  // (an ordinary patch stays one)
    CHECK(toJson(Patch()).has("drawn") == false);
    // another wave: not the same patch (so undo sees a drawing as a change)
    Patch other = drawnOf(std::vector<float>{0.0f, 1.0f});
    CHECK(!(other == p));
    // a file's wave of any length is read as one cycle; garbage in it: 0; values past ±1: clamped
    Json j = toJson(Patch());
    Json wave = Json::array();
    wave.push(Json(-5.0));
    wave.push(Json("x"));
    wave.push(Json(5.0));
    j.set("drawn", wave);
    Patch odd = fromJson(j);
    CHECK(odd.drawn && odd.drawn->size() == (size_t)WAVE_POINTS);
    if (odd.drawn) {
        CHECK((*odd.drawn)[0] == -1.0f);
        for (float v : *odd.drawn) CHECK(std::isfinite(v) && v >= -1.0f && v <= 1.0f);
    }
    // a wave of one point (or none) isn't a drawing
    Json one = toJson(Patch());
    Json single = Json::array();
    single.push(Json(0.5));
    one.set("drawn", single);
    CHECK(fromJson(one).drawn == nullptr);
}

// --- the window ---

namespace {
ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}
ui::MouseEvent mouse(Point at) {
    ui::MouseEvent e;
    e.pos = at;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    return e;
}
void stroke(WaveCanvas* c, Point a, Point b, int moves = 1) {
    ui::MouseEvent e = mouse(a);
    c->mousePress(e);
    for (int i = 1; i <= moves; i++) {
        ui::MouseEvent m = mouse(Point(a.x + (b.x - a.x) * i / moves, a.y + (b.y - a.y) * i / moves));
        c->mouseMove(m);
    }
    ui::MouseEvent r = mouse(b);
    c->mouseRelease(r);
}
void savePicture(ui::Window& w, const std::string& name) {
    w.showWindow();
    ui::App::get().doLayouts();
    w.update();
    ui::App::get().tick();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
}
struct Setup {
    Project project;
    audio::Engine engine{project};
    ui::Window main;
    std::unique_ptr<PluginHost> host;
    Setup() {
        main.resizeWindow(1000, 700);
        sampler_dialog::Host h;
        h.window = &main;
        h.project = &project;
        h.engine = &engine;
        sampler_dialog::setHost(h);
        util::makeDirs(TMP + "/plugins");
        host = std::make_unique<PluginHost>(&main, project, engine);
        host->folders = {TMP + "/plugins"};
        host->cachePath = util::joinPath(TMP, "plugin_cache.json");
        host->vst2Folders = [] { return std::vector<std::string>{}; };
    }
    TrackPtr midiTrack(const std::string& name, ClipPtr* clipOut = nullptr) {
        TrackPtr t = project.addTrack(Kind::Midi);
        t->name = name;
        t->needsInstrument = false;
        ClipPtr clip = makeMidiClip("Tune", {{0.0, 0.5, 60, 100}, {0.5, 1.0, 72, 100}}, 1.0, SR);
        clip->track = t;
        project.addClip(clip);
        clip->prepare(SR);
        if (clipOut) *clipOut = clip;
        return t;
    }
    bool choose(const TrackPtr& t, const std::string& entry) {
        ui::Menu menu;
        instrument_menu::fillInstrumentMenu(&menu, &main, *host, t, nullptr, nullptr);
        ui::Action* a = actionStarting(menu, entry);
        if (!a) return false;
        ui::Menu::activate(a);
        return true;
    }
};
}  // namespace

static void testWindow() {
    Setup s;
    ClipPtr clip;
    TrackPtr track = s.midiTrack("Lead", &clip);
    history::History hist(s.project, SR);
    AudioPtr classic = clip->data;

    // Instrument ▾ → Drawn synth…: the track plays a sine to draw over; its window is the canvas and Close
    CHECK(s.choose(track, instrument_menu::DRAWN));
    CHECK(track->synth && track->synth->drawn && *track->synth->drawn == *drawnPatch().drawn);
    CHECK(clip->data != classic);
    CHECK(hist.check(SR));
    synth_dialog::SynthDialog* d = synth_dialog::openFor(track.get());
    CHECK(d != nullptr);
    if (!d) return;
    ui::App::get().doLayouts();
    CHECK(d->title() == "Drawn synth \xe2\x80\x94 Lead");
    CHECK(d->wave->isVisible() && !d->page->isVisible() && !d->modPanel->isVisible());
    for (ui::Widget* w : {(ui::Widget*)d->presetsButton, (ui::Widget*)d->saveButton, (ui::Widget*)d->modulateButton,
                          (ui::Widget*)d->synthView, (ui::Widget*)d->modView, (ui::Widget*)d->playButton,
                          (ui::Widget*)d->classicNote})
        CHECK(!w->isVisible());
    CHECK(d->wave->width() >= 400 && d->wave->height() >= 200);
    CHECK(d->wave->wave == *track->synth->drawn);
    // the menu ticks it, not the built-in synth (and offers no Synth… for it)
    {
        ui::Menu menu;
        instrument_menu::fillInstrumentMenu(&menu, &s.main, *s.host, track, nullptr, nullptr);
        ui::Action* drawnAction = actionStarting(menu, instrument_menu::DRAWN);
        ui::Action* builtIn = actionStarting(menu, instrument_menu::BUILTIN);
        CHECK(drawnAction && drawnAction->checked && builtIn && !builtIn->checked);
        CHECK(!actionStarting(menu, instrument_menu::SYNTH));
    }
    savePicture(*d, "sine");

    // a stroke across it in one quick move: every point between drawn, along the line (no gaps)
    WaveCanvas* c = d->wave;
    Rect a = c->area();
    AudioPtr sine = clip->data;
    stroke(c, Point(a.x + 2, a.y + 2), Point(a.right() - 2, a.bottom() - 2));
    const std::vector<float>& w = *track->synth->drawn;
    CHECK(w.size() == (size_t)WAVE_POINTS);
    CHECK(w.front() > 0.95f && w.back() < -0.95f);
    bool down = true;
    for (size_t i = 1; i < w.size(); i++) down = down && w[i] <= w[i - 1] + 1e-6f;
    CHECK(down);
    CHECK(clip->data != sine && clip->data->samples != sine->samples);  // (rendered again at the release)
    CHECK(hist.check(SR));
    savePicture(*d, "ramp");
    // the keyboard: Home, Right ×3, Up: that point a 32nd higher; Shift+Down a 256th lower
    ui::KeyEvent k;
    for (int key : {plat::Key_Home, plat::Key_Right, plat::Key_Right, plat::Key_Right}) {
        k.key = key;
        CHECK(c->keyPress(k));
    }
    CHECK(c->cursor == 3);
    float was = c->wave[3];
    k.key = plat::Key_Up;
    c->keyPress(k);
    k.key = plat::Key_Down;
    k.mods = plat::ModShift;
    c->keyPress(k);
    CHECK_NEAR(track->synth->drawn->at(3), std::min(1.0, was + 1.0 / 32) - 1.0 / 256, 1e-6);
    d->done(0);  // (closing renders what the keys changed)
    ui::App::get().collectGarbage();
    CHECK(hist.check(SR));

    // undo: the ramp (then the sine) back, shown again on a window opened anew
    CHECK(hist.undo(SR));
    CHECK(track->synth && track->synth->drawn && track->synth->drawn->front() > 0.95f);
    CHECK(hist.undo(SR));
    CHECK(track->synth && *track->synth->drawn == *drawnPatch().drawn);
    d = synth_dialog::openSynth(track);
    CHECK(d && d->wave->isVisible() && d->wave->wave == *drawnPatch().drawn);
    CHECK(hist.redo(SR) && hist.redo(SR));
    synth_dialog::reloadAll();
    CHECK(d && d->wave->wave == *track->synth->drawn);

    // the built-in synth again: the classic voice, the window its knobs (the canvas gone)
    CHECK(s.choose(track, instrument_menu::BUILTIN));
    CHECK(!track->synth && clip->data->samples == classic->samples);
    if (d) CHECK(d->page->isVisible() && !d->wave->isVisible() && d->title() == "Synth \xe2\x80\x94 Lead");
    // and the Drawn synth once more: a fresh sine, the canvas again
    CHECK(s.choose(track, instrument_menu::DRAWN));
    if (d) CHECK(d->wave->isVisible() && !d->page->isVisible());

    // every skin: the canvas drawn in its colours, inside the window
    skin::setBuiltinDir("data/skins");
    skin::setUserDir(TMP + "/skins");
    for (const skin::Skin& sk : skin::library()) {
        CHECK(skin::apply(sk).empty());
        synth_dialog::SynthDialog* e = synth_dialog::openSynth(track);
        if (!e) continue;
        e->wave->wave = square();
        ui::App::get().doLayouts();
        Rect g = e->wave->geometry();
        CHECK(g.right() <= e->width() && g.bottom() <= e->height());
        savePicture(*e, "skin-" + sk.name);
        e->done(0);
        ui::App::get().collectGarbage();
    }
    if (!skin::library().empty()) skin::apply(skin::library().front());
    synth_dialog::closeGone(s.project);
    if (auto* left = synth_dialog::openFor(track.get())) left->done(0);
    ui::App::get().collectGarbage();
}

// --- the worst case ---

static void testWorstCase() {
    double t0 = plat::now();
    // every key at once (0…127, full velocity, overlapping) on the harshest drawings: finite, bounded
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-1, 1);
    std::vector<float> noise((size_t)WAVE_POINTS);
    for (float& v : noise) v = any(rng);
    std::vector<float> spikes((size_t)WAVE_POINTS, -1.0f);
    for (int i = 0; i < WAVE_POINTS; i += 16) spikes[(size_t)i] = 1.0f;
    std::vector<Note> all;
    for (int pitch = 0; pitch < 128; pitch++) all.push_back({pitch * 0.002, 0.6, (double)pitch, 127});
    for (const auto& wave : {square(), noise, spikes}) {
        AudioPtr out = subsynth::render(drawnOf(wave), all, SR, 0.7);
        CHECK(out && finite(*out));
        if (out) CHECK(peakOf(*out) < 64);
    }
    // 24 tracks, their windows open, each drawn on in 200 quick moves (a scribble), the clips rendering
    Setup s;
    std::vector<TrackPtr> tracks;
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 24; i++) {
        ClipPtr c;
        tracks.push_back(s.midiTrack("T" + std::to_string(i), &c));
        clips.push_back(c);
        CHECK(s.choose(tracks.back(), instrument_menu::DRAWN));
    }
    history::History hist(s.project, SR);
    ui::App::get().doLayouts();
    for (int round = 0; round < 3; round++)
        for (size_t i = 0; i < tracks.size(); i++) {
            synth_dialog::SynthDialog* d = synth_dialog::openFor(tracks[i].get());
            CHECK(d != nullptr);
            if (!d) continue;
            Rect a = d->wave->area();
            ui::MouseEvent e = mouse(Point(a.x, a.y));
            d->wave->mousePress(e);
            for (int m = 0; m < 200; m++) {  // (back and forth, past the edges too)
                ui::MouseEvent mv = mouse(Point(a.x - 20 + (int)(rng() % (unsigned)(a.w + 40)),
                                                a.y - 20 + (int)(rng() % (unsigned)(a.h + 40))));
                d->wave->mouseMove(mv);
            }
            ui::MouseEvent r = mouse(Point(a.x + a.w / 2, a.y + a.h / 2));
            d->wave->mouseRelease(r);
            ui::App::get().advanceTimers(0.2);
            ui::App::get().tick();
        }
    bool sane = true;
    for (size_t i = 0; i < tracks.size(); i++) {
        auto w = tracks[i]->synth ? tracks[i]->synth->drawn : nullptr;
        sane = sane && w && w->size() == (size_t)WAVE_POINTS && clips[i]->data && finite(*clips[i]->data);
        if (w)
            for (float v : *w) sane = sane && std::isfinite(v) && v >= -1 && v <= 1;
    }
    CHECK(sane);
    CHECK(hist.check(SR));
    for (auto& t : tracks)
        if (auto* d = synth_dialog::openFor(t.get())) d->done(0);
    ui::App::get().collectGarbage();
    printf("   worst case (128 keys × 3 drawings; 24 tracks × 3 scribbles of 200 moves): %.2f s\n", plat::now() - t0);
}

// --- the terminal ---

namespace {
tui::Key key(int k, int mods = 0, std::string text = "") { return tui::Key{k, mods, std::move(text)}; }
struct Driver {
    tui::Front* f;
    std::vector<tui::Key> queue;
    size_t at = 0;
    tui::Screen screen{120, 40};
    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    void keys(std::vector<tui::Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    void picture(const std::string& name) {
        f->draw(screen);
        std::ofstream(OUT + "/" + name + ".txt") << screen.lines();
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
};
}  // namespace

static void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    d.keys({key(plat::Key_Tab), key('N', 0, "n")});  // (a MIDI track: the track panel's N)
    TrackPtr midi = w->timeline->currentTrack;
    CHECK(midi && midi->kind == Kind::Midi);
    if (!midi) return;
    instrument_menu::useDrawn(w, *w->plugins, midi, nullptr);
    synth_dialog::SynthDialog* dlg = synth_dialog::openFor(midi.get());
    CHECK(dlg != nullptr);
    if (!dlg) return;
    dlg->wave->setFocus();
    d.keys({});
    CHECK(d.shows("Drawn synth") && d.shows("\xe2\x97\x8f"));  // (its title; the wave's points: ●)
    CHECK(!d.shows("Presets") && !d.shows("Osc 1"));
    CHECK(d.shows("Left/Right: a point") && !d.shows("M: Modulate"));  // (the bottom line: the canvas's keys)
    d.picture("terminal");
    // the keys draw: End, Up ×8: the last point a quarter higher
    float was = midi->synth->drawn->back();
    std::vector<tui::Key> ks{key(plat::Key_End)};
    for (int i = 0; i < 8; i++) ks.push_back(key(plat::Key_Up));
    d.keys(ks);
    CHECK_NEAR(midi->synth->drawn->back(), std::min(1.0, was + 0.25), 1e-6);
    // the F1 list names its keys
    d.keys({key(plat::Key_F12), key(plat::Key_F1)});
    std::string seen;
    for (int i = 0; i < 12; i++) {
        seen += d.screen.lines();
        d.keys({key(plat::Key_PageDown)});
    }
    CHECK(seen.find("The Drawn synth") != std::string::npos);
    d.keys({key(plat::Key_Escape), key(plat::Key_F12), key('W', plat::ModCtrl)});
    w->close();
    app.collectGarbage();
    app.paintWindows = true;
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    util::makeDirs(TMP);
    settings::setPath(util::joinPath(TMP, "settings.json"));
    paths::setDataDir(util::absolutePath(TMP + "/data"));
    testSound();
    testFiles();
    testWindow();
    testWorstCase();
    testTerminal();
    TEST_MAIN_END();
}
