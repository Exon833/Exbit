// Clip gain envelopes (model/clip_envelope.h), assuming the worst: the dB scale and its extremes (+24 dB, silence);
// the mixer's forward walk against Clip::level and the exact curve, buffer by buffer as one; split, cut pieces, join,
// reverse, stretch, copies, loops and another sample rate keeping the envelope on the same sound; save, load and
// undo; broken input; 256 tracks each with a 1,000-point envelope played on every thread and rendered while the
// envelopes are replaced from another thread; the timeline's mouse (add, drag, bend, remove, the menu) and painting;
// and the terminal UI's dialog (F2 → Gain envelope points…).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <thread>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/render.h"
#include "audio/workers.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/clip_envelope.h"
#include "model/edits.h"
#include "model/history.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/clip_painter.h"
#include "ui/gain_envelope_dialog.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;
const std::string TMP = "build/ui-samples/clip_envelope";
const float DC = 0.5f;
using namespace project_io;

AudioPtr dc(int64_t frames, float v = DC) {
    auto a = std::make_shared<AudioBuffer>();
    a->samples.assign((size_t)frames * 2, v);
    return a;
}

ClipPtr dcClip(double seconds, float v = DC) { return makeAudioClip("Dc", dc((int64_t)(seconds * SR), v), SR, SR); }

// what the clip adds at timeline frame `frame` (left channel)
float heard(const Clip& c, int64_t frame) {
    float l = 0, r = 0;
    float* buf[2] = {&l, &r};
    mixer::addClip(buf, 2, 1, c, frame);
    return l;
}

// the clip's gain at timeline frame `frame`: against the same clip without its envelope
double gainAt(const Clip& c, int64_t frame) {
    Clip plain = c;
    plain.gainEnvelope = nullptr;
    float base = heard(plain, frame);
    return base == 0 ? 0 : heard(c, frame) / base;
}

double dbGain(double db) { return std::pow(10.0, db / 20); }

// a ramp in dB over its audio: -12 dB at frame a, +6 dB at frame b
clip_envelope::Points ramp(int64_t a, int64_t b) {
    return clip_envelope::make({{(double)a, clip_envelope::valueOf(-12)}, {(double)b, clip_envelope::valueOf(6)}});
}
double rampDb(int64_t a, int64_t b, double frame) {
    double x = std::min(1.0, std::max(0.0, (frame - a) / (double)(b - a)));
    return -12 + 18 * x;
}

std::vector<AutoPoint> randomPoints(std::mt19937& rng, int n, double frames) {
    std::uniform_real_distribution<double> u(0, 1), b(-1, 1);
    std::vector<AutoPoint> pts;
    for (int i = 0; i < n; i++) pts.push_back({u(rng) * frames, u(rng), i % 3 ? b(rng) : 0.0});
    return pts;
}

