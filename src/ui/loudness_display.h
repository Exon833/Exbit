// Small two-line LUFS readout used on every track row and the master row (loudness_display.py):
// short-term (S, last 3 s) on top, integrated (I, since Play was pressed) below; momentary
// (last 0.4 s) is in the tooltip.
#pragma once
#include <string>

#include "ui/toolkit/controls.h"

namespace loudness_display {
std::string fmtLufs(double value);  // "-23.4", "–" for -inf
std::string text(double shortTerm, double integrated);  // "S -23.4\nI     –"
std::string tip(double momentary);
}  // namespace loudness_display

class LoudnessDisplay : public ui::Label {
public:
    LoudnessDisplay();
    void showValues(double momentary, double shortTerm, double integrated);
    // a row too low for both lines squeezes it, as Qt's layouts do (the font's blank margins go first)
    Point minimumSizeHint() const override { return Point(62, 0); }
    // (painted so that when squeezed, only the lines' blank margins go: each line keeps its digits)
    void paint(Painter& p) override;
};
