// timeline.py, part 3: the mouse (clicks pick what to do, drags carry it out), the right-click
// menus, keys, the wheel, editing automation points, and dropping files.
#include <algorithm>
#include <cmath>

#include "audio/automation.h"
#include "core/util.h"
#include "model/edits.h"
#include "ui/master_lane.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"

namespace {
int indexIn(const std::vector<TrackPtr>& tracks, const TrackPtr& t) {
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i] == t) return (int)i;
    return -1;
}
}  // namespace

// --- automation points --------------------------------------------------------------------------

void Timeline::automationPress(ui::MouseEvent& e, const Lane& lane) {
    // a click in an automation lane: grab the point there, or add one there (and grab it)
    Automation& a = *lane.automation;
    PointF pos(e.pos);
    if (lane.owner->frozen) return;
    int index = pointAt(lane, pos);
    if (index < 0) {
        double seconds = (double)snappedFrameAt(pos.x, e.mods) / engine.sr;
        double value = std::min(1.0, std::max(0.0, automationValue(lane, pos.y)));  // (as setPoints keeps it)
        std::vector<AutoPoint> pts = a.points.copy();
        pts.push_back({seconds, value});
        a.setPoints(pts);
        for (size_t i = 0; i < a.points.size(); ++i)  // (the new one, not another point at the same time)
            if (a.points[i].t == seconds && a.points[i].v == value) index = (int)i;
        if (index < 0) index = 0;
    }
    drag = PointDrag{lane, index};
    showValueTip(lane, a.points[index].v, e.globalPos);
    update();
    automationEdited.emit();
}

void Timeline::dragPoint(PointF pos, Point globalPos, int mods) {
    // move the grabbed point: in time between its neighbours, and in value
    auto& d = std::get<PointDrag>(drag);
    Automation& a = *d.lane.automation;
    std::vector<AutoPoint> points = a.points.copy();
    if (d.index < 0 || d.index >= (int)points.size()) return;
    double seconds = (double)snappedFrameAt(pos.x, mods) / engine.sr;
    double before = d.index > 0 ? points[d.index - 1].t : 0.0;
    bool hasAfter = d.index + 1 < (int)points.size();
    double after = hasAfter ? points[d.index + 1].t : 0.0;
    seconds = std::max(before, hasAfter ? std::min(after, seconds) : seconds);
    double value = automationValue(d.lane, pos.y);
    points[d.index] = {seconds, value};
    a.points = points;  // (still in order: it stays between its neighbours)
    showValueTip(d.lane, value, globalPos);
    update();
    automationEdited.emit();
}

void Timeline::showValueTip(const Lane& lane, double value, Point globalPos) {
    // the parameter's value as the plugin shows it, next to the mouse
    const Automation& a = *lane.automation;
    ui::App::get().showToolTip(globalPos, a.param + ": " + valueText(a, value), this);
}

void Timeline::deletePoint(const Lane& lane, int index) {
    Automation& a = *lane.automation;
    std::vector<AutoPoint> points = a.points.copy();
    if (index < 0 || index >= (int)points.size()) return;
    points.erase(points.begin() + index);
    a.points = points;
    update();
    automationEdited.emit();
}

void Timeline::automationMenu(Point globalPos, const Lane& lane, PointF pos) {
    // right-click an automation lane: delete the point there, clear the curve, or remove the lane
    AutomationPtr a = lane.automation;
    bool frozen = lane.owner->frozen != nullptr;
    int index = pointAt(lane, pos);
    if (index >= 0 && !frozen) {
        deletePoint(lane, index);
        return;
    }
    ui::Menu menu;
    ui::Action* clear = menu.addAction("Clear the curve");
    clear->setEnabled(!a->points.empty() && !frozen);
    ui::Action* remove = menu.addAction("Remove this lane (" + a->param + ")");
    remove->setEnabled(!frozen);
    ui::Action* chosen = menu.exec(globalPos, this);
    if (chosen == clear) {
        a->points = std::vector<AutoPoint>{};
        update();
        automationEdited.emit();
    } else if (chosen == remove) {
        removeAutomation(lane.owner, a);
    }
}

void Timeline::removeAutomation(const TrackPtr& track, const AutomationPtr& a) {
    std::vector<AutomationPtr> kept;
    for (auto& x : track->automation)
        if (x != a) kept.push_back(x);
    track->automation = kept;
    if (kept.empty()) track->showAutomation = false;
    lanesChanged();
}

