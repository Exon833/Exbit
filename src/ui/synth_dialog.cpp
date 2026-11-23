// The built-in synth's window (synth_dialog.h).
#include "ui/synth_dialog.h"

#include <algorithm>
#include <cmath>

#include "audio/live.h"
#include "core/util.h"
#include "ui/sampler_dialog.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"

using namespace subsynth;

namespace synth_dialog {

namespace {
std::map<const Track*, SynthDialog*> gWindows;  // one per track

constexpr int PAD = 8, GAP = 8, CELL_W = 64, CELL_H = 72, TITLE_H = 18, HANDLE = 14;
const char* SOURCE_PART[SOURCES] = {"env1", "env2", "lfo"};
const Color SOURCE_COLOR[SOURCES] = {Color(240, 180, 60), Color(90, 200, 120), Color(90, 160, 240)};

bool isChoice(int p) { return info(p).kind == ParamInfo::Choice || info(p).kind == ParamInfo::Toggle; }
double angleOf(double norm) { return (225 - 270 * std::clamp(norm, 0.0, 1.0)) * M_PI / 180; }  // (0: bottom left)
Path arc(PointF c, float r, double n0, double n1) {
    Path path;
    int steps = std::max(2, (int)std::ceil(std::fabs(n1 - n0) * 40));
    for (int i = 0; i <= steps; i++) {
        double a = angleOf(n0 + (n1 - n0) * i / steps);
        float x = c.x + (float)std::cos(a) * r, y = c.y - (float)std::sin(a) * r;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    return path;
}
std::string pad(const std::string& s, size_t n) { return s.size() >= n ? s + " " : s + std::string(n - s.size(), ' '); }
std::string targetName(int p) { return std::string(info(p).section) + " " + info(p).name; }
std::string depthText(double d) { return util::format("%+.0f %%", d * 100); }
}  // namespace

// =================================================================================================
// SynthPage
// =================================================================================================

SynthPage::SynthPage() {
    setStyleType("SynthPage");
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMouseTracking(true);
    auto range = [](int a, int b) {
        std::vector<int> v;
        for (int p = a; p <= b; p++) v.push_back(p);
        return v;
    };
    sections_ = {{"Osc 1", -1, range(O1_WAVE, O1_WIDTH), {}},   {"Osc 2", -1, range(O2_WAVE, O2_WIDTH), {}},
                 {"Noise", -1, {NOISE}, {}},                    {"Unison", -1, {UNISON, SPREAD}, {}},
                 {"Filter", -1, range(F_TYPE, F_ENV), {}},      {"Env 1", ENV1, range(E1_A, E1_R), {}},
                 {"Env 2", ENV2, range(E2_A, E2_R), {}},        {"LFO", LFO, range(LFO_SHAPE, LFO_RETRIG), {}},
                 {"Voice", -1, range(GLIDE, OUTPUT), {}}};
    layoutCells();
}

const std::vector<int>& SynthPage::order() {
    static std::vector<int> o = [] {
        std::vector<int> v;
        for (int p = 0; p < PARAMS; p++) v.push_back(p);  // (the page shows them in their own order)
        return v;
    }();
    return o;
}

void SynthPage::layoutCells() {
    const int rows[][3] = {{0, 1, -1}, {2, 3, 4}, {5, 6, 7}, {8, -1, -1}};
    int y = PAD;
    for (auto& row : rows) {
        int x = PAD;
        for (int s : row) {
            if (s < 0) continue;
            Section& sec = sections_[(size_t)s];
            sec.box = Rect(x, y, (int)sec.params.size() * CELL_W + 8, TITLE_H + CELL_H + 4);
            for (size_t i = 0; i < sec.params.size(); i++)
                cells_[sec.params[i]] = Rect(x + 4 + (int)i * CELL_W, y + TITLE_H, CELL_W, CELL_H);
            x += sec.box.w + GAP;
        }
        y += TITLE_H + CELL_H + 4 + GAP;
    }
}

Point SynthPage::sizeHint() const {
    int w = 0, h = 0;
    for (auto& s : sections_) w = std::max(w, s.box.x + s.box.w), h = std::max(h, s.box.y + s.box.h);
    return Point(w + PAD, h + PAD);
}

Rect SynthPage::cellOf(int param) const {
    auto it = cells_.find(param);
    return it == cells_.end() ? Rect() : it->second;
}

Point SynthPage::knobCentre(int param) const {
    Rect c = cellOf(param);
    return Point(c.x + c.w / 2, c.y + 4 + 20);
}

Rect SynthPage::handleOf(int source) const {
    for (auto& s : sections_)
        if (s.source == source) return Rect(s.box.x + s.box.w - HANDLE - 4, s.box.y + 2, HANDLE, HANDLE);
    return Rect();
}

int SynthPage::paramAt(Point p) const {
    for (auto& kv : cells_)
        if (kv.second.contains(p)) return kv.first;
    return -1;
}

int SynthPage::ringAt(Point p, int& param) const {
    param = -1;
    int best = -1;
    double bestD = 2.0;  // (pixels from a ring's line, at most)
    for (auto& kv : cells_) {
        if (isChoice(kv.first)) continue;
        Point c = knobCentre(kv.first);
        double d = std::hypot(p.x - c.x, p.y - c.y);
        for (int s = 0; s < SOURCES; s++)
            if (patch.modOf(s, kv.first) && std::fabs(d - ringRadius(s)) < bestD)
                best = s, param = kv.first, bestD = std::fabs(d - ringRadius(s));
    }
    if (best < 0) param = -1;
    return best;
}

void SynthPage::set(int param, double value, bool final) {
    double v = clampValue(param, value);
    if (v == patch[param] && !final) return;
    patch.v[param] = v;
    update();
    if (changed) changed(patch, final);
}

void SynthPage::setDepth(int source, int target, double depth, bool final) {
    patch.setMod(source, target, depth);
    update();
    if (changed) changed(patch, final);
}

void SynthPage::paint(Painter& p) {
    // (a computed style is only good until the next one is asked for: what is needed of each, taken at once)
    Color text = ui::style::foreground(computedStyle());
    Font font = ui::style::font(computedStyle(), theme::uiFont());
    Font titleFont = ui::style::font(computedStyle("title"), font);
    Color titleColor = ui::style::foreground(computedStyle("title"));
    Color knobFill = ui::style::color(computedStyle("knob"), "background-color", theme::c("button"));
    Color pointerColor = ui::style::foreground(computedStyle("pointer"));
    float pointerW = (float)computedStyle("pointer").length("stroke-width", 2);
    Color arcColor = ui::style::color(computedStyle("arc"), "color", theme::c("highlight"));
    float arcW = (float)computedStyle("arc").length("stroke-width", 2);
    Color currentColor = ui::style::color(computedStyle("current"), "color", theme::c("highlight"));
    Color target = ui::style::color(computedStyle("target"), "color", theme::c("highlight"));
    Color choiceText = ui::style::foreground(computedStyle("choice"));
    Color handleFill[SOURCES], ringColor[SOURCES];
    float ringW[SOURCES];
    Color plus = ui::style::color(computedStyle("handle"), "color", theme::c("window"));
    for (int s = 0; s < SOURCES; s++) {
        ringColor[s] = ui::style::color(computedStyle(SOURCE_PART[s]), "color", SOURCE_COLOR[s]);
        ringW[s] = (float)computedStyle(SOURCE_PART[s]).length("stroke-width", 2.5);
        handleFill[s] = ui::style::color(computedStyle("handle"), "background-color", ringColor[s]);
    }
    int focusParam = hasFocus() ? order()[(size_t)std::clamp(current, 0, (int)order().size() - 1)] : -1;
    for (auto& sec : sections_) {
        ui::style::box(p, sec.box, computedStyle("section"));
        ui::style::text(p, Rect(sec.box.x + 6, sec.box.y, sec.box.w - 12, TITLE_H), AlignLeft | AlignVCenter, sec.title,
                        titleFont, titleColor);
        if (sec.source >= 0) {  // its ⊕: drag it onto a knob
            Rect h = handleOf(sec.source);
            RectF hr(h);
            p.fillEllipse(hr, handleFill[sec.source]);
            float cx = hr.x + hr.w / 2, cy = hr.y + hr.h / 2;
            p.drawLine(PointF(cx - 4, cy), PointF(cx + 4, cy), plus, 1.5f);
            p.drawLine(PointF(cx, cy - 4), PointF(cx, cy + 4), plus, 1.5f);
        }
    }
    for (auto& kv : cells_) {
        int param = kv.first;
        Rect cell = kv.second;
        const ParamInfo& in = info(param);
        Point c = knobCentre(param);
        PointF cf((float)c.x, (float)c.y);
        int r = knobRadius();
        if (isChoice(param)) {
            Rect box(cell.x + 3, c.y - 11, cell.w - 6, 22);
            ui::style::box(p, box, computedStyle("choice"));
            ui::style::text(p, box, AlignCenter, valueText(param, patch[param]), font, choiceText, true);
        } else {
            double n = toNorm(param, patch[param]);
            p.fillEllipse(RectF((float)(c.x - r), (float)(c.y - r), (float)(2 * r), (float)(2 * r)), knobFill);
            double from = info(param).lo < 0 && info(param).hi > 0 ? toNorm(param, 0) : 0.0;
            p.strokePath(arc(cf, (float)r - 2, from, n), arcColor, arcW);
            double a = angleOf(n);
            p.drawLine(cf, PointF(cf.x + (float)std::cos(a) * (r - 3), cf.y - (float)std::sin(a) * (r - 3)),
                       pointerColor, pointerW);
            for (int s = 0; s < SOURCES; s++) {  // its modulation: a ring per source, from the knob's value
                const Mod* m = patch.modOf(s, param);
                if (!m) continue;
                float rr = (float)ringRadius(s);
                Color col = ringColor[s];
                Color faint = col;
                faint.a = 70;
                p.strokeEllipse(RectF(cf.x - rr, cf.y - rr, 2 * rr, 2 * rr), faint, 1);
                p.strokePath(arc(cf, rr, n, n + m->depth), col, ringW[s]);
            }
            if (drag_ == Drag::Handle && in.mod && param == paramAt(mouse_))
                p.strokeEllipse(RectF(cf.x - r - 2, cf.y - r - 2, 2.0f * r + 4, 2.0f * r + 4), target, 2);
        }
        if (param == focusParam) p.drawRect(cell.adjusted(1, 1, -1, -1), currentColor, 1);
        ui::style::text(p, Rect(cell.x, cell.y + 46, cell.w, 13), AlignCenter, in.name, font, text, true);
        ui::style::text(p, Rect(cell.x, cell.y + 58, cell.w, 13), AlignCenter,
                        isChoice(param) ? std::string() : valueText(param, patch[param]), font, text, true);
    }
    if (drag_ == Drag::Handle) {  // the source being dragged: a line from its ⊕ to the pointer
        Point h = handleOf(dragSource_).center();
        p.drawLine(PointF((float)h.x, (float)h.y), PointF((float)mouse_.x, (float)mouse_.y),
                   ringColor[dragSource_], 2);
    }
}

bool SynthPage::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return false;
    setFocus();
    pressAt_ = mouse_ = e.pos;
    moved_ = false;
    for (int s = 0; s < SOURCES; s++)
        if (handleOf(s).adjusted(-2, -2, 2, 2).contains(e.pos)) {
            drag_ = Drag::Handle;
            dragSource_ = s;
            util::debugf("synth: dragging %s's handle\n", sourceName(s));
            update();
            return true;
        }
    int param;
    int ring = ringAt(e.pos, param);
    if (ring >= 0) {
        drag_ = Drag::Ring;
        dragSource_ = ring;
        dragParam_ = param;
        startValue_ = patch.modOf(ring, param)->depth;
        return true;
    }
    param = paramAt(e.pos);
    if (param < 0) return true;
    current = (int)(std::find(order().begin(), order().end(), param) - order().begin());
    drag_ = Drag::Knob;
    dragParam_ = param;
    startValue_ = toNorm(param, patch[param]);
    update();
    return true;
}

bool SynthPage::mouseMove(ui::MouseEvent& e) {
    mouse_ = e.pos;
    if (drag_ == Drag::None) return true;
    if (std::abs(e.pos.y - pressAt_.y) + std::abs(e.pos.x - pressAt_.x) > 2) moved_ = true;
    double by = (double)(pressAt_.y - e.pos.y) / DRAG_PIXELS / (e.shift() ? 5 : 1);
    if (drag_ == Drag::Knob && moved_ && !isChoice(dragParam_)) {
        double v = fromNorm(dragParam_, startValue_ + by);
        if (v != patch[dragParam_]) set(dragParam_, v, false);
    } else if (drag_ == Drag::Ring && moved_) {
        double d = std::clamp(startValue_ + 2 * by, -1.0, 1.0);
        if (std::fabs(d) < 0.005) d = d < 0 ? -0.005 : 0.005;  // (dragging through zero doesn't remove it)
        setDepth(dragSource_, dragParam_, d, false);
    } else if (drag_ == Drag::Handle) {
        update();
    }
    return true;
}

bool SynthPage::mouseRelease(ui::MouseEvent& e) {
    Drag was = drag_;
    drag_ = Drag::None;
    mouse_ = e.pos;
    if (was == Drag::Handle) {
        int param = paramAt(e.pos);
        if (param >= 0 && info(param).mod) {
            const Mod* m = patch.modOf(dragSource_, param);
            util::debugf("synth: %s dropped on %s: modulates it\n", sourceName(dragSource_), targetName(param).c_str());
            setDepth(dragSource_, param, m ? m->depth : MOD_START, true);
        }
        update();
    } else if (was == Drag::Knob) {
        if (!moved_ && info(dragParam_).kind == ParamInfo::Toggle) set(dragParam_, patch[dragParam_] >= 0.5 ? 0 : 1, true);
        else if (!moved_ && isChoice(dragParam_)) chooseFrom(dragParam_);
        else if (changed) changed(patch, true);
    } else if (was == Drag::Ring) {
        if (changed) changed(patch, true);
    }
    return true;
}

bool SynthPage::mouseDoubleClick(ui::MouseEvent& e) {
    int param;
    int ring = ringAt(e.pos, param);
    if (ring >= 0) return mousePress(e);
    param = paramAt(e.pos);
    if (param >= 0 && !isChoice(param)) {
        set(param, info(param).def, true);
        return true;
    }
    return mousePress(e);
}

bool SynthPage::wheel(ui::WheelEvent& e) {
    int param = paramAt(e.pos);
    if (param < 0 || e.dy == 0) return false;
    const ParamInfo& in = info(param);
    double v = in.kind == ParamInfo::Number || in.kind == ParamInfo::Log
                   ? fromNorm(param, toNorm(param, patch[param]) + (e.dy > 0 ? 0.01 : -0.01) * (e.shift() ? 0.2 : 1))
                   : patch[param] + (e.dy > 0 ? 1 : -1);
    set(param, v, true);
    return true;
}

bool SynthPage::contextMenu(ui::MouseEvent& e) {
    int param = paramAt(e.pos);
    if (param < 0 || !info(param).mod) return false;
    ui::Menu menu;
    for (int s = 0; s < SOURCES; s++)
        if (const Mod* m = patch.modOf(s, param))
            menu.addAction(std::string("Remove ") + sourceName(s) + " \xe2\x86\x92 " + targetName(param) + " (" +
                               depthText(m->depth) + ")",
                           [this, s, param] { setDepth(s, param, 0, true); });
    menu.addAction("Modulate\xe2\x80\xa6", [this, param] { if (modulate) modulate(param); });
    menu.exec(e.globalPos, this);
    return true;
}

void SynthPage::chooseFrom(int param) {
    const ParamInfo& in = info(param);
    ui::Menu menu;
    for (size_t i = 0; i < in.choices.size(); i++) {
        ui::Action* a = menu.addAction(in.choices[i], [this, param, i] { set(param, (double)i, true); });
        a->checkable = true;
        a->checked = (int)std::nearbyint(patch[param]) == (int)i;
    }
    Rect c = cellOf(param);
    menu.exec(mapToGlobal(Point(c.x, c.y + CELL_H / 2)), this);
}

bool SynthPage::keyPress(ui::KeyEvent& e) {
    if (e.mods & (plat::ModCtrl | plat::ModAlt)) return false;
    int n = (int)order().size();
    current = std::clamp(current, 0, n - 1);
    int param = order()[(size_t)current];
    const ParamInfo& in = info(param);
    bool stepped = in.kind != ParamInfo::Number && in.kind != ParamInfo::Log;
    auto nudge = [&](double steps) {
        double v = stepped ? patch[param] + steps : fromNorm(param, toNorm(param, patch[param]) + steps / 100);
        if (in.kind == ParamInfo::Choice && stepped) v = std::clamp(v, in.lo, in.hi);
        set(param, v, true);
    };
    switch (e.key) {
    case plat::Key_Up: current = std::max(0, current - 1); update(); return true;
    case plat::Key_Down: current = std::min(n - 1, current + 1); update(); return true;
    case plat::Key_Left: nudge(e.shift() ? -0.2 : -1); return true;
    case plat::Key_Right: nudge(e.shift() ? 0.2 : 1); return true;
    case plat::Key_PageDown: nudge(stepped ? -1 : -10); return true;
    case plat::Key_PageUp: nudge(stepped ? 1 : 10); return true;
    case plat::Key_Home: set(param, in.lo, true); return true;
    case plat::Key_End: set(param, in.hi, true); return true;
    case plat::Key_Backspace:
    case plat::Key_Delete: set(param, in.def, true); return true;
    case plat::Key_Return:
    case plat::Key_Enter:
    case plat::Key_Space:
        if (!isChoice(param)) return false;
        set(param, patch[param] + 1 > in.hi ? in.lo : patch[param] + 1, true);
        return true;
    case 'M':
        if (in.mod && modulate) modulate(param);
        return true;
    default: return false;
    }
}

bool SynthPage::listRows(std::vector<std::string>& rows, int& cur) const {
    for (int param : order()) {
        const ParamInfo& in = info(param);
        std::string row = pad(in.section, 7) + pad(in.name, 10) + pad(valueText(param, patch[param]), 10);
        for (int s = 0; s < SOURCES; s++)
            if (const Mod* m = patch.modOf(s, param)) row += std::string(" ~") + sourceName(s) + " " + depthText(m->depth);
        rows.push_back(row);
    }
    cur = current;
    return true;
}

std::string SynthPage::toolTipAt(Point p) const {
    for (int s = 0; s < SOURCES; s++)
        if (handleOf(s).contains(p))
            return std::string("Drag onto a knob to modulate it with ") + sourceName(s);
    int param;
    int ring = ringAt(p, param);
    if (ring >= 0)
        return std::string(sourceName(ring)) + " \xe2\x86\x92 " + targetName(param) + ": " +
               depthText(patch.modOf(ring, param)->depth) + " (drag to change; right-click to remove)";
    param = paramAt(p);
    if (param < 0) return "";
    return targetName(param) + ": " + valueText(param, patch[param]) +
           (isChoice(param) ? " (click to choose)" : " (drag up or down, Shift: finer; double-click: default)");
}

// =================================================================================================
// Modulate…
// =================================================================================================

bool modulateDialog(ui::Window* parent, Patch& patch, int target, int source) {
    ui::Dialog d(parent, "Modulate", 380, 200);
    ui::VBox* col = ui::vbox(&d, 12, 8);
    auto formOwner = std::make_unique<ui::FormLayout>();
    ui::FormLayout* form = formOwner.get();
    col->addLayout(std::move(formOwner));
    ui::ComboBox* from = d.add<ui::ComboBox>();
    for (int s = 0; s < SOURCES; s++) from->addItem(sourceName(s), s);
    from->setCurrentIndexSilently(std::max(0, source));
    form->addRow("Source", from);
    ui::ComboBox* to = d.add<ui::ComboBox>();
    std::vector<int> targets;
    for (int p = 0; p < PARAMS; p++)
        if (info(p).mod) {
            if (p == target) to->setCurrentIndexSilently((int)targets.size());
            targets.push_back(p);
            to->addItem(targetName(p), p);
        }
    if (target >= 0)
        for (size_t i = 0; i < targets.size(); i++)
            if (targets[i] == target) to->setCurrentIndexSilently((int)i);
    form->addRow("Target", to);
    ui::SpinBox* depth = d.add<ui::SpinBox>();
    depth->setRange(-100, 100);
    depth->setDecimals(1);
    depth->setSuffix(" %");
    depth->setToolTip("How far the source moves the target, of the target's whole range (0: no routing)");
    form->addRow("Depth", depth);
    auto shown = [&] {
        const Mod* m = patch.modOf(from->currentIndex(), targets[(size_t)std::max(0, to->currentIndex())]);
        depth->setValueSilently(m ? m->depth * 100 : MOD_START * 100);
    };
    shown();
    from->currentIndexChanged.connect([&](int) { shown(); });
    to->currentIndexChanged.connect([&](int) { shown(); });
    col->addStretch(1);
    ui::ButtonBox* buttons = d.add<ui::ButtonBox>(ui::ButtonBox::Ok | ui::ButtonBox::Cancel);
    buttons->accepted.connect([&] { d.accept(); });
    buttons->rejected.connect([&] { d.reject(); });
    col->addWidget(buttons);
    d.adjustSize();
    depth->setFocus();
    if (d.exec() != ui::Dialog::Accepted) return false;
    Patch before = patch;
    int s = from->currentIndex(), t = targets[(size_t)std::max(0, to->currentIndex())];
    patch.setMod(s, t, depth->value() / 100);
    util::debugf("synth: Modulate…: %s -> %s at %.1f %%\n", sourceName(s), targetName(t).c_str(), depth->value());
    return !(patch == before);
}

// =================================================================================================
// SynthDialog
// =================================================================================================

SynthDialog::SynthDialog(ui::Window* window, TrackPtr track_)
    : ui::Dialog(window, "Synth \xe2\x80\x94 " + track_->name, 780, 560), track(std::move(track_)) {
    deleteOnClose = true;
    setStyleType("SynthDialog");
    ui::VBox* col = ui::styleRoom(ui::vbox(this, 9, 6), computedStyle());
    auto top = std::make_unique<ui::HBox>();
    presetName = add<ui::Label>();
    presetName->addStyleClass("heading");
    presetsButton = ui::pushButton(this, "Presets \xe2\x96\xbe");
    presetsMenu.aboutToShow = [this](ui::Menu* m) { fillPresets(m); };
    presetsButton->setMenu(&presetsMenu);
    presetsButton->setToolTip("The presets by category, your own, and the classic voice");
    top->addWidget(presetsButton);
    saveButton = ui::pushButton(this, "Save preset\xe2\x80\xa6", [this] { savePreset(); });
    top->addWidget(saveButton);
    modulateButton = ui::pushButton(this, "Modulate\xe2\x80\xa6", [this] {
        modulateFor(page->order()[(size_t)std::clamp(page->current, 0, PARAMS - 1)]);
    });
    modulateButton->setToolTip("Route Env 1, Env 2 or the LFO to a control (or drag a \xe2\x8a\x95 onto a knob)");
    top->addWidget(modulateButton);
    top->addStretch(1);
    col->addLayout(std::move(top));
    classicNote = add<ui::Label>("This track plays the classic built-in voice (as projects made before this synth). "
                                 "Change anything, or choose a preset, to play this synth instead.");
    classicNote->setProperty("hint", "true");
    col->addWidget(classicNote);
    auto views = std::make_unique<ui::HBox>();
    synthView = ui::pushButton(this, "Synth", [this] { showModulation(false); });
    modView = ui::pushButton(this, "Modulation", [this] { showModulation(true); });
    for (ui::Button* b : {synthView, modView}) b->setCheckable(true), views->addWidget(b);
    views->addSpacing(12);
    views->addWidget(presetName, 1);
    col->addLayout(std::move(views));

    page = add<SynthPage>();
    page->changed = [this](const Patch& p, bool final) { apply(p, final); };
    page->modulate = [this](int target) { modulateFor(target); };
    page->setSizePolicy(ui::SizePolicy::Fixed, ui::SizePolicy::Fixed);
    page->setFixedSize(page->sizeHint().x, page->sizeHint().y);
    col->addWidget(page);

    modPanel = add<ui::Widget>();
    ui::VBox* mcol = ui::vbox(modPanel, 0, 6);
    modList = modPanel->add<ui::ListWidget>();
    modList->setMinimumHeight(page->sizeHint().y - 40);
    modList->itemActivated.connect([this](int) { editMod->click(); });
    mcol->addWidget(modList, 1);
    auto mrow = std::make_unique<ui::HBox>();
    addMod = ui::pushButton(modPanel, "Add\xe2\x80\xa6", [this] { modulateFor(-1); });
    editMod = ui::pushButton(modPanel, "Edit\xe2\x80\xa6", [this] {
        int row = modList->currentRow();
        if (row >= 0 && row < (int)page->patch.mods.size())
            modulateFor(page->patch.mods[(size_t)row].target, page->patch.mods[(size_t)row].source);
    });
    removeMod = ui::pushButton(modPanel, "Remove", [this] {
        int row = modList->currentRow();
        if (row < 0 || row >= (int)page->patch.mods.size()) return;
        Patch p = page->patch;
        p.mods.erase(p.mods.begin() + row);
        apply(p, true);
    });
    for (ui::Button* b : {addMod, editMod, removeMod}) mrow->addWidget(b);
    mrow->addStretch(1);
    mcol->addLayout(std::move(mrow));
    col->addWidget(modPanel, 1);

    wave = add<WaveCanvas>();
    wave->changed = [this](const std::vector<float>& w, bool final) {
        Patch p = page->patch;
        p.drawn = std::make_shared<const std::vector<float>>(w);
        apply(p, final);
    };
    wave->setVisible(false);
    col->addWidget(wave, 1);

    col->addStretch(1);
    auto buttons = std::make_unique<ui::HBox>();
    playButton = ui::pushButton(this, "Play C4", [this] { playNote(); });
    playButton->setToolTip("Hear it on the track (also: the MIDI editor's keys, or a MIDI keyboard)");
    buttons->addWidget(playButton);
    buttons->addStretch(1);
    buttons->addWidget(ui::pushButton(this, "Close", [this] { close(); }));
    col->addLayout(std::move(buttons));
    showModulation(false);
    showPatch();
    adjustSize();
    (drawnShown_ ? (ui::Widget*)wave : page)->setFocus();
}

SynthDialog::~SynthDialog() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
}

