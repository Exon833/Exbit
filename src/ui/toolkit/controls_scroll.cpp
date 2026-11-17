// Slider, ScrollBar, ScrollArea, ListWidget, TreeWidget: their look is default.css's (Lists, scrolling, tabs and
// menus): every colour, size, padding and radius they draw with comes from their styles and their parts'.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/style.h"

namespace ui {

// ============================================================================================
// PartStyles: the parts with states of their own (also used by menu.cpp and filedialog.cpp)
// ============================================================================================

struct PartStyles::PartNode : css::Node {
    const Widget& w;
    const std::vector<std::string>& own;
    const std::vector<const char*>& states;
    PartNode(const Widget& widget, const std::vector<std::string>& o, const std::vector<const char*>& s)
        : w(widget), own(o), states(s) {}
    const std::string& type() const override { return w.type(); }
    bool isType(const std::string& name) const override { return w.isType(name); }
    const std::string& id() const override { return w.id(); }
    bool hasClass(const std::string& name) const override { return w.hasClass(name); }
    bool hasState(const std::string& name) const override {
        for (const char* s : states)
            if (name == s) return true;
        for (const std::string& s : own)
            if (s == name) return true;
        return false;
    }
    const css::Node* parent() const override { return w.parent(); }
};

PartStyles::PartStyles(const Widget& w) : w_(w) {
    std::vector<std::string> all;
    w.styleStates(all);
    for (std::string& s : all)
        if (s != "hover" && s != "pressed") own_.push_back(std::move(s));
}

PartStyles::~PartStyles() = default;

const css::Style& PartStyles::get(const std::string& part, const std::vector<const char*>& states, const css::Style* from) {
    std::string key = part;
    key += '|';
    for (const char* s : states) {
        key += s;
        key += ' ';
    }
    if (from) {
        char buf[32];
        snprintf(buf, sizeof buf, "|%p", (const void*)from);
        key += buf;
    }
    auto it = cache_.find(key);
    if (it != cache_.end()) return *it->second;
    PartNode node(w_, own_, states);
    const css::Style& base = from ? *from : w_.computedStyle();
    auto st = std::make_unique<css::Style>(theme::styles().compute(node, &base, part));
    const css::Style& ref = *st;
    cache_.emplace(std::move(key), std::move(st));
    return ref;
}

const Font& PartStyles::font(const css::Style& st, const Font& def) {
    auto it = fonts_.find(&st);
    if (it == fonts_.end()) it = fonts_.emplace(&st, style::font(st, def)).first;
    return it->second;
}

double PartStyles::lengthVar(const css::Style& st, const std::string& name, double def) {
    std::optional<std::string> text = st.variable(name);
    if (!text) return def;
    css::Style probe;  // (the variable's text as a property, read as a length)
    probe.props_["width"] = *text;
    probe.emPx_ = st.emPx_;
    return probe.length("width", def);
}

// ============================================================================================
// Slider
// ============================================================================================

Slider::Slider(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    setStyleType("Slider");
    setFocusPolicy(FocusPolicy::Strong);
    setMouseTracking(true);
    if (o == Orientation::Horizontal) {
        hPolicy = SizePolicy::Expanding;
        vPolicy = SizePolicy::Fixed;
    } else {
        hPolicy = SizePolicy::Fixed;
        vPolicy = SizePolicy::Expanding;
    }
}

Slider::~Slider() { stopRepeat(); }

void Slider::stopRepeat() {
    if (repeatTimer_ >= 0) {
        App::get().cancelTimer(repeatTimer_);
        repeatTimer_ = -1;
    }
    repeatDir_ = 0;
}

void Slider::setRange(int lo, int hi) {
    lo_ = lo;
    hi_ = std::max(lo, hi);
    if (pos_ < lo_ || pos_ > hi_) pos_ = std::max(lo_, std::min(hi_, pos_));
    setValue(value_);
    update();
}

void Slider::setValue(int v) {
    v = std::max(lo_, std::min(hi_, v));
    if (!dragging_ || tracking_) pos_ = v;
    if (v == value_) {
        update();
        return;
    }
    value_ = v;
    update();
    valueChanged.emit(v);
}

void Slider::setValueSilently(int v) {
    value_ = pos_ = std::max(lo_, std::min(hi_, v));
    update();
}

void Slider::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    out.push_back(orientation == Orientation::Horizontal ? "horizontal" : "vertical");
}