void Timeline::lanesChanged() {
    // automation lanes were added, removed, shown or hidden: lay out again; the panel rebuilds
    relayout();
    update();
    tracksChanged.emit();
}

// --- mouse --------------------------------------------------------------------------------------

bool Timeline::mousePress(ui::MouseEvent& e) {
    PointF pos(e.pos);
    bool ctrl = e.ctrl();
    auto [clip, zone] = hit(pos);
    if (e.button == plat::RightButton) {  // the context menu acts on what is under the mouse
        if (clip && !selection.contains(clip)) {
            selection.selectOnly(clip);
            update();
        }
        return true;
    }
    if (e.button != plat::LeftButton) return true;
    pressPos_ = pos;  // (moves, trims, markers… start past DRAG_THRESHOLD_PX)
    pastThreshold_ = false;
    TrackPtr under = trackAt(pos.y);
    setCurrentTrack(under ? under : currentTrack);
    auto edge = laneEdgeAt(pos.y);
    if (edge.first || edge.second) {  // bottom edge of a lane: resize the track's height
        int h = edge.first ? edge.first->height : edge.second->height;
        drag = HeightDrag{edge.first, edge.second, pos.y, h};
        return true;
    }
    int loopEnd = loopEndAt(pos);
    if (loopEnd >= 0) {  // an end of the loop range on the ruler
        drag = LoopDrag{loopEnd == 1};
        return true;
    }
    if (auto marker = markerAt(pos)) {  // a marker: go there; dragging moves it
        int64_t frame = (int64_t)std::nearbyint(marker->seconds * engine.sr);
        setCursor(frame);
        if (engine.playing.load()) seek(frame);
        drag = MarkerDrag{*marker};
        return true;
    }
    auto lane = laneAt(pos.y);
    if (lane && lane->automation) {
        setCurrentTrack(lane->owner);
        automationPress(e, *lane);
        return true;
    }
    if (lane && lane->take) {  // a take lane: drag across the part to use
        setCurrentTrack(lane->owner);
        int64_t frame = snappedFrameAt(pos.x, e.mods);
        drag = TakeDrag{*lane, frame, frame};
        return true;
    }
    FadeHit fade = fadeHandleAt(pos);
    if (fade.clip && !ctrl) {  // a fade handle of an audio clip: drag to fade in or out
        if (!selection.contains(fade.clip)) selection.selectOnly(fade.clip);
        drag = FadeDrag{fade.clip, fade.in};
        update();
        return true;
    }
    auto end = edgeAt(pos);
    if (end.first && !ctrl) {  // an end of a clip: trim it, or bring back what was cut off
        if (!selection.contains(end.first)) selection.selectOnly(end.first);
        drag = EdgeDrag{end.first, end.second};
        update();
        return true;
    }
    if (zone == ClipZone::Handle) {
        if (ctrl) {
            drag = CopyDrag{clip, pos, e.alt()};  // (a click toggles it, a drag copies: mouseMove, mouseRelease;
                                                  // Ctrl+Alt: linked copies)
        } else {
            if (!selection.contains(clip)) selection.selectOnly(clip);
            startMove(pos, clip);
        }
    } else {  // ruler, empty lane space or a clip's body
        int64_t frame = snappedFrameAt(pos.x, e.mods);
        setCursor(frame);
        if (pos.y < config::RULER_H && engine.playing.load()) seek(frame);  // the ruler: playback jumps there too
        TrackPtr track = trackAt(pos.y);
        if (track) {  // not over the ruler, a group's header or the master lane
            RangeDrag d;
            d.x = pos.x;
            d.frame = frame;
            d.row = indexIn(visibleTracks(), track);
            d.clip = clip;
            if (ctrl) d.keep = selection.clips;
            d.endRow = d.row;
            drag = d;
        }
        if (!ctrl) selection.clear();
    }
    update();
    return true;
}

bool Timeline::stillAClick(PointF pos) {
    if (!pastThreshold_) {
        if (std::max(std::fabs(pos.x - pressPos_.x), std::fabs(pos.y - pressPos_.y)) < config::DRAG_THRESHOLD_PX)
            return true;
        pastThreshold_ = true;
    }
    return false;
}

