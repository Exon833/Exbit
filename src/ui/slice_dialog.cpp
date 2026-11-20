// The slice window (slice_dialog.h).
#include "ui/slice_dialog.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#include "audio/padkit.h"
#include "config.h"
#include "core/util.h"
#include "ui/formatting.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"

namespace pad_kit_dialog {

const int GRID_DIVISIONS[4] = {4, 8, 16, 32};

// =================================================================================================

SliceView::SliceView(SliceDialog* dialog) : dialog_(dialog) {
    setFixedHeight(theme::metric("sample-waveform-height", HEIGHT) * 2);
    setFocusPolicy(ui::FocusPolicy::Tab);  // (its keys: keyPress)
}

int64_t SliceView::frameAt(int x) const {
    int64_t n = dialog_->frames();
    return std::min(std::max<int64_t>(0, n - 1), std::max<int64_t>(0, std::llround((double)x / std::max(1, width()) * n)));
}

int SliceView::xOf(int64_t frame) const {
    return (int)((double)frame * width() / std::max<int64_t>(1, dialog_->frames()));
}

int SliceView::lineAt(int x) const {
    const std::vector<int64_t>& hits = dialog_->hits;
    auto it = std::lower_bound(hits.begin(), hits.end(), frameAt(x));
    int best = -1, gap = GRAB_PX + 1;
    for (auto c : {it - 1, it})  // (the lines either side of the pointer: the nearer)
        if (c >= hits.begin() && c < hits.end() && std::abs(xOf(*c) - x) < gap)
            best = (int)(c - hits.begin()), gap = std::abs(xOf(*c) - x);
    return best;
}

void SliceView::paint(Painter& p) {
    SampleWaveform::paint(p);
    const std::vector<int64_t>& hits = dialog_->hits;
    if (hits.empty() || dialog_->frames() <= 0) return;
    int h = height(), c = std::min(dialog_->chosen, (int)hits.size() - 1);
    int a = xOf(hits[(size_t)c]), b = xOf(dialog_->sliceEnd(c));
    p.fillRect(Rect(a, 0, std::max(1, b - a), h), theme::c("selection_fill"));
    Color line = theme::c("selected_note"), over = theme::c("meter_hot");  // (past the last pad: flagged)
    for (size_t i = 0; i < hits.size(); i++)
        p.fillRect(Rect(xOf(hits[i]), 0, (int)i == dragging ? 2 : 1, h), (int)i < PadKit::MAX_PADS ? line : over);
}

bool SliceView::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton || dialog_->frames() <= 0) return false;
    setFocus();
    int l = lineAt(e.pos.x);
    if (l >= 0 && dialog_->hits[(size_t)l] > 0) {
        dragging = l;
        dialog_->choose(l);
    } else {
        dialog_->audition(dialog_->sliceAt(frameAt(e.pos.x)));
    }
    return true;
}

bool SliceView::mouseMove(ui::MouseEvent& e) {
    if (dragging < 0) return false;
    int64_t reach = (int64_t)SNAP_PX * dialog_->frames() / std::max(1, width());
    dragging = dialog_->moveLine(dragging, frameAt(e.pos.x), !e.alt(), reach);
    return true;
}

bool SliceView::mouseRelease(ui::MouseEvent& e) {
    if (dragging < 0) return false;
    dragging = -1;
    update();
    return true;
}

bool SliceView::mouseDoubleClick(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton || dialog_->frames() <= 0) return false;
    dragging = -1;
    int l = lineAt(e.pos.x);
    if (l >= 0) dialog_->removeLine(l);
    else dialog_->addLine(frameAt(e.pos.x));
    return true;
}

bool SliceView::keyPress(ui::KeyEvent& e) {
    if (e.mods & (plat::ModCtrl | plat::ModAlt)) return false;
    int c = dialog_->chosen;
    switch (e.key) {
    case plat::Key_Up: dialog_->choose(c - 1); return true;
    case plat::Key_Down: dialog_->choose(c + 1); return true;
    case plat::Key_Left: dialog_->nudge(-1, e.shift()); return true;
    case plat::Key_Right: dialog_->nudge(1, e.shift()); return true;
    case plat::Key_Space: dialog_->audition(c); return true;
    case plat::Key_Insert:
        if (c < (int)dialog_->hits.size()) dialog_->addLine((dialog_->hits[(size_t)c] + dialog_->sliceEnd(c)) / 2);
        return true;
    case plat::Key_Delete: dialog_->removeLine(c); return true;
    default: return false;
    }
}

