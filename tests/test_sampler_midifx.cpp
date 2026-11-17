// Tests of the built-in sampler (sampler.h), the MIDI effects (midi_effects.h, against what the
// successor's midi_effects.py makes of the same notes) and the recording offset's measurement
// (record_offset.h).
#include <algorithm>
#include <cmath>

#include "audio/midi_effects.h"
#include "audio/record_offset.h"
#include "audio/sampler.h"
#include "audio/synth.h"
#include "check.h"
#include "model/model.h"

namespace {
const int SR = 48000;

// a sample (stereo, at SR): left = value(i), right = -value(i)
template <class F>
ClipPtr sampleOf(int64_t frames, F value) {
    auto b = std::make_shared<AudioBuffer>();
    for (int64_t i = 0; i < frames; i++) b->samples.push_back((float)value(i)), b->samples.push_back((float)-value(i));
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->source = b;
    c->sourceSr = SR;
    return c;
}

SamplerSettings plainSettings(ClipPtr sample) {
    SamplerSettings s;
    s.sample = std::move(sample);
    s.attack = 0, s.decay = 0, s.sustain = 1, s.release = 0.1;
    s.velocity = 0;
    return s;
}

std::vector<std::pair<int, int>> pairsOf(const std::vector<Note>& notes) {
    std::vector<std::pair<int, int>> out;
    for (auto& n : notes) out.emplace_back((int)n.pitch, (int)n.velocity);
    return out;
}
}  // namespace

static void testNames() {
    CHECK(sampler::noteName(60) == "C4");
    CHECK(sampler::noteName(0) == "C-1");
    CHECK(sampler::noteName(70) == "A#4");
    CHECK(sampler::rootFromName("Piano C4.wav") == 60);
    CHECK(sampler::rootFromName("flute_A#3") == 58);
    CHECK(sampler::rootFromName("Eb2 bass") == 39);
    CHECK(sampler::rootFromName("kick.wav") == 60);
    CHECK(sampler::rootFromName("vox D-1 G2.wav") == 43);  // (the last one named)
    CHECK(sampler::rootFromName("BD1") == 60);              // (a letter inside a word isn't a note)
    CHECK(sampler::rootFromName("pad C10") == 60);          // (no octave 10)
    CHECK(sampler::rootFromName("x D-1") == 2);
}

static void testSamplerPitch() {
    const int64_t len = 48000;
    auto s = plainSettings(sampleOf(len, [&](int64_t i) { return (double)i / len; }));
    s.root = 60;
    // the root plays the sample as it is
    auto n = sampler::renderNote(s, 60, 100, 0.5, SR);
    CHECK_NEAR(n[1000 * 2], 1000.0 / len, 1e-6);
    CHECK_NEAR(n[1000 * 2 + 1], -1000.0 / len, 1e-6);
    // an octave up: twice as fast; down: half as fast (interpolated)
    n = sampler::renderNote(s, 72, 100, 0.25, SR);
    CHECK_NEAR(n[1000 * 2], 2000.0 / len, 1e-6);
    n = sampler::renderNote(s, 48, 100, 0.25, SR);
    CHECK_NEAR(n[1001 * 2], 500.5 / len, 1e-6);
    // transpose counts like the key
    s.transpose = 12;
    n = sampler::renderNote(s, 60, 100, 0.25, SR);
    CHECK_NEAR(n[1000 * 2], 2000.0 / len, 1e-6);
    s.transpose = 0;
    // the note's length plus the release, not past the sample's end (without loop)
    n = sampler::renderNote(s, 60, 100, 0.5, SR);
    CHECK((int64_t)n.size() / 2 == SR / 2 + (int64_t)(0.1 * SR));
    n = sampler::renderNote(s, 72, 100, 2.0, SR);  // (twice as fast: its end comes after 0.5 s)
    CHECK((int64_t)n.size() / 2 == (len - 1) / 2);
    // start / end: the part played
    s.start = 0.5, s.end = 0.75;
    n = sampler::renderNote(s, 60, 100, 0.1, SR);
    CHECK_NEAR(n[10 * 2], (24000.0 + 10) / len, 1e-6);
    // loop: start…end repeats while held
    s.loop = true;
    n = sampler::renderNote(s, 60, 100, 0.5, SR);
    CHECK((int64_t)n.size() / 2 == SR / 2 + (int64_t)(0.1 * SR));
    CHECK_NEAR(n[(12000 + 10) * 2], (24000.0 + 10) / len, 1e-6);  // (12000 frames: the loop's length)
    CHECK_NEAR(sampler::loopPeriod(s, 60, SR), 12000.0, 1e-9);
    CHECK_NEAR(sampler::loopPeriod(s, 72, SR), 6000.0, 1e-9);
    // velocity: how much it changes the level
    s.loop = false, s.start = 0, s.end = 1;
    s.velocity = 1.0;
    n = sampler::renderNote(s, 60, 64, 0.5, SR);
    CHECK_NEAR(n[1000 * 2], 1000.0 / len * 64 / 127, 1e-6);
    s.velocity = 0.5;
    s.gainDb = -6;
    n = sampler::renderNote(s, 60, 0, 0.5, SR);
    CHECK_NEAR(n[1000 * 2], 1000.0 / len * 0.5 * std::pow(10.0, -6.0 / 20), 1e-6);
}

