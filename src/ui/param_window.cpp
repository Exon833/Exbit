// A plugin's parameters as a list (see param_window.h)
#include "ui/param_window.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "audio/automation.h"
#include "ui/toolkit/layout.h"

namespace {
constexpr int REFRESH_MS = 200;  // the values followed (automation moving them)
constexpr int NAME_W = 28;       // the name column (characters), the value after it
constexpr double TYPED_S = 1.0;  // letters typed this close together find one name

plat::WindowOptions windowOptions(ui::Window* parent, const std::string& title) {
    plat::WindowOptions o;
    o.title = title;
    o.w = 460;
    o.h = 480;
    o.dialog = true;  // (stays above the main window)
    o.transientFor = parent ? parent->native() : nullptr;
    return o;
}
}  // namespace

ParamWindow::ParamWindow(ui::Window* parent, SlotPtr slot_, const std::string& title)
    : ui::Window(windowOptions(parent, title)), slot(std::move(slot_)) {
    deleteOnClose = true;
    setTitle(title);
    if (slot) plugin_ = std::atomic_load(&slot->instance);
    ui::VBox* col = ui::vbox(this, 10, 6);
    col->addWidget(add<ui::Label>(slot ? slot->name + " has no window of its own here: its parameters" : ""));
    list = add<ui::ListWidget>();
    list->vPolicy = ui::SizePolicy::Expanding;
    col->addWidget(list, 1);
    auto* hint = add<ui::Label>("Left/Right: a step  Page Up/Down: a tenth  Home/End: the ends  Backspace: its default");
    hint->setWordWrap(true);
    col->addWidget(hint);
}

ParamWindow::~ParamWindow() {
    if (timer_ >= 0) ui::App::get().cancelTimer(timer_);
}

std::string ParamWindow::rowText(const plug::ParamInfo& p) {
    std::string name = p.name.empty() ? "#" + std::to_string(p.id) : p.name;
    std::string value = plugin_ ? plugin_->paramText(p.id, plugin_->getParam(p.id)) : "";
    if (!p.units.empty() && value.find(p.units) == std::string::npos) value += " " + p.units;
    if ((int)name.size() < NAME_W) name += std::string((size_t)(NAME_W - (int)name.size()), ' ');
    return name + " " + value;
}

bool ParamWindow::attach() {
    if (!plugin_) return false;
    for (const plug::ParamInfo& p : plugin_->parameters())
        if (!p.hidden && !p.readOnly && !p.isProgramChange) params.push_back(p);
    if (params.empty()) return false;
    for (const plug::ParamInfo& p : params) list->addItem(rowText(p));
    list->setCurrentRow(0);
    list->setFocus();
    if (timer_ < 0) timer_ = ui::App::get().every(REFRESH_MS, [this] { follow(); });
    return true;
}

void ParamWindow::follow() {
    if (!plugin_) return;
    for (int i = 0; i < (int)params.size() && i < list->count(); i++) {
        std::string t = rowText(params[(size_t)i]);
        if (list->item(i).text != t) {
            list->item(i).text = t;
            list->update();
        }
    }
}

void ParamWindow::set(int row, double normal) {
    if (!plugin_ || row < 0 || row >= (int)params.size()) return;
    const plug::ParamInfo& p = params[(size_t)row];
    normal = std::clamp(normal, 0.0, 1.0);
    if (p.stepCount > 0) normal = std::round(normal * p.stepCount) / p.stepCount;
    plugin_->windowEdit(p.id, normal);  // (as its own window: "recently tweaked")
    if (watcher) watcher->writeEdits({p.id});  // (written at once, armed and playing: automation_write.h)
    follow();
}

bool ParamWindow::keyPress(ui::KeyEvent& e) {
    int row = list->currentRow();
    if (row < 0 || row >= (int)params.size() || !plugin_) return ui::Window::keyPress(e);
    const plug::ParamInfo& p = params[(size_t)row];
    double v = plugin_->getParam(p.id);
    double step = p.stepCount > 0 ? 1.0 / p.stepCount : 0.01;
    switch (e.key) {
    case plat::Key_Left: set(row, v - step); return true;
    case plat::Key_Right: set(row, v + step); return true;
    case plat::Key_PageDown: set(row, v - std::max(step, 0.1)); return true;
    case plat::Key_PageUp: set(row, v + std::max(step, 0.1)); return true;
    case plat::Key_Home: set(row, 0); return true;
    case plat::Key_End: set(row, 1); return true;
    case plat::Key_Backspace: set(row, p.defaultValue); return true;
    case plat::Key_Escape: close(); return true;
    default: break;
    }
    // letters: the next parameter whose name starts with what was typed
    if (!e.text.empty() && !(e.mods & (plat::ModCtrl | plat::ModAlt)) && isprint((unsigned char)e.text[0])) {
        double now = ui::App::get().now();
        typed_ = now - typedAt_ < TYPED_S ? typed_ + e.text : e.text;
        typedAt_ = now;
        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return s;
        };
        for (int k = 0; k < (int)params.size(); k++) {
            int i = (row + (typed_.size() == 1 ? 1 : 0) + k) % (int)params.size();
            if (lower(params[(size_t)i].name).rfind(lower(typed_), 0) == 0) {
                list->setCurrentRow(i);
                list->scrollToRow(i);
                return true;
            }
        }
        return true;
    }
    return ui::Window::keyPress(e);
}

Point ParamWindow::position() const {
    Rect g = screenGeometry();
    return Point(g.x, g.y);
}

void ParamWindow::place(ui::Window* main, std::optional<Point> last) {
    Rect frame = screenGeometry();
    if (!last) {
        Rect centre = main ? main->screenGeometry() : ui::App::get().screenAt(ui::App::get().cursorPos());
        last = Point(centre.x + (centre.w - frame.w) / 2, centre.y + (centre.h - frame.h) / 3);
    }
    moveWindow(last->x, last->y);
}

void ParamWindow::close() {
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
