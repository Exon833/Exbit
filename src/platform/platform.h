// The platform layer: native windows, input events, the main loop (timers, file descriptors,
// work posted from other threads), the clipboard, cursors, file drops from the desktop, and a few
// desktop services (open a URL or folder, the settings folder). Implemented for Linux on Xlib
// (platform/x11.cpp) and for Windows on Win32 (platform/platform_win.cpp); everything above it is platform-neutral.
//
// Only the GUI (main) thread calls into this, except postToMain(), which any thread may call.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/geom.h"
#include "gfx/image.h"

namespace plat {

// --- keys: printable keys are their Unicode codepoint (letters as UPPERCASE 'A'…'Z', digits
// '0'…'9', ' ', '+', '-', …); the others are these (above the Unicode range) ---
enum Key : int {
    Key_None = 0,
    Key_Space = ' ',
    Key_Escape = 0x01000000, Key_Tab, Key_Backtab, Key_Backspace, Key_Return, Key_Enter, Key_Insert,
    Key_Delete, Key_Pause, Key_Print, Key_Home, Key_End, Key_Left, Key_Up, Key_Right, Key_Down,
    Key_PageUp, Key_PageDown, Key_Shift, Key_Control, Key_Meta, Key_Alt, Key_CapsLock, Key_Menu,
    Key_F1, Key_F2, Key_F3, Key_F4, Key_F5, Key_F6, Key_F7, Key_F8, Key_F9, Key_F10, Key_F11, Key_F12,
};

enum Modifier : int { ModNone = 0, ModShift = 1, ModCtrl = 2, ModAlt = 4, ModMeta = 8 };
enum MouseButton : int { NoButton = 0, LeftButton = 1, RightButton = 2, MiddleButton = 4 };

enum class Cursor { Arrow, IBeam, Wait, Cross, Hand, OpenHand, ClosedHand, SizeHor, SizeVer, SizeAll,
                    SizeFDiag, SizeBDiag, Forbidden, Blank };

struct Event {
    enum Type {
        None,
        MouseDown, MouseUp, MouseMove,  // x, y (window coords), button (the one that changed), buttons (held), mods
        Wheel,                          // x, y, dx, dy in notches (1.0 = one wheel click; up/left positive), mods
        KeyDown, KeyUp,                 // key, mods, autoRepeat; text = what it types (UTF-8), if anything
        Enter, Leave,                   // the pointer came in / went out
        FocusIn, FocusOut,              // the window got / lost the keyboard focus
        Resize,                         // w, h: the new client size (repaint follows)
        Move,                           // x, y: the client area's new screen position
        Expose,                         // the window must be redrawn (present again)
        Close,                          // the user asked to close it (title bar ✕)
        DragEnter, DragMove, DragLeave, Drop,  // files dragged from the desktop: x, y, files (paths)
        StateChanged,                   // the window manager maximized, restored, minimized it or made it full
                                        // screen (isMaximized(), isMinimized(), isFullScreen() say what it is now)
    };
    Type type = None;
    int x = 0, y = 0;      // window coordinates (Move: screen coordinates)
    int w = 0, h = 0;
    int button = 0;        // MouseButton that changed
    int buttons = 0;       // MouseButtons held
    int mods = 0;          // Modifiers held
    float dx = 0, dy = 0;  // wheel notches
    int key = 0;
    bool autoRepeat = false;
    std::string text;
    std::vector<std::string> files;
    double time = 0;       // seconds (monotonic), when it happened
};

class Window;

class WindowListener {
public:
    virtual ~WindowListener() = default;
    virtual void onEvent(Window* w, const Event& e) = 0;
};

struct WindowOptions {
    std::string title = "Exbit";
    int w = 800, h = 600;
    int x = -1, y = -1;           // client area's screen position; -1: centred on the screen (or on transientFor)
    bool resizable = true;
    bool popup = false;           // menus, tooltips, drop-downs: no frame, above everything, no taskbar entry
    bool dialog = false;          // a dialog / tool window: kept above transientFor
    Window* transientFor = nullptr;
    bool acceptsFiles = false;    // takes files dragged from the desktop (Drag…/Drop events)
    bool takesFocus = true;       // false: never takes the keyboard focus (tooltips)
    bool offscreen = false;       // mapped far outside every screen, never shown (photographing plugin windows)
    int minW = 0, minH = 0;
    std::string iconSvg;          // optional: the window's icon, as SVG text
    bool decorated = true;        // false: the window manager draws no frame around it (no title bar, no
                                  // border: the program draws its own, ui::Window's title bar)
    bool opaque = false;          // never see-through (a 24-bit window even while transparency is on)
};

// the edges of a window a resize drags (beginMoveResize): one, or two for a corner; none: a move
enum Edge : int { EdgeNone = 0, EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8 };

class Window {
public:
    // makes a window (not shown yet unless you call show()); events go to `listener`
    static std::unique_ptr<Window> create(const WindowOptions& opts, WindowListener* listener);
    virtual ~Window() = default;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool isVisible() const = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual void resize(int w, int h) = 0;               // client size
    virtual void move(int x, int y) = 0;                 // client area's screen position
    virtual void setMinimumSize(int w, int h) = 0;
    virtual void setResizable(bool resizable) = 0;
    virtual Rect geometry() const = 0;                   // client area, in screen coordinates
    virtual Rect frameGeometry() const = 0;              // with the window manager's frame
    virtual void raise() = 0;                            // to the front, and take the focus
    virtual void setMaximized(bool on) = 0;
    virtual bool isMaximized() const = 0;
    virtual void setFullScreen(bool on) = 0;
    virtual bool isFullScreen() const { return false; }
    virtual void minimize() {}                           // to the taskbar (iconified)
    virtual bool isMinimized() const { return false; }
    // with (false) or without the window manager's frame (WindowOptions::decorated), at once
    virtual void setDecorated(bool on) { (void)on; }
    // The window's shape: its corners cut round (radii in device pixels: top-left, top-right, bottom-right,
    // bottom-left), so a frame of the program's own with rounded corners shows the desktop there, not black. All 0:
    // the whole rectangle. Kept across resizes.
    virtual void setCornerRadii(int tl, int tr, int br, int bl) { (void)tl, (void)tr, (void)br, (void)bl; }
    // The window manager moves the window (edges: EdgeNone) or resizes it by those edges, following the
    // pointer while `button` (a MouseButton) is held, as when its own title bar or border is dragged: it
    // snaps and tiles it. screenPos: the pointer, in screen coordinates. Call it from a press (or a move with
    // the button held); the program gets no more events of that button. False if the window manager can't
    // (no _NET_WM_MOVERESIZE): then the program moves or resizes it itself.
    virtual bool beginMoveResize(int edges, Point screenPos, int button) {
        (void)edges, (void)screenPos, (void)button;
        return false;
    }
    // A window inside this one covering `area` (client coordinates), for a plugin's view to be put into
    // (instead of nativeHandle(), when the program draws something of its own around it: its title bar):
    // made on the first call (shown), moved and sized on the later ones; an empty area hides it (until the
    // next call with one). Its native handle. What is embedded in it is treated as embedded in this window
    // (XEmbed's focus); its size is the program's to keep (the plugin asks for one through its own API).
    virtual uintptr_t embedArea(const Rect& area) { (void)area; return nativeHandle(); }
    virtual bool hasFocus() const = 0;
    virtual void setCursor(Cursor c) = 0;
    // shows `fb` (the size of the client area) on screen; only `dirty` needs updating
    virtual void present(const Image& fb, const Rect& dirty) = 0;
    // pointer grab for popups: every mouse event comes to this window until released
    virtual void grabPointer(bool on) = 0;
    // keys (as Event::key gives them; pressed without Shift, Ctrl, Alt or Meta) that come to this
    // window even while a window inside it has the focus: an embedded plugin's window (the computer
    // keyboard's notes); empty: none. Replaces the keys given before
    virtual void grabKeys(const std::vector<int>& keys) { (void)keys; }
    // the native handle (X11: the Window id; Windows: the HWND), e.g. to embed a plugin's window
    virtual uintptr_t nativeHandle() const = 0;
    // the native display connection (X11: Display*), for plugin windows that need it
    virtual void* nativeDisplay() const = 0;
    // a picture of what the window shows, including child windows (plugin editors); false if it can't
    virtual bool capture(Image& out) = 0;

