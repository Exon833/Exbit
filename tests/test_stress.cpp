// Worst-case runs (CLAUDE.md, rule 8): each aimed at one thing, done as hard as a user (or a slow machine)
// could do it at once, checking that the program stays whole and its sound stays sound:
//   - testOverloadedEngine: 256 tracks (audio with volume and pan automation, MIDI with dense notes) played in
//     small blocks on a thread of its own, while the GUI thread changes every fader, pan, mute and solo, adds
//     tracks and removes them, the whole time
//   - testResizeStorm: the main window resized 400 times, down to 1 x 1 and up to 4000 x 2500, with the
//     interface size changing under it
//   - testEverythingAtOnce: every slider and every toggle of the main window used in every frame, while it
//     plays and a drag is held across the timeline (40 frames); then everything undone and redone
//   - testTerminalStorm: the terminal UI hit with thousands of keys (menus, dialogs, deleting, playing) while
//     its screen is resized, down to 1 x 1
// The random choices come from a fixed seed: a failure happens again the same way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/synth.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "ui/main_window.h"
#include "ui/timeline.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace {
constexpr int SR = 48000;
const std::string TMP = "build/test-stress";
std::mt19937 rng(20261120);
int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

ClipPtr audioClip(const TrackPtr& t, int64_t start, int64_t frames, float value) {
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

// `count` short notes spread over `seconds`, across five octaves, all at full velocity
ClipPtr midiClip(const TrackPtr& t, int count, double seconds) {
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->name = "notes";
    c->track = t;
    c->lengthS = seconds;
    std::vector<Note> notes;
    for (int i = 0; i < count; i++) {
        Note n;
        n.start = seconds * i / count;
        n.end = n.start + 0.05;
        n.pitch = 36 + i % 60;
        n.velocity = 127;
        notes.push_back(n);
    }
    c->notes = std::make_shared<const std::vector<Note>>(std::move(notes));
    c->prepare(SR);
    return c;
}

// a lane of `points` random points for a track's volume or pan
void automate(const TrackPtr& t, const std::string& param, int points, double seconds) {
    auto lane = std::make_shared<Automation>();
    lane->slot = t->controls;
    lane->param = param;
    std::vector<AutoPoint> pts;
    for (int i = 0; i < points; i++) pts.push_back({seconds * i / points, pick(0, 1000) / 1000.0});
    lane->setPoints(pts);
    auto lanes = *t->automation.get();
    lanes.push_back(lane);
    t->automation = lanes;
}

void walk(ui::Widget* w, const std::function<void(ui::Widget*)>& f) {
    f(w);
    for (auto& c : w->children()) walk(c.get(), f);
}

// a main window with `tracks` tracks (audio and MIDI by turns), each with a clip
MainWindow* filledWindow(int tracks) {
    auto* w = new MainWindow();
    w->askBeforeClosing = false;
    w->resizeWindow(1280, 800);
    w->showWindow();
    for (int i = 0; i < tracks; i++) {
        bool midi = i % 2;
        TrackPtr t = w->timeline->addTrack(midi ? Kind::Midi : Kind::Audio);
        if (midi) {
            t->needsInstrument = false;  // (the built-in synth)
            auto c = midiClip(t, 200, 4.0);
            c->start = pick(0, SR * 2);
            w->project.addClip(c);
        } else {
            w->project.addClip(audioClip(t, pick(0, SR * 2), SR * 4, 0.25f));
        }
    }
    w->timeline->relayout();
    return w;
}

// (headless) the frames of `seconds`: timers, then a frame
void frames(double seconds) {
    for (double t = 0; t < seconds; t += 0.02) {
        ui::App::get().advanceTimers(0.02);
        ui::App::get().tick();
    }
}

// dialogs opened meanwhile are closed at once (as Escape would), so the storm goes on
void closeDialogsAtOnce() {
    ui::App::get().nestedLoop = [](const std::function<bool()>& done) {
        for (int i = 0; i < 50 && !done(); i++) {
            const auto& ws = ui::App::get().windows();
            for (auto it = ws.rbegin(); it != ws.rend(); ++it)
                if ((*it)->isWindowShown() && !dynamic_cast<MainWindow*>(*it)) {
                    (*it)->close();
                    break;
                }
            ui::App::get().tick();
        }
    };
}

void closeOtherWindows(MainWindow* main) {
    for (int i = 0; i < 100; i++) {
        ui::Window* top = nullptr;
        for (ui::Window* w : ui::App::get().windows())
            if (w != main && w->isWindowShown()) top = w;
        if (!top) break;
        top->close();
        ui::App::get().tick();
    }
}
}  // namespace