Point Slider::sizeHint() const {
    // (default.css: 84 x 16, or 16 x 84 when vertical)
    const css::Style& st = computedStyle();
    bool horz = orientation == Orientation::Horizontal;
    Point h(style::px(st, "width", horz ? 84 : 16), style::px(st, "height", horz ? 16 : 84));
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point Slider::minimumSizeHint() const {
    Point h = sizeHint();
    const css::Style& st = computedStyle();
    if (orientation == Orientation::Horizontal) h.x = std::min(h.x, style::px(st, "min-width", 20));
    else h.y = std::min(h.y, style::px(st, "min-height", 20));
    return h;
}

void Slider::handleStates(std::vector<const char*>& out) const {
    if (hoverHandle_) out.push_back("hover");
    if (dragging_) out.push_back("pressed");
}

Point Slider::handleSize() const {
    std::vector<const char*> states;
    handleStates(states);
    PartStyles parts(*this);
    const css::Style& hs = parts.get("handle", states);
    return Point(std::max(0, style::px(hs, "width", 10)), std::max(0, style::px(hs, "height", 10)));
}

int Slider::handleCenter(int v) const {
    bool horz = orientation == Orientation::Horizontal;
    int len = horz ? width() : height();
    Point hs = handleSize();
    int along = horz ? hs.x : hs.y;
    int span = std::max(0, len - along);
    double t = hi_ > lo_ ? (double)(v - lo_) / (hi_ - lo_) : 0;
    int c = along / 2 + (int)std::lround(span * t);
    return horz ? c : len - c;
}

int Slider::valueAt(Point p) const {
    bool horz = orientation == Orientation::Horizontal;
    int len = horz ? width() : height();
    Point hs = handleSize();
    int along = horz ? hs.x : hs.y;
    int span = std::max(1, len - along);
    int c = horz ? p.x : len - p.y;
    double t = (double)(c - along / 2) / span;
    t = std::max(0.0, std::min(1.0, t));
    return lo_ + (int)std::lround(t * (hi_ - lo_));
}

void Slider::paint(Painter& p) {
    // the groove across the middle, its part below the value (fill), the handle: boxes of their styles
    bool horz = orientation == Orientation::Horizontal;
    const css::Style& groove = computedStyle("groove");
    const css::Style& fill = computedStyle("fill");
    std::vector<const char*> states;
    handleStates(states);
    PartStyles parts(*this);
    const css::Style& handle = parts.get("handle", states);
    int hw = std::max(0, style::px(handle, "width", 10)), hh = std::max(0, style::px(handle, "height", 10));
    int len = horz ? width() : height(), along = horz ? hw : hh;
    double t = hi_ > lo_ ? (double)(pos_ - lo_) / (hi_ - lo_) : 0;
    int c = along / 2 + (int)std::lround(std::max(0, len - along) * t);
    int hc = horz ? c : len - c;
    Rect hr;
    if (horz) {
        int cy = height() / 2, th = std::max(0, style::px(groove, "height", 4));
        style::box(p, Rect(0, cy - th / 2, width(), th), groove);
        style::box(p, Rect(0, cy - th / 2, hc, th), fill);
        hr = Rect(hc - hw / 2, cy - hh / 2, hw, hh);
    } else {
        int cx = width() / 2, th = std::max(0, style::px(groove, "width", 4));
        style::box(p, Rect(cx - th / 2, 0, th, height()), groove);
        style::box(p, Rect(cx - th / 2, hc, th, height() - hc), fill);
        hr = Rect(cx - hw / 2, hc - hh / 2, hw, hh);
    }
    style::box(p, hr, handle);
}

bool Slider::mousePress(MouseEvent& e) {
    bool horz = orientation == Orientation::Horizontal;
    int c = horz ? e.pos.x : e.pos.y;
    int hc = handleCenter(pos_);
    Point hs = handleSize();
    if (e.button == plat::MiddleButton) {
        dragging_ = true;
        dragOffset_ = 0;
        sliderPressed.emit();
        int v = valueAt(e.pos);
        pos_ = v;
        if (tracking_) setValue(v);
        update();
        return true;
    }
    if (e.button != plat::LeftButton) return false;
    if (std::abs(c - hc) <= (horz ? hs.x : hs.y) / 2 + 1) {  // (on the handle, or a pixel beside it)
        dragging_ = true;
        dragOffset_ = c - hc;
        sliderPressed.emit();
        update();
        return true;
    }
    // on the groove: a page towards the pointer, repeated while held
    int dir = horz ? (c > hc ? 1 : -1) : (c < hc ? 1 : -1);
    repeatDir_ = dir;
    setValue(value_ + dir * page_);
    stopRepeat();
    repeatDir_ = dir;
    repeatTimer_ = App::get().singleShot(300, [this] {
        repeatTimer_ = App::get().every(100, [this] {
            if (!isPressed() || !repeatDir_) return;
            Point lp = mapFromGlobal(App::get().cursorPos());
            int target = valueAt(lp);
            if ((repeatDir_ > 0 && value_ < target) || (repeatDir_ < 0 && value_ > target))
                setValue(value_ + repeatDir_ * page_);
        });
    });
    return true;
}

bool Slider::mouseMove(MouseEvent& e) {
    bool horz = orientation == Orientation::Horizontal;
    int c = horz ? e.pos.x : e.pos.y;
    int across = horz ? e.pos.y : e.pos.x;
    int mid = horz ? height() / 2 : width() / 2;
    // (over the handle: a pixel beside it along the groove, two across it)
    Point hs = handleSize();
    int alongHalf = (horz ? hs.x : hs.y) / 2, acrossHalf = (horz ? hs.y : hs.x) / 2;
    bool hh = rect().contains(e.pos) && std::abs(c - handleCenter(pos_)) <= alongHalf + 1 &&
              std::abs(across - mid) <= acrossHalf + 2;
    if (hh != hoverHandle_) {
        hoverHandle_ = hh;
        update();
    }
    if (!dragging_) return isPressed();
    Point q = e.pos;
    if (horz) q.x -= dragOffset_;
    else q.y -= dragOffset_;
    int v = valueAt(q);
    if (v != pos_) {
        pos_ = v;
        if (tracking_) setValue(v);
        else update();
    }
    return true;
}

bool Slider::mouseRelease(MouseEvent& e) {
    stopRepeat();
    if (dragging_) {
        dragging_ = false;
        if (!tracking_) setValue(pos_);
        sliderReleased.emit();
        update();
    }
    return true;
}

bool Slider::mouseDoubleClick(MouseEvent& e) {
    if (!doubleClicked.empty()) {
        doubleClicked.emit();
        return true;
    }
    return mousePress(e);
}

bool Slider::wheel(WheelEvent& e) {
    if (!wheelOn_ || !isEnabled()) return false;
    float d = std::fabs(e.dy) >= std::fabs(e.dx) ? e.dy : -e.dx;
    wheelAcc_ += d;
    int notches = (int)wheelAcc_;
    if (!notches) return true;
    wheelAcc_ -= notches;
    int delta = (e.ctrl() || e.shift()) ? notches * page_ : std::max(-page_, std::min(page_, notches * 3 * single_));
    int old = value_;
    setValue(value_ + delta);
    return value_ != old;
}

bool Slider::keyPress(KeyEvent& e) {
    switch (e.key) {
    case plat::Key_Left:
    case plat::Key_Down: setValue(value_ - single_); return true;
    case plat::Key_Right:
    case plat::Key_Up: setValue(value_ + single_); return true;
    case plat::Key_PageUp: setValue(value_ + page_); return true;
    case plat::Key_PageDown: setValue(value_ - page_); return true;
    case plat::Key_Home: setValue(lo_); return true;
    case plat::Key_End: setValue(hi_); return true;
    default: return false;
    }
}

// ============================================================================================
// ScrollBar
// ============================================================================================

ScrollBar::ScrollBar(Orientation o, Widget* parent) : Widget(parent), orientation(o) {
    setStyleType("ScrollBar");
    setMouseTracking(true);
    if (o == Orientation::Vertical) {
        hPolicy = SizePolicy::Fixed;
        vPolicy = SizePolicy::Expanding;
    } else {
        hPolicy = SizePolicy::Expanding;
        vPolicy = SizePolicy::Fixed;
    }
}

ScrollBar::~ScrollBar() { stopRepeat(); }

void ScrollBar::stopRepeat() {
    if (repeatTimer_ >= 0) {
        App::get().cancelTimer(repeatTimer_);
        repeatTimer_ = -1;
    }
}

void ScrollBar::setRange(int lo, int hi) {
    lo_ = lo;
    hi_ = std::max(lo, hi);
    setValue(value_);
    update();
}

void ScrollBar::setPageStep(int page) {
    page_ = std::max(0, page);
    update();
}

void ScrollBar::setValue(int v) {
    v = std::max(lo_, std::min(hi_, v));
    if (v == value_) return;
    value_ = v;
    update();
    valueChanged.emit(v);
}

void ScrollBar::styleStates(std::vector<std::string>& out) const {
    Widget::styleStates(out);
    out.push_back(orientation == Orientation::Horizontal ? "horizontal" : "vertical");
}

Point ScrollBar::sizeHint() const {
    // (default.css: --scrollbar-width across it, 40 px along it)
    const css::Style& st = computedStyle();
    bool v = orientation == Orientation::Vertical;
    int w = theme::metric("scrollbar-width", 10);
    Point h(style::px(st, "width", v ? w : 40), style::px(st, "height", v ? 40 : w));
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

int ScrollBar::thickness() const {
    Point h = sizeHint();
    return std::max(0, orientation == Orientation::Vertical ? h.x : h.y);
}

void ScrollBar::thumbStates(std::vector<const char*>& out) const {
    if (hoverHandle_) out.push_back("hover");
    if (dragging_) out.push_back("pressed");
}

namespace {
// the thumb's slot (the thumb with its margins) in a bar `len` long and `across` thick
Rect thumbSlot(const css::Style& thumb, bool v, int len, int across, int lo, int hi, int value, int page) {
    style::Insets m = style::insets(thumb, "margin");
    int range = hi - lo;
    int hl = len;
    int pos = 0;
    if (range > 0) {
        // (its shortest: min-height / min-width, with its margins along the bar; default.css: 24 + 2 + 2)
        int least = v ? style::px(thumb, "min-height", 24) + m.h() : style::px(thumb, "min-width", 24) + m.w();
        hl = (int)std::max<long long>(least, (long long)len * page / std::max(1, range + page));
        hl = std::min(hl, len);
        pos = (int)std::lround((double)(len - hl) * (value - lo) / range);
    }
    return v ? Rect(0, pos, across, hl) : Rect(pos, 0, hl, across);
}
}  // namespace

Rect ScrollBar::slotRect() const {
    std::vector<const char*> states;
    thumbStates(states);
    PartStyles parts(*this);
    bool v = orientation == Orientation::Vertical;
    return thumbSlot(parts.get("thumb", states), v, v ? height() : width(), v ? width() : height(), lo_, hi_, value_,
                     page_);
}

Rect ScrollBar::handleRect() const {
    std::vector<const char*> states;
    thumbStates(states);
    PartStyles parts(*this);
    const css::Style& thumb = parts.get("thumb", states);
    bool v = orientation == Orientation::Vertical;
    Rect slot = thumbSlot(thumb, v, v ? height() : width(), v ? width() : height(), lo_, hi_, value_, page_);
    return style::inside(slot, style::insets(thumb, "margin"));
}

void ScrollBar::paint(Painter& p) {
    style::box(p, rect(), computedStyle());
    std::vector<const char*> states;
    thumbStates(states);
    PartStyles parts(*this);
    const css::Style& thumb = parts.get("thumb", states);
    bool v = orientation == Orientation::Vertical;
    Rect slot = thumbSlot(thumb, v, v ? height() : width(), v ? width() : height(), lo_, hi_, value_, page_);
    style::box(p, style::inside(slot, style::insets(thumb, "margin")), thumb);
}

bool ScrollBar::mousePress(MouseEvent& e) {
    bool v = orientation == Orientation::Vertical;
    int c = v ? e.pos.y : e.pos.x;
    Rect h = slotRect();
    int hs = v ? h.y : h.x, hl = v ? h.h : h.w;
    int len = v ? height() : width();
    if (e.button == plat::MiddleButton && hi_ > lo_) {
        // jump there and drag
        int start = c - hl / 2;
        setValue(lo_ + (int)std::lround((double)std::max(0, start) * (hi_ - lo_) / std::max(1, len - hl)));
        dragging_ = true;
        dragOffset_ = hl / 2;
        update();
        return true;
    }
    if (e.button != plat::LeftButton) return false;
    if (c >= hs && c < hs + hl) {
        dragging_ = true;
        dragOffset_ = c - hs;
        update();
        return true;
    }
    int dir = c < hs ? -1 : 1;
    setValue(value_ + dir * std::max(1, page_));
    stopRepeat();
    repeatTimer_ = App::get().singleShot(300, [this, dir] {
        repeatTimer_ = App::get().every(60, [this, dir] {
            if (!isPressed()) return;
            Point lp = mapFromGlobal(App::get().cursorPos());
            bool vv = orientation == Orientation::Vertical;
            int cc = vv ? lp.y : lp.x;
            Rect hr = slotRect();
            int s = vv ? hr.y : hr.x, l = vv ? hr.h : hr.w;
            if ((dir < 0 && cc < s) || (dir > 0 && cc >= s + l)) setValue(value_ + dir * std::max(1, page_));
        });
    });
    return true;
}

bool ScrollBar::mouseMove(MouseEvent& e) {
    bool hh = slotRect().contains(e.pos);
    if (hh != hoverHandle_) {
        hoverHandle_ = hh;
        update();
    }
    if (!dragging_) return isPressed();
    bool v = orientation == Orientation::Vertical;
    int c = v ? e.pos.y : e.pos.x;
    int len = v ? height() : width();
    Rect h = slotRect();
    int hl = v ? h.h : h.w;
    int start = c - dragOffset_;
    int room = std::max(1, len - hl);
    if (hi_ > lo_) setValue(lo_ + (int)std::lround((double)start * (hi_ - lo_) / room));
    return true;
}

bool ScrollBar::mouseRelease(MouseEvent& e) {
    stopRepeat();
    if (dragging_) {
        dragging_ = false;
        update();
    }
    return true;
}

bool ScrollBar::wheel(WheelEvent& e) {
    float d = orientation == Orientation::Vertical ? e.dy : (e.dx != 0 ? e.dx : e.dy);
    if (d == 0) return false;
    int old = value_;
    setValue(value_ - (int)std::lround(d * 3 * single_));
    return value_ != old;
}

// ============================================================================================
// ScrollArea
// ============================================================================================

namespace {
class Viewport : public Widget {
public:
    ScrollArea* area = nullptr;
    void childSizeHintChanged(Widget*) override { if (area) App::get().scheduleLayout(area); }
};

class AreaLayout : public Layout {
public:
    ScrollArea* area = nullptr;
    void apply(Widget*, const Rect&) override { area->place(); }
    Point sizeHint() const override { return area->sizeHint(); }
    Point minimumSize() const override { return area->minimumSizeHint(); }
    int expandingDirections() const override { return 3; }
};
}  // namespace

ScrollArea::ScrollArea(Widget* parent) : Widget(parent) {
    setStyleType("ScrollArea");
    auto* vp = add<Viewport>();
    vp->area = this;
    viewport_ = vp;
    vbar_ = add<ScrollBar>(Orientation::Vertical);
    hbar_ = add<ScrollBar>(Orientation::Horizontal);
    vbar_->setVisible(false);
    hbar_->setVisible(false);
    auto moveContent = [this](int) {
        if (content_ && Widget::alive(content_)) content_->move(-hbar_->value(), -vbar_->value());
    };
    vbar_->valueChanged.connect(moveContent);
    hbar_->valueChanged.connect(moveContent);
    hPolicy = vPolicy = SizePolicy::Expanding;
    auto l = std::make_unique<AreaLayout>();
    l->area = this;
    setLayout(std::move(l));
}

void ScrollArea::setContent(std::unique_ptr<Widget> content, bool widgetResizable) {
    if (content_ && Widget::alive(content_)) content_->deleteLater();
    content_ = content.get();
    resizable_ = widgetResizable;
    viewport_->adopt(std::move(content));
    if (!resizable_ && content_->width() == 0 && content_->height() == 0) {
        Point h = content_->sizeHint();
        content_->resize(h.x, h.y);
    }
    vbar_->setValue(0);
    hbar_->setValue(0);
    place();
}

void ScrollArea::place() {
    if (placing_) return;
    placing_ = true;
    layoutDirty_ = false;
    int W = width(), H = height(), vsb = vbar_->thickness(), hsb = hbar_->thickness();
    Widget* c = content_ && Widget::alive(content_) ? content_ : nullptr;
    Point chint(0, 0), cmin(0, 0), cmax(1 << 24, 1 << 24);
    if (c) {
        chint = c->sizeHint();
        Point mh = c->minimumSizeHint(), mn = c->minimumSize();
        cmin = Point(mn.x > 0 ? mn.x : mh.x, mn.y > 0 ? mn.y : mh.y);
        if (c->hPolicy == SizePolicy::Fixed) cmin.x = chint.x;
        if (c->vPolicy == SizePolicy::Fixed) cmin.y = chint.y;
        cmax = c->maximumSize();
    }
    bool needV = vpol_ == Policy::AlwaysOn, needH = hpol_ == Policy::AlwaysOn;
    int vw = W, vh = H, cw = 0, ch = 0;
    for (int i = 0; i < 4; ++i) {
        vw = std::max(0, W - (needV ? vsb : 0));
        vh = std::max(0, H - (needH ? hsb : 0));
        if (c) {
            if (resizable_) {
                cw = std::min(std::max(vw, cmin.x), std::max(cmin.x, cmax.x));
                ch = std::min(std::max(vh, cmin.y), std::max(cmin.y, cmax.y));
            } else {
                cw = c->width();
                ch = c->height();
            }
        }
        bool nv = vpol_ == Policy::AlwaysOn || (vpol_ == Policy::AsNeeded && ch > vh);
        bool nh = hpol_ == Policy::AlwaysOn || (hpol_ == Policy::AsNeeded && cw > vw);
        if (nv == needV && nh == needH) break;
        needV = nv;
        needH = nh;
    }
    viewport_->setGeometry(0, 0, vw, vh);
    vbar_->setVisible(needV);
    hbar_->setVisible(needH);
    vbar_->setGeometry(vw, 0, vsb, vh);
    hbar_->setGeometry(0, vh, vw, hsb);
    vbar_->setPageStep(vh);
    hbar_->setPageStep(vw);
    vbar_->setRange(0, std::max(0, ch - vh));
    hbar_->setRange(0, std::max(0, cw - vw));
    if (c) c->setGeometry(-hbar_->value(), -vbar_->value(), cw, ch);
    placing_ = false;
}

void ScrollArea::contentResized() { place(); }

void ScrollArea::resized() { place(); }

void ScrollArea::scrollTo(int x, int y) {
    hbar_->setValue(x);
    vbar_->setValue(y);
}

void ScrollArea::ensureVisible(const Rect& r) {
    int vw = viewport_->width(), vh = viewport_->height();
    int v = vbar_->value(), h = hbar_->value();
    if (r.bottom() > v + vh) v = r.bottom() - vh;
    if (r.y < v) v = r.y;
    if (r.right() > h + vw) h = r.right() - vw;
    if (r.x < h) h = r.x;
    vbar_->setValue(v);
    hbar_->setValue(h);
}

bool ScrollArea::wheel(WheelEvent& e) {
    bool horiz = e.shift() || std::fabs(e.dx) > std::fabs(e.dy);
    float d = horiz ? (e.dx != 0 ? e.dx : e.dy) : e.dy;
    ScrollBar* b = horiz ? hbar_ : vbar_;
    int old = b->value();
    b->setValue(old - (int)std::lround(d * 3 * b->singleStep()));
    return b->value() != old;
}

Point ScrollArea::sizeHint() const {
    int fh = style::textHeight(theme::uiFont());
    Point h(12 * fh, 8 * fh);
    if (content_ && Widget::alive(content_)) {
        Point c = resizable_ ? content_->sizeHint() : Point(content_->width(), content_->height());
        h = c;
    }
    if (vpol_ == Policy::AlwaysOn) h.x += vbar_->thickness();
    if (hpol_ == Policy::AlwaysOn) h.y += hbar_->thickness();
    h.x = std::min(h.x, 36 * fh);
    h.y = std::min(h.y, 24 * fh);
    Point mn = minimumSize(), mx = maximumSize();
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}

Point ScrollArea::minimumSizeHint() const { return Point(4 * vbar_->thickness(), 4 * hbar_->thickness()); }

// ============================================================================================
// ListWidget
// ============================================================================================

namespace {
// a list's (or tree's) default size, and its least, from its style (width / height, min-width / min-height)
Point styledSize(const css::Style& st, Point def, const Point& mn, const Point& mx) {
    Point h(style::px(st, "width", def.x), style::px(st, "height", def.y));
    if (mn.x == mx.x) h.x = mn.x;
    if (mn.y == mx.y) h.y = mn.y;
    return h;
}
Point styledMinimum(const css::Style& st, Point def) {
    return Point(style::px(st, "min-width", def.x), style::px(st, "min-height", def.y));
}
}  // namespace

ListWidget::ListWidget(Widget* parent) : Widget(parent) {
    setStyleType("ListWidget");
    bar_ = add<ScrollBar>(Orientation::Vertical);
    bar_->setVisible(false);
    bar_->valueChanged.connect([this](int) { update(); });
    setFocusPolicy(FocusPolicy::Strong);
    setMouseTracking(true);
    hPolicy = vPolicy = SizePolicy::Expanding;
}

Point ListWidget::sizeHint() const { return styledSize(computedStyle(), Point(256, 192), minimumSize(), maximumSize()); }

Point ListWidget::minimumSizeHint() const { return styledMinimum(computedStyle(), Point(40, 40)); }

bool ListWidget::readStyle() {
    PartStyles parts(*this);
    int h = std::max(1, style::px(parts.get("item"), "height", 22));
    if (h == rowHeight) return false;
    rowHeight = h;
    return true;
}

Rect ListWidget::viewRect() const {
    Rect in = style::contents(rect(), computedStyle());
    if (bar_->isVisible()) in.w = std::max(0, in.w - bar_->thickness());
    return in;
}

void ListWidget::syncBar() {
    readStyle();
    Rect in = style::contents(rect(), computedStyle());
    int total = count() * rowHeight, vh = std::max(0, in.h);
    bool need = total > vh && vh > 0;
    int bw = bar_->thickness();
    bar_->setVisible(need);
    bar_->setGeometry(in.right() - bw, in.y, bw, vh);
    bar_->setPageStep(vh);
    bar_->setSingleStep(rowHeight);
    bar_->setRange(0, std::max(0, total - vh));
    update();
}

void ListWidget::resized() { syncBar(); }

void ListWidget::addItem(const std::string& text, const std::string& data) {
    Item it;
    it.text = text;
    it.data = data;
    addItem(std::move(it));
}

void ListWidget::addItem(Item item) { insertItem(count(), std::move(item)); }

void ListWidget::insertItem(int row, Item item) {
    row = std::max(0, std::min(row, count()));
    items_.insert(items_.begin() + row, std::move(item));
    selected_.insert(selected_.begin() + row, false);
    if (current_ >= row) ++current_;
    if (anchor_ >= row) ++anchor_;
    syncBar();
}

void ListWidget::removeItem(int row) {
    if (row < 0 || row >= count()) return;
    items_.erase(items_.begin() + row);
    selected_.erase(selected_.begin() + row);
    if (hoverRow_ >= count()) hoverRow_ = -1;
    if (anchor_ == row) anchor_ = -1;
    else if (anchor_ > row) --anchor_;
    bool emitIt = false;
    if (current_ > row) --current_;
    else if (current_ == row) {
        current_ = count() ? std::min(row, count() - 1) : -1;
        if (!multi_ && current_ >= 0) selected_[current_] = true;
        emitIt = true;
    }
    syncBar();
    if (emitIt) currentRowChanged.emit(current_);
}

void ListWidget::clear() {
    items_.clear();
    selected_.clear();
    bool had = current_ >= 0;
    current_ = hoverRow_ = anchor_ = pressRow_ = -1;
    bar_->setValue(0);
    syncBar();
    if (had) currentRowChanged.emit(-1);
}

std::vector<int> ListWidget::selectedRows() const {
    std::vector<int> out;
    for (int i = 0; i < count(); ++i)
        if (selected_[i]) out.push_back(i);
    return out;
}

bool ListWidget::isRowSelected(int row) const { return row >= 0 && row < count() && selected_[row]; }

void ListWidget::clearSelection() {
    std::fill(selected_.begin(), selected_.end(), false);
    update();
}

void ListWidget::setRowSelected(int row, bool on) {
    if (row < 0 || row >= count()) return;
    if (!multi_ && on) std::fill(selected_.begin(), selected_.end(), false);
    selected_[row] = on && items_[row].selectable;
    update();
}

int ListWidget::findData(const std::string& data) const {
    for (int i = 0; i < count(); ++i)
        if (items_[i].data == data) return i;
    return -1;
}

void ListWidget::setCurrentRow(int row) {
    if (row < -1 || row >= count()) return;
    std::fill(selected_.begin(), selected_.end(), false);
    if (row >= 0 && items_[row].selectable) selected_[row] = true;
    anchor_ = row;
    if (row >= 0) scrollToRow(row);
    update();
    if (row == current_) return;
    current_ = row;
    currentRowChanged.emit(row);
}

void ListWidget::select(int row, int mods) {
    if (row < 0 || row >= count()) return;
    if (!multi_) {
        setCurrentRow(row);
        return;
    }
    bool ctrl = mods & plat::ModCtrl, shift = mods & plat::ModShift;
    if (ctrl) {
        if (items_[row].selectable) selected_[row] = !selected_[row];
        anchor_ = row;
    } else if (shift && anchor_ >= 0) {
        std::fill(selected_.begin(), selected_.end(), false);
        int a = std::min(anchor_, row), b = std::max(anchor_, row);
        for (int i = a; i <= b; ++i) selected_[i] = items_[i].selectable && items_[i].enabled;
    } else {
        std::fill(selected_.begin(), selected_.end(), false);
        selected_[row] = items_[row].selectable;
        anchor_ = row;
    }
    scrollToRow(row);
    update();
    if (row != current_) {
        current_ = row;
        currentRowChanged.emit(row);
    }
}

void ListWidget::scrollToRow(int row) {
    if (row < 0 || row >= count()) return;
    int y = row * rowHeight, v = bar_->value(), vh = viewRect().h;
    if (y < v) v = y;
    else if (y + rowHeight > v + vh) v = y + rowHeight - vh;
    bar_->setValue(v);
}

int ListWidget::rowAt(Point p) const {
    Rect vr = viewRect();
    if (!vr.contains(p)) return -1;
    int row = (p.y - vr.y + bar_->value()) / std::max(1, rowHeight);
    return row < count() ? row : -1;
}

void ListWidget::rowStates(int row, std::vector<const char*>& out) const {
    const Item& it = items_[row];
    if (selected_[row]) out.push_back("selected");
    if (row == hoverRow_ && it.enabled && isEnabled()) out.push_back("hover");
    if (alternate_ && row % 2) out.push_back("alternate");
    if (!it.enabled) out.push_back("disabled");
    if (it.checkable) out.push_back("checkable");
    if (it.checked) out.push_back("checked");
    if (it.bold) out.push_back("bold");
}

namespace {
// where a row's check box is: at the left of its content, centred in it (ListWidget::indicator)
Rect indicatorRect(const Rect& content, const css::Style& ind) {
    int w = std::max(0, style::px(ind, "width", 14)), h = std::max(0, style::px(ind, "height", 14));
    return Rect(content.x + style::px(ind, "margin-left"), content.y + (content.h - h) / 2, w, h);
}
}  // namespace

Rect ListWidget::checkRect(int row) const {
    if (row < 0 || row >= count()) return Rect();
    std::vector<const char*> states;
    rowStates(row, states);
    PartStyles parts(*this);
    const css::Style& item = parts.get("item", states);
    Rect vr = viewRect();
    Rect r(vr.x, vr.y + row * rowHeight - bar_->value(), vr.w, rowHeight);
    return indicatorRect(style::contents(r, item), parts.get("indicator", states, &item));
}

void ListWidget::paint(Painter& p) {
    // its box, then each row seen: the row's box (ListWidget::item in the row's states), its check box
    // (::indicator), its icon (::icon) and its text; each combination of states is worked out once
    if (readStyle()) syncBar();
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Rect vr = viewRect(), inner = style::contents(rect(), st);
    bool clip = inner != rect();  // (rows inside its border and padding; the bar is drawn over them)
    if (clip) {
        p.save();
        p.setClip(inner);
    }
    PartStyles parts(*this);
    Font f = theme::uiFont();
    std::vector<const char*> states;
    int scroll = bar_->value();
    int first = std::max(0, scroll / std::max(1, rowHeight));
    for (int row = first; row < count(); ++row) {
        int y = vr.y + row * rowHeight - scroll;
        if (y >= vr.bottom()) break;
        const Item& it = items_[row];
        states.clear();
        rowStates(row, states);
        const css::Style& item = parts.get("item", states);
        Rect r(vr.x, y, vr.w, rowHeight);
        style::box(p, r, item);
        Rect c = style::contents(r, item);
        int x = c.x;
        if (it.checkable) {
            const css::Style& ind = parts.get("indicator", states, &item);
            Rect cr = indicatorRect(c, ind);
            style::box(p, cr, ind);
            if (it.checked) {
                Rect mark = style::contents(cr, ind);
                style::icon(p, (float)mark.x, (float)mark.y, (float)std::min(mark.w, mark.h), "check", style::foreground(ind));
            }
            x = cr.right() + style::px(ind, "margin-right", 6);
        }
        if (!it.icon.empty()) {
            const css::Style& ic = parts.get("icon", states, &item);
            int iw = std::max(0, style::px(ic, "width", 16)), ih = std::max(0, style::px(ic, "height", 16));
            x += style::px(ic, "margin-left");
            int iy = c.y + (c.h - it.icon.height()) / 2;
            if (it.icon.width() <= iw && it.icon.height() <= ih) p.drawImage(Point(x, iy), it.icon);
            else p.drawImage(RectF((float)x, c.y + (c.h - ih) / 2.f, (float)iw, (float)ih), it.icon);
            x += iw + style::px(ic, "margin-right", 4);
        }
        // (an item's own colour, unless it is selected or disabled)
        bool plain = !selected_[row] && it.enabled && isEnabled();
        Color tc = it.hasColor && plain ? it.color : style::foreground(item);
        style::text(p, Rect(x, c.y, std::max(0, c.right() - x), c.h), AlignLeft | AlignVCenter, it.text,
                    parts.font(item, f), tc, true);
    }
    if (clip) p.restore();
}

bool ListWidget::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton && e.button != plat::RightButton) return false;
    int row = rowAt(e.pos);
    pressRow_ = row;
    if (row < 0) {
        if (multi_ && e.button == plat::LeftButton && !e.ctrl()) clearSelection();
        return true;
    }
    if (!items_[row].enabled) return true;
    if (e.button == plat::LeftButton && items_[row].checkable && checkRect(row).contains(e.pos)) {
        items_[row].checked = !items_[row].checked;
        update();
        itemChecked.emit(row, items_[row].checked);
        if (!multi_) setCurrentRow(row);
        return true;
    }
    if (e.button == plat::RightButton) {
        if (!selected_[row]) select(row, 0);
        return true;
    }
    select(row, e.mods);
    return true;
}

