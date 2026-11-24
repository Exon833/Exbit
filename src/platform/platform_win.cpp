// platform/platform.h on Windows (Win32), as platform/x11.cpp is on Linux: windows (top-level, dialogs, popups and
// tooltips, the program's own frames), painting (the framebuffer blitted from a DIB section), mouse, keys and text
// (WM_CHAR's UTF-16 as UTF-8, IME results), the wheel, cursors, the clipboard's text, files dropped from Explorer
// (OLE's IDropTarget: enter, move, leave, drop), per-monitor DPI, the screens' work areas, the main loop (timers,
// watched handles, work posted from other threads), icons, focus, sizes, close requests, and the desktop's folders.
// Coordinates are device pixels, as on X11: the program draws at its own interface size (App::scale).
#include "platform/platform.h"

#define UNICODE  // (IDC_ARROW… as wide strings)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

#include "core/os.h"
#include "core/util.h"
#include "gfx/svg.h"

namespace plat {

namespace {

const wchar_t* kClass = L"TinyDAW";
const wchar_t* kEmbedClass = L"TinyDAWEmbed";
const wchar_t* kHelperClass = L"TinyDAWHelper";
constexpr UINT WM_APP_FOCUSIN = WM_APP + 1;  // a popup that takes the keys was shown (posted: as X's MapNotify)
constexpr UINT_PTR MODAL_TIMER = 1;          // the main loop's work while Windows moves or sizes a window

struct Timer {
    double deadline = 0;
    double intervalMs = 0;
    bool repeat = true;
    std::shared_ptr<std::function<void()>> fn;
};

struct Watch {
    int fd = -1;
    HANDLE handle = nullptr;
    bool pipe = false;  // (pipes can't be waited for: asked for bytes on every turn of the loop)
    bool invalid = false;
    std::shared_ptr<std::function<void()>> fn;
};

class WinWindow;

struct State {
    bool display = false;
    HINSTANCE inst = nullptr;
    HWND helper = nullptr;  // owns the clipboard
    std::map<HWND, WinWindow*> windows;
    std::map<int, Timer> timers;
    int nextTimer = 1;
    std::map<int, Watch> watches;
    int nextWatch = 1;
    std::mutex postMutex;
    std::vector<std::function<void()>> posted;
    HANDLE wake = nullptr;
    std::thread::id mainThread;
    bool quit = false;
    std::vector<HWND> pendingExpose;
    WinWindow* keyGrab = nullptr;  // a popup shown that takes the keys (menus): every key goes to it
    bool transparency = true;
    std::function<void()> compositingListener;
    std::string clipText;          // (without a display)
    std::map<Cursor, HCURSOR> cursors;
    double refreshRate = -1;       // (displayRefreshRate, cached until the displays change)
    bool oleOk = false;
};

State& S() {
    static State s;
    return s;
}

double monoNow() { return os::monotonicSeconds(); }

int currentMods() {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) m |= ModMeta;
    return m;
}

int buttonsOf(WPARAM w) {
    int b = 0;
    if (w & MK_LBUTTON) b |= LeftButton;
    if (w & MK_RBUTTON) b |= RightButton;
    if (w & MK_MBUTTON) b |= MiddleButton;
    return b;
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
}

uint32_t firstCodepoint(const std::string& s) {
    if (s.empty()) return 0;
    unsigned char c = s[0];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 31; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 15; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 7; n = 4; }
    else return 0xFFFD;
    if (s.size() < n) return 0xFFFD;
    for (size_t i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 63);
    return cp;
}

uint32_t upperCodepoint(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 128) return c;
    return (uint32_t)std::towupper((wint_t)c);
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

