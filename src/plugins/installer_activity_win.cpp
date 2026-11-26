// A prefix's activity on Windows itself (see installer_activity.h): there are no prefixes; nothing is
// ever taken as hung (the windows can't be told).
#include "plugins/installer_activity.h"

namespace installer_activity {

double processorSeconds(const std::string&) { return 0; }
int visibleWindows(const std::string&) { return -1; }
watchdog::Sample sample(const std::string&, double now) { return {now, 0, -1}; }

}  // namespace installer_activity
