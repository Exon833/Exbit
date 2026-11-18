// Small building blocks shared by the panel widgets (widgets.py).
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/cpu_monitor.h"
#include "ui/formatting.h"
#include "ui/theme.h"

// ============================================================================================
// LaneFrame (lane_frame / lane_style)
// ============================================================================================

LaneFrame::LaneFrame(Color color, std::optional<Color> accent, int height, std::optional<Color> text)
    : color_(color), accent_(accent), text_(text) {
    objectName = "lane";
    setStyleType("LaneFrame");
    setFixedHeight(height);
    hPolicy = ui::SizePolicy::Expanding;
}

void LaneFrame::setColors(Color color, std::optional<Color> accent, std::optional<Color> text) {
    if (!(color == color_)) boxStale_ = true;
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

void LaneFrame::setRowStyle(const css::Style& style) {
    rowStyle_ = style;
    boxStale_ = true;
    if (layout()) layout()->invalidate();  // (its border: the inside may be smaller)
    update();
}

const css::Style& LaneFrame::box() const {
    // (a panel row's style is kept as given: only a skin change or a new colour makes it again; the master's own
    // style follows its states too)
    int gen = theme::styles().generation();
    bool stale = boxStale_ || gen != boxGeneration_;
    if (!rowStyle_) stale = boxStamp_.stale(this) || stale;
    if (stale) {
        box_ = paint::withBackground(rowStyle_ ? *rowStyle_ : computedStyle(), color_);
        boxStale_ = false;
        boxGeneration_ = gen;
    }
    return box_;
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

const LaneFrame::Look& LaneFrame::look() const {
    if (!parts_.refresh()) return look_;
    Look l;
    l.stripeW = std::max(0, panel_style::px(parts_.get("stripe"), "width", 3));
    const css::Style& edge = parts_.get("edge");
    l.edge = panel_style::bg(edge);
    l.edgeW = std::max(0, panel_style::px(edge, "width", 1));
    const css::Style& current = parts_.get("current");
    l.current = ui::style::insets(current, "border-width");
    l.currentEdge = panel_style::color(current, "border-top-color");
    const css::Style& load = parts_.get("load");
    l.loadTrack = &load;
    l.loadInset = ui::style::contentInsets(load);
    l.loadH = std::max(0, panel_style::px(load, "height", 2));
    l.loadBelow = panel_style::px(load, "margin-bottom", 1);
    const char* states[] = {"ok", "warn", "hot"};
    for (int i = 0; i < 3; ++i) {
        l.loadBar[i] = &parts_.get("load-bar", states[i]);
        l.load[i] = panel_style::bg(*l.loadBar[i]);
    }
    look_ = l;
    return look_;
}

Rect LaneFrame::contentsRect() const {
    const Look& look = this->look();
    const ui::style::Insets& border = ui::style::boxLook(box()).border;  // (none in the default look)
    int left = std::max(accent_ ? look.stripeW : look.edgeW, border.left);
    int top = std::max(current_ ? look.current.top : topLine_ ? 1 : 0, border.top);
    int right = std::max(current_ ? look.current.right : 0, border.right);
    int bottom = std::max(current_ ? look.current.bottom : 0, border.bottom);
    return rect().adjusted(left, top, -right, -bottom);
}

Rect LaneFrame::paintRect() const {
    Rect r = ui::Widget::paintRect();
    if (!rowStyle_) return r;  // (the master's row: its own style's, as any widget's)
    const ui::style::Insets& o = ui::style::boxLook(box()).overflow;
    if (o.left || o.top || o.right || o.bottom) r = r.united(rect().adjusted(-o.left, -o.top, o.right, o.bottom));
    return r;
}

void LaneFrame::paint(Painter& p) {
    // its own colour (data) as the box's background; the default look: a plain fill
    ui::style::box(p, rect(), box());
}

void LaneFrame::paintOver(Painter& p) {
    // (the edges are borders: drawn over nothing of the inside, but after it is simpler)
    const Look& look = this->look();
    int w = width(), h = height();
    if (topLine_) p.fillRect(Rect(0, 0, w, 1), *topLine_);
    if (current_) {
        const ui::style::Insets& b = look.current;
        p.fillRect(Rect(0, 0, w, b.top), look.currentEdge);
        p.fillRect(Rect(w - b.right, 0, b.right, h), look.currentEdge);
        p.fillRect(Rect(0, h - b.bottom, w, b.bottom), look.currentEdge);
        p.fillRect(Rect(0, 0, b.left, h), look.currentEdge);
    }
    if (accent_) p.fillRect(Rect(0, 0, look.stripeW, h), *accent_);
    else p.fillRect(Rect(0, 0, look.edgeW, h), look.edge);
    if (load_ >= 0) {  // (Options → CPU per track)
        // the full width is half a core (a track taking more is a lot); coloured as the CPU monitor at
        // twice its share (a quarter of a core: warning colour, 40 %: hot); anything measurable shows
        int x0 = accent_ ? look.stripeW : look.edgeW, full = std::max(0, w - x0 - 1);
        double scaled = std::min(1.0, load_ * 2);
        int y = h - look.loadBelow - look.loadH;
        Rect track(x0, y, full, look.loadH);
        ui::style::box(p, track, *look.loadTrack);
        Rect in = ui::style::inside(track, look.loadInset);  // (the bar inside its border and padding)
        int bar = (int)std::lround(std::max(0, in.w) * scaled);
        if (load_ >= 0.001) bar = std::max(1, bar);
        const char* state = cpu_monitor::loadState(scaled);
        int range = state[0] == 'h' ? 2 : state[0] == 'w' ? 1 : 0;
        if (bar > 0 && in.h > 0)
            panel_style::fillPart(p, RectF(Rect(in.x, in.y, bar, in.h)), look.loadBar[range], look.load[range]);
    }
}

void LaneFrame::setLoad(double share) {
    if (share < 0) share = -1;
    // (repainted only when the bar would change by a pixel or more)
    if ((share < 0) == (load_ < 0) && std::fabs(share - load_) * 2 * std::max(1, width()) < 1) return;
    load_ = share;
    update();
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

void PinToBottom::setMargin(int margin) {
    margin_ = margin;
    cover_->setVisible(margin > 0);
    place();
}

void PinToBottom::init(int height, int margin) {
    height_ = height;
    margin_ = margin;
    auto* cover = viewport_->add<ui::Canvas>();
    cover->addStyleClass("pin-cover");  // (default.css: the window's colour)
    cover->onPaint = [cover](Painter& p) { ui::style::box(p, cover->rect(), cover->computedStyle()); };
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
    setStyleType("NameLabel");
    if (bold) {  // (a group's: NameLabel.group)
        addStyleClass("group");
        setFont(ui::style::font(computedStyle(), theme::uiFont()));
    }
    setMinimumWidth(10);
    hPolicy = ui::SizePolicy::Expanding;
    setElide(true);
}

VolumeSlider::VolumeSlider(double db, std::function<void(double)> slot) : ui::Slider(ui::Orientation::Horizontal) {
    setRange(config::MIN_DB, config::MAX_DB);
    setFocusPolicy(ui::FocusPolicy::None);
    setFixedHeight(theme::metric("volume-slider-height", 14));
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
    setFixedWidth(theme::metric("db-reading-width", 44));
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
    setStyleType("PanKnob");
    int size = theme::metric("pan-knob-size", 22);
    setFixedSize(size, size);
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
        if (grabbed) grabbed(true);
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
    bool was = dragging_;
    dragging_ = false;
    if (was && grabbed) grabbed(false);
    return true;
}

bool PanKnob::mouseDoubleClick(ui::MouseEvent& e) {
    setPan(0.0);
    return true;
}

void PanKnob::paint(Painter& p) {
    const css::Style& st = computedStyle();
    const css::Style& arcStyle = computedStyle("arc");
    const css::Style& pointer = computedStyle("pointer");
    Rect in = ui::style::inside(rect(), ui::style::insets(st, "padding"));
    RectF r((float)in.x, (float)in.y, (float)in.w, (float)in.h);
    p.fillEllipse(r, ui::style::background(st));
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
        p.strokePath(arc, ui::style::foreground(arcStyle), (float)arcStyle.length("stroke-width", 2));
    }
    double angle = (90 - pan * 135) * M_PI / 180;
    p.drawLine(PointF(cx, cy), PointF(cx + (float)std::cos(angle) * rx, cy - (float)std::sin(angle) * ry),
               ui::style::foreground(pointer), (float)pointer.length("stroke-width", 1.5));
}

// ============================================================================================
// SendsButton
// ============================================================================================

SendsPanel::SendsPanel(TrackPtr track, std::vector<TrackPtr> returns,
                       std::function<void(const TrackPtr&, double)> setLevel,
                       std::function<void(const TrackPtr&, bool)> setPreFader) {
    setStyleType("SendsPanel");
    const css::Style& st = computedStyle();
    auto grid = std::make_unique<ui::GridLayout>();
    ui::GridLayout* g = grid.get();
    g->hSpacing = ui::style::px(st, "column-gap", 6);
    g->vSpacing = ui::style::px(st, "row-gap", 4);
    // (inside its border and padding)
    ui::style::Insets pad = ui::style::insets(st, "padding"), border = ui::style::insets(st, "border-width");
    auto* title = add<ui::Label>(track->name + " sends to");
    // (<b>name</b>: the whole line bold is near enough)
    title->setFont(ui::style::font(computedStyle("title"), theme::uiFont()));
    g->addWidget(title, 0, 0, 1, 4);
    int row = 1;
    for (auto& target : returns) {
        double db = config::MIN_DB;
        bool pre = false;
        for (auto& s : track->sends)
            if (s.track == target) db = s.db, pre = s.preFader;
        TrackPtr t = target;
        auto* slider = add<VolumeSlider>(db, [setLevel, t](double v) { if (setLevel) setLevel(t, v); });
        slider->setMinimumWidth(theme::metric("send-slider-width", 150));
        slider->hPolicy = ui::SizePolicy::Expanding;
        g->addWidget(add<ui::Label>(target->name), row, 0);
        g->addWidget(slider, row, 1);
        g->addWidget(add<DbReading>(slider), row, 2);
        // Pre: before the fader (model.h Send PRE-FADER SENDS)
        ui::Button* preButton = widgets::toggleButton(
            this, "Pre", "accent", pre, [setPreFader, t](bool on) { if (setPreFader) setPreFader(t, on); },
            "Pre-fader: send the sound after this track's effects but before its fader and pan, so the send "
            "stays the same however its fader is set\nOff: after the fader and pan (it follows the fader)");
        preButton->setFixedWidth(theme::metric("pre-button-width", 34));
        g->addWidget(preButton, row, 3);
        sliders.push_back(slider);
        preButtons.push_back(preButton);
        ++row;
    }
    g->setColumnStretch(1, 1);
    grid->setContentsMargins(pad.left + border.left, pad.top + border.top, pad.right + border.right,
                             pad.bottom + border.bottom);
    setLayout(std::move(grid));
}

void SendsPanel::paint(Painter& p) {
    ui::style::box(p, rect(), computedStyle());  // (QFrame.StyledPanel)
}

SendsButton::SendsButton(TrackPtr track_, std::vector<TrackPtr> returns_, std::function<void()> changed_)
    : ui::Button("→"), track(std::move(track_)), returns(std::move(returns_)), changed(std::move(changed_)) {
    setStyle(ui::Button::Style::Tool);
    setFixedSize(theme::metric("sends-button-width", 20), theme::metric("sends-button-height", 16));
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

bool SendsButton::preFader(const TrackPtr& target) const {
    for (auto& s : track->sends)
        if (s.track == target) return s.preFader;
    return false;
}

void SendsButton::showState() {
    std::string up;
    for (auto& s : track->sends)
        if (s.db > config::MIN_DB)
            up += (up.empty() ? "" : ", ") + s.track->name + " " + fmt::db(std::nearbyint(s.db), config::MIN_DB) +
                  (s.preFader ? " (pre-fader)" : "");
    setProperty("lit", up.empty() ? "" : "true");  // the skin's accent colour
    setToolTip("Sends to the return tracks: " + (up.empty() ? std::string("none yet") : up) + " (click to set them)");
    update();
}

void SendsButton::setLevel(const TrackPtr& target, double db) {
    std::vector<Send> kept;
    bool pre = preFader(target);
    for (auto& s : track->sends)
        if (s.track != target) kept.push_back(s);
    // (a pre-fader send turned right down stays, silent, so it is still pre-fader when turned up again)
    if (db > config::MIN_DB || pre) kept.push_back(Send{target, std::max(db, (double)config::MIN_DB), pre});
    track->sends = kept;  // (the mixer reads it every buffer)
    showState();
    if (changed) changed();
}

void SendsButton::setPreFader(const TrackPtr& target, bool on) {
    std::vector<Send> sends = track->sends.copy();
    bool found = false;
    for (auto& s : sends)
        if (s.track == target) s.preFader = on, found = true;
    if (!found && on) sends.push_back(Send{target, config::MIN_DB, true});  // (no send yet: Pre for when there is)
    if (!on) {  // (a silent post-fader send is no send at all)
        std::vector<Send> kept;
        for (auto& s : sends)
            if (s.db > config::MIN_DB || s.preFader) kept.push_back(s);
        sends = kept;
    }
    track->sends = sends;  // (replaced as a whole: the mixer reads it every buffer)
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
    auto panel = std::make_unique<SendsPanel>(track, returns, [this](const TrackPtr& t, double db) { setLevel(t, db); },
                                              [this](const TrackPtr& t, bool on) { setPreFader(t, on); });
    Point size = panel->sizeHint();
    const css::Style& st = panel->computedStyle();  // (SendsPanel's min-width / min-height: the popup's least size)
    o.w = std::max(ui::style::px(st, "min-width", 220), size.x);
    o.h = std::max(ui::style::px(st, "min-height", 40), size.y);
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
    ui::Button* b = toolButton(parent, text, theme::metric("toggle-button-width", 26), theme::metric("row-button-height", 22),
                               tooltip);
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
    combo->setFixedWidth(theme::metric("output-selector-width", 58));
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
    ui::Button* b = toolButton(parent, "FX", theme::metric("fx-button-width", 28), theme::metric("row-button-height", 22),
                               effectsSummary(effects));
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
                             std::function<void(ui::Menu&)> fillMenu, int rack, SamplerPtr sampler,
                             PadKitPtr padKit) {
    ui::Button* b = toolButton(parent, "🎹", theme::metric("instrument-button-width", 40),
                               theme::metric("row-button-height", 22), "");
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
    } else if (padKit) {
        int used = padKit->used();
        b->setToolTip("Instrument: Pad Kit, " + std::to_string(used) + " pad" + (used == 1 ? "" : "s") +
                      " with a sound\nClick to open its window; ▾ to choose another instrument");
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
    ui::Button* b = toolButton(parent, text, theme::metric("icon-button-width", 22), theme::metric("row-button-height", 22),
                               tooltip);
    if (slot) b->clicked.connect(slot);
    return b;
}

}  // namespace widgets
