// A return track's row (lane_panel_return.cpp) and its fader on the meter (meter_fader.h): the layout of the
// picture (name and ✕; output, mute ✕, pan; Auto, FX, the fader on the meter, dB, loudness), every control doing
// what the other rows' do, and the worst case: many returns, every height and width, meters running, faders dragged.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <typeinfo>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "tui/app.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/main_window.h"
#include "ui/master_strip.h"
#include "ui/meter_fader.h"
#include "ui/skin.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"
#include "ui/widgets.h"

namespace {

std::string outDir() {
    std::string d = "build/ui-samples/return-row";
    util::makeDirs(d);
    return d;
}

template <class T>
std::vector<T*> findAll(ui::Widget* root) {
    std::vector<T*> out;
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            if (auto* t = dynamic_cast<T*>(c.get())) out.push_back(t);
            walk(c.get());
        }
    };
    walk(root);
    return out;
}

Image paintWidget(ui::Widget* w) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), theme::c("window"));
    Painter p(img);
    w->paintTree(p, w->rect());
    return img;
}

// a widget's rect in `root`'s coordinates
Rect inRoot(ui::Widget* w, ui::Widget* root) {
    Rect r = w->geometry();
    for (ui::Widget* p = w->parent(); p && p != root; p = p->parent())
        r = Rect(r.x + p->geometry().x, r.y + p->geometry().y, r.w, r.h);
    return r;
}

// every visible control of the row inside it (nothing cut off at the bottom or the right)
bool fitsInside(ui::Widget* frame, std::string* why = nullptr) {
    bool ok = true;
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            if (!c->isVisible()) continue;
            Rect r = inRoot(c.get(), frame);
            if (r.w > 0 && r.h > 0 && (r.x < 0 || r.y < 0 || r.x + r.w > frame->width() || r.y + r.h > frame->height())) {
                ok = false;
                if (why)
                    *why += std::string(typeid(*c).name()) + " at " + std::to_string(r.x) + "," + std::to_string(r.y) +
                            " " + std::to_string(r.w) + "x" + std::to_string(r.h) + "; ";
            }
            walk(c.get());
        }
    };
    walk(frame);
    return ok;
}

ui::MouseEvent mouse(int x, int y, int mods = 0) {
    ui::MouseEvent e;
    e.pos = Point(x, y);
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    e.mods = mods;
    return e;
}

struct Panel {
    Project project;
    audio::Engine engine{project};
    ui::Window win{plat::WindowOptions{}};
    Timeline* timeline = nullptr;
    std::unique_ptr<MasterStrip> master;
    LanePanel* panel = nullptr;
    int fxOpened = 0;
    explicit Panel(int returns, int width = 300) {
        engine.channels = 4;
        for (int i = 0; i < 2; i++) project.addTrack(Kind::Audio);
        for (int i = 0; i < returns; i++)
            project.addTrack(Kind::Return)->name = i == 0 ? "Delay" : "Return " + std::to_string(i);
        win.resizeWindow(width + 700, 900);
        auto* scroll = win.add<ui::ScrollArea>();
        scroll->setGeometry(0, 0, 700, 900);
        timeline = scroll->makeContent<Timeline>(project, engine, scroll);
        master = std::make_unique<MasterStrip>(engine, [] {});
        panel = win.add<LanePanel>(project, *timeline, *master, engine.channels, [this](const TrackPtr&) { ++fxOpened; },
                                   [](ui::Menu&, const TrackPtr&) {},
                                   LanePanel::Instruments{[](ui::Menu&, const TrackPtr&) {}, [](const TrackPtr&) {}});
        panel->setGeometry(700, 0, width, 900);
        win.showWindow();
        ui::App::get().doLayouts();
    }
    TrackPtr firstReturn() { return project.returns().front(); }
};

}  // namespace

