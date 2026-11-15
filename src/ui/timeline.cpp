// timeline.py, part 1: coordinates, layout, tracks, clip edits, transport and zoom.
// (Painting: timeline_paint.cpp; mouse, keys, menus and drops: timeline_input.cpp.)
#include "ui/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "audio/file_io.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "model/edits.h"
#include "ui/color_picker.h"
#include "ui/colors.h"
#include "ui/master_lane.h"
#include "ui/toolkit/dialogs.h"

namespace {
// a viewport resize filter that outlives nothing: it checks the timeline is still there
struct FilterGuard {
    bool alive = true;
};
}  // namespace

Timeline::Timeline(Project& project_, audio::Engine& engine_, ui::ScrollArea* scroll_)
    : project(project_), engine(engine_), scroll(scroll_) {
    setAcceptDrops(true);
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMouseTracking(true);  // to show which zone of a clip is under the mouse
    hPolicy = vPolicy = ui::SizePolicy::Fixed;
    hbar_ = scroll->horizontalScrollBar();
    hscrollId_ = hbar_->valueChanged.connect([this](int v) { onHscroll(v); });
    ownedMaster_ = std::make_unique<MasterLaneView>(this);  // the main window pins it to the bottom of the view
    masterView = ownedMaster_.get();
    // re-fit when the view changes size (chained after any filter already there)
    auto guard = std::make_shared<FilterGuard>();
    guard_ = guard;
    ui::Widget* vp = scroll->viewport();
    auto previous = vp->onEventFilter;
    vp->onEventFilter = [this, guard, previous, vp](ui::Widget* w, const char* what) {
        bool swallow = previous && previous(w, what);
        if (guard->alive && w == vp && std::strcmp(what, "resize") == 0) relayout();
        return swallow;
    };
    relayout();
}

Timeline::~Timeline() {
    if (auto g = std::static_pointer_cast<FilterGuard>(guard_)) g->alive = false;
    if (ui::Widget::alive(hbar_)) hbar_->valueChanged.disconnect(hscrollId_);
}

std::unique_ptr<MasterLaneView> Timeline::takeMasterView() { return std::move(ownedMaster_); }

bool Timeline::masterAlive() const {
    return masterView && (ownedMaster_.get() == masterView || ui::Widget::alive(masterView));
}

void Timeline::update() {
    Widget::update();
    if (masterAlive()) masterView->update();
}

void Timeline::onHscroll(int value) {
    // Qt moves the already-drawn pixels and repaints only the clips' name strips (their names stick to
    // the view's left edge); the toolkit repaints the whole view when its content moves, so only the
    // master lane, which isn't scrolled, needs drawing again at the new offset.
    int old = viewLeft;
    viewLeft = value;
    if (old == value) return;
    if (masterAlive()) masterView->update();
}

// --- coordinates --------------------------------------------------------------------------------

double Timeline::xOf(int64_t frame) const { return (double)frame / engine.sr * pps; }

int64_t Timeline::frameAt(double x) const { return (int64_t)(x / pps * engine.sr); }

int64_t Timeline::snappedFrameAt(double x, int mods) const {
    const Timing& t = project.timing;
    if (!t.snaps(mods & plat::ModAlt)) return std::max<int64_t>(0, frameAt(x));
    return (int64_t)std::nearbyint(t.snap(x / pps, pps) * engine.sr);
}

std::vector<Lane> Timeline::lanes() const { return lanes::layout(project, config::RULER_H); }

std::map<const Track*, int> Timeline::trackTops() const {
    std::map<const Track*, int> tops;
    for (auto& lane : lanes())
        if (lane.track) tops[lane.track.get()] = lane.top;
    return tops;
}

std::vector<TrackPtr> Timeline::visibleTracks() const {
    std::vector<TrackPtr> out;
    for (auto& lane : lanes())
        if (lane.track) out.push_back(lane.track);
    return out;
}

