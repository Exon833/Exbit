// Dialog, ButtonBox, message boxes, getText / getItem, ProgressDialog.
#include "ui/toolkit/dialogs.h"

#include <algorithm>
#include <cmath>

#include "ui/toolkit/style.h"

namespace ui {

// ============================================================================================
// Dialog
// ============================================================================================

plat::WindowOptions Dialog::makeOptions(Window* parent, const std::string& title, int w, int h) {
    plat::WindowOptions o;
    o.title = title.empty() ? std::string("Tiny DAW") : title;
    o.w = w;
    o.h = h;
    o.dialog = true;
    o.transientFor = parent ? parent->native() : nullptr;
    return o;
}

Dialog::Dialog(Window* parent, const std::string& title, int w, int h)
    : Window(makeOptions(parent, title, w, h)), parent_(parent) {
    setStyleType("Dialog");
}

void Dialog::place() {
    if (placed_) return;
    placed_ = true;
    centerOn(parent_ && parent_->isWindowShown() ? parent_ : App::get().mainWindow);
}

int Dialog::exec() {
    setModal(true);
    result_ = Rejected;
    running_ = true;
    place();
    showWindow();
    raiseWindow();
    App::get().runUntil([this] { return !running_; });
    return result_;
}

void Dialog::open() {
    running_ = true;
    place();
    showWindow();
    raiseWindow();
}

void Dialog::done(int r) {
    result_ = r;
    bool was = running_;
    running_ = false;
    hideWindow();
    (void)was;
    finished.emit(r);
    closed.emit();
    if (deleteOnClose) deleteLater();
}

void Dialog::close() {
    if (closeRequested && !closeRequested()) return;
    reject();
}

void Dialog::adjustSize() {
    App::get().doLayouts();
    Point h = layout() ? layout()->sizeHint() : sizeHint();
    Point mn = layout() ? layout()->minimumSize() : Point(0, 0);
    Point m = minimumSize();
    int w = std::max({h.x, mn.x, m.x, 120});
    int hh = std::max({h.y, mn.y, m.y, 40});
    resizeWindow(w, hh);
}

bool Dialog::keyPress(KeyEvent& e) {
    bool plain = !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModMeta));
    if (e.key == plat::Key_Escape && plain) {
        close();
        return true;
    }
    if ((e.key == plat::Key_Return || e.key == plat::Key_Enter) && !(e.mods & (plat::ModAlt | plat::ModMeta))) {
        // the default button (a focused push button took the key already)
        Button* def = nullptr;
        std::function<void(Widget*)> find = [&](Widget* w) {
            if (def || !w->isVisible()) return;
            if (auto* b = dynamic_cast<Button*>(w))
                if (b->isDefault() && b->isEnabled()) {
                    def = b;
                    return;
                }
            for (auto& c : w->children()) find(c.get());
        };
        find(this);
        if (def) {
            def->click();
            return true;
        }
        return false;
    }
    return false;
}

// ============================================================================================
// ButtonBox
// ============================================================================================

namespace {
// Windows / KDE order: Reset | stretch | Yes, Accept, Destructive, No, Action, Reject, Apply
int roleOrder(int sb) {
    switch (sb) {
    case ButtonBox::Reset: return 0;
    case ButtonBox::Yes: return 1;
    case ButtonBox::Ok: return 2;
    case ButtonBox::Save: return 2;
    case ButtonBox::Discard: return 3;
    case ButtonBox::No: return 4;
    case ButtonBox::Cancel: return 6;
    case ButtonBox::Close: return 6;
    case ButtonBox::Apply: return 7;
    default: return 5;
    }
}

// the gap between a ButtonBox's buttons (default.css: ButtonBox's column-gap, 6 px)
int buttonGap(const Widget* box) { return std::max(0, style::px(box->computedStyle(), "column-gap", 6)); }

// a dialog's button: at least min-width wide (default.css: ButtonBox > Button, 80 px)
void setButtonWidth(Button* b) { b->setMinimumWidth(std::max(0, style::px(b->computedStyle(), "min-width", 80))); }
}  // namespace

void dialogButton(Button* b) {
    b->addStyleClass("dialog-button");
    setButtonWidth(b);
}