// platform.h's key for a virtual key: printable keys as what they type with Shift alone (X's keysym: Shift+1 is
// '!'), letters upper case; 0 if it's no key we name
int keyFromVk(UINT vk, UINT scan, bool extended) {
    switch (vk) {
        case VK_ESCAPE: return Key_Escape;
        case VK_TAB: return (GetKeyState(VK_SHIFT) & 0x8000) ? Key_Backtab : Key_Tab;
        case VK_BACK: return Key_Backspace;
        case VK_RETURN: return extended ? Key_Enter : Key_Return;
        case VK_INSERT: return Key_Insert;
        case VK_DELETE: return Key_Delete;
        case VK_PAUSE: case VK_CANCEL: return Key_Pause;
        case VK_SNAPSHOT: return Key_Print;
        case VK_HOME: return Key_Home;
        case VK_END: return Key_End;
        case VK_LEFT: return Key_Left;
        case VK_UP: return Key_Up;
        case VK_RIGHT: return Key_Right;
        case VK_DOWN: return Key_Down;
        case VK_PRIOR: return Key_PageUp;
        case VK_NEXT: return Key_PageDown;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return Key_Shift;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return Key_Control;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return Key_Alt;
        case VK_LWIN: case VK_RWIN: return Key_Meta;
        case VK_CAPITAL: return Key_CapsLock;
        case VK_APPS: return Key_Menu;
        case VK_SPACE: return ' ';
        case VK_MULTIPLY: return '*';
        case VK_ADD: return '+';
        case VK_SEPARATOR: return ',';
        case VK_SUBTRACT: return '-';
        case VK_DECIMAL: return '.';
        case VK_DIVIDE: return '/';
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return Key_F1 + (int)(vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (int)(vk - VK_NUMPAD0);
    BYTE state[256] = {};
    if (GetKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
    wchar_t buf[8];
    int n = ToUnicode(vk, scan, state, buf, 8, 4);  // (4: the keyboard's dead-key state is left as it is)
    if (n == 1 && buf[0] >= 0x20 && buf[0] != 0x7F) return (int)upperCodepoint(buf[0]);
    UINT c = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (c >= 0x20 && c != 0x7F) return (int)upperCodepoint(c);
    return 0;
}

HCURSOR cursorFor(Cursor c) {
    State& s = S();
    auto it = s.cursors.find(c);
    if (it != s.cursors.end()) return it->second;
    LPCWSTR id = IDC_ARROW;
    switch (c) {
        case Cursor::Arrow: id = IDC_ARROW; break;
        case Cursor::IBeam: id = IDC_IBEAM; break;
        case Cursor::Wait: id = IDC_WAIT; break;
        case Cursor::Cross: id = IDC_CROSS; break;
        case Cursor::Hand: id = IDC_HAND; break;
        case Cursor::OpenHand: id = IDC_HAND; break;      // (Windows has no open or closed hand of its own)
        case Cursor::ClosedHand: id = IDC_SIZEALL; break;
        case Cursor::SizeHor: id = IDC_SIZEWE; break;
        case Cursor::SizeVer: id = IDC_SIZENS; break;
        case Cursor::SizeAll: id = IDC_SIZEALL; break;
        case Cursor::SizeFDiag: id = IDC_SIZENWSE; break;
        case Cursor::SizeBDiag: id = IDC_SIZENESW; break;
        case Cursor::Forbidden: id = IDC_NO; break;
        case Cursor::Blank: id = nullptr; break;
    }
    HCURSOR h = id ? LoadCursorW(nullptr, id) : nullptr;
    s.cursors[c] = h;
    return h;
}

// what lies inside a window with rounded corners, as rectangles (a row of pixels each in the corners)
std::vector<RECT> roundedRects(int w, int h, const int radii[4]) {
    std::vector<RECT> rects;
    if (w <= 0 || h <= 0) return rects;
    auto inset = [](int r, int row) {
        double d = r - row - 0.5;
        return (int)std::lround(r - std::sqrt(std::max(0.0, (double)r * r - d * d)));
    };
    int top = std::min(h / 2, std::max(radii[0], radii[1]));
    int bottom = std::min(h - top, std::max(radii[2], radii[3]));
    auto row = [&](int x, int y, int rw, int rh) { rects.push_back(RECT{x, y, x + rw, y + rh}); };
    for (int y = 0; y < top; y++) {
        int l = y < radii[0] ? inset(std::min(radii[0], w / 2), y) : 0;
        int r = y < radii[1] ? inset(std::min(radii[1], w / 2), y) : 0;
        if (w - l - r > 0) row(l, y, w - l - r, 1);
    }
    if (h - top - bottom > 0) row(0, top, w, h - top - bottom);
    for (int i = bottom - 1; i >= 0; i--) {
        int y = h - 1 - i;
        int l = i < radii[3] ? inset(std::min(radii[3], w / 2), i) : 0;
        int r = i < radii[2] ? inset(std::min(radii[2], w / 2), i) : 0;
        if (w - l - r > 0) row(l, y, w - l - r, 1);
    }
    return rects;
}

HICON iconFromSvg(const std::string& svg, int size) {
    Image img = renderSvg(svg, size, size);
    if (img.empty()) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = img.width();
    bi.bV5Height = -img.height();
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    uint32_t* px = (uint32_t*)bits;
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) {
            Color c = img.colorAt(x, y);  // (icons want straight alpha)
            px[y * img.width() + x] = ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
        }
    HBITMAP mask = CreateBitmap(img.width(), img.height(), 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

std::vector<std::string> filesOf(IDataObject* data) {
    std::vector<std::string> out;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (!data || FAILED(data->GetData(&fmt, &m))) return out;
    if (HDROP drop = (HDROP)GlobalLock(m.hGlobal)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len + 1, L'\0');
            DragQueryFileW(drop, i, &w[0], len + 1);
            w.resize(len);
            out.push_back(os::portablePath(os::narrow(w)));
        }
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return out;
}

LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l);

// ---------------------------------------------------------------------------------------------
// windows

class DropTarget;

class WinWindow : public Window {
public:
    WinWindow(const WindowOptions& o, WindowListener* l);
    ~WinWindow() override;

    void show() override;
    void hide() override;
    bool isVisible() const override { return visible_; }
    void setTitle(const std::string& title) override { SetWindowTextW(hwnd_, os::widen(title).c_str()); }
    void resize(int w, int h) override;
    void move(int x, int y) override;
    void setMinimumSize(int w, int h) override {
        minW_ = w;
        minH_ = h;
    }
    void setResizable(bool r) override;
    Rect geometry() const override { return Rect(x_, y_, w_, h_); }
    Rect frameGeometry() const override;
    void raise() override;
    void setMaximized(bool on) override;
    bool isMaximized() const override { return maximized_; }
    void setFullScreen(bool on) override;
    bool isFullScreen() const override { return fullscreen_; }
    void minimize() override {
        if (!opts_.popup && !opts_.offscreen) ShowWindow(hwnd_, SW_MINIMIZE);
    }
    bool isMinimized() const override { return minimized_; }
    void setDecorated(bool on) override;
    void setCornerRadii(int tl, int tr, int br, int bl) override;
    bool beginMoveResize(int edges, Point screenPos, int button) override;
    uintptr_t embedArea(const Rect& area) override;
    bool hasFocus() const override { return focus_; }
    void setCursor(Cursor c) override;
    void present(const Image& fb, const Rect& dirty) override;
    void grabPointer(bool on) override;
    void grabKeys(const std::vector<int>& keys) override { grabbedKeys_ = keys; }
    uintptr_t nativeHandle() const override { return (uintptr_t)hwnd_; }
    void* nativeDisplay() const override { return nullptr; }
    bool capture(Image& out) override;
    void setOpacity(float opacity) override;

    LRESULT proc(UINT msg, WPARAM w, LPARAM l);
    // a key pressed or let go in this window, or in a window inside it (keys it grabbed: grabKeys)
    void handleKey(UINT msg, WPARAM w, LPARAM l);
    bool grabs(int key) const {
        return std::find(grabbedKeys_.begin(), grabbedKeys_.end(), key) != grabbedKeys_.end();
    }
    void deliver(Event& e) { emit(e); }
    HWND hwnd() const { return hwnd_; }
    void dragEvent(Event::Type type, POINTL pt, const std::vector<std::string>& files);
    bool acceptsFiles() const { return opts_.acceptsFiles; }

private:
    HWND hwnd_ = nullptr;
    WindowListener* listener_ = nullptr;
    WindowOptions opts_;
    DWORD style_ = 0, exStyle_ = 0;
    bool ownFrame_ = false;  // decorated false: the program draws the frame, the client area is all of it
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    int reportedW_ = 0, reportedH_ = 0;  // the size the listener last heard of (Resize), as X11's
    int minW_ = 0, minH_ = 0;
    bool visible_ = false, shownOnce_ = false, moved_ = false;
    bool focus_ = false, maximized_ = false, minimized_ = false, fullscreen_ = false, wantMaximized_ = false;
    bool pointerInside_ = false, grabbed_ = false;
    int heldButtons_ = 0;
    Cursor cursor_ = Cursor::Arrow;
    int radii_[4] = {0, 0, 0, 0};
    float opacity_ = 1;
    HWND embed_ = nullptr;
    Rect embedRect_;
    std::vector<int> grabbedKeys_;
    std::map<UINT, int> keyOfScan_;  // the key each held scan code pressed (its release says the same)
    DropTarget* drop_ = nullptr;
    HICON icons_[2] = {nullptr, nullptr};
    RECT restore_{};                 // (full screen: where it was)
    DWORD restoreStyle_ = 0;
    // the picture present() last showed (WM_PAINT shows it again)
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr, oldBmp_ = nullptr;
    uint32_t* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;

    // false if the listener closed the window meanwhile (then nothing of it may be touched)
    bool emit(Event& e) {
        HWND self = hwnd_;
        if (!e.time) e.time = monoNow();
        listener_->onEvent(this, e);
        return S().windows.count(self) != 0;
    }
    RECT outerFor(int x, int y, int w, int h) const;
    void updateClientPos();
    void applyRegion();
    void mouse(Event::Type type, int button, WPARAM w, LPARAM l);
    void freeDib();
};

class DropTarget : public IDropTarget {
public:
    explicit DropTarget(WinWindow* w) : win_(w) {}
    virtual ~DropTarget() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *p = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        files_ = filesOf(data);
        entered_ = !files_.empty() && win_;
        *effect = entered_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (entered_) win_->dragEvent(Event::DragEnter, pt, files_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = entered_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (entered_ && win_) win_->dragEvent(Event::DragMove, pt, files_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (entered_ && win_) win_->dragEvent(Event::DragLeave, POINTL{0, 0}, {});
        entered_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        if (files_.empty()) files_ = filesOf(data);
        *effect = files_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        if (win_ && !files_.empty()) win_->dragEvent(Event::Drop, pt, files_);
        else if (win_ && entered_) win_->dragEvent(Event::DragLeave, pt, {});
        entered_ = false;
        files_.clear();
        return S_OK;
    }
    void detach() { win_ = nullptr; }

private:
    LONG refs_ = 1;
    WinWindow* win_;
    bool entered_ = false;
    std::vector<std::string> files_;
};

UINT dpiOf(HWND h) {
    UINT d = h ? GetDpiForWindow(h) : GetDpiForSystem();
    return d ? d : 96;
}

WinWindow::WinWindow(const WindowOptions& o, WindowListener* l) : listener_(l), opts_(o) {
    State& s = S();
    w_ = reportedW_ = std::max(1, o.w);
    h_ = reportedH_ = std::max(1, o.h);
    minW_ = o.minW;
    minH_ = o.minH;
    HWND owner = o.transientFor ? (HWND)o.transientFor->nativeHandle() : nullptr;
    if (o.popup || o.offscreen) {
        style_ = WS_POPUP | WS_CLIPCHILDREN;
        exStyle_ = WS_EX_TOOLWINDOW | (o.offscreen ? 0 : WS_EX_TOPMOST) | (o.takesFocus && !o.offscreen ? 0 : WS_EX_NOACTIVATE);
    } else {
        ownFrame_ = !o.decorated;
        style_ = (o.dialog ? (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME) : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN;
        if (!o.resizable) style_ &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (ownFrame_) style_ |= WS_THICKFRAME | WS_MINIMIZEBOX;  // (still snapped, sized, minimized by Windows)
    }
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
    }
    RECT r = outerFor(x_, y_, w_, h_);
    hwnd_ = CreateWindowExW(exStyle_, kClass, os::widen(o.title).c_str(), style_, r.left, r.top, r.right - r.left,
                            r.bottom - r.top, owner, nullptr, s.inst, this);
    if (!hwnd_) {
        util::debugf("platform: CreateWindowEx failed (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    s.windows[hwnd_] = this;
    if (ownFrame_) SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // (the size Windows gave, at this monitor's DPI)
    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (cr.right != w_ || cr.bottom != h_) {
        r = outerFor(x_, y_, w_, h_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!o.iconSvg.empty()) {
        UINT dpi = dpiOf(hwnd_);
        icons_[0] = iconFromSvg(o.iconSvg, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
        icons_[1] = iconFromSvg(o.iconSvg, GetSystemMetricsForDpi(SM_CXICON, dpi));
        if (icons_[0]) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icons_[0]);
        if (icons_[1]) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icons_[1]);
    }
    if (o.acceptsFiles && s.oleOk) {
        drop_ = new DropTarget(this);
        if (FAILED(RegisterDragDrop(hwnd_, drop_))) {
            drop_->Release();
            drop_ = nullptr;
            DragAcceptFiles(hwnd_, TRUE);  // (no OLE: drops only, as WM_DROPFILES)
        }
    } else if (o.acceptsFiles) {
        DragAcceptFiles(hwnd_, TRUE);
    }
}

WinWindow::~WinWindow() {
    State& s = S();
    if (s.keyGrab == this) s.keyGrab = nullptr;
    if (drop_) {
        RevokeDragDrop(hwnd_);
        drop_->detach();
        drop_->Release();
    }
    s.windows.erase(hwnd_);
    s.pendingExpose.erase(std::remove(s.pendingExpose.begin(), s.pendingExpose.end(), hwnd_), s.pendingExpose.end());
    if (hwnd_) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    freeDib();
    for (HICON i : icons_)
        if (i) DestroyIcon(i);
}

RECT WinWindow::outerFor(int x, int y, int w, int h) const {
    RECT r{x, y, x + w, y + h};
    if (!ownFrame_ && !(opts_.popup || opts_.offscreen))
        AdjustWindowRectExForDpi(&r, style_, FALSE, exStyle_, dpiOf(hwnd_));
    return r;
}

void WinWindow::updateClientPos() {
    POINT p{0, 0};
    ClientToScreen(hwnd_, &p);
    x_ = p.x;
    y_ = p.y;
}

void WinWindow::show() {
    if (!hwnd_) return;
    if (!shownOnce_ && !moved_ && !opts_.offscreen) {
        // centred for its size as it is now, on its screen
        Rect area = opts_.transientFor ? opts_.transientFor->geometry() : screenGeometry(cursorPos());
        x_ = area.x + (area.w - w_) / 2;
        y_ = area.y + (area.h - h_) / 2;
        Rect scr = screenGeometry(Point(x_ + w_ / 2, y_ + h_ / 2));
        x_ = std::max(scr.x, std::min(x_, scr.right() - w_));
        y_ = std::max(scr.y, std::min(y_, scr.bottom() - h_));
        RECT r = outerFor(x_, y_, w_, h_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    shownOnce_ = true;
    visible_ = true;
    bool activate = opts_.takesFocus && !opts_.popup && !opts_.offscreen;
    ShowWindow(hwnd_, wantMaximized_ ? SW_SHOWMAXIMIZED : activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    wantMaximized_ = false;
    if (opts_.popup && opts_.takesFocus) {
        // (menus take the keys, not the focus: the window they came from keeps it, as on X11)
        S().keyGrab = this;
        PostMessageW(hwnd_, WM_APP_FOCUSIN, 0, 0);
    }
    if (activate) SetForegroundWindow(hwnd_);
}

void WinWindow::hide() {
    if (!visible_ || !hwnd_) return;
    visible_ = false;
    if (grabbed_) grabPointer(false);
    if (S().keyGrab == this) {
        S().keyGrab = nullptr;
        focus_ = false;
    }
    ShowWindow(hwnd_, SW_HIDE);
}

void WinWindow::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if ((w == w_ && h == h_) || !hwnd_) return;
    w_ = w;
    h_ = h;
    RECT r = outerFor(x_, y_, w, h);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WinWindow::move(int x, int y) {
    if (opts_.offscreen || !hwnd_) return;
    moved_ = true;
    x_ = x;
    y_ = y;
    RECT r = outerFor(x, y, w_, h_);
    SetWindowPos(hwnd_, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WinWindow::setResizable(bool r) {
    if (opts_.popup || opts_.offscreen || !hwnd_) return;
    opts_.resizable = r;
    DWORD st = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
    DWORD box = opts_.dialog ? 0 : WS_MAXIMIZEBOX;  // (dialogs: no maximize button, as Windows' own)
    if (r || ownFrame_) st |= WS_THICKFRAME | box;
    else st &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    style_ = st;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, (LONG_PTR)st);
    RECT o = outerFor(x_, y_, w_, h_);
    SetWindowPos(hwnd_, nullptr, o.left, o.top, o.right - o.left, o.bottom - o.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

Rect WinWindow::frameGeometry() const {
    RECT r{};
    // (the frame as seen: without Windows 10's invisible resize borders)
    if (FAILED(DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(hwnd_, &r);
    return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

void WinWindow::raise() {
    if (!hwnd_) return;
    if (opts_.popup || opts_.offscreen) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    if (minimized_) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void WinWindow::setMaximized(bool on) {
    if (opts_.popup || opts_.offscreen || !hwnd_) return;
    if (!visible_) {
        wantMaximized_ = on;
        return;
    }
    ShowWindow(hwnd_, on ? SW_MAXIMIZE : SW_RESTORE);
}

void WinWindow::setFullScreen(bool on) {
    if (opts_.popup || opts_.offscreen || !hwnd_ || on == fullscreen_) return;
    if (on) {
        restoreStyle_ = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
        GetWindowRect(hwnd_, &restore_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        fullscreen_ = true;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (LONG_PTR)((restoreStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (LONG_PTR)restoreStyle_);
        SetWindowPos(hwnd_, nullptr, restore_.left, restore_.top, restore_.right - restore_.left,
                     restore_.bottom - restore_.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    Event e;
    e.type = Event::StateChanged;
    emit(e);
}

void WinWindow::setDecorated(bool on) {
    if (opts_.popup || opts_.offscreen || !hwnd_ || ownFrame_ == !on) return;
    ownFrame_ = !on;
    opts_.decorated = on;
    // (the style as it is now: WS_VISIBLE and WS_MAXIMIZE kept)
    style_ = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (ownFrame_) style_ |= WS_THICKFRAME | WS_MINIMIZEBOX;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, (LONG_PTR)style_);
    if (!ownFrame_) SetWindowRgn(hwnd_, nullptr, TRUE);
    RECT r = outerFor(x_, y_, w_, h_);
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    if (ownFrame_) applyRegion();
}

void WinWindow::setCornerRadii(int tl, int tr, int br, int bl) {
    radii_[0] = std::max(0, tl), radii_[1] = std::max(0, tr), radii_[2] = std::max(0, br), radii_[3] = std::max(0, bl);
    applyRegion();
}

// the window's shape: the rectangle less what lies outside each corner's quarter circle (own frames only, not
// popups, not maximized or full screen: as X11's bounding shape)
void WinWindow::applyRegion() {
    if (!hwnd_ || opts_.popup || opts_.offscreen) return;
    bool any = radii_[0] || radii_[1] || radii_[2] || radii_[3];
    if (!any || !ownFrame_ || maximized_ || fullscreen_) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    std::vector<RECT> rects = roundedRects(w_, h_, radii_);
    std::vector<char> buf(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
    auto* rd = (RGNDATA*)buf.data();
    rd->rdh.dwSize = sizeof(RGNDATAHEADER);
    rd->rdh.iType = RDH_RECTANGLES;
    rd->rdh.nCount = (DWORD)rects.size();
    rd->rdh.nRgnSize = (DWORD)(rects.size() * sizeof(RECT));
    rd->rdh.rcBound = RECT{0, 0, w_, h_};
    memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
    HRGN rgn = ExtCreateRegion(nullptr, (DWORD)buf.size(), rd);
    if (rgn) SetWindowRgn(hwnd_, rgn, TRUE);  // (the window owns it now)
}

bool WinWindow::beginMoveResize(int edges, Point screenPos, int button) {
    if (!hwnd_ || opts_.popup || opts_.offscreen || !visible_ || button != LeftButton) return false;
    WPARAM ht = HTCAPTION;
    bool l = edges & EdgeLeft, r = edges & EdgeRight, t = edges & EdgeTop, b = edges & EdgeBottom;
    if (t && l) ht = HTTOPLEFT;
    else if (t && r) ht = HTTOPRIGHT;
    else if (b && r) ht = HTBOTTOMRIGHT;
    else if (b && l) ht = HTBOTTOMLEFT;
    else if (t) ht = HTTOP;
    else if (r) ht = HTRIGHT;
    else if (b) ht = HTBOTTOM;
    else if (l) ht = HTLEFT;
    if (ht != HTCAPTION && !opts_.resizable) return false;
    if (grabbed_) grabPointer(false);
    ReleaseCapture();
    heldButtons_ = 0;
    // (posted: Windows' own move or size loop runs from the main loop, not inside this press's handler)
    PostMessageW(hwnd_, WM_NCLBUTTONDOWN, ht, MAKELPARAM((short)screenPos.x, (short)screenPos.y));
    return true;
}

uintptr_t WinWindow::embedArea(const Rect& area) {
    if (!hwnd_) return 0;
    if (area.empty()) {
        if (embed_) ShowWindow(embed_, SW_HIDE);
        return (uintptr_t)embed_;
    }
    if (!embed_) {
        embed_ = CreateWindowExW(0, kEmbedClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, area.x,
                                 area.y, area.w, area.h, hwnd_, nullptr, S().inst, nullptr);
    } else {
        if (!(area == embedRect_)) MoveWindow(embed_, area.x, area.y, area.w, area.h, TRUE);
        ShowWindow(embed_, SW_SHOWNA);
    }
    embedRect_ = area;
    return (uintptr_t)embed_;
}

void WinWindow::setCursor(Cursor c) {
    if (c == cursor_) return;
    cursor_ = c;
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) == hwnd_) ::SetCursor(cursorFor(c));
}

void WinWindow::freeDib() {
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
    }
    if (dib_) DeleteObject(dib_);
    memDC_ = nullptr;
    dib_ = nullptr;
    bits_ = nullptr;
    dibW_ = dibH_ = 0;
}

void WinWindow::present(const Image& fb, const Rect& dirty) {
    if (!hwnd_ || fb.empty()) return;
    Rect r = dirty.intersected(fb.rect()).intersected(Rect(0, 0, w_, h_));
    if (r.empty()) return;
    if (fb.width() != dibW_ || fb.height() != dibH_) {
        freeDib();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = fb.width();
        bi.bmiHeader.biHeight = -fb.height();  // (top-down, as Image is)
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(hwnd_);
        void* bits = nullptr;
        dib_ = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        memDC_ = CreateCompatibleDC(dc);
        ReleaseDC(hwnd_, dc);
        if (!dib_ || !memDC_) {
            freeDib();
            return;
        }
        oldBmp_ = (HBITMAP)SelectObject(memDC_, dib_);
        bits_ = (uint32_t*)bits;
        dibW_ = fb.width();
        dibH_ = fb.height();
        r = fb.rect().intersected(Rect(0, 0, w_, h_));  // (all of it: the new bitmap holds nothing yet)
    }
    GdiFlush();
    for (int y = r.y; y < r.bottom(); y++) memcpy(bits_ + (size_t)y * dibW_ + r.x, fb.row(y) + r.x, (size_t)r.w * 4);
    HDC dc = GetDC(hwnd_);
    BitBlt(dc, r.x, r.y, r.w, r.h, memDC_, r.x, r.y, SRCCOPY);
    ReleaseDC(hwnd_, dc);
}

void WinWindow::grabPointer(bool on) {
    if (!hwnd_) return;
    if (on) {
        SetCapture(hwnd_);
        grabbed_ = GetCapture() == hwnd_;
    } else if (grabbed_) {
        grabbed_ = false;
        if (GetCapture() == hwnd_ && !heldButtons_) ReleaseCapture();
    }
}

bool WinWindow::capture(Image& out) {
    if (!hwnd_ || w_ <= 0 || h_ <= 0) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(hwnd_);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(dc);
    ReleaseDC(hwnd_, dc);
    if (!bmp || !mem) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    // (PW_CLIENTONLY | PW_RENDERFULLCONTENT: the client area with what is inside it, plugins' views too)
    bool ok = PrintWindow(hwnd_, mem, 1 | 2);
    GdiFlush();
    if (ok) {
        out = Image(w_, h_);
        const uint32_t* src = (const uint32_t*)bits;
        for (int y = 0; y < h_; y++)
            for (int x = 0; x < w_; x++) out.row(y)[x] = src[(size_t)y * w_ + x] | 0xFF000000u;
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    return ok;
}

void WinWindow::setOpacity(float opacity) {
    opacity = std::max(0.f, std::min(1.f, opacity));
    if (!hwnd_ || opacity == opacity_) return;
    opacity_ = opacity;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (opacity < 1) {
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd_, 0, (BYTE)std::lround(opacity * 255), LWA_ALPHA);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
}

void WinWindow::dragEvent(Event::Type type, POINTL pt, const std::vector<std::string>& files) {
    POINT p{pt.x, pt.y};
    ScreenToClient(hwnd_, &p);
    Event e;
    e.type = type;
    e.x = p.x;
    e.y = p.y;
    e.files = files;
    emit(e);
}

void WinWindow::mouse(Event::Type type, int button, WPARAM w, LPARAM l) {
    Event e;
    e.type = type;
    e.x = GET_X_LPARAM(l);
    e.y = GET_Y_LPARAM(l);
    e.mods = currentMods();
    e.buttons = buttonsOf(w);
    e.button = button;
    if (type == Event::MouseDown) {
        heldButtons_ |= button;
        e.buttons |= button;
        if (GetCapture() != hwnd_) SetCapture(hwnd_);  // (X's implicit grab: the button's events come here)
    } else if (type == Event::MouseUp) {
        heldButtons_ &= ~button;
        e.buttons &= ~button;
    } else if (!pointerInside_) {
        pointerInside_ = true;
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        Event enter;
        enter.type = Event::Enter;
        enter.x = e.x;
        enter.y = e.y;
        enter.mods = e.mods;
        enter.buttons = e.buttons;
        if (!emit(enter)) return;
    }
    HWND self = hwnd_;
    if (!emit(e)) return;
    (void)self;
    if (type == Event::MouseUp && !heldButtons_ && !grabbed_ && GetCapture() == hwnd_) ReleaseCapture();
}

void WinWindow::handleKey(UINT msg, WPARAM w, LPARAM l) {
    State& s = S();
    bool press = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    UINT scan = (UINT)((l >> 16) & 0xFF);
    bool extended = (l >> 24) & 1;
    UINT vk = (UINT)w;
    // (Windows tells the left and right Shift apart only by their scan codes)
    if (vk == VK_PROCESSKEY) return;  // (the input method takes it: its text comes as WM_CHAR)
    Event e;
    if (press) {
        std::string text;
        MSG m;
        wchar_t high = 0;
        // the characters TranslateMessage made of this key, already queued (WM_DEADCHAR: a dead key types nothing yet)
        while (PeekMessageW(&m, hwnd_, WM_CHAR, WM_DEADCHAR, PM_REMOVE) ||
               PeekMessageW(&m, hwnd_, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {
            if (m.message != WM_CHAR) continue;
            wchar_t c = (wchar_t)m.wParam;
            if (c >= 0xD800 && c < 0xDC00) {
                high = c;
                continue;
            }
            uint32_t cp = c;
            if (c >= 0xDC00 && c < 0xE000 && high) cp = 0x10000 + (((uint32_t)high - 0xD800) << 10) + (c - 0xDC00);
            high = 0;
            if (cp >= 0x20 && cp != 0x7F) appendUtf8(text, cp);  // (Ctrl+A types "\x01": no text)
        }
        int key = keyFromVk(vk, scan | (extended ? 0x100 : 0), extended);
        if (!key && !text.empty()) key = (int)upperCodepoint(firstCodepoint(text));
        e.type = Event::KeyDown;
        e.key = key;
        e.autoRepeat = (l >> 30) & 1;
        e.mods = currentMods() | modifierOfKey(key);
        // AltGr is Ctrl+Alt to Windows: what it types is plain text, not a shortcut
        if (!text.empty() && (GetKeyState(VK_RMENU) & 0x8000)) e.mods &= ~(ModCtrl | ModAlt);
        e.text = text;
        keyOfScan_[scan | (extended ? 0x100 : 0)] = key;
    } else {
        UINT code = scan | (extended ? 0x100 : 0);
        auto it = keyOfScan_.find(code);
        int key = it != keyOfScan_.end() ? it->second : keyFromVk(vk, code, extended);
        if (it != keyOfScan_.end()) keyOfScan_.erase(it);
        e.type = Event::KeyUp;
        e.key = key;
        e.mods = currentMods() & ~modifierOfKey(key);
    }
    WinWindow* to = (s.keyGrab && s.keyGrab != this && s.keyGrab->visible_) ? s.keyGrab : this;
    to->deliver(e);
}

LRESULT WinWindow::proc(UINT msg, WPARAM w, LPARAM l) {
    State& s = S();
    switch (msg) {
        case WM_NCCALCSIZE:
            if (ownFrame_ && w && !fullscreen_) {
                // the client area is all of the window; maximized, Windows puts its frame outside the screen
                if (IsZoomed(hwnd_)) {
                    auto* p = (NCCALCSIZE_PARAMS*)l;
                    UINT dpi = dpiOf(hwnd_);
                    int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            if (ownFrame_) return DefWindowProcW(hwnd_, msg, w, -1);  // (-1: Windows draws no frame over ours)
            break;
        case WM_NCPAINT:
            if (ownFrame_ && !fullscreen_) return 0;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT& r = ps.rcPaint;
            if (memDC_) BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, memDC_, r.left, r.top, SRCCOPY);
            else FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hwnd_, &ps);
            if (std::find(s.pendingExpose.begin(), s.pendingExpose.end(), hwnd_) == s.pendingExpose.end())
                s.pendingExpose.push_back(hwnd_);
            return 0;
        }
        case WM_SIZE: {
            bool wasMax = maximized_, wasMin = minimized_;
            maximized_ = w == SIZE_MAXIMIZED;
            minimized_ = w == SIZE_MINIMIZED;
            int nw = LOWORD(l), nh = HIWORD(l);
            // (resize() set w_ and h_ already: what counts is what the listener knows)
            bool sized = !minimized_ && nw > 0 && nh > 0 && (nw != reportedW_ || nh != reportedH_);
            if (!minimized_ && nw > 0 && nh > 0) {
                w_ = reportedW_ = nw;
                h_ = reportedH_ = nh;
            }
            if (sized || wasMax != maximized_) applyRegion();
            if (sized) {
                Event e;
                e.type = Event::Resize;
                e.w = w_;
                e.h = h_;
                if (!emit(e)) return 0;
            }
            if (wasMax != maximized_ || wasMin != minimized_) {
                Event e;
                e.type = Event::StateChanged;
                if (!emit(e)) return 0;
            }
            return 0;
        }
        case WM_MOVE: {
            int ox = x_, oy = y_;
            updateClientPos();
            if ((ox != x_ || oy != y_) && !opts_.offscreen && !minimized_) {
                Event e;
                e.type = Event::Move;
                e.x = x_;
                e.y = y_;
                emit(e);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)l;
            if (minW_ > 0 || minH_ > 0) {
                RECT r = outerFor(0, 0, std::max(1, minW_), std::max(1, minH_));
                mm->ptMinTrackSize.x = r.right - r.left;
                mm->ptMinTrackSize.y = r.bottom - r.top;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* r = (RECT*)l;  // (where Windows suggests it goes, at the new monitor's DPI)
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE: {
            Event e;
            e.type = Event::Close;
            emit(e);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            bool in = msg == WM_SETFOCUS;
            if (!in) keyOfScan_.clear();
            if (in == focus_) return 0;
            focus_ = in;
            Event e;
            e.type = in ? Event::FocusIn : Event::FocusOut;
            emit(e);
            return 0;
        }
        case WM_APP_FOCUSIN:
            if (s.keyGrab == this && visible_ && !focus_) {
                focus_ = true;
                Event e;
                e.type = Event::FocusIn;
                emit(e);
            }
            return 0;
        case WM_ACTIVATEAPP:
            // another program took the keys: the popup holding them (a menu) loses them too
            if (!w && s.keyGrab && s.keyGrab->focus_) {
                WinWindow* g = s.keyGrab;
                g->focus_ = false;
                Event e;
                e.type = Event::FocusOut;
                g->emit(e);
            }
            break;
        case WM_MOUSEACTIVATE:
            if (opts_.popup || opts_.offscreen || !opts_.takesFocus) return MA_NOACTIVATE;
            break;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                ::SetCursor(cursorFor(cursor_));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            mouse(Event::MouseMove, NoButton, w, l);
            return 0;
        case WM_LBUTTONDOWN: mouse(Event::MouseDown, LeftButton, w, l); return 0;
        case WM_LBUTTONUP: mouse(Event::MouseUp, LeftButton, w, l); return 0;
        case WM_RBUTTONDOWN: mouse(Event::MouseDown, RightButton, w, l); return 0;
        case WM_RBUTTONUP: mouse(Event::MouseUp, RightButton, w, l); return 0;
        case WM_MBUTTONDOWN: mouse(Event::MouseDown, MiddleButton, w, l); return 0;
        case WM_MBUTTONUP: mouse(Event::MouseUp, MiddleButton, w, l); return 0;
        case WM_MOUSELEAVE:
            if (pointerInside_) {
                pointerInside_ = false;
                Event e;
                e.type = Event::Leave;
                e.mods = currentMods();
                emit(e);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)l != hwnd_) {
                grabbed_ = false;
                heldButtons_ = 0;
            }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};  // (screen coordinates)
            ScreenToClient(hwnd_, &p);
            float notches = (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
            Event e;
            e.type = Event::Wheel;
            e.x = p.x;
            e.y = p.y;
            if (msg == WM_MOUSEWHEEL) e.dy = notches;
            else e.dx = -notches;  // (Windows: right is positive; here left is)
            e.mods = currentMods();
            e.buttons = buttonsOf(GET_KEYSTATE_WPARAM(w));
            emit(e);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            handleKey(msg, w, l);
            if (msg == WM_SYSKEYDOWN && w == VK_F4 && !opts_.popup) break;  // (Alt+F4: Windows asks to close it)
            return 0;
        case WM_CHAR: {
            // text without a key of its own (an input method's result, or a character another program sent)
            wchar_t c = (wchar_t)w;
            static wchar_t high = 0;
            if (c >= 0xD800 && c < 0xDC00) {
                high = c;
                return 0;
            }
            uint32_t cp = c;
            if (c >= 0xDC00 && c < 0xE000 && high) cp = 0x10000 + (((uint32_t)high - 0xD800) << 10) + (c - 0xDC00);
            high = 0;
            if (cp < 0x20 || cp == 0x7F) return 0;
            Event e;
            e.type = Event::KeyDown;
            appendUtf8(e.text, cp);
            e.key = (int)upperCodepoint(cp);
            e.mods = currentMods() & ~(ModCtrl | ModAlt);
            WinWindow* to = (s.keyGrab && s.keyGrab != this && s.keyGrab->visible_) ? s.keyGrab : this;
            to->deliver(e);
            return 0;
        }
        case WM_SYSCHAR:
            return 0;  // (no beep for Alt+letter)
        case WM_SYSCOMMAND:
            if ((w & 0xFFF0) == SC_KEYMENU && l != ' ') return 0;  // (Alt alone doesn't open the system menu)
            break;
        case WM_IME_STARTCOMPOSITION: {
            // the input method's window near the pointer (the program has no caret Windows knows of)
            if (HIMC imc = ImmGetContext(hwnd_)) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd_, &p);
                COMPOSITIONFORM cf{CFS_POINT, p, {}};
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(hwnd_, imc);
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)w;
            std::vector<std::string> files;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring f(len + 1, L'\0');
                DragQueryFileW(drop, i, &f[0], len + 1);
                f.resize(len);
                files.push_back(os::portablePath(os::narrow(f)));
            }
            POINT p{};
            DragQueryPoint(drop, &p);
            DragFinish(drop);
            if (files.empty()) return 0;
            Event e;
            e.type = Event::Drop;
            e.x = p.x;
            e.y = p.y;
            e.files = files;
            emit(e);
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd_, MODAL_TIMER, 8, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd_, MODAL_TIMER);
            return 0;
        case WM_DISPLAYCHANGE:
            s.refreshRate = -1;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

void runPosted();
void runTimers();
void flushExposes();

LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    if (msg == WM_TIMER && w == MODAL_TIMER) {
        // Windows' own loop runs while a window is moved or sized: the program's timers and posted work go on
        runPosted();
        runTimers();
        flushExposes();
        return 0;
    }
    auto* win = (WinWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!win || !S().windows.count(h)) return DefWindowProcW(h, msg, w, l);
    return win->proc(msg, w, l);
}

void flushExposes() {
    State& s = S();
    while (!s.pendingExpose.empty()) {
        std::vector<HWND> ids;
        ids.swap(s.pendingExpose);
        for (HWND id : ids) {
            auto it = s.windows.find(id);
            if (it == s.windows.end()) continue;
            Event e;
            e.type = Event::Expose;
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
    for (auto& fn : work)
        if (fn) fn();
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

// a key in a window inside one of ours (an embedded plugin's) that ours grabbed (grabKeys: the computer keyboard's
// notes): handled as ours, not given to the plugin
bool grabbedKey(const MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_KEYUP) return false;
    State& s = S();
    if (s.windows.count(m.hwnd)) return false;
    HWND root = GetAncestor(m.hwnd, GA_ROOT);
    auto it = s.windows.find(root);
    if (it == s.windows.end()) return false;
    if (currentMods() & (ModCtrl | ModAlt | ModMeta)) return false;
    UINT scan = (UINT)((m.lParam >> 16) & 0xFF);
    int key = keyFromVk((UINT)m.wParam, scan, (m.lParam >> 24) & 1);
    if (!key || !it->second->grabs(key)) return false;
    it->second->handleKey(m.message, m.wParam, m.lParam);
    return true;
}

void dispatchMessages() {
    MSG m;
    int n = 0;
    while (n++ < 2000 && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            S().quit = true;
            continue;
        }
        if (grabbedKey(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void wake() {
    if (S().wake) SetEvent(S().wake);
}

LRESULT CALLBACK helperProc(HWND h, UINT msg, WPARAM w, LPARAM l) { return DefWindowProcW(h, msg, w, l); }

LRESULT CALLBACK embedProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));  // (where the plugin's view doesn't reach)
        return 1;
    }
    return DefWindowProcW(h, msg, w, l);
}

std::string knownFolder(REFKNOWNFOLDERID id, const char* env) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) out = os::portablePath(os::narrow(p));
    if (p) CoTaskMemFree(p);
    if (out.empty())
        if (const char* e = getenv(env)) out = os::portablePath(e);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// the public functions

std::unique_ptr<Window> Window::create(const WindowOptions& opts, WindowListener* listener) {
    if (!S().display) return nullptr;
    auto* w = new WinWindow(opts, listener);
    if (!w->hwnd()) {
        delete w;
        return nullptr;
    }
    return std::unique_ptr<Window>(w);
}

void initWithoutDisplay() {
    State& s = S();
    s.mainThread = std::this_thread::get_id();
    if (!s.wake) s.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timeBeginPeriod(1);  // (timers to the millisecond: Windows' default tick is 15.6 ms)
}

void init(int argc, char** argv) {
    State& s = S();
    if (s.display) return;
    initWithoutDisplay();
    (void)argc;
    (void)argv;
    // Started from Explorer, Windows made a console just for us: gone, and the diagnostics go to a file instead
    DWORD pids[2];
    if (GetConsoleWindow() && GetConsoleProcessList(pids, 2) == 1) {
        FreeConsole();
        std::string log = configDir() + "/tinydaw-log.txt";
        if (freopen(log.c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
        freopen("NUL", "w", stdout);
    }
    s.inst = GetModuleHandleW(nullptr);
    s.oleOk = SUCCEEDED(OleInitialize(nullptr));  // (drag and drop from Explorer)
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = s.inst;
    wc.hCursor = nullptr;  // (WM_SETCURSOR sets the window's own)
    wc.lpszClassName = kClass;
    // the program's own icon (tinydaw_win.rc: 1), Windows' plain one where it isn't built in
    wc.hIcon = LoadIconW(s.inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(s.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    util::debugf("platform: window class icon %s\n", wc.hIcon ? "built in" : "missing: Windows' plain one");
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "Tiny DAW: can't show windows (RegisterClassEx failed: %lu)\n", (unsigned long)GetLastError());
        return;
    }
    WNDCLASSEXW ec{sizeof ec};
    ec.lpfnWndProc = embedProc;
    ec.hInstance = s.inst;
    ec.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ec.lpszClassName = kEmbedClass;
    RegisterClassExW(&ec);
    WNDCLASSEXW hc{sizeof hc};
    hc.lpfnWndProc = helperProc;
    hc.hInstance = s.inst;
    hc.lpszClassName = kHelperClass;
    RegisterClassExW(&hc);
    s.helper = CreateWindowExW(0, kHelperClass, L"Tiny DAW", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, s.inst, nullptr);
    s.display = true;
    util::debugf("platform: Windows, %u dpi, %d monitor(s)\n", GetDpiForSystem(), GetSystemMetrics(SM_CMONITORS));
}

void shutdown() {
    State& s = S();
    if (!s.display) return;
    if (s.helper) DestroyWindow(s.helper);
    s.helper = nullptr;
    if (s.oleOk) OleUninitialize();  // (OleFlushClipboard isn't needed: the text was given to Windows as it was)
    s.oleOk = false;
    s.display = false;
    timeEndPeriod(1);
}

bool hasDisplay() { return S().display; }

bool compositing() { return false; }  // (windows are opaque on Windows: no see-through framebuffers)

void setTransparency(bool on) { S().transparency = on; }

bool transparency() { return S().transparency; }

void setCompositingListener(std::function<void()> fn) { S().compositingListener = std::move(fn); }

void processEvents(int timeoutMs) {
    State& s = S();
    if (!s.wake) initWithoutDisplay();
    bool ready = !s.pendingExpose.empty();
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
    std::vector<HANDLE> handles{s.wake};
    std::vector<int> watchIds;
    bool pipes = false;
    for (auto& kv : s.watches) {
        if (kv.second.invalid) continue;
        if (kv.second.pipe) {
            pipes = true;
            continue;
        }
        if (handles.size() >= MAXIMUM_WAIT_OBJECTS - 1) break;
        handles.push_back(kv.second.handle);
        watchIds.push_back(kv.first);
    }
    if (pipes) wait = wait < 0 ? 0.01 : std::min(wait, 0.01);  // (pipes are asked every 10 ms)
    if (ready) wait = 0;
    DWORD ms = wait < 0 ? INFINITE : (DWORD)std::ceil(wait * 1000.0);
    DWORD r = MsgWaitForMultipleObjectsEx((DWORD)handles.size(), handles.data(), ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    dispatchMessages();
    // the watched handles that are ready (a console's input) and pipes with bytes waiting (or closed)
    std::vector<int> fire;
    if (r >= WAIT_OBJECT_0 + 1 && r < WAIT_OBJECT_0 + handles.size()) fire.push_back(watchIds[r - WAIT_OBJECT_0 - 1]);
    for (size_t i = 0; i < watchIds.size(); i++)
        if (std::find(fire.begin(), fire.end(), watchIds[i]) == fire.end() &&
            WaitForSingleObject(handles[i + 1], 0) == WAIT_OBJECT_0)
            fire.push_back(watchIds[i]);
    for (auto& kv : s.watches) {
        if (!kv.second.pipe || kv.second.invalid) continue;
        DWORD avail = 0;
        if (!PeekNamedPipe(kv.second.handle, nullptr, 0, nullptr, &avail, nullptr) || avail > 0) fire.push_back(kv.first);
    }
    for (int id : fire) {
        auto it = s.watches.find(id);
        if (it == s.watches.end()) continue;  // removed by an earlier callback
        std::shared_ptr<std::function<void()>> fn = it->second.fn;
        if (fn && *fn) (*fn)();
    }
    runPosted();
    runTimers();
    dispatchMessages();
    flushExposes();
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
    State& s = S();
    if (s.refreshRate >= 0) return s.refreshRate;
    double best = 0;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (GetMonitorInfoW(m, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                dm.dmDisplayFrequency > 1)
                *(double*)p = std::max(*(double*)p, (double)dm.dmDisplayFrequency);
            return TRUE;
        },
        (LPARAM)&best);
    s.refreshRate = best;
    return best;
}

int watchFd(int fd, std::function<void()> fn) {
    State& s = S();
    int id = s.nextWatch++;
    Watch w;
    w.fd = fd;
    w.handle = (HANDLE)_get_osfhandle(fd);
    w.pipe = w.handle != INVALID_HANDLE_VALUE && GetFileType(w.handle) == FILE_TYPE_PIPE;
    w.invalid = w.handle == INVALID_HANDLE_VALUE;
    if (w.invalid) fprintf(stderr, "Tiny DAW: can't watch file descriptor %d (not open)\n", fd);
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
double doubleClickTime() { return GetDoubleClickTime() / 1000.0; }
int systemScale() { return (int)std::lround(GetDpiForSystem() * 100.0 / 96.0); }
int doubleClickDistance() { return std::max(5, GetSystemMetrics(SM_CXDOUBLECLK) / 2); }

Rect screenGeometry(Point at) {  // ("near" is a macro of windows.h's)
    if (!S().display) return Rect(0, 0, 1920, 1080);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{at.x, at.y}, MONITOR_DEFAULTTONEAREST), &mi))
        return Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    const RECT& r = mi.rcWork;  // (less the taskbar)
    return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

Rect primaryScreenGeometry() {
    if (!S().display) return Rect(0, 0, 1920, 1080);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi))
        return Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    const RECT& r = mi.rcWork;
    return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

Point cursorPos() {
    POINT p{0, 0};
    GetCursorPos(&p);
    return Point(p.x, p.y);
}

int heldButtons() {
    bool swap = GetSystemMetrics(SM_SWAPBUTTON);  // (GetAsyncKeyState: the physical buttons)
    int b = 0;
    if (GetAsyncKeyState(swap ? VK_RBUTTON : VK_LBUTTON) & 0x8000) b |= LeftButton;
    if (GetAsyncKeyState(swap ? VK_LBUTTON : VK_RBUTTON) & 0x8000) b |= RightButton;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) b |= MiddleButton;
    return b;
}

int heldModifiers() {
    int m = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) m |= ModMeta;
    return m;
}

namespace {
bool openClipboard() {
    for (int i = 0; i < 10; i++) {  // (another program may hold it a moment)
        if (OpenClipboard(S().helper)) return true;
        Sleep(5);
    }
    return false;
}
}  // namespace

void setClipboardText(const std::string& text) {
    State& s = S();
    s.clipText = text;
    if (!s.display || !openClipboard()) return;
    std::string crlf;  // (Windows' programs want "\r\n" between lines)
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf += '\r';
        crlf += text[i];
    }
    std::wstring w = os::widen(crlf);
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

std::string clipboardText() {
    State& s = S();
    if (!s.display) return s.clipText;
    if (!openClipboard()) return std::string();
    std::string out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            out = os::narrow(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return util::replaceAll(out, "\r\n", "\n");
}

std::string homeDir() {
    std::string h = knownFolder(FOLDERID_Profile, "USERPROFILE");
    return h.empty() ? "C:/" : h;
}

std::string configDir() {
    std::string base = knownFolder(FOLDERID_RoamingAppData, "APPDATA");
    std::string dir = (base.empty() ? homeDir() : base) + "/TinyDAW";
    if (!util::isDir(dir)) util::makeDirs(dir);
    return dir;
}

std::string executableDir() {
    std::string exe = os::executablePath();
    return exe.empty() ? "." : util::dirName(exe);
}

std::string dataDir() {
    std::string exe = executableDir();
    if (util::isDir(exe + "/data")) return exe + "/data";
    std::string share = util::dirName(exe) + "/share/tinydaw";
    if (util::isDir(share)) return share;
    return exe + "/data";
}

bool openUrl(const std::string& url) {
    if (url.empty()) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", os::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

bool openFolder(const std::string& path) {
    if (path.empty()) return false;
    return (INT_PTR)ShellExecuteW(nullptr, L"open", os::widen(os::nativePath(path)).c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL) > 32;
}

void beep() { MessageBeep(MB_OK); }

}  // namespace plat
