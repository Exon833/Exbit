// Presets: effects' and tracks' settings in files of their own (presets.h).
#include "model/presets.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

#include "audio/automation.h"
#include "audio/modulation.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/project_io.h"
#include "paths.h"
#include "plugins/library.h"

namespace presets {

namespace {

constexpr const char* AUDIO_SUFFIX = "_audio";

std::string audioPrefix(const std::string& stem) { return stem + AUDIO_SUFFIX + "/"; }

// a folder holding only files (a track preset's audio): its files, then itself
bool removeFolder(const std::string& dir) {
    if (!util::isDir(dir)) return true;
    struct stat st;
    if (lstat(dir.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return ::unlink(dir.c_str()) == 0;  // (a link: not followed)
    bool ok = true;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) ok = removeFolder(p) && ok;
        else ok = util::removeFile(p) && ok;
    }
    return ::rmdir(dir.c_str()) == 0 && ok;
}

// every string in `j` starting with `from` starts with `to` instead (a renamed preset's audio files)
void replacePrefix(Json& j, const std::string& from, const std::string& to) {
    if (j.isString()) {
        const std::string& s = j.toString();
        if (util::startsWith(s, from)) j = Json(to + s.substr(from.size()));
    } else if (j.isArray()) {
        for (Json& item : j.items()) replacePrefix(item, from, to);
    } else if (j.isObject()) {
        for (auto& m : j.members()) replacePrefix(m.second, from, to);
    }
}

std::vector<double> valuesOf(const PluginSlot& slot) {
    std::vector<double> out;
    auto inst = aload(slot.instance);
    effects::Effect* e = inst ? effects::asEffect(inst.get()) : nullptr;
    if (!e) return out;
    for (size_t i = 0; i < e->params().size(); i++) out.push_back(e->getParam((uint32_t)i));
    return out;
}

struct Kept {
    std::weak_ptr<PluginSlot> slot;
    Compare compare;
};
std::map<const PluginSlot*, Kept>& compares() {
    static std::map<const PluginSlot*, Kept> all;  // (GUI thread)
    return all;
}

}  // namespace

// --- folders and files ---------------------------------------------------------------------------

std::string folder() { return paths::dataDir("presets"); }

std::string pluginName(const PluginSlot& slot) {
    if (slot.isControls()) return "";
    return slot.name.empty() ? util::stem(slot.path) : slot.name;
}

std::string safeName(const std::string& name) {
    std::string out;
    for (char c : name) out += (c == '/' || c == '\\' || (unsigned char)c < 32 || c == 127) ? '_' : c;
    out = util::trim(out);
    while (!out.empty() && out[0] == '.') out[0] = '_';  // (not hidden, not "." or "..")
    if (out.size() > 120) {  // (whole UTF-8 characters)
        size_t cut = 120;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        out = util::trim(out.substr(0, cut));
    }
    return out;
}

std::string effectsFolder(const PluginSlot& slot) {
    std::string name = safeName(pluginName(slot));
    return util::joinPath(util::joinPath(folder(), "effects"), name.empty() ? "Unnamed" : name);
}

std::string tracksFolder() { return util::joinPath(folder(), "tracks"); }

std::vector<Entry> list(const std::string& dir) {
    std::vector<Entry> out;
    if (!util::isDir(dir)) return out;
    for (auto& e : util::listDir(dir)) {
        if (e.isDir || e.name.empty() || e.name[0] == '.' || util::extension(e.name) != ".json") continue;
        out.push_back({util::stem(e.name), util::joinPath(dir, e.name)});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Entry& a, const Entry& b) { return util::compareNatural(a.name, b.name) < 0; });
    return out;
}

std::string audioFolderOf(const std::string& presetPath) {
    return util::joinPath(util::dirName(util::absolutePath(presetPath)), util::stem(presetPath) + AUDIO_SUFFIX);
}

bool isTrackPresetFile(const std::string& path) {
    if (path.empty() || util::extension(path) != ".json" || util::isDir(path) || !util::fileExists(path)) return false;
    std::string text;
    if (!util::readFile(path, text)) return false;
    Json data = Json::parse(text);
    return data.isObject() && data.str("format") == project_io::FORMAT && data.str("preset") == TRACK;
}

