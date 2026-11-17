// Tests of the bend wheel and the mod wheel: pitch bend and control changes reaching a VST2 instrument
// (as MIDI, at their frames) and a VST3 plugin (as changes of the parameters its IMidiMapping maps them
// to, at their frames), the built-in synth's live voices bent (the frequency, no click, back again,
// exactly as before when never bent), a track's bend range (saved only when not 2, read back, clamped),
// the MIDI keyboard's path (midi_input: the controller table, the selected track bent, the track bent
// before set straight, the mod wheel on to instrument plugins), and the MIDI Control modulator moving a
// built-in effect's knob through a modulation cable.
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/live.h"
#include "audio/midi_cc.h"
#include "audio/modulation.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/model.h"
#include "model/project_io.h"
#include "plugins/plugin.h"
#include "ui/midi_input.h"
#include "ui/track_dialogs.h"

namespace {

const int SR = 48000;
std::string testDir;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(int n) : l((size_t)n), r((size_t)n) {}
    float* p[2];
    float* const* out() {
        p[0] = l.data(), p[1] = r.data();
        return p;
    }
};

plug::NoteMessage noteOn(int frame, int pitch) { return {frame, true, pitch, 100, 0}; }
plug::NoteMessage bendMsg(int frame, int value) {
    plug::NoteMessage m{frame, false, 0, 0, 0};
    m.kind = plug::NoteMessage::PitchBend;
    m.value = value;
    return m;
}
plug::NoteMessage controlMsg(int frame, int number, int value) {
    plug::NoteMessage m{frame, false, number, 0, 0};
    m.kind = plug::NoteMessage::Control;
    m.value = value;
    return m;
}

// the first frame where a and b differ (-1: none)
int firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        if (a[i] != b[i]) return (int)i;
    return -1;
}

// rising zero crossings in [from, to)
int crossings(const std::vector<float>& x, size_t from, size_t to) {
    int n = 0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); i++)
        if (x[i - 1] < 0 && x[i] >= 0) n++;
    return n;
}

float maxJump(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); i++) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// --- a VST2 instrument: bend (0xE0) and the mod wheel (0xB0 1) at their frames ---

