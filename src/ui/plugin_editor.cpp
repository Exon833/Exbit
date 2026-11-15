// A plugin's own window (plugin_editor.py).
#include "ui/plugin_editor.h"

#include <algorithm>

#include "audio/automation.h"
#include "audio/plugin_audio.h"
#include "plugins/plugin.h"
#include "ui/computer_keyboard.h"

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
    setBackground(Color(0, 0, 0));
}

EditorWindow::~EditorWindow() {
    if (frameId_ >= 0) ui::App::get().removeFrame(frameId_);
    if (!closing_) {  // (deleted without being closed: the plugin lets go of the window first)
        auto instance = aload(slot->instance);
        if (instance && instance->editorOpen()) instance->closeEditor();
    }
}

bool EditorWindow::attach() {
    auto instance = aload(slot->instance);
    if (!instance || !native()) return false;
    int w = 0, h = 0;
    try {
        if (!instance->hasEditor()) return false;
        if (!instance->openEditor(native()->nativeHandle(), native()->nativeDisplay(), 1.0, w, h)) return false;
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
        if (native()) native()->setResizable(true);
    } else {
        setMinimumWindowSize(w, h);
        setMaximumSize(w, h);
        if (native()) native()->setResizable(false);
    }
    resizeWindow(w, h);
    fitting = false;
}

void EditorWindow::idle() {
    if (closing_) return;
    auto instance = aload(slot->instance);
    if (!instance) {
        close();
        return;
    }
    instance->idle();
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

void EditorWindow::onEvent(plat::Window* w, const plat::Event& e) {
    if (computer_keyboard::filter(this, e)) return;
    ui::Window::onEvent(w, e);
}

// the plugin is told with the next frame (idle): a drag gives a size for every move of the pointer,
// many in one go when the program was busy, and each is a round trip to a Windows plugin's host
// (30 sizes in a row held the main thread ~0.5 s)
void EditorWindow::resized() {
    ui::Window::resized();
    if (!resizable || fitting || closing_) return;
    sizeChanged_ = true;
}

Point EditorWindow::position() const {
    Rect g = screenGeometry();
    return Point(g.x, g.y);
}

void EditorWindow::place(ui::Window* main, std::optional<Point> last) {
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