bool SliceView::listRows(std::vector<std::string>& rows, int& current) const {
    const std::vector<int64_t>& hits = dialog_->hits;
    double sr = std::max(1, dialog_->sr);
    for (size_t i = 0; i < hits.size(); i++) {
        char length[32];
        std::snprintf(length, sizeof length, "%.3f s", (dialog_->sliceEnd((int)i) - hits[i]) / sr);
        rows.push_back(std::to_string(i + 1) + "  " + fmt::time(hits[i] / sr) + "  " + length +
                       ((int)i < PadKit::MAX_PADS ? "" : "  (no pad)"));
    }
    current = hits.empty() ? -1 : std::min(dialog_->chosen, (int)hits.size() - 1);
    return true;
}

// =================================================================================================

SliceDialog::SliceDialog(ui::Window* parent, ClipPtr clip_, int sr_, const Timing& timing_)
    : ui::Dialog(parent, "Slice to a Pad Kit", 640, 380), clip(std::move(clip_)), sr(sr_), timing(timing_) {
    deleteOnClose = true;
    setStyleType("SliceDialog");
    ui::VBox* col = ui::styleRoom(ui::vbox(this, 12, 8), computedStyle());
    ui::Label* what = add<ui::Label>("Cuts " + clip->name + " into slices: a new MIDI track with a Pad Kit, a slice "
                                     "on each pad, and a clip playing them where they were.");
    what->setWordWrap(true);
    col->addWidget(what);
    auto row = std::make_unique<ui::HBox>();
    row->setSpacing(8);
    mode = add<ui::ComboBox>();
    for (const char* m : {"Transients", "Grid", "Manual"}) mode->addItem(m);
    mode->setToolTip("Transients: at the hits found; Grid: on the beat; Manual: the lines as you place them");
    row->addWidget(mode);
    row->addWidget(add<ui::Label>("Sensitivity"));
    sensitivity = add<ui::Slider>();
    sensitivity->setRange(0, 100);
    sensitivity->setValueSilently((int)std::lround(padkit::DEFAULT_SENSITIVITY * 100));
    sensitivity->setToolTip("Transients: higher finds quieter hits too (dragged lines snap to them)");
    row->addWidget(sensitivity, 1);
    row->addWidget(add<ui::Label>("Grid"));
    grid = add<ui::ComboBox>();
    for (int d : GRID_DIVISIONS) grid->addItem("1/" + std::to_string(d));
    grid->setCurrentIndexSilently(2);
    grid->setToolTip(clip->sourceBpm > 0 ? "A slice every note of this length, at the clip's own tempo"
                                         : "A slice every note of this length, on the song's grid");
    row->addWidget(grid);
    col->addLayout(std::move(row));
    view = add<SliceView>(this);
    col->addWidget(view);
    count = add<ui::Label>();
    count->setMinimumWidth(std::max(0, ui::style::px(computedStyle("count"), "min-width", 90)));
    col->addWidget(count);
    hint = add<ui::Label>("Click a slice to hear it. Drag a line to move it (it snaps to hits; Alt: it doesn't); "
                          "double-click to add a line, or on one to remove it. Keys: Up/Down choose a slice, "
                          "Left/Right move its start (Shift: more), Space hears it, Insert adds a line in its "
                          "middle, Delete removes its start.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    col->addWidget(hint);
    col->addStretch(1);
    auto buttons = std::make_unique<ui::HBox>();
    buttons->addStretch(1);
    sliceButton = ui::pushButton(this, "Slice", [this] { accept(); });
    sliceButton->setDefault(true);
    buttons->addWidget(sliceButton);
    buttons->addWidget(ui::pushButton(this, "Cancel", [this] { reject(); }));
    col->addLayout(std::move(buttons));
    auto made = std::make_shared<AudioBuffer>();
    made->samples = padkit::clipSound(*clip);
    sound = made;
    for (float v : made->samples) top_ = std::max(top_, std::fabs(v));
    onsets_ = std::make_unique<dsp::Onsets>(made->samples.data(), made->frames(), sr);
    view->setAudio(sound, clip->name, sr);
    view->setToolTip("Click a slice to hear it; drag a line to move it; double-click to add or remove one");
    mode->currentIndexChanged.connect([this](int) { findHits(); });
    sensitivity->valueChanged.connect([this](int) { setMode(Transients); });
    grid->currentIndexChanged.connect([this](int) { setMode(Grid); });
    sliceButton->setEnabled(frames() > 0);
    findHits();
    view->setFocus();
}

void SliceDialog::findHits() {
    found = onsets_ ? onsets_->find(sensitivity->value() / 100.0) : std::vector<int64_t>();
    int m = mode->currentIndex();
    if (m == Transients) {
        setHits(found);
    } else if (m == Grid) {
        setHits(padkit::gridStarts(*clip, frames(), timing, sr, GRID_DIVISIONS[std::min(3, std::max(0, grid->currentIndex()))]));
    } else {
        setHits(hits);
    }
}

void SliceDialog::setMode(Mode m) {
    mode->setCurrentIndexSilently(m);
    findHits();
}

void SliceDialog::setHits(std::vector<int64_t> made) {
    hits = sound ? padkit::sliceStarts(sound->samples.data(), frames(), std::move(made), INT_MAX, top_)
                 : std::vector<int64_t>();
    int n = (int)hits.size();
    chosen = std::min(std::max(0, chosen), std::max(0, n - 1));
    std::string text = std::to_string(n) + (n == 1 ? " slice" : " slices");
    if (n > PadKit::MAX_PADS)
        text += " \xe2\x80\x94 a Pad Kit has " + std::to_string(PadKit::MAX_PADS) + " pads: the first " +
                std::to_string(PadKit::MAX_PADS) + " become pads (the rest stay in the last)";
    count->setText(text);
    util::debugf("slicing %s: %d slices (%s)", clip->name.c_str(), n, mode->currentText().c_str());
    view->update();
    hitsChanged.emit();
}

void SliceDialog::edited() { mode->setCurrentIndexSilently(Manual); }

int SliceDialog::moveLine(int index, int64_t frame, bool snap, int64_t reach) {
    if (index < 0 || index >= (int)hits.size() || hits[(size_t)index] == 0) return index;  // (the clip's start stays)
    if (snap) frame = padkit::snapToHit(frame, found, reach);
    // between its neighbours (lines never pass each other)
    int64_t lo = index > 0 ? hits[(size_t)index - 1] + 1 : 1;
    int64_t hi = (size_t)index + 1 < hits.size() ? hits[(size_t)index + 1] - 1 : frames() - 1;
    if (lo > hi) return index;
    frame = std::min(hi, std::max(lo, frame));
    if (frame == hits[(size_t)index]) return index;
    std::vector<int64_t> v = hits;
    v[(size_t)index] = frame;
    edited();
    setHits(v);
    auto it = std::lower_bound(hits.begin(), hits.end(), frame);
    int at = it != hits.end() && *it == frame ? (int)(it - hits.begin()) : index;
    chosen = at;
    return at;
}

void SliceDialog::addLine(int64_t frame) {
    if (frame <= 0 || frame >= frames() || std::binary_search(hits.begin(), hits.end(), frame)) return;
    std::vector<int64_t> v = hits;
    v.push_back(frame);
    edited();
    setHits(v);
    chosen = sliceAt(frame);
    view->update();
}

void SliceDialog::removeLine(int index) {
    if (index < 0 || index >= (int)hits.size() || hits[(size_t)index] == 0) return;  // (the clip's start stays)
    std::vector<int64_t> v = hits;
    v.erase(v.begin() + index);
    edited();
    chosen = std::max(0, index - 1);
    setHits(v);
}

void SliceDialog::nudge(int direction, bool big) {
    if (chosen < 0 || chosen >= (int)hits.size()) return;
    int64_t step = std::max<int64_t>(1, std::llround((big ? NUDGE_BIG_S : NUDGE_S) * sr));
    moveLine(chosen, hits[(size_t)chosen] + direction * step, false, 0);
}

void SliceDialog::choose(int slice) {
    chosen = std::min(std::max(0, slice), std::max(0, (int)hits.size() - 1));
    view->update();
}

int SliceDialog::sliceAt(int64_t frame) const {
    auto it = std::upper_bound(hits.begin(), hits.end(), frame);
    return std::max(0, (int)(it - hits.begin()) - 1);
}

int64_t SliceDialog::sliceEnd(int slice) const {
    return slice >= 0 && (size_t)slice + 1 < hits.size() ? hits[(size_t)slice + 1] : frames();
}

void SliceDialog::audition(int slice) {
    choose(slice);
    audio::Engine* engine = sampler_dialog::host().engine;
    if (!engine || hits.empty() || !sound) return;
    int64_t a = hits[(size_t)chosen], b = sliceEnd(chosen);
    auto piece = std::make_shared<AudioBuffer>();
    piece->samples.assign(sound->samples.begin() + a * 2, sound->samples.begin() + b * 2);
    engine->preview(piece, 0, false, config::PREVIEW_GAIN);  // (the one heard before stops)
    voice_ = engine->previewVoice();
    util::debugf("slicing: hearing slice %d (%lld frames)", chosen + 1, (long long)(b - a));
}

void SliceDialog::done(int result) {
    audio::Engine* engine = sampler_dialog::host().engine;
    if (engine && voice_ && engine->previewVoice() == voice_) engine->stopPreview();
    voice_.reset();
    ui::Dialog::done(result);
}

}  // namespace pad_kit_dialog
