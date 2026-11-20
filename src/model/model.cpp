#include "model/model.h"
#include "model/pad_tracks.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "audio/stretch.h"
#include "model/clip_envelope.h"
#include "plugins/plugin.h"

namespace clip_hooks {
std::function<std::vector<Note>(const Track&, const std::vector<Note>&, double)> midiEffects;
std::function<AudioPtr(const Track&, const std::vector<Note>&, int, double)> renderSampler;
std::function<TempoMapPtr()> tempoMap;
namespace {
thread_local TempoMapPtr g_using;
}
TempoMapPtr currentTempoMap() {
    if (g_using) return g_using;
    if (tempoMap)
        if (TempoMapPtr m = tempoMap()) return m;
    static const TempoMapPtr fallback = std::make_shared<const TempoMap>();
    return fallback;
}
UseTempoMap::UseTempoMap(TempoMapPtr map) : before(g_using) { g_using = std::move(map); }
UseTempoMap::~UseTempoMap() { g_using = before; }
}  // namespace clip_hooks

std::vector<Cable> chain(const std::vector<SlotPtr>& slots) {
    std::vector<End> ends{End::in()};
    for (auto& s : slots) ends.push_back(End::of(s.get()));
    ends.push_back(End::out());
    std::vector<Cable> out;
    for (size_t i = 0; i + 1 < ends.size(); i++) out.emplace_back(ends[i], ends[i + 1]);
    return out;
}

PluginSlot::PluginSlot(const PluginSlot& o)
    : path(o.path), name(o.name), kind(o.kind), pluginName(o.pluginName), bypass(o.bypass), state(o.state),
      lastRead(o.lastRead), instance(std::atomic_load(&o.instance)), failed(o.failed.load()), error(o.error),
      recent(o.recent), placed(o.placed), atX(o.atX), atY(o.atY), volumeDb(o.volumeDb), mix(o.mix) {}

std::string PluginSlot::currentState() const {
    auto p = std::atomic_load(&instance);
    if (!p) return state;
    try {
        std::string component = p->componentState();  // (first: a Windows plugin gives both at once)
        lastRead = plug::packState(component, p->controllerState());
        return lastRead;
    } catch (const std::exception&) {  // (a plugin that can't give its settings now: the last it gave)
        return lastRead.empty() ? state : lastRead;
    }
}

void Automation::setPoints(std::vector<AutoPoint> pts) {
    for (auto& p : pts) {
        p.t = std::max(0.0, p.t);
        p.v = std::min(1.0, std::max(0.0, p.v));
        p.bend = std::isfinite(p.bend) ? std::min(1.0, std::max(-1.0, p.bend)) : 0.0;
    }
    std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& a, const AutoPoint& b) {
        return a.t < b.t || (a.t == b.t && a.v < b.v);
    });
    points = std::move(pts);
}

// --- MIDI effects ---

const std::vector<std::pair<std::string, std::vector<MidiEffect::Param>>>& MidiEffect::kinds() {
    static const std::vector<std::pair<std::string, std::vector<Param>>> table = {
        {"Arpeggiator", {{"style", 0}, {"rate", 3}, {"gate", 80}, {"octaves", 1}}},
        {"Chord", {{"chord", 0}, {"velocity", 85}}},
        {"Scale", {{"root", 0}, {"scale", 0}}},
        {"Note Length", {{"mode", 0}, {"beats", 0.25}, {"share", 100}}},
        {"Velocity", {{"scale", 100}, {"offset", 0}, {"random", 0}, {"lowest", 1}, {"highest", 127}}},
        {"Random", {{"chance", 50}, {"range", 12}, {"seed", 1}}},
        {"Pitch", {{"semitones", 0}}},
        // (new kinds go last)
        {"Groove", {{"swing", 60}, {"grid", 1}, {"accent", 0}, {"timing", 0}, {"velocity", 0}, {"seed", 1}}},
    };
    return table;
}

bool MidiEffect::make(const std::string& kind, const std::map<std::string, double>& values, MidiEffect& out) {
    for (auto& k : kinds()) {
        if (k.first != kind) continue;
        out.kind = kind;
        out.values.clear();
        for (auto& p : k.second) {
            auto it = values.find(p.key);
            out.values[p.key] = it != values.end() ? it->second : p.defaultValue;
        }
        if (kind == "Groove")  // (its rows' own swing, set in the Steps view: "row:<pitch>" → 0…100 %)
            for (auto& v : values)
                if (v.first.rfind("row:", 0) == 0) out.values[v.first] = v.second;
        return true;
    }
    return false;
}

