// The platform layer on Xlib (Linux): windows, input, the main loop, clipboard, file drops (XDND),
// screens, cursors and a few desktop services. See platform.h for the contract.
//
// Notes
// - Windows use the default 24-bit TrueColor visual (opaque windows: the ARGB framebuffer's alpha
//   byte is ignored). present() copies the dirty rectangle into an MIT-SHM image and sends it with
//   XShmPutImage (XPutImage when shared memory isn't available, e.g. a remote display).
// - Transparency: while a compositing manager runs (the _NET_WM_CM_S<screen> selection has an owner,
//   followed through the MANAGER message on the root window and the owner window's DestroyNotify) and
//   setTransparency is on, top-level windows (not offscreen ones, not WindowOptions::opaque) are made
//   with a 32-bit TrueColor visual and a colormap of their own (border pixel 0, no background pixmap).
//   The framebuffer is premultiplied ARGB32, which is what such a visual takes on a little-endian
//   machine with masks 0xFF0000 / 0xFF00 / 0xFF (alpha the top byte): present() copies it as it is
//   (XShmPutImage at depth 32). Their rounded corners are then left to the alpha (no bounding shape;
//   an input shape cuts them for clicks). If the compositing manager stops, or transparency is turned
//   off, the 32-bit windows stay (they aren't made again: a plugin's view may be inside) and are shown
//   opaque: present() makes every pixel's alpha 255 (as 24-bit windows show it), the 1-bit shape cuts
//   the corners again; when it starts again they are see-through again. Windows made while there was
//   none stay 24-bit until they are made again. The embedArea() window (a plugin's view) is always
//   24-bit, with the default visual and colormap, inside a 32-bit window too.
// - Offscreen windows (photographing plugin editors) are override-redirect windows mapped at
//   (-20000, -20000): outside every screen, unmanaged, never shown. XGetImage refuses rectangles
//   outside the screen on a plain X server (BadMatch), but under XWayland every top-level window
//   is redirected to its own pixmap, so capture() reads the pixels from it. When the window's
//   own XGetImage fails, capture() asks the Composite extension (raw protocol: NameWindowPixmap)
//   for the window's pixmap and reads that.
// - Plugins may embed their editors as child windows (nativeHandle()/nativeDisplay()), on our
//   Display or their own connections. Events for windows we don't know are ignored. X errors
//   are logged, never fatal.
#include "platform/platform.h"
#include "gfx/svg.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// plat::Event's type names that X11's headers #define as macros (None, Expose, FocusIn, FocusOut)
namespace {
constexpr plat::Event::Type EvNone = plat::Event::None;
constexpr plat::Event::Type EvExpose = plat::Event::Expose;
constexpr plat::Event::Type EvFocusIn = plat::Event::FocusIn;
constexpr plat::Event::Type EvFocusOut = plat::Event::FocusOut;
}  // namespace

#include "platform/x11_api.h"  // (Xlib, its extensions: loaded when the display is opened)
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

namespace plat {
TINYDAW_X11(TINYDAW_X11_USE)
namespace {

using XWin = ::Window;

// ---------------------------------------------------------------------------------------------
// state

struct Atoms {
    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, NET_WM_PING, NET_WM_NAME, NET_WM_ICON_NAME, UTF8_STRING,
        NET_WM_PID, NET_WM_WINDOW_TYPE, TYPE_NORMAL, TYPE_DIALOG, TYPE_POPUP_MENU, TYPE_DROPDOWN_MENU,
        TYPE_TOOLTIP, TYPE_UTILITY, NET_WM_STATE, STATE_MAX_V, STATE_MAX_H, STATE_FULLSCREEN,
        STATE_SKIP_TASKBAR, STATE_SKIP_PAGER, NET_ACTIVE_WINDOW, NET_WM_ICON, NET_FRAME_EXTENTS,
        NET_WORKAREA, NET_CURRENT_DESKTOP, NET_WM_USER_TIME, CLIPBOARD, TARGETS, TEXT, TEXT_PLAIN_UTF8,
        INCR, TINYDAW_SEL, XdndAware, XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop,
        XdndFinished, XdndSelection, XdndTypeList, XdndActionCopy, URI_LIST, MOTIF_WM_HINTS, CLIPBOARD_MANAGER,
        SAVE_TARGETS, XEMBED, NET_WM_MOVERESIZE, STATE_HIDDEN, NET_SUPPORTED, NET_WM_WINDOW_OPACITY,
        KDE_BLUR_BEHIND, MANAGER;
};

struct Timer {
    double intervalMs = 0;
    double deadline = 0;
    bool repeat = true;
    std::shared_ptr<std::function<void()>> fn;
};

struct Watch {
    int fd = -1;
    bool invalid = false;
    std::shared_ptr<std::function<void()>> fn;
};

class XWindow;

struct State {
    Display* dpy = nullptr;
    int screen = 0;
    XWin root = 0;
    Visual* visual = nullptr;
    int depth = 24;
    Colormap cmap = 0;
    bool ownCmap = false;
    Atoms a{};
    bool shm = false;
    int shmCompletion = -1;  // the ShmCompletion event type
    int compositeOpcode = 0; // the Composite extension's major opcode (0: none)
    bool xwayland = false;   // the display is XWayland (every top-level window has its own pixmap)
    // transparency (see the notes above)
    Visual* argbVisual = nullptr;  // a 32-bit TrueColor visual with alpha in the top byte (null: none)
    Colormap argbCmap = 0;
    Atom cmAtom = 0;               // _NET_WM_CM_S<screen>
    XWin cmOwner = 0;              // its owner: the compositing manager (0: none)
    bool compositing = false;
    bool transparency = true;
    std::function<void()> compositingListener;
    XIM im = nullptr;
    XIMStyle imStyle = 0;
    XWin helper = 0;         // owns the clipboard, receives selections
    bool detectableRepeat = false;
    unsigned keymapChanges = 0;  // (MappingNotify: keys grabbed by their codes are grabbed again)
    std::unordered_map<XWin, XWindow*> windows;

    std::map<int, Timer> timers;
    int nextTimer = 1;
    std::map<int, Watch> watches;
    int nextWatch = 1;
    std::mutex postMutex;
    std::vector<std::function<void()>> posted;
    int wakeFd = -1;
    std::atomic<bool> quit{false};
    int processDepth = 0;
    std::thread::id mainThread = std::this_thread::get_id();

    std::string clipText;
    bool ownClipboard = false;
    Time lastTime = CurrentTime;     // the last server time seen
    Time lastInputTime = 0;          // of the last key / button press
    double xTimeOffset = 1e300;      // now() - serverTime/1000, smallest seen
    std::map<int, ::Cursor> cursors;
    std::vector<XWin> pendingExpose;
    std::unordered_map<unsigned, int> keyOfCode;
    std::unordered_set<unsigned> keysDown;
    unsigned repeatKeycode = 0;      // the KeyPress that follows a swallowed auto-repeat KeyRelease
    int trapDepth = 0;
    int trappedError = 0;
    int loggedErrors = 0;

    // screens (cached a moment: XRRGetMonitors is a round trip)
    std::vector<Rect> monitors;
    int primaryMonitor = 0;
    double monitorsAt = -10;
};

State& S() {
    static State s;
    return s;
}

double monoNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------------------------
// errors: logged, never fatal (plugins' windows come and go: BadWindow, BadDrawable…)

int onXError(Display* d, XErrorEvent* e) {
    State& s = S();
    if (s.trapDepth > 0) {
        s.trappedError = e->error_code;
        return 0;
    }
    s.loggedErrors++;
    if (s.loggedErrors <= 30 || s.loggedErrors % 500 == 0) {
        char text[160] = "";
        XGetErrorText(d, e->error_code, text, sizeof text);
        fprintf(stderr, "Tiny DAW: X error: %s (request %d.%d, resource 0x%lx)%s\n", text, e->request_code,
                e->minor_code, (unsigned long)e->resourceid,
                s.loggedErrors == 30 ? " (further errors are counted quietly)" : "");
    }
    return 0;
}

int onXIOError(Display*) {
    fprintf(stderr, "Tiny DAW: the connection to the display was lost\n");
    _exit(1);
    return 0;
}

// catches the X errors of the requests between begin and end
struct ErrorTrap {
    ErrorTrap() {
        XSync(S().dpy, False);
        S().trapDepth++;
        S().trappedError = 0;
    }
    int end() {
        XSync(S().dpy, False);
        S().trapDepth--;
        return S().trappedError;
    }
};

// ---------------------------------------------------------------------------------------------
// small helpers

int modsFromState(unsigned state) {
    int m = 0;
    if (state & ShiftMask) m |= ModShift;
    if (state & ControlMask) m |= ModCtrl;
    if (state & Mod1Mask) m |= ModAlt;
    if (state & Mod4Mask) m |= ModMeta;
    return m;
}

int buttonsFromState(unsigned state) {
    int b = 0;
    if (state & Button1Mask) b |= LeftButton;
    if (state & Button2Mask) b |= MiddleButton;
    if (state & Button3Mask) b |= RightButton;
    return b;
}

int buttonOf(unsigned xbutton) {
    switch (xbutton) {
        case 1: return LeftButton;
        case 2: return MiddleButton;
        case 3: return RightButton;
        default: return NoButton;
    }
}

Time eventTime(const XEvent& ev) {
    switch (ev.type) {
        case KeyPress: case KeyRelease: return ev.xkey.time;
        case ButtonPress: case ButtonRelease: return ev.xbutton.time;
        case MotionNotify: return ev.xmotion.time;
        case EnterNotify: case LeaveNotify: return ev.xcrossing.time;
        case PropertyNotify: return ev.xproperty.time;
        case SelectionNotify: return ev.xselection.time;
        case SelectionRequest: return ev.xselectionrequest.time;
        case SelectionClear: return ev.xselectionclear.time;
        default: return 0;
    }
}

// the server time (ms) as plat::now() seconds
double eventSeconds(Time t) {
    double n = monoNow();
    if (!t) return n;
    State& s = S();
    double off = n - t / 1000.0;
    if (off < s.xTimeOffset) s.xTimeOffset = off;
    double r = t / 1000.0 + s.xTimeOffset;
    return std::min(r, n);
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
}

// the first codepoint of UTF-8 text (0 if none); `len` gets its byte length
uint32_t firstCodepoint(const std::string& s, size_t* len = nullptr) {
    if (s.empty()) return 0;
    unsigned char c = s[0];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 31; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 15; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 7; n = 4; }
    else { if (len) *len = 1; return 0xFFFD; }
    if (s.size() < n) { if (len) *len = s.size(); return 0xFFFD; }
    for (size_t i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 63);
    if (len) *len = n;
    return cp;
}

std::string latin1ToUtf8(const char* p, int n) {
    std::string out;
    for (int i = 0; i < n; i++) appendUtf8(out, (unsigned char)p[i]);
    return out;
}

std::string utf8ToLatin1(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t n = 1;
        uint32_t c = firstCodepoint(s.substr(i, 4), &n);
        out += c < 256 ? (char)c : '?';
        i += n ? n : 1;
    }
    return out;
}

// drops control characters (Ctrl+A gives "\x01"): text is what a key types
std::string printableText(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7F) out += (char)c;
    return out;
}

uint32_t upperCodepoint(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 128) return c;
    return (uint32_t)std::towupper((wint_t)c);
}

// platform.h's key code for an X keysym (0 if it's no key we name)
int keyFromKeysym(KeySym ks) {
    switch (ks) {
        case XK_Escape: return Key_Escape;
        case XK_Tab: return Key_Tab;
        case XK_ISO_Left_Tab: return Key_Backtab;
        case XK_BackSpace: return Key_Backspace;
        case XK_Return: return Key_Return;
        case XK_KP_Enter: return Key_Enter;
        case XK_Insert: case XK_KP_Insert: return Key_Insert;
        case XK_Delete: case XK_KP_Delete: return Key_Delete;
        case XK_Pause: case XK_Break: return Key_Pause;
        case XK_Print: case XK_Sys_Req: return Key_Print;
        case XK_Home: case XK_KP_Home: return Key_Home;
        case XK_End: case XK_KP_End: return Key_End;
        case XK_Left: case XK_KP_Left: return Key_Left;
        case XK_Up: case XK_KP_Up: return Key_Up;
        case XK_Right: case XK_KP_Right: return Key_Right;
        case XK_Down: case XK_KP_Down: return Key_Down;
        case XK_Prior: case XK_KP_Prior: return Key_PageUp;
        case XK_Next: case XK_KP_Next: return Key_PageDown;
        case XK_Shift_L: case XK_Shift_R: return Key_Shift;
        case XK_Control_L: case XK_Control_R: return Key_Control;
        case XK_Super_L: case XK_Super_R: case XK_Meta_L: case XK_Meta_R: case XK_Hyper_L: case XK_Hyper_R:
            return Key_Meta;
        case XK_Alt_L: case XK_Alt_R: return Key_Alt;
        case XK_Caps_Lock: return Key_CapsLock;
        case XK_Menu: return Key_Menu;
        case XK_KP_Space: return ' ';
        case XK_KP_Tab: return Key_Tab;
        case XK_KP_Equal: return '=';
        case XK_KP_Multiply: return '*';
        case XK_KP_Add: return '+';
        case XK_KP_Separator: return ',';
        case XK_KP_Subtract: return '-';
        case XK_KP_Decimal: return '.';
        case XK_KP_Divide: return '/';
        default: break;
    }
    if (ks >= XK_F1 && ks <= XK_F12) return Key_F1 + (int)(ks - XK_F1);
    if (ks >= XK_KP_0 && ks <= XK_KP_9) return '0' + (int)(ks - XK_KP_0);
    if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) return (int)upperCodepoint((uint32_t)ks);
    if ((ks & 0xFF000000) == 0x01000000) return (int)upperCodepoint((uint32_t)(ks & 0x00FFFFFF));
    return 0;
}

int modifierOfKey(int key) {
    switch (key) {
        case Key_Shift: return ModShift;
        case Key_Control: return ModCtrl;
        case Key_Alt: return ModAlt;
        case Key_Meta: return ModMeta;
        default: return 0;
    }
}

