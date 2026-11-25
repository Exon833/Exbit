// The plugins' user interface: the patch panel (wiring rules, mouse), the FX window and the
// instrument patch panel, the plugin host (loading, restoring, the kind finder), pictures, the
// instrument menu, the parameter chooser and the VST2 folders dialog. Pictures of the panel and
// the dialogs go to build/ui-samples/plugins/.
//
// With a display (DISPLAY set), the test runs itself again with --display for the parts that need
// real windows: photographing the test VST3 effect's window offscreen, and opening its window
// (closed again at once).
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/automation.h"
#include "audio/engine.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "check.h"
#include "core/util.h"
#include "plugins/library.h"
#include "plugins/probe.h"
#include "effects/effects.h"
#include "ui/automation_dialog.h"
#include "ui/effect_window.h"
#include "ui/plugin_chooser.h"
#include "ui/instrument_menu.h"
#include "ui/patch_panel.h"
#include "ui/plugin_dialog.h"
#include "ui/plugin_editor.h"
#include "ui/plugin_folders_dialog.h"
#include "ui/plugin_host.h"
#include "ui/plugin_pictures.h"
#include "ui/plugin_windows.h"
#include "ui/theme.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

namespace {

const std::string SAMPLES = "build/ui-samples/plugins";
std::string scratch;  // a folder for the caches and pictures of this test

void saveWidget(ui::Widget* w, const std::string& name, bool windowBackground = false) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    if (windowBackground) p.fillRect(img.rect(), theme::c("window"));
    w->paintTree(p, w->rect());
    util::makeDirs(SAMPLES);
    CHECK(savePng(SAMPLES + "/" + name, img));
}

Image testPicture(int w, int h) {  // something like a plugin's window
    Image img(w, h, Color(32, 36, 44));
    Painter p(img);
    p.fillRect(Rect(0, 0, w, 28), Color(60, 66, 80));
    for (int i = 0; i < 5; ++i) {
        float cx = w * (i + 0.5f) / 5, cy = h * 0.55f;
        p.fillEllipse(RectF(cx - 22, cy - 22, 44, 44), Color(90, 150, 220));
        p.fillEllipse(RectF(cx - 15, cy - 15, 30, 30), Color(25, 28, 34));
    }
    Path curve;
    curve.moveTo(10, h - 30.f);
    curve.cubicTo(w * 0.3f, h - 90.f, w * 0.6f, h - 10.f, w - 10.f, h - 60.f);
    p.strokePath(curve, Color(240, 170, 60), 3);
    return img;
}

SlotPtr fakeSlot(const std::string& name, const std::string& kind = plug::EFFECT) {
    auto s = std::make_shared<PluginSlot>();
    s->name = name;
    s->path = "/nowhere/" + name + ".vst3";
    s->kind = kind;
    return s;
}

// a plugin whose resizable window starts bigger than any screen (Surge XT at 150 %), and takes the sizes
// asked of it kept to its own proportions
struct BigEditor : plug::Plugin {
    int w = 5000, h = 4000;
    std::vector<std::pair<int, int>> asked;
    bool open = false;
    bool isVst2() const override { return false; }
    std::string name() const override { return "Big"; }
    std::string vendor() const override { return ""; }
    std::string subcategories() const override { return "Fx"; }
    bool isInstrument() const override { return false; }
    bool hasSidechain() const override { return false; }
    int latency() const override { return 0; }
    double sampleRate() const override { return 48000; }
    void prepare(double, int) override {}
    void reset() override {}
    void setTransport(double, int, int, bool, int64_t, double) override {}
    void process(const float* const[2], const float* const[2], float* const[2], int) override {}
    void render(const std::vector<plug::NoteMessage>&, float* const[2], int) override {}
    void notesOff() override {}
    bool holding() const override { return false; }
    std::vector<plug::ParamInfo> parameters() override { return {}; }
    double getParam(uint32_t) override { return 0; }
    void setParam(uint32_t, double) override {}
    std::string paramText(uint32_t, double) override { return ""; }
    std::vector<uint32_t> edited() override { return {}; }
    void windowEdit(uint32_t, double) override {}
    void idle() override {}
    std::string componentState() override { return "big"; }
    std::string controllerState() override { return ""; }
    void setState(const std::string&, const std::string&) override {}
    bool openEditor(uintptr_t, void*, double, int& ow, int& oh) override {
        open = true;
        ow = w, oh = h;
        return true;
    }
    void closeEditor() override { open = false; }
    bool editorOpen() const override { return open; }
    bool hasEditor() override { return true; }
    bool editorResizable() override { return true; }
    bool wants = false;  // (asks for its own size again: a zoom chosen in it)
    bool requestedSize(int& ow, int& oh) override {
        if (!wants) return false;
        wants = false;
        ow = w, oh = h;
        return true;
    }
    void editorResized(int& ow, int& oh) override {
        asked.push_back({ow, oh});
        double s = std::min((double)ow / 5000, (double)oh / 4000);
        w = ow = (int)(5000 * s), h = oh = (int)(4000 * s);
    }
};

ui::MouseEvent mouse(Point p, int button = plat::LeftButton) {
    ui::MouseEvent e;
    e.pos = p;
    e.globalPos = p;
    e.button = button;
    e.buttons = button;
    return e;
}

Point at(PointF p) { return Point((int)std::lround(p.x), (int)std::lround(p.y)); }

// --- the patch panel's rules -------------------------------------------------------------------