double MidiEffect::get(const std::string& key) const {
    auto it = values.find(key);
    if (it != values.end()) return it->second;
    for (auto& k : kinds())
        if (k.first == kind)
            for (auto& p : k.second)
                if (key == p.key) return p.defaultValue;
    return 0;
}

MidiEffect MidiEffect::with(const std::string& key, double value) const {
    MidiEffect e = *this;
    e.values[key] = value;
    return e;
}

bool SamplerSettings::operator==(const SamplerSettings& o) const {
    return sample == o.sample && root == o.root && transpose == o.transpose && gainDb == o.gainDb &&
           velocity == o.velocity && attack == o.attack && decay == o.decay && sustain == o.sustain &&
           release == o.release && oneShot == o.oneShot && loop == o.loop && start == o.start && end == o.end;
}

bool Pad::empty() const { return !sound.sample || !sound.sample->source || sound.sample->source->frames() == 0; }

PadKit::PadKit() : pads((size_t)MAX_PADS) {
    for (int i = 0; i < MAX_PADS; i++) {
        pads[(size_t)i].note = FIRST_NOTE + i;
        pads[(size_t)i].sound.oneShot = true;  // (drums: each hit plays to its end)
    }
}

int PadKit::used() const {
    int n = 0;
    for (auto& p : pads) n += p.empty() ? 0 : 1;
    return n;
}

// --- tracks ---

Track::Track(std::string n, Kind k)
    : name(std::move(n)), kind(k), controls([] {
          auto s = std::make_shared<PluginSlot>();
          s->kind = PluginSlot::CONTROLS;
          s->name = "Track volume and pan";
          return s;
      }()) {}

Group::Group() : bus(std::make_shared<Track>("", Kind::Audio)) { bus->busOf = this; }

std::string Track::keyLabel(int pitch) const {
    PadKitPtr kit = std::atomic_load(&padKit);
    if (!kit || kind != Kind::Midi || !playsPadKit()) return "";
    for (auto& pad : kit->pads)
        if (pad.note == pitch && !pad.empty()) {
            const std::string& name = pad.sound.sample->name;
            size_t dot = name.find_last_of('.');
            return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);  // (without its extension)
        }
    return "";
}

std::vector<SlotPtr> Track::instruments() const {
    if (kind != Kind::Midi || needsInstrument) return {};
    if (usesRack) return rack.copy();
    if (instrument) return {instrument};
    return {};
}

bool Track::playsPlugins() const {
    if (kind != Kind::Midi || needsInstrument || ext.on) return false;
    if (usesRack) return true;
    SlotPtr inst = std::atomic_load(&instrument);
    return inst && std::atomic_load(&inst->instance) && !inst->failed.load();
}

// --- clips ---

AudioPtr looped(const AudioPtr& audio, int64_t start, int64_t frames, int64_t reach, int64_t seam) {
    if (!audio) return audio;
    int64_t len = audio->frames();
    start = std::max<int64_t>(0, std::min(start, len));
    frames = std::max<int64_t>(0, std::min(frames, len - start));
    int64_t total = start + frames;
    if (total >= reach || frames <= 0) {
        if (total == len) return audio;
        auto cut = std::make_shared<AudioBuffer>();
        cut->samples.assign(audio->samples.begin(), audio->samples.begin() + total * 2);
        return cut;
    }
    int64_t follows = std::max<int64_t>(0, std::min(seam, len - total));
    int64_t k = std::min({seam, follows, frames});
    std::vector<float> one(audio->samples.begin() + start * 2, audio->samples.begin() + total * 2);
    for (int64_t i = 0; i < k; i++) {
        double ramp = k > 1 ? (double)i / (double)(k - 1) : 0.0;  // np.linspace(0, 1, k)
        for (int c = 0; c < 2; c++) {
            float& v = one[(size_t)(i * 2 + c)];
            v = (float)(v * ramp + audio->samples[(size_t)((total + i) * 2 + c)] * (1 - ramp));
        }
    }
    int64_t passes = (reach - total + frames - 1) / frames;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.reserve((size_t)(total + passes * frames) * 2);
    out->samples.assign(audio->samples.begin(), audio->samples.begin() + total * 2);
    for (int64_t p = 0; p < passes; p++) out->samples.insert(out->samples.end(), one.begin(), one.end());
    return out;
}

