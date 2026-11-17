// Small dialogs of the timeline and the lane panel (QInputDialog.getDouble / getInt).
#include "ui/track_dialogs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"

namespace track_dialogs {

std::function<bool(const std::string& title, std::string& value)> answer;

namespace {
std::string number(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// a spin box under a label, OK / Cancel: true when accepted
bool askNumber(ui::Window* parent, const std::string& title, const std::string& label, double& value, double lo,
               double hi, int decimals) {
    if (answer) {
        std::string text = number(value, decimals);
        if (!answer(title, text)) return false;
        double v = std::strtod(text.c_str(), nullptr);
        value = std::min(hi, std::max(lo, v));
        return true;
    }
    ui::Dialog d(parent, title, 320, 120);
    auto* v = ui::vbox(&d, 12, 8);
    auto* lab = d.add<ui::Label>(label);
    lab->setWordWrap(true);
    lab->setMinimumWidth(280);
    v->addWidget(lab);
    auto* box = d.add<ui::SpinBox>();
    box->setRange(lo, hi);
    box->setDecimals(decimals);
    box->setValueSilently(value);
    box->setMinimumWidth(280);
    v->addWidget(box);
    v->addSpacing(4);
    auto* bb = d.add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    v->addWidget(bb);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    d.adjustSize();
    d.setFocusWidget(box);
    if (d.exec() != ui::Dialog::Accepted) return false;
    value = box->value();
    return true;
}
}  // namespace

bool getDouble(ui::Window* parent, const std::string& title, const std::string& label, double& value, double lo,
               double hi, int decimals) {
    double v = std::min(hi, std::max(lo, value));
    if (!askNumber(parent, title, label, v, lo, hi, decimals)) return false;
    double scale = std::pow(10.0, decimals);
    value = std::nearbyint(v * scale) / scale;
    return true;
}

bool getInt(ui::Window* parent, const std::string& title, const std::string& label, int& value, int lo, int hi) {
    double v = std::min<double>(hi, std::max<double>(lo, value));
    if (!askNumber(parent, title, label, v, lo, hi, 0)) return false;
    value = (int)std::lround(v);
    return true;
}

bool getText(ui::Window* parent, const std::string& title, const std::string& label, std::string& text) {
    if (answer) return answer(title, text);
    return ui::getText(parent, title, label, text);
}

bool askTrackDelay(ui::Window* parent, Track& track) {
    double ms = track.delayMs;
    if (!getDouble(parent, "Track delay", track.name + " plays this much later (negative: earlier), in ms:", ms, -500.0,
                   500.0, 1))
        return false;
    track.delayMs = ms;
    return true;
}

bool askBendRange(ui::Window* parent, Track& track) {
    int range = track.bendRange;
    if (!getInt(parent, "Pitch bend range",
                track.name + ": semitones the bend wheel bends the built-in synth or sampler (all the way):", range, 1,
                24))
        return false;
    if (range == track.bendRange) return false;
    track.bendRange = range;
    return true;
}

}  // namespace track_dialogs
