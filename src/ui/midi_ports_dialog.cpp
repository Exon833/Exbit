// MIDI ports, a track's MIDI input, External synths and Sync in the GUI (midi_ports_dialog.h).
#include "ui/midi_ports_dialog.h"

#include <algorithm>
#include <cmath>

#include "audio/midi_out.h"
#include "core/util.h"
#include "platform/platform.h"
#include "ui/sampler_dialog.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/widget.h"

namespace midi_ports_dialog {

using namespace ui;

namespace {
Host g_host;
const Color GREY(128, 128, 128);
}  // namespace

void setHost(const Host& h) { g_host = h; }
const Host& host() { return g_host; }

// --- a track's MIDI input ---

void addTrackMenu(ui::Menu& menu, const TrackPtr& track, std::function<void()> changed) {
    std::string now = track->midiIn.empty() ? "All" : track->midiIn;
    if (!track->midiIn.empty() && track->midiInChannel) now += ", channel " + std::to_string(track->midiInChannel);
    Menu* m = menu.addMenu("MIDI input: " + now);
    auto set = [track, changed](const std::string& port, int channel) {
        if (track->midiIn == port && track->midiInChannel == channel) return;
        track->midiIn = port;
        track->midiInChannel = channel;
        util::debugf("midi: %s's input: %s, channel %d\n", track->name.c_str(), port.empty() ? "All" : port.c_str(), channel);
        if (changed) changed();
    };
    m->addCheckable("All (while it is the selected track)", track->midiIn.empty(), [set](bool) { set("", 0); })->tooltip =
        "What any MIDI input plays goes to this track while it is the selected one";
    std::vector<std::string> keys;
    if (g_host.hub)
        for (auto& p : g_host.hub->ports())
            if (p.input && (p.on || p.key == track->midiIn)) keys.push_back(p.key);
    if (!track->midiIn.empty() && std::find(keys.begin(), keys.end(), track->midiIn) == keys.end())
        keys.push_back(track->midiIn);  // (remembered, not there now)
    if (!keys.empty()) m->addSeparator();
    for (const std::string& key : keys) {
        const midi_ports::Port* p = g_host.hub ? g_host.hub->find(key, true) : nullptr;
        bool here = p && p->present;
        Menu* sub = m->addMenu((key == track->midiIn ? "\xe2\x9c\x93 " : "") + key + (here ? "" : " \xe2\x80\x94 not connected"));
        sub->addCheckable("Any channel", track->midiIn == key && track->midiInChannel == 0, [set, key](bool) { set(key, 0); });
        for (int ch = 1; ch <= 16; ch++)
            sub->addCheckable("Channel " + std::to_string(ch), track->midiIn == key && track->midiInChannel == ch,
                              [set, key, ch](bool) { set(key, ch); });
    }
    if (keys.empty()) m->addAction("(no MIDI inputs on: Options \xe2\x86\x92 MIDI ports\xe2\x80\xa6)")->setEnabled(false);
}

// --- Sync ---

void fillSyncMenu(ui::Menu& menu, midi_sync::Mode current, std::function<void(midi_sync::Mode)> choose) {
    menu.clear();
    struct Item {
        midi_sync::Mode mode;
        const char* tip;
    };
    for (Item it : {Item{midi_sync::Mode::Internal, "Tiny DAW's own tempo and transport"},
                    Item{midi_sync::Mode::MidiClock,
                         "Follow the MIDI clock of the input with Clock in (Options \xe2\x86\x92 MIDI ports\xe2\x80\xa6): its tempo, "
                         "Start, Stop and song position"},
                    Item{midi_sync::Mode::Jack,
                         "Follow and drive JACK's transport: start, stop and locate both ways; JACK's tempo when another "
                         "app is its timebase master, else Tiny DAW's"}}) {
        auto mode = it.mode;
        Action* a = menu.addCheckable(midi_sync::modeName(mode), mode == current, [choose, mode](bool) {
            if (choose) choose(mode);
        });
        a->group = 1;
        a->tooltip = it.tip;
    }
}

// --- Options → MIDI ports… ---

MidiPortsDialog::MidiPortsDialog(Window* parent, midi_ports::Hub& hub) : Dialog(parent, "MIDI ports", 560, 520), hub_(hub) {
    setStyleType("MidiPortsDialog");
    if (g_host.engine && g_host.engine->driverType == "JACK") hub_.useJack();  // (JACK MIDI's ports too)
    hub_.rescan();
    FormLayout* f = styleRoom(form(this, 11, 6), computedStyle());
    Label* about = add<Label>("Every MIDI port Tiny DAW can listen to and play into. Ticked: on (Space ticks the row "
                              "chosen). What the inputs play goes to the selected MIDI track, or to the tracks set to "
                              "listen to one of them (a track's menu: MIDI input).");
    about->setWordWrap(true);
    f->addRow(about);
    f->addRow(add<Label>("Inputs (keyboards, controllers)"));
    inputs = add<ListWidget>();
    f->addRow(inputs);
    clockIn = add<CheckBox>("Clock in: Sync \xe2\x86\x92 MIDI clock follows this input's clock, Start and Stop");
    f->addRow(clockIn);
    f->addRow(add<Label>("Outputs (synths, interfaces)"));
    outputs = add<ListWidget>();
    f->addRow(outputs);
    clockOut = add<CheckBox>("Clock out: send MIDI clock, Start and Stop to this output");
    f->addRow(clockOut);
    alwaysStart = add<CheckBox>("Always send Start (for synths that ignore Continue)");
    f->addRow(alwaysStart);
    offset = add<SpinBox>();
    offset->setRange(-midi_ports::MAX_OFFSET_MS, midi_ports::MAX_OFFSET_MS);
    offset->setDecimals(1);
    offset->setSuffix(" ms");
    f->addRow("Clock offset", offset);
    auto* bb = add<ButtonBox>(ButtonBox::Close);
    bb->rejected.connect([this] { reject(); });
    bb->accepted.connect([this] { accept(); });
    f->addRow(bb);

    inputs->itemChecked.connect([this](int row, bool on) {
        if (!filling_ && row >= 0 && row < (int)inKeys_.size()) hub_.setOn(inKeys_[(size_t)row], true, on);
    });
    outputs->itemChecked.connect([this](int row, bool on) {
        if (!filling_ && row >= 0 && row < (int)outKeys_.size()) hub_.setOn(outKeys_[(size_t)row], false, on);
    });
    inputs->currentRowChanged.connect([this](int) { if (!filling_) showSelected(); });
    outputs->currentRowChanged.connect([this](int) { if (!filling_) showSelected(); });
    clockIn->toggled.connect([this](bool on) {
        int r = inputs->currentRow();
        if (!filling_ && r >= 0 && r < (int)inKeys_.size()) hub_.setClock(inKeys_[(size_t)r], true, on), refresh();
    });
    clockOut->toggled.connect([this](bool on) {
        int r = outputs->currentRow();
        if (!filling_ && r >= 0 && r < (int)outKeys_.size()) hub_.setClock(outKeys_[(size_t)r], false, on), refresh();
    });
    alwaysStart->toggled.connect([this](bool on) {
        int r = outputs->currentRow();
        if (!filling_ && r >= 0 && r < (int)outKeys_.size()) hub_.setAlwaysStart(outKeys_[(size_t)r], on), refresh();
    });
    offset->valueChanged.connect([this](double ms) {
        int r = outputs->currentRow();
        if (!filling_ && r >= 0 && r < (int)outKeys_.size()) hub_.setOffsetMs(outKeys_[(size_t)r], ms), refresh();
    });
    refresh();
    // (ports coming and going while it is open: looked for again, the lists follow)
    timer_ = App::get().every(1000, [this] {
        if (hub_.tick(plat::now())) refresh();
    });
    adjustSize();
}

MidiPortsDialog::~MidiPortsDialog() { App::get().cancelTimer(timer_); }

void MidiPortsDialog::refresh() {
    filling_ = true;
    for (bool in : {true, false}) {
        ListWidget* list = in ? inputs : outputs;
        std::vector<std::string>& keys = in ? inKeys_ : outKeys_;
        int row = list->currentRow();
        std::string was = row >= 0 && row < (int)keys.size() ? keys[(size_t)row] : "";
        list->clear();
        keys.clear();
        for (auto& p : hub_.ports()) {
            if (p.input != in) continue;
            ListWidget::Item item;
            std::string text = p.label();
            if (p.clock) text += in ? "   (clock in)" : "   (clock out)";
            if (!in && p.alwaysStart) text += " (Start)";
            item.text = text;
            item.data = p.key;
            item.checkable = true;
            item.checked = p.on;
            item.tooltip = p.present ? (p.backend ? std::string(p.backend->name()) + " MIDI port" : "")
                                     : "Not connected now: listened to (or played into) again as soon as it comes back";
            if (!p.present) item.hasColor = true, item.color = GREY;
            list->addItem(item);
            keys.push_back(p.key);
        }
        if (keys.empty()) {
            ListWidget::Item none;
            none.text = in ? "(no MIDI inputs found: is the keyboard plugged in?)" : "(no MIDI outputs found)";
            none.enabled = none.selectable = false;
            list->addItem(none);
        }
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == was) list->setCurrentRow((int)i);
        if (list->currentRow() < 0 && !keys.empty()) list->setCurrentRow(0);
    }
    filling_ = false;
    showSelected();
}