bool rename(const std::string& path, const std::string& newName, std::string& newPath, std::string& error) {
    std::string name = safeName(newName);
    if (name.empty()) {
        error = "A preset needs a name.";
        return false;
    }
    std::string dir = util::dirName(util::absolutePath(path));
    newPath = util::joinPath(dir, name + ".json");
    if (util::absolutePath(newPath) == util::absolutePath(path)) return true;
    if (util::fileExists(newPath) || util::isDir(audioFolderOf(newPath))) {
        error = "There is a preset called \xe2\x80\x9c" + name + "\xe2\x80\x9d already.";
        return false;
    }
    std::string audio = audioFolderOf(path);
    if (!util::isDir(audio)) {
        if (::rename(path.c_str(), newPath.c_str()) != 0) {
            error = "Cannot rename " + path + ": " + strerror(errno);
            return false;
        }
        return true;
    }
    // a track preset with audio: its file names point into "<stem>_audio/", which is renamed with it
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    Json data = Json::parse(text);
    replacePrefix(data, audioPrefix(util::stem(path)), audioPrefix(name));
    if (::rename(audio.c_str(), audioFolderOf(newPath).c_str()) != 0) {
        error = "Cannot rename " + audio + ": " + strerror(errno);
        return false;
    }
    if (!project_io::writeJson(newPath, data, error)) {
        ::rename(audioFolderOf(newPath).c_str(), audio.c_str());  // (back as it was)
        return false;
    }
    util::removeFile(path);
    return true;
}

bool remove(const std::string& path, std::string& error) {
    if (util::fileExists(path) && !util::removeFile(path)) {
        error = "Cannot delete " + path + ": " + strerror(errno);
        return false;
    }
    std::string audio = audioFolderOf(path);
    if (util::isDir(audio) && !removeFolder(audio)) {
        error = "Cannot delete " + audio + ": " + strerror(errno);
        return false;
    }
    return true;
}

// --- effect presets -----------------------------------------------------------------------------

bool saveEffect(const PluginSlot& slot, const std::string& name, std::string& path, std::string& error) {
    std::string file = safeName(name);
    if (file.empty()) {
        error = "A preset needs a name.";
        return false;
    }
    if (slot.isControls()) {
        error = "A track's volume and pan have no presets.";
        return false;
    }
    std::string state = slot.currentState();
    if (state.empty()) {
        error = slot.name + " has no settings to keep (it isn't loaded).";
        return false;
    }
    std::string dir = effectsFolder(slot);
    if (!util::isDir(dir) && !util::makeDirs(dir)) {
        error = "Cannot make the folder " + dir + ": " + strerror(errno);
        return false;
    }
    path = util::joinPath(dir, file + ".json");
    Json data = Json::object({{"format", Json(FORMAT)},
                              {"version", Json(VERSION)},
                              {"kind", Json("effect")},
                              {"name", Json(file)},
                              {"plugin", Json::object({{"path", Json(slot.path)},
                                                       {"name", Json(slot.name)},
                                                       {"plugin_name", Json(slot.pluginName)},
                                                       {"kind", Json(slot.kind)}})},
                              {"state", Json(base64Encode(state))}});
    return project_io::writeJson(path, data, error);
}

bool readEffect(const std::string& path, std::string& state, std::string& plugin, std::string& error) {
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    Json data = Json::parse(text);
    if (!data.isObject() || data.str("format") != FORMAT || data.str("kind") != "effect") {
        error = path + " is not an effect preset.";
        return false;
    }
    plugin = data["plugin"].str("name");
    if (!base64Decode(data.str("state"), state) || state.empty()) {
        error = "The preset " + util::stem(path) + " holds no settings.";
        return false;
    }
    return true;
}

bool applyState(Project& project, const SlotPtr& slot, const std::string& state, std::string& error) {
    auto inst = slot ? aload(slot->instance) : nullptr;
    if (!inst) {
        error = (slot ? slot->name : std::string("The plugin")) + " isn't loaded, so it can't take the settings.";
        return false;
    }
    try {
        plug::setState(*inst, state);
    } catch (const std::exception& e) {
        error = slot->name + " didn't take the settings (" + e.what() + ").";
        return false;
    }
    slot->state = state;
    std::vector<uint32_t> ids;  // (its modulated knobs: their base is the new value)
    for (auto& p : automation::parameters(*slot)) ids.push_back(p.id);
    modulation::userEdited(project, slot.get(), ids);
    return true;
}

bool applyEffect(Project& project, const SlotPtr& slot, const std::string& path, std::string& error) {
    std::string state, plugin;
    return readEffect(path, state, plugin, error) && applyState(project, slot, state, error);
}

// --- track presets -------------------------------------------------------------------------------

