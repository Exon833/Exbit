// The main window's MIDI ports, External synths and Sync (ui/midi_ports_dialog.h, audio/midi_ports.h, midi_sync.h).
#include <cmath>

#include "audio/midi_out.h"
#include "config.h"
#include "core/util.h"
#include "platform/platform.h"
#include "ui/main_window.h"
#include "ui/midi_input.h"
#include "ui/midi_ports_dialog.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"

constexpr double CLOCK_TEMPO_EVERY_S = 0.5;  // MIDI clock's tempo is taken this often at most…
constexpr double CLOCK_TEMPO_STEP = 0.1;     // …when it differs by this much (BPM)
constexpr double JACK_MASTER_RETRY_S = 2.0;  // another app was JACK's timebase master: asked again this often

void MainWindow::setUpMidiPorts() {
    midi_ports::Hub& hub = midiInput->hub;
    std::string error;
    Json saved = prefs.midiPorts.empty() ? Json() : Json::parse(prefs.midiPorts, &error);
    if (saved.isObject()) hub.load(saved);
    std::string before = midi_input::savedName();  // (the one port the program chose before MIDI ports)
    if (prefs.midiPorts.empty() && !before.empty()) hub.setOn(before, true, true);
    hub.changed = [this] {
        prefs.midiPorts = midiInput->hub.settings().dump();
        prefs.save();
    };
    hub.start();
    engine.setMidiOut(hub.out);
    midi_ports_dialog::setHost({&hub, &engine});
    for (auto& p : hub.ports())
        if (p.on && !p.present)
            showStatus("MIDI " + std::string(p.input ? "input" : "output") + " \xe2\x80\x9c" + p.key +
                           "\xe2\x80\x9d isn't connected now: it is opened when it comes back (Options \xe2\x86\x92 MIDI ports\xe2\x80\xa6)",
                       5000);
    // MIDI clock in (Sync: MIDI clock): Start, Stop, Song Position and the tempo
    midiInput->realtime = [this](const midi_in::Event& e) {
        if (syncMode != midi_sync::Mode::MidiClock) return;
        std::vector<midi_sync::Action> actions;
        clockFollower.event(e, actions);
        for (auto& a : actions) {
            if (a.kind == midi_sync::Action::Locate) {
                int64_t f = (int64_t)std::llround(project.timing.secondsAt(a.beat) * engine.sr);
                timeline->setCursor(f);
                if (engine.playing) timeline->seek(f);
            } else if ((a.kind == midi_sync::Action::Play) != engine.playing) {
                util::debugf("sync: MIDI clock %s\n", a.kind == midi_sync::Action::Play ? "starts" : "stops");
                a.kind == midi_sync::Action::Play ? togglePlay() : stop();
            }
        }
    };
    transport->fillSyncMenu = [this](ui::Menu& m) {
        midi_ports_dialog::fillSyncMenu(m, syncMode, [this](midi_sync::Mode mode) { setSync(mode); });
    };
    setSync(midi_sync::modeFrom(prefs.sync));
}

void MainWindow::showMidiPorts() {
    if (!midiInput) return;
    midi_ports_dialog::MidiPortsDialog d(this, midiInput->hub);
    d.exec();
}

void MainWindow::setSync(midi_sync::Mode mode) {
    auto* jack = midiInput ? dynamic_cast<midi_sync::JackTransport*>(midiInput->hub.jack()) : nullptr;
    if (jack && mode != midi_sync::Mode::Jack) jack->setMaster(nullptr);
    syncMode = mode;
    clockFollower.reset();
    jackFollower.reset();
    jackMasterAt_ = -1e9;
    std::string name = midi_sync::modeName(mode);
    if (prefs.sync != name) {
        prefs.sync = name;
        prefs.save();
    }
    transport->syncButton->setText(mode == midi_sync::Mode::Internal ? "Sync \xe2\x96\xbe" : "Sync: " + name + " \xe2\x96\xbe");
    transport->fit();
    util::debugf("sync: %s\n", name.c_str());
    if (mode == midi_sync::Mode::Jack && midiInput && !midiInput->hub.useJack())
        showStatus("Sync: JACK needs a JACK server running (jackd, or PipeWire's JACK): Tiny DAW follows its own "
                   "transport until there is one",
                   6000);
    if (mode == midi_sync::Mode::MidiClock && midiInput) {
        bool any = false;
        for (auto& p : midiInput->hub.ports()) any = any || (p.input && p.on && p.clock);
        if (!any)
            showStatus("Sync: MIDI clock follows an input with Clock in: tick it for your clock's port in Options "
                       "\xe2\x86\x92 MIDI ports\xe2\x80\xa6",
                       6000);
    }
}

void MainWindow::midiFrame() {
    if (!midiInput) return;
    midi_ports::Hub& hub = midiInput->hub;
    // the External synth tracks, for the audio thread (cheap when nothing changed)
    std::vector<midi_out::ExternalTrack> external;
    for (auto& t : project.tracks)
        if (t->kind == Kind::Midi && t->ext.on && !t->needsInstrument && !t->ext.port.empty())
            if (const midi_ports::Port* p = hub.find(t->ext.port, false); p && p->open)
                external.push_back({t.get(), p->slot, (t->ext.channel - 1) & 15});
    hub.out->setTracks(std::move(external));
    double now = plat::now();
    if (syncMode == midi_sync::Mode::MidiClock && clockFollower.running()) {  // its tempo
        double bpm = std::round(clockFollower.bpm() * 10) / 10;
        if (bpm >= config::MIN_BPM && bpm <= config::MAX_BPM && std::fabs(bpm - project.timing.bpm) >= CLOCK_TEMPO_STEP &&
            now - clockTempoAt_ >= CLOCK_TEMPO_EVERY_S) {
            clockTempoAt_ = now;
            TimingChange c;
            c.bpm = bpm;
            setTiming(c);
        }
    }
    if (syncMode == midi_sync::Mode::Jack) {
        auto* jack = dynamic_cast<midi_sync::JackTransport*>(hub.jack());
        if (!jack) return;
        if (!jack->master() && now - jackMasterAt_ >= JACK_MASTER_RETRY_S) {
            jackMasterAt_ = now;
            jack->setMaster(&project.timing);  // (Tiny DAW's tempo map as JACK's BBT, unless another app tells its own)
        }
        midi_sync::JackFollower::Host h;
        h.sr = engine.sr;
        h.playing = [this] { return engine.playing.load(); };
        h.pos = [this] { return engine.pos.load(); };
        h.play = [this] {
            timeline->setCursor(engine.pos.load());  // (play starts from the cursor: where JACK is)
            if (!engine.playing) togglePlay();
        };
        h.stop = [this] { stop(); };
        h.seek = [this](int64_t f) {
            timeline->setCursor(f);
            timeline->seek(f);
        };
        h.tempo = [this](double bpm) {
            if (std::fabs(bpm - project.timing.bpm) < 0.01 || bpm < config::MIN_BPM || bpm > config::MAX_BPM) return;
            TimingChange c;
            c.bpm = bpm;
            setTiming(c);
        };
        jackFollower.tick(*jack, h, now);
    }
}
