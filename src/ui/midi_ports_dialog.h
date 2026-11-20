// MIDI in and out in the GUI (and the terminal UI, which draws these same dialogs and menus as text):
//
//   - Options → "MIDI ports…" (MidiPortsDialog): every input and output the Hub knows (audio/midi_ports.h: the ALSA
//     sequencer's, JACK MIDI's while a JACK server runs), each with On (its list's tick: Space), and for the one
//     chosen in the list Clock in (inputs: Sync ▸ MIDI clock follows it) or Clock out, Always send Start and its clock
//     offset (outputs). A port remembered but missing now shows "— not connected", greyed. Changes apply at once and
//     are remembered (the Hub's `changed`); the lists follow ports coming and going while it is open.
//   - A MIDI track's menu: "MIDI input ▸" (addTrackMenu): All (what is played goes to the track while it is the
//     selected one: the default) or one input, and a channel ("Any" or 1-16): Track::midiIn (saved, undoable).
//   - The instrument menu's "External synth…" (and the track's menu, 🎹 on its row once it plays one): the
//     ExternalSynthDialog: a MIDI output and channel, the audio input its sound comes back on (none: heard from the
//     synth itself), and its delay in ms, typed or measured (Measure: midi_out.h); OK makes the track play it
//     (Track::ext: model.h EXTERNAL SYNTH).
//   - The transport bar's "Sync ▾" (fillSyncMenu): Internal, MIDI clock, JACK (midi_sync.h).
//
// The main window gives them its Hub and engine with setHost() at start (none: the menus offer only All).
#pragma once
#include <functional>
#include <string>

#include "audio/engine.h"
#include "audio/midi_ports.h"
#include "audio/midi_sync.h"
#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/menu.h"

namespace midi_ports_dialog {

struct Host {
    midi_ports::Hub* hub = nullptr;
    audio::Engine* engine = nullptr;
};
void setHost(const Host& h);
const Host& host();

// "MIDI input ▸" in a MIDI track's menu; changed() after the track's input changed
void addTrackMenu(ui::Menu& menu, const TrackPtr& track, std::function<void()> changed);
// the Sync ▾ menu: the three modes, `current` ticked
void fillSyncMenu(ui::Menu& menu, midi_sync::Mode current, std::function<void(midi_sync::Mode)> choose);
// "External synth…": the dialog for `track`; true when OK made it play one (chosen() called)
bool editExternal(ui::Window* parent, const TrackPtr& track, std::function<void()> chosen = nullptr);

// Options → MIDI ports…
class MidiPortsDialog : public ui::Dialog {
public:
    MidiPortsDialog(ui::Window* parent, midi_ports::Hub& hub);
    ~MidiPortsDialog() override;
    ui::ListWidget* inputs;
    ui::ListWidget* outputs;
    ui::CheckBox* clockIn;      // the input chosen in the list
    ui::CheckBox* clockOut;     // the output chosen in the list…
    ui::CheckBox* alwaysStart;
    ui::SpinBox* offset;
    void refresh();             // the lists as the Hub has them now (the rows chosen kept)

private:
    midi_ports::Hub& hub_;
    std::vector<std::string> inKeys_, outKeys_;
    int timer_ = -1;
    bool filling_ = false;
    void showSelected();
};

class ExternalSynthDialog : public ui::Dialog {
public:
    ExternalSynthDialog(ui::Window* parent, const TrackPtr& track);
    ~ExternalSynthDialog() override;
    ui::ComboBox* port;
    ui::SpinBox* channel;
    ui::ComboBox* input;        // its data: "first,stereo" ("-1,0": none)
    ui::SpinBox* delay;
    ui::Button* measureButton;
    ui::Label* result;
    bool measure();             // Measure pressed: false (and why, in `result`) if it can't start
    void done(int r) override;  // OK: the track plays it

private:
    TrackPtr track_;
    int timer_ = -1;
    void poll();                // (while measuring)
};

}  // namespace midi_ports_dialog
