// Right-click audio clips → Warp… (see warp_dialog.h)
#include "ui/warp_dialog.h"

#include <algorithm>
#include <cmath>

#include "core/util.h"
#include "model/warping.h"
#include "platform/platform.h"
#include "ui/toolkit/layout.h"

std::function<bool(WarpDialog&)> WarpDialog::testRun;

bool WarpMarkerList::keyPress(ui::KeyEvent& e) {
    int row = currentRow();
    if (!e.mods && (e.key == plat::Key_Left || e.key == plat::Key_Right) && row >= 0) {
        moved.emit(row, e.key == plat::Key_Left ? -1 : 1);
        return true;
    }
    if (!e.mods && (e.key == plat::Key_Delete || e.key == plat::Key_Backspace) && row >= 0) {
        removed.emit(row);
        return true;
    }
    return ui::ListWidget::keyPress(e);
}

namespace {
std::string clock(double seconds) {
    double s = std::max(0.0, seconds);
    int m = (int)(s / 60);
    return util::format("%d:%06.3f", m, s - 60 * m);
}
}  // namespace

WarpDialog::WarpDialog(ui::Window* parent, const Clip& clip, TempoMapPtr map, int count, int sr, int64_t at,
                       double gridBeats)
    : ui::Dialog(parent, "Warp", 460, 560), clip_(clip), map_(map ? map : std::make_shared<const TempoMap>()), sr_(sr),
      at_(at), grid_(gridBeats > 0 ? gridBeats : 0.25) {
    setStyleType("WarpDialog");
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    if (count > 1) form->addRow(add<ui::Label>("For the " + std::to_string(count) + " selected audio clips"));
    warpBox = add<ui::CheckBox>("Warp: its audio follows the song's tempo");
    warpBox->setCheckedSilently(clip.warp);
    warpBox->setToolTip("Its beats stay on the song's beats when the tempo changes (and along tempo changes)");
    warpBox->toggled.connect([this](bool) { enable(); });
    form->addRow(warpBox);
    modeBox = add<ui::ComboBox>();
    modeBox->addItem("Beats: drums, cut at hits and placed");
    modeBox->addItem("Tones: one instrument, a voice");
    modeBox->addItem("Complex: full mixes, hits kept");
    modeBox->setCurrentIndexSilently((int)clip.warpMode);
    modeBox->currentIndexChanged.connect([this](int) { enable(); });
    form->addRow("Mode", modeBox);
    preserveBox = add<ui::ComboBox>();
    preserveBox->addItem("Transients");
    preserveBox->addItem("Every 1/4");
    preserveBox->addItem("Every 1/8");
    preserveBox->addItem("Every 1/16");
    preserveBox->setCurrentIndexSilently(clip.warpPreserve);
    preserveBox->setToolTip("Where Beats cuts the audio: at its hits, or on a grid of its beats");
    form->addRow("Beats cut at", preserveBox);
    bpm = add<ui::SpinBox>();
    bpm->setRange(0, 400);
    bpm->setDecimals(2);
    bpm->textFromValue = [](double v) -> std::string { return v <= 0 ? "not known" : util::format("%.2f BPM", v); };
    bpm->setValueSilently(clip.sourceBpm > 0 ? clip.sourceBpm : 0);
    bpm->setToolTip("The tempo the audio was made at (a loop's BPM, often in its file name)");
    bpm->hPolicy = ui::SizePolicy::Expanding;
    matchButton = add<ui::Button>(util::format("Match %g BPM", std::nearbyint(songBpm() * 100) / 100));
    matchButton->setToolTip("Not warped: the length that plays it at the song's tempo where it starts");
    matchButton->clicked.connect([this] { match(); });
    auto tempoRow = std::make_unique<ui::HBox>();
    tempoRow->setSpacing(6);
    ui::styleRoom(tempoRow.get(), computedStyle("row"));
    tempoRow->addWidget(bpm, 1);
    tempoRow->addWidget(matchButton);
    form->addRow("Recorded at", std::move(tempoRow));
    if (clip.source) detected = tempo_detect::detect(clip.sourcePath, clip.source, clip.sourceSr, clip.name);
    detectedLabel = add<ui::Label>(detected.bpm > 0 ? util::format("%.2f BPM (from its %s)", detected.bpm, detected.from.c_str())
                                                    : std::string("none found"));
    detectedLabel->hPolicy = ui::SizePolicy::Expanding;
    useButton = add<ui::Button>("Use");
    useButton->setToolTip("Warp it at this tempo: its markers straight at it from its first frame");
    useButton->clicked.connect([this] { use(); });
    auto detectedRow = std::make_unique<ui::HBox>();
    detectedRow->setSpacing(6);
    ui::styleRoom(detectedRow.get(), computedStyle("row"));
    detectedRow->addWidget(detectedLabel, 1);
    detectedRow->addWidget(useButton);
    form->addRow("Detected", std::move(detectedRow));
    length = add<ui::SpinBox>();
    length->setRange(25, 400);
    length->setDecimals(2);
    length->setSuffix(" %");
    length->setValueSilently(clip.stretch * 100);
    length->setToolTip("Not warped: how long it plays compared with how it was recorded, at the same pitch (200 %: "
                       "twice as long, half the speed)");
    form->addRow("Length", length);
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
    resetButton->setToolTip("100 %, no pitch change, not warped");
    resetButton->clicked.connect([this] {
        length->setValue(100);
        semitones->setValue(0);
        cents->setValue(0);
        warpBox->setChecked(false);
    });
    form->addRow(resetButton);
    auto* hint = add<ui::Label>("Markers (←/→ move the chosen one by the grid, Delete removes it):");
    hint->setProperty("hint", "true");
    form->addRow(hint);
    markerList = add<WarpMarkerList>();
    markerList->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    markerList->moved.connect([this](int r, int steps) { moveMarker(r, steps); });
    markerList->removed.connect([this](int r) { removeMarker(r); });
    form->addRow(markerList);
    fromHereButton = add<ui::Button>("Warp from here");
    fromHereButton->setToolTip("A marker where you clicked (or at the cursor) on the nearest bar, straight at its "
                               "tempo from there");
    fromHereButton->clicked.connect([this] { fromHere(); });
    removeButton = add<ui::Button>("Remove marker");
    removeButton->clicked.connect([this] { removeMarker(markerList->currentRow()); });
    auto markerRow = std::make_unique<ui::HBox>();
    markerRow->setSpacing(6);
    ui::styleRoom(markerRow.get(), computedStyle("row"));
    markerRow->addWidget(fromHereButton);
    markerRow->addWidget(removeButton);
    form->addRow("", std::move(markerRow));
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    form->addRow(buttons);
    rows_ = warping::markersOf(clip_);
    fill(0);
    enable();
    adjustSize();
}

