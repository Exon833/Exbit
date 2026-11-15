// The dialogs and skins (headless): skin files (parse, validate, write, the library), the colour
// picker (wheel mapping, recent colours, tones), the skin editor, the metronome settings, Audio
// properties, recording helpers (input choices, count-in, take placement), freezing / bouncing
// and rendering the mix to a file. Pictures of every dialog go to build/ui-samples/dialogs/.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "audio/engine.h"
#include "audio/sound_file.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/model.h"
#include "ui/audio_dialog.h"
#include "ui/color_picker.h"
#include "ui/colors.h"
#include "ui/metronome_dialog.h"
#include "ui/mix_export.h"
#include "ui/record_offset_dialog.h"
#include "ui/recording.h"
#include "ui/skin.h"
#include "ui/skin_dialog.h"
#include "ui/theme.h"
#include "ui/track_render.h"
#include "ui/toolkit/menu.h"
#include "ui/wine_prefixes_dialog.h"
#include "ui/wine_setup.h"

using namespace ui;

namespace {
const std::string OUT = "build/ui-samples/dialogs";
const std::string TMP = "build/ui-samples/dialogs/tmp";

void snap(Window& w, const std::string& name) {  // (shown already)
    App::get().doLayouts();
    w.update();
    App::get().tick();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
}

void picture(Window& w, const std::string& name) {
    w.showWindow();
    App::get().doLayouts();
    w.update();
    App::get().tick();
    savePng(OUT + "/" + name + ".png", w.framebuffer());
    w.hideWindow();
}

plat::Event mouseEv(plat::Event::Type t, int x, int y, int button, int buttons) {
    static double time = 100;
    plat::Event e;
    e.type = t;
    e.x = x;
    e.y = y;
    e.button = button;
    e.buttons = buttons;
    e.time = (time += 1.0);
    return e;
}

ClipPtr audioClipOf(int64_t start, int64_t frames, float value, int sr) {
    auto src = std::make_shared<AudioBuffer>();
    src->samples.assign((size_t)frames * 2, value);
    ClipPtr c = makeAudioClip("c", src, sr, sr);
    c->start = start;
    return c;
}
}  // namespace

