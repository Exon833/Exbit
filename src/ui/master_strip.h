// The master lane's controls (master_strip.py): LUFS and true peak (dBTP), effects (FX), mute, a level meter (a bar per
// output in use), and volume. Its row stays at the bottom of the screen, over the tracks (the main
// window pins it there with PinToBottom). Under the controls, slightly dimmed: the update notice
// (update_notice.h; MASTER_FOOTER_H tall).
//
// Its row is a LaneFrame of the class .master (default.css): its background-color (the master lane's colour), its
// padding and row-gap / column-gap (around and between its controls); ::title ("Master": its font) and ::notice
// (margin-top: above the update notice).
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "model/project_io.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/update_notice.h"
#include "ui/widgets.h"

namespace audio { class Engine; }
class ScopeView;

// Builds the master lane frame; `frame` is the widget to place (take it with releaseFrame() for
// PinToBottom), `meter` gets fed peaks and `loudness` LUFS values.
class MasterStrip {
public:
    MasterStrip(audio::Engine& engine, std::function<void()> openPlugins);
    ~MasterStrip();
    std::unique_ptr<ui::Widget> releaseFrame();  // hands the frame over (once); `frame` stays valid
    // what the update check found (a failure after an answer: the answer stays, the tooltip says it failed)
    void showUpdate(const update_check::Result& result);
    void refreshUpdate();  // the notice again: how long ago (every minute)
    void restyle();                                       // a new skin: the colours this row was built with
    void refreshFx(const std::vector<SlotPtr>& effects);  // (re)make the FX button (lit while there are any)
    project_io::MasterSettings settings() const;          // what a saved project keeps of the master
    void apply(const project_io::MasterSettings& s);
    void setMute(bool on);
    void setVolume(int db);

    audio::Engine& engine;
    std::function<void()> openPlugins;  // the master's plugins window (FX button)
    LaneFrame* frame;
    LoudnessDisplay* loudness;
    LoudnessDisplay* truePeak;  // the true peak since Play (audio/true_peak.h), next to the LUFS
    void showTruePeak(double dbtp);
    LevelMeter* meter;
    ui::Button* fx = nullptr;
    ui::Button* muteButton;
    ui::Slider* volume;
    DbReading* dbLabel;
    ui::Label* versionLabel = nullptr;  // (none any more: a rolling release has no version number)
    UpdateNotice* notice;
    ScopeView* scope;  // under the controls (ui/scope_view.h)
    // shows or hides the scope: the frame lanes::masterHeight() tall (set that first)
    void setScopeShown(bool on);
    std::optional<update_check::Result> updateResult;
    std::optional<update_check::Result> updateFailure;  // the last check's, while an earlier answer is shown

private:
    std::unique_ptr<LaneFrame> owned_;
    ui::HBox* top_;
};
