// The slice window at its worst (slice_dialog.h): Grid lines exactly where the song's grid (3/4, a tempo change)
// or the clip's own BPM puts them, Transients exactly dsp::Onsets' hits, lines dragged (snapping to hits, Alt
// not), added and removed by the mouse, a slice heard (and the one before stopped), the terminal's keys (choose,
// nudge, insert, delete, Space) through the TUI's own window drawing, 2,000 slices with the 32-pad limit, and
// Slice making the kit from the edited lines. Headless; no sound (the engine is never opened). Pictures go to
// build/ui-samples/slicing/.
#include <chrono>
#include <fstream>
#include <climits>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "audio/padkit.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "tui/screen.h"
#include "tui/window_view.h"
#include "ui/pad_kit_dialog.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"

using namespace ui;
using pad_kit_dialog::SliceDialog;

namespace {
const int SR = 48000;
const std::string OUT = "build/ui-samples/slicing";

// `seconds` of sound with a click-like hit at each of `hits` (frames)
AudioPtr hitsAt(double seconds, const std::vector<int64_t>& hits) {
    auto a = std::make_shared<AudioBuffer>();
    int64_t n = (int64_t)(seconds * SR);
    a->samples.assign((size_t)n * 2, 0.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int64_t h : hits)
        for (int64_t k = 0; k < SR / 8 && h + k < n; k++) {
            float v = 0.8f * noise(rng) * (float)std::exp(-k / 1500.0);
            a->samples[(size_t)(h + k) * 2] += v;
            a->samples[(size_t)(h + k) * 2 + 1] += v;
        }
    return a;
}

MouseEvent mouse(int x, int mods = 0) {
    MouseEvent e;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    e.pos = Point(x, 20);
    e.mods = mods;
    return e;
}

tui::Key key(int k, int mods = 0, std::string text = "") { return tui::Key{k, mods, std::move(text)}; }

struct World {
    Project project;
    audio::Engine engine{project};
    ScrollArea scroll;
    Timeline* tl;
    TrackPtr track;
    World() {
        sampler_dialog::Host host;
        host.project = &project;
        host.engine = &engine;
        sampler_dialog::setHost(host);
        scroll.resize(900, 300);
        tl = scroll.makeContent<Timeline>(project, engine, &scroll);
        track = project.addTrack(Kind::Audio);
    }
    ClipPtr clip(AudioPtr audio, int64_t start = 0) {
        ClipPtr c = makeAudioClip("Loop.wav", std::move(audio), SR, SR);
        c->track = track;
        c->start = start;
        project.addClip(c);
        tl->relayout();
        return c;
    }
    SliceDialog* open(const ClipPtr& c) {
        SliceDialog* d = tl->sliceToPadKit(c);
        if (d) {
            d->resize(640, 380);
            App::get().doLayouts();
        }
        return d;
    }
};
}  // namespace

// Grid: the song's grid (4/4, then 3/4 with a tempo change), and the clip's own BPM; exact frames
static void testGrid() {
    Clip c;
    c.kind = Kind::Audio;
    Timing t;
    t.bpm = 120;
    t.beatsPerBar = 4;
    std::vector<int64_t> g = padkit::gridStarts(c, SR * 2, t, SR, 16);  // (a 16th at 120: 6000 frames)
    CHECK(g.size() == 16);
    for (size_t i = 0; i < g.size(); i++) CHECK(g[i] == (int64_t)i * 6000);
    // 3/4 at 120 for two bars (6 beats: 3 s), then 90 BPM (a beat: 32000 frames); the clip from 1 s, 6 s long
    t.beatsPerBar = 3;
    t.setChange(3, 90, 3);
    c.start = SR;
    g = padkit::gridStarts(c, SR * 6, t, SR, 4);
    std::vector<int64_t> want{0, 24000, 48000, 72000, 96000, 128000, 160000, 192000, 224000, 256000};
    CHECK(g == want);
    g = padkit::gridStarts(c, SR * 6, t, SR, 8);
    CHECK(g.size() == 20 && g[8] == 96000 && g[9] == 112000 && g.back() == 272000);
    // the clip's own tempo: 100 BPM stretched 1.2 times (an 8th: 17280 frames), its window 1000 frames in
    c.sourceBpm = 100;
    c.stretch = 1.2;
    c.offset = 1000;
    g = padkit::gridStarts(c, SR * 2, t, SR, 8);
    CHECK(!g.empty() && g[0] == 16280 && g[1] == 33560);
    for (size_t i = 1; i < g.size(); i++) CHECK(g[i] - g[i - 1] == 17280);
    // snapping: the nearest within reach
    std::vector<int64_t> hits{100, 1000, 5000};
    CHECK(padkit::snapToHit(1040, hits, 50) == 1000 && padkit::snapToHit(1060, hits, 50) == 1060 &&
          padkit::snapToHit(80, hits, 50) == 100 && padkit::snapToHit(4990, hits, 50) == 5000);
}

