#include "ui/preferences.h"

#include <algorithm>

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/wine.h"

namespace prefs {
const std::vector<int>& bufferSizes() {
    static const std::vector<int> v{0, 128, 256, 512, 1024, 2048};
    return v;
}
const std::vector<int>& refreshRates() {
    static const std::vector<int> v{0, 60, 30, 15};  // (0: as often as the screen shows a picture)
    return v;
}
const std::vector<int>& interfaceSizes() {
    static const std::vector<int> v{75, 90, 100, 110, 125, 150, 175, 200};
    return v;
}
}  // namespace prefs

std::vector<std::string> Preferences::vst2List() const {
    std::vector<std::string> out;
    for (auto& f : util::split(vst2Folders, '|'))
        if (!f.empty()) out.push_back(f);
    return out;
}

void Preferences::setVst2List(const std::vector<std::string>& folders) {
    std::vector<std::string> seen;
    for (auto& f : folders)
        if (!f.empty() && std::find(seen.begin(), seen.end(), f) == seen.end()) seen.push_back(f);
    std::string joined;
    for (size_t i = 0; i < seen.size(); i++) joined += (i ? "|" : "") + seen[i];
    vst2Folders = joined;
}

std::vector<std::string> Preferences::wineList() const {
    std::vector<std::string> out;
    for (auto& f : util::split(winePrefixes, '|'))
        if (!f.empty()) out.push_back(f);
    return out;
}

void Preferences::setWineList(const std::vector<std::string>& prefixes) {
    std::vector<std::string> seen;
    for (auto& f : prefixes)
        if (!f.empty() && std::find(seen.begin(), seen.end(), f) == seen.end()) seen.push_back(f);
    std::string joined;
    for (size_t i = 0; i < seen.size(); i++) joined += (i ? "|" : "") + seen[i];
    winePrefixes = joined;
}

std::map<std::string, std::string> Preferences::wineProgramMap() const {
    std::map<std::string, std::string> out;
    Json j = Json::parse(winePrograms);
    for (auto& [prefix, program] : j.members())
        if (!prefix.empty() && program.isString() && !program.toString().empty()) out[prefix] = program.toString();
    return out;
}

void Preferences::setWineProgramMap(const std::map<std::string, std::string>& programs) {
    Json j = Json::object();
    for (auto& [prefix, program] : programs)
        if (!prefix.empty() && !program.empty()) j[prefix] = Json(program);
    winePrograms = programs.empty() ? "" : j.dump();
}

namespace {
// QSettings on Windows stored booleans as "true"/"false" strings and ints as strings in some
// cases; accept those too
bool getBool(const std::string& key, bool def) {
    Json j = settings::get("options/" + key);
    if (j.isBool()) return j.toBool();
    if (j.isString()) return j.toString() == "true";
    if (j.isNumber()) return j.toInt() != 0;
    return def;
}
int getInt(const std::string& key, int def) {
    Json j = settings::get("options/" + key);
    if (j.isNumber()) return j.toInt();
    if (j.isString()) {
        try { return std::stoi(j.toString()); } catch (...) {}
    }
    return def;
}
std::string getStr(const std::string& key, const std::string& def) {
    Json j = settings::get("options/" + key);
    return j.isString() ? j.toString() : def;
}
}  // namespace

int prefs::interfaceScale() {
    auto& sizes = interfaceSizes();
    // (none chosen yet: the largest size not above the desktop's own scale, 100 % on X11)
    int fallback = 100;
    for (int s : sizes)
        if (s >= 100 && s <= plat::systemScale()) fallback = s;
    int size = getInt("interface_scale", fallback);
    return std::find(sizes.begin(), sizes.end(), size) != sizes.end() ? size : fallback;
}

bool prefs::ownWindowFrames() { return getStr("window_frames", "own") != "system"; }

bool prefs::transparentWindows() { return getBool("transparency", true); }

