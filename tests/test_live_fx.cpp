// MIDI effects on notes played live (audio/live_fx.h, live.h's timed path), in the worst case:
//   - testAgainstClip: 16 chords a beat through arpeggiator → note length → groove (and the song swinging),
//     played live buffer by buffer with the transport running, against the same keys as a clip's notes
//     prepared offline: the same note-ons and note-offs, exact to one frame; nothing stuck after the keys
//     are let go
//   - testStopAndChanges: the transport stopping mid-arpeggio (it runs on, free, at the song's tempo), the
//     chain changed while notes sound, keys struck again while held: every note-on gets its note-off
//   - testBuiltIn: the built-in synth: voices start at their frame, play at their velocity, all fade out
//   - testNoAllocation: the audio thread's side never allocates, and keeps up with 10-note chords at 16 a beat
//     through chord → 4-octave arpeggio → long notes → groove, in 64-frame buffers
//   - testRecording: recording the played notes (the keys) or the effects' notes (what the chain made)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <new>

#include "audio/engine.h"
#include "audio/live.h"
#include "audio/live_fx.h"
#include "audio/midi_effects.h"
#include "check.h"
#include "core/util.h"
#include "ui/recording.h"

// --- allocations counted while gCount is on (this program's own operator new) ---
static std::atomic<bool> gCount{false};
static std::atomic<long> gAllocs{0};
void* operator new(size_t n) {
    if (gCount.load(std::memory_order_relaxed)) gAllocs++;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
const int SR = 48000;
const int BEAT = SR / 2;      // frames a beat at 120 BPM
const int CHORD = BEAT / 16;  // 16 chords a beat

MidiEffect fx(const std::string& kind, std::map<std::string, double> values) {
    MidiEffect e;
    MidiEffect::make(kind, values, e);
    return e;
}

struct Msg {
    int64_t frame;
    bool on;
    int pitch, velocity;
};

std::vector<int> chordAt(int k, int notes = 3) {
    std::vector<int> out;
    int root = 48 + (k * 7) % 19;
    const int shape[] = {0, 4, 7, 11, 14, 17, 21, 24, 26, 28};
    for (int i = 0; i < notes; i++) out.push_back(root + shape[i]);
    return out;
}

// the keys of `chords` chords, played live into `notes` buffer by buffer (the transport from frame 0 while
// `playing(frame)`), plus `tail` frames: every MIDI message out, at its timeline frame
std::vector<Msg> playLive(live::LiveNotes& notes, const Track& t, int chords, int buffer, int64_t tail,
                          const std::function<bool(int64_t)>& playing, int chordNotes = 3,
                          const std::function<void(int64_t)>& each = nullptr) {
    std::vector<Msg> out;
    plugin_audio::Messages msgs;
    msgs.reserve(8192);
    int64_t end = (int64_t)chords * CHORD + tail;
    int next = 0;  // (the next chord: played at the start of the buffer it falls in)
    for (int64_t f = 0; f < end; f += buffer) {
        if (each) each(f);
        for (; next <= chords && (int64_t)next * CHORD < f + buffer; next++) {
            int k = next;
            if (k > 0 && k - 1 < chords)
                for (int p : chordAt(k - 1, chordNotes)) notes.noteOff(p);
            if (k < chords)
                for (int p : chordAt(k, chordNotes)) {
                    notes.useEffects(t, false, SR, p);
                    notes.keyOn(p, 60 + p % 50, SR);
                }
        }
        msgs.clear();
        live::Clock clock;
        if (playing(f)) {
            clock.playing = true;
            clock.start = f;
            clock.beat = (double)f / BEAT;
            clock.beatsPerFrame = 1.0 / BEAT;
        } else {
            clock.beatsPerFrame = 1.0 / BEAT;
        }
        notes.take(buffer, SR, msgs, &clock);
        for (auto& m : msgs)
            if (m.type == plugin_audio::MidiMsg::NoteOn || m.type == plugin_audio::MidiMsg::NoteOff)
                out.push_back({f + m.frame, m.type == plugin_audio::MidiMsg::NoteOn, m.pitch, m.velocity});
    }
    return out;
}

// every note-on has its note-off (pitch by pitch, in order)
bool balanced(const std::vector<Msg>& msgs) {
    std::map<int, int> held;
    for (auto& m : msgs) {
        held[m.pitch] += m.on ? 1 : -1;
        if (held[m.pitch] < 0) return false;
    }
    for (auto& kv : held)
        if (kv.second) return false;
    return true;
}

void testAgainstClip() {
    Timing timing;
    timing.swing = 20;  // (the song's swing: applied first, live as in clips)
    midi_effects::install(&timing);
    auto t = std::make_shared<Track>("Arp", Kind::Midi);
    t->midiEffects = std::vector<MidiEffect>{fx("Arpeggiator", {{"rate", 5}, {"gate", 50}, {"octaves", 2}}),
                                             fx("Note Length", {{"beats", 0.1}}),
                                             fx("Groove", {{"swing", 60}, {"grid", 1}, {"accent", 20}})};
    CHECK(live_fx::timed(*t));
    const int chords = 256;
    // offline: the keys as a clip's notes, at timeline 0
    std::vector<Note> keys;
    for (int k = 0; k < chords; k++)
        for (int p : chordAt(k)) keys.push_back(Note{(double)k * CHORD / SR, (double)(k + 1) * CHORD / SR, (double)p, (double)(60 + p % 50)});
    std::sort(keys.begin(), keys.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
    ClipPtr c = makeMidiClip("keys", keys, (double)chords * CHORD / SR, SR);
    c->track = t;
    c->prepare(SR);
    std::vector<Msg> want;
    for (auto& e : *c->events) want.push_back({e.on, true, e.pitch, e.velocity}), want.push_back({e.off, false, e.pitch, 0});
    // live, in 500-frame buffers (the chords start on buffer starts: a key's frame is its buffer's)
    live::LiveNotes notes;
    std::vector<Msg> got = playLive(notes, *t, chords, 500, 2 * BEAT, [](int64_t) { return true; });
    std::printf("offline %zu messages, live %zu\n", want.size(), got.size());
    CHECK(want.size() > 200 && got.size() == want.size());
    auto key = [](const Msg& m) { return std::make_tuple(m.on, m.pitch, m.frame); };
    auto sorted = [&](std::vector<Msg> v) {
        std::sort(v.begin(), v.end(), [&](const Msg& a, const Msg& b) { return key(a) < key(b); });
        return v;
    };
    std::vector<Msg> a = sorted(want), b = sorted(got);
    int64_t worst = 0;
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); i++) {
        same = a[i].on == b[i].on && a[i].pitch == b[i].pitch && (!a[i].on || a[i].velocity == b[i].velocity);
        worst = std::max<int64_t>(worst, std::llabs(a[i].frame - b[i].frame));
    }
    std::printf("live against the clip: the worst difference %lld frame(s)\n", (long long)worst);
    CHECK(same && worst <= 1);
    CHECK(balanced(got) && !notes.holding() && !notes.active());
    midi_effects::install(nullptr);
}