void Timeline::startCopyDrag() {
    // copies of the selected clips (and the one grabbed) are dragged from where the originals are
    CopyDrag d = std::get<CopyDrag>(drag);
    std::vector<ClipPtr> chosen;
    for (auto& c : project.clips)
        if ((selection.contains(c) || c == d.clip) && !c->track->frozen) chosen.push_back(c);
    if (std::find(chosen.begin(), chosen.end(), d.clip) == chosen.end()) {
        drag = std::monostate();
        return;
    }
    std::vector<ClipPtr> all = project.clips.copy();
    ClipPtr grabbed;
    selection.clear();
    for (auto& c : chosen) {
        // (they share their audio and notes, as split pieces do; linked or independent: model.h)
        ClipPtr copy = d.linked ? links::linkedCopy(c) : links::plainCopy(c);
        all.push_back(copy);
        selection.clips.insert(copy);
        if (c == d.clip) grabbed = copy;
    }
    project.clips = all;
    startMove(d.pos, grabbed);
    if (auto* m = std::get_if<MoveDrag>(&drag)) m->altLinks = d.linked;  // (that Alt doesn't free it from the grid)
}

bool Timeline::mouseMove(ui::MouseEvent& e) {
    PointF pos(e.pos);
    if ((std::holds_alternative<MoveDrag>(drag) || std::holds_alternative<EdgeDrag>(drag) ||
         std::holds_alternative<FadeDrag>(drag) || std::holds_alternative<MarkerDrag>(drag) ||
         std::holds_alternative<PointDrag>(drag) || std::holds_alternative<LoopDrag>(drag) ||
         std::holds_alternative<CopyDrag>(drag)) &&
        stillAClick(pos))
        return true;
    if (std::holds_alternative<CopyDrag>(drag)) startCopyDrag();
    if (auto* d = std::get_if<HeightDrag>(&drag)) {
        double h = d->height + pos.y - d->y;
        if (d->track) setTrackHeight(*d->track, h);
        else setTrackHeight(*d->automation, h);
    } else if (std::holds_alternative<MoveDrag>(drag)) {
        moveSelected(pos, e.mods);
    } else if (std::holds_alternative<EdgeDrag>(drag)) {
        dragEdge(pos, e.mods);
    } else if (std::holds_alternative<FadeDrag>(drag)) {
        dragFade(pos);
    } else if (auto* t = std::get_if<TakeDrag>(&drag)) {
        t->to = snappedFrameAt(pos.x, e.mods);
        update();
    } else if (auto* m = std::get_if<MarkerDrag>(&drag)) {
        double seconds = (double)snappedFrameAt(pos.x, e.mods) / engine.sr;
        if (seconds != m->marker.seconds) {
            Marker moved{seconds, m->marker.name};
            Marker was = m->marker;
            m->marker = moved;
            replaceMarker(was, moved);
        }
    } else if (std::holds_alternative<PointDrag>(drag)) {
        dragPoint(pos, e.globalPos, e.mods);
    } else if (auto* l = std::get_if<LoopDrag>(&drag)) {
        int sr = engine.sr;
        const Timing& t = project.timing;
        int64_t frame = snappedFrameAt(pos.x, e.mods);
        int64_t start = (int64_t)std::nearbyint(t.loopStart * sr), end = (int64_t)std::nearbyint(t.loopEnd * sr);
        int64_t minLen = (int64_t)std::nearbyint(config::LOOP_MIN_S * sr);
        if (l->end) setLoop(start, std::max(frame, start + minLen));
        else setLoop(std::min(frame, end - minLen), end);
    } else if (std::holds_alternative<RangeDrag>(drag)) {
        selectRange(pos, e.mods);
    } else {
        updateCursor(pos);
    }
    return true;
}

bool Timeline::mouseRelease(ui::MouseEvent& e) {
    if (auto* d = std::get_if<CopyDrag>(&drag)) {  // Ctrl+click on a name strip, without dragging: in or out of the selection
        selection.toggle(d->clip);
        update();
    }
    if (auto* d = std::get_if<RangeDrag>(&drag)) {
        if (d->active && selection.range) {
            setCursor(selection.range->start);  // play from the start of the selection
        } else if (d->clip) {  // a click on a clip's body selects the clip
            if (!d->keep.empty()) selection.toggle(d->clip);
            else selection.selectOnly(d->clip);
            update();
        }
    }
    if (auto* d = std::get_if<MoveDrag>(&drag)) {
        std::vector<ClipPtr> moved;
        for (auto& o : d->origin)
            if (project.clips.contains(o.first)) moved.push_back(o.first);
        edits::clearUnder(project, moved);  // moved clips replace what they now cover
        auto shown = visibleTracks();       // (as when the drag began: its rows)
        std::map<ClipPtr, TrackPtr> tracks;
        std::map<ClipPtr, std::optional<int64_t>> starts;
        for (auto& [c, o] : d->origin) {
            tracks[c] = o.row < (int)shown.size() ? shown[o.row] : c->track;
            starts[c] = o.start;
        }
        rerenderMoved(tracks, starts);
        relayout();
    }
    if (std::holds_alternative<EdgeDrag>(drag)) endEdgeDrag();
    if (auto* t = std::get_if<TakeDrag>(&drag)) {
        if (t->to != t->frame) {
            TakeDrag d = *t;
            drag = std::monostate();
            comp(d.lane.owner, d.lane.take, std::min(d.frame, d.to), std::max(d.frame, d.to));
        }
    }
    drag = std::monostate();
    updateCursor(PointF(e.pos));
    return true;
}

