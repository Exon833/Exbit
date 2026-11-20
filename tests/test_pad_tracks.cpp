// Pads on their own tracks (model/pad_tracks.h; mixer.h PADS ON THEIR OWN TRACKS), the worst cases: 32 pads on 32
// tracks playing a 1/64 roll with sidechains between the pad tracks, mixed on one thread and on every core (bit for
// bit the same), and while pads are routed and unrouted from another thread mid-playback; pad tracks at unity
// summing to exactly the kit's sound before routing; chokes across tracks (clips and live notes); live notes
// reaching pad tracks; Pads to tracks making the group and the tracks; freezing a pad track; save, load, undo and
// damaged references.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "audio/engine.h"
#include "audio/live.h"
#include "audio/mixer.h"
#include "audio/padkit.h"
#include "audio/render.h"
#include "audio/sampler.h"
#include "audio/workers.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/history.h"
#include "model/pad_tracks.h"
#include "model/project_io.h"
#include "model/routing.h"
#include "plugins/plugin.h"

namespace {
const int SR = 48000;
const std::string kDir = "build/test-tmp/pad-tracks";

// a drum-like hit (decaying noise, each pad its own), `frames` long
ClipPtr hit(int index, int64_t frames, float level = 0.6f) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    uint32_t seed = 12345u + (uint32_t)index * 7919u;
    for (int64_t i = 0; i < frames; i++) {
        seed = seed * 1664525u + 1013904223u;
        float n = ((float)(seed >> 8) / (1 << 24) - 0.5f) * 2;
        float env = level * std::exp(-(float)i / (frames / 4.0f));
        b->samples[(size_t)i * 2] = n * env;
        b->samples[(size_t)i * 2 + 1] = n * env * 0.7f + 0.01f * (float)index;
    }
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = "hit" + std::to_string(index + 1) + ".wav";
    c->source = b;
    c->sourceSr = SR;
    return c;
}

// a constant sample (choke tests: its level is easy to read)
ClipPtr constant(int64_t frames, float value, const std::string& name) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.assign((size_t)frames * 2, value);
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = name;
    c->source = b;
    c->sourceSr = SR;
    return c;
}

// every pad used, pads 1/2 and 3/4 in choke groups
PadKitPtr kit32() {
    auto kit = std::make_shared<PadKit>();
    for (int i = 0; i < PadKit::MAX_PADS; i++) kit->pads[(size_t)i] = padkit::newPad(hit(i, 1200 + 97 * i), kit->pads[(size_t)i].note);
    kit->pads[0].choke = kit->pads[1].choke = 1;
    kit->pads[2].choke = kit->pads[3].choke = 2;
    return kit;
}

Note noteAt(double start, double end, int pitch, double velocity = 100) {
    Note n;
    n.start = start, n.end = end, n.pitch = pitch, n.velocity = velocity;
    return n;
}

// a 1/64 roll over every pad (at 120 BPM: 1/64 = 31.25 ms) for `seconds`, starting a little after 0 (the clip's
// edge fade never touches a hit)
std::vector<Note> roll(double seconds) {
    std::vector<Note> notes;
    int k = 0;
    for (double t = 0.01; t < seconds; t += 0.03125, k++) {
        notes.push_back(noteAt(t, t + 0.02, PadKit::FIRST_NOTE + k % PadKit::MAX_PADS, 40 + (k * 37) % 88));
        if (k % 5 == 0) notes.push_back(noteAt(t, t + 0.02, PadKit::FIRST_NOTE + (k * 7) % PadKit::MAX_PADS, 90));
    }
    return notes;
}

struct Kit {
    Project p;
    TrackPtr kit;
    ClipPtr clip;
    explicit Kit(PadKitPtr pads = kit32(), std::vector<Note> notes = roll(2.0), double length = 2.0) {
        kit = p.addTrack(Kind::Midi);
        kit->name = "Drums";
        kit->needsInstrument = false;
        kit->padKit = pads;
        clip = makeMidiClip("Roll", notes, length, SR);
        clip->track = kit;
        clip->start = SR / 10;
        clip->prepare(SR);
        p.addClip(clip);
    }
    void render() { clip->prepare(SR); }
};

std::vector<float> mixAll(Project& p, workers::Pool* pool, int64_t total, const live::LiveMap* live = nullptr) {
    mixer::MixState st;
    st.pool = pool;
    std::vector<float> all, buf;
    const int sizes[] = {256, 333, 1024, 64, 517};
    int64_t pos = 0;
    for (size_t k = 0; pos < total; k++) {
        int n = sizes[k % 5];
        buf.assign((size_t)n * 2, 0.0f);
        mixer::mixClips(p, buf.data(), 2, n, true, pos, st, nullptr, SR, k == 0, true, live);
        all.insert(all.end(), buf.begin(), buf.end());
        pos += n;
    }
    return all;
}

