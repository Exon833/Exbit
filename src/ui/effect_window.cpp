// A built-in effect's window (effect_window.py).
#include "ui/effect_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/automation.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"

using effect_window::REFRESH_MS;
using effect_window::SLIDER_STEPS;

namespace {

// Double-click: back to its default.
class DefaultSlider : public ui::Slider {
public:
    explicit DefaultSlider(double def) : ui::Slider(ui::Orientation::Horizontal), defaultNormal(def) {}
    bool mouseDoubleClick(ui::MouseEvent&) override {
        setValue((int)std::lround(defaultNormal * SLIDER_STEPS));
        return true;
    }
    double defaultNormal;
};

}  // namespace

// --- the curve ---

CurveView::CurveView(std::shared_ptr<effects::Effect> e) : effect(std::move(e)) {
    setMinimumHeight(130);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
}

void CurveView::refresh() {
    points = effect ? effect->curve() : std::vector<effects::CurvePoint>();
    update();
}

void CurveView::paint(Painter& p) {
    int w = width(), h = height();
    p.fillRect(rect(), theme::c("base"));
    auto toXY = [&](double x, double y) { return PointF((float)(x * (w - 1)), (float)((1 - (y + 1) / 2) * (h - 1))); };
    Color grid = theme::c("grid");
    Font font = theme::uiFont();
    if (effect) {
        for (const auto& g : effect->curveYGrid) {
            PointF a = toXY(0, g.pos), b = toXY(1, g.pos);
            p.drawLine(PointF(a.x, a.y + 0.5f), PointF(b.x, b.y + 0.5f), grid, 1);
            p.drawText(PointF(4, a.y - 2), g.label, font, grid);
        }
        for (const auto& g : effect->curveXGrid) {
            PointF a = toXY(g.pos, -1), b = toXY(g.pos, 1);
            p.drawLine(PointF(a.x + 0.5f, a.y), PointF(b.x + 0.5f, b.y), grid, 1);
            p.drawText(PointF(toXY(g.pos, 0).x + 3, (float)h - 4), g.label, font, grid);
        }
    }
    if (spectrum.size() > 1) {  // (the analyzer: a soft filled shape behind the response)
        Color shade = theme::c("dimmed_text");
        std::vector<PointF> outline, fill;
        for (const auto& pt : spectrum) outline.push_back(toXY(pt.x, std::max(-1.0, std::min(1.0, pt.y))));
        fill = outline;
        fill.push_back(PointF(outline.back().x, (float)h));
        fill.push_back(PointF(outline.front().x, (float)h));
        p.fillPolygon(fill, shade.withAlpha(60));
        p.drawPolyline(outline, shade.withAlpha(150), 1, LineCap::Round, LineJoin::Round);
    }
    if (!points.empty()) {
        std::vector<PointF> line;
        for (const auto& pt : points) line.push_back(toXY(pt.x, std::max(-1.0, std::min(1.0, pt.y))));
        p.drawPolyline(line, theme::c("accent"), 2, LineCap::Round, LineJoin::Round);
    }
}

void CurveView::refreshSpectrum(double dt) {
    if (!effect) return;
    // (a band every 3 pixels or so)
    int count = std::max(16, std::min(256, width() / 3));
    if (!effect->analyzer(count, dt, spectrum)) spectrum.clear();
    update();
}

// --- the meters ---

MeterView::MeterView(std::shared_ptr<effects::Effect> e) : effect(std::move(e)) {
    if (effect) values = effect->meters();
    setMinimumHeight(20 * std::max<int>(1, (int)values.size()));
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
}

void MeterView::refresh() {
    if (effect) values = effect->meters();
    update();
}

void MeterView::paint(Painter& p) {
    const int labelW = 110;
    Font font = theme::uiFont();
    for (size_t i = 0; i < values.size(); i++) {
        const auto& m = values[i];
        float y = (float)(i * 20 + 2);
        p.drawText(RectF(0, y, labelW - 6, 16), AlignVCenter | AlignRight, m.label, font, theme::c("window_text"));
        RectF bar((float)labelW, y + 2, (float)std::max(0, width() - labelW - 80), 12);
        p.fillRect(bar, theme::c("meter_bg"));
        float fraction = (float)std::max(0.0, std::min(1.0, m.fraction));
        p.fillRect(RectF(bar.x, bar.y, bar.w * fraction, bar.h), theme::c("accent"));
        p.drawText(RectF(bar.right() + 6, y, 74, 16), AlignVCenter | AlignLeft, m.text, font, theme::c("window_text"));
    }
}

// --- the tuner ---

TunerView::TunerView(std::shared_ptr<effects::Effect> e) : MeterView(std::move(e)) { setMinimumHeight(120); }