std::optional<Lane> Timeline::laneAt(double y) const {
    if (y < config::RULER_H) return std::nullopt;
    auto ls = lanes();
    for (auto& lane : ls)
        if (y < lane.bottom()) return lane;
    return ls.back();
}

TrackPtr Timeline::trackAt(double y) const {
    auto lane = laneAt(y);
    return lane ? lane->track : nullptr;
}

std::pair<TrackPtr, AutomationPtr> Timeline::laneEdgeAt(double y) const {
    if (y < config::RULER_H) return {};
    for (auto& lane : lanes())
        if ((lane.track || lane.automation) && std::fabs(y - lane.bottom()) <= config::LANE_EDGE_GRAB_PX)
            return {lane.track, lane.automation};
    return {};
}

std::optional<RectF> Timeline::clipRect(const Clip& clip, const std::map<const Track*, int>& tops) const {
    auto it = tops.find(clip.track.get());
    if (it == tops.end()) return std::nullopt;
    double x0 = xOf(clip.start), x1 = xOf(clip.end());
    return RectF((float)x0, (float)(it->second + 2), (float)(x1 - x0), (float)(clip.track->height - 4));
}

namespace {
// QRectF::contains: the edges count
bool containsQt(const RectF& r, PointF p) { return p.x >= r.x && p.x <= r.right() && p.y >= r.y && p.y <= r.bottom(); }
}  // namespace

std::pair<ClipPtr, ClipZone> Timeline::hit(PointF pos) const {
    if (pos.y < config::RULER_H) return {nullptr, ClipZone::None};
    auto tops = trackTops();
    auto& clips = *project.clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
        auto rect = clipRect(**it, tops);
        if (rect && !(*it)->track->frozen && containsQt(rect->adjusted(0, -2, 0, 2), pos))
            return {*it, pos.y < rect->y + config::CLIP_HANDLE_H ? ClipZone::Handle : ClipZone::Body};
    }
    return {nullptr, ClipZone::None};
}

std::pair<ClipPtr, ClipSide> Timeline::edgeAt(PointF pos) const {
    if (pos.y < config::RULER_H) return {nullptr, ClipSide::None};
    auto tops = trackTops();
    const float grab = config::CLIP_EDGE_PX;
    auto& clips = *project.clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
        auto rect = clipRect(**it, tops);
        if (!rect || (*it)->track->frozen || rect->w < 3 * grab || !containsQt(rect->adjusted(0, -2, 0, 2), pos))
            continue;
        if (pos.x <= rect->x + grab) return {*it, ClipSide::Left};
        if (pos.x >= rect->right() - grab) return {*it, ClipSide::Right};
    }
    return {nullptr, ClipSide::None};
}

void Timeline::dragEdge(PointF pos, int mods) {
    // Move the grabbed end of a clip to the mouse (snapped): no shorter than CLIP_MIN_S, not past its
    // audio (an audio clip), not before its start (offset 0). The clip is changed in place, one value
    // at a time in an order the audio thread can always play from (at worst a buffer of the old window).
    auto& d = std::get<EdgeDrag>(drag);
    Clip& clip = *d.clip;
    int sr = engine.sr;
    int64_t frame = snappedFrameAt(pos.x, mods);
    int64_t shortest = std::max<int64_t>(1, (int64_t)std::nearbyint(config::CLIP_MIN_S * sr));
    if (d.side == ClipSide::Right) {
        int64_t end = std::max(clip.start + shortest, frame);
        int64_t have = clip.data ? clip.data->frames() : 0;
        if (clip.kind == Kind::Audio && clip.loop) {  // (its loop repeats: its audio is built a little ahead of its end)
            if (clip.offset + end - clip.start > have)
                prepareLive(d.clip, clip.offset + end - clip.start + (int64_t)std::nearbyint(config::LOOP_DRAG_AHEAD_S * sr));
        } else if (clip.kind == Kind::Audio) {
            end = std::min(end, clip.start + have - clip.offset);
        }
        clip.length = end - clip.start;
    } else {
        int64_t end = clip.end();
        int64_t start = std::max({clip.start - clip.offset, (int64_t)0, std::min(frame, end - shortest)});
        int64_t moved = start - clip.start;
        if (moved > 0) {  // shorter first, then the window and the start
            clip.length = clip.frames() - moved;
            clip.offset += moved;
            clip.start = start;
        } else if (moved < 0) {
            clip.offset += moved;
            clip.start = start;
            clip.length = end - start;
        }
    }
    relayout();
}