std::string ButtonBox::standardText(StandardButton b, int all) {
    switch (b) {
    case Ok: return "OK";
    case Cancel: return "Cancel";
    case Close: return "Close";
    case Save: return "Save";
    case Discard: return (all & Save) ? "Don't Save" : "Discard";
    case Yes: return "Yes";
    case No: return "No";
    case Apply: return "Apply";
    case Reset: return "Reset";
    }
    return "";
}

ButtonBox::ButtonBox(int buttons, Widget* parent) : Widget(parent) {
    setStyleType("ButtonBox");
    vPolicy = SizePolicy::Fixed;
    hbox(this, 0, buttonGap(this));
    static const StandardButton all[] = {Ok, Save, Yes, Discard, No, Cancel, Close, Apply, Reset};
    bool haveDefault = false;
    for (StandardButton sb : all) {
        if (!(buttons & sb)) continue;
        Button* b = add<Button>(standardText(sb, buttons));
        setButtonWidth(b);
        buttons_.push_back({sb, b});
        roles_.push_back(roleOrder(sb));
        int code = sb;
        b->clicked.connect([this, code] {
            clicked.emit(code);
            if (code == Ok || code == Save || code == Yes) accepted.emit();
            else if (code == Cancel || code == Close || code == No) rejected.emit();
        });
        if (!haveDefault && (sb == Ok || sb == Save || sb == Yes)) {
            b->setDefault(true);
            haveDefault = true;
        }
    }
    arrange();
}

void ButtonBox::arrange() {
    auto* l = dynamic_cast<BoxLayout*>(layout());
    if (!l) return;
    l->clear();
    std::vector<size_t> order(buttons_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) { return roles_[a] < roles_[b]; });
    for (size_t i : order)
        if (roles_[i] == 0) l->addWidget(buttons_[i].second);
    l->addStretch(1);
    for (size_t i : order)
        if (roles_[i] != 0) l->addWidget(buttons_[i].second);
}

Button* ButtonBox::button(StandardButton sb) const {
    for (auto& p : buttons_)
        if (p.first == sb) return p.second;
    return nullptr;
}

Button* ButtonBox::addButton(const std::string& text, int role) {
    Button* b = add<Button>(text);
    setButtonWidth(b);
    buttons_.push_back({0, b});
    roles_.push_back(role == 1 ? 2 : role == 0 ? 6 : 5);
    b->clicked.connect([this, role] {
        clicked.emit(0);
        if (role == 1) accepted.emit();
        else if (role == 0) rejected.emit();
    });
    if (role == 1) {
        bool any = false;
        for (auto& p : buttons_)
            if (p.second != b && p.second->isDefault()) any = true;
        if (!any) b->setDefault(true);
    }
    arrange();
    return b;
}

Point ButtonBox::sizeHint() const {
    if (!layout()) return Point(0, 0);
    // (the gap as the stylesheets have it now: layouts ask for this before placing the buttons)
    if (auto* l = dynamic_cast<BoxLayout*>(layout())) l->spacing = buttonGap(this);
    return layout()->sizeHint();
}

// ============================================================================================
// message boxes
// ============================================================================================