void testVst2() {
    std::string path = testDir + "/TinyTestSynth.so", error;
    auto a = plug::Plugin::load(path, "", error), b = plug::Plugin::load(path, "", error);
    CHECK(a && b);
    if (!a || !b) return;
    a->prepare(SR, 512), b->prepare(SR, 512);
    Stereo outA(512), outB(512);
    // a note in both, then the bend wheel all the way up in one, at frame 100 of the third buffer
    for (int k = 0; k < 2; k++) {
        std::vector<plug::NoteMessage> notes;
        if (k == 0) notes.push_back(noteOn(0, 69));
        a->render(notes, outA.out(), 512);
        b->render(notes, outB.out(), 512);
        CHECK(outA.l == outB.l);
    }
    a->render({}, outA.out(), 512);
    b->render({bendMsg(100, 8191)}, outB.out(), 512);
    // (the sample at frame 100 was already on its way: the new pitch shows from the next one)
    CHECK(firstDifference(outA.l, outB.l) == 101);
    // its frequency now: 2 semitones up (the test synth's range)
    std::vector<float> ra, rb;
    for (int k = 0; k < 40; k++) {
        a->render({}, outA.out(), 512);
        b->render({}, outB.out(), 512);
        ra.insert(ra.end(), outA.l.begin(), outA.l.end());
        rb.insert(rb.end(), outB.l.begin(), outB.l.end());
    }
    double ratio = (double)crossings(rb, 0, rb.size()) / std::max(1, crossings(ra, 0, ra.size()));
    CHECK_NEAR(ratio, std::pow(2.0, 2.0 / 12), 0.01);
    // all the way down, and back to the centre: the same pitch as the other again
    b->render({bendMsg(0, -8192), bendMsg(256, 0)}, outB.out(), 512);
    a->render({}, outA.out(), 512);
    ra.clear(), rb.clear();
    for (int k = 0; k < 40; k++) {
        a->render({}, outA.out(), 512);
        b->render({}, outB.out(), 512);
        ra.insert(ra.end(), outA.l.begin(), outA.l.end());
        rb.insert(rb.end(), outB.l.begin(), outB.l.end());
    }
    CHECK(std::abs(crossings(ra, 0, ra.size()) - crossings(rb, 0, rb.size())) <= 1);

    // the mod wheel through plugin_audio::playInstrument (MidiMsg::Control → its 0xB0 1): the test synth
    // turns the sound down to half from the frame it arrives at
    auto c = plug::Plugin::load(path, "", error), d = plug::Plugin::load(path, "", error);
    CHECK(c && d);
    if (!c || !d) return;
    c->prepare(SR, 512), d->prepare(SR, 512);
    auto slotOf = [](std::unique_ptr<plug::Plugin> p) {
        auto s = std::make_shared<PluginSlot>();
        s->kind = "instrument";
        s->instance = std::shared_ptr<plug::Plugin>(p.release());
        return s;
    };
    SlotPtr sc = slotOf(std::move(c)), sd = slotOf(std::move(d));
    plugin_audio::NoteScratch scratch;
    plugin_audio::Transport t;
    using plugin_audio::MidiMsg;
    MidiMsg on;
    on.type = MidiMsg::NoteOn, on.pitch = 60, on.velocity = 100;
    Stereo oc(512), od(512);
    std::fill(oc.l.begin(), oc.l.end(), 0.0f), std::fill(od.l.begin(), od.l.end(), 0.0f);
    plugin_audio::playInstrument(*sc, &on, 1, oc.out(), 2, 512, SR, scratch, t);
    plugin_audio::playInstrument(*sd, &on, 1, od.out(), 2, 512, SR, scratch, t);
    CHECK(oc.l == od.l);
    MidiMsg wheel;
    wheel.type = MidiMsg::Control, wheel.frame = 200, wheel.pitch = 1, wheel.value = 127;
    std::fill(oc.l.begin(), oc.l.end(), 0.0f), std::fill(od.l.begin(), od.l.end(), 0.0f);
    std::fill(oc.r.begin(), oc.r.end(), 0.0f), std::fill(od.r.begin(), od.r.end(), 0.0f);
    plugin_audio::playInstrument(*sc, nullptr, 0, oc.out(), 2, 512, SR, scratch, t);
    plugin_audio::playInstrument(*sd, &wheel, 1, od.out(), 2, 512, SR, scratch, t);
    CHECK(firstDifference(oc.l, od.l) == 200);
    bool half = true;
    for (int i = 200; i < 512; i++)
        if (std::fabs(od.l[(size_t)i] - oc.l[(size_t)i] * (1.0f - 127 / 254.0f)) > 1e-6f) half = false;
    CHECK(half);
    CHECK(!sd->failed.load());
}

// --- a VST3 plugin: IMidiMapping, changes of the mapped parameter at the messages' frames ---

int counts(int which) {
    std::string bin = testDir + "/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so";
    void* h = dlopen(bin.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!h) return -100;
    auto f = (int (*)(int))dlsym(h, "TinyTestFx_counts");
    int n = f ? f(which) : -101;
    dlclose(h);
    return n;
}

void testVst3() {
    std::string error;
    auto p = plug::Plugin::load(testDir + "/TinyTestFx.vst3", "Tiny Test Single", error);
    CHECK(p != nullptr);
    if (!p) return;
    p->prepare(SR, 512);
    Stereo out(512);
    int changes = counts(7);
    // the mod wheel (mapped to Level), half way, at frame 37
    p->render({controlMsg(37, 1, 64)}, out.out(), 512);
    CHECK(counts(5) == 37 && counts(6) == (int)std::lround(64 / 127.0 * 10000) && counts(7) == changes + 1);
    p->idle();  // (the window, and getParam, follow: as a change the plugin made)
    CHECK_NEAR(p->getParam(1), 64 / 127.0, 1e-9);
    // the bend wheel (mapped too): all the way up at 300, all the way down at 5
    p->render({bendMsg(300, 8191)}, out.out(), 512);
    CHECK(counts(5) == 300 && counts(6) == 10000);
    p->render({bendMsg(5, -8192)}, out.out(), 512);
    CHECK(counts(5) == 5 && counts(6) == 0);
    // a control it maps nothing to: nothing changes
    changes = counts(7);
    p->render({controlMsg(10, 7, 100)}, out.out(), 512);
    CHECK(counts(7) == changes);
    // a buffer longer than its blocks: the change in the block it falls in, at its offset there
    p->prepare(SR, 128);
    p->render({controlMsg(300, 1, 127)}, out.out(), 512);
    CHECK(counts(5) == 300 - 256 && counts(6) == 10000);
    // notes only, as before: nothing mapped changes
    changes = counts(7);
    p->render({noteOn(0, 60)}, out.out(), 512);
    CHECK(counts(7) == changes);
}