bool ListWidget::mouseMove(MouseEvent& e) {
    int row = rowAt(e.pos);
    if (row != hoverRow_) {
        hoverRow_ = row;
        update();
    }
    if (isPressed() && (e.buttons & plat::LeftButton) && !multi_ && row >= 0 && items_[row].enabled && row != current_) {
        setCurrentRow(row);
        pressRow_ = row;
    }
    return isPressed();
}

bool ListWidget::mouseRelease(MouseEvent& e) {
    int row = rowAt(e.pos);
    if (e.button == plat::LeftButton && row >= 0 && row == pressRow_ && items_[row].enabled) itemClicked.emit(row);
    pressRow_ = -1;
    return true;
}

bool ListWidget::mouseDoubleClick(MouseEvent& e) {
    if (e.button != plat::LeftButton) return mousePress(e);
    int row = rowAt(e.pos);
    if (row < 0 || !items_[row].enabled) return true;
    if (items_[row].checkable && checkRect(row).contains(e.pos)) return mousePress(e);
    pressRow_ = -1;
    itemActivated.emit(row);
    return true;
}

bool ListWidget::wheel(WheelEvent& e) {
    int old = bar_->value();
    bar_->setValue(old - (int)std::lround(e.dy * 3 * rowHeight));
    return bar_->value() != old;
}