static void testLayout() {
    Panel t(1);
    TrackPtr ret = t.firstReturn();
    ui::Widget* frame = t.panel->frames[ret];
    CHECK(frame != nullptr);
    if (!frame) return;
    // the parts, where the picture has them
    auto faders = findAll<MeterFader>(frame);
    auto knobs = findAll<PanKnob>(frame);
    auto displays = findAll<LoudnessDisplay>(frame);
    auto combos = findAll<ui::ComboBox>(frame);
    CHECK(faders.size() == 1 && knobs.size() == 1 && displays.size() == 1 && combos.size() == 1);
    ui::Button *remove = nullptr, *mute = nullptr, *fx = nullptr, *autoB = nullptr;
    for (auto* b : findAll<ui::Button>(frame)) {
        if (b->text() == "✕" && !b->isCheckable()) remove = b;
        if (b->text() == "✕" && b->isCheckable()) mute = b;
        if (b->text() == "FX") fx = b;
        if (b->text().empty() && b->isCheckable()) autoB = b;
    }
    CHECK(remove && mute && fx && autoB);
    DbReading* reading = nullptr;
    for (auto* l : findAll<DbReading>(frame)) reading = l;
    CHECK(reading != nullptr);
    if (!remove || !mute || !fx || !autoB || faders.empty() || knobs.empty() || !reading || displays.empty()) return;
    Rect rRemove = inRoot(remove, frame), rMute = inRoot(mute, frame), rFx = inRoot(fx, frame), rAuto = inRoot(autoB, frame);
    Rect rFader = inRoot(faders[0], frame), rPan = inRoot(knobs[0], frame), rOut = inRoot(combos[0], frame);
    Rect rRead = inRoot(reading, frame), rLufs = inRoot(displays[0], frame);
    // row 1: ✕ at the top right; row 2: output, mute, pan; row 3: Auto, FX, fader, dB, loudness
    CHECK(rRemove.x + rRemove.w > frame->width() - 40 && rRemove.y < rOut.y);
    CHECK(rOut.x < rMute.x && rMute.x < rPan.x);
    CHECK(rOut.y + rOut.h <= rAuto.y + 2);
    CHECK(rAuto.x < rFx.x && rFx.x < rFader.x && rFader.x + rFader.w <= rRead.x && rRead.x < rLufs.x);
    CHECK(rFader.w * 10 >= frame->width() * 3);  // (the meter takes the room: a third, as in the picture)
    std::string why;
    CHECK(fitsInside(frame, &why));
    if (!why.empty()) printf("  outside the row: %s\n", why.c_str());
    CHECK(frame->height() == ret->height && ret->height >= config::RETURN_MIN_H);
    savePng(outDir() + "/return-row.png", paintWidget(frame));
    // the meter's scale and the fader's agree: 0 dB at the bars' right edge, -60 at their left
    MeterFader* f = faders[0];
    Rect bars = f->meter()->barsArea();
    int barsLeft = f->meter()->geometry().x + bars.x;
    CHECK(std::abs(f->handleX(config::MIN_DB) - barsLeft) <= 1);
    CHECK(std::abs(f->handleX(0) - (barsLeft + bars.w)) <= 1);
    CHECK(f->handleX(config::MAX_DB) <= f->width());
    CHECK(std::abs(f->dbAt(f->handleX(-24)) + 24) < 1.0);
    CHECK(f->meter()->barsW() == f->meter()->barsArea().w + (f->meter()->barsW() - f->meter()->barsArea().w));
    CHECK(t.panel->meters[ret] == f->meter() && t.panel->loudness[ret] == displays[0]);
    CHECK(t.panel->mixers[ret].slider == f && t.panel->mixers[ret].pan == knobs[0]);
}

// the row in every skin, pictured (each skin's own colours: the picture's only gave the layout)
static void testSkins() {
    skin::setBuiltinDir("data/skins");
    skin::setUserDir(outDir() + "/skins");
    util::makeDirs(outDir() + "/skins");
    CHECK(skin::library().size() >= 9);
    for (const skin::Skin& sk : skin::library()) {
        CHECK(skin::apply(sk).empty());
        Panel t(1);
        TrackPtr ret = t.firstReturn();
        ui::Widget* frame = t.panel->frames[ret];
        CHECK(frame && fitsInside(frame));
        if (frame) savePng(outDir() + "/return-row-" + sk.name + ".png", paintWidget(frame));
    }
    if (!skin::library().empty()) skin::apply(skin::library().front());
}