void testWiringRules() {
    using N = PanelNode;
    ui::Window win;
    win.resizeWindow(1100, 700);
    auto* panel = win.add<PatchPanel>();
    panel->setGeometry(0, 0, 1100, 700);
    std::vector<PanelUnit> units(3);
    units[0].name = "EQ";
    units[1].name = "Comp";
    units[1].sidechain = true;
    units[2].name = "Delay";
    std::vector<PanelCable> cables{{N::in(), N::unit(0)}, {N::unit(0), N::unit(1)}, {N::unit(1), N::out()}};
    panel->setUnits(units, cables, 0, {{"Kick", false}, {"Bass", true}});
    CHECK(panel->units()[0].pos.has_value() && panel->units()[2].pos.has_value());  // (free spots found)
    // loops refused; a unit not into itself; not twice
    CHECK(!panel->canPlug({N::unit(1), N::unit(0)}));
    CHECK(!panel->canPlug({N::unit(0), N::unit(0)}));
    CHECK(!panel->canPlug({N::in(), N::unit(0)}));          // (already there)
    CHECK(panel->canPlug({N::in(), N::out()}));             // straight through, besides
    CHECK(panel->canPlug({N::unit(2), N::unit(0)}));        // (nothing feeds 2 from 0)
    CHECK(!panel->canPlug({N::unit(1), N::side(1)}));       // its own sidechain
    CHECK(!panel->canPlug({N::unit(1), N::side(1)}));
    // an input adds up cables: a second one into the same input is fine
    CHECK(panel->canPlug({N::in(), N::unit(1)}));
    CHECK(panel->canPlug({N::unit(2), N::unit(1)}));
    // sidechains: from IN, from an effect, from another track (not a blocked one); tracks only into sidechains
    CHECK(panel->canPlug({N::in(), N::side(1)}));
    CHECK(panel->canPlug({N::unit(0), N::side(1)}));
    CHECK(panel->canPlug({N::track(0), N::side(1)}));
    CHECK(!panel->canPlug({N::track(1), N::side(1)}));      // Bass listens to this track already
    CHECK(!panel->canPlug({N::track(0), N::unit(1)}));
    CHECK(!panel->canPlug({N::track(0), N::out()}));
    // a sidechain closing a loop: 1 → 2 → (2 feeds 1's sidechain): refused once 1 feeds 2
    std::vector<PanelCable> more = cables;
    more.push_back({N::unit(1), N::unit(2)});
    panel->setUnits(units, more, 0, {{"Kick", false}, {"Bass", true}});
    CHECK(!panel->canPlug({N::unit(2), N::side(1)}));
    CHECK(!panel->canPlug({N::unit(2), N::unit(0)}));
    // the moved cable doesn't count against itself
    CHECK(panel->canPlug({N::unit(2), N::unit(0)}, PanelCable{N::unit(1), N::unit(2)}));
    CHECK(PatchPanel::reaches(N::unit(0), N::unit(2), more));
    CHECK(!PatchPanel::reaches(N::unit(2), N::unit(0), more));

    // dragging with the mouse: from IN's jack to unit 2's input plugs a cable in
    panel->setUnits(units, cables, 0, {{"Kick", false}, {"Bass", true}});
    std::vector<std::pair<std::optional<PanelCable>, std::optional<PanelCable>>> patched;
    panel->patched.connect([&](std::optional<PanelCable> r, std::optional<PanelCable> a) { patched.push_back({r, a}); });
    Point inJack = at(panel->jackPoint({Jack::Source, N::in()}));
    Point unit2In = at(panel->jackPoint({Jack::Target, N::unit(2)}));
    auto e = mouse(inJack);
    panel->mousePress(e);
    CHECK(panel->wiring.has_value());
    CHECK(panel->wiringTarget(Jack{Jack::Target, N::unit(2)}).has_value());
    CHECK(!panel->wiringTarget(Jack{Jack::Source, N::unit(2)}).has_value());  // (outputs go to inputs)
    e = mouse(unit2In);
    panel->mouseMove(e);
    saveWidget(panel, "wiring.png");
    panel->mouseRelease(e);
    CHECK(patched.size() == 1 && !patched[0].first && patched[0].second &&
          *patched[0].second == PanelCable(N::in(), N::unit(2)));
    // either way round: from an input to an output
    patched.clear();
    Point unit2Out = at(panel->jackPoint({Jack::Source, N::unit(2)}));
    Point sideJack = at(panel->jackPoint({Jack::SideIn, N::unit(1)}));
    e = mouse(sideJack);
    panel->mousePress(e);
    e = mouse(unit2Out);
    panel->mouseMove(e);
    panel->mouseRelease(e);
    CHECK(patched.size() == 1 && patched[0].second && *patched[0].second == PanelCable(N::unit(2), N::side(1)));
    // a plug pulled off an input and dropped on nothing: unplugged
    patched.clear();
    Point unit1In = at(panel->jackPoint({Jack::Target, N::unit(1)}));
    e = mouse(unit1In);
    panel->mousePress(e);
    CHECK(panel->wiring && panel->wiring->lifted && *panel->wiring->lifted == PanelCable(N::unit(0), N::unit(1)));
    e = mouse(Point(600, 650));
    panel->mouseMove(e);
    panel->mouseRelease(e);
    CHECK(patched.size() == 1 && patched[0].first && *patched[0].first == PanelCable(N::unit(0), N::unit(1)) &&
          !patched[0].second);
    // moved to another input: removed there, plugged in here
    patched.clear();
    e = mouse(unit1In);
    panel->mousePress(e);
    e = mouse(unit2In);
    panel->mouseMove(e);
    panel->mouseRelease(e);
    CHECK(patched.size() == 1 && patched[0].first && patched[0].second &&
          *patched[0].second == PanelCable(N::unit(0), N::unit(2)));
    // put back where it was: nothing
    patched.clear();
    e = mouse(unit1In);
    panel->mousePress(e);
    panel->mouseRelease(e);
    CHECK(patched.empty());
    // dragging a unit moves it on the grid
    int moved = -1;
    Point movedTo;
    panel->unitMoved.connect([&](int i, Point p) { moved = i, movedTo = p; });
    Rect r2 = panel->unitRect(2);
    e = mouse(Point(r2.x + 100, r2.y + 60));
    panel->mousePress(e);
    CHECK(panel->current == 2);
    e = mouse(Point(r2.x + 100 + 40, r2.y + 60 + 300));
    panel->mouseMove(e);
    panel->mouseRelease(e);
    CHECK(moved == 2 && movedTo.x % patch_panel::SNAP == 0 && movedTo.y % patch_panel::SNAP == 0);
    CHECK(std::abs(movedTo.y - (r2.y + 300)) <= patch_panel::SNAP);
    CHECK(panel->minimumSize().y >= movedTo.y + panel->cardH);  // (the space grew)
    // the light toggles bypass; keys select and remove
    int bypassed = -1, removed = -1, opened = -1;
    panel->bypassToggled.connect([&](int i) { bypassed = i; });
    panel->removeRequested.connect([&](int i) { removed = i; });
    panel->opened.connect([&](int i) { opened = i; });
    e = mouse(panel->ledRect(0).center());
    panel->mousePress(e);
    panel->mouseRelease(e);
    CHECK(bypassed == 0);
    panel->current = 0;
    ui::KeyEvent k;
    k.key = plat::Key_Right;
    panel->keyPress(k);
    CHECK(panel->current == 1);
    k.key = plat::Key_Delete;
    panel->keyPress(k);
    CHECK(removed == 1);
    k.key = plat::Key_Return;
    panel->keyPress(k);
    CHECK(opened == 1);
    // double-click an empty spot: place an effect there
    std::optional<Point> addAt;
    bool addAsked = false;
    panel->addRequested.connect([&](std::optional<Point> p) { addAsked = true, addAt = p; });
    e = mouse(Point(700, 560));
    panel->mouseDoubleClick(e);
    CHECK(addAsked && addAt && addAt->x % patch_panel::SNAP == 0);
    // tooltips
    CHECK(util::startsWith(panel->toolTipAt(inJack), "IN: the track's sound"));
    CHECK(panel->toolTipAt(at(panel->jackPoint({Jack::Source, N::track(1)}))).find("already listens") != std::string::npos);
    CHECK(panel->nodeName(N::side(1)) == "Comp's sidechain");
    // a cable is found under the mouse along its curve
    auto [a, b] = panel->cableEnds({N::unit(1), N::out()});
    (void)b;
    CHECK(panel->cableAt(at(a) + Point(4, 6)).has_value());
}

void testInstrumentRules() {
    using N = PanelNode;
    ui::Window win;
    win.resizeWindow(900, 600);
    auto* panel = win.add<PatchPanel>(true);
    panel->setGeometry(0, 0, 900, 600);
    std::vector<PanelUnit> units(2);
    units[0].name = "Piano";
    units[0].volumeDb = 0.0;
    units[1].name = "Strings";
    units[1].volumeDb = -12.0;
    panel->setUnits(units, {{N::in(), N::unit(0)}}, 0);
    CHECK(panel->canPlug({N::unit(0), N::out()}));
    CHECK(panel->canPlug({N::in(), N::unit(1)}));
    CHECK(!panel->canPlug({N::in(), N::unit(0)}));  // (twice)
    CHECK(!panel->canPlug({N::unit(0), N::unit(1)}));
    CHECK(!panel->canPlug({N::in(), N::out()}));
    CHECK(panel->volumeRect(0).has_value());
    // the fader: in half dB, caught at 0 dB; wheel 1 dB a notch; double-click 0 dB
    auto [x0, x1] = panel->groove(0);
    CHECK_NEAR(panel->volumeAt(0, x0 - 50), config::MIN_DB, 1e-9);
    CHECK_NEAR(panel->volumeAt(0, x1 + 50), config::MAX_DB, 1e-9);
    CHECK_NEAR(panel->volumeAt(0, panel->volumeX(0, 0.4)), 0.0, 1e-9);
    double v = panel->volumeAt(0, panel->volumeX(0, -20.3));
    CHECK(std::fabs(v * 2 - std::round(v * 2)) < 1e-9 && std::fabs(v + 20.5) <= 0.5);
    std::vector<std::pair<int, double>> changes;
    panel->volumeChanged.connect([&](int i, double db) { changes.push_back({i, db}); });
    Rect vr = *panel->volumeRect(1);
    ui::WheelEvent w;
    w.pos = vr.center();
    w.dy = 1;
    CHECK(panel->wheel(w));
    CHECK(!changes.empty() && changes.back().first == 1 && changes.back().second == -11.0);
    auto e = mouse(vr.center());
    panel->mouseDoubleClick(e);
    CHECK(changes.back().second == 0.0);
    CHECK(panel->toolTipAt(vr.center()).find("volume") != std::string::npos);
}

// --- pictures -------------------------------------------------------------------------------------

