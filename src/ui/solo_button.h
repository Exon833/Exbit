// A track's S button (solo), with the mixer's solo essentials:
//   - a click solos the track or lets it go; solo is additive (several tracks soloed are all heard);
//   - Alt-click is EXCLUSIVE SOLO: this track soloed, every other track and group let go;
//   - right-click: a menu with "Solo safe" (Track::soloSafe: the track stays heard while others are soloed,
//     as return tracks do; a lock shows on the S) and "Solo only this track" (exclusive solo, for window
//     managers that take Alt-click for themselves).
// Styled as the other toggle buttons (toggle role "solo").
#pragma once
#include <functional>

#include "model/model.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/menu.h"

class SoloButton : public ui::Button {
public:
    // what a click or the menu changed: the track's solo, its solo safe, or EXCLUSIVE (Alt-click or the menu's
    // "Solo only this track"), for the caller to do (Project::soloOnly) and show on every row
    enum class Change { Solo, Safe, Exclusive };
    SoloButton(TrackPtr track, std::function<void(Change)> changed);
    void fillMenu(ui::Menu& menu);  // the right-click menu
    void showSafe();                // the lock and tooltip as Track::soloSafe is now
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseRelease(ui::MouseEvent& e) override;
    bool contextMenu(ui::MouseEvent& e) override;
    void paint(Painter& p) override;
    TrackPtr track;

private:
    std::function<void(Change)> changed_;
    bool alt_ = false;  // the click being made holds Alt
    Image lock_;        // (drawn in lockFor_)
    Color lockFor_;
};
