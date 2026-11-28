#include "audio/power_throttling_win.h"

#include <windows.h>

#include <string>

#include "core/util.h"

namespace audio {

namespace {

// THREAD_POWER_THROTTLING_STATE / PROCESS_POWER_THROTTLING_STATE (processthreadsapi.h; older MinGW headers lack them)
struct PowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr ULONG POWER_THROTTLING_VERSION = 1;
constexpr int THREAD_POWER_THROTTLING_CLASS = 3;   // ThreadPowerThrottling
constexpr int PROCESS_POWER_THROTTLING_CLASS = 4;  // ProcessPowerThrottling
constexpr ULONG EXECUTION_SPEED = 0x1;             // THREAD_POWER_THROTTLING_EXECUTION_SPEED
constexpr ULONG IGNORE_TIMER_RESOLUTION = 0x4;     // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION

using SetInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

// kernel32's SetThreadInformation or SetProcessInformation; null on Windows 7
SetInformation kernelFunction(const char* name) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    return kernel ? reinterpret_cast<SetInformation>(reinterpret_cast<void*>(GetProcAddress(kernel, name))) : nullptr;
}

}  // namespace

bool keepThreadAtFullSpeed() {
    static SetInformation setThreadInformation = kernelFunction("SetThreadInformation");
    PowerThrottlingState state{POWER_THROTTLING_VERSION, EXECUTION_SPEED, 0};  // (managed by us, and off)
    if (setThreadInformation &&
        setThreadInformation(GetCurrentThread(), THREAD_POWER_THROTTLING_CLASS, &state, sizeof state))
        return true;
    util::debugf("audio: this thread's power throttling stays as Windows chooses (%s)\n",
                 setThreadInformation ? ("error " + std::to_string(GetLastError())).c_str() : "no SetThreadInformation");
    return false;
}

bool keepProcessTimerResolution() {
    static SetInformation setProcessInformation = kernelFunction("SetProcessInformation");
    PowerThrottlingState state{POWER_THROTTLING_VERSION, IGNORE_TIMER_RESOLUTION, 0};  // (never ignored)
    if (setProcessInformation &&
        setProcessInformation(GetCurrentProcess(), PROCESS_POWER_THROTTLING_CLASS, &state, sizeof state))
        return true;
    util::debugf("audio: the timer resolution may be lowered while minimized (%s)\n",
                 setProcessInformation ? ("error " + std::to_string(GetLastError())).c_str()
                                       : "no SetProcessInformation");
    return false;
}

}  // namespace audio
