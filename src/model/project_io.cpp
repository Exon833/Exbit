#include "model/project_io.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <cstdio>

#include "audio/file_io.h"
#include "audio/md5.h"
#include "audio/sound_file.h"
#include "core/util.h"

namespace project_io {

namespace {

Json nullOr(const std::string& s) { return s.empty() ? Json() : Json(s); }
std::string strOf(const Json& j) { return j.isString() ? j.toString() : std::string(); }

// a whole number (Python's isinstance(x, int) for what json.load gives)
bool isIndex(const Json& j, size_t size, size_t& out) {
    if (!j.isNumber()) return false;
    double v = j.toDouble();
    if (!(v >= 0) || v != std::floor(v) || v >= (double)size) return false;
    out = (size_t)v;
    return true;
}

// a number written as Python would: an int if it's whole (sliders give ints), else a float
Json wholeOrFloat(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json((long long)v);
    return Json(v);
}

int64_t roundFrames(double seconds, int sr) {  // Python's round(): halves to even
    return (int64_t)std::nearbyint(seconds * sr);
}

struct Damaged {
    std::string what;
};

const Json& need(const Json& obj, const char* key) {
    if (!obj.isObject() || !obj.has(key)) throw Damaged{std::string("'") + key + "' is missing"};
    return obj[key];
}

const Json& field(const Json& obj, const char* key) {
    static const Json none;
    return obj.isObject() ? obj[key] : none;
}

// the name of a cable end, or null if it can't be named
Json endName(const End& e, const std::vector<SlotPtr>& effects, const std::vector<TrackPtr>& tracks) {
    auto slotIndex = [&](const PluginSlot* s) -> int {
        for (size_t i = 0; i < effects.size(); i++)
            if (effects[i].get() == s) return (int)i;
        return -1;
    };
    switch (e.type) {
        case End::In: return Json("in");
        case End::Out: return Json("out");
        case End::Slot: {
            int i = slotIndex(e.slot);
            return i < 0 ? Json() : Json(i);
        }
        case End::Sidechain: {
            int i = slotIndex(e.slot);
            return i < 0 ? Json() : Json::array({Json("side"), Json(i)});
        }
        case End::TrackSource:
            for (size_t i = 0; i < tracks.size(); i++)
                if (tracks[i].get() == e.track) return Json::array({Json("track"), Json((int)i)});
            return Json();
    }
    return Json();
}

// one end of a cable read back: `from` / `to` say which side of the cable it is on
bool readEnd(const Json& name, bool from, bool to, const std::vector<SlotPtr>& effects,
             const std::vector<TrackPtr>& tracks, End& out) {
    size_t i;
    if (name.isArray() && name.size() == 2 && name[size_t(0)].isString() && name[size_t(0)].toString() == "track") {
        // (only ever where a cable comes from)
        if (!from || !isIndex(name[size_t(1)], tracks.size(), i)) return false;
        out = End::source(tracks[i].get());
        return true;
    }
    if (name.isArray()) {  // ["side", n]: only ever where a cable goes to
        if (!(name.size() == 2 && name[size_t(0)].isString() && name[size_t(0)].toString() == "side" && to)) return false;
        End target;
        if (!readEnd(name[size_t(1)], false, false, effects, tracks, target) || target.type != End::Slot) return false;
        out = End::sidechain(target.slot);
        return true;
    }
    if (name.isString()) {
        if (from && name.toString() == "in") { out = End::in(); return true; }
        if (to && name.toString() == "out") { out = End::out(); return true; }
        return false;
    }
    if (isIndex(name, effects.size(), i)) {
        out = End::of(effects[i].get());
        return true;
    }
    return false;
}


// os.path.normpath of a relative path ("../." -> "..", "a/../.." -> "..")
std::string normRelative(const std::string& p) {
    if (!p.empty() && p[0] == '/') return util::absolutePath(p);
    std::vector<std::string> parts;
    for (auto& part : util::split(p, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(part);
    }
    std::string out;
    for (auto& part : parts) out += (out.empty() ? "" : "/") + part;
    return out.empty() ? "." : out;
}

// --- notes: [start, end, pitch, velocity], as the Python program wrote them. A note whose own properties
// (Note::chance, repeats, slide, pan, fine) aren't all their defaults gets a fifth item: an object holding
// only the ones that aren't ({"chance": 50, "slide": true}). Unchanged notes are written as they always
// were, and a project without such notes opens anywhere it did. ---

Json noteProperties(const Note& n) {
    Json props = Json::object();
    if (n.chance < 100) props.set("chance", wholeOrFloat(n.chance));
    if (n.repeats > 1) props.set("repeats", Json(n.repeats));
    if (n.slide) props.set("slide", Json(true));
    if (n.pan != 0) props.set("pan", Json(n.pan));
    if (n.fine != 0) props.set("fine", wholeOrFloat(n.fine));
    return props;
}

// the properties of a note's fifth item (anything missing, or not a number: the default), kept in range
void readNoteProperties(const Json& props, Note& n) {
    auto clamped = [&](const char* key, double low, double high, double def) {
        double v = props.num(key, def);
        return std::isfinite(v) ? std::min(high, std::max(low, v)) : def;
    };
    n.chance = clamped("chance", 0, 100, 100);
    n.repeats = (int)std::lround(clamped("repeats", 1, note_props::MAX_REPEATS, 1));
    n.slide = props.flag("slide", false);
    n.pan = clamped("pan", -1, 1, 0);
    n.fine = clamped("fine", -100, 100, 0);
}

Json notesJson(const std::shared_ptr<const std::vector<Note>>& notes) {
    Json out = Json::array();
    if (notes)
        for (auto& n : *notes) {
            Json entry = Json::array({Json(n.start), Json(n.end), Json(n.pitch), Json(n.velocity)});
            if (!n.plain()) entry.push(noteProperties(n));
            out.push(std::move(entry));
        }
    return out;
}

std::shared_ptr<const std::vector<Note>> readNotes(const Json& entries, bool strict) {
    std::vector<Note> notes;
    for (const Json& n : entries.items()) {
        bool props = n.isArray() && n.size() == 5 && n[size_t(4)].isObject();
        if (!n.isArray() || (n.size() != 4 && !props)) {
            if (strict) throw Damaged{"a note is not [start, end, pitch, velocity] (and its properties)"};
            continue;
        }
        notes.push_back(Note{n[size_t(0)].toDouble(), n[size_t(1)].toDouble(), n[size_t(2)].toDouble(),
                             n[size_t(3)].toDouble()});
        if (props) readNoteProperties(n[size_t(4)], notes.back());
    }
    return std::make_shared<const std::vector<Note>>(std::move(notes));
}

// a MIDI clip whose rendering failed opens silent, so the project still opens (its notes are kept)
void prepareSafely(Clip& clip, int sr) {
    try {
        clip.prepare(sr);
    } catch (const std::exception& e) {
        if (clip.kind != Kind::Midi) throw;
        fprintf(stderr, "Tiny DAW: %s couldn't be rendered (%s); it is silent until changed\n", clip.name.c_str(),
                e.what());
        clip.played = clip.notes;
        auto silent = std::make_shared<AudioBuffer>();
        silent->samples.assign((size_t)std::max<int64_t>(1, (int64_t)std::nearbyint(clip.lengthS * sr)) * 2, 0.0f);
        clip.data = silent;
        auto ev = std::make_shared<std::vector<Clip::Event>>();
        if (clip.notes)
            for (auto& n : *clip.notes)
                ev->push_back({(int64_t)std::nearbyint(n.start * sr), (int64_t)std::nearbyint(n.end * sr), (int)n.pitch,
                               (int)n.velocity});
        clip.events = ev;
    }
    clip.fitWindow();
}

Json describeAutomation(const Track& track, const Automation& lane) {
    Json plugin;
    if (lane.slot && lane.slot == track.controls) {
        plugin = Json("track");
    } else if (track.instrument && lane.slot == track.instrument) {
        plugin = Json("instrument");
    } else {
        int rack = track.rack.indexOf(lane.slot);
        if (rack >= 0) {
            plugin = Json::array({Json("rack"), Json(rack)});
        } else {
            int fx = track.effects.indexOf(lane.slot);
            if (fx < 0) return Json();  // (its plugin is gone)
            plugin = Json(fx);
        }
    }
    Json points = Json::array();
    for (auto& p : lane.points) points.push(Json::array({Json(p.t), Json(p.v)}));
    return Json::object({{"plugin", plugin}, {"param", Json(lane.param)}, {"height", Json(lane.height)},
                         {"points", points}});
}

std::vector<AutomationPtr> readAutomation(const Track& track, const Json& entries) {
    std::vector<AutomationPtr> lanes;
    for (const Json& entry : entries.items()) {
        if (!entry.isObject()) continue;
        const Json& plugin = entry["plugin"];
        SlotPtr slot;
        size_t i;
        if (plugin.isString() && plugin.toString() == "track") {
            slot = track.controls;
        } else if (plugin.isArray() && plugin.size() == 2 && plugin[size_t(0)].isString() &&
                   plugin[size_t(0)].toString() == "rack") {
            if (isIndex(plugin[size_t(1)], track.rack.size(), i)) slot = track.rack[i];
        } else if (plugin.isString() && plugin.toString() == "instrument") {
            slot = track.instrument;
        } else if (isIndex(plugin, track.effects.size(), i)) {
            slot = track.effects[i];
        }
        std::string param = entry.str("param");
        if (!slot || param.empty()) continue;
        auto lane = std::make_shared<Automation>();
        lane->slot = slot;
        lane->param = param;
        lane->height = entry.integer("height", config::AUTOMATION_H);
        std::vector<AutoPoint> pts;
        for (const Json& p : entry["points"].items())
            if (p.isArray() && p.size() == 2 && p[size_t(0)].isNumber() && p[size_t(1)].isNumber())
                pts.push_back({p[size_t(0)].toDouble(), p[size_t(1)].toDouble()});
        lane->setPoints(std::move(pts));
        lanes.push_back(lane);
    }
    return lanes;
}

Json describeSampler(const SamplerSettings& s, const AudioFileFn& audioFile) {
    return Json::object({
        {"file", Json(s.sample ? audioFile(*s.sample) : std::string())},
        {"name", Json(s.sample ? s.sample->name : std::string())},
        {"root", Json(s.root)},
        {"transpose", Json(s.transpose)},
        {"gain_db", Json(s.gainDb)},
        {"velocity", Json(s.velocity)},
        {"attack", Json(s.attack)},
        {"decay", Json(s.decay)},
        {"sustain", Json(s.sustain)},
        {"release", Json(s.release)},
        {"one_shot", Json(s.oneShot)},
        {"loop", Json(s.loop)},
        {"start", Json(s.start)},
        {"end", Json(s.end)},
    });
}

Json describeTake(const Clip& take, int sr, const AudioFileFn& audioFile) {
    Json entry = Json::object({{"kind", Json(kindName(take.kind))},
                               {"name", Json(take.name)},
                               {"start", Json((double)take.start / sr)},
                               {"offset", Json((double)take.offset / sr)},
                               {"length", take.length < 0 ? Json() : Json((double)take.length / sr)}});
    if (take.kind == Kind::Audio) {
        entry.set("file", Json(audioFile(take)));
    } else {
        entry.set("notes", notesJson(take.notes));
        entry.set("midi_length", Json(take.lengthS));
    }
    return entry;
}

// A track's modulation cables (model.h ModCable): [{"from": <effect index>, "to": <effect index> | "instrument" |
// {"rack": <index>}, "param": <name>, "depth": d, "base": b}]; cables whose ends aren't the track's are left out.
static Json describeModulation(const Track& track) {
    auto indexIn = [](const List<SlotPtr>& slots, const PluginSlot* s) {
        for (size_t i = 0; i < slots.size(); i++)
            if (slots[i].get() == s) return (int)i;
        return -1;
    };
    Json out = Json::array();
    for (auto& c : track.modCables) {
        int from = indexIn(track.effects, c.from);
        if (from < 0) continue;
        Json to;
        int e = indexIn(track.effects, c.to), r = indexIn(track.rack, c.to);
        if (e >= 0) to = Json(e);
        else if (track.instrument && track.instrument.get() == c.to) to = Json("instrument");
        else if (r >= 0) to = Json::object({{"rack", Json(r)}});
        else continue;
        double base = c.state ? c.state->base.load() : -1;
        out.push(Json::object({{"from", Json(from)}, {"to", to}, {"param", Json(c.param)}, {"depth", Json(c.depth)},
                               {"base", base >= 0 ? Json(base) : Json()}}));
    }
    return out;
}

static std::vector<ModCable> readModulation(const Track& track, const Json& entries) {
    std::vector<ModCable> out;
    if (!entries.isArray()) return out;
    for (const Json& m : entries.items()) {
        if (!m.isObject()) continue;
        size_t from;
        if (!isIndex(m["from"], track.effects.size(), from)) continue;
        ModCable c;
        c.from = track.effects[from].get();
        const Json& to = m["to"];
        size_t i;
        if (isIndex(to, track.effects.size(), i)) c.to = track.effects[i].get();
        else if (to.isString() && strOf(to) == "instrument" && track.instrument) c.to = track.instrument.get();
        else if (to.isObject() && isIndex(to["rack"], track.rack.size(), i)) c.to = track.rack[i].get();
        if (!c.to || c.to == c.from) continue;
        c.param = m.str("param");
        c.depth = std::max(-1.0, std::min(1.0, m.num("depth", 0.5)));
        double base = m.num("base", -1);
        if (base >= 0 && base <= 1) c.state->base = base;
        if (!c.param.empty()) out.push_back(c);
    }
    return out;
}

Json describeTrack(const Track& track, const std::vector<GroupPtr>& groups, int sr, const AudioFileFn& audioFile,
                   bool states, const std::vector<TrackPtr>& tracks) {
    Json group;
    for (size_t i = 0; i < groups.size(); i++)
        if (groups[i] == track.group) group = Json((int)i);
    Json frozen;
    if (track.frozen)
        frozen = Json::object({{"file", Json(audioFile(*track.frozen))},
                               {"start", Json((double)track.frozen->start / sr)}});
    Json effects = Json::array(), rack = Json::array(), automation = Json::array(), sends = Json::array(),
         takes = Json::array();
    for (auto& s : track.effects) effects.push(describeSlot(*s, states));
    for (auto& s : track.rack) rack.push(describeSlot(*s, states));
    for (auto& a : track.automation) {
        Json lane = describeAutomation(track, *a);
        if (!lane.isNull()) automation.push(lane);
    }
    Json preFader = Json::array();  // (pre-fader sends: the returns', by index; the key only when there are any)
    for (auto& send : track.sends)
        for (size_t i = 0; i < tracks.size(); i++)
            if (tracks[i] == send.track) {
                sends.push(Json::array({Json((int)i), Json(send.db)}));
                if (send.preFader) preFader.push(Json((int)i));
                break;
            }
    for (auto& take : track.takes) takes.push(describeTake(*take, sr, audioFile));
    Json entry = Json::object({
        {"name", Json(track.name)},
        {"kind", Json(kindName(track.kind))},
        {"muted", Json(track.muted)},
        {"soloed", Json(track.soloed)},
        {"output", Json(track.output)},
        {"volume_db", Json(track.volumeDb)},
        {"pan", Json(track.pan)},
        {"delay_ms", Json(track.delayMs)},
        {"height", Json(track.height)},
        {"color", nullOr(track.color)},
        {"tone", nullOr(track.tone)},
        {"needs_instrument", Json(track.needsInstrument)},
        {"show_automation", Json(track.showAutomation)},
        {"uses_rack", Json(track.usesRack)},
        {"group", group},
        {"frozen", frozen},
        {"instrument", track.instrument ? describeSlot(*track.instrument, states) : Json()},
        {"effects", effects},
        {"cables", describeCables(*track.cables, *track.effects, tracks)},
        {"rack", rack},
        {"rack_cables", describeCables(*track.rackCables, *track.rack, {})},
        {"automation", automation},
        {"sends", sends},
        {"sampler", track.sampler ? describeSampler(*track.sampler, audioFile) : Json()},
        {"midi_effects", describeMidiEffects(*track.midiEffects)},
        {"takes", takes},
        {"show_takes", Json(track.showTakes)},
    });
    // pre-fader sends: a key of their own (older versions read `sends` as they always did: after the fader)
    if (preFader.size()) entry.set("pre_fader_sends", preFader);
    if (!track.modCables.empty()) entry.set("modulation", describeModulation(track));  // (only then: older files unchanged)
    return entry;
}

// os.path.splitext(name)[0]
std::string splitextRoot(const std::string& name) {
    size_t sep = name.find_last_of('/');
    size_t baseStart = sep == std::string::npos ? 0 : sep + 1;
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot < baseStart) return name;
    // leading dots of the file name don't start an extension
    size_t firstNonDot = name.find_first_not_of('.', baseStart);
    if (firstNonDot == std::string::npos || firstNonDot > dot) return name;
    return name.substr(0, dot);
}

// Python's \w for one code point: ASCII letters, digits and "_", and (roughly) the letters and digits
// of other scripts; the common punctuation and symbol blocks are not word characters.
bool isWordChar(uint32_t cp) {
    if (cp < 0x80) return std::isalnum((int)cp) || cp == '_';
    if (cp <= 0xBF) return cp == 0xAA || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 || cp == 0xBA;
    if (cp == 0xD7 || cp == 0xF7) return false;
    if (cp >= 0x2000 && cp <= 0x206F) return false;  // general punctuation (dashes, quotes, spaces…)
    if (cp >= 0x20A0 && cp <= 0x2BFF) return false;  // currency, arrows, maths, technical, box drawing, symbols
    if (cp >= 0x3000 && cp <= 0x3003) return false;  // CJK punctuation
    if (cp >= 0x3008 && cp <= 0x3020) return false;
    if (cp >= 0xFF01 && cp <= 0xFF0F) return false;  // fullwidth punctuation
    if (cp >= 0xFE10 && cp <= 0xFE6F) return false;
    return true;
}

// the next UTF-8 code point of s at i (bytes that aren't UTF-8: taken one by one)
uint32_t nextCodePoint(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + len > s.size()) len = 1;
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < len; k++) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { len = 1; cp = c; break; }
        cp = (cp << 6) | (d & 0x3F);
    }
    i += len;
    return cp;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Json MasterSettings::toJson() const {
    return Json::object({{"volume_db", wholeOrFloat(volumeDb)}, {"mute", Json(mute)}});
}

