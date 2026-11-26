// The log of each Windows plugin install (Options → Install Windows plugin…): what Wine and the
// installer printed, kept in Exbit's data folder (install-logs/), the newest few, so a failed install
// can say where to look and show its last lines.
#pragma once
#include <string>
#include <vector>

namespace install_log {

constexpr int KEPT_LOGS = 10;                          // the newest kept; older ones are deleted
constexpr long long LARGEST_LOG = 32LL * 1024 * 1024;  // a log longer than this is cut to its end

// the folder: <data folder>/install-logs (made if missing)
std::string folder();

// a new log's path in `dir` for this installer, named by the time and the installer:
// "2026-11-26_18-41-07 Kontakt 8 Setup PC.log" (characters a file name can't have become '_')
std::string newLogPath(const std::string& dir, const std::string& installer, long long unixSeconds);

// deletes all but the newest `keep` logs (*.log) in `dir`; how many it deleted
int rotate(const std::string& dir, int keep = KEPT_LOGS);

// the last `lines` lines of a file ("" if it can't be read), without the empty ones at its end
std::string lastLines(const std::string& file, int lines);

// a log longer than `largest` bytes is cut to its last `largest` bytes (after a line about it)
void limitSize(const std::string& file, long long largest = LARGEST_LOG);

}  // namespace install_log
