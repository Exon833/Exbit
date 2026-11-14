// Right-click the ruler → Tempo and time signature from bar N… (tempo_dialog.py).
#include "ui/tempo_dialog.h"

#include "config.h"
#include "ui/toolkit/layout.h"

std::function<bool(TempoDialog&)> TempoDialog::testRun;

TempoDialog::TempoDialog(ui::Window* parent, int bar_, double bpm_, int beatsPerBar_, bool hasChange)
    : ui::Dialog(parent, "Tempo from bar " + std::to_string(bar_), 360, 200), bar(bar_), bpm(bpm_), beatsPerBar(beatsPerBar_) {
    ui::FormLayout* form = ui::form(this, 11, 6);
    auto* intro = add<ui::Label>("From the start of bar " + std::to_string(bar) + " on" +
                                 (bar > 1 ? "" : " (the whole song, unless it changes later)") + ":");
    intro->setWordWrap(true);
    form->addRow(intro);
    tempo = add<ui::SpinBox>();
    tempo->setRange(config::MIN_BPM, config::MAX_BPM);
    tempo->setDecimals(2);
    tempo->setSuffix(" BPM");
    tempo->setValueSilently(bpm);
    form->addRow("Tempo", tempo);
    signature = add<ui::SpinBox>();
    signature->setRange(1, config::MAX_BEATS_PER_BAR);
    signature->setSuffix(" / 4");
    signature->setValueSilently(beatsPerBar);
    form->addRow("Time signature", signature);
    auto* hint = add<ui::Label>("Clips stay where they are in seconds; the grid, the metronome and what follows the "
                                "tempo change.");
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

void TempoDialog::done(int result) {
    if (result == Accepted && !removed) {
        bpm = tempo->value();
        beatsPerBar = signature->intValue();
    }
    ui::Dialog::done(result);
}

bool TempoDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok && !removed) {
            bpm = tempo->value();
            beatsPerBar = signature->intValue();
        }
        return ok;
    }
    return exec() == Accepted;
}
