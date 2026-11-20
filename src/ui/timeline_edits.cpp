// timeline.py, part 4 (the successor's): fades and clip gain, stretch and pitch, reverse, join,
// crossfade, looping clips, MIDI files in beats, take lanes (comping), markers, tempo changes on the
// ruler, and inserting and deleting time in the whole song.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/util.h"
#include "model/edits.h"
#include "model/song_time.h"
#include "ui/clip_painter.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "audio/midi_effects.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/style.h"
#include "ui/track_dialogs.h"

namespace {
// QRectF::contains: the edges count
bool containsQt(const RectF& r, PointF p) { return p.x >= r.x && p.x <= r.right() && p.y >= r.y && p.y <= r.bottom(); }

std::string fixed2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

// --- fades ----------------------------------------------------------------------------------------

FadeHit Timeline::fadeHandleAt(PointF pos) const {
    if (pos.y < lanes::lanesTop()) return {};
    auto tops = trackTops();
    auto& clips = *project.clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
        const Clip& clip = **it;
        if (clip.kind != Kind::Audio || clip.disabled) continue;
        auto rect = clipRect(clip, tops);
        if (!rect || clip.track->frozen) continue;
        RectF body = rect->adjusted(0, (float)clip_painter::headerHeight(), 0, 0);
        RectF in, out;
        if (!clip_painter::fadeHandles(clip, body, pps, engine.sr, in, out)) continue;
        if (containsQt(in.adjusted(-2, -2, 2, 2), pos)) return {*it, true};
        if (containsQt(out.adjusted(-2, -2, 2, 2), pos)) return {*it, false};
    }
    return {};
}

void Timeline::dragFade(PointF pos) {
    // the grabbed fade to the mouse; the other selected audio clips' fades on that side change by as much
    auto& d = std::get<FadeDrag>(drag);
    auto setFade = [&d](Clip& clip, int64_t frames) {
        int64_t partIn, partOut;
        double a, b;
        clip.fadeParts(partIn, partOut, a, b);  // (a piece of a longer fade: it starts afresh at the edge)
        if (d.in) {
            clip.fadeIn = std::max<int64_t>(0, std::min(frames, clip.frames() - partOut));
            clip.fadeLead = 0;
        } else {
            clip.fadeOut = std::max<int64_t>(0, std::min(frames, clip.frames() - partIn));
            clip.fadeTail = 0;
        }
    };
    int64_t frame = frameAt(pos.x);
    int64_t grabbed = d.in ? frame - d.clip->start : d.clip->end() - frame;
    auto it = d.origin.find(d.clip);
    if (it != d.origin.end())
        for (auto& [c, fade] : d.origin)
            if (c != d.clip) setFade(*c, fade + grabbed - it->second);
    setFade(*d.clip, grabbed);
    update();
}

void Timeline::removeFadesSelected() {
    for (auto& clip : selection.clips) clip->fadeIn = clip->fadeOut = clip->fadeLead = clip->fadeTail = 0;
    update();
}

// --- fade shapes (model.h FADE SHAPES): from the clip menu, or a fade handle's own (right-click it) ---

void Timeline::setFadeShape(const std::vector<ClipPtr>& clips, bool in, FadeShape shape) {
    // (a plain field the mixer reads as it goes: changed in place, like the fades' lengths)
    for (auto& clip : clips)
        if (clip->kind == Kind::Audio) (in ? clip->fadeInShape : clip->fadeOutShape) = shape;
    update();
}

void Timeline::addFadeShapes(ui::Menu& menu, const std::vector<ClipPtr>& clips, bool in) {
    for (FadeShape shape : FADE_SHAPES) {
        bool all = !clips.empty();  // (ticked: every clip's fade has it)
        for (auto& c : clips)
            if ((in ? c->fadeInShape : c->fadeOutShape) != shape) all = false;
        ui::Action* a = menu.addAction(fadeShapeLabel(shape), [this, clips, in, shape] { setFadeShape(clips, in, shape); });
        a->checkable = true;
        a->checked = all;
        a->setEnabled(!clips.empty());
    }
}

