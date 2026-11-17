// Tests of the Pad Kit (audio/padkit.h, model.h PAD KIT): keys mapped to pads, choke groups, one-shot and
// held pads, live notes, render == playback, saving and loading (samples in the audio folder), undo, and
// slicing a drum loop into a kit (dsp::Onsets) whose clip replays it.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "audio/engine.h"
#include "audio/padkit.h"
#include "audio/patch.h"
#include "audio/render.h"
#include "audio/sampler.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"

namespace {
const int SR = 48000;
const std::string kDir = "build/test-tmp/padkit";

// a sample (stereo, at SR) of `frames` frames of `value`
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

// a kit with a constant sample on each of the first pads: pad i plays values[i] for frames[i] frames
PadKitPtr kitOf(const std::vector<float>& values, int64_t frames) {
    auto kit = std::make_shared<PadKit>();
    for (size_t i = 0; i < values.size(); i++) {
        Pad pad = padkit::newPad(constant(frames, values[i], "pad" + std::to_string(i + 1) + ".wav"), kit->pads[i].note);
        pad.sound.velocity = 0;  // (every note at full level)
        kit->pads[i] = pad;
    }
    return kit;
}

Note noteAt(double start, double end, int pitch, double velocity = 100) {
    Note n;
    n.start = start, n.end = end, n.pitch = pitch, n.velocity = velocity;
    return n;
}

float at(const AudioPtr& a, double seconds, int channel = 0) {
    int64_t k = (int64_t)std::nearbyint(seconds * SR);
    return k < a->frames() ? a->samples[(size_t)(k * 2 + channel)] : 0.0f;
}

void clearDir(const std::string& dir) {
    if (!util::isDir(dir)) return;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) {
            clearDir(p);
            rmdir(p.c_str());
        } else {
            util::removeFile(p);
        }
    }
}

// a drum loop of 4 hits (a kick, a snare-like noise burst, a hi-hat-like burst, a tom) on these frames, over
// a quiet floor of noise, 2 s long
std::vector<float> drumLoop(const std::vector<int64_t>& hits) {
    std::vector<float> loop((size_t)SR * 2 * 2, 0.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (size_t i = 0; i < loop.size(); i++) loop[i] = 0.0005f * noise(rng);
    for (size_t h = 0; h < hits.size(); h++) {
        int64_t a = hits[h];
        const int64_t length = SR * 2 / 5;
        for (int64_t k = 0; k < length && a + k < SR * 2; k++) {
            double t = (double)k / SR, v;
            if (h == 0) v = 0.9 * std::sin(2 * M_PI * 55 * t) * std::exp(-t * 12);           // kick
            else if (h == 1) v = 0.6 * noise(rng) * std::exp(-t * 25);                     // snare
            else if (h == 2) v = 0.25 * noise(rng) * std::exp(-t * 60);                    // hi-hat
            else v = 0.7 * std::sin(2 * M_PI * 140 * t) * std::exp(-t * 15);              // tom
            v *= std::min(1.0, (double)(length - k) / (SR / 20));  // (dying away over its last 50 ms)
            loop[(size_t)(a + k) * 2] += (float)v;
            loop[(size_t)(a + k) * 2 + 1] += (float)(v * 0.9);
        }
    }
    return loop;
}
}  // namespace