MasterSettings MasterSettings::fromJson(const Json& j) {
    MasterSettings m;
    if (j.isObject()) {
        m.volumeDb = j.num("volume_db", 0);
        m.mute = j["mute"].isBool() ? j["mute"].toBool() : j.num("mute", 0) != 0;
    }
    return m;
}

Json describeSlot(const PluginSlot& slot, bool states) {
    std::string state = states ? slot.currentState() : std::string();
    Json entry = Json::object({
        {"path", Json(slot.path)},
        {"name", Json(slot.name)},
        {"kind", Json(slot.kind)},
        {"plugin_name", nullOr(slot.pluginName)},
        {"bypass", Json(slot.bypass)},
        {"volume_db", Json(slot.volumeDb)},
        {"mix", Json(slot.mix)},
        {"state", state.empty() ? Json() : Json(base64Encode(state))},
    });
    if (slot.placed)
        entry.set("at", Json::array({Json((long long)std::lround(slot.atX)), Json((long long)std::lround(slot.atY))}));
    return entry;
}

SlotPtr readSlot(const Json& entry) {
    auto slot = std::make_shared<PluginSlot>();
    slot->path = entry.str("path");
    slot->name = entry.str("name");
    slot->kind = entry.str("kind");
    slot->pluginName = entry.str("plugin_name");
    slot->bypass = entry.flag("bypass", false);
    slot->volumeDb = entry.num("volume_db", 0.0);
    slot->mix = entry.num("mix", 1.0);
    std::string state = entry.str("state");
    if (!state.empty()) {
        std::string bytes;
        if (base64Decode(state, bytes)) slot->state = bytes;
    }
    const Json& at = entry["at"];
    if (at.isArray() && at.size() == 2 && at[size_t(0)].isNumber() && at[size_t(1)].isNumber()) {
        slot->placed = true;
        slot->atX = std::trunc(at[size_t(0)].toDouble());  // (int(), as the Python program reads it)
        slot->atY = std::trunc(at[size_t(1)].toDouble());
    }
    return slot;
}