bool Timeline::fillFadeMenu(ui::Menu& menu, PointF pos) {
    FadeHit fade = fadeHandleAt(pos);
    if (!fade.clip) return false;
    menu.addSection(fade.in ? "Fade-in shape" : "Fade-out shape");
    addFadeShapes(menu, {fade.clip}, fade.in);
    return true;
}

void Timeline::addFadeShapeMenus(ui::Menu& menu, const std::vector<ClipPtr>& clips) {
    for (bool in : {true, false}) {
        addFadeShapes(*menu.addMenu(in ? "Fade-in shape" : "Fade-out shape"), clips, in);
        menu.actions().back()->setEnabled(!clips.empty());  // (the submenu's entry)
    }
}

// --- gain, stretch, reverse, join, crossfade, loops -------------------------------------------------

namespace {
std::vector<ClipPtr> selectedAudio(const Project& project, const Selection& selection) {
    std::vector<ClipPtr> out;  // (in the project's order: the first one's settings are shown)
    for (auto& c : project.clips)
        if (c->kind == Kind::Audio && selection.contains(c)) out.push_back(c);
    return out;
}
}  // namespace

void Timeline::setGainSelected() {
    auto clips = selectedAudio(project, selection);
    if (clips.empty()) return;
    double gain = clips[0]->gainDb;
    if (!track_dialogs::getDouble(dialogParent(), "Clip gain", "Gain of the selected audio clips (dB):", gain,
                                  config::MIN_DB, 24.0, 1))
        return;
    for (auto& clip : clips) clip->gainDb = std::nearbyint(gain * 10) / 10;
    update();
}

void Timeline::replaceClips(const std::map<ClipPtr, ClipPtr>& made) {
    auto swap = [&made](const ClipPtr& c) {
        auto it = made.find(c);
        return it != made.end() ? it->second : c;
    };
    std::vector<ClipPtr> all;
    for (auto& c : project.clips) all.push_back(swap(c));
    project.clips = all;
    std::set<ClipPtr> kept;
    for (auto& c : selection.clips)
        if (project.clips.contains(swap(c))) kept.insert(swap(c));
    selection.clips = kept;
    relayout();
}

void Timeline::prepareLive(const ClipPtr& clip, int64_t reach) {
    Clip ready = *clip;
    ready.track = std::atomic_load(&clip->track);
    ready.prepare(engine.sr, reach);
    clip->baseFrames = ready.baseFrames;
    clip->loopFrom = ready.loopFrom;
    clip->loopFor = ready.loopFor;
    std::atomic_store(&clip->peaks, ready.peaks);
    std::atomic_store(&clip->played, ready.played);
    std::atomic_store(&clip->events, ready.events);  // (the audio thread atomic_loads them)
    std::atomic_store(&clip->data, ready.data);
}

