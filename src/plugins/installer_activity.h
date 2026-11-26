// What a prefix's Windows programs are doing, for the hung-installer watchdog (installer_watchdog.h):
// how much processor time they have used and how many windows they show.
#pragma once
#include <string>

#include "plugins/installer_watchdog.h"

namespace installer_activity {

// the processor time (seconds) used so far by the Linux processes of this prefix's Windows programs and
// its wineserver (those with WINEPREFIX=<prefix> in their environment); 0 when none run
double processorSeconds(const std::string& prefix);

// the windows the prefix's programs show (wine-fixes/exbit-wine-fix-check.exe windows, run there);
// -1 when that can't be told (the fixes weren't built, or it didn't answer)
int visibleWindows(const std::string& prefix);

// one look at the prefix, taken at `now` (seconds, any clock that goes forward)
watchdog::Sample sample(const std::string& prefix, double now);

}  // namespace installer_activity