bool Timeline::mouseDoubleClick(ui::MouseEvent& e) {
    PointF pos(e.pos);
    auto edge = laneEdgeAt(pos.y);
    auto lane = laneAt(pos.y);
    auto [clip, zone] = hit(pos);
    FadeHit fade = fadeHandleAt(pos);
    auto marker = markerAt(pos);
    if (marker) {  // double-click a marker: rename it
        drag = std::monostate();
        renameMarker(*marker);
    } else if (lane && lane->take) {  // double-click a take: use all of it
        drag = std::monostate();
        useTake(lane->owner, lane->take);
    } else if (edge.first || edge.second) {  // double-click a lane's bottom edge: back to the default height
        if (edge.first) setTrackHeight(*edge.first, defaultHeight(*edge.first));
        else setTrackHeight(*edge.second, defaultHeight(*edge.second));
    } else if (fade.clip) {  // double-click a fade handle: no fade on that side
        drag = std::monostate();
        if (fade.in) fade.clip->fadeIn = fade.clip->fadeLead = 0;
        else fade.clip->fadeOut = fade.clip->fadeTail = 0;
        update();
    } else if (lane && lane->automation) {  // double-click a point: delete it
        int index = pointAt(*lane, pos);
        if (index >= 0 && !lane->owner->frozen) {
            drag = std::monostate();
            deletePoint(*lane, index);
        }
    } else if (lane && lane->group) {  // double-click a group's header lane: fold or unfold it
        toggleFold(lane->group);
    } else if (zone == ClipZone::Handle) {  // double-click a clip's name strip: rename it
        selection.selectOnly(clip);
        drag = std::monostate();
        renameSelected();
    } else if (clip && clip->kind == Kind::Midi) {  // double-click a MIDI clip: edit its notes
        selection.selectOnly(clip);
        drag = std::monostate();
        update();
        editNotes.emit(clip);
    } else if (e.button == plat::LeftButton && lane && lane->track && lane->track->kind == Kind::Midi &&
               !lane->track->frozen) {
        drag = std::monostate();  // double-click empty space on a MIDI track: a new clip there, to edit
        ClipPtr made = newMidiClip(lane->track, snappedFrameAt(pos.x, e.mods));
        editNotes.emit(made);
    } else {
        return mousePress(e);
    }
    return true;
}

void Timeline::updateCursor(PointF pos) {
    ClipZone zone = hit(pos).second;
    auto edge = laneEdgeAt(pos.y);
    std::optional<Lane> lane;
    if (std::holds_alternative<HeightDrag>(drag) || edge.first || edge.second) {
        setCursor(ui::Cursor::SizeVer);
    } else if (std::holds_alternative<LoopDrag>(drag) || std::holds_alternative<EdgeDrag>(drag) ||
               std::holds_alternative<FadeDrag>(drag) || std::holds_alternative<MarkerDrag>(drag) || loopEndAt(pos) >= 0) {
        setCursor(ui::Cursor::SizeHor);
    } else if ((lane = laneAt(pos.y)) && lane->automation) {
        bool dragging = std::holds_alternative<PointDrag>(drag);
        bool grabbed = dragging || pointAt(*lane, pos) >= 0;
        setCursor(dragging ? ui::Cursor::ClosedHand : grabbed ? ui::Cursor::OpenHand : ui::Cursor::Cross);
    } else if (std::holds_alternative<std::monostate>(drag) && markerAt(pos)) {
        setCursor(ui::Cursor::Hand);  // a marker: click to go there, drag to move it
    } else if (std::holds_alternative<std::monostate>(drag) && (fadeHandleAt(pos).clip || edgeAt(pos).first)) {
        setCursor(ui::Cursor::SizeHor);  // an end of a clip: drag to trim or lengthen it
    } else if (zone == ClipZone::Handle) {
        setCursor(std::holds_alternative<MoveDrag>(drag) ? ui::Cursor::ClosedHand : ui::Cursor::OpenHand);
    } else if (trackAt(pos.y)) {
        setCursor(ui::Cursor::IBeam);  // selecting a range
    } else {
        setCursor(ui::Cursor::Arrow);
    }
}

