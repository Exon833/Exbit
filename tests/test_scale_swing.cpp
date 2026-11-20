// The song's key and scale and its swing (ui/song_scale.h, audio/key_detect.h, midi_effects.h SWING), each
// in the worst case:
//   - testStraight: swing 0 % (the song's, a track's own, the 8ths) leaves the notes and their sound bit for
//     bit as they were without swing; committing it changes nothing
//   - testSwingMath: where swung notes land (16ths, 8ths, a track's own amount, a Steps row's own amount, a
//     Groove effect's rows), and Commit writing them into the notes as one undo step that sounds the same
//   - testPlaybackMatchesRender: swing heard in playback is what a render writes, sample for sample
//   - testSixtyFourTracks: the key, the scale, every clip's own scale and the swing (song, tracks, rows,
//     8ths / 16ths) switched over and over while 64 MIDI tracks play on a thread of their own
//   - testDetection: 10,000 random note sets ranked on every thread, fast, the clear major and minor ones
//     found; natural major / minor first on a tie; nonsense notes
//   - testSaveLoadUndo: a clip's own scale, a track's swing and the song's saved, loaded (older files: the
//     defaults) and undone
//   - testTui: the terminal's Key and Swing controls, and a Steps row's swing from its F2 menu
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <thread>

#include "audio/engine.h"
#include "audio/key_detect.h"
#include "audio/midi_effects.h"
#include "audio/render.h"
#include "audio/workers.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/term.h"
#include "ui/main_window.h"
#include "ui/midi_editor.h"
#include "ui/song_scale.h"
#include "ui/timeline.h"
#include "ui/toolkit/widget.h"
#include "ui/transport_bar.h"

namespace {
constexpr int SR = 48000;
const std::string TMP = "build/test-scale-swing";

std::mt19937& rng() {
    static std::mt19937 r(1234);
    return r;
}
int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }

ClipPtr midiClip(Project& p, const TrackPtr& t, std::vector<Note> notes, double lengthS, int64_t start = 0) {
    auto c = makeMidiClip("notes", std::move(notes), lengthS, SR);
    c->track = t;
    c->start = start;
    c->prepare(SR);
    p.addClip(c);
    return c;
}

// every 16th (120 BPM: 0.125 s) of `seconds`, a little shorter than a step
std::vector<Note> sixteenths(double seconds, int pitch = 60) {
    std::vector<Note> ns;
    for (int i = 0; i * 0.125 < seconds; i++) ns.push_back({i * 0.125, i * 0.125 + 0.1, (double)pitch, 100});
    return ns;
}

TrackPtr midiTrack(Project& p) {
    TrackPtr t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    return t;
}

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::memcmp(&a[i].start, &b[i].start, sizeof(double) * 4) != 0 || a[i].chance != b[i].chance ||
            a[i].repeats != b[i].repeats || a[i].slide != b[i].slide || a[i].pan != b[i].pan || a[i].fine != b[i].fine)
            return false;
    return true;
}

ui::Action* findAction(ui::Menu& m, const std::string& start) {
    for (auto& a : m.actions()) {
        if (a->text.compare(0, start.size(), start) == 0) return a.get();
        if (a->submenu)
            if (ui::Action* found = findAction(*a->submenu, start)) return found;
    }
    return nullptr;
}
}  // namespace

