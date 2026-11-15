// The master lane's controls (master_strip.py).
#include "ui/master_strip.h"

#include "audio/dsp.h"
#include "audio/engine.h"
#include "ui/lanes.h"
#include "ui/scope_view.h"
#include "ui/theme.h"

MasterStrip::MasterStrip(audio::Engine& engine_, std::function<void()> openPlugins_)
    : engine(engine_), openPlugins(std::move(openPlugins_)) {
    owned_ = std::make_unique<LaneFrame>(theme::c("master_lane"), std::nullopt, lanes::masterHeight());
    frame = owned_.get();
    ui::VBox* col = ui::vbox(frame, 0, 2);
    col->setContentsMargins(8, 3, 8, 3);

    auto top = std::make_unique<ui::HBox>();
    top_ = top.get();
    top_->setSpacing(2);
    col->addLayout(std::move(top));
    auto* title = frame->add<ui::Label>("Master");
    title->setFont(theme::uiFont().withBold(true));
    top_->addWidget(title, 1);
    loudness = frame->add<LoudnessDisplay>();
    top_->addWidget(loudness);
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
    col->addSpacing(2);
    col->addWidget(notice);
    setVolume(0);
}

MasterStrip::~MasterStrip() = default;

std::unique_ptr<ui::Widget> MasterStrip::releaseFrame() { return std::unique_ptr<ui::Widget>(owned_.release()); }

void MasterStrip::showUpdate(const update_check::Result& result) {
    updateResult = result;
    notice->setNotice(update_notice::noticeText(&result));
    notice->setToolTip(update_notice::noticeTip(&result));
}

void MasterStrip::restyle() {
    frame->setColors(theme::c("master_lane"));
    notice->setNotice(update_notice::noticeText(updateResult ? &*updateResult : nullptr));  // (its link's colour)
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

void MasterStrip::setMute(bool on) { engine.masterMute = on; }

void MasterStrip::setVolume(int db) { engine.masterGain = dsp::dbToGain(db, config::MIN_DB); }
