// Right-click an audio clip → Gain envelope points… (model/clip_envelope.h): its envelope's points as a list
// (time from the clip's start, gain in dB) to add, change and remove, typed rather than drawn. The terminal UI's
// way to edit an envelope (it reaches the TUI as every dialog does), and a precise one in the window.
#pragma once
#include <functional>
#include <vector>

#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class GainEnvelopeDialog : public ui::Dialog {
public:
    GainEnvelopeDialog(ui::Window* parent, const Clip& clip, int sr);
    std::vector<AutoPoint> points;  // the envelope as edited: frames of the clip's audio (none: no envelope)
    ui::ListWidget* list;
    ui::SpinBox* time;              // s from the clip's start
    ui::SpinBox* gain;              // dB (its lowest: silence)
    ui::Button *addButton, *changeButton, *removeButton;
    void addPoint();                // a point at the time and gain typed (replacing one at that very time)
    void changePoint();             // the point chosen in the list: moved to the time and gain typed
    void removePoint();             // the point chosen in the list: gone
    bool run();                     // shown (modal): true on OK
    static std::function<bool(GainEnvelopeDialog&)> testRun;  // tests: instead of showing it

private:
    int64_t offset_;
    int sr_;
    void fill(int select);          // the list again, `select` chosen
    AutoPoint typed() const;        // the time and gain typed, as a point
};
