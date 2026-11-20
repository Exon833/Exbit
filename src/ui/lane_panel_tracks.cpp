// The lane panel's TRACK SELECTION (lane_panel.h): selecting several tracks, duplicating and moving them, and a
// track's row (TrackRow) that selects its track when clicked and moves it when dragged.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/patch.h"
#include "core/util.h"
#include "model/track_ops.h"
#include "ui/lane_panel.h"
#include "ui/timeline.h"

namespace {
constexpr int DRAG_PX = 6;  // how far a row must be dragged before it moves its track (a click stays a click)

bool has(const std::vector<TrackPtr>& ts, const TrackPtr& t) { return std::find(ts.begin(), ts.end(), t) != ts.end(); }
}  // namespace

// ============================================================================================
// TrackRow
// ============================================================================================

TrackRow::TrackRow(LanePanel& panel, TrackPtr track, Color color, std::optional<Color> accent, int height,
                   std::optional<Color> text)
    : LaneFrame(color, accent, height, text), panel_(panel), track_(std::move(track)) {
    // (any other button, a right-click: the current track; the menu is the selection's when it is in it)
    onPress = [this] {
        if (has(panel_.selected, track_)) panel_.timeline.setCurrentTrack(track_);
        else panel_.select(track_);
    };
}

bool TrackRow::mousePress(ui::MouseEvent& e) {
    if (e.button != plat::LeftButton) return LaneFrame::mousePress(e);
    panel_.timeline.clearSelection();  // (Ctrl+D, which the timeline takes, then duplicates the track, not clips)
    panel_.timeline.setFocus();
    panel_.select(track_, e.mods);
    pressed_ = true;
    dragging_ = false;
    startY_ = e.globalPos.y;
    return true;
}

bool TrackRow::mouseMove(ui::MouseEvent& e) {
    if (pressed_ && !dragging_ && std::abs(e.globalPos.y - startY_) >= DRAG_PX) {
        dragging_ = true;
        setCursor(ui::Cursor::ClosedHand);
        if (panel_.status) panel_.status("Moving " + track_->name + ": let go over the row it goes to", 3000);
    }
    return true;
}

bool TrackRow::mouseRelease(ui::MouseEvent& e) {
    bool drop = dragging_;
    pressed_ = dragging_ = false;
    setCursor(ui::Cursor::Arrow);
    if (drop) panel_.dropAt(track_, panel_.mapFromGlobal(e.globalPos).y);  // (the rows are made again)
    return true;
}

// ============================================================================================
// LanePanel: TRACK SELECTION
// ============================================================================================

std::vector<TrackPtr> LanePanel::targets(const TrackPtr& track) const {
    std::vector<TrackPtr> out;
    if (has(selected, track))
        for (auto& t : project.tracks)
            if (has(selected, t)) out.push_back(t);
    if (out.empty()) out.push_back(track);
    return out;
}

void LanePanel::select(const TrackPtr& track, int mods) {
    if (!track || !project.tracks.contains(track)) return;
    TrackPtr from = anchor_ && project.tracks.contains(anchor_) ? anchor_ : timeline.currentTrack;
    if ((mods & plat::ModShift) && from && project.tracks.contains(from)) {  // from the one clicked last
        int a = project.indexOf(from), b = project.indexOf(track);
        selected.clear();
        for (int i = std::min(a, b); i <= std::max(a, b); i++) selected.push_back(project.tracks[(size_t)i]);
    } else if (mods & plat::ModCtrl) {  // added, or taken out
        anchor_ = track;
        auto it = std::find(selected.begin(), selected.end(), track);
        if (it != selected.end() && selected.size() > 1) {
            selected.erase(it);
            if (!has(selected, timeline.currentTrack)) timeline.setCurrentTrack(selected.back());
            showCurrentTrack();
            util::debugf("lane_panel: %s taken out of the selection (%zu tracks)\n", track->name.c_str(), selected.size());
            return;
        }
        if (it == selected.end()) selected.push_back(track);
    } else {
        anchor_ = track;
        selected = {track};
    }
    timeline.setCurrentTrack(track);
    showCurrentTrack();
    util::debugf("lane_panel: %s selected (%zu tracks)\n", track->name.c_str(), selected.size());
    if (selected.size() > 1 && status) status(std::to_string(selected.size()) + " tracks selected", 2000);
}

void LanePanel::say(const std::string& what, size_t count) {
    util::debugf("lane_panel: %s: %zu track(s)\n", what.c_str(), count);
    if (count > 1 && status) status(what + ": " + std::to_string(count) + " tracks", 3000);
}

