// Windows' own windows (platform/platform_win.cpp): the mouse capture a press takes is let go before the release's
// handler runs, so that a dialog the release opens and waits on ("save the changes?" from the close button) gets
// the next click, not the window under it. Windows only (elsewhere: nothing to do).
#ifdef _WIN32
#include <windows.h>

#include <cstdio>

#include "check.h"
#include "platform/platform.h"

namespace {
struct Listener : plat::WindowListener {
    HWND hwnd = nullptr;
    int ups = 0;
    bool capturedAtUp = true;
    void onEvent(plat::Window*, const plat::Event& e) override {
        if (e.type != plat::Event::MouseUp) return;
        ups++;
        capturedAtUp = GetCapture() == hwnd;
        printf("   the release: the capture %s\n", capturedAtUp ? "still held" : "already let go");
    }
};
}  // namespace

int main() {
    plat::init(0, nullptr);
    for (bool decorated : {true, false}) {
        printf("== a press and its release, %s\n", decorated ? "Windows' frame" : "the window's own frame");
        Listener l;
        plat::WindowOptions o;
        o.title = "test_window_win";
        o.w = 200, o.h = 120;
        o.decorated = decorated;
        auto w = plat::Window::create(o, &l);
        CHECK(w != nullptr);
        if (!w) continue;
        l.hwnd = (HWND)w->nativeHandle();
        w->show();
        SendMessageW(l.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
        CHECK(GetCapture() == l.hwnd);  // (the press: its release comes here, wherever the pointer is)
        SendMessageW(l.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(20, 20));
        CHECK(l.ups == 1);
        CHECK(!l.capturedAtUp);
        CHECK(GetCapture() != l.hwnd);
    }
    TEST_MAIN_END();
}
#else
#include <cstdio>
int main() {
    printf("test_window_win: Windows only\n");
    return 0;
}
#endif