bool ListWidget::keyPress(KeyEvent& e) {
    int n = count();
    if (!n) return false;
    int page = std::max(1, viewRect().h / std::max(1, rowHeight) - 1);
    int row = current_;
    auto next = [&](int from, int dir, int steps) {
        int r = from, last = from;
        for (int k = 0; k < steps; ++k) {
            int j = r + dir;
            while (j >= 0 && j < n && !items_[j].enabled) j += dir;
            if (j < 0 || j >= n) break;
            r = last = j;
        }
        return last;
    };
    switch (e.key) {
    case plat::Key_Up: row = next(row < 0 ? n : row, -1, 1); break;
    case plat::Key_Down: row = next(row, 1, 1); break;
    case plat::Key_PageUp: row = next(row < 0 ? n : row, -1, page); break;
    case plat::Key_PageDown: row = next(row, 1, page); break;
    case plat::Key_Home: row = next(-1, 1, 1); break;
    case plat::Key_End: row = next(n, -1, 1); break;
    case plat::Key_Return:
    case plat::Key_Enter:
        if (current_ >= 0) {
            itemActivated.emit(current_);
            return true;
        }
        return false;
    case plat::Key_Space:
        if (current_ >= 0 && items_[current_].checkable) {
            items_[current_].checked = !items_[current_].checked;
            update();
            itemChecked.emit(current_, items_[current_].checked);
            return true;
        }
        if (current_ >= 0 && multi_) {
            select(current_, plat::ModCtrl);
            return true;
        }
        return false;
    case 'A':
        if (multi_ && e.ctrl()) {
            for (int i = 0; i < n; ++i) selected_[i] = items_[i].selectable && items_[i].enabled;
            update();
            return true;
        }
        return false;
    default:
        return false;
    }
    if (row >= 0 && row < n) {
        if (multi_) select(row, e.mods & plat::ModShift);
        else setCurrentRow(row);
    }
    return true;
}