Preferences Preferences::load() {
    Preferences p;
    p.driverType = getStr("driver_type", p.driverType);
    p.deviceName = getStr("device_name", p.deviceName);
    p.wasapiExclusive = getBool("wasapi_exclusive", p.wasapiExclusive);
    p.bufferSize = getInt("buffer_size", p.bufferSize);
    p.processingCores = std::max(0, getInt("processing_cores", p.processingCores));
    p.levelMeters = getBool("level_meters", p.levelMeters);
    p.trackCpu = getBool("track_cpu", p.trackCpu);
    p.tempoLane = getBool("tempo_lane", p.tempoLane);
    p.signatureLane = getBool("signature_lane", p.signatureLane);
    p.loudnessMeters = getBool("loudness_meters", p.loudnessMeters);
    p.refreshFps = getInt("refresh_fps", p.refreshFps);
    p.metronome = getBool("metronome", p.metronome);
    p.skin = getStr("skin", p.skin);
    p.overviewH = getInt("overview_h", p.overviewH);
    p.randomTrackColors = getBool("random_track_colors", p.randomTrackColors);
    p.clipsFollowTrack = getBool("clips_follow_track", p.clipsFollowTrack);
    p.roundedCorners = getBool("rounded_corners", p.roundedCorners);
    p.recordInput = getStr("record_input", p.recordInput);
    p.countInBars = getInt("count_in_bars", p.countInBars);
    p.loopTakes = getBool("loop_takes", p.loopTakes);
    p.recordEffectsNotes = getBool("record_effects_notes", p.recordEffectsNotes);
    p.midiLoopTakes = getBool("midi_loop_takes", p.midiLoopTakes);
    p.lowLatencyMonitoring = getBool("low_latency_monitoring", p.lowLatencyMonitoring);
    p.vst2Folders = getStr("vst2_folders", p.vst2Folders);
    p.winePrefixes = getStr("wine_prefixes", wine::defaultPrefix());
    p.winePrograms = getStr("wine_programs", p.winePrograms);
    p.midiInput = getStr("midi_input", p.midiInput);
    p.midiPorts = getStr("midi_ports", p.midiPorts);
    p.sync = getStr("sync", p.sync);
    p.computerKeyboard = getBool("computer_keyboard", p.computerKeyboard);
    p.keyboardOctave = getInt("keyboard_octave", p.keyboardOctave);
    p.keyboardVelocity = getInt("keyboard_velocity", p.keyboardVelocity);
    p.recentProjects = getStr("recent_projects", p.recentProjects);
    p.defaultTemplate = getStr("default_template", p.defaultTemplate);
    p.renderFormat = getStr("render_format", p.renderFormat);
    p.renderNormalize = getStr("render_normalize", p.renderNormalize);
    p.favouritePlugins = getStr("favourite_plugins", p.favouritePlugins);
    p.recentPlugins = getStr("recent_plugins", p.recentPlugins);
    p.recordOffsets = getStr("record_offsets", p.recordOffsets);
    p.scope = getStr("scope", p.scope);
    p.interfaceScale = prefs::interfaceScale();
    p.nativeWayland = getBool("native_wayland", p.nativeWayland);
    p.ghostNotes = getStr("ghost_notes", p.ghostNotes);
    p.reloadCrashedPlugins = getBool("reload_crashed_plugins", p.reloadCrashedPlugins);
    p.statusHints = getBool("status_hints", p.statusHints);
    p.warpNewFiles = getBool("warp_new_files", p.warpNewFiles);
    p.windowFrames = prefs::ownWindowFrames() ? "own" : "system";
    p.transparency = prefs::transparentWindows();
    auto& sizes = prefs::bufferSizes();
    if (std::find(sizes.begin(), sizes.end(), p.bufferSize) == sizes.end()) p.bufferSize = 0;
    auto& rates = prefs::refreshRates();
    if (std::find(rates.begin(), rates.end(), p.refreshFps) == rates.end()) p.refreshFps = 60;
    p.known_["driver_type"] = Json(p.driverType);
    p.known_["device_name"] = Json(p.deviceName);
    p.known_["wasapi_exclusive"] = Json(p.wasapiExclusive);
    p.known_["buffer_size"] = Json(p.bufferSize);
    p.known_["processing_cores"] = Json(p.processingCores);
    p.known_["track_cpu"] = Json(p.trackCpu);
    p.known_["tempo_lane"] = Json(p.tempoLane);
    p.known_["signature_lane"] = Json(p.signatureLane);
    p.known_["level_meters"] = Json(p.levelMeters);
    p.known_["loudness_meters"] = Json(p.loudnessMeters);
    p.known_["refresh_fps"] = Json(p.refreshFps);
    p.known_["metronome"] = Json(p.metronome);
    p.known_["skin"] = Json(p.skin);
    p.known_["overview_h"] = Json(p.overviewH);
    p.known_["random_track_colors"] = Json(p.randomTrackColors);
    p.known_["clips_follow_track"] = Json(p.clipsFollowTrack);
    p.known_["rounded_corners"] = Json(p.roundedCorners);
    p.known_["record_input"] = Json(p.recordInput);
    p.known_["count_in_bars"] = Json(p.countInBars);
    p.known_["loop_takes"] = Json(p.loopTakes);
    p.known_["record_effects_notes"] = Json(p.recordEffectsNotes);
    p.known_["midi_loop_takes"] = Json(p.midiLoopTakes);
    p.known_["low_latency_monitoring"] = Json(p.lowLatencyMonitoring);
    p.known_["vst2_folders"] = Json(p.vst2Folders);
    p.known_["wine_prefixes"] = Json(p.winePrefixes);  // (so the default prefix is only written once changed)
    p.known_["midi_input"] = Json(p.midiInput);
    p.known_["midi_ports"] = Json(p.midiPorts);
    p.known_["sync"] = Json(p.sync);
    p.known_["computer_keyboard"] = Json(p.computerKeyboard);
    p.known_["keyboard_octave"] = Json(p.keyboardOctave);
    p.known_["keyboard_velocity"] = Json(p.keyboardVelocity);
    p.known_["recent_projects"] = Json(p.recentProjects);
    p.known_["default_template"] = Json(p.defaultTemplate);
    p.known_["render_format"] = Json(p.renderFormat);
    p.known_["render_normalize"] = Json(p.renderNormalize);
    p.known_["favourite_plugins"] = Json(p.favouritePlugins);
    p.known_["recent_plugins"] = Json(p.recentPlugins);
    p.known_["record_offsets"] = Json(p.recordOffsets);
    p.known_["scope"] = Json(p.scope);
    p.known_["wine_programs"] = Json(p.winePrograms);
    p.known_["interface_scale"] = Json(p.interfaceScale);
    p.known_["native_wayland"] = Json(p.nativeWayland);
    p.known_["ghost_notes"] = Json(p.ghostNotes);
    p.known_["reload_crashed_plugins"] = Json(p.reloadCrashedPlugins);
    p.known_["status_hints"] = Json(p.statusHints);
    p.known_["warp_new_files"] = Json(p.warpNewFiles);
    p.known_["window_frames"] = Json(p.windowFrames);
    p.known_["transparency"] = Json(p.transparency);
    return p;
}