void Timeline::endEdgeDrag() {
    // A MIDI clip made longer than its notes were rendered: render it that long; then the clip
    // replaces what it now covers.
    ClipPtr clip = std::get<EdgeDrag>(drag).clip;
    int sr = engine.sr;
    int64_t have = clip->data ? clip->data->frames() : 0;
    int64_t spare = have - clip->offset - clip->frames();
    if (clip->kind == Kind::Midi && clip->loop) {  // (its notes repeat as far as it now reaches)
        prepareLive(clip);
    } else if (clip->loop && !(0 <= spare && spare <= timeline::LOOP_SLACK_S * sr)) {
        prepareLive(clip);  // (a loop: its audio built again only when it grew past it, or holds much more than it plays)
    } else if (clip->kind == Kind::Midi && clip->offset + clip->frames() > have) {
        clip->lengthS = std::max(clip->lengthS, (double)(clip->offset + clip->frames()) / sr);
        prepareLive(clip);
    }
    edits::clearUnder(project, {clip});
    relayout();
}

void Timeline::changeSampleRate(int oldSr, int newSr) {
    cursor = cursor * newSr / oldSr;
    if (selection.range) {
        selection.range->start = selection.range->start * newSr / oldSr;
        selection.range->end = selection.range->end * newSr / oldSr;
    }
    relayout();
}

double Timeline::endSeconds() const {
    return (double)std::max({project.endFrame(), (int64_t)engine.pos.load(), cursor}) / engine.sr;
}

double Timeline::maxPps() const {
    // MAX_PPS, or less for a long song, so the whole of it fits in the widest a widget may be
    return std::max(config::MIN_PPS,
                    std::min(config::MAX_PPS, timeline::WIDEST_PX / (endSeconds() + config::EMPTY_SECONDS_AFTER_END)));
}

std::vector<Rect> Timeline::strips(std::vector<Rect> region) {
    // each pass costs a whole ruler, grid and loop over the clips; while following playback a frame brings
    // many small ones (the playhead's strips, a clip name per long clip under the view's left edge)
    std::sort(region.begin(), region.end(), [](const Rect& a, const Rect& b) { return a.left() < b.left(); });
    std::vector<Rect> columns;
    for (auto& r : region) {
        if (!columns.empty() && r.left() <= columns.back().right() - 1 + STRIP_GAP_PX)
            columns.back() = columns.back().united(r);
        else
            columns.push_back(r);
    }
    return columns;
}

void Timeline::relayout() {
    if (currentTrack && !project.tracks.contains(currentTrack))
        setCurrentTrack(nullptr);  // (it was removed, undone or replaced: nothing may go onto it any more)
    pps = std::min(pps, maxPps());  // (the song grew past what this zoom can show)
    double end = endSeconds();
    ui::Widget* vp = scroll->viewport();
    int width = std::max((int)((end + config::EMPTY_SECONDS_AFTER_END) * pps), vp->width());
    // at least as tall as the view; with the master lane under the view its place at the bottom of the
    // lanes is left out (so it scrolls as far as the panel, whose master row lies over its tracks)
    const Lane master = lanes().back();  // (a copy: lanes() returns a new vector)
    setFixedSize(width, std::max(masterBelowView ? master.top : master.bottom(), vp->height()));
    if (scroll->content() == this) scroll->place();  // the scroll bars follow at once (zoom sets them next)
    playheadX = (int)xOf(engine.pos.load());
    cursorX = (int)xOf(cursor);
    update();
}

// --- tracks -------------------------------------------------------------------------------------

