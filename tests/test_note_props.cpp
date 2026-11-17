// Tests of the notes' own properties (synth.h: Note::chance, repeats, slide, pan, fine; note_props): which
// notes play (decided the same way every time), rolls, glides in the built-in synth and sampler, pan and
// fine tuning, the legato events for VST instruments, MIDI effects keeping them, render == playback, and
// saving and opening them (old projects unchanged).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/midi_effects.h"
#include "audio/render.h"
#include "audio/sampler.h"
#include "audio/synth.h"
#include "check.h"
#include "config.h"
#include "core/json.h"
#include "core/util.h"
#include "model/model.h"
#include "model/project_io.h"

namespace {
const int SR = 48000;

Note note(double s, double e, double pitch, double vel = 100) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

bool sameNotes(const std::vector<Note>& a, const std::vector<Note>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].pitch != b[i].pitch ||
            a[i].velocity != b[i].velocity || a[i].chance != b[i].chance || a[i].repeats != b[i].repeats ||
            a[i].slide != b[i].slide || a[i].pan != b[i].pan || a[i].fine != b[i].fine)
            return false;
    return true;
}

// the frequency of a (mono) signal around frame `at`: the lag, within that of 80…2000 Hz, at which it best
// matches itself (normalized autocorrelation over `window` frames), refined between frames
double frequencyAt(const std::vector<float>& x, int64_t at, int64_t window = 960) {
    int64_t lo = SR / 2000, hi = SR / 80;
    int64_t a = at - window / 2;
    if (a < 0 || a + window + hi >= (int64_t)x.size()) return 0;
    auto score = [&](int64_t lag) {
        double num = 0, e1 = 0, e2 = 0;
        for (int64_t i = 0; i < window; i++) {
            double p = x[(size_t)(a + i)], q = x[(size_t)(a + i + lag)];
            num += p * q, e1 += p * p, e2 += q * q;
        }
        return e1 > 0 && e2 > 0 ? num / std::sqrt(e1 * e2) : 0.0;
    };
    // the first lag past the zero-lag peak that scores near the best (not a multiple of the period)
    std::vector<double> s((size_t)(hi + 2), 0.0);
    double best = 0;
    for (int64_t lag = lo; lag <= hi + 1; lag++) best = std::max(best, s[(size_t)lag] = score(lag));
    for (int64_t lag = lo + 1; lag <= hi; lag++)
        if (s[(size_t)lag] >= 0.9 * best && s[(size_t)lag] >= s[(size_t)lag - 1] && s[(size_t)lag] >= s[(size_t)lag + 1]) {
            double l = s[(size_t)lag - 1], c = s[(size_t)lag], r = s[(size_t)lag + 1];
            double shift = (l - 2 * c + r) != 0 ? 0.5 * (l - r) / (l - 2 * c + r) : 0.0;
            return SR / ((double)lag + shift);
        }
    return 0;
}

std::vector<float> channel(const AudioPtr& a, int c) {
    std::vector<float> out;
    for (int64_t i = 0; i < a->frames(); i++) out.push_back(a->samples[(size_t)(2 * i + c)]);
    return out;
}

double energy(const std::vector<float>& x, int64_t from = 0, int64_t to = -1) {
    if (to < 0) to = (int64_t)x.size();
    double e = 0;
    for (int64_t i = from; i < to && i < (int64_t)x.size(); i++) e += (double)x[(size_t)i] * x[(size_t)i];
    return e;
}

ClipPtr sineSample(double hz, double seconds) {  // a sample: a sine, the same on both sides
    auto b = std::make_shared<AudioBuffer>();
    for (int64_t i = 0; i < (int64_t)(seconds * SR); i++) {
        float v = (float)(0.5 * std::sin(2 * M_PI * hz * (double)i / SR));
        b->samples.push_back(v), b->samples.push_back(v);
    }
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->source = b;
    c->sourceSr = SR;
    return c;
}
}  // namespace

