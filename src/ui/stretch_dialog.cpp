// Right-click audio clips → Stretch and pitch… (stretch_dialog.py).
#include "ui/stretch_dialog.h"

#include <cmath>
#include <cstdio>

#include "ui/toolkit/layout.h"

std::function<bool(StretchDialog&)> StretchDialog::testRun;

namespace {
std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

template <class T>
T* nest(ui::FormLayout* form, const std::string& label, std::unique_ptr<T> layout) {
    T* raw = layout.get();
    form->addRow(label, std::move(layout));
    return raw;
}
}  // namespace

StretchDialog::StretchDialog(ui::Window* parent, const Clip& clip, double projectBpm_, int count)
    : ui::Dialog(parent, "Stretch and pitch", 420, 240), projectBpm(projectBpm_) {
    setStyleType("StretchDialog");
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    if (count > 1) form->addRow(add<ui::Label>("For the " + std::to_string(count) + " selected audio clips"));
    length = add<ui::SpinBox>();
    length->setRange(25, 400);
    length->setDecimals(2);
    length->setSuffix(" %");
    length->setValueSilently(clip.stretch * 100);
    length->setToolTip("How long it plays compared with how it was recorded, at the same pitch (200 %: twice as "
                       "long, half the speed)");
    form->addRow("Length", length);
    bpm = add<ui::SpinBox>();
    bpm->setRange(0, 400);
    bpm->setDecimals(2);
    bpm->setSuffix(" BPM");
    bpm->textFromValue = [](double v) -> std::string {  // (setSpecialValueText: the minimum shows "not known")
        if (v <= 0) return "not known";
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f BPM", v);
        return buf;
    };
    bpm->setValueSilently(clip.sourceBpm > 0 ? clip.sourceBpm : 0);
    bpm->setToolTip("The tempo the audio was made at (a loop's BPM, often in its file name)");
    bpm->hPolicy = ui::SizePolicy::Expanding;
    matchButton = add<ui::Button>("Match the project's tempo (" + g(projectBpm) + " BPM)");
    matchButton->clicked.connect([this] { match(); });
    auto row = std::make_unique<ui::HBox>();
    row->setSpacing(6);
    ui::styleRoom(row.get(), computedStyle("row"));
    row->addWidget(bpm, 1);
    row->addWidget(matchButton);
    form->addRow("Recorded at", std::move(row));
    semitones = add<ui::SpinBox>();
    semitones->setRange(-24, 24);
    semitones->setSuffix(" semitones");
    cents = add<ui::SpinBox>();
    cents->setRange(-50, 50);
    cents->setSuffix(" cents");
    double whole = std::nearbyint(clip.pitch);
    semitones->setValueSilently(whole);
    cents->setValueSilently(std::nearbyint((clip.pitch - whole) * 100));
    auto pitchRow = std::make_unique<ui::HBox>();
    pitchRow->setSpacing(6);
    ui::styleRoom(pitchRow.get(), computedStyle("row"));
    pitchRow->addWidget(semitones);
    pitchRow->addWidget(cents);
    form->addRow("Pitch", std::move(pitchRow));
    resetButton = add<ui::Button>("As recorded");
    resetButton->setToolTip("100 %, no pitch change");
    resetButton->clicked.connect([this] {
        length->setValue(100);
        semitones->setValue(0);
        cents->setValue(0);
    });
    form->addRow(resetButton);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    form->addRow(buttons);
    adjustSize();
}

void StretchDialog::match() {
    if (bpm->value() > 0) length->setValue(bpm->value() / projectBpm * 100);
}

void StretchDialog::read() {
    stretch = length->value() / 100;
    pitch = semitones->value() + cents->value() / 100;
    sourceBpm = bpm->value() > 0 ? bpm->value() : 0;
}

void StretchDialog::done(int result) {
    if (result == Accepted) read();
    ui::Dialog::done(result);
}

bool StretchDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok) read();
        return ok;
    }
    return exec() == Accepted;
}
