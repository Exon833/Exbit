// The timeline's linked MIDI clips (model.h, "linked MIDI clips"; timeline.h): duplicating as linked,
// making clips unique, the notes of one passed on to its linked clips, and the chain mark's tooltip.
#include <algorithm>
#include <set>

#include "model/auto_follow.h"
#include "model/edits.h"
#include "ui/clip_painter.h"
#include "ui/timeline.h"

void Timeline::duplicateLinkedSelected() {
    // as Ctrl+D (duplicateClips): the copies right after the selected clips, the MIDI ones linked to them
    std::vector<ClipPtr> chosen;
    for (auto& c : project.clips)  // (in the project's order: the selection is a set)
        if (selection.contains(c) && c->kind == Kind::Midi && !c->track->frozen) chosen.push_back(c);
    if (chosen.empty()) return;
    int64_t first = chosen[0]->start, last = chosen[0]->end();
    for (auto& c : chosen) first = std::min(first, c->start), last = std::max(last, c->end());
    std::vector<ClipPtr> copies;
    for (auto& c : chosen) {
        ClipPtr k = links::linkedCopy(c);
        k->start += last - first;
        copies.push_back(k);
    }
    edits::placeCopies(project, copies);
    std::vector<auto_follow::Move> follow;  // (their automation copied along)
    for (size_t i = 0; i < copies.size(); i++)
        follow.push_back(auto_follow::moveOf(*copies[i], chosen[i]->track, chosen[i]->start, engine.sr));
    if (auto_follow::carry(project, follow, true)) lanesChanged();
    selection.clear();
    std::map<ClipPtr, TrackPtr> tracks;
    std::map<ClipPtr, std::optional<int64_t>> starts;
    for (auto& c : copies)
        if (project.clips.contains(c)) {
            selection.clips.insert(c);
            tracks[c] = nullptr;  // (copies: prepared for their place, as MIDI effects following the beat hear it)
            starts[c] = std::nullopt;
        }
    rerenderMoved(tracks, starts);
    relayout();
}

void Timeline::makeUniqueSelected() {
    std::vector<ClipPtr> chosen;
    for (auto& c : project.clips)
        if (selection.contains(c) && c->link) chosen.push_back(c);
    if (chosen.empty()) return;
    links::makeUnique(project, chosen);
    update();
}

void Timeline::linkedNotesChanged(const ClipPtr& clip) {
    if (clip && clip->link && !links::notesChanged(project, clip, engine.sr).empty()) update();
}

void Timeline::splitPiecesUnique(const std::vector<ClipPtr>& before) {
    std::set<Clip*> had;
    for (auto& c : before) had.insert(c.get());
    std::vector<ClipPtr> pieces;
    for (auto& c : project.clips)
        if (c->link && !had.count(c.get())) pieces.push_back(c);
    if (!pieces.empty()) links::makeUnique(project, pieces);
}

ClipPtr Timeline::linkBadgeAt(PointF pos) const {
    auto tops = trackTops();
    std::map<const ClipLink*, int> counts = links::counts(project);
    if (counts.empty()) return nullptr;
    const std::vector<ClipPtr>& clips = *project.clips;
    for (auto it = clips.rbegin(); it != clips.rend(); ++it) {  // (later clips are on top)
        const ClipPtr& c = *it;
        if (!c->link || counts[c->link.get()] < 2) continue;
        auto rect = clipRect(*c, tops);
        RectF badge;
        if (!rect || !clip_painter::linkBadge(*rect, badge)) continue;
        if (pos.x >= badge.x - 2 && pos.x <= badge.right() + 2 && pos.y >= rect->y && pos.y <= rect->y + clip_painter::headerHeight())
            return c;
    }
    return nullptr;
}

std::string Timeline::toolTipAt(Point pos) const {
    if (ClipPtr c = linkBadgeAt(PointF(pos))) {
        int n = links::count(project, *c);
        return "Linked: " + std::to_string(n) + " clips share these notes (editing one changes them all).\n"
               "Right-click \xe2\x86\x92 Make unique to edit this one on its own.";
    }
    return ui::Widget::toolTipAt(pos);
}