static void testChance() {
    // 100 % and 0 %: always and never, whatever the pass
    Note n = note(0.5, 0.75, 60);
    for (int64_t pass = 0; pass < 50; pass++) CHECK(note_props::sounds(n, pass));
    n.chance = 0;
    for (int64_t pass = 0; pass < 50; pass++) CHECK(!note_props::sounds(n, pass));
    auto silent = makeMidiClip("never", {n}, 1.0, SR);
    CHECK(silent->played->empty() && silent->events->empty());
    double loudest = 0;
    for (float v : silent->data->samples) loudest = std::max(loudest, (double)std::fabs(v));
    CHECK(loudest == 0);  // (nothing played: silence)
    // the rate over many notes is the chance
    std::vector<Note> many;
    for (int i = 0; i < 4000; i++) {
        Note m = note(i * 0.01, i * 0.01 + 0.005, 36 + i % 48);
        m.chance = 30;
        many.push_back(m);
    }
    auto clip = makeMidiClip("maybe", many, 40.0, SR);
    double rate = (double)clip->played->size() / many.size();
    CHECK(rate > 0.27 && rate < 0.33);
    printf("chance 30 %%: %.1f %% of 4000 notes played\n", rate * 100);
    // decided the same way every time it is prepared (a re-prepare doesn't reshuffle)
    std::vector<Note> first = *clip->played;
    clip->prepare(SR);
    CHECK(sameNotes(first, *clip->played));
    auto again = makeMidiClip("maybe again", many, 40.0, SR);
    CHECK(sameNotes(first, *again->played));
    CHECK(again->data->samples == clip->data->samples);  // (its audio too)
    // …and another note elsewhere doesn't change which of the others play
    std::vector<Note> more = many;
    more.insert(more.begin(), note(100.0, 100.1, 90));
    auto moreClip = makeMidiClip("more", more, 101.0, SR);
    std::vector<Note> others;
    for (const Note& m : *moreClip->played)
        if (m.pitch != 90) others.push_back(m);
    CHECK(sameNotes(first, others));
    // a loop's passes each decide afresh (the same every time, but not all alike)
    std::vector<Note> bar;
    for (int i = 0; i < 16; i++) {
        Note m = note(i * 0.125, i * 0.125 + 0.1, 60);
        m.chance = 50;
        bar.push_back(m);
    }
    auto looped = makeMidiClip("loop", bar, 2.0, SR);
    looped->loop = true;
    looped->length = SR * 16;  // 8 passes
    looped->prepare(SR);
    std::vector<std::vector<double>> passes(8);
    for (const Note& m : *looped->played) passes[(size_t)(m.start / 2.0)].push_back(std::fmod(m.start, 2.0));
    bool differ = false;
    for (int k = 1; k < 8; k++) differ = differ || passes[(size_t)k] != passes[0];
    CHECK(differ);
    std::vector<Note> loopedFirst = *looped->played;
    looped->prepare(SR);
    CHECK(sameNotes(loopedFirst, *looped->played));
    // a switched-off note stays silent, whatever its chance
    auto offClip = makeMidiClip("off", {note(0, 0.5, 60)}, 1.0, SR);
    Note sure = note(0.5, 1.0, 64);
    offClip->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{sure});
    offClip->prepare(SR);
    CHECK(offClip->played->size() == 1 && (*offClip->played)[0].pitch == 60);
    // the notes as written (heardNotes: MIDI export, joining clips) keep their properties, undecided
    CHECK(clip->heardNotes(SR).size() == many.size() && clip->heardNotes(SR)[0].chance == 30);
}

static void testRepeats() {
    Note n = note(1.0, 2.0, 60, 90);
    n.repeats = 4;
    n.slide = true;
    n.pan = 0.5;
    std::vector<Note> out;
    note_props::perform(n, 0, out);
    CHECK(out.size() == 4);
    for (int k = 0; k < 4 && out.size() == 4; k++) {
        CHECK_NEAR(out[(size_t)k].start, 1.0 + 0.25 * k, 1e-12);
        CHECK_NEAR(out[(size_t)k].end, 1.25 + 0.25 * k, 1e-12);
        CHECK(out[(size_t)k].pitch == 60 && out[(size_t)k].velocity == 90 && out[(size_t)k].pan == 0.5);
        CHECK(out[(size_t)k].repeats == 1);
        CHECK(out[(size_t)k].slide == (k == 0));  // (only the first hit slides)
    }
    CHECK(out.back().end == 2.0);  // (the last hit ends where the note does)
    // more than 8 hits: 8
    n.repeats = 50;
    out.clear();
    note_props::perform(n, 0, out);
    CHECK((int)out.size() == note_props::MAX_REPEATS);
    // in a clip: each hit is a note heard (and an event for a VST instrument), each with its own attack
    Note roll = note(0.0, 0.5, 72);
    roll.repeats = 3;
    auto clip = makeMidiClip("roll", {roll}, 1.0, SR);
    CHECK(clip->played->size() == 3 && clip->events->size() == 3);
    if (clip->events->size() == 3) {
        CHECK((*clip->events)[1].on == (int64_t)std::nearbyint(0.5 / 3 * SR));
        CHECK((*clip->events)[1].off == (int64_t)std::nearbyint(1.0 / 3 * SR));
    }
    // the synth plays them as three notes, each struck afresh (its own attack)
    CHECK(clip->data->samples == synth::render(*clip->played, SR, 1.0)->samples);
    for (size_t k = 0; k < clip->played->size(); k++) {
        CHECK_NEAR((*clip->played)[k].start, 0.5 / 3 * (double)k, 1e-12);
        CHECK_NEAR((*clip->played)[k].end, 0.5 / 3 * (double)(k + 1), 1e-12);
    }
    CHECK(energy(channel(clip->data, 0)) > 0);
    // the same notes with repeats of 1 play as they always did (bit for bit)
    auto plain = makeMidiClip("plain", {note(0, 0.5, 60), note(0.5, 1.0, 64)}, 1.0, SR);
    auto direct = synth::render({note(0, 0.5, 60), note(0.5, 1.0, 64)}, SR, 1.0);
    CHECK(plain->data->samples == direct->samples);
}

