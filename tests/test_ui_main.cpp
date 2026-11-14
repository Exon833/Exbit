// The main window's own modules: the interface size (the toolkit's scale), recent projects and
// templates (ui/templates.h), autosave and crash recovery (ui/autosave.h). Pictures of the scaled
// controls go to build/ui-samples/main/.
#include <cstdio>
#include <string>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "model/project_io.h"
#include "paths.h"
#include "ui/autosave.h"
#include "ui/preferences.h"
#include "ui/templates.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/widget.h"

using namespace ui;

namespace {
const std::string SAMPLES = "build/ui-samples/main";

void picture(Window& w, const std::string& name) {
    w.update();
    App::get().tick();
    savePng(SAMPLES + "/" + name + ".png", w.framebuffer());
}

// a few controls, as a window at the App's scale
struct Controls {
    Window w{plat::WindowOptions{}};
    Button* button;
    CheckBox* check;
    int clicks = 0;
    Controls() {
        w.resizeWindow(360, 150);
        w.moveWindow(0, 0);
        auto* root = vbox(&w, 10, 8);
        auto* bar = w.add<MenuBar>();
        bar->addMenu("File");
        bar->addMenu("Edit");
        root->addWidget(bar);
        auto row = std::make_unique<HBox>();
        HBox* r = row.get();
        r->spacing = 8;
        root->addLayout(std::move(row));
        button = w.add<Button>("Click me");
        button->clicked.connect([this] { clicks++; });
        r->addWidget(button);
        check = w.add<CheckBox>("Snap to the grid");
        check->setChecked(true);
        r->addWidget(check);
        r->addStretch();
        auto* edit = w.add<LineEdit>("Some text");
        root->addWidget(edit);
        auto* slider = w.add<Slider>(Orientation::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(60);
        root->addWidget(slider);
        root->addStretch();
        w.showWindow();
    }
};

void click(Window& w, int x, int y) {  // device pixels, as the platform sends them
    plat::Event e;
    e.type = plat::Event::MouseDown;
    e.x = x;
    e.y = y;
    e.button = plat::LeftButton;
    e.buttons = plat::LeftButton;
    w.onEvent(nullptr, e);
    e.type = plat::Event::MouseUp;
    e.buttons = 0;
    w.onEvent(nullptr, e);
}
}  // namespace

static void testPainterScale() {
    // a scaled painter: logical pixels × scale; whole pixels stay sharp
    Image img(30, 30);
    img.fill(Color(0, 0, 0));
    Painter p(img, 1.5f);
    CHECK(p.scale() == 1.5f);
    p.translate(2, 2);
    p.fillRect(Rect(0, 0, 4, 4), Color(255, 0, 0));  // logical 2..6 → device 3..9
    CHECK(img.colorAt(3, 3) == Color(255, 0, 0));
    CHECK(img.colorAt(8, 8) == Color(255, 0, 0));
    CHECK(img.colorAt(9, 9) == Color(0, 0, 0));
    CHECK(img.colorAt(2, 2) == Color(0, 0, 0));
    p.drawLine(PointF(0, 10.5f), PointF(10, 10.5f), Color(0, 255, 0));  // a 1 px line: whole device pixels
    CHECK(img.colorAt(6, 18) == Color(0, 255, 0) || img.colorAt(6, 19) == Color(0, 255, 0));
    Rect c = p.clip();  // logical, from the translation
    CHECK(c.x <= -2 && c.right() >= 18);
    p.setClip(Rect(0, 0, 2, 2));
    CHECK(p.clip().w == 2 || p.clip().w == 3);
    // unscaled: nothing differs from a plain painter
    Image a(20, 20), b(20, 20);
    a.fill(Color(10, 20, 30));
    b.fill(Color(10, 20, 30));
    Painter pa(a), pb(b, 1.f);
    for (Painter* q : {&pa, &pb}) {
        q->translate(1.5f, 2);
        q->fillRoundedRect(RectF(1, 1, 12, 9), 3, Color(200, 100, 50));
        q->drawText(PointF(2, 14), "Ab", Font::ui(), Color(255, 255, 255));
        q->drawLine(PointF(0, 0), PointF(15, 12), Color(0, 0, 255), 1.5f);
    }
    bool same = true;
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++) same = same && a.pixel(x, y) == b.pixel(x, y);
    CHECK(same);
}