Json describeCables(const std::vector<Cable>& cables, const std::vector<SlotPtr>& effects,
                    const std::vector<TrackPtr>& tracks) {
    Json out = Json::array();
    for (auto& c : cables) {
        Json a = endName(c.first, effects, tracks), b = endName(c.second, effects, tracks);
        if (a.isNull() || b.isNull()) continue;  // (from a track that is gone, or an effect not in the list)
        out.push(Json::array({a, b}));
    }
    return out;
}

std::vector<Cable> readCables(const Json& entries, const std::vector<SlotPtr>& effects,
                              const std::vector<TrackPtr>& tracks) {
    if (entries.isNull()) return chain(effects);
    std::vector<Cable> cables;
    for (const Json& entry : entries.items()) {
        if (!entry.isArray() || entry.size() != 2) continue;
        End a, b;
        if (!readEnd(entry[size_t(0)], true, false, effects, tracks, a) || !readEnd(entry[size_t(1)], false, true, effects, tracks, b))
            continue;
        if (a == b) continue;
        if (std::find(cables.begin(), cables.end(), Cable(a, b)) != cables.end()) continue;
        if (a.type == End::Slot && b.type == End::Sidechain && b.slot == a.slot) continue;  // into its own sidechain
        if (a.type == End::TrackSource && b.type != End::Sidechain) continue;  // other tracks only feed sidechains
        cables.emplace_back(a, b);
    }
    return cables;
}