void Timeline::setLoopNotes(const std::vector<ClipPtr>& clips, bool on) {
    int sr = engine.sr;
    for (auto& clip : clips) {
        if (clip->kind == Kind::Audio) {
            if (on && !clip->loop) {
                clip->loopStart = (double)clip->offset / sr;
                clip->loopLength = (double)clip->frames() / sr;
                clip->length = clip->frames();
            } else if (!on && clip->loop && clip->loopLength > 0) {  // (a start inside a repeat: into the loop's own audio)
                int64_t a = (int64_t)std::nearbyint(clip->loopStart * sr);
                int64_t n = std::max<int64_t>(1, (int64_t)std::nearbyint(clip->loopLength * sr));
                if (clip->offset >= a + n) clip->offset = a + (clip->offset - a) % n;
            }
            Clip ready = *clip;
            ready.track = std::atomic_load(&clip->track);
            ready.loop = on;
            ready.prepare(sr);
            int64_t have = ready.data ? ready.data->frames() : 0;
            clip->baseFrames = ready.baseFrames;
            clip->loopFrom = ready.loopFrom;
            clip->loopFor = ready.loopFor;
            std::atomic_store(&clip->peaks, ready.peaks);
            if (have < (clip->data ? clip->data->frames() : 0))  // (shorter audio: the window first)
                clip->length = std::max<int64_t>(1, std::min(clip->frames(), have - clip->offset));
            std::atomic_store(&clip->data, ready.data);
            clip->loop = on;
            clip->length = std::max<int64_t>(1, std::min(clip->frames(), have - clip->offset));  // (off: no further than its audio)
            continue;
        }
        if (on && !clip->loop)
            clip->lengthS = clip->length >= 0 ? (double)(clip->offset + clip->frames()) / sr : clip->lengthS;
        clip->loop = on;
        prepareLive(clip);
        linkedNotesChanged(clip);  // (its linked clips loop, or not, with it)
    }
    relayout();
}

void Timeline::reverseSelected() {
    std::map<ClipPtr, ClipPtr> made;
    for (auto& c : selectedAudio(project, selection)) made[c] = edits::reversedClip(c, engine.sr);
    replaceClips(made);
}

void Timeline::joinSelected() {
    // per track, the selected clips (or what lies in the selected range, cut at its edges) become one
    std::vector<ClipPtr> chosen;
    if (selection.range) {
        const TimeRange& r = *selection.range;
        for (auto& c : project.clipsIn(r.tracks, r.start, r.end))
            if (ClipPtr p = edits::pieceWithin(c, r.start, r.end)) chosen.push_back(p);
    } else {
        for (auto& c : project.clips)
            if (selection.contains(c)) chosen.push_back(c);
    }
    std::vector<TrackPtr> order;
    std::map<TrackPtr, std::vector<ClipPtr>> byTrack;
    std::vector<ClipPtr> used;
    for (auto& c : chosen) {
        if (c->disabled || c->track->frozen) continue;
        if (!byTrack.count(c->track)) order.push_back(c->track);
        byTrack[c->track].push_back(c);
        used.push_back(c);
    }
    for (auto& t : order) {  // (some clips with their MIDI effects committed into their notes, some not: refused)
        auto& cs = byTrack[t];
        bool some = std::any_of(cs.begin(), cs.end(), [](const ClipPtr& c) { return c->effectsCommitted; });
        bool all = std::all_of(cs.begin(), cs.end(), [](const ClipPtr& c) { return c->effectsCommitted; });
        if (some && !all) {
            util::debugf("join refused on %s: clips with and without committed MIDI effects\n", t->name.c_str());
            if (status) status("Can't join: some of these clips have their MIDI effects committed and some don't", 6000);
            return;
        }
    }
    std::vector<ClipPtr> joined;
    for (auto& t : order)
        if (ClipPtr j = edits::consolidated(byTrack[t], engine.sr)) joined.push_back(j);
    if (joined.empty()) return;
    std::vector<ClipPtr> kept;  // disabled clips in the range: left out of the join, but kept where they are
    if (selection.range) {
        const TimeRange r = *selection.range;
        for (auto& c : project.clipsIn(order, r.start, r.end))
            if (c->disabled)
                if (ClipPtr p = edits::pieceWithin(c, r.start, r.end)) kept.push_back(p);
        edits::deleteRange(project, order, r.start, r.end);
    } else {
        project.removeClips(used);
    }
    std::vector<ClipPtr> all = project.clips.copy();
    all.insert(all.end(), joined.begin(), joined.end());
    all.insert(all.end(), kept.begin(), kept.end());
    project.clips = all;
    selection.clear();
    selection.clips = std::set<ClipPtr>(joined.begin(), joined.end());
    relayout();
}