bool Timeline::contextMenu(ui::MouseEvent& e) {
    if (e.pos.y < config::RULER_H) {
        rulerMenu(e.globalPos, PointF(e.pos));
        return true;
    }
    auto lane = laneAt(e.pos.y);
    if (lane && lane->automation) {
        automationMenu(e.globalPos, *lane, PointF(e.pos));
        return true;
    }
    if (lane && lane->take) {
        ui::Menu menu;
        fillTakeMenu(menu, *lane);
        menu.exec(e.globalPos, this);
        return true;
    }
    ui::Menu fadeMenu;  // a fade handle: that fade's shapes (model.h FADE SHAPES), for its clip alone
    if (fillFadeMenu(fadeMenu, PointF(e.pos))) {
        fadeMenu.exec(e.globalPos, this);
        return true;
    }
    showMenu(e.globalPos, e.pos);
    return true;
}

void Timeline::showMenu(Point globalPos, std::optional<Point> pos) {
    // the right-click menu; pos: where it was asked for on the timeline (none: not over a lane)
    ui::Menu menu;
    fillMenu(menu, pos);
    menu.exec(globalPos, this);
}

void Timeline::fillMenu(ui::Menu& menu, std::optional<Point> pos) {
    bool hasRange = selection.range.has_value(), hasClips = !selection.clips.empty();
    TrackPtr under = pos ? trackAt(pos->y) : nullptr;
    std::vector<ClipPtr> midiClips, audioClips, loopClips;
    for (auto& c : project.clips) {  // (in the project's order)
        if (!selection.contains(c)) continue;
        if (c->kind == Kind::Midi) midiClips.push_back(c);
        if (c->kind == Kind::Audio) audioClips.push_back(c);
        if (!c->track->frozen) loopClips.push_back(c);
    }
    std::vector<TrackPtr> rangeMidi;
    if (hasRange)
        for (auto& t : selection.range->tracks)
            if (t->kind == Kind::Midi) rangeMidi.push_back(t);
    // (Python shows "double-click" as this entry's shortcut; the toolkit's menus show key sequences only)
    ui::Action* edit = menu.addAction("Edit notes…", [this, midiClips] { editNotes.emit(midiClips[0]); });
    edit->setEnabled(midiClips.size() == 1);
    ui::Action* made = menu.addAction(std::string("New MIDI clip") + (rangeMidi.empty() ? "" : " over the range"),
                                      [this, rangeMidi, under, pos] {
        std::vector<ClipPtr> news;
        if (!rangeMidi.empty()) {  // one per MIDI track of the range, as long as the range
            TimeRange r = *selection.range;
            for (auto& t : rangeMidi) news.push_back(newMidiClip(t, r.start, r.end - r.start));
        } else {
            news.push_back(newMidiClip(under, snappedFrameAt(pos->x, 0)));
        }
        if (news.size() == 1) editNotes.emit(news[0]);
    });
    made->setEnabled(!rangeMidi.empty() || (under && under->kind == Kind::Midi && !under->frozen));
    menu.addSeparator();
    ui::Action* duplicate = menu.addAction(hasRange ? "Duplicate range" : "Duplicate clips", [this] { duplicateSelected(); },
                                           "Ctrl+D");
    duplicate->setEnabled(hasRange || hasClips);
    // linked MIDI clips (timeline_links.cpp)
    std::vector<ClipPtr> linkable, linkedClips;
    for (auto& c : midiClips) {
        if (!c->track->frozen) linkable.push_back(c);
        if (links::linked(project, *c)) linkedClips.push_back(c);
    }
    ui::Action* dupLinked = menu.addAction("Duplicate as linked", [this] { duplicateLinkedSelected(); });
    dupLinked->tooltip = "Copies of the selected MIDI clips right after them, LINKED to them: editing the notes of "
                         "one changes them all (Ctrl+Alt+drag a clip's name strip does the same)";
    dupLinked->setEnabled(!linkable.empty());
    ui::Action* unique = menu.addAction("Make unique", [this] { makeUniqueSelected(); });
    unique->tooltip = "The selected linked clips keep their notes but no longer change with their linked copies";
    unique->setEnabled(!linkedClips.empty());
    ui::Action* split = menu.addAction(hasRange ? "Split at selection edges" : "Split at cursor", [this] { splitSelected(); },
                                       "S");
    split->setEnabled(hasRange || hasClips);
    ui::Action* del = menu.addAction(hasRange ? "Cut out selected range" : "Delete clips", [this] { deleteSelected(); },
                                     "Del");
    del->setEnabled(hasRange || hasClips);
    menu.addSeparator();
    ui::Action* color = menu.addAction("Clip colour…", [this] { chooseColor(); }, "C");
    color->setEnabled(hasClips);
    ui::Action* rename = menu.addAction(selection.clips.size() < 2 ? "Rename clip…" : "Rename clips…",
                                        [this] { renameSelected(); }, "Ctrl+R");
    rename->setEnabled(hasClips);
    bool allDisabled = hasClips;
    for (auto& c : selection.clips)
        if (!c->disabled) allDisabled = false;
    ui::Action* disable = menu.addAction(allDisabled ? "Enable clips" : "Disable clips", [this] { toggleDisabled(); }, "0");
    disable->setEnabled(hasClips);
    bool allLoop = !loopClips.empty();
    for (auto& c : loopClips)
        if (!c->loop) allLoop = false;
    ui::Action* loopNotes = menu.addAction("Loop (it repeats when it is made longer)",
                                           [this, loopClips, allLoop] { setLoopNotes(loopClips, !allLoop); });
    loopNotes->checkable = true;
    loopNotes->checked = allLoop;
    loopNotes->setEnabled(!loopClips.empty());
    ui::Action* exportMidiAction = menu.addAction("Export as MIDI file…", [this, midiClips] { exportMidi.emit(midiClips); });
    exportMidiAction->setEnabled(!midiClips.empty());
    menu.addSeparator();
    auto clipboardAction = [this, pos](const std::string& what) {
        return [this, pos, what] {
            if (pos && trackAt(pos->y)) setCurrentTrack(trackAt(pos->y));  // (paste: from the lane right-clicked)
            clipboardChosen.emit(what);
        };
    };
    ui::Action* cut = menu.addAction("Cut", clipboardAction("cut"), "Ctrl+X");
    cut->setEnabled(hasRange || hasClips);
    ui::Action* copy = menu.addAction("Copy", clipboardAction("copy"), "Ctrl+C");
    copy->setEnabled(hasRange || hasClips);
    menu.addAction("Paste at the cursor", clipboardAction("paste"), "Ctrl+V");
    ui::Action* gain = menu.addAction("Clip gain…", [this] { setGainSelected(); });
    gain->setEnabled(!audioClips.empty());
    bool anyFades = false, allReversed = !audioClips.empty();
    for (auto& c : audioClips) {
        if (c->fadeIn || c->fadeOut) anyFades = true;
        if (!c->reverse) allReversed = false;
    }
    ui::Action* noFades = menu.addAction("Remove fades", [this] { removeFadesSelected(); });
    noFades->setEnabled(anyFades);
    addFadeShapeMenus(menu, audioClips);  // Fade-in shape ▸, Fade-out shape ▸ (model.h FADE SHAPES)
    ui::Action* stretch = menu.addAction("Stretch and pitch…", [this] { stretchSelected(); });
    stretch->setEnabled(!audioClips.empty());
    ui::Action* reverse = menu.addAction(allReversed ? "Play forwards again" : "Reverse (play backwards)",
                                         [this] { reverseSelected(); });
    reverse->setEnabled(!audioClips.empty());
    ui::Action* crossfade = menu.addAction("Crossfade…", [this] { crossfadeSelected(); });
    crossfade->setEnabled(audioClips.size() == 2);
    ui::Action* join = menu.addAction("Join clips", [this] { joinSelected(); }, "Ctrl+J");
    join->setEnabled(hasRange || selection.clips.size() >= 1);
    ui::Action* group = menu.addAction("Group these tracks", [this] { groupSelected(); }, "Ctrl+G");
    group->setEnabled(hasRange || hasClips);
    ui::Action* loopRange = menu.addAction("Loop this range", [this] {
        setLoop(selection.range->start, selection.range->end);
        loopRangeChosen.emit();
    });
    loopRange->setEnabled(hasRange);
    menu.addSeparator();
    menu.addAction("Select all clips", [this] { selectAll(); }, "Ctrl+A");
}