void SynthDialog::showPatch() {
    PatchPtr now = std::atomic_load(&track->synth);
    page->patch = now ? *now : Patch();
    classicNote->setVisible(!now);
    presetName->setText(now ? (now->category.empty() ? now->name : now->name + "  (" + now->category + ")")
                            : std::string("Init (classic)"));
    int row = modList->currentRow();
    modList->clear();
    for (const Mod& m : page->patch.mods)
        modList->addItem(std::string(sourceName(m.source)) + " \xe2\x86\x92 " + targetName(m.target) + "   " +
                         depthText(m.depth));
    if (modList->count()) modList->setCurrentRow(std::clamp(row, 0, modList->count() - 1));
    editMod->setEnabled(modList->count() > 0);
    removeMod->setEnabled(modList->count() > 0);
    page->update();
    // a Drawn synth: its canvas alone (the wave as the track has it, unless it is being drawn)
    bool drawn = now && now->drawn;
    if (drawn && !wave->drawing() && wave->wave != *now->drawn) {
        wave->wave = *now->drawn;
        wave->update();
    }
    if (drawn != drawnShown_) {
        drawnShown_ = drawn;
        for (ui::Widget* w : std::initializer_list<ui::Widget*>{presetsButton, saveButton, modulateButton, synthView,
                                                                 modView, presetName, playButton})
            w->setVisible(!drawn);
        wave->setVisible(drawn);
        if (drawn) {
            classicNote->setVisible(false);
            page->setVisible(false);
            modPanel->setVisible(false);
            wave->setFocus();
        } else {
            showModulation(false);
        }
        setTitle((drawn ? "Drawn synth \xe2\x80\x94 " : "Synth \xe2\x80\x94 ") + track->name);
        adjustSize();
    }
}