void testScale() {
    using namespace clip_envelope;
    CHECK_NEAR(dbOf(1), 24, 1e-12);
    CHECK_NEAR(dbOf(0), config::MIN_DB, 1e-12);
    CHECK_NEAR(dbOf(unity()), 0, 1e-12);
    CHECK(gainOf(0) == 0.0f);
    CHECK_NEAR(gainOf(1), dbGain(24), 1e-4);
    CHECK_NEAR(gainOf(unity()), 1, 1e-6);
    CHECK(valueOf(-1000) == 0 && valueOf(1000) == 1 && valueOf(-INFINITY) == 0);
    CHECK(dbText(0) == "-inf dB" && dbText(1) == "+24.0 dB" && dbText(valueOf(-6)) == "-6.0 dB");
    // broken input made safe: sorted, clamped, NaN gone; none: null
    CHECK(!make({}));
    auto p = make({{500, 2, 5}, {NAN, NAN, NAN}, {-10, -1, -9}, {100, 0.5, 0}});
    CHECK(p && p->size() == 4);
    bool ordered = true, inRange = true;
    for (size_t i = 0; i < p->size(); i++) {
        const AutoPoint& q = (*p)[i];
        if (i && q.t < (*p)[i - 1].t) ordered = false;
        if (!(q.t >= 0 && q.v >= 0 && q.v <= 1 && q.bend >= -1 && q.bend <= 1)) inRange = false;
    }
    CHECK(ordered && inRange);
    // mirrored twice: as it was; a bend stays on its segment
    std::vector<AutoPoint> pts{{0, 0.2, 0.7}, {100, 0.9, -0.4}, {300, 0.5, 0}};
    auto m = mirrored(pts, 1000);
    CHECK(m.size() == 3 && m[0].t == 699 && m[0].bend == -0.4 && m[1].bend == 0.7);
    for (double x : {720.0, 800.0, 950.0})
        CHECK_NEAR(automation::valueAt(m, x), automation::valueAt(pts, 999 - x), 1e-9);
    auto back = mirrored(m, 1000);
    for (size_t i = 0; i < pts.size(); i++) CHECK(back[i].t == pts[i].t && back[i].v == pts[i].v);
    // the walking valueAt is the searching one, forwards, backwards, and after jumps
    std::mt19937 rng(7);
    auto many = *make(randomPoints(rng, 1000, 1e6));
    size_t cursor = 0;
    bool same = true;
    for (double x = -50; x < 1.01e6; x += 997) same = same && automation::valueAt(many, x, cursor) == automation::valueAt(many, x);
    for (double x : {5e5, 1.0, 9e5, 9e5, -3.0, 2e6, 0.0})
        same = same && automation::valueAt(many, x, cursor) == automation::valueAt(many, x);
    CHECK(same);
}

void testExtremes() {
    ClipPtr c = dcClip(1);
    c->gainEnvelope = clip_envelope::make({{0, 1.0}});  // +24 dB all through
    CHECK_NEAR(heard(*c, 24000), DC * dbGain(24), 1e-3);
    c->gainEnvelope = clip_envelope::make({{0, 0.0}});  // silence
    bool silent = true;
    for (int64_t f = 0; f < 48000; f += 7) silent = silent && heard(*c, f) == 0.0f;
    CHECK(silent);
    // from silence to +24 dB: rising all the way, ending at +24 dB
    c->gainEnvelope = clip_envelope::make({{1000, 0.0}, {40000, 1.0}});
    CHECK(heard(*c, 500) == 0.0f);
    bool rising = true;
    for (int64_t f = 1000; f < 40000; f += 32) rising = rising && heard(*c, f + 32) >= heard(*c, f);
    CHECK(rising);
    CHECK_NEAR(heard(*c, 44000), DC * dbGain(24), 1e-3);
    CHECK(c->hasLevels());
    CHECK_NEAR(c->level(44000), dbGain(24), 1e-3);
}

void testWalk() {
    // a 1,000-point random envelope: the mixer, in one buffer or in odd-sized ones, gives the same samples; they are
    // Clip::level's; at every STEP they are the exact curve's
    std::mt19937 rng(11);
    ClipPtr c = dcClip(2);
    c->gainEnvelope = clip_envelope::make(randomPoints(rng, 1000, 2.0 * SR));
    int64_t n = c->frames();
    std::vector<float> whole((size_t)n * 2), parts((size_t)n * 2);
    {
        float* buf[2] = {whole.data(), whole.data() + n};
        mixer::addClip(buf, 2, n, *c, 0);
    }
    std::uniform_int_distribution<int> size(1, 777);
    for (int64_t at = 0; at < n;) {
        int64_t k = std::min<int64_t>(size(rng), n - at);
        float* buf[2] = {parts.data() + at, parts.data() + n + at};
        mixer::addClip(buf, 2, k, *c, at);
        at += k;
    }
    CHECK(whole == parts);
    double worstLevel = 0, worstExact = 0;
    const int edge = config::EDGE_FADE_FRAMES;
    for (int64_t f = edge; f < n - edge; f++) {
        worstLevel = std::max(worstLevel, (double)std::fabs(whole[(size_t)f] - DC * c->level(f)));
        if (f % clip_envelope::STEP == 0)
            worstExact = std::max(worstExact, (double)std::fabs(whole[(size_t)f] / DC -
                                                         clip_envelope::gainOf(clip_envelope::valueAt(*c, f))));
    }
    CHECK(worstLevel < 1e-5);
    CHECK(worstExact < 1e-5);
    // a start part-way into its audio (trimmed): the same audio frames, the same gain
    ClipPtr t = copyClip(c);
    t->offset = 12345;
    t->length = 20000;
    t->start = 99999;
    bool same = true;
    for (int64_t p = edge; p < 20000 - edge; p += 13) same = same && heard(*t, 99999 + p) == whole[(size_t)(12345 + p)];
    CHECK(same);
}

