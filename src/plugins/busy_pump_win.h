// Windows: the program's main thread kept responding while it waits a long time for a plugin's host (Surge
// XT makes its window in about 4 s). Windows calls a program "Not Responding" once its messages wait unread
// for 5 s (peeking at the messages other threads send it isn't enough), and its windows go unpainted.
//
// A BusyPump is made on the main thread for one long wait; pump(), called often while waiting, repaints the
// program's windows (their WM_PAINT shows the picture they last had: platform_win.cpp) and takes every other
// message waiting, so that nothing of the program runs in the middle of the wait (a click, a key, a timer:
// the plugin, half-way through, can't be asked anything else). When it ends, those messages are posted
// again, in their order, and the program goes on with them (timers' messages are left out: they come again).
#pragma once
#include <windows.h>

#include <vector>

class BusyPump {
public:
    BusyPump() = default;
    BusyPump(const BusyPump&) = delete;
    BusyPump& operator=(const BusyPump&) = delete;
    ~BusyPump();
    void pump();
    size_t held() const { return held_.size(); }

private:
    std::vector<MSG> held_;
    bool quit_ = false;
    int quitCode_ = 0;
};
