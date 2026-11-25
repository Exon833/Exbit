// exbit --tui: the MIDI editor's note tools by the keyboard alone. F2 in the editor opens the tools' menu,
// Tools… the panel as a dialog drawn as text (Enter applies, Esc cancels: nothing left applied); K then Enter
// splits at the keyboard's cursor, P selects the cursor's row, Ctrl+J joins; the hints at the bottom and the F1
// list name them. Pictures of the screen go to build/ui-samples/tui/.
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "ui/main_window.h"
#include "ui/midi_editor.h"
#include "ui/note_tools_panel.h"
#include "ui/timeline.h"
#include "ui/toolkit/widget.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/tui";

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{110, 34};
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

MidiEditor* editorOpen() {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (auto* e = dynamic_cast<MidiEditor*>(*it))
            if (e->isWindowShown()) return e;
    return nullptr;
}

Note note(double s, double e, int pitch) {
    Note n;
    n.start = s;
    n.end = e;
    n.pitch = pitch;
    return n;
}

}  // namespace

static void testTuiTools() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    // a MIDI track (the track panel's N), a clip on it (the timeline's N): its editor opens
    d.keys({key(plat::Key_Tab), key('N', 0, "n")});
    TrackPtr midi = w->timeline->currentTrack;
    CHECK(midi && midi->kind == Kind::Midi);
    d.keys({key(plat::Key_Tab), key(plat::Key_Tab), key(plat::Key_Tab)});
    f.setCurrent(midi);
    w->timeline->setCursor(0);
    d.keys({key('N', 0, "n")});
    MidiEditor* ed = editorOpen();
    CHECK(ed != nullptr);
    if (!ed) return;
    ed->notes = {note(0, 1, 60), note(0, 1, 64), note(0, 1, 67)};
    ed->off.clear();
    ed->commit();
    ClipPtr clip = ed->clip;
    auto before = clip->notes;
    d.keys({});
    CHECK(d.shows("F2: tools") && d.shows("K: knife"));
    // F2: the tools' menu; Tools… opens the panel as a dialog; Esc cancels it: nothing applied
    bool sawMenu = false;  // (the menu waits in a loop of its own: looked at there, then Enter on Tools…)
    app.nestedLoop = [&](const std::function<bool()>& done) {
        if (!sawMenu) {
            d.picture("note-tools-menu");
            sawMenu = d.shows("Tools…") && d.shows("Remove overlaps (same pitch)") && d.shows("Legato");
        }
        d.step(done);
    };
    d.keys({key(plat::Key_F2), key(plat::Key_Return)});
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    CHECK(sawMenu);
    CHECK(ed->toolsPanel != nullptr);
    CHECK(d.shows("Note tools") && d.shows("Apply") && d.shows("Time per note"));
    CHECK(!ed->preview.empty() && clip->notes == before);
    CHECK(d.shows("▌"));  // (the preview, drawn as text)
    d.picture("note-tools-panel");
    d.keys({key(plat::Key_Escape)});
    CHECK(!ed->toolsPanel && ed->preview.empty() && clip->notes == before);
    // again, Enter: Apply (a strum: 30 ms a note, up)
    d.keys({key(plat::Key_F2), key(plat::Key_Return), key(plat::Key_Return)});
    CHECK(!ed->toolsPanel && clip->notes != before && clip->notes->size() == 3);
    if (clip->notes->size() == 3) CHECK(std::fabs((*clip->notes)[2].start - 0.06) < 1e-9);
    // undone (Ctrl+Z, the window's): back as it was, in the editor too
    d.keys({key('Z', plat::ModCtrl)});
    CHECK(clip->notes->size() == 3 && (*clip->notes)[2].start == 0);
    // K, Enter: the knife splits the cursor's row at the cursor; P selects that row; Ctrl+J joins it again
    ed->keyT = 0.5;
    ed->keyPitch = 64;
    d.keys({key('K', 0, "k"), key(plat::Key_Return)});
    CHECK(ed->knife && clip->notes->size() == 4);
    d.keys({key(plat::Key_Escape), key('P', 0, "p")});
    CHECK(!ed->knife && ed->selected.size() == 2);
    d.keys({key('J', plat::ModCtrl)});
    CHECK(clip->notes->size() == 3);
    d.picture("note-tools-editor");
    // the F1 list names them (the windows hidden first: F12)
    d.keys({key(plat::Key_F12), key(plat::Key_F1)});
    std::string seen;  // (longer than the screen: scrolled through)
    for (int i = 0; i < 8; i++) {
        seen += d.screen.lines();
        d.keys({key(plat::Key_PageDown)});
    }
    CHECK(f.helpShown && seen.find("the knife") != std::string::npos && seen.find("the note tools") != std::string::npos);
    d.keys({key(plat::Key_Space, 0, " "), key(plat::Key_F12), key('W', plat::ModCtrl)});
    CHECK(!editorOpen());
    w->close();
    app.collectGarbage();
}

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs("build/test-tui-note-tools");
    settings::setPath("build/test-tui-note-tools/settings.json");
    testTuiTools();
    TEST_MAIN_END();
}
