// Windows: stand-ins for the sources not ported yet (the Makefile's WIN_PENDING), so the program builds and runs
// without them: no Wine (Windows plugins load directly there), no terminal UI. Whoever ports one of those files
// takes it off WIN_PENDING and its stand-ins out of here.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "plugins/wine.h"
#include "tui/app.h"

// --- plugins/wine.cpp and bridged.cpp: Wine is Linux's; on Windows plugins load in the program itself ---
namespace wine {
bool isPrefix(const std::string&) { return false; }
std::string defaultPrefix() { return std::string(); }
std::vector<std::string> prefixes() { return {}; }
void setPrefixes(const std::vector<std::string>&) {}
std::string prefixOf(const std::string&) { return std::string(); }
std::string prefixFor(const std::string&) { return std::string(); }
std::vector<std::string> vst3Folders(const std::vector<std::string>&) { return {}; }
std::vector<std::string> vst2UsualFolders(const std::vector<std::string>&) { return {}; }
std::string prefixArch(const std::string&) { return std::string(); }
std::string winePath() { return std::string(); }
std::string automaticWine(const std::string&, std::string& source) {
    source.clear();
    return std::string();
}
void setWinePrograms(const std::map<std::string, std::string>&) {}
std::string wineVersion(const std::string&) { return std::string(); }
int wineMajor(const std::string&) { return 0; }
bool available(std::string& why) {
    why = "Wine isn't used on Windows";
    return false;
}
int startPrefix(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
int startServerWait(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
int startInstaller(const std::string&, const std::string&, std::string& why) {
    why = "Wine isn't used on Windows";
    return -1;
}
bool finished(int, int& code) {
    code = -1;
    return true;
}
void stop(int) {}
}  // namespace wine

namespace plug {
int bridgedHostPid(Plugin&) { return -1; }
bool isBridged(const Plugin&) { return false; }
std::vector<std::string> bridgedDraggedFiles(Plugin&) { return {}; }
std::string bridgedDropFiles(Plugin&, const std::vector<std::string>&, int, int) { return std::string(); }
bool bridgedHostEnded(const Plugin&) { return false; }
void endBridgedHost(Plugin&) {}
}  // namespace plug

// --- tui/: the terminal UI ---
namespace tui {
int run(const std::vector<std::string>&) {
    fprintf(stderr, "Tiny DAW: the terminal UI (--tui) isn't on Windows yet\n");
    return 1;
}
}  // namespace tui
