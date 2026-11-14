// A built-in effect's window (effect_window.py).
#include "ui/effect_window.h"

#include <algorithm>
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
    if (!points.empty()) {
        std::vector<PointF> line;
        for (const auto& pt : points) line.push_back(toXY(pt.x, std::max(-1.0, std::min(1.0, pt.y))));
        p.drawPolyline(line, theme::c("accent"), 2, LineCap::Round, LineJoin::Round);
    }
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
    }
    if (!effect->meters().empty()) {
        meterView = add<MeterView>(effect);
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