void WarpDialog::fill(int select) {
    markerList->clear();
    for (auto& m : rows_) {
        double t = (double)warping::frameOfBeat(clip_, m.beat, sr_) / std::max(1, sr_);
        int bar = 0, beatIn = 0;
        double b = map_->beatAt(t);
        bar = map_->barAt(t);
        double unit = map_->sigAt(t).unitQuarters().value();  // (counted in the signature's unit: 8ths in 6/8)
        beatIn = (int)std::floor((b - map_->beatAt(map_->barSeconds(bar))) / unit + 1e-6);
        markerList->addItem(util::format("beat %7.3f  bar %d.%d  %s  (audio %.3f s)", m.beat, bar + 1,
                                         beatIn + 1, clock(t).c_str(), m.frame / std::max(1, clip_.sourceSr)));
    }
    if (select >= 0 && select < (int)rows_.size()) markerList->setCurrentRow(select);
}

void WarpDialog::enable() {
    bool on = warpBox->isChecked();
    modeBox->setEnabled(on);
    preserveBox->setEnabled(on && modeBox->currentIndex() == 0);
    length->setEnabled(!on);
    matchButton->setEnabled(!on);
    useButton->setEnabled(detected.bpm > 0);
    fromHereButton->setEnabled(clip_.warp && clip_.warpKey && at_ >= clip_.start && at_ < clip_.end());
    removeButton->setEnabled(rows_.size() > 1);
}

