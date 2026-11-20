// Right-click the ruler → Tempo and time signature from bar N…, or double-click a tempo point (tempo_dialog.py).
#include "ui/tempo_dialog.h"

#include <cstdio>

#include "config.h"
#include "ui/toolkit/layout.h"

std::function<bool(TempoDialog&)> TempoDialog::testRun;

namespace {
std::string placeName(int bar, double beat) {  // "bar 5" / "bar 5, beat 3" (beats counted from 1 there)
    if (beat == 0) return "bar " + std::to_string(bar);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", beat + 1);
    return "bar " + std::to_string(bar) + ", beat " + buf;
}
}  // namespace

TempoDialog::TempoDialog(ui::Window* parent, int bar_, double bpm_, int beatsPerBar_, bool hasChange, double beat_,
                         bool ramp_, bool canRamp, bool beatsTimebase)
    : ui::Dialog(parent, "Tempo from " + placeName(bar_, beat_), 380, 230), bar(bar_), beat(beat_), bpm(bpm_),
      beatsPerBar(beatsPerBar_), ramp(ramp_) {
    setStyleType("TempoDialog");
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    std::string from = beat == 0 ? "From the start of bar " + std::to_string(bar) : "From " + placeName(bar, beat);
    auto* intro = add<ui::Label>(from + " on" + (bar > 1 || beat > 0 ? "" : " (the whole song, unless it changes later)") + ":");
    intro->setWordWrap(true);
    form->addRow(intro);
    tempo = add<ui::SpinBox>();
    tempo->setRange(config::MIN_BPM, config::MAX_BPM);
    tempo->setDecimals(2);
    tempo->setSuffix(" BPM");
    tempo->setValueSilently(bpm);
    form->addRow("Tempo", tempo);
    rampBox = add<ui::CheckBox>("Ramp to the next change");
    rampBox->setCheckedSilently(ramp && canRamp);
    rampBox->setEnabled(canRamp);
    rampBox->setToolTip(canRamp ? "On: the tempo goes smoothly from here to the next change's (a ramp). Off: it stays "
                                 "until the next change (a step)"
                               : "There is no later change to ramp to");
    form->addRow(nullptr, rampBox);
    signature = add<ui::SpinBox>();
    signature->setRange(1, config::MAX_BEATS_PER_BAR);
    signature->setSuffix(" / 4");
    signature->setValueSilently(beatsPerBar);
    signature->setEnabled(beat == 0);  // (a time signature starts at a bar)
    form->addRow("Time signature", signature);
    auto* hint = add<ui::Label>(beatsTimebase ? "The song keeps its beats (Timebase: Beats): clips, notes, automation and "
                                                "markers move with the tempo; audio plays at its own speed."
                                              : "Clips stay where they are in seconds (Timebase: Time); the grid, the "
                                                "metronome and what follows the tempo change.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    form->addRow(hint);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    if (hasChange) {
        removeButton = buttons->addButton("Remove this change", 2);
        removeButton->clicked.connect([this] { remove(); });
    }
    form->addRow(buttons);
    adjustSize();
}

void TempoDialog::remove() {
    removed = true;
    ui::Dialog::done(Accepted);
}

void TempoDialog::take() {
    bpm = tempo->value();
    beatsPerBar = signature->intValue();
    ramp = rampBox->isChecked();
}

void TempoDialog::done(int result) {
    if (result == Accepted && !removed) take();
    ui::Dialog::done(result);
}

bool TempoDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok && !removed) take();
        return ok;
    }
    return exec() == Accepted;
}