void TunerView::paint(Painter& p) {
    int w = width(), h = height();
    p.fillRect(rect(), theme::c("base"));
    Color text = theme::c("window_text"), dim = theme::c("dimmed_text"), accent = theme::c("accent");
    std::string note = values.size() > 0 ? values[0].text : "—";
    double fraction = values.size() > 1 ? values[1].fraction : 0.5;
    std::string cents = values.size() > 1 ? values[1].text : "", hz = values.size() > 2 ? values[2].text : "";
    bool heard = values.size() > 0 && values[0].fraction > 0;
    bool inTune = heard && std::fabs(fraction - 0.5) * 100 <= 5;
    Font big = theme::uiFont().withPixelSize(40).withBold(true), small = theme::uiFont();
    p.drawText(RectF(0, 4, (float)w, 48), AlignCenter, note, big, heard ? (inTune ? theme::c("meter_ok") : text) : dim);
    // the scale: −50…+50 cents, a tick every 10, the middle longer
    float left = 24, right = (float)w - 24, y = 66;
    p.fillRect(RectF(left, y, right - left, 2), theme::c("meter_bg"));
    for (int c = -50; c <= 50; c += 10) {
        float x = left + (right - left) * (c + 50) / 100.0f;
        float tall = c == 0 ? 12 : 6;
        p.fillRect(RectF(x - 0.5f, y - tall + 1, 1, tall * 2), c == 0 ? text : dim);
    }
    p.drawText(RectF(left - 20, y + 8, 40, 14), AlignCenter, "−50", small, dim);
    p.drawText(RectF(right - 20, y + 8, 40, 14), AlignCenter, "+50", small, dim);
    if (heard) {
        float x = left + (right - left) * (float)std::max(0.0, std::min(1.0, fraction));
        p.fillRoundedRect(RectF(x - 2, y - 14, 4, 30), 2, inTune ? theme::c("meter_ok") : accent);
    }
    p.drawText(RectF(0, (float)h - 22, (float)w / 2 - 8, 18), AlignVCenter | AlignRight, cents, small, text);
    p.drawText(RectF((float)w / 2 + 8, (float)h - 22, (float)w / 2 - 8, 18), AlignVCenter | AlignLeft, hz, small, dim);
}

// --- the window ---

plat::WindowOptions EffectWindow::options(ui::Window* parent, const std::string& title, bool invisible) {
    plat::WindowOptions o;
    o.title = title;
    o.w = 420;
    o.h = 300;
    o.dialog = true;  // (stays above the main window)
    o.transientFor = parent ? parent->native() : nullptr;
    if (invisible) {
        o.offscreen = true;  // mapped outside every screen: never seen, no taskbar entry
        o.takesFocus = false;
        o.transientFor = nullptr;
    }
    return o;
}

EffectWindow::EffectWindow(ui::Window* parent, SlotPtr slot_, const std::string& title, bool invisible_)
    : ui::Window(options(parent, title, invisible_)), slot(std::move(slot_)), invisible(invisible_) {
    deleteOnClose = true;
    setTitle(title);
    if (slot) effect = std::dynamic_pointer_cast<effects::Effect>(std::atomic_load(&slot->instance));
    ui::VBox* col = ui::vbox(this, 9, 6);
    if (!effect) return;
    if (effect->hasCurve()) {
        curveView = add<CurveView>(effect);
        col->addWidget(curveView);
        auto choices = effect->analyzerChoices();
        if (!choices.empty()) {  // (what the analyzer behind the curve listens to)
            auto rowOwner = std::make_unique<ui::HBox>();
            rowOwner->addWidget(add<ui::Label>("Analyzer"));
            analyzerBox = add<ui::ComboBox>();
            for (const auto& c : choices) analyzerBox->addItem(c);
            analyzerBox->setCurrentIndexSilently(effect->analyzerChoice());
            analyzerBox->setToolTip("The spectrum behind the curve: of the sound going in (Pre), coming out (Post), or none");
            analyzerBox->currentIndexChanged.connect([this](int k) {
                if (!effect || !curveView) return;
                effect->setAnalyzerChoice(k);
                curveView->refreshSpectrum(0);
            });
            rowOwner->addWidget(analyzerBox);
            rowOwner->addStretch(1);
            col->addLayout(std::move(rowOwner));
        }
    }
    if (!effect->meters().empty()) {
        meterView = effect->id() == "tuner" ? add<TunerView>(effect) : add<MeterView>(effect);
        col->addWidget(meterView);
    }
    auto gridOwner = std::make_unique<ui::GridLayout>();
    ui::GridLayout* grid = gridOwner.get();
    grid->setColumnStretch(1, 1);
    grid->hSpacing = 6;
    grid->vSpacing = 6;
    const auto& params = effect->params();
    for (int i = 0; i < (int)params.size(); i++) {
        const auto& param = params[i];
        auto* label = add<ui::Label>(param.name);
        grid->addWidget(label, i, 0);
        if (param.isToggle()) {
            auto* box = add<ui::CheckBox>();
            box->toggled.connect([this, i](bool on) { edit(i, on ? 1.0 : 0.0); });
            grid->addWidget(box, i, 1);
            controls.push_back({i, box, nullptr});
        } else if (!param.choices.empty()) {
            auto* box = add<ui::ComboBox>();
            for (const auto& c : param.choices) box->addItem(c);
            int n = (int)param.choices.size();
            box->currentIndexChanged.connect([this, i, n](int k) { edit(i, (double)k / std::max(1, n - 1)); });
            grid->addWidget(box, i, 1, 1, 2);
            controls.push_back({i, box, nullptr});
        } else {
            auto* slider = add<DefaultSlider>(param.toNormal(param.def));
            slider->setRange(0, SLIDER_STEPS);
            slider->setMinimumWidth(220);
            slider->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
            slider->valueChanged.connect([this, i](int v) { edit(i, (double)v / SLIDER_STEPS); });
            auto* text = add<ui::Label>();
            text->setMinimumWidth(80);
            grid->addWidget(slider, i, 1);
            grid->addWidget(text, i, 2);
            controls.push_back({i, slider, text});
        }
    }
    col->addLayout(std::move(gridOwner));
    auto rowOwner = std::make_unique<ui::HBox>();
    addPresetsButton(rowOwner.get());  // (presets and compare: effect_window_presets.cpp)
    rowOwner->addStretch(1);
    defaultsButton = add<ui::Button>("Defaults");
    defaultsButton->setToolTip("Every setting back to its default");
    defaultsButton->clicked.connect([this] { defaults(); });
    rowOwner->addWidget(defaultsButton);
    col->addLayout(std::move(rowOwner));
    follow();
    Point size = col->sizeHint();
    resizeWindow(std::max(size.x, 300), std::max(size.y, 100));
}