bool ListWidget::contextMenu(MouseEvent& e) {
    if (itemContextMenu.empty()) return false;
    itemContextMenu.emit(rowAt(e.pos), e);
    return true;
}

std::string ListWidget::toolTipAt(Point p) const {
    int row = rowAt(p);
    if (row >= 0 && !items_[row].tooltip.empty()) return items_[row].tooltip;
    return toolTip();
}

// ============================================================================================
// TreeWidget
// ============================================================================================

TreeWidget::Node* TreeWidget::Node::add(const std::string& text_, const std::string& data_, bool expandable_) {
    auto n = std::make_unique<Node>();
    n->text = text_;
    n->data = data_;
    n->expandable = expandable_;
    n->parent = this;
    Node* raw = n.get();
    children.push_back(std::move(n));
    return raw;
}

TreeWidget::TreeWidget(Widget* parent) : Widget(parent) {
    setStyleType("TreeWidget");
    bar_ = add<ScrollBar>(Orientation::Vertical);
    bar_->setVisible(false);
    bar_->valueChanged.connect([this](int) { update(); });
    setFocusPolicy(FocusPolicy::Strong);
    setMouseTracking(true);
    hPolicy = vPolicy = SizePolicy::Expanding;
    root_.expanded = true;
    root_.filled = true;
}

