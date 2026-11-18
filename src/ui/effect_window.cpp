// A built-in effect's window (effect_window.py).
#include "ui/effect_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/automation.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
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
    setStyleType("CurveView");
    setMinimumHeight(theme::metric("curve-view-height", 130));
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
}

void CurveView::refresh() {
    points = effect ? effect->curve() : std::vector<effects::CurvePoint>();
    update();
}

const CurveView::Look& CurveView::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    l.self = &parts_.get("");
    const css::Style& grid = parts_.get("grid");
    l.grid = panel_style::fg(grid);
    l.gridW = panel_style::lengthF(grid, "stroke-width", 1);
    l.font = ui::style::font(grid, theme::uiFont());
    const css::Style& analyzer = parts_.get("analyzer");
    Color shade = panel_style::fg(analyzer);
    auto alpha = [&](const char* property, double def) {
        return (int)std::lround(std::clamp(analyzer.number(property, def), 0.0, 1.0) * shade.a);
    };
    l.analyzerFill = shade.withAlpha(alpha("fill-opacity", 60 / 255.0));
    l.analyzerLine = shade.withAlpha(alpha("stroke-opacity", 150 / 255.0));
    l.analyzerW = panel_style::lengthF(analyzer, "stroke-width", 1);
    const css::Style& curve = parts_.get("curve");
    l.curve = panel_style::fg(curve);
    l.curveW = panel_style::lengthF(curve, "stroke-width", 2);
    look_ = l;
    return look_;
}

