// plugins/wine.h on Windows itself: no Wine. Windows plugins are native there, and run out of
// process as under Wine on Linux: each in a plugin host of its own, exbit-plugin-host.exe (64-bit;
// exbit-plugin-host-32.exe for 32-bit plugins), started directly (child_win.h), speaking the same
// bridge (plugins/bridge.h, bridged.cpp). Wine's prefixes, programs and set-up have nothing to do
// here: none are found, and starting them says so (the menus that offer them aren't shown on Windows).
//
// A 64-bit plugin with no 64-bit host next to the program is loaded in the program itself (a
// failsafe: it works, but a crash of the plugin then ends the program); a 32-bit one needs its host.
#include "plugins/wine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/child_win.h"
#include "core/program_environment.h"

namespace wine {

bool isPrefix(const std::string&) { return false; }
std::string defaultPrefix() { return ""; }
std::vector<std::string> prefixes() { return {}; }
void setPrefixes(const std::vector<std::string>&) {}
std::string prefixOf(const std::string&) { return ""; }
std::string prefixFor(const std::string&) { return ""; }
std::vector<std::string> vst3Folders(const std::vector<std::string>&) { return {}; }
std::vector<std::string> vst2UsualFolders(const std::vector<std::string>&) { return {}; }
std::string prefixArch(const std::string&) { return ""; }

bool isWindowsBinary(const std::string& file, int* machine) {
    if (machine) *machine = 0;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    unsigned char dos[64];
    bool ok = fread(dos, 1, sizeof(dos), f) == sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z';
    if (ok) {
        uint32_t at = dos[60] | dos[61] << 8 | dos[62] << 16 | (uint32_t)dos[63] << 24;
        unsigned char pe[6];
        ok = at < (1u << 20) && fseek(f, (long)at, SEEK_SET) == 0 && fread(pe, 1, sizeof(pe), f) == sizeof(pe) &&
             std::memcmp(pe, "PE\0\0", 4) == 0;
        if (ok && machine) *machine = pe[4] | pe[5] << 8;
    }
    fclose(f);
    return ok;
}

bool is32BitPlugin(const std::string& file) {
    int machine = 0;
    return isWindowsBinary(file, &machine) && machine == 0x14c;
}

// (in a host: every plugin file, but a 64-bit one when there's no 64-bit host: the program's own failsafe)
bool isWindowsPlugin(const std::string& path) {
    int machine = 0;
    if (util::isDir(path) || !isWindowsBinary(path, &machine)) return false;
    return machine == 0x14c || !hostPath(false).empty();
}

bool looksWindows(const std::string&) { return false; }  // (every plugin is: nothing to tell apart)

std::string winePath() { return ""; }
std::string winePathFor(const std::string&) { return ""; }
std::string wineSource(const std::string&) { return ""; }
std::string automaticWine(const std::string&, std::string& source) {
    source.clear();
    return "";
}
void setWinePrograms(const std::map<std::string, std::string>&) {}
std::map<std::string, std::string> winePrograms() { return {}; }
std::string wineVersion(const std::string&) { return ""; }
int wineMajor(const std::string&) { return 0; }

std::string hostPath(bool bits32) {
    if (const char* env = environment::readProgramVariable(bits32 ? "PLUGIN_HOST32" : "PLUGIN_HOST"))
        if (*env) return util::fileExists(env) ? env : "";
    std::string p = util::joinPath(plat::executableDir(), bits32 ? "exbit-plugin-host-32.exe" : "exbit-plugin-host.exe");
    return util::fileExists(p) ? p : "";
}

bool available(std::string& why) {
    if (!hostPath(false).empty()) return true;
    why = "exbit-plugin-host.exe isn't next to exbit.exe (make builds it)";
    return false;
}

bool availableIn(const std::string&, std::string& why, bool bits32) {
    if (!hostPath(bits32).empty()) return true;
    why = bits32 ? "32-bit plugins need exbit-plugin-host-32.exe next to exbit.exe (built by make where a 32-bit "
                   "MinGW, i686-w64-mingw32-g++, is installed)"
                 : "exbit-plugin-host.exe isn't next to exbit.exe (make builds it)";
    return false;
}

namespace {
int notHere(std::string& why) {
    why = "Wine isn't used on Windows: plugins and their installers run as they are";
    return -1;
}
}  // namespace

int startPrefix(const std::string&, const std::string&, std::string& why) { return notHere(why); }
int startServerWait(const std::string&, const std::string&, std::string& why) { return notHere(why); }
int startInstaller(const std::string&, const std::string&, std::string& why) { return notHere(why); }

bool finished(int pid, int& code) { return child::finished(pid, code); }
void stop(int pid) { child::kill(pid); }

int spawnHost(const std::string&, const std::vector<std::string>& args, int outFd, std::string& why, int inFd, bool bits32) {
    std::string host = hostPath(bits32);
    if (host.empty()) {
        availableIn("", why, bits32);
        return -1;
    }
    util::debugf("plugin host: %s", host.c_str());
    for (const auto& a : args) util::debugf(" %s", a.c_str());
    util::debugf("\n");
    return child::spawn(host, args, inFd, outFd, why);
}

}  // namespace wine
