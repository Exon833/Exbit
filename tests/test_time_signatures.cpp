// Tests of time signatures with a denominator (timing.h TimeSig: 6/8, 7/8, 5/16, 3/2, 12/8…): bars and beats
// against seconds (with changes and ramps), the grid, the ruler and snapping, the metronome (clicks on the
// unit, accents), MIDI files' time_signature events, the time info plugins and JACK get, projects saved and
// loaded, the fallbacks for bad values, the tempo lane's signature API, inserting time by bars; a worst case
// (a signature change every bar through every denominator, 64 tracks playing, rendered, saved and loaded);
// and a REGRESSION check: songs in N/4 give bit for bit what they gave before denominators (the hashes
// below were made by this file's REGRESSION_ONLY part built on main before the change: make
// build/tests/test_time_signatures CXXFLAGS+=-DREGRESSION_ONLY there, and run it).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/automation.h"
#include "audio/automation_write.h"
#include "audio/engine.h"
#include "audio/file_io.h"
#include "audio/metronome.h"
#include "audio/midi_sync.h"
#include "audio/plugin_audio.h"
#include "audio/render.h"
#include "audio/sound_file.h"
#include "check.h"
#include "config.h"
#include "core/util.h"
#include "model/model.h"
#include "model/project_io.h"
#include "model/song_time.h"
#include "model/timing.h"
#include "ui/recording.h"

namespace {
const int SR = 48000;
const std::string kDir = "build/test-tmp/time_signatures";

// FNV-1a over everything a song's timing gives
struct Hash {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 1099511628211ULL;
    }
    void d(double v) { bytes(&v, sizeof v); }
    void i(int64_t v) { bytes(&v, sizeof v); }
    void s(const std::string& v) { i((int64_t)v.size()), bytes(v.data(), v.size()); }
    void f(const std::vector<float>& v) { bytes(v.data(), v.size() * sizeof(float)); }
};

// songs in N/4 (as every song was before denominators): steps, ramps, changes inside bars, signatures
Timing n4Song(int which) {
    Timing t;
    t.timebaseBeats = true;
    if (which == 0) {
        t.bpm = 120, t.beatsPerBar = 4;
    } else if (which == 1) {
        t.bpm = 96, t.beatsPerBar = 3, t.ramp = true;
        t.changes = {TempoChange{3, 140, 5}, TempoChange{5, 140, 7, 0, true}, TempoChange{7, 70, 2},
                     TempoChange{8, 110, 2, 1.5}, TempoChange{10, 180, 4}};
    } else {
        t.bpm = 77.5, t.beatsPerBar = 7;
        t.changes = {TempoChange{2, 150, 1}, TempoChange{4, 61, 16, 0, true}, TempoChange{6, 200, 9, 3.25, false},
                     TempoChange{9, 99, 6}};
    }
    t.publish();
    return t;
}

void hashTiming(Hash& h, const Timing& song) {
    for (bool thirds : {false, true})
        for (double pps : {4.0, 25.0, 90.0, 400.0}) {
            Timing t = song;
            t.thirds = thirds;
            for (auto& l : t.lines(0, 40, pps)) h.d(l.seconds), h.i(l.strength), h.s(l.label);
            for (double s = 0; s < 40; s += 0.37)
                h.d(t.snap(s, pps)), h.d(t.snap(s, pps, true)), h.d(t.gridStep(pps, s).value());
            for (const Frac& g : timing::fixedSteps(thirds)) {
                t.grid = g.value();
                for (double s = 0; s < 40; s += 0.53) h.d(t.snap(s, pps)), h.d(t.gridStep(pps, s, true).value());
                for (auto& l : t.lines(0, 20, pps)) h.d(l.seconds), h.i(l.strength), h.s(l.label);
                h.s(t.gridName());
                t.stepGrid(true, pps, 3.0);
                h.d(t.grid);
            }
            t.grid = 0;
            t.stepGrid(false, pps, 7.0);
            h.d(t.grid);
            for (double s = 0; s < 40; s += 0.29) {
                int bar, beat, part;
                t.position(s, bar, beat, part);
                h.i(bar), h.i(beat), h.i(part);
            }
        }
    for (auto& b : song.beatTimes(0, 40)) h.d(b.seconds), h.i(b.barStart);
    for (int bar = 0; bar < 30; bar++) h.d(song.barSeconds(bar)), h.d(song.barBeat(bar));
    for (double s = 0; s < 40; s += 0.41) h.i(song.barAt(s)), h.d(song.beatAt(s)), h.d(song.bpmAt(s));
    h.s(project_io::describeTiming(song).dump());
    TempoMapPtr m = song.map();
    for (double s = 0; s < 40; s += 0.61) {
        jack::Position pos{};
        midi_sync::fillBBT(*m, (uint32_t)(s * SR), SR, pos);
        h.i(pos.bar), h.i(pos.beat), h.i(pos.tick), h.d(pos.bar_start_tick), h.d(pos.beats_per_bar), h.d(pos.beat_type),
            h.d(pos.beats_per_minute);
        plugin_audio::Transport tr = plugin_audio::Transport::at(*m, true, (int64_t)(s * SR), SR);
        h.d(tr.tempo), h.i(tr.numerator), h.i(tr.denominator), h.d(tr.beats);
    }
    for (bool perBar : {true, false})
        for (auto& p : automation::drawShape({}, 0.3, 17.9, automation::Shape::Triangle, song, perBar))
            h.d(p.t), h.d(p.v);
}

