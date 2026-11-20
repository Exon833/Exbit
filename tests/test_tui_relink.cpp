// tinydaw --tui: the missing samples window by the keyboard alone. A project opened with samples gone shows it,
// drawn as text (each file, where it was expected, what uses it; Search a folder…, Locate…, Leave missing); Esc
// leaves them missing (their clips silent, marked [missing] on the timeline); File → Find missing samples… (Alt+F,
// D) opens it again, and a search puts every one back. Collect and save and Save as archive… are in the File
// menu. Pictures of the screen go to build/ui-samples/tui/.
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/sound_file.h"
#include "check.h"
#include "core/util.h"
#include "model/project_io.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/main_window.h"
#include "ui/relink_dialog.h"
#include "ui/toolkit/widget.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/tui";
const std::string TMP = util::absolutePath("build/test-tui-relink");

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{120, 34};
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

RelinkDialog* relinkOpen() {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (auto* d = dynamic_cast<RelinkDialog*>(*it))
            if (d->isWindowShown()) return d;
    return nullptr;
}

int missingClips(const Project& p) {
    int n = 0;
    for (auto& c : p.clips) n += c->missing != nullptr;
    return n;
}

}  // namespace

static void testTuiRelink() {
    // a project of two samples, one of them moved away
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    for (int i = 0; i < 2; i++) {
        auto b = std::make_shared<AudioBuffer>();
        b->samples.assign(2 * (size_t)(48000 + i * 100), 0.1f * (float)(i + 1));
        auto c = makeAudioClip(i ? "Bass" : "Kick", b, 48000, 48000);
        c->track = t;
        c->start = i * 96000;
        p.addClip(c);
    }
    std::string err, path = TMP + "/Tui.json";
    project_io::MasterSettings master;
    CHECK(project_io::saveProject(path, p, 48000, master, err));
    util::makeDirs(TMP + "/elsewhere/sub");
    CHECK(rename((TMP + "/Tui_audio/Bass.wav").c_str(), (TMP + "/elsewhere/sub/Bass.wav").c_str()) == 0);

    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{&f};
    f.showFolder(TMP);
    d.keys({});
    // opening it: the window, drawn as text; Esc leaves the sample missing
    bool saw = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        if (!saw && relinkOpen()) {
            d.picture("relink-dialog");
            saw = d.shows("Missing samples") && d.shows("Bass.wav") && d.shows("used by clip") && d.shows("expected in") &&
                  d.shows("Search a folder") && d.shows("Locate") && d.shows("Leave missing");
        }
        d.step(done);
    };
    d.queue = {key(plat::Key_Escape)};
    d.at = 0;
    CHECK(w->openProjectFile(path));
    CHECK(saw && !relinkOpen());
    CHECK(w->project.clips.size() == 2 && missingClips(w->project) == 1);
    d.keys({});
    d.picture("relink-missing-clip");
    CHECK(d.shows("[mis"));  // ("[missing] Bass", as far as its second on the screen holds)
    // the File menu holds the rest
    d.keys({key(plat::Key_F10)});
    CHECK(d.shows("Collect and save") && d.shows("Save as archive") && d.shows("Find missing samples"));
    d.keys({key(plat::Key_Escape)});
    // Alt+F, D: Find missing samples… again; a search of the folder puts it back, and the window closes
    bool searched = false;
    app.nestedLoop = [&](const std::function<bool()>& done) {
        if (RelinkDialog* r = relinkOpen(); r && !searched) {
            searched = true;
            CHECK(r->list->count() == 1);
            CHECK(r->searchFolder(TMP + "/elsewhere") == 1);
        }
        d.step(done);
    };
    d.keys({key('F', plat::ModAlt, "f"), key('D', 0, "d")});
    CHECK(searched && !relinkOpen());
    CHECK(missingClips(w->project) == 0);
    CHECK(w->project.clips[1]->sourcePath == TMP + "/elsewhere/sub/Bass.wav");
    d.keys({});
    CHECK(!d.shows("[mis"));
    w->close();
    app.collectGarbage();
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs(TMP);
    for (const char* f : {"/Tui.json", "/Tui_audio/Kick.wav", "/Tui_audio/Bass.wav", "/elsewhere/sub/Bass.wav"})
        util::removeFile(TMP + f);
    settings::setPath(TMP + "/settings.json");
    settings::set("explorer/folders", Json::array({Json(TMP + "/explorer")}));  // (never the user's own Music folder)
    util::makeDirs(TMP + "/explorer");
    testTuiRelink();
    TEST_MAIN_END();
}