// waits until fd is readable or `seconds` passed
void waitReadable(int fd, double seconds) {
    if (seconds < 0) seconds = 0;
    pollfd p{fd, POLLIN, 0};
    timespec ts{(time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9)};
    ppoll(&p, 1, &ts, nullptr);
}

bool readProperty(XWin w, Atom prop, Atom& type, int& format, std::vector<unsigned char>& data, bool del = false,
                  Atom reqType = AnyPropertyType) {
    data.clear();
    unsigned char* p = nullptr;
    unsigned long n = 0, after = 0;
    type = 0;
    format = 0;
    int r = XGetWindowProperty(S().dpy, w, prop, 0, LONG_MAX / 4, del ? True : False, reqType, &type, &format, &n,
                               &after, &p);
    if (r != Success) return false;
    if (p) {
        // format 32 data comes as longs
        size_t unit = format == 32 ? sizeof(long) : format == 16 ? sizeof(short) : 1;
        data.assign(p, p + n * unit);
        XFree(p);
    }
    return type != 0;
}

std::vector<long> readLongs(XWin w, Atom prop, Atom reqType = AnyPropertyType) {
    Atom type;
    int format;
    std::vector<unsigned char> data;
    std::vector<long> out;
    if (!readProperty(w, prop, type, format, data, false, reqType) || format != 32) return out;
    out.resize(data.size() / sizeof(long));
    if (!out.empty()) memcpy(out.data(), data.data(), out.size() * sizeof(long));  // (an empty property: no data)
    return out;
}

// asks the owner of `selection` for it as `target` (into requestor's TINYDAW_SEL property) and
// waits up to timeoutMs, handling the INCR protocol for big transfers. Other events stay queued.
bool convertSelection(XWin requestor, Atom selection, Atom target, Time t, int timeoutMs, std::string& out,
                      Atom* gotType = nullptr);

void handleSelectionRequest(const XSelectionRequestEvent& r);

// ---------------------------------------------------------------------------------------------
// cursors

::Cursor cursorFor(Cursor c) {
    State& s = S();
    auto it = s.cursors.find((int)c);
    if (it != s.cursors.end()) return it->second;
    ::Cursor xc = 0;
    if (c == Cursor::Blank) {
        static char bits[1] = {0};
        Pixmap pm = XCreateBitmapFromData(s.dpy, s.root, bits, 1, 1);
        XColor black{};
        xc = XCreatePixmapCursor(s.dpy, pm, pm, &black, &black, 0, 0);
        XFreePixmap(s.dpy, pm);
    } else {
        struct Names { const char* names[4]; unsigned font; };
        static const Names table[] = {
            {{"left_ptr", "default", "arrow", nullptr}, XC_left_ptr},                     // Arrow
            {{"xterm", "text", "ibeam", nullptr}, XC_xterm},                              // IBeam
            {{"watch", "wait", "progress", nullptr}, XC_watch},                           // Wait
            {{"crosshair", "cross", "tcross", nullptr}, XC_crosshair},                    // Cross
            {{"pointer", "hand2", "pointing_hand", "hand1"}, XC_hand2},                   // Hand
            {{"openhand", "grab", "fleur", nullptr}, XC_hand1},                           // OpenHand
            {{"closedhand", "grabbing", "dnd-move", nullptr}, XC_fleur},                  // ClosedHand
            {{"sb_h_double_arrow", "ew-resize", "size_hor", "h_double_arrow"}, XC_sb_h_double_arrow},  // SizeHor
            {{"sb_v_double_arrow", "ns-resize", "size_ver", "v_double_arrow"}, XC_sb_v_double_arrow},  // SizeVer
            {{"fleur", "size_all", "move", "all-scroll"}, XC_fleur},                      // SizeAll
            {{"size_fdiag", "nwse-resize", "bd_double_arrow", nullptr}, XC_bottom_right_corner},  // SizeFDiag
            {{"size_bdiag", "nesw-resize", "fd_double_arrow", nullptr}, XC_bottom_left_corner},   // SizeBDiag
            {{"not-allowed", "forbidden", "crossed_circle", "circle"}, XC_X_cursor},      // Forbidden
        };
        int i = (int)c;
        if (i >= 0 && i < (int)(sizeof table / sizeof table[0])) {
            for (const char* n : table[i].names) {
                if (!n) break;
                xc = XcursorLibraryLoadCursor(s.dpy, n);
                if (xc) break;
            }
            if (!xc) xc = XCreateFontCursor(s.dpy, table[i].font);
        }
    }
    s.cursors[(int)c] = xc;
    return xc;
}

// ---------------------------------------------------------------------------------------------
// Composite's NameWindowPixmap, spoken directly (libXcomposite isn't linked): a pixmap of a
// redirected window's contents, which XGetImage reads wherever the window is.

// CompositeQueryVersion (the server wants it before the other requests); true if 0.2 or later
bool compositeQueryVersion() {
    State& s = S();
    Display* dpy = s.dpy;
    struct Req {
        CARD8 reqType;
        CARD8 compositeReqType;
        CARD16 length;
        CARD32 majorVersion;
        CARD32 minorVersion;
    };
    struct Rep {
        BYTE type;
        BYTE pad1;
        CARD16 sequenceNumber;
        CARD32 length;
        CARD32 majorVersion;
        CARD32 minorVersion;
        CARD32 pad2, pad3, pad4, pad5;
    } rep;
    LockDisplay(dpy);
    Req* req = (Req*)_XGetRequest(dpy, 0, sizeof(Req));
    req->reqType = (CARD8)s.compositeOpcode;
    req->compositeReqType = 0;
    req->majorVersion = 0;
    req->minorVersion = 4;
    Status ok = _XReply(dpy, (xReply*)&rep, 0, xTrue);
    UnlockDisplay(dpy);
    SyncHandle();
    return ok && (rep.majorVersion > 0 || rep.minorVersion >= 2);
}

// CompositeRedirectWindow, automatic: the server keeps the window's contents in a pixmap of its
// own (and still draws it as usual), so an offscreen window keeps its pixels on a plain X server
void compositeRedirectWindow(XWin w) {
    State& s = S();
    Display* dpy = s.dpy;
    struct Req {
        CARD8 reqType;
        CARD8 compositeReqType;
        CARD16 length;
        CARD32 window;
        CARD8 update;
        CARD8 pad1;
        CARD16 pad2;
    };
    LockDisplay(dpy);
    Req* req = (Req*)_XGetRequest(dpy, 1, sizeof(Req));
    req->reqType = (CARD8)s.compositeOpcode;
    req->compositeReqType = 1;
    req->window = (CARD32)w;
    req->update = 0;  // CompositeRedirectAutomatic
    req->pad1 = 0;
    req->pad2 = 0;
    UnlockDisplay(dpy);
    SyncHandle();
}

Pixmap nameWindowPixmap(XWin w) {
    State& s = S();
    if (!s.compositeOpcode) return 0;
    Display* dpy = s.dpy;
    Pixmap pm = XAllocID(dpy);
    struct Req {
        CARD8 reqType;
        CARD8 compositeReqType;
        CARD16 length;
        CARD32 window;
        CARD32 pixmap;
    };
    LockDisplay(dpy);
    Req* req = (Req*)_XGetRequest(dpy, 6 /* X_CompositeNameWindowPixmap */, sizeof(Req));
    req->reqType = (CARD8)s.compositeOpcode;
    req->compositeReqType = 6;
    req->window = (CARD32)w;
    req->pixmap = (CARD32)pm;
    UnlockDisplay(dpy);
    SyncHandle();
    return pm;
}

// ---------------------------------------------------------------------------------------------
// windows

class XWindow : public Window {
public:
    XWindow(const WindowOptions& o, WindowListener* l);
    ~XWindow() override;

    void show() override;
    void hide() override;
    // (centred on its parent or the screen) on the screen that holds its middle: its top-left corner at least
    void keepOnScreen() {
        Rect scr = screenGeometry(Point(x_ + w_ / 2, y_ + h_ / 2));
        x_ = std::max(scr.x, std::min(x_, scr.right() - w_));
        y_ = std::max(scr.y, std::min(y_, scr.bottom() - h_));
    }
    bool isVisible() const override { return visible_; }
    void setTitle(const std::string& title) override;
    void resize(int w, int h) override;
    void move(int x, int y) override;
    void setMinimumSize(int w, int h) override;
    void setResizable(bool r) override;
    Rect geometry() const override { return Rect(x_, y_, w_, h_); }
    Rect frameGeometry() const override;
    void raise() override;
    void setMaximized(bool on) override;
    bool isMaximized() const override { return maximized_; }
    void setFullScreen(bool on) override;
    bool isFullScreen() const override { return fullscreen_; }
    void minimize() override;
    bool isMinimized() const override { return minimized_; }
    void setDecorated(bool on) override;
    void setCornerRadii(int tl, int tr, int br, int bl) override;
    bool beginMoveResize(int edges, Point screenPos, int button) override;
    uintptr_t embedArea(const Rect& area) override;
    bool hasFocus() const override { return focus_; }
    void setCursor(Cursor c) override;
    void present(const Image& fb, const Rect& dirty) override;
    void grabPointer(bool on) override;
    void grabKeys(const std::vector<int>& keys) override;
    uintptr_t nativeHandle() const override { return (uintptr_t)win_; }
    void* nativeDisplay() const override { return S().dpy; }
    bool capture(Image& out) override;
    bool hasAlpha() const override { return argb_ && S().compositing && S().transparency; }
    void setInputArea(const Rect& area) override;
    void setOpacity(float opacity) override;
    void setBlurBehind(bool on, const Rect& area) override;
    // compositing or transparency changed: the shapes (bounding or input) and the blur as hasAlpha() says now
    void alphaChanged() { applyShape(); }

    // handles one X event for this window (it may be destroyed by the listener: emit last)
    void handle(XEvent& ev);
    void shmDone() { shmPending_ = false; }
    void deliver(Event& e) { emit(e); }
    XWin id() const { return win_; }

private:
    XWin win_ = 0;
    WindowListener* listener_ = nullptr;
    WindowOptions opts_;
    GC gc_ = nullptr;
    XIC ic_ = nullptr;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    int minW_ = 0, minH_ = 0;
    bool resizable_ = true;
    bool visible_ = false, mapped_ = false, shownOnce_ = false, moved_ = false;
    bool focus_ = false, maximized_ = false, fullscreen_ = false, minimized_ = false;
    int radii_[4] = {0, 0, 0, 0};  // setCornerRadii (device pixels)
    bool shaped_ = false;          // a bounding shape is set on the window
    bool inputShaped_ = false;     // an input shape is
    void applyShape();             // (the bounding or input shape, and the blur's region)
    bool argb_ = false;            // made with the 32-bit visual (see-through while hasAlpha())
    Visual* visual_ = nullptr;
    int depth_ = 24;
    Rect inputArea_;               // setInputArea (empty: all of it)
    bool blurOn_ = false;          // setBlurBehind
    Rect blurArea_;
    std::vector<long> blurSent_;   // the blur region as last set (blurSet_: set at all)
    bool blurSet_ = false;
    float opacity_ = 1;
    bool focusOnMap_ = false;
    XWin embed_ = 0;              // embedArea(): the window plugins' views go into (0: none, they go into win_)
    Rect embedRect_;
    bool embedShown_ = false;
    bool grabbed_ = false;
    bool kbdGrabbed_ = false;     // a popup that takes the keys (menus): keyboard grab, see MapNotify
    XWin embedFocus_ = 0;         // an embedded window (XEmbed) that asked for the keys: see handleClientMessage
    std::vector<int> grabbedKeys_;          // grabKeys: as given
    std::vector<KeyCode> grabbedKeycodes_;  // and as grabbed
    unsigned grabbedKeymap_ = 0;            // (State::keymapChanges then)
    std::vector<KeyCode> grabbedDown_;      // grabbed keys held: the whole keyboard is ours meanwhile
    bool keyboardHeld_ = false;             // (XGrabKeyboard for them)
    bool pointerInside_ = false;
    int reportedW_ = 0, reportedH_ = 0;  // the size the listener last heard of (Resize)
    long frame_[4] = {0, 0, 0, 0};  // left, right, top, bottom
    Cursor cursor_ = Cursor::Arrow;

    // the back buffer present() puts on screen
    XImage* img_ = nullptr;
    XShmSegmentInfo shmInfo_{};
    bool imgShm_ = false;
    bool shmPending_ = false;
    bool fastCopy_ = false;

    // a drag from the desktop (XDND)
    struct Dnd {
        XWin source = 0;
        int version = 0;
        bool uriList = false;
        bool entered = false;
        bool fetched = false;
        std::vector<std::string> files;
        int x = 0, y = 0;
        Time time = CurrentTime;
    } dnd_;

    void emit(Event& e) {
        if (!e.time) e.time = monoNow();
        listener_->onEvent(this, e);
    }
    void applySizeHints();
    void setNetState(Atom a1, Atom a2, bool on);
    void readNetState(bool tell = false);
    void readFrameExtents();
    void setMotifDecorations(bool on);
    void updatePosition();
    bool ensureImage(int w, int h);
    void freeImage();
    void waitShm();
    void handleClientMessage(XClientMessageEvent& cm);
    XWin embedded();
    void focusEmbedded();
    void deactivateEmbedded();
    void handleKey(XKeyEvent& k, bool press);
    void noteKey(XKeyEvent& k, bool press);
    void forgetHeldKeys();
    void sendDndStatus(bool accept);
    void sendDndFinished(bool accepted);
    bool fetchDndFiles();
};

std::vector<std::string> parseUriList(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 7, "file://") != 0) continue;
        std::string rest = line.substr(7);
        // file://host/path: skip the host (localhost or our own name)
        size_t slash = rest.find('/');
        if (slash == std::string::npos) continue;
        rest = rest.substr(slash);
        std::string path;
        for (size_t k = 0; k < rest.size(); k++) {
            char c = rest[k];
            if (c == '%' && k + 2 < rest.size() && isxdigit((unsigned char)rest[k + 1]) &&
                isxdigit((unsigned char)rest[k + 2])) {
                path += (char)strtol(rest.substr(k + 1, 2).c_str(), nullptr, 16);
                k += 2;
            } else {
                path += c;
            }
        }
        if (!path.empty()) out.push_back(path);
    }
    return out;
}