int64_t differing(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1;
    int64_t d = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i]) d++;
    return d;
}

double loudest(const std::vector<float>& a, size_t from = 0, size_t to = SIZE_MAX) {
    double m = 0;
    for (size_t i = from; i < std::min(to, a.size()); i++) m = std::max(m, (double)std::fabs(a[i]));
    return m;
}

SlotPtr builtinSlot(const char* id) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->instance = fx;
    return s;
}

// a compressor on `t` whose sidechain listens to `source`
void sidechainFrom(const TrackPtr& t, const TrackPtr& source) {
    auto comp = builtinSlot("compressor");
    t->effects = std::vector<SlotPtr>{comp};
    std::vector<Cable> cables = chain({comp});
    cables.push_back(Cable(End::source(source.get()), End::sidechain(comp.get())));
    t->cables = cables;
}
}  // namespace

// Pads to tracks: a track per used pad, named after its sound, below the kit, grouped with it in a group named
// after the kit (a bus); again: nothing more
static void testPadsToTracks() {
    Kit k;
    auto other = k.p.addTrack(Kind::Audio);  // (below the kit: the pad tracks go between them)
    auto kit = std::make_shared<PadKit>(*k.kit->padKit);
    kit->pads[5] = Pad();  // (an empty pad: no track)
    kit->pads[5].note = PadKit::FIRST_NOTE + 5;
    k.kit->padKit = kit;
    std::vector<TrackPtr> made = pad_tracks::padsToTracks(k.p, k.kit);
    CHECK(made.size() == 31);
    CHECK(k.kit->group && k.kit->group->name == "Drums" && k.kit->group->bus->name == "Drums");
    CHECK(made.size() == 31 && made[0]->name == "hit1 (pad 1)" && made[5]->name == "hit7 (pad 7)");
    CHECK(made[0]->kind == Kind::Audio && made[0]->group == k.kit->group);
    CHECK(k.p.indexOf(k.kit) == 0 && k.p.indexOf(made[0]) == 1 && k.p.indexOf(made.back()) == 31 &&
          k.p.indexOf(other) == 32);
    CHECK(pad_tracks::trackOf(*k.kit, 0) == made[0] && !pad_tracks::trackOf(*k.kit, 5));
    CHECK(pad_tracks::routedMask(*k.kit) == (0xFFFFFFFFu & ~(1u << 5)));
    std::vector<int> pads;
    CHECK(pad_tracks::kitOf(k.p, *made[2], &pads) == k.kit && pads == std::vector<int>{2});
    CHECK(pad_tracks::inputName(k.p, *made[2]) == "Drums pad 3" && pad_tracks::inputName(k.p, *other).empty());
    CHECK(routing::isPlain(*k.kit->group->bus));  // (a plain bus: skipped, nothing changes)
    CHECK(pad_tracks::padsToTracks(k.p, k.kit).empty());  // (again: every pad has one)
    // a second giveTrack for a pad with one: the same track; the group reused
    CHECK(pad_tracks::giveTrack(k.p, k.kit, 0) == made[0]);
    // back to the kit, then its own track again: a new track in the same group, right below the others
    pad_tracks::backToKit(k.p, *made[1]);
    CHECK(!pad_tracks::trackOf(*k.kit, 1) && !pad_tracks::kitOf(k.p, *made[1]));
    TrackPtr again = pad_tracks::giveTrack(k.p, k.kit, 1);
    CHECK(again && again != made[1] && again->group == k.kit->group && k.p.indexOf(again) == 32);
    // removing a pad track sends its pad back to the kit
    k.p.removeTrack(again);
    CHECK(!pad_tracks::trackOf(*k.kit, 1));
    // a kit already in a group of its own name keeps it; one in another group gets one inside it
    Kit nested;
    GroupPtr outer = nested.p.groupTracks({nested.kit});
    outer->name = "Beats";
    pad_tracks::giveTrack(nested.p, nested.kit, 0);
    CHECK(nested.kit->group && nested.kit->group->name == "Drums" && aload(nested.kit->group->parent) == outer);
}

