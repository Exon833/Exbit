// Copy, cut and paste clips on the timeline (clipboard.py).
#include "ui/clipboard.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/util.h"
#include "model/edits.h"
#include "ui/timeline.h"

namespace clipboard {

std::optional<Copied> held;
std::optional<CopiedCurve> heldCurve;

namespace {
int indexIn(const std::vector<TrackPtr>& tracks, const TrackPtr& t) {
    auto it = std::find(tracks.begin(), tracks.end(), t);
    return it == tracks.end() ? -1 : (int)(it - tracks.begin());
}
}  // namespace

int copy(Timeline& timeline) {
    Selection& sel = timeline.selection;
    std::vector<TrackPtr> shown = timeline.visibleTracks();
    std::vector<ClipPtr> pieces;
    int64_t start, end;
    if (sel.range) {
        const TimeRange& r = *sel.range;
        for (auto& c : timeline.project.clipsIn(r.tracks, r.start, r.end))
            if (ClipPtr piece = edits::pieceWithin(c, r.start, r.end)) pieces.push_back(piece);
        start = r.start;
        end = r.end;
    } else {
        for (auto& c : timeline.project.clips)  // (in the project's order: the selection is a set)
            if (sel.contains(c) && indexIn(shown, c->track) >= 0) pieces.push_back(copyClip(c));
        if (pieces.empty()) return 0;
        start = pieces[0]->start;
        end = pieces[0]->end();
        for (auto& c : pieces) {
            start = std::min(start, c->start);
            end = std::max(end, c->end());
        }
    }
    std::vector<ClipPtr> kept;
    for (auto& p : pieces)
        if (indexIn(shown, p->track) >= 0) kept.push_back(p);
    if (kept.empty()) return 0;
    int top = (int)shown.size();
    for (auto& p : kept) top = std::min(top, indexIn(shown, p->track));
    Copied c;
    // the automation over what was copied (auto_follow.h): the range on each of its tracks, or each clip's span
    int sr = timeline.engine.sr;
    auto lanes = [&](const TrackPtr& t, int64_t from, int64_t frames) {
        auto pieces = auto_follow::take(*t, (double)from / sr, (double)frames / sr);
        if (!pieces.empty()) c.automation.push_back({t, indexIn(shown, t), from - start, frames, std::move(pieces)});
    };
    if (sel.range) {
        for (auto& t : sel.range->tracks)
            if (indexIn(shown, t) >= 0) lanes(t, start, end - start);
    } else {
        for (auto& p : kept) lanes(p->track, p->start, p->frames());
    }
    for (auto& a : c.automation) top = std::min(top, a.lane);
    for (auto& a : c.automation) a.lane -= top;
    for (auto& p : kept) c.clips.push_back({p, p->start - start, indexIn(shown, p->track) - top});
    c.span = end - start;
    c.sr = sr;
    held = std::move(c);
    return (int)kept.size();
}

std::vector<ClipPtr> paste(Timeline& timeline, std::string& reason) {
    reason.clear();
    if (!held || held->clips.empty()) {
        reason = "Nothing copied yet: select clips (or a time range) and press Ctrl+C";
        return {};
    }
    if (held->sr != timeline.engine.sr) {
        reason = "What was copied was at another sample rate: copy it again";
        return {};
    }
    std::vector<TrackPtr> shown = timeline.visibleTracks();
    TrackPtr first = indexIn(shown, timeline.currentTrack) >= 0 ? timeline.currentTrack : nullptr;
    if (!first)
        for (auto& item : held->clips)
            if (indexIn(shown, item.clip->track) >= 0) {
                first = item.clip->track;
                break;
            }
    if (!first) {
        reason = "Select the track to paste onto first: click its lane or its row";
        return {};
    }
    int top = indexIn(shown, first);
    std::vector<ClipPtr> placed;
    for (auto& item : held->clips) {
        if (top + item.lane >= (int)shown.size()) {
            reason = "There aren't enough tracks below the selected one for what was copied";
            return {};
        }
        const TrackPtr& track = shown[top + item.lane];
        if (track->kind != item.clip->kind || track->frozen) {
            std::string kind = item.clip->kind == Kind::Midi ? "MIDI" : "audio";
            reason = track->name + " can't take " + kind + " clips" + (track->frozen ? " (it is frozen)" : "") +
                     ": select another track to paste onto";
            return {};
        }
        ClipPtr copy = links::plainCopy(item.clip);
        copy->track = track;
        copy->start = timeline.cursor + item.offset;
        placed.push_back(copy);
    }
    // linked MIDI clips copied together stay linked among themselves (a new link: not to the originals)
    std::map<const ClipLink*, std::vector<ClipPtr>> together;
    for (size_t i = 0; i < placed.size(); ++i)
        if (held->clips[i].clip->link) together[held->clips[i].clip->link.get()].push_back(placed[i]);
    for (auto& kv : together) {
        if (kv.second.size() < 2) continue;
        auto link = std::make_shared<ClipLink>();
        for (auto& c : kv.second) c->link = link;
    }
    for (auto& c : placed)  // (their new tracks' sound: rendered before the audio thread sees them)
        if (c->kind == Kind::Midi) c->prepare(timeline.engine.sr);
    edits::placeCopies(timeline.project, placed);
    if (timeline.project.timing.automationFollows) {  // (their automation along: auto_follow.h)
        bool added = false;
        int laid = 0;
        double sr = timeline.engine.sr;
        for (auto& a : held->automation) {
            int row = top + a.lane;
            if (row < 0 || row >= (int)shown.size() || shown[(size_t)row]->frozen) continue;
            laid += auto_follow::lay(*shown[(size_t)row], *a.from, a.pieces, (double)(timeline.cursor + a.offset) / sr,
                                     (double)a.length / sr, added);
        }
        if (laid) util::debugf("paste: %d automation curve pieces laid with the clips\n", laid);
        if (added) timeline.lanesChanged();
    }
    timeline.selection.clear();
    timeline.selection.clips = std::set<ClipPtr>(placed.begin(), placed.end());
    timeline.setCursor(timeline.cursor + held->span);
    timeline.relayout();
    return placed;
}

int cut(Timeline& timeline) {
    int count = copy(timeline);
    if (!count) return count;
    if (timeline.project.timing.automationFollows) {  // (the automation went with the clips: auto_follow.h)
        double sr = timeline.engine.sr;
        int64_t start = held->clips[0].clip->start - held->clips[0].offset;  // (where the copied part starts)
        for (auto& a : held->automation)
            for (auto& piece : a.pieces)
                piece.lane->setPoints(
                    auto_follow::cleared(*piece.lane->points, (double)(start + a.offset) / sr, (double)a.length / sr));
    }
    timeline.deleteSelected();
    return count;
}

AutomationPtr laneInUse(const Timeline& timeline, bool needsRange) {
    const AutomationPtr& lane = timeline.currentAutomation;
    if (!lane) return nullptr;
    for (auto& t : timeline.project.tracks)
        if (t->automation.contains(lane)) {
            if (t->frozen) return nullptr;
            if (!needsRange) return lane;
            const auto& r = timeline.selection.range;
            return r && std::find(r->tracks.begin(), r->tracks.end(), t) != r->tracks.end() ? lane : nullptr;
        }
    return nullptr;
}

bool clearLaneRange(Timeline& timeline) {
    AutomationPtr lane = laneInUse(timeline);
    if (!lane) return false;
    const TimeRange& r = *timeline.selection.range;
    double sr = timeline.engine.sr;
    lane->setPoints(auto_follow::cleared(*lane->points, (double)r.start / sr, (double)(r.end - r.start) / sr));
    util::debugf("automation lane %s: %.3f-%.3f s cleared\n", lane->param.c_str(), r.start / sr, r.end / sr);
    timeline.update();
    timeline.automationEdited.emit();
    return true;
}

namespace {
// Ctrl+C / X / V in an automation lane (see AUTOMATION at the top): what to say in the status bar
std::string useLane(Timeline& timeline, const AutomationPtr& lane, const std::string& what) {
    double sr = timeline.engine.sr;
    if (what == "paste") {
        double at = (double)timeline.cursor / sr;
        lane->setPoints(auto_follow::pasted(*lane->points, heldCurve->points, at, heldCurve->length));
        util::debugf("automation lane %s: %zu points pasted at %.3f s\n", lane->param.c_str(), heldCurve->points.size(), at);
        timeline.setCursor(timeline.cursor + (int64_t)std::llround(heldCurve->length * sr));
        timeline.update();
        timeline.automationEdited.emit();
        return "Pasted the curve into " + lane->param;
    }
    const TimeRange& r = *timeline.selection.range;
    double start = (double)r.start / sr, length = (double)(r.end - r.start) / sr;
    if (lane->points.empty()) return lane->param + " has no curve to " + what + " yet";
    heldCurve = CopiedCurve{auto_follow::cut(*lane->points, start, length), length};
    util::debugf("automation lane %s: %.3f s of its curve copied (%zu points)\n", lane->param.c_str(), length,
                 heldCurve->points.size());
    if (what == "cut") clearLaneRange(timeline);
    return std::string(what == "cut" ? "Cut" : "Copied") + " the selected part of " + lane->param;
}
}  // namespace

std::string use(Timeline& timeline, const std::string& what) {
    // an automation lane's range first: copy and cut need one selected over it, paste a curve copied
    if (AutomationPtr lane = laneInUse(timeline, what == "paste"))
        if (what != "paste" || heldCurve) return useLane(timeline, lane, what);
    auto plural = [](size_t n) { return std::to_string(n) + (n != 1 ? " clips" : " clip"); };
    if (what == "paste") {
        std::string reason;
        auto placed = paste(timeline, reason);
        return reason.empty() ? "Pasted " + plural(placed.size()) : reason;
    }
    int count = what == "cut" ? cut(timeline) : copy(timeline);
    if (!count) return "Select clips (or a time range over them) first";
    return std::string(what == "cut" ? "Cut " : "Copied ") + plural(count);
}

}  // namespace clipboard