// --- the built-in synth played live, bent ---

void testLiveVoices() {
    CHECK_NEAR(live::bendSemitones(8191, 2), 2.0, 1e-12);
    CHECK_NEAR(live::bendSemitones(-8192, 2), -2.0, 1e-12);
    CHECK_NEAR(live::bendSemitones(0, 12), 0.0, 1e-12);
    CHECK_NEAR(live::bendSemitones(4096, 12), 12 * 4096 / 8191.0, 1e-12);

    live::LiveNotes notes;
    const int N = 480;
    std::vector<float> sound;
    std::vector<float> buf(N);
    plugin_audio::Messages msgs;
    auto play = [&](int buffers) {
        for (int k = 0; k < buffers; k++) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            float* sig[2] = {buf.data(), buf.data()};
            msgs.clear();
            notes.take(N, SR, msgs);
            notes.addVoices(sig, 1, N, SR);
            sound.insert(sound.end(), buf.begin(), buf.end());
        }
    };
    CHECK(notes.noteOn(69, 100, SR, true));
    play(100);  // 1 s
    // never bent: exactly the note as rendered
    int64_t loopStart;
    std::vector<float> rendered = live::builtInNote(live::NoteRender{}, 69, 100, -1, SR, loopStart);
    CHECK(sound.size() == 48000 && rendered.size() >= sound.size());
    CHECK(std::equal(sound.begin(), sound.end(), rendered.begin()));
    float steadyJump = maxJump(sound, 4800, 48000);
    int before = crossings(sound, 4800, 48000);
    // the wheel all the way up (± 2 semitones): MIDI for a VST instrument too
    CHECK(notes.pitchBend(8191, live::bendSemitones(8191, 2)));
    size_t at = sound.size();
    play(1);
    CHECK(msgs.size() == 1 && msgs[0].type == plugin_audio::MidiMsg::PitchBend && msgs[0].value == 8191);
    play(119);
    CHECK_NEAR(notes.bend(), 2.0, 1e-9);
    // no click on the way (the bend followed smoothly), and 2 semitones up once there
    CHECK(maxJump(sound, at - 1, at + 4800) <= steadyJump * 1.2f);
    int after = crossings(sound, at + 9600, at + 9600 + 43200);
    CHECK_NEAR((double)after / before, std::pow(2.0, 2.0 / 12), 0.01);
    // a sudden octave (another range) doesn't click either
    notes.pitchBend(8191, 12.0);
    at = sound.size();
    play(50);
    CHECK(maxJump(sound, at - 1, sound.size()) <= steadyJump * 2.1f);
    CHECK_NEAR(notes.bend(), 12.0, 1e-9);
    // back to the centre: the pitch it was played at again
    notes.pitchBend(0, 0.0);
    play(20);
    CHECK(notes.bend() == 0.0);
    at = sound.size();
    play(90);
    CHECK_NEAR((double)crossings(sound, at, at + 43200) / before, 1.0, 0.01);
    // a note struck while the wheel is bent sounds bent at once; let go, it fades out and ends
    notes.noteOff(69);
    play(10);
    CHECK(!notes.active());
    notes.pitchBend(-8192, -2.0);
    notes.noteOn(69, 100, SR, true);
    sound.clear();
    play(100);
    CHECK_NEAR(notes.bend(), -2.0, 1e-12);  // (nothing was sounding: no glide)
    CHECK_NEAR((double)crossings(sound, 4800, 48000) / before, std::pow(2.0, -2.0 / 12), 0.01);
    notes.noteOff(69);
    play(10);
    CHECK(!notes.active());
    // a control change: for a VST instrument only
    notes.control(1, 99);
    msgs.clear();
    notes.take(N, SR, msgs);
    CHECK(msgs.size() == 1 && msgs[0].type == plugin_audio::MidiMsg::Control && msgs[0].pitch == 1 &&
          msgs[0].value == 99);
    CHECK(!notes.control(200, 1));  // (no such control number)
    // a bend cleared away (the track not heard meanwhile) still counts: for the built-in voices at once,
    // for a VST instrument with the next buffer
    notes.pitchBend(8191, 2.0);
    notes.clear();
    CHECK(notes.bend() == 2.0 && notes.active());
    msgs.clear();
    notes.take(N, SR, msgs);
    CHECK(msgs.size() == 1 && msgs[0].type == plugin_audio::MidiMsg::PitchBend && msgs[0].value == 8191);
    msgs.clear();
    notes.take(N, SR, msgs);
    CHECK(msgs.empty() && !notes.active());
    notes.pitchBend(-8192, -2.0);
    play(1);
    // a sampler note (stereo, one-shot to its end) bent too: it still ends
    live::NoteRender stereo;
    stereo.channels = 2;
    stereo.oneShot = true;
    stereo.render = [](int, int, double, int sr) {
        std::vector<float> v((size_t)sr / 10 * 2);
        for (size_t i = 0; i < v.size() / 2; i++) v[2 * i] = v[2 * i + 1] = (float)std::sin(i * 0.05);
        return v;
    };
    notes.noteOn(60, 100, SR, true, -1, &stereo);
    play(20);  // 0.2 s: its 0.1 s, read at 2^(-2/12), is over
    CHECK(!notes.active());
}