void Timeline::startMove(PointF pos, const ClipPtr& grabbed) {
    selection.range.reset();  // the range stays put in time; only clips move
    auto tracks = visibleTracks();  // clips move between the lanes shown
    std::map<const Track*, int> rows;
    for (size_t i = 0; i < tracks.size(); ++i) rows[tracks[i].get()] = (int)i;
    MoveDrag d;
    for (auto& c : selection.clips) {
        auto it = rows.find(c->track.get());
        if (it != rows.end()) d.origin[c] = {c->start, it->second};
    }
    // moved clips go last: drawn on top of the clips they pass over (and hit first)
    std::vector<ClipPtr> order;
    for (auto& c : project.clips)
        if (!d.origin.count(c)) order.push_back(c);
    for (auto& c : project.clips)
        if (d.origin.count(c)) order.push_back(c);
    project.clips = order;
    d.frame = frameAt(pos.x);
    TrackPtr under = trackAt(pos.y);
    auto it = rows.find(under.get());
    d.row = it != rows.end() ? it->second : 0;
    d.anchor = grabbed->start;
    drag = d;
    setCursor(ui::Cursor::ClosedHand);
}

void Timeline::moveSelected(PointF pos, int mods) {
    // Shift every selected clip by the same time and lane offset. In bars and beats the clip grabbed
    // starts on the nearest grid line (Alt: freely); the others keep their distance to it. Clips stay
    // at or after 0 s, and only change lanes when every clip lands on a lane of its own kind (audio
    // on audio, MIDI on MIDI).
    auto& d = std::get<MoveDrag>(drag);
    if (d.origin.empty()) return;
    auto tracks = visibleTracks();
    const Timing& t = project.timing;
    int64_t df = frameAt(pos.x) - d.frame;
    if (d.altLinks && !(mods & plat::ModAlt)) d.altLinks = false;  // (the Alt of Ctrl+Alt let go: Alt frees it again)
    if (t.snaps((mods & plat::ModAlt) && !d.altLinks)) {
        int sr = engine.sr;
        df = (int64_t)std::nearbyint(t.snap((double)(d.anchor + df) / sr, pps) * sr) - d.anchor;
    }
    int64_t earliest = d.origin.begin()->second.start;
    for (auto& o : d.origin) earliest = std::min(earliest, o.second.start);
    df = std::max(df, -earliest);
    TrackPtr under = trackAt(pos.y);  // over a group's header or the master lane: lanes stay as they are
    int dr = under ? indexIn(tracks, under) - d.row : d.rowsMoved;
    bool fits = true;
    for (auto& o : d.origin) {
        int row = o.second.row + dr;
        if (!(0 <= row && row < (int)tracks.size() && tracks[row]->kind == o.first->kind && !tracks[row]->frozen))
            fits = false;
    }
    if (fits) d.rowsMoved = dr;
    for (auto& o : d.origin) {
        int row = std::min((int)tracks.size() - 1, std::max(0, o.second.row + d.rowsMoved));
        o.first->start = o.second.start + df;
        std::atomic_store(&o.first->track, tracks[row]);  // (the audio thread atomic_loads it)
    }
    update();
}

