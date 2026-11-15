// Small building blocks shared by the panel widgets (widgets.py).
#include "ui/widgets.h"

#include <cmath>
#include <functional>

#include "ui/formatting.h"
#include "ui/theme.h"

// ============================================================================================
// LaneFrame (lane_frame / lane_style)
// ============================================================================================

LaneFrame::LaneFrame(Color color, std::optional<Color> accent, int height, std::optional<Color> text)
    : color_(color), accent_(accent), text_(text) {
    objectName = "lane";
    setFixedHeight(height);
    hPolicy = ui::SizePolicy::Expanding;
}

void LaneFrame::setColors(Color color, std::optional<Color> accent, std::optional<Color> text) {
    color_ = color;
    accent_ = accent;
    text_ = text;
    applyTextColor();
    if (layout()) layout()->invalidate();
    update();
}

void LaneFrame::setCurrent(bool on) {
    if (current_ == on) return;
    current_ = on;
    if (layout()) layout()->invalidate();  // (the edges take a pixel of the inside, as in Qt)
    update();
}

void LaneFrame::setTopLine(std::optional<Color> c) {
    topLine_ = c;
    if (layout()) layout()->invalidate();
    update();
}

void LaneFrame::applyTextColor() {
    // "#lane QLabel { color: … }": every label inside the row
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& child : w->children()) {
            if (auto* label = dynamic_cast<ui::Label*>(child.get())) {
                if (text_) label->setColor(*text_);
                else label->clearColor();
            }
            walk(child.get());
        }
    };
    walk(this);
}

Rect LaneFrame::contentsRect() const {
    int left = accent_ ? 3 : 1;
    int top = (current_ || topLine_) ? 1 : 0;
    int side = current_ ? 1 : 0;
    return rect().adjusted(left, top, -side, -side);
}

void LaneFrame::paint(Painter& p) {
    p.fillRect(rect(), color_);
}

void LaneFrame::paintOver(Painter& p) {
    // (the edges are borders: drawn over nothing of the inside, but after it is simpler)
    int w = width(), h = height();
    if (topLine_) p.fillRect(Rect(0, 0, w, 1), *topLine_);
    if (current_) {
        Color edge = theme::c("accent");
        p.fillRect(Rect(0, 0, w, 1), edge);
        p.fillRect(Rect(w - 1, 0, 1, h), edge);
        p.fillRect(Rect(0, h - 1, w, 1), edge);
    }
    if (accent_) p.fillRect(Rect(0, 0, 3, h), *accent_);
    else p.fillRect(Rect(0, 0, 1, h), theme::c("frame_edge"));
}

bool LaneFrame::mousePress(ui::MouseEvent& e) {
    if (onPress) {
        onPress();
        return true;
    }
    return false;
}

bool LaneFrame::contextMenu(ui::MouseEvent& e) {
    if (!fillMenu) return false;
    ui::Menu menu;
    fillMenu(menu);
    menu.exec(e.globalPos, this);
    return true;
}

// ============================================================================================
// PinToBottom
// ============================================================================================

PinToBottom::PinToBottom(ui::Widget* widget, ui::Widget* viewport, int height, int margin)
    : widget_(widget), viewport_(viewport) {
    if (widget->parent() != viewport) {
        std::unique_ptr<ui::Widget> owned = widget->parent()->release(widget);
        viewport->adopt(std::move(owned));
    }
    init(height, margin);
}

PinToBottom::PinToBottom(std::unique_ptr<ui::Widget> widget, ui::Widget* viewport, int height, int margin)
    : widget_(widget.get()), viewport_(viewport) {
    viewport->adopt(std::move(widget));
    init(height, margin);
}

void PinToBottom::setHeight(int height) {
    height_ = height;
    widget_->setFixedHeight(height);
    place();
}

void PinToBottom::init(int height, int margin) {
    height_ = height;
    margin_ = margin;
    auto* cover = viewport_->add<ui::Canvas>();
    cover->onPaint = [cover](Painter& p) { p.fillRect(cover->rect(), theme::c("window")); };
    cover_ = cover;
    cover_->setVisible(margin > 0);
    previousFilter_ = viewport_->onEventFilter;
    std::weak_ptr<bool> alive = alive_;
    viewport_->onEventFilter = [this, alive, prev = previousFilter_](ui::Widget* w, const char* what) {
        bool swallowed = prev && prev(w, what);
        if (!alive.expired() && std::string(what) == "resize") place();
        return swallowed;
    };
    // (Qt's ChildAdded: contents added to the viewport later must not cover it)
    frame_ = ui::App::get().onFrame([this] {
        const auto& kids = viewport_->children();
        size_t n = kids.size();
        bool onTop = n >= 2 && kids[n - 1].get() == cover_ && kids[n - 2].get() == widget_;
        Rect want(0, viewport_->height() - margin_ - height_, viewport_->width(), height_);
        if (!onTop || widget_->geometry() != want) place();
    });
    widget_->show();
    place();
}

