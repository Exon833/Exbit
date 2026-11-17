// MIDI learn (ui/midi_learn.h): a control change links a parameter while learning, then moves it (its value
// 0…127 → 0…1 of the parameter): a built-in effect's knob, the test VST3 plugin's, a track's fader and pan;
// modulation's base follows; other channels, channel mode messages and Escape; the links saved with the
// project and read back (older files unchanged), removed (one, by parameter, in the links window), gone
// with their track; MIDI input's control hook; all of it on the calling (GUI) thread.
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/engine.h"
#include "audio/modulation.h"
#include "audio/automation.h"
#include "audio/automation_write.h"
#include "check.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/project_io.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "ui/midi_input.h"
#include "ui/midi_learn.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;
std::string scratch;

SlotPtr loaded(const std::string& path) {
    std::string error;
    std::unique_ptr<plug::Plugin> p = plug::Plugin::load(path, "", error);
    if (!p) {
        fprintf(stderr, "cannot load %s: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    p->prepare(SR);
    auto slot = std::make_shared<PluginSlot>();
    slot->path = path;
    slot->kind = "effect";
    slot->name = p->name();
    slot->instance = std::shared_ptr<plug::Plugin>(std::move(p));
    return slot;
}

effects::Effect* fx(const SlotPtr& s) { return s ? effects::asEffect(aload(s->instance).get()) : nullptr; }

// the first continuous parameter of a built-in effect
int continuous(const SlotPtr& s) {
    effects::Effect* e = fx(s);
    for (size_t i = 0; e && i < e->params().size(); i++)
        if (e->params()[i].steps() == 0 && e->params()[i].choices.empty()) return (int)i;
    return -1;
}

midi_in::Event cc(int number, int value, int channel = 0) { return midi_in::Event::control(number, value, channel); }

void pump(const std::shared_ptr<plug::Plugin>& p) {
    float a[64] = {}, b[64] = {}, c[64], d[64];
    const float* in[2] = {a, b};
    float* out[2] = {c, d};
    p->process(in, nullptr, out, 64);
    p->idle();
}

void testLearning() {
    Project project;
    TrackPtr track = project.addTrack(Kind::Audio);
    track->name = "Bass";
    SlotPtr eq = loaded("builtin:eq");
    SlotPtr lfo = loaded("builtin:mod_lfo");
    CHECK(eq && lfo);
    if (!eq || !lfo) return;
    track->effects = std::vector<SlotPtr>{eq, lfo};
    track->cables = chain({eq});
    int k = continuous(eq);
    CHECK(k >= 0);
    std::string param = fx(eq)->params()[k].name;
    midi_learn::Learner learner(project);
    std::vector<std::string> said;
    std::thread::id gui = std::this_thread::get_id();
    bool sameThread = true;
    int mixerMoves = 0;
    learner.status = [&](const std::string& t, int) {
        said.push_back(t);
        sameThread = sameThread && std::this_thread::get_id() == gui;
    };
    learner.mixerMoved = [&](const TrackPtr& t) {
        mixerMoves += t == track;
        sameThread = sameThread && std::this_thread::get_id() == gui;
    };
    // nothing linked: control changes aren't used
    CHECK(!learner.handle(cc(74, 100)));
    // learning: the first control change links it (not moving it yet)
    double was = fx(eq)->getParam((uint32_t)k);
    learner.learn(track, eq, param);
    CHECK(learner.waiting() && learner.waitingFor() == "Bass \xe2\x80\xba EQ \xe2\x80\xba " + param);
    CHECK(!said.empty() && said.back().find("Esc cancels") != std::string::npos);
    CHECK(!learner.handle(cc(123, 0)) && learner.waiting());  // (all notes off: never learnt)
    CHECK(!learner.handle(midi_in::Event::noteOn(60, 100)) && learner.waiting());
    CHECK(learner.handle(cc(74, 10, 2), "Knobs 49"));
    CHECK(!learner.waiting() && project.midiLinks.size() == 1);
    if (project.midiLinks.size() != 1) return;
    const MidiLink& link = project.midiLinks[0];
    CHECK(link.cc == 74 && link.channel == 2 && link.controller == "Knobs 49" && link.track == track &&
          link.slot == eq && link.param == param);
    CHECK(midi_learn::controlName(link) == "CC 74, channel 3 (Knobs 49)");
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), was, 1e-12);
    // then it moves the parameter: 0…127 → 0…1
    CHECK(learner.handle(cc(74, 127, 2)));
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), 1.0, 1e-9);
    CHECK(learner.handle(cc(74, 0, 2)));
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), 0.0, 1e-9);
    CHECK(learner.handle(cc(74, 64, 2)));
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), 64.0 / 127, 1e-9);
    CHECK(fx(eq)->edited().empty());  // (no window open: its edits were taken here, for modulation)
    // another channel, another number: not this link
    CHECK(!learner.handle(cc(74, 127, 0)) && !learner.handle(cc(75, 127, 2)));
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), 64.0 / 127, 1e-9);
    // with its window open, its TweakWatcher hears it as a turn of the knob
    learner.windowOpen = [&](const PluginSlot* s) { return s == eq.get(); };
    CHECK(learner.handle(cc(74, 32, 2)));
    std::vector<uint32_t> edited = fx(eq)->edited();
    CHECK(edited.size() == 1 && edited[0] == (uint32_t)k);
    learner.windowOpen = nullptr;
    // Escape cancels learning
    learner.learn(track, eq, param);
    learner.cancel();
    CHECK(!learner.waiting() && said.back().find("cancelled") != std::string::npos);
    CHECK(learner.handle(cc(74, 127, 2)) && project.midiLinks.size() == 1);  // (the link as it was)

    // a modulated knob: its base follows the controller
    ModCable m;
    m.from = lfo.get();
    m.to = eq.get();
    m.param = param;
    m.depth = 0.5;
    track->modCables = std::vector<ModCable>{m};
    modulation::resolve(project);
    CHECK(m.state->hasTarget);
    CHECK(learner.handle(cc(74, 127, 2)));
    CHECK_NEAR(m.state->base.load(), 1.0, 1e-9);
    CHECK(learner.handle(cc(74, 0, 2)));
    CHECK_NEAR(m.state->base.load(), 0.0, 1e-9);

    // the track's fader and pan
    learner.learn(track, track->controls, "Volume");
    CHECK(learner.handle(cc(7, 50)));
    learner.learn(track, track->controls, "Pan");
    CHECK(learner.handle(cc(10, 50)));
    CHECK(project.midiLinks.size() == 3);
    CHECK(learner.handle(cc(7, 127)) && track->volumeDb == config::MAX_DB);
    CHECK(learner.handle(cc(7, 0)) && track->volumeDb == config::MIN_DB);
    CHECK(learner.handle(cc(10, 0)) && track->pan == -1);
    CHECK(learner.handle(cc(10, 127)) && track->pan == 1);
    CHECK(learner.handle(cc(10, 64)));
    CHECK_NEAR(track->pan, 0.01, 1e-12);
    CHECK(mixerMoves == 5);
    // learning again replaces a parameter's link
    learner.learn(track, track->controls, "Pan");
    CHECK(learner.handle(cc(11, 0)));
    CHECK(project.midiLinks.size() == 3 && learner.linksOf(track->controls.get(), "Pan").size() == 1);
    CHECK(!learner.handle(cc(10, 0)) && learner.handle(cc(11, 0)) && track->pan == -1);
    // one knob may move several parameters
    MidiLink both = project.midiLinks[1];  // (Volume)
    both.cc = 11;
    both.slot = track->controls;
    both.param = "Volume";
    learner.add(both);
    CHECK(project.midiLinks.size() == 3);  // (Volume's link replaced)
    CHECK(learner.handle(cc(11, 127)) && track->pan == 1 && track->volumeDb == config::MAX_DB);
    // a frozen track's plugins are locked (its fader isn't)
    auto frozen = std::make_shared<Clip>();
    track->frozen = frozen;
    double before = fx(eq)->getParam((uint32_t)k);
    CHECK(!learner.handle(cc(74, 127, 2)));
    CHECK_NEAR(fx(eq)->getParam((uint32_t)k), before, 1e-12);
    track->frozen = nullptr;

    // removal: by parameter, by index
    learner.removeLinksOf(track->controls.get(), "Pan");
    CHECK(project.midiLinks.size() == 2 && learner.linksOf(track->controls.get(), "Pan").empty());
    learner.remove(0);
    CHECK(project.midiLinks.size() == 1 && project.midiLinks[0].param == "Volume");
    CHECK(sameThread);
}