static void testSynthGlide() {
    // A (C4) then B (C5) sliding from it, legato: one voice gliding up over GLIDE_S
    Note a = note(0.0, 0.5, 60), b = note(0.5, 1.0, 72);
    b.slide = true;
    std::vector<note_props::Slide> s = note_props::slides({a, b});
    CHECK(s[0].from == -1 && s[1].from == 0 && s[1].legato);
    AudioPtr glide = synth::render({a, b}, SR, 1.0);
    std::vector<float> x = channel(glide, 0);
    double f60 = synth::noteFrequency(60), f72 = synth::noteFrequency(72), f66 = synth::noteFrequency(66);
    double before = frequencyAt(x, (int64_t)(0.3 * SR)), after = frequencyAt(x, (int64_t)(0.8 * SR));
    CHECK(std::fabs(before - f60) < f60 * 0.01);
    CHECK(std::fabs(after - f72) < f72 * 0.01);
    // halfway through the glide: halfway in pitch (F#4), well between the two
    double mid = frequencyAt(x, (int64_t)((0.5 + note_props::GLIDE_S / 2) * SR), 480);
    printf("glide C4 -> C5: %.1f Hz, mid-glide %.1f Hz (F#4 %.1f), then %.1f Hz\n", before, mid, f66, after);
    CHECK(mid > synth::noteFrequency(63) && mid < synth::noteFrequency(69));
    // no new attack: the level never dips where B takes the voice over (one voice, the phase unbroken)
    double rmsBefore = std::sqrt(energy(x, (int64_t)(0.45 * SR), (int64_t)(0.49 * SR)) / (0.04 * SR));
    double rmsAt = std::sqrt(energy(x, (int64_t)(0.5 * SR), (int64_t)(0.504 * SR)) / (0.004 * SR));
    CHECK(rmsAt > rmsBefore * 0.6);
    // without the slide: a new note (an attack from silence) at 0.5 s
    AudioPtr apart = synth::render({a, note(0.5, 1.0, 72)}, SR, 1.0);
    std::vector<float> y = channel(apart, 0);
    CHECK(frequencyAt(y, (int64_t)((0.5 + note_props::GLIDE_S / 2) * SR), 480) > f72 * 0.98 ||
          frequencyAt(y, (int64_t)((0.5 + note_props::GLIDE_S / 2) * SR), 480) < f60 * 1.02);
    // a sliding note after a gap glides too, but with its own attack (no voice to take over)
    Note late = note(0.7, 1.0, 72);
    late.slide = true;
    s = note_props::slides({a, late});
    CHECK(s[1].from == 0 && !s[1].legato);
    AudioPtr gap = synth::render({a, late}, SR, 1.0);
    std::vector<float> z = channel(gap, 0);
    CHECK(std::fabs(z[(size_t)(0.7 * SR)]) < 0.01);
    double gapMid = frequencyAt(z, (int64_t)((0.7 + note_props::GLIDE_S / 2) * SR), 480);
    CHECK(gapMid > synth::noteFrequency(63) && gapMid < synth::noteFrequency(69));
    // a slide with nothing before it: an ordinary note
    s = note_props::slides({b});
    CHECK(s[0].from == -1);
    CHECK(synth::render({b}, SR, 1.0)->samples == synth::render({note(0.5, 1.0, 72)}, SR, 1.0)->samples);
    // a chain: three notes gliding into each other are one voice
    Note c = note(1.0, 1.5, 67);
    c.slide = true;
    s = note_props::slides({a, b, c});
    CHECK(s[2].from == 1 && s[2].legato);
    std::vector<float> w = channel(synth::render({a, b, c}, SR, 1.5), 0);
    CHECK(std::fabs(frequencyAt(w, (int64_t)(1.3 * SR)) - synth::noteFrequency(67)) < 2.0);
}