Json describeMidiEffects(const std::vector<MidiEffect>& effects) {
    Json out = Json::array();
    for (auto& e : effects) {
        Json values = Json::object();
        bool known = false;
        for (auto& k : MidiEffect::kinds())  // (in the order of its parameters)
            if (k.first == e.kind) {
                known = true;
                for (auto& p : k.second) values.set(p.key, Json(e.get(p.key)));
            }
        if (!known)
            for (auto& v : e.values) values.set(v.first, Json(v.second));
        out.push(Json::object({{"kind", Json(e.kind)}, {"values", values}}));
    }
    return out;
}

std::vector<MidiEffect> readMidiEffects(const Json& entries) {
    std::vector<MidiEffect> out;
    for (const Json& e : entries.items()) {
        if (!e.isObject()) continue;
        std::map<std::string, double> values;
        for (auto& m : e["values"].members())
            if (m.second.isNumber() || m.second.isBool())
                values[m.first] = m.second.isBool() ? (m.second.toBool() ? 1.0 : 0.0) : m.second.toDouble();
        MidiEffect effect;
        if (MidiEffect::make(e.str("kind"), values, effect)) out.push_back(effect);
    }
    return out;
}

const std::vector<std::string>& scaleNames() {
    static const std::vector<std::string> names = {"Major", "Minor", "Dorian", "Mixolydian", "Harmonic minor",
                                                   "Major pentatonic", "Minor pentatonic", "Blues"};
    return names;
}

Json describeTiming(const Timing& t) {
    Json changes = Json::array();
    for (auto& c : t.changes) changes.push(Json::array({Json(c.bar), Json(c.bpm), Json(c.beatsPerBar)}));
    return Json::object({{"bpm", Json(t.bpm)},
                         {"beats_per_bar", Json(t.beatsPerBar)},
                         {"beats", Json(t.beats)},
                         {"thirds", Json(t.thirds)},
                         {"loop_start", Json(t.loopStart)},
                         {"loop_end", Json(t.loopEnd)},
                         {"looping", Json(t.looping)},
                         {"changes", changes},
                         {"grid", Json(t.grid)},
                         {"snap_on", Json(t.snapOn)},
                         {"key", Json(t.key)},
                         {"scale", Json(t.scale)}});
}

Timing readTiming(const Json& entry) {
    Timing t;
    if (entry.isObject()) {
        t.bpm = entry.num("bpm", t.bpm);
        t.beatsPerBar = entry.integer("beats_per_bar", t.beatsPerBar);
        t.beats = entry.flag("beats", t.beats);
        t.thirds = entry.flag("thirds", t.thirds);
        t.loopStart = entry.num("loop_start", t.loopStart);
        t.loopEnd = entry.num("loop_end", t.loopEnd);
        t.looping = entry.flag("looping", t.looping);
        const Json& snap = entry["snap_on"];
        t.snapOn = snap.isBool() ? snap.toBool() : snap.isNumber() ? snap.toDouble() != 0 : t.snapOn;
        const Json& grid = entry["grid"];
        if (grid.isNumber()) t.grid = grid.toDouble();
        std::vector<TempoChange> changes;
        for (const Json& c : entry["changes"].items()) {
            if (!c.isArray() || c.size() != 3 || !c[size_t(0)].isNumber() || !c[size_t(1)].isNumber() ||
                !c[size_t(2)].isNumber())
                continue;
            TempoChange change{(int)c[size_t(0)].toDouble(), c[size_t(1)].toDouble(), (int)c[size_t(2)].toDouble()};
            if (change.bar >= 2 && config::MIN_BPM <= change.bpm && change.bpm <= config::MAX_BPM &&
                1 <= change.beatsPerBar && change.beatsPerBar <= config::MAX_BEATS_PER_BAR)
                changes.push_back(change);
        }
        std::sort(changes.begin(), changes.end());
        t.changes = changes;
        const Json& key = entry["key"];
        const Json& scale = entry["scale"];
        bool keyOk = key.isNull() || (key.isNumber() && key.toDouble() == std::floor(key.toDouble()));
        bool scaleOk = scale.isNull() || scale.isString();
        if (keyOk && key.isNumber()) t.key = (int)key.toDouble();
        if (scaleOk && scale.isString()) t.scale = scale.toString();
        const auto& names = scaleNames();
        if (!keyOk || !scaleOk || t.key < 0 || t.key > 11 ||
            (!t.scale.empty() && std::find(names.begin(), names.end(), t.scale) == names.end())) {
            t.key = 0;  // (no scale, or one this version doesn't know)
            t.scale.clear();
        }
    }
    if (!(config::MIN_BPM <= t.bpm && t.bpm <= config::MAX_BPM)) t.bpm = config::DEFAULT_BPM;
    if (!(1 <= t.beatsPerBar && t.beatsPerBar <= config::MAX_BEATS_PER_BAR))
        t.beatsPerBar = config::DEFAULT_BEATS_PER_BAR;
    double s, e;
    if (t.loopStart < 0 || !t.loopRange(s, e)) {
        t.loopStart = t.loopEnd = 0.0;
        t.looping = false;
    }
    if (!(0 <= t.grid && t.grid <= 8)) t.grid = 0.0;
    t.publish();
    return t;
}