TrackPtr Timeline::addTrack(Kind kind, GroupPtr group) {
    TrackPtr track = project.addTrack(kind, group);
    if (randomTrackColors) {  // (Options → Colours)
        std::vector<std::string> taken;
        for (auto& t : project.tracks) taken.push_back(t->color);
        track->color = colors::randomColor(taken);
    }
    relayout();
    tracksChanged.emit();
    return track;
}

void Timeline::tracksRegrouped() {
    std::set<const Track*> shown;
    for (auto& t : visibleTracks())
        if (!t->frozen) shown.insert(t.get());
    std::vector<ClipPtr> gone;
    for (auto& c : selection.clips)
        if (!shown.count(c->track.get())) gone.push_back(c);
    selection.forget(gone);
    if (selection.range) {
        std::vector<TrackPtr> kept;
        for (auto& t : selection.range->tracks)
            if (shown.count(t.get())) kept.push_back(t);
        selection.range->tracks = kept;
        if (kept.empty()) selection.range.reset();
    }
    relayout();
    tracksChanged.emit();
}

GroupPtr Timeline::groupTracks(const std::vector<TrackPtr>& tracks) {
    GroupPtr group = project.groupTracks(tracks);
    tracksRegrouped();
    return group;
}

void Timeline::groupSelected() {
    std::set<const Track*> tracks;
    if (selection.range) {
        for (auto& t : selection.range->tracks) tracks.insert(t.get());
    } else {
        for (auto& c : selection.clips) tracks.insert(c->track.get());
    }
    if (tracks.empty()) return;
    std::vector<TrackPtr> ordered;
    for (auto& t : project.tracks)
        if (tracks.count(t.get())) ordered.push_back(t);
    groupTracks(ordered);
}

void Timeline::setGroup(const TrackPtr& track, const GroupPtr& group) {
    track->group = group;
    project.gatherGroups();
    tracksRegrouped();
}

void Timeline::ungroup(const GroupPtr& group) {
    project.ungroup(group);
    tracksRegrouped();
}

void Timeline::toggleFold(const GroupPtr& group) {
    group->folded = !group->folded;
    tracksRegrouped();
}

void Timeline::setTrackHeight(Track& track, double height) {
    track.height = std::min(config::MAX_TRACK_H, std::max(config::MIN_TRACK_H, (int)height));
    relayout();
    heightsChanged.emit();
}

void Timeline::setTrackHeight(Automation& lane, double height) {
    lane.height = std::min(config::AUTOMATION_MAX_H, std::max(config::AUTOMATION_MIN_H, (int)height));
    relayout();
    heightsChanged.emit();
}

void Timeline::removeTrack(const TrackPtr& track) {
    auto clips = project.clipsOn(track);
    if (!clips.empty() &&
        ui::msg::question(dialogParent(), "Remove track",
                          "Remove " + track->name + " and its " + std::to_string(clips.size()) + " clip(s)?") !=
            ui::msg::Yes)
        return;
    selection.forget(clips, track);
    project.removeTrack(track);
    relayout();
    tracksChanged.emit();
}

// --- clips --------------------------------------------------------------------------------------

ClipPtr Timeline::loadClip(const std::string& path) {
    std::string name = util::baseName(path), error;
    auto clip = std::make_shared<Clip>();
    clip->name = name;
    if (file_io::isMidi(path)) {  // (in beats: placeClip puts them on the song's beats where it is dropped)
        std::vector<Note> notes;
        double beats = 0;
        if (file_io::loadMidiBeats(path, notes, beats, error)) {
            clip->kind = Kind::Midi;
            clip->notes = std::make_shared<const std::vector<Note>>();
            clip->lengthS = 1.0;
            inBeats_[clip.get()] = {std::move(notes), beats};
            return clip;
        }
    } else {
        AudioPtr data;
        int sr = 0;
        if (file_io::loadAudio(path, data, sr, error)) {
            clip->kind = Kind::Audio;
            clip->source = data;
            clip->sourceSr = sr;
            return clip;
        }
    }
    ui::msg::warning(dialogParent(), "Cannot load file", path + "\n\n" + error);
    return nullptr;
}

