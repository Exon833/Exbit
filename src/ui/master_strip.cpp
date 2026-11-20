// The master lane's controls (master_strip.py).
#include "ui/master_strip.h"

#include "audio/dsp.h"
#include "audio/engine.h"
#include <cmath>

#include "ui/lanes.h"
#include "ui/scope_view.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"

MasterStrip::MasterStrip(audio::Engine& engine_, std::function<void()> openPlugins_)
    : engine(engine_), openPlugins(std::move(openPlugins_)) {
    owned_ = std::make_unique<LaneFrame>(Color(), std::nullopt, lanes::masterHeight());
    frame = owned_.get();
    frame->addStyleClass("master");  // (default.css: LaneFrame.master)
    const css::Style& st = frame->computedStyle();
    frame->setColors(ui::style::background(st));
    ui::style::Insets pad = ui::style::insets(st, "padding");
    ui::VBox* col = ui::vbox(frame, 0, ui::style::px(st, "row-gap", 2));
    col->setContentsMargins(pad.left, pad.top, pad.right, pad.bottom);

    auto top = std::make_unique<ui::HBox>();
    top_ = top.get();
    top_->setSpacing(ui::style::px(st, "column-gap", 2));
    col->addLayout(std::move(top));
    auto* title = frame->add<ui::Label>("Master");
    title->setFont(ui::style::font(frame->computedStyle("title"), theme::uiFont()));
    top_->addWidget(title, 1);
    loudness = frame->add<LoudnessDisplay>();
    top_->addWidget(loudness);
    truePeak = frame->add<LoudnessDisplay>();
    truePeak->setFixedWidth(theme::metric("true-peak-width", 40));
    top_->addWidget(truePeak);
    showTruePeak(-INFINITY);
    refreshFx({});
    muteButton = widgets::toggleButton(frame, "M", "mute", false, [this](bool on) { setMute(on); },
                                       "Mute the master output");
    top_->addWidget(muteButton);

    meter = frame->add<LevelMeter>(engine.channels, level_meter::MASTER_CLIP_DB);  // (warns just under full scale)
    col->addWidget(meter, 1);

    auto bottom = std::make_unique<ui::HBox>();
    ui::HBox* bottomRow = bottom.get();
    col->addLayout(std::move(bottom));
    volume = frame->add<ui::Slider>(ui::Orientation::Horizontal);
    volume->setRange(config::MIN_DB, config::MAX_DB);
    volume->setValueSilently(0);
    volume->setFocusPolicy(ui::FocusPolicy::None);
    volume->setToolTip("Master volume (double-click the dB value to reset)");
    volume->valueChanged.connect([this](int db) { setVolume(db); });
    volume->hPolicy = ui::SizePolicy::Expanding;
    dbLabel = frame->add<DbReading>(volume);
    bottomRow->addWidget(volume, 1);
    bottomRow->addWidget(dbLabel);

    versionLabel = nullptr;  // (a rolling release: no version number to show)
    notice = frame->add<UpdateNotice>();
    notice->setToolTip(update_notice::noticeTip(nullptr));
    scope = frame->add<ScopeView>(engine);
    col->addWidget(scope);
    col->addSpacing(ui::style::px(frame->computedStyle("notice"), "margin-top", 2));
    col->addWidget(notice);
    setVolume(0);
}

MasterStrip::~MasterStrip() = default;

std::unique_ptr<ui::Widget> MasterStrip::releaseFrame() { return std::unique_ptr<ui::Widget>(owned_.release()); }

void MasterStrip::showUpdate(const update_check::Result& result) {
    if (result.state == "failed" && updateResult && updateResult->state != "failed") {
        updateFailure = result;  // (offline a while: what was found last stays shown)
    } else {
        updateResult = result;
        updateFailure.reset();
    }
    refreshUpdate();
}

void MasterStrip::refreshUpdate() {
    const update_check::Result* r = updateResult ? &*updateResult : nullptr;
    notice->setNotice(update_notice::noticeText(r));
    notice->setToolTip(update_notice::noticeTip(r, 0, updateFailure ? &*updateFailure : nullptr));
}

void MasterStrip::restyle() {
    frame->setColors(ui::style::background(frame->computedStyle()));  // (LaneFrame.master)
    refreshUpdate();  // (its link's colour)
}

void MasterStrip::refreshFx(const std::vector<SlotPtr>& effects) {
    ui::Button* button = widgets::fxButton(frame, effects, [this] {
        if (openPlugins) openPlugins();
    });
    button->setToolTip(button->toolTip() + "\nMaster effects work on outputs 1/2.");
    if (!fx) {
        top_->insertWidget(2 > top_->count() ? top_->count() : 2, button);
    } else {
        int index = 2;
        for (int i = 0; i < top_->count(); ++i)
            if (top_->items[i].widget == fx) index = i;
        top_->removeWidget(fx);
        top_->insertWidget(index, button);
        fx->hide();
        fx->deleteLater();
    }
    fx = button;
}

project_io::MasterSettings MasterStrip::settings() const {
    project_io::MasterSettings s;
    s.volumeDb = volume->value();
    s.mute = muteButton->isChecked();
    return s;
}

void MasterStrip::apply(const project_io::MasterSettings& s) {
    volume->setValue((int)s.volumeDb);
    muteButton->setChecked(s.mute);
}

void MasterStrip::setScopeShown(bool on) {
    scope->settings.shown = on;
    scope->setVisible(on);
    frame->setFixedHeight(lanes::masterHeight());
}

void MasterStrip::showTruePeak(double dbtp) {
    truePeak->setText("TP\n" + loudness_display::fmtLufs(dbtp));
    truePeak->setToolTip("True peak in dBTP (ITU-R BS.1770-4: 4× oversampled, so peaks between the samples count),\n"
                         "the loudest since Play. Keep it under −1 dBTP for streaming and lossy files.");
}

void MasterStrip::setMute(bool on) { engine.masterMute = on; }

void MasterStrip::setVolume(int db) { engine.masterGain = dsp::dbToGain(db, config::MIN_DB); }
