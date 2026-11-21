// A return track's row in the track panel (LanePanel::trackHeader hands returns here once their name and ✕ are
// placed):
//
//     | Delay                                   ✕ |   its name (NameLabel.return), remove
//     | [1/2] [✕] (pan)                           |   its output pair, mute (✕, lit while muted), its pan
//     | [⟋⟍] [FX] [====▼=========] [+0 dB]  S -  |   Auto (its automation lanes), its effects, the fader on its
//     |           [==============]          I -  |   meter (meter_fader.h), its dB, its loudness (short-term, integrated)
//
// Every control works as on the other rows: changes go to every selected track, a held fader or pan knob writes
// its armed automation lane, and the panel's meters / loudness / mixers maps hold its meter, display and fader.
#include <algorithm>

#include "audio/automation.h"
#include "audio/automation_write.h"
#include "config.h"
#include "core/util.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/meter_fader.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/transport_bar.h"
#include "ui/widgets.h"

void LanePanel::returnControls(LaneFrame* frame, ui::BoxLayout* col, const TrackPtr& track) {
    std::weak_ptr<bool> alive = alive_;
    Timeline* tl = &timeline;
    int gap = std::max(0, ui::style::px(frame->computedStyle(), "column-gap", 4));

    // --- its output pair, mute and pan ---
    auto second = std::make_unique<ui::HBox>();
    ui::HBox* row2 = second.get();
    row2->setSpacing(gap);
    col->addLayout(std::move(second));
    auto* meterFader = frame->add<MeterFader>(track->volumeDb, nullptr, channels);  // (wired below; made first: the
    LevelMeter* meter = meterFader->meter();                                        // output selector moves its meter)
    meter->showChannels(lane_panel::trackOutputs(*track));
    ui::ComboBox* outputs = widgets::outputSelector(frame, channels, track->output, [this, alive, track, meter](int first) {
        track->output = first;
        meter->showChannels(lane_panel::trackOutputs(*track));
        if (!alive.expired()) showUsedOutputs();  // the master's meter shows the outputs in use
    });
    outputs->setFixedHeight(theme::metric("output-selector-height", 18));
    row2->addWidget(outputs);
    row2->addWidget(widgets::toggleButton(frame, "✕", "mute", track->muted,
                                          [this, alive, tl, track](bool on) {
                                              auto ts = alive.expired() ? std::vector<TrackPtr>{track} : targets(track);
                                              for (auto& t : ts) t->muted = on;
                                              tl->update();  // silent clips are drawn grey
                                              util::debugf("return %s %s (%zu track(s))\n", track->name.c_str(),
                                                           on ? "muted" : "unmuted", ts.size());
                                              if (ts.size() > 1 && !alive.expired()) {
                                                  rebuild();  // (their buttons)
                                                  say(on ? "Mute" : "Unmute", ts.size());
                                              }
                                          },
                                          "Mute"));
    auto* pan = frame->add<PanKnob>(track->pan, [this, alive, track](double v) {
        if (!alive.expired())  // (the other selected tracks' by as much)
            for (auto& t : targets(track))
                if (t != track) {
                    t->pan = std::clamp(t->pan + v - track->pan, -1.0, 1.0);
                    showMixer(t);
                }
        track->pan = v;  // the mixer reads it every buffer
        automation::recorder().move(automation::armedControlLane(*track, automation::PAN), (v + 1) / 2);
    });
    pan->grabbed = [this, alive, track, pan](bool on) {
        if (on && !alive.expired()) say("Pan", targets(track).size());
        AutomationPtr lane = automation::armedControlLane(*track, automation::PAN);
        if (on) automation::recorder().grab(lane, (pan->pan + 1) / 2);
        else automation::recorder().release(lane);
    };
    row2->addWidget(pan);
    row2->addStretch(1);

    // --- Auto, FX, the fader on its meter, its dB, its loudness ---
    auto third = std::make_unique<ui::HBox>();
    ui::HBox* row3 = third.get();
    row3->setSpacing(gap);
    col->addLayout(std::move(third), 1);
    auto* autoButton = frame->add<ui::Button>("");
    autoButton->setStyle(ui::Button::Style::Tool);
    autoButton->setCheckable(true);
    autoButton->setCheckedSilently(track->showAutomation);
    autoButton->setFocusPolicy(ui::FocusPolicy::None);
    autoButton->setProperty("accent", "true");  // lit while its lanes show
    std::string svg;
    int iconPx = theme::metric("return-icon-size", 20);
    if (util::readFile(transport_bar::iconDir() + "/automation.svg", svg)) autoButton->setIconSvg(svg, iconPx);
    else autoButton->setText("Auto");  // (the icon file missing: its name instead)
    int side = theme::metric("return-button-size", 30);
    autoButton->setFixedSize(side, side);
    size_t count = track->automation.size();
    autoButton->setToolTip(count ? "Show or hide this track's automation (" + std::to_string(count) + " lane" +
                                       (count != 1 ? "s" : "") + ")"
                                 : "Automate this return's volume or pan, or a parameter of its effects");
    autoButton->toggled.connect([this, alive, track](bool on) {
        if (!alive.expired()) toggleAutomation(track, on);
    });
    row3->addWidget(autoButton);
    auto open = openPlugins;
    ui::Button* fx = widgets::fxButton(frame, *track->effects, [open, track] { if (open) open(track); });
    fx->setFixedSize(side, side);
    row3->addWidget(fx);

    auto toVolume = [](double db) { return (db - config::MIN_DB) / (double)(config::MAX_DB - config::MIN_DB); };
    meterFader->valueChanged.connect([this, alive, track, toVolume](int v) {
        double db = v;
        if (!alive.expired())  // (the other selected tracks' by as much)
            for (auto& t : targets(track))
                if (t != track) {
                    t->volumeDb = std::clamp(t->volumeDb + db - track->volumeDb, (double)config::MIN_DB,
                                             (double)config::MAX_DB);
                    showMixer(t);
                }
        track->volumeDb = db;  // the mixer reads it every buffer
        automation::recorder().move(automation::armedControlLane(*track, automation::VOLUME), toVolume(db));
    });
    meterFader->sliderPressed.connect([this, alive, track, meterFader, toVolume] {
        if (!alive.expired()) say("Volume", targets(track).size());
        automation::recorder().grab(automation::armedControlLane(*track, automation::VOLUME),
                                    toVolume(meterFader->value()));
    });
    meterFader->sliderReleased.connect(
        [track] { automation::recorder().release(automation::armedControlLane(*track, automation::VOLUME)); });
    meterFader->hPolicy = ui::SizePolicy::Expanding;
    row3->addWidget(meterFader, 1, AlignVCenter);
    auto* reading = frame->add<DbReading>(meterFader);
    reading->addStyleClass("boxed");  // (a box round it: Label.boxed)
    reading->setFixedHeight(side);
    row3->addWidget(reading, 0, AlignVCenter);
    auto* display = frame->add<LoudnessDisplay>();
    row3->addWidget(display, 0, AlignVCenter);

    meters[track] = meter;
    loudness[track] = display;
    mixers[track] = Mixer{meterFader, reading, pan};  // (automation shows its values on them while playing)
}
