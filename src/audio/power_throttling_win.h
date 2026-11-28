// Windows: power throttling kept off the audio threads. Without it, Windows guesses each thread's quality of service
// and may slow an audio thread down or move it to an efficiency core (Windows 10 1709 and later, chiefly on laptops);
// and on Windows 11 a minimized program's timer resolution (timeBeginPeriod) is no longer honoured. Both calls are
// looked up when used (kernel32's SetThreadInformation / SetProcessInformation: Windows 8 and later), so older
// Windows goes on without them.
#pragma once

namespace audio {

// the calling thread asks for full speed (ThreadPowerThrottling: execution speed never throttled); false if this
// Windows can't
bool keepThreadAtFullSpeed();

// the program's timer resolution kept while minimized or hidden (ProcessPowerThrottling:
// PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION off, Windows 11); false if this Windows can't
bool keepProcessTimerResolution();

}  // namespace audio