void SynthDialog::apply(const Patch& p, bool final) {
    PatchPtr before = std::atomic_load(&track->synth);
    if (!before || !(*before == p)) {
        std::atomic_store(&track->synth, PatchPtr(std::make_shared<const Patch>(p)));  // (replaced whole: live notes use it at once)
        util::debugf("synth: %s plays \"%s\" (%zu modulations)%s\n", track->name.c_str(), p.name.c_str(), p.mods.size(),
                     before ? "" : ", no longer the classic voice");
    }
    showPatch();
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    renderTimer_ = -1;
    if (final) {
        renderClips();  // (now: the change and its rendering one undo step)
    } else {
        renderTimer_ = ui::App::get().singleShot(RENDER_DELAY_MS, [this] {
            renderTimer_ = -1;
            renderClips();
        });
    }
}

void SynthDialog::classic() {
    if (std::atomic_load(&track->synth)) util::debugf("synth: %s back to the classic voice\n", track->name.c_str());
    std::atomic_store(&track->synth, PatchPtr());
    showPatch();
    renderClips();
}

bool SynthDialog::choosePreset(const std::string& path) {
    Patch p;
    std::string error;
    if (!load(path, p, error)) {
        ui::msg::warning(this, "Cannot load preset", error);
        return false;
    }
    apply(p, true);
    return true;
}

