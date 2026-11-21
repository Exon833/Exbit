// A track's fader drawn on its level meter (see meter_fader.h).
#include "ui/meter_fader.h"

#include <algorithm>
#include <cmath>

#include "config.h"
#include "ui/panel_style.h"
#include "ui/theme.h"

namespace {
// the row's meter: its clicks go on to the fader around it (they move the volume; Play resets the peaks)
class FaderMeter : public LevelMeter {
public:
    using LevelMeter::LevelMeter;
    bool mousePress(ui::MouseEvent&) override { return false; }
    bool mouseDoubleClick(ui::MouseEvent&) override { return false; }
};
constexpr double RANGE = -(double)config::MIN_DB;  // dB from MIN_DB to 0 dB: the bars' width
}  // namespace

MeterFader::MeterFader(double db, std::function<void(double)> slot, int channels) : VolumeSlider(db, std::move(slot)) {
    setStyleType("MeterFader");
    setFixedHeight(theme::metric("meter-fader-height", 30));
    meter_ = add<FaderMeter>(channels);
    meter_->setNumberShown(false);
}

int MeterFader::handleH() const {
    return std::max(3, ui::style::px(computedStyle("marker"), "height", 8));
}

void MeterFader::resized() {
    if (!meter_) return;  // (the constructor sizing it before the meter is made)
    // the bars from the handle's half width on the left to where 0 dB leaves room for MAX_DB on the right
    int half = std::max(2, ui::style::px(computedStyle("marker"), "width", 12) / 2);
    int top = handleH() + 1;
    meter_->setGeometry(0, top, width(), std::max(0, height() - top));
    Rect bars = meter_->barsArea();
    int extra = width() - bars.w;  // (the meter's own border and padding, and its track's)
    double usable = std::max(1, width() - 2 * half);
    int want = (int)std::floor(usable * RANGE / (RANGE + config::MAX_DB));
    meter_->setGeometry(half - bars.x, top, want + extra, std::max(0, height() - top));
    bars = meter_->barsArea();
    x0_ = meter_->geometry().x + bars.x;
    span_ = std::max(1, bars.w);
}

int MeterFader::handleX(double db) const {
    db = std::clamp(db, (double)config::MIN_DB, (double)config::MAX_DB);
    return x0_ + (int)std::lround(span_ * (db - config::MIN_DB) / RANGE);
}

double MeterFader::dbAt(int x) const {
    return std::clamp(config::MIN_DB + (x - x0_) * RANGE / span_, (double)config::MIN_DB, (double)config::MAX_DB);
}

void MeterFader::paint(Painter& p) {
    // (no slider groove: the meter is the track; the ▼ above it)
    const css::Style& handle = computedStyle("marker");
    int w = std::max(4, ui::style::px(handle, "width", 12)), h = handleH();
    float cx = (float)handleX(value());
    // (no colour of its own: the row's text colour, so it shows on any row, a coloured one too)
    Color c = handle.has("background-color") ? panel_style::bg(handle) : ui::style::foreground(computedStyle());
    p.fillPolygon({PointF(cx - w / 2.0f, 0), PointF(cx + w / 2.0f, 0), PointF(cx, (float)h)}, c);
}

void MeterFader::styleStates(std::vector<std::string>& out) const {
    ui::Widget::styleStates(out);  // (:hover…; not the slider's own)
    if (dragging_) out.push_back("pressed");
}

bool MeterFader::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    dragging_ = true;
    int half = std::max(2, ui::style::px(computedStyle("marker"), "width", 12) / 2);
    // on the handle: it is held where it is; elsewhere it jumps there first
    if (std::abs(e.pos.x - handleX(value())) > half) setValue((int)std::lround(dbAt(e.pos.x)));
    grabX_ = e.pos.x;
    grabDb_ = value();
    sliderPressed.emit();
    statesChanged();
    update();
    return true;
}

bool MeterFader::mouseMove(ui::MouseEvent& e) {
    if (!dragging_) return false;
    double perPx = RANGE / span_ * (e.shift() ? 0.1 : 1.0);  // (Shift: finer)
    double db = std::clamp(grabDb_ + (e.pos.x - grabX_) * perPx, (double)config::MIN_DB, (double)config::MAX_DB);
    setValue((int)std::lround(db));
    return true;
}

bool MeterFader::mouseRelease(ui::MouseEvent& e) {
    if (!dragging_) return false;
    dragging_ = false;
    sliderReleased.emit();
    statesChanged();
    update();
    return true;
}

bool MeterFader::mouseDoubleClick(ui::MouseEvent& e) {
    doubleClicked.emit();  // (VolumeSlider: 0 dB)
    return true;
}

Point MeterFader::sizeHint() const { return Point(120, height()); }
Point MeterFader::minimumSizeHint() const { return Point(40, height()); }