XWindow::XWindow(const WindowOptions& o, WindowListener* l) : listener_(l), opts_(o) {
    State& s = S();
    Display* d = s.dpy;
    w_ = std::max(1, o.w);
    h_ = std::max(1, o.h);
    reportedW_ = w_;
    reportedH_ = h_;
    minW_ = o.minW;
    minH_ = o.minH;
    resizable_ = o.resizable;

    if (o.offscreen) {
        x_ = -20000;
        y_ = -20000;
    } else if (o.x >= 0 && o.y >= 0) {
        x_ = o.x;
        y_ = o.y;
        moved_ = true;
    } else {
        Rect area = o.transientFor ? o.transientFor->geometry() : screenGeometry(cursorPos());
        x_ = area.x + (area.w - w_) / 2;
        y_ = area.y + (area.h - h_) / 2;
        keepOnScreen();
    }

    // see-through (32-bit) while a compositing manager runs and transparency is on; offscreen windows (photographs
    // of plugins' windows) and opaque ones never
    argb_ = s.argbVisual && s.compositing && s.transparency && !o.offscreen && !o.opaque;
    visual_ = argb_ ? s.argbVisual : s.visual;
    depth_ = argb_ ? 32 : s.depth;
    XSetWindowAttributes a{};
    unsigned long mask = CWBackPixmap | CWBorderPixel | CWColormap | CWBitGravity | CWEventMask;
    a.background_pixmap = None;
    a.border_pixel = 0;
    a.colormap = argb_ ? s.argbCmap : s.cmap;
    a.bit_gravity = NorthWestGravity;
    a.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                   ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                   PropertyChangeMask;
    if (o.popup || o.offscreen) {
        a.override_redirect = True;
        a.save_under = True;
        mask |= CWOverrideRedirect | CWSaveUnder;
    }
    win_ = XCreateWindow(d, s.root, x_, y_, (unsigned)w_, (unsigned)h_, 0, depth_, InputOutput, visual_, mask, &a);
    s.windows[win_] = this;

    // WM_CLASS, WM_HINTS, WM_NORMAL_HINTS, WM_CLIENT_MACHINE, WM_LOCALE_NAME
    XClassHint ch;
    ch.res_name = (char*)"tinydaw";
    ch.res_class = (char*)"TinyDAW";
    XWMHints wmh{};
    wmh.flags = InputHint | StateHint;
    wmh.input = (o.takesFocus && !o.offscreen) ? True : False;
    wmh.initial_state = NormalState;
    Xutf8SetWMProperties(d, win_, o.title.c_str(), o.title.c_str(), nullptr, 0, nullptr, &wmh, &ch);
    applySizeHints();
    setTitle(o.title);

    Atom protocols[2] = {s.a.WM_DELETE_WINDOW, s.a.NET_WM_PING};
    XSetWMProtocols(d, win_, protocols, 2);
    long pid = (long)getpid();
    XChangeProperty(d, win_, s.a.NET_WM_PID, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);

    Atom type = s.a.TYPE_NORMAL;
    if (o.popup) type = o.takesFocus ? s.a.TYPE_POPUP_MENU : s.a.TYPE_TOOLTIP;
    else if (o.dialog) type = s.a.TYPE_DIALOG;
    else if (o.offscreen) type = s.a.TYPE_UTILITY;
    XChangeProperty(d, win_, s.a.NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char*)&type, 1);
    if (o.offscreen) {
        Atom st[2] = {s.a.STATE_SKIP_TASKBAR, s.a.STATE_SKIP_PAGER};
        XChangeProperty(d, win_, s.a.NET_WM_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char*)st, 2);
    }
    if (o.transientFor) {
        XSetTransientForHint(d, win_, (XWin)o.transientFor->nativeHandle());
    }
    if (!o.decorated && !o.popup && !o.offscreen) setMotifDecorations(false);
    if (o.offscreen && s.compositeOpcode && !s.xwayland) compositeRedirectWindow(win_);
    if (o.acceptsFiles) {
        long version = 5;
        XChangeProperty(d, win_, s.a.XdndAware, XA_ATOM, 32, PropModeReplace, (unsigned char*)&version, 1);
    }

    // the icon: _NET_WM_ICON, ARGB (not premultiplied) as CARDINALs, several sizes
    if (!o.iconSvg.empty()) {
        std::vector<unsigned long> data;
        for (int size : {16, 24, 32, 48, 64, 128}) {
            Image img = renderSvg(o.iconSvg, size, size);
            if (img.empty()) continue;
            data.push_back((unsigned long)img.width());
            data.push_back((unsigned long)img.height());
            for (int y = 0; y < img.height(); y++)
                for (int x = 0; x < img.width(); x++) {
                    Color c = img.colorAt(x, y);
                    data.push_back(((unsigned long)c.a << 24) | ((unsigned long)c.r << 16) | ((unsigned long)c.g << 8) |
                                   c.b);
                }
        }
        if (!data.empty())
            XChangeProperty(d, win_, s.a.NET_WM_ICON, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)data.data(),
                            (int)data.size());
    }

    gc_ = XCreateGC(d, win_, 0, nullptr);
    XSetGraphicsExposures(d, gc_, False);

    if (s.im) {
        ic_ = XCreateIC(s.im, XNInputStyle, s.imStyle, XNClientWindow, win_, XNFocusWindow, win_, nullptr);
        if (ic_) {
            long filter = 0;
            XGetICValues(ic_, XNFilterEvents, &filter, nullptr);
            XSelectInput(d, win_, a.event_mask | filter);
        }
    }
    XDefineCursor(d, win_, cursorFor(Cursor::Arrow));
}

XWindow::~XWindow() {
    State& s = S();
    if (s.dpy) {
        if (grabbed_) XUngrabPointer(s.dpy, CurrentTime);
        if (kbdGrabbed_) XUngrabKeyboard(s.dpy, CurrentTime);
        waitShm();
        freeImage();
        if (ic_) XDestroyIC(ic_);
        if (gc_) XFreeGC(s.dpy, gc_);
        XDestroyWindow(s.dpy, win_);  // (and embed_, inside it)
        XFlush(s.dpy);
    }
    s.windows.erase(win_);
    if (embed_) s.windows.erase(embed_);
}

void XWindow::applySizeHints() {
    XSizeHints* sh = XAllocSizeHints();
    sh->flags = PPosition | USPosition | PSize | PWinGravity;
    sh->x = x_;
    sh->y = y_;
    sh->width = w_;
    sh->height = h_;
    sh->win_gravity = StaticGravity;  // the position is the client area's
    if (!resizable_) {
        sh->flags |= PMinSize | PMaxSize;
        sh->min_width = sh->max_width = w_;
        sh->min_height = sh->max_height = h_;
    } else if (minW_ > 0 || minH_ > 0) {
        sh->flags |= PMinSize;
        sh->min_width = std::max(1, minW_);
        sh->min_height = std::max(1, minH_);
    }
    XSetWMNormalHints(S().dpy, win_, sh);
    XFree(sh);
}

void XWindow::show() {
    State& s = S();
    if (!shownOnce_ && !moved_ && !opts_.offscreen) {
        // centred for its size as it is now
        Rect area = opts_.transientFor ? opts_.transientFor->geometry() : screenGeometry(cursorPos());
        x_ = area.x + (area.w - w_) / 2;
        y_ = area.y + (area.h - h_) / 2;
        keepOnScreen();
        XMoveWindow(s.dpy, win_, x_, y_);
        applySizeHints();
    }
    shownOnce_ = true;
    if (s.lastInputTime) {
        long t = (long)s.lastInputTime;
        XChangeProperty(s.dpy, win_, s.a.NET_WM_USER_TIME, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&t, 1);
    }
    XMapRaised(s.dpy, win_);
    focusOnMap_ = opts_.popup && opts_.takesFocus;
    visible_ = true;
    XFlush(s.dpy);
}

void XWindow::hide() {
    State& s = S();
    if (!visible_) return;
    visible_ = false;
    if (grabbed_) grabPointer(false);
    if (kbdGrabbed_) {
        XUngrabKeyboard(s.dpy, CurrentTime);
        kbdGrabbed_ = false;
        focus_ = false;
    }
    if (opts_.popup || opts_.offscreen) XUnmapWindow(s.dpy, win_);
    else XWithdrawWindow(s.dpy, win_, s.screen);
    XFlush(s.dpy);
}

void XWindow::setTitle(const std::string& title) {
    State& s = S();
    opts_.title = title;
    XChangeProperty(s.dpy, win_, s.a.NET_WM_NAME, s.a.UTF8_STRING, 8, PropModeReplace,
                    (const unsigned char*)title.data(), (int)title.size());
    XChangeProperty(s.dpy, win_, s.a.NET_WM_ICON_NAME, s.a.UTF8_STRING, 8, PropModeReplace,
                    (const unsigned char*)title.data(), (int)title.size());
    XTextProperty tp;
    char* list[1] = {(char*)title.c_str()};
    if (Xutf8TextListToTextProperty(s.dpy, list, 1, XStdICCTextStyle, &tp) >= Success) {
        XSetWMName(s.dpy, win_, &tp);
        XFree(tp.value);
    }
    XFlush(s.dpy);
}

void XWindow::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == w_ && h == h_) return;
    w_ = w;
    h_ = h;
    applySizeHints();
    XResizeWindow(S().dpy, win_, (unsigned)w, (unsigned)h);
    XFlush(S().dpy);
}

void XWindow::move(int x, int y) {
    if (opts_.offscreen) return;
    moved_ = true;
    x_ = x;
    y_ = y;
    applySizeHints();
    XMoveWindow(S().dpy, win_, x, y);
    XFlush(S().dpy);
}

void XWindow::setMinimumSize(int w, int h) {
    minW_ = w;
    minH_ = h;
    applySizeHints();
}

void XWindow::setResizable(bool r) {
    resizable_ = r;
    applySizeHints();
}

Rect XWindow::frameGeometry() const {
    return Rect(x_ - (int)frame_[0], y_ - (int)frame_[2], w_ + (int)(frame_[0] + frame_[1]),
                h_ + (int)(frame_[2] + frame_[3]));
}

void XWindow::raise() {
    State& s = S();
    XRaiseWindow(s.dpy, win_);
    if (opts_.popup || opts_.offscreen) {
        if (opts_.popup && opts_.takesFocus && mapped_ && !kbdGrabbed_)
            kbdGrabbed_ = XGrabKeyboard(s.dpy, win_, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
    } else if (visible_ && !mapped_ && minimized_) {
        XMapRaised(s.dpy, win_);  // (minimized: mapped again, the window manager shows it again, ICCCM 4.1.4)
    } else if (mapped_) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win_;
        ev.xclient.message_type = s.a.NET_ACTIVE_WINDOW;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 1;  // from an application
        ev.xclient.data.l[1] = (long)(s.lastInputTime ? s.lastInputTime : CurrentTime);
        ev.xclient.data.l[2] = 0;
        XSendEvent(s.dpy, s.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    }
    XFlush(s.dpy);
}

// _NET_WM_STATE: asks the window manager when mapped, else sets the property for the first map
void XWindow::setNetState(Atom a1, Atom a2, bool on) {
    State& s = S();
    if (mapped_) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = win_;
        ev.xclient.message_type = s.a.NET_WM_STATE;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = on ? 1 : 0;  // _NET_WM_STATE_ADD / _REMOVE
        ev.xclient.data.l[1] = (long)a1;
        ev.xclient.data.l[2] = (long)a2;
        ev.xclient.data.l[3] = 1;
        XSendEvent(s.dpy, s.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    } else {
        std::vector<long> cur = readLongs(win_, s.a.NET_WM_STATE, XA_ATOM);
        std::vector<long> next;
        for (long a : cur)
            if (a != (long)a1 && a != (long)a2) next.push_back(a);
        if (on) {
            next.push_back((long)a1);
            if (a2) next.push_back((long)a2);
        }
        XChangeProperty(s.dpy, win_, s.a.NET_WM_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char*)next.data(),
                        (int)next.size());
    }
    XFlush(s.dpy);
}

void XWindow::setMaximized(bool on) {
    setNetState(S().a.STATE_MAX_V, S().a.STATE_MAX_H, on);
    // (shown: as the window manager says, once it did it: StateChanged; it may refuse, or there may be none)
    if (!mapped_) maximized_ = on;
}

void XWindow::setFullScreen(bool on) {
    setNetState(S().a.STATE_FULLSCREEN, 0, on);
    fullscreen_ = on;
}

// (tell: a StateChanged event when maximized, full screen or minimized changed: emitted last)
void XWindow::readNetState(bool tell) {
    State& s = S();
    std::vector<long> st = readLongs(win_, s.a.NET_WM_STATE, XA_ATOM);
    bool v = false, h = false, f = false, hidden = false;
    for (long a : st) {
        if (a == (long)s.a.STATE_MAX_V) v = true;
        if (a == (long)s.a.STATE_MAX_H) h = true;
        if (a == (long)s.a.STATE_FULLSCREEN) f = true;
        if (a == (long)s.a.STATE_HIDDEN) hidden = true;
    }
    bool changed = maximized_ != (v && h) || fullscreen_ != f || minimized_ != hidden;
    maximized_ = v && h;
    fullscreen_ = f;
    minimized_ = hidden;
    if (tell && changed) {
        Event e;
        e.type = Event::StateChanged;
        emit(e);
    }
}

// _MOTIF_WM_HINTS: with (on) or without the window manager's decorations (its title bar and border).
// Five CARDINALs: flags, functions, decorations, input mode, status; flags 2: the decorations are given
void XWindow::setMotifDecorations(bool on) {
    State& s = S();
    long hints[5] = {2, 0, on ? 1 : 0, 0, 0};
    XChangeProperty(s.dpy, win_, s.a.MOTIF_WM_HINTS, s.a.MOTIF_WM_HINTS, 32, PropModeReplace, (unsigned char*)hints, 5);
}