void MidiPortsDialog::showSelected() {
    filling_ = true;
    int r = inputs->currentRow();
    const midi_ports::Port* in = r >= 0 && r < (int)inKeys_.size() ? hub_.find(inKeys_[(size_t)r], true) : nullptr;
    clockIn->setChecked(in && in->clock);
    clockIn->setEnabled(in != nullptr);
    r = outputs->currentRow();
    const midi_ports::Port* out = r >= 0 && r < (int)outKeys_.size() ? hub_.find(outKeys_[(size_t)r], false) : nullptr;
    clockOut->setChecked(out && out->clock);
    alwaysStart->setChecked(out && out->alwaysStart);
    offset->setValueSilently(out ? out->offsetMs : 0.0);
    for (Widget* w : {(Widget*)clockOut, (Widget*)alwaysStart, (Widget*)offset}) w->setEnabled(out != nullptr);
    filling_ = false;
}

// --- External synth ---

ExternalSynthDialog::ExternalSynthDialog(Window* parent, const TrackPtr& track)
    : Dialog(parent, "External synth", 480, 360), track_(track) {
    setStyleType("ExternalSynthDialog");
    const ExternalSynth& x = track->ext;
    FormLayout* f = styleRoom(form(this, 11, 6), computedStyle());
    Label* about = add<Label>(track->name + " plays a hardware synth: its notes go out to the synth's MIDI port, and "
                              "its sound comes back on an audio input, through the track's effects.");
    about->setWordWrap(true);
    f->addRow(about);
    port = add<ComboBox>();
    if (g_host.hub)
        for (auto& p : g_host.hub->ports())
            if (!p.input) port->addItem(p.label(), p.key);
    if (!x.port.empty() && !(g_host.hub && g_host.hub->find(x.port, false)))
        port->addItem(x.port + " \xe2\x80\x94 not connected", x.port);
    if (!port->count()) port->addItem("(no MIDI outputs found)", "");
    for (int i = 0; i < port->count(); i++)
        if (port->itemData(i) == x.port) port->setCurrentIndexSilently(i);
    if (port->currentIndex() < 0) port->setCurrentIndexSilently(0);
    f->addRow("MIDI output", port);
    channel = add<SpinBox>();
    channel->setRange(1, 16);
    channel->setValueSilently(x.channel);
    f->addRow("MIDI channel", channel);
    input = add<ComboBox>();
    input->addItem("None: heard from the synth itself", "-1,0");
    int available = g_host.engine ? g_host.engine->inputCount() : 0;  // (the duplex stream's, or the default input's)
    int n = available > 0 ? available : 8;
    for (int k = 0; k < n; k++) {
        input->addItem("In " + std::to_string(k + 1), std::to_string(k) + ",0");
        if (k + 1 < n) input->addItem("In " + std::to_string(k + 1) + "/" + std::to_string(k + 2), std::to_string(k) + ",1");
    }
    std::string want = std::to_string(x.input) + "," + (x.input >= 0 && x.stereo ? "1" : "0");
    input->setCurrentIndexSilently(0);
    for (int i = 0; i < input->count(); i++)
        if (input->itemData(i) == want) input->setCurrentIndexSilently(i);
    f->addRow("Audio input", input);
    delay = add<SpinBox>();
    delay->setRange(0, 2000);
    delay->setDecimals(1);
    delay->setSuffix(" ms");
    delay->setValueSilently(x.delayMs);
    f->addRow("Delay", delay);
    measureButton = add<Button>("Measure");
    measureButton->setToolTip("Sends middle C and listens for it on the audio input: the round trip is the delay");
    measureButton->clicked.connect([this] { measure(); });
    f->addRow(measureButton);
    result = add<Label>(available > 0
                            ? "The delay is how long the synth's sound takes to come back; its notes go out that much early."
                            : "This audio device has no inputs: choose one that has (Options \xe2\x86\x92 Audio "
                              "properties\xe2\x80\xa6) to hear the synth through the track.");
    result->setWordWrap(true);
    f->addRow(result);
    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });
    f->addRow(bb);
    adjustSize();
}

