// What a prefix's Windows programs are doing (see installer_activity.h): read from /proc, and from
// the fixes' check program run in the prefix.
#include "plugins/installer_activity.h"

#include <unistd.h>

#include <cstdlib>

#include "core/util.h"
#include "plugins/wine.h"
#include "plugins/wine_prefix_fixes.h"

namespace installer_activity {

namespace {

// does this Linux process run in the prefix (WINEPREFIX=<prefix> in its environment)?
bool runsIn(const std::string& processFolder, const std::string& wanted) {
    std::string environment;
    if (!util::readFile(processFolder + "/environ", environment))
        return false;
    for (const auto& entry : util::split(environment, '\0'))
        if (util::startsWith(entry, "WINEPREFIX="))
            return util::absolutePath(entry.substr(11)) == wanted;
    return false;
}

// the processor time a process has used (its user and system time), in seconds
double processSeconds(const std::string& processFolder) {
    std::string stat;
    if (!util::readFile(processFolder + "/stat", stat))
        return 0;
    size_t nameEnd = stat.rfind(')');  // (its name may hold spaces)
    if (nameEnd == std::string::npos)
        return 0;
    auto fields = util::splitWords(stat.substr(nameEnd + 1));
    // (after the name: state is field 3, utime 14 and stime 15)
    if (fields.size() < 13)
        return 0;
    double ticks = std::atof(fields[11].c_str()) + std::atof(fields[12].c_str());
    return ticks / (double)sysconf(_SC_CLK_TCK);
}

}  // namespace

double processorSeconds(const std::string& prefix) {
    std::string wanted = util::absolutePath(prefix);
    while (wanted.size() > 1 && wanted.back() == '/')
        wanted.pop_back();
    double total = 0;
    for (const auto& entry : util::listDir("/proc")) {
        if (entry.name.empty() || entry.name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::string folder = "/proc/" + entry.name;
        if (runsIn(folder, wanted))
            total += processSeconds(folder);
    }
    return total;
}

int visibleWindows(const std::string& prefix) {
    std::string folder = wine::fixesFolder();
    std::string program = folder.empty() ? "" : util::joinPath(folder, "exbit-wine-fix-check.exe");
    if (program.empty() || !util::fileExists(program))
        program = folder.empty() ? "" : util::joinPath(folder, "exbit-wine-fix-check-32.exe");
    if (program.empty() || !util::fileExists(program))
        return -1;
    std::string output;
    if (wine::runInPrefix(prefix, {program, "windows"}, 30, &output) != 0)
        return -1;
    size_t at = output.find("windows: ");
    return at == std::string::npos ? -1 : std::atoi(output.c_str() + at + 9);
}

watchdog::Sample sample(const std::string& prefix, double now) {
    watchdog::Sample look;
    look.seconds = now;
    look.processorSeconds = processorSeconds(prefix);
    look.visibleWindows = visibleWindows(prefix);
    return look;
}

}  // namespace installer_activity