// --- saved with the project, read back; older files and projects without links unchanged ---

void testSaving() {
    Project project;
    TrackPtr a = project.addTrack(Kind::Audio);
    a->name = "Drums";
    TrackPtr b = project.addTrack(Kind::Midi);
    b->name = "Keys";
    SlotPtr eq = loaded("builtin:eq"), comp = loaded("builtin:compressor"), master = loaded("builtin:limiter");
    CHECK(eq && comp && master);
    if (!eq || !comp || !master) return;
    b->effects = std::vector<SlotPtr>{eq, comp};
    b->cables = chain({eq, comp});
    project.masterEffects = std::vector<SlotPtr>{master};
    project.masterCables = chain({master});
    project_io::MasterSettings ms;
    Json plainJson = project_io::describe(project, SR, ms);
    CHECK(!plainJson.has("midi_links"));  // (no links: saved as before)
    midi_learn::Learner learner(project);
    learner.learn(b, comp, fx(comp)->params()[1].name);
    learner.handle(cc(21, 0, 0), "Knobs");
    learner.learn(a, a->controls, "Pan");
    learner.handle(cc(10, 0, 15), "Knobs");
    learner.learn(nullptr, master, fx(master)->params()[0].name);
    learner.handle(cc(22, 0, 1), "Knobs");
    CHECK(project.midiLinks.size() == 3);
    std::string path = util::joinPath(scratch, "linked.json"), error;
    CHECK(project_io::saveProject(path, project, SR, ms, error));
    std::string text;
    util::readFile(path, text);
    Json data = Json::parse(text);
    const Json& links = static_cast<const Json&>(data)["midi_links"];
    CHECK(links.size() == 3);
    CHECK(links[size_t(0)]["track"] == Json(1) && links[size_t(0)]["plugin"] == Json(1));
    CHECK(links[size_t(1)]["plugin"] == Json("track"));
    CHECK(links[size_t(2)]["track"] == Json("master"));
    Project loadedProject;
    CHECK(project_io::loadProject(path, loadedProject, SR, ms, error));
    CHECK(loadedProject.midiLinks.size() == 3);
    if (loadedProject.midiLinks.size() == 3) {
        const MidiLink& l0 = loadedProject.midiLinks[0];
        CHECK(l0.track == loadedProject.tracks[1] && l0.slot == loadedProject.tracks[1]->effects[1] &&
              l0.param == fx(comp)->params()[1].name && l0.cc == 21 && l0.channel == 0 && l0.controller == "Knobs");
        const MidiLink& l1 = loadedProject.midiLinks[1];
        CHECK(l1.track == loadedProject.tracks[0] && l1.slot == loadedProject.tracks[0]->controls && l1.param == "Pan" &&
              l1.channel == 15);
        const MidiLink& l2 = loadedProject.midiLinks[2];
        CHECK(!l2.track && l2.slot == loadedProject.masterEffects[0] && l2.cc == 22);
    }
    // they move the loaded project's parameters (its fader here: no plugins loaded)
    midi_learn::Learner again(loadedProject);
    CHECK(again.handle(cc(10, 127, 15)) && loadedProject.tracks[0]->pan == 1);
    CHECK(!again.handle(cc(21, 127, 0)));  // (its compressor isn't loaded: nothing moves)
    // a file without links: none; links naming what isn't there: left out
    data.erase("midi_links");
    CHECK(project_io::writeJson(path, data, error) && project_io::loadProject(path, loadedProject, SR, ms, error));
    CHECK(loadedProject.midiLinks.empty());
    Json bad = Json::parse("[{\"cc\": 5, \"channel\": 0, \"track\": 9, \"plugin\": 0, \"param\": \"x\"},"
                           " {\"cc\": 300, \"channel\": 0, \"track\": 0, \"plugin\": \"track\", \"param\": \"Pan\"},"
                           " {\"cc\": 5, \"channel\": 0, \"track\": 0, \"plugin\": \"track\", \"param\": \"Pan\"}]");
    std::vector<MidiLink> read = project_io::readMidiLinks(bad, *loadedProject.tracks, *loadedProject.masterEffects);
    CHECK(read.size() == 1 && read[0].param == "Pan" && read[0].cc == 5);
    // a link whose track is gone: shown so, moves nothing, not saved (undo may bring the track back)
    project.removeTrack(a);
    CHECK(!midi_learn::present(project, project.midiLinks[1]));
    CHECK(!learner.handle(cc(10, 127, 15)));
    CHECK(project_io::describeMidiLinks(project).size() == 2);
    // the links window: each listed (the gone one said so), removable
    midi_learn::LinksDialog d(nullptr, learner);
    CHECK(d.list->count() == 3 && d.list->item(1).text.find("(gone)") != std::string::npos);
    CHECK(!d.list->item(0).tooltip.empty() && !d.removeButton->toolTip().empty());
    d.list->setCurrentRow(1);
    d.removeSelected();
    CHECK(project.midiLinks.size() == 2 && d.list->count() == 2);
}

