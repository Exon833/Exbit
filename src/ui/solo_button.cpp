#include "ui/solo_button.h"

#include <algorithm>

#include "core/util.h"
#include "gfx/svg.h"
#include "ui/theme.h"

namespace {
const char* LOCK_SVG =
    "<svg width=\"512\" height=\"512\" viewBox=\"0 0 512 512\" xmlns=\"http://www.w3.org/2000/svg\">"
    "<path fill=\"none\" stroke=\"#ffffff\" stroke-width=\"64\" d=\"M 160,250 V 170 A 96,96 0 0 1 352,170 V 250\"/>"
    "<rect fill=\"#ffffff\" x=\"96\" y=\"230\" width=\"320\" height=\"250\" rx=\"32\"/></svg>";
}  // namespace

SoloButton::SoloButton(TrackPtr t, std::function<void(Change)> changed)
    : ui::Button("S"), track(std::move(t)), changed_(std::move(changed)) {
    setStyle(ui::Button::Style::Tool);  // (as widgets::toggleButton)
    setFixedSize(theme::metric("toggle-button-width", 26), theme::metric("row-button-height", 22));
    setFocusPolicy(ui::FocusPolicy::None);
    setCheckable(true);
    setCheckedSilently(track->soloed);
    setProperty("toggle", "solo");
    showSafe();
    toggled.connect([this](bool on) {
        if (alt_) {  // exclusive solo: it stays lit, whatever it was
            setCheckedSilently(true);
            util::debugf("solo: %s alone (exclusive solo)", track->name.c_str());
            if (changed_) changed_(Change::Exclusive);
            return;
        }
        track->soloed = on;
        util::debugf("solo: %s %s", track->name.c_str(), on ? "on" : "off");
        if (changed_) changed_(Change::Solo);
    });
}

void SoloButton::showSafe() {
    setToolTip(track->soloSafe ? "Solo (Alt-click: this track alone)\nSolo safe (the lock): heard even while other "
                                 "tracks are soloed; right-click to change"
                               : "Solo (Alt-click: this track alone; right-click: solo safe)");
    update();
}

void SoloButton::fillMenu(ui::Menu& menu) {
    menu.addCheckable("Solo safe", track->soloSafe, [this](bool on) {
        track->soloSafe = on;
        util::debugf("solo safe: %s %s", track->name.c_str(), on ? "on" : "off");
        showSafe();
        if (changed_) changed_(Change::Safe);
    })->tooltip = "Keep this track heard while other tracks are soloed (as return tracks are)";
    menu.addAction("Solo only this track", [this] {
        setCheckedSilently(true);
        util::debugf("solo: %s alone (exclusive solo)", track->name.c_str());
        if (changed_) changed_(Change::Exclusive);
    })->tooltip = "Exclusive solo (Alt-click on S): every other track and group stops being soloed";
}

bool SoloButton::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton) alt_ = e.alt();  // (the release toggles it)
    return ui::Button::mousePress(e);
}

bool SoloButton::mouseRelease(ui::MouseEvent& e) {
    bool handled = ui::Button::mouseRelease(e);  // (toggles it: with alt_ while held)
    alt_ = false;
    return handled;
}

bool SoloButton::contextMenu(ui::MouseEvent& e) {
    ui::Menu menu;
    fillMenu(menu);
    menu.exec(e.globalPos, this);
    return true;
}

void SoloButton::paint(Painter& p) {
    ui::Button::paint(p);
    if (!track->soloSafe) return;
    int px = std::max(6, height() * 2 / 5);  // a small lock in the top right corner
    Color c = textColor();
    if (lock_.width() != px || lockFor_ != c) {
        lock_ = renderSvgTinted(LOCK_SVG, px, px, c);
        lockFor_ = c;
    }
    p.drawImage(Point(width() - px - 2, 2), lock_);
}
