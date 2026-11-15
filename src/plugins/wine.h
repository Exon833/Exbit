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

// --- Wine and the host ---
std::string winePath();   // the wine program: $TINYDAW_WINE, else wine on the PATH, else /opt/wine-*/bin/wine ("" none)
std::string hostPath();   // tinydaw-wine-host.exe: $TINYDAW_WINE_HOST, else next to the program ("" none)
// can Windows plugins run here? if not, why (for the error a load gives)
bool available(std::string& why);
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
// is it a Windows plugin, in a Wine host of its own (so it can be started afresh: PluginHost::reload)?
bool isBridged(const Plugin& plugin);
// a Windows plugin whose Wine host has ended (the plugin crashed)
bool bridgedHostEnded(const Plugin& plugin);
// ends a Windows plugin's Wine host now (one stopped while its host lives on, late or hung: before a reload)
void endBridgedHost(Plugin& plugin);
// (tests) a Windows plugin whose settings are known here (what a save gets after a crash)
bool bridgedStateKnown(const Plugin& plugin);

}  // namespace plug