static void testStraight() {
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = midiTrack(p);
    std::vector<Note> notes;
    for (int i = 0; i < 400; i++) {  // (dense, odd times, with their own properties)
        Note n{pick(0, 3999) / 1000.0, 0, (double)pick(30, 90), (double)pick(1, 127)};
        n.end = n.start + pick(1, 500) / 1000.0;
        if (i % 7 == 0) n.repeats = 3;
        if (i % 11 == 0) n.chance = 50;
        if (i % 13 == 0) n.slide = true;
        notes.push_back(n);
    }
    ClipPtr c = midiClip(p, t, notes, 4.5, SR / 3);
    // as before swing existed: no hook at all
    auto hook = clip_hooks::midiEffects;
    clip_hooks::midiEffects = nullptr;
    Clip plain = *c;
    plain.prepare(SR);
    clip_hooks::midiEffects = hook;
    for (int round = 0; round < 3; round++) {
        if (round == 1) t->swing = 0;             // (the track's own 0 %)
        if (round == 2) p.timing.swingGrid = 0;   // (the 8ths, still 0 %)
        CHECK(!midi_effects::swings(*t));
        song_scale::reprepare(p, SR);
        CHECK(sameNotes(*c->played, *plain.played));
        CHECK(c->data && plain.data && c->data->samples == plain.data->samples);
        CHECK(c->events->size() == plain.events->size());
        CHECK(sameNotes(midi_effects::apply(*t, *c->notes, 0.3), *c->notes));  // (not even sorted)
    }
    auto before = c->notes;
    CHECK(song_scale::commitSwing(p, SR) == 0 && c->notes == before);
}

static void testSwingMath() {
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = midiTrack(p);
    ClipPtr c = midiClip(p, t, sixteenths(2.0), 2.0);
    auto starts = [&] {
        std::vector<double> s;
        for (const Note& n : *c->played) s.push_back(n.start);
        return s;
    };
    // 100 % of the 16ths: each second 16th at three quarters of its pair (0.1875 s into it)
    song_scale::setSongSwing(p, SR, 100, 1);
    std::vector<double> s = starts();
    CHECK(s.size() == 16);
    for (size_t i = 0; i < s.size(); i++) CHECK_NEAR(s[i], (i / 2) * 0.25 + (i % 2 ? 0.1875 : 0), 1e-9);
    CHECK(c->events && (*c->events)[1].on == (int64_t)std::nearbyint(0.1875 * SR));
    // the 8ths: the off-beat 8th (0.25 s) to 0.375 s; the 16ths between in proportion
    song_scale::setSongSwing(p, SR, -1, 0);
    s = starts();
    CHECK_NEAR(s[2], 0.375, 1e-9);
    CHECK_NEAR(s[1], 0.1875, 1e-9);  // (0.125 s: half the way to the off-beat, which moved by half)
    CHECK_NEAR(s[4], 0.5, 1e-9);     // (on the beat: stays)
    // the track's own amount: 50 % of the 16ths' pair → +0.03125 s; 0 % straight while the song swings
    song_scale::setSongSwing(p, SR, -1, 1);
    song_scale::setTrackSwing(p, SR, t, 50);
    CHECK_NEAR(starts()[1], 0.125 + 0.03125, 1e-9);
    song_scale::setTrackSwing(p, SR, t, 0);
    CHECK_NEAR(starts()[1], 0.125, 1e-9);
    // a Steps row's own amount: only its pitch swings (a straight Groove added to keep it)
    std::vector<Note> two = sixteenths(1.0, 36), hats = sixteenths(1.0, 42);
    two.insert(two.end(), hats.begin(), hats.end());
    ClipPtr drums = midiClip(p, t, two, 1.0);
    song_scale::setRowSwing(p, SR, t, 42, 100);
    CHECK(t->midiEffects.size() == 1 && t->midiEffects[0].kind == "Groove" && midi_effects::rowSwing(*t, 42) == 100);
    for (const Note& n : *drums->played) {
        int step = (int)std::lround(n.start / 0.0625);  // (in 16th-halves)
        if (n.pitch == 36) CHECK(step % 2 == 0);           // straight
        if (n.pitch == 42 && (step % 4) == 3) CHECK_NEAR(std::fmod(n.start, 0.25), 0.1875, 1e-9);
    }
    // the Groove's own swing leaves that row to it; the other rows get the Groove's
    std::vector<MidiEffect> fx = t->midiEffects.copy();
    fx[0] = fx[0].with("swing", 75);
    t->midiEffects = fx;
    song_scale::reprepare(p, SR, {t});
    int swungKicks = 0;
    for (const Note& n : *drums->played)
        if (n.pitch == 36 && std::fabs(std::fmod(n.start, 0.25) - 0.1875) < 1e-9) swungKicks++;
    CHECK(swungKicks == 4);
    // a row back to the track's; the last row amount gone: no rows left
    song_scale::setRowSwing(p, SR, t, 42, -1);
    CHECK(midi_effects::rowSwing(*t, 42) < 0);
    // Commit: the swing written into the notes, played straight after; it sounds the same; one undo step
    t->midiEffects = std::vector<MidiEffect>{};
    song_scale::setTrackSwing(p, SR, t, -1);
    song_scale::setSongSwing(p, SR, 60, 1);
    auto heard = *c->played;
    auto sound = c->data->samples;
    history::History h(p, SR);
    CHECK(song_scale::commitSwing(p, SR) == 2);
    CHECK(p.timing.swing == 0 && t->swing < 0);
    CHECK(!midi_effects::swings(*t));
    CHECK(c->played->size() == heard.size());
    for (size_t i = 0; i < heard.size(); i++) CHECK_NEAR((*c->played)[i].start, heard[i].start, 1e-9);
    CHECK(c->data->samples.size() == sound.size());
    CHECK(h.check(SR) && h.labels().back() == "Swing");
    CHECK(h.undo(SR));
    CHECK(p.timing.swing == 60 && (*c->notes)[1].start == 0.125);
    CHECK(c->played->size() == heard.size() && (*c->played)[1].start == heard[1].start);
}