void Timeline::crossfadeSelected() {
    auto clips = selectedAudio(project, selection);
    std::stable_sort(clips.begin(), clips.end(), [](const ClipPtr& a, const ClipPtr& b) { return a->start < b->start; });
    if (clips.size() != 2 || clips[0]->track != clips[1]->track) {
        ui::msg::information(dialogParent(), "Crossfade", "Select two audio clips on one track, where one ends and the "
                                                          "other starts.");
        return;
    }
    double ms = 40.0;
    if (!track_dialogs::getDouble(dialogParent(), "Crossfade", "Length of the crossfade (ms):", ms, 1.0, 5000.0, 1)) return;
    ClipPtr left, right;
    if (!edits::crossfaded(clips[0], clips[1], (int64_t)std::nearbyint(ms / 1000 * engine.sr), left, right)) {
        ui::msg::information(dialogParent(), "Crossfade", "These clips can't overlap: neither has audio beyond where "
                                                          "they meet (fade them out and in instead).");
        return;
    }
    replaceClips({{clips[0], left}, {clips[1], right}});
}

void Timeline::rerenderMoved(const std::map<ClipPtr, TrackPtr>& tracksBefore,
                             const std::map<ClipPtr, std::optional<int64_t>>& startsBefore) {
    for (auto& [clip, track] : tracksBefore) {
        if (clip->kind != Kind::Midi) continue;
        auto it = startsBefore.find(clip);
        bool moved = it == startsBefore.end() || !it->second || clip->start != *it->second;  // (no start known: moved)
        if (clip->track != track || (!clip->track->midiEffects.empty() && moved))
            prepareLive(clip);
    }
}

void Timeline::fromBeats(const ClipPtr& clip, int64_t frame) {
    auto it = inBeats_.find(clip.get());
    if (it == inBeats_.end()) return;
    std::vector<Note> notes = std::move(it->second.first);
    double beats = it->second.second;
    inBeats_.erase(it);
    const Timing& t = project.timing;
    int sr = engine.sr;
    double at = (double)frame / sr;
    double beat0 = t.beatAt(at);
    int perBar = t.beatsPerBarAt(at);
    for (auto& n : notes) beats = std::max(beats, n.end);
    int bars = std::max(1, (int)std::ceil(beats / perBar - 1e-6));
    auto seconds = [&](double b) { return t.secondsAt(beat0 + b) - at; };
    std::vector<Note> placed;
    placed.reserve(notes.size());
    for (auto& n : notes) {
        double s = seconds(n.start);
        placed.push_back({s, std::max(seconds(n.end), s + 0.005), n.pitch, n.velocity});
    }
    clip->notes = std::make_shared<const std::vector<Note>>(std::move(placed));
    clip->lengthS = seconds(bars * perBar);
    clip->length = (int64_t)std::nearbyint(clip->lengthS * sr);
}

// --- take lanes -------------------------------------------------------------------------------------

void Timeline::paintTake(Painter& p, const Rect& r, const Lane& lane) {
    // a take lane (its background: paintLanes): its top edge again (over the grid), its take as a clip, and the
    // part being chosen while dragging across it (Timeline::selection)
    const TimelineLook& lk = look();
    const LaneLook& l = lk.lanes->get(LaneKind::Take);
    if (l.topEdge.width > 0) p.fillRect(Rect(r.left(), lane.top, r.w, l.topEdge.width), l.topEdge.color);
    const Clip& take = *lane.take;
    double x0 = xOf(take.start), x1 = xOf(take.end());
    if (x1 >= r.left() && x0 <= r.right() - 1)
        clip_painter::paintClip(p, clipLooks_.get(take, false, true, false, 0), take,
                                RectF((float)x0, (float)lane.top + 2, (float)(x1 - x0), (float)lane.height - 4), r,
                                scroll->horizontalScrollBar()->value(), pps, engine.sr, false, true, false, false);
    if (auto* d = std::get_if<TakeDrag>(&drag)) {
        if (d->lane.take == lane.take) {
            int64_t a = std::min(d->frame, d->to), b = std::max(d->frame, d->to);
            ui::style::box(
                p, clip_painter::pixels(RectF((float)xOf(a), (float)lane.top, (float)(xOf(b) - xOf(a)), (float)lane.height)),
                lk.selection);
        }
    }
}