void TreeWidget::rebuild(Node* n, int depth) {
    for (auto& c : n->children) {
        rows_.push_back({c.get(), depth});
        if (c->expanded) rebuild(c.get(), depth + 1);
    }
}

void TreeWidget::refreshRows() {
    rows_.clear();
    rebuild(&root_, 0);
    // forget nodes that are gone
    std::unordered_set<const Node*> all;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        all.insert(n);
        for (auto& c : n->children) walk(c.get());
    };
    walk(&root_);
    bool lostCurrent = current_ && !all.count(current_);
    if (hover_ && !all.count(hover_)) hover_ = nullptr;
    if (pressNode_ && !all.count(pressNode_)) pressNode_ = nullptr;
    if (lostCurrent) current_ = nullptr;
    syncBar();
    if (lostCurrent) currentChanged.emit(nullptr);
}

void TreeWidget::clear() {
    root_.children.clear();
    bool had = current_ != nullptr;
    current_ = hover_ = pressNode_ = nullptr;
    rows_.clear();
    bar_->setValue(0);
    syncBar();
    if (had) currentChanged.emit(nullptr);
}

void TreeWidget::syncBar() {
    readStyle();
    Rect in = style::contents(rect(), computedStyle());
    int total = (int)rows_.size() * rowHeight, vh = std::max(0, in.h);
    bool need = total > vh && vh > 0;
    int bw = bar_->thickness();
    bar_->setVisible(need);
    bar_->setGeometry(in.right() - bw, in.y, bw, vh);
    bar_->setPageStep(vh);
    bar_->setSingleStep(rowHeight);
    bar_->setRange(0, std::max(0, total - vh));
    update();
}