void CurveView::paint(Painter& p) {
    const Look& look = this->look();
    int w = width(), h = height();
    ui::style::box(p, rect(), *look.self);
    auto toXY = [&](double x, double y) { return PointF((float)(x * (w - 1)), (float)((1 - (y + 1) / 2) * (h - 1))); };
    Color grid = look.grid;
    const Font& font = look.font;
    if (effect) {
        for (const auto& g : effect->curveYGrid) {
            PointF a = toXY(0, g.pos), b = toXY(1, g.pos);
            p.drawLine(PointF(a.x, a.y + 0.5f), PointF(b.x, b.y + 0.5f), grid, look.gridW);
            p.drawText(PointF(4, a.y - 2), g.label, font, grid);
        }
        for (const auto& g : effect->curveXGrid) {
            PointF a = toXY(g.pos, -1), b = toXY(g.pos, 1);
            p.drawLine(PointF(a.x + 0.5f, a.y), PointF(b.x + 0.5f, b.y), grid, look.gridW);
            p.drawText(PointF(toXY(g.pos, 0).x + 3, (float)h - 4), g.label, font, grid);
        }
    }
    if (spectrum.size() > 1) {  // (the analyzer: a soft filled shape behind the response)
        std::vector<PointF> outline, fill;
        for (const auto& pt : spectrum) outline.push_back(toXY(pt.x, std::max(-1.0, std::min(1.0, pt.y))));
        fill = outline;
        fill.push_back(PointF(outline.back().x, (float)h));
        fill.push_back(PointF(outline.front().x, (float)h));
        p.fillPolygon(fill, look.analyzerFill);
        p.drawPolyline(outline, look.analyzerLine, look.analyzerW, LineCap::Round, LineJoin::Round);
    }
    if (!points.empty()) {
        std::vector<PointF> line;
        for (const auto& pt : points) line.push_back(toXY(pt.x, std::max(-1.0, std::min(1.0, pt.y))));
        p.drawPolyline(line, look.curve, look.curveW, LineCap::Round, LineJoin::Round);
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
    setStyleType("MeterView");
    if (effect) values = effect->meters();
    setMinimumHeight(rowH() * std::max<int>(1, (int)values.size()));
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
}

void MeterView::refresh() {
    if (effect) values = effect->meters();
    update();
}

const MeterView::Look& MeterView::look() const {
    if (!parts_.refresh()) return look_;
    Look l;
    const css::Style& self = parts_.get("");
    l.text = ui::style::foreground(self);
    l.font = ui::style::font(self, theme::uiFont());
    l.rowH = std::max(1, panel_style::px(self, "line-height", 20));
    const css::Style& label = parts_.get("label");
    l.labelW = panel_style::px(label, "width", 110);
    l.labelPad = panel_style::px(label, "padding-right", 6);
    l.track = &parts_.get("track");
    l.trackH = panel_style::px(*l.track, "height", 12);
    l.barStyle = &parts_.get("bar");
    l.bar = panel_style::bg(*l.barStyle);
    const css::Style& value = parts_.get("value");
    l.valueW = panel_style::px(value, "width", 80);
    l.valuePad = panel_style::px(value, "padding-left", 6);
    look_ = l;
    return look_;
}

void MeterView::paint(Painter& p) {
    const Look& look = this->look();
    const int labelW = look.labelW, rowH = look.rowH;
    const Font& font = look.font;
    int textH = rowH - 4, trackY = (rowH - look.trackH) / 2;  // (16 and 4 px in a 20 px row, a 12 px bar)
    for (size_t i = 0; i < values.size(); i++) {
        const auto& m = values[i];
        int y = (int)i * rowH + 2;
        p.drawText(RectF(0, (float)y, (float)(labelW - look.labelPad), (float)textH), AlignVCenter | AlignRight, m.label,
                   font, look.text);
        Rect bar(labelW, (int)i * rowH + trackY, std::max(0, width() - labelW - look.valueW), look.trackH);
        ui::style::box(p, bar, *look.track);
        float fraction = (float)std::max(0.0, std::min(1.0, m.fraction));
        panel_style::fillPart(p, RectF((float)bar.x, (float)bar.y, bar.w * fraction, (float)bar.h), look.barStyle, look.bar);
        p.drawText(RectF((float)(bar.right() + look.valuePad), (float)y, (float)(look.valueW - look.valuePad), (float)textH),
                   AlignVCenter | AlignLeft, m.text, font, look.text);
    }
}

// --- the tuner ---

TunerView::TunerView(std::shared_ptr<effects::Effect> e) : MeterView(std::move(e)) {
    setStyleType("Tuner");
    setMinimumHeight(theme::metric("tuner-height", 120));
}

const TunerView::TunerLook& TunerView::tunerLook() {
    if (!tparts_.refresh()) return tunerLook_;
    TunerLook l;
    l.self = &tparts_.get("");
    const char* noteStates[] = {"", "in-tune", "silent"};
    Font big = theme::uiFont().withPixelSize(40).withBold(true), small = theme::uiFont();
    for (int i = 0; i < 3; ++i) {
        l.note[i] = &tparts_.get("note", noteStates[i]);
        l.noteFont[i] = ui::style::font(*l.note[i], big);
    }
    const css::Style& scale = tparts_.get("scale");
    l.scale = panel_style::bg(scale);
    l.scaleH = panel_style::px(scale, "height", 2);
    l.scaleLeft = panel_style::px(scale, "margin-left", 24);
    l.scaleRight = panel_style::px(scale, "margin-right", 24);
    l.scaleTop = panel_style::px(scale, "top", 66);
    const css::Style& tick = tparts_.get("tick");
    const css::Style& tickCentre = tparts_.get("tick", "centre");
    l.tick = panel_style::fg(tick);
    l.tickCentre = panel_style::fg(tickCentre);
    l.tickW = std::max(0.f, panel_style::lengthF(tick, "width", 1));
    l.tickH = std::max(0.f, panel_style::lengthF(tick, "height", 12));
    l.tickCentreH = std::max(0.f, panel_style::lengthF(tickCentre, "height", 24));
    const css::Style& needle = tparts_.get("needle");
    l.needle = panel_style::bg(needle);
    l.needleR = (float)std::max(0.0, needle.length("border-top-left-radius", 2));
    l.needleW = std::max(0.f, panel_style::lengthF(needle, "width", 4));
    l.needleH = std::max(0.f, panel_style::lengthF(needle, "height", 30));
    l.needleTop = panel_style::lengthF(needle, "top", -14);
    l.needleInTune = panel_style::bg(tparts_.get("needle", "in-tune"));
    const css::Style& cents = tparts_.get("cents");
    l.cents = panel_style::fg(cents);
    l.centsFont = ui::style::font(cents, small);
    const css::Style& hz = tparts_.get("hz");
    l.hz = panel_style::fg(hz);
    l.hzFont = ui::style::font(hz, small);
    const css::Style& ends = tparts_.get("end-label");
    l.endLabel = panel_style::fg(ends);
    l.endFont = ui::style::font(ends, small);
    tunerLook_ = l;
    return tunerLook_;
}

void TunerView::paint(Painter& p) {
    const TunerLook& look = tunerLook();
    int w = width(), h = height();
    ui::style::box(p, rect(), *look.self);
    std::string note = values.size() > 0 ? values[0].text : "—";
    double fraction = values.size() > 1 ? values[1].fraction : 0.5;
    std::string cents = values.size() > 1 ? values[1].text : "", hz = values.size() > 2 ? values[2].text : "";
    bool heard = values.size() > 0 && values[0].fraction > 0;
    bool inTune = heard && std::fabs(fraction - 0.5) * 100 <= 5;
    int state = heard ? (inTune ? 1 : 0) : 2;
    p.drawText(RectF(0, 4, (float)w, 48), AlignCenter, note, look.noteFont[state], ui::style::foreground(*look.note[state]));
    // the scale: −50…+50 cents, a tick every 10, the middle longer
    float left = (float)look.scaleLeft, right = (float)(w - look.scaleRight), y = (float)look.scaleTop;
    p.fillRect(RectF(left, y, right - left, (float)look.scaleH), look.scale);
    for (int c = -50; c <= 50; c += 10) {
        float x = left + (right - left) * (c + 50) / 100.0f;
        float tall = c == 0 ? look.tickCentreH : look.tickH;
        p.fillRect(RectF(x - look.tickW / 2, y - tall / 2 + 1, look.tickW, tall), c == 0 ? look.tickCentre : look.tick);
    }
    p.drawText(RectF(left - 20, y + 8, 40, 14), AlignCenter, "−50", look.endFont, look.endLabel);
    p.drawText(RectF(right - 20, y + 8, 40, 14), AlignCenter, "+50", look.endFont, look.endLabel);
    if (heard) {
        float x = left + (right - left) * (float)std::max(0.0, std::min(1.0, fraction));
        float r = theme::rounded ? look.needleR : 0.f;  // (square with Options → Rounded corners off)
        RectF needle(x - look.needleW / 2, y + look.needleTop, look.needleW, look.needleH);
        if (r > 0.01f) p.fillRoundedRect(needle, r, inTune ? look.needleInTune : look.needle);
        else p.fillRect(needle, inTune ? look.needleInTune : look.needle);
    }
    p.drawText(RectF(0, (float)h - 22, (float)w / 2 - 8, 18), AlignVCenter | AlignRight, cents, look.centsFont, look.cents);
    p.drawText(RectF((float)w / 2 + 8, (float)h - 22, (float)w / 2 - 8, 18), AlignVCenter | AlignLeft, hz, look.hzFont,
               look.hz);
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
    setStyleType("EffectWindow");
    const css::Style& st = computedStyle();
    ui::VBox* col = ui::vbox(this, ui::style::px(st, "padding-top", 9), ui::style::px(st, "row-gap", 6));
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
    grid->hSpacing = ui::style::px(st, "column-gap", 6);
    grid->vSpacing = ui::style::px(st, "row-gap", 6);
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
            slider->setMinimumWidth(ui::style::px(computedStyle("slider"), "min-width", 220));
            slider->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
            slider->valueChanged.connect([this, i](int v) { edit(i, (double)v / SLIDER_STEPS); });
            // --- writing automation: the slider held is one touch of its armed lane (automation_write.h) ---
            slider->sliderPressed.connect([this, i] { if (watcher) watcher->grab((uint32_t)i); });
            slider->sliderReleased.connect([this, i] { if (watcher) watcher->letGo((uint32_t)i); });
            auto* text = add<ui::Label>();
            text->setMinimumWidth(ui::style::px(computedStyle("value"), "min-width", 80));
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
    if (watcher) watcher->writeEdits({(uint32_t)index});  // (written at once, armed and playing: automation_write.h)
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
    Image img(std::max(1, width()), std::max(1, height()), ui::style::background(computedStyle()));
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