static void testSamplerEnvelope() {
    auto s = plainSettings(sampleOf(SR * 2, [](int64_t) { return 1.0; }));
    s.attack = 0.01, s.decay = 0.02, s.sustain = 0.5, s.release = 0.05;
    auto n = sampler::renderNote(s, 60, 127, 0.1, SR);
    auto at = [&](double t) { return n[(size_t)std::nearbyint(t * SR) * 2]; };
    CHECK_NEAR(at(0.005), 0.5, 1e-5);           // attack: rising
    CHECK_NEAR(at(0.01), 1.0, 1e-5);
    CHECK_NEAR(at(0.02), 0.75, 1e-5);           // decay to the sustain level
    CHECK_NEAR(at(0.05), 0.5, 1e-5);
    CHECK_NEAR(at(0.1 + 0.025), 0.25, 1e-4);    // let go: down over the release
    CHECK((int64_t)n.size() / 2 == (int64_t)(0.1 * SR) + (int64_t)(0.05 * SR));
    auto env = sampler::envelope(10, 5, s, SR);
    CHECK(env.size() == 10 && env[0] == 0.0f);
    // the very end fades over EDGE_S, so nothing clicks
    CHECK_NEAR(n[(n.size() / 2 - 1) * 2], 0.0, 1e-9);
    // one-shot: to the sample's end, however short the note; no release
    s.oneShot = true;
    s.attack = 0, s.decay = 0, s.sustain = 1;
    n = sampler::renderNote(s, 60, 127, 0.01, SR);
    CHECK((int64_t)n.size() / 2 == SR * 2 - 1);
    CHECK_NEAR(n[SR * 2], 1.0, 1e-6);  // (1 s in: still at full level)
    n = sampler::renderNote(s, 72, 127, 0.01, SR);
    CHECK((int64_t)n.size() / 2 == (SR * 2 - 1) / 2);
}

static void testSamplerClips() {
    // a MIDI clip on a sampler track renders through it (clip_hooks::renderSampler)
    sampler::install();
    auto t = std::make_shared<Track>("MIDI 1", Kind::Midi);
    t->needsInstrument = false;
    auto settings = std::make_shared<SamplerSettings>(plainSettings(sampleOf(SR, [](int64_t) { return 0.5; })));
    t->sampler = settings;
    CHECK(t->playsSampler());
    std::vector<Note> notes = {{0.0, 0.25, 60, 127}, {0.5, 0.75, 60, 127}};
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->track = t;
    c->notes = std::make_shared<const std::vector<Note>>(notes);
    c->lengthS = 1.0;
    c->prepare(SR);
    auto direct = sampler::render(*settings, notes, SR, 1.0);
    CHECK(c->data && c->data->samples == direct->samples);
    double tail = std::min(sampler::MAX_TAIL_S, std::max(synth::RELEASE_S, settings->release));
    CHECK(direct->frames() == (int64_t)((1.0 + tail) * SR) + 1);
    CHECK_NEAR(direct->frame(SR / 10)[0], 0.5, 1e-6);
    CHECK_NEAR(direct->frame(SR / 2 + 100)[0], 0.5, 1e-6);
    // the live renderer
    auto r = sampler::noteRenderer(*t);
    CHECK(r.render && r.channels == 2 && !r.oneShot && !r.sustains);
    auto live = r.render(60, 127, 0.1, SR);
    CHECK(live.size() == sampler::renderNote(*settings, 60, 127, 0.1, SR).size());
    t->sampler = nullptr;  // no sampler: the built-in synth
    CHECK(!sampler::noteRenderer(*t).render);
    auto synthAudio = sampler::renderNotes(*t, notes, SR, 1.0);
    CHECK(synthAudio->samples == synth::render(notes, SR, 1.0)->samples);
}