// --- a track's bend range: the menu's dialog, saved only when not 2 ---

void testBendRange() {
    Project p;
    TrackPtr keys = p.addTrack(Kind::Midi), bass = p.addTrack(Kind::Midi);
    CHECK(keys->bendRange == 2);
    project_io::MasterSettings master;
    std::string text = project_io::describe(p, SR, master).dump();
    CHECK(text.find("bend_range") == std::string::npos);  // (older files unchanged)
    // Pitch bend range… (answered by the test)
    track_dialogs::answer = [](const std::string& title, std::string& value) {
        CHECK(title == "Pitch bend range" && value == "2");
        value = "12";
        return true;
    };
    CHECK(track_dialogs::askBendRange(nullptr, *keys));
    track_dialogs::answer = [](const std::string&, std::string& value) {
        value = "99";  // (kept within 1…24)
        return true;
    };
    CHECK(track_dialogs::askBendRange(nullptr, *bass));
    track_dialogs::answer = nullptr;
    CHECK(keys->bendRange == 12 && bass->bendRange == 24);
    std::string folder = "build/test-tmp/bend";
    util::makeDirs(folder);
    std::string path = folder + "/b.json", error;
    bass->bendRange = 2;
    CHECK(project_io::saveProject(path, p, SR, master, error));
    std::string saved;
    CHECK(util::readFile(path, saved));
    CHECK(saved.find("\"bend_range\": 12") != std::string::npos);
    CHECK(saved.find("bend_range") == saved.rfind("bend_range"));  // (only the track whose range isn't 2)
    Project q;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, q, SR, m2, error));
    CHECK(q.tracks.size() == 2 && q.tracks[0]->bendRange == 12 && q.tracks[1]->bendRange == 2);
    // a file with a range out of bounds: kept within 1…24
    std::string text0 = saved;
    text0.replace(text0.find("\"bend_range\": 12"), 17, "\"bend_range\": 0");
    std::string bad = folder + "/bad.json";
    CHECK(project_io::writeJson(bad, Json::parse(text0), error));
    Project r;
    CHECK(project_io::loadProject(bad, r, SR, m2, error));
    CHECK(r.tracks.size() == 2 && r.tracks[0]->bendRange == 1);
}

// --- the MIDI keyboard: the controller table, the selected track bent, the mod wheel ---

midi_in::Event bendEvent(int value) {
    midi_in::Event e;
    e.type = midi_in::Event::PitchBend;
    e.bend = (int16_t)value;
    return e;
}

