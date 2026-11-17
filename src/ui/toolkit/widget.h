// Tiny DAW's own GUI toolkit, core: signals, events, Widget, top-level windows, the application.
// (The stand-in for Qt's QObject/QWidget/QApplication; the controls are in controls.h, menus in
// menu.h, dialogs in dialogs.h, layouts in layout.h.)
//
// - Widgets form a tree. A parent owns its children (add<T>(…) makes one); deleteLater() removes
//   one safely, even from inside its own event handler.
// - geometry() is relative to the parent. A widget paints itself in paint(Painter&) with (0, 0) at
//   its top-left and the painter clipped to it; children are painted over it, in order.
// - Mouse: the widget under the pointer gets the press and then every move and the release, until
//   the last button is up (an implicit grab, as in Qt). Double clicks come as mouseDoubleClick
//   (after a mousePress for the first click). Moves without a button go to the widget under the
//   pointer (only when it has mouseTracking or hover effects: every widget gets enter/leave).
// - Wheel and key events not accepted (handlers return false) go on to the parent, then to the
//   window's shortcuts. Shortcut objects (Shortcut / menu actions) with ApplicationWide scope work
//   in every window of the program (Space, Home… from inside an FX window).
// - update() asks for a repaint (of the widget's area) at the next frame; nothing is drawn
//   immediately. Each top-level window keeps its own framebuffer and presents only what changed.
// - Keys (as Qt): a key press first goes to the window's shortcuts, unless the focus widget (or an
//   ancestor of it) overridesShortcut() for it (a LineEdit takes printable keys and editing keys, so
//   Space typed into a field doesn't start playback, while Ctrl+S still saves); then to the focus
//   widget and up its parents; Tab / Shift+Tab not taken by anyone move the focus.
// - A dialog window (WindowOptions::dialog) gives the focus to its first Tab-focusable widget when
//   shown without one; other windows keep the focus where the program puts it (clicks on widgets
//   with Click / Strong focus policy move it, clicks elsewhere leave it).
// - deleteLater() deletes after the event being handled, at the loop level it was asked at (a
//   nested loop — Dialog::exec, Menu::exec — doesn't delete what its caller still runs in). Deleted
//   widgets are forgotten everywhere (hover, grab, focus, tooltips, drags, shortcuts, popups).
// - Styles (stylesheets: core/css.h, ui/skin.h, docs/skins.md): every widget is a node the skin's
//   stylesheets match. Its type is the class's style type (Button, Slider…; also the types it counts as,
//   setStyleType), its id its objectName, its classes its properties (setProperty("toggle", "mute"):
//   .toggle and .toggle-mute; "false", "0" or "" set none) and addStyleClass; its states :hover :pressed
//   :disabled :focus and whatever styleStates() adds (:checked…). computedStyle(part) is what applies to it now,
//   kept until the skin, its states, its classes or an ancestor's style change.
// - Layouts are redone lazily: updateGeometry() (called by setText, setVisible, size changes…)
//   marks the parents' layouts dirty and they run before the next frame is painted (or at once with
//   App::doLayouts() / relayout()).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/css.h"
#include "gfx/font.h"
#include "gfx/geom.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "platform/platform.h"