// Transients is dsp::Onsets; the dialog's Grid on the song; editing by the mouse; hearing slices; then Slice
static void testEditAndSlice() {
    World w;
    const std::vector<int64_t> hits{SR / 4, SR / 2 + 300, SR + 1000, SR * 3 / 2 + 50};
    ClipPtr c = w.clip(hitsAt(2, hits));
    SliceDialog* d = w.open(c);
    CHECK(d && w.tl->slicePreview);
    if (!d) return;
    CHECK(d->mode->currentIndex() == SliceDialog::Transients);
    std::vector<float> sound = padkit::clipSound(*c);
    dsp::Onsets onsets(sound.data(), (int64_t)sound.size() / 2, SR);
    std::vector<int64_t> want = padkit::sliceStarts(sound.data(), (int64_t)sound.size() / 2,
                                                    onsets.find(d->sensitivity->value() / 100.0), INT_MAX);
    CHECK(d->hits == want && d->hits.size() == 4);
    CHECK(w.tl->slicePreview->frames == d->hits);
    // Grid 1/8 at 120 BPM: a line every 12000 frames (the sound before the first hit is silent: still a line at 0)
    d->grid->setCurrentIndex(1);
    CHECK(d->mode->currentIndex() == SliceDialog::Grid && d->hits.size() == 8 && d->hits[3] == 36000);
    CHECK(w.tl->slicePreview->frames == d->hits);
    // back to Transients, then the mouse: the view is 1 px per frames()/width frames
    d->setMode(SliceDialog::Transients);
    pad_kit_dialog::SliceView* v = d->view;
    CHECK(v->width() > 100);
    int64_t moved = d->hits[1];
    int x = v->xOf(moved);
    MouseEvent e = mouse(x);
    v->mousePress(e);
    CHECK(v->dragging == 1);
    // dragged a few px past the hit: snaps back to it (a hit of Transients' list)
    e = mouse(x + 3);
    v->mouseMove(e);
    CHECK(d->hits[1] == moved && d->mode->currentIndex() == SliceDialog::Transients);  // (nothing changed yet)
    // with Alt: exactly where the pointer is
    e = mouse(x + 30, plat::ModAlt);
    v->mouseMove(e);
    CHECK(d->hits[1] == v->frameAt(x + 30) && d->mode->currentIndex() == SliceDialog::Manual);
    CHECK(w.tl->slicePreview->frames == d->hits);
    // without: back near the hit, it snaps onto it
    e = mouse(x + 2);
    v->mouseMove(e);
    CHECK(d->hits[1] == moved);
    // never past its neighbour
    e = mouse(v->xOf(d->hits[2]) + 40, plat::ModAlt);
    v->mouseMove(e);
    CHECK(d->hits[1] < d->hits[2] && d->hits.size() == 4);
    v->mouseRelease(e);
    CHECK(v->dragging == -1);
    int64_t edited1 = d->hits[1];
    // double-click: a line added in the silence; a double-click on it removes it again
    int addX = v->xOf(SR / 8);
    e = mouse(addX);
    v->mouseDoubleClick(e);
    CHECK(d->hits.size() == 5 && std::binary_search(d->hits.begin(), d->hits.end(), v->frameAt(addX)));
    v->mouseDoubleClick(e);
    CHECK(d->hits.size() == 4);
    // the line at the clip's start (sound before the first hit? none here: the first hit's) can be removed only
    // if it isn't frame 0
    if (d->hits[0] == 0) {
        d->removeLine(0);
        CHECK(d->hits[0] == 0);
    }
    // clicking inside a slice hears it (Engine::preview), and the next one stops it
    e = mouse(v->xOf((d->hits[2] + d->sliceEnd(2)) / 2));
    v->mousePress(e);
    auto voice = w.engine.previewVoice();
    CHECK(d->chosen == 2 && voice && voice->data && voice->data->frames() == d->sliceEnd(2) - d->hits[2]);
    if (voice && voice->data) CHECK(voice->data->samples[0] == sound[(size_t)d->hits[2] * 2]);
    d->audition(3);
    auto next = w.engine.previewVoice();
    CHECK(next && next != voice && next->data->frames() == d->sliceEnd(3) - d->hits[3]);
    // add a line by the keys' Insert: in the middle of the chosen slice
    d->choose(3);
    int64_t mid = (d->hits[3] + d->sliceEnd(3)) / 2;
    KeyEvent ins;
    ins.key = plat::Key_Insert;
    v->keyPress(ins);
    CHECK(d->hits.size() == 5 && d->hits[4] == mid);
    std::vector<int64_t> edited = d->hits;
    CHECK(edited[1] == edited1);
    // Slice: the kit from the edited lines (the notes start there); the slice playing stops
    d->accept();
    CHECK(!w.engine.previewVoice());
    CHECK(!w.tl->slicePreview && w.project.tracks.size() == 2);
    TrackPtr made = w.project.tracks.size() == 2 ? w.project.tracks[1] : nullptr;
    CHECK(made && made->playsPadKit() && made->padKit->used() == (int)edited.size());
    std::vector<ClipPtr> mc = made ? w.project.clipsOn(made) : std::vector<ClipPtr>();
    CHECK(mc.size() == 1 && mc[0]->notes->size() == edited.size());
    if (mc.size() == 1)
        for (size_t i = 0; i < std::min(edited.size(), mc[0]->notes->size()); i++)
            CHECK(std::llround((*mc[0]->notes)[i].start * SR) == edited[i]);
    App::get().collectGarbage();
}

