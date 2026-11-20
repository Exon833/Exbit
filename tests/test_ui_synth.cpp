// The built-in synth's window, headless: "Synth…" in a MIDI track's instrument menu, drag-to-modulate (a ⊕ dropped
// on a knob, its ring dragged to change the depth), knobs dragged, presets chosen (and the classic voice back),
// the Modulation view, undo shown again; then in the terminal (tinydaw --tui): the page as a list, Left/Right on
// a control, M and the Modulate… dialog, the hints and the F1 list. Pictures go to build/ui-samples/synth/.
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "paths.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/instrument_menu.h"
#include "ui/main_window.h"
#include "ui/plugin_host.h"
#include "ui/sampler_dialog.h"
#include "ui/synth_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

using namespace subsynth;

namespace {
const int SR = 48000;
const std::string OUT = "build/ui-samples/synth";
const std::string TMP = "build/test-tmp/ui-synth";

void savePicture(ui::Window& w, const std::string& name) {
    w.showWindow();
    ui::App::get().doLayouts();
    w.update();
    ui::App::get().tick();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
}

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

ui::MouseEvent mouse(Point at, int mods = 0) {
    ui::MouseEvent e;
    e.pos = at;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    e.mods = mods;
    return e;
}

// drag on the page from a to b (in steps, as the pointer moves)
void drag(synth_dialog::SynthPage* page, Point a, Point b) {
    ui::MouseEvent e = mouse(a);
    page->mousePress(e);
    for (int i = 1; i <= 8; i++) {
        ui::MouseEvent m = mouse(Point(a.x + (b.x - a.x) * i / 8, a.y + (b.y - a.y) * i / 8));
        page->mouseMove(m);
    }
    ui::MouseEvent r = mouse(b);
    page->mouseRelease(r);
}
}  // namespace