void testMidiInput() {
    midi_cc::reset();
    Project project;
    audio::Engine engine{project};
    TrackPtr keys = project.addTrack(Kind::Midi), pads = project.addTrack(Kind::Midi);
    keys->needsInstrument = pads->needsInstrument = false;
    keys->bendRange = 12;
    TrackPtr current = keys;
    midi_input::MidiInput input(engine, project, [&] { return current; }, nullptr);
    auto taken = [&](const TrackPtr& t) {
        plugin_audio::Messages m;
        engine.liveNotes(t.get())->take(256, engine.sr, m);
        std::vector<float> buf(256);
        float* sig[2] = {buf.data(), buf.data()};
        engine.liveNotes(t.get())->addVoices(sig, 1, 256, engine.sr);
        return m;
    };
    input.handle(bendEvent(4096));
    CHECK(midi_cc::raw(midi_cc::PITCH_BEND) == 4096 && midi_cc::last() == midi_cc::PITCH_BEND);
    CHECK_NEAR(midi_cc::bend(), 4096 / 8191.0, 1e-12);
    auto m = taken(keys);
    CHECK(m.size() == 1 && m[0].type == plugin_audio::MidiMsg::PitchBend && m[0].value == 4096);
    CHECK_NEAR(engine.liveNotes(keys.get())->bend(), live::bendSemitones(4096, 12), 1e-12);  // (its range: 12)
    // another track selected: the one bent before is set straight, the new one bent
    current = pads;
    input.handle(bendEvent(-100));
    m = taken(keys);
    CHECK(m.size() == 1 && m[0].value == 0 && engine.liveNotes(keys.get())->bend() == 0.0);
    m = taken(pads);
    CHECK(m.size() == 1 && m[0].value == -100);
    CHECK_NEAR(engine.liveNotes(pads.get())->bend(), live::bendSemitones(-100, 2), 1e-12);
    // the mod wheel: noted in the table; on to the track's instrument plugins only
    uint32_t moves = midi_cc::moves();
    input.handle(midi_in::Event::control(1, 99));
    CHECK(midi_cc::raw(1) == 99 && midi_cc::moves() == moves + 1 && midi_cc::last() == 1);
    CHECK_NEAR(midi_cc::value(1), 99 / 127.0, 1e-12);
    CHECK(taken(pads).empty());  // (the built-in synth has no use for it)
    pads->usesRack = true;       // (an instrument patch panel: plays plugins)
    input.handle(midi_in::Event::control(1, 98));
    m = taken(pads);
    CHECK(m.size() == 1 && m[0].type == plugin_audio::MidiMsg::Control && m[0].pitch == 1 && m[0].value == 98);
    // other controls are noted (the sustain pedal too) but not sent on
    input.handle(midi_in::Event::control(74, 50));
    input.handle(midi_in::Event::control(midi_in::SUSTAIN, 127));
    CHECK(midi_cc::raw(74) == 50 && midi_cc::raw(midi_in::SUSTAIN) == 127);
    CHECK(taken(pads).empty());
    input.handle(midi_in::Event::control(midi_in::SUSTAIN, 0));
    // closing the input sets the bent track straight
    input.close();
    m = taken(pads);
    CHECK(m.size() == 1 && m[0].type == plugin_audio::MidiMsg::PitchBend && m[0].value == 0);
    // out of range: ignored by the table
    moves = midi_cc::moves();
    midi_cc::control(128, 5);
    midi_cc::control(-1, 5);
    CHECK(midi_cc::moves() == moves && midi_cc::raw(midi_cc::PITCH_BEND) == -100);
    CHECK(midi_cc::value(-1) == 0 && midi_cc::value(500) == 0 && midi_cc::raw(129) == 0);
}

// --- the MIDI Control modulator ---

SlotPtr builtinSlot(const char* id) {
    auto s = std::make_shared<PluginSlot>();
    std::shared_ptr<plug::Plugin> fx(effects::create(id).release());
    fx->prepare(SR, 4096);
    s->path = std::string("builtin:") + id;
    s->name = fx->name();
    s->kind = "effect";
    s->instance = fx;
    return s;
}

effects::Effect* fxOf(const SlotPtr& s) { return dynamic_cast<effects::Effect*>(aload(s->instance).get()); }