static void testKeysMapToPads() {
    PadKitPtr kit = kitOf({0.5f, 0.25f}, SR);
    CHECK(kit->pads.size() == (size_t)PadKit::MAX_PADS && kit->used() == 2);
    CHECK(kit->pads[0].note == 36 && kit->pads[1].note == 37 && kit->pads[31].note == 67);
    CHECK(kit->pads[5].empty() && kit->pads[5].sound.oneShot);  // (pads are one-shot to start with)
    CHECK(padkit::padFor(*kit, 36) == 0 && padkit::padFor(*kit, 37) == 1 && padkit::padFor(*kit, 38) == -1);
    CHECK(padkit::padName(kit->pads[0]) == "pad1");
    AudioPtr a = padkit::render(*kit, {noteAt(0, 0.1, 36), noteAt(0.5, 0.6, 37), noteAt(0.2, 0.3, 50)}, SR, 2.0);
    CHECK_NEAR(at(a, 0.1), 0.5, 1e-6);    // pad 1 (to the end of its 1 s sample: one-shot)
    CHECK_NEAR(at(a, 0.4), 0.5, 1e-6);    // (key 50 has no pad: silent)
    CHECK_NEAR(at(a, 0.7), 0.75, 1e-6);   // pads 1 and 2 together
    CHECK_NEAR(at(a, 1.2), 0.25, 1e-6);   // pad 1 over, pad 2 on
    CHECK_NEAR(at(a, 1.6), 0.0, 1e-9);
    // a pad sounds exactly as the sampler with its settings, the sample at its own pitch (`transpose` tuning it)
    auto sine = std::make_shared<AudioBuffer>();
    for (int i = 0; i < SR / 2; i++) {
        float v = (float)(0.3 * std::sin(i * 0.031) * std::exp(-i / 9000.0));
        sine->samples.push_back(v), sine->samples.push_back(-v);
    }
    auto sample = constant(1, 0, "tone.wav");
    sample->source = sine;
    Pad pad = padkit::newPad(sample, 40);
    pad.sound.transpose = 3;
    pad.sound.oneShot = false;
    pad.sound.release = 0.05;
    pad.sound.root = 72;  // (not used by a pad: its key plays the sample at its own pitch)
    PadKitPtr tuned = padkit::withPad(PadKit(), 4, pad);
    CHECK(tuned->pads[4].note == 40 && padkit::padFor(*tuned, 40) == 4);
    std::vector<Note> notes{noteAt(0.1, 0.3, 40, 90), noteAt(0.5, 0.6, 40, 60)};
    notes[1].pan = 0.5, notes[1].fine = 20;
    AudioPtr viaKit = padkit::render(*tuned, notes, SR, 1.0);
    for (auto& n : notes) n.pitch = 72;
    AudioPtr viaSampler = sampler::render(pad.sound, notes, SR, 1.0);
    CHECK(viaKit->samples == viaSampler->samples);  // (bit for bit)
    // Track: the Pad Kit wins over a sampler; plugins over both; the MIDI editor's key labels
    Track t("Drums", Kind::Midi);
    t.padKit = kit;
    t.sampler = std::make_shared<SamplerSettings>();
    CHECK(t.playsPadKit() && !t.playsSampler());
    CHECK(t.keyLabel(37) == "pad2" && t.keyLabel(40) == "");
    t.usesRack = true;
    CHECK(!t.playsPadKit() && t.keyLabel(37) == "");
}

static void testChokeGroups() {
    PadKitPtr base = kitOf({0.5f, 0.25f, 0.125f}, SR);
    auto kit = std::make_shared<PadKit>(*base);
    kit->pads[0].choke = 1;  // open hi-hat
    kit->pads[1].choke = 1;  // closed hi-hat
    // no choke: the open one rings on under the closed one
    AudioPtr free = padkit::render(*base, {noteAt(0, 0.1, 36), noteAt(0.25, 0.3, 37)}, SR, 1.0);
    CHECK_NEAR(at(free, 0.4), 0.75, 1e-6);
    // choked: the open one fades out over CHOKE_S from the closed one's start, then is gone
    AudioPtr cut = padkit::render(*kit, {noteAt(0, 0.1, 36), noteAt(0.25, 0.3, 37), noteAt(0.1, 0.2, 38)}, SR, 1.0);
    CHECK_NEAR(at(cut, 0.2), 0.5 + 0.125, 1e-6);         // before: as it was (pad 3, no group: not cut)
    CHECK_NEAR(at(cut, 0.25 + live::CHOKE_S / 2), 0.25 + 0.125 + 0.25, 0.01);  // half-way through the fade
    CHECK_NEAR(at(cut, 0.4), 0.25 + 0.125, 1e-6);        // gone: only the closed one (and pad 3)
    double level = 0;  // (measured: the open one's level after the cut, over 0.3…0.9 s, without the others)
    for (int64_t k = (int64_t)(0.3 * SR); k < (int64_t)(0.9 * SR); k++)
        level = std::max(level, std::fabs(cut->samples[(size_t)k * 2] - (k < (int64_t)(1.1 * SR) ? 0.375 : 0.0)));
    CHECK(level < 1e-6);
    // notes starting together don't cut each other; a pad cuts its own earlier note (same group)
    AudioPtr together = padkit::render(*kit, {noteAt(0, 0.1, 36), noteAt(0, 0.1, 37)}, SR, 1.0);
    CHECK_NEAR(at(together, 0.5), 0.75, 1e-6);
    AudioPtr again = padkit::render(*kit, {noteAt(0, 0.1, 36), noteAt(0.5, 0.6, 36)}, SR, 2.0);
    CHECK_NEAR(at(again, 0.7), 0.5, 1e-6);  // (one at a time, not 1.0)
}