// the terminal: the dialog drawn as text by the TUI's window drawing, worked by its keys
static void testTuiKeys() {
    World w;
    ClipPtr c = w.clip(hitsAt(4, {SR / 4, SR / 2, SR, SR * 3 / 2}));
    SliceDialog* d = w.open(c);
    CHECK(d);
    if (!d) return;
    d->setMode(SliceDialog::Grid);  // (1/16: 32 slices, every 6000 frames)
    CHECK(d->hits.size() == 32);
    tui::Screen screen(110, 40);
    tui::drawWindow(screen, *d, 0, 0, 110, 40);
    std::string text = screen.lines();
    CHECK(text.find("> 1  0:00.000  0.125 s") != std::string::npos);
    CHECK(text.find("Up/Down") != std::string::npos && text.find("Insert") != std::string::npos);  // (the hint names the keys)
    tui::sendKey(*d, key(plat::Key_Down));
    tui::sendKey(*d, key(plat::Key_Down));
    CHECK(d->chosen == 2);
    // Right: its start 5 ms later (240 frames), Shift+Right 50 ms; Left back; Manual now
    tui::sendKey(*d, key(plat::Key_Right));
    CHECK(d->hits[2] == 12240 && d->mode->currentIndex() == SliceDialog::Manual);
    tui::sendKey(*d, key(plat::Key_Right, plat::ModShift));
    CHECK(d->hits[2] == 14640);
    tui::sendKey(*d, key(plat::Key_Left, plat::ModShift));
    tui::sendKey(*d, key(plat::Key_Left));
    CHECK(d->hits[2] == 12000);
    // never past a neighbour: far right stops before the next line
    for (int i = 0; i < 10; i++) tui::sendKey(*d, key(plat::Key_Right, plat::ModShift));
    CHECK(d->hits[2] == 17999 && d->hits.size() == 32);
    for (int i = 0; i < 10; i++) tui::sendKey(*d, key(plat::Key_Left, plat::ModShift));
    CHECK(d->hits[2] == 6001);
    // Space hears it
    tui::sendKey(*d, key(plat::Key_Space, 0, " "));
    auto voice = w.engine.previewVoice();
    CHECK(voice && voice->data && voice->data->frames() == d->sliceEnd(2) - d->hits[2]);
    // Insert: a line in its middle (33 slices: the 33rd flagged); Delete removes the chosen one's start
    tui::sendKey(*d, key(plat::Key_Insert));
    CHECK(d->hits.size() == 33 && d->hits[3] == (6001 + 18000) / 2 && d->chosen == 3);
    CHECK(d->count->text().find("33 slices") == 0 && d->count->text().find("32 pads") != std::string::npos);
    tui::drawWindow(screen, *d, 0, 0, 110, 40);
    std::ofstream(OUT + "/slice-window.txt") << screen.lines();
    d->showWindow();
    App::get().doLayouts();
    d->update();
    App::get().tick();
    savePng(OUT + "/slice_window.png", d->framebuffer());
    tui::sendKey(*d, key(plat::Key_Delete));
    CHECK(d->hits.size() == 32 && d->chosen == 2);
    // the first line (the clip's start) stays
    tui::sendKey(*d, key(plat::Key_Up));
    tui::sendKey(*d, key(plat::Key_Up));
    tui::sendKey(*d, key(plat::Key_Up));
    CHECK(d->chosen == 0);
    tui::sendKey(*d, key(plat::Key_Delete));
    tui::sendKey(*d, key(plat::Key_Right));
    CHECK(d->hits.size() == 32 && d->hits[0] == 0);
    CHECK(w.tl->slicePreview && w.tl->slicePreview->frames == d->hits);
    d->reject();
    CHECK(!w.engine.previewVoice() && !w.tl->slicePreview && w.project.tracks.size() == 1);
    App::get().collectGarbage();
}