namespace ui {

using plat::Cursor;
using plat::Key;

// --- signals: several listeners, called in the order connected ---
template <class... Args>
class Signal {
public:
    using Fn = std::function<void(Args...)>;
    int connect(Fn fn) { slots_.push_back({++next_, std::move(fn)}); return next_; }
    void disconnect(int id) { for (auto& s : slots_) if (s.id == id) s.fn = nullptr; }
    void disconnectAll() { slots_.clear(); }
    void emit(Args... args) const {
        auto copy = slots_;  // (a listener may connect or disconnect others)
        for (auto& s : copy) if (s.fn) s.fn(args...);
    }
    void operator()(Args... args) const { emit(args...); }
    bool empty() const { for (auto& s : slots_) if (s.fn) return false; return true; }
private:
    struct Slot { int id; Fn fn; };
    std::vector<Slot> slots_;
    int next_ = 0;
};

// --- events (positions are in the receiving widget's coordinates) ---
struct MouseEvent {
    Point pos;          // in the widget
    Point globalPos;    // on the screen
    int button = 0;     // plat::MouseButton that changed (press / release / double-click)
    int buttons = 0;    // held
    int mods = 0;       // plat::Modifier held
    double time = 0;
    bool shift() const { return mods & plat::ModShift; }
    bool ctrl() const { return mods & plat::ModCtrl; }
    bool alt() const { return mods & plat::ModAlt; }
};
struct WheelEvent {
    Point pos, globalPos;
    float dx = 0, dy = 0;   // notches; up / left positive (Qt angleDelta / 120)
    int mods = 0;
    bool ctrl() const { return mods & plat::ModCtrl; }
    bool shift() const { return mods & plat::ModShift; }
    bool alt() const { return mods & plat::ModAlt; }
};
struct KeyEvent {
    int key = 0;            // plat::Key (letters UPPERCASE)
    int mods = 0;
    std::string text;       // what it types, if anything
    bool autoRepeat = false;
    bool ctrl() const { return mods & plat::ModCtrl; }
    bool shift() const { return mods & plat::ModShift; }
    bool alt() const { return mods & plat::ModAlt; }
};

// A drag carrying data between widgets of the program (samples from the Explorer, files from the
// desktop). Started with Widget::startDrag(); targets get dragEnter/dragMove/dragLeave/drop.
struct DragData {
    std::vector<std::string> files;   // file paths (samples, desktop files)
    std::string kind;                 // "files" or a program-specific kind
    std::string text;
};
struct DragEvent {
    Point pos, globalPos;
    int mods = 0;
    const DragData* data = nullptr;
    bool accepted = false;            // set by dragEnter/dragMove to take it
    bool alt() const { return mods & plat::ModAlt; }
};

class Window;
class Menu;

enum class FocusPolicy { None, Click, Tab, Strong };
enum class SizePolicy { Fixed, Preferred, Expanding };

class Widget : public css::Node {
public:
    explicit Widget(Widget* parent = nullptr);
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // --- the tree ---
    template <class T, class... A>
    T* add(A&&... args) {  // a new child, owned by this widget
        auto w = std::make_unique<T>(std::forward<A>(args)...);
        T* raw = w.get();
        adopt(std::move(w));
        return raw;
    }
    void adopt(std::unique_ptr<Widget> child);         // take ownership of a widget (made elsewhere)
    std::unique_ptr<Widget> release(Widget* child);    // give it up (still alive)
    void deleteLater();                                // removed from its parent and deleted after the current event
    void clearChildren();                              // delete every child (later)
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    Window* window() const;                            // the top-level window it is in (nullptr if none)
    void raise();                                      // on top of its siblings
    void lower();

    // --- geometry ---
    const Rect& geometry() const { return geom_; }
    void setGeometry(const Rect& r);
    void setGeometry(int x, int y, int w, int h) { setGeometry(Rect(x, y, w, h)); }
    void move(int x, int y) { setGeometry(Rect(x, y, geom_.w, geom_.h)); }
    void resize(int w, int h) { setGeometry(Rect(geom_.x, geom_.y, w, h)); }
    int x() const { return geom_.x; }
    int y() const { return geom_.y; }
    int width() const { return geom_.w; }
    int height() const { return geom_.h; }
    Rect rect() const { return {0, 0, geom_.w, geom_.h}; }
    Point mapToGlobal(Point p) const;
    Point mapFromGlobal(Point p) const;
    Point mapTo(const Widget* ancestor, Point p) const;   // into an ancestor's coordinates
    Point mapToWindow(Point p) const;
    // size hints for layouts
    virtual Point sizeHint() const;                     // default: the fixed size, else 0x0 (or the layout's)
    // the smallest size a layout may give it (QWidget::minimumSizeHint): default the layout's minimum, else 0x0
    virtual Point minimumSizeHint() const;
    // tells the layouts above that its size hint changed (they are redone before the next frame)
    void updateGeometry();
    // the area its layout fills (GroupBox: below the title)
    virtual Rect contentsRect() const { return rect(); }
    Point minimumSize() const { return minSize_; }
    Point maximumSize() const { return maxSize_; }
    void setMinimumSize(int w, int h);
    void setMaximumSize(int w, int h);
    void setFixedSize(int w, int h);
    void setFixedWidth(int w);
    void setFixedHeight(int h);
    void setMinimumWidth(int w) { setMinimumSize(w, minSize_.y); }
    void setMinimumHeight(int h) { setMinimumSize(minSize_.x, h); }
    void setMaximumWidth(int w) { setMaximumSize(w, maxSize_.y); }
    void setMaximumHeight(int h) { setMaximumSize(maxSize_.x, h); }
    SizePolicy hPolicy = SizePolicy::Preferred, vPolicy = SizePolicy::Preferred;
    void setSizePolicy(SizePolicy h, SizePolicy v) { hPolicy = h; vPolicy = v; }

