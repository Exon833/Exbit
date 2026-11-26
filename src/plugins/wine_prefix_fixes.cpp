// Exbit's Wine fixes in a prefix (see wine_prefix_fixes.h).
#include "plugins/wine_prefix_fixes.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "core/program_environment.h"
#include "core/util.h"
#include "platform/platform.h"
#include "plugins/wine.h"

namespace wine {

namespace {

const char* const OVERRIDES_KEY = "HKCU\\Software\\Wine\\DllOverrides";
const char* const PREFERRED = "native,builtin";
const char* const BACKUP_FOLDER = ".exbit-wine-fixes";

// one of the prefix's Windows folders and the stand-ins' build for it
struct Target {
    std::string windowsFolder;  // "system32", "syswow64"
    std::string machineFolder;  // "x86_64-windows", "i386-windows" (in the fixes folder)
    std::string checkProgram;   // the check built for that machine
};

// where the stand-ins go: a 64-bit prefix's system32 (64-bit) and syswow64 (32-bit); a 32-bit one's system32
std::vector<Target> targetsOf(const std::string& prefix) {
    std::string arch = prefixArch(prefix);
    bool wow64 = arch == "win64" || (arch.empty() && util::isDir(util::joinPath(prefix, "drive_c/windows/syswow64")));
    if (!wow64)
        return {{"system32", "i386-windows", "exbit-wine-fix-check-32.exe"}};
    return {{"system32", "x86_64-windows", "exbit-wine-fix-check.exe"},
            {"syswow64", "i386-windows", "exbit-wine-fix-check-32.exe"}};
}

// where the stand-in for `dll` goes in the prefix (that target's Windows folder)
std::string installedPath(const std::string& prefix, const Target& target, const std::string& dll) {
    return util::joinPath(prefix, "drive_c/windows/" + target.windowsFolder + "/" + dll);
}

// where Wine's own file is kept while the stand-in is in
std::string backupPath(const std::string& prefix, const Target& target, const std::string& dll) {
    return util::joinPath(prefix, std::string(BACKUP_FOLDER) + "/" + target.windowsFolder + "/" + dll);
}

// a file's bytes ("" if it can't be read)
std::string fileText(const std::string& path) {
    std::string text;
    util::readFile(path, text);
    return text;
}

// copies a file (its folder made); true if it was written
bool copyFile(const std::string& from, const std::string& to) {
    std::string data;
    return util::readFile(from, data) && util::makeDirs(util::dirName(to)) && util::writeFile(to, data);
}

// the prefix's DLL overrides ({"msi": "native,builtin"}), asked of Wine (the registry file may be behind)
std::map<std::string, std::string> overridesOf(const std::string& prefix) {
    std::string output;
    std::map<std::string, std::string> found;
    if (runInPrefix(prefix, {"reg", "query", OVERRIDES_KEY}, 60, &output) != 0)
        return found;
    for (const auto& line : util::split(output, '\n')) {
        auto words = util::splitWords(line);
        if (words.size() >= 3 && words[1] == "REG_SZ")
            found[util::lower(words[0])] = util::trim(line.substr(line.find("REG_SZ") + 6));
        else if (words.size() == 2 && words[1] == "REG_SZ")
            found[util::lower(words[0])] = "";
    }
    return found;
}

// tells Wine how to load a DLL in the prefix (HKCU DllOverrides); true if `reg add` worked
bool setOverride(const std::string& prefix, const std::string& name, const std::string& value) {
    return runInPrefix(prefix, {"reg", "add", OVERRIDES_KEY, "/v", name, "/d", value, "/f"}, 60) == 0;
}

// lets Wine choose for a DLL again (its override deleted); true if `reg delete` worked
bool deleteOverride(const std::string& prefix, const std::string& name) {
    return runInPrefix(prefix, {"reg", "delete", OVERRIDES_KEY, "/v", name, "/f"}, 60) == 0;
}

// is any copy of this DLL in the prefix one that isn't Wine's or Exbit's (DXVK's, Windows')?
bool foreignIn(const std::string& prefix, const std::vector<Target>& targets, const std::string& dll) {
    for (const auto& target : targets)
        if (dllKind(installedPath(prefix, target, dll)) == "other")
            return true;
    return false;
}

// the stand-ins Exbit has built for this target
bool builtFor(const std::string& folder, const Target& target, const std::string& dll) {
    return util::fileExists(util::joinPath(folder, target.machineFolder + "/" + dll));
}

// puts Wine's own file back (as it was before the stand-in) and lets Wine choose its own again; a file is
// taken out when it is Exbit's, or the one in `folder` (Exbit's fixes: one that may not even load)
void takeOut(const std::string& prefix, const std::vector<Target>& targets, const std::string& dll,
             const std::string& folder = "") {
    for (const auto& target : targets) {
        std::string installed = installedPath(prefix, target, dll), backup = backupPath(prefix, target, dll);
        bool fromFolder = !folder.empty() && builtFor(folder, target, dll) &&
                          fileText(installed) == fileText(util::joinPath(folder, target.machineFolder + "/" + dll));
        if (dllKind(installed) != "exbit" && !fromFolder)
            continue;
        if (util::fileExists(backup))
            copyFile(backup, installed);
        else
            util::removeFile(installed);  // (Wine finds its own without a placeholder)
    }
    auto overrides = overridesOf(prefix);
    std::string name = util::stem(dll);
    if (overrides.count(name) && overrides[name] == PREFERRED)
        deleteOverride(prefix, name);
}

// runs the check programs in the prefix: {dll: works in every target it was put in}
std::map<std::string, bool> checkFixes(const std::string& prefix, const std::string& folder,
                                       const std::vector<Target>& targets, const std::vector<std::string>& placed,
                                       std::string& report) {
    std::map<std::string, bool> works;
    for (const auto& dll : placed)
        works[dll] = true;
    for (const auto& target : targets) {
        std::string program = util::joinPath(folder, target.checkProgram), output;
        bool checked = util::fileExists(program) && runInPrefix(prefix, {program}, 120, &output) >= 0;
        for (const auto& dll : placed) {
            if (!builtFor(folder, target, dll))
                continue;
            bool fixed = checked && output.find(dll + ": fixed") != std::string::npos;
            if (!fixed) {
                works[dll] = false;
                std::string said;
                for (const auto& line : util::split(output, '\n'))
                    if (util::startsWith(line, dll + ":")) said = util::trim(line);
                report += dll + " (" + target.windowsFolder + "): the check says " +
                          (said.empty() ? std::string("nothing") : "\"" + said + "\"") + "\n";
            }
        }
    }
    return works;
}

}  // namespace

std::string fixesFolder() {
    if (const char* chosen = environment::readProgramVariable("WINE_FIXES"))
        if (*chosen)
            return util::isDir(chosen) ? util::absolutePath(chosen) : "";
    std::string exe = plat::executableDir();
    for (const char* relative : {"", "..", "../..", "../lib/exbit"}) {
        std::string dir = util::joinPath(*relative ? util::joinPath(exe, relative) : exe, "wine-fixes");
        if (util::fileExists(util::joinPath(dir, "x86_64-windows/msi.dll")) ||
            util::fileExists(util::joinPath(dir, "i386-windows/msi.dll")))
            return util::absolutePath(dir);
    }
    return "";
}

const std::vector<std::string>& fixedDllNames() {
    static const std::vector<std::string> names = {"msi.dll", "dxgi.dll"};
    return names;
}

std::string dllKind(const std::string& file) {
    std::string data;
    if (!util::readFile(file, data))
        return "";
    if (data.size() > 0x40 + 20 && data.compare(0x40, 20, "Wine placeholder DLL") == 0)
        return "placeholder";
    if (data.size() > 0x40 + 16 && data.compare(0x40, 16, "Wine builtin DLL") == 0)
        return "builtin";
    if (util::startsWith(data, "MZ") && data.find("ExbitWineFixReady") != std::string::npos)
        return "exbit";
    return "other";
}

bool prefixHasFixes(const std::string& prefix) {
    std::string folder = fixesFolder();
    if (folder.empty() || !isPrefix(prefix))
        return false;
    auto targets = targetsOf(prefix);
    std::map<std::string, std::string> overrides;
    bool overridesRead = false;
    for (const auto& dll : fixedDllNames()) {
        if (foreignIn(prefix, targets, dll))
            continue;
        bool any = false;
        for (const auto& target : targets) {
            if (!builtFor(folder, target, dll))
                continue;
            any = true;
            if (fileText(installedPath(prefix, target, dll)) != fileText(util::joinPath(folder, target.machineFolder + "/" + dll)))
                return false;
        }
        if (!any)
            continue;
        if (!overridesRead) {
            overrides = overridesOf(prefix);
            overridesRead = true;
        }
        if (overrides[util::stem(dll)] != PREFERRED)
            return false;
    }
    return true;
}

bool applyFixes(const std::string& prefix, std::string& report) {
    report.clear();
    std::string folder = fixesFolder();
    if (folder.empty()) {
        report = "Exbit's Wine fixes weren't built with this copy of Exbit (they need MinGW): the prefix is left as Wine made it.";
        return false;
    }
    if (!isPrefix(prefix)) {
        report = prefix + " isn't a Wine prefix.";
        return false;
    }
    auto targets = targetsOf(prefix);
    auto overrides = overridesOf(prefix);
    std::vector<std::string> placed;
    for (const auto& dll : fixedDllNames()) {
        if (foreignIn(prefix, targets, dll)) {
            report += dll + ": left alone (the prefix has one that isn't Wine's, e.g. DXVK's)\n";
            continue;
        }
        bool any = false;
        for (const auto& target : targets) {
            if (!builtFor(folder, target, dll)) {
                report += dll + " (" + target.windowsFolder + "): not built (no MinGW for it), Wine's own kept\n";
                continue;
            }
            std::string installed = installedPath(prefix, target, dll), backup = backupPath(prefix, target, dll);
            std::string source = util::joinPath(folder, target.machineFolder + "/" + dll), kind = dllKind(installed);
            if ((kind == "placeholder" || kind == "builtin") && !util::fileExists(backup))
                copyFile(installed, backup);
            if (fileText(installed) != fileText(source) && !copyFile(source, installed)) {
                report += dll + " (" + target.windowsFolder + "): can't be written\n";
                continue;
            }
            any = true;
        }
        if (!any)
            continue;
        std::string name = util::stem(dll);
        if (overrides[name] != PREFERRED && !setOverride(prefix, name, PREFERRED)) {
            report += dll + ": Wine's settings couldn't be changed (reg add failed)\n";
            takeOut(prefix, targets, dll, folder);
            continue;
        }
        placed.push_back(dll);
    }
    // (the failsafe: a stand-in that doesn't work there is taken out at once)
    auto works = checkFixes(prefix, folder, targets, placed, report);
    bool allWork = true;
    for (const auto& dll : placed) {
        if (works[dll]) {
            report += dll + ": fixed\n";
        } else {
            takeOut(prefix, targets, dll, folder);
            report += dll + ": taken out again, Wine's own is used\n";
            allWork = false;
        }
    }
    while (!report.empty() && report.back() == '\n')
        report.pop_back();
    return allWork && !placed.empty();
}

bool removeFixes(const std::string& prefix, std::string& report) {
    report.clear();
    if (!isPrefix(prefix)) {
        report = prefix + " isn't a Wine prefix.";
        return false;
    }
    auto targets = targetsOf(prefix);
    std::string folder = fixesFolder();
    for (const auto& dll : fixedDllNames()) {
        bool ours = false;
        for (const auto& target : targets)
            ours = ours || dllKind(installedPath(prefix, target, dll)) == "exbit";
        if (!ours)
            continue;
        takeOut(prefix, targets, dll, folder);
        report += dll + ": Wine's own again\n";
    }
    if (report.empty())
        report = "The prefix has none of Exbit's fixes.";
    while (!report.empty() && report.back() == '\n')
        report.pop_back();
    return true;
}

}  // namespace wine
