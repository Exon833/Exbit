// The Drawn synth's canvas (its window: synth_dialog, for a patch with Patch::drawn): one cycle of its wave,
// drawn by hand. Nothing else to set: press and drag across it to draw (the points between two moves joined, so
// a quick stroke leaves no gaps); what is drawn is heard at once.
//
// The keyboard (and tinydaw --tui, where the wave is drawn as text): Left / Right the point (Shift: 16 at a
// time), Home / End the first / last, Up / Down its value by a 32nd (Shift: a 256th).
//
// Styles (default.css): WaveCanvas (background-color, color: the wave's line), ::grid (color: the centre line
// and quarters), ::cursor (color: the keyboard's point).
#pragma once
#include <functional>
#include <vector>

#include "ui/toolkit/widget.h"

class WaveCanvas : public ui::Widget {
public:
    WaveCanvas();
    std::vector<float> wave;  // subsynth::WAVE_POINTS values -1…1, as shown
    int cursor = 0;           // the keyboard's point
    // every change made here (final: the mouse let go; keys: not final, the clips rendered a moment later)
    std::function<void(const std::vector<float>&, bool final)> changed;

    Rect area() const;              // where the wave is drawn
    int pointAt(int x) const;       // the point under x (clamped)
    double valueAt(int y) const;    // the value at y (clamped to -1…1)
    bool drawing() const { return last_ >= 0; }

    Point sizeHint() const override;
    Point minimumSizeHint() const override { return Point(160, 80); }  // (room to draw in, however small its window)
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override { return mousePress(e); }
    bool keyPress(ui::KeyEvent& e) override;
    // (as text) the wave across the columns, -1…1 down the rows; the centre line; the keyboard's point reversed
    bool textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const override;

private:
    int last_ = -1;         // while drawing: the point drawn last…
    double lastValue_ = 0;  // …and its value
    void drawTo(int point, double value);  // the points from the last to this one, along a straight line
};