static void testOneShotAndHeld() {
    PadKitPtr base = kitOf({0.5f}, SR);
    AudioPtr shot = padkit::render(*base, {noteAt(0, 0.05, 36)}, SR, 0.1);
    CHECK_NEAR(at(shot, 0.8), 0.5, 1e-6);  // one-shot: to the sample's end, however short the note
    auto kit = std::make_shared<PadKit>(*base);
    kit->pads[0].sound.oneShot = false;
    kit->pads[0].sound.release = 0.05;
    kit->pads[0].sound.attack = 0;
    AudioPtr held = padkit::render(*kit, {noteAt(0, 0.2, 36)}, SR, 1.0);
    CHECK_NEAR(at(held, 0.1), 0.5, 1e-6);
    CHECK_NEAR(at(held, 0.225), 0.25, 0.01);  // releasing
    CHECK_NEAR(at(held, 0.4), 0.0, 1e-9);     // held: over once let go (+ its release)
    // live: a one-shot pad ignores the key's release; a held pad stops with it
    live::LiveNotes notes;
    notes.setRenderer(padkit::noteRenderer(kit));
    auto mixed = std::make_shared<PadKit>(*kit);
    mixed->pads[1] = padkit::newPad(constant(SR, 0.25f, "shot.wav"), 37);
    mixed->pads[1].sound.velocity = 0;
    notes.setRenderer(padkit::noteRenderer(mixed));
    std::vector<float> l(512), r(512);
    float* sig[2] = {l.data(), r.data()};
    plugin_audio::Messages msgs;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; b++) {
            std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
            notes.take(512, SR, msgs);
            notes.addVoices(sig, 2, 512, SR);
        }
    };
    CHECK(notes.noteOn(36, 127, SR, true));
    CHECK(notes.noteOn(37, 127, SR, true));
    run(4);
    CHECK_NEAR(l[100], 0.75, 1e-6);
    notes.noteOff(36);
    notes.noteOff(37);  // (one-shot: ignored)
    run(20);
    CHECK_NEAR(l[100], 0.25, 1e-6);  // the held one let go, the one-shot plays on
    run(80);
    CHECK(!notes.active());  // (its 1 s sample over)
}

static void testLiveChokeAndEngine() {
    PadKitPtr base = kitOf({0.5f, 0.25f}, SR * 2);
    auto kit = std::make_shared<PadKit>(*base);
    kit->pads[0].choke = 3, kit->pads[1].choke = 3;
    live::LiveNotes notes;
    notes.setRenderer(padkit::noteRenderer(kit));
    std::vector<float> l(256), r(256);
    float* sig[2] = {l.data(), r.data()};
    plugin_audio::Messages msgs;
    auto run = [&] {
        std::fill(l.begin(), l.end(), 0.0f), std::fill(r.begin(), r.end(), 0.0f);
        notes.take(256, SR, msgs);
        notes.addVoices(sig, 2, 256, SR);
    };
    notes.noteOn(36, 100, SR, true);
    run();
    CHECK_NEAR(l[200], 0.5, 1e-6);
    notes.noteOn(37, 100, SR, true);  // the other pad of its group: the first fades out over CHOKE_S
    run();
    int fade = (int)(live::CHOKE_S * SR);
    CHECK_NEAR(l[0], 0.75, 1e-6);
    CHECK_NEAR(l[fade / 2], 0.5 + 0.25, 0.26);
    CHECK(l[fade / 2] < 0.75f - 0.1f);
    CHECK_NEAR(l[fade + 10], 0.25, 1e-6);  // (the measured level: the first pad gone)
    run();
    CHECK_NEAR(l[100], 0.25, 1e-6);
    // through the engine: the track's live notes play its kit
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->padKit = kit;
    audio::Engine e(p);
    auto live = e.liveNotes(t.get());
    live->noteOn(37, 100, e.sr, true);
    std::vector<float> out(256 * 2);
    e.callback(nullptr, out.data(), 256);
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.1f);
    live->noteOn(50, 100, e.sr, true);  // (a key without a pad: nothing, and nothing stuck)
    live->noteOff(50);
    e.idle();
}

