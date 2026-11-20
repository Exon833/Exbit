// A track row's recording controls (ui/recording.h RECORDING SEVERAL TRACKS): ● arms the track (ArmButton),
// the speaker its monitoring (MonitorButton: a click goes Auto → In → Off; right-click to choose), and an audio
// track's input chooser (inputSelector: None, each device input alone, each pair). Styled as the row's other
// toggles: ● with toggle role "record", the speaker lit (role "accent") while set to In.
#pragma once
#include <functional>

#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"

namespace record_arm {
// what a monitoring setting is called ("Auto", "In", "Off"), and the next one a click gives
const char* monitorName(Track::Monitor m);
Track::Monitor nextMonitor(Track::Monitor m);
// an audio track's input chooser: None, then the device's `inputs` (the track's own kept even when the device
// lacks it); `chosen(setting)` with recording::inputChoices's settings ("none", "1", "1/2")
ui::ComboBox* inputSelector(ui::Widget* parent, const Track& track, int inputs,
                            std::function<void(const std::string&)> chosen);
}  // namespace record_arm

// ● on a track's row: a click arms the track (`changed(on, more)`: more = Shift held, arming it besides the others)
class ArmButton : public ui::Button {
public:
    ArmButton(TrackPtr track, std::function<void(bool on, bool more)> changed);
    bool mousePress(ui::MouseEvent& e) override;
    TrackPtr track;

private:
    std::function<void(bool, bool)> changed_;
    bool shift_ = false;  // (the click being made holds Shift)
};

// the speaker on an audio track's row: its monitoring (a click: the next setting; right-click: a menu)
class MonitorButton : public ui::Button {
public:
    MonitorButton(TrackPtr track, std::function<void(Track::Monitor)> changed);
    void show();  // its look and tooltip as the track's monitoring is now
    bool contextMenu(ui::MouseEvent& e) override;
    void fillMenu(ui::Menu& menu);
    void paint(Painter& p) override;
    TrackPtr track;

private:
    std::function<void(Track::Monitor)> changed_;
    Image icon_;
    Color iconFor_;
    Track::Monitor iconOf_ = Track::Monitor::Auto;
};