    // --- layout: a widget with a layout places its visible children by it on every resize ---
    void setLayout(std::unique_ptr<class Layout> layout);
    class Layout* layout() const { return layout_.get(); }
    void relayout();          // run the layout (and later its children's) now
    // a child without a layout above it changed its size hint (ScrollArea's viewport: re-place the content)
    virtual void childSizeHintChanged(Widget* child) {}
    virtual void resized() {} // called after the geometry's size changed (and the layout ran)
    virtual void moved() {}

    // --- state ---
    bool isVisible() const { return visible_; }   // its own flag
    bool isShown() const;                          // it and every ancestor visible, in a shown window
    virtual void setVisible(bool v);   // (a Window: showWindow / hideWindow)
    void show() { setVisible(true); }
    void hide() { setVisible(false); }
    bool isEnabled() const;                        // it and every ancestor enabled
    void setEnabled(bool e);
    bool underMouse() const { return hovered_; }
    bool isPressed() const { return pressed_; }    // a mouse button went down on it and is still held
    void setToolTip(const std::string& t) { tooltip_ = t; }
    const std::string& toolTip() const { return tooltip_; }
    virtual std::string toolTipAt(Point p) const { return tooltip_; }  // for widgets whose tip depends on the place
    void setCursor(Cursor c);
    Cursor cursor() const { return cursor_; }
    void setMouseTracking(bool on) { tracking_ = on; }  // mouseMove also without buttons
    bool hasMouseTracking() const { return tracking_; }
    void setAcceptDrops(bool on) { acceptDrops_ = on; }
    bool acceptsDrops() const { return acceptDrops_; }
    void setFocusPolicy(FocusPolicy p) { focusPolicy_ = p; }
    FocusPolicy focusPolicy() const { return focusPolicy_; }
    void setFocus();
    void clearFocus();
    bool hasFocus() const;
    Widget* focusProxy = nullptr;  // setFocus() / a click focuses this one instead (SpinBox: its editor)
    // the focus widget (or an ancestor of it) takes this key before the window's shortcuts (QEvent::ShortcutOverride)
    virtual bool overridesShortcut(const KeyEvent& e) const { return false; }
    bool isAncestorOf(const Widget* w) const;      // w is inside it (or it is w)
    // a background filled before paint() (the window colour by default where autoFill)
    void setBackground(Color c) { background_ = c; hasBackground_ = true; update(); }
    void clearBackground() { hasBackground_ = false; update(); }
    // string properties used by styles (as Qt dynamic properties): "lit", "toggle", "accent", "hint", "dimmed"…
    void setProperty(const std::string& key, const std::string& value);
    std::string property(const std::string& key) const;
    std::string objectName;  // (its #id in stylesheets)