Json describe(const Project& project, int sr, const MasterSettings& master, AudioFileFn audioFile, bool states) {
    if (!audioFile)
        audioFile = [](const Clip& clip) { return util::format("%p", (const void*)clip.source.get()); };
    std::vector<GroupPtr> groups = project.groups();
    std::vector<TrackPtr> tracks = *project.tracks;
    Json clips = Json::array();
    // linked MIDI clips (model.h): a "link" number per clip sharing its link with another one saved
    std::map<const ClipLink*, int> linkSize, linkNumber;
    for (auto& clip : project.clips)
        if (clip->link && clip->kind == Kind::Midi && project.tracks.indexOf(clip->track) >= 0)
            linkSize[clip->link.get()]++;
    for (auto& clip : project.clips) {
        int row = project.tracks.indexOf(clip->track);
        if (row < 0) continue;  // (on no track of the project)
        Json entry = Json::object({
            {"kind", Json(kindName(clip->kind))},
            {"name", Json(clip->name)},
            {"track", Json(row)},
            {"start", Json((double)clip->start / sr)},
            {"offset", Json((double)clip->offset / sr)},
            {"length", clip->length < 0 ? Json() : Json((double)clip->length / sr)},
            {"color", nullOr(clip->color)},
            {"tone", nullOr(clip->tone)},
            {"disabled", Json(clip->disabled)},
        });
        if (clip->kind != Kind::Midi) {
            entry.set("file", Json(audioFile(*clip)));
            entry.set("gain_db", Json(clip->gainDb));
            entry.set("fade_in", Json((double)clip->fadeIn / sr));
            entry.set("fade_out", Json((double)clip->fadeOut / sr));
            entry.set("stretch", Json(clip->stretch));
            entry.set("pitch", Json(clip->pitch));
            entry.set("source_bpm", clip->sourceBpm > 0 ? Json(clip->sourceBpm) : Json());
            entry.set("reverse", Json(clip->reverse));
            entry.set("loop", Json(clip->loop));
            entry.set("loop_start", Json(clip->loopStart));
            entry.set("loop_length", Json(clip->loopLength));
            // fade shapes (model.h FADE SHAPES): keys only for the curved ones, so straight fades save as before
            if (clip->fadeInShape != FadeShape::Linear) entry.set("fade_in_shape", Json(fadeShapeName(clip->fadeInShape)));
            if (clip->fadeOutShape != FadeShape::Linear)
                entry.set("fade_out_shape", Json(fadeShapeName(clip->fadeOutShape)));
            if (clip->fadeLead || clip->fadeTail) {  // (a piece of a faded clip: where its fades' ramps began / end)
                entry.set("fade_lead", Json((double)clip->fadeLead / sr));
                entry.set("fade_tail", Json((double)clip->fadeTail / sr));
            }
        } else {
            entry.set("notes", notesJson(clip->notes));  // [start s, end s, pitch, velocity]
            entry.set("midi_length", Json(clip->lengthS));
            entry.set("muted_notes", notesJson(clip->mutedNotes));
            entry.set("loop", Json(clip->loop));
            if (clip->link && linkSize[clip->link.get()] >= 2) {  // (only then: unlinked projects save as before)
                auto made = linkNumber.emplace(clip->link.get(), (int)linkNumber.size() + 1);
                entry.set("link", Json(made.first->second));
            }
        }
        clips.push(entry);
    }
    // tracks_made in the Python program's order (audio, midi, return, group), then anything else
    Json made = Json::object();
    for (const char* key : {"audio", "midi", "return", "group"}) {
        auto it = project.tracksMade.find(key);
        made.set(key, Json(it == project.tracksMade.end() ? 0 : it->second));
    }
    for (auto& kv : project.tracksMade)
        if (!made.has(kv.first)) made.set(kv.first, Json(kv.second));
    Json markers = Json::array();
    for (auto& m : project.markers) markers.push(Json::array({Json(m.seconds), Json(m.name)}));
    Json groupList = Json::array();
    for (auto& g : groups)
        groupList.push(Json::object({{"name", Json(g->name)},
                                     {"folded", Json(g->folded)},
                                     {"muted", Json(g->muted)},
                                     {"soloed", Json(g->soloed)},
                                     {"color", nullOr(g->color)},
                                     {"tone", nullOr(g->tone)}}));
    Json trackList = Json::array();
    for (auto& t : tracks) trackList.push(describeTrack(*t, groups, sr, audioFile, states, tracks));
    Json masterEffects = Json::array();
    for (auto& s : project.masterEffects) masterEffects.push(describeSlot(*s, states));
    return Json::object({
        {"format", Json(FORMAT)},
        {"version", Json(VERSION)},
        {"master", master.toJson()},
        {"tracks_made", made},
        {"timing", describeTiming(project.timing)},
        {"markers", markers},
        {"groups", groupList},
        {"tracks", trackList},
        {"clips", clips},
        {"master_effects", masterEffects},
        {"master_cables", describeCables(*project.masterCables, *project.masterEffects, tracks)},
    });
}

std::string audioFolder(const std::string& jsonPath) {
    return util::joinPath(util::dirName(util::absolutePath(jsonPath)), util::stem(jsonPath) + "_audio");
}

std::string backupsFolder(const std::string& jsonPath) {
    return util::joinPath(util::dirName(util::absolutePath(jsonPath)), util::stem(jsonPath) + "_backups");
}

std::vector<Backup> backups(const std::string& jsonPath) {
    std::string folder = backupsFolder(jsonPath);
    std::vector<Backup> found;
    if (!util::isDir(folder)) return found;
    for (auto& e : util::listDir(folder)) {
        if (e.isDir || !util::endsWith(util::lower(e.name), ".json")) continue;
        std::string p = util::joinPath(folder, e.name);
        found.push_back({p, util::fileMtime(p)});
    }
    std::stable_sort(found.begin(), found.end(), [](const Backup& a, const Backup& b) { return a.mtime > b.mtime; });
    return found;
}

bool writeJson(const std::string& path, const Json& data, std::string& error) {
    if (!util::writeFile(path, data.dump(2))) {  // (a temporary file, then renamed)
        error = "Cannot write " + path + ": " + strerror(errno);
        return false;
    }
    return true;
}

bool keepBackup(const std::string& path, std::string& backupPath, std::string& error, int kept) {
    backupPath.clear();
    if (!util::fileExists(path) || util::isDir(path)) return true;
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    Json data = Json::parse(text);
    if (!data.isObject() || data.str("format") != FORMAT)
        return true;  // (not a Tiny DAW project: saving over it is the user's choice, but it isn't ours to keep)
    std::string base = data["base"].isString() && !data["base"].toString().empty() ? data["base"].toString() : ".";
    data.set("base", Json(normRelative(util::joinPath("..", base))));
    std::string folder = backupsFolder(path);
    if (!util::isDir(folder) && !util::makeDirs(folder)) {
        error = "Cannot make the folder " + folder + ": " + strerror(errno);
        return false;
    }
    std::vector<Backup> newest = backups(path);
    if (!newest.empty()) {
        std::string old;
        if (util::readFile(newest[0].path, old) && Json::parse(old) == data) {
            backupPath = newest[0].path;
            return true;
        }
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    char stamp[64];
    time_t mtime = st.st_mtime;
    struct tm local;
    localtime_r(&mtime, &local);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &local);
    std::string stem = util::stem(path);
    std::string target = util::joinPath(folder, stem + " " + stamp + ".json");
    for (int n = 2; util::fileExists(target); n++)
        target = util::joinPath(folder, stem + " " + stamp + " (" + std::to_string(n) + ").json");
    if (!writeJson(target, data, error)) return false;
    struct timespec times[2] = {st.st_atim, st.st_mtim};  // (its time is when that version was saved)
    utimensat(AT_FDCWD, target.c_str(), times, 0);
    std::vector<Backup> all = backups(path);
    for (size_t i = (size_t)std::max(0, kept); i < all.size(); i++) util::removeFile(all[i].path);
    backupPath = target;
    return true;
}

