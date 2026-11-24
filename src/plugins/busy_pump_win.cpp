// The main thread kept responding during a long wait (see busy_pump_win.h)
#include "plugins/busy_pump_win.h"

#include "core/util.h"

namespace {
constexpr UINT SYSTIMER = 0x0118;  // (Windows' own timers: the caret's blinking…)
}  // namespace

void BusyPump::pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {  // (sent messages are answered meanwhile, by Windows)
        if (m.message == WM_PAINT) {
            DispatchMessageW(&m);
        } else if (m.message == WM_QUIT) {
            quit_ = true;
            quitCode_ = (int)m.wParam;
        } else if (m.message != WM_TIMER && m.message != SYSTIMER && m.message != WM_MOUSEMOVE &&
                   m.message != WM_NCMOUSEMOVE) {  // (timers come again; a move is soon stale: the next one counts)
            held_.push_back(m);
        }
    }
}

BusyPump::~BusyPump() {
    size_t lost = 0;
    for (const MSG& m : held_) {
        bool posted = m.hwnd ? (IsWindow(m.hwnd) && PostMessageW(m.hwnd, m.message, m.wParam, m.lParam))
                             : PostThreadMessageW(GetCurrentThreadId(), m.message, m.wParam, m.lParam);
        if (!posted) ++lost;
    }
    if (quit_) PostQuitMessage(quitCode_);
    if (!held_.empty())
        util::debugf("busy pump: %zu messages held while waiting, posted again (%zu lost: their window went)\n",
                     held_.size(), lost);
}