void Timeline::selectRange(PointF pos, int mods) {
    auto& d = std::get<RangeDrag>(drag);
    if (!d.active && std::fabs(pos.x - d.x) < config::DRAG_THRESHOLD_PX) return;
    d.active = true;
    int64_t frame = snappedFrameAt(pos.x, mods);
    auto visible = visibleTracks();
    auto lane = laneAt(pos.y);
    if (lane && (lane->track || lane->owner)) {  // (an automation lane counts as its track's)
        d.endRow = indexIn(visible, lane->track ? lane->track : lane->owner);
    } else if (lane && lane->isMaster()) {  // the master lane: down to the last track
        d.endRow = (int)visible.size() - 1;
    }
    // over a group's header the range keeps its lanes
    int row = d.endRow;
    int64_t start = std::min(d.frame, frame), end = std::max(d.frame, frame);
    std::vector<TrackPtr> tracks;
    int r0 = std::max(0, std::min(d.row, row)), r1 = std::min((int)visible.size() - 1, std::max(d.row, row));
    for (int i = r0; i <= r1; ++i)
        if (!visible[i]->frozen) tracks.push_back(visible[i]);
    if (start == end || tracks.empty()) {  // both edges on the same grid line, or only frozen lanes: no range
        selection.range.reset();
        selection.clips = d.keep;
    } else {
        std::vector<ClipPtr> clips(d.keep.begin(), d.keep.end());
        for (auto& c : project.clipsIn(tracks, start, end)) clips.push_back(c);
        selection.setRange(TimeRange{start, end, tracks}, clips);
    }
    update();
}