void Clip::prepare(int sr, int64_t reach) {
    if (kind != Kind::Midi) {
        AudioPtr d;
        if (warp && source) {  // (warped: rendered for the song's tempo where its data starts: audio/warp.h)
            TempoMapPtr map = clip_hooks::currentTempoMap();
            d = warp::rendered(warpJob(map, start - offset, sr));
            warpKey = std::make_shared<const WarpKey>(WarpKey{map, start - offset});
        } else {
            AudioPtr base = source;
            if (source && (stretch != 1.0 || pitch != 0.0 || reverse))  // (at its own rate, so its pieces share it)
                base = ::stretch::stretched(source, stretch, pitch, reverse);
            d = dsp::resample(base, sourceSr, sr);
            warpKey = nullptr;
        }
        baseFrames = d ? d->frames() : 0;
        loopFrom = loopFor = 0;
        if (loop && loopLength > 0 && d && d->frames()) {
            int64_t len = d->frames();
            int64_t a = std::min(len - 1, std::max<int64_t>(0, (int64_t)std::nearbyint(loopStart * sr)));
            int64_t n = std::max<int64_t>(1, std::min(len - a, (int64_t)std::nearbyint(loopLength * sr)));
            if (reach < 0) reach = offset + (length >= 0 ? length : len - offset);
            loopFrom = a;
            loopFor = n;
            d = looped(d, a, n, reach, (int64_t)std::nearbyint(config::LOOP_SEAM_S * sr));
        }
        data = d;
        peaks = data ? dsp::waveformPeaks(*data, config::PEAK_BLOCK) : nullptr;
        return;
    }
    std::vector<Note> notesPlayed = performed(sr);  // (its notes' chances and repeats, then the MIDI effects)
    TrackPtr t = std::atomic_load(&track);
    if (t && clip_hooks::midiEffects && !effectsCommitted)  // (the song's swing, then its track's MIDI effects: arpeggiator, chord…)
        notesPlayed = clip_hooks::midiEffects(*t, notesPlayed, (double)(start - offset) / sr);
    double len = loop && length >= 0 ? std::max(lengthS, (double)(offset + length) / sr) : lengthS;
    if (t && (t->playsPlugins() || t->ext.on)) {  // (its instrument plugins or External synth play its events: silent)
        auto silent = std::make_shared<AudioBuffer>();
        silent->samples.assign((size_t)((int64_t)((len + synth::RELEASE_S) * sr) + 1) * 2, 0.0f);
        data = silent;
    } else if (t && (t->playsSampler() || t->playsPadKit()) && clip_hooks::renderSampler) {
        data = clip_hooks::renderSampler(*t, notesPlayed, sr, len);
    } else {
        data = synth::render(notesPlayed, sr, len);
    }
    auto ev = std::make_shared<std::vector<Event>>();
    ev->reserve(notesPlayed.size());
    for (auto& n : notesPlayed) {
        int64_t on = (int64_t)std::nearbyint(n.start * sr), off = (int64_t)std::nearbyint(n.end * sr);
        // every note at least a frame long: a note-off at its note-on's very frame would be sent first
        // (offs go before ons at the same moment), leaving the instrument's note on
        ev->push_back({on, std::max(off, on + 1), (int)n.pitch, (int)n.velocity});
    }
    // A sliding note on a VST instrument: its note-on comes before the note it slides from lets go (legato),
    // so a mono synth with portamento glides as the built-in instruments do. (Its pan and fine tuning
    // can't be sent per note: plugins ignore them.)
    std::vector<note_props::Slide> slid = note_props::slides(notesPlayed);
    for (size_t i = 0; i < slid.size(); i++) {
        if (!slid[i].legato) continue;
        Event& before = (*ev)[(size_t)slid[i].from];
        const Event& after = (*ev)[i];
        if (before.pitch != after.pitch && before.off <= after.on) before.off = after.on + 1;  // (offs go first
    }                                                                                          // at one moment)
    events = ev;
    played = std::make_shared<const std::vector<Note>>(std::move(notesPlayed));
}

