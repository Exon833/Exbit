// The dialogs of editing many clips at once (timeline_slip.cpp): Slip contents… and Fades at every boundary….
//
// Slip contents… asks how far to move the selected clips' contents inside them, in beats (or seconds when the
// song is edited in minutes and seconds); ←/→ (and ↑/↓) move the number by the grid's step, typing sets it.
// Fades at every boundary… asks one fade-in and one fade-out (length in ms, shape) for every selected audio clip.
//
// Tests: set the `answer…` hooks and the dialogs ask them instead of opening a window (false: Cancel).
#pragma once
#include <functional>
#include <string>

#include "model/model.h"
#include "ui/toolkit/widget.h"

namespace clip_edit_dialogs {

extern std::function<bool(double& amount)> answerSlip;
// `clips` selected; `unit` "beats" or "s"; `step` the grid's (in the unit); true (and `amount`) on OK
bool askSlip(ui::Window* parent, int clips, const std::string& unit, double step, double& amount);

struct FadeChoice {
    double inMs = 10, outMs = 10;
    FadeShape inShape = FadeShape::Linear, outShape = FadeShape::Linear;
};
extern std::function<bool(FadeChoice& choice)> answerFades;
// `clips` audio clips selected; true (and `choice`) on OK
bool askFades(ui::Window* parent, int clips, FadeChoice& choice);

}  // namespace clip_edit_dialogs