void XWindow::setDecorated(bool on) {
    if (opts_.popup || opts_.offscreen || opts_.decorated == on) return;
    opts_.decorated = on;
    setMotifDecorations(on);
    XFlush(S().dpy);
}

void XWindow::setCornerRadii(int tl, int tr, int br, int bl) {
    int r[4] = {std::max(0, tl), std::max(0, tr), std::max(0, br), std::max(0, bl)};
    if (opts_.offscreen || std::equal(r, r + 4, radii_)) return;
    std::copy(r, r + 4, radii_);
    applyShape();
}

void XWindow::setInputArea(const Rect& area) {
    if (area == inputArea_) return;
    inputArea_ = area;
    applyShape();
}

void XWindow::setOpacity(float opacity) {
    State& s = S();
    float o = std::isfinite(opacity) ? std::max(0.f, std::min(1.f, opacity)) : 1.f;
    if (o == opacity_) return;
    opacity_ = o;
    if (o >= 1.f) {
        XDeleteProperty(s.dpy, win_, s.a.NET_WM_WINDOW_OPACITY);
    } else {
        long v = (long)(unsigned long)std::lround(o * 4294967295.0);  // (0xFFFFFFFF: opaque)
        XChangeProperty(s.dpy, win_, s.a.NET_WM_WINDOW_OPACITY, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&v, 1);
    }
    XFlush(s.dpy);
}

void XWindow::setBlurBehind(bool on, const Rect& area) {
    if (on == blurOn_ && (!on || area == blurArea_)) return;
    blurOn_ = on;
    blurArea_ = on ? area : Rect();
    applyShape();
}

// the rows of rectangles that make `box` with its corners cut round (radii: top-left, top-right, bottom-right,
// bottom-left), each corner's quarter circle; X shapes are 1-bit, so an anti-aliased edge drawn inside stays
std::vector<XRectangle> roundedRects(const Rect& box, const int radii[4]) {
    std::vector<XRectangle> rects;
    int w = box.w, h = box.h;
    if (w <= 0 || h <= 0) return rects;
    auto inset = [](int r, int row) {  // how far in from the side the inside starts on this row of a corner
        double d = r - row - 0.5;
        return (int)std::lround(r - std::sqrt(std::max(0.0, (double)r * r - d * d)));
    };
    int top = std::min(h / 2, std::max(radii[0], radii[1]));
    int bottom = std::min(h - top, std::max(radii[2], radii[3]));
    auto row = [&](int x, int y, int rw, int rh) {
        rects.push_back({(short)(box.x + x), (short)(box.y + y), (unsigned short)rw, (unsigned short)rh});
    };
    for (int y = 0; y < top; y++) {
        int l = y < radii[0] ? inset(std::min(radii[0], w / 2), y) : 0;
        int r = y < radii[1] ? inset(std::min(radii[1], w / 2), y) : 0;
        if (w - l - r > 0) row(l, y, w - l - r, 1);
    }
    if (h - top - bottom > 0) row(0, top, w, h - top - bottom);
    for (int i = bottom - 1; i >= 0; i--) {  // (top to bottom: YXBanded wants the rows in order)
        int y = h - 1 - i;
        int l = i < radii[3] ? inset(std::min(radii[3], w / 2), i) : 0;
        int r = i < radii[2] ? inset(std::min(radii[2], w / 2), i) : 0;
        if (w - l - r > 0) row(l, y, w - l - r, 1);
    }
    return rects;
}

// The window's shapes, as it is shown now. Opaque (24-bit, or 32-bit while not hasAlpha()): its bounding shape is
// the rectangle less what lies outside each corner's quarter circle (not for popups). See-through: no bounding
// shape (the corners' alpha shows the desktop, anti-aliased), and an input shape (setInputArea's area, or all of
// it, its corners cut) so clicks on what is see-through there go to what is under it. And the blur behind it.
void XWindow::applyShape() {
    State& s = S();
    bool alpha = hasAlpha();
    bool any = radii_[0] || radii_[1] || radii_[2] || radii_[3];
    Rect whole(0, 0, w_, h_);
    // the bounding shape (opaque, framed: the corners cut)
    if (!alpha && any && !opts_.popup && w_ > 0 && h_ > 0) {
        std::vector<XRectangle> rects = roundedRects(whole, radii_);
        XShapeCombineRectangles(s.dpy, win_, ShapeBounding, 0, 0, rects.data(), (int)rects.size(), ShapeSet, YXBanded);
        shaped_ = true;
    } else if (shaped_) {
        XShapeCombineMask(s.dpy, win_, ShapeBounding, 0, 0, 0 /*None*/, ShapeSet);
        shaped_ = false;
    }
    // the input shape (see-through: where it can be clicked)
    Rect base = inputArea_.empty() ? whole : inputArea_.intersected(whole);
    if (alpha && (any || !inputArea_.empty()) && !base.empty()) {
        std::vector<XRectangle> rects = roundedRects(base, radii_);
        XShapeCombineRectangles(s.dpy, win_, ShapeInput, 0, 0, rects.data(), (int)rects.size(), ShapeSet, YXBanded);
        inputShaped_ = true;
    } else if (inputShaped_) {
        XShapeCombineMask(s.dpy, win_, ShapeInput, 0, 0, 0 /*None*/, ShapeSet);
        inputShaped_ = false;
    }
    // the blur behind it: its area, the corners round where they are the input area's (the window's, a popup's box)
    std::vector<long> region;
    bool blur = alpha && blurOn_;
    if (blur) {
        Rect area = blurArea_.empty() ? base : blurArea_.intersected(whole);
        int r[4] = {0, 0, 0, 0};
        if (area.x == base.x && area.y == base.y) r[0] = radii_[0];
        if (area.right() == base.right() && area.y == base.y) r[1] = radii_[1];
        if (area.right() == base.right() && area.bottom() == base.bottom()) r[2] = radii_[2];
        if (area.x == base.x && area.bottom() == base.bottom()) r[3] = radii_[3];
        if (!(area == whole) || r[0] || r[1] || r[2] || r[3]) {  // (else none: the whole window)
            for (const XRectangle& x : roundedRects(area, r)) {
                region.push_back(x.x);
                region.push_back(x.y);
                region.push_back(x.width);
                region.push_back(x.height);
            }
            if (region.empty()) blur = false;
        }
    }
    if (blur && (!blurSet_ || region != blurSent_)) {
        XChangeProperty(s.dpy, win_, s.a.KDE_BLUR_BEHIND, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char*)region.data(), (int)region.size());
        blurSent_ = region;
        blurSet_ = true;
    } else if (!blur && blurSet_) {
        XDeleteProperty(s.dpy, win_, s.a.KDE_BLUR_BEHIND);
        blurSent_.clear();
        blurSet_ = false;
    }
    XFlush(s.dpy);
}

void XWindow::minimize() {
    State& s = S();
    if (opts_.popup || opts_.offscreen) return;
    XIconifyWindow(s.dpy, win_, s.screen);
    XFlush(s.dpy);
}

