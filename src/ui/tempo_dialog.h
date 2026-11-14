// Right-click the ruler → Tempo and time signature from bar N… (tempo_dialog.py): the tempo and beats
// per bar from the start of that bar on (Timing::changes), or, for bar 1, the song's own (as the BPM
// and / 4 boxes set them).
#pragma once
#include <functional>

#include "ui/toolkit/dialogs.h"

// After run() (or exec() and accept): `bpm`, `beatsPerBar`, or `removed` (the change at this bar goes).
class TempoDialog : public ui::Dialog {
public:
    TempoDialog(ui::Window* parent, int bar, double bpm, int beatsPerBar, bool hasChange);
    int bar;
    double bpm;
    int beatsPerBar;
    bool removed = false;
    ui::SpinBox* tempo;
    ui::SpinBox* signature;
    ui::Button* removeButton = nullptr;  // "Remove this change" (only when there is one at this bar)
    void done(int result) override;      // accepted: bpm and beatsPerBar from the boxes
    void remove();                       // the change at this bar goes
    bool run();                          // exec(); true when accepted (or removed)
    // tests: run() calls this instead of showing the dialog (set the boxes, return whether accepted)
    static std::function<bool(TempoDialog&)> testRun;
};
