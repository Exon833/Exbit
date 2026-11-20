// tinydaw --tui: MIDI in and out by the keyboard alone. Options → MIDI ports… drawn as text (each port "[x]" on or
// "[ ]" off, Space ticks the row chosen, a port missing "— not connected"), Clock in / Clock out ticked for the row
// chosen; the transport's Sync control (Up/Down, Enter its menu); a MIDI track's menu's MIDI input; the External
// synth window. Ports come from a fake backend (no MIDI device needed). Pictures go to build/ui-samples/tui/.
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "audio/midi_ports.h"
#include "check.h"
#include "core/util.h"
#include "platform/platform.h"
#include "tui/app.h"
#include "tui/screen.h"
#include "tui/window_view.h"
#include "ui/main_window.h"
#include "ui/midi_input.h"
#include "ui/midi_ports_dialog.h"
#include "ui/timeline.h"
#include "ui/toolkit/widget.h"
#include "ui/transport_bar.h"

using namespace tui;

namespace {

const std::string OUT = "build/ui-samples/tui";

Key key(int k, int mods = 0, std::string text = "") { return Key{k, mods, std::move(text)}; }

struct Driver {
    Front* f;
    std::vector<Key> queue;
    size_t at = 0;
    Screen screen{100, 40};  // (narrow: the transport's Sync, at its far end, still shown when chosen)
    std::function<void()> onOpen;  // (once, when a dialog's loop first runs)
    void step(const std::function<bool()>& done) {
        if (onOpen) {
            auto fn = onOpen;
            onOpen = nullptr;
            fn();
        }
        while (at < queue.size() && !done()) {
            ui::App::get().advanceTimers(0.05);
            ui::App::get().tick();
            f->key(queue[at++]);
            f->draw(screen);
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

class Fake : public midi_ports::Backend {
public:
    std::vector<midi_ports::Found> ports;
    const char* name() const override { return "Fake"; }
    void list(std::vector<midi_ports::Found>& out) override { out.insert(out.end(), ports.begin(), ports.end()); }
    bool open(const midi_ports::Found&, int) override { return true; }
    void close(int) override {}
    void send(int, const uint8_t*, int, double) override {}
    void add(const std::string& key, bool input) {
        midi_ports::Found f;
        f.key = key;
        f.input = input;
        ports.push_back(f);
    }
};

template <class T>
T* topDialog() {
    const auto& ws = ui::App::get().windows();
    for (auto it = ws.rbegin(); it != ws.rend(); ++it)
        if (auto* d = dynamic_cast<T*>(*it))
            if (d->isWindowShown()) return d;
    return nullptr;
}

int rowOf(ui::ListWidget* list, const std::string& key) {
    for (int i = 0; i < list->count(); i++)
        if (list->item(i).data == key) return i;
    return -1;
}

void testTuiMidiPorts() {
    ui::App& app = ui::App::get();
    app.paintWindows = false;
    auto* w = new MainWindow();
    w->resizeWindow(1280, 800);
    w->showWindow();
    w->askBeforeClosing = false;
    Front f(*w);
    Driver d{&f};
    app.nestedLoop = [&d](const std::function<bool()>& done) { d.step(done); };
    midi_ports::Hub& hub = w->midiInput->hub;
    auto fake = std::make_unique<Fake>();
    fake->add("Fake keys", true);
    fake->add("Fake synth", false);
    hub.addBackend(std::move(fake));
    hub.setOn("Fake gone", true, true);  // (remembered, not there)
    hub.rescan();
    CHECK(hub.find("Fake keys", true) && hub.find("Fake keys", true)->open);

    // Options → MIDI ports…: the keys typed into it once it is open
    int inRow = -1, outRow = -1;
    d.onOpen = [&] {
        auto* dlg = topDialog<midi_ports_dialog::MidiPortsDialog>();
        CHECK(dlg != nullptr);
        if (!dlg) return;
        inRow = rowOf(dlg->inputs, "Fake keys");
        outRow = rowOf(dlg->outputs, "Fake synth");
        CHECK(inRow >= 0 && outRow >= 0 && rowOf(dlg->inputs, "Fake gone") >= 0);
        dlg->inputs->setCurrentRow(inRow);
        dlg->outputs->setCurrentRow(outRow);
        dlg->inputs->setFocus();
        f.draw(d.screen);
        d.picture("midi-ports");
        CHECK(d.shows("[x] Fake keys"));
        CHECK(d.shows("[ ] Fake synth"));
        CHECK(d.shows("Fake gone \xe2\x80\x94 not connected"));
        CHECK(d.shows("Clock in"));
        d.queue = {key(plat::Key_Space),                          // Fake keys off
                   key(plat::Key_Space),                          // …and on again
                   key(plat::Key_Tab), key(plat::Key_Space),      // Clock in
                   key(plat::Key_Tab), key(plat::Key_Space),      // Fake synth on
                   key(plat::Key_Tab), key(plat::Key_Space),      // Clock out
                   key(plat::Key_Tab), key(plat::Key_Space),      // Always send Start
                   key(plat::Key_Escape)};
        d.at = 0;
    };
    w->showMidiPorts();
    d.f->draw(d.screen);
    const midi_ports::Port* in = hub.find("Fake keys", true);
    const midi_ports::Port* out = hub.find("Fake synth", false);
    CHECK(in && in->on && in->open && in->clock);
    CHECK(out && out->on && out->open && out->clock && out->alwaysStart);
    CHECK(!topDialog<midi_ports_dialog::MidiPortsDialog>());
    CHECK(w->prefs.midiPorts.find("Fake synth") != std::string::npos);  // (remembered)

    // the transport's Sync: Up from Internal is MIDI clock; Down again Internal; Enter its menu
    f.pane = Front::Pane::Transport;
    f.control = 13;  // (C_SYNC)
    d.keys({key(plat::Key_Up)});
    CHECK(w->syncMode == midi_sync::Mode::MidiClock);
    CHECK(d.shows("Sync MIDI clock"));
    CHECK(w->transport->syncButton->text() == "Sync: MIDI clock \xe2\x96\xbe");
    CHECK(w->prefs.sync == "MIDI clock");
    d.picture("sync");
    d.keys({key(plat::Key_Down)});
    CHECK(w->syncMode == midi_sync::Mode::Internal && d.shows("Sync Internal"));
    d.keys({key(plat::Key_Return)});
    CHECK(f.menuOpen() && d.shows("MIDI clock") && d.shows("JACK"));
    d.keys({key(plat::Key_Escape)});
    // MIDI clock in, through the main window: Start plays, Stop stops (no device: the engine stays stopped, no crash)
    w->setSync(midi_sync::Mode::MidiClock);
    midi_in::Event start;
    start.type = midi_in::Event::Start;
    start.port = (int16_t)in->slot;
    w->midiInput->handle(start);
    midi_in::Event stop = start;
    stop.type = midi_in::Event::Stop;
    w->midiInput->handle(stop);
    w->setSync(midi_sync::Mode::Internal);

    // a MIDI track's menu: MIDI input ▸ Fake keys ▸ Channel 10
    TrackPtr t = w->timeline->addTrack(Kind::Midi);
    t->needsInstrument = false;
    ui::Menu menu;
    int changed = 0;
    midi_ports_dialog::addTrackMenu(menu, t, [&] { changed++; });
    ui::Action* inputMenu = menu.actions().empty() ? nullptr : menu.actions()[0].get();
    CHECK(inputMenu && inputMenu->submenu && inputMenu->text == "MIDI input: All");
    ui::Action* ch10 = nullptr;
    if (inputMenu && inputMenu->submenu)
        for (auto& a : inputMenu->submenu->actions())
            if (a->submenu && a->text == "Fake keys")
                for (auto& c : a->submenu->actions())
                    if (c->text == "Channel 10") ch10 = c.get();
    CHECK(ch10 != nullptr);
    if (ch10 && ch10->toggled) ch10->toggled(true);
    CHECK(t->midiIn == "Fake keys" && t->midiInChannel == 10 && changed == 1);
    // …and it is in the track's menu in the terminal too (F2 on its row)
    f.pane = Front::Pane::Panel;
    f.laneRow = 0;
    w->timeline->setCurrentTrack(t);
    d.keys({key(plat::Key_F2)});
    CHECK(f.menuOpen() && d.shows("MIDI input: Fake keys, channel 10"));
    d.picture("track-midi-input");
    d.keys({key(plat::Key_Escape)});

    // the External synth window: the output, channel, input and delay; Measure without an open device says why
    d.onOpen = [&] {
        auto* dlg = topDialog<midi_ports_dialog::ExternalSynthDialog>();
        CHECK(dlg != nullptr);
        if (!dlg) return;
        for (int i = 0; i < dlg->port->count(); i++)
            if (dlg->port->itemData(i) == "Fake synth") dlg->port->setCurrentIndex(i);
        dlg->channel->setValue(5);
        for (int i = 0; i < dlg->input->count(); i++)
            if (dlg->input->itemData(i) == "0,0") dlg->input->setCurrentIndex(i);  // In 1 (whatever the machine has)
        dlg->delay->setValue(12.5);
        bool started = dlg->measure();  // (a device open with an input: it listens; else it says why)
        CHECK(started == (w->engine.isOpen() && w->engine.inputCount() > 0 && w->engine.monitoring()));
        CHECK(!dlg->result->text().empty());
        f.draw(d.screen);
        d.picture("external-synth");
        CHECK(d.shows("External synth") && d.shows("MIDI output") && d.shows("Measure"));
        d.queue = {key(plat::Key_Return)};  // OK
        d.at = 0;
    };
    CHECK(midi_ports_dialog::editExternal(w, t));
    CHECK(t->ext.on && t->ext.port == "Fake synth" && t->ext.channel == 5 && t->ext.input == 0 && !t->ext.stereo &&
          t->ext.delayMs == 12.5);
    CHECK(!t->playsPlugins() && !t->playsSampler());
    // the frame hands it to the audio thread's scheduler (its output open)
    w->midiFrame();
    w->close();
    app.tick();
}

}  // namespace

int main() {
    plat::initWithoutDisplay();
    ui::App::get().initHeadless();
    util::makeDirs("build/test-tui-midi-ports");
    settings::setPath("build/test-tui-midi-ports/settings.json");
    util::removeFile("build/test-tui-midi-ports/settings.json");
    testTuiMidiPorts();
    TEST_MAIN_END();
}