void Preferences::save() const {
    // only what changed since it was loaded or saved: other modules write some of these keys themselves
    // (the MIDI input, the computer keyboard, the plugin chooser…), and an old value here mustn't undo theirs
    auto put = [this](const char* key, const Json& v) {
        auto it = known_.find(key);
        if (it != known_.end() && it->second == v) return;
        settings::set(std::string("options/") + key, v);
        known_[key] = v;
    };
    put("driver_type", driverType);
    put("device_name", deviceName);
    put("wasapi_exclusive", wasapiExclusive);
    put("buffer_size", bufferSize);
    put("processing_cores", processingCores);
    put("level_meters", levelMeters);
    put("track_cpu", trackCpu);
    put("tempo_lane", tempoLane);
    put("signature_lane", signatureLane);
    put("loudness_meters", loudnessMeters);
    put("refresh_fps", refreshFps);
    put("metronome", metronome);
    put("skin", skin);
    put("overview_h", overviewH);
    put("random_track_colors", randomTrackColors);
    put("clips_follow_track", clipsFollowTrack);
    put("rounded_corners", roundedCorners);
    put("record_input", recordInput);
    put("count_in_bars", countInBars);
    put("loop_takes", loopTakes);
    put("record_effects_notes", recordEffectsNotes);
    put("midi_loop_takes", midiLoopTakes);
    put("low_latency_monitoring", lowLatencyMonitoring);
    put("vst2_folders", vst2Folders);
    put("wine_prefixes", winePrefixes);
    put("midi_input", midiInput);
    put("midi_ports", midiPorts);
    put("sync", sync);
    put("computer_keyboard", computerKeyboard);
    put("keyboard_octave", keyboardOctave);
    put("keyboard_velocity", keyboardVelocity);
    put("recent_projects", recentProjects);
    put("default_template", defaultTemplate);
    put("render_format", renderFormat);
    put("render_normalize", renderNormalize);
    put("favourite_plugins", favouritePlugins);
    put("recent_plugins", recentPlugins);
    put("record_offsets", recordOffsets);
    put("scope", scope);
    put("wine_programs", winePrograms);
    put("interface_scale", interfaceScale);
    put("native_wayland", nativeWayland);
    put("ghost_notes", ghostNotes);
    put("reload_crashed_plugins", reloadCrashedPlugins);
    put("status_hints", statusHints);
    put("warp_new_files", warpNewFiles);
    put("window_frames", windowFrames);
    put("transparency", transparency);
}