static void testPlaybackMatchesRender() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    TrackPtr a = midiTrack(p), b = midiTrack(p);
    midiClip(p, a, sixteenths(6.0, 60), 6.0);
    midiClip(p, b, sixteenths(6.0, 67), 6.0, SR / 2);
    song_scale::setSongSwing(p, SR, 50, 1);
    song_scale::setTrackSwing(p, SR, b, 75);
    const int block = config::RENDER_BLOCK;
    const int64_t start = SR * 2, end = SR * 5;
    e.seek(0);
    e.play();
    std::vector<float> played, buf((size_t)block * 2);
    while ((int64_t)played.size() / 2 < end + SR) {
        e.callback(nullptr, buf.data(), block);
        played.insert(played.end(), buf.begin(), buf.end());
    }
    e.stop();
    auto rendered = render::renderMix(p, start, end, SR);
    CHECK(rendered && (int64_t)rendered->size() == (end - start) * 2);
    if (!rendered) return;
    int64_t differ = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++)
        if ((*rendered)[(size_t)j] != played[(size_t)((start + e.latency) * 2 + j)]) differ++;
    CHECK(differ == 0);
    // and it is swung: straight renders otherwise
    song_scale::setSongSwing(p, SR, 0);
    song_scale::setTrackSwing(p, SR, b, -1);
    auto straight = render::renderMix(p, start, end, SR);
    CHECK(straight && *straight != *rendered);
}