namespace {
// the round sign of a message box (style type MessageIcon, with .information / .warning / .critical / .question: the
// circle's background-color, the sign in `color`; width / height: its size, the drawing (made for 32 px) scaled to it)
class MsgIcon : public Widget {
public:
    explicit MsgIcon(msg::Icon i) : icon(i) {
        setStyleType("MessageIcon");
        addStyleClass(i == msg::Icon::Warning ? "warning" : i == msg::Icon::Critical ? "critical"
                      : i == msg::Icon::Question ? "question" : "information");
        fitStyle();
    }
    msg::Icon icon;
    // its size from its style (again on a skin change: MainWindow::restyle and App::repaintAll call resized())
    void fitStyle() {
        const css::Style& st = computedStyle();
        Point size(std::max(1, style::px(st, "width", 32)), std::max(1, style::px(st, "height", 32)));
        if (minimumSize() != size || maximumSize() != size) setFixedSize(size.x, size.y);
    }
    void resized() override { fitStyle(); }
    void paint(Painter& p) override {
        const css::Style& st = computedStyle();
        if (style::glyph(p, RectF(rect()), st)) return;  // (an image of the whole icon)
        Color bg = style::background(st);
        Color fg = style::foreground(st);
        // (the drawing is made for 32 px: scaled to its size, centred in it when it isn't square)
        float side = (float)std::min(width(), height()), s = side / 32.f;
        float ox = (width() - side) / 2.f, oy = (height() - side) / 2.f;
        auto at = [&](float x, float y) { return PointF(ox + x * s, oy + y * s); };
        auto box = [&](float x, float y, float w, float h) { return RectF(ox + x * s, oy + y * s, w * s, h * s); };
        p.fillEllipse(box(1, 1, 30, 30), bg);
        switch (icon) {
        case msg::Icon::Information:
            p.fillEllipse(box(14, 7, 4, 4), fg);
            p.fillRoundedRect(box(14.25f, 13, 3.5f, 12), 1.5f * s, fg);
            break;
        case msg::Icon::Warning:
            p.fillRoundedRect(box(14.25f, 6.5f, 3.5f, 13), 1.5f * s, fg);
            p.fillEllipse(box(14, 21.5f, 4, 4), fg);
            break;
        case msg::Icon::Question: {
            Path path;
            auto curve = [&](PointF c1, PointF c2, PointF e) { path.cubicTo(c1.x, c1.y, c2.x, c2.y, e.x, e.y); };
            PointF start = at(11.5f, 12.5f);
            path.moveTo(start.x, start.y);
            curve(at(11.5f, 9.2f), at(13.6f, 7.5f), at(16, 7.5f));
            curve(at(18.6f, 7.5f), at(20.5f, 9.3f), at(20.5f, 11.6f));
            curve(at(20.5f, 14.6f), at(16, 15.2f), at(16, 18.5f));
            p.strokePath(path, fg, 3.2f * s, LineCap::Round, LineJoin::Round);
            p.fillEllipse(box(14, 21.5f, 4, 4), fg);
            break;
        }
        case msg::Icon::Critical:
            p.drawLine(at(11, 11), at(21, 21), fg, 3.2f * s, LineCap::Round);
            p.drawLine(at(21, 11), at(11, 21), fg, 3.2f * s, LineCap::Round);
            break;
        default:
            break;
        }
    }
};

int toStandard(int mb) {
    int s = 0;
    if (mb & msg::Ok) s |= ButtonBox::Ok;
    if (mb & msg::Cancel) s |= ButtonBox::Cancel;
    if (mb & msg::Yes) s |= ButtonBox::Yes;
    if (mb & msg::No) s |= ButtonBox::No;
    if (mb & msg::Save) s |= ButtonBox::Save;
    if (mb & msg::Discard) s |= ButtonBox::Discard;
    if (mb & msg::Close) s |= ButtonBox::Close;
    return s;
}

int fromStandard(int sb) {
    switch (sb) {
    case ButtonBox::Ok: return msg::Ok;
    case ButtonBox::Cancel: return msg::Cancel;
    case ButtonBox::Yes: return msg::Yes;
    case ButtonBox::No: return msg::No;
    case ButtonBox::Save: return msg::Save;
    case ButtonBox::Discard: return msg::Discard;
    case ButtonBox::Close: return msg::Close;
    default: return 0;
    }
}
}  // namespace