void testEdits() {
    const int64_t N = 2 * SR;
    ClipPtr c = dcClip(2);
    c->start = SR;  // (timeline 1 s … 3 s)
    c->gainEnvelope = ramp(SR / 2, SR + SR / 2);
    for (int64_t p : {(int64_t)SR / 2, (int64_t)SR, (int64_t)SR + SR / 2})
        CHECK_NEAR(gainAt(*c, c->start + p), dbGain(rampDb(SR / 2, SR + SR / 2, (double)p)), 1e-3);

    // split: the pieces keep their part (the same frames of the same sound, the same gain)
    ClipPtr left, right;
    CHECK(edits::splitClip(c, c->start + SR * 3 / 4, left, right));
    CHECK(left->gainEnvelope == c->gainEnvelope && right->gainEnvelope == c->gainEnvelope);
    bool same = true;
    for (int64_t f = c->start + 100; f < c->start + SR * 3 / 4 - 100; f += 101) same = same && heard(*left, f) == heard(*c, f);
    for (int64_t f = c->start + SR * 3 / 4 + 100; f < c->end() - 100; f += 101) same = same && heard(*right, f) == heard(*c, f);
    CHECK(same);
    ClipPtr piece = edits::pieceWithin(c, c->start + SR / 3, c->start + SR);
    CHECK(piece && std::fabs(gainAt(*piece, c->start + SR / 2) - gainAt(*c, c->start + SR / 2)) < 1e-6);

    // joined: the envelope baked into the audio, as the pieces played it
    ClipPtr joined = edits::consolidated({left, right}, SR);
    CHECK(joined && !joined->gainEnvelope);
    double worst = 0;
    for (int64_t f = c->start + 100; f < c->end() - 100; f += 7)
        worst = std::max(worst, (double)std::fabs(heard(*joined, f) - (heard(*left, f) + heard(*right, f))));
    CHECK(worst < 1e-5);

    // reversed: the envelope mirrored, so each frame of sound keeps its gain (heard at the mirrored place)
    ClipPtr rev = edits::reversedClip(c, SR);
    CHECK(rev->gainEnvelope && rev->gainEnvelope != c->gainEnvelope);
    worst = 0;
    for (int64_t p = 1000; p < N - 1000; p += 997)
        worst = std::max(worst, std::fabs(gainAt(*rev, rev->start + p) - gainAt(*c, c->start + (N - 1 - p))));
    CHECK(worst < 0.01);
    ClipPtr again = edits::reversedClip(rev, SR);  // (and back)
    worst = 0;
    for (int64_t p = 1000; p < N - 1000; p += 997)
        worst = std::max(worst, std::fabs(gainAt(*again, again->start + p) - gainAt(*c, c->start + p)));
    CHECK(worst < 0.01);

    // stretched twice as long: the envelope too (what played at p plays at 2p)
    ClipPtr st = edits::restretched(c, 2.0, 0, SR);
    CHECK(st->gainEnvelope && std::fabs((*st->gainEnvelope)[1].t - 2 * (*c->gainEnvelope)[1].t) < 1e-9);
    worst = 0;
    for (int64_t p = 2000; p < N - 2000; p += 1009)
        worst = std::max(worst, std::fabs(gainAt(*st, st->start + 2 * p) - gainAt(*c, c->start + p)));
    CHECK(worst < 0.01);

    // copies and duplicates share it
    ClipPtr copy = copyClip(c);
    CHECK(copy->gainEnvelope == c->gainEnvelope);
    Project p;
    auto track = p.addTrack(Kind::Audio);
    c->track = track;
    p.addClip(c);
    auto dups = edits::duplicateClips(p, {c});
    CHECK(dups.size() == 1 && dups[0]->gainEnvelope == c->gainEnvelope);
    CHECK(dups.size() == 1 && heard(*dups[0], dups[0]->start + SR) == heard(*c, c->start + SR));

    // looping: every pass has it again
    ClipPtr loop = copyClip(c);
    loop->offset = SR / 4;
    loop->length = SR;
    loop->loop = true;
    loop->loopStart = 0.25;
    loop->loopLength = 1.0;
    loop->length = 4 * SR;
    loop->prepare(SR);
    CHECK(loop->loopFrom == SR / 4 && loop->loopFor == SR);
    worst = 0;
    for (int64_t q = 2000; q < SR - 2000; q += 997)
        for (int pass = 1; pass < 4; pass++)
            worst = std::max(worst, std::fabs(gainAt(*loop, loop->start + q + pass * SR) - gainAt(*loop, loop->start + q)));
    CHECK(worst < 1e-4);
    CHECK_NEAR(gainAt(*loop, loop->start + SR / 4), dbGain(rampDb(SR / 2, SR + SR / 2, SR / 2)), 1e-3);
    // where its points are drawn: once per pass
    CHECK(clip_envelope::framesOf(*loop, SR, loop->offset, loop->offset + loop->frames()).size() == 4);

    // another sample rate: the same seconds
    Project q;
    auto qt = q.addTrack(Kind::Audio);
    ClipPtr r = copyClip(c);
    r->track = qt;
    q.addClip(r);
    double before = gainAt(*r, r->start + SR);
    q.changeSampleRate(SR, 44100);
    CHECK_NEAR(gainAt(*r, r->start + 44100), before, 2e-3);
}