static void testPanAndFine() {
    // pan: hard left leaves the right silent, a little right is louder on the right
    Note left = note(0, 0.5, 60);
    left.pan = -1;
    AudioPtr l = synth::render({left}, SR, 0.5);
    CHECK(energy(channel(l, 0)) > 1 && energy(channel(l, 1)) < 1e-12);
    Note right = note(0, 0.5, 60);
    right.pan = 0.5;
    AudioPtr r = synth::render({right}, SR, 0.5);
    CHECK(energy(channel(r, 1)) > energy(channel(r, 0)) * 1.5);
    // in the middle: as before (the same on both sides, bit for bit)
    AudioPtr c = synth::render({note(0, 0.5, 60)}, SR, 0.5);
    CHECK(channel(c, 0) == channel(c, 1));
    // fine tuning: +100 cents is the next semitone, exactly
    Note up = note(0, 0.5, 60);
    up.fine = 100;
    CHECK(synth::render({up}, SR, 0.5)->samples == synth::render({note(0, 0.5, 61)}, SR, 0.5)->samples);
    Note bit = note(0, 0.5, 69);
    bit.fine = 50;
    double f = frequencyAt(channel(synth::render({bit}, SR, 0.5), 0), SR / 4);
    CHECK(std::fabs(f - 440 * std::pow(2.0, 0.5 / 12)) < 2.0);

    // the sampler: a 440 Hz sine played at its root (A4)
    SamplerSettings s;
    s.sample = sineSample(440, 2.0);
    s.root = 69;
    s.attack = 0, s.decay = 0, s.sustain = 1, s.release = 0.05;
    s.velocity = 0;
    Note sl = note(0, 0.5, 69);
    sl.pan = -1;
    AudioPtr sp = sampler::render(s, {sl}, SR, 0.5);
    CHECK(energy(channel(sp, 0)) > 1 && energy(channel(sp, 1)) < 1e-12);
    Note sf = note(0, 0.5, 69);
    sf.fine = 100;
    CHECK(sampler::render(s, {sf}, SR, 0.5)->samples == sampler::render(s, {note(0, 0.5, 70)}, SR, 0.5)->samples);
    CHECK(sampler::render(s, {note(0, 0.5, 69)}, SR, 0.5)->samples ==
          sampler::render(s, {note(0, 0.5, 69)}, SR, 0.5)->samples);
    // a slide in the sampler: the sample plays on, its speed gliding from A4's to A5's
    Note sa = note(0, 0.5, 69), sb = note(0.5, 1.0, 81);
    sb.slide = true;
    std::vector<float> g = channel(sampler::render(s, {sa, sb}, SR, 1.0), 0);
    double gBefore = frequencyAt(g, (int64_t)(0.3 * SR)), gAfter = frequencyAt(g, (int64_t)(0.8 * SR));
    double gMid = frequencyAt(g, (int64_t)((0.5 + note_props::GLIDE_S / 2) * SR), 480);
    printf("sampler glide A4 -> A5: %.1f Hz, mid-glide %.1f Hz, then %.1f Hz\n", gBefore, gMid, gAfter);
    CHECK(std::fabs(gBefore - 440) < 4.4 && std::fabs(gAfter - 880) < 8.8);
    CHECK(gMid > 440 * std::pow(2.0, 3 / 12.0) && gMid < 440 * std::pow(2.0, 9 / 12.0));
    // …and a one-shot sample glides too (to its end)
    s.oneShot = true;
    std::vector<float> o = channel(sampler::render(s, {sa, sb}, SR, 1.0), 0);
    CHECK(std::fabs(frequencyAt(o, (int64_t)(0.8 * SR)) - 880) < 8.8);
}