static void testWindowScale() {
    App& app = App::get();
    for (float s : {1.f, 1.5f, 2.f}) {
        app.setScale(s);
        Controls c;
        CHECK(c.w.scale() == s);
        CHECK(c.w.width() == 360 && c.w.height() == 150);  // logical
        picture(c.w, "controls-" + std::to_string((int)(s * 100)));
        CHECK(c.w.framebuffer().width() == (int)(360 * s + 0.5f));
        CHECK(c.w.framebuffer().height() == (int)(150 * s + 0.5f));
        // a click in device pixels reaches the button at its logical place
        Rect g = c.button->geometry();
        Point centre = c.button->mapToWindow(Point(g.w / 2, g.h / 2));
        click(c.w, (int)(centre.x * s), (int)(centre.y * s));
        CHECK(c.clicks == 1);
        // a resize from the platform (device pixels): the logical size follows
        plat::Event r;
        r.type = plat::Event::Resize;
        r.w = (int)(400 * s);
        r.h = (int)(200 * s);
        c.w.onEvent(nullptr, r);
        CHECK(c.w.width() == 400 && c.w.height() == 200);
        // a window kept at its own pixels (a plugin's)
        Window own{plat::WindowOptions{}};
        own.setScale(1);
        own.resizeWindow(100, 80);
        own.showWindow();
        own.update();
        app.tick();
        CHECK(own.framebuffer().width() == 100);
        own.hideWindow();
        c.w.hideWindow();
    }
    app.setScale(1);
}

static void testRecent() {
    std::string dir = "build/test-tmp/ui_main";
    autosave::removeTree(dir);
    util::makeDirs(dir + "/songs/A_backups");
    templates::setDirs(dir + "/program_templates", dir + "/user_templates");
    Preferences prefs;
    std::vector<std::string> files;
    for (int i = 0; i < 12; ++i) {
        std::string f = dir + "/songs/song" + std::to_string(i) + ".json";
        util::writeFile(f, "{}");
        files.push_back(f);
        templates::remember(prefs, f);
    }
    auto r = templates::recent(prefs);
    CHECK(r.size() == (size_t)templates::RECENT_KEPT);
    CHECK(r[0] == util::absolutePath(files[11]));
    CHECK(r[9] == util::absolutePath(files[2]));
    templates::remember(prefs, files[5]);  // (again: first)
    r = templates::recent(prefs);
    CHECK(r[0] == util::absolutePath(files[5]) && r.size() == 10);
    // backups, templates and recovery files aren't remembered
    std::string backup = dir + "/songs/A_backups/A 2026.json";
    util::writeFile(backup, "{}");
    CHECK(templates::keptOut(backup));
    CHECK(templates::keptOut(dir + "/user_templates/T.json"));
    CHECK(templates::keptOut(autosave::recoveryDir() + "/run/recovered.json"));
    CHECK(!templates::keptOut(files[0]));
    templates::remember(prefs, backup);
    CHECK(templates::recent(prefs)[0] == util::absolutePath(files[5]));
    // gone files aren't listed (but kept); the old "|" list is read too
    util::removeFile(files[5]);
    CHECK(templates::recent(prefs)[0] != util::absolutePath(files[5]));
    CHECK(templates::recent(prefs, true)[0] == util::absolutePath(files[5]));
    prefs.recentProjects = files[1] + "|" + files[2];
    CHECK(templates::recent(prefs).size() == 2);
    templates::clearRecent(prefs);
    CHECK(templates::recent(prefs, true).empty());
    // templates: the program's first, then yours, by name
    util::makeDirs(dir + "/program_templates");
    util::makeDirs(dir + "/user_templates");
    util::writeFile(dir + "/program_templates/Songwriting.json", "{}");
    util::writeFile(dir + "/program_templates/Recording.json", "{}");
    util::writeFile(dir + "/user_templates/Mine.json", "{}");
    util::writeFile(dir + "/user_templates/notes.txt", "x");
    auto t = templates::list();
    CHECK(t.size() == 3);
    CHECK(t.size() == 3 && t[0].name == "Recording" && t[0].own && t[1].name == "Songwriting" && t[2].name == "Mine" &&
          !t[2].own);
    prefs.defaultTemplate = dir + "/user_templates/Mine.json";
    CHECK(templates::defaultTemplate(prefs) == prefs.defaultTemplate);
    prefs.defaultTemplate = dir + "/user_templates/Gone.json";
    CHECK(templates::defaultTemplate(prefs).empty());
    templates::setDirs("", "");
}

