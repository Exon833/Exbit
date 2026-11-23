// The built-in synth's patch: its parameters, files, presets and live notes (synth_patch.h).
#include "audio/synth_patch.h"

#include <algorithm>
#include <cmath>

#include "audio/live.h"
#include "audio/sampler.h"
#include "core/util.h"
#include "model/model.h"
#include "paths.h"
#include "platform/platform.h"

namespace subsynth {

namespace {
using K = ParamInfo;
const std::vector<const char*> WAVES = {"Sine", "Triangle", "Saw", "Pulse"};
const std::vector<ParamInfo>& table() {
    static const std::vector<ParamInfo> t = {
        {"osc1_wave", "Wave", "Osc 1", K::Choice, 0, 3, SAW, "", WAVES, false},
        {"osc1_octave", "Octave", "Osc 1", K::Int, -3, 3, 0, "", {}, false},
        {"osc1_semi", "Semitones", "Osc 1", K::Int, -24, 24, 0, "st", {}, true},
        {"osc1_detune", "Detune", "Osc 1", K::Number, -100, 100, 0, "ct", {}, true},
        {"osc1_level", "Level", "Osc 1", K::Number, 0, 1, 1, "%", {}, true},
        {"osc1_width", "Width", "Osc 1", K::Number, 0.05, 0.95, 0.5, "%", {}, true},
        {"osc2_wave", "Wave", "Osc 2", K::Choice, 0, 3, SAW, "", WAVES, false},
        {"osc2_octave", "Octave", "Osc 2", K::Int, -3, 3, 0, "", {}, false},
        {"osc2_semi", "Semitones", "Osc 2", K::Int, -24, 24, 0, "st", {}, true},
        {"osc2_detune", "Detune", "Osc 2", K::Number, -100, 100, 7, "ct", {}, true},
        {"osc2_level", "Level", "Osc 2", K::Number, 0, 1, 0, "%", {}, true},
        {"osc2_width", "Width", "Osc 2", K::Number, 0.05, 0.95, 0.5, "%", {}, true},
        {"noise", "Noise", "Noise", K::Number, 0, 1, 0, "%", {}, true},
        {"unison", "Voices", "Unison", K::Int, 1, MAX_UNISON, 1, "", {}, false},
        {"spread", "Spread", "Unison", K::Number, 0, 100, 15, "ct", {}, true},
        {"filter_type", "Type", "Filter", K::Choice, 0, 2, LOWPASS, "", {"Low-pass", "High-pass", "Band-pass"}, false},
        {"cutoff", "Cutoff", "Filter", K::Log, 20, 20000, 12000, "Hz", {}, true},
        {"resonance", "Resonance", "Filter", K::Number, 0, 1, 0, "%", {}, true},
        {"drive", "Drive", "Filter", K::Number, 0, 1, 0, "%", {}, true},
        {"key_track", "Key track", "Filter", K::Number, 0, 1, 0, "%", {}, true},
        {"filter_env", "Env 2", "Filter", K::Number, -1, 1, 0, "%", {}, true},
        {"env1_attack", "Attack", "Env 1", K::Log, 0.001, 10, 0.005, "s", {}, false},
        {"env1_decay", "Decay", "Env 1", K::Log, 0.001, 10, 0.3, "s", {}, false},
        {"env1_sustain", "Sustain", "Env 1", K::Number, 0, 1, 1, "%", {}, false},
        {"env1_release", "Release", "Env 1", K::Log, 0.001, 10, 0.15, "s", {}, false},
        {"env2_attack", "Attack", "Env 2", K::Log, 0.001, 10, 0.005, "s", {}, false},
        {"env2_decay", "Decay", "Env 2", K::Log, 0.001, 10, 0.4, "s", {}, false},
        {"env2_sustain", "Sustain", "Env 2", K::Number, 0, 1, 0, "%", {}, false},
        {"env2_release", "Release", "Env 2", K::Log, 0.001, 10, 0.2, "s", {}, false},
        {"lfo_shape", "Shape", "LFO", K::Choice, 0, 4, 0, "", {"Sine", "Triangle", "Saw", "Square", "Random"}, false},
        {"lfo_rate", "Rate", "LFO", K::Log, 0.05, 30, 2, "Hz", {}, true},
        {"lfo_sync", "Sync", "LFO", K::Choice, 0, 11, 0, "",
         {"Free", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/4 dotted"}, false},
        {"lfo_retrigger", "Retrigger", "LFO", K::Toggle, 0, 1, 1, "", {}, false},
        {"glide", "Glide", "Voice", K::Number, 0, 2, 0, "s", {}, false},
        {"mono", "Mode", "Voice", K::Choice, 0, 1, 0, "", {"Poly", "Mono"}, false},
        {"drift", "Drift", "Voice", K::Number, 0, 1, 0, "%", {}, true},
        {"output", "Output", "Voice", K::Number, -24, 12, 0, "dB", {}, true},
    };
    return t;
}
}  // namespace

const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.5};
const std::vector<const char*> CATEGORIES = {"Bass", "Lead", "Pad", "Keys", "Pluck", "FX"};

const ParamInfo& info(int p) { return table()[(size_t)std::clamp(p, 0, PARAMS - 1)]; }

int paramOf(const std::string& key) {
    for (int p = 0; p < PARAMS; p++)
        if (key == info(p).key) return p;
    return -1;
}

const char* sourceName(int s) { return s == ENV1 ? "Env 1" : s == ENV2 ? "Env 2" : "LFO"; }
const char* sourceKey(int s) { return s == ENV1 ? "env1" : s == ENV2 ? "env2" : "lfo"; }
int sourceOf(const std::string& key) {
    for (int s = 0; s < SOURCES; s++)
        if (key == sourceKey(s)) return s;
    return -1;
}

double clampValue(int p, double v) {
    const ParamInfo& i = info(p);
    if (!std::isfinite(v)) return i.def;
    v = std::clamp(v, i.lo, i.hi);
    if (i.kind == K::Int || i.kind == K::Choice || i.kind == K::Toggle) v = std::nearbyint(v);
    return v;
}

double toNorm(int p, double v) {
    const ParamInfo& i = info(p);
    v = std::clamp(std::isfinite(v) ? v : i.def, i.lo, i.hi);
    if (i.kind == K::Log) return std::log(v / i.lo) / std::log(i.hi / i.lo);
    return i.hi > i.lo ? (v - i.lo) / (i.hi - i.lo) : 0;
}

double fromNorm(int p, double n) {
    const ParamInfo& i = info(p);
    n = std::clamp(std::isfinite(n) ? n : 0.0, 0.0, 1.0);
    double v = i.kind == K::Log ? i.lo * std::pow(i.hi / i.lo, n) : i.lo + n * (i.hi - i.lo);
    return clampValue(p, v);
}

std::string valueText(int p, double v) {
    const ParamInfo& i = info(p);
    std::string unit = i.unit;
    switch (i.kind) {
    case K::Choice: return i.choices[(size_t)std::clamp((int)std::nearbyint(v), 0, (int)i.choices.size() - 1)];
    case K::Toggle: return v >= 0.5 ? "On" : "Off";
    case K::Int:
        return util::format(i.lo < 0 ? "%+d%s%s" : "%d%s%s", (int)std::nearbyint(v), unit.empty() ? "" : " ", unit.c_str());
    default: break;
    }
    if (unit == "%") return util::format("%.0f %%", v * 100);
    if (unit == "Hz") return v >= 1000 ? util::format("%.2f kHz", v / 1000) : util::format(v < 10 ? "%.2f Hz" : "%.0f Hz", v);
    if (unit == "s") return v < 1 ? util::format("%.0f ms", v * 1000) : util::format("%.2f s", v);
    if (unit == "dB") return util::format("%+.1f dB", v);
    return util::format("%+.0f %s", v, unit.c_str());
}

Patch::Patch() {
    for (int p = 0; p < PARAMS; p++) v[p] = info(p).def;
}

bool Patch::operator==(const Patch& o) const {
    for (int p = 0; p < PARAMS; p++)
        if (v[p] != o.v[p]) return false;
    bool sameWave = drawn == o.drawn || (drawn && o.drawn && *drawn == *o.drawn);
    return sameWave && mods == o.mods && name == o.name && category == o.category;
}

std::vector<float> fitWave(const std::vector<float>& wave) {
    std::vector<float> out((size_t)WAVE_POINTS, 0.0f);
    size_t n = wave.size();
    if (n < 2) return out;
    for (int i = 0; i < WAVE_POINTS; i++) {  // (read as one cycle: the last point leads back to the first)
        double at = (double)i * n / WAVE_POINTS;
        size_t a = (size_t)at % n, b = (a + 1) % n;
        double f = at - std::floor(at);
        double v = wave[a] + (wave[b] - wave[a]) * f;
        out[(size_t)i] = std::isfinite(v) ? (float)std::clamp(v, -1.0, 1.0) : 0.0f;
    }
    return out;
}

Patch drawnPatch() {
    Patch p;
    p.name = "Drawn";
    std::vector<float> sine((size_t)WAVE_POINTS);
    for (int i = 0; i < WAVE_POINTS; i++) sine[(size_t)i] = (float)std::sin(2 * 3.14159265358979323846 * i / WAVE_POINTS);
    p.drawn = std::make_shared<const std::vector<float>>(std::move(sine));
    return p;
}

const Mod* Patch::modOf(int source, int target) const {
    for (const Mod& m : mods)
        if (m.source == source && m.target == target) return &m;
    return nullptr;
}

void Patch::setMod(int source, int target, double depth) {
    depth = std::isfinite(depth) ? std::clamp(depth, -1.0, 1.0) : 0.0;
    for (size_t i = 0; i < mods.size(); i++)
        if (mods[i].source == source && mods[i].target == target) {
            if (depth == 0) mods.erase(mods.begin() + (long)i);
            else mods[i].depth = depth;
            return;
        }
    if (depth != 0 && source >= 0 && source < SOURCES && target >= 0 && target < PARAMS && info(target).mod)
        mods.push_back({source, target, depth});
}

Json toJson(const Patch& p) {
    Json params = Json::object();
    for (int i = 0; i < PARAMS; i++) params.set(info(i).key, Json(p.v[i]));
    Json mods = Json::array();
    for (const Mod& m : p.mods)
        mods.push(Json::object({{"source", Json(sourceKey(m.source))}, {"target", Json(info(m.target).key)},
                                {"depth", Json(m.depth)}}));
    Json out = Json::object({{"format", Json(FORMAT)}, {"version", Json(1)}, {"name", Json(p.name)},
                             {"category", Json(p.category)}, {"params", params}, {"mods", mods}});
    if (p.drawn) {
        Json wave = Json::array();
        for (float v : *p.drawn) wave.push(Json(std::round(v * 1e5) / 1e5));  // (5 decimals: far finer than drawn)
        out.set("drawn", wave);
    }
    return out;
}

Patch fromJson(const Json& j) {
    Patch p;
    p.name = j.str("name", "Init");
    p.category = j.str("category");
    for (const auto& kv : j["params"].members()) {
        int i = paramOf(kv.first);
        if (i >= 0 && kv.second.isNumber()) p.v[i] = clampValue(i, kv.second.toDouble());
    }
    for (const Json& m : j["mods"].items()) {
        int s = sourceOf(m.str("source")), t = paramOf(m.str("target"));
        if (s >= 0 && t >= 0) p.setMod(s, t, m.num("depth", 0));
    }
    if (j.has("drawn")) {  // a Drawn synth's: its wave as one cycle
        const Json& wave = j["drawn"];
        if (wave.isArray() && wave.items().size() >= 2) {
            std::vector<float> w;
            for (const Json& v : wave.items()) w.push_back(v.isNumber() ? (float)v.toDouble() : 0.0f);
            p.drawn = std::make_shared<const std::vector<float>>(fitWave(w));
        } else {  // (damaged: still the Drawn synth, its wave the sine it starts with, not another instrument)
            util::debugf("synth: a Drawn synth's wave was unreadable: the sine it starts with instead\n");
            p.drawn = drawnPatch().drawn;
        }
    }
    return p;
}

double tailS(const Patch& patch) { return std::min(MAX_TAIL_S, patch[E1_R]); }

bool plays(const Track& track) {
    return std::atomic_load(&track.synth) && !track.sampler && !track.padKit && !track.instrument && !track.usesRack &&
           !track.ext.on;
}

live::NoteRender noteRenderer(const Track& track) {
    PatchPtr patch = std::atomic_load(&track.synth);
    if (!patch || !plays(track)) return sampler::noteRenderer(track);
    TempoMapPtr map = clip_hooks::currentTempoMap();
    double beatS = map ? map->beatSAt(0) : 0.5;
    live::NoteRender r;
    r.render = [patch, beatS](int pitch, int velocity, double durationS, int sr) {
        return renderNote(*patch, pitch, velocity, durationS, sr, beatS);
    };
    r.channels = 2;
    r.period = [](int, int) { return -1.0; };  // (unison, drift and the LFO: no exact period; LOOP_S crossfaded)
    r.releaseS = tailS(*patch);
    return r;
}

// --- presets ---

namespace {
std::string gShipped;
}

void setShippedFolder(const std::string& dir) { gShipped = dir; }
std::string shippedFolder() {
    return gShipped.empty() ? util::joinPath(util::joinPath(plat::dataDir(), "presets"), "synth") : gShipped;
}
std::string userFolder() { return util::joinPath(paths::dataDir("presets"), "synth"); }

std::vector<Preset> presets() {
    std::vector<Preset> out;
    for (int user = 0; user < 2; user++) {
        std::string root = user ? userFolder() : shippedFolder();
        std::vector<Preset> found;
        for (const auto& dir : util::listDir(root)) {
            if (!dir.isDir) continue;
            for (const auto& f : util::listDir(util::joinPath(root, dir.name)))
                if (!f.isDir && util::extension(f.name) == ".json")
                    found.push_back({util::stem(f.name), dir.name, util::joinPath(util::joinPath(root, dir.name), f.name),
                                     user == 1});
        }
        std::stable_sort(found.begin(), found.end(), [](const Preset& a, const Preset& b) {
            if (a.category != b.category) return util::compareNatural(a.category, b.category) < 0;
            return util::compareNatural(a.name, b.name) < 0;
        });
        out.insert(out.end(), found.begin(), found.end());
    }
    util::debugf("synth: %zu presets (%s, %s)\n", out.size(), shippedFolder().c_str(), userFolder().c_str());
    return out;
}

bool load(const std::string& path, Patch& out, std::string& error) {
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Cannot read " + path;
        return false;
    }
    Json j = Json::parse(text, &error);
    if (!j.isObject() || j.str("format") != FORMAT) {
        if (error.empty()) error = path + " is not a synth preset";
        return false;
    }
    out = fromJson(j);
    if (out.name.empty() || out.name == "Init") out.name = util::stem(path);
    if (out.category.empty()) out.category = util::baseName(util::dirName(path));
    util::debugf("synth: preset \"%s\" (%s) loaded from %s, %zu modulations\n", out.name.c_str(), out.category.c_str(),
                 path.c_str(), out.mods.size());
    return true;
}

bool save(const Patch& patch, const std::string& name, const std::string& category, std::string& path,
          std::string& error) {
    auto safe = [](std::string s) {
        for (char& c : s)
            if (c == '/' || (unsigned char)c < 32) c = '_';
        s = util::trim(s);
        return s == "." || s == ".." ? std::string() : s;
    };
    std::string n = safe(name), c = safe(category.empty() ? "User" : category);
    if (n.empty() || c.empty()) {
        error = "The preset needs a name";
        return false;
    }
    std::string dir = util::joinPath(userFolder(), c);
    if (!util::makeDirs(dir)) {
        error = "Cannot make the folder " + dir;
        return false;
    }
    Patch p = patch;
    p.name = n, p.category = c;
    path = util::joinPath(dir, n + ".json");
    if (!util::writeFile(path, toJson(p).dump(2) + "\n")) {
        error = "Cannot write " + path;
        return false;
    }
    util::debugf("synth: preset \"%s\" saved to %s\n", n.c_str(), path.c_str());
    return true;
}

}  // namespace subsynth