static void testOverloadedEngine() {
    Project p;
    audio::Engine e(p);  // (sets the MIDI clips' renderer: before they're made)
    e.offline = true;
    const int TRACKS = 256, BLOCK = 64;
    const int64_t LENGTH = SR * 4;
    for (int i = 0; i < TRACKS; i++) {
        if (i % 2 == 0) {
            auto t = p.addTrack(Kind::Audio);
            p.addClip(audioClip(t, pick(0, SR), LENGTH, 0.5f));
            automate(t, automation::VOLUME, 64, 4.0);
            automate(t, automation::PAN, 64, 4.0);
        } else {
            auto t = p.addTrack(Kind::Midi);
            t->needsInstrument = false;
            p.addClip(midiClip(t, 2000, 4.0));
        }
    }
    // the audio thread: 4 s in 64-frame blocks (3000 callbacks), as fast as it can
    std::atomic<bool> done{false};
    bool finite = true;
    float peak = 0;
    double slowest = 0;
    e.play();
    std::thread audio([&] {
        std::vector<float> out((size_t)BLOCK * 2);
        for (int64_t f = 0; f < LENGTH; f += BLOCK) {
            auto t0 = std::chrono::steady_clock::now();
            e.callback(nullptr, out.data(), BLOCK);
            slowest = std::max(slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            for (float v : out) {
                if (!std::isfinite(v)) finite = false;
                peak = std::max(peak, std::fabs(v));
            }
        }
        done = true;
    });
    // the GUI thread meanwhile: every control of every track, tracks added and removed
    int rounds = 0, added = 0, removed = 0;
    while (!done) {
        auto tracks = p.tracks.get();
        for (auto& t : *tracks) {
            t->muted = pick(0, 3) == 0;
            t->soloed = pick(0, 15) == 0;
            t->volumeDb = pick((int)config::MIN_DB, (int)config::MAX_DB);
            t->pan = pick(-100, 100) / 100.0;
        }
        e.masterGain = pick(0, 100) / 100.0;
        e.masterMute = pick(0, 9) == 0;
        if (rounds % 2 == 0) {
            auto t = p.addTrack(Kind::Audio);
            p.addClip(audioClip(t, 0, SR, 0.5f));
            added++;
        } else if (tracks->size() > 1) {
            p.removeTrack((*tracks)[(size_t)pick(0, (int)tracks->size() - 1)]);
            removed++;
        }
        e.idle();
        rounds++;
    }
    audio.join();
    printf("overload: %d tracks, %d rounds of changes (%d tracks added, %d removed); slowest block %.2f ms "
           "(%.2f ms of audio), peak %.2f\n",
           TRACKS, rounds, added, removed, slowest * 1000, BLOCK * 1000.0 / SR, peak);
    CHECK(finite);
    CHECK(peak < 1000);  // (loud is allowed: 256 tracks; wild values aren't)
    CHECK(e.pos == LENGTH);
    CHECK(rounds > 0);
    CHECK(e.lastError().empty());
    e.stop();
    CHECK(!e.playing);
    std::vector<float> out((size_t)BLOCK * 2);
    e.callback(nullptr, out.data(), BLOCK);  // stopped: still answers
    CHECK(e.pos == LENGTH);
}

static void testResizeStorm() {
    ui::App& app = ui::App::get();
    MainWindow* w = filledWindow(48);
    frames(0.1);
    const int fixed[][2] = {{1, 1}, {2, 600}, {1600, 1}, {40, 30}, {4000, 2500}, {1280, 800}, {320, 200}};
    const float scales[] = {1.0f, 0.5f, 2.0f, 3.0f};
    bool sane = true;
    for (int i = 0; i < 400; i++) {
        if (i % 25 == 0) app.setScale(scales[(i / 25) % 4]);  // (Options → Interface size, meanwhile)
        plat::Event r;
        r.type = plat::Event::Resize;
        r.w = i % 8 < 7 ? fixed[i % 7][0] : pick(1, 3000);
        r.h = i % 8 < 7 ? fixed[i % 7][1] : pick(1, 2000);
        w->onEvent(nullptr, r);
        app.advanceTimers(0.016);
        app.tick();
        walk(w, [&](ui::Widget* x) {
            if (x->isShown() && (x->geometry().w < 0 || x->geometry().h < 0)) sane = false;
        });
    }
    app.setScale(1);
    w->resizeWindow(1280, 800);
    frames(0.1);
    printf("resize storm: 400 sizes from 1 x 1 to 4000 x 2500, at 4 interface sizes\n");
    CHECK(sane);  // (no widget with a negative size)
    CHECK(w->width() == 1280 && w->height() == 800);
    CHECK(w->project.tracks.size() == 48);
    CHECK(!w->timeline->lanes().empty());
    delete w;
    app.collectGarbage();
}

static void testEverythingAtOnce() {
    ui::App& app = ui::App::get();
    MainWindow* w = filledWindow(16);
    closeDialogsAtOnce();
    frames(0.5);  // (the tracks become an undo step)
    size_t before = w->project.tracks.size();
    w->togglePlay();
    // a drag held across the timeline the whole time
    ui::Widget* tl = w->timeline;
    Point from = tl->mapToWindow(Point(tl->geometry().w / 3, 30));
    plat::Event m;
    m.type = plat::Event::MouseDown;
    m.x = from.x, m.y = from.y;
    m.button = m.buttons = plat::LeftButton;
    w->onEvent(nullptr, m);
    int sliders = 0, toggles = 0;
    for (int frame = 0; frame < 40; frame++) {
        std::vector<ui::Slider*> ss;
        walk(w, [&](ui::Widget* x) {
            if (auto* s = dynamic_cast<ui::Slider*>(x); s && s->isShown()) ss.push_back(s);
        });
        for (ui::Slider* s : ss) s->setValue(pick(s->minimum(), s->maximum()));
        sliders += (int)ss.size();
        // the toggles one at a time (one may rebuild the panel: the rest are looked for again)
        for (size_t k = 0;; k++) {
            std::vector<ui::Widget*> ts;
            walk(w, [&](ui::Widget* x) {
                if (!x->isShown()) return;
                if (auto* b = dynamic_cast<ui::Button*>(x); b && b->isCheckable()) ts.push_back(b);
                else if (dynamic_cast<ui::CheckBox*>(x)) ts.push_back(x);
            });
            if (k >= ts.size()) break;
            if (auto* b = dynamic_cast<ui::Button*>(ts[k])) b->click();
            else if (auto* c = dynamic_cast<ui::CheckBox*>(ts[k])) c->setChecked(!c->isChecked());
            toggles++;
        }
        m.type = plat::Event::MouseMove;
        m.x = from.x + pick(-300, 600), m.y = from.y + pick(-20, 400);
        w->onEvent(nullptr, m);
        app.advanceTimers(0.02);
        app.tick();
    }
    m.type = plat::Event::MouseUp;
    m.buttons = 0;
    w->onEvent(nullptr, m);
    w->stop();
    closeOtherWindows(w);
    frames(0.5);  // (the undo history looks every 250 ms)
    printf("everything at once: %d slider moves, %d toggles over 40 frames\n", sliders, toggles);
    CHECK(sliders > 0 && toggles > 0);
    // everything undone, then redone: the tracks are all there again
    for (int i = 0; i < 3000; i++) w->undo();
    frames(0.1);
    for (int i = 0; i < 3000; i++) w->redo();
    frames(0.1);
    CHECK(w->project.tracks.size() == before);
    CHECK(w->engine.lastError().empty());
    app.nestedLoop = nullptr;
    delete w;
    app.collectGarbage();
}

static void testTerminalStorm() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;  // (as tui::run)
    MainWindow* w = filledWindow(12);
    closeDialogsAtOnce();
    {
        tui::Front f(*w);
        using tui::Key;
        const std::vector<Key> keys = {
            {plat::Key_Up, 0, ""},     {plat::Key_Down, 0, ""},     {plat::Key_Left, 0, ""},
            {plat::Key_Right, 0, ""},  {plat::Key_PageUp, 0, ""},   {plat::Key_PageDown, 0, ""},
            {plat::Key_Home, 0, ""},   {plat::Key_End, 0, ""},      {plat::Key_Tab, 0, ""},
            {plat::Key_Return, 0, ""}, {plat::Key_Escape, 0, ""},   {plat::Key_Space, 0, " "},
            {plat::Key_F2, 0, ""},     {plat::Key_F12, 0, ""},      {plat::Key_Delete, 0, ""},
            {'M', 0, "m"},             {'S', 0, "s"},               {'E', 0, "e"},
            {'U', 0, "u"},             {'K', 0, "k"},               {'I', 0, "i"},
            {'O', 0, "o"},             {'[', 0, "["},               {']', 0, "]"},
            {'+', 0, "+"},             {'-', 0, "-"},               {',', 0, ","},
            {'.', 0, "."},             {'W', plat::ModCtrl, ""},    {'Z', plat::ModCtrl, ""},
        };
        const int sizes[][2] = {{1, 1}, {20, 5}, {100, 30}, {80, 24}, {300, 90}, {40, 12}};
        tui::Screen screen{100, 30};
        int drawn = 0;
        for (int i = 0; i < 3000; i++) {
            if (i % 60 == 0) screen = tui::Screen{sizes[(i / 60) % 6][0], sizes[(i / 60) % 6][1]};
            f.key(keys[(size_t)pick(0, (int)keys.size() - 1)]);
            app.advanceTimers(0.02);
            app.tick();
            f.draw(screen);
            drawn++;
        }
        printf("terminal storm: %d keys and pictures, the screen resized 50 times\n", drawn);
        CHECK(drawn == 3000);
        screen = tui::Screen{100, 30};
        f.draw(screen);
        CHECK(screen.line(0).find("File") != std::string::npos);  // (the menu bar is still there)
        w->stop();
        closeOtherWindows(w);
        frames(0.1);
    }
    app.nestedLoop = nullptr;
    delete w;
    app.collectGarbage();
    app.paintWindows = true;
}

int main() {
    util::makeDirs(TMP);
    setenv("XDG_DATA_HOME", (TMP + "/data").c_str(), 1);  // (recovery files and the like: not yours)
    setenv("TINYDAW_NO_UPDATE_CHECK", "1", 1);
    settings::setPath(TMP + "/settings.json");
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    testOverloadedEngine();
    testResizeStorm();
    testEverythingAtOnce();
    testTerminalStorm();
    TEST_MAIN_END();
}