void Timeline::comp(const TrackPtr& track, const ClipPtr& take, int64_t start, int64_t end) {
    ClipPtr piece = edits::pieceWithin(take, start, end);
    if (!piece || !project.tracks.contains(track) || take->kind != track->kind || track->frozen)
        return;  // (a track only holds clips of its own kind; a frozen track's are locked)
    piece->track = track;
    if (piece->kind == Kind::Midi) piece->prepare(engine.sr);  // (with the track's sound as it is now)
    edits::placeCopies(project, {piece});
    selection.selectOnly(piece);
    relayout();
}

void Timeline::useTake(const TrackPtr& track, const ClipPtr& take) { comp(track, take, take->start, take->end()); }

void Timeline::deleteTake(const TrackPtr& track, const ClipPtr& take) {
    if (track->frozen) return;  // (locked until it is unfrozen)
    std::vector<ClipPtr> kept;
    for (auto& t : track->takes)
        if (t != take) kept.push_back(t);
    track->takes = kept;
    if (kept.empty()) track->showTakes = false;
    lanesChanged();
}

void Timeline::fillTakeMenu(ui::Menu& menu, const Lane& lane) {
    TrackPtr owner = lane.owner;
    ClipPtr take = lane.take;
    // (Python shows "double-click" as the first entry's shortcut; the toolkit's menus show key sequences only)
    menu.addAction("Use all of " + take->name, [this, owner, take] { useTake(owner, take); });
    menu.addAction("Delete " + take->name + " (from the takes)", [this, owner, take] { deleteTake(owner, take); });
    menu.addAction("Hide the takes", [this, owner] {
        owner->showTakes = false;
        lanesChanged();
    });
}

// --- markers ----------------------------------------------------------------------------------------

std::vector<std::pair<Marker, Rect>> Timeline::markerFlags() const {
    // (Ruler::marker: its height, font and padding: as wide as its name with its padding-left at both sides, or
    // padding-right at the right when that is more)
    const TimelineLook& lk = look();
    const Font& font = lk.markerFont;
    const int top = lk.rulerHeight - lk.markerHeight;
    const int room = lk.markerPadLeft + std::max(lk.markerPadLeft, lk.markerPadRight);
    auto& markers = *project.markers;
    std::vector<int> xs;
    for (auto& m : markers) xs.push_back((int)std::nearbyint(m.seconds * pps));
    std::vector<std::pair<Marker, Rect>> flags;
    for (size_t i = 0; i < markers.size(); ++i) {
        int width = (int)std::ceil(font.width(markers[i].name)) + room;
        if (i + 1 < xs.size()) width = std::max(timeline::MARKER_MIN_PX, std::min(width, xs[i + 1] - xs[i] - 1));
        flags.push_back({markers[i], Rect(xs[i], top, width, lk.markerHeight)});
    }
    return flags;
}

void Timeline::paintMarkers(Painter& p, const Rect& r) {
    // each marker's flag (Ruler::marker) and its place across the ruler (Ruler::marker-line), boxes
    const TimelineLook& lk = look();
    for (auto& [marker, flag] : markerFlags()) {
        Rect place(flag.left(), 0, lk.markerLineW, lk.rulerHeight);
        if (std::max(flag.right(), place.right()) - 1 < r.left() || flag.left() > r.right() - 1) continue;
        ui::style::box(p, flag, lk.marker);
        ui::style::box(p, place, lk.markerLine);
        Rect text = flag.adjusted(lk.markerPadLeft, 0, -lk.markerPadRight, 0);
        if (text.w > 0) p.drawText(RectF(text), AlignLeft | AlignVCenter, marker.name, lk.markerFont, lk.markerText, true);
    }
}