// _NET_WM_MOVERESIZE (if the window manager has it, _NET_SUPPORTED): the pointer's implicit grab (from the
// press) is let go first, so the window manager can take the pointer
bool XWindow::beginMoveResize(int edges, Point screenPos, int button) {
    State& s = S();
    if (!mapped_ || opts_.popup || opts_.offscreen) return false;
    std::vector<long> supported = readLongs(s.root, s.a.NET_SUPPORTED, XA_ATOM);
    if (std::find(supported.begin(), supported.end(), (long)s.a.NET_WM_MOVERESIZE) == supported.end()) return false;
    long direction = 8;  // _NET_WM_MOVERESIZE_MOVE
    bool l = edges & EdgeLeft, r = edges & EdgeRight, t = edges & EdgeTop, b = edges & EdgeBottom;
    if (t && l) direction = 0;       // _NET_WM_MOVERESIZE_SIZE_TOPLEFT
    else if (t && r) direction = 2;  // …TOPRIGHT
    else if (b && r) direction = 4;  // …BOTTOMRIGHT
    else if (b && l) direction = 6;  // …BOTTOMLEFT
    else if (t) direction = 1;
    else if (r) direction = 3;
    else if (b) direction = 5;
    else if (l) direction = 7;
    if (grabbed_) grabPointer(false);
    XUngrabPointer(s.dpy, CurrentTime);
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win_;
    ev.xclient.message_type = s.a.NET_WM_MOVERESIZE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = screenPos.x;
    ev.xclient.data.l[1] = screenPos.y;
    ev.xclient.data.l[2] = direction;
    ev.xclient.data.l[3] = button == RightButton ? 3 : button == MiddleButton ? 2 : 1;
    ev.xclient.data.l[4] = 1;  // (from an application)
    XSendEvent(s.dpy, s.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(s.dpy);
    return true;
}

uintptr_t XWindow::embedArea(const Rect& area) {
    State& s = S();
    if (area.empty()) {  // (hidden: what was in it went, and the window says something of its own there)
        if (embed_ && embedShown_) XUnmapWindow(s.dpy, embed_);
        embedShown_ = false;
        XFlush(s.dpy);
        return (uintptr_t)embed_;
    }
    Rect r = area;
    if (embed_ && !embedShown_) {
        XMapWindow(s.dpy, embed_);
        embedShown_ = true;
    }
    if (!embed_) {
        XSetWindowAttributes a{};
        a.background_pixel = BlackPixel(s.dpy, s.screen);  // (where the plugin's view doesn't reach)
        a.border_pixel = 0;
        a.colormap = s.cmap;
        embed_ = XCreateWindow(s.dpy, win_, r.x, r.y, (unsigned)r.w, (unsigned)r.h, 0, s.depth, InputOutput, s.visual,
                               CWBackPixel | CWBorderPixel | CWColormap, &a);
        s.windows[embed_] = this;  // (XEmbed's messages to it, the embedder of what is inside: handleClientMessage)
        XMapWindow(s.dpy, embed_);
        embedShown_ = true;
    } else if (!(r == embedRect_)) {
        XMoveResizeWindow(s.dpy, embed_, r.x, r.y, (unsigned)r.w, (unsigned)r.h);
    }
    embedRect_ = r;
    XFlush(s.dpy);
    return (uintptr_t)embed_;
}

void XWindow::readFrameExtents() {
    std::vector<long> e = readLongs(win_, S().a.NET_FRAME_EXTENTS, XA_CARDINAL);
    if (e.size() >= 4)
        for (int i = 0; i < 4; i++) frame_[i] = e[i];
}

void XWindow::updatePosition() {
    State& s = S();
    int x = 0, y = 0;
    XWin child;
    if (XTranslateCoordinates(s.dpy, win_, s.root, 0, 0, &x, &y, &child)) {
        x_ = x;
        y_ = y;
    }
}

void XWindow::setCursor(Cursor c) {
    if (c == cursor_) return;
    cursor_ = c;
    XDefineCursor(S().dpy, win_, cursorFor(c));
    XFlush(S().dpy);
}

// X gives a key grabbed on a window to it whenever the focus is in it or in a window inside it (an
// embedded plugin's): grabbed without modifiers, Caps Lock and Num Lock either way, so Shift+letter
// (a capital typed into the plugin) and Ctrl+… still go where the focus is
// Only keys that give the symbol unshifted are grabbed (on a German keyboard ';' is Shift+',': plain
// ',' stays the plugin's). A passive grab ends when its own key is let go, so while any grabbed key is
// held the whole keyboard is taken (noteKey): another note's release then comes here too, not to the
// plugin (A down, S down, A up, S up: S would sound on)
void XWindow::grabKeys(const std::vector<int>& keys) {
    State& s = S();
    if (keys == grabbedKeys_ && grabbedKeymap_ == s.keymapChanges) return;
    const unsigned locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (KeyCode code : grabbedKeycodes_)
        for (unsigned m : locks) XUngrabKey(s.dpy, code, m, win_);
    grabbedKeycodes_.clear();
    grabbedKeys_ = keys;
    grabbedKeymap_ = s.keymapChanges;
    if (keys.empty() && keyboardHeld_) {  // (turned off while one was held)
        XUngrabKeyboard(s.dpy, CurrentTime);
        keyboardHeld_ = false;
        grabbedDown_.clear();
    }
    int first = 0, last = 0;
    XDisplayKeycodes(s.dpy, &first, &last);
    for (int key : keys) {
        KeySym ks = key >= 'A' && key <= 'Z' ? (KeySym)(XK_a + (key - 'A')) : (KeySym)key;  // (Event::key's letters: capitals)
        for (int code = first; code <= last; code++) {
            if (XkbKeycodeToKeysym(s.dpy, (KeyCode)code, 0, 0) != ks) continue;
            grabbedKeycodes_.push_back((KeyCode)code);
            for (unsigned m : locks) XGrabKey(s.dpy, code, m, win_, False, GrabModeAsync, GrabModeAsync);
        }
    }
    XFlush(s.dpy);
}

// the grabbed keys' releases will come elsewhere (unmapped, or another grab took the keyboard): our
// keyboard grab is already gone (not let go here: a grab is the program's, a popup's would go with
// it), and the next grabbed key takes it again
void XWindow::forgetHeldKeys() {
    keyboardHeld_ = false;
    grabbedDown_.clear();
}

// (KeyPress / KeyRelease) a grabbed key held: the keyboard is ours until every one is let go
void XWindow::noteKey(XKeyEvent& k, bool press) {
    State& s = S();
    bool grabbed = std::find(grabbedKeycodes_.begin(), grabbedKeycodes_.end(), (KeyCode)k.keycode) != grabbedKeycodes_.end();
    if (press && grabbed) {
        if (std::find(grabbedDown_.begin(), grabbedDown_.end(), (KeyCode)k.keycode) == grabbedDown_.end())
            grabbedDown_.push_back((KeyCode)k.keycode);
        if (!keyboardHeld_)
            keyboardHeld_ = XGrabKeyboard(s.dpy, win_, False, GrabModeAsync, GrabModeAsync, k.time) == GrabSuccess;
    } else if (!press) {
        grabbedDown_.erase(std::remove(grabbedDown_.begin(), grabbedDown_.end(), (KeyCode)k.keycode), grabbedDown_.end());
        if (grabbedDown_.empty() && keyboardHeld_) {
            XUngrabKeyboard(s.dpy, k.time);
            keyboardHeld_ = false;
        }
    }
}

void XWindow::grabPointer(bool on) {
    State& s = S();
    if (on) {
        int r = XGrabPointer(s.dpy, win_, False,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        grabbed_ = r == GrabSuccess;
    } else if (grabbed_) {
        XUngrabPointer(s.dpy, CurrentTime);
        grabbed_ = false;
    }
    XFlush(s.dpy);
}

// --- present ---

void XWindow::freeImage() {
    State& s = S();
    if (!img_) return;
    if (imgShm_) {
        XShmDetach(s.dpy, &shmInfo_);
        XSync(s.dpy, False);
        img_->data = nullptr;
        XDestroyImage(img_);
        shmdt(shmInfo_.shmaddr);
    } else {
        XDestroyImage(img_);  // frees the malloc'ed pixels
    }
    img_ = nullptr;
    imgShm_ = false;
}

bool XWindow::ensureImage(int w, int h) {
    State& s = S();
    if (img_ && img_->width >= w && img_->height >= h && (long)img_->width * img_->height <= 4L * w * h + 65536)
        return true;
    waitShm();
    freeImage();
    // some room to grow while the window is being resized
    int aw = (w + 63) & ~63, ah = (h + 63) & ~63;
    if (s.shm) {
        img_ = XShmCreateImage(s.dpy, visual_, (unsigned)depth_, ZPixmap, nullptr, &shmInfo_, (unsigned)aw,
                               (unsigned)ah);
        if (img_) {
            shmInfo_.shmid = shmget(IPC_PRIVATE, (size_t)img_->bytes_per_line * ah, IPC_CREAT | 0600);
            void* addr = shmInfo_.shmid >= 0 ? shmat(shmInfo_.shmid, nullptr, 0) : (void*)-1;
            if (addr == (void*)-1) {
                if (shmInfo_.shmid >= 0) shmctl(shmInfo_.shmid, IPC_RMID, nullptr);
                XDestroyImage(img_);
                img_ = nullptr;
            } else {
                shmInfo_.shmaddr = img_->data = (char*)addr;
                shmInfo_.readOnly = False;
                ErrorTrap trap;
                XShmAttach(s.dpy, &shmInfo_);
                int err = trap.end();
                shmctl(shmInfo_.shmid, IPC_RMID, nullptr);  // gone once both sides detach
                if (err) {
                    img_->data = nullptr;
                    XDestroyImage(img_);
                    shmdt(addr);
                    img_ = nullptr;
                    s.shm = false;  // e.g. a remote display: don't try again
                } else {
                    imgShm_ = true;
                }
            }
        } else {
            s.shm = false;
        }
    }
    if (!img_) {
        char* data = (char*)malloc((size_t)aw * ah * 4);
        if (!data) return false;
        img_ = XCreateImage(s.dpy, visual_, (unsigned)depth_, ZPixmap, 0, data, (unsigned)aw, (unsigned)ah, 32, 0);
        if (!img_) {
            free(data);
            return false;
        }
        imgShm_ = false;
    }
    const bool little = [] { uint32_t one = 1; return *(const uint8_t*)&one == 1; }();
    fastCopy_ = img_->bits_per_pixel == 32 && img_->red_mask == 0xFF0000 && img_->green_mask == 0xFF00 &&
                img_->blue_mask == 0xFF && (img_->byte_order == LSBFirst) == little;
    return true;
}

void XWindow::waitShm() {
    if (!shmPending_) return;
    State& s = S();
    double deadline = monoNow() + 0.25;
    XEvent ev;
    while (shmPending_) {
        if (XCheckTypedWindowEvent(s.dpy, win_, s.shmCompletion, &ev)) {
            shmPending_ = false;
            break;
        }
        double left = deadline - monoNow();
        if (left <= 0) {
            XSync(s.dpy, False);
            shmPending_ = false;
            break;
        }
        waitReadable(ConnectionNumber(s.dpy), std::min(left, 0.01));
    }
}

void XWindow::present(const Image& fb, const Rect& dirty) {
    State& s = S();
    Rect r = dirty.intersected(fb.rect()).intersected(Rect(0, 0, w_, h_));
    if (r.empty() || fb.empty()) return;
    if (!ensureImage(fb.width(), fb.height())) return;
    waitShm();  // the server may still be reading the last picture
    // (32-bit: the premultiplied pixels as they are while see-through; shown opaque, every alpha 255, so the
    // compositing manager shows them as a 24-bit window would: what is see-through, over black)
    const bool opaque32 = argb_ && !hasAlpha();
    for (int y = r.y; y < r.bottom(); y++) {
        const uint32_t* src = fb.row(y) + r.x;
        char* dst = img_->data + (size_t)y * img_->bytes_per_line + (size_t)r.x * 4;
        if (fastCopy_ && opaque32) {
            uint32_t* d32 = (uint32_t*)dst;
            for (int x = 0; x < r.w; x++) d32[x] = src[x] | 0xFF000000u;
        } else if (fastCopy_) {
            memcpy(dst, src, (size_t)r.w * 4);
        } else {
            for (int x = 0; x < r.w; x++) {
                uint32_t p = src[x];
                unsigned long pix = 0;
                auto put = [&](unsigned long mask, uint32_t v) {
                    if (!mask) return;
                    int shift = 0;
                    while (!((mask >> shift) & 1)) shift++;
                    int bits = 0;
                    while ((mask >> (shift + bits)) & 1) bits++;
                    pix |= ((unsigned long)(v >> (8 - std::min(bits, 8))) << shift) & mask;
                };
                put(img_->red_mask, (p >> 16) & 255);
                put(img_->green_mask, (p >> 8) & 255);
                put(img_->blue_mask, p & 255);
                if (argb_) {  // (the bits no colour has: alpha)
                    unsigned long alphaMask = 0xFFFFFFFFul & ~(img_->red_mask | img_->green_mask | img_->blue_mask);
                    put(alphaMask, opaque32 ? 255u : p >> 24);
                }
                XPutPixel(img_, r.x + x, y, pix);
            }
        }
    }
    if (imgShm_) {
        XShmPutImage(s.dpy, win_, gc_, img_, r.x, r.y, r.x, r.y, (unsigned)r.w, (unsigned)r.h, True);
        shmPending_ = true;
    } else {
        XPutImage(s.dpy, win_, gc_, img_, r.x, r.y, r.x, r.y, (unsigned)r.w, (unsigned)r.h);
    }
    XFlush(s.dpy);
}

// --- capture ---

bool ximageToImage(XImage* xi, int w, int h, Image& out) {
    out = Image(w, h);
    if (!xi->red_mask && !xi->green_mask && !xi->blue_mask) {  // from a pixmap: no visual, no masks
        xi->red_mask = S().visual->red_mask;
        xi->green_mask = S().visual->green_mask;
        xi->blue_mask = S().visual->blue_mask;
    }
    const bool little = [] { uint32_t one = 1; return *(const uint8_t*)&one == 1; }();
    bool fast = xi->bits_per_pixel == 32 && xi->red_mask == 0xFF0000 && xi->green_mask == 0xFF00 &&
                xi->blue_mask == 0xFF && (xi->byte_order == LSBFirst) == little;
    auto chan = [](unsigned long pix, unsigned long mask) -> uint32_t {
        if (!mask) return 0;
        int shift = 0;
        while (!((mask >> shift) & 1)) shift++;
        unsigned long v = (pix & mask) >> shift, max = mask >> shift;
        return (uint32_t)((v * 255 + max / 2) / max);
    };
    for (int y = 0; y < h; y++) {
        uint32_t* dst = out.row(y);
        if (fast) {
            const uint32_t* src = (const uint32_t*)(xi->data + (size_t)y * xi->bytes_per_line);
            for (int x = 0; x < w; x++) dst[x] = 0xFF000000u | (src[x] & 0xFFFFFF);
        } else {
            for (int x = 0; x < w; x++) {
                unsigned long p = XGetPixel(xi, x, y);
                dst[x] = 0xFF000000u | (chan(p, xi->red_mask) << 16) | (chan(p, xi->green_mask) << 8) |
                         chan(p, xi->blue_mask);
            }
        }
    }
    return true;
}

bool XWindow::capture(Image& out) {
    State& s = S();
    XWindowAttributes wa;
    if (!XGetWindowAttributes(s.dpy, win_, &wa) || wa.map_state != IsViewable || wa.width <= 0 || wa.height <= 0)
        return false;
    ErrorTrap trap;
    XImage* xi = XGetImage(s.dpy, win_, 0, 0, (unsigned)wa.width, (unsigned)wa.height, AllPlanes, ZPixmap);
    trap.end();
    if (!xi) {
        // outside the screen: read the window's composite pixmap instead (XWayland redirects every
        // top-level window; on a plain X server only a compositing manager does)
        ErrorTrap t2;
        Pixmap pm = nameWindowPixmap(win_);
        XSync(s.dpy, False);
        if (!S().trappedError && pm)
            xi = XGetImage(s.dpy, pm, 0, 0, (unsigned)wa.width, (unsigned)wa.height, AllPlanes, ZPixmap);
        if (pm) XFreePixmap(s.dpy, pm);
        t2.end();
    }
    if (!xi) return false;
    ximageToImage(xi, wa.width, wa.height, out);
    XDestroyImage(xi);
    return true;
}

// --- XDND ---

void XWindow::sendDndStatus(bool accept) {
    State& s = S();
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = s.dpy;
    ev.xclient.window = dnd_.source;
    ev.xclient.message_type = s.a.XdndStatus;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)win_;
    ev.xclient.data.l[1] = accept ? 3 : 2;  // accept, send positions even inside the (empty) rectangle
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = accept ? (long)s.a.XdndActionCopy : 0;
    XSendEvent(s.dpy, dnd_.source, False, NoEventMask, &ev);
    XFlush(s.dpy);
}

void XWindow::sendDndFinished(bool accepted) {
    State& s = S();
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = s.dpy;
    ev.xclient.window = dnd_.source;
    ev.xclient.message_type = s.a.XdndFinished;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)win_;
    ev.xclient.data.l[1] = accepted ? 1 : 0;
    ev.xclient.data.l[2] = accepted ? (long)s.a.XdndActionCopy : 0;
    XSendEvent(s.dpy, dnd_.source, False, NoEventMask, &ev);
    XFlush(s.dpy);
}

bool XWindow::fetchDndFiles() {
    std::string text;
    dnd_.fetched = true;
    if (!convertSelection(win_, S().a.XdndSelection, S().a.URI_LIST, dnd_.time, 300, text)) return false;
    dnd_.files = parseUriList(text);
    return true;
}

// XEmbed, as the embedder (a plugin's window put in ours: Windows plugins' through Wine). An embedded
// window asks for the keys (XEMBED_REQUEST_FOCUS: Wine doesn't take the X focus itself once
// embedded): it gets the X focus while ours has it (else when ours gets it again), until a click on
// our own part of the window takes it back; when the focus leaves our window, it is told it isn't
// active any more (XEMBED_WINDOW_DEACTIVATE: Wine's plugin then knows it lost the keys).
constexpr long XEMBED_WINDOW_DEACTIVATE = 2, XEMBED_REQUEST_FOCUS = 3;

// the window embedded in ours (the topmost of our children, or of embed_'s when there is one; 0: none)
XWin XWindow::embedded() {
    XWin root = 0, parent = 0, *children = nullptr, top = 0;
    unsigned count = 0;
    if (XQueryTree(S().dpy, embed_ ? embed_ : win_, &root, &parent, &children, &count) && children) {
        if (count) top = children[count - 1];
        XFree(children);
    }
    return top;
}

void XWindow::focusEmbedded() {
    if (embedFocus_ && embedded() == embedFocus_) XSetInputFocus(S().dpy, embedFocus_, RevertToParent, CurrentTime);
}

void XWindow::deactivateEmbedded() {
    if (!embedFocus_ || embedded() != embedFocus_) return;
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = embedFocus_;
    e.xclient.message_type = S().a.XEMBED;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = XEMBED_WINDOW_DEACTIVATE;
    XSendEvent(S().dpy, embedFocus_, False, NoEventMask, &e);
}

