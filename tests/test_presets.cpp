// Presets (model/presets.h, ui/presets_ui.h): an effect's settings saved and loaded again (a built-in effect:
// the same values; the test VST3 plugin: the same state), a track's chain saved and read back (effects and
// their places, cables, modulation cables, the sampler and its sample file, MIDI effects), loading one
// replacing a track's chain as one undo step, a preset whose plugin is missing, renaming and deleting, the
// Explorer's Presets place, and comparing (Keep this, Now / Kept) both ways, with the effect window marking
// the knobs that differ.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

#include "audio/modulation.h"
#include "check.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/history.h"
#include "model/presets.h"
#include "model/project_io.h"
#include "paths.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "ui/effect_window.h"
#include "ui/explorer.h"
#include "ui/presets_ui.h"
#include "ui/theme.h"
#include "ui/toolkit/menu.h"

namespace {

const int SR = 48000;
std::string scratch;

SlotPtr loaded(const std::string& path, const std::string& kind = "effect", const std::string& pluginName = "") {
    std::string error;
    std::unique_ptr<plug::Plugin> p = plug::Plugin::load(path, pluginName, error);
    if (!p) {
        fprintf(stderr, "cannot load %s: %s\n", path.c_str(), error.c_str());
        return nullptr;
    }
    p->prepare(SR);
    auto slot = std::make_shared<PluginSlot>();
    slot->path = path;
    slot->pluginName = pluginName;
    slot->kind = kind;
    slot->name = p->name();
    slot->instance = std::shared_ptr<plug::Plugin>(std::move(p));
    return slot;
}

effects::Effect* fx(const SlotPtr& s) { return s ? effects::asEffect(aload(s->instance).get()) : nullptr; }

// a built-in effect's parameters (0…1), to 1e-9 (its settings are saved as plain values: the same once
// read back, within rounding)
std::vector<double> values(const SlotPtr& s) {
    std::vector<double> out;
    if (effects::Effect* e = fx(s))
        for (size_t i = 0; i < e->params().size(); i++) out.push_back(std::round(e->getParam((uint32_t)i) * 1e9) / 1e9);
    return out;
}
// …and its values as saved: identical
std::string plain(const SlotPtr& s) { return fx(s) ? fx(s)->componentState() : std::string(); }

// a buffer through a plugin: changes made as in its window reach its processor (and its state)
void pump(const std::shared_ptr<plug::Plugin>& p) {
    float a[64] = {}, b[64] = {}, c[64], d[64];
    const float* in[2] = {a, b};
    float* out[2] = {c, d};
    p->process(in, nullptr, out, 64);
    p->idle();
}

std::string vst3() { return std::string(check::testDir()) + "/TinyTestFx.vst3"; }

AudioPtr tone(int frames) {
    auto a = std::make_shared<AudioBuffer>();
    a->samples.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) a->samples[i * 2] = a->samples[i * 2 + 1] = (float)std::sin(i * 0.05) * 0.5f;
    return a;
}

// --- an effect's preset ---