void testStopAndChanges() {
    midi_effects::install(nullptr);
    auto t = std::make_shared<Track>("Arp", Kind::Midi);
    t->midiEffects = std::vector<MidiEffect>{fx("Chord", {{"chord", 4}}), fx("Arpeggiator", {{"rate", 3}, {"octaves", 3}, {"style", 2}}),
                                             fx("Note Length", {{"mode", 1}, {"share", 250}}), fx("Groove", {{"timing", 20}, {"velocity", 10}})};
    live::LiveNotes notes;
    int changes = 0;
    // stopped from the middle on; the chain changed twice while it plays; a key struck again while held
    std::vector<Msg> got = playLive(notes, *t, 128, 256, 3 * BEAT, [](int64_t f) { return f < 4 * BEAT; }, 4,
                                    [&](int64_t f) {
                                        auto at = [f](int64_t x) { return f <= x && x < f + 256; };  // (in this buffer)
                                        if (at(2 * BEAT) || at(6 * BEAT)) {
                                            std::vector<MidiEffect> e = t->midiEffects.copy();
                                            e[1] = e[1].with("rate", changes++ ? 1 : 5);
                                            t->midiEffects = e;
                                        }
                                        if (at(3 * BEAT)) {
                                            notes.useEffects(*t, false, SR, 100);
                                            notes.keyOn(100, 90, SR);
                                            notes.keyOn(100, 90, SR);
                                        }
                                        if (at(3 * BEAT + 4096)) notes.noteOff(100), notes.noteOff(100);
                                    });
    int whileStopped = 0;
    for (auto& m : got) whileStopped += m.on && m.frame >= 4 * BEAT;
    int late = 0;
    for (auto& m : got) late += m.on && m.frame >= 6 * BEAT;
    std::printf("notes while stopped: %d (after the second change %d), all %zu\n", whileStopped, late, got.size());
    CHECK(whileStopped >= 20);  // (free-running: 16 at 1/32, then 4 at 1/8)
    CHECK(balanced(got) && !notes.holding() && !notes.active());
    // a key into a VST instrument chain, then clear() (the track muted): nothing left
    notes.useEffects(*t, false, SR, 60);
    notes.keyOn(60, 100, SR);
    plugin_audio::Messages msgs;
    msgs.reserve(1024);
    notes.take(512, SR, msgs);
    CHECK(notes.holding());
    notes.clear();
    CHECK(!notes.holding() && !notes.active());
    notes.noteOff(60);  // (forgotten: nothing happens)
    msgs.clear();
    notes.take(512, SR, msgs);
    CHECK(!notes.holding());
}