static void testSixtyFourTracks() {
    Project p;
    audio::Engine e(p);
    e.offline = true;
    std::vector<TrackPtr> tracks;
    std::vector<ClipPtr> clips;
    for (int i = 0; i < 64; i++) {
        tracks.push_back(midiTrack(p));
        std::vector<Note> ns;
        for (int k = 0; k < 64; k++) {
            double s = pick(0, 15) * 0.125;
            ns.push_back({s, s + 0.1, (double)pick(36, 84), (double)pick(40, 127)});
        }
        clips.push_back(midiClip(p, tracks.back(), ns, 2.0));
    }
    std::atomic<bool> done{false};
    bool finite = true;
    e.play();
    std::thread audio([&] {
        std::vector<float> out(256 * 2);
        while (!done) {
            e.callback(nullptr, out.data(), 256);
            for (float v : out)
                if (!std::isfinite(v)) finite = false;
        }
    });
    auto t0 = std::chrono::steady_clock::now();
    for (int round = 0; round < 24; round++) {
        p.timing.key = round % 12;
        p.timing.scale = midi_effects::SCALES[(size_t)round % midi_effects::SCALES.size()].first;
        for (int k = 0; k < 16; k++) {
            ClipPtr c = clips[(size_t)pick(0, 63)];
            c->key = pick(-1, 11);
            c->scale = c->key < 0 ? "" : midi_effects::SCALES[(size_t)pick(0, 7)].first;
        }
        switch (round % 4) {
        case 0: song_scale::setSongSwing(p, SR, pick(0, 100), pick(0, 1)); break;
        case 1: song_scale::setTrackSwing(p, SR, tracks[(size_t)pick(0, 63)], pick(-1, 100)); break;
        case 2: song_scale::setRowSwing(p, SR, tracks[(size_t)pick(0, 63)], pick(36, 84), pick(-1, 100)); break;
        case 3:
            for (auto& c : clips) {
                CHECK(song_scale::keyOf(p, *c) == (c->key >= 0 ? c->key : p.timing.key));
            }
            break;
        }
    }
    song_scale::commitSwing(p, SR);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    done = true;
    audio.join();
    e.stop();
    CHECK(finite);
    CHECK(seconds < 20);
    printf("64 tracks: 24 changes of key, scale and swing while playing, and a commit: %.2f s\n", seconds);
    // what the workers prepared is what a clip prepared alone gets
    for (int i = 0; i < 64; i += 9) {
        Clip alone = *clips[(size_t)i];
        alone.prepare(SR);
        CHECK(sameNotes(*alone.played, *clips[(size_t)i]->played));
        CHECK(alone.data->samples == clips[(size_t)i]->data->samples);
    }
}