void setValue(const SlotPtr& s, const std::string& key, double value) {
    effects::Effect* e = fxOf(s);
    for (size_t i = 0; i < e->params().size(); i++)
        if (e->params()[i].key == key) aload(s->instance)->setParam((uint32_t)i, e->params()[i].toNormal(value));
}

void testModulator() {
    midi_cc::reset();
    TempoMap map(120, 4);
    auto clock = [&](double s) { return modulation::clockAt(map, true, (int64_t)std::llround(s * SR), 128, SR); };
    SlotPtr cc = builtinSlot("mod_cc");
    effects::Effect* m = fxOf(cc);
    CHECK(m && m->isModulator() && modulation::isModulator(cc.get()));
    CHECK(effects::categoryOf("builtin:mod_cc") == "Modulators");
    CHECK(aload(cc->instance)->paramText(0, aload(cc->instance)->getParam(0)).find("Mod wheel") != std::string::npos);
    // it follows the mod wheel (smoothly by default: 20 ms)
    CHECK_NEAR(m->modulation(clock(0)), 0.0, 1e-12);
    midi_cc::control(1, 127);
    double first = m->modulation(clock(0));
    CHECK(first > 0 && first < 1);
    for (int k = 0; k < 200; k++) m->modulation(clock(0));  // (200 steps of 128 frames: half a second)
    CHECK_NEAR(m->lastModulation(), 1.0, 1e-6);
    setValue(cc, "smooth", 0);  // no smoothing: at once
    midi_cc::control(1, 64);
    CHECK_NEAR(m->modulation(clock(0)), 64 / 127.0, 1e-12);
    // another controller: what it sent last
    setValue(cc, "controller", 74);
    midi_cc::control(74, 32);
    CHECK_NEAR(m->modulation(clock(0)), 32 / 127.0, 1e-12);
    // the bend wheel: both ways, 0 at its centre
    setValue(cc, "controller", midi_cc::PITCH_BEND);
    CHECK(aload(cc->instance)->paramText(0, aload(cc->instance)->getParam(0)) == "Pitch bend");
    CHECK_NEAR(m->modulation(clock(0)), 0.0, 1e-12);
    midi_cc::pitchBend(-8192);
    CHECK_NEAR(m->modulation(clock(0)), -1.0, 1e-12);
    midi_cc::pitchBend(8191);
    CHECK_NEAR(m->modulation(clock(0)), 1.0, 1e-12);

    // through a cable: the mod wheel turns a built-in effect's knob (Utility's Width) around its base
    setValue(cc, "controller", 1);
    midi_cc::control(1, 0);
    auto track = std::make_shared<Track>("T", Kind::Audio);
    SlotPtr util = builtinSlot("utility");
    track->effects = std::vector<SlotPtr>{cc, util};
    plug::Plugin* u = aload(util->instance).get();
    uint32_t width = 0;
    for (auto& p : automation::parameters(*util))
        if (p.name == "Width") width = p.id;
    u->setParam(width, 0.2);
    ModCable c;
    c.from = cc.get(), c.to = util.get(), c.param = "Width", c.depth = 0.6;
    track->modCables = std::vector<ModCable>{c};
    modulation::resolve(*track);
    CHECK(c.state->hasTarget && c.state->paramId == width);
    modulation::apply(*track->modCables, clock(0));
    CHECK_NEAR(u->getParam(width), 0.2, 1e-9);
    midi_cc::control(1, 127);  // (the mod wheel all the way up)
    modulation::apply(*track->modCables, clock(0));
    CHECK_NEAR(u->getParam(width), 0.2 + 0.6, 1e-9);
    midi_cc::control(1, 64);
    modulation::apply(*track->modCables, clock(0));
    CHECK_NEAR(u->getParam(width), 0.2 + 0.6 * 64 / 127.0, 1e-9);
    modulation::release(c);
    CHECK_NEAR(u->getParam(width), 0.2, 1e-9);
    midi_cc::reset();
}

}  // namespace

int main() {
    testDir = util::absolutePath(check::testDir());
    testVst2();
    testVst3();
    testLiveVoices();
    testBendRange();
    testMidiInput();
    testModulator();
    TEST_MAIN_END();
}
