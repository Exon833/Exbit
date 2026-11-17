// The Pad Kit's windows, headless: choosing "Pad Kit…" from a MIDI track's instrument menu, the Pad Kit
// window (files dropped on a pad fill it and the pads after it, a pad's right-click menu: choke group and
// Clear, its settings, undo shown again), the MIDI editor's keys naming the pads, and the timeline's "Slice
// to a Pad Kit…" (the slice window, its lines on the clip, the track it makes). Pictures go to
// build/ui-samples/padkit/. No sound: the engine is never opened.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "audio/engine.h"
#include "audio/padkit.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/history.h"
#include "ui/instrument_menu.h"
#include "ui/midi_editor.h"
#include "ui/pad_kit_dialog.h"
#include "ui/plugin_host.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

using namespace ui;

namespace {
const int SR = 48000;
const std::string OUT = "build/ui-samples/padkit";
const std::string TMP = "build/test-tmp/ui-padkit";

void savePicture(Window& w, const std::string& name) {
    w.showWindow();
    App::get().doLayouts();
    w.update();
    App::get().tick();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
}

Action* actionStarting(const Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

// a WAV file of a decaying tone (frames at SR), its path
std::string writeTone(const std::string& name, double hz, int frames) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; i++)
        s[(size_t)i * 2] = s[(size_t)i * 2 + 1] = (float)(0.6 * std::sin(2 * M_PI * hz * i / SR) * std::exp(-i / 6000.0));
    std::string path = util::joinPath(TMP, name), error;
    CHECK(writeWav(path, s.data(), (size_t)frames, 2, SR, SampleFormat::Float32, error));
    return path;
}

// a 2 s drum loop: 4 hits (kick, snare, hi-hat, tom) at these frames
AudioPtr drumLoop(const std::vector<int64_t>& hits) {
    auto a = std::make_shared<AudioBuffer>();
    a->samples.assign((size_t)SR * 2 * 2, 0.0f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (size_t h = 0; h < hits.size(); h++)
        for (int64_t k = 0; k < SR * 2 / 5 && hits[h] + k < SR * 2; k++) {
            double t = (double)k / SR, v;
            if (h == 0) v = 0.9 * std::sin(2 * M_PI * 55 * t) * std::exp(-t * 12);
            else if (h == 1) v = 0.6 * noise(rng) * std::exp(-t * 25);
            else if (h == 2) v = 0.25 * noise(rng) * std::exp(-t * 60);
            else v = 0.7 * std::sin(2 * M_PI * 140 * t) * std::exp(-t * 15);
            v *= std::min(1.0, (double)(SR * 2 / 5 - k) / (SR / 20));
            a->samples[(size_t)(hits[h] + k) * 2] += (float)v;
            a->samples[(size_t)(hits[h] + k) * 2 + 1] += (float)v;
        }
    return a;
}

struct Fixture {
    Project project;
    audio::Engine engine{project};
    Window main;
    TrackPtr track;
    int relayouts = 0;
    Fixture() {
        main.resizeWindow(1000, 700);
        sampler_dialog::Host host;
        host.window = &main;
        host.project = &project;
        host.engine = &engine;
        host.relayout = [this] { relayouts++; };
        sampler_dialog::setHost(host);
        track = project.addTrack(Kind::Midi);
        track->name = "Drums";
    }
};
}  // namespace