void XWindow::handleClientMessage(XClientMessageEvent& cm) {
    State& s = S();
    const Atoms& A = s.a;
    if (cm.message_type == A.XEMBED) {
        if (cm.data.l[1] == XEMBED_REQUEST_FOCUS) {
            embedFocus_ = embedded();
            XWin focused = 0;
            int revert = 0;
            XGetInputFocus(s.dpy, &focused, &revert);
            if (focused == win_ || (focused && focused == embedFocus_)) focusEmbedded();
        }
        return;
    }
    if (cm.message_type == A.WM_PROTOCOLS) {
        Atom what = (Atom)cm.data.l[0];
        if (what == A.WM_DELETE_WINDOW) {
            Event e;
            e.type = Event::Close;
            emit(e);
        } else if (what == A.NET_WM_PING) {
            XEvent reply;
            reply.xclient = cm;
            reply.xclient.window = s.root;
            XSendEvent(s.dpy, s.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &reply);
            XFlush(s.dpy);
        }
        return;
    }
    if (!opts_.acceptsFiles) return;
    if (cm.message_type == A.XdndEnter) {
        dnd_ = Dnd();
        dnd_.source = (XWin)cm.data.l[0];
        dnd_.version = (int)((unsigned long)cm.data.l[1] >> 24);
        std::vector<long> types;
        if (cm.data.l[1] & 1) types = readLongs(dnd_.source, A.XdndTypeList, XA_ATOM);
        else
            for (int i = 2; i < 5; i++)
                if (cm.data.l[i]) types.push_back(cm.data.l[i]);
        for (long t : types)
            if ((Atom)t == A.URI_LIST) dnd_.uriList = true;
    } else if (cm.message_type == A.XdndPosition) {
        if (!dnd_.source) dnd_.source = (XWin)cm.data.l[0];
        int rx = (int)(((unsigned long)cm.data.l[2] >> 16) & 0xFFFF), ry = (int)((unsigned long)cm.data.l[2] & 0xFFFF);
        if (rx >= 0x8000) rx -= 0x10000;
        if (ry >= 0x8000) ry -= 0x10000;
        dnd_.time = (Time)cm.data.l[3];
        int x = rx - x_, y = ry - y_;
        XWin child;
        XTranslateCoordinates(s.dpy, s.root, win_, rx, ry, &x, &y, &child);
        dnd_.x = x;
        dnd_.y = y;
        bool ok = true;
        if (dnd_.uriList && !dnd_.fetched) ok = fetchDndFiles();
        bool accept = dnd_.uriList && (!dnd_.files.empty() || !ok);
        sendDndStatus(accept);
        if (!accept) return;
        Event e;
        e.type = dnd_.entered ? Event::DragMove : Event::DragEnter;
        dnd_.entered = true;
        e.x = x;
        e.y = y;
        e.files = dnd_.files;
        emit(e);
    } else if (cm.message_type == A.XdndLeave) {
        bool entered = dnd_.entered;
        dnd_ = Dnd();
        if (entered) {
            Event e;
            e.type = Event::DragLeave;
            emit(e);
        }
    } else if (cm.message_type == A.XdndDrop) {
        if (!dnd_.source) dnd_.source = (XWin)cm.data.l[0];
        dnd_.time = (Time)cm.data.l[2];
        if (dnd_.uriList && dnd_.files.empty()) fetchDndFiles();
        bool accepted = !dnd_.files.empty();
        sendDndFinished(accepted);
        Dnd d = dnd_;
        dnd_ = Dnd();
        Event e;
        e.type = accepted ? Event::Drop : Event::DragLeave;
        e.x = d.x;
        e.y = d.y;
        e.files = d.files;
        if (accepted || d.entered) emit(e);
    }
}

// --- keys ---

void XWindow::handleKey(XKeyEvent& k, bool press) {
    State& s = S();
    Event e;
    e.time = eventSeconds(k.time);
    e.x = k.x;
    e.y = k.y;
    if (press) {
        char buf[128];
        KeySym ks = NoSymbol;
        std::string text;
        if (ic_) {
            Status st = 0;
            int n = Xutf8LookupString(ic_, &k, buf, (int)sizeof buf - 1, &ks, &st);
            if (st == XBufferOverflow) {
                std::vector<char> big((size_t)n + 1);
                n = Xutf8LookupString(ic_, &k, big.data(), n, &ks, &st);
                if (st == XLookupChars || st == XLookupBoth) text.assign(big.data(), (size_t)std::max(0, n));
            } else if (st == XLookupChars || st == XLookupBoth) {
                text.assign(buf, (size_t)std::max(0, n));
            }
            if (st != XLookupKeySym && st != XLookupBoth) ks = NoSymbol;
        } else {
            int n = XLookupString(&k, buf, (int)sizeof buf - 1, &ks, nullptr);
            text = latin1ToUtf8(buf, n);
        }
        if (ks == NoSymbol && k.keycode) ks = XLookupKeysym(&k, 0);
        text = printableText(text);
        int key = keyFromKeysym(ks);
        if (!key && !text.empty()) key = (int)upperCodepoint(firstCodepoint(text));
        bool repeat = false;
        if (s.repeatKeycode && s.repeatKeycode == k.keycode) {
            repeat = true;
            s.repeatKeycode = 0;
        } else if (s.detectableRepeat && k.keycode && s.keysDown.count(k.keycode)) {
            repeat = true;
        }
        if (k.keycode) {
            s.keysDown.insert(k.keycode);
            s.keyOfCode[k.keycode] = key;
        }
        e.type = Event::KeyDown;
        e.key = key;
        e.mods = modsFromState(k.state) | modifierOfKey(key);
        e.autoRepeat = repeat;
        e.text = text;
    } else {
        // an auto-repeat (without detectable auto-repeat): KeyRelease + KeyPress with the same time
        if (!s.detectableRepeat && XEventsQueued(s.dpy, QueuedAfterReading)) {
            XEvent next;
            XPeekEvent(s.dpy, &next);
            if (next.type == KeyPress && next.xkey.keycode == k.keycode && next.xkey.time == k.time &&
                next.xkey.window == k.window) {
                s.repeatKeycode = k.keycode;
                return;
            }
        }
        s.keysDown.erase(k.keycode);
        int key = 0;
        auto it = s.keyOfCode.find(k.keycode);
        if (it != s.keyOfCode.end()) key = it->second;
        else key = keyFromKeysym(XLookupKeysym(&k, 0));
        e.type = Event::KeyUp;
        e.key = key;
        e.mods = modsFromState(k.state) & ~modifierOfKey(key);
    }
    emit(e);
}

// --- events ---