ExternalSynthDialog::~ExternalSynthDialog() {
    App::get().cancelTimer(timer_);
    if (g_host.hub && timer_ >= 0) g_host.hub->out->cancelMeasure();
    if (g_host.engine) g_host.engine->inputWanted = false;  // (ui/recording turns monitoring off if nothing else hears)
}

static void parseInput(const std::string& data, int& first, bool& stereo) {
    first = -1, stereo = false;
    size_t comma = data.find(',');
    if (comma == std::string::npos) return;
    first = std::atoi(data.substr(0, comma).c_str());
    stereo = data.substr(comma + 1) == "1";
}

bool ExternalSynthDialog::measure() {
    auto fail = [this](const std::string& why) {
        result->setText(why);
        adjustSize();
        return false;
    };
    audio::Engine* engine = g_host.engine;
    midi_ports::Hub* hub = g_host.hub;
    std::string key = port->currentData();
    int first;
    bool stereo;
    parseInput(input->currentData(), first, stereo);
    if (!hub || key.empty()) return fail("Choose the synth's MIDI output first.");
    if (first < 0) return fail("Choose the audio input the synth's sound comes back on first.");
    if (!engine || !engine->isOpen() || engine->inputCount() <= first)
        return fail("The audio device has no input " + std::to_string(first + 1) +
                    ": choose one with inputs (Options \xe2\x86\x92 Audio properties\xe2\x80\xa6), or type the delay.");
    std::string why;  // (heard as monitoring hears an input: the duplex stream's, or the default input's)
    engine->inputWanted = true;
    if (!engine->setMonitoring(true, why)) return fail("The input can't be heard: " + why);
    hub->setOn(key, false, true);
    const midi_ports::Port* p = hub->find(key, false);
    if (!p || !p->open) return fail(key + " isn't connected now.");
    hub->out->measure(p->slot, (int)channel->value() - 1, first);
    util::debugf("midi: measuring %s (channel %d) on input %d\n", key.c_str(), (int)channel->value(), first + 1);
    result->setText("Listening\xe2\x80\xa6");
    measureButton->setEnabled(false);
    App::get().cancelTimer(timer_);
    timer_ = App::get().every(50, [this] { poll(); });
    return true;
}