void testSaveLoadUndo() {
    util::makeDirs(TMP);
    Project p;
    auto t = p.addTrack(Kind::Audio);
    ClipPtr c = dcClip(1);
    c->track = t;
    p.addClip(c);
    c->gainEnvelope = clip_envelope::make({{0, 0.3, 0.5}, {12000, 1.0, -0.25}, {30000, 0.0, 0}});
    ClipPtr plain = dcClip(0.5);  // (none: saved as before)
    plain->track = t;
    plain->start = SR * 2;
    p.addClip(plain);
    std::string path = TMP + "/song.json", error;
    MasterSettings master;
    CHECK(saveProject(path, p, SR, master, error));
    std::string text;
    util::readFile(path, text);
    CHECK(text.find("gain_envelope") != std::string::npos);
    Project q;
    MasterSettings m2;
    CHECK(loadProject(path, q, SR, m2, error));
    CHECK(q.clips.size() == 2);
    if (q.clips.size() == 2) {
        auto e = q.clips[0]->gainEnvelope;
        CHECK(e && e->size() == 3);
        if (e && e->size() == 3)
            for (size_t i = 0; i < 3; i++) {
                CHECK_NEAR((*e)[i].t, (*c->gainEnvelope)[i].t, 1e-6);
                CHECK((*e)[i].v == (*c->gainEnvelope)[i].v && (*e)[i].bend == (*c->gainEnvelope)[i].bend);
            }
        CHECK(!q.clips[1]->gainEnvelope);
        CHECK_NEAR(gainAt(*q.clips[0], 6000), gainAt(*c, 6000), 1e-6);
    }
    // a broken entry: left out, the project still loads
    {
        size_t at = text.find("\"gain_envelope\"");
        std::string broken = text.substr(0, at) + "\"gain_envelope\": [[\"x\", 1], 7, [0.1]], \"x_\"" +
                             text.substr(text.find(':', at) + 0);
        std::ofstream(TMP + "/broken.json") << broken;
        Project b;
        MasterSettings m3;
        bool loaded = loadProject(TMP + "/broken.json", b, SR, m3, error);
        CHECK(loaded);
        CHECK(loaded && !b.clips.empty() && !b.clips[0]->gainEnvelope);
    }
    // undo and redo: the whole envelope back, by name
    history::History h(p, SR);
    auto had = c->gainEnvelope;
    c->gainEnvelope = clip_envelope::make({{5, 0.9}});
    CHECK(h.check(SR));
    CHECK(!h.labels().empty() && h.labels().back() == "Gain envelope");
    CHECK(h.undo(SR) && c->gainEnvelope == had);
    CHECK(h.redo(SR) && c->gainEnvelope && c->gainEnvelope->size() == 1);
    c->gainEnvelope = nullptr;
    CHECK(h.check(SR) && h.undo(SR) && c->gainEnvelope && c->gainEnvelope->size() == 1);
}