void hashClicks(Hash& h, const Timing& song) {
    metronome::MetronomeSettings settings;
    metronome::Clicks clicks = metronome::renderClicks(settings, SR);
    std::vector<float> out(512 * 2);
    for (int64_t f = 0; f < 20 * SR; f += 512) {
        std::fill(out.begin(), out.end(), 0.0f);
        metronome::addClicks(out.data(), 2, 512, f, clicks, song, SR);
        h.f(out);
    }
    for (int bars : {1, 2})
        for (double at : {0.0, 3.3, 9.0}) {
            int64_t frames = 0;
            AudioPtr a = metronome::countIn(settings, song, bars, SR, at, frames);
            h.i(frames);
            if (a) h.f(a->samples);
            AudioPtr b;
            h.i(recording::countInAudio(settings, song, bars, SR, b, at));
            if (b) h.f(b->samples);
        }
}

// MIDI clips (the built-in synth) and volume automation a cycle per bar, rendered; the song's MIDI file
std::vector<float> renderSong(const Timing& song, int tracks, double seconds, Project* keep = nullptr) {
    Project local;
    Project& p = keep ? *keep : local;
    audio::Engine e(p);  // (sets the MIDI clips' renderer: before they're made)
    e.offline = true;
    p.timing = song;
    for (int i = 0; i < tracks; i++) {
        auto t = p.addTrack(Kind::Midi);
        t->needsInstrument = false;
        auto c = std::make_shared<Clip>();
        c->kind = Kind::Midi;
        c->name = "notes";
        c->track = t;
        c->lengthS = seconds;
        std::vector<Note> notes;
        for (double b = 0; song.secondsAt(b) < seconds; b += 0.5 + 0.25 * (i % 3)) {
            double s = song.secondsAt(b);
            notes.push_back(Note{s, std::min(seconds, song.secondsAt(b + 0.4)), (double)(40 + (i * 7 + (int)(b * 2)) % 40),
                                 90});
        }
        c->notes = std::make_shared<const std::vector<Note>>(std::move(notes));
        c->prepare(SR);
        p.addClip(c);
        auto lane = std::make_shared<Automation>();
        lane->slot = t->controls;
        lane->param = automation::VOLUME;
        lane->setPoints(automation::drawShape({}, 0, seconds, automation::Shape::Sine, p.timing, i % 2 == 0));
        auto lanes = *t->automation.get();
        lanes.push_back(lane);
        t->automation = lanes;
    }
    auto out = render::renderMix(p, 0, (int64_t)(seconds * SR), SR);
    return out ? *out : std::vector<float>();
}

std::string midiBytes(const Timing& song, const std::string& name) {
    file_io::MidiPart part{"Part", {Note{0.25, 1.0, 60, 100}, Note{2.0, 7.5, 64, 80}}};
    std::string path = kDir + "/" + name, error, text;
    CHECK(file_io::saveMidi(path, {part}, song, 0.0, error));
    CHECK(util::readFile(path, text));
    return text;
}

uint64_t regressionHash(int which) {
    Timing song = n4Song(which);
    Hash h;
    hashTiming(h, song);
    hashClicks(h, song);
    h.f(renderSong(song, 6, 12.0));
    h.s(midiBytes(song, "n4_" + std::to_string(which) + ".mid"));
    return h.h;
}