// pad tracks at unity, no effects: exactly the kit's sound before routing, with and without workers; and only the
// last pads routed (the kit sums its pads in order, then their tracks follow)
static void testBitExact() {
    Kit k;
    const int64_t total = SR * 5 / 2;
    workers::Pool pool(workers::workersFor(0) > 0 ? std::min(7, workers::workersFor(0)) : 3, false);
    std::vector<float> before = mixAll(k.p, nullptr, total);
    std::vector<float> beforeWorkers = mixAll(k.p, &pool, total);
    CHECK(loudest(before) > 0.1);
    CHECK(differing(before, beforeWorkers) == 0);
    std::vector<TrackPtr> made = pad_tracks::padsToTracks(k.p, k.kit);
    k.render();
    AudioPtr data = k.clip->data;
    CHECK(data && data->pads.size() == 32);
    float rest = 0;  // (the kit's own sound: nothing left in it)
    for (float v : data->samples) rest = std::max(rest, std::fabs(v));
    CHECK(rest == 0.0f);
    std::vector<float> after = mixAll(k.p, nullptr, total);
    std::vector<float> afterWorkers = mixAll(k.p, &pool, total);
    CHECK(differing(before, after) == 0);
    CHECK(differing(before, afterWorkers) == 0);
    if (differing(before, after)) printf("routed: %lld samples differ\n", (long long)differing(before, after));
    // only pads 25…32 on their own tracks
    for (int i = 0; i < 24; i++) pad_tracks::setTrack(k.kit, i, nullptr);
    k.render();
    CHECK(k.clip->data->pads.size() == 8);
    CHECK(differing(before, mixAll(k.p, nullptr, total)) == 0);
    CHECK(differing(before, mixAll(k.p, &pool, total)) == 0);
    // a pad track muted: its pad is gone from the mix
    made[31]->muted = true;
    std::vector<float> without = mixAll(k.p, &pool, total);
    CHECK(differing(before, without) > 0);
    made[31]->muted = false;
    // the kit muted: its own pads go quiet, the pads on their own tracks still play
    k.kit->muted = true;
    std::vector<float> padsOnly = mixAll(k.p, &pool, total);
    CHECK(loudest(padsOnly) > 0.05 && differing(before, padsOnly) > 0);
    k.kit->muted = false;
    // a pad track removed: its pad plays in the kit again (before its clips are rendered again: kept apart still)
    k.p.removeTrack(made[31]);
    CHECK(!pad_tracks::trackOf(*k.kit, 31) && k.clip->data->pads.size() == 8);
    std::vector<float> fallback = mixAll(k.p, nullptr, total);
    double worst = 0;
    for (size_t i = 0; i < before.size(); i++) worst = std::max(worst, (double)std::fabs(before[i] - fallback[i]));
    CHECK(worst < 1e-5);  // (the same sound; only the order of a few sums differs)
    k.render();
    CHECK(k.clip->data->pads.size() == 7);
}

// chokes cut across tracks: pad 1 (long) on its own track is cut off by pad 2 (in the kit) of its choke group, in a
// clip and played live; live notes of a routed pad reach its track (the kit muted)
static void testChokesAndLive() {
    auto kit = std::make_shared<PadKit>();
    kit->pads[0] = padkit::newPad(constant(SR, 0.5f, "long.wav"), PadKit::FIRST_NOTE);
    kit->pads[1] = padkit::newPad(constant(SR, 0.25f, "short.wav"), PadKit::FIRST_NOTE + 1);
    for (int i = 0; i < 2; i++) kit->pads[(size_t)i].sound.velocity = 0, kit->pads[(size_t)i].choke = 1;
    Kit k(kit, {noteAt(0.1, 0.2, 36), noteAt(0.4, 0.5, 37)}, 1.0);
    k.clip->start = 0;
    TrackPtr own = pad_tracks::giveTrack(k.p, k.kit, 0);
    k.render();
    k.kit->muted = true;  // (only pad 1's track heard)
    std::vector<float> out = mixAll(k.p, nullptr, SR);
    auto atS = [&](const std::vector<float>& a, double s) { return a[(size_t)(s * SR) * 2]; };
    CHECK(std::fabs(atS(out, 0.3) - 0.5f) < 1e-6);
    CHECK(atS(out, 0.4 + 2 * live::CHOKE_S) == 0.0f);  // (cut off by pad 2, in the kit's own sound)
    // live: the kit's notes, pad 1's voice on its track, cut off by pad 2's
    k.p.removeClips({k.clip});
    auto notes = std::make_shared<live::LiveNotes>();
    notes->setRenderer(sampler::noteRenderer(*k.kit));
    live::LiveMap map{{k.kit.get(), notes}};
    mixer::MixState st;
    std::vector<float> buf(512 * 2);
    auto play = [&] {
        std::fill(buf.begin(), buf.end(), 0.0f);
        mixer::mixClips(k.p, buf.data(), 2, 512, false, 0, st, nullptr, SR, false, true, &map);
    };
    CHECK(notes->noteOn(36, 100, SR, true));
    play();
    CHECK(std::fabs(buf[300 * 2] - 0.5f) < 1e-6);  // (on its track: heard, the kit muted)
    play();
    CHECK(std::fabs(buf[100 * 2] - 0.5f) < 1e-6);
    CHECK(notes->noteOn(37, 100, SR, true));  // (pad 2, in the muted kit, chokes pad 1 on its track)
    play();
    CHECK(buf[(size_t)(2 * live::CHOKE_S * SR) * 2] == 0.0f && buf[0] > 0.1f);
    // back to the kit: live notes of pad 1 are the kit's again (muted: silent)
    pad_tracks::setTrack(k.kit, 0, nullptr);
    CHECK(notes->noteOn(36, 100, SR, true));
    play();
    CHECK(loudest(buf) == 0.0);
    k.kit->muted = false;
    CHECK(notes->noteOn(36, 100, SR, true));
    play();
    CHECK(loudest(buf) > 0.4);
    (void)own;
}