void testBuiltIn() {
    auto t = std::make_shared<Track>("Arp", Kind::Midi);
    t->midiEffects = std::vector<MidiEffect>{fx("Arpeggiator", {{"rate", 3}, {"gate", 50}})};  // 16ths: 6000 frames
    // a key struck at 1000 frames: its first step on the next 16th, 6000, in the middle of a 256-frame buffer
    auto run = [&](int velocity, std::vector<float>& left) {
        live::LiveNotes notes;
        plugin_audio::Messages msgs;
        left.assign(SR * 3, 0.0f);
        std::vector<float> right(left.size());
        const int B = 256;
        live::Clock clock;
        clock.beatsPerFrame = 1.0 / BEAT;
        for (int f = 0; f + B <= (int)left.size(); f += B) {
            if (f <= 1000 && 1000 < f + B) {
                notes.useEffects(*t, true, SR, 64);
                notes.keyOn(64, velocity, SR);
            }
            if (f <= SR && SR < f + B) notes.noteOff(64);
            msgs.clear();
            clock.playing = true, clock.start = f, clock.beat = (double)f / BEAT;
            notes.take(B, SR, msgs, &clock);
            float* sig[2] = {left.data() + f, right.data() + f};
            notes.addVoices(sig, 2, B, SR);
        }
        CHECK(msgs.empty());  // (the built-in synth: voices, no MIDI)
        return notes.active();
    };
    std::vector<float> loud, soft;
    CHECK(!run(127, loud));  // (all faded out after the key was let go)
    run(32, soft);
    int first = -1;
    for (int i = 0; i < (int)loud.size() && first < 0; i++)
        if (loud[(size_t)i] != 0) first = i;
    std::printf("the first step sounds from frame %d\n", first);
    CHECK(first >= 6000 && first <= 6001);  // (its attack starts from 0 at its frame)
    float peakLoud = 0, peakSoft = 0, after = 0;
    for (size_t i = 0; i < loud.size(); i++) {
        peakLoud = std::max(peakLoud, std::fabs(loud[i])), peakSoft = std::max(peakSoft, std::fabs(soft[i]));
        if (i > (size_t)(SR + SR / 2)) after = std::max(after, std::fabs(loud[i]));
    }
    CHECK(peakLoud > 0 && std::fabs(peakSoft / peakLoud - 32.0 / 127) < 1e-3);  // (as loud as its velocity)
    CHECK(after == 0);
}