void testEffectPresets() {
    Project project;
    SlotPtr eq = loaded("builtin:eq");
    CHECK(eq && fx(eq));
    if (!eq) return;
    effects::Effect* e = fx(eq);
    for (size_t i = 0; i < e->params().size(); i++) e->windowEdit((uint32_t)i, 0.1 + 0.8 * ((i * 37) % 11) / 10.0);
    std::vector<double> before = values(eq);
    std::string beforePlain = plain(eq);
    std::string path, error;
    CHECK(presets::saveEffect(*eq, "Bright / airy", path, error));
    CHECK(util::fileExists(path) && util::baseName(path) == "Bright _ airy.json");
    CHECK(util::dirName(path) == util::joinPath(paths::dataDir("presets"), "effects/EQ"));
    std::vector<presets::Entry> list = presets::list(presets::effectsFolder(*eq));
    CHECK(list.size() == 1 && list[0].name == "Bright _ airy");
    // back to the defaults, then the preset: the very same values
    for (size_t i = 0; i < e->params().size(); i++) e->windowEdit((uint32_t)i, e->params()[i].toNormal(e->params()[i].def));
    CHECK(values(eq) != before);
    CHECK(presets::applyEffect(project, eq, path, error));
    CHECK(values(eq) == before && plain(eq) == beforePlain);
    CHECK(!eq->state.empty());
    // another EQ (a new slot) lists and takes it too
    SlotPtr eq2 = loaded("builtin:eq");
    CHECK(eq2 && presets::list(presets::effectsFolder(*eq2)).size() == 1);
    CHECK(eq2 && presets::applyEffect(project, eq2, path, error) && values(eq2) == before);
    // not loaded: said so
    auto empty = std::make_shared<PluginSlot>(*eq);
    empty->instance = nullptr;
    error.clear();
    CHECK(!presets::applyEffect(project, empty, path, error) && !error.empty());
    // not a preset
    std::string bogus = util::joinPath(scratch, "bogus.json");
    util::writeFile(bogus, "{\"format\": \"something\"}");
    CHECK(!presets::applyEffect(project, eq, bogus, error));

    // the test VST3 plugin: its state comes back identical
    SlotPtr v = loaded(vst3());
    CHECK(v != nullptr);
    if (v) {
        auto inst = aload(v->instance);
        inst->windowEdit(100, 0.8);
        inst->windowEdit(101, 1.0);
        pump(inst);
        std::string want = plug::stateOf(*inst);
        CHECK(presets::saveEffect(*v, "Loud", path, error));
        inst->windowEdit(100, 0.2);
        inst->windowEdit(101, 0.0);
        pump(inst);
        CHECK(plug::stateOf(*inst) != want);
        CHECK(presets::applyEffect(project, v, path, error));
        pump(inst);
        CHECK(plug::stateOf(*inst) == want);
        CHECK_NEAR(inst->getParam(100), 0.8, 1e-6);
        // rename and delete
        std::string renamed;
        CHECK(presets::rename(path, "Louder", renamed, error) && util::fileExists(renamed) && !util::fileExists(path));
        CHECK(presets::list(presets::effectsFolder(*v)).size() == 1);
        CHECK(presets::remove(renamed, error) && !util::fileExists(renamed));
        CHECK(presets::list(presets::effectsFolder(*v)).empty());
    }
}

// --- compare: Keep this, Now / Kept ---

void testCompare() {
    Project project;
    SlotPtr comp = loaded("builtin:compressor");
    CHECK(comp != nullptr);
    if (!comp) return;
    effects::Effect* e = fx(comp);
    std::string error;
    CHECK(presets::compareOf(comp.get()) == nullptr);
    CHECK(!presets::switchTo(project, comp, true, error));  // (nothing kept yet)
    e->windowEdit(0, 0.2);
    e->windowEdit(1, 0.3);
    std::vector<double> a = values(comp);
    CHECK(presets::keep(project, comp, error));
    CHECK(presets::differing(*comp).empty());
    e->windowEdit(0, 0.7);
    std::vector<double> b = values(comp);
    CHECK(presets::differing(*comp) == std::vector<int>{0});
    CHECK(presets::switchTo(project, comp, true, error));  // to KEPT
    CHECK(values(comp) == a && presets::compareOf(comp.get())->onKept);
    CHECK(presets::differing(*comp) == std::vector<int>{0});
    CHECK(!presets::switchTo(project, comp, true, error));  // (already)
    e->windowEdit(1, 0.9);  // a tweak of KEPT, remembered as KEPT's
    std::vector<double> a2 = values(comp);
    CHECK(presets::switchTo(project, comp, false, error));  // back to NOW
    CHECK(values(comp) == b);
    CHECK(presets::switchTo(project, comp, true, error));
    CHECK(values(comp) == a2);
    // a plugin: just switches
    SlotPtr v = loaded(vst3());
    if (v) {
        auto inst = aload(v->instance);
        inst->windowEdit(100, 0.25);
        pump(inst);
        std::string kept = plug::stateOf(*inst);
        CHECK(presets::keep(project, v, error));
        inst->windowEdit(100, 0.75);
        pump(inst);
        std::string now = plug::stateOf(*inst);
        CHECK(presets::switchTo(project, v, true, error) && plug::stateOf(*inst) == kept);
        CHECK(presets::switchTo(project, v, false, error) && plug::stateOf(*inst) == now);
        CHECK(presets::differing(*v).empty());  // (only built-in effects' knobs are marked)
    }
    // forgotten with the slot
    const PluginSlot* gone = comp.get();
    comp.reset();
    CHECK(presets::compareOf(gone) == nullptr);
}