void Timeline::placeClip(const ClipPtr& clip, int64_t frame, const TrackPtr& track) {
    if (inBeats_.count(clip.get())) fromBeats(clip, std::max<int64_t>(0, frame));  // (a MIDI file: its beats on the song's)
    std::atomic_store(&clip->track, track);
    clip->start = std::max<int64_t>(0, frame);
    clip->prepare(engine.sr);
    project.addClip(clip);
    edits::clearUnder(project, {clip});  // the new clip replaces what it covers
    selection.selectOnly(clip);
    relayout();
}

void Timeline::addFile(const std::string& path, int64_t frame) {
    ClipPtr clip = loadClip(path);
    if (!clip) return;
    TrackPtr track = project.firstEmptyTrack(clip->kind);
    placeClip(clip, frame, track ? track : addTrack(clip->kind));
}

void Timeline::deleteSelected() {
    if (selection.range) {
        TimeRange r = *selection.range;
        edits::deleteRange(project, r.tracks, r.start, r.end);
    } else if (!selection.clips.empty()) {
        project.removeClips(selection.clipList());
    }
    selection.clear();
    relayout();
}

void Timeline::duplicateSelected() {
    if (selection.range) {
        TimeRange r = *selection.range;
        auto copies = edits::duplicateRange(project, r.tracks, r.start, r.end);
        int64_t span = r.end - r.start;
        selection.setRange(TimeRange{r.end, r.end + span, r.tracks}, copies);
    } else if (!selection.clips.empty()) {
        auto copies = edits::duplicateClips(project, selection.clipList());
        selection.clips = std::set<ClipPtr>(copies.begin(), copies.end());
    } else {
        return;
    }
    std::map<ClipPtr, TrackPtr> tracks;
    std::map<ClipPtr, std::optional<int64_t>> starts;
    for (auto& c : selection.clips) {
        tracks[c] = nullptr;  // (copies: rendered for their track as it plays notes now)
        starts[c] = std::nullopt;
    }
    rerenderMoved(tracks, starts);
    relayout();
}

void Timeline::splitSelected() {
    if (selection.range) {
        TimeRange r = *selection.range;
        auto pieces = edits::splitClips(project, project.clipsIn(r.tracks, r.start, r.end), {r.start, r.end});
        selection.clips.clear();
        for (auto& p : pieces)
            if (p->start >= r.start && p->end() <= r.end) selection.clips.insert(p);
    } else if (!selection.clips.empty()) {
        auto pieces = edits::splitClips(project, selection.clipList(), {cursor});
        selection.clips = std::set<ClipPtr>(pieces.begin(), pieces.end());
    }
    update();
}

void Timeline::chooseColor() {
    if (selection.clips.empty()) return;
    const ClipPtr& first = *selection.clips.begin();
    std::string color, tone;
    colors::clipColor(*first, color, tone);
    ColorPickerDialog dialog(dialogParent(), color, first->tone, ColorSubject::Clip);
    if (dialog.exec() == ui::Dialog::Accepted) {
        for (auto& clip : selection.clips) {
            clip->color = dialog.chosen;  // "" = back to the default for its kind
            clip->tone = dialog.tone;     // "" = automatic waveform/name colour
        }
        update();
    }
}

ClipPtr Timeline::newMidiClip(const TrackPtr& track, int64_t frame, int64_t frames) {
    const Timing& t = project.timing;
    double at = (double)frame / engine.sr;
    double seconds = frames ? (double)frames / engine.sr : (t.beats ? t.beatSAt(at) * t.beatsPerBarAt(at) : 2.0);
    auto clip = std::make_shared<Clip>();
    clip->kind = Kind::Midi;
    clip->name = "MIDI clip";
    clip->notes = std::make_shared<const std::vector<Note>>();
    clip->lengthS = seconds;
    clip->length = (int64_t)std::nearbyint(seconds * engine.sr);  // (exactly that long: not its notes' release tail too)
    placeClip(clip, frame, track);
    return clip;
}

