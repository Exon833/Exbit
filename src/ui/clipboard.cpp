// Copy, cut and paste clips on the timeline (clipboard.py).
#include "ui/clipboard.h"

#include <algorithm>
#include <map>

#include "model/edits.h"
#include "ui/timeline.h"

namespace clipboard {

std::optional<Copied> held;

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
    for (auto& p : kept) c.clips.push_back({p, p->start - start, indexIn(shown, p->track) - top});
    c.span = end - start;
    c.sr = timeline.engine.sr;
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
        ClipPtr copy = copyClip(item.clip);
        copy->track = track;
        copy->start = timeline.cursor + item.offset;
        placed.push_back(copy);
    }
    for (auto& c : placed)  // (their new tracks' sound: rendered before the audio thread sees them)
        if (c->kind == Kind::Midi) c->prepare(timeline.engine.sr);
    edits::placeCopies(timeline.project, placed);
    timeline.selection.clear();
    timeline.selection.clips = std::set<ClipPtr>(placed.begin(), placed.end());
    timeline.setCursor(timeline.cursor + held->span);
    timeline.relayout();
    return placed;
}

int cut(Timeline& timeline) {
    int count = copy(timeline);
    if (count) timeline.deleteSelected();
    return count;
}

std::string use(Timeline& timeline, const std::string& what) {
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