    // --- style: what the skin's stylesheets give it (see the top) ---
    const std::string& type() const override { return types_.back(); }
    bool isType(const std::string& name) const override;
    const std::string& id() const override { return objectName; }
    bool hasClass(const std::string& name) const override;
    bool hasState(const std::string& name) const override;
    Widget* parent() const override { return parent_; }
    void addStyleClass(const std::string& name);
    void removeStyleClass(const std::string& name);
    // what applies to it (or its part: "handle", "arrow"…) in its states now
    const css::Style& computedStyle(const std::string& part = "") const;
    // the names of the states it is in (the base: hover, pressed, disabled, focus; overrides add theirs)
    virtual void styleStates(std::vector<std::string>& out) const;
    // opaque per-widget data for the program
    std::shared_ptr<void> userData;

    // --- painting ---
    void update();                     // repaint all of it at the next frame
    void update(const Rect& r);        // a part of it
    virtual void paint(Painter& p) {}  // draw itself; children are drawn over it
    virtual void paintOver(Painter& p) {}  // drawn after the children (overlays)
    void setClipsChildren(bool on) { clipsChildren_ = on; }

    // --- events: return true when handled (else it goes on to the parent: wheel, keys, context menus) ---
    virtual bool mousePress(MouseEvent& e) { return false; }
    virtual bool mouseRelease(MouseEvent& e) { return false; }
    virtual bool mouseDoubleClick(MouseEvent& e) { return mousePress(e); }
    virtual bool mouseMove(MouseEvent& e) { return false; }
    virtual bool wheel(WheelEvent& e) { return false; }
    virtual bool keyPress(KeyEvent& e) { return false; }
    virtual bool keyRelease(KeyEvent& e) { return false; }
    virtual bool contextMenu(MouseEvent& e) { return false; }  // right-click (after mousePress); pos as there
    virtual void enter() {}
    virtual void leave() {}
    virtual void focusIn() {}
    virtual void focusOut() {}
    virtual void dragEnter(DragEvent& e) {}
    virtual void dragMove(DragEvent& e) { dragEnter(e); }
    virtual void dragLeave() {}
    virtual void drop(DragEvent& e) {}
    virtual void showEvent() {}
    virtual void hideEvent() {}
    // start dragging `data` from this widget (call from mouseMove while a button is held); the
    // drop happens when the button is released over a widget that accepted it
    void startDrag(const DragData& data, const Image* picture = nullptr);
    // the widget at a point (in this widget's coordinates): the deepest visible, enabled-or-not child
    Widget* childAt(Point p);

    // an event filter (QObject::installEventFilter) on this widget: called with (this, what) before
    // its own handler; return true to swallow the event. `what`: "mousePress", "mouseRelease",
    // "mouseDoubleClick", "mouseMove", "wheel", "keyPress", "keyRelease", "contextMenu", "enter",
    // "leave", "focusIn", "focusOut", "resize", "move", "show", "hide" (the last ones can't be swallowed)
    std::function<bool(Widget*, const char* what)> onEventFilter;
    bool filterEvent(const char* what) { return onEventFilter && onEventFilter(this, what); }