void LanePanel::showMixer(const TrackPtr& track) {
    auto it = mixers.find(track);
    if (it == mixers.end()) return;
    int db = (int)std::nearbyint(track->volumeDb);
    it->second.slider->setValueSilently(db);
    it->second.reading->showValue(db);
    it->second.pan->showValue(track->pan);
}

void LanePanel::duplicate(const TrackPtr& track, bool withClips) {
    if (!track || !project.tracks.contains(track)) return;
    std::vector<std::pair<TrackPtr, TrackPtr>> made;  // (original, copy)
    std::vector<SlotPtr> slots;
    for (auto& t : targets(track)) {
        made.emplace_back(t, track_ops::copyOf(*t));
        for (auto& s : track_ops::slotsOf(*made.back().second)) slots.push_back(s);
    }
    // the copies' plugins loaded first, then all of them in at once (one undo step)
    std::vector<std::string> missing;
    std::weak_ptr<bool> alive = alive_;
    if (!slots.empty() && loadPlugins) missing = loadPlugins(slots);
    if (alive.expired()) return;
    std::vector<TrackPtr> copies;
    for (auto& [t, c] : made) {
        if (!project.tracks.contains(t)) continue;  // (removed while its plugins loaded)
        track_ops::add(project, t, c, withClips);
        copies.push_back(c);
    }
    if (copies.empty()) return;
    selected = copies;
    anchor_ = copies.front();
    timeline.setCurrentTrack(copies.front());  // (first: the selection stays the copies)
    timeline.relayout();
    timeline.tracksChanged.emit();  // (the rows)
    std::string text = copies.size() > 1 ? "Duplicated " + std::to_string(copies.size()) + " tracks"
                                         : "Duplicated " + track->name + " as " + copies.front()->name;
    if (!withClips) text += " (without clips)";
    for (size_t i = 0; i < missing.size(); i++) text += (i ? ", " : "; not loaded: ") + missing[i];
    util::debugf("lane_panel: %s\n", text.c_str());
    if (status) status(text, 4000);
}

void LanePanel::move(const TrackPtr& track, int dir) {
    if (!track || !project.tracks.contains(track)) return;
    auto ts = targets(track);
    if (!track_ops::step(project, ts, dir)) {
        if (status) status(std::string("Already at the ") + (dir < 0 ? "top" : "bottom"), 2000);
        return;
    }
    timeline.tracksRegrouped();  // (the lanes and rows)
    say(dir < 0 ? "Moved up" : "Moved down", ts.size());
}

void LanePanel::dropAt(const TrackPtr& track, int y) {
    if (!track || !project.tracks.contains(track)) return;
    // the row under y: above or below a track (by which half), under an automation or take lane's track, at the
    // top of a group (its header); past them all: under the last track (of its kind: a return among the returns)
    TrackPtr target;
    bool below = true;
    for (const Lane& l : lanes::layout(project)) {
        if (y < l.top || y >= l.bottom()) continue;
        if (l.track) {
            target = l.track;
            below = y >= l.top + l.height / 2;
        } else if (l.owner) {
            target = l.owner;
        } else if (l.group) {
            auto members = project.members(l.group);
            if (!members.empty()) target = members.front(), below = false;
        }
        break;
    }
    if (!target) {
        for (auto& t : project.tracks)
            if ((t->kind == Kind::Return) == (track->kind == Kind::Return)) target = t;
        below = true;
    }
    auto ts = targets(track);
    if (!track_ops::moveTo(project, ts, target, below)) return;
    timeline.tracksRegrouped();
    say("Moved", ts.size());
}

int LanePanel::addToSelected(const TrackPtr& owner, const SlotPtr& slot) {
    std::vector<std::pair<TrackPtr, SlotPtr>> adds;
    std::vector<SlotPtr> slots;
    for (auto& t : targets(owner))
        if (t != owner && !t->frozen) {
            adds.emplace_back(t, track_ops::freshCopy(slot));
            slots.push_back(adds.back().second);
        }
    if (adds.empty()) return 0;
    std::weak_ptr<bool> alive = alive_;
    if (loadPlugins) loadPlugins(slots);
    if (alive.expired()) return 0;
    for (auto& [t, s] : adds) {
        std::vector<SlotPtr> fx = t->effects.copy();
        fx.push_back(s);
        t->effects = fx;  // (first: a cable never leads to an effect that isn't there)
        t->cables = track_ops::wiredAtEnd(*t->cables, s.get());
        patch::ensureState(*t);
    }
    timeline.tracksChanged.emit();  // (the rows' FX buttons, the open effects windows)
    say("Added " + slot->name, adds.size() + 1);
    return (int)adds.size();
}
