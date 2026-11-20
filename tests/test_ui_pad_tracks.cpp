// Pads on their own tracks in the windows (model/pad_tracks.h), headless, and in the terminal (tinydaw --tui): the
// Pad Kit window's pad menu (Output ▸ The kit / Its own track, Pads to tracks), the track's instrument menu (Pads
// to tracks), a pad track's row menu (Input: <kit> pad N ▸ Back to the kit; Bounce in place refused), its tooltip,
// and the same by keys: the instrument menu's Pads to tracks, the pad menu (F2) choosing Output, the pad tracks in
// the track panel. Pictures of the screen go to build/ui-samples/tui/.
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/padkit.h"
#include "check.h"
#include "core/util.h"
#include "model/pad_tracks.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/instrument_menu.h"
#include "ui/lane_panel.h"
#include "ui/main_window.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_host.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace tui;

namespace {
const int SR = 48000;
const std::string OUT = "build/ui-samples/tui";

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{120, 40};
    void step(const std::function<bool()>& done) {
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
        }
    }
    void keys(std::vector<Key> ks) {
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
        util::makeDirs(OUT);
        std::ofstream(OUT + "/" + name + ".txt") << screen.lines();
    }
    bool shows(const std::string& text) const { return screen.lines().find(text) != std::string::npos; }
};

void settle() {
    for (int i = 0; i < 6; i++) {
        ui::App::get().advanceTimers(0.05);
        ui::App::get().tick();
    }
}

ui::Action* actionNamed(const ui::Menu& menu, const std::string& prefix) {
    for (auto& a : menu.actions())
        if (a->text.rfind(prefix, 0) == 0) return a.get();
    return nullptr;
}

ui::Window* topWindowBut(ui::Window* main) {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (*it != main && (*it)->isWindowShown()) return *it;
    return nullptr;
}

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

// a kit of three pads (Kick, Snare, Hat), its track named Drums, a clip playing them
TrackPtr drums(MainWindow& w) {
    TrackPtr t = w.timeline->addTrack(Kind::Midi);
    t->name = "Drums";
    t->needsInstrument = false;
    auto kit = std::make_shared<PadKit>();
    const char* names[3] = {"Kick.wav", "Snare.wav", "Hat.wav"};
    for (int i = 0; i < 3; i++) kit->pads[(size_t)i] = padkit::newPad(constant(SR / 10, 0.2f * (i + 1), names[i]), kit->pads[(size_t)i].note);
    t->padKit = kit;
    std::vector<Note> notes;
    for (int i = 0; i < 3; i++) {
        Note n;
        n.start = 0.1 + 0.2 * i, n.end = n.start + 0.1, n.pitch = PadKit::FIRST_NOTE + i, n.velocity = 100;
        notes.push_back(n);
    }
    ClipPtr clip = makeMidiClip("Beat", notes, 1.0, w.engine.sr);
    clip->track = t;
    clip->prepare(w.engine.sr);
    w.project.addClip(clip);
    w.timeline->tracksChanged.emit();
    return t;
}
}  // namespace