// ------------------------------------------------------------------------------------------
static void testSkinFiles() {
    skin::setBuiltinDir("data/skins");
    skin::setUserDir(TMP + "/skins");
    // the shipped skins read without problems, and write back the same
    for (const char* name : {"dark", "light", "midnight", "oled"}) {
        skin::Skin s;
        std::vector<std::string> problems;
        std::string err;
        CHECK(skin::read(std::string("data/skins/") + name + ".json", s, problems, err, true));
        CHECK(problems.empty());
        CHECK(!s.name.empty() && s.builtin);
        std::string path = TMP + "/roundtrip-" + name + ".json";
        CHECK(skin::write(s, path));
        skin::Skin back;
        CHECK(skin::read(path, back, problems, err));
        CHECK(problems.empty());
        CHECK(back.name == s.name);
        CHECK(back.completeColors() == s.completeColors());
        CHECK(back.fonts.ui == s.fonts.ui && back.fonts.uiSize == s.fonts.uiSize && back.fonts.mono == s.fonts.mono);
        // written complete: every colour of theme
        CHECK(back.colors.size() == theme::colors().size());
        Json j = skin::toJson(s);
        CHECK(j["format"].toString() == "tinydaw-skin" && j["version"].toInt() == 1);
    }
    // the library: the program's own first, Dark first of them
    std::vector<skin::Skin> lib = skin::library();
    CHECK(lib.size() >= 4);
    CHECK(!lib.empty() && lib[0].name == "Dark" && lib[0].builtin);
    // problems: unknown names and values that aren't colours are left out, with a message
    util::writeFile(TMP + "/odd.json",
                    R"({"format": "tinydaw-skin", "name": "  Odd one ", "colors": {"lane": "#ABCDEF", "nope": "#000000",)"
                    R"( "playhead": "red", "cursor": 12, "loop": "#11223344"}, "fonts": {"ui_size": 99, "mono": 3, "x": 1}})");
    skin::Skin odd;
    std::vector<std::string> problems;
    std::string err;
    CHECK(skin::read(TMP + "/odd.json", odd, problems, err));
    CHECK(odd.name == "Odd one");
    CHECK(odd.colors.size() == 2 && odd.colors["lane"] == "#abcdef" && odd.colors["loop"] == "#11223344");
    CHECK(problems.size() == 6);
    bool sawUnknown = false, sawRed = false, sawNumber = false, sawSize = false, sawMono = false, sawFont = false;
    for (auto& p : problems) {
        sawUnknown = sawUnknown || p == "unknown colour name: nope";
        sawRed = sawRed || p == "playhead: not a colour ('red'; use \"#rrggbb\" or \"#rrggbbaa\")";
        sawNumber = sawNumber || p == "cursor: not a colour (12; use \"#rrggbb\" or \"#rrggbbaa\")";
        sawSize = sawSize || p == "ui_size: not a size in points (99)";
        sawMono = sawMono || p == "mono: not a font name (3)";
        sawFont = sawFont || p == "unknown font setting: x";
    }
    CHECK(sawUnknown && sawRed && sawNumber && sawSize && sawMono && sawFont);
    CHECK(odd.color("playhead") == "#ff5050");  // (the default)
    // not a skin / not JSON
    util::writeFile(TMP + "/notskin.json", R"({"name": "x"})");
    CHECK(!skin::read(TMP + "/notskin.json", odd, problems, err));
    CHECK(err == "Not a Tiny DAW skin (it needs \"format\": \"tinydaw-skin\").");
    util::writeFile(TMP + "/broken.json", "{\"format\": ");
    CHECK(!skin::read(TMP + "/broken.json", odd, problems, err));
    CHECK(util::startsWith(err, "Not valid JSON: "));
    // no name: the file's
    util::writeFile(TMP + "/Nameless.json", R"({"format": "tinydaw-skin"})");
    CHECK(skin::read(TMP + "/Nameless.json", odd, problems, err) && odd.name == "Nameless");
    // file names
    CHECK(skin::fileName("My skin") == "My skin.json");
    CHECK(skin::fileName("a/b:c?") == "a_b_c_.json");
    CHECK(skin::fileName("  ") == "skin.json");
    CHECK(skin::fileName("Dark (mine)") == "Dark _mine_.json");
    // yours first, then the program's; unknown -> Dark
    skin::Skin mine = skin::find("Midnight");
    mine.builtin = false;
    mine.colors["lane"] = "#010203";
    CHECK(skin::write(mine, skin::userDir() + "/Midnight.json"));
    skin::Skin found = skin::find("Midnight");
    CHECK(!found.builtin && found.color("lane") == "#010203");
    CHECK(skin::find("No such skin").name == "Dark");
    lib = skin::library();
    CHECK(lib.back().name == "Midnight" && !lib.back().builtin);
    util::removeFile(skin::userDir() + "/Midnight.json");
    // apply: the theme's colours, the hooks
    int hooked = 0;
    int hook = skin::onApply([&] { hooked++; });
    skin::apply(skin::find("Light"));
    CHECK(hooked == 1);
    skin::removeOnApply(hook);
    CHECK(theme::c("window") == Color::fromHex(skin::find("Light").color("window")));
    skin::apply(skin::find("Dark"));
}

// ------------------------------------------------------------------------------------------
static void testColorWheel() {
    double h, s;
    color_picker::hueSatAt(200, 100, 100, 100, 98, h, s);  // right: red, at the rim
    CHECK_NEAR(h, 0.0, 1e-9);
    CHECK_NEAR(s, 1.0, 1e-9);
    color_picker::hueSatAt(100, 51, 100, 100, 98, h, s);   // above: a quarter turn
    CHECK_NEAR(h, 0.25, 1e-9);
    CHECK_NEAR(s, 0.5, 1e-9);
    color_picker::hueSatAt(51, 100, 100, 100, 98, h, s);   // left
    CHECK_NEAR(h, 0.5, 1e-9);
    color_picker::hueSatAt(100, 149, 100, 100, 98, h, s);  // below
    CHECK_NEAR(h, 0.75, 1e-9);
    color_picker::hueSatAt(100, 100, 100, 100, 98, h, s);  // centre: no saturation
    CHECK_NEAR(s, 0.0, 1e-9);
    ColorWheel w;
    w.resize(200, 200);
    w.pick(PointF(199, 100));
    CHECK(w.color().hexRgb() == "#ff0000");
    w.setColor(Color::fromHex("#00ff00"));
    CHECK_NEAR(w.h, 1 / 3.0, 1e-3);
    CHECK_NEAR(w.s, 1.0, 1e-6);
    w.setColor(Color::fromHex("#808080"));  // grey: hue 0
    CHECK(w.h == 0 && w.s == 0);
    // the wheel's image: red at the right rim, white in the middle, nothing outside
    w.setColor(Color::fromHex("#ffffff"));
    const Image& img = w.wheelImage();
    Color right = img.colorAt(196, 100), mid = img.colorAt(100, 100);
    CHECK(right.r > 240 && right.g < 20 && right.b < 20);
    CHECK(mid.r > 245 && mid.g > 245 && mid.b > 245);
    CHECK(img.colorAt(1, 1).a == 0);
    std::string hex;
    CHECK(color_picker::parseHex("#ABC", hex) && hex == "#aabbcc");
    CHECK(color_picker::parseHex("#12ab3F", hex) && hex == "#12ab3f");
    CHECK(!color_picker::parseHex("12ab3f", hex) && !color_picker::parseHex("#12ab3", hex));
}

static void testRecentColours() {
    settings::remove("recent_clip_colors");
    CHECK(color_picker::loadRecent().empty());
    for (int i = 0; i < 12; i++) color_picker::remember(util::format("#0000%02x", i));
    std::vector<std::string> r = color_picker::loadRecent();
    CHECK(r.size() == 10);
    CHECK(r.front() == "#00000b" && r.back() == "#000002");
    color_picker::remember("#000005");  // again: to the front, once
    r = color_picker::loadRecent();
    CHECK(r.size() == 10 && r.front() == "#000005");
    int fives = 0;
    for (auto& c : r) fives += c == "#000005";
    CHECK(fives == 1);
    settings::set("recent_clip_colors", Json("#123456"));  // a one-item list stored as a string
    r = color_picker::loadRecent();
    CHECK(r.size() == 1 && r[0] == "#123456");
}

static void testColorPickerDialog() {
    settings::remove("recent_clip_colors");
    {
        ColorPickerDialog d(nullptr, "#326996", "", ColorSubject::Clip);
        CHECK(d.title() == "Clip colour");
        CHECK(d.chosen == "#326996" && d.hexEdit->text() == "#326996");
        CHECK(d.toneButtons.size() == 3 && d.toneButtons[0]->isChecked());
        CHECK(d.recentButtons.empty());
        picture(d, "color-picker-clip-empty-recent");
        // a tone that fits is kept; the brightness follows the colour
        d.setTone("light");
        bool lightOk = colors::toneAvailable("#326996", "light");
        CHECK(d.tone == (lightOk ? "light" : ""));
        CHECK(d.brightness->value() == (int)std::lround(0x96 / 255.0 * 100));
        // a very light colour: White isn't readable on it, so it shows as Auto
        d.setColor(Color::fromHex("#f0f0e0"));
        CHECK(!d.toneButtons[1]->isEnabled());
        CHECK(d.tone.empty() && d.toneButtons[0]->isChecked());
        CHECK(d.toneButtons[1]->toolTip() == "Not enough contrast on this colour");
        // …and comes back on a dark one
        d.setColor(Color::fromHex("#202040"));
        CHECK(d.toneButtons[1]->isEnabled() && d.tone == "light" && d.toneButtons[1]->isChecked());
        // the wheel moves the colour
        d.wheel->pick(PointF(199, 100));
        CHECK(d.chosen == d.wheel->color().hexRgb() && d.hexEdit->text() == d.chosen);
        // OK remembers it
        d.accept();
        CHECK(color_picker::loadRecent().size() == 1 && color_picker::loadRecent()[0] == d.chosen);
    }
    {
        color_picker::remember("#c0392b");
        color_picker::remember("#5abe78");
        ColorPickerDialog d(nullptr, "#d4a017", "dark", ColorSubject::Track);
        CHECK(d.title() == "Track colour");
        CHECK(d.recentButtons.size() == 3);
        CHECK(d.tone == "dark");
        picture(d, "color-picker-track");
        // a swatch sets its colour
        d.recentButtons[0]->click();
        CHECK(d.chosen == "#5abe78");
        // Default: none, not remembered
        size_t before = color_picker::loadRecent().size();
        d.useDefault();
        CHECK(d.result() == Dialog::Accepted && d.chosen.empty() && d.tone.empty());
        CHECK(color_picker::loadRecent().size() == before);
    }
    {
        ColorPickerDialog d(nullptr, "#3d7ab8", "", ColorSubject::Group);
        CHECK(d.title() == "Group colour");
        CHECK(d.defaultButton->toolTip() == "Back to the standard look of group rows");
        picture(d, "color-picker-group");
    }
}

// ------------------------------------------------------------------------------------------
static void testSkinDialog() {
    skin::Skin dark = skin::find("Dark");
    int applied = 0, savedCount = 0;
    skin::Skin lastSaved;
    {
        SkinDialog d(nullptr, dark, [&](const skin::Skin&) { applied++; },
                     [&](const skin::Skin& s) { savedCount++, lastSaved = s; }, "Tracks and clips");
        CHECK(d.title() == "Customize skin");
        CHECK(d.tabs->tabText(d.tabs->currentIndex()) == "Tracks and clips");
        CHECK(d.tabs->count() == 7);  // Window, Timeline, Tracks and clips, Meters, MIDI editor, Explorer, Fonts
        CHECK(d.tabs->tabText(d.tabs->count() - 1) == "Fonts");
        CHECK(d.buttons.size() == theme::colors().size());
        CHECK(!d.saveButton->isEnabled());
        CHECK(d.saveButton->toolTip() == "The program's own skins can't be changed: use Save as new skin");
        picture(d, "skin-dialog-tracks");
        d.tabs->setCurrentIndex(0);
        picture(d, "skin-dialog-window");
        d.tabs->setCurrentIndex(d.tabs->count() - 1);
        picture(d, "skin-dialog-fonts");
        CHECK(d.systemFont->isChecked() && !d.uiFont->isEnabled());
        CHECK(d.uiSize->text() == "The system's own");
        // a change shows at once, and is unsaved (a builtin skin: Save stays off)
        d.setColor("lane", "#102030");
        CHECK(applied == 1 && d.dirty && d.working.colors["lane"] == "#102030");
        CHECK(!d.saveButton->isEnabled());
        d.uiSize->setValue(10);
        CHECK(applied == 2 && d.working.fonts.uiSize == 10);
        CHECK(d.current().name == "Dark");
        d.revert();
        CHECK(!d.dirty && d.working.colors["lane"] == dark.color("lane") && d.working.fonts.uiSize == 0);
        CHECK(applied == 3);
    }
    {
        // your own skin: saved in place
        skin::Skin mine = dark;
        mine.name = "Mine";
        mine.builtin = false;
        mine.path = skin::userDir() + "/Mine.json";
        CHECK(skin::write(mine, mine.path));
        SkinDialog d(nullptr, mine, [&](const skin::Skin&) {}, [&](const skin::Skin& s) { savedCount++, lastSaved = s; });
        d.setColor("playhead", "#00ff0080");
        CHECK(d.saveButton->isEnabled());
        d.nameEdit->setText("Mine too");
        CHECK(d.save());
        CHECK(savedCount == 1 && lastSaved.name == "Mine too" && !d.dirty);
        skin::Skin back;
        std::vector<std::string> problems;
        std::string err;
        CHECK(skin::read(mine.path, back, problems, err));
        CHECK(back.name == "Mine too" && back.color("playhead") == "#00ff0080");
        d.reject();  // nothing unsaved: closes without asking
        CHECK(d.result() == Dialog::Rejected);
        util::removeFile(mine.path);
    }
    // the colour chooser of one colour (with opacity for see-through ones)
    {
        CHECK(skin_dialog::offersTransparency("selection_fill") && !skin_dialog::offersTransparency("lane"));
        SkinColorDialog c(nullptr, "Selected range", "#78aae62d", true);
        CHECK(c.opacity && c.opacity->value() == 0x2d);
        CHECK(c.value == "#78aae62d");
        picture(c, "skin-color-chooser");
        c.opacity->setValue(255);
        CHECK(c.value == "#78aae6");
        c.hexEdit->setText("#11223380");
        c.hexEdit->editingFinished.emit();
        CHECK(c.value == "#11223380" && c.opacity->value() == 0x80);
        SkinColorDialog solid(nullptr, "Track lane", "#232529", false);
        CHECK(!solid.opacity);
        solid.hexEdit->setText("#11223380");  // no transparency here
        solid.hexEdit->editingFinished.emit();
        CHECK(solid.value == "#232529");
    }
}

// ------------------------------------------------------------------------------------------
static void testMetronome() {
    namespace m = metronome;
    m::MetronomeSettings s;
    s.bar.pitch = 2000, s.bar.waveform = "square", s.beat.releaseMs = 120, s.beat.levelDb = -12.5;
    Json j = metronome_dialog::toJson(s);
    CHECK(j["bar"]["pitch"].toDouble() == 2000 && j["bar"]["waveform"].toString() == "square");
    CHECK(j["beat"]["release_ms"].toDouble() == 120 && j["beat"].has("attack_ms") && j["beat"].has("level_db"));
    CHECK(metronome_dialog::fromJson(j) == s);
    CHECK(metronome_dialog::fromJson(Json(j.dump())) == s);  // as the Python program stored it: a JSON string
    metronome_dialog::saveSettings(s);
    CHECK(metronome_dialog::loadSettings() == s);
    // missing: defaults; out of range: limited; invalid: all defaults
    CHECK(metronome_dialog::fromJson(Json()) == m::MetronomeSettings());
    Json wild = Json::parse(R"({"bar": {"pitch": 99999, "attack_ms": -3, "waveform": "saw"}})");
    m::MetronomeSettings w = metronome_dialog::fromJson(wild);
    CHECK(w.bar.pitch == m::MAX_PITCH && w.bar.attackMs == 0 && w.bar.waveform == "sine");
    CHECK(w.bar.levelDb == m::MetronomeSettings::barDefault().levelDb && w.beat == m::Click());
    CHECK(metronome_dialog::fromJson(Json::parse(R"({"bar": {"pitch": "loud"}})")) == m::MetronomeSettings());
    CHECK(metronome_dialog::GRAPH_MS == 500);

    // the dialog: changes apply at once and are saved
    m::MetronomeSettings live;
    int applied = 0;
    std::vector<m::Click> tested;
    MetronomeDialog d(nullptr, live, [&](const m::MetronomeSettings&) { applied++; },
                      [&](const m::Click& c) { tested.push_back(c); });
    CHECK(d.title() == "Metronome" && d.editors.size() == 2);
    picture(d, "metronome");
    ClickEditor* bar = d.editors[0];
    bar->boxes["pitch"]->setValue(1234);
    CHECK(live.bar.pitch == 1234 && applied == 1);
    CHECK(metronome_dialog::loadSettings().bar.pitch == 1234);
    bar->waveform->setCurrentIndex(3);  // Noise: no pitch
    CHECK(live.bar.waveform == "noise" && !bar->boxes["pitch"]->isEnabled());
    CHECK(bar->waveform->itemText(0) == "Sine");
    bar->testButton->click();
    CHECK(tested.size() == 1 && tested[0].waveform == "noise");
    // dragging the attack point (in the dialog's window, through its events)
    d.showWindow();
    App::get().doLayouts();
    EnvelopeGraph* g = bar->graph;
    std::vector<PointF> pts = g->points();
    Point at = g->mapToWindow(Point((int)pts[1].x, (int)pts[1].y));
    Point to = g->mapToWindow(Point((int)g->xOf(50), (int)g->yOf(-20)));
    d.onEvent(nullptr, mouseEv(plat::Event::MouseMove, at.x, at.y, 0, 0));
    d.onEvent(nullptr, mouseEv(plat::Event::MouseDown, at.x, at.y, plat::LeftButton, 0));
    CHECK(g->dragging == 1);
    d.onEvent(nullptr, mouseEv(plat::Event::MouseMove, to.x, to.y, 0, plat::LeftButton));
    snap(d, "metronome-dragging");
    d.onEvent(nullptr, mouseEv(plat::Event::MouseUp, to.x, to.y, plat::LeftButton, plat::LeftButton));
    CHECK(g->dragging == 0);
    CHECK_NEAR(live.bar.attackMs, 50, 1.5);
    CHECK_NEAR(live.bar.levelDb, -20, 0.6);
    CHECK(bar->boxes["attack_ms"]->value() == live.bar.attackMs);
    d.hideWindow();
    d.useDefaults();
    CHECK(live == m::MetronomeSettings());
    CHECK(metronome_dialog::loadSettings() == m::MetronomeSettings());
}

// ------------------------------------------------------------------------------------------
static void testAudioDialog() {
    Project project;
    audio::Engine engine(project);
    CHECK(audio_dialog::bufferText(0, 48000) == "Driver default");
    CHECK(audio_dialog::bufferText(512, 48000) == "512 frames  (10.7 ms)");
    CHECK(audio_dialog::nowPlaying(engine).empty());
    AudioPropertiesDialog d(nullptr, "PulseAudio", "", 256, false, engine);
    CHECK(d.title() == "Audio properties");
    CHECK(d.buffers->count() == 6 && d.buffers->currentDataInt() == 256);
    CHECK(!d.exclusiveBox->isVisible());
    picture(d, "audio-properties");
    d.buffers->setCurrentIndex(4);
    d.accept();
    CHECK(d.bufferSize == 1024 && !d.exclusive);
    CHECK(d.driverType == d.types->currentData());
}

// ------------------------------------------------------------------------------------------
static void testRecordingHelpers() {
    auto choices = recording::inputChoices(4);
    CHECK(choices.size() == 6);
    CHECK(choices[0].first == "In 1 (mono)" && choices[0].second == "1");
    CHECK(choices[4].first == "In 1/2 (stereo)" && choices[5].second == "3/4");
    CHECK(recording::inputChoices(3).size() == 4);  // 3 mono, one pair
    CHECK(recording::inputChoices(0).empty());
    CHECK((recording::inputChannels("1") == std::vector<int>{0}));
    CHECK((recording::inputChannels("3/4") == std::vector<int>{2, 3}));
    // the count-in: bars of clicks, the bar's click first
    Timing t;
    t.bpm = 120, t.beatsPerBar = 4;
    metronome::MetronomeSettings ms;
    AudioPtr clicks;
    CHECK(recording::countInAudio(ms, t, 0, 48000, clicks) == 0 && !clicks);
    int64_t frames = recording::countInAudio(ms, t, 2, 48000, clicks);
    CHECK(frames == 8 * 24000 && clicks && clicks->frames() == frames);
    float peak0 = 0, peak1 = 0;
    for (int64_t i = 0; i < 2000; i++) peak0 = std::max(peak0, std::fabs(clicks->frame(i)[0]));
    for (int64_t i = 24000; i < 26000; i++) peak1 = std::max(peak1, std::fabs(clicks->frame(i)[0]));
    CHECK(peak0 > peak1 && peak1 > 0);  // (the bar's click is louder)
    // the take: mono -> stereo; a take that began before 0 s is cut there
    std::vector<float> mono{0.1f, 0.2f, 0.3f, 0.4f}, stereo;
    int64_t start = 1000;
    CHECK(recording::takeAudio(start, mono, 1, stereo));
    CHECK(start == 1000 && stereo.size() == 8 && stereo[2] == 0.2f && stereo[3] == 0.2f);
    start = -3;  // moved earlier by the latency, past 0 s
    CHECK(recording::takeAudio(start, mono, 1, stereo));
    CHECK(start == 0 && stereo.size() == 2 && stereo[0] == 0.4f);
    start = -10;
    CHECK(!recording::takeAudio(start, mono, 1, stereo));
    std::vector<float> two{1, 2, 3, 4};
    start = 5;
    CHECK(recording::takeAudio(start, two, 2, stereo) && stereo == two);
    // placing it replaces what it covers
    Project p;
    TrackPtr track = p.addTrack(Kind::Audio);
    ClipPtr old = audioClipOf(0, 48000, 0.5f, 48000);
    old->track = track;
    p.addClip(old);
    ClipPtr take = std::make_shared<Clip>();
    take->kind = Kind::Audio;
    auto src = std::make_shared<AudioBuffer>();
    src->samples.assign(2 * 12000, 0.25f);
    take->source = src;
    take->sourceSr = 48000;
    recording::placeClip(p, take, 12000, track, 48000);
    CHECK(take->track == track && take->start == 12000 && take->data && take->frames() == 12000);
    int64_t covered = 0;
    for (auto& c : p.clipsOn(track))
        if (c != take && c->start < take->end() && c->end() > take->start) covered++;
    CHECK(covered == 0);
    CHECK(p.clipsOn(track).size() == 3);  // left piece, take, right piece
    // the settings window
    Preferences prefs;
    prefs.recordInput = "3/4";
    prefs.countInBars = 2;
    RecordSettingsDialog d(nullptr, prefs, 4);
    CHECK(d.input->currentData() == "3/4" && d.countIn->currentText() == "2 bars" && d.input->isEnabled());
    CHECK(d.countIn->itemText(0) == "Off" && d.countIn->itemText(1) == "1 bar");
    picture(d, "record-settings");
    RecordSettingsDialog none(nullptr, prefs, 0);
    CHECK(!none.input->isEnabled());
    picture(none, "record-settings-no-inputs");
}

// ------------------------------------------------------------------------------------------
static void testTrackRender() {
    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr track = project.addTrack(Kind::Audio);
    track->name = "Drums";
    ClipPtr clip = audioClipOf(sr, sr / 2, 0.5f, sr);
    clip->track = track;
    project.addClip(clip);
    int regrouped = 0;
    std::string status;
    TrackRenderHost host;
    host.tracksRegrouped = [&] { regrouped++; };
    host.showMessage = [&](const std::string& t, int) { status = t; };
    TrackRenderer r(host, engine);
    // the menu
    Menu menu;
    r.fillMenu(&menu, track);
    std::vector<std::string> texts;
    for (auto& a : menu.actions()) texts.push_back(a->separator ? "-" : a->text);
    CHECK((texts == std::vector<std::string>{"-", "Freeze track", "Bounce to new audio track", "Bounce in place…"}));
    TrackPtr empty = project.addTrack(Kind::Audio);
    Menu menu2;
    r.fillMenu(&menu2, empty);
    CHECK(!menu2.actions()[1]->enabled && !menu2.actions()[2]->enabled);
    // freeze: plays its rendering
    r.freeze(track);
    ClipPtr frozen = std::atomic_load(&track->frozen);
    CHECK(frozen && frozen->name == "Drums (frozen)" && regrouped == 1);
    CHECK(frozen && frozen->start <= sr && frozen->end() >= sr + sr / 2);
    CHECK(status == "Drums is frozen: its plugins rest until you unfreeze it");
    Menu menu3;
    r.fillMenu(&menu3, track);
    CHECK(menu3.actions()[1]->text == "Unfreeze track");
    // a frozen track bounces its rendering as is
    auto rendered = r.render(track);
    CHECK(rendered && rendered->start == frozen->start && rendered->frames() == frozen->frames());
    r.unfreeze(track);
    CHECK(!track->frozen && regrouped == 2);
    // bounce to a new track below: the original is kept and muted
    size_t before = project.tracks.size();
    r.bounce(track, false);
    CHECK(project.tracks.size() == before + 1);
    TrackPtr below = project.tracks[project.indexOf(track) + 1];
    CHECK(below->name == "Drums bounce" && track->muted);
    auto onBelow = project.clipsOn(below);
    CHECK(onBelow.size() == 1 && onBelow[0]->name == "Drums (bounced)");
    float peak = 0;
    if (!onBelow.empty())
        for (float v : onBelow[0]->data->samples) peak = std::max(peak, std::fabs(v));
    CHECK_NEAR(peak, 0.5, 0.01);
    // in place: its clips become one
    track->muted = false;
    r.answerInPlace = 0;  // "No"
    r.bounce(track, true);
    CHECK(project.clipsOn(track).size() == 1 && project.clipsOn(track)[0] == clip);
    r.answerInPlace = 1;
    r.bounce(track, true);
    auto onTrack = project.clipsOn(track);
    CHECK(onTrack.size() == 1 && onTrack[0]->name == "Drums (bounced)" && track->kind == Kind::Audio);
}

static void testMixExport() {
    auto& f = mix_export::formats();
    CHECK(f.size() == 6);
    CHECK(std::string(f[0].filter) == "WAV, 24-bit (*.wav)" && f[0].sampleFormat == SampleFormat::Pcm24);
    CHECK(std::string(f[2].filter) == "WAV, 32-bit float (*.wav)" && f[2].sampleFormat == SampleFormat::Float32);
    CHECK(f[3].flac && std::string(f[3].extension) == ".flac");
    CHECK(&mix_export::formatOf("FLAC, 24-bit (*.flac)") == &f[3]);
    CHECK(&mix_export::formatOf("whatever") == &f[0]);
    CHECK(mix_export::suggestedPath("", "mix") == "Untitled (mix).wav");
    CHECK(mix_export::suggestedPath("/songs/Song.json", "selection") == "/songs/Song (selection).wav");
    CHECK(mix_export::withExtension("/a/b.wav", f[3]) == "/a/b.flac");
    CHECK(mix_export::withExtension("/a/b", f[3]) == "/a/b.flac");
    CHECK(mix_export::withExtension("/a/b.WAV", f[0]) == "/a/b.WAV");

    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr track = project.addTrack(Kind::Audio);
    ClipPtr clip = audioClipOf(0, sr / 4, 0.25f, sr);
    clip->track = track;
    project.addClip(clip);
    std::string status;
    std::string target = TMP + "/mix";
    std::string filter = "FLAC, 24-bit (*.flac)";
    MixExportHost host;
    host.showMessage = [&](const std::string& t, int) { status = t; };
    host.askFile = [&](std::string& path, std::string& chosen) {
        path = target;
        chosen = filter;
        return true;
    };
    host.selection = [](int64_t& s, int64_t& e) {
        s = 0, e = 4800;
        return true;
    };
    MixExporter x(host, engine);
    std::string path = x.renderProject();
    CHECK(path == target + ".flac");
    SoundData back;
    std::string err;
    CHECK(readSoundFile(path, back, err) && back.channels == 2 && back.sampleRate == sr);
    CHECK(back.frames() >= (size_t)(sr / 4));
    CHECK(util::startsWith(status, "Rendered ") && util::endsWith(status, " s to " + path));
    filter = "WAV, 16-bit (*.wav)";
    path = x.renderSelection();
    CHECK(path == target + ".wav");
    CHECK(readSoundFile(path, back, err) && back.frames() == 4800);
}

// --- the successor's features: render formats and normalizing, stems, recording offset, MIDI
// recording, takes, the count-in on the beat, bounces that keep the mix, the High contrast skin ---

const std::string OUT2 = "build/ui-samples/plugins2";

static ClipPtr sineClip(int64_t start, double seconds, double amplitude, int sr, double hz = 1000.0,
                        double spike = 0.0) {
    auto src = std::make_shared<AudioBuffer>();
    int64_t frames = (int64_t)(seconds * sr);
    src->samples.resize((size_t)frames * 2);
    for (int64_t i = 0; i < frames; ++i) {
        float v = (float)(amplitude * std::sin(2 * M_PI * hz * i / sr));
        src->samples[i * 2] = src->samples[i * 2 + 1] = v;
    }
    if (spike > 0) src->samples[(size_t)frames] = src->samples[(size_t)frames + 1] = (float)spike;  // (mid-way)
    ClipPtr c = makeAudioClip("sine", src, sr, sr);
    c->start = start;
    return c;
}

static double dbOf(double v) { return 20 * std::log10(v); }

static void testRenderFormats() {
    using namespace mix_export;
    auto& f = formats();
    CHECK(f.size() == 6);
    CHECK(f[4].kind == Codec::Mp3 && std::string(f[4].filter) == "MP3, 320 kbps (*.mp3)" && f[4].shown() == "MP3, 320 kbps");
    CHECK(f[5].kind == Codec::Ogg && std::string(f[5].extension) == ".ogg");
    CHECK(f[0].shown() == "WAV, 24-bit");
    auto& n = normalizeChoices();
    CHECK(n.size() == 5 && std::string(n[0].text) == "Off: as it is" && n[1].how == Normalize::Peak);
    CHECK(n[2].lufs == -14.0 && n[3].lufs == -16.0 && n[4].lufs == -23.0);
    CHECK(&normalizeOf("whatever") == &n[0]);
    CHECK(suggestedPath("/songs/Song.json", "mix", ".mp3") == "/songs/Song (mix).mp3");
    CHECK(stemName("Bass / synth") == "Bass _ synth" && stemName("???") == "_" && stemName("  ") == "track");

    // the normalizing itself
    int sr = 48000;
    ClipPtr sine = sineClip(0, 2.0, 0.2, sr);
    std::vector<float> a = sine->source->samples;
    double before = loudnessOf(a, sr);
    printf("  a 1 kHz sine at -14 dB: %.2f LUFS\n", before);
    CHECK(std::fabs(before - (-0.691 + 20 * std::log10(0.2))) < 0.8);  // (a 1 kHz sine, both sides; K-weighting adds a little)
    std::string note = normalize(a, sr, n[1]);
    CHECK_NEAR(dbOf(peakOf(a)), -0.3, 1e-3);
    CHECK(note.rfind("peaks to -0.3 dB (+", 0) == 0);
    a = sine->source->samples;
    note = normalize(a, sr, n[2]);
    CHECK_NEAR(loudnessOf(a, sr), -14.0, 0.01);
    CHECK(note.rfind("loudness -14.0 LUFS (+0.", 0) == 0);
    a = sine->source->samples;
    normalize(a, sr, n[4]);
    CHECK_NEAR(loudnessOf(a, sr), -23.0, 0.01);
    // a spike: held back to keep the peaks under -1 dB
    std::vector<float> spiky = sineClip(0, 2.0, 0.2, sr, 1000.0, 0.9)->source->samples;
    note = normalize(spiky, sr, n[2]);
    CHECK_NEAR(dbOf(peakOf(spiky)), -1.0, 1e-3);
    CHECK(note.find(": held back from -14 LUFS to keep the peaks under -1 dB") != std::string::npos);
    // shorter than a gating block: measured as one; silence can't be
    std::vector<float> blip(sineClip(0, 0.2, 0.2, sr)->source->samples);
    CHECK(std::isfinite(loudnessOf(blip, sr)) && std::fabs(loudnessOf(blip, sr) - before) < 0.5);
    std::vector<float> quiet(2 * 48000, 0.0f);  // (a second: longer than a gating block)
    CHECK(normalize(quiet, sr, n[2]).empty());  // (no peak: nothing to do)
    quiet[100] = 1e-9f;
    CHECK(normalize(quiet, sr, n[2]) == "too quiet to measure its loudness: left as it is");
    CHECK(normalize(a, sr, n[0]).empty());

    // a test mix, rendered in every format with every normalizing, read back
    Project project;
    audio::Engine engine(project);
    sr = engine.sr;
    TrackPtr track = project.addTrack(Kind::Audio);
    ClipPtr clip = sineClip(0, 2.0, 0.2, sr, 440.0);
    clip->track = track;
    project.addClip(clip);
    std::string status, chosenFormat, chosenNormalize, target;
    MixExportHost host;
    host.showMessage = [&](const std::string& t, int) { status = t; };
    host.askFormat = [&](std::string& filter, std::string& normalizing) {
        filter = chosenFormat;
        normalizing = chosenNormalize;
        return true;
    };
    host.askFile = [&](std::string& path, std::string& filter) {
        path = target;
        filter.clear();
        return true;
    };
    host.selection = [&](int64_t& s, int64_t& e) {
        s = 0, e = 2 * sr;
        return true;
    };
    MixExporter x(host, engine);
    struct Case { int format, how; double peakTol, lufsTol; };
    std::vector<Case> cases = {{0, 1, 0.01, 0}, {1, 2, 0.02, 0.05}, {2, 4, 0.01, 0.05}, {3, 3, 0.01, 0.05},
                               {4, 2, 0.6, 0.3},  {5, 3, 0.8, 0.3},  {4, 1, 0.8, 0},     {5, 0, 0.8, 0}};
    for (auto& c : cases) {
        chosenFormat = f[c.format].filter;
        chosenNormalize = n[c.how].text;
        target = TMP + "/render_" + std::to_string(c.format) + "_" + std::to_string(c.how);
        std::string path = x.renderSelection();
        CHECK(path == target + f[c.format].extension);
        CHECK(savedFormat() == chosenFormat && savedNormalize() == chosenNormalize);  // (remembered)
        SoundData back;
        std::string err;
        bool read = readSoundFile(path, back, err);
        CHECK(read && back.channels == 2);
        if (!read) {
            printf("  %s: %s\n", path.c_str(), err.c_str());
            continue;
        }
        double frames = (double)back.frames();
        CHECK(std::fabs(frames - 2.0 * sr) <= 0.01 * sr);
        double peak = dbOf(peakOf(back.samples)), lufs = loudnessOf(back.samples, back.sampleRate);
        printf("  %-34s %-44s peak %6.2f dB  %6.2f LUFS  %s\n", f[c.format].filter, n[c.how].text, peak, lufs,
               x.lastNote.c_str());
        if (n[c.how].how == Normalize::Peak) CHECK(std::fabs(peak - (-0.3)) <= c.peakTol);
        else if (n[c.how].how == Normalize::Loudness) CHECK(std::fabs(lufs - n[c.how].lufs) <= c.lufsTol);
        else CHECK(std::fabs(peak - dbOf(0.2)) <= c.peakTol);
        CHECK(util::startsWith(status, "Rendered 2.0 s to " + path));
        if (n[c.how].how != Normalize::Off) CHECK(status.find("; ") != std::string::npos);
    }
    // the render window
    RenderDialog d(nullptr, f[4].filter, n[2].text);
    CHECK(d.format->count() == 6 && d.format->currentText() == "MP3, 320 kbps");
    CHECK(d.normalize->count() == 5 && d.normalize->currentData() == n[2].text);
    picture(d, "render-dialog");
    {
        d.showWindow();
        App::get().doLayouts();
        d.update();
        App::get().tick();
        util::makeDirs(OUT2);
        savePng(OUT2 + "/render_dialog.png", d.framebuffer());
        d.hideWindow();
    }
}

static void testStems() {
    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr drums = project.addTrack(Kind::Audio);
    drums->name = "Drums";
    TrackPtr bass = project.addTrack(Kind::Audio);
    bass->name = "Bass / synth";
    bass->volumeDb = -6;
    ClipPtr a = sineClip(0, 1.0, 0.3, sr), b = sineClip(sr, 2.0, 0.3, sr, 110.0);
    a->track = drums;
    b->track = bass;
    project.addClip(a);
    project.addClip(b);
    std::string folder = TMP + "/stems";
    util::makeDirs(folder);
    for (auto& e : util::listDir(folder)) util::removeFile(util::joinPath(folder, e.name));
    std::string status;
    MixExportHost host;
    host.showMessage = [&](const std::string& t, int) { status = t; };
    host.askFolder = [&] { return folder; };
    MixExporter x(host, engine);
    std::vector<std::string> paths = x.exportStems();
    CHECK(paths.size() == 2);
    if (paths.size() != 2) return;
    CHECK(util::baseName(paths[0]) == "01 Drums.wav" && util::baseName(paths[1]) == "02 Bass _ synth.wav");
    SoundData d0, d1;
    std::string err;
    CHECK(readSoundFile(paths[0], d0, err) && readSoundFile(paths[1], d1, err));
    CHECK(d0.frames() == d1.frames() && d0.frames() >= (size_t)(3 * sr));  // (all as long as the song)
    // each its own sound: the drums' stem is silent where only the bass plays, and the bass's fader counts
    double drumsLate = 0, bassEarly = 0, bassPeak = 0;
    for (size_t i = (size_t)(1.5 * sr); i < (size_t)(2.5 * sr); ++i) drumsLate = std::max(drumsLate, (double)std::fabs(d0.samples[i * 2]));
    for (size_t i = 0; i < (size_t)(0.9 * sr); ++i) bassEarly = std::max(bassEarly, (double)std::fabs(d1.samples[i * 2]));
    for (size_t i = 0; i < d1.frames(); ++i) bassPeak = std::max(bassPeak, (double)std::fabs(d1.samples[i * 2]));
    CHECK(drumsLate < 1e-4 && bassEarly < 1e-4);
    printf("  stems: %zu frames each, bass peak %.2f dB\n", d0.frames(), dbOf(bassPeak));
    CHECK(bassPeak < 0.3 * 0.75);  // (-6 dB on its fader, maybe less on one side by its pan law)
    CHECK(util::startsWith(status, "Exported 2 stems (") && util::endsWith(status, " s each) to " + folder));
    // again: the files there are kept, the new ones numbered
    paths = x.exportStems();
    CHECK(paths.size() == 2 && util::baseName(paths[0]) == "01 Drums (2).wav");
}

static void testRecordOffset() {
    using namespace record_offset_dialog;
    settings::remove(KEY);
    Preferences prefs;
    prefs.driverType = "ALSA";
    prefs.deviceName = "hw:1";
    CHECK(deviceKey("ALSA", "hw:1") == "ALSA|hw:1" && offsetMs(prefs) == 0.0 && offsets().empty());
    setOffsetMs(prefs, 12.3456);
    setOffsetMs("JACK", "", -3);
    CHECK(offsetMs(prefs) == 12.35 && offsetMs("JACK", "") == -3.0 && offsets().size() == 2);
    settings::set(KEY, std::string("not json"));
    CHECK(offsets().empty() && offsetMs(prefs) == 0.0);
    setOffsetMs(prefs, 12.35);

    // the measurement: metronome clicks recorded 7 ms late
    int sr = 48000;
    Timing timing;
    timing.bpm = 120, timing.beatsPerBar = 4;
    auto src = std::make_shared<AudioBuffer>();
    src->samples.assign((size_t)sr * 4 * 2, 0.0f);
    int64_t takeStart = sr / 4;  // (the take starts at 0.25 s: beats at 0.5, 1.0, … land 7 ms late in it)
    for (double beat = 0.5; beat < 4.0; beat += 0.5) {
        int64_t at = (int64_t)((beat + 0.007) * sr) - takeStart;
        for (int i = 0; i < 200; ++i) src->samples[(size_t)(at + i) * 2] = src->samples[(size_t)(at + i) * 2 + 1] = 0.8f * std::exp(-i / 50.0);
    }
    ClipPtr take = makeAudioClip("take", src, sr, sr);
    take->start = takeStart;
    auto m = measure(*take, timing, sr);
    CHECK(m && m->count >= 6 && std::fabs(m->late - 0.007) < 0.0005 && m->spread < 0.001);
    auto silent = makeAudioClip("silence", std::make_shared<AudioBuffer>(AudioBuffer{std::vector<float>(9600, 0.0f)}), sr, sr);
    CHECK(!measure(*silent, timing, sr));

    // the dialog: Measure corrects from the offset the take was placed with; OK keeps it
    {
        RecordOffsetDialog d(nullptr, prefs, nullptr, 0.0, timing, sr);
        CHECK(std::fabs(d.offset->value() - 12.35) <= 0.051);  // (shown with one decimal)
        CHECK(!d.measureLastTake() && d.result->text().rfind("There is no audio take yet", 0) == 0);
        d.reject();
    }
    {
        RecordOffsetDialog d(nullptr, prefs, take, 2.0, timing, sr);
        auto late = d.measureLastTake();
        CHECK(late && std::fabs(d.offset->value() - 9.0) < 0.1);
        CHECK(d.result->text().rfind("From 7 clicks: the take was 7.0 ms late.", 0) == 0);
        util::makeDirs(OUT2);
        d.showWindow();
        App::get().doLayouts();
        d.update();
        App::get().tick();
        savePng(OUT2 + "/record_offset.png", d.framebuffer());
        d.hideWindow();
        d.accept();
        CHECK(std::fabs(offsetMs(prefs) - 9.0) < 0.1);
    }

    // applied: the offset new takes get (the engine's Recorder takes it off with the device's delays),
    // and a take is kept with the track's takes and as the last take, with the offset it was placed with
    Project project;
    audio::Engine engine(project);
    sr = engine.sr;
    TrackPtr track = project.addTrack(Kind::Audio);
    RecordingHost host;
    host.prefs = &prefs;
    int lanes = 0;
    host.lanesChanged = [&] { ++lanes; };
    Recording rec(host, engine);
    CHECK(std::fabs(rec.takesOffsetMs() - offsetMs(prefs)) < 1e-9 && std::fabs(rec.takesOffsetMs() - 9.0) < 0.1);
    host.offsetMs = [] { return 10.0; };
    Recording rec10(host, engine);
    CHECK(rec10.takesOffsetMs() == 10.0);
    audio::Recorder recorder({0}, 0.004, sr, 1.0, rec10.takesOffsetMs());
    recorder.begin(sr, 0);
    std::vector<float> in((size_t)sr / 2, 0.25f);
    recorder.take(in.data(), 1, (int)in.size());
    std::vector<float> raw;
    int64_t at = recorder.audio(raw);
    CHECK(at == sr - (int64_t)(0.004 * sr) - sr / 100 && raw.size() == in.size());  // (delay and offset taken off)
    ClipPtr first = rec10.placeAudioTake(track, at, raw, 1, recorder.offsetMs);
    CHECK(first && first->start == at);
    CHECK(rec10.lastAudio && rec10.lastAudio != first && rec10.lastAudio->start == first->start &&
          rec10.lastOffsetMs == 10.0);
    CHECK(track->takes.size() == 1 && !track->showTakes && lanes == 0);
    ClipPtr second = rec10.placeAudioTake(track, 2 * sr, raw, 1, 10.0);
    CHECK(second && track->takes.size() == 2 && track->showTakes && lanes == 1);
    CHECK((*track->takes)[0]->track == nullptr && (*track->takes)[1]->name == "Recording 2");
    settings::remove(KEY);
}

static void testMidiRecording() {
    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr keys = project.addTrack(Kind::Midi);
    keys->name = "Keys";
    RecordingHost host;
    std::string status;
    host.showMessage = [&](const std::string& t, int) { status = t; };
    Recording rec(host, engine);
    rec.beginMidiTake(keys, sr);  // (from 1 s)
    CHECK(rec.recordingMidi() && rec.active());
    rec.midiNoteAt(60, 100, sr / 2.0);       // (during the count-in: starts where the recording does)
    rec.midiNoteAt(64, 90, 1.5 * sr);
    rec.midiNoteAt(60, 0, 2.0 * sr);
    rec.midiNoteAt(64, 0, 1.5 * sr + 10);    // (very short: at least 10 ms)
    rec.midiNoteAt(67, 80, 2.5 * sr);        // (still held at the end: ends there)
    rec.midiNoteAt(72, 0, 2.6 * sr);         // (an off without its on: ignored)
    ClipPtr clip = rec.finishMidiAtFrame(3.0 * sr);
    CHECK(!rec.recordingMidi() && !rec.active());
    CHECK(clip && clip->kind == Kind::Midi && clip->name == "Recording 1" && clip->start == sr);
    if (clip && clip->notes) {
        const auto& n = *clip->notes;
        CHECK(n.size() == 3);
        CHECK(n.size() == 3 && n[0].pitch == 60 && n[0].start == 0.0 && std::fabs(n[0].end - 1.0) < 1e-9 && n[0].velocity == 100);
        CHECK(n.size() == 3 && n[1].pitch == 64 && std::fabs(n[1].start - 0.5) < 1e-9 && std::fabs(n[1].end - 0.51) < 1e-9);
        CHECK(n.size() == 3 && n[2].pitch == 67 && std::fabs(n[2].start - 1.5) < 1e-9 && std::fabs(n[2].end - 2.0) < 1e-9);
        CHECK(std::fabs(clip->lengthS - 2.0) < 1e-9);
    }
    CHECK(project.clipsOn(keys).size() == 1 && keys->takes.size() == 1);
    CHECK(status == "Recorded 3 notes onto Keys");
    rec.beginMidiTake(keys, 0);
    CHECK(!rec.finishMidiAtFrame(sr) && status == "No notes were played");
    // start(): a MIDI track needs a keyboard to play
    host.currentTrack = [&] { return keys; };
    host.midiInputReady = [] { return false; };
    Recording rec2(host, engine);
    CHECK(!rec2.start() && !rec2.active());
}

static void testCountInOnTheBeat() {
    Timing t;
    t.bpm = 120, t.beatsPerBar = 4;
    metronome::MetronomeSettings ms;
    AudioPtr clicks;
    int sr = 48000;
    auto clickAt = [&](int64_t from, int64_t to) {
        float peak = 0;
        for (int64_t i = std::max<int64_t>(0, from); i < std::min<int64_t>(to, clicks->frames()); ++i)
            peak = std::max(peak, std::fabs(clicks->frame(i)[0]));
        return peak;
    };
    // from bar 2 (2 s): the same as from 0, the bar's click first
    int64_t frames = recording::countInAudio(ms, t, 1, sr, clicks, 2.0);
    CHECK(frames == 4 * 24000);
    float bar = clickAt(0, 2000), beat = clickAt(24000, 26000);
    CHECK(bar > beat && beat > 0);
    // a quarter of a beat late (2.125 s): clicks a quarter beat into the count-in, the bar's one at 2 s - 2 s
    frames = recording::countInAudio(ms, t, 1, sr, clicks, 2.125);
    CHECK(frames == 4 * 24000);
    CHECK(clickAt(0, 3000) < 1e-6);  // (nothing right at its start)
    CHECK(clickAt(18000, 20000) > 0);  // (the next beat: 0.375 s after the count-in starts)
    // in 3/4 from bar 3 (12 beats? no: 3 beats a bar) the count-in is 3 beats
    Timing w;
    w.bpm = 120, w.beatsPerBar = 3;
    frames = recording::countInAudio(ms, w, 2, sr, clicks, 3.0);
    CHECK(frames == 6 * 24000);
    CHECK(clickAt(0, 2000) > clickAt(24000, 26000));          // (bar line at 0 s… 3 s - 3 s)
    CHECK(clickAt(3 * 24000, 3 * 24000 + 2000) > clickAt(24000, 26000));
}

static void testBounceKeepsTheMix() {
    Project project;
    audio::Engine engine(project);
    int sr = engine.sr;
    TrackPtr ret = project.addTrack(Kind::Return);
    TrackPtr track = project.addTrack(Kind::Audio);
    track->name = "Vox";
    track->volumeDb = -4.5;
    track->pan = 0.25;
    track->delayMs = 3;
    track->sends = std::vector<Send>{Send{ret, -8}};
    auto a = std::make_shared<Automation>();
    a->slot = track->controls;
    a->param = "Volume";
    a->setPoints({{0.0, 0.5}, {1.0, 0.8}});
    track->automation = std::vector<AutomationPtr>{a};
    track->showAutomation = true;
    ClipPtr clip = sineClip(0, 0.5, 0.3, sr);
    clip->track = track;
    project.addClip(clip);
    TrackRenderHost host;
    TrackRenderer r(host, engine);
    r.bounce(track, false);
    TrackPtr bounced;
    for (auto& t : project.tracks)
        if (t->name == "Vox bounce") bounced = t;
    CHECK(bounced && track->muted);
    if (bounced) {
        CHECK(bounced->volumeDb == -4.5 && bounced->pan == 0.25 && bounced->delayMs == 3);
        CHECK(bounced->sends.size() == 1 && (*bounced->sends)[0].track == ret && (*bounced->sends)[0].db == -8);
        CHECK(bounced->automation.size() == 1 && (*bounced->automation)[0]->slot == bounced->controls &&
              (*bounced->automation)[0]->points.size() == 2 && bounced->showAutomation);
    }
    // in place: a MIDI track's MIDI takes go
    TrackPtr keys = project.addTrack(Kind::Midi);
    keys->name = "Keys";
    ClipPtr notes = makeMidiClip("n", {Note{0, 0.5, 60, 100}}, 0.5, sr);
    notes->track = keys;
    project.addClip(notes);
    ClipPtr midiTake = copyClip(notes);
    midiTake->track = nullptr;
    keys->takes = std::vector<ClipPtr>{midiTake, midiTake};
    keys->showTakes = true;
    r.answerInPlace = 1;
    r.bounce(keys, true);
    CHECK(keys->kind == Kind::Audio && keys->takes.empty() && !keys->showTakes && !keys->sampler);
}

static void testHighContrast() {
    skin::Skin s;
    std::vector<std::string> problems;
    std::string err;
    CHECK(skin::read("data/skins/high_contrast.json", s, problems, err, true));
    CHECK(problems.empty() && s.name == "High contrast" && s.colors.at("window") == "#000000");
    bool listed = false;
    for (auto& k : skin::library()) listed = listed || k.name == "High contrast";
    CHECK(listed);
    // "colors" and "fonts" must be names with their values
    util::writeFile(TMP + "/bad_colors.json", R"({"format": "tinydaw-skin", "colors": ["#000000"]})");
    CHECK(!skin::read(TMP + "/bad_colors.json", s, problems, err));
    CHECK(err == "Its \"colors\" must be names with their values: {\"name\": value, …}.");
    util::writeFile(TMP + "/no_fonts.json", R"({"format": "tinydaw-skin", "fonts": null, "colors": {}})");
    CHECK(skin::read(TMP + "/no_fonts.json", s, problems, err));
    skin::apply(skin::find("High contrast"));
    {
        metronome::MetronomeSettings ms;
        MetronomeDialog m(nullptr, ms, nullptr, nullptr);
        m.showWindow();
        App::get().doLayouts();
        m.update();
        App::get().tick();
        util::makeDirs(OUT2);
        savePng(OUT2 + "/high_contrast_metronome.png", m.framebuffer());
        m.hideWindow();
    }
    skin::apply(skin::find("Dark"));
}

// Options → Wine prefixes: each prefix's facts, and the wine program chosen for it (or automatic)
void testWinePrefixesDialog() {
    std::string dir = TMP + "/wine";
    auto prefix = [&](const std::string& name, const std::string& arch) {
        std::string p = util::absolutePath(dir + "/" + name);
        util::makeDirs(p + "/drive_c");
        util::writeFile(p + "/system.reg", "WINE REGISTRY Version 2\n\n#arch=" + arch + "\n");
        return p;
    };
    std::string p64 = prefix("p64", "win64"), p32 = prefix("p32", "win32");
    std::string fake = util::absolutePath(dir + "/bin/wine");
    util::makeDirs(dir + "/bin");
    util::writeFile(fake, "#!/bin/sh\n[ \"$1\" = --version ] && echo wine-8.0 && exit 0\nexit 1\n");
    chmod(fake.c_str(), 0755);
    WinePrefixesDialog d(nullptr, {p64, p32}, {{p64, fake}, {"/not/listed", fake}});
    CHECK(d.list->count() == 2 && d.list->currentRow() == 0);
    CHECK(d.programs().size() == 1 && d.programs()[p64] == fake);  // (only of the prefixes listed)
    std::string about = d.about->text();
    CHECK(about.find("64-bit prefix") != std::string::npos && about.find(fake) != std::string::npos);
    CHECK(about.find("wine-8.0") != std::string::npos && about.find("chosen") != std::string::npos);
    CHECK(about.find("tested with Wine 11") != std::string::npos);  // (an older Wine: said)
    CHECK(d.automaticButton->isEnabled() && d.wineButton->isEnabled());
    picture(d, "wine-prefixes");
    d.setWine("");  // Automatic
    CHECK(d.programs().empty() && !d.automaticButton->isEnabled());
    CHECK(d.about->text().find("chosen") == std::string::npos);
    d.list->setCurrentRow(1);
    CHECK(d.about->text().find("32-bit prefix") != std::string::npos);
    d.setWine(fake);
    CHECK(d.programs().size() == 1 && d.programs()[p32] == fake);
    d.removePrefix();
    CHECK(d.prefixes().size() == 1 && d.programs().empty());
    CHECK(d.add(p32) && d.prefixes().size() == 2 && d.list->currentRow() == 1);  // (added: selected)
    CHECK(!d.add(dir + "/bin"));                                                 // (not a prefix)
    // New prefix… and Install Windows plugin… wait for their Wine process with a progress window
    for (int want : {0, 3}) {
        int pid = fork();
        if (pid == 0) {
            usleep(300000);
            _exit(want);
        }
        int code = -5;
        double t0 = App::get().now();
        CHECK(wine_setup::waitFor(nullptr, pid, "Waiting", "A process…", code));
        CHECK(code == want && App::get().now() - t0 < 5);
    }
}

int main() {
    App::get().initHeadless();
    util::makeDirs(OUT);
    util::makeDirs(TMP);
    settings::setPath(TMP + "/settings.json");
    testSkinFiles();
    testColorWheel();
    testRecentColours();
    testColorPickerDialog();
    testSkinDialog();
    testMetronome();
    testAudioDialog();
    testRecordingHelpers();
    testTrackRender();
    testMixExport();
    testRenderFormats();
    testStems();
    testRecordOffset();
    testMidiRecording();
    testCountInOnTheBeat();
    testBounceKeepsTheMix();
    testHighContrast();
    testWinePrefixesDialog();
    // the dialogs in another skin
    skin::apply(skin::find("Light"));
    {
        ColorPickerDialog d(nullptr, "#377d55", "", ColorSubject::Clip);
        picture(d, "color-picker-clip-light");
        metronome::MetronomeSettings s;
        MetronomeDialog m(nullptr, s, nullptr, nullptr);
        picture(m, "metronome-light");
    }
    skin::apply(skin::find("Dark"));
    TEST_MAIN_END();
}