// 2,000 slices: Grid 1/32 over 125 s at 120 BPM; the count flags the 32-pad limit; editing, drawing (window and
// text) and slicing stay quick; the kit has 32 pads, the last holding the rest of the sound
static void testThousands() {
    auto t0 = std::chrono::steady_clock::now();
    World w;
    std::vector<int64_t> hits;
    for (int i = 0; i < 250; i++) hits.push_back((int64_t)i * SR / 2);
    ClipPtr c = w.clip(hitsAt(125, hits));
    SliceDialog* d = w.open(c);
    CHECK(d);
    if (!d) return;
    d->grid->setCurrentIndex(3);
    CHECK(d->hits.size() == 2000 && d->hits[1999] == 1999 * 3000);
    CHECK(d->count->text().find("2000 slices") == 0 && d->count->text().find("first 32") != std::string::npos);
    CHECK(w.tl->slicePreview->frames.size() == 2000);
    std::vector<std::string> rows;
    int cur = -1;
    d->view->listRows(rows, cur);
    CHECK(rows.size() == 2000 && rows[31].find("(no pad)") == std::string::npos &&
          rows[32].find("(no pad)") != std::string::npos);
    // every line dragged a little, every slice heard: no line lost, none crossing
    for (int i = 1; i < 2000; i += 37) d->moveLine(i, d->hits[(size_t)i] + 700, true, 1000);
    for (int i = 0; i < 2000; i += 97) d->audition(i);
    CHECK(d->hits.size() == 2000 && std::is_sorted(d->hits.begin(), d->hits.end()));
    for (int i = 0; i < 1000; i++) d->addLine(d->hits[(size_t)i * 2] + 1);
    CHECK(d->hits.size() == 3000);
    for (int i = 0; i < 1000; i++) d->removeLine(1);
    CHECK(d->hits.size() == 2000);
    // drawn: the window, and as text
    {
        Image img(640, 380, theme::c("window"));
        Painter p(img);
        d->paintTree(p, d->rect());
    }
    tui::Screen screen(110, 40);
    tui::drawWindow(screen, *d, 0, 0, 110, 40);
    std::vector<int64_t> lines = d->hits;
    d->accept();
    TrackPtr made = w.project.tracks.size() == 2 ? w.project.tracks[1] : nullptr;
    CHECK(made && made->padKit && made->padKit->used() == PadKit::MAX_PADS);
    std::vector<ClipPtr> mc = made ? w.project.clipsOn(made) : std::vector<ClipPtr>();
    CHECK(mc.size() == 1 && mc[0]->notes->size() == (size_t)PadKit::MAX_PADS);
    if (made && made->padKit) {  // (the last pad: from the 32nd line to the end)
        const Pad& last = made->padKit->pads[PadKit::MAX_PADS - 1];
        CHECK(!last.empty() && last.sound.sample->source->frames() == c->frames() - lines[PadKit::MAX_PADS - 1]);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("2,000 slices: %.2f s\n", s);
    CHECK(s < 8);
    App::get().collectGarbage();
}

int main() {
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    testGrid();
    testEditAndSlice();
    testTuiKeys();
    testThousands();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