// --- the effect window: Presets ▾, and the knobs that differ marked ---

void testEffectWindow() {
    Project project;
    ui::Window main;
    ui::App::get().mainWindow = &main;
    presets_ui::Host h;
    h.project = &project;
    h.ask = [](const std::string&, std::string&) { return true; };
    presets_ui::setHost(h);
    SlotPtr comp = loaded("builtin:compressor");
    CHECK(comp != nullptr);
    if (!comp) return;
    auto* w = new EffectWindow(&main, comp, "Compressor — Test");
    CHECK(w->presetsButton != nullptr && w->nameLabels.size() == fx(comp)->params().size());
    // (its menu draws its ▾: none in the text)
    CHECK(w->presetsButton && w->presetsButton->text() == "Presets" && w->presetsButton->menu());
    bool all = true;
    for (auto* l : w->nameLabels) all = all && l;
    CHECK(all);
    ui::Menu menu;
    presets_ui::fillEffectMenu(menu, w, comp);
    std::vector<std::string> texts;
    for (auto& a : menu.actions()) texts.push_back(a->text);
    auto has = [&](const std::string& t) { return std::find(texts.begin(), texts.end(), t) != texts.end(); };
    CHECK(has("Save as\xe2\x80\xa6") && has("Keep this") && has("Now") && has("Kept"));
    bool tips = true;  // (every entry that does something says what)
    for (auto& a : menu.actions())
        if (!a->separator && !a->section && a->enabled && a->submenu == nullptr) tips = tips && !a->tooltip.empty();
    CHECK(tips);
    CHECK(presets_ui::keep(w, comp));
    fx(comp)->windowEdit(2, 0.95);
    w->follow();
    CHECK(w->marked == std::vector<int>{2});
    CHECK(w->nameLabels[2]->color() == theme::c("accent"));
    CHECK(presets_ui::compare(w, comp, true));
    w->follow();
    CHECK(w->marked == std::vector<int>{2});
    CHECK(presets_ui::compare(w, comp, false));
    fx(comp)->windowEdit(2, fx(comp)->getParam(2));  // (unchanged)
    // saving from the window's menu (the name answered), and it is listed
    presets_ui::host().ask = [](const std::string&, std::string& answer) {
        answer = "Glue";
        return true;
    };
    CHECK(presets_ui::saveEffectAs(w, comp));
    ui::Menu again;
    presets_ui::fillEffectMenu(again, w, comp);
    bool listed = false;
    for (auto& a : again.actions()) listed = listed || a->text == "Glue";
    CHECK(listed);
    w->close();
    ui::App::get().processEvents();
    presets::forgetCompare(comp.get());
    ui::App::get().mainWindow = nullptr;
}

// --- a track's chain ---

struct Chain {
    TrackPtr track;
    SlotPtr eq, comp, lfo;
};