void WarpDialog::use() {
    if (detected.bpm <= 0) return;
    bpm->setValue(detected.bpm);
    rows_ = warp::straight(clip_.source ? clip_.source->frames() : 0, clip_.sourceSr, detected.bpm);
    clip_.sourceBpm = detected.bpm;
    clip_.warpMarkers = nullptr;
    edited_ = true;
    warpBox->setChecked(true);
    fill(0);
    util::debugf("warp dialog: the detected %.2f BPM used (%s)\n", detected.bpm, detected.from.c_str());
}

double WarpDialog::songBpm() const {
    // the song's tempo over the clip: through a ramp, its average there, so a match ends on its beat
    double s0 = (double)clip_.start / std::max(1, sr_), s1 = (double)clip_.end() / std::max(1, sr_);
    return s1 > s0 + 1e-6 ? (map_->beatAt(s1) - map_->beatAt(s0)) * 60 / (s1 - s0) : map_->bpmAt(s0);
}

void WarpDialog::match() {
    double song = songBpm();
    if (bpm->value() > 0) length->setValue(bpm->value() / song * 100);
}

void WarpDialog::fromHere() {
    if (!clip_.warp || !clip_.warpKey) return;
    clip_.warpMarkers = std::make_shared<const warp::Markers>(rows_);
    rows_ = warping::fromHere(clip_, at_, *map_, sr_);
    edited_ = true;
    int at = 0;  // (the new one chosen: the last but one, or the last)
    for (size_t i = 0; i < rows_.size(); i++)
        if (warping::frameOfSource(clip_, rows_[i].frame, sr_) <= at_ + 1) at = (int)i;
    fill(at);
    enable();
}

void WarpDialog::moveMarker(int r, int steps) {
    if (r < 0 || r >= (int)rows_.size()) return;
    double beat = rows_[(size_t)r].beat + steps * grid_;
    const double gap = 1e-3;
    if (r > 0) beat = std::max(beat, rows_[(size_t)r - 1].beat + gap);
    if (r + 1 < (int)rows_.size()) beat = std::min(beat, rows_[(size_t)r + 1].beat - gap);
    rows_[(size_t)r].beat = beat;
    edited_ = true;
    fill(r);
    util::debugf("warp dialog: marker %d to beat %.3f\n", r, beat);
}

void WarpDialog::removeMarker(int r) {
    if (r < 0 || r >= (int)rows_.size() || rows_.size() <= 1) return;
    rows_.erase(rows_.begin() + r);
    edited_ = true;
    fill(std::min(r, (int)rows_.size() - 1));
    enable();
}

void WarpDialog::read() {
    warp = warpBox->isChecked();
    mode = (warp::Mode)std::max(0, std::min(2, modeBox->currentIndex()));
    preserve = std::max(0, preserveBox->currentIndex());
    stretch = length->value() / 100;
    pitch = semitones->value() + cents->value() / 100;
    sourceBpm = bpm->value() > 0 ? bpm->value() : 0;
    markers = edited_ ? std::make_shared<const warp::Markers>(rows_) : nullptr;
}

void WarpDialog::done(int result) {
    if (result == Accepted) read();
    ui::Dialog::done(result);
}

bool WarpDialog::run() {
    if (testRun) {
        bool ok = testRun(*this);
        if (ok) read();
        return ok;
    }
    return exec() == Accepted;
}