static void testDetection() {
    // clear major and minor sets (the tonic triad heard most, the rest of the scale too), 10,000 of them
    const int SETS = 10000;
    std::vector<std::vector<Note>> sets(SETS);
    std::vector<std::pair<int, std::string>> truth(SETS);
    for (int i = 0; i < SETS; i++) {
        int root = pick(0, 11);
        bool minor = pick(0, 1);
        const std::vector<int>& steps = midi_effects::SCALES[minor ? 1 : 0].second;
        truth[(size_t)i] = {root, minor ? "Minor" : "Major"};
        int count = pick(24, 96);  // (clear: the whole scale heard, its tonic triad most)
        for (int k = 0; k < count; k++) {
            int degree = k < 7 ? k : k % 2 == 0 ? std::vector<int>{0, 2, 4}[(size_t)(k / 2 % 3)] : pick(0, 6);
            double s = pick(0, 63) * 0.125, len = pick(2, 4) * 0.125;
            sets[(size_t)i].push_back({s, s + len, (double)(root + steps[(size_t)degree] + 12 * pick(3, 6)), 100});
        }
    }
    std::vector<key_detect::Candidate> best(SETS);
    struct Ctx {
        std::vector<std::vector<Note>>* sets;
        std::vector<key_detect::Candidate>* best;
    } ctx{&sets, &best};
    auto t0 = std::chrono::steady_clock::now();
    {
        workers::Pool pool(workers::workersFor(0), false);  // (every core)
        pool.run(SETS, [](void* c, int i) {
            Ctx& x = *static_cast<Ctx*>(c);
            auto top = key_detect::detect((*x.sets)[(size_t)i], 1);
            if (!top.empty()) (*x.best)[(size_t)i] = top[0];
        }, &ctx);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int right = 0;
    std::map<std::string, int> wrong;  // (what the misses were taken for, relative to the truth)
    for (int i = 0; i < SETS; i++) {
        bool ok = best[(size_t)i].key == truth[(size_t)i].first && best[(size_t)i].scale == truth[(size_t)i].second;
        right += ok;
        if (!ok) wrong[truth[(size_t)i].second + " as " + std::to_string((best[(size_t)i].key - truth[(size_t)i].first + 12) % 12) + " " + best[(size_t)i].scale]++;
    }
    for (auto& kv : wrong) printf("  missed: %s x%d\n", kv.first.c_str(), kv.second);
    printf("detection: %d of %d right in %.3f s\n", right, SETS, seconds);
    CHECK(right >= SETS * 97 / 100);  // (the misses: a relative major / minor, where the random notes blur it)
    CHECK(seconds < 2);
    // a tie (every note of C major alike: also D Dorian's, G Mixolydian's…): natural major or minor first
    std::vector<Note> plain;
    for (int s : midi_effects::SCALES[0].second) plain.push_back({0, 1, (double)(60 + s), 100});
    auto ranked = key_detect::detect(plain, 5);
    CHECK(ranked.size() == 5);
    CHECK((ranked[0].scale == "Major" && ranked[0].key == 0) || (ranked[0].scale == "Minor" && ranked[0].key == 9));
    CHECK(key_detect::name(ranked[0]) == "C major" || key_detect::name(ranked[0]) == "A minor");
    // the scores are in order
    auto all = key_detect::rank(plain);
    CHECK(all.size() == 12 * midi_effects::SCALES.size());
    CHECK(std::is_sorted(all.begin(), all.end(), [](auto& x, auto& y) { return x.score > y.score; }));
    // nonsense: nothing, every pitch class alike, NaN and out-of-range pitches, no length
    CHECK(key_detect::detect({}).empty());
    std::vector<Note> chromatic;
    for (int k = 0; k < 12; k++) chromatic.push_back({0, 1, (double)(60 + k), 100});
    CHECK(key_detect::detect(chromatic).empty());
    CHECK(key_detect::detect({{0, 1, NAN, 100}, {0, 1, -5, 100}, {0, 1, 400, 100}}).empty());
    CHECK(!key_detect::detect({{0, 0, 60, 100}, {0, 0, 64, 100}, {0, 0, 67, 100}}).empty());
}

static void testSaveLoadUndo() {
    util::makeDirs(TMP);
    Project p;
    midi_effects::install(&p.timing);
    TrackPtr t = midiTrack(p);
    ClipPtr own = midiClip(p, t, sixteenths(1.0), 1.0), plain = midiClip(p, t, sixteenths(1.0), 1.0, SR);
    own->key = 9;
    own->scale = "Minor";
    t->swing = 33;
    p.timing.key = 2;
    p.timing.scale = "Dorian";
    p.timing.swing = 25;
    p.timing.swingGrid = 0;
    project_io::MasterSettings master;
    std::string error, path = TMP + "/song.json";
    CHECK(project_io::saveProject(path, p, SR, master, error));
    Project q;
    CHECK(project_io::loadProject(path, q, SR, master, error));
    CHECK(q.clips.size() == 2 && q.clips[0]->key == 9 && q.clips[0]->scale == "Minor" && q.clips[1]->key == -1);
    CHECK(q.tracks[0]->swing == 33 && q.timing.swing == 25 && q.timing.swingGrid == 0 && q.timing.scale == "Dorian");
    CHECK(song_scale::scaleOf(q, *q.clips[0]) == "Minor" && song_scale::scaleOf(q, *q.clips[1]) == "Dorian");
    // prepared swung as loaded (its own swing, not the project open before): the off-beat 8th at 0.25 s later
    CHECK(q.clips[0]->played && q.clips[0]->played->size() == 8);
    if (q.clips[0]->played && q.clips[0]->played->size() == 8) CHECK_NEAR((*q.clips[0]->played)[2].start, 0.25 + 0.5 * 33 / 400, 1e-9);
    // an older file (none of the keys) and a damaged one (a key out of range, a scale unknown): the defaults
    std::string text;
    CHECK(util::readFile(path, text));
    const std::vector<std::pair<std::string, std::string>> damage = {
        {"\"swing\": 33.0", "\"x\": 1"},                         // (the track's: none)
        {"\"key\": 9,", "\"key\": 99,"},                          // (the clip's: out of range)
        {"\"scale\": \"Minor\"", "\"scale\": \"Lydian bebop\""},  // (…and unknown)
        {"\"swing\": 25.0,", "\"swing\": -4,"},                   // (the song's: out of range)
        {"\"swing_grid\": 0", "\"x\": 1"}};                      // (none)
    for (auto& [from, to] : damage) {
        size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) text.replace(at, from.size(), to);
    }
    std::ofstream(TMP + "/old.json") << text;
    Project r;
    CHECK(project_io::loadProject(TMP + "/old.json", r, SR, master, error));
    CHECK(r.clips.size() == 2 && r.clips[0]->key == -1 && r.clips[0]->scale.empty());
    CHECK(r.tracks[0]->swing == -1 && r.timing.swing == 0 && r.timing.swingGrid == 1);
    // undo: a clip's own scale, a track's swing
    midi_effects::install(&p.timing);
    history::History h(p, SR);
    plain->key = 4;
    plain->scale = "Major";
    CHECK(h.check(SR) && h.labels().back() == "Clip scale");
    song_scale::setTrackSwing(p, SR, t, 80);
    CHECK(h.check(SR) && h.labels().back() == "Swing");
    CHECK(h.undo(SR) && t->swing == 33);
    CHECK(h.undo(SR) && plain->key == -1 && plain->scale.empty());
    CHECK(h.redo(SR) && plain->key == 4 && plain->scale == "Major");
    // the clip menu's Scale ▸: its own, detected, the song's again
    ui::Menu menu;
    song_scale::addClipScaleMenu(menu, p, {plain});
    ui::Action* song = findAction(menu, "The song's");
    CHECK(song && !song->checked);
    ui::Action* detected = findAction(menu, "C major");  // (its notes: all C)
    CHECK(detected != nullptr);
    if (detected) ui::Menu::activate(detected);
    CHECK(plain->key == 0 && plain->scale == "Major");
    if (song) ui::Menu::activate(song);
    CHECK(plain->key == -1);
}