    // (internal, used by the toolkit)
    bool hovered_ = false, pressed_ = false;
    void setParentInternal(Widget* p) { parent_ = p; }
protected:
    // its type in stylesheets (called by each class's constructor: Widget, then Button, then ToolButton…);
    // the earlier ones stay types it counts as
    void setStyleType(const std::string& name);
    // the type that was `was` becomes `now` (Button: PushButton <-> ToolButton); else it is added
    void replaceStyleType(const std::string& was, const std::string& now);
public:
    void paintTree(Painter& p, const Rect& dirty);  // dirty in this widget's coordinates
    bool deleted_ = false;
    bool layoutDirty_ = false;
    static bool alive(const Widget* w);  // not destroyed yet (dispatch safety)

private:
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    std::unique_ptr<class Layout> layout_;
    Rect geom_;
    Point minSize_{0, 0}, maxSize_{1 << 24, 1 << 24};
    bool visible_ = true, enabled_ = true, tracking_ = false, acceptDrops_ = false, clipsChildren_ = true;
    bool hasBackground_ = false;
    Color background_;
    Cursor cursor_ = Cursor::Arrow;
    bool cursorSet_ = false;           // (else the parent's cursor shows)
    FocusPolicy focusPolicy_ = FocusPolicy::None;
    std::string tooltip_;
    std::map<std::string, std::string> props_;
    std::vector<std::string> types_{"Widget"}, classes_;
    int classesVersion_ = 0;
    struct StyleEntry {
        std::string part, states;
        std::shared_ptr<const css::Style> parent;  // (kept alive: never mistaken for another)
        int generation = -1, classes = -1;
        std::shared_ptr<const css::Style> style;
    };
    mutable std::vector<StyleEntry> styles_;
    std::shared_ptr<const css::Style> styleShared(const std::string& part) const;
    friend class Window;
    friend class App;
};

// --- a widget that calls a function to paint and handle the mouse (quick custom widgets) ---
// (its style type: Canvas; what it draws is the program's, which may read computedStyle() for it)
class Canvas : public Widget {
public:
    explicit Canvas(Widget* parent = nullptr) : Widget(parent) { setStyleType("Canvas"); }
    std::function<void(Painter&)> onPaint;
    std::function<bool(MouseEvent&)> onPress, onRelease, onMove, onDoubleClick, onContextMenu;
    std::function<bool(WheelEvent&)> onWheel;
    std::function<void()> onResized;
    void paint(Painter& p) override { if (onPaint) onPaint(p); }
    bool mousePress(MouseEvent& e) override { return onPress && onPress(e); }
    bool mouseRelease(MouseEvent& e) override { return onRelease && onRelease(e); }
    bool mouseMove(MouseEvent& e) override { return onMove && onMove(e); }
    bool mouseDoubleClick(MouseEvent& e) override { return onDoubleClick ? onDoubleClick(e) : mousePress(e); }
    bool contextMenu(MouseEvent& e) override { return onContextMenu && onContextMenu(e); }
    bool wheel(WheelEvent& e) override { return onWheel && onWheel(e); }
    void resized() override { if (onResized) onResized(); }
};

// --- keyboard shortcuts ---
struct KeySequence {
    int key = 0, mods = 0;
    KeySequence() = default;
    KeySequence(int k, int m = 0) : key(k), mods(m) {}
    static KeySequence parse(const std::string& text);  // "Ctrl+Shift+S", "Space", "Del", "F1"
    std::string text() const;                           // "Ctrl+Shift+S"
    bool matches(const KeyEvent& e) const;
    bool empty() const { return key == 0; }
};

enum class ShortcutScope { Window, ApplicationWide };

class Shortcut {
public:
    // active while `owner` exists and its window has the focus (ApplicationWide: any of the program's windows)
    Shortcut(Widget* owner, KeySequence seq, std::function<void()> fn, ShortcutScope scope = ShortcutScope::Window);
    ~Shortcut();
    KeySequence seq;
    std::function<void()> fn;
    ShortcutScope scope;
    Widget* owner;
    bool enabled = true;
    std::function<bool()> active;  // optional: false makes the key go on as if there were no shortcut (disabled actions)
};

// --- a top-level window (main window, dialogs, the MIDI editor, FX windows, plugin windows, popups) ---
// (its style type: Window; its background-color fills it under its widgets; tooltips are ToolTip windows)
class Window : public Widget, public plat::WindowListener {
public:
    struct Options : plat::WindowOptions {};
    explicit Window(const plat::WindowOptions& opts = plat::WindowOptions());
    ~Window() override;