// made by the REGRESSION_ONLY build on main (before denominators): see the top of this file
const uint64_t MAIN_HASHES[] = {0x10901230fabedae1ULL, 0xabd9d26c56cbb631ULL, 0x16b8c58b3f39d84eULL};

#ifndef REGRESSION_ONLY
// a song with one signature from bar 1 at `bpm`
Timing inSig(TimeSig sig, double bpm = 120) {
    Timing t;
    t.bpm = bpm;
    t.setSignature(1, sig);
    t.publish();
    return t;
}

void testBarsAndSeconds() {
    struct Case {
        TimeSig sig;
        double barQ;
    } cases[] = {{{3, 4}, 3}, {{6, 8}, 3}, {{7, 8}, 3.5}, {{5, 16}, 1.25}, {{3, 2}, 6}, {{12, 8}, 6}};
    for (auto& c : cases) {
        Timing t = inSig(c.sig);  // (120 BPM: a quarter 0.5 s)
        CHECK(c.sig.barQuarters().value() == c.barQ);
        for (int bar = 0; bar < 12; bar++) {
            CHECK(t.barBeat(bar) == bar * c.barQ);
            CHECK_NEAR(t.barSeconds(bar), bar * c.barQ * 0.5, 1e-12);
            CHECK(t.barAt(t.barSeconds(bar) + 1e-6) == bar);
        }
        // position(): bars and beats counted in the unit (8ths in 7/8), parts the 16ths in it
        double unitS = 0.5 * 4.0 / c.sig.den;
        int bar, beat, part;
        t.position(t.barSeconds(2) + 2 * unitS, bar, beat, part);
        CHECK(bar == 3 && beat == 3 && part == 1);
        t.position(t.barSeconds(2) + 2.5 * unitS, bar, beat, part);  // (half a unit on: 16ths in it, if any)
        CHECK(bar == 3 && beat == 3 && part == std::max(1, 16 / c.sig.den) / 2 + 1);
        t.position(t.barSeconds(1) + (c.sig.num - 1) * unitS, bar, beat, part);
        CHECK(bar == 2 && beat == c.sig.num);
        for (double s = 0; s < 20; s += 0.173) {
            CHECK_NEAR(t.secondsAt(t.beatAt(s)), s, 1e-9);
        }
        CHECK(t.sigAt(5.0) == c.sig && t.barQuartersAt(5.0) == c.barQ);
    }
    // changes mid-song with ramps, against fine numeric integration of 60 / BPM
    Timing t;
    t.bpm = 120;  // 4/4 for bars 1-2 (8 quarters)
    t.changes = {TempoChange{3, 120, 7, 0, true, 8},   // 7/8 from bar 3, ramping…
                 TempoChange{5, 60, 5, 0, false, 16},  // …to 60 BPM at bar 5 (15 quarters), then 5/16
                 TempoChange{7, 90, 3, 0, true, 2},    // 3/2 at bar 7 (17.5), ramping to…
                 TempoChange{9, 150, 12, 0, false, 8}};  // …150 at bar 9 (29.5), 12/8 on
    double expectBeats[] = {0, 4, 8, 11.5, 15, 16.25, 17.5, 23.5, 29.5, 35.5, 41.5};
    for (int bar = 0; bar < 11; bar++) {
        CHECK(t.barBeat(bar) == expectBeats[bar]);
    }
    auto bpmAt = [](double b) {
        if (b < 8) return 120.0;
        if (b < 15) return 120 - 60 * (b - 8) / 7;
        if (b < 17.5) return 60.0;
        if (b < 29.5) return 90 + 60 * (b - 17.5) / 12;
        return 150.0;
    };
    double secs = 0;
    for (int bar = 1; bar < 11; bar++) {  // (Simpson's rule over each bar)
        const int N = 2000;
        double a = expectBeats[bar - 1], h = (expectBeats[bar] - a) / N, sum = 0;
        for (int k = 0; k <= N; k++) sum += (k == 0 || k == N ? 1 : k % 2 ? 4 : 2) * 60 / bpmAt(a + k * h - (k == N ? 1e-12 : 0));
        secs += sum * h / 3;
        CHECK_NEAR(t.barSeconds(bar), secs, 1e-9);
    }
    CHECK(t.sigAtBar(4) == (TimeSig{7, 8}) && t.sigAtBar(6) == (TimeSig{5, 16}) && t.sigAtBar(8) == (TimeSig{3, 2}) &&
          t.sigAtBar(10) == (TimeSig{12, 8}));
    // a change inside a 7/8 bar: its beat is quarters into the bar, held inside it
    t.changes.push_back(TempoChange{4, 100, 7, 3.0, false, 8});
    CHECK(t.map()->beatOf(t.changes.back()) == 11.5 + 3.0);
    TempoChange past{4, 100, 7, 9.0, false, 8};  // (past the bar's end: its end)
    CHECK(t.map()->beatOf(past) == 11.5 + 3.5);
}