// --- MIDI effects: each one's output for the same input as midi_effects.py ---

static MidiEffect effect(const char* kind, std::map<std::string, double> values = {}) {
    MidiEffect e;
    CHECK(MidiEffect::make(kind, values, e));
    return e;
}

static void testNumpyRandom() {
    using midi_effects::NumpyRandom;
    // np.random.default_rng(seed).random()
    CHECK(NumpyRandom(0).random() == 0.6369616873214543);
    CHECK(NumpyRandom(1).random() == 0.5118216247002567);
    CHECK(NumpyRandom(42).random() == 0.7739560485559633);
    CHECK(NumpyRandom(12345).random() == 0.22733602246716966);
    NumpyRandom r(7);
    for (int i = 0; i < 100; i++) {
        int64_t v = r.integers(-12, 13);
        CHECK(v >= -12 && v <= 12);
    }
    NumpyRandom a(5), b(5);
    CHECK(a.integers(0, 1000) == b.integers(0, 1000));
    CHECK(NumpyRandom(3).integers(4, 5) == 4);  // (a range of one: nothing is drawn)
}

static void testSimpleEffects() {
    std::vector<Note> in = {{0.0, 0.5, 60, 100}, {0.5, 1.0, 120, 50}};
    // Pitch: up an octave; a note past 127 goes
    auto out = midi_effects::process(effect("Pitch", {{"semitones", 12}}), in, 0);
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{72, 100}}));
    // Chord: Major (index 2), the added notes at 85 % velocity (round half to even: 42.5 -> 42)
    out = midi_effects::process(effect("Chord", {{"chord", 2}}), in, 0);
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{60, 100}, {64, 85}, {67, 85}, {120, 50}, {124, 42}, {127, 42}}));
    CHECK(out[1].start == 0.0 && out[1].end == 0.5 && out[3].start == 0.5);
    out = midi_effects::process(effect("Chord", {{"chord", 8}}), {{0, 1, 5, 100}}, 0);  // Octave down: below 0 goes
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{5, 100}}));
    // Scale: the nearest note of C major (the lower one when two are as near)
    auto steps = midi_effects::scaleSteps(0, "Major");
    CHECK(midi_effects::nearestInScale(61, steps) == 60);
    CHECK(midi_effects::nearestInScale(66, steps) == 65);
    CHECK(midi_effects::nearestInScale(64, steps) == 64);
    out = midi_effects::process(effect("Scale", {{"root", 2}, {"scale", 6}}), {{0, 1, 63, 90}}, 0);  // D minor pentatonic
    CHECK(out.size() == 1 && out[0].pitch == 62);  // (D F G A C: 63 → 62, nearer than 65)
    CHECK((midi_effects::inScaleChord(61, steps) == std::vector<int>{60, 64, 67}));
    // Note Length: a length in beats (120 BPM: 0.5 beat = 0.25 s), or a share of each note's own
    midi_effects::install(nullptr);
    out = midi_effects::process(effect("Note Length", {{"beats", 0.5}}), in, 0);
    CHECK(out[0].end == 0.25 && out[1].end == 0.75);
    out = midi_effects::process(effect("Note Length", {{"mode", 1}, {"share", 50}}), in, 0);
    CHECK(out[0].end == 0.25 && out[1].end == 0.75);
    // Velocity: amount and add, within lowest…highest
    out = midi_effects::process(effect("Velocity", {{"scale", 50}, {"offset", 10}}), in, 0);
    CHECK(out[0].velocity == 60 && out[1].velocity == 35);
    out = midi_effects::process(effect("Velocity", {{"lowest", 70}, {"highest", 90}}), in, 0);
    CHECK(out[0].velocity == 90 && out[1].velocity == 70);
    // its "random": numpy's default_rng seeded by the note: round(start × 1000) × 131 + pitch
    out = midi_effects::process(effect("Velocity", {{"random", 20}}), in, 0);
    double expected = std::nearbyint(100 + (midi_effects::NumpyRandom(60).random() * 2 - 1) * 20);
    CHECK(out[0].velocity == expected);
    expected = std::nearbyint(50 + (midi_effects::NumpyRandom(500 * 131 + 120).random() * 2 - 1) * 20);
    CHECK(out[1].velocity == expected);
    // Random: chance 0 leaves notes alone; 100 moves them within the range, the same every time
    out = midi_effects::process(effect("Random", {{"chance", 0}}), in, 0);
    CHECK(out[0].pitch == 60 && out[1].pitch == 120);
    auto r1 = midi_effects::process(effect("Random", {{"chance", 100}, {"range", 3}}), in, 0);
    auto r2 = midi_effects::process(effect("Random", {{"chance", 100}, {"range", 3}}), in, 0);
    CHECK(r1[0].pitch == r2[0].pitch && std::fabs(r1[0].pitch - 60) <= 3 && r1[1].pitch <= 127);
    // (chance 50, seed 1: the note at 0 s, pitch 60 is moved when default_rng(60 + 7919).random() < 0.5)
    auto half = midi_effects::process(effect("Random"), {{0, 1, 60, 100}}, 0);
    bool moves = midi_effects::NumpyRandom(60 + 7919).random() * 100 < 50;
    CHECK(moves == true || half[0].pitch == 60);
}