static void testEventsAndEffects() {
    // a VST instrument: a sliding note's note-on comes before the note it slides from lets go
    Note a = note(0.0, 0.5, 60), b = note(0.5, 1.0, 64);
    b.slide = true;
    auto clip = makeMidiClip("legato", {a, b}, 1.0, SR);
    CHECK(clip->events->size() == 2);
    if (clip->events->size() == 2) {
        const Clip::Event& first = (*clip->events)[0];
        const Clip::Event& second = (*clip->events)[1];
        CHECK(first.pitch == 60 && second.pitch == 64);
        CHECK(second.on == SR / 2 && first.off > second.on);  // (the overlap: legato)
    }
    // without the slide they meet, as before
    auto plain = makeMidiClip("plain", {a, note(0.5, 1.0, 64)}, 1.0, SR);
    CHECK((*plain->events)[0].off == SR / 2);
    // sliding to the same pitch: no overlap (its note-off would end the new note)
    Note same = note(0.5, 1.0, 60);
    same.slide = true;
    auto tie = makeMidiClip("tie", {a, same}, 1.0, SR);
    CHECK((*tie->events)[0].off == SR / 2);
    // the MIDI effects keep a note's own properties (a Pitch, a Chord), and work on the notes as played
    Project p;
    audio::Engine e(p);
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    MidiEffect pitch, chord;
    MidiEffect::make("Pitch", {{"semitones", 12}}, pitch);
    MidiEffect::make("Chord", {{"chord", 2}}, chord);
    t->midiEffects = std::vector<MidiEffect>{pitch, chord};
    Note panned = note(0, 0.5, 48);
    panned.pan = -0.5;
    panned.fine = 20;
    panned.repeats = 2;
    auto mc = makeMidiClip("fx", {panned}, 1.0, SR);
    mc->track = t;
    mc->prepare(SR);
    CHECK(mc->played->size() == 6);  // (2 hits × a 3-note chord)
    for (const Note& n : *mc->played) CHECK(n.pan == -0.5 && n.fine == 20 && n.pitch >= 60);
}

static void testRenderEqualsPlayback() {
    // the built-in synth and sampler playing notes with every property: a render gives exactly what playback plays
    Project p;
    audio::Engine e(p);
    e.offline = true;
    auto syn = p.addTrack(Kind::Midi);
    syn->needsInstrument = false;
    std::vector<Note> ns;
    for (int i = 0; i < 24; i++) {
        Note n = note(i * 0.125, i * 0.125 + 0.12, 48 + (i * 5) % 24, 70 + i);
        n.chance = 60;
        n.repeats = 1 + i % 3;
        n.slide = i % 4 == 1;
        n.pan = (i % 5 - 2) * 0.4;
        n.fine = (i % 7 - 3) * 10;
        ns.push_back(n);
    }
    auto sc = makeMidiClip("synth", ns, 3.0, SR);
    sc->track = syn;
    sc->start = SR / 2;
    sc->loop = true;
    sc->length = SR * 5;
    sc->prepare(SR);
    p.addClip(sc);
    auto smp = p.addTrack(Kind::Midi);
    smp->needsInstrument = false;
    auto settings = std::make_shared<SamplerSettings>();
    settings->sample = sineSample(330, 1.0);
    settings->root = 64;
    settings->loop = true;
    smp->sampler = settings;
    auto mc = makeMidiClip("sampler", ns, 3.0, SR);
    mc->track = smp;
    mc->start = SR;
    mc->prepare(SR);
    p.addClip(mc);
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
    double loudest = 0;
    for (int64_t j = 0; j < (end - start) * 2; j++) {
        float x = (*rendered)[(size_t)j], y = played[(size_t)((start + e.latency) * 2 + j)];
        if (x != y) differ++;
        loudest = std::max(loudest, (double)std::fabs(x));
    }
    CHECK(differ == 0);
    CHECK(loudest > 0.05);
    // a clip prepared again (an edit elsewhere, an undo) sounds the same: nothing reshuffled
    AudioPtr before = sc->data;
    sc->prepare(SR);
    CHECK(sc->data->samples == before->samples);
}