void testGridAndSnapping() {
    // 6/8 at 120: zoomed in, a beat line every eighth (labelled 1.2 … 1.6), the bar's lines on bar lines
    Timing t = inSig({6, 8});
    double pps = 400;  // (an eighth: 100 px)
    std::vector<GridLine> ls = t.lines(0, 1.5, pps);
    std::vector<std::string> labels;
    for (auto& l : ls)
        if (l.strength >= timing::BEAT) labels.push_back(l.label);
    CHECK((labels == std::vector<std::string>{"1", "1.2", "1.3", "1.4", "1.5", "1.6", "2"}));
    for (auto& l : ls)
        if (l.strength == timing::BAR) {
            CHECK(std::fabs(std::fmod(l.seconds, 1.5)) < 1e-9);
        }
    // 3/2: beats are half notes (1 s), the quarters between them fine lines
    Timing half = inSig({3, 2});
    int beats = 0, fine = 0;
    for (auto& l : half.lines(0, 2.999, 200)) {
        if (l.strength == timing::BEAT) {
            beats++;
            CHECK(l.label == "1." + std::to_string((int)std::lround(l.seconds) + 1));
        }
        if (l.strength == timing::FINE && std::fabs(l.seconds - 0.5) < 1e-9) fine++;
    }
    CHECK(beats == 2 && fine == 1);
    // 7/8: a step must fit the bar (3.5 quarters): never 1 or 2 beats; zoomed out whole bars and their doubles
    Timing seven = inSig({7, 8});
    for (double p : {1.0, 3.0, 10.0, 30.0, 60.0, 120.0, 400.0, 2000.0}) {
        Frac step = seven.gridStep(p, 0);
        CHECK((Frac(7, 2) / step).den == 1 || (step / Frac(7, 2)).den == 1);
    }
    for (auto& l : seven.lines(0, 20, 3)) {  // (zoomed far out: lines on bar lines only)
        CHECK(std::fabs(std::remainder(l.seconds, 1.75)) < 1e-9);
    }
    // the fixed "1 bar" grid is a bar of the signature (3.5 quarters); snapping to it
    seven.grid = timing::WHOLE_BAR.value();
    CHECK(seven.gridStep(100, 0) == Frac(7, 2) && seven.gridName() == "1 bar");
    CHECK(seven.snap(2.5, 100) == 1.75 && seven.snap(2.7, 100) == 3.5 && seven.snap(3.4, 100, true) == 1.75);
    // a fixed 1-beat grid doesn't fit a 7/8 bar: counted from each bar line, so every bar line stays a line
    seven.grid = 1;
    CHECK(seven.snap(1.6, 100) == 1.5 && seven.snap(1.7, 100) == 1.75 && seven.snap(2.3, 100) == 2.25 &&
          seven.snap(3.3, 100) == 3.25 && seven.snap(3.4, 100) == 3.5);
    bool barLines = true;
    std::vector<double> at;
    for (auto& l : seven.lines(0, 7.1, 100)) at.push_back(l.seconds);
    for (int bar = 0; bar <= 4; bar++)
        barLines = barLines && std::any_of(at.begin(), at.end(), [&](double s) { return std::fabs(s - bar * 1.75) < 1e-9; });
    CHECK(barLines);
    // 5/16 at 60: an eighth fits (2.5 a bar), a quarter doesn't (1.25)
    Timing five = inSig({5, 16}, 60);
    five.grid = 0.5;
    CHECK(five.snap(1.3, 100) == 1.25 && five.snap(0.6, 100) == 0.5 && five.snap(1.24, 100, true) == 1.0);
    // a change of signature: the snap goes to the change's bar line when it is nearer
    Timing changing = inSig({7, 8});
    changing.changes = {TempoChange{3, 120, 3, 0, false, 4}};
    changing.grid = 2;  // (two beats from 3.5: lines at 2 and 3.5 in bar 2…)
    CHECK(changing.snap(3.45, 100) == 3.5);
}

