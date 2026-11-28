// The audio thread's priority in plugin-speed-reference.exe, as Exbit's on Windows (audio/devices_win.cpp, which
// the reference doesn't link: it opens no sound device): MMCSS "Pro Audio", else time-critical.
#include <windows.h>

#include <avrt.h>

namespace audio::detail {

// makes the calling thread an audio thread (the worker pool's threads call it too)
void raiseThreadPriority() {
    DWORD task = 0;
    if (HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task))
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    else
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

}  // namespace audio::detail
