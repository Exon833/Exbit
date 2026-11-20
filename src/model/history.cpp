#include "model/history.h"

#include <set>

namespace history {

// The data fields of each kind of object a snapshot keeps (history.py's _fields: everything but
// VIEW_FIELDS, TIMING_VIEW_FIELDS and RUNTIME_FIELDS). Left out on purpose:
//   Track: height, showAutomation, showTakes, armed (view); patchState, rackState, controls (runtime)
//   Group: folded (view)
//   PluginSlot: state, instance, failed, error, recent (runtime)
//   Automation: height (view); targetPlugin, targetParam, targetSteps, hasTarget, last (runtime)
//   Timing: beats, thirds, grid, snapOn, looping (view)
//   a looping audio clip: data, peaks, baseFrames (built again from its other fields)
#define TRACK_FIELDS(X)                                                                                         \
    X(name) X(kind) X(muted) X(soloed) X(soloSafe) X(output) X(volumeDb) X(delayMs) X(pan) X(sends) X(instrument)          \
    X(midiEffects) X(sampler) X(padKit) X(needsInstrument) X(effects) X(cables) X(modCables) X(color) X(tone)     \
    X(group) X(frozen) X(automation) X(takes) X(usesRack) X(rack) X(rackCables) X(bendRange) \
    X(swing) X(into) X(padOut) X(midiIn) X(midiInChannel) X(ext) X(monitor) X(inputCh) X(inputStereo)
#define GROUP_FIELDS(X) X(name) X(muted) X(soloed) X(color) X(tone) X(foldOnly) X(parent)
#define CLIP_FIELDS(X)                                                                                          \
    X(kind) X(name) X(track) X(start) X(offset) X(length) X(color) X(tone) X(disabled) X(gainDb) X(fadeIn)     \
    X(fadeOut) X(fadeLead) X(fadeTail) X(fadeInShape) X(fadeOutShape) X(loop) X(source) X(sourceSr)            \
    X(sourcePath) X(stretch) X(pitch) X(sourceBpm) X(reverse) X(loopStart) X(loopLength) X(notes) X(lengthS)   \
    X(mutedNotes) X(played) X(events) X(link) X(gainEnvelope) X(key) X(scale) X(effectsCommitted) X(warp)        \
    X(warpMode) X(warpPreserve) X(warpMarkers)
#define CLIP_AUDIO_FIELDS(X) X(data) X(peaks) X(baseFrames) X(warpKey)  // (not for looping audio clips)
#define SLOT_FIELDS(X) X(path) X(name) X(kind) X(pluginName) X(bypass) X(placed) X(atX) X(atY) X(volumeDb) X(mix)
#define LANE_FIELDS(X) X(slot) X(param) X(points)
#define TIMING_FIELDS(X)                                                                                         \
    X(bpm) X(beatsPerBar) X(loopStart) X(loopEnd) X(changes) X(ramp) X(timebaseBeats) X(key) X(scale) X(swing) X(swingGrid) X(automationFollows)

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
bool same(const PadKitPtr& a, const PadKitPtr& b) { return a == b || (a && b && *a == *b); }

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
                  o.source == d.source && o.warp == d.warp && o.warpMode == d.warpMode &&
                  o.warpPreserve == d.warpPreserve && o.warpMarkers == d.warpMarkers &&
                  (!d.warp || o.start == d.start);  // (a warped one's audio is rendered for where it starts)
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
    for (auto& g : p.groups())  // (nested groups too, and their buses below as tracks: model.h GROUP BUSES)
        if (first(g.get())) d.groupData.emplace_back(g, take(*g));
    for (auto& t : p.allTracks()) {
        if (first(t.get())) d.trackData.emplace_back(t, take(*t));
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
    history::restore(d.timing, p.timing);  // (first: warped clips prepared again follow its tempo)
    for (auto& o : d.clipData) history::restore(o.second, *o.first, sr);
    p.tracks = d.tracks;
    p.clips = d.clips;
    p.masterEffects = d.masterEffects;
    p.masterCables = d.masterCables;
    p.markers = d.markers;
    p.tracksMade = d.tracksMade;
}

// --- what a step did, by name ---
namespace {

// a name and its plural (a change to several objects of a kind)
struct Name {
    const char* one;
    const char* many;
};

// the first of a track's fields that changed, by name (nullptr: none)
const char* trackChange(const TrackData& d, const Track& o) {
    if (!same(d.volumeDb, o.volumeDb)) return "Volume";
    if (!same(d.pan, o.pan)) return "Pan";
    if (!same(d.muted, o.muted)) return "Mute";
    if (!same(d.soloed, o.soloed)) return "Solo";
    if (!same(d.soloSafe, o.soloSafe)) return "Solo safe";
    if (!same(d.name, o.name)) return "Rename track";
    if (!same(d.sends, o.sends)) return "Sends";
    if (!same(d.effects, o.effects) || !same(d.cables, o.cables)) return "Effects";
    if (!same(d.automation, o.automation)) return "Automation";
    if (!same(d.frozen, o.frozen)) return "Freeze";
    if (!same(d.instrument, o.instrument) || !same(d.rack, o.rack) || !same(d.rackCables, o.rackCables) ||
        !same(d.usesRack, o.usesRack) || !same(d.sampler, o.sampler) || !same(d.padKit, o.padKit) ||
        !same(d.needsInstrument, o.needsInstrument))
        return "Instrument";
    if (!same(d.midiEffects, o.midiEffects)) return "MIDI effects";
    if (!same(d.color, o.color) || !same(d.tone, o.tone)) return "Track colour";
    if (!same(d.group, o.group)) return "Group tracks";
    if (!same(d.into, o.into)) return "Track output";
    if (!same(d.padOut, o.padOut)) return "Pad output";
    if (!same(d.takes, o.takes)) return "Takes";
    if (!same(d.output, o.output)) return "Track output";
    if (!same(d.delayMs, o.delayMs)) return "Track delay";
    if (!same(d.bendRange, o.bendRange)) return "Pitch bend range";
    if (!same(d.swing, o.swing)) return "Swing";
    if (!same(d.midiIn, o.midiIn) || !same(d.midiInChannel, o.midiInChannel)) return "MIDI input";
    if (!same(d.ext, o.ext)) return "External synth";
    if (!same(d.monitor, o.monitor)) return "Monitoring";
    if (!same(d.inputCh, o.inputCh) || !same(d.inputStereo, o.inputStereo)) return "Track input";
    return matches(d, o) ? nullptr : "Track settings";
}

// the first of a clip's fields that changed, by name ({nullptr, nullptr}: none)
Name clipChange(const ClipData& d, const Clip& o) {
    if (!same(d.track, o.track) || !same(d.start, o.start)) return {"Move clip", "Move clips"};
    // (before the window: warping changes it too)
    if (!same(d.warp, o.warp) || !same(d.warpMode, o.warpMode) || !same(d.warpPreserve, o.warpPreserve))
        return {"Warp", "Warp"};
    if (!same(d.warpMarkers, o.warpMarkers)) return {"Warp markers", "Warp markers"};
    if (!same(d.fadeIn, o.fadeIn) || !same(d.fadeOut, o.fadeOut) || !same(d.fadeLead, o.fadeLead) ||
        !same(d.fadeTail, o.fadeTail) || !same(d.fadeInShape, o.fadeInShape) || !same(d.fadeOutShape, o.fadeOutShape))
        return {"Fades", "Fades"};
    if (!same(d.loop, o.loop) || !same(d.loopStart, o.loopStart) || !same(d.loopLength, o.loopLength))
        return {"Loop", "Loop"};
    if (!same(d.offset, o.offset) || !same(d.length, o.length)) return {"Resize clip", "Resize clips"};
    if (!same(d.gainDb, o.gainDb)) return {"Clip gain", "Clip gain"};
    if (!same(d.gainEnvelope, o.gainEnvelope)) return {"Gain envelope", "Gain envelopes"};
    if (!same(d.name, o.name)) return {"Rename clip", "Rename clips"};
    if (!same(d.notes, o.notes) || !same(d.mutedNotes, o.mutedNotes) || !same(d.lengthS, o.lengthS))
        return {"Edit notes", "Edit notes"};
    if (!same(d.color, o.color) || !same(d.tone, o.tone)) return {"Clip colour", "Clip colours"};
    if (!same(d.disabled, o.disabled))
        return o.disabled ? Name{"Disable clip", "Disable clips"} : Name{"Enable clip", "Enable clips"};
    if (!same(d.stretch, o.stretch) || !same(d.pitch, o.pitch) || !same(d.sourceBpm, o.sourceBpm))
        return {"Stretch / pitch", "Stretch / pitch"};
    if (!same(d.reverse, o.reverse)) return {"Reverse", "Reverse"};
    if (!same(d.key, o.key) || !same(d.scale, o.scale)) return {"Clip scale", "Clip scales"};
    if (!matches(d, o)) return {"Edit clip", "Edit clips"};
    return {nullptr, nullptr};
}

// the first of a plugin slot's fields that changed, by name (nullptr: none)
const char* slotChange(const SlotData& d, const PluginSlot& o) {
    if (!same(d.bypass, o.bypass)) return "Bypass";
    if (!same(d.placed, o.placed) || !same(d.atX, o.atX) || !same(d.atY, o.atY)) return "Move effect";
    if (!same(d.volumeDb, o.volumeDb) || !same(d.mix, o.mix)) return "Effect level";
    return matches(d, o) ? nullptr : "Effect settings";
}

// added and removed objects between two lists, by identity
template <class T>
void addedRemoved(const List<T>& before, const List<T>& after, size_t& added, size_t& removed) {
    std::set<const void*> a, b;
    for (auto& x : before) a.insert(x.get());
    for (auto& x : after) b.insert(x.get());
    added = removed = 0;
    for (auto* x : b) added += !a.count(x);
    for (auto* x : a) removed += !b.count(x);
}

}  // namespace

std::string Snapshot::describe(const Project& p) const {
    const SnapshotData& d = *d_;
    if (d.tracks != p.tracks) {  // tracks added, removed or moved
        size_t added, removed;
        addedRemoved(d.tracks, p.tracks, added, removed);
        if (added && !removed) return added > 1 ? "Add tracks" : "Add track";
        if (removed && !added) return removed > 1 ? "Delete tracks" : "Delete track";
        if (!added && !removed) return "Move tracks";
        return "Edit tracks";
    }
    if (d.clips != p.clips) {  // clips added or removed (a split, a join, a paste…)
        size_t added, removed;
        addedRemoved(d.clips, p.clips, added, removed);
        if (added && !removed) return added > 1 ? "Add clips" : "Add clip";
        if (removed && !added) return removed > 1 ? "Delete clips" : "Delete clip";
        if (added && removed) return "Edit clips";
        // (the same clips in another order: their fields tell more, below)
    }
    if (d.masterEffects != p.masterEffects || d.masterCables != p.masterCables) return "Master effects";
    {  // (before the markers: with the timebase in beats a tempo edit moves them too)
        const TimingData& t = d.timing;
        const Timing& o = p.timing;
        if (!same(t.bpm, o.bpm) || !same(t.beatsPerBar, o.beatsPerBar) || !same(t.changes, o.changes) ||
            !same(t.ramp, o.ramp))
            return "Tempo";
        if (!same(t.timebaseBeats, o.timebaseBeats)) return "Timebase";
    }
    if (d.markers != p.markers) return "Markers";
    {
        const TimingData& t = d.timing;
        const Timing& o = p.timing;
        if (!same(t.loopStart, o.loopStart) || !same(t.loopEnd, o.loopEnd)) return "Loop range";
        if (!same(t.key, o.key) || !same(t.scale, o.scale)) return "Key";
        if (!same(t.swing, o.swing) || !same(t.swingGrid, o.swingGrid)) return "Swing";
        if (!same(t.automationFollows, o.automationFollows)) return "Automation follows clips";
    }
    for (auto& t : d.trackData)  // (before the clips: a track's swing prepares its clips again)
        if (!same(t.second.swing, t.first->swing)) return "Swing";
    // the clips' fields: the first change found, named for as many clips as had that same change
    Name clipName{nullptr, nullptr};
    int clipCount = 0;
    for (auto& c : d.clipData) {
        Name n = clipChange(c.second, *c.first);
        if (!n.one) continue;
        if (!clipName.one) clipName = n;
        if (n.one == clipName.one) clipCount++;
    }
    if (clipName.one) return clipCount > 1 ? clipName.many : clipName.one;
    for (auto& t : d.trackData)
        if (const char* n = trackChange(t.second, *t.first)) return n;
    for (auto& g : d.groupData) {
        const GroupData& gd = g.second;
        const Group& o = *g.first;
        if (!same(gd.muted, o.muted)) return "Mute";
        if (!same(gd.soloed, o.soloed)) return "Solo";
        if (!same(gd.name, o.name)) return "Rename group";
        if (!same(gd.foldOnly, o.foldOnly)) return o.foldOnly ? "Fold only" : "Group bus";
        if (!same(gd.parent, o.parent)) return "Group tracks";
        if (!history::matches(gd, o)) return "Group colour";
    }
    for (auto& s : d.slotData)
        if (const char* n = slotChange(s.second, *s.first)) return n;
    for (auto& l : d.laneData)
        if (!history::matches(l.second, *l.first)) return "Automation";
    if (d.tracksMade != p.tracksMade) return "Add track";
    return "Edit";
}

History::History(Project& project, int sr) : project_(project) { reset(sr); }

void History::reset(int sr) {
    sr_ = sr;
    undo_.clear();
    redo_.clear();
    undoLabels_.clear();
    redoLabels_.clear();
    nextLabel_.clear();
    current_ = std::make_unique<Snapshot>(project_);
    changes_++;
}

bool History::check(int sr) {
    if (sr != sr_) {
        reset(sr);
        return false;
    }
    if (current_->matches(project_)) return false;
    undoLabels_.push_back(nextLabel_.empty() ? current_->describe(project_) : nextLabel_);
    nextLabel_.clear();
    undo_.push_back(std::move(*current_));
    if ((int)undo_.size() > MAX_STEPS) {
        undo_.erase(undo_.begin(), undo_.end() - MAX_STEPS);
        undoLabels_.erase(undoLabels_.begin(), undoLabels_.end() - MAX_STEPS);
    }
    redo_.clear();
    redoLabels_.clear();
    current_ = std::make_unique<Snapshot>(project_);
    changes_++;
    return true;
}

bool History::undo(int sr) {
    check(sr);
    if (undo_.empty()) return false;
    redo_.push_back(std::move(*current_));
    redoLabels_.push_back(undoLabels_.back());
    undoLabels_.pop_back();
    Snapshot step = std::move(undo_.back());
    undo_.pop_back();
    step.restore(project_, sr);
    current_ = std::make_unique<Snapshot>(project_);
    changes_++;
    return true;
}

bool History::redo(int sr) {
    check(sr);
    if (redo_.empty()) return false;
    undo_.push_back(std::move(*current_));
    undoLabels_.push_back(redoLabels_.back());
    redoLabels_.pop_back();
    Snapshot step = std::move(redo_.back());
    redo_.pop_back();
    step.restore(project_, sr);
    current_ = std::make_unique<Snapshot>(project_);
    changes_++;
    return true;
}

std::vector<std::string> History::labels() const {
    std::vector<std::string> all = undoLabels_;
    all.insert(all.end(), redoLabels_.rbegin(), redoLabels_.rend());  // (redo_'s last is the next to redo)
    return all;
}

bool History::jumpTo(int position, int sr) {
    check(sr);  // (a change not yet noted is a step of its own; another rate starts afresh)
    int total = (int)(undo_.size() + redo_.size());
    if (position < 0 || position > total || position == (int)undo_.size()) return false;
    while ((int)undo_.size() > position && undo(sr)) {
    }
    while ((int)undo_.size() < position && redo(sr)) {
    }
    return true;
}

}  // namespace history