static void testWindow() {
    Project project;
    audio::Engine engine{project};
    ui::Window main;
    main.resizeWindow(1000, 700);
    sampler_dialog::Host h;
    h.window = &main;
    h.project = &project;
    h.engine = &engine;
    sampler_dialog::setHost(h);
    TrackPtr track = project.addTrack(Kind::Midi);
    track->name = "Lead";
    track->needsInstrument = false;
    ClipPtr clip = makeMidiClip("Tune", {{0.0, 0.5, 60, 100}, {0.5, 1.0, 64, 100}}, 1.0, SR);
    clip->track = track;
    project.addClip(clip);
    clip->prepare(SR);
    AudioPtr classic = clip->data;
    history::History hist(project, SR);

    // the instrument menu: "Synth…" while it plays the built-in synth; it opens the window
    util::makeDirs(TMP + "/plugins");
    PluginHost host(&main, project, engine);
    host.folders = {TMP + "/plugins"};
    host.cachePath = util::joinPath(TMP, "plugin_cache.json");
    host.vst2Folders = [] { return std::vector<std::string>{}; };
    ui::Menu menu;
    instrument_menu::fillInstrumentMenu(&menu, &main, host, track, nullptr, nullptr);
    ui::Action* synthAction = actionStarting(menu, instrument_menu::SYNTH);
    CHECK(synthAction != nullptr);
    if (!synthAction) return;
    ui::Menu::activate(synthAction);
    synth_dialog::SynthDialog* d = synth_dialog::openFor(track.get());
    CHECK(d && d->title() == "Synth \xe2\x80\x94 Lead");
    if (!d) return;
    ui::App::get().doLayouts();
    CHECK(!track->synth && d->classicNote->isVisible());  // (the classic voice until something changes)
    savePicture(*d, "classic");
    synth_dialog::SynthPage* page = d->page;

    // the LFO's ⊕ dragged onto Cutoff: LFO → Cutoff at MOD_START, the track plays the patch, its clip rendered again
    drag(page, page->handleOf(LFO).center(), page->knobCentre(CUTOFF));
    PatchPtr p = track->synth;
    CHECK(p && p->modOf(LFO, CUTOFF) && std::fabs(p->modOf(LFO, CUTOFF)->depth - synth_dialog::MOD_START) < 1e-9);
    CHECK(!d->classicNote->isVisible());
    CHECK(clip->data != classic && clip->data->samples != classic->samples);
    CHECK(hist.check(SR));
    // dropped where nothing can be modulated (the wave's choice, empty space): nothing
    drag(page, page->handleOf(ENV2).center(), page->knobCentre(O1_WAVE));
    drag(page, page->handleOf(ENV2).center(), Point(2, 2));
    CHECK(track->synth->mods.size() == 1);
    // Env 2 onto Cutoff too: a second ring
    drag(page, page->handleOf(ENV2).center(), page->knobCentre(CUTOFF));
    CHECK(track->synth->mods.size() == 2 && track->synth->modOf(ENV2, CUTOFF));
    // the LFO's ring dragged up 50 px: its depth up by 2 × 50 / DRAG_PIXELS
    Point c = page->knobCentre(CUTOFF);
    Point ring(c.x, c.y - page->ringRadius(LFO));
    int param = -1;
    CHECK(page->ringAt(ring, param) == LFO && param == CUTOFF);
    drag(page, ring, Point(ring.x, ring.y - 50));
    CHECK(std::fabs(track->synth->modOf(LFO, CUTOFF)->depth - (synth_dialog::MOD_START + 0.5)) < 0.02);
    CHECK(track->synth->modOf(ENV2, CUTOFF)->depth == synth_dialog::MOD_START);
    CHECK(hist.check(SR));
    // a knob dragged: Resonance up; double-click: its default again
    double before = track->synth->v[RESO];
    drag(page, page->knobCentre(RESO), Point(page->knobCentre(RESO).x, page->knobCentre(RESO).y - 100));
    CHECK(std::fabs(track->synth->v[RESO] - (before + 0.5)) < 0.02);
    ui::MouseEvent dbl = mouse(page->knobCentre(RESO));
    page->mouseDoubleClick(dbl);
    CHECK(track->synth->v[RESO] == info(RESO).def);
    ui::MouseEvent up = mouse(page->knobCentre(RESO));
    page->mouseRelease(up);
    drag(page, page->knobCentre(RESO), Point(page->knobCentre(RESO).x, page->knobCentre(RESO).y - 40));
    CHECK(hist.check(SR));
    savePicture(*d, "modulated");

    // undo: the depth as it was, shown in the window again
    PatchPtr dragged = track->synth;
    CHECK(hist.undo(SR) && hist.undo(SR));
    synth_dialog::reloadAll();
    CHECK(track->synth && std::fabs(track->synth->modOf(LFO, CUTOFF)->depth - synth_dialog::MOD_START) < 1e-9);
    CHECK(page->patch == *track->synth);
    CHECK(hist.redo(SR) && hist.redo(SR));
    synth_dialog::reloadAll();
    CHECK(*track->synth == *dragged);

    // the Modulation view: a row per routing; Remove takes the selected one away
    d->showModulation(true);
    CHECK(d->modList->count() == 2 && !page->isVisible() && d->modPanel->isVisible());
    CHECK(d->modList->item(0).text.find("LFO") != std::string::npos);
    savePicture(*d, "modulation");
    d->modList->setCurrentRow(1);
    d->removeMod->click();
    CHECK(track->synth->mods.size() == 1 && d->modList->count() == 1);
    d->showModulation(false);

    // presets: a shipped one by category; "Init (classic)" back to the classic voice, bit for bit
    ui::Menu presets;
    d->presetsMenu.aboutToShow(&presets);
    ui::Action* bass = actionStarting(presets, "Bass");
    CHECK(bass && bass->submenu);
    if (bass && bass->submenu) {
        ui::Action* acid = actionStarting(*bass->submenu, "Acid Bass");
        CHECK(acid != nullptr);
        if (acid) ui::Menu::activate(acid);
    }
    CHECK(track->synth && track->synth->name == "Acid Bass" && page->patch.name == "Acid Bass");
    CHECK(d->presetName->text().find("Acid Bass") != std::string::npos);
    ui::Menu again;
    d->presetsMenu.aboutToShow(&again);
    ui::Action* classicAction = actionStarting(again, "Init (classic)");
    CHECK(classicAction && !classicAction->checked);
    if (classicAction) ui::Menu::activate(classicAction);
    CHECK(!track->synth && clip->data->samples == classic->samples && d->classicNote->isVisible());
    // another instrument chosen: the window closes (the patch kept)
    std::atomic_store(&track->synth, PatchPtr(std::make_shared<const Patch>()));
    track->sampler = std::make_shared<SamplerSettings>();
    synth_dialog::closeGone(project);
    ui::App::get().collectGarbage();
    CHECK(!synth_dialog::openFor(track.get()) && track->synth);
}