static void testWindows() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    TrackPtr kit = drums(*w);
    settle();
    ClipPtr clip = w->project.clipsOn(kit)[0];
    // the pad menu: Output ▸ The kit (ticked) / Its own track, and Pads to tracks
    pad_kit_dialog::PadKitDialog* d = pad_kit_dialog::openPadKit(kit);
    CHECK(d != nullptr);
    if (!d) return;
    ui::Menu menu;
    d->fillPadMenu(menu, 1);
    ui::Action* output = actionNamed(menu, "Output");
    CHECK(output && output->submenu && actionNamed(menu, "Pads to tracks"));
    if (!output || !output->submenu) return;
    ui::Action* inKit = actionNamed(*output->submenu, "The kit");
    ui::Action* own = actionNamed(*output->submenu, "Its own track");
    CHECK(inKit && inKit->checked && own && !own->checked && own->enabled);
    ui::Menu::activate(own);
    settle();
    TrackPtr snare = pad_tracks::trackOf(*kit, 1);
    CHECK(snare && snare->name == "Snare (pad 2)" && snare->kind == Kind::Audio);
    CHECK(kit->group && kit->group->name == "Drums" && snare->group == kit->group);
    CHECK(w->project.indexOf(snare) == w->project.indexOf(kit) + 1);
    CHECK(clip->data && clip->data->pads.size() == 1 && clip->data->pads[0].first == 1);  // (rendered again)
    CHECK(w->panel->meters.count(snare));  // (its row in the panel, with its meter)
    std::vector<std::string> rows;
    int current = 0;
    d->pads[1]->listRows(rows, current);
    CHECK(rows.size() == 2 && rows[1].find("-> Snare (pad 2)") != std::string::npos);
    ui::Menu again;
    d->fillPadMenu(again, 1);
    ui::Action* now = actionNamed(again, "Output");
    CHECK(now && now->submenu && actionNamed(*now->submenu, "Its own track: Snare (pad 2)") &&
          actionNamed(*now->submenu, "Its own track: Snare (pad 2)")->checked);
    // the instrument menu's Pads to tracks: the other two pads
    ui::Menu instruments;
    instrument_menu::fillInstrumentMenu(&instruments, w, *w->plugins, kit, nullptr, nullptr);
    ui::Action* toTracks = actionNamed(instruments, "Pads to tracks");
    CHECK(toTracks != nullptr);
    if (toTracks) ui::Menu::activate(toTracks);
    settle();
    CHECK(pad_tracks::routedMask(*kit) == 7u && pad_tracks::trackOf(*kit, 0)->name == "Kick (pad 1)");
    CHECK(w->project.indexOf(pad_tracks::trackOf(*kit, 2)) == w->project.indexOf(kit) + 3);
    CHECK(clip->data->pads.size() == 3);
    // a pad track's row: Input: Drums pad 2 ▸ Back to the kit; Bounce in place refused
    ui::Menu tm;
    w->panel->fillTrackMenu(tm, snare);
    ui::Action* input = actionNamed(tm, "Input: Drums pad 2");
    CHECK(input && input->submenu && actionNamed(*input->submenu, "Back to the kit"));
    ui::Action* inPlace = actionNamed(tm, "Bounce in place");
    CHECK(inPlace && !inPlace->enabled);
    CHECK(actionNamed(tm, "Freeze track") && actionNamed(tm, "Freeze track")->enabled);  // (its pad from the kit's clip)
    w->history->check(w->engine.sr);  // (the steps so far: one undo step)
    if (input && input->submenu) ui::Menu::activate(actionNamed(*input->submenu, "Back to the kit"));
    settle();
    CHECK(!pad_tracks::trackOf(*kit, 1) && pad_tracks::routedMask(*kit) == 5u && clip->data->pads.size() == 2);
    CHECK(w->project.tracks.contains(snare));  // (the track stays, without its pad)
    // undo puts the pad back on its track (and the kit's rendering as it was)
    w->history->check(w->engine.sr);
    w->undo();
    settle();
    CHECK(pad_tracks::trackOf(*kit, 1) == snare && clip->data->pads.size() == 3);
    // removing the pad track: its pad back in the kit
    w->timeline->removeTrack(snare);
    settle();
    CHECK(!pad_tracks::trackOf(*kit, 1));
    d->close();
    w->close();
}

static void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    TrackPtr kit = drums(*w);
    d.keys({key(plat::Key_Tab)});  // (the track panel)
    f.setCurrent(kit);
    // I: the instrument menu; its fourth entry: Pads to tracks
    d.keys({key('I', 0, "i"), key(plat::Key_Down), key(plat::Key_Down), key(plat::Key_Down)});
    d.picture("pad-tracks-instrument-menu");
    CHECK(d.shows("Pads to tracks"));
    d.keys({key(plat::Key_Return)});
    CHECK(pad_tracks::routedMask(*kit) == 7u);
    while (ui::Window* top = topWindowBut(w)) top->close();
    d.keys({});
    d.picture("pad-tracks-panel");
    CHECK(d.shows("Kick (pad") && d.shows("Snare (pad") && d.shows("Hat (pad 3)"));
    // the Pad Kit window by keys: pad 1's menu (F2) → Output ▸ The kit
    pad_kit_dialog::PadKitDialog* dlg = pad_kit_dialog::openPadKit(kit);
    CHECK(dlg != nullptr);
    if (!dlg) return;
    dlg->pads[0]->setFocus();
    d.keys({});
    d.picture("pad-tracks-pad-kit");
    CHECK(d.shows("C2  -> Kick"));
    // (the pad menu: Load sample…, Clear, Choke group, Output, Pads to tracks: Output's submenu, then The kit)
    // (the menu waits in a loop of its own: looked at there after each key)
    bool sawOutput = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        while (d.at < d.queue.size() && !done()) {
            app.advanceTimers(0.05);
            app.tick();
            f.key(d.queue[d.at++]);
            d.f->draw(d.screen);
            if (!sawOutput && d.shows("The kit") && d.shows("Its own track: Kick (pad 1)") && d.shows("Pads to tracks")) {
                sawOutput = true;
                d.picture("pad-tracks-output-menu");
            }
        }
    };
    d.keys({key(plat::Key_F2), key(plat::Key_Down), key(plat::Key_Down), key(plat::Key_Down), key(plat::Key_Right),
            key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawOutput);
    CHECK(!pad_tracks::trackOf(*kit, 0) && pad_tracks::routedMask(*kit) == 6u);
    while (ui::Window* top = topWindowBut(w)) top->close();
    w->close();
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs("build/test-ui-pad-tracks");
    settings::setPath("build/test-ui-pad-tracks/settings.json");
    testWindows();
    testTerminal();
    TEST_MAIN_END();
}