EffectWindow::~EffectWindow() {
    if (timer_ >= 0) ui::App::get().cancelTimer(timer_);
}

bool EffectWindow::attach() {
    if (!effect) return false;
    if (timer_ < 0) timer_ = ui::App::get().every(REFRESH_MS, [this] { follow(); });
    return true;
}

void EffectWindow::edit(int index, double normal) {
    if (updating_ || !effect) return;
    effect->windowEdit((uint32_t)index, normal);
    follow();
}

void EffectWindow::defaults() {
    if (!effect) return;
    const auto& params = effect->params();
    for (size_t i = 0; i < params.size(); i++) effect->windowEdit((uint32_t)i, params[i].toNormal(params[i].def));
    follow();
}

void EffectWindow::follow() {
    if (!effect) return;
    std::vector<double> normals;
    for (size_t i = 0; i < effect->params().size(); i++) normals.push_back(effect->getParam((uint32_t)i));
    if (normals != shown_) {
        shown_ = normals;
        updating_ = true;
        for (const auto& c : controls) {
            double value = normals[c.index];
            if (auto* box = dynamic_cast<ui::CheckBox*>(c.widget)) {
                box->setChecked(value >= 0.5);
            } else if (auto* combo = dynamic_cast<ui::ComboBox*>(c.widget)) {
                combo->setCurrentIndex((int)std::lround(value * std::max(1, combo->count() - 1)));
            } else if (auto* slider = dynamic_cast<ui::Slider*>(c.widget)) {
                if (!slider->isSliderDown()) slider->setValue((int)std::lround(value * SLIDER_STEPS));
                c.text->setText(effect->paramText((uint32_t)c.index, value));
            }
        }
        updating_ = false;
        if (curveView) curveView->refresh();
    }
    if (meterView) meterView->refresh();
    if (curveView && analyzerBox) {  // (the analyzer: its spectrum again, as it falls back over time)
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        curveView->refreshSpectrum(lastSpectrum_ < 0 ? 0 : std::min(1.0, now - lastSpectrum_));
        lastSpectrum_ = now;
    }
    showDifferences();  // (compare: the knobs that differ from the kept settings; effect_window_presets.cpp)
}

Image EffectWindow::snapshot() {
    ui::App::get().doLayouts();
    Image img(std::max(1, width()), std::max(1, height()), theme::c("window"));
    Painter p(img);
    paintTree(p, rect());
    return img;
}

Point EffectWindow::position() const {
    Rect g = screenGeometry();
    return Point(g.x, g.y);
}

void EffectWindow::place(ui::Window* main, std::optional<Point> last) {
    Rect frame = screenGeometry();
    int fw = std::max(frame.w, width()), fh = std::max(frame.h, height());
    if (!last) {
        Rect centre = main ? main->screenGeometry() : ui::App::get().screenAt(ui::App::get().cursorPos());
        last = Point(centre.x + (centre.w - fw) / 2, centre.y + (centre.h - fh) / 3);
    }
    Rect area = ui::App::get().screenAt(Point(last->x + fw / 2, last->y + 16));
    int x = std::max(area.x, std::min(last->x, area.right() - fw));
    int y = std::max(area.y, std::min(last->y, area.bottom() - fh));
    moveWindow(x, y);
}

void EffectWindow::close() {
    if (closing_) return;
    closing_ = true;
    if (timer_ >= 0) {
        ui::App::get().cancelTimer(timer_);
        timer_ = -1;
    }
    SlotPtr s = slot;
    editorClosed.emit(s);
    ui::Window::close();
}
