// The built-in effects: listing and making them (daw/audio/effects.py).
#include "effects/effects.h"

#include <algorithm>
#include <stdexcept>

#include "effects/builtin.h"

namespace effects {
namespace {

struct Entry {
    Info info;
    std::unique_ptr<Effect> (*make)();
};

// (plugin_chooser.py's BUILTIN_CATEGORIES), sorted by name, as effects.listing()
const std::vector<Entry>& entries() {
    static const std::vector<Entry> all = [] {
        std::vector<Entry> e = {
            {{"bitcrusher", "Bit-crusher", "Distortion"}, makeBitcrusher},
            {{"chorus", "Chorus", "Modulation"}, makeChorus},
            {{"compressor", "Compressor", "Dynamics"}, makeCompressor},
            {{"delay", "Delay", "Delay"}, makeDelay},
            {{"eq", "EQ", "EQ"}, makeEq},
            {{"filter_motion", "Filter Motion", "Filter"}, makeFilterMotion},
            {{"gate", "Gate", "Dynamics"}, makeGate},
            {{"limiter", "Limiter", "Dynamics"}, makeLimiter},
            {{"multiband_compressor", "Multi-band compressor", "Dynamics"}, makeMultibandCompressor},
            {{"phaser_flanger", "Phaser / Flanger", "Modulation"}, makePhaserFlanger},
            {{"reverb", "Reverb", "Reverb"}, makeReverb},
            {{"saturation", "Saturation", "Distortion"}, makeSaturation},
            {{"tremolo_pan", "Tremolo / Pan", "Modulation"}, makeTremoloPan},
            {{"utility", "Utility", "Utility"}, makeUtility},
            // (transient shaping and time manipulation: transient.cpp, timefx.cpp)
            {{"transient_shaper", "Transient Shaper", "Dynamics"}, makeTransientShaper},
            {{"time_fx", "Time FX", "Delay"}, makeTimeFx},
        };
        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        std::stable_sort(e.begin(), e.end(),
                         [&](const Entry& a, const Entry& b) { return lower(a.info.name) < lower(b.info.name); });
        return e;
    }();
    return all;
}

std::string idOf(const std::string& idOrPath) {
    return isBuiltin(idOrPath) ? idOrPath.substr(std::string(PREFIX).size()) : idOrPath;
}

// plug::Plugin::load()'s way to the built-in effects (plugins/plugin.h)
std::unique_ptr<plug::Plugin> createForHost(const std::string& path, std::string& error) {
    return create(path, error);
}

struct Registrar {
    Registrar() { plug::createBuiltin = createForHost; }
} registrar;

}  // namespace

bool isBuiltin(const std::string& path) { return path.compare(0, std::string(PREFIX).size(), PREFIX) == 0; }

const std::vector<Info>& list() {
    static const std::vector<Info> infos = [] {
        std::vector<Info> v;
        for (const auto& e : entries()) v.push_back(e.info);
        return v;
    }();
    return infos;
}

const Info* find(const std::string& idOrPath) {
    std::string id = idOf(idOrPath);
    for (const auto& info : list())
        if (info.id == id) return &info;
    return nullptr;
}

std::string categoryOf(const std::string& path) {
    const Info* info = find(path);
    return info ? info->category : "Other";
}

std::unique_ptr<Effect> create(const std::string& idOrPath, std::string& error) {
    error.clear();
    std::string id = idOf(idOrPath);
    for (const auto& e : entries())
        if (e.info.id == id) {
            auto effect = e.make();
            effect->prepare(48000.0);
            return effect;
        }
    error = "There is no built-in effect " + id + " (its script is not in the effects folder)";
    return nullptr;
}

std::unique_ptr<Effect> create(const std::string& idOrPath) {
    std::string error;
    auto effect = create(idOrPath, error);
    if (!effect) throw std::runtime_error(error);
    return effect;
}

}  // namespace effects