std::vector<ClipPtr> audioHolders(const Project& project) {
    std::vector<ClipPtr> out;
    for (auto& c : project.clips)
        if (c->kind != Kind::Midi && c->source) out.push_back(c);
    for (auto& t : project.tracks)
        if (t->frozen && t->frozen->source) out.push_back(t->frozen);
    for (auto& t : project.tracks)
        if (t->sampler && t->sampler->sample && t->sampler->sample->source) out.push_back(t->sampler->sample);
    for (auto& t : project.tracks)
        for (auto& take : t->takes)
            if (take->kind != Kind::Midi && take->source) out.push_back(take);
    return out;
}

Json describeRecovery(const Project& project, int sr, const MasterSettings& master, const std::string& folder,
                      std::map<std::string, std::string>& written, std::vector<PendingAudio>& toWrite) {
    std::map<const AudioBuffer*, std::string> files;
    for (auto& clip : audioHolders(project)) {
        if (files.count(clip->source.get())) continue;
        if (!clip->sourcePath.empty() && util::fileExists(clip->sourcePath)) {
            files[clip->source.get()] = util::absolutePath(clip->sourcePath);
            continue;
        }
        std::string sound = fingerprint(clip->source, clip->sourceSr);
        auto it = written.find(sound);
        std::string file = it == written.end() ? std::string() : it->second;
        bool pending = false;  // (identical audio in two clips: written once)
        for (auto& w : toWrite) pending = pending || w.file == file;
        if (file.empty() || !(util::fileExists(file) || pending)) {  // (not written yet, or its file went)
            std::set<std::string> taken;
            for (auto& kv : written)
                if (kv.second != file) taken.insert(kv.second);
            file = util::joinPath(folder, uniqueFileName(folder, clip->name, taken));
            written[sound] = file;
            toWrite.push_back({clip->source, clip->sourceSr, file});
        }
        files[clip->source.get()] = file;
    }
    return describe(project, sr, master, [&](const Clip& clip) {
        auto f = files.find(clip.source.get());
        return f == files.end() ? std::string() : f->second;
    });
}

bool writeRecovery(const std::string& path, const Json& data, const std::vector<PendingAudio>& toWrite,
                   std::string& error) {
    if (!toWrite.empty()) {
        std::string dir = util::dirName(toWrite[0].file);
        if (!util::isDir(dir) && !util::makeDirs(dir)) {
            error = "Cannot make the folder " + dir + ": " + strerror(errno);
            return false;
        }
    }
    for (auto& w : toWrite) {
        std::string temp = w.file + ".tmp";
        const AudioBuffer& buf = *w.audio;
        if (!writeWav(temp, buf.samples.data(), (size_t)buf.frames(), 2, w.sampleRate, SampleFormat::Float32, error))
            return false;
        if (rename(temp.c_str(), w.file.c_str()) != 0) {
            error = "Cannot write " + w.file + ": " + strerror(errno);
            return false;
        }
    }
    return writeJson(path, data, error);
}

namespace {
struct Fingerprint {
    std::weak_ptr<const AudioBuffer> audio;
    std::string digest;
};
std::mutex fingerprintLock;
std::map<std::pair<const AudioBuffer*, int>, Fingerprint> fingerprints;  // forgotten with the audio
}  // namespace

std::string fingerprint(const AudioPtr& audio, int sr) {
    if (!audio) return std::string();
    std::lock_guard<std::mutex> hold(fingerprintLock);
    auto key = std::make_pair(audio.get(), sr);
    auto it = fingerprints.find(key);
    if (it != fingerprints.end() && it->second.audio.lock() == audio) return it->second.digest;
    Md5 md5;
    md5.update(audio->samples.data(), audio->samples.size() * sizeof(float));
    md5.update(util::format("(%lld, 2) float32 %d", (long long)audio->frames(), sr));
    std::string digest = md5.hex();
    for (auto i = fingerprints.begin(); i != fingerprints.end();)  // (audio that is gone)
        i = i->second.audio.expired() ? fingerprints.erase(i) : std::next(i);
    fingerprints[key] = Fingerprint{audio, digest};
    return digest;
}

std::string uniqueFileName(const std::string& folder, const std::string& clipName, const std::set<std::string>& taken) {
    // re.sub(r"[^\w\- ]+", "_", stem).strip() or "audio"
    std::string root = splitextRoot(clipName), stem;
    bool inRun = false;
    for (size_t i = 0; i < root.size();) {
        size_t at = i;
        uint32_t cp = nextCodePoint(root, i);
        if (isWordChar(cp) || cp == '-' || cp == ' ') {
            stem.append(root, at, i - at);
            inRun = false;
        } else if (!inRun) {
            stem += '_';
            inRun = true;
        }
    }
    size_t a = stem.find_first_not_of(' '), b = stem.find_last_not_of(' ');
    stem = a == std::string::npos ? std::string() : stem.substr(a, b - a + 1);
    if (stem.empty()) stem = "audio";
    std::string name = stem + ".wav";
    for (int n = 2; util::fileExists(util::joinPath(folder, name)) || taken.count(util::joinPath(folder, name)); n++) name = stem + " (" + std::to_string(n) + ").wav";
    return name;
}

bool saveProject(const std::string& path, Project& project, int sr, const MasterSettings& master,
                 std::string& error) {
    std::string folder = audioFolder(path);
    if (!util::makeDirs(folder) && !util::isDir(folder)) {
        error = "Cannot make the folder " + folder + ": " + strerror(errno);
        return false;
    }
    std::string folderName = util::baseName(folder);

    // audio clips, frozen renderings, samplers' samples, recorded takes
    std::vector<ClipPtr> audio = audioHolders(project);
    std::map<const AudioBuffer*, std::string> files;  // audio -> file name in the folder
    std::map<std::string, std::string> bySound;      // fingerprint -> file name: the same audio is kept in one file
    // audio already saved in this folder (from an earlier save or load) is reused as is
    for (auto& clip : audio) {
        const std::string& p = clip->sourcePath;
        if (!p.empty() && util::dirName(util::absolutePath(p)) == folder && util::fileExists(p))
            files[clip->source.get()] =
                bySound.emplace(fingerprint(clip->source, clip->sourceSr), util::baseName(p)).first->second;
    }
    // everything else is written as a new file, never over an existing one (once per sound)
    for (auto& clip : audio) {
        if (files.count(clip->source.get())) continue;
        std::string sound = fingerprint(clip->source, clip->sourceSr);
        auto it = bySound.find(sound);
        if (it == bySound.end()) {
            std::string name = uniqueFileName(folder, clip->name);
            const AudioBuffer& buf = *clip->source;
            if (!writeWav(util::joinPath(folder, name), buf.samples.data(), (size_t)buf.frames(), 2, clip->sourceSr,
                          SampleFormat::Float32, error))
                return false;
            it = bySound.emplace(sound, name).first;
        }
        files[clip->source.get()] = it->second;
    }
    for (auto& clip : audio) clip->sourcePath = util::joinPath(folder, files[clip->source.get()]);

    Json data = describe(project, sr, master, [&](const Clip& clip) {
        auto it = files.find(clip.source.get());
        return it == files.end() ? std::string() : folderName + "/" + it->second;
    });
    return writeJson(path, data, error);
}

