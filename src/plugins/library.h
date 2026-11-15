// The plugins on this computer (daw/audio/plugins.py's PluginLibrary and helpers, and
// juce_state.py), for the GUI: finding them, their kinds, loading them, their states.
//
// Finding plugins is instant: the VST3 folders are listed, and each plugin's kind (instrument or
// effect) comes from the bundle's moduleinfo.json or, failing that, from a cache. The kinds still
// unknown are found out in the background when the program starts (plugins/probe.h, in a process of
// its own; learn()), once per plugin file (again only if the file changes), and otherwise the first
// time a plugin is loaded. VST2 plugins are shared libraries (.so; Windows: .dll) in the folders
// chosen in Options → VST2 plugin folders: one exporting "VSTPluginMain" (or "main") is one (read
// from the file, never run; isVst2File()), and the answer is cached by the file's date. Their names
// end in " (VST2)", as many plugins come in both kinds (plugin_cache.json in plat::configDir()).
// The cache also keeps how long each plugin took to load last time, for the "please wait" message.
//
// The cache file's layout is the Python program's:
//   { "<file>": {"mtime": 1712345678.123, "plugins": [{"name": …, "kind": "instrument"|"effect"|null,
//                 "plugin_name": …|null}, …], "probe_failed": "<why>" (only if it failed)},
//     …, "load seconds": {"<file>|<plugin name>": 0.42, …} }
#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "plugins/plugin.h"

namespace plug {

constexpr const char* INSTRUMENT = "instrument";
constexpr const char* EFFECT = "effect";
constexpr const char* VST2_SUFFIX = " (VST2)";
constexpr const char* LOAD_TIMES = "load seconds";  // key in the cache: {"path|plugin name": seconds the last load took}
constexpr const char* STATE_MAGIC = "TDV3";

// the VST3 folders (Linux: ~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3)
std::vector<std::string> defaultFolders();
// where VST2 plugins usually are (Options → VST2 plugin folders offers those that exist)
std::vector<std::string> vst2UsualFolders();
// plugin_cache.json in plat::configDir()
std::string defaultCachePath();

struct PluginInfo {
    std::string path;        // the file to load (the one inside a bundle folder, if it is a bundle)
    std::string name;
    std::string kind;        // INSTRUMENT, EFFECT, or "" until found out (probe) or loaded once
    std::string pluginName;  // which plugin, for files that hold several (e.g. "Serum 2" / "Serum 2 FX"); "" = none
    std::string label() const {
        if (isBuiltinPath(path))
            return name + "  (built-in)";
        std::string windows = looksWindows() ? (is32Bit() ? "  (Windows, 32-bit)" : "  (Windows)") : "";
        return kind.empty() ? name + windows + "  (type not known yet)" : name + windows;
    }
    // a Windows plugin, run through Wine (from the path: inside a Wine prefix, or a .dll)
    bool looksWindows() const;
    // a 32-bit Windows plugin (run by the 32-bit Wine host; read from the file once, then remembered)
    bool is32Bit() const;
    bool operator==(const PluginInfo& o) const {
        return path == o.path && name == o.name && kind == o.kind && pluginName == o.pluginName;
    }
    bool operator!=(const PluginInfo& o) const { return !(*this == o); }
};

// The file holds several plugins; `names` lists them.
class PluginChoiceNeeded : public std::runtime_error {
public:
    explicit PluginChoiceNeeded(std::vector<std::string> names);
    std::vector<std::string> names;
};

// a plugin file's [(name, kind)] as the probe or moduleinfo.json give them
using KindList = std::vector<std::pair<std::string, std::string>>;

// For a bundle folder (X.vst3/Contents/x86_64-linux/X.so) the inner file (projects name plugins by it).
std::string loadableFile(const std::string& path);
// is this a VST3 plugin's path: a ".vst3" bundle or file, or the binary inside a bundle? (else VST2)
bool isVst3Path(const std::string& path);
inline bool isVst2Path(const std::string& path) { return !isVst3Path(path); }
// a bundle's Contents/Resources/moduleinfo.json (JSON5-ish: comments, trailing commas): the audio
// classes' (name, kind); false if it has none or can't be read
bool readModuleinfo(const std::string& bundle, KindList& classes);
// VST3 modules stay loaded once loaded (plugins live in them); this unloads those no plugin uses any
// more (ModuleExit, then the file). Returns how many. Main thread, never while loading. (vst3host.cpp)
int unloadUnusedModules();

class PluginLibrary {
public:
    // cachePath "": defaultCachePath() (looked up now, so tests can use another)
    explicit PluginLibrary(std::vector<std::string> folders = defaultFolders(), std::string cachePath = "",
                           std::vector<std::string> vst2Folders = {});

    std::vector<PluginInfo> plugins;  // sorted by name (case-insensitively)
    std::vector<std::string> folders, vst2Folders;
    std::string cachePath;
    Json cache;

    std::vector<PluginInfo> scan();
    // the VST2 plugins in the chosen folders (and their subfolders)
    std::vector<PluginInfo> scanVst2();
    // the plugin files with a plugin whose kind isn't known yet, and that weren't looked at in vain
    // before (for kind finding: probe.h)
    std::vector<std::string> unknownFiles() const;
    bool probeFailedBefore(const std::string& file) const;
    // the kinds the probe found for a file. One plugin keeps the name it is listed by; a file holding
    // several is listed once per plugin (by their names).
    void learn(const std::string& file, const KindList& classes);
    // the probe couldn't tell (the plugin failed, crashed or hung): not tried again unless the file
    // changes; its kind is learnt when it is loaded
    void probeFailed(const std::string& file, const std::string& why);
    // plugins of that kind, plus those whose kind is not known yet
    std::vector<PluginInfo> ofKind(const std::string& kind) const;
    // Load a plugin (main thread only), ready to process at `sampleRate`. Learns and remembers its
    // kind and how long it took. Throws on failure: PluginChoiceNeeded (the file holds several),
    // std::runtime_error (why it can't).
    std::unique_ptr<Plugin> load(const PluginInfo& info, double sampleRate = 48000);
    static std::string loadKey(const PluginInfo& info);
    // how long this plugin took to load last time (seconds), or -1 if it never loaded here
    double lastLoadSeconds(const PluginInfo& info) const;
    void saveCache();
    void remember(const std::string& file, const std::vector<PluginInfo>& infos);
};

// --- states (plugins.py state_of / set_state, juce_state.py) ---

// a loaded plugin's settings, to save: packState(component, controller)
std::string stateOf(Plugin& plugin);
// give a loaded plugin saved settings (from stateOf(), or from an older version: JUCE's format);
// throws std::invalid_argument("unknown plugin state")
void setState(Plugin& plugin, const std::string& state);

// States saved by Tiny DAW versions up to 0.3.1, which hosted plugins through pedalboard (JUCE): a
// VST3 plugin's state was JUCE's binary XML, "VC2!" + length + XML holding the processor's state
// (<IComponent>) and the window's (<IEditController>), each in JUCE's own base64 ("<byte
// count>.<characters>", six bits per character, lowest bits first).
bool isJuceState(const std::string& blob);
std::string juceDecodeBase64(const std::string& text);  // JUCE's MemoryBlock::fromBase64Encoding
// (processor state, window state) from an old JUCE state; either may be ""; false if it isn't one
bool juceSplit(const std::string& blob, std::string& component, std::string& controller);

}  // namespace plug