void ExternalSynthDialog::poll() {
    midi_ports::Hub* hub = g_host.hub;
    if (!hub) return;
    auto state = hub->out->measuring();
    if (state != midi_out::Out::Measuring::Done && state != midi_out::Out::Measuring::Failed) return;
    App::get().cancelTimer(timer_);
    timer_ = -1;
    measureButton->setEnabled(true);
    if (state == midi_out::Out::Measuring::Done) {
        int sr = g_host.engine ? g_host.engine->sr : 48000;
        double ms = std::round(hub->out->measuredFrames() * 10000.0 / sr) / 10;
        delay->setValue(ms);
        result->setText("Measured: its sound comes back " + util::fixed(ms, 1) + " ms after its note. OK keeps it.");
        util::debugf("midi: measured %.1f ms\n", ms);
    } else {
        result->setText("No sound came back within " + util::fixed(midi_out::MEASURE_TIMEOUT_S, 0) +
                        " s: is the synth's output connected to that input, and turned up? (Or type the delay.)");
    }
    hub->out->cancelMeasure();
    adjustSize();
}

void ExternalSynthDialog::done(int r) {
    if (r == Accepted) {
        ExternalSynth x;
        x.on = true;
        x.port = port->currentData();
        x.channel = (int)channel->value();
        parseInput(input->currentData(), x.input, x.stereo);
        if (x.input < 0) x.stereo = true;  // (the default)
        x.delayMs = delay->value();
        track_->ext = x;
        track_->needsInstrument = false;
        if (g_host.hub && !x.port.empty()) g_host.hub->setOn(x.port, false, true);
        util::debugf("midi: %s plays the External synth on %s, channel %d\n", track_->name.c_str(), x.port.c_str(), x.channel);
    }
    Dialog::done(r);
}

bool editExternal(ui::Window* parent, const TrackPtr& track, std::function<void()> chosen) {
    ExternalSynthDialog d(parent, track);
    if (d.exec() != Dialog::Accepted) return false;
    sampler_dialog::renderTrackClips(track);  // (its clips: silent, the synth plays them)
    if (chosen) chosen();
    return true;
}

}  // namespace midi_ports_dialog