namespace msg {
int box(Window* parent, Icon icon, const std::string& title, const std::string& text, int buttons, int defaultButton,
        const std::string& details) {
    if (!buttons) buttons = Ok;
    Dialog d(parent, title, 400, 150);
    d.addStyleClass("message-box");
    d.setMinimumWindowSize(0, 0);
    // (default.css's sizes: 14 px around and between the parts, 8 px between the text and its details)
    auto* outer = vbox(&d, theme::metric("message-box-margin", 14), theme::metric("message-box-spacing", 14));
    auto row = std::make_unique<HBox>();
    row->spacing = theme::metric("message-box-spacing", 14);
    HBox* rowp = row.get();
    outer->addLayout(std::move(row));
    if (icon != Icon::None) {
        auto* ic = d.add<MsgIcon>(icon);
        rowp->addWidget(ic, 0, AlignTop);
    }
    auto col = std::make_unique<VBox>();
    col->spacing = theme::metric("message-box-text-spacing", 8);
    VBox* colp = col.get();
    rowp->addLayout(std::move(col), 1);
    auto* lab = d.add<Label>(text);
    lab->setWordWrap(true);
    lab->setAlignment(AlignLeft | AlignTop);
    colp->addWidget(lab);
    if (!details.empty()) {
        auto* det = d.add<Label>(details);
        det->setWordWrap(true);
        det->setAlignment(AlignLeft | AlignTop);
        det->setProperty("hint", "true");
        colp->addWidget(det);
    }
    colp->addStretch(1);
    auto* bb = d.add<ButtonBox>(toStandard(buttons));
    outer->addWidget(bb);
    int escape = (buttons & Cancel) ? Cancel : (buttons & No) ? No : (buttons & Close) ? Close : (buttons & Ok) ? Ok : 0;
    if (!escape) escape = (buttons & Discard) ? Discard : (buttons & Yes) ? Yes : Save;
    int pressed = escape;
    bb->clicked.connect([&](int sb) {
        pressed = fromStandard(sb);
        d.accept();
    });
    if (defaultButton) {
        for (int sb : {ButtonBox::Ok, ButtonBox::Cancel, ButtonBox::Yes, ButtonBox::No, ButtonBox::Save,
                       ButtonBox::Discard, ButtonBox::Close}) {
            ui::Button* b = bb->button((ButtonBox::StandardButton)sb);
            if (b) b->setDefault(fromStandard(sb) == defaultButton);
        }
        ui::Button* b = bb->button((ButtonBox::StandardButton)toStandard(defaultButton));
        if (b) b->setFocus();
    }
    d.adjustSize();
    int r = d.exec();
    (void)r;
    return pressed;
}
}  // namespace msg

// ============================================================================================
// getText / getItem
// ============================================================================================

namespace {
// getText / getItem's column (default.css's sizes: 12 px around, 8 px between)
VBox* inputDialogLayout(Dialog& d) {
    return vbox(&d, theme::metric("input-dialog-margin", 12), theme::metric("input-dialog-spacing", 8));
}
}  // namespace

bool getText(Window* parent, const std::string& title, const std::string& label, std::string& text) {
    Dialog d(parent, title, 320, 120);
    d.addStyleClass("input-dialog");
    auto* v = inputDialogLayout(d);
    auto* lab = d.add<Label>(label);
    v->addWidget(lab);
    auto* edit = d.add<LineEdit>(text);
    edit->setMinimumWidth(theme::metric("input-dialog-field-width", 280));
    v->addWidget(edit);
    v->addSpacing(theme::metric("input-dialog-button-gap", 4));
    auto* bb = d.add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    v->addWidget(bb);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    d.adjustSize();
    d.setFocusWidget(edit);
    edit->selectAll();
    if (d.exec() != Dialog::Accepted) return false;
    text = edit->text();
    return true;
}

bool getItem(Window* parent, const std::string& title, const std::string& label, const std::vector<std::string>& items,
             int& index) {
    Dialog d(parent, title, 320, 120);
    d.addStyleClass("input-dialog");
    auto* v = inputDialogLayout(d);
    auto* lab = d.add<Label>(label);
    v->addWidget(lab);
    auto* combo = d.add<ComboBox>();
    for (auto& it : items) combo->addItem(it);
    combo->setCurrentIndexSilently(std::max(0, std::min(index, (int)items.size() - 1)));
    combo->setMinimumWidth(theme::metric("input-dialog-field-width", 280));
    v->addWidget(combo);
    v->addSpacing(theme::metric("input-dialog-button-gap", 4));
    auto* bb = d.add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    v->addWidget(bb);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    d.adjustSize();
    d.setFocusWidget(combo);
    if (d.exec() != Dialog::Accepted || items.empty()) return false;
    index = combo->currentIndex();
    return index >= 0;
}

// ============================================================================================
// ProgressDialog
// ============================================================================================

namespace {
// (style type ProgressBar: a box (background-color, border, border-radius, padding) with the percentage in `color`;
// ::chunk the part done (or the busy block: its min-width), a box, the percentage over it in its `color`; its
// width and height: what it asks for)
class ProgressBar : public Widget {
public:
    std::function<void(Painter&)> draw;
    ProgressBar() {
        setStyleType("ProgressBar");
        setFixedHeight(std::max(1, style::px(computedStyle(), "height", 20)));
        hPolicy = SizePolicy::Expanding;
    }
    Point sizeHint() const override {
        const css::Style& st = computedStyle();
        return Point(style::px(st, "width", 200), style::px(st, "height", 20));
    }
    void paint(Painter& p) override {
        if (draw) draw(p);
    }
};
}  // namespace