void testMetronome() {
    struct Case {
        TimeSig sig;
        double unitS;
        std::vector<int> groups;
    } cases[] = {{{6, 8}, 0.25, {3}}, {{7, 8}, 0.25, {}}, {{3, 2}, 1.0, {}}, {{12, 8}, 0.25, {3, 6, 9}},
                 {{5, 16}, 0.125, {}}, {{9, 8}, 0.25, {3, 6}}};
    for (auto& c : cases) {
        Timing t = inSig(c.sig);
        double barS = c.sig.num * c.unitS;
        std::vector<BeatTime> beats = t.beatTimes(0, 2 * barS - 1e-6);
        CHECK((int)beats.size() == 2 * c.sig.num);
        for (size_t i = 0; i < beats.size(); i++) {
            int in = (int)i % c.sig.num;
            CHECK_NEAR(beats[i].seconds, i * c.unitS, 1e-12);
            CHECK(beats[i].barStart == (in == 0));
            CHECK(beats[i].groupStart == (std::find(c.groups.begin(), c.groups.end(), in) != c.groups.end()));
        }
    }
    // the clicks as heard: in 6/8 at 120 the bar's click at 0 s, the group's (the bar's at half) at 0.75 s, the
    // beat's at the other eighths
    Timing t = inSig({6, 8});
    metronome::MetronomeSettings settings;
    metronome::Clicks clicks = metronome::renderClicks(settings, SR);
    std::vector<float> out((size_t)SR * 2, 0.0f);
    metronome::addClicks(out.data(), 2, SR, 0, clicks, t, SR);
    auto at = [&](double s, size_t k) { return out[((size_t)std::llround(s * SR) + k) * 2]; };
    bool bar = true, group = true, beat = true;
    for (size_t k = 0; k < clicks.bar.size() && k < 2000; k++) {
        bar = bar && at(0, k) == clicks.bar[k];
        group = group && std::fabs(at(0.75, k) - (float)(clicks.bar[k] * metronome::GROUP_GAIN)) < 1e-7f;
    }
    for (size_t k = 0; k < clicks.beat.size() && k < 2000; k++) beat = beat && at(0.25, k) == clicks.beat[k];
    CHECK(bar && group && beat);
    // the count-in: a bar of 6/8 is six eighths, 0.25 s apart; in 3/2 three halves
    int64_t frames = 0;
    AudioPtr a = metronome::countIn(settings, t, 1, SR, 0.0, frames);
    CHECK(frames == (int64_t)(1.5 * SR) && a);
    AudioPtr b;
    CHECK(recording::countInAudio(settings, t, 2, SR, b, 0.0) == 3 * SR && b);
    Timing halves = inSig({3, 2});
    CHECK(recording::countInAudio(settings, halves, 1, SR, b, 0.0) == 3 * SR);
    metronome::countIn(settings, halves, 1, SR, 0.0, frames);
    CHECK(frames == 3 * SR);
}

void testMidiFiles() {
    util::makeDirs(kDir);
    Timing t = inSig({7, 8});
    t.changes = {TempoChange{3, 120, 6, 0, false, 8}, TempoChange{4, 100, 5, 0, false, 16},
                 TempoChange{6, 100, 3, 0, false, 2}, TempoChange{7, 100, 4, 0, false, 4}};
    std::string bytes = midiBytes(t, "sigs.mid");
    CHECK(bytes.find(std::string("\xFF\x58\x04\x07\x03", 5)) != std::string::npos);  // 7/8: 2^3
    CHECK(bytes.find(std::string("\xFF\x58\x04\x05\x04", 5)) != std::string::npos);  // 5/16: 2^4
    CHECK(bytes.find(std::string("\xFF\x58\x04\x03\x01", 5)) != std::string::npos);  // 3/2: 2^1
    std::vector<MidiFileSig> sigs;
    std::string error;
    CHECK(readMidiFileSignatures(kDir + "/sigs.mid", sigs, error));
    struct E {
        double beat;
        int num, den;
    } expect[] = {{0, 7, 8}, {7, 6, 8}, {10, 5, 16}, {12.5, 3, 2}, {18.5, 4, 4}};
    CHECK(sigs.size() == 5);
    for (size_t i = 0; i < sigs.size() && i < 5; i++) {
        CHECK(sigs[i].beat == expect[i].beat && sigs[i].num == expect[i].num && sigs[i].den == expect[i].den);
    }
    // a damaged one (2^9) reads as 4/4
    std::string bad = bytes;
    size_t k = bad.find(std::string("\xFF\x58\x04\x07\x03", 5));
    bad[k + 4] = 9;
    CHECK(util::writeFile(kDir + "/bad.mid", bad));
    CHECK(readMidiFileSignatures(kDir + "/bad.mid", sigs, error) && sigs[0].num == 4 && sigs[0].den == 4);
}