    plat::Window* native() const { return native_.get(); }
    void setTitle(const std::string& t);
    const std::string& title() const { return title_; }
    void showWindow();                 // map it (and lay it out)
    void setVisible(bool v) override { if (v) showWindow(); else hideWindow(); }
    void hideWindow();
    virtual void close();              // as the title bar's ✕: asks closeRequested() first (Dialog: reject)
    bool isWindowShown() const { return shown_; }
    void raiseWindow();
    void resizeWindow(int w, int h);
    void moveWindow(int x, int y);     // client area's screen position
    Rect screenGeometry() const;       // client area on the screen
    void setMinimumWindowSize(int w, int h);
    void centerOn(Window* other);      // over another window (or the screen when null)
    // return false to keep it open (e.g. "save changes?" answered Cancel)
    std::function<bool()> closeRequested;
    Signal<> closed;                   // after it was hidden by close()
    Signal<> activated;                // got the keyboard focus
    bool deleteOnClose = false;        // delete the Window object after closing
    // the root content: by default the window itself is the root widget; add children to it
    Widget* focusWidget() const { return focus_; }
    void setFocusWidget(Widget* w);
    std::vector<Shortcut*> shortcuts;  // (unused: shortcuts are kept in App::allShortcuts and matched by their owner's window)
    // modality: while a modal window is shown, other windows ignore mouse and keys
    void setModal(bool m) { modal_ = m; }
    bool isModal() const { return modal_; }
    // paint now (normally once per frame by the application)
    void flush();
    // events from the platform
    void onEvent(plat::Window* w, const plat::Event& e) override;
    // the window's framebuffer is repainted where dirty at the next frame
    void invalidate(const Rect& r);
    // keyboard focus handling: key presses not taken by the focus chain go to keyPress() of the window
    bool isActiveWindow() const { return active_; }  // has the keyboard focus
    bool isPopup() const { return opts_.popup; }
    const plat::WindowOptions& options() const { return opts_; }
    void focusNext(bool forward);      // Tab / Shift+Tab
    // the mouse / wheel / key paths (also used by tests: e.g. dispatchMouse with x, y in the window)
    void dispatchMouse(const plat::Event& e);
    void dispatchKey(const plat::Event& e);
    void dispatchWheel(const plat::Event& e);
    Widget* hoverWidget() const { return hover_; }
    Widget* grabWidget() const { return grab_; }
    const Image& framebuffer() const { return fb_; }  // (in device pixels: logical size × scale())
    // (added) the interface size: widgets are laid out and painted in logical pixels, the native
    // window and the framebuffer are scale() times bigger, events are divided by it. A window starts
    // at App::scale(); setScale(1) keeps one at its own pixels (plugins' windows). Global (screen)
    // coordinates are logical at App::scale() for every window.
    float scale() const { return scale_; }
    void setScale(float s);
    int toDevice(int v) const;          // a size in logical pixels, in device pixels
    Rect toDeviceRect(const Rect& r) const;
    const Rect& dirtyRect() const { return dirty_; }
    int showSerial_ = 0;               // (internal) order in which windows were shown
    int focusReason = 0;               // while focusIn() runs: 0 other, 1 mouse click, 2 Tab key
private:
    std::unique_ptr<plat::Window> native_;
    std::string title_;
    Image fb_;
    Rect dirty_;
    bool shown_ = false, modal_ = false;
    Widget* focus_ = nullptr;
    Widget* grab_ = nullptr;           // the widget that got the press
    Widget* hover_ = nullptr;
    int heldButtons_ = 0;
    double lastClickTime_ = 0;
    Point lastClickPos_;
    int lastClickButton_ = 0;
    bool dragging_ = false;
    bool active_ = false;
    plat::WindowOptions opts_;
    float scale_ = 1;
    int physW_ = 0, physH_ = 0;        // the native client size (device pixels)
    Rect hgeom_;                       // (headless: where it would be on the screen)
    Point lastMouse_{-1, -1};
    friend class App;
    friend class Widget;
    void updateHover(Point p, const plat::Event& e);
    void setHover(Widget* w);
    void releaseGrab();
    void applyCursor();
    void paintBackground(Painter& p);  // its style's background (and border), under its widgets
    bool blockedByModal() const;
    void externalDrag(const plat::Event& e);
    plat::Event toLogical(const plat::Event& e) const;
};

// --- the application: windows, the frame timer, tooltips, drags, the modal stack ---
class App {
public:
    static App& get();
    ~App();
    void init(int argc, char** argv);
    // tests: no display; windows are not mapped (painted only into their framebuffers), timers run
    // only through advanceTimers(), the clipboard is a variable, the screen is 1920x1080
    void initHeadless();
    bool headless() const { return headless_; }
    void advanceTimers(double seconds);  // (headless) moves the clock on and fires the timers due
    double now() const;                  // seconds (plat::now(), or the headless clock)
    int exec();                         // runs until quit()
    void quit();
    // runs a nested loop until done() is true (Dialog::exec, blocking menus)
    void runUntil(const std::function<bool()>& done);
    void processEvents();               // handle what is pending, without waiting (progress windows)
    // calls fn every frame (the playhead and meter refresh); returns an id
    int onFrame(std::function<void()> fn);
    void removeFrame(int id);
    // how often onFrame callbacks run: 60 / 30 / 15, or 0: as often as the screen shows a picture
    // (Options → Screen refresh). Windows are painted at the screen's rate whatever it is, so what
    // follows the mouse (a drag, a resize) moves with every picture the screen shows.
    void setFrameRate(int fps);
    int frameRate() const { return fps_; }
    double displayRate() const { return displayHz_; }  // the screen's pictures a second (60 if unknown)
    // (added) the interface size (1 = 100 %): set before windows are made (main(), from the settings);
    // windows made afterwards are drawn that much bigger (Window::scale())
    void setScale(float s);
    float scale() const { return scale_; }
    // (added) every key press and release of every window goes here first (QApplication's event filter:
    // the computer keyboard playing notes); return true to take it. press: KeyDown, else KeyUp.
    std::function<bool(Window* window, const KeyEvent& e, bool press)> keyFilter;
    // (added) none of the program's windows has the keyboard focus any more (another program took it)
    Signal<> deactivated;
    int toScreen(int v) const;          // a global logical coordinate, in screen pixels
    int fromScreen(int v) const;
    // one-shot / repeating timers on the main thread (QTimer)
    int singleShot(int ms, std::function<void()> fn);
    int every(int ms, std::function<void()> fn);
    void cancelTimer(int id);
    const std::vector<Window*>& windows() const { return windows_; }
    Window* activeWindow() const;
    Window* modalWindow() const;        // the topmost modal window shown, if any
    Window* mainWindow = nullptr;
    void repaintAll();                  // after a skin change
    // current drag (internal and from the desktop), for Window
    DragData* drag() { return dragActive_ ? &drag_ : nullptr; }
    void beginDrag(const DragData& d, Widget* source);
    // a drag that began outside the program's windows (a Windows plugin's, from its window, which its
    // Wine host holds the pointer for while the button is down): it follows the pointer, asked every
    // frame, and drops where the left button is let go
    void followPointerDrag(const DragData& d);
    // (tests) where the pointer is and which MouseButtons are held, instead of asking the system
    std::function<std::pair<Point, int>()> pointerForDrags;
    void endDrag();
    // tooltips
    void showToolTip(Point globalPos, const std::string& text, Widget* owner);
    void hideToolTip();
    Window* toolTipWindow() const { return tip_.get(); }
    // (added) the hint line: once the pointer has been over a widget for HINT_MS, the first line of its
    // tooltip (or its nearest parent's), with the window it is in; "" when the pointer leaves that window
    // or rests where there is none. Sent only while someone listens (a StatusBar following hints), and
    // only when it changes. The tooltip itself still shows as usual.
    static constexpr int HINT_MS = 300;
    Signal<Window*, std::string> hintChanged;
    // a widget being deleted: forget it everywhere
    void forget(Widget* w);
    // a widget leaving its window (released, hidden): drop hover / grab / focus pointers into it
    void detach(Widget* w);
    // popups (menus, drop-down lists): they get every key and the mouse first; a press outside all
    // of them closes them (dismiss is called; it should hide the popup and call closePopup)
    void openPopup(Window* popup, Widget* owner, std::function<void()> dismiss);
    void closePopup(Window* popup);
    void closeAllPopups();
    const std::vector<Window*>& popups() const { return popupStack_; }
    Widget* popupOwner(Window* popup) const;
    // layouts marked dirty by updateGeometry(): run them now (done before every frame)
    void doLayouts();
    void scheduleLayout(Widget* w);
    int loopLevel() const { return depth_; }
    Rect screenAt(Point globalPos) const;  // the available area of the screen there
    Point cursorPos() const;
    int grabEpoch_ = 0;                 // (internal) bumped when a popup / modal takes the mouse
    // (internal)
    void addWindow(Window* w);
    void removeWindow(Window* w);
    void collectGarbage();              // deletes widgets that asked deleteLater()
    std::vector<std::pair<Widget*, int>> trash;  // (widget, loop level when it asked)
    void tick();                        // one frame: paint dirty windows, run onFrame callbacks
    // drags in progress (internal, for Window)
    void dragMoveTo(Point globalPos, int mods);
    void dragFinish(Point globalPos, int mods, bool drop);
    Widget* dragTarget() const { return dragTarget_; }
    Window* windowAt(Point globalPos) const;  // the topmost shown window of ours there
    void noteActive(Window* w, bool on);
    std::vector<Shortcut*> allShortcuts;  // (registered by Shortcut)
    bool triggerShortcut(Window* from, const KeyEvent& e);
private:
    std::vector<Window*> windows_;
    std::map<int, std::function<void()>> frames_;
    int nextFrame_ = 0;
    int fps_ = 60;
    double displayHz_ = 60;
    double lastFrameFns_ = -1;  // when the onFrame callbacks last ran (frameTimer)
    float scale_ = 1;
    int frameTimer_ = -1;
    int followTimer_ = -1;  // (followPointerDrag)
    void startFrameTimer();
    void frameTimer();          // a picture of the screen: paints; runs the onFrame callbacks when due
    bool quit_ = false;
    DragData drag_;
    bool dragActive_ = false;
    Widget* dragSource_ = nullptr;
    std::unique_ptr<Window> tip_;
    Widget* tipOwner_ = nullptr;
    std::string tipText_;
    bool headless_ = false, inited_ = false;
    double clock_ = 0;                  // headless clock
    int depth_ = 0;                     // nested loops (exec, runUntil, processEvents)
    struct Timer { int plat = -1; std::function<void()> fn; bool repeat = false; double due = 0, interval = 0; };
    std::map<int, Timer> timers_;
    int nextTimer_ = 0;
    int addTimer(int ms, std::function<void()> fn, bool repeat);
    int addTimerMs(double ms, std::function<void()> fn, bool repeat);
    void fireTimer(int id);
    std::vector<Window*> popupStack_;
    std::vector<std::pair<Widget*, std::function<void()>>> popupInfo_;  // parallel: owner, dismiss
    std::vector<Widget*> pendingLayouts_;
    Widget* dragTarget_ = nullptr;
    bool dragAccepted_ = false;
    std::unique_ptr<Window> dragPic_;
    Window* active_ = nullptr;
    int tipTimer_ = -1;
    double tipHiddenAt_ = -10;
    Point lastCursor_;
    std::string clip_;                  // headless clipboard
    int showCounter_ = 0;
    friend class Window;
    friend class Widget;
    friend void setClipboard(const std::string&);
    friend std::string clipboard();
public:
    // tooltip timing (internal, for Window): the pointer rests on / leaves `w`
    void toolTipWake(Widget* w, Point globalPos);
    void toolTipLeave();
    // hint timing (internal, for Window): the pointer moved in / left `w` (see hintChanged)
    void hintWake(Window* w);
    void hintLeave(Window* w);
private:
    int hintTimer_ = -1;
    Window* hintWindow_ = nullptr;  // the window the hint last sent belongs to (null: none shown)
    std::string hintText_;
    void sendHint(Window* w, const std::string& text);
};

// the clipboard (text)
void setClipboard(const std::string& text);
std::string clipboard();

}  // namespace ui