static void testChooseAndDrop() {
    Fixture f;
    // a clip on the track, to hear its rendering change
    ClipPtr clip = makeMidiClip("Beat", {{0.0, 0.1, 36, 100}, {0.5, 0.6, 38, 100}}, 1.0, SR);
    clip->track = f.track;
    f.project.addClip(clip);
    // the instrument menu offers the Pad Kit; choosing it gives the track an empty kit and opens its window
    util::makeDirs(TMP + "/plugins");
    PluginHost host(&f.main, f.project, f.engine);
    host.folders = {TMP + "/plugins"};
    host.cachePath = util::joinPath(TMP, "plugin_cache.json");
    host.vst2Folders = [] { return std::vector<std::string>{}; };
    Menu menu;
    int chosen = 0;
    instrument_menu::fillInstrumentMenu(&menu, &f.main, host, f.track, [&] { chosen++; }, nullptr);
    Action* kitAction = actionStarting(menu, instrument_menu::PAD_KIT);
    CHECK(kitAction && !kitAction->checked);
    if (!kitAction) return;
    Menu::activate(kitAction);
    CHECK(chosen == 1 && f.track->padKit && f.track->playsPadKit() && !f.track->needsInstrument);
    CHECK(f.track->padKit && f.track->padKit->used() == 0);
    pad_kit_dialog::PadKitDialog* d = pad_kit_dialog::openFor(f.track.get());
    CHECK(d && d->title() == "Pad Kit \xe2\x80\x94 Drums");
    if (!d) return;
    CHECK(d->pads.size() == 16 && d->bankA->isChecked() && !d->bankB->isChecked());
    Menu again;
    instrument_menu::fillInstrumentMenu(&again, &f.main, host, f.track, nullptr, nullptr);
    Action* ticked = actionStarting(again, "Pad Kit (0 pads)");
    CHECK(ticked && ticked->checked);
    // three files dropped on pad 3: pads 3, 4 and 5 (a file that isn't sound is skipped, said in the hint)
    std::vector<std::string> files{writeTone("kick.wav", 60, 9000), writeTone("snare.wav", 400, 6000),
                                   util::joinPath(TMP, "notes.txt"), writeTone("hat.wav", 3000, 3000)};
    CHECK(util::writeFile(files[2], "not a sound"));
    PadKitPtr before = f.track->padKit;
    DragData data;
    data.files = files;
    data.kind = "files";
    DragEvent drop;
    drop.data = &data;
    d->pads[2]->dragEnter(drop);
    CHECK(drop.accepted);
    d->pads[2]->drop(drop);
    PadKitPtr kit = f.track->padKit;
    CHECK(kit != before && kit->used() == 3);
    CHECK(padkit::padName(kit->pads[2]) == "kick" && padkit::padName(kit->pads[3]) == "snare" &&
          padkit::padName(kit->pads[4]) == "hat");
    CHECK(kit->pads[2].note == 38 && kit->pads[2].sound.oneShot);
    CHECK(d->hint->text().find("notes.txt") != std::string::npos);
    CHECK(d->selected == 2 && d->padTitle->text().find("kick.wav") != std::string::npos);
    // its clips are rendered again a moment later (key 38 now plays the kick)
    AudioPtr silentBefore = clip->data;
    App::get().advanceTimers(0.3);
    CHECK(clip->data != silentBefore && f.relayouts >= 1);
    float loud = 0;
    for (int64_t k = (int64_t)(0.5 * SR); k < (int64_t)(0.6 * SR); k++) loud = std::max(loud, std::fabs(clip->data->samples[(size_t)k * 2]));
    CHECK(loud > 0.1f);
    // the right-click menu: Load sample…, Clear, Choke group ▸ None, 1…8
    Menu padMenu;
    d->fillPadMenu(padMenu, 3);
    CHECK(padMenu.actions().size() == 3 && padMenu.actions()[0]->text == "Load sample\xe2\x80\xa6" &&
          padMenu.actions()[1]->text == "Clear");
    Menu* groups = nullptr;
    for (auto& a : padMenu.actions())
        if (a->text == "Choke group") groups = a->submenu;
    CHECK(groups && groups->actions().size() == 9 && groups->actions()[0]->checked);
    if (groups) Menu::activate(groups->actions()[2].get());  // group 2
    CHECK(f.track->padKit->pads[3].choke == 2);
    d->setChoke(4, 2);
    CHECK(f.track->padKit->pads[4].choke == 2);
    // the selected pad's settings: its key, choke group and the sampler's controls (no root key)
    d->select(3, false);
    CHECK(d->key->intValue() == 39 && d->choke->currentIndex() == 2 && d->controls.root == nullptr);
    CHECK(d->controls.mode->currentIndex() == 1);  // (one-shot)
    d->controls.transpose->setValue(-3);
    CHECK(f.track->padKit->pads[3].sound.transpose == -3);
    d->key->setValue(50);
    CHECK(f.track->padKit->pads[3].note == 50 && f.track->keyLabel(50) == "snare");
    d->select(3, true);  // (played: the track's live notes)
    savePicture(*d, "pad_kit");
    // the second bank
    d->select(20, false);
    CHECK(d->bank == 1 && d->bankB->isChecked() && d->pads[4]->pad() == 20);
    d->showBank(0);
    // Clear: the sample goes, its key and choke group stay
    d->clearPad(4);
    CHECK(f.track->padKit->pads[4].empty() && f.track->padKit->pads[4].choke == 2 && f.track->padKit->pads[4].note == 40);
    // undo puts an earlier kit back: the window shows it again (no change of its own)
    PadKitPtr undone = kit;
    std::atomic_store(&f.track->padKit, undone);
    pad_kit_dialog::reloadAll();
    CHECK(f.track->padKit == undone && d->pads[4]->audio() != nullptr);
    // the MIDI editor names the keys after the pads
    {
        MidiEditor ed(nullptr, clip, f.project, f.engine, [] {});
        CHECK(ed.keyLabel(38) == "kick" && ed.keyLabel(37) == "" && ed.keyLabel(39) == "snare");
        ed.showWindow();
        ed.setRowHeight(14);
        App::get().doLayouts();
        ed.update();
        App::get().tick();
        savePng(OUT + "/midi_editor_pads.png", ed.framebuffer());
        ed.close();
    }
    // the built-in synth again: the kit forgotten, its window closed
    Menu third;
    instrument_menu::fillInstrumentMenu(&third, &f.main, host, f.track, nullptr, nullptr);
    Action* synth = actionStarting(third, instrument_menu::BUILTIN);
    CHECK(synth != nullptr);
    if (synth) Menu::activate(synth);
    CHECK(!f.track->padKit && !f.track->playsPadKit());
    App::get().collectGarbage();
    CHECK(pad_kit_dialog::openFor(f.track.get()) == nullptr);
}

