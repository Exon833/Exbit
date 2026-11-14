// A MIDI track's MIDI effects window (right-click its row → MIDI effects…; audio/midi_effects): the
// effects in the order they change the notes (Add ▾, Remove, ↑ ↓), and the chosen one's settings
// (midi_effects_dialog.py). Changes are heard a moment later: the track's clips are rendered again after
// the last change. Like the FX windows it doesn't block anything; one per track.
//
// THE MAIN WINDOW (after sampler_dialog::setHost): the lane panel's "MIDI effects…" →
// midi_effects_dialog::openMidiEffects(track); after undo / redo midi_effects_dialog::reloadAll(); when
// tracks go away midi_effects_dialog::closeGone(project).
#pragma once
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace midi_effects_dialog {

constexpr int RENDER_DELAY_MS = 250;
constexpr int STEPS = 1000;

// a MIDI effect's parameter as its window shows it (audio/effect.py's Param)
struct Param {
    std::string key, name;
    double low = 0, high = 1, def = 0;
    std::string unit;                  // "%", "semitones", "beats"…
    std::string scale = "linear";      // "linear", "log", "int"
    std::vector<std::string> choices;  // the value is the index of the one chosen
    int decimals = -1;                 // -1: from the range
    double toNormal(double value) const;
    double fromNormal(double normal) const;
    std::string text(double value) const;
};
struct Kind {
    std::string name;   // "Arpeggiator", "Chord"… (MidiEffect::kind)
    bool live = true;   // applies to notes played live too (it doesn't change their timing)
    std::vector<Param> params;
};
// the seven kinds, in midi_effects.KINDS order (Arpeggiator, Chord, Scale, Note Length, Velocity, Random, Pitch)
const std::vector<Kind>& kinds();
const Kind* kindOf(const std::string& name);

class MidiEffectsDialog : public ui::Dialog {
public:
    MidiEffectsDialog(ui::Window* window, TrackPtr track);
    ~MidiEffectsDialog() override;
    TrackPtr track;
    ui::ListWidget* list;
    ui::Button* addButton;
    ui::Widget* settingsPane;  // the chosen effect's settings (rebuilt for each)

    void fillList(int current = 0);
    void reload();  // the track's MIDI effects as they are now (after undo or redo)
    void changed(std::vector<MidiEffect> effects, int current = -1);
    void addEffect(const std::string& kind);  // (Python: add)
    void remove();
    void move(int step);
    void setValue(int row, const std::string& key, double value);
    void showSettings(int row);
    void done(int result) override;

private:
    int renderTimer_ = -1;
    bool filling_ = false;
};

MidiEffectsDialog* openMidiEffects(const TrackPtr& track);
MidiEffectsDialog* openFor(const Track* track);
void reloadAll();
void closeGone(const Project& project);

}  // namespace midi_effects_dialog
