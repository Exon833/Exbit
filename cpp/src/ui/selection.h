// What is selected on the timeline (selection.py): a set of clips and, optionally, a time range
// over some lanes. Dragging over empty lane space makes a range; every clip it touches is selected
// too. Clicking clips selects them (Ctrl+click adds or removes one).
#pragma once
#include <optional>
#include <set>
#include <vector>

#include "model/model.h"

struct TimeRange {
    int64_t start = 0;  // frames
    int64_t end = 0;
    std::vector<TrackPtr> tracks;  // lanes the range covers, top to bottom
};

class Selection {
public:
    std::set<ClipPtr> clips;
    std::optional<TimeRange> range;

    void clear() { clips.clear(); range.reset(); }
    void selectOnly(const ClipPtr& c) { clips = {c}; range.reset(); }
    void toggle(const ClipPtr& c) {
        if (!clips.erase(c)) clips.insert(c);
        range.reset();
    }
    void setRange(const TimeRange& r, const std::vector<ClipPtr>& cs) {
        range = r;
        clips = std::set<ClipPtr>(cs.begin(), cs.end());
    }
    bool contains(const ClipPtr& c) const { return clips.count(c) > 0; }
    // drop clips (and a track) that no longer exist
    void forget(const std::vector<ClipPtr>& gone, const TrackPtr& track = nullptr) {
        for (auto& c : gone) clips.erase(c);
        if (track && range) {
            std::vector<TrackPtr> kept;
            for (auto& t : range->tracks)
                if (t != track) kept.push_back(t);
            if (kept.empty()) range.reset();
            else range->tracks = kept;
        }
    }
    std::vector<ClipPtr> clipList() const { return std::vector<ClipPtr>(clips.begin(), clips.end()); }
};
