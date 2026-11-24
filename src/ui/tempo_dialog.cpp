// Right-click the ruler → Tempo and time signature from bar N…, or double-click a tempo point (tempo_dialog.py).
#include "ui/tempo_dialog.h"

#include <cstdio>

#include "config.h"
#include "ui/toolkit/controls.h"
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

TempoDialog::TempoDialog(ui::Window* parent, int bar_, double bpm_, TimeSig sig_, bool hasChange, double beat_,
                         bool ramp_, bool canRamp, bool beatsTimebase)
    : ui::Dialog(parent, "Tempo from " + placeName(bar_, beat_), 380, 230), bar(bar_), beat(beat_), bpm(bpm_),
      beatsPerBar(sig_.num), beatUnit(timing::checkedUnit(sig_.den, "the tempo dialog")), ramp(ramp_) {
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
    signature->setValueSilently(beatsPerBar);
    signature->setEnabled(beat == 0);  // (a time signature starts at a bar)
    signature->setToolTip("Beats in a bar");
    unit = add<ui::ComboBox>();
    for (int den : {1, 2, 4, 8, 16, 32}) unit->addItem(std::to_string(den), den);
    unit->setCurrentIndexSilently(std::max(0, unit->findData(beatUnit)));
    unit->setEnabled(beat == 0);
    unit->setToolTip("The note a beat is: 4 a quarter, 8 an eighth (6/8: six eighths a bar)");
    auto row = std::make_unique<ui::HBox>();
    row->setSpacing(6);
    row->addWidget(signature, 1);
    row->addWidget(add<ui::Label>("/"));
    row->addWidget(unit, 1);
    form->addRow("Time signature", std::move(row));
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
    beatUnit = timing::checkedUnit(unit->currentDataInt(4), "the tempo dialog");
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

// --- the time-signature lane's dialog ---

std::function<bool(SignatureDialog&)> SignatureDialog::testRun;

SignatureDialog::SignatureDialog(ui::Window* parent, int bar_, TimeSig sig_, bool removable, bool beatsTimebase)
    : ui::Dialog(parent, "Time signature at bar " + std::to_string(bar_), 340, 170), bar(bar_), sig(sig_) {
    setStyleType("SignatureDialog");
    sig.num = timing::clampedBeats(sig.num);
    sig.den = timing::checkedUnit(sig.den, "the time-signature dialog");
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    auto* intro = add<ui::Label>(bar == 1 ? "The song's time signature (from bar 1, until it changes):"
                                          : "From the start of bar " + std::to_string(bar) + " on:");
    intro->setWordWrap(true);
    form->addRow(intro);
    numerator = add<ui::SpinBox>();  // (as the tempo dialog's)
    numerator->setRange(1, config::MAX_BEATS_PER_BAR);
    numerator->setValueSilently(sig.num);
    numerator->setToolTip("Beats in a bar");
    unit = add<ui::ComboBox>();
    for (int den : {1, 2, 4, 8, 16, 32}) unit->addItem(std::to_string(den), den);
    unit->setCurrentIndexSilently(std::max(0, unit->findData(sig.den)));
    unit->setToolTip("The note a beat is: 4 a quarter, 8 an eighth (6/8: six eighths a bar)");
    auto row = std::make_unique<ui::HBox>();
    row->setSpacing(6);
    row->addWidget(numerator, 1);
    row->addWidget(add<ui::Label>("/"));
    row->addWidget(unit, 1);
    form->addRow("Time signature", std::move(row));
    auto* hint = add<ui::Label>(beatsTimebase ? "The song keeps its beats (Timebase: Beats): what comes after moves with "
                                                "the bars."
                                              : "Clips stay where they are in seconds (Timebase: Time).");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    form->addRow(hint);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    if (removable) {
        removeButton = buttons->addButton("Remove this time signature", 2);
        removeButton->clicked.connect([this] { remove(); });
    }
    form->addRow(buttons);
    adjustSize();
}

void SignatureDialog::remove() {
    removed = true;
    ui::Dialog::done(Accepted);
}

void SignatureDialog::take() {
    sig = TimeSig{numerator->intValue(), timing::checkedUnit(unit->currentDataInt(4), "the time-signature dialog")};
}

void SignatureDialog::done(int result) {
    if (result == Accepted && !removed) take();
    ui::Dialog::done(result);
}

bool SignatureDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok && !removed) take();
        return ok;
    }
    return exec() == Accepted;
}

// --- the tempo lane's range ---

std::function<bool(TempoRangeDialog&)> TempoRangeDialog::testRun;

TempoRangeDialog::TempoRangeDialog(ui::Window* parent, double lo_, double hi_, bool automatic_)
    : ui::Dialog(parent, "The tempo lane's range", 340, 170), lo(lo_), hi(hi_), automatic(automatic_) {
    setStyleType("TempoRangeDialog");
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    auto box = [this](double v) {
        auto* b = add<ui::SpinBox>();
        b->setRange(config::MIN_BPM, config::MAX_BPM);
        b->setSuffix(" BPM");
        b->setValueSilently(v);
        return b;
    };
    maxBox = box(hi);
    minBox = box(lo);
    form->addRow("Top (Max)", maxBox);
    form->addRow("Bottom (Min)", minBox);
    autoBox = add<ui::CheckBox>("Auto: fit the tempo changes");
    autoBox->setCheckedSilently(automatic);
    autoBox->setToolTip("On: the range fits the tempo changes (rounded to tens). Off: Min to Max");
    form->addRow(nullptr, autoBox);
    // (two lines of its own, not wrapped: as tall at the dialog's smallest as at any width: it fits, unscrolled)
    auto* hint = add<ui::Label>("Changing a number switches Auto off.\nMax must be above Min (else: Auto).");
    hint->setProperty("hint", "true");
    form->addRow(hint);
    auto off = [this](double) { autoBox->setChecked(false); };
    minBox->valueChanged.connect(off);
    maxBox->valueChanged.connect(off);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    form->addRow(buttons);
    adjustSize();
}

void TempoRangeDialog::take() {
    lo = minBox->value(), hi = maxBox->value();
    automatic = autoBox->isChecked();
}

void TempoRangeDialog::done(int result) {
    if (result == Accepted) take();
    ui::Dialog::done(result);
}

bool TempoRangeDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok) take();
        return ok;
    }
    return exec() == Accepted;
}
