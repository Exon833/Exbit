// The platform layer: native windows, input events, the main loop (timers, file descriptors,
// work posted from other threads), the clipboard, cursors, file drops from the desktop, and a few
// desktop services (open a URL or folder, the settings folder). Implemented for Linux on Xlib
// (platform/x11.cpp); everything above it is platform-neutral.
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
    std::string title = "Tiny DAW";
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
};

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
};

// --- the main loop ---
void init(int argc, char** argv);      // connects to the display; call first
void shutdown();
// handles events, timers, watched fds and posted work; waits at most timeoutMs (-1: until something happens)
void processEvents(int timeoutMs);
// runs until quit()
void run();
void quit();
bool quitting();
// how many nested loops are running inside run() (dialogs' exec()); 0 when only run() runs
int loopDepth();

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
std::string configDir();             // where settings are kept: ~/.config/TinyDAW (Windows: %APPDATA%\TinyDAW), made if missing
std::string homeDir();
std::string executableDir();         // the folder of the running program
std::string dataDir();               // the program's data (skins/, icons/): next to it (or ../share/tinydaw)
bool openUrl(const std::string& url);
bool openFolder(const std::string& path);
void beep();

// --- additions (platform/x11.cpp) ---
// false when init() could not connect to a display (no windows then; timers and posted work still run)
bool hasDisplay();
// events for windows this layer does not know (e.g. windows a plugin made on our own Display):
// `fn` gets the native event (X11: XEvent*) and returns true if it handled it
void setNativeEventFilter(std::function<bool(void* nativeEvent)> fn);
// runs fn on the main thread at the next loop iteration (from the main thread: after the current event)
inline void callLater(std::function<void()> fn) { postToMain(std::move(fn)); }

}  // namespace plat