warp::Job Clip::warpJob(const TempoMapPtr& map, int64_t origin, int sr) const {
    warp::Job j;
    j.source = source;
    j.sourceSr = sourceSr;
    j.markers = warpMarkers ? *warpMarkers : warp::Markers{};
    j.bpm = sourceBpm > 0 ? sourceBpm : warp::DEFAULT_BPM;
    j.mode = warpMode;
    j.preserve = warpPreserve;
    j.semitones = pitch;
    j.reverse = reverse;
    j.map = map;
    j.origin = sr > 0 ? (double)origin / sr : 0;
    j.sr = sr;
    return j;
}

void Clip::fitWindow() {
    if (kind == Kind::Audio && length >= 0 && data) length = std::max<int64_t>(1, std::min(length, data->frames() - offset));
}

void Clip::fades(int64_t& fi, int64_t& fo) const {
    fi = std::max<int64_t>(0, fadeIn);
    fo = std::max<int64_t>(0, fadeOut);
    int64_t n = frames();
    if (n <= 0) {
        fi = fo = 0;
        return;
    }
    int64_t whole = n + std::max<int64_t>(0, fadeLead) + std::max<int64_t>(0, fadeTail);
    if (fi + fo > whole) {
        int64_t sum = fi + fo;
        fi = fi * whole / sum;
        fo = fo * whole / sum;
    }
}

float Clip::level(int64_t pos) const {
    if (!hasLevels()) return 1.0f;
    int64_t fi, fo;
    fades(fi, fo);
    double level = std::pow(10.0, gainDb / 20.0);
    if (fi) level *= fadeCurve(fadeInShape, std::min(1.0, std::max(0.0, (double)(pos + std::max<int64_t>(0, fadeLead) + 1) / fi)), false);
    if (fo) level *= fadeCurve(fadeOutShape, std::min(1.0, std::max(0.0, (double)(frames() + std::max<int64_t>(0, fadeTail) - pos) / fo)), true);
    if (auto envelope = std::atomic_load(&gainEnvelope)) {  // (as the mixer plays it: clip_envelope::Walk)
        clip_envelope::Walk walk{*this, *envelope};
        level *= walk.gain(offset + pos);
    }
    return (float)level;
}

// --- fade shapes (model.h FADE SHAPES) ---
const char* fadeShapeName(FadeShape s) {
    switch (s) {
    case FadeShape::Smooth: return "smooth";
    case FadeShape::Fast: return "fast";
    case FadeShape::Slow: return "slow";
    case FadeShape::EqualPower: return "equal_power";
    default: return "linear";
    }
}

FadeShape fadeShapeFromName(const std::string& s) {
    for (FadeShape f : FADE_SHAPES)
        if (s == fadeShapeName(f)) return f;
    return FadeShape::Linear;
}

const char* fadeShapeLabel(FadeShape s) {
    switch (s) {
    case FadeShape::Smooth: return "Smooth (S-curve)";
    case FadeShape::Fast: return "Fast (most of the change early)";
    case FadeShape::Slow: return "Slow (most of the change late)";
    case FadeShape::EqualPower: return "Equal power (for crossfades)";
    default: return "Linear";
    }
}

void Clip::fadeParts(int64_t& partIn, int64_t& partOut, double& levelStart, double& levelEnd) const {
    int64_t fi, fo;
    fades(fi, fo);
    int64_t lead = std::max<int64_t>(0, fadeLead), tail = std::max<int64_t>(0, fadeTail);
    partIn = std::max<int64_t>(0, fi - lead);
    partOut = std::max<int64_t>(0, fo - tail);
    levelStart = fi ? std::min(1.0, (double)lead / fi) : 1.0;
    levelEnd = fo ? std::min(1.0, (double)tail / fo) : 1.0;
}

namespace {
// each note of the clip as far as it reaches, as repeated() lays them out: f(the note as written, the
// loop pass it plays in, the note moved to that pass)
template <class F>
void eachPass(const Clip& c, int sr, F f) {
    if (!c.notes) return;
    const std::vector<Note>& ns = *c.notes;
    double reach = c.length >= 0 ? (double)(c.offset + c.length) / sr : c.lengthS;
    if (!c.loop || c.lengthS <= 0 || reach <= c.lengthS || ns.empty()) {
        for (const Note& n : ns) f(n, (int64_t)0, n);
        return;
    }
    int64_t passes = (int64_t)std::ceil(reach / c.lengthS);
    for (int64_t k = 0; k < passes; k++)
        for (const Note& written : ns) {
            if (!(written.start < c.lengthS)) continue;
            Note n = written;
            n.start += k * c.lengthS;
            n.end += k * c.lengthS;
            if (n.start < reach) f(written, k, n);
        }
}
}  // namespace

