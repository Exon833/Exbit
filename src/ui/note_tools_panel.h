// The MIDI editor's note tools (Tools ▾ in its toolbar, F2 → Tools… in it): a panel beside the editor. Pick a
// tool (Strum, Arpeggiate, Chop, Flam, Legato, Quantize, Humanize, Velocity shape), set its few controls, and
// the editor shows the result ghosted over the notes (MidiEditor::preview) as the controls change; Apply puts it
// into the clip as one edit (one undo step), Cancel (Esc, ✕) drops it. Nothing reaches the clip before Apply, so
// a preview can never be left applied: closing the editor, or going to another clip's notes, closes the panel.
// The tool works on the selected notes, or on all of them when none are selected (what the panel says at its
// top). The transforms themselves: model/note_tools.h. In the terminal (tinydaw --tui) it is a dialog, drawn as
// text like any other.
//
// The same file holds the MidiEditor's tool commands (midi_editor.h, "the note tools"): the knife (K), join
// (Ctrl+J), a key's row selected (Shift+click a key, P), remove overlaps (same pitch) and legato, and Tools ▾.
#pragma once
#include "model/note_tools.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class MidiEditor;

class NoteToolsPanel : public ui::Dialog {
public:
    explicit NoteToolsPanel(MidiEditor* editor);
    ~NoteToolsPanel() override;
    MidiEditor* editor;               // null once the editor is gone
    ui::ComboBox* tool;
    ui::Label* target;                // "On the 3 selected notes" / "On all 12 notes"
    ui::Label* labels[5];             // the spin boxes', then the choices'
    ui::SpinBox* spins[3];
    ui::ComboBox* choices[2];
    ui::CheckBox* ends;               // Quantize: the ends too
    ui::Button* applyButton;
    void setTool(note_tools::Tool t); // its controls, as last set (remembered while the program runs)
    note_tools::Params params() const;  // what the controls say, for this editor's clip
    void updatePreview();             // the editor's preview from the controls (and its notes now)
    void apply();                     // Apply: into the clip, the panel closes
    void done(int result) override;   // (closed either way: the preview goes)
private:
    void remember();
};