    // --- transparency (see compositing() and setTransparency() below) ---
    // true while the alpha of what present() shows is the window's own: where its framebuffer is see-through
    // (premultiplied alpha below 255), the desktop shows. False for windows made without transparency (24-bit,
    // opaque: alpha is ignored, the corners are cut by setCornerRadii's shape) and while no compositing manager
    // runs or transparency is off (a see-through window made earlier is then shown opaque, as over black)
    virtual bool hasAlpha() const { return false; }
    // where clicks reach the window (device pixels, its own coordinates; the corners given by setCornerRadii cut
    // round while hasAlpha()); elsewhere they go to what is under it (a popup's drop shadow). Empty: everywhere
    virtual void setInputArea(const Rect& area) { (void)area; }
    // the whole window's opacity, 0…1 (_NET_WM_WINDOW_OPACITY: the compositing manager fades all of it)
    virtual void setOpacity(float opacity) { (void)opacity; }
    // asks the compositing manager to blur what is behind the window (KWin's _KDE_NET_WM_BLUR_BEHIND_REGION): in
    // `area` (device pixels; empty: the input area, else the whole window; corners cut round where they are the
    // window's), or not at all (on false). Only while hasAlpha(): an opaque window hides what is behind it anyway
    virtual void setBlurBehind(bool on, const Rect& area) { (void)on, (void)area; }
};

// --- the main loop ---
void init(int argc, char** argv);      // connects to the display; call first
// the loop's timers, watched fds and posted work without a display (exbit --tui); instead of init()
void initWithoutDisplay();
void shutdown();
// handles events, timers, watched fds and posted work; waits at most timeoutMs (-1: until something happens)
void processEvents(int timeoutMs);
// runs until quit()
void run();
void quit();
bool quitting();

int addTimer(int intervalMs, std::function<void()> fn, bool repeat = true);  // returns an id
// the same with a fractional interval (a frame of a 60 Hz screen is 16.67 ms, not 16)
int addTimerMs(double intervalMs, std::function<void()> fn, bool repeat = true);
// how many pictures a second the fastest screen shows (its mode's refresh rate); 0 if unknown
double displayRefreshRate();
void removeTimer(int id);
// fd readable (VST3 plugins' IRunLoop on Linux): fn runs on the main thread
int watchFd(int fd, std::function<void()> fn);
void unwatchFd(int id);
// any thread: run fn on the main thread soon (wakes the loop)
void postToMain(std::function<void()> fn);
bool isMainThread();

double now();             // monotonic seconds
double doubleClickTime(); // seconds between two clicks that make a double-click (0.4)
// the desktop's own display scale, in percent (Windows: its "Scale" setting, 150 at 144 dpi; X11: 100): the
// interface size the program starts with until one is chosen
int systemScale();
int doubleClickDistance();

// --- screens ---
Rect screenGeometry(Point near);     // the available area of the screen that holds `near`
Rect primaryScreenGeometry();
Point cursorPos();                   // screen coordinates
int heldButtons();                   // MouseButtons held now
int heldModifiers();                 // Modifiers held now (e.g. Alt while dragging)

// --- clipboard ---
void setClipboardText(const std::string& text);
std::string clipboardText();

// --- desktop ---
std::string configDir();             // where settings are kept: ~/.config/Exbit (Windows: %APPDATA%\Exbit), made if missing
std::string homeDir();
std::string executableDir();         // the folder of the running program
std::string dataDir();               // the program's data (skins/, icons/): next to it (or ../share/exbit)
bool openUrl(const std::string& url);
bool openFolder(const std::string& path);
void beep();

// --- transparency ---
// a compositing manager runs (X11: the _NET_WM_CM_S<screen> selection has an owner); followed while it starts and
// stops (setCompositingListener)
bool compositing();
// Windows made while it is on and compositing() is true are see-through where their framebuffer is (32-bit, with
// their own alpha: Window::hasAlpha); off, they are made as before (24-bit, opaque). Turning it off shows the
// see-through ones opaque at once; turning it on again (or a compositing manager starting) makes them see-through
// again; windows made while it was off stay opaque until they are made again. On by default.
void setTransparency(bool on);
bool transparency();
// called on the main thread after compositing() or transparency() changed (one listener: the toolkit's App)
void setCompositingListener(std::function<void()> fn);

// --- additions (platform/x11.cpp) ---
// false when init() could not connect to a display (no windows then; timers and posted work still run)
bool hasDisplay();
// runs fn on the main thread at the next loop iteration (from the main thread: after the current event)
inline void callLater(std::function<void()> fn) { postToMain(std::move(fn)); }

}  // namespace plat