static void testSaveAndOpen() {
    std::string dir = "build/test-note-props";
    util::makeDirs(dir);
    Project p;
    audio::Engine e(p);
    auto t = p.addTrack(Kind::Midi);
    t->needsInstrument = false;
    Note a = note(0, 0.5, 60, 100);
    Note b = note(0.5, 1.0, 62.5, 90);
    b.chance = 42.5;
    b.repeats = 3;
    b.slide = true;
    b.pan = -0.35;
    b.fine = -12;
    Note c = note(1.0, 1.5, 64);
    c.slide = true;  // (just one: only it is written)
    Note off = note(1.5, 2.0, 65);
    off.chance = 10;
    auto clip = makeMidiClip("props", {a, b, c}, 2.0, SR);
    clip->mutedNotes = std::make_shared<const std::vector<Note>>(std::vector<Note>{off});
    clip->track = t;
    clip->prepare(SR);
    p.addClip(clip);
    project_io::MasterSettings master;
    const Json j = project_io::describe(p, SR, master);
    const Json& entries = j["clips"][size_t(0)]["notes"];
    CHECK(entries.size() == 3);
    CHECK(entries[size_t(0)].size() == 4);  // (a plain note: [start, end, pitch, velocity], as always)
    CHECK(entries[size_t(1)].size() == 5 && entries[size_t(1)][size_t(4)].isObject());
    const Json& props = entries[size_t(1)][size_t(4)];
    CHECK(props.size() == 5 && props.num("chance", 0) == 42.5 && props.integer("repeats", 0) == 3 &&
          props.flag("slide", false) && props.num("pan", 0) == -0.35 && props.num("fine", 0) == -12);
    CHECK(entries[size_t(2)][size_t(4)].size() == 1 && entries[size_t(2)][size_t(4)].flag("slide", false));
    CHECK(j["clips"][size_t(0)]["muted_notes"][size_t(0)][size_t(4)].num("chance", 0) == 10);
    // saved and opened: every field back
    std::string err, path = dir + "/props.json";
    CHECK(project_io::saveProject(path, p, SR, master, err));
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, err));
    CHECK(q.clips.size() == 1);
    if (q.clips.size() == 1) {
        CHECK(sameNotes(*q.clips[0]->notes, *clip->notes));
        CHECK(q.clips[0]->mutedNotes && sameNotes(*q.clips[0]->mutedNotes, *clip->mutedNotes));
        CHECK(sameNotes(*q.clips[0]->played, *clip->played));  // (the same notes play: chance and all)
        CHECK(q.clips[0]->data->samples == clip->data->samples);
    }
    // a project without such notes is written exactly as before: no fifth items anywhere
    Project old;
    audio::Engine e2(old);
    auto ot = old.addTrack(Kind::Midi);
    auto oc = makeMidiClip("old", {note(0, 0.5, 60), note(0.5, 1, 67, 80)}, 1.0, SR);
    oc->track = ot;
    old.addClip(oc);
    std::string text = project_io::describe(old, SR, master).dump();
    CHECK(text.find("\"notes\": [[0.0, 0.5, 60.0, 100.0], [0.5, 1.0, 67.0, 80.0]]") != std::string::npos);
    CHECK(text.find("chance") == std::string::npos && text.find("repeats") == std::string::npos);
    // an old project's notes open with the defaults; damaged properties are kept in range
    std::string oldPath = dir + "/old.json";
    CHECK(project_io::saveProject(oldPath, old, SR, master, err));
    std::string saved;
    CHECK(util::readFile(oldPath, saved));
    std::string tweaked = saved;
    size_t at = tweaked.find("67.0,");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        size_t close = tweaked.find(']', at);
        tweaked.insert(close, ", {\"chance\": 250, \"repeats\": 99, \"pan\": -7, \"fine\": \"x\", \"slide\": 1}");
    }
    std::string tweakedPath = dir + "/tweaked.json";
    CHECK(util::writeFile(tweakedPath, tweaked));
    Project r;
    CHECK(project_io::loadProject(tweakedPath, r, SR, m2, err));
    if (r.clips.size() == 1 && r.clips[0]->notes->size() == 2) {
        const Note& plain = (*r.clips[0]->notes)[0];
        CHECK(plain.plain() && plain.chance == 100 && plain.repeats == 1 && !plain.slide && plain.pan == 0 &&
              plain.fine == 0);
        const Note& odd = (*r.clips[0]->notes)[1];
        CHECK(odd.chance == 100 && odd.repeats == 8 && odd.pan == -1 && odd.fine == 0 && !odd.slide);
    } else {
        CHECK(false);
    }
    // a fifth item that isn't an object: a damaged note
    std::string broken = saved;
    at = broken.find("67.0,");
    if (at != std::string::npos) broken.insert(broken.find(']', at), ", 5");
    std::string brokenPath = dir + "/broken.json";
    CHECK(util::writeFile(brokenPath, broken));
    Project bq;
    CHECK(!project_io::loadProject(brokenPath, bq, SR, m2, err));
}

int main() {
    testChance();
    testRepeats();
    testSynthGlide();
    testPanAndFine();
    testEventsAndEffects();
    testRenderEqualsPlayback();
    testSaveAndOpen();
    TEST_MAIN_END();
}