void SynthDialog::fillPresets(ui::Menu* menu) {
    menu->clear();
    menu->addCheckable("Init (classic)", !track->synth, [this](bool) { classic(); })->tooltip =
        "The classic built-in voice, as projects made before this synth play it";
    menu->addAction("Init", [this] { apply(Patch(), true); })->tooltip = "One saw through an open filter: a start";
    std::vector<Preset> all = presets();
    PatchPtr now = std::atomic_load(&track->synth);
    std::map<std::string, ui::Menu*> menus;
    for (int user = 0; user < 2; user++) {
        bool any = false;
        for (const Preset& pr : all) {
            if (pr.user != (user == 1)) continue;
            if (!any) {
                menu->addSeparator();
                if (user) menu->addSection("Yours");
                any = true;
            }
            std::string key = (user ? "u:" : "s:") + pr.category;
            if (!menus.count(key)) menus[key] = menu->addMenu(pr.category);
            std::string path = pr.path;
            menus[key]->addCheckable(pr.name, now && now->name == pr.name && now->category == pr.category,
                                     [this, path](bool) { choosePreset(path); });
        }
    }
}

void SynthDialog::savePreset() {
    std::string name = page->patch.name == "Init" ? std::string() : page->patch.name;
    if (!ui::getText(this, "Save preset", "Name of the preset:", name) || util::trim(name).empty()) return;
    std::vector<std::string> cats(CATEGORIES.begin(), CATEGORIES.end());
    cats.push_back("User");
    int index = 0;
    for (size_t i = 0; i < cats.size(); i++)
        if (cats[i] == page->patch.category) index = (int)i;
    if (!ui::getItem(this, "Save preset", "Category:", cats, index)) return;
    std::string path, error;
    if (!save(page->patch, name, cats[(size_t)std::clamp(index, 0, (int)cats.size() - 1)], path, error)) {
        ui::msg::warning(this, "Cannot save preset", error);
        return;
    }
    Patch p = page->patch;
    p.name = util::trim(name), p.category = cats[(size_t)std::clamp(index, 0, (int)cats.size() - 1)];
    apply(p, true);
    if (sampler_dialog::host().status) sampler_dialog::host().status("Saved the synth preset " + p.name);
}