// --- the terminal ---

namespace {
tui::Key key(int k, int mods = 0, std::string text = "") { return tui::Key{k, mods, std::move(text)}; }
struct Driver {
    tui::Front* f;
    std::vector<tui::Key> queue;
    size_t at = 0;
    tui::Screen screen{120, 40};
    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    void keys(std::vector<tui::Key> ks) {
        queue = std::move(ks);
        at = 0;
        step([] { return false; });
        for (int i = 0; i < 6; i++) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
        }
        f->draw(screen);
    }
    void picture(const std::string& name) {
        f->draw(screen);
        std::ofstream(OUT + "/" + name + ".txt") << screen.lines();
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
};
}  // namespace

static void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    tui::Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    // a MIDI track (the track panel's N), playing the built-in synth: Shift+I opens its window
    d.keys({key(plat::Key_Tab), key('N', 0, "n")});
    TrackPtr midi = w->timeline->currentTrack;
    CHECK(midi && midi->kind == Kind::Midi);
    if (!midi) return;
    midi->needsInstrument = false;
    f.setCurrent(midi);
    d.keys({key('I', plat::ModShift, "I")});
    synth_dialog::SynthDialog* dlg = synth_dialog::openFor(midi.get());
    CHECK(dlg != nullptr);
    if (!dlg) return;
    CHECK(d.shows("Synth") && d.shows("Osc 1  Wave") && d.shows("Saw") && d.shows("Presets"));
    CHECK(d.shows("M: Modulate"));
    d.picture("page");
    // Down to Cutoff, Right: a step brighter (the track now plays the patch)
    dlg->page->setFocus();
    std::vector<tui::Key> down((size_t)CUTOFF, key(plat::Key_Down));
    d.keys(down);
    CHECK(dlg->page->current == CUTOFF);
    d.keys({key(plat::Key_Right)});
    CHECK(midi->synth && midi->synth->v[CUTOFF] > info(CUTOFF).def);
    // M: Modulate… for it, drawn as text; Down picks the LFO as the source, Enter: OK
    bool sawDialog = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        if (!sawDialog) {
            d.f->draw(d.screen);
            sawDialog = d.shows("Modulate") && d.shows("Source") && d.shows("Depth") && d.shows("Filter Cutoff");
            d.picture("modulate");
        }
        d.step(done);
    };
    d.keys({key('M', 0, "m"), key(plat::Key_Tab, plat::ModShift), key(plat::Key_Tab, plat::ModShift),
            key(plat::Key_Down), key(plat::Key_Down), key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawDialog);
    CHECK(midi->synth && midi->synth->mods.size() == 1);
    if (midi->synth && midi->synth->mods.size() == 1) {
        CHECK(midi->synth->mods[0].target == CUTOFF && midi->synth->mods[0].depth == synth_dialog::MOD_START);
        printf("Modulate… in the terminal: %s -> %s\n", sourceName(midi->synth->mods[0].source),
               info(midi->synth->mods[0].target).key);
    }
    d.keys({});
    CHECK(d.shows("~"));  // (the list shows the routing on Cutoff's row)
    d.picture("page-modulated");
    // the F1 list names the synth's keys
    d.keys({key(plat::Key_F12), key(plat::Key_F1)});
    std::string seen;
    for (int i = 0; i < 10; i++) {
        seen += d.screen.lines();
        d.keys({key(plat::Key_PageDown)});
    }
    CHECK(seen.find("The built-in synth") != std::string::npos && seen.find("Modulate") != std::string::npos);
    d.keys({key(plat::Key_Escape), key(plat::Key_F12), key('W', plat::ModCtrl)});
    w->close();
    app.collectGarbage();
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    util::makeDirs(TMP);
    settings::setPath(util::joinPath(TMP, "settings.json"));
    paths::setDataDir(util::absolutePath(TMP + "/data"));
    setShippedFolder(util::absolutePath("data/presets/synth"));
    testWindow();
    testTerminal();
    TEST_MAIN_END();
}