void testPluginsAndSync() {
    Timing t = inSig({6, 8});
    t.changes = {TempoChange{3, 120, 3, 0, false, 2}};
    TempoMapPtr m = t.map();
    plugin_audio::Transport tr = plugin_audio::Transport::at(*m, true, SR / 2, SR);
    CHECK(tr.numerator == 6 && tr.denominator == 8 && tr.beats == 1.0 && tr.tempo == 120);
    tr = plugin_audio::Transport::at(*m, true, 4 * SR, SR);  // (bar 3 starts at 3 s)
    CHECK(tr.numerator == 3 && tr.denominator == 2 && tr.beats == 8.0);
    // JACK's BBT counts the unit: 0.875 s into 6/8 is its 4th eighth, half way
    jack::Position pos{};
    midi_sync::fillBBT(*m, (uint32_t)(0.875 * SR), SR, pos);
    CHECK(pos.bar == 1 && pos.beat == 4 && pos.beats_per_bar == 6 && pos.beat_type == 8 && pos.beats_per_minute == 240);
    CHECK(pos.tick == (int32_t)(midi_sync::TICKS_PER_BEAT / 2));
    midi_sync::fillBBT(*m, (uint32_t)(5.5 * SR), SR, pos);  // (bar 3: 3/2 at 120 from 3 s: 2.5 s in, beat 3 half way)
    CHECK(pos.bar == 3 && pos.beat == 3 && pos.beat_type == 2 && pos.beats_per_minute == 60);
}

void testProjects() {
    util::makeDirs(kDir);
    // N/4 writes nothing new (old files stay the same); others write the denominator
    Timing plain = n4Song(1);
    Json d = project_io::describeTiming(plain);
    CHECK(!d.has("beat_unit") && d.dump().find("beat_unit") == std::string::npos);
    for (const Json& c : d["changes"].items()) {
        CHECK(c.size() == 3 || c.size() == 5);
    }
    Timing t = inSig({7, 8}, 133);
    t.changes = {TempoChange{2, 133, 6, 0, false, 8}, TempoChange{3, 90, 5, 1.5, true, 4}, TempoChange{4, 120, 3, 0, false, 2}};
    t.setSignature(5, {9, 16});
    Json j = project_io::describeTiming(t);
    CHECK(j["beat_unit"].toInt() == 8);
    Timing back = project_io::readTiming(j);
    CHECK(back.beatUnit == 8 && back.beatsPerBar == 7 && back.changes == t.changes);
    for (double s = 0; s < 20; s += 0.31) {
        CHECK(back.beatAt(s) == t.beatAt(s));
    }
    // a whole project, saved and loaded
    Project p;
    std::vector<float> before = renderSong(t, 2, 6.0, &p);
    std::string error;
    project_io::MasterSettings master, got;
    CHECK(project_io::saveProject(kDir + "/Sig.json", p, SR, master, error));
    Project q;
    audio::Engine e(q);
    e.offline = true;
    CHECK(project_io::loadProject(kDir + "/Sig.json", q, SR, got, error));
    CHECK(q.timing.beatUnit == 8 && q.timing.changes == t.changes && q.timing.sigAtBar(5) == (TimeSig{9, 16}));
    auto after = render::renderMix(q, 0, 6 * SR, SR);
    CHECK(after && *after == before);
}