bool saveTrack(const Track& track, int sr, const std::string& name, std::string& path, std::string& error) {
    std::string file = safeName(name);
    if (file.empty()) {
        error = "A preset needs a name.";
        return false;
    }
    std::string dir = tracksFolder();
    if (!util::isDir(dir) && !util::makeDirs(dir)) {
        error = "Cannot make the folder " + dir + ": " + strerror(errno);
        return false;
    }
    path = util::joinPath(dir, file + ".json");
    // written under a temporary name first (".saving <name>": no preset's name starts with a dot), so an
    // older preset of that name is only replaced once the new one is complete
    std::string temp = util::joinPath(dir, ".saving " + file + ".json");
    remove(temp, error);  // (a leftover of an interrupted save)
    error.clear();
    // the chain on a track of its own (the same plugins: only read), as a one-track project
    Project holder;
    auto t = std::make_shared<Track>(track.name, track.kind);
    t->instrument = track.instrument;
    t->needsInstrument = track.needsInstrument;
    t->usesRack = track.usesRack;
    t->rack = track.rack;
    t->rackCables = track.rackCables;
    t->effects = track.effects;
    t->cables = track.cables;    // (from other tracks: left out, they aren't in this project)
    t->modCables = track.modCables;
    t->midiEffects = track.midiEffects;
    t->synth = std::atomic_load(&track.synth);  // (the built-in synth's patch)
    if (track.sampler) {  // (a copy of its sample's clip: saving points that one at the preset's audio)
        auto s = std::make_shared<SamplerSettings>(*track.sampler);
        if (s->sample) s->sample = copyClip(s->sample);
        t->sampler = s;
    }
    holder.tracks = std::vector<TrackPtr>{t};
    auto failed = [&] {
        std::string ignored;
        remove(temp, ignored);
        return false;
    };
    if (!project_io::saveProject(temp, holder, sr, project_io::MasterSettings(), error)) return failed();
    std::string text;
    Json data;
    if (util::readFile(temp, text)) data = Json::parse(text);
    if (!data.isObject()) {
        error = "Cannot read back " + temp;
        return failed();
    }
    data.set("preset", Json(TRACK));
    data.set("preset_version", Json(VERSION));
    replacePrefix(data, audioPrefix(util::stem(temp)), audioPrefix(file));  // (its audio folder, renamed below)
    std::string tempAudio = audioFolderOf(temp), audio = audioFolderOf(path);
    if (util::isDir(tempAudio) && util::listDir(tempAudio).empty()) ::rmdir(tempAudio.c_str());  // (no sample: no folder)
    if (!remove(path, error)) return failed();  // (the older one of that name, its audio too)
    if (util::isDir(tempAudio) && ::rename(tempAudio.c_str(), audio.c_str()) != 0) {
        error = "Cannot rename " + tempAudio + ": " + strerror(errno);
        return failed();
    }
    if (!project_io::writeJson(path, data, error)) {
        std::string ignored;
        remove(path, ignored);
        return failed();
    }
    util::removeFile(temp);
    return true;
}

std::vector<SlotPtr> TrackPreset::slots() const {
    std::vector<SlotPtr> out;
    if (!chain) return out;
    if (chain->instrument) out.push_back(chain->instrument);
    for (auto& s : chain->rack) out.push_back(s);
    for (auto& s : chain->effects) out.push_back(s);
    return out;
}

bool readTrack(const std::string& path, int sr, TrackPreset& out, std::string& error) {
    if (!isTrackPresetFile(path)) {
        error = path + " is not a track preset.";
        return false;
    }
    Project holder;
    project_io::MasterSettings master;
    if (!project_io::loadProject(path, holder, sr, master, error)) return false;
    if (holder.tracks.empty()) {
        error = "The track preset " + util::stem(path) + " is empty.";
        return false;
    }
    out.name = util::stem(path);
    out.chain = holder.tracks[0];
    out.kind = out.chain->kind;
    return true;
}

