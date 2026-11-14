// Right-click audio clips → Stretch and pitch… (stretch_dialog.py): how long they play compared with
// how they were recorded (at the same pitch), and how much higher or lower (at the same speed);
// audio/stretch.h does it.
//
// "Recorded at" is the tempo the audio was made at (a loop's BPM): Match the project's tempo then
// sets the length so it plays at the project's tempo. It is remembered with the clip, to match again
// after the tempo changes (clips don't follow tempo changes by themselves: their place stays in
// seconds, like all clips in Tiny DAW).
#pragma once
#include <functional>

#include "model/model.h"
#include "ui/toolkit/dialogs.h"

// After run() (or exec() and accept): `stretch` (times the recorded length), `pitch` (semitones),
// `sourceBpm` (0: not known).
class StretchDialog : public ui::Dialog {
public:
    StretchDialog(ui::Window* parent, const Clip& clip, double projectBpm, int count);
    double projectBpm;
    double stretch = 1.0, pitch = 0.0, sourceBpm = 0.0;
    ui::SpinBox* length;     // %
    ui::SpinBox* bpm;        // "not known" at 0
    ui::SpinBox* semitones;
    ui::SpinBox* cents;
    ui::Button* matchButton;
    ui::Button* resetButton;  // "As recorded"
    void match();             // the length that plays the recorded tempo at the project's
    void done(int result) override;
    bool run();
    static std::function<bool(StretchDialog&)> testRun;  // tests: instead of showing it
private:
    void read();
};
