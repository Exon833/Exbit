// A plugin's own window (plugin_editor.py).
#include "ui/plugin_editor.h"

#include <algorithm>

#include "audio/automation.h"
#include "audio/plugin_audio.h"
#include "plugins/plugin.h"
#include "plugins/wine.h"
#include "ui/computer_keyboard.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

plat::WindowOptions EditorWindow::options(ui::Window* parent, const std::string& title, bool invisible) {
    plat::WindowOptions o;
    o.title = title;
    o.w = 400;
    o.h = 300;
    o.dialog = true;  // (stays above the main window)
    o.transientFor = parent ? parent->native() : nullptr;
    if (invisible) {
        o.offscreen = true;   // mapped outside every screen: never seen, no taskbar entry
        o.takesFocus = false;
        o.transientFor = nullptr;
    }
    return o;
}

EditorWindow::EditorWindow(ui::Window* parent, SlotPtr slot_, const std::string& title, bool invisible_)
    : ui::Window(options(parent, title, invisible_)), slot(std::move(slot_)), invisible(invisible_) {
    deleteOnClose = true;
    setScale(1);  // (a plugin draws its window at its own size, whatever the interface size)
    setStyleType("EditorWindow");
    setBackground(ui::style::background(computedStyle("plugin")));  // (behind the plugin's own window)
    setAcceptDrops(true);  // (files, for a Windows plugin: dragEnter)
}

void EditorWindow::dragEnter(ui::DragEvent& e) {
    auto instance = aload(slot->instance);
    e.accepted = instance && plug::isBridged(*instance) && instance->editorOpen() && e.data && !e.data->files.empty() &&
                 !slot->failed.load(std::memory_order_acquire);
}

void EditorWindow::drop(ui::DragEvent& e) {
    auto instance = aload(slot->instance);
    if (!instance || !e.data || e.data->files.empty()) return;
    // (this window is at the plugin's own pixels: setScale(1))
    std::string how = plug::bridgedDropFiles(*instance, e.data->files, e.pos.x, e.pos.y);
    if (how.empty())
        ui::App::get().showToolTip(e.globalPos, "The plugin didn't take the file there", this);
}

EditorWindow::~EditorWindow() {
    if (frameId_ >= 0) ui::App::get().removeFrame(frameId_);
    if (!closing_) {  // (deleted without being closed: the plugin lets go of the window first)
        auto instance = aload(slot->instance);
        if (instance && instance->editorOpen()) instance->closeEditor();
    }
}

// The window the plugin's view goes into: a window of its own inside ours, below the title bar (our own frame,
// Window::isFramed) or filling it (the window manager's), where the inside is. (Offscreen, for photographs: ours
// itself, never framed.)
uintptr_t EditorWindow::pluginParent() {
    if (!native()) return 0;
    if (invisible) return native()->nativeHandle();
    embedded_ = true;
    return native()->embedArea(pluginArea());
}

Rect EditorWindow::pluginArea() const {
    FrameInsets f = frameInsets();  // (scale 1: logical pixels are the native window's)
    return Rect(f.left, f.top, width(), height());
}

void EditorWindow::placePlugin() {
    if (embedded_ && native() && !stoppedShown_) native()->embedArea(pluginArea());
}

void EditorWindow::frameChanged() {
    ui::Window::frameChanged();
    placePlugin();
}

bool EditorWindow::attach() {
    auto instance = aload(slot->instance);
    if (!instance || !native()) return false;
    int w = 0, h = 0;
    try {
        if (!instance->hasEditor()) return false;
        if (!instance->openEditor(pluginParent(), native()->nativeDisplay(), 1.0, w, h)) return false;
        resizable = instance->editorResizable();
    } catch (const std::exception&) {
        return false;
    }
    fit(w, h);
    frameId_ = ui::App::get().onFrame([this] { idle(); });
    return true;
}

void EditorWindow::fit(int width, int height) {
    int w = std::max(1, width), h = std::max(1, height);
    fitting = true;
    if (resizable) {
        setMinimumWindowSize(1, 1);
        setMaximumSize(1 << 24, 1 << 24);
        setResizable(true);
    } else {
        setMinimumWindowSize(w, h);
        setMaximumSize(w, h);
        setResizable(false);
    }
    resizeWindow(w, h);
    placePlugin();
    fitting = false;
}