static void testRenderEqualsPlayback() {
    Project p;
    audio::Engine e(p);  // (first: it installs the sampler hook clips are prepared through)
    e.offline = true;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto kit = std::make_shared<PadKit>(*kitOf({0.4f, 0.2f, 0.3f}, SR / 2));
    kit->pads[0].choke = 1, kit->pads[1].choke = 1;
    kit->pads[2].sound.oneShot = false;
    kit->pads[2].sound.transpose = 5;
    t->padKit = kit;
    std::vector<Note> notes{noteAt(0, 0.1, 36), noteAt(0.2, 0.3, 37), noteAt(0.5, 0.9, 38, 70), noteAt(1.0, 1.1, 36)};
    auto mc = makeMidiClip("beat", notes, 2.0, SR);
    mc->track = t;
    mc->start = SR * 2;
    mc->prepare(SR);
    p.addClip(mc);
    patch::ensureStates(p);
    const int block = config::RENDER_BLOCK;
    const int64_t start = SR * 2, end = SR * 4;
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
    double loudest = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++) {
        float x = (*rendered)[(size_t)j], y = played[(size_t)((start + e.latency) * 2 + j)];
        if (x != y) differ++;
        loudest = std::max(loudest, (double)std::fabs(x));
    }
    CHECK(differ == 0);
    CHECK(loudest > 0.05);
    // the clip's own rendering is the kit's
    AudioPtr direct = padkit::render(*kit, *mc->played, SR, 2.0);
    CHECK(mc->data && mc->data->samples == direct->samples);
}

static void testSaveLoadAndUndo() {
    clearDir(kDir);
    util::makeDirs(kDir);
    Project p;
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    auto kit = std::make_shared<PadKit>(*kitOf({0.5f, 0.25f}, SR / 4));
    kit->pads[1].choke = 4;
    kit->pads[1].sound.transpose = -2;
    kit->pads[1].sound.oneShot = false;
    kit->pads[7].note = 80;  // (an empty pad moved to another key: kept)
    t->padKit = kit;
    auto plain = p.addTrack(Kind::Midi);  // (no kit: no "pad_kit" in the file, as before)
    plain->needsInstrument = false;
    project_io::MasterSettings master;
    std::string path = kDir + "/Kit.json", err;
    CHECK(project_io::saveProject(path, p, SR, master, err));
    std::string folder = project_io::audioFolder(path);
    int wavs = 0;
    for (auto& e : util::listDir(folder))
        if (util::extension(e.name) == ".wav") wavs++;
    CHECK(wavs == 2);  // (each pad's sample in the audio folder)
    std::string text;
    CHECK(util::readFile(path, text));
    Json data = Json::parse(text);
    const Json& tracks = data["tracks"];
    CHECK(tracks.size() == 2 && tracks[size_t(0)]["pad_kit"].isObject() && tracks[size_t(1)]["pad_kit"].isNull());
    CHECK(tracks[size_t(0)]["pad_kit"]["pads"].size() == 3);  // (the two used, the moved one)
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, err));
    CHECK(q.tracks.size() == 2 && q.tracks[0]->padKit && !q.tracks[1]->padKit);
    if (q.tracks.size() == 2 && q.tracks[0]->padKit) {
        const PadKit& back = *q.tracks[0]->padKit;
        CHECK(back.used() == 2 && back.pads[1].choke == 4 && back.pads[1].sound.transpose == -2);
        CHECK(!back.pads[1].sound.oneShot && back.pads[0].sound.oneShot && back.pads[7].note == 80);
        CHECK(back.pads[0].sound.sample->name == "pad1.wav");
        CHECK(back.pads[0].sound.sample->source->samples == kit->pads[0].sound.sample->source->samples);
        CHECK(util::dirName(back.pads[1].sound.sample->sourcePath) == folder);
        CHECK(q.tracks[0]->playsPadKit());
    }
    // a project saved before Pad Kits opens as it was
    std::string old = R"({"format": "tinydaw-project", "version": 1, "sample_rate": 48000, "groups": [],
        "tracks": [{"name": "MIDI 1", "kind": "midi", "muted": false, "soloed": false, "volume_db": 0,
                    "needs_instrument": false, "effects": [], "cables": [["in", "out"]]}], "clips": []})";
    std::string oldPath = kDir + "/Old.json";
    CHECK(util::writeFile(oldPath, old));
    Project o;
    if (project_io::loadProject(oldPath, o, SR, m2, err)) {
        CHECK(o.tracks.size() == 1 && !o.tracks[0]->padKit && !o.tracks[0]->playsPadKit());
    } else {
        printf("(old file: %s)\n", err.c_str());
        CHECK(false);
    }
    // undo: the kit as it was before a change, and before it was chosen
    Project u;
    auto ut = u.addTrack(Kind::Midi);
    history::History h(u, SR);
    std::atomic_store(&ut->padKit, PadKitPtr(kit));
    CHECK(h.check(SR));
    std::atomic_store(&ut->padKit, padkit::withPad(*kit, 0, Pad()));
    CHECK(h.check(SR));
    CHECK(h.labels().back() == "Instrument");
    CHECK(h.undo(SR));
    CHECK(ut->padKit == kit);
    CHECK(h.undo(SR));
    CHECK(!ut->padKit);
    CHECK(h.redo(SR) && ut->padKit == kit);
}