static void testTui() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    tui::Screen screen(160, 30);
    std::vector<tui::Key> queue;
    size_t at = 0;
    std::string seen, watched;  // (the screen once it showed `watched`, through dialogs and menus too)
    auto step = [&](const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            f.key(queue[at++]);
            if (!watched.empty()) {
                f.draw(screen);
                if (screen.lines().find(watched) != std::string::npos) seen = screen.lines();
            }
        }
    };
    app.nestedLoop = [&](const std::function<bool()>& done) { step(done); };
    auto keys = [&](std::vector<tui::Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) {
            app.advanceTimers(0.05);
            app.tick();
        }
        f.draw(screen);
    };
    auto K = [](int k) { return tui::Key{k, 0, ""}; };
    TrackPtr t = w->timeline->addTrack(Kind::Midi);
    t->needsInstrument = false;
    std::vector<Note> ns = sixteenths(1.0, 36);
    ClipPtr c = makeMidiClip("beat", ns, 1.0, w->engine.sr);
    c->track = t;
    c->prepare(w->engine.sr);
    w->project.addClip(c);
    // the transport: Left from Play wraps to Swing, then Key
    f.pane = tui::Front::Pane::Transport;
    // the transport: Left from Play wraps to Sync, then Swing, then Key
    keys({K(plat::Key_Left), K(plat::Key_Left), K(plat::Key_Left), K(plat::Key_Up), K(plat::Key_Up)});
    CHECK(w->project.timing.key == 2 && w->project.timing.scale == "Major");
    CHECK(screen.lines().find("Key D major") != std::string::npos);
    CHECK(w->transport->keyButton->text() == "Key: D major \xe2\x96\xbe");
    // Enter: its menu (the window's own), Detect among it
    keys({K(plat::Key_Return)});
    std::ofstream(TMP + "/tui-key-menu.txt") << screen.lines();
    CHECK(f.menuOpen() && screen.lines().find("Detect from every MIDI clip") != std::string::npos);
    keys({K(plat::Key_Escape)});
    CHECK(!f.menuOpen());
    // Swing: Up twice → 10 %, the clip prepared swung
    auto before = c->played;
    keys({K(plat::Key_Right), K(plat::Key_Up), K(plat::Key_Up)});
    CHECK(w->project.timing.swing == 10 && c->played != before);
    CHECK(screen.lines().find("Swing 10 %") != std::string::npos);
    keys({K(plat::Key_Down), K(plat::Key_Down), K(plat::Key_Down)});
    CHECK(w->project.timing.swing == 0);
    keys({K(plat::Key_F2)});
    CHECK(f.menuOpen() && screen.lines().find("Commit swing into the notes") != std::string::npos);
    keys({K(plat::Key_Escape)});
    // a Steps row's F2 menu: the row at the cursor, its own swing chosen by keys
    w->openMidiEditor(c);
    MidiEditor* ed = w->midiEditors.count(c.get()) ? w->midiEditors[c.get()] : nullptr;
    CHECK(ed != nullptr);
    if (ed) {
        // the piano roll snapping to the clip's own scale (C major): a new note and moved ones; Alt: outside it
        c->key = 0;
        c->scale = "Major";
        ed->syncScaleTools();
        CHECK(ed->scaleBox->currentData() == "Major" && ed->scale() && ed->scale()->count(1) == 0);
        ed->snapScaleButton->setChecked(true);
        ed->keyT = 1.0;
        ed->keyPitch = 61;  // (C#: between C and D, as near: the lower)
        ed->keyNote();
        CHECK(!ed->notes.empty() && ed->notes.back().pitch == 60);
        ed->selected.clear();
        for (int i = 0; i < (int)ed->notes.size(); i++)
            if (ed->notes[(size_t)i].start == 1.0) ed->selected.insert(i);
        CHECK(ed->selected.size() == 1);
        auto pitchOf = [&] {  // (the note moved: found again by its time, as commit sorts them)
            for (const Note& n : ed->notes)
                if (n.start == 1.0) return (int)n.pitch;
            return -1;
        };
        ed->nudge(plat::Key_Up, false, 0);
        CHECK(pitchOf() == 62);  // (D)
        ed->nudge(plat::Key_Up, false, 0);
        ed->nudge(plat::Key_Up, false, 0);
        CHECK(pitchOf() == 65);  // (E, then F: no sharps on the way)
        ed->nudge(plat::Key_Up, false, plat::ModAlt);
        CHECK(pitchOf() == 66);  // (Alt: F#, outside it)
        ed->nudge(plat::Key_Down, false, 0);
        CHECK(pitchOf() == 65);
        ed->snapScaleButton->setChecked(false);
        ed->nudge(plat::Key_Up, false, 0);
        CHECK(pitchOf() == 66);  // (off: every semitone)
        c->key = -1;
        c->scale.clear();
        ed->setStepsView(true);
        ed->keyPitch = 36;
        ed->steps->setFocus();
        watched = "Swing of this row";
        queue = {K(plat::Key_F2), K(plat::Key_Down), K(plat::Key_Down), K(plat::Key_Right), K(plat::Key_Down),
                 K(plat::Key_Down), K(plat::Key_Down), K(plat::Key_Down), K(plat::Key_Down), K(plat::Key_Down),
                 K(plat::Key_Return)};
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) app.tick();
        std::ofstream(TMP + "/tui-row-menu.txt") << seen;
        CHECK(!seen.empty());
        CHECK(midi_effects::rowSwing(*t, 36) > 0);
        printf("row 36's own swing from the TUI: %g %%\n", midi_effects::rowSwing(*t, 36));
        ed->close();
    }
    w->close();
    for (int i = 0; i < 4; i++) app.tick();
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testStraight();
    testSwingMath();
    testPlaybackMatchesRender();
    testSixtyFourTracks();
    testDetection();
    testSaveLoadUndo();
    testTui();
    TEST_MAIN_END();
}
