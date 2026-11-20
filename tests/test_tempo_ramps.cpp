// Tempo ramps and the song's timebase (model/timing.h, model/timebase.h), at their worst: the closed forms
// against fine numeric integration; 10,000 tempo changes (ramps, steps, changes inside bars, time signatures)
// round-tripped, played from every bar (the metronome's clicks, the clock live MIDI gets, the transport plugins
// get); a render equal to playback through ramps; the timebase moving clips, notes, automation and markers (and
// Time leaving them); old files as Time; undo; inserting and deleting time through ramps; MIDI export of ramps;
// an Ableton set's tempo envelope.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/file_io.h"
#include "audio/live.h"
#include "audio/metronome.h"
#include "audio/midi_effects.h"
#include "audio/plugin_audio.h"
#include "audio/render.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "importers/ableton.h"
#include "model/history.h"
#include "model/project_io.h"
#include "model/song_time.h"
#include "model/timebase.h"

namespace {

constexpr int SR = 48000;

// ∫ 60 / bpm(b) db over [0, beats] by Simpson's rule (n steps), for bpm(b) = b0 + k·b
double simpson(double b0, double k, double beats, int n = 200000) {
    double h = beats / n, sum = 0;
    for (int i = 0; i <= n; i++) {
        double f = 60.0 / (b0 + k * i * h);
        sum += f * (i == 0 || i == n ? 1 : i % 2 ? 4 : 2);
    }
    return sum * h / 3;
}

void testClosedForms() {
    // bar 1 ramps from 100 to 160 BPM over its two bars (8 beats), then steady; a falling ramp from bar 5 to 9
    // (160 → 60), a ramp so gentle it is nearly steady from bar 11, and a step at bar 13
    std::vector<TempoChange> changes = {{3, 160, 4}, {5, 160, 4, 0, true}, {9, 60, 4}, {11, 90, 4, 0, true},
                                        {13, 90.0001, 4}, {14, 200, 4}};
    TempoMap m(100, 4, changes, true);
    CHECK(m.hasChanges() && m.segments().size() == 7);
    struct Span {
        double b0, bpm0, bpm1, beats;
    } spans[] = {{0, 100, 160, 8}, {16, 160, 60, 16}, {40, 90, 90.0001, 8}};
    double worst = 0;
    for (auto& s : spans) {
        double k = (s.bpm1 - s.bpm0) / s.beats;
        for (double x = 0; x <= s.beats; x += 0.37) {
            double want = m.secondsAt(s.b0) + simpson(s.bpm0, k, x);
            worst = std::max(worst, std::fabs(m.secondsAt(s.b0 + x) - want));
            CHECK_NEAR(m.bpmAtBeat(s.b0 + x), s.bpm0 + k * x, 1e-9);
            CHECK_NEAR(m.bpmAt(m.secondsAt(s.b0 + x)), s.bpm0 + k * x, 1e-7);
            CHECK_NEAR(m.beatAt(m.secondsAt(s.b0 + x)), s.b0 + x, 1e-9);
        }
    }
    printf("closed forms against Simpson's rule: worst %.3g s\n", worst);
    CHECK(worst < 1e-9);
    // the first ramp: 60/k · ln(160/100) seconds; steady from bar 3 at 160 (a beat 0.375 s)
    CHECK_NEAR(m.secondsAt(8), 60.0 / 7.5 * std::log(1.6), 1e-12);
    CHECK_NEAR(m.secondsAt(12) - m.secondsAt(8), 4 * 0.375, 1e-12);
    // a step: the tempo jumps at bar 14's start; the time signature stays 4/4 throughout
    CHECK_NEAR(m.bpmAtBeat(52 - 1e-6), 90.0001, 1e-6);
    CHECK_NEAR(m.bpmAtBeat(52), 200, 0);
    CHECK(m.barAt(m.secondsAt(52) + 1e-9) == 13 && m.beatsPerBarAt(m.secondsAt(52)) == 4);
    // the points as the lane draws them: bar 1's and each change, ramps where there is a next point
    CHECK(m.points().size() == 7 && m.points()[0].ramp && m.points()[0].change == -1 && !m.points()[6].ramp);
    // a change inside a bar (beat 2.5 of bar 2) and a time signature at bar 3: the meter counts from bar 3
    TempoMap in(120, 4, {{2, 60, 4, 2.5}, {3, 60, 3}, {4, 120, 3, 1, true}, {5, 90, 3}});
    CHECK_NEAR(in.secondsAt(6.5), 6.5 * 0.5, 1e-12);
    CHECK_NEAR(in.secondsAt(8), 3.25 + 1.5, 1e-12);
    CHECK(in.barAtBeat(8) == 2 && in.barAtBeat(10.9) == 2 && in.barAtBeat(11) == 3 && in.barBeat(4) == 14);
    CHECK_NEAR(in.bpmAtBeat(12.5), 112.5, 1e-12);  // (a ramp from beat 1 of bar 4: 120 → 90 over 2 beats)
    std::vector<BeatTime> beats = in.beatTimes(0, in.secondsAt(14) + 1e-9);
    CHECK(beats.size() == 15 && beats[8].barStart && !beats[9].barStart && beats[11].barStart && beats[14].barStart);
    // Timing's grid follows the ramp: a beat's line at every beat, closer together as the tempo rises
    Timing t;
    t.bpm = 60;
    t.ramp = true;
    t.setChange(TempoChange{3, 240, 4});
    t.grid = 1;
    std::vector<GridLine> lines = t.lines(0, t.barSeconds(2), 200);
    CHECK(lines.size() == 9);
    for (size_t i = 0; i < lines.size(); i++) CHECK_NEAR(lines[i].seconds, t.secondsAt((double)i), 1e-9);
    CHECK(lines[4].strength == timing::BAR && lines[4].label == "2");
    CHECK_NEAR(t.snap(t.secondsAt(5.4), 200), t.secondsAt(5), 1e-9);
    CHECK_NEAR(t.snap(t.secondsAt(5.6), 200), t.secondsAt(6), 1e-9);
    int bar, beat, part;
    t.position(t.secondsAt(6.5), bar, beat, part);
    CHECK(bar == 2 && beat == 3 && part == 3);
}

// a tempo map of n changes (steps and ramps, some inside bars, time signatures at bar starts)
Timing bigMap(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> bpm(40, 240);
    Timing t;
    t.bpm = 120;
    t.ramp = true;
    int bar = 2;
    for (int i = 0; i < n; i++) {
        TempoChange c;
        c.bar = bar;
        c.bpm = std::nearbyint(bpm(rng) * 100) / 100;
        c.ramp = rng() % 3 != 0;
        c.beatsPerBar = 2 + (int)(rng() % 6);
        c.beat = rng() % 4 == 0 ? (double)(rng() % 2) + 0.5 * (rng() % 2) : 0.0;  // (some inside a bar)
        t.changes.push_back(c);
        bar += 1 + (int)(rng() % 2);
    }
    std::sort(t.changes.begin(), t.changes.end());
    return t;
}

void testTenThousand() {
    auto began = std::chrono::steady_clock::now();
    Timing t = bigMap(10000, 7);
    TempoMapPtr m = t.map();
    CHECK(m->points().size() == 10001 && m->segments().size() > 10000);
    // beatAt(secondsAt(b)) == b, everywhere; seconds only ever grow
    double worst = 0, last = -1;
    bool rising = true;
    double total = m->points().back().beat + 16;
    for (double b = 0; b < total; b += 0.173) {
        double s = m->secondsAt(b);
        rising = rising && s > last;
        last = s;
        worst = std::max(worst, std::fabs(m->beatAt(s) - b) / std::max(1.0, b));
    }
    printf("10,000 changes: beatAt(secondsAt(b)) off by at most %.3g (relative), %.0f beats, %.0f s\n", worst, total, last);
    CHECK(worst < 1e-12 && rising);
    // the tempo inside every ramp goes straight from its point's to the next point's
    const std::vector<TempoPoint>& ps = m->points();
    int ramps = 0;
    for (size_t i = 0; i + 1 < ps.size(); i++) {
        if (!ps[i].ramp) continue;
        ramps++;
        double mid = (ps[i].beat + ps[i + 1].beat) / 2;
        CHECK_NEAR(m->bpmAtBeat(mid), (ps[i].bpm + ps[i + 1].bpm) / 2, 1e-9);
        CHECK_NEAR(m->bpmAtBeat(ps[i + 1].beat - 1e-7), ps[i + 1].bpm, 1e-3);
    }
    CHECK(ramps > 5000);
    // play from every bar: the metronome's first click is that bar's accent, right on its frame; the clock live
    // MIDI effects get and the transport plugins get are at its beat and its tempo
    metronome::MetronomeSettings settings;
    metronome::Clicks clicks = metronome::renderClicks(settings, SR);
    std::vector<float> out(256 * 2);
    std::vector<BeatTime> scratch;
    scratch.reserve(256);
    int lastBar = m->barAtBeat(ps.back().beat);
    int bad = 0, badClock = 0;
    for (int b = 0; b <= lastBar; b++) {
        double at = m->barSeconds(b);
        int64_t frame = (int64_t)std::llround(at * SR);  // (where playback starts from a snapped click)
        std::vector<BeatTime> next = m->beatTimes((double)(frame - 1) / SR, (double)frame / SR + 0.5);
        bool onBar = !next.empty() && next[0].barStart && std::fabs(next[0].seconds - at) < 1.0 / SR;
        std::fill(out.begin(), out.end(), 0.0f);
        metronome::addClicks(out.data(), 2, 256, frame, clicks, *m, SR, 1.0, frame, scratch);
        int first = -1;
        for (int i = 0; i < 256 && first < 0; i++)
            if (out[(size_t)i * 2] != 0) first = i;
        if ((!onBar || first < 0 || first > 1) && bad++ < 3) printf("bar %d: on its bar %d, first click at %d\n", b, onBar, first);
        live::Clock c = live::clockAt(*m, true, frame, SR);
        plugin_audio::Transport tr = plugin_audio::Transport::at(*m, true, frame, SR);
        double beat = m->beatAt((double)frame / SR);
        if (std::fabs(c.beat - beat) > 1e-9 || std::fabs(tr.beats - beat) > 1e-9 ||
            std::fabs(tr.tempo - m->bpmAt((double)frame / SR)) > 1e-9 || std::fabs(beat - m->barBeat(b)) > 1e-3 ||
            tr.numerator != m->beatsPerBarAt((double)frame / SR))
            badClock++;
    }
    printf("played from each of %d bars: %d off the bar, %d clocks off\n", lastBar + 1, bad, badClock);
    CHECK(bad == 0 && badClock == 0);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    printf("10,000 changes: %.2f s\n", took);
}

void testRenderEqualsPlayback() {
    // through ramps (and a step inside a bar): MIDI effects, the arpeggiator, Note Length, the metronome-free mix
    Project p;
    audio::Engine e(p);
    e.offline = true;
    p.timing.bpm = 90;
    p.timing.ramp = true;
    p.timing.setChange(TempoChange{3, 150, 4, 0, true});
    p.timing.setChange(TempoChange{4, 70, 4, 2});
    p.timing.publish();
    auto m = p.addTrack(Kind::Midi);
    m->needsInstrument = false;
    MidiEffect chord, arp, len;
    MidiEffect::make("Chord", {{"chord", 2}}, chord);
    MidiEffect::make("Arpeggiator", {{"rate", 5}}, arp);
    m->midiEffects = std::vector<MidiEffect>{chord, arp};
    auto mc = makeMidiClip("notes", {{0.0, 3.0, 60, 100}, {4.0, 6.0, 65, 80}}, 7.0, SR);
    mc->track = m;
    mc->start = SR / 2;
    mc->prepare(SR);
    p.addClip(mc);
    auto syn = p.addTrack(Kind::Midi);
    syn->needsInstrument = false;
    MidiEffect::make("Note Length", {{"beats", 0.5}}, len);
    syn->midiEffects = std::vector<MidiEffect>{len};
    auto sc = makeMidiClip("synth", {{0.0, 0.1, 72, 110}, {1.0, 1.1, 76, 90}, {3.0, 3.1, 79, 90}}, 4.0, SR);
    sc->track = syn;
    sc->start = SR;
    sc->prepare(SR);
    p.addClip(sc);
    CHECK(mc->played && mc->played->size() > 20);
    // the arpeggio's notes are on the song's 16ths through the ramp
    int onGrid = 0;
    for (auto& n : *mc->played) {
        double b = p.timing.beatAt(0.5 + n.start) * 8;  // (rate 5: 32nds)
        if (std::fabs(b - std::nearbyint(b)) < 1e-6) onGrid++;
    }
    CHECK(onGrid == (int)mc->played->size());
    const int block = config::RENDER_BLOCK;
    const int64_t start = SR * 2, end = SR * 7;
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
    if (differ) printf("render vs playback through ramps: %lld samples differ\n", (long long)differ);
    CHECK(differ == 0 && loudest > 0.01);
}

// a project at 120 BPM: an audio clip at 2 s, a MIDI clip at bar 2 (notes on beats), volume automation, markers
struct Song {
    Project p;
    TrackPtr audio, midi;
    ClipPtr a, n;
    AutomationPtr lane;
    Song() {
        audio = p.addTrack(Kind::Audio);
        auto source = std::make_shared<AudioBuffer>();
        source->samples.assign((size_t)SR * 2, 0.25f);
        a = makeAudioClip("a", source, SR, SR);
        a->track = audio;
        a->start = SR * 2;
        p.addClip(a);
        midi = p.addTrack(Kind::Midi);
        midi->needsInstrument = false;
        n = makeMidiClip("n", {{0.0, 0.5, 60, 100}, {1.0, 1.25, 64, 100}, {1.5, 2.0, 67, 100}}, 2.0, SR);
        n->track = midi;
        n->start = SR * 2;
        n->prepare(SR);
        p.addClip(n);
        lane = std::make_shared<Automation>();
        lane->slot = audio->controls;
        lane->param = automation::VOLUME;
        lane->setPoints({{1.0, 0.2}, {3.0, 0.8}});
        audio->automation = std::vector<AutomationPtr>{lane};
        p.setMarkers({{4.0, "Chorus"}});
        p.timing.loopStart = 2.0;
        p.timing.loopEnd = 4.0;
    }
};

void testTimebase() {
    // Beats (a new project's): halving the tempo puts everything twice as late, notes twice as long
    {
        Song s;
        CHECK(s.p.timing.timebaseBeats);
        timebase::Before before = timebase::remember(s.p);
        s.p.timing.bpm = 60;
        std::vector<ClipPtr> moved = timebase::follow(s.p, before, SR);
        CHECK(moved.size() == 2);
        CHECK(s.a->start == SR * 4 && s.a->offset == 0 && s.a->frames() == SR);  // (audio: its own speed)
        CHECK(s.n->start == SR * 4 && std::fabs(s.n->lengthS - 4.0) < 1e-9);
        CHECK(s.n->notes && std::fabs((*s.n->notes)[1].start - 2.0) < 1e-9 && std::fabs((*s.n->notes)[1].end - 2.5) < 1e-9);
        CHECK(s.n->played && s.n->data && s.n->data->frames() > SR * 4);  // (prepared again: its sound as long)
        CHECK((*s.lane->points)[0].t == 2.0 && (*s.lane->points)[1].t == 6.0);
        CHECK(s.p.markers[0].seconds == 8.0 && s.p.timing.loopStart == 4.0 && s.p.timing.loopEnd == 8.0);
        // a ramp from bar 2 (120 → 240 over 4 beats, then 240): the notes keep their beats exactly
        timebase::Before again = timebase::remember(s.p);
        s.p.timing.bpm = 120;
        s.p.timing.setChange(TempoChange{2, 120, 4, 0, true});
        s.p.timing.setChange(TempoChange{3, 240, 4});
        timebase::follow(s.p, again, SR);
        const Timing& t = s.p.timing;
        CHECK(s.n->start == SR * 2);
        for (auto& note : *s.n->notes) {
            double b = t.beatAt(2.0 + note.start);
            CHECK(std::fabs(b - std::nearbyint(b * 2) / 2) < 1e-9);  // (still on its half beat)
        }
        CHECK_NEAR(t.beatAt(2.0 + (*s.n->notes)[2].end) - t.beatAt(2.0 + (*s.n->notes)[2].start), 1.0, 1e-9);
        CHECK_NEAR(t.beatAt(s.p.markers[0].seconds), 8, 1e-9);
        // the warping module's hook hears which clips moved
        size_t heard = 0;
        timebase::retimed = [&](Project&, const std::vector<ClipPtr>& clips, int) { heard = clips.size(); };
        timebase::Before third = timebase::remember(s.p);
        s.p.timing.removeChange(3);
        timebase::follow(s.p, third, SR);
        CHECK(heard == 1);  // (the audio clip at bar 3 moved; the MIDI clip at bar 2 stays, its later notes move)
        timebase::retimed = nullptr;
    }
    // Time: nothing moves
    {
        Song s;
        s.p.timing.timebaseBeats = false;
        timebase::Before before = timebase::remember(s.p);
        CHECK(before.empty());
        s.p.timing.bpm = 60;
        CHECK(timebase::follow(s.p, before, SR).empty());
        CHECK(s.a->start == SR * 2 && s.n->start == SR * 2 && (*s.n->notes)[1].start == 1.0);
        CHECK((*s.lane->points)[1].t == 3.0 && s.p.markers[0].seconds == 4.0);
    }
}

void testExtremes() {
    // ramps from the slowest tempo to the fastest and back, over a 3840th of a beat and over 10,000 beats; points
    // past their bar's end, two at one place; nothing breaks the round trip
    std::vector<TempoChange> changes = {{2, config::MAX_BPM, 4, 0, true}, {2, config::MIN_BPM, 4, 1.0 / 3840, true},
                                        {3, config::MAX_BPM, 4, 0, true}, {2503, config::MIN_BPM, 4, 0, false},
                                        {2504, 300, 4, 9.5}, {2505, 100, 4}, {2505, 101, 4}};
    TempoMap m(config::MIN_BPM, 4, changes, true);
    double worst = 0, last = -1;
    bool rising = true;
    for (double b = 0; b < 10040; b += 0.0371) {
        double s = m.secondsAt(b);
        rising = rising && s > last && std::isfinite(s);
        last = s;
        worst = std::max(worst, std::fabs(m.beatAt(s) - b) / std::max(1.0, b));
    }
    for (double b = 4; b < 4.001; b += 1e-5) {
        double s = m.secondsAt(b);
        worst = std::max(worst, std::fabs(m.beatAt(s) - b) / b);
    }
    printf("extreme ramps: beatAt(secondsAt(b)) off by at most %.3g (relative)\n", worst);
    CHECK(worst < 1e-11 && rising);
    CHECK_NEAR(m.bpmAtBeat(10014.5), config::MIN_BPM, 0);  // (bar 2503 on: the slowest)
    CHECK_NEAR(m.bpmAtBeat(10016.5), 101, 0);  // (beat 9.5 of a 4-beat bar: at the next bar, where 101 wins)
    CHECK(m.barAtBeat(10020) == 2505);
}

void testFollowMany() {
    // 300 MIDI clips of 200 notes each (copies sharing their notes, some linked) through 2,000 ramps: every note
    // keeps its beat, on every core
    Project p;
    auto track = p.addTrack(Kind::Midi);
    track->needsInstrument = false;
    std::vector<Note> notes;
    for (int k = 0; k < 200; k++) notes.push_back(Note{k * 0.125, k * 0.125 + 0.1, 48.0 + k % 24, 90});
    ClipPtr first = makeMidiClip("m", notes, 25.0, SR);
    first->track = track;
    first->link = std::make_shared<ClipLink>();  // (copies linked to it, but every third one)
    std::vector<ClipPtr> clips;
    for (int k = 0; k < 300; k++) {
        ClipPtr c = k == 0 ? first : copyClip(first);  // (copies share the notes)
        if (k % 3 == 0) c->link = nullptr;
        c->start = (int64_t)SR * 13 * k;
        c->prepare(SR);
        p.addClip(c);
        clips.push_back(c);
    }
    auto beatsOf = [&](const ClipPtr& c) {
        std::vector<double> out;
        double origin = (double)(c->start - c->offset) / SR;
        for (auto& n : *c->notes) out.push_back(p.timing.beatAt(origin + n.start));
        return out;
    };
    std::vector<std::vector<double>> was;
    for (auto& c : clips) was.push_back(beatsOf(c));
    Timing big = bigMap(2000, 3);
    timebase::Before before = timebase::remember(p);
    p.timing.changes = big.changes;
    p.timing.ramp = true;
    auto began = std::chrono::steady_clock::now();
    std::vector<ClipPtr> moved = timebase::follow(p, before, SR);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    double worst = 0;
    for (size_t k = 0; k < clips.size(); k++) {
        if (clips[k]->link) continue;  // (linked clips share one clip's notes: checked below for the first)
        std::vector<double> now = beatsOf(clips[k]);
        for (size_t i = 0; i < now.size(); i++) worst = std::max(worst, std::fabs(now[i] - was[k][i]));
    }
    printf("300 clips, 60,000 notes through 2,000 ramps: %zu moved in %.2f s; notes off their beats by %.3g\n",
           moved.size(), took, worst);
    CHECK(moved.size() >= 299 && worst < 1e-6 && took < 10);
}

void testFiles() {
    // an old file (no timebase): Time; a new project's: Beats; ramps and changes inside bars come back
    Json old = Json::parse("{\"bpm\": 100, \"beats_per_bar\": 4, \"changes\": [[3, 90, 3]]}");
    Timing t = project_io::readTiming(old);
    CHECK(!t.timebaseBeats && !t.ramp && t.changes.size() == 1 && t.changes[0] == (TempoChange{3, 90, 3}));
    Timing now;
    now.ramp = true;
    now.setChange(TempoChange{3, 90, 3, 0, true});
    now.setChange(TempoChange{4, 140, 3, 1.5});
    now.setChange(TempoChange{6, 70, 5});
    Json j = project_io::describeTiming(now);
    CHECK(j.str("timebase") == "beats" && j["ramp"].toBool());
    CHECK(j["changes"][size_t(2)].size() == 3);  // (a plain step: as before)
    Timing back = project_io::readTiming(Json::parse(j.dump()));
    CHECK(back.timebaseBeats && back.ramp && back.changes == now.changes);
    now.timebaseBeats = false;
    CHECK(project_io::readTiming(Json::parse(project_io::describeTiming(now).dump())).timebaseBeats == false);
    // damaged entries are left out, not trusted
    Timing bad = project_io::readTiming(Json::parse(
        "{\"bpm\": 100, \"timebase\": \"beats\", \"changes\": [[3, 90, 3, -1, true], [1, 90, 3, 0, false], [2, 90, 3, 1, 5], "
        "[1, 80, 4, 2, true]]}"));
    CHECK(bad.changes.size() == 1 && bad.changes[0] == (TempoChange{1, 80, 4, 2, true}));
}

void testUndo() {
    Song s;
    history::History h(s.p, SR);
    int64_t start = s.a->start;
    auto notes = s.n->notes;
    timebase::Before before = timebase::remember(s.p);
    s.p.timing.bpm = 90;
    s.p.timing.ramp = true;
    s.p.timing.setChange(TempoChange{4, 180, 4});
    s.p.timing.setChange(TempoChange{6, 100, 4});
    timebase::follow(s.p, before, SR);
    CHECK(s.a->start != start && s.n->notes != notes);
    CHECK(h.check(SR) && h.labels().back() == "Tempo");  // (one step: the tempo and what followed it)
    CHECK(h.undo(SR));
    CHECK(s.a->start == start && s.n->notes == notes && s.p.timing.changes.empty() && s.p.markers[0].seconds == 4.0);
    CHECK(h.redo(SR));
    CHECK(s.a->start != start && s.p.timing.changes.size() == 2);
}

void testSongTime() {
    // a ramp from bar 2 to bar 6 (100 → 180): 4 bars inserted at bar 4 hold the tempo there (140), then the ramp
    // goes on from 140 to 180, as long as before
    Project p;
    p.timing.bpm = 100;
    p.timing.setChange(TempoChange{2, 100, 4, 0, true});
    p.timing.setChange(TempoChange{6, 180, 4});
    TempoMapPtr was = p.timing.map();
    double ramp = was->secondsAt(20) - was->secondsAt(12);  // (bars 4 and 5: the ramp's second half)
    std::string note, error;
    int64_t at = (int64_t)std::llround(was->barSeconds(3) * SR);
    CHECK(song_time::insertTime(p, at, (int64_t)std::llround((was->barSeconds(5) - was->barSeconds(3)) * SR), SR, note, error));
    TempoMapPtr m = p.timing.map();
    CHECK_NEAR(m->bpmAtBeat(12), 140, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(16), 140, 1e-9);  // (the inserted bars: steady)
    CHECK_NEAR(m->bpmAtBeat(20), 140, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(24), 160, 1e-9);  // (the ramp, on from 140)
    CHECK_NEAR(m->bpmAtBeat(28), 180, 1e-9);
    CHECK_NEAR(m->secondsAt(28) - m->secondsAt(20), ramp, 1e-9);
    CHECK_NEAR(m->secondsAt(12), was->secondsAt(12), 1e-9);  // (before the cut: as it was)
    // deleting bars 4 and 5 again (the steady ones): back as it was
    CHECK(song_time::deleteTime(p, (int64_t)std::llround(m->barSeconds(3) * SR), (int64_t)std::llround(m->barSeconds(5) * SR),
                                SR, note, error));
    m = p.timing.map();
    for (double b = 0; b < 30; b += 0.5) CHECK_NEAR(m->secondsAt(b), was->secondsAt(b), 1e-6);
    // deleting the middle of the ramp (bars 3 and 4): the ramp goes on from where the music after the cut was (160)
    CHECK(song_time::deleteTime(p, (int64_t)std::llround(m->barSeconds(2) * SR), (int64_t)std::llround(m->barSeconds(4) * SR),
                                SR, note, error));
    m = p.timing.map();
    CHECK_NEAR(m->bpmAtBeat(4), 100, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(8), 160, 1e-9);   // (the cut: the tempo after it)
    CHECK_NEAR(m->bpmAtBeat(10), 170, 1e-9);  // (the ramp on to 180)
    CHECK_NEAR(m->bpmAtBeat(12), 180, 1e-9);
    // bar 1's own ramp and an insert at bar 1: the new bars at its tempo, then the ramp
    Project q;
    q.timing.bpm = 80;
    q.timing.ramp = true;
    q.timing.setChange(TempoChange{3, 160, 4});
    CHECK(song_time::insertTime(q, 0, (int64_t)std::llround(q.timing.barSeconds(2) * SR), SR, note, error));  // (2 bars)
    m = q.timing.map();
    CHECK(!q.timing.ramp && q.timing.changes.size() == 2);
    CHECK_NEAR(m->bpmAtBeat(4), 80, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(12), 120, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(16), 160, 1e-9);
}

void testMidiExport() {
    // notes on 16ths through a ramp: the file's stepped tempo events put each one where the song has it
    Timing t;
    t.bpm = 80;
    t.ramp = true;
    t.setChange(TempoChange{3, 160, 3, 0, true});
    t.setChange(TempoChange{4, 100, 3, 1.5});
    file_io::MidiPart part{"Ramp", {}};
    for (int k = 0; k < 64; k++) {
        double b = k * 0.25;
        part.notes.push_back(Note{t.secondsAt(b), t.secondsAt(b + 0.25), 60.0 + k % 12, 100});
    }
    std::string path = "build/test-tempo-ramps.mid", error;
    CHECK(file_io::saveMidi(path, {part}, t, 0, error));
    std::vector<MidiFileNote> secs, beats;
    double len = 0;
    CHECK(readMidiFile(path, secs, len, error) && readMidiFileBeats(path, beats, len, error));
    CHECK(secs.size() == 64 && beats.size() == 64);
    double worst = 0;
    for (size_t i = 0; i < secs.size() && i < beats.size(); i++) {
        worst = std::max(worst, std::fabs(secs[i].start - t.secondsAt(beats[i].start)));
        CHECK_NEAR(beats[i].start, i * 0.25, 1e-9);
    }
    printf("MIDI export of ramps: notes off by at most %.3g s\n", worst);
    CHECK(worst < 1e-4);  // (a tempo event's microseconds and a tick's rounding)
    std::remove(path.c_str());
}

void testAbleton() {
    // Live's tempo envelope: its default event far before the song, a ramp 120 → 150 from beat 16 to 32, a jump
    // to 100 at beat 32, held; a step at beat 48 to 90
    std::vector<std::pair<double, double>> events = {{-63072000, 120}, {16, 120}, {32, 150}, {32, 100}, {48, 100}, {48, 90}};
    Timing t;
    int made = ableton::tempoFromEnvelope(t, events, 4);
    TempoMapPtr m = t.map();
    CHECK(made == 4 && t.bpm == 120 && !t.ramp);
    CHECK_NEAR(m->bpmAtBeat(8), 120, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(24), 135, 1e-3);  // (the ramp, halfway)
    CHECK_NEAR(m->bpmAtBeat(31.99), 150, 0.05);
    CHECK_NEAR(m->bpmAtBeat(32), 100, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(40), 100, 1e-9);
    CHECK_NEAR(m->bpmAtBeat(50), 90, 1e-9);
    CHECK(t.changeAtBar(5) && t.changeAtBar(5)->ramp && t.changeAtBar(9) && !t.changeAtBar(9)->ramp);
    // the time of the ramp: within a thousandth of a beat of Live's linear ramp
    double want = 16 * 0.5 + 60.0 / (30.0 / 16) * std::log(150.0 / 120);
    CHECK_NEAR(m->secondsAt(32), want, 1e-3);
    // none: nothing made
    Timing none;
    CHECK(ableton::tempoFromEnvelope(none, {}, 4) == 0 && none.changes.empty());
}

}  // namespace

int main() {
    testClosedForms();
    testTenThousand();
    testRenderEqualsPlayback();
    testTimebase();
    testExtremes();
    testFollowMany();
    testFiles();
    testUndo();
    testSongTime();
    testMidiExport();
    testAbleton();
    TEST_MAIN_END();
}