PinToBottom::~PinToBottom() {
    *alive_ = false;
    if (frame_ >= 0) ui::App::get().removeFrame(frame_);
    // (the viewport's filter stays chained: it checks that this is alive; others may have chained after it)
}

void PinToBottom::place() {
    ui::Widget* vp = viewport_;
    if (!ui::Widget::alive(vp) || !ui::Widget::alive(widget_)) return;
    widget_->setGeometry(0, vp->height() - margin_ - height_, vp->width(), height_);
    cover_->setGeometry(0, vp->height() - margin_, vp->width(), margin_);
    const auto& kids = vp->children();
    size_t n = kids.size();
    if (!(n >= 2 && kids[n - 1].get() == cover_ && kids[n - 2].get() == widget_)) {
        widget_->raise();
        cover_->raise();
    }
}

// ============================================================================================
// labels, slider
// ============================================================================================

NameLabel::NameLabel(const std::string& text, bool bold) : ui::Label(text) {
    if (bold) setFont(theme::uiFont().withBold(true));
    setMinimumWidth(10);
    hPolicy = ui::SizePolicy::Expanding;
    setElide(true);
}

VolumeSlider::VolumeSlider(double db, std::function<void(double)> slot) : ui::Slider(ui::Orientation::Horizontal) {
    setRange(config::MIN_DB, config::MAX_DB);
    setFocusPolicy(ui::FocusPolicy::None);
    setFixedHeight(14);
    setWheelEnabled(false);
    setValueSilently((int)std::nearbyint(db));  // (round half to even, as Python's round)
    valueChanged.connect([this, slot](int v) {
        showTip();
        if (slot) slot((double)v);
    });
    doubleClicked.connect([this] { setValue(0); });
    showTip();
}

void VolumeSlider::showTip() {
    setToolTip("Volume " + fmt::db(value(), config::MIN_DB) + " (double-click: 0 dB)");
}

DbReading::DbReading(ui::Slider* slider_) : slider(slider_) {
    setFixedWidth(44);
    setAlignment(AlignRight | AlignVCenter);
    setToolTip("Volume (double-click: 0 dB)");
    slider->valueChanged.connect([this](int v) { showValue(v); });
    doubleClicked.connect([this] { slider->setValue(0); });
    showValue(slider->value());
}

void DbReading::showValue(int db) { setText(fmt::db(db, config::MIN_DB)); }

// ============================================================================================
// PanKnob
// ============================================================================================

PanKnob::PanKnob(double pan_, std::function<void(double)> slot) : pan(pan_), slot_(std::move(slot)) {
    setFixedSize(22, 22);
    setCursor(ui::Cursor::SizeVer);
    showTip();
}

void PanKnob::showTip() { setToolTip("Pan " + fmt::pan(pan) + " (drag up/down; double-click: centre)"); }

void PanKnob::showValue(double value) {
    if (std::fabs(value - pan) > 1e-4) {
        pan = value;
        showTip();
        update();
    }
}

void PanKnob::setPan(double value) {
    value = std::nearbyint(std::max(-1.0, std::min(1.0, value)) * 100) / 100;  // (Python's round(…, 2))
    if (value != pan) {
        pan = value;
        showTip();
        update();
        if (slot_) slot_(value);
    }
}

bool PanKnob::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton) {
        dragging_ = true;
        start_ = e.pos;
        startPan_ = pan;
    }
    return true;
}

bool PanKnob::mouseMove(ui::MouseEvent& e) {
    if (dragging_) {
        double moved = (e.pos.x - start_.x) - (e.pos.y - start_.y);
        setPan(startPan_ + moved / PIXELS / ((e.mods & plat::ModShift) ? 5 : 1));
    }
    return true;
}

bool PanKnob::mouseRelease(ui::MouseEvent& e) {
    dragging_ = false;
    return true;
}

bool PanKnob::mouseDoubleClick(ui::MouseEvent& e) {
    setPan(0.0);
    return true;
}

void PanKnob::paint(Painter& p) {
    RectF r(3, 3, (float)width() - 6, (float)height() - 6);
    p.fillEllipse(r, theme::c("button"));
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2, ry = r.h / 2;
    if (pan != 0) {  // the arc from the top (centre) to where it is turned
        double a0 = M_PI / 2, a1 = M_PI / 2 - pan * 135 * M_PI / 180;
        Path arc;
        int steps = std::max(2, (int)std::ceil(std::fabs(pan) * 24));
        for (int i = 0; i <= steps; ++i) {
            double a = a0 + (a1 - a0) * i / steps;
            float x = cx + (float)std::cos(a) * rx, y = cy - (float)std::sin(a) * ry;
            if (i == 0) arc.moveTo(x, y);
            else arc.lineTo(x, y);
        }
        p.strokePath(arc, theme::c("accent"), 2);
    }
    double angle = (90 - pan * 135) * M_PI / 180;
    p.drawLine(PointF(cx, cy), PointF(cx + (float)std::cos(angle) * rx, cy - (float)std::sin(angle) * ry),
               theme::c("button_text"), 1.5f);
}