void XWindow::handle(XEvent& ev) {
    State& s = S();
    // (embed_ selects no events: what comes for it is XEmbed's messages from what is inside it)
    if (ev.xany.window != win_ && ev.type != ClientMessage) return;
    switch (ev.type) {
        case Expose:
            if (std::find(s.pendingExpose.begin(), s.pendingExpose.end(), win_) == s.pendingExpose.end())
                s.pendingExpose.push_back(win_);
            break;
        case ConfigureNotify: {
            XConfigureEvent& c = ev.xconfigure;
            if (c.window != win_) break;
            w_ = c.width;
            h_ = c.height;
            bool sized = w_ != reportedW_ || h_ != reportedH_;
            if (sized && (shaped_ || inputShaped_ || blurSet_)) applyShape();  // (its corners follow the new size)
            reportedW_ = w_;
            reportedH_ = h_;
            int ox = x_, oy = y_;
            if (c.send_event) {
                x_ = c.x;
                y_ = c.y;
            } else {
                updatePosition();
            }
            bool moved = ox != x_ || oy != y_;
            XWin self = win_;
            if (sized) {
                Event e;
                e.type = Event::Resize;
                e.w = w_;
                e.h = h_;
                emit(e);
                if (!s.windows.count(self)) return;
            }
            if (moved && !opts_.offscreen) {
                Event e;
                e.type = Event::Move;
                e.x = x_;
                e.y = y_;
                emit(e);
            }
            break;
        }
        case MapNotify:
            mapped_ = true;
            updatePosition();
            readFrameExtents();
            if (focusOnMap_) {
                // Popups (menus) take the keys with a keyboard grab, not the focus: window managers
                // (KWin) give the focus straight back to the active window, and that window keeps
                // its focus (no FocusOut that would close the menu).
                focusOnMap_ = false;
                kbdGrabbed_ = XGrabKeyboard(s.dpy, win_, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
                if (kbdGrabbed_ && !focus_) {
                    focus_ = true;
                    Event e;
                    e.type = EvFocusIn;
                    emit(e);
                    return;
                }
            }
            break;
        case UnmapNotify:
            mapped_ = false;
            forgetHeldKeys();  // (X lets go of a grab whose window can't be seen)
            if (kbdGrabbed_) {
                XUngrabKeyboard(s.dpy, CurrentTime);
                kbdGrabbed_ = false;
            }
            break;
        case ReparentNotify:
            updatePosition();
            break;
        case PropertyNotify:
            if (ev.xproperty.atom == s.a.NET_WM_STATE) readNetState(true);
            else if (ev.xproperty.atom == s.a.NET_FRAME_EXTENTS) readFrameExtents();
            break;
        case ClientMessage:
            handleClientMessage(ev.xclient);
            break;
        case ButtonPress:
        case ButtonRelease: {
            XButtonEvent& b = ev.xbutton;
            Event e;
            e.time = eventSeconds(b.time);
            e.x = b.x;
            e.y = b.y;
            e.mods = modsFromState(b.state);
            if (b.button >= 4 && b.button <= 7) {
                if (ev.type == ButtonRelease) break;
                e.type = Event::Wheel;
                e.dy = b.button == 4 ? 1.f : b.button == 5 ? -1.f : 0.f;
                e.dx = b.button == 6 ? 1.f : b.button == 7 ? -1.f : 0.f;
                e.buttons = buttonsFromState(b.state);
                emit(e);
                break;
            }
            int btn = buttonOf(b.button);
            if (!btn) break;
            if (ev.type == ButtonPress && embedFocus_) {  // (our own part of the window: the keys are ours again)
                embedFocus_ = 0;
                XSetInputFocus(s.dpy, win_, RevertToParent, b.time);
            }
            if (ev.type == ButtonPress) {
                s.lastInputTime = b.time;
                e.type = Event::MouseDown;
                e.buttons = buttonsFromState(b.state) | btn;
            } else {
                e.type = Event::MouseUp;
                e.buttons = buttonsFromState(b.state) & ~btn;
            }
            e.button = btn;
            emit(e);
            break;
        }
        case MotionNotify: {
            // only the latest of consecutive motions
            XEvent next;
            while (XEventsQueued(s.dpy, QueuedAlready) > 0) {
                XPeekEvent(s.dpy, &next);
                if (next.type != MotionNotify || next.xmotion.window != win_) break;
                XNextEvent(s.dpy, &ev);
            }
            XMotionEvent& m = ev.xmotion;
            Event e;
            e.type = Event::MouseMove;
            e.time = eventSeconds(m.time);
            e.x = m.x;
            e.y = m.y;
            e.mods = modsFromState(m.state);
            e.buttons = buttonsFromState(m.state);
            pointerInside_ = true;
            emit(e);
            break;
        }
        case EnterNotify:
        case LeaveNotify: {
            XCrossingEvent& c = ev.xcrossing;
            bool in = ev.type == EnterNotify;
            if (in == pointerInside_ && c.mode == NotifyNormal) break;
            pointerInside_ = in;
            Event e;
            e.type = in ? Event::Enter : Event::Leave;
            e.time = eventSeconds(c.time);
            e.x = c.x;
            e.y = c.y;
            e.mods = modsFromState(c.state);
            e.buttons = buttonsFromState(c.state);
            emit(e);
            break;
        }
        case FocusIn:
        case FocusOut: {
            XFocusChangeEvent& f = ev.xfocus;
            // (another grab took the keyboard from ours, a popup's: what is held is let go of elsewhere)
            if (ev.type == FocusOut && f.mode == NotifyGrab && f.detail != NotifyInferior) forgetHeldKeys();
            if (f.mode == NotifyGrab || f.mode == NotifyUngrab || f.detail == NotifyPointer) break;
            if (kbdGrabbed_) break;  // a popup with the keyboard grab counts as focused while shown
            bool in = ev.type == FocusIn;
            if (ic_) {
                if (in) XSetICFocus(ic_);
                else XUnsetICFocus(ic_);
            }
            if (!in) {
                s.keysDown.clear();
                s.repeatKeycode = 0;
            }
            if (f.detail != NotifyInferior) {  // (the focus came to our window, or left it: not just to a window inside)
                if (in) focusEmbedded();
                else deactivateEmbedded();
            }
            if (in == focus_) break;
            focus_ = in;
            Event e;
            e.type = in ? EvFocusIn : EvFocusOut;
            emit(e);
            break;
        }
        case KeyPress:
            s.lastInputTime = ev.xkey.time;
            noteKey(ev.xkey, true);
            handleKey(ev.xkey, true);
            break;
        case KeyRelease:
            noteKey(ev.xkey, false);
            handleKey(ev.xkey, false);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// selections (clipboard, drops)

bool convertSelection(XWin requestor, Atom selection, Atom target, Time t, int timeoutMs, std::string& out,
                      Atom* gotType) {
    State& s = S();
    Display* d = s.dpy;
    Atom prop = s.a.TINYDAW_SEL;
    out.clear();
    XDeleteProperty(d, requestor, prop);
    XConvertSelection(d, selection, target, prop, requestor, t ? t : CurrentTime);
    XFlush(d);
    double deadline = monoNow() + timeoutMs / 1000.0;
    XEvent ev;
    for (;;) {
        if (XCheckTypedWindowEvent(d, requestor, SelectionNotify, &ev)) {
            if (ev.xselection.selection != selection) continue;
            break;
        }
        // asked something ourselves meanwhile (we may own the clipboard)
        if (XCheckTypedEvent(d, SelectionRequest, &ev)) {
            handleSelectionRequest(ev.xselectionrequest);
            continue;
        }
        double left = deadline - monoNow();
        if (left <= 0) return false;
        waitReadable(ConnectionNumber(d), std::min(left, 0.02));
    }
    if (ev.xselection.property == None) return false;
    Atom type;
    int format;
    std::vector<unsigned char> data;
    if (!readProperty(requestor, prop, type, format, data, true)) return false;
    if (gotType) *gotType = type;
    if (type != s.a.INCR) {
        out.assign(data.begin(), data.end());
        return true;
    }
    // INCR: chunks arrive as new values of the property, until an empty one
    XFlush(d);
    deadline = monoNow() + 2.0;
    for (;;) {
        if (XCheckIfEvent(d, &ev,
                          [](Display*, XEvent* e, XPointer arg) -> Bool {
                              XWin w = *(XWin*)arg;
                              return e->type == PropertyNotify && e->xproperty.window == w &&
                                     e->xproperty.atom == S().a.TINYDAW_SEL && e->xproperty.state == PropertyNewValue;
                          },
                          (XPointer)&requestor)) {
            if (!readProperty(requestor, prop, type, format, data, true)) return false;
            if (data.empty()) return true;
            out.append(data.begin(), data.end());
            if (gotType) *gotType = type;
            deadline = monoNow() + 2.0;
            continue;
        }
        double left = deadline - monoNow();
        if (left <= 0) return false;
        waitReadable(ConnectionNumber(d), std::min(left, 0.02));
    }
}

void handleSelectionRequest(const XSelectionRequestEvent& r) {
    State& s = S();
    const Atoms& A = s.a;
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = s.dpy;
    reply.xselection.requestor = r.requestor;
    reply.xselection.selection = r.selection;
    reply.xselection.target = r.target;
    reply.xselection.time = r.time;
    reply.xselection.property = None;
    Atom prop = r.property != None ? r.property : r.target;
    if (r.selection == A.CLIPBOARD && s.ownClipboard) {
        long maxBytes = XExtendedMaxRequestSize(s.dpy);
        if (!maxBytes) maxBytes = XMaxRequestSize(s.dpy);
        maxBytes = maxBytes * 4 - 256;
        if (r.target == A.TARGETS) {
            Atom list[5] = {A.TARGETS, A.UTF8_STRING, A.TEXT_PLAIN_UTF8, XA_STRING, A.TEXT};
            XChangeProperty(s.dpy, r.requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char*)list, 5);
            reply.xselection.property = prop;
        } else if ((r.target == A.UTF8_STRING || r.target == A.TEXT || r.target == A.TEXT_PLAIN_UTF8) &&
                   (long)s.clipText.size() < maxBytes) {
            XChangeProperty(s.dpy, r.requestor, prop, r.target == A.TEXT ? A.UTF8_STRING : r.target, 8, PropModeReplace,
                            (const unsigned char*)s.clipText.data(), (int)s.clipText.size());
            reply.xselection.property = prop;
        } else if (r.target == XA_STRING && (long)s.clipText.size() < maxBytes) {
            std::string l1 = utf8ToLatin1(s.clipText);
            XChangeProperty(s.dpy, r.requestor, prop, XA_STRING, 8, PropModeReplace, (const unsigned char*)l1.data(),
                            (int)l1.size());
            reply.xselection.property = prop;
        }
    }
    XSendEvent(s.dpy, r.requestor, False, NoEventMask, &reply);
    XFlush(s.dpy);
}

// ---------------------------------------------------------------------------------------------
// transparency: is there a compositing manager (it owns _NET_WM_CM_S<screen>)? Its window is watched for
// DestroyNotify (it went); MANAGER on the root window says when one starts. tell: the windows and the
// listener hear of a change

// every window shown as hasAlpha() says now (its shapes, blur), and the listener told
void alphaChanged() {
    State& s = S();
    std::vector<XWindow*> all;
    for (auto& kv : s.windows)
        if (std::find(all.begin(), all.end(), kv.second) == all.end()) all.push_back(kv.second);
    for (XWindow* w : all) w->alphaChanged();
    if (s.compositingListener) {
        std::function<void()> fn = s.compositingListener;
        postToMain([fn] { fn(); });
    }
}

void checkCompositing(bool tell) {
    State& s = S();
    if (!s.dpy || !s.cmAtom) return;
    XWin owner = XGetSelectionOwner(s.dpy, s.cmAtom);
    if (owner && owner != s.cmOwner) {
        ErrorTrap trap;  // (it may be gone already: then there is none)
        XSelectInput(s.dpy, owner, StructureNotifyMask);
        if (trap.end()) owner = 0;
    }
    s.cmOwner = owner;
    bool was = s.compositing;
    s.compositing = owner != 0;
    if (tell && was != s.compositing) alphaChanged();
}

// ---------------------------------------------------------------------------------------------
// the main loop's parts

void handleXEvent(XEvent& ev) {
    State& s = S();
    if (Time t = eventTime(ev)) s.lastTime = t;
    // a compositing manager started (MANAGER on the root window, for its selection) or went (its window destroyed)
    if ((ev.type == ClientMessage && ev.xclient.window == s.root && ev.xclient.message_type == s.a.MANAGER &&
         (Atom)ev.xclient.data.l[1] == s.cmAtom) ||
        (ev.type == DestroyNotify && s.cmOwner && ev.xdestroywindow.window == s.cmOwner)) {
        checkCompositing(true);
        return;
    }
    switch (ev.type) {
        case SelectionRequest:
            handleSelectionRequest(ev.xselectionrequest);
            return;
        case SelectionClear:
            if (ev.xselectionclear.selection == s.a.CLIPBOARD) s.ownClipboard = false;
            return;
        case MappingNotify:
            if (ev.xmapping.request != MappingPointer) {
                XRefreshKeyboardMapping(&ev.xmapping);
                s.keymapChanges++;
            }
            return;
        default:
            break;
    }
    if (ev.type == s.shmCompletion) {
        auto it = s.windows.find(((XShmCompletionEvent&)ev).drawable);
        if (it != s.windows.end()) it->second->shmDone();
        return;
    }
    auto it = s.windows.find(ev.xany.window);
    if (it == s.windows.end()) return;
    it->second->handle(ev);
}

void dispatchX() {
    State& s = S();
    if (!s.dpy) return;
    int n = 0;
    while (n++ < 2000 && XPending(s.dpy)) {
        XEvent ev;
        XNextEvent(s.dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;
        handleXEvent(ev);
    }
}

void flushExposes() {
    State& s = S();
    while (!s.pendingExpose.empty()) {
        std::vector<XWin> ids;
        ids.swap(s.pendingExpose);
        for (XWin id : ids) {
            auto it = s.windows.find(id);
            if (it == s.windows.end()) continue;
            Event e;
            e.type = EvExpose;
            it->second->deliver(e);
        }
    }
}

void runPosted() {
    State& s = S();
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(s.postMutex);
        work.swap(s.posted);
    }
    for (auto& fn : work) {
        if (fn) fn();
    }
}

void runTimers() {
    State& s = S();
    double t = monoNow();
    std::vector<std::pair<double, int>> due;
    for (auto& kv : s.timers)
        if (kv.second.deadline <= t) due.push_back({kv.second.deadline, kv.first});
    std::sort(due.begin(), due.end());
    for (auto& d : due) {
        auto it = s.timers.find(d.second);
        if (it == s.timers.end() || it->second.deadline > t) continue;  // removed or rescheduled meanwhile
        std::shared_ptr<std::function<void()>> fn = it->second.fn;
        if (it->second.repeat) {
            Timer& tm = it->second;
            if (tm.intervalMs <= 0) {
                tm.deadline = t;
            } else {
                double iv = tm.intervalMs / 1000.0;
                tm.deadline += iv;  // keeps its phase: no drift
                if (tm.deadline <= t) tm.deadline += std::ceil((t - tm.deadline) / iv + 1e-9) * iv;  // skip missed ticks
                if (tm.deadline <= t) tm.deadline = t + iv;
            }
        } else {
            s.timers.erase(it);
        }
        if (fn && *fn) (*fn)();
    }
}

void wake() {
    int fd = S().wakeFd;
    if (fd >= 0) {
        uint64_t one = 1;
        ssize_t r = write(fd, &one, sizeof one);
        (void)r;
    }
}

std::string dirOf(const std::string& path) {
    size_t p = path.rfind('/');
    if (p == std::string::npos) return ".";
    if (p == 0) return "/";
    return path.substr(0, p);
}

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void makeDirs(const std::string& path) {
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        size_t e = path.find('/', i);
        if (e == std::string::npos) e = path.size();
        cur = path.substr(0, e);
        if (!cur.empty()) mkdir(cur.c_str(), 0755);
        i = e + 1;
    }
}

// fork + exec, detached (a grandchild, so no zombie and no waiting on it); true if exec worked
bool spawnDetached(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        setsid();
        pid_t p2 = fork();
        if (p2 != 0) _exit(p2 < 0 ? 1 : 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execvp(argv[0], argv.data());
        int e = errno;
        ssize_t w = write(pfd[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(pfd[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int e = 0;
    ssize_t n;
    do {
        n = read(pfd[0], &e, sizeof e);
    } while (n < 0 && errno == EINTR);
    close(pfd[0]);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 && n <= 0;
}

void loadMonitors() {
    State& s = S();
    double t = monoNow();
    if (t - s.monitorsAt < 2.0 && !s.monitors.empty()) return;
    s.monitorsAt = t;
    s.monitors.clear();
    s.primaryMonitor = 0;
    if (!s.dpy) {
        s.monitors.push_back(Rect(0, 0, 1920, 1080));
        return;
    }
    int n = 0;
    XRRMonitorInfo* mons = nullptr;
    int evb, erb;
    if (XRRQueryExtension(s.dpy, &evb, &erb)) mons = XRRGetMonitors(s.dpy, s.root, True, &n);
    for (int i = 0; i < n; i++) {
        s.monitors.push_back(Rect(mons[i].x, mons[i].y, mons[i].width, mons[i].height));
        if (mons[i].primary) s.primaryMonitor = i;
    }
    if (mons) XRRFreeMonitors(mons);
    if (s.monitors.empty())
        s.monitors.push_back(Rect(0, 0, DisplayWidth(s.dpy, s.screen), DisplayHeight(s.dpy, s.screen)));
}

// the monitor rect minus the panels (_NET_WORKAREA of the current desktop)
Rect available(const Rect& mon) {
    State& s = S();
    if (!s.dpy) return mon;
    std::vector<long> wa = readLongs(s.root, s.a.NET_WORKAREA, XA_CARDINAL);
    if (wa.size() < 4) return mon;
    size_t desk = 0;
    std::vector<long> cur = readLongs(s.root, s.a.NET_CURRENT_DESKTOP, XA_CARDINAL);
    if (!cur.empty() && (size_t)cur[0] * 4 + 4 <= wa.size()) desk = (size_t)cur[0];
    Rect work((int)wa[desk * 4], (int)wa[desk * 4 + 1], (int)wa[desk * 4 + 2], (int)wa[desk * 4 + 3]);
    Rect r = mon.intersected(work);
    return r.empty() ? mon : r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// the public functions

std::unique_ptr<Window> Window::create(const WindowOptions& opts, WindowListener* listener) {
    if (!S().dpy) return nullptr;
    return std::unique_ptr<Window>(new XWindow(opts, listener));
}

void initWithoutDisplay() {
    State& s = S();
    s.mainThread = std::this_thread::get_id();
    if (s.wakeFd < 0) s.wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

void init(int argc, char** argv) {
    State& s = S();
    if (s.dpy) return;
    initWithoutDisplay();
    std::string why;
    if (!xlib::load(why)) {
        fprintf(stderr, "Tiny DAW: can't show windows: %s (tinydaw --tui runs in the terminal)\n", why.c_str());
        return;
    }
    XInitThreads();
    setlocale(LC_CTYPE, "");
    if (!XSupportsLocale()) setlocale(LC_CTYPE, "C.UTF-8");
    XSetLocaleModifiers("");
    s.dpy = XOpenDisplay(nullptr);
    if (!s.dpy) {
        fprintf(stderr, "Tiny DAW: can't open the display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(DISPLAY not set)");
        return;
    }
    XSetErrorHandler(onXError);
    XSetIOErrorHandler(onXIOError);
    s.screen = DefaultScreen(s.dpy);
    s.root = RootWindow(s.dpy, s.screen);

    // a 24-bit TrueColor visual (the default one, almost always)
    Visual* dv = DefaultVisual(s.dpy, s.screen);
    int dd = DefaultDepth(s.dpy, s.screen);
    if (dd == 24 && dv->c_class == TrueColor) {
        s.visual = dv;
        s.depth = 24;
        s.cmap = DefaultColormap(s.dpy, s.screen);
    } else {
        XVisualInfo vi;
        if (XMatchVisualInfo(s.dpy, s.screen, 24, TrueColor, &vi)) {
            s.visual = vi.visual;
            s.depth = 24;
            s.cmap = XCreateColormap(s.dpy, s.root, vi.visual, AllocNone);
            s.ownCmap = true;
        } else {
            s.visual = dv;
            s.depth = dd;
            s.cmap = DefaultColormap(s.dpy, s.screen);
        }
    }

    static const char* names[] = {
        "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_PING", "_NET_WM_NAME", "_NET_WM_ICON_NAME", "UTF8_STRING",
        "_NET_WM_PID", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG",
        "_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_TOOLTIP",
        "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT",
        "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_SKIP_TASKBAR",
        "_NET_WM_STATE_SKIP_PAGER", "_NET_ACTIVE_WINDOW", "_NET_WM_ICON", "_NET_FRAME_EXTENTS", "_NET_WORKAREA",
        "_NET_CURRENT_DESKTOP", "_NET_WM_USER_TIME", "CLIPBOARD", "TARGETS", "TEXT", "text/plain;charset=utf-8",
        "INCR", "TINYDAW_SELECTION", "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop",
        "XdndFinished", "XdndSelection", "XdndTypeList", "XdndActionCopy", "text/uri-list", "_MOTIF_WM_HINTS",
        "CLIPBOARD_MANAGER", "SAVE_TARGETS", "_XEMBED", "_NET_WM_MOVERESIZE", "_NET_WM_STATE_HIDDEN",
        "_NET_SUPPORTED", "_NET_WM_WINDOW_OPACITY", "_KDE_NET_WM_BLUR_BEHIND_REGION", "MANAGER"};
    const int count = (int)(sizeof names / sizeof names[0]);
    static_assert(sizeof(Atoms) == sizeof(Atom) * (sizeof names / sizeof names[0]), "atom names and Atoms differ");
    XInternAtoms(s.dpy, (char**)names, count, False, (Atom*)&s.a);

    // MIT-SHM (local displays)
    int major, minor;
    Bool pixmaps;
    if (XShmQueryExtension(s.dpy) && XShmQueryVersion(s.dpy, &major, &minor, &pixmaps)) {
        s.shm = true;
        s.shmCompletion = XShmGetEventBase(s.dpy) + ShmCompletion;
    }
    int opcode, ev, err;
    if (XQueryExtension(s.dpy, "Composite", &opcode, &ev, &err)) {
        s.compositeOpcode = opcode;
        if (!compositeQueryVersion()) s.compositeOpcode = 0;
    }
    s.xwayland = XQueryExtension(s.dpy, "XWAYLAND", &opcode, &ev, &err);

    // transparency: a 32-bit TrueColor visual whose colours are 0xFF0000 / 0xFF00 / 0xFF (alpha the top byte), its
    // colormap; the compositing manager's selection, followed (the root window's MANAGER messages)
    {
        XVisualInfo tmpl{};
        tmpl.screen = s.screen;
        tmpl.depth = 32;
        tmpl.c_class = TrueColor;
        int n = 0;
        XVisualInfo* list = XGetVisualInfo(s.dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
        for (int i = 0; list && i < n && !s.argbVisual; i++)
            if (list[i].red_mask == 0xFF0000 && list[i].green_mask == 0xFF00 && list[i].blue_mask == 0xFF)
                s.argbVisual = list[i].visual;
        if (list) XFree(list);
        if (s.argbVisual) s.argbCmap = XCreateColormap(s.dpy, s.root, s.argbVisual, AllocNone);
        char name[40];
        snprintf(name, sizeof name, "_NET_WM_CM_S%d", s.screen);
        s.cmAtom = XInternAtom(s.dpy, name, False);
        XWindowAttributes ra;
        long rootMask = XGetWindowAttributes(s.dpy, s.root, &ra) ? ra.your_event_mask : 0;
        XSelectInput(s.dpy, s.root, rootMask | StructureNotifyMask);
        checkCompositing(false);
    }

    Bool supported = False;
    XkbSetDetectableAutoRepeat(s.dpy, True, &supported);
    s.detectableRepeat = supported;

    // input method (XIM) for text input; without one, plain XLookupString
    s.im = XOpenIM(s.dpy, nullptr, nullptr, nullptr);
    if (!s.im) {
        XSetLocaleModifiers("@im=none");
        s.im = XOpenIM(s.dpy, nullptr, nullptr, nullptr);
    }
    if (s.im) {
        XIMStyles* styles = nullptr;
        XGetIMValues(s.im, XNQueryInputStyle, &styles, nullptr);
        XIMStyle want[] = {XIMPreeditNothing | XIMStatusNothing, XIMPreeditNothing | XIMStatusNone,
                           XIMPreeditNone | XIMStatusNothing, XIMPreeditNone | XIMStatusNone};
        if (styles) {
            for (XIMStyle w : want) {
                for (int i = 0; i < styles->count_styles && !s.imStyle; i++)
                    if (styles->supported_styles[i] == w) s.imStyle = w;
                if (s.imStyle) break;
            }
            XFree(styles);
        }
        if (!s.imStyle) {
            XCloseIM(s.im);
            s.im = nullptr;
        }
    }

    // the helper window: owns the clipboard and receives selections
    XSetWindowAttributes a{};
    a.event_mask = PropertyChangeMask;
    s.helper = XCreateWindow(s.dpy, s.root, -10, -10, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent, CWEventMask, &a);
    (void)argc;
    (void)argv;
    XFlush(s.dpy);
}

void shutdown() {
    State& s = S();
    if (!s.dpy) return;
    // text we copied: hand it to the clipboard manager (ICCCM SAVE_TARGETS), so it outlives us
    if (s.ownClipboard && XGetSelectionOwner(s.dpy, s.a.CLIPBOARD) == s.helper &&
        XGetSelectionOwner(s.dpy, s.a.CLIPBOARD_MANAGER) != None) {
        XConvertSelection(s.dpy, s.a.CLIPBOARD_MANAGER, s.a.SAVE_TARGETS, None, s.helper, s.lastTime ? s.lastTime : CurrentTime);
        XFlush(s.dpy);
        double deadline = monoNow() + 1.0;
        XEvent ev;
        for (;;) {
            if (XCheckTypedWindowEvent(s.dpy, s.helper, SelectionNotify, &ev) &&
                ev.xselection.selection == s.a.CLIPBOARD_MANAGER)
                break;
            if (XCheckTypedEvent(s.dpy, SelectionRequest, &ev)) {
                handleSelectionRequest(ev.xselectionrequest);
                continue;
            }
            double left = deadline - monoNow();
            if (left <= 0) break;
            waitReadable(ConnectionNumber(s.dpy), std::min(left, 0.02));
        }
    }
    // the windows still open belong to their owners; forget them here
    for (auto& kv : s.cursors)
        if (kv.second) XFreeCursor(s.dpy, kv.second);
    s.cursors.clear();
    if (s.helper) XDestroyWindow(s.dpy, s.helper);
    s.helper = 0;
    if (s.im) XCloseIM(s.im);
    s.im = nullptr;
    if (s.ownCmap) XFreeColormap(s.dpy, s.cmap);
    if (s.argbCmap) XFreeColormap(s.dpy, s.argbCmap);
    s.argbCmap = 0;
    s.argbVisual = nullptr;
    s.compositing = false;
    s.cmOwner = 0;
    // windows deleted after this must not talk to the display
    XCloseDisplay(s.dpy);
    s.dpy = nullptr;
}

bool hasDisplay() { return S().dpy != nullptr; }

bool compositing() { return S().compositing; }

void setTransparency(bool on) {
    State& s = S();
    if (s.transparency == on) return;
    s.transparency = on;
    if (s.dpy) alphaChanged();
}

bool transparency() { return S().transparency; }

void setCompositingListener(std::function<void()> fn) { S().compositingListener = std::move(fn); }

void processEvents(int timeoutMs) {
    State& s = S();
    if (s.wakeFd < 0) s.wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    s.processDepth++;
    struct Leave {
        ~Leave() { S().processDepth--; }
    } leave;

    bool ready = !s.pendingExpose.empty();
    if (s.dpy && XEventsQueued(s.dpy, QueuedAlready) > 0) ready = true;
    {
        std::lock_guard<std::mutex> lock(s.postMutex);
        if (!s.posted.empty()) ready = true;
    }
    double wait = timeoutMs < 0 ? -1.0 : timeoutMs / 1000.0;
    if (!s.timers.empty()) {
        double next = 1e300;
        for (auto& kv : s.timers) next = std::min(next, kv.second.deadline);
        double dt = std::max(0.0, next - monoNow());
        wait = wait < 0 ? dt : std::min(wait, dt);
    }
    if (ready) wait = 0;
    if (s.dpy) XFlush(s.dpy);

    std::vector<pollfd> fds;
    std::vector<int> watchIds;
    if (s.dpy) fds.push_back({ConnectionNumber(s.dpy), POLLIN, 0});
    size_t wakeIndex = fds.size();
    if (s.wakeFd >= 0) fds.push_back({s.wakeFd, POLLIN, 0});
    size_t firstWatch = fds.size();
    for (auto& kv : s.watches) {
        if (kv.second.invalid) continue;
        fds.push_back({kv.second.fd, POLLIN, 0});
        watchIds.push_back(kv.first);
    }
    timespec ts{};
    if (wait >= 0) {
        ts.tv_sec = (time_t)wait;
        ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);
    }
    int r = ppoll(fds.data(), fds.size(), wait < 0 ? nullptr : &ts, nullptr);
    if (r > 0 && s.wakeFd >= 0 && wakeIndex < fds.size() && (fds[wakeIndex].revents & POLLIN)) {
        uint64_t v;
        ssize_t n = read(s.wakeFd, &v, sizeof v);
        (void)n;
    }

    dispatchX();
    if (r > 0) {
        for (size_t i = 0; i < watchIds.size(); i++) {
            short re = fds[firstWatch + i].revents;
            if (!re) continue;
            auto it = s.watches.find(watchIds[i]);
            if (it == s.watches.end()) continue;  // removed by an earlier callback
            if (re & POLLNVAL) {
                fprintf(stderr, "Tiny DAW: a watched file descriptor (%d) was closed; no longer watched\n", it->second.fd);
                it->second.invalid = true;
                continue;
            }
            std::shared_ptr<std::function<void()>> fn = it->second.fn;
            if (fn && *fn) (*fn)();
        }
    }
    runPosted();
    runTimers();
    dispatchX();
    flushExposes();
    if (s.dpy) XFlush(s.dpy);
}

void run() {
    State& s = S();
    s.quit = false;
    while (!s.quit) processEvents(-1);
}

void quit() {
    S().quit = true;
    wake();
}

bool quitting() { return S().quit; }

int addTimer(int intervalMs, std::function<void()> fn, bool repeat) {
    return addTimerMs((double)intervalMs, std::move(fn), repeat);
}

int addTimerMs(double intervalMs, std::function<void()> fn, bool repeat) {
    State& s = S();
    int id = s.nextTimer++;
    Timer t;
    t.intervalMs = std::max(0.0, intervalMs);
    t.deadline = monoNow() + t.intervalMs / 1000.0;
    t.repeat = repeat;
    t.fn = std::make_shared<std::function<void()>>(std::move(fn));
    s.timers[id] = std::move(t);
    return id;
}

void removeTimer(int id) { S().timers.erase(id); }

double displayRefreshRate() {
    // each lit CRTC's mode: pixel clock / (total width x total height), the highest of them
    State& s = S();
    int evb, erb;
    if (!s.dpy || !XRRQueryExtension(s.dpy, &evb, &erb)) return 0;
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(s.dpy, s.root);
    if (!res) return 0;
    double best = 0;
    for (int c = 0; c < res->ncrtc; c++) {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo(s.dpy, res, res->crtcs[c]);
        if (!crtc) continue;
        for (int m = 0; crtc->mode && m < res->nmode; m++) {
            const XRRModeInfo& mode = res->modes[m];
            if (mode.id != crtc->mode || !mode.hTotal || !mode.vTotal) continue;
            double v = mode.vTotal;
            if (mode.modeFlags & RR_DoubleScan) v *= 2;
            if (mode.modeFlags & RR_Interlace) v /= 2;
            best = std::max(best, (double)mode.dotClock / ((double)mode.hTotal * v));
        }
        XRRFreeCrtcInfo(crtc);
    }
    XRRFreeScreenResources(res);
    return best;
}

int watchFd(int fd, std::function<void()> fn) {
    State& s = S();
    int id = s.nextWatch++;
    Watch w;
    w.fd = fd;
    w.fn = std::make_shared<std::function<void()>>(std::move(fn));
    s.watches[id] = std::move(w);
    return id;
}

void unwatchFd(int id) { S().watches.erase(id); }

void postToMain(std::function<void()> fn) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lock(s.postMutex);
        s.posted.push_back(std::move(fn));
    }
    wake();
}

bool isMainThread() { return std::this_thread::get_id() == S().mainThread; }

double now() { return monoNow(); }
double doubleClickTime() { return 0.4; }
int systemScale() { return 100; }
int doubleClickDistance() { return 5; }

Rect screenGeometry(Point near) {
    State& s = S();
    loadMonitors();
    const Rect* best = &s.monitors[(size_t)s.primaryMonitor];
    long bestDist = LONG_MAX;
    for (const Rect& m : s.monitors) {
        if (m.contains(near)) {
            best = &m;
            break;
        }
        long dx = near.x < m.x ? m.x - near.x : near.x >= m.right() ? near.x - m.right() + 1 : 0;
        long dy = near.y < m.y ? m.y - near.y : near.y >= m.bottom() ? near.y - m.bottom() + 1 : 0;
        long d = dx * dx + dy * dy;
        if (d < bestDist) {
            bestDist = d;
            best = &m;
        }
    }
    return available(*best);
}

Rect primaryScreenGeometry() {
    State& s = S();
    loadMonitors();
    return available(s.monitors[(size_t)s.primaryMonitor]);
}

namespace {
bool queryPointer(int& x, int& y, unsigned& mask) {
    State& s = S();
    x = y = 0;
    mask = 0;
    if (!s.dpy) return false;
    XWin rootRet, child;
    int wx, wy;
    return XQueryPointer(s.dpy, s.root, &rootRet, &child, &x, &y, &wx, &wy, &mask);
}
}  // namespace

Point cursorPos() {
    int x, y;
    unsigned m;
    queryPointer(x, y, m);
    return Point(x, y);
}

int heldButtons() {
    int x, y;
    unsigned m;
    queryPointer(x, y, m);
    return buttonsFromState(m);
}

int heldModifiers() {
    int x, y;
    unsigned m;
    queryPointer(x, y, m);
    return modsFromState(m);
}

void setClipboardText(const std::string& text) {
    State& s = S();
    s.clipText = text;
    if (!s.dpy) {
        s.ownClipboard = true;
        return;
    }
    XSetSelectionOwner(s.dpy, s.a.CLIPBOARD, s.helper, s.lastTime ? s.lastTime : CurrentTime);
    s.ownClipboard = XGetSelectionOwner(s.dpy, s.a.CLIPBOARD) == s.helper;
    XFlush(s.dpy);
}

std::string clipboardText() {
    State& s = S();
    if (!s.dpy) return s.ownClipboard ? s.clipText : std::string();
    XWin owner = XGetSelectionOwner(s.dpy, s.a.CLIPBOARD);
    if (owner == None) return std::string();
    if (owner == s.helper && s.ownClipboard) return s.clipText;
    std::string out;
    Atom type = 0;
    if (convertSelection(s.helper, s.a.CLIPBOARD, s.a.UTF8_STRING, s.lastTime, 200, out, &type)) return out;
    if (convertSelection(s.helper, s.a.CLIPBOARD, XA_STRING, s.lastTime, 200, out, &type))
        return type == s.a.UTF8_STRING ? out : latin1ToUtf8(out.data(), (int)out.size());
    return std::string();
}

std::string homeDir() {
    const char* h = getenv("HOME");
    if (h && *h) return h;
    struct passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/tmp";
}

std::string configDir() {
    const char* x = getenv("XDG_CONFIG_HOME");
    std::string base = (x && *x == '/') ? std::string(x) : homeDir() + "/.config";
    std::string dir = base + "/TinyDAW";
    if (!isDir(dir)) makeDirs(dir);
    return dir;
}

std::string executableDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return dirOf(buf);
}

std::string dataDir() {
    std::string exe = executableDir();
    if (isDir(exe + "/data")) return exe + "/data";
    std::string share = dirOf(exe) + "/share/tinydaw";
    if (isDir(share)) return share;
    return exe + "/data";
}

bool openUrl(const std::string& url) {
    if (url.empty()) return false;
    return spawnDetached({"xdg-open", url});
}

bool openFolder(const std::string& path) {
    if (path.empty()) return false;
    return spawnDetached({"xdg-open", path});
}

void beep() {
    State& s = S();
    if (!s.dpy) return;
    XBell(s.dpy, 0);
    XFlush(s.dpy);
}

}  // namespace plat