// The worst case: 32 pads on 32 tracks, each pad track's compressor listening to the pad track above it, a 1/64
// roll: one thread and every core the same, bit for bit; then mixed on every core while another thread routes and
// unroutes pads, renders the clip again and adds tracks, as the GUI would mid-playback
static void testStress() {
    // (two projects alike: the compressors keep what they heard)
    auto build = [](Kit& k) {
        std::vector<TrackPtr> made = pad_tracks::padsToTracks(k.p, k.kit);
        for (size_t i = 1; i < made.size(); i++) sidechainFrom(made[i], made[i - 1]);
        k.render();
        return made;
    };
    Kit k(kit32(), roll(4.0), 4.0), twin(kit32(), roll(4.0), 4.0);
    std::vector<TrackPtr> made = build(k);
    build(twin);
    workers::Pool pool(std::max(3, workers::workersFor(0)), false);
    const int64_t total = SR * 9 / 2;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> one = mixAll(twin.p, nullptr, total);
    std::vector<float> many = mixAll(k.p, &pool, total);
    CHECK(differing(one, many) == 0 && loudest(one) > 0.05);
    util::debugf("test_pad_tracks: 33 tracks, 32 sidechains, 4.5 s mixed twice in %.2f s\n",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    // mid-playback routing (this thread: the GUI; another: the audio thread)
    std::atomic<bool> done{false};
    std::atomic<int> buffers{0};
    std::atomic<bool> bad{false};
    std::thread audio([&] {
        mixer::MixState st;
        st.pool = &pool;
        std::vector<float> buf;
        int64_t pos = 0;
        const int sizes[] = {128, 512, 333};
        for (int k2 = 0; !done.load() || buffers.load() < 50; k2++) {
            int n = sizes[k2 % 3];
            buf.assign((size_t)n * 2, 0.0f);
            mixer::mixClips(k.p, buf.data(), 2, n, true, pos, st, nullptr, SR, k2 == 0);
            for (float v : buf)
                if (!std::isfinite(v) || std::fabs(v) > 64) bad = true;
            pos = (pos + n) % (SR * 4);
            buffers++;
        }
    });
    std::mt19937 rng(7);
    for (int step = 0; step < 40; step++) {
        int pad = (int)(rng() % PadKit::MAX_PADS);
        pad_tracks::setTrack(k.kit, pad, rng() % 2 ? made[(size_t)pad] : nullptr);
        if (step % 4 == 0) {  // (rendered again on a copy, swapped in: clip_edits' way)
            Clip copy = *k.clip;
            copy.prepare(SR);
            std::atomic_store(&k.clip->data, copy.data);
        }
        if (step == 20) pad_tracks::giveTrack(k.p, k.kit, 0);  // (has one, or a new track)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    done = true;
    audio.join();
    CHECK(!bad.load() && buffers.load() >= 50);
}

// save and load (by index), a damaged file's references cleaned, undo and redo of Pads to tracks; freezing a pad
// track renders its pad through its effects
static void testSaveLoadUndoFreeze() {
    util::makeDirs(kDir);
    Kit k(kit32(), roll(1.0), 1.0);
    auto first = k.p.addTrack(Kind::Audio);  // (above: indexes shift)
    std::vector<TrackPtr> ts = k.p.tracks.copy();
    std::rotate(ts.begin(), ts.end() - 1, ts.end());
    k.p.tracks = ts;
    std::vector<TrackPtr> made = pad_tracks::padsToTracks(k.p, k.kit);
    pad_tracks::setTrack(k.kit, 3, nullptr);
    k.render();
    project_io::MasterSettings master;
    std::string path = kDir + "/Pads.json", err;
    CHECK(project_io::saveProject(path, k.p, SR, master, err));
    std::string text;
    CHECK(util::readFile(path, text));
    Json data = Json::parse(text);
    CHECK(data["tracks"][size_t(1)]["pad_tracks"].size() == 31);
    Project q;
    CHECK(project_io::loadProject(path, q, SR, master, err));
    CHECK(q.tracks.size() == k.p.tracks.size());
    if (q.tracks.size() == k.p.tracks.size()) {
        TrackPtr kit = q.tracks[1];
        CHECK(kit->padKit && pad_tracks::routedMask(*kit) == (0xFFFFFFFFu & ~(1u << 3)));
        CHECK(pad_tracks::trackOf(*kit, 0) == q.tracks[2] && q.tracks[2]->name == "hit1 (pad 1)");
        CHECK(pad_tracks::trackOf(*kit, 31) && pad_tracks::trackOf(*kit, 31)->name == "hit32 (pad 32)");
        ClipPtr clip = q.clipsOn(kit).empty() ? nullptr : q.clipsOn(kit)[0];
        CHECK(clip && clip->data && clip->data->pads.size() == 31);  // (rendered on loading with its pads apart)
        CHECK(kit->group && kit->group->name == "Drums");
    }
    // a damaged file: a track that isn't there, the kit itself, a pad out of range, a return, junk, and another kit
    // claiming a track the first plays into
    Json tracks = data["tracks"];
    std::string damaged = text;
    size_t at = damaged.find("\"pad_tracks\"");
    CHECK(at != std::string::npos);
    size_t open = damaged.find('[', at), close = open;
    for (int depth = 0; close < damaged.size(); close++) {
        if (damaged[close] == '[') depth++;
        if (damaged[close] == ']' && --depth == 0) break;
    }
    damaged.replace(open, close - open + 1, "[[0, 999], [1, 1], [40, 2], [2, 3], \"x\", [4, -1], [5, 2.5], [6, 4]]");
    std::string bad = kDir + "/Damaged.json";
    CHECK(util::writeFile(bad, damaged));
    Project d;
    CHECK(project_io::loadProject(bad, d, SR, master, err));
    if (d.tracks.size() > 4) {
        TrackPtr kit = d.tracks[1];
        CHECK(pad_tracks::routedMask(*kit) == ((1u << 2) | (1u << 6)));
        CHECK(pad_tracks::trackOf(*kit, 2) == d.tracks[3] && pad_tracks::trackOf(*kit, 6) == d.tracks[4]);
        // another kit claiming pad 3's track: cleared (one kit per pad track)
        TrackPtr second = d.addTrack(Kind::Midi);
        second->needsInstrument = false;
        second->padKit = kit->padKit;
        pad_tracks::setTrack(second, 0, d.tracks[3]);
        CHECK(pad_tracks::forgetGone(d) == 1 && !second->padOut);
    }
    // undo and redo
    Kit u(kit32(), roll(1.0), 1.0);
    history::History h(u.p, SR);
    AudioPtr plain = u.clip->data;
    std::vector<TrackPtr> tracks2 = pad_tracks::padsToTracks(u.p, u.kit);
    u.render();
    CHECK(h.check(SR));
    CHECK(u.p.tracks.size() == 33 && u.clip->data->pads.size() == 32);
    pad_tracks::setTrack(u.kit, 4, nullptr);
    CHECK(h.check(SR) && h.labels().back() == "Pad output");
    CHECK(h.undo(SR) && pad_tracks::trackOf(*u.kit, 4) == tracks2[4]);
    CHECK(h.undo(SR));
    CHECK(u.p.tracks.size() == 1 && !u.kit->padOut && !u.kit->group && u.clip->data == plain);
    CHECK(h.redo(SR) && u.p.tracks.size() == 33 && pad_tracks::routedMask(*u.kit) == 0xFFFFFFFFu &&
          u.clip->data->pads.size() == 32);
    // freezing a pad track: its pad's sound (through its effects) from the kit's clip
    std::optional<render::Rendered> frozen = render::renderTrack(u.p, tracks2[0], SR);
    CHECK(frozen && loudest(frozen->audio) > 0.05);
    CHECK(!render::renderTrack(u.p, u.p.addTrack(Kind::Audio), SR));  // (any other empty track: nothing)
    (void)first;
}

int main() {
    sampler::install();  // (the clips' rendering through the Pad Kit)
    testPadsToTracks();
    testBitExact();
    testChokesAndLive();
    testStress();
    testSaveLoadUndoFreeze();
    TEST_MAIN_END();
}