static void testSlicing() {
    const std::vector<int64_t> hits{SR / 10, SR / 2 + 123, SR + 4321, SR * 3 / 2 + 77};
    std::vector<float> loop = drumLoop(hits);
    int64_t frames = (int64_t)loop.size() / 2;
    dsp::Onsets onsets(loop.data(), frames, SR);
    std::vector<int64_t> found = onsets.find(padkit::DEFAULT_SENSITIVITY);
    CHECK(found.size() == hits.size());
    for (size_t i = 0; i < std::min(found.size(), hits.size()); i++) {
        CHECK(std::llabs(found[i] - hits[i]) <= 24);  // (half a millisecond)
        if (std::llabs(found[i] - hits[i]) > 24) printf("hit %zu: found at %lld, is at %lld\n", i, (long long)found[i], (long long)hits[i]);
    }
    // less sensitive: the quiet hi-hat is left out; more: no extra hits in this clean loop
    CHECK(onsets.find(0.0).size() < hits.size());
    CHECK(onsets.find(1.0).size() == hits.size());
    // the slices: one per hit (the quiet start before the first is no slice), notes at the hits' times
    padkit::Sliced sliced = padkit::slice(loop.data(), frames, SR, found, "Loop");
    CHECK(sliced.kit->used() == 4 && sliced.notes.size() == 4 && sliced.starts == found);
    CHECK(sliced.kit->pads[0].sound.sample->name == "Loop slice 1.wav" && sliced.kit->pads[3].sound.oneShot);
    CHECK_NEAR(sliced.lengthS, 2.0, 1e-9);
    for (size_t i = 0; i < sliced.notes.size() && i < found.size(); i++) {
        CHECK_NEAR(sliced.notes[i].start, (double)found[i] / SR, 1e-12);
        CHECK(sliced.notes[i].pitch == PadKit::FIRST_NOTE + (int)i);
    }
    // the clip replays the loop: its rendering correlates with the original (from the first hit on)
    Project p;
    audio::Engine e(p);
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    t->padKit = sliced.kit;
    auto mc = makeMidiClip("Loop", sliced.notes, sliced.lengthS, SR);
    mc->track = t;
    mc->prepare(SR);
    double xy = 0, xx = 0, yy = 0;
    for (int64_t k = found.empty() ? 0 : found[0]; k < frames; k++)
        for (int c = 0; c < 2; c++) {
            double x = loop[(size_t)(k * 2 + c)], y = mc->data->samples[(size_t)(k * 2 + c)];
            xy += x * y, xx += x * x, yy += y * y;
        }
    double correlation = xy / std::sqrt(xx * yy + 1e-30);
    CHECK(correlation > 0.99);
    if (correlation <= 0.99) printf("correlation %.5f\n", correlation);
    // sound before the first hit is a slice of its own; more than 32 hits: the rest in the last slice
    std::vector<float> ramp((size_t)SR * 2, 0.3f);
    std::vector<int64_t> many;
    for (int i = 1; i <= 40; i++) many.push_back(i * 1000);
    padkit::Sliced cut = padkit::slice(ramp.data(), SR, SR, many, "Ramp");
    CHECK(cut.starts.size() == (size_t)padkit::MAX_SLICES && cut.starts[0] == 0 && cut.starts[1] == 1000);
    CHECK(cut.kit->pads[31].sound.sample->source->frames() == SR - 31000);  // (hits 31…40 merged into it)
    CHECK(padkit::slice(ramp.data(), SR, SR, {}, "None").notes.size() == 1);  // (no hits: one slice, all of it)
    // an audio clip's sound as it plays: its window, gain and fades
    auto src = std::make_shared<AudioBuffer>();
    src->samples.assign((size_t)SR * 2, 0.5f);
    ClipPtr clip = makeAudioClip("c", src, SR, SR);
    clip->offset = 1000;
    clip->length = 2000;
    clip->gainDb = -6.0205999;
    std::vector<float> sound = padkit::clipSound(*clip);
    CHECK(sound.size() == 4000);
    CHECK_NEAR(sound[10], 0.25, 1e-4);
}

int main() {
    testKeysMapToPads();
    testChokeGroups();
    testOneShotAndHeld();
    testLiveChokeAndEngine();
    testRenderEqualsPlayback();
    testSaveLoadAndUndo();
    testSlicing();
    TEST_MAIN_END();
}
