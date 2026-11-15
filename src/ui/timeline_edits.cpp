// timeline.py, part 4 (the successor's): fades and clip gain, stretch and pitch, reverse, join,
// crossfade, looping clips, MIDI files in beats, take lanes (comping), markers, tempo changes on the
// ruler, and inserting and deleting time in the whole song.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "model/edits.h"
#include "model/song_time.h"
#include "ui/clip_painter.h"
#include "ui/stretch_dialog.h"
#include "ui/tempo_dialog.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/dialogs.h"
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
    if (pos.y < config::RULER_H) return {};
    auto tops = trackTops();
    auto& clips = *project.clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
        const Clip& clip = **it;
        if (clip.kind != Kind::Audio || clip.disabled) continue;
        auto rect = clipRect(clip, tops);
        if (!rect || clip.track->frozen) continue;
        RectF body = rect->adjusted(0, (float)config::CLIP_HANDLE_H, 0, 0);
        RectF in, out;
        if (!clip_painter::fadeHandles(clip, body, pps, engine.sr, in, out)) continue;
        if (containsQt(in.adjusted(-2, -2, 2, 2), pos)) return {*it, true};
        if (containsQt(out.adjusted(-2, -2, 2, 2), pos)) return {*it, false};
    }
    return {};
}

void Timeline::dragFade(PointF pos) {
    auto& d = std::get<FadeDrag>(drag);
    Clip& clip = *d.clip;
    int64_t frame = frameAt(pos.x);
    int64_t partIn, partOut;
    double a, b;
    clip.fadeParts(partIn, partOut, a, b);  // (a piece of a longer fade: it starts afresh at the edge)
    if (d.in) {
        clip.fadeIn = std::max<int64_t>(0, std::min(frame - clip.start, clip.frames() - partOut));
        clip.fadeLead = 0;
    } else {
        clip.fadeOut = std::max<int64_t>(0, std::min(clip.end() - frame, clip.frames() - partIn));
        clip.fadeTail = 0;
    }
    update();
}

void Timeline::removeFadesSelected() {
    for (auto& clip : selection.clips) clip->fadeIn = clip->fadeOut = clip->fadeLead = clip->fadeTail = 0;
    update();
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

void Timeline::stretchSelected() {
    auto clips = selectedAudio(project, selection);
    if (clips.empty()) return;
    StretchDialog dialog(dialogParent(), *clips[0], project.timing.bpmAt((double)clips[0]->start / engine.sr),
                         (int)clips.size());
    if (!dialog.run()) return;
    std::map<ClipPtr, ClipPtr> made;
    std::vector<ClipPtr> news;
    for (auto& c : clips) {
        ClipPtr n = edits::restretched(c, dialog.stretch, dialog.pitch, engine.sr);
        n->sourceBpm = dialog.sourceBpm;
        made[c] = n;
        news.push_back(n);
    }
    std::vector<ClipPtr> all;
    for (auto& c : project.clips) all.push_back(made.count(c) ? made[c] : c);
    project.clips = all;  // (all at once: the audio thread)
    edits::clearUnder(project, news);
    std::set<ClipPtr> kept;
    for (auto& c : selection.clips) {
        ClipPtr n = made.count(c) ? made[c] : c;
        if (project.clips.contains(n)) kept.insert(n);
    }
    selection.clips = kept;
    relayout();
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

void Timeline::paintTake(Painter& p, const Rect& r, const Lane& lane, bool background) {
    // a take lane: its take as a clip, and the part being chosen while dragging across it
    if (background) p.fillRect(Rect(r.left(), lane.top, r.w, lane.height), theme::c("automation_lane"));
    p.fillRect(Rect(r.left(), lane.top, r.w, 1), theme::c("grid_bar"));
    const Clip& take = *lane.take;
    double x0 = xOf(take.start), x1 = xOf(take.end());
    if (x1 >= r.left() && x0 <= r.right() - 1)
        clip_painter::paintClip(p, take, RectF((float)x0, (float)lane.top + 2, (float)(x1 - x0), (float)lane.height - 4), r,
                                scroll->horizontalScrollBar()->value(), pps, engine.sr, false, true, false, false);
    if (auto* d = std::get_if<TakeDrag>(&drag)) {
        if (d->lane.take == lane.take) {
            int64_t a = std::min(d->frame, d->to), b = std::max(d->frame, d->to);
            p.fillRect(clip_painter::pixels(RectF((float)xOf(a), (float)lane.top, (float)(xOf(b) - xOf(a)), (float)lane.height)),
                       theme::c("selection_fill"));
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
    Font font = theme::uiFont();
    const int top = config::RULER_H / 2;
    auto& markers = *project.markers;
    std::vector<int> xs;
    for (auto& m : markers) xs.push_back((int)std::nearbyint(m.seconds * pps));
    std::vector<std::pair<Marker, Rect>> flags;
    for (size_t i = 0; i < markers.size(); ++i) {
        int width = (int)std::ceil(font.width(markers[i].name)) + 10;
        if (i + 1 < xs.size()) width = std::max(timeline::MARKER_MIN_PX, std::min(width, xs[i + 1] - xs[i] - 1));
        flags.push_back({markers[i], Rect(xs[i], top, width, config::RULER_H - top)});
    }
    return flags;
}

void Timeline::paintMarkers(Painter& p, const Rect& r) {
    Font font = theme::uiFont();
    for (auto& [marker, flag] : markerFlags()) {
        if (flag.right() - 1 < r.left() || flag.left() > r.right() - 1) continue;
        p.fillRect(flag, theme::c("accent"));
        p.fillRect(Rect(flag.left(), 0, 2, config::RULER_H), theme::c("accent"));  // its place, across the ruler
        Rect text = flag.adjusted(5, 0, -2, 0);
        if (text.w > 0)
            p.drawText(RectF(text), AlignLeft | AlignVCenter, marker.name, font, theme::c("accent_text"), true);
    }
}

std::optional<Marker> Timeline::markerAt(PointF pos) const {
    if (pos.y >= config::RULER_H) return std::nullopt;
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
        if (clip->kind == Kind::Midi && !clip->track->midiEffects.empty()) prepareLive(clip);
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
    Font font = theme::uiFont();
    const Timing& t = project.timing;
    for (auto& change : t.changes) {
        int x = (int)(t.barSeconds(change.bar - 1) * pps);
        if (r.left() - 200 <= x && x <= r.right() - 1)
            p.drawText(PointF((float)x + 10 + font.width(std::to_string(change.bar)), 15),
                       g(change.bpm) + " BPM " + std::to_string(change.beatsPerBar) + "/4", font, theme::c("accent"));
    }
}

void Timeline::editTempo(int bar) {
    Timing& t = project.timing;
    double at = t.barSeconds(bar - 1);
    TempoDialog dialog(dialogParent(), bar, t.bpmAt(at), t.beatsPerBarAt(at), t.changeAtBar(bar) != nullptr);
    if (!dialog.run()) return;
    if (bar == 1) {
        t.bpm = dialog.bpm;
        t.beatsPerBar = dialog.beatsPerBar;
    } else if (dialog.removed) {
        t.removeChange(bar);
    } else {
        t.setChange(bar, dialog.bpm, dialog.beatsPerBar);
    }
    timingChanged.emit();
    update();
}

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