static void testControls() {
    Panel t(2);
    TrackPtr ret = t.firstReturn(), other = t.project.returns()[1];
    MeterFader* f = findAll<MeterFader>(t.panel->frames[ret])[0];
    // a click on the meter sets the volume there; a drag moves it on; double-click: 0 dB
    int pressed = 0, released = 0;
    f->sliderPressed.connect([&] { ++pressed; });
    f->sliderReleased.connect([&] { ++released; });
    ui::MouseEvent e = mouse(f->handleX(-24), f->height() - 3);
    f->mousePress(e);
    CHECK(ret->volumeDb == -24 && pressed == 1);
    e = mouse(f->handleX(-12), f->height() - 3);
    f->mouseMove(e);
    CHECK(ret->volumeDb == -12);
    f->mouseRelease(e);
    CHECK(released == 1);
    // on the handle it is held where it is; Shift: a tenth as far
    e = mouse(f->handleX(-12), 2);
    f->mousePress(e);
    CHECK(ret->volumeDb == -12);
    e = mouse(f->handleX(-12) + (f->handleX(-2) - f->handleX(-12)), 2, plat::ModShift);
    f->mouseMove(e);
    CHECK(ret->volumeDb == -11);
    f->mouseRelease(e);
    e = mouse(5, 5);
    f->mouseDoubleClick(e);
    CHECK(ret->volumeDb == 0);
    // past 0 dB to the most, and clamped at both ends
    e = mouse(f->width() + 50, 5);
    f->mousePress(e);
    CHECK(ret->volumeDb == config::MAX_DB);
    e = mouse(-500, 5);
    f->mouseMove(e);
    CHECK(ret->volumeDb == config::MIN_DB);
    f->mouseRelease(e);
    // the wheel does nothing (scrolling the panel never changes a volume)
    ui::WheelEvent w;
    CHECK(!f->wheel(w));
    // mute ✕, on both selected returns
    t.panel->select(ret);
    t.panel->select(other, plat::ModCtrl);
    ui::Button* mute = nullptr;
    for (auto* b : findAll<ui::Button>(t.panel->frames[ret]))
        if (b->text() == "✕" && b->isCheckable()) mute = b;
    CHECK(mute != nullptr);
    if (mute) mute->click();
    CHECK(ret->muted && other->muted);
    ui::App::get().doLayouts();
    // the selected returns' faders move by as much
    ret->volumeDb = -10;
    other->volumeDb = -20;
    f = findAll<MeterFader>(t.panel->frames[ret])[0];
    f->setValueSilently(-10);
    f->setValue(-4);
    CHECK(ret->volumeDb == -4 && other->volumeDb == -14);
    // FX opens its effects window
    for (auto* b : findAll<ui::Button>(t.panel->frames[ret]))
        if (b->text() == "FX") b->click();
    CHECK(t.fxOpened == 1);
}

// many returns, every height and panel width, meters running, all faders dragged
static void testWorstCase() {
    Panel t(64, 420);
    double t0 = plat::now();
    int bad = 0;
    std::vector<float> peaks{0.9f, 1.4f, 0.2f, 0.0f}, squares{0.3f, 0.5f, 0.01f, 0.0f};
    for (int height : {config::MIN_TRACK_H, config::RETURN_MIN_H, 100, 200, config::MAX_TRACK_H}) {
        for (auto& r : t.project.returns()) t.timeline->setTrackHeight(*r, height);
        for (int width : {200, 276, 420, 800}) {
            t.panel->setGeometry(700, 0, width, 900);
            ui::App::get().doLayouts();
            for (auto& r : t.project.returns()) {
                ui::Widget* frame = t.panel->frames[r];
                if (!frame) {
                    bad++;
                    continue;
                }
                std::string why;
                if (width >= 276 && !fitsInside(frame, &why)) {  // (narrower than the panel's own width may squeeze)
                    if (!bad) printf("  %d px tall, %d wide: outside: %s\n", height, width, why.c_str());
                    bad++;
                }
                MeterFader* f = findAll<MeterFader>(frame)[0];
                f->meter()->push(peaks);
                f->meter()->pushRms(&squares);
                ui::MouseEvent e = mouse(f->width() / 2, 4);
                f->mousePress(e);
                e = mouse(f->width() / 3, 4);
                f->mouseMove(e);
                f->mouseRelease(e);
            }
            paintWidget(t.panel);
        }
    }
    CHECK(bad == 0);
    bool sane = true;
    for (auto& r : t.project.returns())
        sane = sane && r->volumeDb >= config::MIN_DB && r->volumeDb <= config::MAX_DB && r->height >= config::RETURN_MIN_H;
    CHECK(sane);
    printf("  64 returns, 5 heights x 4 widths, meters and faders: %.2f s\n", plat::now() - t0);
}

// the terminal: a return's row as text (its name, as before)
static void testTerminal() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    TrackPtr ret = w->timeline->addTrack(Kind::Return);
    ret->name = "Delay";
    for (int i = 0; i < 6; i++) {
        app.advanceTimers(0.05);
        app.tick();
    }
    CHECK(ret->height >= config::RETURN_MIN_H);
    tui::Front f(*w);
    tui::Screen screen(120, 40);
    f.draw(screen);
    CHECK(screen.lines().find("Delay") != std::string::npos);
    delete w;
    app.paintWindows = true;
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    settings::setPath(outDir() + "/settings.json");
    theme::setSkin();
    transport_bar::iconFolder = "data/icons";
    testLayout();
    testSkins();
    testControls();
    testWorstCase();
    testTerminal();
    TEST_MAIN_END();
}