void testStress() {
    // 256 tracks, each a clip with a 1,000-point envelope (and bends), played on every thread in small buffers
    // while another thread replaces every envelope again and again (as dragging points does); then rendered
    const int TRACKS = 256, POINTS = 1000;
    const int64_t LEN = SR;  // (1 s each)
    Project p;
    std::mt19937 rng(3);
    std::vector<ClipPtr> clips;
    for (int i = 0; i < TRACKS; i++) {
        auto t = p.addTrack(Kind::Audio);
        ClipPtr c = dcClip(1, 0.01f);
        c->track = t;
        c->start = (i % 7) * 1000;
        c->gainEnvelope = clip_envelope::make(randomPoints(rng, POINTS, (double)LEN));
        if (i % 5 == 0) {  // (some loop, some are reversed, all at once)
            c->loop = true;
            c->loopStart = 0.1;
            c->loopLength = 0.3;
            c->length = LEN;
            c->prepare(SR);
        }
        p.addClip(c);
        clips.push_back(c);
    }
    auto silent = clips[1];
    silent->gainEnvelope = clip_envelope::make({{0, 0.0}});
    int threads = (int)std::max(1u, std::thread::hardware_concurrency());
    workers::Pool pool(threads - 1, false);
    mixer::MixState st;
    st.pool = &pool;
    st.reserve(256, TRACKS);
    std::atomic<bool> done{false};
    std::atomic<int> replaced{0};
    std::thread editor([&] {
        std::mt19937 r2(5);
        while (!done.load()) {
            for (int i = 2; i < TRACKS && !done.load(); i += 3) {
                std::atomic_store(&clips[(size_t)i]->gainEnvelope, clip_envelope::make(randomPoints(r2, POINTS, (double)LEN)));
                replaced++;
            }
        }
    });
    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> buf(256 * 2);
    bool finite = true;
    double peak = 0;
    int64_t pos = 0;
    for (int k = 0; pos < LEN + 7000; k++) {
        int n = 64 + (k * 37) % 193;  // (odd sizes)
        std::fill(buf.begin(), buf.end(), 0.0f);
        mixer::mixClips(p, buf.data(), 2, n, true, pos, st, nullptr, SR, k == 0);
        for (int i = 0; i < n * 2; i++) {
            finite = finite && std::isfinite(buf[(size_t)i]);
            peak = std::max(peak, (double)std::fabs(buf[(size_t)i]));
        }
        pos += n;
    }
    done = true;
    editor.join();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("stress: %d tracks x %d points, %.2f s of audio mixed in %.2f s on %d threads (%d envelopes replaced)\n",
                TRACKS, POINTS, (double)pos / SR, seconds, threads, replaced.load());
    CHECK(finite && peak > 0);
    CHECK(replaced.load() > 0);
    // rendered: finite, sounding; the silenced track alone is silent
    auto mix = render::renderMix(p, 0, LEN, SR);
    CHECK(mix && !mix->empty());
    if (mix) {
        bool ok = true;
        double top = 0;
        for (float v : *mix) ok = ok && std::isfinite(v), top = std::max(top, (double)std::fabs(v));
        CHECK(ok && top > 0);
    }
    auto stem = render::renderMix(p, 0, LEN, SR, 0.0, false, nullptr, silent->track.get());
    CHECK(stem);
    if (stem) {
        double top = 0;
        for (float v : *stem) top = std::max(top, (double)std::fabs(v));
        CHECK(top == 0);
    }
    // a huge envelope (200,000 points) on one clip, mixed in one go
    ClipPtr huge = dcClip(4);
    huge->gainEnvelope = clip_envelope::make(randomPoints(rng, 200000, 4.0 * SR));
    std::vector<float> out((size_t)huge->frames() * 2);
    float* b2[2] = {out.data(), out.data() + huge->frames()};
    auto t1 = std::chrono::steady_clock::now();
    mixer::addClip(b2, 2, huge->frames(), *huge, 0);
    double hugeS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::printf("stress: 200,000 points over 4 s mixed in %.3f s\n", hugeS);
    CHECK(hugeS < 1.0);
    // …and in 32-frame buffers from late in the clip (each buffer finds its place with one search, not a walk)
    std::vector<float> small(64);
    float* b3[2] = {small.data(), small.data() + 32};
    t1 = std::chrono::steady_clock::now();
    bool same = true;
    for (int64_t at = 3 * SR; at + 32 <= huge->frames(); at += 32) {
        std::fill(small.begin(), small.end(), 0.0f);
        mixer::addClip(b3, 2, 32, *huge, at);
        same = same && small[5] == out[(size_t)at + 5];
    }
    double smallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::printf("stress: its last second in 32-frame buffers in %.3f s\n", smallS);
    CHECK(same && smallS < 0.5);
}