Chain makeChain(Project& project) {
    Chain c;
    c.track = project.addTrack(Kind::Midi);
    c.track->name = "Keys";
    c.track->needsInstrument = false;
    c.eq = loaded("builtin:eq");
    c.comp = loaded("builtin:compressor");
    c.lfo = loaded("builtin:mod_lfo");
    if (!c.eq || !c.comp || !c.lfo) return c;
    c.eq->placed = true;
    c.eq->atX = 140;
    c.eq->atY = 60;
    c.comp->placed = true;
    c.comp->atX = 300;
    c.comp->atY = 200;
    c.comp->mix = 0.5;
    c.comp->bypass = true;
    fx(c.eq)->windowEdit(1, 0.66);
    fx(c.comp)->windowEdit(0, 0.3);
    c.track->effects = std::vector<SlotPtr>{c.eq, c.comp, c.lfo};
    TrackPtr other = project.addTrack(Kind::Audio);
    // in parallel: IN → EQ → OUT and IN → Compressor → OUT, the EQ into the compressor's sidechain, and
    // another track into it too (left out of a preset)
    c.track->cables = std::vector<Cable>{{End::in(), End::of(c.eq.get())},
                                         {End::of(c.eq.get()), End::out()},
                                         {End::in(), End::of(c.comp.get())},
                                         {End::of(c.comp.get()), End::out()},
                                         {End::of(c.eq.get()), End::sidechain(c.comp.get())},
                                         {End::source(other.get()), End::sidechain(c.comp.get())},
                                         {End::in(), End::of(c.lfo.get())}};
    ModCable m;
    m.from = c.lfo.get();
    m.to = c.eq.get();
    m.param = fx(c.eq)->params()[2].name;
    m.depth = -0.4;
    m.state->base = 0.3;
    c.track->modCables = std::vector<ModCable>{m};
    auto s = std::make_shared<SamplerSettings>();
    s->sample = makeAudioClip("Piano C4", tone(4800), 44100, SR);
    s->root = 62;
    s->transpose = 0.5;
    s->attack = 0.01;
    s->oneShot = true;
    s->start = 0.1;
    s->end = 0.9;
    c.track->sampler = s;
    MidiEffect chord;
    MidiEffect::make("Chord", {}, chord);
    c.track->midiEffects = std::vector<MidiEffect>{chord};
    auto clip = makeMidiClip("Riff", {Note{0, 0.5, 60, 100}, Note{0.5, 1, 64, 90}}, 1.0, SR);
    clip->track = c.track;
    project.addClip(clip);
    return c;
}