void EditorWindow::idle() {
    if (closing_) return;
    auto instance = aload(slot->instance);
    if (!instance) {
        close();
        return;
    }
    // (a Windows plugin stopped: its view went with its host, nothing more to ask of it. A plugin of
    // Linux keeps its view here, over anything said in the window: left as it is)
    if (reloadable && slot->failed.load(std::memory_order_acquire)) {
        if (!stoppedShown_) showStopped();
        return;
    }
    instance->idle();
    // files the plugin dragged out of its window (a Windows plugin's, through its Wine host): dragged
    // on under the pointer, to be dropped on a track, the explorer…
    if (plug::isBridged(*instance)) {
        auto files = plug::bridgedDraggedFiles(*instance);
        if (!files.empty()) {
            ui::DragData d;
            d.files = files;
            d.kind = "files";
            ui::App::get().followPointerDrag(d);
        }
    }
    if (sizeChanged_) {  // (the window's edge dragged since the last frame: the latest size, once)
        sizeChanged_ = false;
        int w = width(), h = height();
        if (instance->editorOpen()) {
            instance->editorResized(w, h);
            if (w > 0 && h > 0 && (w != width() || h != height())) fit(w, h);  // (the plugin only takes certain sizes)
        }
    }
    int w = 0, h = 0;
    if (instance->requestedSize(w, h) && w > 0 && h > 0) fit(w, h);
    // the computer keyboard's keys come to this window even after a click gave the plugin the keys
    // (a Windows plugin's window, inside ours, takes them then): they play notes; the others, and
    // Shift+…, still go to the plugin (letters typed into its own text fields play notes meanwhile)
    if (native()) native()->grabKeys(computer_keyboard::keysInUse());
}

void EditorWindow::showStopped() {
    if (stoppedShown_) return;
    stoppedShown_ = true;
    if (embedded_ && native()) native()->embedArea(Rect());  // (the plugin's view went: what is said here shows)
    const css::Style& st = computedStyle("stopped");
    setBackground(ui::style::background(st));
    auto* lay = ui::vbox(this, ui::style::px(st, "padding-top", 16), ui::style::px(st, "row-gap", 10));
    auto* text = add<ui::Label>(slot->name + " stopped" + (slot->error.empty() ? "." : ": " + slot->error) +
                                (reloadable ? "\nReload starts it afresh, with the settings it last had." : ""));
    text->setWordWrap(true);
    lay->addWidget(text);
    if (reloadable) {
        auto row = std::make_unique<ui::HBox>();
        auto* reload = add<ui::Button>("Reload");
        reload->setToolTip("Start " + slot->name + " again in a new Wine host, with the settings it last had");
        reload->clicked.connect([this] {
            SlotPtr s = slot;
            reloadRequested.emit(s);
        });
        row->addWidget(reload);
        row->addStretch(1);
        lay->addLayout(std::move(row));
    }
    lay->addStretch(1);
    const css::Style& least = computedStyle("stopped");  // (min-width / min-height: the least it is made)
    int minW = ui::style::px(least, "min-width", 360), minH = ui::style::px(least, "min-height", 140);
    if (width() < minW || height() < minH) fit(std::max(width(), minW), std::max(height(), minH));
    update();
}

void EditorWindow::onEvent(plat::Window* w, const plat::Event& e) {
    if (computer_keyboard::filter(this, e)) return;
    ui::Window::onEvent(w, e);
}

// the plugin is told with the next frame (idle): a drag gives a size for every move of the pointer,
// many in one go when the program was busy, and each is a round trip to a Windows plugin's host
// (30 sizes in a row held the main thread ~0.5 s)
void EditorWindow::resized() {
    ui::Window::resized();
    placePlugin();  // (its window: the inside's size, below the title bar)
    if (!resizable || fitting || closing_) return;
    sizeChanged_ = true;
}

Point EditorWindow::position() const {
    Rect g = screenGeometry();
    return Point(g.x, g.y);
}

void EditorWindow::place(ui::Window* main, std::optional<Point> last) {
    // (the whole window on the screen: its own frame too, when it has one; `last` is the inside's place)
    Rect inside = screenGeometry(), frame = outerScreenGeometry();
    Point f(inside.x - frame.x, inside.y - frame.y);  // (the frame's left and top, in global coordinates)
    int fw = std::max(frame.w, width()), fh = std::max(frame.h, height());  // (as before: never less than its own size)
    Point at;
    if (last) {
        at = Point(last->x - f.x, last->y - f.y);
    } else {
        Rect centre = main ? main->screenGeometry() : ui::App::get().screenAt(ui::App::get().cursorPos());
        at = Point(centre.x + (centre.w - fw) / 2, centre.y + (centre.h - fh) / 3);
    }
    Rect area = ui::App::get().screenAt(Point(at.x + fw / 2, at.y + 16));
    int x = std::max(area.x, std::min(at.x, area.right() - fw));
    int y = std::max(area.y, std::min(at.y, area.bottom() - fh));
    moveWindow(x + f.x, y + f.y);
}

void EditorWindow::close() {
    if (closing_) return;
    closing_ = true;
    if (frameId_ >= 0) {
        ui::App::get().removeFrame(frameId_);
        frameId_ = -1;
    }
    auto instance = aload(slot->instance);
    if (instance) {
        try {
            instance->closeEditor();
        } catch (const std::exception&) {
        }
    }
    SlotPtr s = slot;
    editorClosed.emit(s);
    ui::Window::close();
}