std::vector<Note> Clip::repeated(int sr) const {
    std::vector<Note> out;
    eachPass(*this, sr, [&](const Note&, int64_t, const Note& n) { out.push_back(n); });
    return out;
}

std::vector<Note> Clip::performed(int sr) const {
    std::vector<Note> out;
    eachPass(*this, sr, [&](const Note& written, int64_t pass, const Note& n) {
        if (written.plain()) {
            out.push_back(n);
        } else if (note_props::sounds(written, pass)) {  // (its chance: by the note as written, and the pass)
            Note played = n;
            played.chance = 100;  // (decided)
            note_props::perform(played, pass, out);
        }
    });
    return out;
}

std::vector<Note> Clip::heardNotes(int sr) const {
    double lo = (double)offset / sr, hi = (double)(offset + frames()) / sr;
    std::vector<Note> out;
    for (auto n : repeated(sr)) {
        if (!(n.start < hi && n.end > lo)) continue;
        n.start = std::max(n.start, lo) - lo;
        n.end = std::min(n.end, hi) - lo;
        out.push_back(n);
    }
    return out;
}

ClipPtr copyClip(const ClipPtr& c) {
    auto k = std::make_shared<Clip>(*c);
    k->track = std::atomic_load(&c->track);
    return k;
}

ClipPtr makeAudioClip(const std::string& name, AudioPtr source, int sourceSr, int sr) {
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Audio;
    c->name = name;
    c->source = std::move(source);
    c->sourceSr = sourceSr;
    c->prepare(sr);
    return c;
}

ClipPtr makeMidiClip(const std::string& name, std::vector<Note> notes, double lengthS, int sr) {
    auto c = std::make_shared<Clip>();
    c->kind = Kind::Midi;
    c->name = name;
    c->notes = std::make_shared<const std::vector<Note>>(std::move(notes));
    c->lengthS = lengthS;
    c->prepare(sr);
    return c;
}

// --- the project ---

void Project::clear() {
    tracks = std::vector<TrackPtr>{};
    clips = std::vector<ClipPtr>{};
    for (auto& s : *masterEffects)
        if (s->instance) retire(s->instance);
    masterEffects = std::vector<SlotPtr>{};
    masterCables = straight();
    masterPatchState.reset();
    tracksMade = {{"audio", 0}, {"midi", 0}, {"return", 0}, {"group", 0}};
    timing = Timing();
    markers = std::vector<Marker>{};
    midiLinks.clear();
}

void Project::forgetGoneTracks() {
    std::set<Track*> present;
    std::vector<TrackPtr> all = allTracks();
    for (auto& t : all) present.insert(t.get());
    for (auto& t : all) {  // (routed into a track that is gone: back to its group's bus, or the master)
        TrackPtr to = std::atomic_load(&t->into);
        if (to && !present.count(to.get())) std::atomic_store(&t->into, TrackPtr());
    }
    pad_tracks::forgetGone(*this);  // (a pad's track that is gone: the pad back in its kit)
    for (auto& t : all) {  // (the sends to returns that are gone)
        bool gone = false;
        for (auto& s : t->sends) gone = gone || !present.count(s.track.get());
        if (!gone) continue;
        std::vector<Send> kept;
        for (auto& s : t->sends)
            if (present.count(s.track.get())) kept.push_back(s);
        t->sends = kept;
    }
    auto clean = [&](Cables& cables) {
        std::vector<Cable> kept;
        for (auto& c : cables)
            if (!(c.first.type == End::TrackSource && !present.count(c.first.track))) kept.push_back(c);
        if (kept.size() != cables.size()) cables = kept;
    };
    for (auto& t : all) clean(t->cables);
    clean(masterCables);
}

std::vector<SlotPtr> Project::pluginSlots() const {
    std::vector<SlotPtr> out;
    for (auto& t : allTracks()) {
        if (t->instrument) out.push_back(t->instrument);
        for (auto& s : t->rack) out.push_back(s);
        for (auto& s : t->effects) out.push_back(s);
    }
    for (auto& s : masterEffects) out.push_back(s);
    return out;
}