Point TreeWidget::sizeHint() const { return styledSize(computedStyle(), Point(256, 192), minimumSize(), maximumSize()); }

Point TreeWidget::minimumSizeHint() const { return styledMinimum(computedStyle(), Point(40, 40)); }

bool TreeWidget::readStyle() {
    PartStyles parts(*this);
    const css::Style& item = parts.get("item");
    int h = std::max(1, style::px(item, "height", 20));
    int in = std::max(0, style::px(parts.get("branch", {}, &item), "width", 16));
    if (h == rowHeight && in == indent) return false;
    rowHeight = h;
    indent = in;
    return true;
}

Rect TreeWidget::viewRect() const {
    Rect in = style::contents(rect(), computedStyle());
    if (bar_->isVisible()) in.w = std::max(0, in.w - bar_->thickness());
    return in;
}

void TreeWidget::resized() { syncBar(); }

void TreeWidget::setExpanded(Node* n, bool on) {
    if (!n || n == &root_) return;
    if (on && !n->expandable && n->children.empty()) return;
    if (on && !n->filled) {
        n->filled = true;
        if (onFill) onFill(n);
    }
    if (n->expanded == on) {
        refreshRows();
        return;
    }
    n->expanded = on;
    if (!on && current_) {
        for (Node* p = current_->parent; p; p = p->parent)
            if (p == n) {
                setCurrent(n);
                break;
            }
    }
    refreshRows();
}

void TreeWidget::setCurrent(Node* n) {
    if (n == current_) return;
    current_ = n;
    update();
    currentChanged.emit(n);
}

int TreeWidget::rowOf(const Node* n) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].first == n) return (int)i;
    return -1;
}

void TreeWidget::scrollToNode(Node* n) {
    if (!n) return;
    bool changed = false;
    for (Node* p = n->parent; p && p != &root_; p = p->parent)
        if (!p->expanded) {
            if (!p->filled) {
                p->filled = true;
                if (onFill) onFill(p);
            }
            p->expanded = true;
            changed = true;
        }
    if (changed) refreshRows();
    int row = rowOf(n);
    if (row < 0) return;
    int y = row * rowHeight, v = bar_->value(), vh = viewRect().h;
    if (y < v) v = y;
    else if (y + rowHeight > v + vh) v = y + rowHeight - vh;
    bar_->setValue(v);
}

TreeWidget::Node* TreeWidget::nodeAt(Point p) const {
    Rect vr = viewRect();
    if (!vr.contains(p)) return nullptr;
    int row = (p.y - vr.y + bar_->value()) / std::max(1, rowHeight);
    return row < (int)rows_.size() ? rows_[row].first : nullptr;
}

bool TreeWidget::onArrow(Point p, Node* n) const {
    int row = rowOf(n);
    if (row < 0) return false;
    std::vector<const char*> states;
    rowStates(n, states);
    PartStyles parts(*this);
    Rect vr = viewRect();
    Rect c = style::contents(Rect(vr.x, 0, vr.w, rowHeight), parts.get("item", states));
    int x = c.x + rows_[row].second * indent;
    return p.x >= x && p.x < x + indent;
}

void TreeWidget::rowStates(const Node* n, std::vector<const char*>& out) const {
    if (n == current_) out.push_back("selected");
    if (n == hover_ && isEnabled()) out.push_back("hover");
    if (n->expandable || !n->children.empty()) out.push_back("has-children");
    if (n->expanded) out.push_back("open");
    if (n->bold) out.push_back("bold");
}