// --- the timeline: the mouse and painting ---

ui::MouseEvent mouse(double x, double y, int button = plat::LeftButton, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point((int)x, (int)y);
    e.globalPos = e.pos;
    e.button = button;
    e.buttons = button;
    e.mods = mods;
    return e;
}

ui::Action* actionNamed(const ui::Menu& m, const std::string& text) {
    for (auto& a : m.actions())
        if (a->text == text) return a.get();
    return nullptr;
}

void testTimeline() {
    Project project;
    audio::Engine engine{project};
    ui::ScrollArea scroll;
    scroll.setHorizontalPolicy(ui::ScrollArea::Policy::AlwaysOn);
    scroll.resize(900, 420);
    Timeline& tl = *scroll.makeContent<Timeline>(project, engine, &scroll);
    project.timing.beats = false;
    project.timing.snapOn = false;
    auto track = project.addTrack(Kind::Audio);
    ClipPtr c = dcClip(2);
    c->track = track;
    c->start = SR;  // x 100…300 at 100 px/s
    project.addClip(c);
    tl.relayout();
    tl.selection.selectOnly(c);
    // the menu: Gain envelope shows its line
    ui::Menu menu;
    tl.fillMenu(menu, Point(150, 60));
    ui::Action* show = actionNamed(menu, "Gain envelope");
    CHECK(show && show->checkable && !show->checked);
    CHECK(actionNamed(menu, "Remove gain envelope") && !actionNamed(menu, "Remove gain envelope")->enabled);
    if (show) ui::Menu::activate(show);
    CHECK(c->envelopeShown);
    auto body = tl.envelopeBody(*c);
    CHECK(body.has_value());
    if (!body) return;
    // a click adds a point there (and drags it)
    float y6 = clip_painter::envelopeY(*body, clip_envelope::valueOf(-6));
    auto e = mouse(150, y6);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EnvelopeDrag>(tl.drag));
    tl.mouseRelease(e);
    CHECK(c->gainEnvelope && c->gainEnvelope->size() == 1);
    if (!c->gainEnvelope || c->gainEnvelope->size() != 1) return;
    CHECK_NEAR((*c->gainEnvelope)[0].t, 0.5 * SR, SR / 100.0);
    CHECK_NEAR(clip_envelope::dbOf((*c->gainEnvelope)[0].v), -6, 2.0);
    CHECK(clip_painter::clipLabel(*c).find("env") != std::string::npos);
    // a second, then dragged: later and to the top (+24 dB)
    e = mouse(250, y6);
    tl.mousePress(e);
    tl.mouseRelease(e);
    CHECK(c->gainEnvelope->size() == 2);
    e = mouse(250, y6);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EnvelopeDrag>(tl.drag));
    e = mouse(270, body->y - 50);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK(c->gainEnvelope->size() == 2 && (*c->gainEnvelope)[1].v == 1.0);
    CHECK_NEAR((*c->gainEnvelope)[1].t, 1.7 * SR, SR / 100.0);
    CHECK_NEAR(gainAt(*c, c->start + (int64_t)(1.9 * SR)), dbGain(24), 0.05);
    // Ctrl+drag the segment up: bowed up
    double mid = clip_painter::envelopeY(*body, clip_envelope::valueAt(*c, SR));
    e = mouse(200, mid, plat::LeftButton, plat::ModCtrl);
    tl.mousePress(e);
    CHECK(std::holds_alternative<EnvelopeBendDrag>(tl.drag));
    e = mouse(200, mid - 15, plat::LeftButton, plat::ModCtrl);
    tl.mouseMove(e);
    tl.mouseRelease(e);
    CHECK((*c->gainEnvelope)[0].bend > 0.2);
    // painted (to a picture), the line and its points
    util::makeDirs(TMP);
    {
        ui::App::get().doLayouts();
        Image img(scroll.width(), scroll.height(), theme::c("window"));
        Painter pt(img);
        scroll.paintTree(pt, scroll.rect());
        savePng(TMP + "/timeline.png", img);
        CHECK(img.width() > 0);
    }
    // right-click a point: gone; double-click the other: gone too
    float yFirst = clip_painter::envelopeY(*body, (*c->gainEnvelope)[0].v);
    e = mouse(150, yFirst, plat::RightButton);
    tl.contextMenu(e);
    CHECK(c->gainEnvelope->size() == 1);
    e = mouse(270, clip_painter::envelopeY(*body, 1.0));
    tl.mouseDoubleClick(e);
    CHECK(!c->gainEnvelope);
    // the points dialog (answered by the test): two points typed
    GainEnvelopeDialog::testRun = [](GainEnvelopeDialog& d) {
        d.time->setValue(0.25);
        d.gain->setValue(config::MIN_DB);  // (silence)
        d.addPoint();
        d.time->setValue(1.0);
        d.gain->setValue(24);
        d.addPoint();
        return true;
    };
    tl.editEnvelopePoints(c);
    GainEnvelopeDialog::testRun = nullptr;
    CHECK(c->gainEnvelope && c->gainEnvelope->size() == 2);
    CHECK(heard(*c, c->start + SR / 8) == 0.0f);
    CHECK_NEAR(gainAt(*c, c->start + SR + SR / 2), dbGain(24), 1e-3);
    // a frozen track's clip: left alone
    track->frozen = dcClip(1);
    e = mouse(150, y6);
    tl.mousePress(e);
    CHECK(!std::holds_alternative<EnvelopeDrag>(tl.drag));
    tl.mouseRelease(e);
    track->frozen = nullptr;
    // removed from the menu
    tl.selection.selectOnly(c);
    ui::Menu again;
    tl.fillMenu(again, Point(150, 60));
    if (ui::Action* rm = actionNamed(again, "Remove gain envelope")) ui::Menu::activate(rm);
    CHECK(!c->gainEnvelope && !c->envelopeShown);
}