void SynthDialog::reload() { showPatch(); }

void SynthDialog::showModulation(bool on) {
    page->setVisible(!on);
    modPanel->setVisible(on);
    synthView->setCheckedSilently(!on);
    modView->setCheckedSilently(on);
    (on ? (ui::Widget*)modList : page)->setFocus();
}

bool SynthDialog::modulateFor(int target, int source) {
    Patch p = page->patch;
    if (!modulateDialog(this, p, target, source)) return false;
    apply(p, true);
    return true;
}

void SynthDialog::renderClips() { sampler_dialog::renderTrackClips(track); }

void SynthDialog::playNote() {
    const sampler_dialog::Host& h = sampler_dialog::host();
    if (!h.engine) return;
    live::NoteRender render = noteRenderer(*track);
    h.engine->liveNotes(track.get())->noteOn(60, 100, h.engine->sr, true, 1.0, render.render ? &render : nullptr);
}

void SynthDialog::done(int result) {
    if (renderTimer_ >= 0) {  // (a change not rendered yet: now)
        ui::App::get().cancelTimer(renderTimer_);
        renderTimer_ = -1;
        renderClips();
    }
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
    ui::Dialog::done(result);
}

SynthDialog* openFor(const Track* track) {
    auto it = gWindows.find(track);
    return it == gWindows.end() ? nullptr : it->second;
}

SynthDialog* openSynth(const TrackPtr& track) {
    if (!track) return nullptr;
    SynthDialog* dialog = openFor(track.get());
    if (!dialog) {
        dialog = new SynthDialog(sampler_dialog::host().window, track);
        gWindows[track.get()] = dialog;
    } else {
        dialog->reload();  // (as the track plays now: a Drawn synth's canvas, or the knobs)
    }
    dialog->open();
    dialog->raiseWindow();
    util::debugf("synth: window of %s open\n", track->name.c_str());
    return dialog;
}

void reloadAll() {
    for (auto& kv : gWindows) kv.second->reload();
}

void closeGone(const Project& project) {
    std::vector<SynthDialog*> gone;
    for (auto& kv : gWindows) {
        const TrackPtr& t = kv.second->track;
        bool builtIn = !t->sampler && !t->padKit && !t->instrument && !t->usesRack && !t->ext.on;
        if (!project.tracks.contains(t) || !builtIn) gone.push_back(kv.second);
    }
    for (SynthDialog* d : gone) d->close();
}

}  // namespace synth_dialog