static void testSliceToPadKit() {
    Project project;
    audio::Engine engine(project);
    sampler_dialog::Host host;
    host.project = &project;
    host.engine = &engine;
    sampler_dialog::setHost(host);
    ScrollArea scroll;
    scroll.setHorizontalPolicy(ScrollArea::Policy::AlwaysOn);
    scroll.resize(900, 300);
    Timeline* tl = scroll.makeContent<Timeline>(project, engine, &scroll);
    TrackPtr audio = project.addTrack(Kind::Audio);
    audio->name = "Loop";
    const std::vector<int64_t> hits{SR / 10, SR / 2 + 123, SR + 4321, SR * 3 / 2 + 77};
    ClipPtr loop = makeAudioClip("Drum loop.wav", drumLoop(hits), SR, SR);
    loop->track = audio;
    loop->start = SR;  // (1 s in)
    project.addClip(loop);
    TrackPtr below = project.addTrack(Kind::Audio);
    tl->relayout();
    // the clip's menu offers it for one audio clip
    tl->selection.selectOnly(loop);
    Menu menu;
    tl->fillMenu(menu, std::nullopt);
    Action* slice = actionStarting(menu, "Slice to a Pad Kit");
    CHECK(slice && slice->enabled);
    if (!slice) return;
    Menu::activate(slice);
    pad_kit_dialog::SliceDialog* d = tl->sliceWindow;
    CHECK(d != nullptr && tl->slicePreview && tl->slicePreview->clip == loop);
    if (!d || !tl->slicePreview) return;
    CHECK(d->hits.size() == 4 && tl->slicePreview->frames == d->hits && d->count->text() == "4 slices");
    for (size_t i = 0; i < std::min<size_t>(4, d->hits.size()); i++) CHECK(std::llabs(d->hits[i] - hits[i]) <= 24);
    // the lines on the clip, while the window is open
    App::get().doLayouts();
    Image img(std::max(1, tl->width()), std::max(1, tl->height()), theme::c("window"));
    {
        Painter p(img);
        tl->paintTree(p, tl->rect());
    }
    savePng(OUT + "/slice_lines.png", img);
    auto rect = tl->clipRect(*loop, tl->trackTops());
    CHECK(rect.has_value());
    if (rect) {
        int x = (int)std::lround(tl->xOf(loop->start + d->hits[1]));
        int y = (int)(rect->y + rect->h * 0.7);
        CHECK(img.colorAt(x, y) == theme::c("selected_note"));
    }
    // less sensitive: fewer slices, the lines follow
    d->sensitivity->setValue(0);
    CHECK(d->hits.size() < 4 && tl->slicePreview->frames == d->hits);
    d->sensitivity->setValue(50);
    savePicture(*d, "slice_window");
    // Slice: a new MIDI track right under the clip's, a kit with a slice per pad, a clip replaying them
    size_t tracksBefore = project.tracks.size();
    d->accept();
    CHECK(!tl->sliceWindow && !tl->slicePreview);
    CHECK(project.tracks.size() == tracksBefore + 1);
    TrackPtr made = project.tracks.size() > 1 ? project.tracks[1] : nullptr;
    CHECK(made && made != below && made->kind == Kind::Midi && made->name == "Drum loop kit");
    if (!made) return;
    CHECK(made->playsPadKit() && made->padKit->used() == 4 && !made->needsInstrument);
    std::vector<ClipPtr> clips = project.clipsOn(made);
    CHECK(clips.size() == 1);
    if (clips.size() != 1) return;
    ClipPtr mc = clips[0];
    CHECK(mc->start == loop->start && mc->frames() == loop->frames() && mc->notes->size() == 4);
    CHECK(made->keyLabel(36) == "Drum loop slice 1" && made->keyLabel(39) == "Drum loop slice 4");
    // its sound replays the loop (from the first hit on)
    double xy = 0, xx = 0, yy = 0;
    for (int64_t k = hits[0]; k < loop->frames(); k++) {
        double x = loop->data->samples[(size_t)k * 2], y = mc->data->samples[(size_t)k * 2];
        xy += x * y, xx += x * x, yy += y * y;
    }
    CHECK(xy / std::sqrt(xx * yy + 1e-30) > 0.99);
    // Cancel leaves everything as it was
    tl->selection.selectOnly(loop);
    pad_kit_dialog::SliceDialog* d2 = tl->sliceToPadKit(loop);
    CHECK(d2 && tl->slicePreview);
    if (d2) d2->reject();
    CHECK(!tl->slicePreview && project.tracks.size() == tracksBefore + 1);
    App::get().collectGarbage();
}

int main() {
    App::get().initHeadless();
    theme::setSkin();
    util::makeDirs(OUT);
    util::makeDirs(TMP);
    settings::setPath(util::joinPath(TMP, "settings.json"));
    testChooseAndDrop();
    testSliceToPadKit();
    App::get().collectGarbage();
    TEST_MAIN_END();
}