// --- the terminal UI: F2 → Gain envelope points…, a dialog worked by keys ---

void testTui() {
    using namespace tui;
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Screen screen{100, 30};
    std::vector<Key> queue;
    size_t at = 0;
    auto step = [&](const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            f.key(queue[at++]);
        }
    };
    auto settle = [&] {
        for (int i = 0; i < 8; i++) {
            app.advanceTimers(0.05);
            app.tick();
        }
        f.draw(screen);
    };
    auto shows = [&](const std::string& s) { return screen.lines().find(s) != std::string::npos; };
    app.nestedLoop = step;
    settle();
    TrackPtr t = w->project.addTrack(Kind::Audio);
    ClipPtr c = makeAudioClip("Vox", dc(2 * w->engine.sr), w->engine.sr, w->engine.sr);
    c->track = t;
    w->project.addClip(c);
    w->timeline->relayout();
    w->timeline->selection.selectOnly(c);
    f.pane = Front::Pane::Timeline;
    settle();
    // F2: the clip's menu; its Gain envelope points… chosen
    queue = {Key{plat::Key_F2, 0, ""}};
    at = 0;
    step([] { return false; });
    CHECK(f.menuOpen());
    bool found = false;
    if (f.menuOpen())
        for (int i = 0; i < (int)f.open.back().menu->actions().size(); i++)
            if (f.open.back().menu->actions()[(size_t)i]->text == "Gain envelope points…") {
                f.open.back().item = i;
                found = true;
            }
    CHECK(found);
    bool sawDialog = false, typedByKeys = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        f.draw(screen);
        sawDialog = shows("Gain envelope: Vox") && shows("no points");
        std::ofstream(TMP + "/tui-dialog.txt") << screen.lines();
        GainEnvelopeDialog* d = nullptr;
        for (ui::Window* win : app.windows())
            if (auto* g = dynamic_cast<GainEnvelopeDialog*>(win)) d = g;
        if (d) {  // (typed: a time, Tab, a gain, Return adds it; then more, one changed, one added and removed)
            for (const Key& k : std::vector<Key>{{'A', plat::ModCtrl, ""}, {'0', 0, "0"}, {'.', 0, "."}, {'5', 0, "5"},
                                                 {plat::Key_Tab, 0, ""}, {'A', plat::ModCtrl, ""}, {'-', 0, "-"},
                                                 {'6', 0, "6"}, {plat::Key_Return, 0, ""}})
                queue.push_back(k);
            step(done);
            typedByKeys = d->points.size() == 1 && std::fabs(d->points[0].t - 0.5 * w->engine.sr) < 1 &&
                          std::fabs(clip_envelope::dbOf(d->points[0].v) + 6) < 1e-6;
            d->time->setValue(1.5);
            d->gain->setValue(12);
            d->addPoint();
            d->time->setValue(1.0);
            d->addPoint();
            d->list->setCurrentRow(1);
            d->removePoint();
            d->list->setCurrentRow(0);
            d->gain->setValue(-3);
            d->changePoint();
            f.draw(screen);
            std::ofstream(TMP + "/tui-dialog-points.txt") << screen.lines();
            d->accept();  // (OK)
        }
    };
    queue = {Key{plat::Key_Return, 0, ""}};
    at = 0;
    step([] { return false; });
    app.nestedLoop = step;
    settle();
    CHECK(sawDialog && typedByKeys);
    CHECK(c->gainEnvelope && c->gainEnvelope->size() == 2);
    if (c->gainEnvelope && c->gainEnvelope->size() == 2) {
        CHECK_NEAR((*c->gainEnvelope)[0].t, 0.5 * w->engine.sr, 1);
        CHECK_NEAR(clip_envelope::dbOf((*c->gainEnvelope)[0].v), -3, 1e-6);
        CHECK_NEAR(clip_envelope::dbOf((*c->gainEnvelope)[1].v), 12, 1e-6);
    }
    f.secondsPerColumn = 0.05;  // (zoomed in: the clip wide enough for its title)
    f.draw(screen);
    std::ofstream(TMP + "/tui-env.txt") << screen.lines();
    CHECK(shows("Vox env"));
    // undone by the window's Ctrl+Z
    queue = {Key{'Z', plat::ModCtrl, ""}};
    at = 0;
    step([] { return false; });
    settle();
    CHECK(!c->gainEnvelope);
    app.nestedLoop = nullptr;
    delete w;
    app.paintWindows = true;
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testScale();
    testExtremes();
    testWalk();
    testEdits();
    testSaveLoadUndo();
    testStress();
    testTimeline();
    testTui();
    TEST_MAIN_END();
}