static void testAutosave() {
    std::string root = "build/test-tmp/ui_main/recovery";
    autosave::removeTree(root);
    Project project;
    project.tracks = std::vector<TrackPtr>{std::make_shared<Track>()};
    bool unsaved = true;
    std::string opened, recoveredAs = "?";
    auto host = [&] {
        autosave::Host h;
        h.hasUnsavedChanges = [&] { return unsaved; };
        h.snapshot = [&] { return project_io::describe(project, 48000, {}, nullptr, false); };
        h.projectPath = [] { return std::string("/nowhere/Song.json"); };
        h.describe = [&](const std::string& folder, std::map<std::string, std::string>& written,
                         std::vector<project_io::PendingAudio>& toWrite) {
            return project_io::describeRecovery(project, 48000, {}, folder, written, toWrite);
        };
        h.openProjectFile = [&](const std::string& path) {
            opened = path;
            return true;
        };
        h.recovered = [&](const std::string& original) { recoveredAs = original; };
        return h;
    };
    {
        autosave::Autosave a(host(), root, false);
        a.tick();  // unsaved: written
        CHECK(a.wait().empty());
        CHECK(!a.folder().empty());
        CHECK(util::fileExists(a.folder() + "/" + autosave::PROJECT_FILE));
        autosave::Header h;
        CHECK(autosave::readHeader(a.folder() + "/" + autosave::PROJECT_FILE, h));
        CHECK(h.tracks == 1 && h.recoveryOf == "/nowhere/Song.json" && h.savedAt > 0);
        // this run holds its folder: not left behind
        CHECK(autosave::leftBehind(root).empty());
        // saved: emptied (the folder stays)
        a.clear();
        CHECK(!util::fileExists(a.folder() + "/" + autosave::PROJECT_FILE));
        a.tick();
        a.wait();
        // the program "crashes": the Autosave goes without close() (the lock goes with it)
    }
    auto left = autosave::leftBehind(root);
    CHECK(left.size() == 1);
    {
        autosave::Autosave b(host(), root, false);
        b.answer = 2;  // Not now: kept
        CHECK(!b.offerRecovery());
        CHECK(autosave::leftBehind(root).size() == 1);
        b.answer = 0;  // Open it
        CHECK(b.offerRecovery());
        CHECK(util::endsWith(opened, autosave::PROJECT_FILE));
        CHECK(recoveredAs.empty());  // (its folder doesn't exist here: untitled)
        CHECK(b.folder() == left[0].folder);
        b.close();  // normally closed: the folder goes
        CHECK(!util::isDir(left[0].folder));
    }
    {
        autosave::Autosave c(host(), root, false);
        unsaved = false;
        c.tick();  // nothing unsaved: nothing written
        CHECK(c.folder().empty());
        unsaved = true;
        c.saveNow();
        c.wait();
    }
    autosave::Autosave d(host(), root, false);
    d.answer = 1;  // Discard it
    CHECK(!d.offerRecovery());
    CHECK(autosave::leftBehind(root).empty());
    d.close();
}

int main() {
    App::get().initHeadless();
    util::makeDirs(SAMPLES);
    util::makeDirs("build/test-tmp/ui_main");
    settings::setPath("build/test-tmp/ui_main/settings.json");
    paths::setDataDir("build/test-tmp/ui_main/data");
    testPainterScale();
    testWindowScale();
    testRecent();
    testAutosave();
    TEST_MAIN_END();
}
