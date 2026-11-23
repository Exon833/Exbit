// Right-click the ruler → Tempo and time signature from bar N… (tempo_dialog.py), or double-click a point of
// the tempo lane (timeline_tempo.cpp): the tempo from that place on (Timing::changes), whether it ramps to the
// next change, and the time signature N / D (only at a bar's start), or, for bar 1, the song's own (as the BPM
// and signature boxes set them).
#pragma once
#include <functional>

#include "model/timing.h"
#include "ui/toolkit/dialogs.h"

// After run() (or exec() and accept): `bpm`, `beatsPerBar` / `beatUnit` (sig()), `ramp`, or `removed` (the change
// there goes).
class TempoDialog : public ui::Dialog {
public:
    // beat: beats into the bar (0: at its start; else the time signature can't change there); canRamp: there is
    // a later change to ramp to; beatsTimebase: the song follows the tempo in beats (its hint says so)
    TempoDialog(ui::Window* parent, int bar, double bpm, TimeSig sig, bool hasChange, double beat = 0,
                bool ramp = false, bool canRamp = true, bool beatsTimebase = false);
    int bar;
    double beat;
    double bpm;
    int beatsPerBar;  // the time signature: beatsPerBar / beatUnit
    int beatUnit;
    bool ramp;
    bool removed = false;
    TimeSig sig() const { return TimeSig{beatsPerBar, beatUnit}; }
    ui::SpinBox* tempo;
    ui::SpinBox* signature;  // the numerator
    ui::ComboBox* unit;      // the denominator: 1, 2, 4, 8, 16, 32 (its item data)
    ui::CheckBox* rampBox;               // "Ramp to the next change"
    ui::Button* removeButton = nullptr;  // "Remove this change" (only when there is one here)
    void done(int result) override;      // accepted: bpm, the signature and ramp from the boxes
    void remove();                       // the change here goes
    bool run();                          // exec(); true when accepted (or removed)
    // tests: run() calls this instead of showing the dialog (set the boxes, return whether accepted)
    static std::function<bool(TempoDialog&)> testRun;

private:
    void take();  // the boxes' values
};

// The time-signature lane's dialog (timeline_signature.cpp; the terminal's S on the ruler): bar N's time signature,
// numerator and denominator as the tempo dialog has them; Remove when the bar has its own (not bar 1's).
class SignatureDialog : public ui::Dialog {
public:
    SignatureDialog(ui::Window* parent, int bar, TimeSig sig, bool removable, bool beatsTimebase = false);
    int bar;
    TimeSig sig;           // after run(): the one typed
    bool removed = false;
    ui::SpinBox* numerator;
    ui::ComboBox* unit;    // 1, 2, 4, 8, 16, 32 (its item data)
    ui::Button* removeButton = nullptr;
    void done(int result) override;
    void remove();         // (its Remove button: the bar's own time signature goes)
    bool run();            // exec(); true when accepted (or removed)
    static std::function<bool(SignatureDialog&)> testRun;  // (tests: instead of showing it)

private:
    void take();
};

// The tempo lane's range typed (the terminal's R on the ruler; the window has the row's Min, Max and Auto): `lo` and
// `hi` BPM, or Auto (fitted to the tempo changes)
class TempoRangeDialog : public ui::Dialog {
public:
    TempoRangeDialog(ui::Window* parent, double lo, double hi, bool automatic);
    double lo, hi;
    bool automatic;
    ui::SpinBox* minBox;
    ui::SpinBox* maxBox;
    ui::CheckBox* autoBox;
    void done(int result) override;
    bool run();
    static std::function<bool(TempoRangeDialog&)> testRun;

private:
    void take();
};
