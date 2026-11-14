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
    : Window(makeOptions(parent, title, w, h)), parent_(parent) {}

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
}  // namespace

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
    vPolicy = SizePolicy::Fixed;
    hbox(this, 0, 6);
    static const StandardButton all[] = {Ok, Save, Yes, Discard, No, Cancel, Close, Apply, Reset};
    bool haveDefault = false;
    for (StandardButton sb : all) {
        if (!(buttons & sb)) continue;
        Button* b = add<Button>(standardText(sb, buttons));
        b->setMinimumWidth(80);
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
    b->setMinimumWidth(80);
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

Point ButtonBox::sizeHint() const { return layout() ? layout()->sizeHint() : Point(0, 0); }

// ============================================================================================
// message boxes
// ============================================================================================

namespace {
class MsgIcon : public Widget {
public:
    explicit MsgIcon(msg::Icon i) : icon(i) { setFixedSize(32, 32); }
    msg::Icon icon;
    void paint(Painter& p) override {
        Color bg = theme::c("accent");
        if (icon == msg::Icon::Warning) bg = theme::c("solo");
        else if (icon == msg::Icon::Critical) bg = theme::c("mute");
        Color fg = theme::c("accent_text");
        p.fillEllipse(RectF(1, 1, 30, 30), bg);
        switch (icon) {
        case msg::Icon::Information:
            p.fillEllipse(RectF(14, 7, 4, 4), fg);
            p.fillRoundedRect(RectF(14.25f, 13, 3.5f, 12), 1.5f, fg);
            break;
        case msg::Icon::Warning:
            p.fillRoundedRect(RectF(14.25f, 6.5f, 3.5f, 13), 1.5f, fg);
            p.fillEllipse(RectF(14, 21.5f, 4, 4), fg);
            break;
        case msg::Icon::Question: {
            Path path;
            path.moveTo(11.5f, 12.5f);
            path.cubicTo(11.5f, 9.2f, 13.6f, 7.5f, 16, 7.5f);
            path.cubicTo(18.6f, 7.5f, 20.5f, 9.3f, 20.5f, 11.6f);
            path.cubicTo(20.5f, 14.6f, 16, 15.2f, 16, 18.5f);
            p.strokePath(path, fg, 3.2f, LineCap::Round, LineJoin::Round);
            p.fillEllipse(RectF(14, 21.5f, 4, 4), fg);
            break;
        }
        case msg::Icon::Critical:
            p.drawLine(PointF(11, 11), PointF(21, 21), fg, 3.2f, LineCap::Round);
            p.drawLine(PointF(21, 11), PointF(11, 21), fg, 3.2f, LineCap::Round);
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
    d.setMinimumWindowSize(0, 0);
    auto* outer = vbox(&d, 14, 14);
    auto row = std::make_unique<HBox>();
    row->spacing = 14;
    HBox* rowp = row.get();
    outer->addLayout(std::move(row));
    if (icon != Icon::None) {
        auto* ic = d.add<MsgIcon>(icon);
        rowp->addWidget(ic, 0, AlignTop);
    }
    auto col = std::make_unique<VBox>();
    col->spacing = 8;
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

bool getText(Window* parent, const std::string& title, const std::string& label, std::string& text) {
    Dialog d(parent, title, 320, 120);
    auto* v = vbox(&d, 12, 8);
    auto* lab = d.add<Label>(label);
    v->addWidget(lab);
    auto* edit = d.add<LineEdit>(text);
    edit->setMinimumWidth(280);
    v->addWidget(edit);
    v->addSpacing(4);
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
    auto* v = vbox(&d, 12, 8);
    auto* lab = d.add<Label>(label);
    v->addWidget(lab);
    auto* combo = d.add<ComboBox>();
    for (auto& it : items) combo->addItem(it);
    combo->setCurrentIndexSilently(std::max(0, std::min(index, (int)items.size() - 1)));
    combo->setMinimumWidth(280);
    v->addWidget(combo);
    v->addSpacing(4);
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
class ProgressBar : public Widget {
public:
    std::function<void(Painter&)> draw;
    ProgressBar() {
        setFixedHeight(20);
        hPolicy = SizePolicy::Expanding;
    }
    Point sizeHint() const override { return Point(200, 20); }
    void paint(Painter& p) override {
        if (draw) draw(p);
    }
};
}  // namespace

ProgressDialog::ProgressDialog(Window* parent, const std::string& title, const std::string& label, bool cancellable)
    : Dialog(parent, title, 380, 130) {
    auto* v = vbox(this, 12, 10);
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
        cancel_->setMinimumWidth(80);
        r->addWidget(cancel_);
        cancel_->clicked.connect([this] { cancel(); });
    }
    setMinimumWindowSize(320, 0);
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
    const theme::Shades& sh = theme::shades();
    Rect r = bar_->rect();
    float rad = (float)theme::radius();
    style::frame(p, r, theme::c("base"), sh.fieldEdge, rad);
    Rect in = r.adjusted(1, 1, -1, -1);
    Font f = theme::uiFont();
    if (hi_ <= lo_) {
        // busy: a block going to and fro
        double t = App::get().now();
        int bw = std::max(20, in.w / 4);
        double ph = std::fmod(t * 0.8, 2.0);
        double x = ph < 1 ? ph : 2 - ph;
        Rect chunk(in.x + (int)std::lround(x * (in.w - bw)), in.y, bw, in.h);
        style::fill(p, chunk, theme::c("accent"), std::max(0.f, rad - 1));
        return;
    }
    double frac = std::max(0.0, std::min(1.0, (double)(value_ - lo_) / (hi_ - lo_)));
    int cw = (int)std::lround(frac * in.w);
    Rect chunk(in.x, in.y, cw, in.h);
    if (cw > 0) style::fill(p, chunk, theme::c("accent"), std::max(0.f, rad - 1));
    std::string pct = std::to_string((int)std::lround(frac * 100)) + "%";
    style::text(p, r, AlignCenter, pct, f, theme::c("text"));
    if (cw > 0) {
        p.save();
        p.setClip(chunk);
        style::text(p, r, AlignCenter, pct, f, theme::c("accent_text"));
        p.restore();
    }
}

}  // namespace ui
