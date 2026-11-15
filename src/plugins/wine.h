// Windows plugins on Linux, through Wine (no yabridge: Tiny DAW's own host, src/winehost, and
// plugins/bridged.cpp; the protocol is plugins/bridge.h).
//
// Wine prefixes are where Windows programs (and plugins) are installed: a folder holding drive_c
// (its C: drive) and the registry (system.reg). Options → Wine prefixes chooses which are searched
// (Preferences "wine_prefixes"; the default: $WINEPREFIX, else ~/.wine, if it is one). In each:
//   - VST3 plugins in the usual Windows folder, C:\Program Files\Common Files\VST3 (bundles,
//     X.vst3/Contents/x86_64-win/X.vst3, and single-file .vst3 DLLs), searched with its subfolders;
//   - VST2 plugins (.dll) in the folders chosen in Options → VST2 plugin folders, which offers the
//     usual Windows ones of every prefix (C:\Program Files\VstPlugins, …\Steinberg\VstPlugins,
//     …\Common Files\VST2, …\Common Files\Steinberg\VST2). A .dll outside every prefix runs in the
//     first one.
// Each plugin then runs in a Wine host process of its own, in the plugin's prefix. Only 64-bit
// plugins (the host is 64-bit).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "plugins/plugin.h"

namespace wine {

// --- prefixes ---
bool isPrefix(const std::string& dir);              // has drive_c and system.reg
std::string defaultPrefix();                        // $WINEPREFIX or ~/.wine ("" if it isn't a prefix)
std::vector<std::string> prefixes();                // the chosen ones (setPrefixes; else the default one)
void setPrefixes(const std::vector<std::string>& chosen);
std::string prefixOf(const std::string& path);      // the prefix holding this file ("" if none)
std::string prefixFor(const std::string& path);     // the one to run it in: prefixOf(), else the first chosen

// the VST3 folder of these prefixes (those that exist)
std::vector<std::string> vst3Folders(const std::vector<std::string>& prefixes);
// the usual Windows VST2 folders of these prefixes (those that exist)
std::vector<std::string> vst2UsualFolders(const std::vector<std::string>& prefixes);

// --- plugin files ---
// a 64-bit Windows DLL? (read from its header); `machine` gets its machine type (0x8664: x64, 0x14c: x86)
bool isWindowsBinary(const std::string& file, int* machine = nullptr);
// is this plugin path (a file, or the binary inside a bundle) a Windows one, run through Wine?
bool isWindowsPlugin(const std::string& path);
// from the path alone, without reading anything: inside a prefix's drive_c, or a .dll (for labels)
bool looksWindows(const std::string& path);

// --- a prefix's facts ---
// the Windows a prefix was made for, from its registry: "win64", "win32" ("" when not written yet:
// a prefix being made has no system.reg until its wineserver ends)
std::string prefixArch(const std::string& prefix);

// --- Wine and the host ---
std::string winePath();   // the wine program: $TINYDAW_WINE, else wine on the PATH, else /opt/wine-*/bin/wine ("" none)
// the wine program to run in this prefix: the one chosen for it (setWinePrograms), else the one of the
// Bottles bottle it is (its runner), else the one of the Lutris game it is the prefix of (its Wine
// version), else winePath(); "" none. A runner that is missing gives winePath() (and wineSource says so).
std::string winePathFor(const std::string& prefix);
// where winePathFor's choice comes from, for people: "chosen", "Bottles: <runner>", "Lutris: <version>",
// "" (the usual one), or why a Bottles/Lutris one wasn't used ("Bottles: <runner> isn't installed")
std::string wineSource(const std::string& prefix);
// the automatic choice (without setWinePrograms'), and where it comes from (as wineSource)
std::string automaticWine(const std::string& prefix, std::string& source);
// Options → Wine prefixes: a wine program for some prefixes ({prefix: program}; the rest: automatic)
void setWinePrograms(const std::map<std::string, std::string>& chosen);
std::map<std::string, std::string> winePrograms();
// "wine-11.22 (Staging)": what this wine program says it is (asked once; "" if it doesn't answer)
std::string wineVersion(const std::string& wine);
// its major version (11 for "wine-11.22"; 0 unknown)
int wineMajor(const std::string& version);
constexpr int TESTED_WINE_MAJOR = 11;  // the Wine Tiny DAW's host is tested with (older: a warning, not a refusal)
std::string hostPath();   // tinydaw-wine-host.exe: $TINYDAW_WINE_HOST, else next to the program ("" none)
// can Windows plugins run here? if not, why (for the error a load gives)
bool available(std::string& why);
// …and in this prefix (its Wine there, a 64-bit prefix)? if not, why
bool availableIn(const std::string& prefix, std::string& why);
// starts the host under Wine in `prefix` with these arguments; its stdout goes to outFd (-1: this
// process's), its stdin comes from inFd (-1: nothing); returns its pid, or -1 (and why)
int spawnHost(const std::string& prefix, const std::vector<std::string>& args, int outFd, std::string& why,
              int inFd = -1);

}  // namespace wine

namespace plug {

// (plugins/bridged.cpp) a Windows plugin in a Wine host of its own (Plugin::load does this for
// Windows plugin files); nullptr and `error` if it can't
std::unique_ptr<Plugin> loadBridged(const std::string& path, const std::string& name, std::string& error);
// the plugins in a Windows plugin file (vst3Classes / vst2Classes do this for Windows files), asked
// of a Wine host started for the purpose
std::vector<ClassInfo> bridgedClasses(const std::string& path, std::string& error);
// (tests) waits until the host has handed over its news (edited parameters, sizes); false if it
// isn't a Windows plugin
bool settleBridged(Plugin& plugin, double seconds = 5);
// (tests, the CPU monitor) buffers the host was too late for
int bridgedLateBuffers(Plugin& plugin);
// (tests) where Wine believes the plugin's window is on the screen
bool bridgedWindowRect(Plugin& plugin, int& left, int& top, int& right, int& bottom);
// (tests) the host's process id (-1: not a Windows plugin)
int bridgedHostPid(Plugin& plugin);

}  // namespace plug
