#include "ui/record_arm.h"

#include <algorithm>

#include "core/util.h"
#include "gfx/svg.h"
#include "ui/recording.h"
#include "ui/theme.h"

namespace {
// a speaker; with sound coming out (one wave: Auto, two: In) or crossed out (Off)
std::string speakerSvg(Track::Monitor m) {
    std::string s = "<svg width=\"512\" height=\"512\" viewBox=\"0 0 512 512\" xmlns=\"http://www.w3.org/2000/svg\">"
                    "<path fill=\"#ffffff\" d=\"M 48,190 H 140 L 260,80 V 432 L 140,322 H 48 Z\"/>";
    const char* wave = "<path fill=\"none\" stroke=\"#ffffff\" stroke-width=\"40\" stroke-linecap=\"round\" d=\"";
    if (m == Track::Monitor::Off) {
        s += "<path fill=\"none\" stroke=\"#ffffff\" stroke-width=\"44\" stroke-linecap=\"round\" "
             "d=\"M 320,190 L 450,320 M 450,190 L 320,320\"/>";
    } else {
        s += std::string(wave) + "M 320,196 A 80,80 0 0 1 320,316\"/>";
        if (m == Track::Monitor::In) s += std::string(wave) + "M 380,136 A 160,160 0 0 1 380,376\"/>";
    }
    return s + "</svg>";
}
}  // namespace

namespace record_arm {

const char* monitorName(Track::Monitor m) {
    return m == Track::Monitor::In ? "In" : m == Track::Monitor::Off ? "Off" : "Auto";
}

Track::Monitor nextMonitor(Track::Monitor m) {
    return m == Track::Monitor::Auto ? Track::Monitor::In : m == Track::Monitor::In ? Track::Monitor::Off
                                                                                     : Track::Monitor::Auto;
}

ui::ComboBox* inputSelector(ui::Widget* parent, const Track& track, int inputs,
                            std::function<void(const std::string&)> chosen) {
    auto* combo = parent->add<ui::ComboBox>();
    combo->setWheelEnabled(false);  // scrolling the panel never changes a value by accident
    combo->setFocusPolicy(ui::FocusPolicy::None);
    combo->setFixedWidth(theme::metric("input-selector-width", 46));
    std::string own = recording::inputSetting(track);
    bool listed = false;
    for (auto& [label, setting] : recording::trackInputChoices(inputs)) {
        // (short: "1", "1/2"; the list says which are pairs)
        combo->addItem(setting == "none" ? "None" : setting, setting);
        listed = listed || setting == own;
    }
    if (!listed) combo->addItem(own + " ?", own);  // (the device lacks it: kept, so nothing changes by itself)
    combo->setCurrentIndexSilently(std::max(0, combo->findData(own)));
    combo->setToolTip("Input: " + recording::inputName(track) +
                      (listed ? "" : " (this audio device lacks it)") +
                      ". What this track records from when armed (●), and hears through it when monitoring");
    combo->currentIndexChanged.connect([combo, chosen](int i) {
        if (chosen && i >= 0) chosen(combo->itemData(i));
    });
    return combo;
}

}  // namespace record_arm

ArmButton::ArmButton(TrackPtr t, std::function<void(bool, bool)> changed)
    : ui::Button("●"), track(std::move(t)), changed_(std::move(changed)) {
    setStyle(ui::Button::Style::Tool);  // (as widgets::toggleButton)
    setFixedSize(theme::metric("toggle-button-width", 26), theme::metric("row-button-height", 22));
    setFocusPolicy(ui::FocusPolicy::None);
    setCheckable(true);
    setCheckedSilently(track->armed);
    setProperty("toggle", "record");
    setToolTip("Arm for recording: ● Rec records every armed track at once, each from its own input (click: this "
               "track, or the selected ones; Shift-click: arm more)");
    toggled.connect([this](bool on) {
        if (changed_) changed_(on, shift_);
    });
}

bool ArmButton::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton) shift_ = e.shift();  // (the release toggles it)
    return ui::Button::mousePress(e);
}

MonitorButton::MonitorButton(TrackPtr t, std::function<void(Track::Monitor)> changed)
    : ui::Button(""), track(std::move(t)), changed_(std::move(changed)) {
    setStyle(ui::Button::Style::Tool);
    setFixedSize(theme::metric("toggle-button-width", 26), theme::metric("row-button-height", 22));
    setFocusPolicy(ui::FocusPolicy::None);
    setCheckable(true);  // (lit while In)
    setProperty("toggle", "accent");
    toggled.connect([this](bool) {
        Track::Monitor next = record_arm::nextMonitor(track->monitor);
        track->monitor = next;
        util::debugf("monitoring: %s %s\n", track->name.c_str(), record_arm::monitorName(next));
        show();
        if (changed_) changed_(next);
    });
    show();
}

void MonitorButton::show() {
    setCheckedSilently(track->monitor == Track::Monitor::In);
    const char* what = track->monitor == Track::Monitor::In    ? "In: always hears its input"
                       : track->monitor == Track::Monitor::Off ? "Off: never hears its input"
                                                                : "Auto: hears its input while armed (●)";
    setToolTip(std::string("Monitoring ") + what +
               " (through its effects, fader and pan). Click: Auto → In → Off; right-click to choose");
    update();
}

void MonitorButton::fillMenu(ui::Menu& menu) {
    for (Track::Monitor m : {Track::Monitor::Auto, Track::Monitor::In, Track::Monitor::Off})
        menu.addCheckable(std::string("Monitoring: ") + record_arm::monitorName(m), track->monitor == m, [this, m](bool) {
            track->monitor = m;
            util::debugf("monitoring: %s %s\n", track->name.c_str(), record_arm::monitorName(m));
            show();
            if (changed_) changed_(m);
        });
}

bool MonitorButton::contextMenu(ui::MouseEvent& e) {
    ui::Menu menu;
    fillMenu(menu);
    menu.exec(e.globalPos, this);
    return true;
}

void MonitorButton::paint(Painter& p) {
    ui::Button::paint(p);
    int px = std::max(8, std::min(width(), height()) - 8);
    Color c = textColor();
    if (icon_.width() != px || iconFor_ != c || iconOf_ != track->monitor) {
        icon_ = renderSvgTinted(speakerSvg(track->monitor), px, px, c);
        iconFor_ = c;
        iconOf_ = track->monitor;
    }
    p.drawImage(Point((width() - px) / 2, (height() - px) / 2), icon_);
}