ProgressDialog::ProgressDialog(Window* parent, const std::string& title, const std::string& label, bool cancellable)
    : Dialog(parent, title, 380, 130) {
    setStyleType("ProgressDialog");
    // (default.css's sizes: 12 px around, 10 px between; at least 320 px wide)
    auto* v = vbox(this, theme::metric("progress-dialog-margin", 12), theme::metric("progress-dialog-spacing", 10));
    label_ = add<Label>(label);
    label_->setWordWrap(true);
    v->addWidget(label_);
    auto* bar = add<ProgressBar>();
    bar->draw = [this](Painter& p) { paintBar(p); };
    bar_ = bar;
    v->addWidget(bar);
    if (cancellable) {
        auto row = std::make_unique<HBox>();
        HBox* r = row.get();
        v->addLayout(std::move(row));
        r->addStretch(1);
        cancel_ = add<Button>("Cancel");
        setButtonWidth(cancel_);
        r->addWidget(cancel_);
        cancel_->clicked.connect([this] { cancel(); });
    }
    setMinimumWindowSize(theme::metric("progress-dialog-min-width", 320), 0);
    adjustSize();
    frameId_ = App::get().onFrame([this] {
        if (isWindowShown() && hi_ <= lo_) bar_->update();
    });
}

ProgressDialog::~ProgressDialog() {
    if (frameId_ >= 0) App::get().removeFrame(frameId_);
}

void ProgressDialog::setLabel(const std::string& t) {
    label_->setText(t);
}

void ProgressDialog::setRange(int lo, int hi) {
    lo_ = lo;
    hi_ = std::max(lo, hi);
    bar_->update();
}

void ProgressDialog::setValue(int v) {
    value_ = v;
    if (!isWindowShown() && !canceled_ && !(hi_ > lo_ && v >= hi_)) {
        setModal(true);
        open();
    }
    bar_->update();
    if (hi_ > lo_ && v >= hi_ && isWindowShown()) {
        hideWindow();
        return;
    }
    App::get().processEvents();
}

void ProgressDialog::cancel() {
    if (canceled_) return;
    canceled_ = true;
    canceled.emit();
    done(Rejected);
}

bool ProgressDialog::keyPress(KeyEvent& e) {
    if (e.key == plat::Key_Escape) {
        if (cancel_) cancel();
        return true;
    }
    return Dialog::keyPress(e);
}

void ProgressDialog::paint(Painter& p) {}

void ProgressDialog::paintBar(Painter& p) {
    // (default.css: a field's edge around the base colour; the chunk in the accent colour, 1 px inside it)
    const css::Style& st = bar_->computedStyle();
    Rect r = bar_->rect();
    style::box(p, r, st);
    Rect in = style::contents(r, st);
    Font f = style::font(st, theme::uiFont());
    Color textColor = style::foreground(st);
    const css::Style& cs = bar_->computedStyle("chunk");
    if (hi_ <= lo_) {
        // busy: a block going to and fro
        double t = App::get().now();
        int bw = std::max(style::px(cs, "min-width", 20), in.w / 4);
        double ph = std::fmod(t * 0.8, 2.0);
        double x = ph < 1 ? ph : 2 - ph;
        Rect chunk(in.x + (int)std::lround(x * (in.w - bw)), in.y, bw, in.h);
        style::box(p, chunk, cs);
        return;
    }
    double frac = std::max(0.0, std::min(1.0, (double)(value_ - lo_) / (hi_ - lo_)));
    int cw = (int)std::lround(frac * in.w);
    Rect chunk(in.x, in.y, cw, in.h);
    if (cw > 0) style::box(p, chunk, cs);
    std::string pct = std::to_string((int)std::lround(frac * 100)) + "%";
    style::text(p, r, AlignCenter, pct, f, textColor);
    if (cw > 0) {
        p.save();
        p.setClip(chunk);
        style::text(p, r, AlignCenter, pct, style::font(cs, f), style::foreground(cs));
        p.restore();
    }
}

}  // namespace ui
