// Right-click an audio clip → Gain envelope points… (model/clip_envelope.h).
#include "ui/gain_envelope_dialog.h"

#include <algorithm>
#include <cmath>

#include "config.h"
#include "core/util.h"
#include "model/clip_envelope.h"
#include "ui/toolkit/layout.h"

std::function<bool(GainEnvelopeDialog&)> GainEnvelopeDialog::testRun;

GainEnvelopeDialog::GainEnvelopeDialog(ui::Window* parent, const Clip& clip, int sr)
    : ui::Dialog(parent, "Gain envelope: " + clip.name, 380, 420), offset_(clip.offset), sr_(sr) {
    if (auto envelope = clip.gainEnvelope) points = *envelope;
    auto* col = ui::styleRoom(ui::vbox(this, 10, 6), computedStyle());
    auto* hint = add<ui::Label>("Its volume over time, before the track's effects (s from the clip's start):");
    hint->setProperty("hint", "true");
    col->addWidget(hint);
    list = add<ui::ListWidget>();
    list->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    list->currentRowChanged.connect([this](int row) {
        if (row < 0 || row >= (int)points.size()) return;
        time->setValueSilently((points[(size_t)row].t - (double)offset_) / sr_);
        gain->setValueSilently(std::max((double)config::MIN_DB, clip_envelope::dbOf(points[(size_t)row].v)));
    });
    col->addWidget(list, 1);
    auto formLayout = std::make_unique<ui::FormLayout>();
    ui::FormLayout* form = formLayout.get();
    col->addLayout(std::move(formLayout));
    time = add<ui::SpinBox>();
    time->setRange(-(double)offset_ / sr, 24 * 3600.0);
    time->setDecimals(3);
    time->setSingleStep(0.1);
    time->setSuffix(" s");
    form->addRow("Time", time);
    gain = add<ui::SpinBox>();
    gain->setRange(config::MIN_DB, clip_envelope::MAX_DB);
    gain->setDecimals(1);
    gain->setSuffix(" dB");
    gain->textFromValue = [](double db) { return db <= config::MIN_DB ? std::string("-inf dB") : util::format("%+.1f dB", db); };
    gain->setValueSilently(0);
    gain->setToolTip("Up to +24 dB; the lowest is silence");
    form->addRow("Gain", gain);
    auto row = std::make_unique<ui::HBox>();
    row->setSpacing(6);
    addButton = add<ui::Button>("Add");
    addButton->setToolTip("A point at this time and gain");
    addButton->clicked.connect([this] { addPoint(); });
    changeButton = add<ui::Button>("Change");
    changeButton->setToolTip("The point chosen in the list: to this time and gain");
    changeButton->clicked.connect([this] { changePoint(); });
    removeButton = add<ui::Button>("Remove");
    removeButton->clicked.connect([this] { removePoint(); });
    row->addWidget(addButton);
    row->addWidget(changeButton);
    row->addWidget(removeButton);
    col->addLayout(std::move(row));
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([this] { accept(); });
    buttons->rejected.connect([this] { reject(); });
    col->addWidget(buttons);
    // (Return in the fields adds the point typed: type a time, Tab, a gain, Return; OK when done)
    if (ui::Button* ok = buttons->button(ui::ButtonBox::Ok)) ok->setDefault(false);
    addButton->setDefault(true);
    fill(points.empty() ? -1 : 0);
    time->setFocus();
}

AutoPoint GainEnvelopeDialog::typed() const {
    return {std::max(0.0, (double)offset_ + time->value() * sr_), clip_envelope::valueOf(gain->value()), 0.0};
}

void GainEnvelopeDialog::fill(int select) {
    list->clear();
    for (auto& p : points)
        list->addItem(util::format("%9.3f s    %s", (p.t - (double)offset_) / sr_, clip_envelope::dbText(p.v).c_str()));
    if (points.empty()) {
        ui::ListWidget::Item none;
        none.text = "(no points: the clip plays at its own gain)";
        none.enabled = false;
        none.selectable = false;
        list->addItem(none);
    }
    if (select >= 0 && select < (int)points.size()) list->setCurrentRow(select);
    changeButton->setEnabled(!points.empty());
    removeButton->setEnabled(!points.empty());
}

void GainEnvelopeDialog::addPoint() {
    AutoPoint p = typed();
    auto same = std::find_if(points.begin(), points.end(), [&](const AutoPoint& q) { return q.t == p.t; });
    if (same != points.end()) {
        same->v = p.v;
    } else {
        auto at = std::upper_bound(points.begin(), points.end(), p.t, [](double t, const AutoPoint& q) { return t < q.t; });
        points.insert(at, p);
    }
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].t == p.t) fill((int)i);
    util::debugf("gain envelope: a point at %.3f s, %s (%zu points)\n", time->value(), clip_envelope::dbText(p.v).c_str(),
                 points.size());
}

void GainEnvelopeDialog::changePoint() {
    int row = list->currentRow();
    if (row < 0 || row >= (int)points.size()) return;
    double bend = points[(size_t)row].bend;
    points.erase(points.begin() + row);
    addPoint();
    for (auto& p : points)
        if (p.t == typed().t) p.bend = bend;  // (its curve kept)
}

void GainEnvelopeDialog::removePoint() {
    int row = list->currentRow();
    if (row < 0 || row >= (int)points.size()) return;
    points.erase(points.begin() + row);
    fill(std::min(row, (int)points.size() - 1));
}

bool GainEnvelopeDialog::run() {
    if (testRun) return testRun(*this);
    return exec() == Accepted;
}