// --- the test VST3 plugin's parameter, the menu entries, MIDI input's control hook ---

void testPluginAndInput() {
    Project project;
    TrackPtr track = project.addTrack(Kind::Audio);
    SlotPtr v = loaded(std::string(check::testDir()) + "/TinyTestFx.vst3");
    CHECK(v != nullptr);
    if (!v) return;
    track->effects = std::vector<SlotPtr>{v};
    track->cables = chain({v});
    midi_learn::Learner learner(project);
    midi_learn::setActive(&learner);
    std::vector<std::string> names = midi_learn::parameterNames(*v);
    CHECK(std::find(names.begin(), names.end(), "Gain") != names.end());
    CHECK((midi_learn::parameterNames(*track->controls) == std::vector<std::string>{"Volume", "Pan"}));
    // the menu entries (with tooltips), as a knob's right-click shows them
    ui::Menu menu;
    midi_learn::fillParamMenu(menu, track, v, "Gain");
    CHECK(menu.actions().size() == 1 && menu.actions()[0]->text == "Link to a MIDI controller\xe2\x80\xa6" &&
          !menu.actions()[0]->tooltip.empty());
    menu.actions()[0]->triggered();
    CHECK(learner.waiting());
    // through the MIDI input, as the main window wires it
    audio::Engine engine(project);
    midi_input::MidiInput input(engine, project, [] { return TrackPtr(); }, nullptr);
    input.controlHook = [&](const midi_in::Event& e) { return learner.handle(e, "Knobs"); };
    input.handle(cc(30, 0));
    CHECK(project.midiLinks.size() == 1 && project.midiLinks[0].param == "Gain");
    auto inst = aload(v->instance);
    input.handle(cc(30, 127));
    pump(inst);
    CHECK_NEAR(inst->getParam(100), 1.0, 1e-6);
    input.handle(cc(30, 0));
    pump(inst);
    CHECK_NEAR(inst->getParam(100), 0.0, 1e-6);
    // notes and the pedal still play as before
    input.handle(midi_in::Event::noteOn(60, 100));
    input.handle(midi_in::Event::noteOff(60));
    CHECK(input.log.size() == 2);
    ui::Menu linked;
    midi_learn::fillParamMenu(linked, track, v, "Gain");
    CHECK(linked.actions().size() == 2 && linked.actions()[1]->text.find("CC 30") != std::string::npos);
    linked.actions()[1]->triggered();
    CHECK(project.midiLinks.empty());
    midi_learn::setActive(nullptr);
    ui::Menu none;
    midi_learn::fillParamMenu(none, track, v, "Gain");
    CHECK(none.isEmpty());  // (no learner: not offered)
}

}  // namespace