void testTrackPresets() {
    Project project;
    Chain c = makeChain(project);
    CHECK(c.eq && c.comp && c.lfo);
    if (!c.eq || !c.comp || !c.lfo) return;
    std::string path, error;
    CHECK(presets::saveTrack(*c.track, SR, "Keys", path, error));
    CHECK(path == util::joinPath(presets::tracksFolder(), "Keys.json") && presets::isTrackPresetFile(path));
    CHECK(c.track->sampler->sample->sourcePath.empty());  // (the track's own sample is left as it was)
    std::string audio = presets::audioFolderOf(path);
    CHECK(util::isDir(audio) && util::listDir(audio).size() == 1);
    // saved again over it: replaced once the new one is written (nothing temporary left)
    CHECK(presets::saveTrack(*c.track, SR, "Keys", path, error));
    CHECK(util::listDir(audio).size() == 1 && util::listDir(presets::tracksFolder()).size() == 2);
    // read back: everything of the chain
    presets::TrackPreset p;
    CHECK(presets::readTrack(path, SR, p, error));
    CHECK(p.name == "Keys" && p.kind == Kind::Midi && p.chain);
    if (!p.chain) return;
    const Track& t = *p.chain;
    CHECK(t.effects.size() == 3 && p.slots().size() == 3);
    if (t.effects.size() != 3) return;
    CHECK(t.effects[0]->path == "builtin:eq" && t.effects[1]->path == "builtin:compressor" &&
          t.effects[2]->path == "builtin:mod_lfo");
    CHECK(t.effects[0]->placed && t.effects[0]->atX == 140 && t.effects[0]->atY == 60);
    CHECK(t.effects[1]->mix == 0.5 && t.effects[1]->bypass && !t.effects[0]->bypass);
    CHECK(!aload(t.effects[0]->instance) && !t.effects[0]->state.empty());  // (unloaded, its settings kept)
    CHECK(project_io::describeCables(*t.cables, *t.effects, {}) ==
          project_io::describeCables(*c.track->cables, *c.track->effects, {}));
    CHECK(t.cables.size() == 6);  // (the other track's cable left out)
    CHECK(t.modCables.size() == 1);
    if (t.modCables.size() == 1) {
        const ModCable& m = t.modCables[0];
        CHECK(m.from == t.effects[2].get() && m.to == t.effects[0].get());
        CHECK(m.param == fx(c.eq)->params()[2].name && m.depth == -0.4);
        CHECK_NEAR(m.state->base.load(), 0.3, 1e-9);
    }
    CHECK(t.sampler && t.sampler->sample);
    if (t.sampler && t.sampler->sample) {
        const SamplerSettings& s = *t.sampler;
        CHECK(s.root == 62 && s.transpose == 0.5 && s.attack == 0.01 && s.oneShot && s.start == 0.1 && s.end == 0.9);
        CHECK(s.sample->sourceSr == 44100 && s.sample->source->samples == c.track->sampler->sample->source->samples);
        CHECK(util::dirName(s.sample->sourcePath) == audio);
    }
    CHECK(*t.midiEffects == *c.track->midiEffects);

    // --- loaded onto a track: it replaces the chain, one undo step ---
    TrackPtr target = project.addTrack(Kind::Midi);
    target->name = "Pad";
    target->needsInstrument = false;
    SlotPtr util1 = loaded("builtin:utility");
    target->effects = std::vector<SlotPtr>{util1};
    target->cables = chain({util1});
    auto lane = std::make_shared<Automation>();
    lane->slot = util1;
    lane->param = fx(util1)->params()[0].name;
    lane->setPoints({{0, 0.2}, {1, 0.8}});
    target->automation = std::vector<AutomationPtr>{lane};
    history::History hist(project, SR);
    std::vector<std::string> said;
    presets_ui::Host h;
    h.project = &project;
    h.sampleRate = [] { return SR; };
    h.status = [&](const std::string& text, int) { said.push_back(text); };
    h.ask = [](const std::string&, std::string&) { return true; };
    h.nameStep = [&](const std::string& label) {
        hist.check(SR);
        hist.nameNext(label);
    };
    h.trackChanged = [&](const TrackPtr&) { hist.check(SR); };
    h.newTrack = [&](Kind k, const std::string& name) {
        TrackPtr n = project.addTrack(k);
        n->name = name;
        return n;
    };
    presets_ui::setHost(h);
    CHECK(presets_ui::loadTrack(nullptr, target, path));
    CHECK(target->effects.size() == 3 && target->effects[0]->path == "builtin:eq");
    bool allLoaded = true;
    for (auto& s : target->effects) allLoaded = allLoaded && aload(s->instance);
    CHECK(allLoaded);
    if (target->effects.size() == 3) {
        CHECK(values(target->effects[0]) == values(c.eq) && values(target->effects[1]) == values(c.comp));
        CHECK(target->modCables.size() == 1 && target->modCables[0].from == target->effects[2].get() &&
              target->modCables[0].to == target->effects[0].get());
    }
    CHECK(target->sampler && target->sampler->root == 62 && target->midiEffects.size() == 1);
    CHECK(target->automation.empty());  // (the utility's lane went with it)
    CHECK(hist.labels().size() == 1 && hist.labels().back() == "Load track preset");
    CHECK(aload(util1->instance) != nullptr);  // (retired, not destroyed: undo brings it back as it was)
    CHECK(hist.undo(SR));
    CHECK(target->effects.size() == 1 && target->effects[0] == util1 && aload(util1->instance));
    CHECK(!target->sampler && target->midiEffects.empty() && target->modCables.empty());
    CHECK(target->automation.size() == 1 && *target->cables == chain({util1}));
    CHECK(hist.redo(SR));
    CHECK(target->effects.size() == 3 && target->sampler);

    // --- onto an audio track: its effects only ---
    TrackPtr drums = project.addTrack(Kind::Audio);
    said.clear();
    CHECK(presets_ui::loadTrack(nullptr, drums, path));
    CHECK(drums->effects.size() == 3 && !drums->sampler && drums->midiEffects.empty() && drums->modCables.size() == 1);
    CHECK(!said.empty() && said.back().find("for MIDI tracks") != std::string::npos);

    // --- onto no track (dropped below the tracks): a new one, named after it; one undo step ---
    size_t tracks = project.tracks.size();
    hist.check(SR);
    CHECK(presets_ui::loadTrack(nullptr, nullptr, path));
    CHECK(project.tracks.size() == tracks + 1);
    TrackPtr made = project.tracks.size() ? project.tracks[project.tracks.size() - 1] : nullptr;
    CHECK(made && made->name == "Keys" && made->kind == Kind::Midi && made->effects.size() == 3 && made->sampler);
    CHECK(hist.undo(SR) && project.tracks.size() == tracks);

    // --- a plugin that isn't installed: said so, the rest loaded, its slot kept silent with its settings ---
    std::string text;
    util::readFile(path, text);
    Json data = Json::parse(text);
    Json& fxList = data["tracks"].at(0)["effects"];
    fxList.at(1).set("path", Json("/nowhere/Missing Comp.vst3"));
    fxList.at(1).set("name", Json("Missing Comp"));
    std::string broken = util::joinPath(presets::tracksFolder(), "Broken.json");
    CHECK(project_io::writeJson(broken, data, error));
    TrackPtr other = project.addTrack(Kind::Midi);
    said.clear();
    CHECK(presets_ui::loadTrack(nullptr, other, broken));
    CHECK(other->effects.size() == 3);
    if (other->effects.size() == 3) {
        CHECK(aload(other->effects[0]->instance) && !aload(other->effects[1]->instance) &&
              aload(other->effects[2]->instance));
        CHECK(!other->effects[1]->state.empty() && other->sampler);  // (its settings kept; the rest loaded)
    }
    bool told = false;
    for (auto& s : said) told = told || s.find("Missing Comp") != std::string::npos;
    CHECK(told);

    // --- the Explorer's Presets place lists them (not their audio folders) ---
    FileView view(FileView::Mode::Tree);
    view.setRootPath(presets::tracksFolder());
    std::vector<std::string> names;
    for (int i = 0; i < view.rowCount(); i++) names.push_back(view.itemAtRow(i)->name);
    CHECK((names == std::vector<std::string>{"Broken.json", "Keys.json"}));

    // --- renamed (its audio folder too: it still opens with its sample), then deleted ---
    std::string renamed;
    CHECK(presets::rename(path, "Keys 2", renamed, error));
    CHECK(!util::fileExists(path) && !util::isDir(audio) && util::isDir(presets::audioFolderOf(renamed)));
    presets::TrackPreset q;
    CHECK(presets::readTrack(renamed, SR, q, error) && q.chain && q.chain->sampler);
    CHECK(!presets::rename(renamed, "Broken", path, error));  // (taken)
    CHECK(presets::remove(renamed, error) && !util::fileExists(renamed) && !util::isDir(presets::audioFolderOf(renamed)));
    CHECK(presets::list(presets::tracksFolder()).size() == 1);
    // not a track preset
    CHECK(!presets::readTrack(util::joinPath(scratch, "bogus.json"), SR, q, error));
    presets_ui::setHost({});
}