static void testArpeggiator() {
    midi_effects::install(nullptr);  // (120 BPM)
    std::vector<Note> chord = {{0.0, 0.5, 64, 90}, {0.0, 0.5, 60, 100}, {0.0, 0.5, 67, 80}};
    auto arp = [&](std::map<std::string, double> v, double origin = 0) {
        return midi_effects::process(effect("Arpeggiator", v), chord, origin);
    };
    // Up, 1/16 (0.125 s at 120 BPM), gate 80 %: four steps while the chord is held
    auto out = arp({});
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{60, 100}, {64, 90}, {67, 80}, {60, 100}}));
    CHECK(out[1].start == 0.125 && std::fabs(out[1].end - 0.225) < 1e-12);
    out = arp({{"style", 1}});  // Down
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{67, 80}, {64, 90}, {60, 100}, {67, 80}}));
    out = arp({{"style", 2}});  // Up and down: 60 64 67 64 …
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{60, 100}, {64, 90}, {67, 80}, {64, 90}}));
    out = arp({{"style", 3}});  // As played (by start: equal starts keep their order)
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{64, 90}, {60, 100}, {67, 80}, {64, 90}}));
    out = arp({{"octaves", 2}, {"rate", 5}});  // 1/32, two octaves: 60 64 67 72 76 79 60 64
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{
                               {60, 100}, {64, 90}, {67, 80}, {72, 100}, {76, 90}, {79, 80}, {60, 100}, {64, 90}}));
    out = arp({{"rate", 0}, {"gate", 200}});  // 1/4: one step, twice as long
    CHECK(out.size() == 1 && out[0].end == 1.0);
    // on the grid of the timeline: a clip starting at 0.1 s steps from 0.125 s (0.025 s into it)
    out = arp({}, 0.1);
    CHECK(out.size() == 4 && std::fabs(out[0].start - 0.025) < 1e-12);
    // Random style: numpy's default_rng(1) picks; the pool's notes only
    out = arp({{"style", 4}});
    CHECK(out.size() == 4);
    for (auto& n : out) CHECK(n.pitch == 60 || n.pitch == 64 || n.pitch == 67);
    // following the tempo map: at 60 BPM a 1/16 is 0.25 s
    Timing timing;
    timing.bpm = 60;
    midi_effects::install(&timing);
    out = arp({});
    CHECK(out.size() == 2 && out[1].start == 0.25);
    timing.setChange(2, 120, 4);  // (bar 2 at 4 s: 120 BPM from there)
    std::vector<Note> later = {{3.5, 4.5, 60, 100}};
    out = midi_effects::process(effect("Arpeggiator"), later, 0);
    CHECK(out.size() == 6 && out[0].start == 3.5 && out[1].start == 3.75 && out[2].start == 4.0 && out[3].start == 4.125);
    midi_effects::install(nullptr);
}