std::optional<Marker> Timeline::markerAt(PointF pos) const {
    if (pos.y >= lanes::rulerHeight()) return std::nullopt;
    auto flags = markerFlags();
    Point at((int)std::lround(pos.x), (int)std::lround(pos.y));
    for (auto it = flags.rbegin(); it != flags.rend(); ++it)
        if (it->second.contains(at)) return it->first;
    return std::nullopt;
}

void Timeline::addMarker(int64_t frame) {
    std::string name = "Marker " + std::to_string(project.markers.size() + 1);
    if (!track_dialogs::getText(dialogParent(), "Add a marker", "Name:", name)) return;
    name = trimmed(name);
    if (name.empty()) return;
    std::vector<Marker> ms = project.markers.copy();
    ms.push_back(Marker{(double)frame / engine.sr, name});
    project.setMarkers(ms);
    update();
}

void Timeline::renameMarker(const Marker& marker) {
    std::string name = marker.name;
    if (!track_dialogs::getText(dialogParent(), "Rename the marker", "Name:", name)) return;
    name = trimmed(name);
    if (!name.empty()) replaceMarker(marker, Marker{marker.seconds, name});
}

void Timeline::replaceMarker(const Marker& marker, const std::optional<Marker>& made) {
    std::vector<Marker> ms;
    bool done = false;
    for (auto& m : project.markers) {
        if (!done && m == marker) {
            done = true;
            if (made) ms.push_back(*made);
            continue;
        }
        ms.push_back(m);
    }
    project.setMarkers(ms);
    update();
}

// --- inserting and deleting time ----------------------------------------------------------------------

std::string Timeline::insertTime() {
    int sr = engine.sr;
    const Timing& t = project.timing;
    int64_t at, length = 0;
    if (selection.range) {
        at = selection.range->start;
        length = selection.range->end - selection.range->start;
    } else {
        at = cursor;
        if (t.beats) {
            int bars = 1;
            if (track_dialogs::getInt(dialogParent(), "Insert time",
                                      "Bars of empty time to insert at the selection cursor (everything after it moves "
                                      "later):",
                                      bars, 1, 999)) {
                int bar = t.barAt((double)at / sr);
                length = (int64_t)std::nearbyint((t.barSeconds(bar + bars) - t.barSeconds(bar)) * sr);
            }
        } else {
            double seconds = 4.0;
            if (track_dialogs::getDouble(dialogParent(), "Insert time",
                                         "Seconds of empty time to insert at the selection cursor:", seconds, 0.01, 3600, 2))
                length = (int64_t)std::nearbyint(seconds * sr);
        }
    }
    if (length <= 0) return "";
    return editTime([this, at, length, sr](std::string& note, std::string& error) {
        return song_time::insertTime(project, at, length, sr, note, error);
    }, "Inserted " + fixed2((double)length / sr) + " s of empty time");
}

std::string Timeline::deleteTime() {
    if (!selection.range)
        return "Select a time range first (drag across a lane): its stretch is taken out of the whole song";
    TimeRange r = *selection.range;
    int sr = engine.sr;
    return editTime([this, r, sr](std::string& note, std::string& error) {
        return song_time::deleteTime(project, r.start, r.end, sr, note, error);
    }, "Deleted " + fixed2((double)(r.end - r.start) / sr) + " s from the song");
}

std::string Timeline::editTime(const std::function<bool(std::string& note, std::string& error)>& edit,
                               const std::string& done) {
    std::string note, error;
    if (!edit(note, error)) return error;
    for (auto& clip : project.clips)  // (MIDI effects follow the beat where their clips now are)
        if (clip->kind == Kind::Midi && (!clip->track->midiEffects.empty() || midi_effects::swings(*clip->track)))
            prepareLive(clip);
    selection.clear();
    timingChanged.emit();  // (the tempo changes may have moved)
    loopChanged.emit();
    relayout();
    tracksChanged.emit();
    return done + (note.empty() ? "" : ". " + note);
}