void Timeline::toggleDisabled() {
    if (selection.clips.empty()) return;
    bool disable = false;
    for (auto& c : selection.clips)
        if (!c->disabled) disable = true;
    for (auto& c : selection.clips) c->disabled = disable;
    update();
}

void Timeline::renameSelected() {
    if (selection.clips.empty()) return;
    std::string name = (*selection.clips.begin())->name;
    if (ui::getText(dialogParent(), selection.clips.size() > 1 ? "Rename clips" : "Rename clip", "Name:", name)) {
        name = util::trim(name);
        if (name.empty()) return;
        for (auto& c : selection.clips) c->name = name;
        update();
    }
}

void Timeline::selectAll() {
    // every clip that can be seen and edited (not in a folded group, not on a frozen track)
    selection.clips.clear();
    std::set<const Track*> shown;
    for (auto& t : visibleTracks()) shown.insert(t.get());
    for (auto& c : project.clips)
        if (shown.count(c->track.get()) && !c->track->frozen) selection.clips.insert(c);
    selection.range.reset();
    update();
}

void Timeline::clearSelection() {
    selection.clear();
    update();
}

// --- transport and zoom -------------------------------------------------------------------------

void Timeline::seek(int64_t frame) {
    engine.seek(frame);
    setPlayhead(engine.pos.load());
}

void Timeline::setPlayhead(int64_t frame) { moveLine(playheadX, (int)xOf(frame)); }

void Timeline::setCursor(int64_t frame) {
    cursor = std::max<int64_t>(0, frame);
    moveLine(cursorX, (int)xOf(cursor));
}

void Timeline::moveLine(int& line, int x) {
    int old = line;
    if (x == old) return;
    updateLine(old);
    line = x;
    updateLine(x);
    if (masterAlive()) {
        masterView->updateLine(old);
        masterView->updateLine(x);
    }
}

void Timeline::updateLine(int x) {
    int half = config::CURSOR_ARROW_W / 2 + 2;
    update(Rect(x - 1, 0, 3, height()));
    update(Rect(x - half, 0, 2 * half + 1, config::CURSOR_ARROW_H + 2));
}

void Timeline::zoom(double factor, double anchorX) {
    ui::ScrollBar* hbar = scroll->horizontalScrollBar();
    double seconds = anchorX / pps, offset = anchorX - hbar->value();
    pps = std::min(maxPps(), std::max(config::MIN_PPS, pps * factor));
    relayout();
    hbar->setValue((int)(seconds * pps - offset));
}

void Timeline::setCurrentTrack(const TrackPtr& track) {
    if (track != currentTrack) {
        currentTrack = track;
        currentTrackChanged.emit();
    }
}

void Timeline::setLoop(int64_t start, int64_t end) {
    int sr = engine.sr;
    Timing& t = project.timing;
    start = std::max<int64_t>(0, std::min(start, end));
    end = std::max(start + (int64_t)std::nearbyint(config::LOOP_MIN_S * sr), std::max(start, end));
    t.loopStart = (double)start / sr;
    t.loopEnd = (double)end / sr;
    update();
    loopChanged.emit();
}

bool Timeline::loopXs(int& x0, int& x1) const {
    double s, e;
    if (!project.timing.loopRange(s, e)) return false;
    x0 = (int)std::nearbyint(s * pps);
    x1 = (int)std::nearbyint(e * pps);
    return true;
}

int Timeline::loopEndAt(PointF pos) const {
    int xs[2];
    if (!loopXs(xs[0], xs[1]) || pos.y >= config::RULER_H) return -1;
    int best = -1;
    double bestD = 0;
    for (int i = 0; i < 2; ++i) {
        double d = std::fabs(pos.x - xs[i]);
        if (d <= timeline::LOOP_GRAB_PX && (best < 0 || d < bestD)) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

ui::Window* Timeline::dialogParent() const { return window(); }