static void testApplyAndLive() {
    midi_effects::install(nullptr);
    Track t("MIDI 1", Kind::Midi);
    t.midiEffects = std::vector<MidiEffect>{effect("Chord", {{"chord", 3}}), effect("Pitch", {{"semitones", -1}})};
    // apply: in order, then sorted by start and pitch
    auto out = midi_effects::apply(t, {{0.5, 1, 62, 100}, {0.0, 0.5, 60, 100}}, 0);
    CHECK((pairsOf(out) == std::vector<std::pair<int, int>>{{59, 100}, {62, 85}, {66, 85}, {61, 100}, {64, 85}, {68, 85}}));
    // live: only those that don't change timing (no Arpeggiator / Note Length)
    t.midiEffects = std::vector<MidiEffect>{effect("Arpeggiator"), effect("Note Length"), effect("Pitch", {{"semitones", 2}}),
                                            effect("Chord", {{"chord", 0}})};
    CHECK((midi_effects::live(t, 60, 100) == std::vector<std::pair<int, int>>{{62, 100}, {74, 85}}));
    CHECK(midi_effects::isLive("Chord") && !midi_effects::isLive("Arpeggiator") && !midi_effects::isLive("Note Length"));
    // a clip prepared on the track hears them (clip_hooks::midiEffects)
    auto track = std::make_shared<Track>("MIDI 2", Kind::Midi);
    track->needsInstrument = false;
    track->midiEffects = std::vector<MidiEffect>{effect("Pitch", {{"semitones", 5}})};
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->track = track;
    c->notes = std::make_shared<const std::vector<Note>>(std::vector<Note>{{0, 0.5, 60, 100}});
    c->lengthS = 1;
    c->prepare(SR);
    CHECK(c->played && c->played->size() == 1 && (*c->played)[0].pitch == 65 && (*c->events)[0].pitch == 65);
    // the dialog's parameters
    const std::string arp = "Arpeggiator";  // (a named string: GCC warns of a dangling reference for a temporary)
    auto& ps = midi_effects::params(arp);
    CHECK(ps.size() == 4 && ps[1].key == "rate" && ps[1].choices.size() == 6 && ps[1].defaultValue == 3 && ps[1].steps() == 5);
    CHECK(midi_effects::params("Velocity")[1].steps() == 254);
    CHECK(midi_effects::params("Nope").empty() && midi_effects::kinds().size() == 8);
    for (auto& k : MidiEffect::kinds()) {  // (the model's defaults are the same)
        auto& mine = midi_effects::params(k.first);
        CHECK(mine.size() == k.second.size());
        for (size_t i = 0; i < mine.size() && i < k.second.size(); i++)
            CHECK(mine[i].key == k.second[i].key && mine[i].defaultValue == k.second[i].defaultValue);
    }
}

// --- the recording offset: clicks in a take against the beats ---

static void testRecordOffset() {
    using namespace record_offset;
    CHECK(deviceKey("JACK", "system") == "JACK|system");
    std::string prefs = setOffsetMs("", "JACK|system", 3.456);
    CHECK_NEAR(offsetMs(prefs, "JACK|system"), 3.46, 1e-12);
    prefs = setOffsetMs(prefs, "ALSA|hw:1", -2);
    CHECK(offsetMs(prefs, "ALSA|hw:1") == -2 && offsetMs(prefs, "JACK|system") == 3.46);
    CHECK(offsetMs(prefs, "other") == 0 && offsetMs("not json", "x") == 0);
    // a take of the metronome (120 BPM) recorded 3 ms late, from 1 s on, with a stray click off the beat
    Timing timing;
    auto take = std::make_shared<Clip>();
    take->kind = Kind::Audio;
    take->start = SR;
    auto data = std::make_shared<AudioBuffer>();
    data->samples.assign((size_t)SR * 4 * 2, 0.001f);  // (a little noise floor)
    auto click = [&](double timelineS, float level) {
        int64_t at = (int64_t)std::nearbyint(timelineS * SR) - take->start;
        for (int64_t i = 0; i < 200; i++) data->samples[(size_t)(at + i) * 2] = level * (i % 2 ? -1 : 1);
    };
    for (int k = 0; k < 8; k++) click(1.0 + 0.5 * k + 0.003, 0.8f);
    click(2.25, 0.5f);  // (0.25 s from any beat: not one of the metronome's)
    take->data = data;
    auto on = onsets(data->samples.data(), data->frames(), 2, SR);
    CHECK(on.size() == 9 && on[0] == 144);
    auto m = measure(*take, timing, SR);
    CHECK(m.has_value());
    if (m) {
        CHECK_NEAR(m->late, 0.003, 1e-9);
        CHECK_NEAR(m->spread, 0.0, 1e-9);
        CHECK(m->count == 8);
    }
    // too few clicks: nothing measured
    auto few = std::make_shared<AudioBuffer>();
    few->samples.assign((size_t)SR * 2, 0.0f);
    few->samples[1000 * 2] = 1.0f;
    take->data = few;
    CHECK(!measure(*take, timing, SR));
}

int main() {
    testNames();
    testSamplerPitch();
    testSamplerEnvelope();
    testSamplerClips();
    testNumpyRandom();
    testSimpleEffects();
    testArpeggiator();
    testApplyAndLive();
    testRecordOffset();
    TEST_MAIN_END();
}