void testPictures() {
    CHECK(plugin_pictures::sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(plugin_pictures::sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    plugin_pictures::setFolder(util::joinPath(scratch, "pictures"));
    PluginSlot s;
    s.name = "Pro-Q 4 (VST2)";
    s.path = "/x/Pro-Q 4.so";
    std::string file = plugin_pictures::fileFor(s);
    util::removeFile(file);  // (from an earlier run)
    CHECK(util::baseName(file).rfind("Pro-Q_4_VST2-", 0) == 0);
    CHECK(util::endsWith(file, ".png") && util::baseName(file).size() == std::string("Pro-Q_4_VST2-").size() + 10 + 4);
    CHECK(!plugin_pictures::hasPicture(s));
    Image blank(300, 200, Color(10, 10, 10)), out;
    CHECK(!plugin_pictures::fromCapture(blank, out));  // (nothing drawn yet)
    Image drawn = testPicture(900, 500);
    CHECK(plugin_pictures::fromCapture(drawn, out));
    CHECK(plugin_pictures::save(s, out));
    auto pic = plugin_pictures::picture(s);
    CHECK(pic && pic->width() == plugin_pictures::WIDTH && std::abs(pic->height() - 267) <= 1);
    // a VST3 bundle's own snapshot (two plugins in it: the one named so in moduleinfo.json)
    std::string bundle = util::joinPath(scratch, "Snap.vst3");
    util::makeDirs(bundle + "/Contents/x86_64-linux");
    util::makeDirs(bundle + "/Contents/Resources/Snapshots");
    util::writeFile(bundle + "/Contents/x86_64-linux/Snap.so", "");
    util::writeFile(bundle + "/Contents/Resources/moduleinfo.json",
                    "{\n // comment\n \"Classes\": [ {\"CID\": \"AAAA\", \"Name\": \"Snap A\",},\n"
                    " {\"CID\": \"BBBB\", \"Name\": \"Snap B\"}, ],\n}");
    savePng(bundle + "/Contents/Resources/Snapshots/aaaa_snapshot.png", testPicture(100, 60));
    savePng(bundle + "/Contents/Resources/Snapshots/BBBB_snapshot.png", testPicture(110, 60));
    savePng(bundle + "/Contents/Resources/Snapshots/BBBB_snapshot_2.0x.png", testPicture(220, 120));
    PluginSlot b;
    b.name = "Snap B";
    b.pluginName = "Snap B";
    b.path = bundle + "/Contents/x86_64-linux/Snap.so";
    CHECK(plugin_pictures::bundleOf(b.path) == bundle);
    CHECK(util::endsWith(plugin_pictures::snapshot(b), "BBBB_snapshot_2.0x.png"));
    auto bp = plugin_pictures::picture(b);
    CHECK(bp && bp->width() == 220);
    PluginSlot c = b;
    c.name = c.pluginName = "Snap C";
    CHECK(plugin_pictures::snapshot(c).empty());
}

// --- the FX window, with slots that aren't loaded (and the test plugins) ---------------------------

std::string testPlugins() { return util::absolutePath(check::testDir()); }

void setUpHost(PluginHost& host) {
    host.folders = {testPlugins()};
    host.cachePath = util::joinPath(scratch, "plugin_cache.json");
    host.vst2Folders = [] { return std::vector<std::string>{testPlugins()}; };
}

const plug::PluginInfo* findPlugin(PluginHost& host, const std::string& name) {
    for (auto& p : host.library().plugins)
        if (p.name == name || util::baseName(p.path) == name) return &p;
    return nullptr;
}

void testDialogs() {
    Project project;
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(1280, 800);
    ui::App::get().mainWindow = &main;
    PluginHost host(&main, project, engine);
    setUpHost(host);

    TrackPtr kick = project.addTrack(Kind::Audio);
    kick->name = "Kick";
    TrackPtr bass = project.addTrack(Kind::Audio);
    bass->name = "Bass";
    TrackPtr lead = project.addTrack(Kind::Midi);
    lead->name = "Lead";

    // the effects of Bass: an EQ, a compressor (sidechain from Kick), a delay not wired, a missing one
    SlotPtr eq = fakeSlot("Pro EQ"), comp = fakeSlot("Compressor"), delay = fakeSlot("Delay"), gone = fakeSlot("Gone");
    bass->effects = std::vector<SlotPtr>{eq, comp, delay};
    bass->cables = std::vector<Cable>{Cable(End::in(), End::of(eq.get())), Cable(End::of(eq.get()), End::of(comp.get())),
                                      Cable(End::of(comp.get()), End::out()),
                                      Cable(End::source(kick.get()), End::sidechain(comp.get()))};
    // Lead listens to Bass (so Bass can't take Lead into a sidechain: a loop)
    SlotPtr duck = fakeSlot("Ducker");
    lead->effects = std::vector<SlotPtr>{duck};
    lead->cables = std::vector<Cable>{Cable(End::in(), End::of(duck.get())), Cable(End::of(duck.get()), End::out()),
                                      Cable(End::source(bass.get()), End::sidechain(duck.get()))};
    CHECK(plugin_dialog::listensTo(*lead, bass.get()));
    CHECK(!plugin_dialog::listensTo(*kick, bass.get()));
    CHECK(!plugin_dialog::listensTo(*lead, nullptr));

    int changes = 0;
    auto* fx = new PluginsDialog(&main, host, "Effects: Bass", project, bass, [&] { ++changes; });
    fx->open();
    PatchPanel* panel = fx->panel;
    CHECK(panel->units().size() == 3);
    CHECK(panel->units()[0].status == "Not loaded" && panel->units()[0].problem);
    CHECK(panel->units()[1].sidechain);  // (its cables keep the jack, loaded or not)
    CHECK(panel->sources().size() == 2 && panel->sources()[0].first == "Kick" && !panel->sources()[0].second &&
          panel->sources()[1].first == "Lead" && panel->sources()[1].second);
    CHECK(panel->cables().size() == 4);
    CHECK(panel->units()[0].pos && panel->units()[2].pos && panel->units()[2].pos->x > panel->units()[1].pos->x);

    // plug a cable in: Delay after the compressor, into OUT
    fx->patch(std::nullopt, PanelCable(PanelNode::unit(1), PanelNode::unit(2)));
    fx->patch(std::nullopt, PanelCable(PanelNode::unit(2), PanelNode::out()));
    CHECK(bass->cables.contains(Cable(End::of(comp.get()), End::of(delay.get()))));
    CHECK(changes == 2);
    CHECK(bass->patchState != nullptr);  // (ensureStates made the plan)
    // moved: off OUT, onto... removed
    fx->patch(PanelCable(PanelNode::unit(2), PanelNode::out()), std::nullopt);
    CHECK(!bass->cables.contains(Cable(End::of(delay.get()), End::out())));
    // a unit moved stays there
    fx->unitMoved(2, Point(400, 320));
    CHECK(delay->placed && delay->atX == 400 && delay->atY == 320 && eq->placed);
    // wire every effect in a row (left to right): IN → EQ → Compressor → Delay → OUT
    fx->wireInARow();
    auto expected = chain({eq, comp, delay});
    CHECK(*bass->cables == expected);
    // unplug every cable: IN straight to OUT
    {
        ui::Menu menu;
        fx->fillSpaceMenu(menu, Point(100, 100));
        CHECK(menu.actions().size() == 4 && menu.actions()[0]->text == "Place an effect here…");
        CHECK(menu.actions()[2]->text == "Wire every effect in a row (left to right)" && menu.actions()[2]->enabled);
        CHECK(menu.actions()[3]->enabled);
        menu.actions()[3]->triggered();
        CHECK(*bass->cables == straight());
        ui::Menu again;
        fx->fillSpaceMenu(again, Point(100, 100));
        CHECK(!again.actions()[3]->enabled);
    }
    fx->wireInARow();
    // the unit's menu
    {
        ui::Menu menu;
        fx->fillUnitMenu(menu, 1);
        CHECK(menu.actions().size() == 7 && menu.actions()[3]->text == "Presets" && menu.actions()[3]->submenu);
        CHECK(menu.actions()[0]->text == "Open its window" && !menu.actions()[0]->enabled);  // (not loaded)
        CHECK(menu.actions()[1]->text == "Bypass" && menu.actions()[1]->checkable);
        CHECK(menu.actions()[5]->text == "Unplug its cables" && menu.actions()[5]->enabled);
        menu.actions()[5]->triggered();
        for (auto& c : bass->cables) CHECK(!plugin_dialog::touches(c, comp.get()));
    }
    fx->toggleBypass(0);
    CHECK(eq->bypass && panel->units()[0].bypass);
    fx->toggleBypass(0);
    // remove the selected effect: its cables go too
    fx->wireInARow();
    panel->current = 1;
    fx->removeEffect();
    CHECK(bass->effects.size() == 2 && !bass->effects.contains(comp));
    for (auto& c : bass->cables) CHECK(!plugin_dialog::touches(c, comp.get()));
    // placing an effect of the wrong kind is refused
    // (not shown: msg boxes are modal; place() of the right kind only)
    SlotPtr chorus = fakeSlot("Chorus");
    CHECK(fx->place(chorus, Point(80, 300)));
    CHECK(bass->effects.size() == 3 && chorus->placed && chorus->atX == 80);
    CHECK(panel->current == 2);
    // a sample of the window
    fx->resizeWindow(1000, 700);
    saveWidget(fx, "fx_window.png", true);
    fx->close();
    ui::App::get().collectGarbage();

    // the master's: every track is a source, none blocked
    auto* master = new PluginsDialog(&main, host, "Effects: Master (outputs 1/2)", project, nullptr, [] {});
    CHECK(master->where == "Master");
    CHECK(master->panel->sources().size() == 3);
    for (auto& s : master->panel->sources()) CHECK(!s.second);
    CHECK(master->panel->units().empty());
    saveWidget(master, "master_empty.png", true);
    delete master;

    // a MIDI track's instrument patch panel: the rack rules
    SlotPtr piano = fakeSlot("Piano", plug::INSTRUMENT), strings = fakeSlot("Strings", plug::INSTRUMENT);
    strings->volumeDb = -9.5;
    lead->rack = std::vector<SlotPtr>{piano, strings};
    lead->rackCables = std::vector<Cable>{Cable(End::in(), End::of(piano.get()))};
    lead->usesRack = true;
    auto* rack = new InstrumentPanelDialog(&main, host, "Instruments: Lead", project, lead, [] {});
    CHECK(rack->INSTRUMENTS && rack->panel->instruments && rack->panel->sources().empty());
    CHECK(rack->panel->units()[1].volumeDb && *rack->panel->units()[1].volumeDb == -9.5);
    CHECK(!rack->panel->units()[0].sidechain);
    rack->wireAll();
    CHECK(rack->cables().size() == 4);
    CHECK(rack->cables().contains(Cable(End::of(strings.get()), End::out())));
    rack->volumeChanged(0, -3);
    CHECK(piano->volumeDb == -3);
    {
        ui::Menu menu;
        rack->fillSpaceMenu(menu, Point(50, 50));
        CHECK(menu.actions()[0]->text == "Place an instrument here…");
        CHECK(menu.actions()[2]->text == "Wire every instrument in (IN → each → OUT)");
        CHECK(menu.actions()[3]->text == "Unplug every cable" && menu.actions()[3]->enabled);
        menu.actions()[3]->triggered();
        CHECK(rack->cables().empty());
        ui::Menu unit;
        rack->fillUnitMenu(unit, 0);
        CHECK(unit.actions()[1]->text == "Mute");
    }
    rack->wireAll();
    CHECK(rack->wrongKind(*fakeSlot("Verb")).find("FX button") != std::string::npos);
    rack->resizeWindow(900, 720);
    saveWidget(rack, "instrument_panel.png", true);
    delete rack;

    // the instrument menu: useRack puts the track's one instrument in the panel, wired
    TrackPtr keys = project.addTrack(Kind::Midi);
    keys->name = "Keys";
    SlotPtr synth = fakeSlot("Synth", plug::INSTRUMENT);
    keys->instrument = synth;
    keys->needsInstrument = false;
    int opened = 0, chosen = 0;
    instrument_menu::useRack(&main, host, keys, [&] { ++chosen; }, [&](const TrackPtr&) { ++opened; });
    CHECK(keys->usesRack && keys->rack.size() == 1 && keys->rack[0] == synth && !keys->instrument);
    CHECK(keys->rackCables.size() == 2 && chosen == 1 && opened == 1);
    instrument_menu::useRack(&main, host, keys, [&] { ++chosen; }, [&](const TrackPtr&) { ++opened; });
    CHECK(chosen == 1 && opened == 2);  // (already using it: only opened)
    // back to the built-in synth: the panel stays, not heard
    CHECK(instrument_menu::setInstrument(&main, host, keys, nullptr, [&] { ++chosen; }));
    CHECK(!keys->usesRack && keys->rack.size() == 1 && !keys->instrument && chosen == 2);
    // the menu's entries
    ui::Menu menu;
    instrument_menu::fillInstrumentMenu(&menu, &main, host, keys, [] {}, [](const TrackPtr&) {});
    std::vector<std::string> texts;
    for (auto& a : menu.actions()) texts.push_back(a->separator ? "-" : a->text);
    CHECK(texts.size() >= 3 && texts[0] == instrument_menu::BUILTIN && menu.actions()[0]->checked);
    {  // (the built-in sampler may come before it)
        int panelEntry = -1;
        for (int i = 0; i < (int)texts.size(); ++i)
            if (texts[i] == "Instrument patch panel (1 instrument)…") panelEntry = i;
        CHECK(panelEntry > 0 && !menu.actions()[panelEntry]->checked);
    }
    bool synthListed = false;
    for (auto& a : menu.actions()) {
        if (a->text.find("TinyTestSynth (VST2)") != std::string::npos) synthListed = true;
        if (a->submenu)
            for (auto& b : a->submenu->actions())
                if (b->text.find("TinyTestSynth (VST2)") != std::string::npos) synthListed = true;
    }
    CHECK(synthListed);
    bool effectListed = false;
    for (auto& t : texts)
        if (t.find("Tiny Test Fx") != std::string::npos) effectListed = true;
    CHECK(!effectListed);  // (an effect, known from its moduleinfo.json)
    // frozen: nothing to choose
    ui::Menu frozenMenu;
    TrackPtr ice = project.addTrack(Kind::Midi);
    ice->name = "Ice";
    ice->frozen = std::make_shared<Clip>();
    instrument_menu::fillInstrumentMenu(&frozenMenu, &main, host, ice, [] {}, [](const TrackPtr&) {});
    CHECK(frozenMenu.actions().size() == 1 && !frozenMenu.actions()[0]->enabled &&
          frozenMenu.actions()[0]->text == "Ice is frozen: unfreeze it to change its instrument");

    // the VST2 plugin folders dialog
    {
        PluginFoldersDialog d(&main, {"/a/b/", "/c//d/../e"});
        CHECK(d.folders().size() == 2 && d.folders()[0] == "/a/b/");
        d.add("/a/b/");
        CHECK(d.folders().size() == 3);  // (as Python: the list keeps what it was given; added folders are normalised)
        d.add("/a/b");
        CHECK(d.folders().size() == 3 && d.folders()[2] == "/a/b");
        CHECK(PluginFoldersDialog::normPath("/c//d/../e/") == "/c/e");
        CHECK(!d.removeButton->isEnabled());
        d.list->setCurrentRow(0);
        CHECK(d.removeButton->isEnabled());
        d.removeFolder();
        CHECK(d.folders().size() == 2);
        saveWidget(&d, "vst2_folders.png", true);
    }
    ui::App::get().mainWindow = nullptr;
}

// --- the plugin host with the test plugins -----------------------------------------------------

void testHost() {
    Project project;
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(1280, 800);
    PluginHost host(&main, project, engine);
    setUpHost(host);
    std::vector<std::pair<std::string, int>> statuses;
    host.showStatus = [&](const std::string& t, int ms) { statuses.push_back({t, ms}); };

    // the kind finder: the VST2 synth's kind isn't known until it is looked at
    const plug::PluginInfo* synthInfo = findPlugin(host, "TinyTestSynth.so");
    CHECK(synthInfo && synthInfo->kind.empty());
    CHECK(host.expected(*synthInfo) == "Loading it for the first time can take a little longer.");
    host.findKinds(false);
    CHECK(host.finder != nullptr);
    for (int guard = 0; host.finder && host.finder->poll(200) && guard < 600; ++guard) {}
    CHECK(!statuses.empty() && statuses[0].first == "Finding out the types of 1 plugin…");
    CHECK(!statuses.empty() && statuses.back().first == "Found the types of 1 of 1 plugins");
    CHECK(!host.finder);
    synthInfo = findPlugin(host, "TinyTestSynth.so");
    CHECK(synthInfo && synthInfo->kind == plug::INSTRUMENT);

    // loading: a slot with the plugin loaded, and how long it took
    const plug::PluginInfo* fxInfo = findPlugin(host, "Tiny Test Fx");
    CHECK(fxInfo && fxInfo->kind == plug::EFFECT);
    SlotPtr fx = fxInfo ? host.load(*fxInfo) : nullptr;
    CHECK(fx && fx->kind == plug::EFFECT && fx->instance && fx->pluginName == "Tiny Test Fx");
    if (!fx) return;
    std::string expect = host.expected(*fxInfo);
    CHECK(expect == "Last time it took under a second." || util::startsWith(expect, "Last time it took "));
    CHECK(fx->instance->hasSidechain());
    // how it shows: on and wired, not wired, no input, bypassed, stopped
    auto u = plugin_dialog::unitOf(*fx, true, true);
    CHECK(u.status == "On" && !u.idle && !u.problem && u.sidechain);
    CHECK(u.tooltip.find("sidechain input (SC") != std::string::npos);
    CHECK(plugin_dialog::unitOf(*fx, false, true).status == "Not wired" && plugin_dialog::unitOf(*fx, false, true).idle);
    CHECK(plugin_dialog::unitOf(*fx, true, false).status == "No input");
    fx->bypass = true;
    CHECK(plugin_dialog::unitOf(*fx, true, true).status == "Bypassed");
    fx->bypass = false;
    fx->error = "boom";
    fx->failed = true;
    CHECK(plugin_dialog::unitOf(*fx, true, true).status == "Stopped" && plugin_dialog::unitOf(*fx, true, true).problem);
    CHECK(plugin_dialog::slotLabel(*fx) == "Tiny Test Fx  (stopped after an error: boom)");
    fx->failed = false;
    fx->error.clear();
    CHECK(plugin_dialog::slotLabel(*fx) == "Tiny Test Fx");
    CHECK(plugin_dialog::slotLabel(*fakeSlot("X")) == "X  (not loaded)");
    // instruments: muted, no notes
    SlotPtr synth = synthInfo ? host.load(*synthInfo) : nullptr;
    CHECK(synth && synth->kind == plug::INSTRUMENT);
    if (synth) {
        CHECK(plugin_dialog::unitOf(*synth, true, false, false, true).status == "No notes");
        synth->bypass = true;
        CHECK(plugin_dialog::unitOf(*synth, true, true, false, true).status == "Muted");
        synth->bypass = false;
        CHECK(plugin_dialog::unitOf(*synth, true, true, false, true).volumeDb.has_value());
    }

    // restoring a project's plugins, with their settings; a missing one keeps its settings
    fx->instance->setParam(fx->instance->parameters()[0].id, 0.25);
    plugin_audio::syncState(*fx, engine.sr);  // (the change reaches its processor)
    std::string state = plug::stateOf(*fx->instance);
    auto restored = std::make_shared<PluginSlot>(*fx);
    restored->instance.reset();
    restored->state = state;
    SlotPtr missing = fakeSlot("Missing");
    missing->state = "keep me";
    auto failed = host.restore({restored, missing});
    CHECK(failed.size() == 1 && failed[0] == "Missing");
    CHECK(restored->instance && !missing->instance && missing->state == "keep me");
    if (restored->instance) CHECK_NEAR(restored->instance->getParam(restored->instance->parameters()[0].id), 0.25, 1e-6);
    host.idle({restored, missing, fx});

    // a stopped plugin started afresh (as a Windows plugin whose Wine host crashed is: the test effect
    // stands for one), with its settings, found again by its automation; by itself (Options) at most
    // AUTO_RELOADS times a minute, then it stays stopped
    {
        uint32_t pid = fx->instance->parameters()[0].id;
        host.reloadable = [](const plug::Plugin&) { return true; };
        std::string why;
        CHECK(!host.canReload(fx) && !host.reload(fx, why) && !why.empty());  // (it plays)
        fx->instance->setParam(pid, 0.4);
        plugin_audio::syncState(*fx, engine.sr);
        TrackPtr keys = project.addTrack(Kind::Audio);
        keys->name = "Keys";
        keys->effects = std::vector<SlotPtr>{fx};
        keys->cables = chain({fx});
        auto lane = std::make_shared<Automation>();
        lane->slot = fx;
        lane->param = fx->instance->parameters()[0].name;
        keys->automation = std::vector<AutomationPtr>{lane};
        automation::resolve(project);
        auto before = aload(fx->instance);
        CHECK(aload(lane->targetPlugin) == before && lane->hasTarget);
        plugin_audio::fail(*fx, "boom");
        CHECK(host.canReload(fx) && host.whereOf(fx) == "Keys");
        statuses.clear();
        host.idle({fx});
        host.idle({fx});  // (said once)
        CHECK(statuses.size() == 1 &&
              statuses[0].first == "Tiny Test Fx stopped (boom). Reload it from its effects window or its menu.");
        CHECK(fx->failed);  // (not by itself: the option is off)
        {
            // its effects window: Reload on its picture, and in its menu; a click reloads it
            int changed = 0;
            int conn = host.slotChanged.connect([&](SlotPtr s) { changed += s == fx; });
            PluginsDialog d(&main, host, "Effects: Keys", project, keys, nullptr);
            CHECK(d.panel->units().size() == 1 && d.panel->units()[0].reload && d.panel->reloadRect(0).has_value());
            CHECK(d.panel->units()[0].status == "Stopped");
            d.resizeWindow(700, 420);
            saveWidget(d.panel, "stopped_unit.png");
            ui::Menu menu;
            d.fillUnitMenu(menu, 0);
            CHECK(!menu.actions().empty() && menu.actions()[0]->text == "Reload (it stopped)");
            if (auto r = d.panel->reloadRect(0)) {
                CHECK(d.panel->toolTipAt(r->center()).find("Reload Tiny Test Fx") == 0);
                ui::MouseEvent e = mouse(r->center());
                d.panel->mousePress(e);
                d.panel->mouseRelease(e);
            }
            for (int i = 0; i < 20 && fx->failed; ++i) ui::App::get().advanceTimers(0.01);  // (after the click)
            CHECK(!fx->failed && fx->error.empty() && aload(fx->instance) != before);
            CHECK(!d.panel->units()[0].reload && d.panel->units()[0].status == "On");
            CHECK(!d.panel->reloadRect(0).has_value());
            CHECK(changed >= 1);
            host.slotChanged.disconnect(conn);
        }
        CHECK(!statuses.empty() && statuses.back().first == "Reloaded Tiny Test Fx (it had stopped: boom)");
        CHECK_NEAR(fx->instance->getParam(pid), 0.4, 1e-6);  // (its settings came back)
        CHECK(aload(lane->targetPlugin) == aload(fx->instance) && lane->hasTarget);  // (automation follows)
        // by itself: AUTO_RELOADS times, then it stays stopped (said once)
        host.autoReload = true;
        int reloaded = 0;
        for (int i = 0; i < plugin_host::AUTO_RELOADS + 1; ++i) {
            auto was = aload(fx->instance);
            plugin_audio::fail(*fx, "boom again");
            host.idle({fx});
            if (!fx->failed && aload(fx->instance) != was) ++reloaded;
        }
        CHECK(reloaded == plugin_host::AUTO_RELOADS && fx->failed);
        CHECK(!statuses.empty() && statuses.back().first.find("crashed again: it stays stopped") != std::string::npos);
        size_t said = statuses.size();
        host.idle({fx});
        CHECK(statuses.size() == said);
        CHECK_NEAR(fx->instance->getParam(pid), 0.4, 1e-6);
        // by hand it still can
        CHECK(host.reload(fx, why) && !fx->failed);
        CHECK_NEAR(fx->instance->getParam(pid), 0.4, 1e-6);
        host.autoReload = false;
        // a plugin of Linux can't: it runs in the program's own process
        host.reloadable = nullptr;
        plugin_audio::fail(*fx, "boom");
        CHECK(!host.canReload(fx) && !host.reload(fx, why));
        statuses.clear();
        host.idle({fx});
        CHECK(statuses.size() == 1 && statuses[0].first == "Tiny Test Fx stopped after an error: boom");
        fx->error.clear();
        fx->failed = false;
        keys->automation = std::vector<AutomationPtr>{};
        keys->effects = std::vector<SlotPtr>{};
        keys->cables = straight();
        project.removeTrack(keys);
    }
    // an instrument in an instrument patch panel, and a track's one instrument, alike
    if (synth) {
        host.reloadable = [](const plug::Plugin&) { return true; };
        TrackPtr piano = project.addTrack(Kind::Midi);
        piano->name = "Piano";
        piano->usesRack = true;
        piano->needsInstrument = false;
        piano->rack = std::vector<SlotPtr>{synth};
        piano->rackCables = std::vector<Cable>{Cable(End::in(), End::of(synth.get())), Cable(End::of(synth.get()), End::out())};
        CHECK(host.whereOf(synth) == "Piano");
        auto before = aload(synth->instance);
        plugin_audio::fail(*synth, "boom");
        {
            InstrumentPanelDialog d(&main, host, "Instruments: Piano", project, piano, nullptr);
            CHECK(d.panel->units().size() == 1 && d.panel->units()[0].reload);
            ui::Menu menu;
            d.fillUnitMenu(menu, 0);
            CHECK(!menu.actions().empty() && menu.actions()[0]->text == "Reload (it stopped)");
            std::string why;
            CHECK(host.reload(synth, why));
            CHECK(!d.panel->units()[0].reload && d.panel->units()[0].status != "Stopped");  // (shown at once)
        }
        CHECK(!synth->failed && aload(synth->instance) != before && synth->kind == plug::INSTRUMENT);
        // the track's one instrument: its menus offer Reload
        piano->usesRack = false;
        piano->rack = std::vector<SlotPtr>{};
        piano->rackCables = std::vector<Cable>{};
        std::atomic_store(&piano->instrument, synth);
        plugin_audio::fail(*synth, "boom");
        ui::Menu menu;
        instrument_menu::fillInstrumentMenu(&menu, &main, host, piano, nullptr, nullptr);
        CHECK(!menu.actions().empty() && menu.actions()[0]->text == "Reload " + synth->name + " (it stopped)");
        std::string why;
        CHECK(host.reload(synth, why) && !synth->failed);
        host.reloadable = nullptr;
        std::atomic_store(&piano->instrument, SlotPtr());
        project.removeTrack(piano);
    }

    // the parameter chooser: the track's plugins, the parameters, the recently tweaked first
    TrackPtr t = project.addTrack(Kind::Audio);
    t->name = "Drums";
    t->effects = std::vector<SlotPtr>{fx, missing};
    auto params = automation::parameters(*fx);
    CHECK(params.size() >= 2);
    if (params.size() >= 2) {
        fx->recent = {params[1].name};
        auto lane = std::make_shared<Automation>();
        lane->slot = fx;
        lane->param = params[0].name;
        t->automation = std::vector<AutomationPtr>{lane};
        ParameterDialog d(&main, t);
        CHECK(d.plugins->count() == 3 && d.plugins->itemText(0) == "Track: volume and pan" &&
              d.plugins->itemText(1) == "Effect: Tiny Test Fx" && d.plugins->itemText(2) == "Effect: Missing  (not loaded)");
        d.plugins->setCurrentIndex(1);
        CHECK(d.list->count() == (int)params.size() + 3);  // headings, the recent one, all
        CHECK(d.list->item(0).text == "Recently tweaked" && !d.list->item(0).enabled);
        CHECK(d.list->item(1).data == params[1].name && d.list->currentRow() == 1);
        bool automatedShown = false;
        for (int i = 0; i < d.list->count(); ++i)
            if (d.list->item(i).data == params[0].name && !d.list->item(i).enabled &&
                d.list->item(i).text.find("— automated already") != std::string::npos)
                automatedShown = true;
        CHECK(automatedShown);
        d.resizeWindow(420, 520);
        saveWidget(&d, "parameter_dialog.png", true);
        d.search->setText(util::lower(params[1].name.substr(0, 3)));
        CHECK(d.list->count() >= 1);
        d.accept();
        CHECK(d.chosenSlot == fx && d.chosenParam == params[1].name);
        d.plugins->setCurrentIndex(2);
        CHECK(d.count->text() == "Missing isn't loaded, so its parameters can't be listed.");
    }
    // a unit with a picture: as the patch panel shows the loaded plugin
    plugin_pictures::save(*fx, testPicture(640, 400));
    CHECK(plugin_pictures::hasPicture(*fx));
    project.retire(fx->instance);
    if (synth) project.retire(synth->instance);
    if (restored->instance) project.retire(restored->instance);
}

// --- samples of the panel in every state ---------------------------------------------------------

void testSamples() {
    using N = PanelNode;
    ui::Window win;
    win.resizeWindow(1100, 760);
    auto* panel = win.add<PatchPanel>();
    panel->setGeometry(0, 0, 1100, 760);
    std::vector<PanelUnit> units(5);
    units[0].name = "Pro EQ";
    units[0].picture = testPicture(640, 400);
    units[0].tooltip = "Pro EQ";
    units[1].name = "Bus Compressor";
    units[1].sidechain = true;
    units[1].picture = testPicture(400, 400);
    units[2].name = "Tape Delay";
    units[2].status = "Not wired";
    units[2].idle = true;
    units[3].name = "Plate Reverb";
    units[3].bypass = true;
    units[3].status = "Bypassed";
    units[3].picture = testPicture(500, 300);
    units[4].name = "A plugin with a very long name that is cut";
    units[4].status = "Not loaded";
    units[4].problem = true;
    units[0].pos = Point(88, 30);
    units[1].pos = Point(358, 30);
    units[2].pos = Point(628, 30);
    units[3].pos = Point(88, 288);
    units[4].pos = Point(628, 288);
    std::vector<PanelCable> cables{{N::in(), N::unit(0)},       {N::unit(0), N::unit(1)}, {N::unit(1), N::out()},
                                   {N::track(0), N::side(1)},  {N::in(), N::unit(3)},    {N::unit(3), N::out()}};
    panel->setUnits(units, cables, 1, {{"Kick", false}, {"Lead (listens to this one)", true}, {"Vocals", false}});
    panel->hover = PatchPanel::Hover{PatchPanel::Hover::Led, 0, {}};
    saveWidget(panel, "patch_panel.png");
    // a cable in the hand, from Tape Delay's output: rings where it can go
    panel->hover = PatchPanel::Hover{};
    panel->startWiring({Jack::Source, N::unit(2)}, PointF(900, 600));
    panel->hover = PatchPanel::Hover{};
    saveWidget(panel, "patch_panel_wiring.png");
    panel->wiring.reset();

    // the instrument patch panel, with faders
    auto* inst = win.add<PatchPanel>(true);
    inst->setGeometry(0, 0, 900, 560);
    std::vector<PanelUnit> rack(3);
    rack[0].name = "Piano";
    rack[0].volumeDb = 0.0;
    rack[0].picture = testPicture(600, 300);
    rack[1].name = "Strings";
    rack[1].volumeDb = -12.5;
    rack[1].status = "No notes";
    rack[1].idle = true;
    rack[2].name = "Pad";
    rack[2].volumeDb = (double)config::MIN_DB;
    rack[2].bypass = true;
    rack[2].status = "Muted";
    inst->setUnits(rack, {{N::in(), N::unit(0)}, {N::unit(0), N::out()}, {N::unit(1), N::out()}}, 0);
    inst->hover = PatchPanel::Hover{PatchPanel::Hover::Volume, 1, {}};
    saveWidget(inst, "instrument_panel_faders.png");

    // empty
    auto* empty = win.add<PatchPanel>();
    empty->setGeometry(0, 0, 900, 400);
    empty->setUnits({}, {{N::in(), N::out()}});
    saveWidget(empty, "patch_panel_empty.png");
}

// --- with a display: the test plugins' windows ----------------------------------------------------

// --- the successor's features: the chooser, the Mix fader, built-in effects' windows ------------

const std::string SAMPLES2 = "build/ui-samples/plugins2";

void saveWidget2(ui::Widget* w, const std::string& name) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    w->paintTree(p, w->rect());
    util::makeDirs(SAMPLES2);
    CHECK(savePng(SAMPLES2 + "/" + name, img));
}

plug::PluginInfo vst(const std::string& name, const std::string& kind = plug::EFFECT) {
    return plug::PluginInfo{"/plugins/" + name + ".vst3", name, kind, ""};
}

void testChooser() {
    using namespace plugin_chooser;
    settings::remove(FAVOURITES_KEY);
    settings::remove(RECENT_KEY);
    // categories: a built-in effect's own, else guessed from the name
    CHECK(categoryOf(plug::PluginInfo{"builtin:reverb", "Reverb", plug::EFFECT, ""}) == "Reverb");
    CHECK(categoryOf(plug::PluginInfo{"builtin:multiband_compressor", "Multi-band compressor", plug::EFFECT, ""}) ==
          "Dynamics");
    CHECK(categoryOf(plug::PluginInfo{"builtin:utility", "Utility", plug::EFFECT, ""}) == "Utility");
    CHECK(guessCategory("Pro EQ 3") == "EQ" && guessCategory("Graphic Equalizer") == "EQ");
    CHECK(guessCategory("Bus Compressor") == "Dynamics" && guessCategory("DeEsser") == "Dynamics");
    CHECK(guessCategory("Valhalla VintageVerb") == "Reverb" && guessCategory("Tape Echo") == "Delay");
    CHECK(guessCategory("TAL-Chorus-LX") == "Modulation" && guessCategory("Tube Screamer") == "Distortion");
    CHECK(guessCategory("Wah Filter") == "Filter" && guessCategory("Youlean Loudness Meter") == "Analyzer");
    CHECK(guessCategory("Mystery Box") == "Other");
    CHECK(categoryOf(vst("Surge XT", plug::INSTRUMENT)) == "Instrument");
    CHECK(keyOf(vst("Surge XT")) == "/plugins/Surge XT.vst3|");
    CHECK(categories().size() == 11 && categories().front() == "EQ" && categories().back() == "Other");

    // Add… offers the built-in effects first (the library doesn't list them), then the plugins
    plug::PluginLibrary empty({}, util::joinPath(scratch, "empty_cache.json"));
    auto choices = plugin_dialog::choicesOf(empty, plug::EFFECT);
    CHECK(choices.size() == effects::list().size() && !choices.empty() && effects::isBuiltin(choices[0].path));
    CHECK(plugin_dialog::choicesOf(empty, plug::INSTRUMENT).empty());
    for (auto& v : {vst("Valhalla VintageVerb"), vst("Bus Compressor"), vst("Tube Screamer"), vst("Mystery Box"),
                    vst("Pro EQ 3")})
        choices.push_back(v);

    ui::Window main;
    main.resizeWindow(1000, 700);
    {
        PluginChooser c(&main, choices);
        CHECK(c.title() == "Add effect");
        CHECK(c.category->currentText() == ALL);  // (nothing used yet)
        CHECK(c.category->itemText(0) == ALL && c.category->itemText(1) == FAVOURITES &&
              c.category->itemText(2) == RECENT && c.category->itemText(3) == BUILTIN);
        CHECK(c.category->findText("Instrument") < 0 && c.category->findText("Reverb") > 0 &&
              c.category->findText("Other") > 0);
        CHECK((int)c.shown().size() == (int)choices.size() && c.list->count() == (int)choices.size());
        // sorted by label: "Bit-crusher  (built-in)" before "Bus Compressor"
        CHECK(c.list->item(0).text.find("☆  ") == 0 && c.list->item(0).text.find("· ") != std::string::npos);
        // the search: every word, in the name, the category or "built-in"
        c.search->setText("verb");
        auto found = c.shown();
        CHECK(found.size() == 3 && found[0]->name == "Reverb" && found[1]->name == "Space" &&
              found[2]->name == "Valhalla VintageVerb");  // (Space: its category, Reverb)
        c.search->setText("dynamics built-in");
        for (auto* i : c.shown()) CHECK(effects::isBuiltin(i->path) && categoryOf(*i) == "Dynamics");
        CHECK(c.shown().size() == 5);  // compressor, multi-band, gate, limiter, transient shaper
        c.search->setText("nothing like this");
        CHECK(c.shown().empty() && c.list->count() == 1 && !c.list->item(0).enabled &&
              c.list->item(0).text == "Nothing here: try other words, or All" && !c.current());
        c.search->setText("");
        c.category->setCurrentIndex(c.category->findText(BUILTIN));
        CHECK(c.shown().size() == effects::list().size());
        c.category->setCurrentIndex(c.category->findText("Distortion"));
        CHECK(c.shown().size() == 3);  // Bit-crusher, Saturation, Tube Screamer
        c.category->setCurrentIndex(c.category->findText(FAVOURITES));
        CHECK(c.shown().empty() && c.list->item(0).text == "Nothing here: no favourites yet (☆ makes one)");
        // ☆: a favourite (remembered), listed first
        c.category->setCurrentIndex(0);
        c.search->setText("tube");
        CHECK(c.current() && c.current()->name == "Tube Screamer");
        c.toggleFavourite();
        CHECK(favouritesText() == "/plugins/Tube Screamer.vst3|");
        c.search->setText("");
        CHECK(c.shown()[0]->name == "Tube Screamer" && c.list->item(0).text.find("★  ") == 0);
        // a click on a row's star
        ui::App::get().doLayouts();
        int row = 2;
        std::string name2 = c.shown()[row]->name;
        auto press = mouse(Point(10, row * c.list->rowHeight + c.list->rowHeight / 2));
        c.list->mousePress(press);
        CHECK(c.favourites.count(keyOf(*c.current())) && c.current()->name == name2);
        CHECK(keysIn(favouritesText()).size() == 2);
        // F on the list: off again
        ui::KeyEvent f;
        f.key = 'F';
        f.text = "f";
        c.list->keyPress(f);
        CHECK(keysIn(favouritesText()).size() == 1);
        // Down in the search box moves the highlight
        c.list->setCurrentRow(0);
        ui::KeyEvent down;
        down.key = plat::Key_Down;
        c.search->keyPress(down);
        CHECK(c.list->currentRow() == 1);
        c.category->setCurrentIndex(c.category->findText(FAVOURITES));
        CHECK(c.shown().size() == 1 && c.shown()[0]->name == "Tube Screamer");
        c.category->setCurrentIndex(0);
        c.search->setText("bus");
        auto chosen = c.chosen();
        CHECK(chosen && chosen->name == "Bus Compressor");
        CHECK(keysIn(recentText()) == std::vector<std::string>{"/plugins/Bus Compressor.vst3|"});
        c.search->setText("");
        c.list->setCurrentRow(0);
        saveWidget2(&c, "chooser.png");
    }
    {
        // opened again: on Recently used, newest first; at most RECENT_KEPT kept
        PluginChooser c(&main, choices);
        CHECK(c.category->currentText() == RECENT && c.shown().size() == 1);
        for (auto& info : choices) {
            c.category->setCurrentIndex(0);
            c.search->setText(info.name);
            for (int r = 0; r < c.list->count(); ++r)
                if (c.shown()[r]->name == info.name) c.list->setCurrentRow(r);
            c.chosen();
        }
        auto recent = keysIn(recentText());
        CHECK((int)recent.size() == RECENT_KEPT && recent[0] == keyOf(choices.back()));
        c.search->setText("");
        c.category->setCurrentIndex(c.category->findText(RECENT));
        CHECK((int)c.shown().size() == RECENT_KEPT && c.shown()[0]->name == choices.back().name);
        saveWidget2(&c, "chooser_recent.png");
    }
    {
        // instruments: no built-ins, the Instrument category
        std::vector<plug::PluginInfo> synths = {vst("Surge XT", plug::INSTRUMENT), vst("Vital", plug::INSTRUMENT)};
        PluginChooser c(&main, synths, "instrument");
        CHECK(c.title() == "Add instrument" && c.category->findText("Instrument") > 0 && c.shown().size() == 2);
        CHECK(c.search->placeholder() == "Search instruments by name or kind (Ctrl+F)…");
    }
}

void testMixFader() {
    using N = PanelNode;
    ui::Window win;
    win.resizeWindow(900, 600);
    auto* panel = win.add<PatchPanel>();
    panel->setGeometry(0, 0, 900, 600);
    CHECK(panel->cardH == patch_panel::CARD_H + patch_panel::VOLUME_H);  // (every unit has a fader now)
    std::vector<PanelUnit> units(2);
    units[0].name = "Reverb";
    units[0].mix = 1.0;
    units[1].name = "Delay";
    units[1].mix = 0.35;
    panel->setUnits(units, {{N::in(), N::unit(0)}, {N::unit(0), N::out()}}, 0);
    CHECK(panel->volumeRect(0).has_value() && panel->units()[0].fader() == 1.0);
    auto [x0, x1] = panel->groove(0);
    CHECK(panel->valueWidth() >= patch_panel::VOLUME_TEXT_W);
    CHECK_NEAR(panel->volumeAt(0, x0 - 20), 0.0, 1e-12);
    CHECK_NEAR(panel->volumeAt(0, x1 + 20), 1.0, 1e-12);
    double half = panel->volumeAt(0, (x0 + x1) / 2 + 0.3);
    CHECK(std::fabs(half - 0.5) <= 0.011 && std::fabs(half * 100 - std::round(half * 100)) < 1e-9);  // whole percent
    CHECK_NEAR(panel->volumeX(0, 0.0), x0, 1e-9);
    CHECK_NEAR(panel->volumeX(0, 1.0), x1, 1e-9);
    std::vector<std::pair<int, double>> changes;
    panel->volumeChanged.connect([&](int i, double v) { changes.push_back({i, v}); });
    Rect vr = *panel->volumeRect(1);
    ui::WheelEvent w;
    w.pos = vr.center();
    w.dy = 1;
    CHECK(panel->wheel(w));  // 5 % a notch
    CHECK(!changes.empty() && changes.back().first == 1 && std::fabs(changes.back().second - 0.4) < 1e-9);
    w.dy = -1;
    panel->wheel(w);
    panel->wheel(w);
    CHECK(std::fabs(changes.back().second - 0.3) < 1e-9);
    auto press = mouse(Point((int)x0 - 4, vr.center().y));  // (the knob to the left end: 0 %)
    panel->mousePress(press);
    CHECK(changes.back().second == 0.0);
    auto release = mouse(Point((int)x0 - 4, vr.center().y));
    panel->mouseRelease(release);
    auto dbl = mouse(vr.center());
    panel->mouseDoubleClick(dbl);  // back to 100 %
    CHECK(changes.back().second == 1.0 && panel->units()[1].mix == 1.0);
    panel->setVolume(1, 1.7);  // (kept within 0…1)
    CHECK(panel->units()[1].mix == 1.0);
    CHECK(panel->toolTipAt(vr.center()).find("Delay's Mix: how much of its sound") == 0);
    // an instrument panel's faders are still volumes
    PatchPanel instruments(true);
    std::vector<PanelUnit> synths(1);
    synths[0].name = "Piano";
    synths[0].volumeDb = -6.0;
    instruments.setUnits(synths, {});
    CHECK(instruments.units()[0].fader() == -6.0 && !instruments.units()[0].mix);

    // the effects window: a unit's Mix is its slot's
    Project project;
    TrackPtr bass = project.addTrack(Kind::Audio);
    SlotPtr comp = fakeSlot("Compressor");
    comp->mix = 0.6;
    PanelUnit u = plugin_dialog::unitOf(*comp, true, true);
    CHECK(u.mix && *u.mix == 0.6 && !u.volumeDb);
    PanelUnit iu = plugin_dialog::unitOf(*comp, true, true, false, true);
    CHECK(iu.volumeDb && !iu.mix);
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(1280, 800);
    PluginHost host(&main, project, engine);
    setUpHost(host);
    bass->effects = std::vector<SlotPtr>{comp};
    bass->cables = chain({comp});
    auto* fx = new PluginsDialog(&main, host, "Effects: Bass", project, bass, [] {});
    fx->open();
    CHECK(fx->panel->units().size() == 1 && fx->panel->units()[0].mix == 0.6);
    fx->volumeChanged(0, 0.25);
    CHECK(comp->mix == 0.25);
    // returns aren't offered as sidechain sources
    TrackPtr ret = project.addTrack(Kind::Return);
    TrackPtr kick = project.addTrack(Kind::Audio);
    kick->name = "Kick";
    fx->refresh();
    CHECK(fx->panel->sources().size() == 1 && fx->panel->sources()[0].first == "Kick");
    fx->close();
    ui::App::get().processEvents();

    // a sample: faders at 100 %, 35 %, an instrument's volume
    units[1].mix = 0.35;
    units[0].picture = testPicture(480, 300);
    panel->setUnits(units, {{N::in(), N::unit(0)}, {N::unit(0), N::unit(1)}, {N::unit(1), N::out()}}, 1);
    panel->hover = PatchPanel::Hover{PatchPanel::Hover::Volume, 1, {}};
    saveWidget2(panel, "mix_faders.png");
    instruments.setGeometry(0, 0, 600, 400);
    saveWidget2(&instruments, "instrument_volume.png");
}

void testBuiltinWindows() {
    Project project;
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(1280, 800);
    ui::App::get().mainWindow = &main;
    PluginHost host(&main, project, engine);
    setUpHost(host);
    TrackPtr bass = project.addTrack(Kind::Audio);
    bass->name = "Bass";
    SlotPtr eq = host.load(plug::PluginInfo{"builtin:eq", "EQ", plug::EFFECT, ""});
    CHECK(eq && eq->kind == plug::EFFECT && eq->path == "builtin:eq");
    if (!eq) return;
    bass->effects = std::vector<SlotPtr>{eq};
    bass->cables = chain({eq});
    // a built-in effect's window is its EffectWindow, not a plugin's own window
    host.edit(eq, "Bass");
    CHECK(host.editors.size() == 1 && !host.changed());  // (opening it changes nothing)
    EffectWindow* w = host.effectWindowOf(eq);
    CHECK(w && !host.editorOf(eq));
    if (w) {
        CHECK(w->title() == "EQ — Bass" && !w->controls.empty());
        Image shot = host.pictureOf(w);
        CHECK(!shot.empty() && shot.width() == w->width());
        util::makeDirs(SAMPLES2);
        savePng(SAMPLES2 + "/effect_window_eq.png", shot);
        host.edit(eq, "Bass");  // (open already: brought forward)
        CHECK(host.editors.size() == 1);
    }
    host.closeEditor({eq});
    CHECK(host.editors.empty() && host.windowPosition(eq).has_value());
    ui::App::get().processEvents();
    // its picture for the patch panel: what its window shows, taken without showing it
    SlotPtr comp = host.load(plug::PluginInfo{"builtin:compressor", "Compressor", plug::EFFECT, ""});
    if (comp) util::removeFile(plugin_pictures::fileFor(*comp));  // (from an earlier run)
    CHECK(comp && !plugin_pictures::hasPicture(*comp));
    int taken = 0;
    host.pictureTaken.connect([&](SlotPtr) { ++taken; });
    CHECK(comp && host.photograph(comp) && taken == 1 && plugin_pictures::hasPicture(*comp));
    if (comp) {
        auto pic = plugin_pictures::picture(*comp);
        CHECK(pic && pic->width() <= plugin_pictures::WIDTH);
        PanelUnit u = plugin_dialog::unitOf(*comp, true, true);
        CHECK(!u.picture.empty());
        PatchPanel panel;
        panel.setGeometry(0, 0, 700, 360);
        bass->effects = std::vector<SlotPtr>{eq, comp};
        std::vector<PanelUnit> units{plugin_dialog::unitOf(*eq, true, true), u};
        units[0].pos = Point(patch_panel::END_W + patch_panel::GAP, panel.top());
        units[1].pos = Point(patch_panel::END_W + 2 * patch_panel::GAP + patch_panel::CARD_W, panel.top());
        panel.setUnits(units, {{PanelNode::in(), PanelNode::unit(0)}, {PanelNode::unit(0), PanelNode::unit(1)},
                               {PanelNode::unit(1), PanelNode::out()}}, 1);
        saveWidget2(&panel, "builtin_units.png");
    }
    // the automation chooser lists the track's own volume and pan first
    {
        ParameterDialog d(&main, bass);
        CHECK(d.plugins->count() == 3 && d.plugins->itemText(0) == "Track: volume and pan");
        CHECK(d.plugins->itemText(1) == "Effect: EQ" && d.slots[0] == bass->controls);
        CHECK(d.list->count() >= 2);
        bool volume = false, pan = false;
        for (int i = 0; i < d.list->count(); ++i) {
            volume = volume || d.list->item(i).data == "Volume";
            pan = pan || d.list->item(i).data == "Pan";
        }
        CHECK(volume && pan);
        saveWidget2(&d, "automation_track_controls.png");
    }
    project.retire(eq->instance);
    if (comp) project.retire(comp->instance);
}

int displayTests() {
    ui::App::get().init(0, nullptr);
    if (!plat::hasDisplay()) {
        printf("(no display)\n");
        return 0;
    }
    Project project;
    audio::Engine engine(project);
    ui::Window main;
    main.resizeWindow(800, 500);
    PluginHost host(&main, project, engine);
    setUpHost(host);
    CHECK(plugin_windows::available());
    const plug::PluginInfo* info = findPlugin(host, "Tiny Test Fx");
    CHECK(info != nullptr);
    if (!info) return 1;
    SlotPtr fx = host.load(*info);
    CHECK(fx != nullptr);
    if (!fx) return 1;
    // a picture, taken offscreen (the window is never seen)
    std::remove(plugin_pictures::fileFor(*fx).c_str());
    int taken = 0;
    host.pictureTaken.connect([&](SlotPtr) { ++taken; });
    double t0 = plat::now();
    bool ok = host.photograph(fx);
    printf("photographed: %s in %.2f s\n", ok ? "yes" : "no", plat::now() - t0);
    CHECK(ok && taken == 1 && plugin_pictures::hasPicture(*fx));
    if (auto pic = plugin_pictures::picture(*fx)) {
        util::makeDirs(SAMPLES);
        savePng(SAMPLES + "/photographed_TinyTestFx.png", *pic);
        printf("picture %dx%d\n", pic->width(), pic->height());
    }
    // an offscreen editor window: attached, sized by the plugin, idle every frame, resized
    {
        auto* w = new EditorWindow(&main, fx, "Tiny Test Fx — Bass", true);
        CHECK(w->attach());
        CHECK(fx->instance->editorOpen());
        printf("editor %dx%d resizable=%d\n", w->width(), w->height(), (int)w->resizable);
        CHECK(w->width() > 10 && w->height() > 10);
        // files dragged onto it: only a Windows plugin's are taken here (a Linux plugin's view takes
        // its own drops, through XDND)
        {
            ui::DragData files;
            files.files = {"/tmp/kick.wav"};
            ui::DragEvent de;
            de.data = &files;
            w->dragEnter(de);
            CHECK(w->acceptsDrops() && !de.accepted);
        }
        w->showWindow();
        for (int i = 0; i < 5; ++i) {
            ui::App::get().processEvents();
            w->idle();
        }
        if (w->resizable) {
            w->resizeWindow(w->width() + 40, w->height() + 30);
            ui::App::get().processEvents();
        }
        bool closed = false;
        w->editorClosed.connect([&](SlotPtr) { closed = true; });
        w->close();
        CHECK(closed && !fx->instance->editorOpen());
        ui::App::get().processEvents();
    }
    // a resizable window bigger than the screen: made to fit the screen, the plugin asked to take that size
    {
        auto big = std::make_shared<BigEditor>();
        SlotPtr slot = fakeSlot("Big");
        slot->instance = big;
        auto* w = new EditorWindow(&main, slot, "Big");  // (not shown: not a photograph)
        CHECK(w->attach());
        Rect screen = plat::primaryScreenGeometry();
        printf("a 5000x4000 window: %dx%d (the screen %dx%d), the plugin asked %d times\n", w->width(), w->height(),
               screen.w, screen.h, (int)big->asked.size());
        CHECK(!big->asked.empty() && w->width() < 5000 && w->height() < 4000);
        CHECK(w->width() == big->w && w->height() == big->h);  // (the size it took)
        if (!screen.empty()) CHECK(w->width() <= screen.w && w->height() <= screen.h);
        // shown low on the screen, it grows bigger than the screen (a zoom chosen in it): kept on the screen
        w->showWindow();
        ui::App::get().processEvents();
        Rect area = ui::App::get().screenAt(ui::App::get().cursorPos());
        w->moveWindow(area.x + area.w / 2, area.bottom() - 120);
        ui::App::get().processEvents();
        big->w = 5000, big->h = 4000, big->wants = true;
        w->idle();
        ui::App::get().processEvents();
        Rect outer = w->outerScreenGeometry();
        area = ui::App::get().screenAt(Point(outer.x + outer.w / 2, outer.y + 16));
        printf("grown: %dx%d at %d,%d (the screen's free area %dx%d at %d,%d)\n", outer.w, outer.h, outer.x, outer.y, area.w,
               area.h, area.x, area.y);
        CHECK(outer.x >= area.x && outer.y >= area.y && outer.right() <= area.right() && outer.bottom() <= area.bottom());
        w->close();
        ui::App::get().processEvents();
        slot->instance.reset();
    }
    // the real thing (shown for a moment, closed at once): titled after the plugin and the track. Opened and
    // closed with nothing changed in it, the project has no changes to save
    host.edit(fx, "Bass");
    CHECK(host.editors.size() == 1 && !host.changed());
    if (!host.editors.empty()) {
        ui::Window* w = host.editors.begin()->second;
        CHECK(w->title() == "Tiny Test Fx — Bass");
        ui::App::get().processEvents();
    }
    host.closeEditor({fx});
    ui::App::get().processEvents();
    CHECK(!host.changed());
    // a knob turned in it: a change, still one once the window is closed (and none once saved)
    host.edit(fx, "Bass");
    if (auto instance = aload(fx->instance)) {
        auto params = instance->parameters();
        CHECK(!params.empty());
        if (!params.empty()) instance->windowEdit(params[0].id, instance->getParam(params[0].id) < 0.5 ? 0.9 : 0.1);
        double until = plat::now() + 3 * plugin_host::TWEAK_MS / 1000.0;
        ui::App::get().runUntil([&] { return plat::now() > until; });
    }
    CHECK(host.changed());
    host.closeEditor({fx});
    ui::App::get().processEvents();
    CHECK(host.changed());
    host.saved();
    CHECK(!host.changed());
    host.edit(fx, "Bass");
    // stopped while open (as a Windows plugin whose host crashed: the test effect stands for one): its
    // window says so, with Reload, which opens it again on the plugin started afresh
    host.reloadable = [](const plug::Plugin&) { return true; };
    host.closeEditor({fx});
    ui::App::get().processEvents();
    host.edit(fx, "Bass");
    if (auto* w = host.editorOf(fx)) {
        CHECK(w->reloadable);
        auto before = aload(fx->instance);
        plugin_audio::fail(*fx, "its Wine host has ended");
        w->idle();
        CHECK(w->stoppedShown());
        saveWidget(w, "stopped_window.png", true);
        int asked = 0;
        w->reloadRequested.connect([&](SlotPtr) { ++asked; });
        w->reloadRequested.emit(fx);  // (as its Reload button does)
        CHECK(asked == 1);
        ui::App::get().processEvents();
        CHECK(!fx->failed && aload(fx->instance) != before);
        EditorWindow* again = host.editorOf(fx);
        CHECK(again != nullptr && again != w);
        CHECK(again && !again->stoppedShown() && again->title() == "Tiny Test Fx");  // (in no track here)
        project.retire(before);
    } else {
        CHECK(false);
    }
    host.closeEditor({fx});
    CHECK(host.editors.empty() && host.windowPosition(fx).has_value());
    ui::App::get().processEvents();
    project.retire(fx->instance);
    return 0;
}

// a stopped Windows plugin's window: its name said once (a reason, the watchdog's, starts with it too), and
// never narrower than its stylesheet's min-width, where a long word of the reason was cut
void testStoppedWindow() {
    ui::Window main;
    auto slot = std::make_shared<PluginSlot>();
    slot->name = "Some Plugin With A Long Name (x64)";
    slot->error = slot->name + " didn't answer for 10 s; its exbit-wine-host.exe hung, and was ended";
    CHECK(plugin_audio::whyStopped(*slot) == "didn't answer for 10 s; its exbit-wine-host.exe hung, and was ended");
    PluginSlot other;
    other.name = "Fx";
    other.error = "Fxy broke";
    CHECK(plugin_audio::whyStopped(other) == "Fxy broke");
    auto* w = new EditorWindow(&main, slot, slot->name);
    w->resizable = true;  // (a plugin that takes any size: no minimum of its own)
    w->reloadable = true;
    w->showWindow();
    w->showStopped();
    ui::App::get().doLayouts();
    std::string said;
    for (auto& c : w->children())
        if (auto* l = dynamic_cast<ui::Label*>(c.get())) said += l->text();
    CHECK(said.find(slot->name) == 0 && said.find(slot->name, 1) == std::string::npos);
    const css::Style& st = w->computedStyle("stopped");
    CHECK(w->minimumSize().x >= ui::style::px(st, "min-width", 360) &&
          w->minimumSize().y >= ui::style::px(st, "min-height", 140));
    w->close();
    ui::App::get().processEvents();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--probe") return plug::runProbe(argv[2]);  // (the kind finder's process)
    scratch = util::absolutePath("build/tests/ui-plugins-scratch");
    util::makeDirs(scratch);
    if (!(argc > 1 && std::string(argv[1]) == "--display"))
        util::removeFile(util::joinPath(scratch, "plugin_cache.json"));  // (every run finds the kinds afresh)
    plugin_pictures::setFolder(util::joinPath(scratch, "pictures"));
    settings::setPath(util::joinPath(scratch, "settings.json"));
    if (argc > 1 && std::string(argv[1]) == "--display") {
        displayTests();
        TEST_MAIN_END();
    }
    ui::App::get().initHeadless();
    testWiringRules();
    testInstrumentRules();
    testPictures();
    testSamples();
    testDialogs();
    testHost();
    testChooser();
    testMixFader();
    testBuiltinWindows();
    testStoppedWindow();
    const char* display = getenv("DISPLAY");
    if (display && *display && !getenv("EXBIT_NO_WINDOWS")) {
        std::string cmd = "'" + std::string(argv[0]) + "' --display";
        int rc = system(cmd.c_str());
        CHECK(rc == 0);
    }
    TEST_MAIN_END();
}