void testFallbacks() {
    util::setDebug(true);  // (the messages are part of the fallback: shown)
    CHECK(timing::checkedUnit(0) == 4 && timing::checkedUnit(3) == 4 && timing::checkedUnit(64) == 4 &&
          timing::checkedUnit(-8) == 4 && timing::checkedUnit(16) == 16 && timing::checkedUnit(1) == 1);
    CHECK(timing::clampedBeats(0) == 1 && timing::clampedBeats(99) == config::MAX_BEATS_PER_BAR);
    for (const char* bad : {R"({"bpm": 100, "beats_per_bar": 5, "beat_unit": 3})", R"({"bpm": 100, "beats_per_bar": 5, "beat_unit": 0})",
                            R"({"bpm": 100, "beats_per_bar": 5, "beat_unit": 64})", R"({"bpm": 100, "beats_per_bar": 5, "beat_unit": "8"})"}) {
        Timing t = project_io::readTiming(Json::parse(bad));
        CHECK(t.beatUnit == 4 && t.beatsPerBar == 5);
    }
    Timing missing = project_io::readTiming(Json::parse(R"({"bpm": 100, "beats_per_bar": 5})"));
    CHECK(missing.beatUnit == 4);
    Timing c = project_io::readTiming(
        Json::parse(R"({"changes": [[2, 100, 7, 0, false, 12], [3, 100, 7, 0, false, 8], [4, 100, 99, 0, false, 8]]})"));
    CHECK(c.changes.size() == 3 && c.changes[0].beatUnit == 4 && c.changes[1].beatUnit == 8 &&
          c.changes[2].beatsPerBar == config::MAX_BEATS_PER_BAR);
    TimeSig sig;
    CHECK(TimeSig::parse("7/8", sig) && sig == (TimeSig{7, 8}));
    CHECK(TimeSig::parse(" 12 / 8 ", sig) && sig == (TimeSig{12, 8}));
    CHECK(TimeSig::parse("5", sig) && sig == (TimeSig{5, 4}));
    CHECK(!TimeSig::parse("7/6", sig) && sig == (TimeSig{7, 4}));
    CHECK(!TimeSig::parse("40/8", sig) && sig == (TimeSig{config::MAX_BEATS_PER_BAR, 8}));
    sig = {3, 4};
    CHECK(!TimeSig::parse("x", sig) && !TimeSig::parse("7/", sig) && !TimeSig::parse("7/8x", sig) && sig == (TimeSig{3, 4}));
    // a map handed a bad denominator directly (not through readTiming): 4
    TempoMap m(120, 3, {TempoChange{2, 120, 3, 0, false, 0}}, false, 7);
    CHECK(m.meters()[0].beatUnit == 4 && m.barBeat(2) == 6);
    Timing set;
    set.setSignature(3, {0, 5});
    CHECK(set.sigAtBar(3) == (TimeSig{1, 4}));
    util::setDebug(false);
}

void testLaneApi() {
    Timing t;
    t.bpm = 100;
    t.changes = {TempoChange{5, 140, 4}, TempoChange{9, 140, 4, 0, true}, TempoChange{13, 80, 4}};
    TempoMapPtr was = t.map();
    // a new signature keeps the tempo there as it was (inside the ramp: it ramps on, the same curve)
    t.setSignature(3, {7, 8});
    CHECK(t.sigAtBar(3) == (TimeSig{7, 8}) && t.sigAtBar(4) == (TimeSig{7, 8}) && t.sigAtBar(5) == (TimeSig{4, 4}));
    CHECK(t.changeAtBar(3)->bpm == 100 && !t.changeAtBar(3)->ramp);
    was = t.map();
    t.setSignature(11, {6, 8});
    const TempoChange* r = t.changeAtBar(11);
    CHECK(r && r->ramp && std::fabs(r->bpm - was->bpmAtBeat(t.barBeat(10))) < 1e-9);
    // changing one: only its signature
    t.setSignature(5, {3, 2});
    CHECK(t.changeAtBar(5)->bpm == 140 && t.sigAtBar(5) == (TimeSig{3, 2}));
    // moving one: from bar 3 to bar 4
    CHECK(t.moveSignature(3, 4) && !t.changeAtBar(3) && t.sigAtBar(4) == (TimeSig{7, 8}) && t.sigAtBar(3) == (TimeSig{4, 4}));
    // removing: a change only for the signature goes; one that changes the tempo too stays, with the signature before
    CHECK(t.removeSignature(4) && !t.changeAtBar(4));
    CHECK(t.removeSignature(5) && t.changeAtBar(5) && t.changeAtBar(5)->bpm == 140 && t.sigAtBar(5) == (TimeSig{4, 4}));
    CHECK(!t.removeSignature(6) && !t.moveSignature(1, 3));
    // bar 1: the song's own
    t.setSignature(1, {5, 16});
    CHECK(t.beatsPerBar == 5 && t.beatUnit == 16 && t.sigAtBar(1) == (TimeSig{5, 16}) && t.barBeat(1) == 1.25);
}