// --- tempo changes and the ruler's menu ------------------------------------------------------------

void Timeline::paintTempoChanges(Painter& p, const Rect& r) {
    // each tempo change after its bar's number on the ruler
    // (Ruler::tempo: after the bar's number, Ruler::label, by its margin-left)
    const TimelineLook& lk = look();
    if (lanes::tempoLaneShown()) return;  // (the tempo lane under it shows them)
    const Timing& t = project.timing;
    TempoMapPtr m = t.map();  // (once: a song may have thousands of changes)
    for (auto& change : m->changes) {
        if (!change.valid()) continue;
        int x = (int)(m->secondsAt(m->beatOf(change)) * pps);
        if (x < r.left() - 200 || x > r.right() - 1) continue;
        // (a change inside a bar: no bar number there, nor a time signature; a ramp: an arrow)
        float after = change.atBar() ? lk.labelFont.width(std::to_string(change.bar)) : 0.0f;
        std::string text = g(change.bpm) + " BPM" + (change.atBar() ? " " + std::to_string(change.beatsPerBar) + "/4" : "") +
                           (change.ramp ? " ↗" : "");
        p.drawText(PointF((float)x + (lk.labelPadLeft + lk.tempoMargin) + after, (float)lk.tempoBaseline), text,
                   lk.tempoFont, lk.tempoColor);
    }
}

void Timeline::editTempo(int bar) { editTempoPoint(TempoRef{std::max(1, bar), 0.0}); }  // (timeline_tempo.cpp)

void Timeline::fillRulerMenu(ui::Menu& menu, PointF pos) {
    // right-click on the ruler: markers, the tempo from a bar, inserting and deleting time
    std::optional<Marker> marker = markerAt(pos);
    menu.addAction("Add a marker here", [this, pos] { addMarker(snappedFrameAt(pos.x, 0)); });
    menu.addAction("Add a marker at the selection cursor", [this] { addMarker(cursor); });
    if (marker) {
        Marker m = *marker;
        menu.addSeparator();
        menu.addAction("Rename the marker “" + m.name + "”…", [this, m] { renameMarker(m); });
        menu.addAction("Delete the marker “" + m.name + "”", [this, m] { replaceMarker(m, std::nullopt); });
    }
    menu.addSeparator();
    const Timing& timing = project.timing;
    int bar = timing.barAt(std::max(0.0, pos.x / pps));
    const TempoChange* change = timing.changeAtBar(bar + 1);
    ui::Action* tempo = menu.addAction(
        "Tempo and time signature from bar " + std::to_string(bar + 1) + "…" +
            (change ? "  (" + g(change->bpm) + " BPM " + std::to_string(change->beatsPerBar) + "/4)" : ""),
        [this, bar] { editTempo(bar + 1); });
    tempo->setEnabled(timing.beats);
    {  // (the tempo lane's: a change near here, or one to add; the timebase)
        double seconds = std::max(0.0, pos.x / pps);
        ui::Menu* lane = menu.addMenu("Tempo changes");
        fillTempoMenu(*lane, seconds, tempoPointNear(seconds, config::AUTOMATION_GRAB_PX));
    }
    menu.addSeparator();
    menu.addAction(std::string("Insert time…") + (selection.range ? "  (as long as the range)" : ""),
                   [this] { timeEdited.emit(insertTime()); });
    ui::Action* del = menu.addAction("Delete the selected time", [this] { timeEdited.emit(deleteTime()); });
    del->setEnabled(selection.range.has_value());
    menu.addSeparator();
    ui::Action* all = menu.addAction("Delete every marker", [this] {
        project.setMarkers({});
        update();
    });
    all->setEnabled(!project.markers.empty());
}

void Timeline::rulerMenu(Point globalPos, PointF pos) {
    ui::Menu menu;
    fillRulerMenu(menu, pos);
    menu.exec(globalPos, this);
}