void TreeWidget::paint(Painter& p) {
    // as ListWidget's: its box, then each row's box (TreeWidget::item), its arrow (::branch, whose width is the
    // indent of each level), its icon (::icon) and its text
    if (readStyle()) syncBar();
    const css::Style& st = computedStyle();
    style::box(p, rect(), st);
    Rect vr = viewRect(), inner = style::contents(rect(), st);
    bool clip = inner != rect();
    if (clip) {
        p.save();
        p.setClip(inner);
    }
    PartStyles parts(*this);
    Font f = theme::uiFont();
    std::vector<const char*> states;
    int scroll = bar_->value();
    int first = std::max(0, scroll / std::max(1, rowHeight));
    for (int row = first; row < (int)rows_.size(); ++row) {
        int y = vr.y + row * rowHeight - scroll;
        if (y >= vr.bottom()) break;
        Node* n = rows_[row].first;
        int depth = rows_[row].second;
        states.clear();
        rowStates(n, states);
        const css::Style& item = parts.get("item", states);
        Rect r(vr.x, y, vr.w, rowHeight);
        style::box(p, r, item);
        Rect c = style::contents(r, item);
        int x = c.x + depth * indent;
        if (n->expandable || !n->children.empty()) {
            const css::Style& br = parts.get("branch", states, &item);
            style::Insets pad = style::insets(br, "padding");
            float size = (float)std::max(0, indent - pad.w());
            style::icon(p, (float)(x + pad.left), c.y + (c.h - size) / 2.f, size, n->expanded ? "down" : "right",
                        style::foreground(br));
        }
        x += indent;
        if (!n->icon.empty()) {
            const css::Style& ic = parts.get("icon", states, &item);
            int iw = std::max(0, style::px(ic, "width", 16)), ih = std::max(0, style::px(ic, "height", 16));
            x += style::px(ic, "margin-left");
            if (n->icon.width() <= iw && n->icon.height() <= ih)
                p.drawImage(Point(x, c.y + (c.h - n->icon.height()) / 2), n->icon);
            else
                p.drawImage(RectF((float)x, c.y + (c.h - ih) / 2.f, (float)iw, (float)ih), n->icon);
            x += iw + style::px(ic, "margin-right", 4);
        }
        style::text(p, Rect(x, c.y, std::max(0, c.right() - x), c.h), AlignLeft | AlignVCenter, n->text,
                    parts.font(item, f), style::foreground(item), true);
    }
    if (clip) p.restore();
}

bool TreeWidget::mousePress(MouseEvent& e) {
    if (e.button != plat::LeftButton && e.button != plat::RightButton) return false;
    Node* n = nodeAt(e.pos);
    if (!n) return true;
    if (e.button == plat::LeftButton && (n->expandable || !n->children.empty()) && onArrow(e.pos, n)) {
        setExpanded(n, !n->expanded);
        return true;
    }
    setCurrent(n);
    nodePressed.emit(n, e);
    if (e.button == plat::LeftButton) {
        pressNode_ = n;
        pressPos_ = e.pos;
    }
    return true;
}

bool TreeWidget::mouseMove(MouseEvent& e) {
    Node* n = nodeAt(e.pos);
    if (n != hover_) {
        hover_ = n;
        update();
    }
    if (pressNode_ && (e.buttons & plat::LeftButton) && dragDataFor) {
        int d = std::abs(e.pos.x - pressPos_.x) + std::abs(e.pos.y - pressPos_.y);
        if (d >= 10) {
            Node* src = pressNode_;
            pressNode_ = nullptr;
            DragData data = dragDataFor(src);
            if (!data.files.empty() || !data.text.empty() || !data.kind.empty()) startDrag(data);
        }
    }
    return isPressed();
}

bool TreeWidget::mouseRelease(MouseEvent& e) {
    pressNode_ = nullptr;
    return true;
}

bool TreeWidget::mouseDoubleClick(MouseEvent& e) {
    if (e.button != plat::LeftButton) return mousePress(e);
    Node* n = nodeAt(e.pos);
    if (!n) return true;
    bool branch = n->expandable || !n->children.empty();
    if (branch && onArrow(e.pos, n)) {
        setExpanded(n, !n->expanded);
        return true;
    }
    if (branch) setExpanded(n, !n->expanded);
    activated.emit(n);
    return true;
}

bool TreeWidget::wheel(WheelEvent& e) {
    int old = bar_->value();
    bar_->setValue(old - (int)std::lround(e.dy * 3 * rowHeight));
    return bar_->value() != old;
}

bool TreeWidget::keyPress(KeyEvent& e) {
    int n = (int)rows_.size();
    if (!n) return false;
    int row = current_ ? rowOf(current_) : -1;
    int page = std::max(1, viewRect().h / std::max(1, rowHeight) - 1);
    switch (e.key) {
    case plat::Key_Up: row = row < 0 ? 0 : std::max(0, row - 1); break;
    case plat::Key_Down: row = std::min(n - 1, row + 1); break;
    case plat::Key_PageUp: row = std::max(0, row - page); break;
    case plat::Key_PageDown: row = std::min(n - 1, std::max(0, row) + page); break;
    case plat::Key_Home: row = 0; break;
    case plat::Key_End: row = n - 1; break;
    case plat::Key_Right:
        if (current_) {
            if ((current_->expandable || !current_->children.empty()) && !current_->expanded) {
                setExpanded(current_, true);
                return true;
            }
            if (current_->expanded && !current_->children.empty()) row = rowOf(current_->children[0].get());
        }
        break;
    case plat::Key_Left:
        if (current_) {
            if (current_->expanded) {
                setExpanded(current_, false);
                return true;
            }
            if (current_->parent && current_->parent != &root_) row = rowOf(current_->parent);
        }
        break;
    case plat::Key_Return:
    case plat::Key_Enter:
        if (current_) {
            activated.emit(current_);
            return true;
        }
        return false;
    default:
        return false;
    }
    if (row >= 0 && row < n) {
        setCurrent(rows_[row].first);
        scrollToNode(rows_[row].first);
    }
    return true;
}

bool TreeWidget::contextMenu(MouseEvent& e) {
    if (nodeContextMenu.empty()) return false;
    nodeContextMenu.emit(nodeAt(e.pos), e);
    return true;
}

std::string TreeWidget::toolTipAt(Point p) const {
    Node* n = nodeAt(p);
    if (n && !n->tooltip.empty()) return n->tooltip;
    return toolTip();
}

}  // namespace ui
