// The main window (main_window.h): the transport's Key ▾ and Swing ▾ menus (ui/song_scale.h).
#include "ui/main_window.h"
#include "ui/song_scale.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"

void MainWindow::connectScale() {
    auto shown = [this] {
        transport->showTiming(project.timing);
        timeline->update();
    };
    transport->fillKeyMenu = [this, shown](ui::Menu& m) {
        // detected from the selected MIDI clips (none selected: every MIDI clip)
        std::vector<Note> selected, all;
        for (const ClipPtr& c : project.clips) {
            if (c->kind != Kind::Midi || !c->notes) continue;
            all.insert(all.end(), c->notes->begin(), c->notes->end());
            if (timeline->selection.contains(c)) selected.insert(selected.end(), c->notes->begin(), c->notes->end());
        }
        bool some = !selected.empty();
        song_scale::fillKeyMenu(m, project, some ? selected : all, some ? "the selected clips" : "every MIDI clip", shown);
    };
    transport->fillSwingMenu = [this, shown](ui::Menu& m) { song_scale::fillSwingMenu(m, project, engine.sr, shown); };
    std::weak_ptr<bool> alive = alive_;
    song_scale::notesCommitted = [this, alive] {  // (the open MIDI editors: the swung notes)
        if (!alive.expired()) reloadEditors();
    };
}
