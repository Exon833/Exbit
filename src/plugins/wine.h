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
// Each plugin then runs in a Wine host process of its own, in the plugin's prefix: 64-bit plugins in
// tinydaw-wine-host.exe, 32-bit ones in tinydaw-wine-host-32.exe (in a 64-bit prefix through Wine's
// WoW64; also in a 32-bit-only prefix). 32-bit plugins are looked for in Program Files (x86) too.
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
// a Windows DLL? (read from its header); `machine` gets its machine type (0x8664: x64, 0x14c: x86)
bool isWindowsBinary(const std::string& file, int* machine = nullptr);
// a 32-bit (x86) Windows binary, run by the 32-bit host?
bool is32BitPlugin(const std::string& file);
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
// tinydaw-wine-host.exe (bits32: tinydaw-wine-host-32.exe): $TINYDAW_WINE_HOST (…_HOST32), else next
// to the program ("" none)
std::string hostPath(bool bits32 = false);
// can Windows plugins run here? if not, why (for the error a load gives)
bool available(std::string& why);
// …and in this prefix, for a 64-bit plugin (or a 32-bit one: bits32)? Its Wine there, the host for it,
// a prefix it can run in (a 32-bit-only prefix: 32-bit plugins only). If not, why
bool availableIn(const std::string& prefix, std::string& why, bool bits32 = false);
// --- setting up (Options → Wine prefixes → New prefix…, Options → Install Windows plugin…) ---
// Each starts a Wine process and returns its pid at once (-1 and why if it can't); wait with
// finished() (the main thread stays free: a progress window polls it), stop() gives up.
// a new 64-bit prefix in `dir` (missing or empty; its parent is made) with this wine program:
// wineboot -u, WINEARCH=win64. Its registry is written when its wineserver ends: startServerWait.
int startPrefix(const std::string& dir, const std::string& wine, std::string& why);
// waits for the prefix's wineserver to end (`wineserver -w`, the one beside `wine`): not where Tiny
// DAW's own plugin hosts run, which keep it going
int startServerWait(const std::string& prefix, const std::string& wine, std::string& why);
// an installer (.exe; .msi through msiexec /i) in a prefix with its Wine, started in its own folder
int startInstaller(const std::string& prefix, const std::string& installer, std::string& why);
// has this child ended? `code`: its exit status (-1: killed by a signal)
bool finished(int pid, int& code);
void stop(int pid);  // kills it and waits for it

// starts the host under Wine in `prefix` with these arguments; its stdout goes to outFd (-1: this
// process's), its stdin comes from inFd (-1: nothing); returns its pid, or -1 (and why)
int spawnHost(const std::string& prefix, const std::vector<std::string>& args, int outFd, std::string& why,
              int inFd = -1, bool bits32 = false);

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
