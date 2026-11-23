// The rows beside the tempo lane and the time-signature lane (timeline.h), in the right-hand panel under the CPU
// monitor, as tall as their lanes (so the track rows under them line up) and shown with them:
//   Tempo      its name, ✕ (hides the lane: View → Tempo lane), and its BPM range: Min, Max and Auto (fitted to the
//              tempo changes). A view saved with the song (Timing::tempoLaneLo / Hi), not an undo step. Max not
//              above Min falls back to Auto (Timeline::setTempoLaneRange).
//   Signature  its name and ✕ (View → Time signature lane).
//
// Styled as TimingLaneRow (default.css): a box (background, border, padding; row-gap between its lines, column-gap
// between its controls), classes .tempo and .signature; its name a `Label.name`, its ✕ a `Button.close`.
#pragma once
#include <functional>

#include "ui/toolkit/controls.h"

class Timeline;

class TimingLaneRow : public ui::Widget {
public:
    enum class Kind { Tempo, Signature };
    TimingLaneRow(Kind kind, Timeline& timeline, std::function<void()> hide);
    Kind kind;
    ui::Label* name;
    ui::Button* close;                 // ✕
    ui::SpinBox* minBox = nullptr;     // (the tempo row's)
    ui::SpinBox* maxBox = nullptr;
    ui::CheckBox* autoBox = nullptr;
    void refresh();                    // its values from the song (Auto: the range fitted now); not while typed in
    void fit();                        // as wide as the panel, as tall as its lane (hidden with it)
    void paint(Painter& p) override;
    void resized() override { fit(); }

private:
    Timeline& timeline_;
    void rangeTyped();                 // Min or Max typed: that range (Auto off)
};