// --- names made safe; an effects-only track preset leaves no audio folder ---
void testNames() {
    CHECK(presets::safeName(" ../a/b ") == "_._a_b");
    CHECK(presets::safeName("..") == "_.");
    CHECK(presets::safeName("   ").empty());
    CHECK(presets::safeName("Lead\tSynth") == "Lead_Synth");
    Project project;
    TrackPtr t = project.addTrack(Kind::Audio);
    SlotPtr eq = loaded("builtin:eq");
    t->effects = std::vector<SlotPtr>{eq};
    t->cables = chain({eq});
    std::string path, error;
    CHECK(presets::saveTrack(*t, SR, "Just EQ", path, error));
    CHECK(!util::isDir(presets::audioFolderOf(path)));
    CHECK(!presets::saveTrack(*t, SR, "  ", path, error) && !error.empty());
    CHECK(presets::remove(util::joinPath(presets::tracksFolder(), "Just EQ.json"), error));
}

// every file and folder under `dir`, then it
void removeAll(const std::string& dir) {
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) removeAll(p);
        else util::removeFile(p);
    }
    ::rmdir(dir.c_str());
}

}  // namespace

int main() {
    scratch = util::absolutePath("build/tests/presets-scratch");
    std::string data = util::joinPath(scratch, "data");
    removeAll(data);  // (every run starts without presets)
    util::makeDirs(scratch);
    paths::setDataDir(data);
    ui::App::get().initHeadless();
    testNames();
    testEffectPresets();
    testCompare();
    testEffectWindow();
    testTrackPresets();
    TEST_MAIN_END();
}