// ============================================================================================
// SendsButton
// ============================================================================================

SendsPanel::SendsPanel(TrackPtr track, std::vector<TrackPtr> returns,
                       std::function<void(const TrackPtr&, double)> setLevel) {
    auto grid = std::make_unique<ui::GridLayout>();
    ui::GridLayout* g = grid.get();
    g->hSpacing = 6;
    g->vSpacing = 4;
    auto* title = add<ui::Label>(track->name + " sends to");
    title->setFont(theme::uiFont().withBold(true));  // (<b>name</b>: the whole line bold is near enough)
    g->addWidget(title, 0, 0, 1, 3);
    int row = 1;
    for (auto& target : returns) {
        double db = config::MIN_DB;
        for (auto& s : track->sends)
            if (s.track == target) db = s.db;
        TrackPtr t = target;
        auto* slider = add<VolumeSlider>(db, [setLevel, t](double v) { if (setLevel) setLevel(t, v); });
        slider->setMinimumWidth(150);
        slider->hPolicy = ui::SizePolicy::Expanding;
        g->addWidget(add<ui::Label>(target->name), row, 0);
        g->addWidget(slider, row, 1);
        g->addWidget(add<DbReading>(slider), row, 2);
        sliders.push_back(slider);
        ++row;
    }
    g->setColumnStretch(1, 1);
    grid->setContentsMargins(9, 9, 9, 9);
    setLayout(std::move(grid));
}

void SendsPanel::paint(Painter& p) {
    p.fillRect(rect(), theme::c("window"));
    p.drawRect(rect(), theme::shades().line);  // (QFrame.StyledPanel)
}

SendsButton::SendsButton(TrackPtr track_, std::vector<TrackPtr> returns_, std::function<void()> changed_)
    : ui::Button("→"), track(std::move(track_)), returns(std::move(returns_)), changed(std::move(changed_)) {
    setStyle(ui::Button::Style::Tool);
    setFixedSize(20, 16);
    setFocusPolicy(ui::FocusPolicy::None);
    clicked.connect([this] { showSends(); });
    showState();
}

SendsButton::~SendsButton() { closePopup(); }

double SendsButton::level(const TrackPtr& target) const {
    for (auto& s : track->sends)
        if (s.track == target) return s.db;
    return config::MIN_DB;
}

void SendsButton::showState() {
    std::string up;
    for (auto& s : track->sends)
        if (s.db > config::MIN_DB) up += (up.empty() ? "" : ", ") + s.track->name + " " + fmt::db(std::nearbyint(s.db), config::MIN_DB);
    setProperty("lit", up.empty() ? "" : "true");  // the skin's accent colour
    setToolTip("Sends to the return tracks: " + (up.empty() ? std::string("none yet") : up) + " (click to set them)");
    update();
}

void SendsButton::setLevel(const TrackPtr& target, double db) {
    std::vector<Send> kept;
    for (auto& s : track->sends)
        if (s.track != target) kept.push_back(s);
    if (db > config::MIN_DB) kept.push_back(Send{target, db});
    track->sends = kept;  // (the mixer reads it every buffer)
    showState();
    if (changed) changed();
}

void SendsButton::closePopup() {
    if (!popup_) return;
    ui::App::get().closePopup(popup_.get());
    popup_.reset();
}

void SendsButton::showSends() {
    closePopup();
    plat::WindowOptions o;
    o.popup = true;
    o.resizable = false;
    o.title = "Sends";
    auto panel = std::make_unique<SendsPanel>(track, returns, [this](const TrackPtr& t, double db) { setLevel(t, db); });
    Point size = panel->sizeHint();
    o.w = std::max(220, size.x);
    o.h = std::max(40, size.y);
    o.transientFor = window() ? window()->native() : nullptr;
    popup_ = std::make_unique<ui::Window>(o);
    SendsPanel* raw = panel.get();
    popup_->adopt(std::move(panel));
    ui::vbox(popup_.get(), 0, 0)->addWidget(raw);
    Point at = mapToGlobal(Point(0, height()));
    popup_->moveWindow(at.x, at.y);
    ui::Window* win = popup_.get();
    std::weak_ptr<bool> alive = alive_;
    ui::App::get().openPopup(win, this, [this, alive] {
        if (!alive.expired()) ui::App::get().singleShot(0, [this, alive] { if (!alive.expired()) closePopup(); });
    });
}

