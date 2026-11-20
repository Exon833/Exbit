// A MIDI track's MIDI effects window (right-click its row → MIDI effects…; audio/midi_effects): the
// effects in the order they change the notes (Add ▾, Remove, ↑ ↓), and the chosen one's settings
// (midi_effects_dialog.py). Changes are heard a moment later: the track's clips are rendered again after
// the last change. Like the FX windows it doesn't block anything; one per track.
//
// THE MAIN WINDOW (after sampler_dialog::setHost): the lane panel's "MIDI effects…" →
// midi_effects_dialog::openMidiEffects(track); after undo / redo midi_effects_dialog::reloadAll(); when
// tracks go away midi_effects_dialog::closeGone(project); setHooks() for Commit to notes.
//
// Each effect can be switched off (its "On" box: bypassed, MidiEffect::bypass; "(off)" in the list), kept in
// the chain. "Commit to notes ▾" writes what the effects make into notes (ui/midi_commit.h): the clips selected
// on the timeline (of this track) or every clip of the track, into the same clips or into new clips on a new
// track below. In the terminal (--tui) the window is drawn as text: Tab to the button, Enter opens its menu.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "model/model.h"
#include "ui/midi_commit.h"
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
// the kinds, in midi_effects.KINDS order (Arpeggiator, Chord, Scale, Note Length, Velocity, Random, Pitch, Groove)
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
    void setBypass(int row, bool bypass);  // an effect switched off (kept in the chain) or on again
    // Commit to notes: the clips selected on the timeline (of this track), or every one of its clips; into
    // them, or onto a new track below
    midi_commit::Result commit(bool everyClip, bool newTrack);
    ui::Button* commitButton;
    void showSettings(int row);
    void done(int result) override;

private:
    int renderTimer_ = -1;
    bool filling_ = false;
};

// what Commit to notes needs from the main window
struct Hooks {
    std::function<std::vector<ClipPtr>()> selectedClips;   // the clips selected on the timeline
    midi_commit::Loader loadPlugins;                       // loads a new track's plugins
    std::function<void(const midi_commit::Result&, const std::string& message)> committed;  // after one
};
void setHooks(Hooks hooks);

MidiEffectsDialog* openMidiEffects(const TrackPtr& track);
MidiEffectsDialog* openFor(const Track* track);
void reloadAll();
void closeGone(const Project& project);

}  // namespace midi_effects_dialog
