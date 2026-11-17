#include "model/history.h"

#include <set>

namespace history {

// The data fields of each kind of object a snapshot keeps (history.py's _fields: everything but
// VIEW_FIELDS, TIMING_VIEW_FIELDS and RUNTIME_FIELDS). Left out on purpose:
//   Track: height, showAutomation, showTakes (view); patchState, rackState, controls (runtime)
//   Group: folded (view)
//   PluginSlot: state, instance, failed, error, recent (runtime)
//   Automation: height (view); targetPlugin, targetParam, targetSteps, hasTarget, last (runtime)
//   Timing: beats, thirds, grid, snapOn, looping (view)
//   a looping audio clip: data, peaks, baseFrames (built again from its other fields)
#define TRACK_FIELDS(X)                                                                                         \
    X(name) X(kind) X(muted) X(soloed) X(output) X(volumeDb) X(delayMs) X(pan) X(sends) X(instrument)          \
    X(midiEffects) X(sampler) X(needsInstrument) X(effects) X(cables) X(color) X(tone) X(group) X(frozen)       \
    X(automation) X(takes) X(usesRack) X(rack) X(rackCables)
#define GROUP_FIELDS(X) X(name) X(muted) X(soloed) X(color) X(tone)
#define CLIP_FIELDS(X)                                                                                          \
    X(kind) X(name) X(track) X(start) X(offset) X(length) X(color) X(tone) X(disabled) X(gainDb) X(fadeIn)     \
    X(fadeOut) X(fadeLead) X(fadeTail) X(loop) X(source) X(sourceSr) X(sourcePath) X(stretch) X(pitch)         \
    X(sourceBpm) X(reverse) X(loopStart) X(loopLength) X(notes) X(lengthS) X(mutedNotes) X(played) X(events)  \
    X(link)
#define CLIP_AUDIO_FIELDS(X) X(data) X(peaks) X(baseFrames)  // (not for looping audio clips)
#define SLOT_FIELDS(X) X(path) X(name) X(kind) X(pluginName) X(bypass) X(placed) X(atX) X(atY) X(volumeDb) X(mix)
#define LANE_FIELDS(X) X(slot) X(param) X(points)
#define TIMING_FIELDS(X) X(bpm) X(beatsPerBar) X(loopStart) X(loopEnd) X(changes) X(key) X(scale)

#define DECL_TRACK(f) decltype(Track::f) f;
#define DECL_GROUP(f) decltype(Group::f) f;
#define DECL_CLIP(f) decltype(Clip::f) f;
#define DECL_SLOT(f) decltype(PluginSlot::f) f;
#define DECL_LANE(f) decltype(Automation::f) f;
#define DECL_TIMING(f) decltype(Timing::f) f;
struct TrackData { TRACK_FIELDS(DECL_TRACK) };
struct GroupData { GROUP_FIELDS(DECL_GROUP) };
struct ClipData {
    CLIP_FIELDS(DECL_CLIP)
    CLIP_AUDIO_FIELDS(DECL_CLIP)
    bool looping = false;  // a looping audio clip: its audio isn't kept
};
struct SlotData { SLOT_FIELDS(DECL_SLOT) };
struct LaneData { LANE_FIELDS(DECL_LANE) };
struct TimingData { TIMING_FIELDS(DECL_TIMING) };

namespace {

// field values compared as history.py does: shared audio and notes by identity, the rest by value
template <class T>
bool same(const T& a, const T& b) { return a == b; }
bool same(const SamplerPtr& a, const SamplerPtr& b) { return a == b || (a && b && *a == *b); }

// written back: what the audio thread reads through a shared pointer is replaced atomically
template <class T>
void put(T& dst, const T& src) { dst = src; }
template <class T>
void put(std::shared_ptr<T>& dst, const std::shared_ptr<T>& src) { std::atomic_store(&dst, src); }

#define TAKE(f) d.f = o.f;
#define SAME(f) if (!same(d.f, o.f)) return false;
#define PUT(f) put(o.f, d.f);

TrackData take(const Track& o) { TrackData d; TRACK_FIELDS(TAKE) return d; }
GroupData take(const Group& o) { GroupData d; GROUP_FIELDS(TAKE) return d; }
SlotData take(const PluginSlot& o) { SlotData d; SLOT_FIELDS(TAKE) return d; }
LaneData take(const Automation& o) { LaneData d; LANE_FIELDS(TAKE) return d; }
TimingData take(const Timing& o) { TimingData d; TIMING_FIELDS(TAKE) return d; }
ClipData take(const Clip& o) {
    ClipData d;
    CLIP_FIELDS(TAKE)
    d.looping = o.kind == Kind::Audio && o.loop;
    if (!d.looping) { CLIP_AUDIO_FIELDS(TAKE) }
    return d;
}

bool matches(const TrackData& d, const Track& o) { TRACK_FIELDS(SAME) return true; }
bool matches(const GroupData& d, const Group& o) { GROUP_FIELDS(SAME) return true; }
bool matches(const SlotData& d, const PluginSlot& o) { SLOT_FIELDS(SAME) return true; }
bool matches(const LaneData& d, const Automation& o) { LANE_FIELDS(SAME) return true; }
bool matches(const TimingData& d, const Timing& o) { TIMING_FIELDS(SAME) return true; }
bool matches(const ClipData& d, const Clip& o) {
    CLIP_FIELDS(SAME)
    if ((o.kind == Kind::Audio && o.loop) != d.looping) return false;
    if (!d.looping) { CLIP_AUDIO_FIELDS(SAME) }
    return true;
}

void restore(const TrackData& d, Track& o) { TRACK_FIELDS(PUT) }
void restore(const GroupData& d, Group& o) { GROUP_FIELDS(PUT) }
void restore(const SlotData& d, PluginSlot& o) { SLOT_FIELDS(PUT) }
void restore(const LaneData& d, Automation& o) { LANE_FIELDS(PUT) }
void restore(const TimingData& d, Timing& o) {
    TIMING_FIELDS(PUT)
    o.publish();
}
void restore(const ClipData& d, Clip& o, int sr) {
    if (!d.looping) {
        CLIP_FIELDS(PUT)
        CLIP_AUDIO_FIELDS(PUT)
        return;
    }
    // a looping audio clip: its audio as its fields now say (built again only when they changed)
    bool shapes = o.loop == d.loop && o.loopStart == d.loopStart && o.loopLength == d.loopLength &&
                  o.offset == d.offset && o.stretch == d.stretch && o.pitch == d.pitch && o.reverse == d.reverse &&
                  o.source == d.source;
    CLIP_FIELDS(PUT)
    if (!o.data || !shapes || o.offset + o.frames() > o.data->frames()) o.prepare(sr);
}

#undef TAKE
#undef SAME
#undef PUT

}  // namespace

struct SnapshotData {
    List<TrackPtr> tracks;
    List<ClipPtr> clips;
    List<SlotPtr> masterEffects;
    Cables masterCables;
    List<Marker> markers;
    std::map<std::string, int> tracksMade;
    std::vector<std::pair<TrackPtr, TrackData>> trackData;
    std::vector<std::pair<GroupPtr, GroupData>> groupData;
    std::vector<std::pair<ClipPtr, ClipData>> clipData;
    std::vector<std::pair<SlotPtr, SlotData>> slotData;
    std::vector<std::pair<AutomationPtr, LaneData>> laneData;
    TimingData timing;
};

Snapshot::Snapshot(const Project& p) : d_(std::make_unique<SnapshotData>()) {
    SnapshotData& d = *d_;
    d.tracks = p.tracks;
    d.clips = p.clips;
    d.masterEffects = p.masterEffects;
    d.masterCables = p.masterCables;
    d.markers = p.markers;
    d.tracksMade = p.tracksMade;
    // every object whose fields a snapshot keeps (each once)
    std::set<const void*> seen;
    auto first = [&](const void* o) { return o && seen.insert(o).second; };
    auto slot = [&](const SlotPtr& s) {
        if (first(s.get())) d.slotData.emplace_back(s, take(*s));
    };
    auto clip = [&](const ClipPtr& c) {
        if (first(c.get())) d.clipData.emplace_back(c, take(*c));
    };
    for (auto& t : p.tracks) {
        if (first(t.get())) d.trackData.emplace_back(t, take(*t));
        if (t->group && first(t->group.get())) d.groupData.emplace_back(t->group, take(*t->group));
        if (t->frozen) clip(t->frozen);
        for (auto& lane : t->automation)
            if (first(lane.get())) d.laneData.emplace_back(lane, take(*lane));
        if (t->instrument) slot(t->instrument);
        for (auto& s : t->rack) slot(s);
        for (auto& s : t->effects) slot(s);
    }
    for (auto& c : p.clips) clip(c);
    for (auto& s : p.masterEffects) slot(s);
    d.timing = take(p.timing);
}

Snapshot::~Snapshot() = default;
Snapshot::Snapshot(Snapshot&&) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;

bool Snapshot::matches(const Project& p) const {
    const SnapshotData& d = *d_;
    if (d.tracks != p.tracks || d.clips != p.clips || d.masterEffects != p.masterEffects ||
        d.masterCables != p.masterCables || d.markers != p.markers || d.tracksMade != p.tracksMade)
        return false;
    if (!history::matches(d.timing, p.timing)) return false;
    for (auto& o : d.trackData)
        if (!history::matches(o.second, *o.first)) return false;
    for (auto& o : d.groupData)
        if (!history::matches(o.second, *o.first)) return false;
    for (auto& o : d.clipData)
        if (!history::matches(o.second, *o.first)) return false;
    for (auto& o : d.slotData)
        if (!history::matches(o.second, *o.first)) return false;
    for (auto& o : d.laneData)
        if (!history::matches(o.second, *o.first)) return false;
    return true;
}

void Snapshot::restore(Project& p, int sr) const {
    const SnapshotData& d = *d_;
    for (auto& o : d.trackData) history::restore(o.second, *o.first);
    for (auto& o : d.groupData) history::restore(o.second, *o.first);
    for (auto& o : d.slotData) history::restore(o.second, *o.first);
    for (auto& o : d.laneData) history::restore(o.second, *o.first);
    for (auto& o : d.clipData) history::restore(o.second, *o.first, sr);
    history::restore(d.timing, p.timing);
    p.tracks = d.tracks;
    p.clips = d.clips;
    p.masterEffects = d.masterEffects;
    p.masterCables = d.masterCables;
    p.markers = d.markers;
    p.tracksMade = d.tracksMade;
}

History::History(Project& project, int sr) : project_(project) { reset(sr); }

void History::reset(int sr) {
    sr_ = sr;
    undo_.clear();
    redo_.clear();
    current_ = std::make_unique<Snapshot>(project_);
}

bool History::check(int sr) {
    if (sr != sr_) {
        reset(sr);
        return false;
    }
    if (current_->matches(project_)) return false;
    undo_.push_back(std::move(*current_));
    if ((int)undo_.size() > MAX_STEPS) undo_.erase(undo_.begin(), undo_.end() - MAX_STEPS);
    redo_.clear();
    current_ = std::make_unique<Snapshot>(project_);
    return true;
}

bool History::undo(int sr) {
    check(sr);
    if (undo_.empty()) return false;
    redo_.push_back(std::move(*current_));
    Snapshot step = std::move(undo_.back());
    undo_.pop_back();
    step.restore(project_, sr);
    current_ = std::make_unique<Snapshot>(project_);
    return true;
}

bool History::redo(int sr) {
    check(sr);
    if (redo_.empty()) return false;
    undo_.push_back(std::move(*current_));
    Snapshot step = std::move(redo_.back());
    redo_.pop_back();
    step.restore(project_, sr);
    current_ = std::make_unique<Snapshot>(project_);
    return true;
}

}  // namespace history
