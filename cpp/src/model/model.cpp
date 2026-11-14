#include "model/model.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "audio/stretch.h"
#include "plugins/plugin.h"

namespace clip_hooks {
std::function<std::vector<Note>(const Track&, const std::vector<Note>&, double)> midiEffects;
std::function<AudioPtr(const Track&, const std::vector<Note>&, int, double)> renderSampler;
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
      instance(std::atomic_load(&o.instance)), failed(o.failed.load()), error(o.error), recent(o.recent),
      placed(o.placed), atX(o.atX), atY(o.atY), volumeDb(o.volumeDb), mix(o.mix) {}

std::string PluginSlot::currentState() const {
    auto p = std::atomic_load(&instance);
    if (!p) return state;
    return plug::packState(p->componentState(), p->controllerState());
}

void Automation::setPoints(std::vector<AutoPoint> pts) {
    for (auto& p : pts) {
        p.t = std::max(0.0, p.t);
        p.v = std::min(1.0, std::max(0.0, p.v));
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

// --- tracks ---

Track::Track(std::string n, Kind k)
    : name(std::move(n)), kind(k), controls([] {
          auto s = std::make_shared<PluginSlot>();
          s->kind = PluginSlot::CONTROLS;
          s->name = "Track volume and pan";
          return s;
      }()) {}

std::vector<SlotPtr> Track::instruments() const {
    if (kind != Kind::Midi || needsInstrument) return {};
    if (usesRack) return rack.copy();
    if (instrument) return {instrument};
    return {};
}

bool Track::playsPlugins() const {
    if (kind != Kind::Midi || needsInstrument) return false;
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
        AudioPtr base = source;
        if (source && (stretch != 1.0 || pitch != 0.0 || reverse))  // (at its own rate, so its pieces share it)
            base = ::stretch::stretched(source, stretch, pitch, reverse);
        AudioPtr d = dsp::resample(base, sourceSr, sr);
        baseFrames = d ? d->frames() : 0;
        if (loop && loopLength > 0 && d && d->frames()) {
            int64_t len = d->frames();
            int64_t a = std::min(len - 1, std::max<int64_t>(0, (int64_t)std::nearbyint(loopStart * sr)));
            int64_t n = std::max<int64_t>(1, std::min(len - a, (int64_t)std::nearbyint(loopLength * sr)));
            if (reach < 0) reach = offset + (length >= 0 ? length : len - offset);
            d = looped(d, a, n, reach, (int64_t)std::nearbyint(config::LOOP_SEAM_S * sr));
        }
        data = d;
        peaks = data ? dsp::waveformPeaks(*data, config::PEAK_BLOCK) : nullptr;
        return;
    }
    std::vector<Note> notesPlayed = repeated(sr);
    TrackPtr t = std::atomic_load(&track);
    if (t && !t->midiEffects.empty() && clip_hooks::midiEffects)  // (its track's MIDI effects: arpeggiator, chord…)
        notesPlayed = clip_hooks::midiEffects(*t, notesPlayed, (double)(start - offset) / sr);
    double len = loop && length >= 0 ? std::max(lengthS, (double)(offset + length) / sr) : lengthS;
    if (t && t->playsPlugins()) {  // (its instrument plugins play its events: as long, silent)
        auto silent = std::make_shared<AudioBuffer>();
        silent->samples.assign((size_t)((int64_t)((len + synth::RELEASE_S) * sr) + 1) * 2, 0.0f);
        data = silent;
    } else if (t && t->playsSampler() && clip_hooks::renderSampler) {
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
    events = ev;
    played = std::make_shared<const std::vector<Note>>(std::move(notesPlayed));
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
    if (fi) level *= std::min(1.0, std::max(0.0, (double)(pos + std::max<int64_t>(0, fadeLead) + 1) / fi));
    if (fo) level *= std::min(1.0, std::max(0.0, (double)(frames() + std::max<int64_t>(0, fadeTail) - pos) / fo));
    return (float)level;
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

std::vector<Note> Clip::repeated(int sr) const {
    static const std::vector<Note> none;
    const std::vector<Note>& ns = notes ? *notes : none;
    double reach = length >= 0 ? (double)(offset + length) / sr : lengthS;
    if (!loop || lengthS <= 0 || reach <= lengthS || ns.empty()) return ns;
    std::vector<Note> one;
    for (auto& n : ns)
        if (n.start < lengthS) one.push_back(n);
    int64_t passes = (int64_t)std::ceil(reach / lengthS);
    std::vector<Note> out;
    for (int64_t k = 0; k < passes; k++)
        for (auto n : one) {
            n.start += k * lengthS;
            n.end += k * lengthS;
            if (n.start < reach) out.push_back(n);
        }
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
}

void Project::forgetGoneTracks() {
    std::set<Track*> present;
    for (auto& t : tracks) present.insert(t.get());
    for (auto& t : tracks) {  // (the sends to returns that are gone)
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
    for (auto& t : tracks) clean(t->cables);
    clean(masterCables);
}

std::vector<SlotPtr> Project::pluginSlots() const {
    std::vector<SlotPtr> out;
    for (auto& t : tracks) {
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
    for (auto& t : tracks)
        if (t->group && std::find(out.begin(), out.end(), t->group) == out.end()) out.push_back(t->group);
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
    for (auto& t : ts)
        if (t->kind != Kind::Return) t->group = g;  // (return tracks stay out of groups)
    gatherGroups();
    return g;
}

void Project::ungroup(const GroupPtr& g) {
    for (auto& t : members(g)) t->group = nullptr;
}

void Project::gatherGroups() {
    std::vector<TrackPtr> order;
    std::set<Group*> done;
    for (auto& t : tracks) {
        if (!t->group) {
            order.push_back(t);
        } else if (!done.count(t->group.get())) {
            done.insert(t->group.get());
            for (auto& m : members(t->group)) order.push_back(m);
        }
    }
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

bool Project::audibleIn(const Track& track, const std::vector<TrackPtr>& ts) {
    Group* g = track.group.get();
    if (track.muted || (g && g->muted) || track.needsInstrument) return false;
    if (track.soloed || (g && g->soloed) || track.kind == Kind::Return) return true;  // (soloing a track keeps the returns)
    for (auto& t : ts)
        if (t->kind != Kind::Return && (t->soloed || (t->group && t->group->soloed))) return false;
    return true;
}

bool Project::audible(const Track& t) const { return audibleIn(t, *tracks); }

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
