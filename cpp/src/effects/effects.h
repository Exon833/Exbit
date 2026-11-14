// The built-in effects that come with Tiny DAW (daw/audio/effects.py's listing and create(), with
// the effects of the program's effects/ folder): EQ, Chorus, Compressor, Multi-band compressor,
// Gate, Reverb, Limiter, Saturation, Bit-crusher, Delay, Utility.
//
// Each is known by an id (its script's name in the Python program: "eq", "multiband_compressor"…);
// projects save it as the slot's path "builtin:<id>" (PluginSlot.path, PluginInfo.path) with kind
// "effect", and its state as any plugin's (plug::stateOf: "TDV3" + the JSON of its values), so
// projects interchange with the Python program. They are listed first, by name, wherever effects
// are chosen, labelled "<name>  (built-in)", with their own category (plugin_chooser.py's
// BUILTIN_CATEGORIES).
//
// plug::Plugin::load() and plug::PluginLibrary::load() make them too, by their path (this file
// registers itself with plugins/plugin.cpp's plug::createBuiltin hook when it is linked in).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "effects/effect.h"

namespace effects {

constexpr const char* PREFIX = "builtin:";

struct Info {
    std::string id;        // "eq"
    std::string name;      // "EQ"
    std::string category;  // "EQ", "Dynamics", "Reverb", "Delay", "Modulation", "Distortion", "Utility"
    std::string path() const { return PREFIX + id; }  // "builtin:eq"
    std::string label() const { return name + "  (built-in)"; }
};

// is this a built-in effect's path ("builtin:…")?
bool isBuiltin(const std::string& path);
// every built-in effect, sorted by name (case-insensitively)
const std::vector<Info>& list();
// the one with this id or path, or nullptr
const Info* find(const std::string& idOrPath);
// the category of a built-in effect's path ("Other" if unknown)
std::string categoryOf(const std::string& path);

// A new instance of the effect `idOrPath` ("eq" or "builtin:eq"), prepared at 48 kHz; nullptr and
// `error` ("There is no built-in effect x (its script is not in the effects folder)") if none.
std::unique_ptr<Effect> create(const std::string& idOrPath, std::string& error);
// the same, throwing std::runtime_error
std::unique_ptr<Effect> create(const std::string& idOrPath);

// the plugin as a built-in effect, or nullptr (a VST plugin)
inline Effect* asEffect(plug::Plugin* p) { return dynamic_cast<Effect*>(p); }

}  // namespace effects