void testNoAllocation() {
    auto t = std::make_shared<Track>("Busy", Kind::Midi);
    t->midiEffects = std::vector<MidiEffect>{fx("Chord", {{"chord", 6}}), fx("Arpeggiator", {{"rate", 5}, {"octaves", 4}, {"style", 2}}),
                                             fx("Velocity", {{"random", 10}}), fx("Note Length", {{"beats", 2}}),
                                             fx("Groove", {{"swing", 70}, {"timing", 10}, {"velocity", 5}})};
    live::LiveNotes notes;
    const int chords = 16 * 64;  // 64 beats
    plugin_audio::Messages msgs;
    msgs.reserve(16384);
    // warmed up: the chain compiled once (GUI thread)
    notes.useEffects(*t, false, SR, 60);
    double busy = 0;
    long allocs = 0;
    int64_t end = (int64_t)chords * CHORD + 3 * BEAT;
    size_t made = 0;
    int next = 0;
    for (int64_t f = 0; f < end; f += 64) {
        for (; next <= chords && (int64_t)next * CHORD < f + 64; next++) {
            int k = next;
            if (k > 0 && k - 1 < chords)
                for (int p : chordAt(k - 1, 10)) notes.noteOff(p);
            if (k < chords)
                for (int p : chordAt(k, 10)) notes.keyOn(p, 100, SR);
        }
        msgs.clear();
        live::Clock clock;
        clock.playing = true, clock.start = f, clock.beat = (double)f / BEAT, clock.beatsPerFrame = 1.0 / BEAT;
        auto t0 = std::chrono::steady_clock::now();
        gAllocs = 0;
        gCount = true;
        notes.take(64, SR, msgs, &clock);
        gCount = false;
        allocs += gAllocs;
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        made += msgs.size();
    }
    double audio = (double)end / SR;
    std::printf("10-note chords, 16 a beat, 64 beats: %zu messages; the audio thread busy %.1f %% of the time; "
                "%ld allocations\n", made, 100 * busy / audio, allocs);
    CHECK(allocs == 0);
    CHECK(busy < audio * 0.25);
    CHECK(made > 900);
    CHECK(!notes.holding());
}

void testRecording() {
    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr t = project.addTrack(Kind::Midi);
    t->name = "Arp";
    t->needsInstrument = false;
    t->midiEffects = std::vector<MidiEffect>{fx("Arpeggiator", {{"rate", 3}, {"gate", 50}})};  // 16ths
    RecordingHost host;
    auto play = [&](Recording& rec, bool processed) {
        rec.beginMidiTake(t, 0);
        rec.recordEffectsNotes(processed);
        auto notes = engine.liveNotes(t.get());
        plugin_audio::Messages msgs;
        int beat = sr / 2;
        for (int64_t f = 0; f < 2 * beat; f += 256) {
            if (f == 0) {
                for (int p : {60, 64, 67}) {
                    notes->useEffects(*t, false, sr, p);
                    notes->keyOn(p, 100, sr);
                    rec.midiNoteAt(p, 100, 0);  // (the keys, as the MIDI input hands them)
                }
            }
            if (f == beat - beat % 256)
                for (int p : {60, 64, 67}) notes->noteOff(p), rec.midiNoteAt(p, 0, (double)f);
            msgs.clear();
            live::Clock clock = live::clockAt(*project.timing.map(), true, f, sr);
            notes->take(256, sr, msgs, &clock);
            rec.poll();
        }
        return rec.finishMidiAtFrame(2.0 * beat);
    };
    Recording raw(host, engine), effects(host, engine);
    ClipPtr keys = play(raw, false);
    CHECK(keys && keys->notes && keys->notes->size() == 3);
    ClipPtr arp = play(effects, true);
    CHECK(arp && arp->notes && arp->notes->size() == 4);  // (a beat of 16ths)
    if (arp && arp->notes && arp->notes->size() == 4) {
        const auto& n = *arp->notes;
        CHECK(n[0].pitch == 60 && n[1].pitch == 64 && n[2].pitch == 67 && n[3].pitch == 60);
        CHECK(std::fabs(n[1].start - 0.125) < 1e-9 && std::fabs(n[1].end - n[1].start - 0.0625) < 1e-9);
    }
}
}  // namespace

int main() {
    testAgainstClip();
    testStopAndChanges();
    testBuiltIn();
    testNoAllocation();
    testRecording();
    TEST_MAIN_END();
}