TrackPtr Project::addTrack(Kind kind, GroupPtr group, TrackPtr after) {
    int n = ++tracksMade[kindName(kind)];
    std::string name;
    if (kind == Kind::Return)
        name = n <= 26 ? std::string("Return ") + char('A' + n - 1) : "Return " + std::to_string(n);
    else
        name = std::string(kind == Kind::Midi ? "MIDI " : "Audio ") + std::to_string(n);
    auto track = std::make_shared<Track>(name, kind);
    track->group = kind == Kind::Return ? nullptr : group;
    track->needsInstrument = kind == Kind::Midi;
    std::vector<TrackPtr> v = tracks.copy();
    int firstReturn = (int)v.size();
    for (size_t i = 0; i < v.size(); i++)
        if (v[i]->kind == Kind::Return) {
            firstReturn = (int)i;
            break;
        }
    int afterAt = after ? tracks.indexOf(after) : -1;
    int at = kind == Kind::Return ? (int)v.size() : afterAt >= 0 ? afterAt + 1 : firstReturn;
    v.insert(v.begin() + at, track);
    tracks = v;
    gatherGroups();
    return track;
}

std::vector<TrackPtr> Project::returns() const {
    std::vector<TrackPtr> out;
    for (auto& t : tracks)
        if (t->kind == Kind::Return) out.push_back(t);
    return out;
}