Applied applyTrack(Project& project, const TrackPtr& track, TrackPreset& preset, int sr) {
    Applied done;
    const TrackPtr& c = preset.chain;
    if (!track || !c) return done;
    bool instrumentPart = c->kind == Kind::Midi && track->kind == Kind::Midi;
    done.instrumentSkipped = c->kind == Kind::Midi && track->kind != Kind::Midi &&
                             (c->instrument || c->usesRack || c->sampler || !c->midiEffects.empty());
    std::vector<SlotPtr> before;
    if (track->instrument) before.push_back(track->instrument);
    for (auto& s : track->rack) before.push_back(s);
    for (auto& s : track->effects) before.push_back(s);

    // the old modulation first (knobs back to their base), then the cables (a cable to an effect not in the
    // list is never used: the audio thread sees old or new, never a cable to an effect that is gone)
    for (auto& m : track->modCables) modulation::release(m);
    track->modCables = std::vector<ModCable>{};
    track->cables = c->cables;
    track->effects = c->effects;
    if (instrumentPart) {
        track->rackCables = std::vector<Cable>{};
        track->rack = c->rack;
        track->rackCables = c->rackCables;
        std::atomic_store(&track->sampler, SamplerPtr(c->sampler));
        std::atomic_store(&track->synth, subsynth::PatchPtr(c->synth));
        std::atomic_store(&track->instrument, SlotPtr(c->instrument));
        track->usesRack = c->usesRack;
        track->needsInstrument = c->needsInstrument && !c->instrument && !c->usesRack && !c->sampler;
        track->midiEffects = c->midiEffects;
    }
    // modulation cables whose ends are both this track's now (a cable to the preset's instrument stays out
    // when the instrument wasn't taken)
    std::vector<ModCable> mods;
    std::set<const PluginSlot*> mine;
    for (auto& s : track->effects) mine.insert(s.get());
    for (auto& s : track->rack) mine.insert(s.get());
    if (track->instrument) mine.insert(track->instrument.get());
    for (auto& m : c->modCables)
        if (mine.count(m.from) && mine.count(m.to)) mods.push_back(m);
    track->modCables = mods;

    std::set<const PluginSlot*> now;
    if (track->instrument) now.insert(track->instrument.get());
    for (auto& s : track->rack) now.insert(s.get());
    for (auto& s : track->effects) now.insert(s.get());
    for (auto& s : before)
        if (!now.count(s.get())) {
            done.removed.push_back(s);
            if (auto inst = aload(s->instance)) project.retire(inst);  // (never destroyed under the audio thread)
            forgetCompare(s.get());
        }
    automation::prune(*track);
    patch::ensureStates(project);
    automation::resolve(project);
    modulation::resolve(*track);
    if (instrumentPart) {  // (the built-in synth's or sampler's rendering, as it now plays)
        for (const ClipPtr& clip : project.clipsOn(track)) clip->prepare(sr);
        for (const ClipPtr& take : track->takes)
            if (take->kind == Kind::Midi) take->prepare(sr);
    }
    return done;
}

// --- compare (A/B) -------------------------------------------------------------------------------

Compare* compareOf(const PluginSlot* slot) {
    auto& all = compares();
    auto it = all.find(slot);
    if (it == all.end()) return nullptr;
    if (it->second.slot.expired()) {  // (a slot gone; its address may be another's now)
        all.erase(it);
        return nullptr;
    }
    return &it->second.compare;
}

void forgetCompare(const PluginSlot* slot) { compares().erase(slot); }

bool keep(Project& project, const SlotPtr& slot, std::string& error) {
    (void)project;
    if (!slot || !aload(slot->instance)) {
        error = (slot ? slot->name : std::string("The plugin")) + " isn't loaded.";
        return false;
    }
    auto& all = compares();
    for (auto it = all.begin(); it != all.end();)  // (the slots gone)
        it = it->second.slot.expired() ? all.erase(it) : std::next(it);
    Kept& k = all[slot.get()];
    if (k.slot.lock() != slot) k = Kept{slot, Compare{}};
    if (k.compare.hasKept && k.compare.onKept) return true;  // (KEPT is what it has now: kept as it is)
    k.compare.hasKept = true;
    k.compare.onKept = false;
    k.compare.other = slot->currentState();
    k.compare.otherValues = valuesOf(*slot);
    return true;
}

bool switchTo(Project& project, const SlotPtr& slot, bool kept, std::string& error) {
    Compare* c = slot ? compareOf(slot.get()) : nullptr;
    if (!c || !c->hasKept) {
        error = "Nothing is kept to compare with: choose \xe2\x80\x9cKeep this\xe2\x80\x9d first.";
        return false;
    }
    if (c->onKept == kept) return false;
    std::string leaving = slot->currentState();
    std::vector<double> leavingValues = valuesOf(*slot);
    if (!applyState(project, slot, c->other, error)) return false;
    c->other = leaving;
    c->otherValues = leavingValues;
    c->onKept = kept;
    return true;
}

std::vector<int> differing(const PluginSlot& slot) {
    std::vector<int> out;
    Compare* c = compareOf(&slot);
    if (!c || !c->hasKept) return out;
    std::vector<double> now = valuesOf(slot);
    for (size_t i = 0; i < now.size() && i < c->otherValues.size(); i++)
        if (std::fabs(now[i] - c->otherValues[i]) > 1e-6) out.push_back((int)i);
    return out;
}

}  // namespace presets
