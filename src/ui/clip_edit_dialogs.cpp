// The dialogs of editing many clips at once (clip_edit_dialogs.h).
#include "ui/clip_edit_dialogs.h"

#include <cstdio>

#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"

namespace clip_edit_dialogs {

std::function<bool(double& amount)> answerSlip;
std::function<bool(FadeChoice& choice)> answerFades;

namespace {

// A spin box that keeps the keyboard itself (not its editor): ←/→ step it as ↑/↓ do (by the grid), anything
// else is typed into it.
class GridSpin : public ui::SpinBox {
public:
    GridSpin() {
        focusProxy = nullptr;
        editor()->setFocusPolicy(ui::FocusPolicy::None);
    }
    void focusIn() override { update(); }
    bool overridesShortcut(const ui::KeyEvent& e) const override { return editor()->overridesShortcut(e); }
    bool keyPress(ui::KeyEvent& e) override {
        if ((e.key == plat::Key_Left || e.key == plat::Key_Right) && !(e.mods & (plat::ModCtrl | plat::ModAlt))) {
            stepBy(e.key == plat::Key_Right ? 1 : -1);
            return true;
        }
        if (ui::SpinBox::keyPress(e)) return true;
        return editor()->keyPress(e);  // (Return goes on: the dialog's OK)
    }
};

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

ui::ComboBox* shapeBox(ui::Dialog& d, FadeShape now) {
    auto* box = d.add<ui::ComboBox>();
    for (FadeShape s : FADE_SHAPES) {
        box->addItem(fadeShapeLabel(s), (int)s);
        if (s == now) box->setCurrentIndexSilently(box->count() - 1);
    }
    return box;
}

ui::SpinBox* msBox(ui::Dialog& d, double ms) {
    auto* box = d.add<ui::SpinBox>();
    box->setRange(0, 60000);
    box->setDecimals(1);
    box->setSuffix(" ms");
    box->setValueSilently(ms);
    return box;
}

}  // namespace

bool askSlip(ui::Window* parent, int clips, const std::string& unit, double step, double& amount) {
    if (answerSlip) return answerSlip(amount);
    ui::Dialog d(parent, "Slip contents", 360, 140);
    auto* v = ui::vbox(&d, theme::metric("input-dialog-margin", 12), theme::metric("input-dialog-spacing", 8));
    int fieldWidth = theme::metric("input-dialog-field-width", 280);
    std::string what = clips == 1 ? "the clip" : "the " + std::to_string(clips) + " selected clips";
    auto* lab = d.add<ui::Label>("Move the contents of " + what + " later (negative: earlier), in " + unit +
                                 ". The clips stay where they are.");
    lab->setWordWrap(true);
    lab->setMinimumWidth(fieldWidth);
    v->addWidget(lab);
    auto* box = d.add<GridSpin>();
    box->setRange(-100000, 100000);
    box->setDecimals(3);
    box->setSingleStep(step > 0 ? step : 1);
    box->setSuffix(" " + unit);
    box->setValueSilently(amount);
    box->setMinimumWidth(fieldWidth);
    v->addWidget(box);
    auto* hint = d.add<ui::Label>("Left/Right, Up/Down: a grid step (" + g(step) + " " + unit + ")");
    v->addWidget(hint);
    auto* bb = d.add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    v->addWidget(bb);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    d.adjustSize();
    d.setFocusWidget(box);
    if (d.exec() != ui::Dialog::Accepted) return false;
    amount = box->value();
    return true;
}

bool askFades(ui::Window* parent, int clips, FadeChoice& choice) {
    if (answerFades) return answerFades(choice);
    ui::Dialog d(parent, "Fades at every boundary", 380, 170);
    ui::FormLayout* form = ui::form(&d, 11, 6);
    form->addRow(d.add<ui::Label>(clips == 1 ? "For the selected audio clip"
                                             : "For the " + std::to_string(clips) + " selected audio clips"));
    ui::SpinBox* inMs = msBox(d, choice.inMs);
    ui::ComboBox* inShape = shapeBox(d, choice.inShape);
    auto inRow = std::make_unique<ui::HBox>();
    inRow->setSpacing(6);
    inRow->addWidget(inMs, 1);
    inRow->addWidget(inShape);
    form->addRow("Fade-in", std::move(inRow));
    ui::SpinBox* outMs = msBox(d, choice.outMs);
    ui::ComboBox* outShape = shapeBox(d, choice.outShape);
    auto outRow = std::make_unique<ui::HBox>();
    outRow->setSpacing(6);
    outRow->addWidget(outMs, 1);
    outRow->addWidget(outShape);
    form->addRow("Fade-out", std::move(outRow));
    auto* bb = d.add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    form->addRow(bb);
    d.adjustSize();
    d.setFocusWidget(inMs);
    if (d.exec() != ui::Dialog::Accepted) return false;
    choice.inMs = inMs->value();
    choice.outMs = outMs->value();
    choice.inShape = (FadeShape)inShape->currentIndex();  // (FADE_SHAPES' order: the enum's)
    choice.outShape = (FadeShape)outShape->currentIndex();
    return true;
}

}  // namespace clip_edit_dialogs
