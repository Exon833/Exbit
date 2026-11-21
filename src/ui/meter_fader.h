// A track's fader drawn on its level meter (a return track's row: lane_panel_return.cpp): the meter's bars, and
// above them a ▼ handle at the volume, on the meter's own scale: -60 dB at the bars' left edge, 0 dB at their right
// edge, so the handle stands over the level it lets through; the room past 0 dB (to MAX_DB) follows on the right.
//
// It is the track's VolumeSlider (valueChanged, sliderPressed / sliderReleased, a DbReading beside it, the panel's
// Mixer entry, automation writing while held: all as with the plain slider). Drag anywhere on it to move the handle
// there (Shift: finer); double-click for 0 dB. Like VolumeSlider it ignores the mouse wheel, so scrolling the panel
// never changes a volume. Clicks on the meter move the fader (the meter's own click, resetting its peaks, is Play's
// job here). The meter shows no peak number (the row has its loudness beside it).
//
// Styled as MeterFader (default.css): ::marker (the ▼: background-color, else the row's text colour; width and height)
// in its states :hover and :pressed; the meter inside is a LevelMeter (its own rules).
#pragma once
#include "ui/level_meter.h"
#include "ui/widgets.h"

class MeterFader : public VolumeSlider {
public:
    MeterFader(double db, std::function<void(double)> slot, int channels);
    LevelMeter* meter() const { return meter_; }
    int handleX(double db) const;     // the ▼'s centre for `db`, in its own coordinates
    double dbAt(int x) const;         // the dB a press at `x` sets (MIN_DB … MAX_DB, unrounded)
    void paint(Painter& p) override;
    void resized() override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool mouseDoubleClick(ui::MouseEvent& e) override;
    bool wheel(ui::WheelEvent& e) override { return false; }
    void enter() override { update(); }
    void leave() override { update(); }
    void styleStates(std::vector<std::string>& out) const override;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;

private:
    LevelMeter* meter_ = nullptr;
    bool dragging_ = false;
    int x0_ = 0;          // where MIN_DB is (the meter bars' left edge)…
    double span_ = 1;     // …and the px from there to 0 dB (the bars' width): dB → x is linear on the meter
    int grabX_ = 0;
    double grabDb_ = 0;
    int handleH() const;
};