// --- wheel and keys -----------------------------------------------------------------------------

bool Timeline::wheel(ui::WheelEvent& e) {
    wheel(e.dy * 120.0, e.mods, e.pos.x);
    return true;
}

void Timeline::wheel(double delta, int mods, double x) {
    // Ctrl+wheel zooms around timeline x; the wheel alone scrolls sideways
    if (delta == 0) return;  // (a sideways-only wheel event: Qt's angleDelta().y() is 0)
    if (mods & plat::ModCtrl) {
        zoom(delta > 0 ? config::ZOOM_STEP : 1 / config::ZOOM_STEP, x);
    } else {
        ui::ScrollBar* hbar = scroll->horizontalScrollBar();
        hbar->setValue(hbar->value() - (int)std::lround(delta));
    }
}

bool Timeline::keyPress(ui::KeyEvent& e) {
    int key = e.key >= 'a' && e.key <= 'z' ? e.key - 'a' + 'A' : e.key;
    const int mods = e.mods & (plat::ModShift | plat::ModCtrl | plat::ModAlt | plat::ModMeta);
    const int ctrlShift = plat::ModCtrl | plat::ModShift;
    // (Ctrl+C / X / V, Ctrl+Shift+I and Ctrl+Shift+Del are the main window's Edit menu shortcuts and come
    // here only without them: they then do the same)
    if (key == plat::Key_Delete && mods == ctrlShift) timeEdited.emit(deleteTime());
    else if (key == 'I' && mods == ctrlShift) timeEdited.emit(insertTime());
    else if ((key == 'C' || key == 'X' || key == 'V') && mods == plat::ModCtrl)
        clipboardChosen.emit(key == 'C' ? "copy" : key == 'X' ? "cut" : "paste");
    else if (key == plat::Key_Delete || key == plat::Key_Backspace) deleteSelected();
    else if (key == 'S' && !mods) splitSelected();
    else if (key == 'C' && !mods) chooseColor();
    else if (key == 'A' && (mods & plat::ModCtrl)) selectAll();
    else if (key == 'G' && (mods & plat::ModCtrl)) groupSelected();
    else if (key == 'D' && (mods & plat::ModCtrl)) duplicateSelected();
    else if (key == 'J' && (mods & plat::ModCtrl)) joinSelected();
    else if (key == 'R' && (mods & plat::ModCtrl)) renameSelected();
    else if (key == '0' && !mods) toggleDisabled();
    else if (key == plat::Key_Escape) clearSelection();
    else return false;
    return true;
}

// --- dropping files from the Explorer or the desktop --------------------------------------------

void Timeline::dragEnter(ui::DragEvent& e) {
    if (e.data && !e.data->files.empty()) e.accepted = true;
}

void Timeline::drop(ui::DragEvent& e) {
    // Each file goes on the lane under the cursor (then the ones below) if the kind matches;
    // otherwise, or on the master lane, onto a new track of its kind. Dropped on a group's header,
    // files go onto new tracks in that group. On the grid in bars and beats, unless Alt.
    if (!e.data) return;
    std::vector<std::string> paths = e.data->files;
    dropPaths(paths, snappedFrameAt(e.pos.x, e.mods), laneAt(e.pos.y));
    e.accepted = true;
}

void Timeline::dropPaths(const std::vector<std::string>& paths, int64_t frame, const std::optional<Lane>& lane) {
    auto tracks = visibleTracks();
    TrackPtr track = lane ? (lane->track ? lane->track : lane->owner) : nullptr;  // (an automation lane: its track)
    int row = track ? indexIn(tracks, track) : (int)tracks.size();
    if (row < 0) row = (int)tracks.size();
    GroupPtr group = lane ? lane->group : nullptr;
    if (group && group->folded) {
        group->folded = false;
        tracksRegrouped();
    }
    int i = 0;
    for (auto& path : paths) {
        if (path.empty() || util::isDir(path)) continue;
        ClipPtr clip = loadClip(path);
        if (clip) {
            TrackPtr target = row + i < (int)tracks.size() ? tracks[row + i] : nullptr;
            if (target && (target->kind != clip->kind || target->frozen)) target = nullptr;
            placeClip(clip, frame, target ? target : addTrack(clip->kind, group));
        }
        ++i;
    }
}