// a knob of a MIDI controller linked to the fader writes an armed Volume lane while the song plays, as moving
// the fader by hand does (the automation recorder: automation_write.h)
void testWritingAutomation() {
    Project project;
    TrackPtr a = project.addTrack(Kind::Audio);
    auto lane = std::make_shared<Automation>();
    lane->slot = a->controls;
    lane->param = automation::VOLUME;
    lane->armed = true;
    a->automation = std::vector<AutomationPtr>{lane};
    CHECK(automation::armedControlLane(*a, automation::VOLUME) == lane);
    midi_learn::Learner learner(project);
    learner.learn(a, a->controls, automation::VOLUME);
    learner.handle(cc(7, 0), "Knobs");
    CHECK(project.midiLinks.size() == 1);
    double now = 1.0;
    automation::recorder().songTime = [&](double& seconds) {
        seconds = now;
        return true;
    };
    for (int k = 0; k <= 10; k++) {  // turned from 0 up to 127 over half a second of the song
        now = 1.0 + k * 0.05;
        CHECK(learner.handle(cc(7, (int)std::lround(127.0 * k / 10))));
    }
    automation::recorder().releaseAll();
    automation::recorder().songTime = nullptr;
    auto points = lane->points.copy();
    CHECK(points.size() >= 2);
    if (points.size() >= 2) {
        double first = 1e9, last = -1e9;
        for (auto& p : points) first = std::min(first, p.t), last = std::max(last, p.t);
        CHECK(first <= 1.0 + 1e-6 && last >= 1.5 - 1e-6);
        CHECK_NEAR(automation::valueAt(points, 1.5, 0), 1.0, 0.02);
        CHECK(automation::valueAt(points, 1.0, 0) < 0.1);
    }
}

int main() {
    scratch = util::absolutePath("build/tests/midi-learn-scratch");
    util::makeDirs(scratch);
    settings::setPath(util::joinPath(scratch, "settings.json"));
    ui::App::get().initHeadless();
    testLearning();
    testSaving();
    testPluginAndInput();
    testWritingAutomation();
    TEST_MAIN_END();
}