namespace {
struct AudioLoader {
    std::map<std::string, std::pair<AudioPtr, int>> sources;  // file path -> (audio, rate), loaded once and shared
    bool load(const std::string& file, AudioPtr& audio, int& rate, std::string& error) {
        auto it = sources.find(file);
        if (it == sources.end()) {
            AudioPtr source;
            int sourceSr = 0;
            std::string err;
            if (!file_io::loadAudio(file, source, sourceSr, err)) {
                error = file + "\n\n" + err;
                return false;
            }
            it = sources.emplace(file, std::make_pair(source, sourceSr)).first;
        }
        audio = it->second.first;
        rate = it->second.second;
        return true;
    }
};
struct LoadFailed {};  // (`error` says why)
}  // namespace

bool loadProject(const std::string& path, Project& project, int sr, MasterSettings& master, std::string& error) {
    std::string text;
    if (!util::readFile(path, text)) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    std::string parseError;
    Json data = Json::parse(text, &parseError);
    if (!data.isObject() || data.str("format") != FORMAT) {
        error = "This is not a Tiny DAW project file.";
        return false;
    }
    std::string base = util::dirName(util::absolutePath(path));
    if (data["base"].isString() && !data["base"].toString().empty() && data["base"].toString()[0] != '/')
        base = util::absolutePath(util::joinPath(base, data["base"].toString()));  // (a backup: see the top)
    auto fileIn = [&](const std::string& name) { return util::absolutePath(util::joinPath(base, name)); };
    AudioLoader loader;
    auto audioClip = [&](const std::string& file, const std::string& name) {
        auto clip = std::make_shared<Clip>();
        clip->kind = Kind::Audio;
        clip->name = name;
        if (!loader.load(file, clip->source, clip->sourceSr, error)) throw LoadFailed{};
        clip->sourcePath = file;
        return clip;
    };
    // its window from where it starts and ends, so clips that met still meet
    auto window = [&](Clip& clip, const Json& entry) {
        double startS = need(entry, "start").toDouble();
        clip.start = roundFrames(startS, sr);
        clip.offset = roundFrames(need(entry, "offset").toDouble(), sr);
        const Json& length = need(entry, "length");
        clip.length = length.isNull() ? -1 : roundFrames(startS + length.toDouble(), sr) - clip.start;
    };
    try {
        std::vector<GroupPtr> groups;
        for (const Json& g : field(data, "groups").items()) {
            if (!g.isObject()) throw Damaged{"a group is not an object"};
            auto group = std::make_shared<Group>();
            group->name = strOf(need(g, "name"));
            group->folded = g.flag("folded", false);
            group->muted = g.flag("muted", false);
            group->soloed = g.flag("soloed", false);
            group->color = g.str("color");
            group->tone = g.str("tone");
            groups.push_back(group);
        }
        const Json& trackEntries = need(data, "tracks");
        if (!trackEntries.isArray()) throw Damaged{"'tracks' is not a list"};
        std::vector<TrackPtr> tracks;
        for (const Json& t : trackEntries.items()) {
            if (!t.isObject()) throw Damaged{"a track is not an object"};
            auto track = std::make_shared<Track>(strOf(need(t, "name")), kindFromName(t.str("kind", "audio")));
            track->muted = t.flag("muted", false);
            track->soloed = t.flag("soloed", false);
            track->output = t.integer("output", 0);
            track->volumeDb = t.num("volume_db", 0.0);
            track->pan = t.num("pan", 0.0);
            track->delayMs = t.num("delay_ms", 0.0);
            track->height = t.integer("height", config::TRACK_H);
            track->color = t.str("color");
            track->tone = t.str("tone");
            track->needsInstrument = t.flag("needs_instrument", false);
            track->showAutomation = t.flag("show_automation", false);
            track->usesRack = t.flag("uses_rack", false);
            if (!t["group"].isNull()) {
                size_t i;
                if (!isIndex(t["group"], groups.size(), i)) throw Damaged{"a track's group is not there"};
                track->group = groups[i];
            }
            if (t["instrument"].isObject() && t["instrument"].size()) track->instrument = readSlot(t["instrument"]);
            std::vector<SlotPtr> effects, rack;
            for (const Json& s : t["effects"].items())
                if (s.isObject()) effects.push_back(readSlot(s));
            for (const Json& s : t["rack"].items())
                if (s.isObject()) rack.push_back(readSlot(s));
            track->effects = effects;
            track->rack = rack;
            tracks.push_back(track);
        }
        for (size_t k = 0; k < tracks.size(); k++) {
            Track& track = *tracks[k];
            const Json& entry = trackEntries[k];
            track.cables = readCables(entry["cables"], *track.effects, tracks);
            const Json& rc = entry["rack_cables"];
            std::vector<Cable> rackCables;
            for (auto& c : readCables(rc.isNull() ? Json::array() : rc, *track.rack))
                if ((c.first.type == End::In) != (c.second.type == End::Out))  // (notes in, sound out: nothing else)
                    rackCables.push_back(c);
            track.rackCables = rackCables;
            track.automation = readAutomation(track, entry["automation"]);
            track.modCables = readModulation(track, entry["modulation"]);
            const Json& sampler = entry["sampler"];
            if (sampler.isObject() && sampler.size()) {
                std::string file = fileIn(strOf(need(sampler, "file")));
                if (util::fileExists(file)) {  // (its sample missing: the built-in synth plays)
                    auto s = std::make_shared<SamplerSettings>();
                    s->sample = audioClip(file, sampler.str("name", util::baseName(file)));
                    s->root = sampler.integer("root", s->root);
                    s->transpose = sampler.num("transpose", s->transpose);
                    s->gainDb = sampler.num("gain_db", s->gainDb);
                    s->velocity = sampler.num("velocity", s->velocity);
                    s->attack = sampler.num("attack", s->attack);
                    s->decay = sampler.num("decay", s->decay);
                    s->sustain = sampler.num("sustain", s->sustain);
                    s->release = sampler.num("release", s->release);
                    s->oneShot = sampler.flag("one_shot", s->oneShot);
                    s->loop = sampler.flag("loop", s->loop);
                    s->start = sampler.num("start", s->start);
                    s->end = sampler.num("end", s->end);
                    track.sampler = s;
                }
            }
            track.midiEffects = readMidiEffects(entry["midi_effects"]);
            std::vector<ClipPtr> takes;
            for (const Json& e : entry["takes"].items()) {
                if (!e.isObject()) continue;
                ClipPtr take;
                if (e.str("kind") == "audio") {
                    std::string file = fileIn(strOf(need(e, "file")));
                    if (!util::fileExists(file)) continue;  // (its audio file missing: the take is left out)
                    take = audioClip(file, strOf(need(e, "name")));
                } else {
                    take = std::make_shared<Clip>();
                    take->kind = Kind::Midi;
                    take->name = strOf(need(e, "name"));
                    take->notes = readNotes(need(e, "notes"), true);
                    take->lengthS = need(e, "midi_length").toDouble();
                }
                window(*take, e);
                prepareSafely(*take, sr);
                takes.push_back(take);
            }
            track.takes = takes;
            track.showTakes = entry.flag("show_takes", false) && !takes.empty();
            std::vector<Send> sends;
            for (const Json& s : entry["sends"].items()) {
                size_t i;
                if (s.isArray() && s.size() == 2 && isIndex(s[size_t(0)], tracks.size(), i) && s[size_t(1)].isNumber() &&
                    tracks[i]->kind == Kind::Return && tracks[i].get() != &track)
                    sends.push_back({tracks[i], s[size_t(1)].toDouble()});
            }
            for (const Json& k : entry["pre_fader_sends"].items()) {  // (pre-fader sends: the returns' indices)
                size_t i;
                if (!isIndex(k, tracks.size(), i)) continue;
                for (auto& s : sends)
                    if (s.track == tracks[i]) s.preFader = true;
            }
            track.sends = sends;
            const Json& frozen = entry["frozen"];
            if (frozen.isObject() && frozen.size()) {
                std::string file = fileIn(strOf(need(frozen, "file")));
                if (util::fileExists(file)) {  // (its file missing: the track opens unfrozen, playing live)
                    auto clip = audioClip(file, track.name + " (frozen)");
                    // (no clip->track: the track owns its rendering, and a back pointer would be a cycle)
                    clip->start = roundFrames(need(frozen, "start").toDouble(), sr);
                    clip->prepare(sr);
                    track.frozen = clip;
                }
            }
        }

        Timing timing = readTiming(field(data, "timing"));
        std::vector<std::string> missing;
        std::vector<ClipPtr> clips;
        std::map<double, ClipPtr> linked;  // linked MIDI clips: "link" number -> the first clip with it
        const Json& clipEntries = need(data, "clips");
        for (const Json& entry : clipEntries.items()) {
            if (!entry.isObject()) throw Damaged{"a clip is not an object"};
            size_t row;
            if (!isIndex(need(entry, "track"), tracks.size(), row)) throw Damaged{"a clip's track is not there"};
            ClipPtr clip;
            if (strOf(need(entry, "kind")) == "audio") {
                std::string name = strOf(need(entry, "file"));
                std::string file = fileIn(name);
                if (!util::fileExists(file)) {
                    missing.push_back(name);
                    continue;
                }
                clip = audioClip(file, strOf(need(entry, "name")));
                clip->stretch = entry.num("stretch", 1.0);
                clip->pitch = entry.num("pitch", 0.0);
                clip->sourceBpm = entry.num("source_bpm", 0.0);
                clip->reverse = entry.flag("reverse", false);
                clip->loop = entry.flag("loop", false);
                clip->loopStart = entry.num("loop_start", 0.0);
                clip->loopLength = entry.num("loop_length", 0.0);
            } else {
                clip = std::make_shared<Clip>();
                clip->kind = Kind::Midi;
                clip->name = strOf(need(entry, "name"));
                clip->notes = readNotes(need(entry, "notes"), true);
                clip->mutedNotes = readNotes(entry["muted_notes"], false);
                clip->lengthS = need(entry, "midi_length").toDouble();
                clip->loop = entry.flag("loop", false);
                const Json& link = entry["link"];  // (linked MIDI clips: the first one's notes are everyone's)
                if (link.isNumber()) {
                    auto made = linked.emplace(link.toDouble(), clip);
                    if (!made.second) {
                        const ClipPtr& first = made.first->second;
                        if (!first->link) first->link = std::make_shared<ClipLink>();
                        clip->link = first->link;
                        clip->notes = first->notes;
                        clip->mutedNotes = first->mutedNotes;
                        clip->lengthS = first->lengthS;
                        clip->loop = first->loop;
                    }
                }
            }
            clip->track = tracks[row];
            window(*clip, entry);
            clip->color = entry.str("color");
            clip->tone = entry.str("tone");
            clip->disabled = entry.flag("disabled", false);
            clip->gainDb = entry.num("gain_db", 0.0);
            clip->fadeIn = roundFrames(entry.num("fade_in", 0.0), sr);
            clip->fadeOut = roundFrames(entry.num("fade_out", 0.0), sr);
            clip->fadeLead = roundFrames(entry.num("fade_lead", 0.0), sr);
            clip->fadeTail = roundFrames(entry.num("fade_tail", 0.0), sr);
            clip->fadeInShape = fadeShapeFromName(entry.str("fade_in_shape"));  // (none: linear)
            clip->fadeOutShape = fadeShapeFromName(entry.str("fade_out_shape"));
            prepareSafely(*clip, sr);
            clips.push_back(clip);
        }
        if (!missing.empty()) {
            error = "Audio files are missing:";
            for (auto& m : missing) error += "\n" + m;
            return false;
        }
        std::vector<Marker> markers;
        for (const Json& m : field(data, "markers").items())
            if (m.isArray() && m.size() == 2 && m[size_t(0)].isNumber())
                markers.push_back({std::max(0.0, m[size_t(0)].toDouble()),
                                   m[size_t(1)].isString() ? m[size_t(1)].toString() : m[size_t(1)].dump()});
        std::vector<SlotPtr> masterEffects;
        for (const Json& s : field(data, "master_effects").items())
            if (s.isObject()) masterEffects.push_back(readSlot(s));
        std::vector<Cable> masterCables = readCables(field(data, "master_cables"), masterEffects, tracks);

        // all read: now the project is replaced (its plugins retired, never destroyed under the audio thread)
        for (auto& slot : project.pluginSlots())
            if (slot->instance) project.retire(slot->instance);
        project.tracks = tracks;
        project.clips = clips;
        project.masterEffects = masterEffects;
        project.masterCables = masterCables;
        project.masterPatchState.reset();
        const Json& made = field(data, "tracks_made");
        int returns = 0;
        for (auto& t : tracks) returns += t->kind == Kind::Return;
        project.tracksMade = {{"audio", made.integer("audio", 0)},
                              {"midi", made.integer("midi", 0)},
                              {"group", made.integer("group", (int)groups.size())},
                              {"return", made.integer("return", returns)}};
        project.gatherGroups();
        project.timing = timing;
        project.setMarkers(markers);
        master = MasterSettings::fromJson(field(data, "master"));
        return true;
    } catch (const Damaged& d) {
        error = "This project file is damaged: " + d.what + ".";
        return false;
    } catch (const LoadFailed&) {
        return false;
    }
}

}  // namespace project_io