// ============================================================================================
// buttons
// ============================================================================================

namespace widgets {

namespace {
ui::Button* toolButton(ui::Widget* parent, const std::string& text, int w, int h, const std::string& tooltip) {
    auto* b = parent->add<ui::Button>(text);
    b->setStyle(ui::Button::Style::Tool);
    b->setToolTip(tooltip);
    b->setFixedSize(w, h);
    b->setFocusPolicy(ui::FocusPolicy::None);
    return b;
}
}  // namespace

ui::Button* toggleButton(ui::Widget* parent, const std::string& text, const std::string& role, bool checked,
                         std::function<void(bool)> slot, const std::string& tooltip) {
    ui::Button* b = toolButton(parent, text, 26, 22, tooltip);
    b->setCheckable(true);
    b->setCheckedSilently(checked);
    b->setProperty("toggle", role);  // coloured by the skin (theme toggleColor)
    if (slot) b->toggled.connect(slot);
    return b;
}

std::vector<std::pair<int, std::string>> outputPairs(int channels) {
    std::vector<std::pair<int, std::string>> out;
    for (int i = 0; i < channels; i += 2)
        out.push_back({i, i + 1 < channels ? std::to_string(i + 1) + "/" + std::to_string(i + 2) : std::to_string(i + 1)});
    return out;
}

ui::ComboBox* outputSelector(ui::Widget* parent, int channels, int current, std::function<void(int)> slot) {
    auto* combo = parent->add<ui::ComboBox>();
    combo->setWheelEnabled(false);  // scrolling the panel never changes a value by accident
    combo->setFocusPolicy(ui::FocusPolicy::None);
    combo->setToolTip("Device outputs this track plays on");
    combo->setFixedWidth(58);
    for (auto& [first, label] : outputPairs(channels)) combo->addItem(label, first);
    combo->setCurrentIndexSilently(std::max(0, combo->findData(current)));
    combo->currentIndexChanged.connect([combo, slot](int i) {
        if (slot && i >= 0) slot(std::atoi(combo->itemData(i).c_str()));
    });
    return combo;
}

std::string effectsSummary(const std::vector<SlotPtr>& effects) {
    if (effects.empty()) return "No effects yet: click to add some";
    std::string s = "Effects: ";
    for (size_t i = 0; i < effects.size(); ++i)
        s += (i ? ", " : "") + effects[i]->name + (effects[i]->bypass ? " (bypassed)" : "");
    return s;
}

ui::Button* fxButton(ui::Widget* parent, const std::vector<SlotPtr>& effects, std::function<void()> slot) {
    ui::Button* b = toolButton(parent, "FX", 28, 22, effectsSummary(effects));
    if (!effects.empty()) b->setProperty("lit", "true");  // the skin's accent colour
    if (slot) b->clicked.connect(slot);
    return b;
}

ui::Menu* ownedMenu(ui::Widget* owner) {
    auto menu = std::make_shared<ui::Menu>();
    owner->userData = menu;
    return menu.get();
}

ui::Button* instrumentButton(ui::Widget* parent, const SlotPtr& instrument, std::function<void()> edit,
                             std::function<void(ui::Menu&)> fillMenu, int rack, SamplerPtr sampler) {
    ui::Button* b = toolButton(parent, "🎹", 40, 22, "");
    ui::Menu* menu = ownedMenu(b);
    menu->aboutToShow = [fillMenu](ui::Menu* m) {
        m->clear();
        if (fillMenu) fillMenu(*m);
    };
    b->setMenu(menu, true);  // MenuButtonPopup: the ▾ part shows it
    if (rack >= 0) {
        b->setToolTip("Instrument patch panel: " + std::to_string(rack) + " instrument" + (rack == 1 ? "" : "s") +
                      "\nClick to open it; ▾ to choose another instrument");
        if (edit) b->clicked.connect(edit);
    } else if (instrument) {
        b->setToolTip("Instrument: " + instrument->name +
                      "\nClick to open its window and configure it; ▾ to choose another instrument");
        if (edit) b->clicked.connect(edit);
    } else if (sampler) {
        b->setToolTip("Instrument: built-in sampler, playing " + (sampler->sample ? sampler->sample->name : std::string()) +
                      "\nClick to open its window; ▾ to choose another instrument");
        if (edit) b->clicked.connect(edit);
    } else {
        b->setToolTip("Instrument: Built-in synth (no settings)\nClick to choose another instrument");
        b->clicked.connect([b] { b->showMenu(); });
    }
    return b;
}

ui::Button* iconButton(ui::Widget* parent, const std::string& text, std::function<void()> slot, const std::string& tooltip) {
    ui::Button* b = toolButton(parent, text, 22, 22, tooltip);
    if (slot) b->clicked.connect(slot);
    return b;
}

}  // namespace widgets
