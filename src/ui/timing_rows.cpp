#include "ui/timing_rows.h"

#include <cmath>

#include "config.h"
#include "core/util.h"
#include "ui/lanes.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

TimingLaneRow::TimingLaneRow(Kind kind_, Timeline& timeline, std::function<void()> hide)
    : kind(kind_), timeline_(timeline) {
    setStyleType("TimingLaneRow");
    bool tempo = kind == Kind::Tempo;
    addStyleClass(tempo ? "tempo" : "signature");
    auto* lines = ui::styleRoom(ui::vbox(this, 0, 0), computedStyle());
    auto top = std::make_unique<ui::HBox>();
    top->setSpacing(4);
    name = add<ui::Label>(tempo ? "Tempo" : "Signature");
    name->addStyleClass("name");
    name->setToolTip(tempo ? "The tempo lane beside it: click it to add a tempo change, drag one, double-click to type it"
                           : "The time-signature lane beside it: click a bar to add one, drag a block's start along "
                             "the bars, double-click to type it, right-click for its menu");
    int closeSize = theme::metric("dock-close-size", 18);
    close = ui::toolButton(this, "✕", [hide] { if (hide) hide(); },
                           tempo ? "Hide the tempo lane (View → Tempo lane)" : "Hide the time-signature lane (View → Time "
                                                                               "signature lane)");
    close->addStyleClass("close");
    close->setProperty("autoRaise", "true");
    close->setFixedSize(closeSize, closeSize);
    top->addWidget(name, 1);
    top->addWidget(close);
    lines->addLayout(std::move(top));
    if (tempo) {
        auto range = std::make_unique<ui::HBox>();
        range->setSpacing(4);
        auto spin = [this](const std::string& tip) {
            auto* b = add<ui::SpinBox>();
            b->setRange(config::MIN_BPM, config::MAX_BPM);
            b->setKeyboardTracking(false);  // (the range once typed: Return or leaving the box)
            b->setToolTip(tip);
            b->valueChanged.connect([this](double) { rangeTyped(); });
            return b;
        };
        minBox = spin("The tempo at the lane's bottom (BPM). Typing one switches Auto off");
        maxBox = spin("The tempo at the lane's top (BPM). Typing one switches Auto off; it must be above Min");
        autoBox = add<ui::CheckBox>("Auto");
        autoBox->setToolTip("On: the range fits the tempo changes (rounded to tens). Off: Min to Max");
        autoBox->toggled.connect([this](bool on) {
            double lo, hi;
            timeline_.tempoRange(lo, hi);  // (Auto off: the range it fits now, fixed)
            if (on) timeline_.setTempoLaneRange(0, 0);
            else timeline_.setTempoLaneRange(lo, hi);
            refresh();
        });
        range->addWidget(add<ui::Label>("Min"));
        range->addWidget(minBox, 1);
        range->addWidget(add<ui::Label>("Max"));
        range->addWidget(maxBox, 1);
        range->addWidget(autoBox);
        lines->addLayout(std::move(range));
    }
    lines->addStretch(1);
    fit();
    refresh();
}

void TimingLaneRow::fit() {
    bool tempo = kind == Kind::Tempo;
    bool shown = tempo ? lanes::tempoLaneShown() : lanes::signatureLaneShown();
    Point size(theme::metric("panel-width", config::PANEL_W), tempo ? lanes::tempoLaneHeight() : lanes::signatureLaneHeight());
    if (shown && (minimumSize() != size || maximumSize() != size)) setFixedSize(size.x, size.y);
    if (isVisible() != shown) setVisible(shown);
}

void TimingLaneRow::refresh() {
    if (kind != Kind::Tempo) return;
    const Timing& t = timeline_.project.timing;
    double lo, hi;
    timeline_.tempoRange(lo, hi);
    // (a box being typed in keeps what is typed)
    if (!minBox->hasFocus() && minBox->value() != lo) minBox->setValueSilently(lo);
    if (!maxBox->hasFocus() && maxBox->value() != hi) maxBox->setValueSilently(hi);
    if (autoBox->isChecked() == t.tempoLaneFixed()) autoBox->setCheckedSilently(!t.tempoLaneFixed());
}

void TimingLaneRow::rangeTyped() {
    double lo = minBox->value(), hi = maxBox->value();
    util::debugf("tempo lane row: %g…%g BPM typed\n", lo, hi);
    timeline_.setTempoLaneRange(lo, hi);  // (Max not above Min: Auto, with a word in the status bar)
    autoBox->setCheckedSilently(!timeline_.project.timing.tempoLaneFixed());
}

void TimingLaneRow::paint(Painter& p) { ui::style::box(p, rect(), computedStyle()); }