std::vector<GroupPtr> Project::groups() const {
    std::vector<GroupPtr> out;
    for (auto& t : tracks) {
        std::vector<GroupPtr> path;  // (outer first)
        for (GroupPtr g = t->group; g && path.size() < 1000; g = std::atomic_load(&g->parent)) path.insert(path.begin(), g);
        for (auto& g : path)
            if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    return out;
}

std::vector<TrackPtr> Project::allTracks() const {
    std::vector<TrackPtr> out = *tracks;
    for (auto& g : groups()) out.push_back(g->bus);
    return out;
}

std::vector<TrackPtr> Project::members(const GroupPtr& g) const {
    std::vector<TrackPtr> out;
    for (auto& t : tracks)
        if (t->group == g) out.push_back(t);
    return out;
}

GroupPtr Project::groupTracks(const std::vector<TrackPtr>& ts) {
    int n = ++tracksMade["group"];
    auto g = std::make_shared<Group>();
    g->name = "Group " + std::to_string(n);
    g->bus->name = g->name;
    // (tracks all in one group: the new group goes inside it)
    GroupPtr common;
    bool first = true, same = true;
    for (auto& t : ts)
        if (t->kind != Kind::Return) {
            if (!first && t->group != common) same = false;
            common = t->group;
            first = false;
        }
    if (same && common) std::atomic_store(&g->parent, common);
    for (auto& t : ts)
        if (t->kind != Kind::Return) t->group = g;  // (return tracks stay out of groups)
    gatherGroups();
    return g;
}

void Project::ungroup(const GroupPtr& g) {
    GroupPtr up = std::atomic_load(&g->parent);
    for (auto& t : members(g)) t->group = up;  // (nested: into the group it was in)
    for (auto& other : groups())
        if (std::atomic_load(&other->parent) == g) std::atomic_store(&other->parent, up);
    for (auto& t : allTracks())  // (routed into its bus: back to their default)
        if (std::atomic_load(&t->into) == g->bus) std::atomic_store(&t->into, TrackPtr());
}

namespace {
// the groups a track is in, outer first
std::vector<Group*> pathOf(const Track& t) {
    std::vector<Group*> path;
    for (Group* g = t.group.get(); g && path.size() < 1000; g = std::atomic_load(&g->parent).get()) path.insert(path.begin(), g);
    return path;
}
// ts in their order, each group's tracks (nested groups' too) together where its first one is
void gatherFrom(const std::vector<TrackPtr>& ts, size_t depth, std::vector<TrackPtr>& out) {
    std::set<Group*> done;
    for (auto& t : ts) {
        auto path = pathOf(*t);
        if (path.size() <= depth) {
            out.push_back(t);
            continue;
        }
        if (!done.insert(path[depth]).second) continue;
        std::vector<TrackPtr> inside;
        for (auto& u : ts) {
            auto p = pathOf(*u);
            if (p.size() > depth && p[depth] == path[depth]) inside.push_back(u);
        }
        gatherFrom(inside, depth + 1, out);
    }
}
}  // namespace

void Project::gatherGroups() {
    std::vector<TrackPtr> order;
    gatherFrom(*tracks, 0, order);
    std::vector<TrackPtr> out;  // (returns last)
    for (auto& t : order)
        if (t->kind != Kind::Return) out.push_back(t);
    for (auto& t : order)
        if (t->kind == Kind::Return) out.push_back(t);
    if (out != *tracks) tracks = out;
}

void Project::removeTrack(const TrackPtr& track) {
    std::vector<ClipPtr> keep;
    for (auto& c : clips)
        if (c->track != track) keep.push_back(c);
    clips = keep;
    std::vector<TrackPtr> ts;
    for (auto& t : tracks)
        if (t != track) ts.push_back(t);
    tracks = ts;
    for (auto& t : allTracks())  // (routed into it: back to their default)
        if (std::atomic_load(&t->into) == track) std::atomic_store(&t->into, TrackPtr());
    pad_tracks::forgetGone(*this);  // (a pad playing on it: back in its kit)
    if (track->instrument && track->instrument->instance) retire(track->instrument->instance);
    for (auto& s : track->rack)
        if (s->instance) retire(s->instance);
    for (auto& s : track->effects)
        if (s->instance) retire(s->instance);
}

TrackPtr Project::firstEmptyTrack(Kind kind) const {
    std::set<Track*> used;
    for (auto& c : clips) used.insert(c->track.get());
    for (auto& t : tracks)
        if (t->kind == kind && !used.count(t.get()) && !t->frozen && !(t->group && t->group->folded)) return t;
    return nullptr;
}

namespace {
// the innermost group a track's own flags go with: its group, or (a bus) the group it belongs to
Group* flagsGroup(const Track& t) { return t.busOf ? t.busOf : t.group.get(); }
// (nested groups: every group around it, up to 1000 deep; parents never loop)
bool anyGroup(const Track& t, bool Group::*flag) {
    int depth = 0;
    for (Group* g = flagsGroup(t); g && depth < 1000; g = std::atomic_load(&g->parent).get(), depth++)
        if (g->*flag) return true;
    return false;
}
}  // namespace

bool Project::mutedIn(const Track& t) { return t.muted || anyGroup(t, &Group::muted); }

bool Project::audibleIn(const Track& track, const std::vector<TrackPtr>& ts) {
    if (mutedIn(track) || track.needsInstrument) return false;
    if (track.soloed || anyGroup(track, &Group::soloed) || track.kind == Kind::Return || track.soloSafe)
        return true;  // (soloing a track keeps the returns, and the solo-safe tracks)
    for (auto& t : ts)
        if (t->kind != Kind::Return && (t->soloed || anyGroup(*t, &Group::soloed))) return false;
    return true;
}

bool Project::audible(const Track& t) const { return audibleIn(t, *tracks); }

void Project::soloOnly(const TrackPtr& t) {
    for (auto& o : tracks) o->soloed = o == t;
    for (auto& g : groups()) g->soloed = false;
}

void Project::removeClips(const std::vector<ClipPtr>& cs) {
    std::set<Clip*> gone;
    for (auto& c : cs) gone.insert(c.get());
    std::vector<ClipPtr> keep;
    for (auto& c : clips)
        if (!gone.count(c.get())) keep.push_back(c);
    clips = keep;
}

std::vector<ClipPtr> Project::clipsOn(const TrackPtr& t) const {
    std::vector<ClipPtr> out;
    for (auto& c : clips)
        if (c->track == t) out.push_back(c);
    return out;
}

std::vector<ClipPtr> Project::clipsIn(const std::vector<TrackPtr>& ts, int64_t start, int64_t end) const {
    std::set<Track*> want;
    for (auto& t : ts) want.insert(t.get());
    std::vector<ClipPtr> out;
    for (auto& c : clips)
        if (want.count(c->track.get()) && c->start < end && c->end() > start) out.push_back(c);
    return out;
}

void Project::setMarkers(std::vector<Marker> ms) {
    std::stable_sort(ms.begin(), ms.end(), [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
    markers = std::move(ms);
}

int64_t Project::endFrame() const {
    int64_t e = 0;
    for (auto& c : clips) e = std::max(e, c->end());
    return e;
}

static int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

void Project::changeSampleRate(int oldSr, int newSr) {
    if (oldSr <= 0 || newSr <= 0 || oldSr == newSr) return;
    // (the nearest frame: a rate and back is where it was)
    auto at = [&](int64_t frames) { return floorDiv(frames * newSr + oldSr / 2, oldSr); };
    std::vector<ClipPtr> all = clips.copy();
    for (auto& t : tracks) {
        if (t->frozen) all.push_back(t->frozen);
        for (auto& take : t->takes) all.push_back(take);
    }
    for (auto& c : all) {
        int64_t start = at(c->start), end = at(c->end()), origin = at(c->start - c->offset);
        c->start = start;
        c->offset = start - origin;
        c->fadeIn = at(c->fadeIn);
        c->fadeOut = at(c->fadeOut);
        c->fadeLead = at(c->fadeLead);
        c->fadeTail = at(c->fadeTail);
        if (auto envelope = c->gainEnvelope)  // (on frames of its audio)
            c->gainEnvelope = clip_envelope::make(clip_envelope::scaled(*envelope, (double)newSr / oldSr));
        if (c->length >= 0) c->length = end - start;
        c->prepare(newSr);
        c->fitWindow();
    }
}

void Project::retire(std::shared_ptr<plug::Plugin> p) {
    if (!p) return;
    for (auto& r : retired_)
        if (r.second == p) return;  // (retired already: undo brought it back and it went again)
    retired_.emplace_back(-1.0, std::move(p));
}

void Project::purgeRetired(double now) {
    // a plugin is destroyed on the GUI thread 2 s after it was retired (the audio thread let go of it long
    // since), once nothing else holds it (an undo step can still hold its slot)
    for (auto& r : retired_)
        if (r.first < 0) r.first = now;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [now](auto& r) { return now - r.first > 2.0 && r.second.use_count() == 1; }),
                   retired_.end());
}

// --- linked MIDI clips (see model.h) ---

namespace links {

std::vector<ClipPtr> partners(const Project& p, const Clip& c) {
    std::vector<ClipPtr> out;
    if (!c.link) return out;
    for (auto& o : p.clips)
        if (o->link == c.link) out.push_back(o);
    return out;
}

int count(const Project& p, const Clip& c) {
    if (!c.link) return 0;
    int n = 0;
    for (auto& o : p.clips) n += o->link == c.link;
    return n;
}

std::map<const ClipLink*, int> counts(const Project& p) {
    std::map<const ClipLink*, int> out;
    for (auto& c : p.clips)
        if (c->link) out[c->link.get()]++;
    return out;
}

ClipPtr linkedCopy(const ClipPtr& c) {
    if (c->kind != Kind::Midi) return plainCopy(c);
    if (!c->link) c->link = std::make_shared<ClipLink>();
    return copyClip(c);
}

ClipPtr plainCopy(const ClipPtr& c) {
    ClipPtr k = copyClip(c);
    k->link = nullptr;
    return k;
}

std::vector<ClipPtr> notesChanged(Project& p, const ClipPtr& from, int sr) {
    std::vector<ClipPtr> changed;
    if (!from || !from->link || from->kind != Kind::Midi) return changed;
    for (auto& o : partners(p, *from)) {
        if (o == from) continue;
        if (o->notes == from->notes && o->mutedNotes == from->mutedNotes && o->lengthS == from->lengthS &&
            o->loop == from->loop)
            continue;
        if (o->length < 0) o->length = std::max<int64_t>(1, o->frames());  // (its window as it is now)
        o->notes = from->notes;  // (the same immutable vectors: never changed in place)
        o->mutedNotes = from->mutedNotes;
        o->lengthS = from->lengthS;
        o->loop = from->loop;
        // prepared on a copy, then swapped in whole (the audio thread may be playing it)
        Clip ready = *o;
        ready.track = std::atomic_load(&o->track);
        ready.prepare(sr);
        o->baseFrames = ready.baseFrames;
        std::atomic_store(&o->peaks, ready.peaks);
        std::atomic_store(&o->played, ready.played);
        std::atomic_store(&o->events, ready.events);
        std::atomic_store(&o->data, ready.data);
        changed.push_back(o);
    }
    return changed;
}

void makeUnique(Project& p, const std::vector<ClipPtr>& clips) {
    std::set<ClipLink*> touched;
    for (auto& c : clips)
        if (c->link) {
            touched.insert(c->link.get());
            c->link = nullptr;
        }
    if (touched.empty()) return;
    std::map<ClipLink*, std::vector<ClipPtr>> left;
    for (auto& c : p.clips)
        if (c->link && touched.count(c->link.get())) left[c->link.get()].push_back(c);
    for (auto& kv : left)
        if (kv.second.size() == 1) kv.second[0]->link = nullptr;  // (a link of one clip is no link)
}

}  // namespace links