void testInsertTimeByBars() {
    // a 7/8 song with 3/2 from bar 5: one bar inserted at bar 3 moves the change to bar 6 and keeps both signatures
    Timing t = inSig({7, 8});
    t.changes = {TempoChange{5, 120, 3, 0, false, 2}};
    double at = t.barSeconds(2), bar = t.barSeconds(3) - at;
    CHECK(song_time::moveTempo(t, at, bar, true).empty());
    CHECK(t.changeAtBar(6) && t.sigAtBar(6) == (TimeSig{3, 2}) && t.sigAtBar(5) == (TimeSig{7, 8}));
    // …and deleted again
    CHECK(song_time::moveTempo(t, at, bar, false).empty());
    CHECK(t.changeAtBar(5) && t.sigAtBar(5) == (TimeSig{3, 2}) && t.sigAtBar(4) == (TimeSig{7, 8}));
    // deleting bar 1 of a 5/16 song whose bar 2 is 9/8: the song starts in 9/8
    Timing u = inSig({5, 16});
    u.setSignature(2, {9, 8});
    CHECK(song_time::moveTempo(u, 0, u.barSeconds(1), false).empty());
    CHECK(u.beatsPerBar == 9 && u.beatUnit == 8 && u.changes.empty());
    // half a 7/8 bar is not whole bars: the changes stay
    u.setSignature(3, {7, 8});
    CHECK(!song_time::moveTempo(u, 0, 0.3, true).empty());
}

void testWorstCase() {
    // a signature change at every bar through every denominator, 64 tracks playing, rendered, saved, loaded
    const int dens[] = {1, 2, 4, 8, 16, 32};
    Timing t;
    t.bpm = 132;
    t.setSignature(1, {3, 8});
    for (int bar = 2; bar <= 400; bar++)
        t.changes.push_back(TempoChange{bar, 90.0 + (bar * 37) % 90, 1 + (bar * 5) % 7, 0, bar % 5 == 0, dens[bar % 6]});
    std::sort(t.changes.begin(), t.changes.end());
    t.publish();
    TempoMapPtr m = t.map();
    CHECK(m->meters().size() == 400);
    bool ordered = true;
    for (int bar = 1; bar < 400; bar++) ordered = ordered && t.barSeconds(bar) > t.barSeconds(bar - 1);
    CHECK(ordered);
    std::vector<BeatTime> beats = t.beatTimes(0, t.barSeconds(399));
    int bars = 0;
    for (auto& b : beats) bars += b.barStart;
    CHECK(bars == 400);  // (bar 400's start included)
    auto t0 = std::chrono::steady_clock::now();
    Project p;
    std::vector<float> mix = renderSong(t, 64, 30.0, &p);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    bool finite = !mix.empty();
    float peak = 0;
    for (float v : mix) finite = finite && std::isfinite(v), peak = std::max(peak, std::fabs(v));
    CHECK(finite && peak > 0.01f && mix.size() >= (size_t)30 * SR * 2);
    std::string error;
    project_io::MasterSettings master, got;
    CHECK(project_io::saveProject(kDir + "/Worst.json", p, SR, master, error));
    Project q;
    audio::Engine e(q);
    e.offline = true;
    CHECK(project_io::loadProject(kDir + "/Worst.json", q, SR, got, error));
    CHECK(q.timing.changes == t.changes && q.timing.sig() == t.sig());
    auto again = render::renderMix(q, 0, 30 * SR, SR);
    CHECK(again && *again == mix);
    printf("worst case: 400 bars, a signature change each (every denominator), 64 tracks: 30 s rendered in %.2f s, "
           "saved and loaded the same\n",
           took);
}
#endif
}  // namespace

int main() {
    util::setDebug(false);
    util::makeDirs(kDir);
#ifdef REGRESSION_ONLY
    for (int i = 0; i < 3; i++) printf("0x%016llxULL\n", (unsigned long long)regressionHash(i));
    return 0;
#else
    for (int i = 0; i < 3; i++) {
        uint64_t h = regressionHash(i);
        if (h != MAIN_HASHES[i]) printf("N/4 song %d: hash 0x%016llx, main 0x%016llx\n", i, (unsigned long long)h,
                                        (unsigned long long)MAIN_HASHES[i]);
        CHECK(h == MAIN_HASHES[i]);  // (bit for bit as before denominators)
    }
    testBarsAndSeconds();
    testGridAndSnapping();
    testMetronome();
    testMidiFiles();
    testPluginsAndSync();
    testProjects();
    testFallbacks();
    testLaneApi();
    testInsertTimeByBars();
    testWorstCase();
    TEST_MAIN_END();
#endif
}
