#include "model/song_time.h"

#include <cmath>

#include "audio/automation.h"
#include "model/edits.h"

namespace song_time {

std::string checkFrozen(const Project& project, const std::vector<int64_t>& frames, int64_t spanStart,
                        int64_t spanEnd) {
    bool span = spanEnd > spanStart;
    for (auto& track : project.tracks) {
        const ClipPtr r = track->frozen;
        if (!r) continue;
        bool cut = false;
        for (int64_t f : frames) cut = cut || (r->start < f && f < r->end());
        if (span)
            cut = cut || (r->start < spanEnd && r->end() > spanStart) ||
                  !project.clipsIn({track}, spanStart, spanEnd).empty();
        if (cut) return track->name + " is frozen: unfreeze it first (right-click its row)";
    }
    return std::string();
}

bool wholeBars(const Timing& timing, double startS, double endS, int& first, int& count) {
    int a = timing.barAt(startS), b = timing.barAt(endS);
    // (within a few frames: through a ramp, bar lines fall between frames)
    bool onLines = std::fabs(timing.barSeconds(a) - startS) < 1e-4 && std::fabs(timing.barSeconds(b) - endS) < 1e-4;
    if (!onLines || !timing.beats) return false;
    first = a;
    count = b - a;
    return true;
}

void moveAutomation(Project& project, double atS, double shiftS, double cutTo) {
    bool inserting = cutTo < 0;
    double end = inserting ? atS : cutTo;
    for (auto& track : project.tracks) {
        for (auto& lane : track->automation) {
            const std::vector<AutoPoint>& points = *lane->points;
            if (points.empty()) continue;
            // (curved segments cut at the edit point and at its end keep their shape on either side: splitAt)
            std::vector<AutoPoint> shaped = points;
            automation::splitAt(shaped, atS);
            automation::splitAt(shaped, end);
            std::vector<AutoPoint> out;
            for (auto& p : shaped)
                if (p.t < atS) out.push_back(p);
            // (the curve as it was, up to the edit point: on a jump there, the value before it…)
            double atValue = automation::valueAt(points, atS);
            for (auto& p : points)
                if (p.t == atS) {
                    atValue = p.v;
                    break;
                }
            std::vector<AutoPoint> edge{{atS, atValue}};
            double later = automation::valueAt(points, end);  // (…and on from it)
            if (inserting && later != atValue) edge.push_back({atS + EDGE_S, later});  // (a jump there: it stays)
            edge.push_back({atS + (inserting ? shiftS : EDGE_S), later});
            for (auto& p : shaped)
                if (p.t == end) edge.back().bend = p.bend;  // (the curve on from there, as it was)
            double last = edge.back().t;
            out.insert(out.end(), edge.begin(), edge.end());
            for (auto& p : shaped)
                if (p.t >= end && p.t + shiftS > last) out.push_back({p.t + shiftS, p.v, p.bend});
            lane->setPoints(std::move(out));
        }
    }
}

void moveMarkers(Project& project, double atS, double shiftS, double cutTo) {
    double end = cutTo < 0 ? atS : cutTo;
    std::vector<Marker> kept;
    for (auto& m : project.markers)
        if (m.seconds < atS) kept.push_back(m);
    for (auto& m : project.markers)
        if (m.seconds >= end) kept.push_back({m.seconds + shiftS, m.name});
    project.setMarkers(std::move(kept));
}

void moveLoop(Timing& timing, double atS, double shiftS, double cutTo) {
    double s, e;
    if (!timing.loopRange(s, e)) return;
    double end = cutTo < 0 ? atS : cutTo;
    auto moved = [&](double t) { return t < atS ? t : t < end ? atS : t + shiftS; };
    timing.loopStart = moved(timing.loopStart);
    timing.loopEnd = moved(timing.loopEnd);
    if (!timing.loopRange(s, e)) {  // (all of it was deleted)
        timing.loopStart = timing.loopEnd = 0.0;
        timing.looping = false;
    }
}

namespace {
// the tempo arriving at `beat` from before it (a ramp running into it: where it has got), and whether it ramps
void arriving(const TempoMap& m, double beat, double& bpm, bool& ramps) {
    const std::vector<TempoPoint>& ps = m.points();
    size_t i = 0;
    while (i + 1 < ps.size() && ps[i + 1].beat < beat - 1e-9) i++;
    const TempoPoint& p = ps[i];
    ramps = p.ramp && p.beat < beat - 1e-9;
    bpm = ramps ? p.bpm + (ps[i + 1].bpm - p.bpm) * (beat - p.beat) / (ps[i + 1].beat - p.beat) : p.bpm;
}
// is there a tempo point at `beat`? Does the tempo ramp on from there (a point there ramping, or a ramp through it)?
bool pointAt(const TempoMap& m, double beat) {
    for (auto& p : m.points())
        if (std::fabs(p.beat - beat) < 1e-9) return true;
    return false;
}
bool rampsOn(const TempoMap& m, double beat) {
    const std::vector<TempoPoint>& ps = m.points();
    size_t i = 0;
    while (i + 1 < ps.size() && ps[i + 1].beat <= beat + 1e-9) i++;
    return ps[i].ramp;
}
int perBarAt(const TempoMap& m, double beat) {
    int bar, perBar;
    double at;
    m.meterAt(beat, bar, at, perBar);
    return perBar;
}
}  // namespace

std::string moveTempo(Timing& timing, double atS, double lengthS, bool insert) {
    if (timing.changes.empty() && !timing.ramp) return std::string();
    int first, count;
    if (!wholeBars(timing, atS, atS + lengthS, first, count))
        return "Tempo and time-signature changes stay at their bars (the time wasn't whole bars).";
    TempoMapPtr was = timing.map();
    double cut = was->barBeat(first);
    // (0-based bars; changes are kept by 1-based bar number: bar k is change k + 1)
    if (insert) {  // (the new bars go on at the tempo before them; a ramp cut there goes on after them)
        double before = 0;
        bool rampIn = false;
        if (first > 0) arriving(*was, cut, before, rampIn);
        bool through = !pointAt(*was, cut) && rampsOn(*was, cut);  // (a ramp running through the cut)
        for (auto& c : timing.changes)
            if (c.bar - 1 >= first) c.bar += count;
        if (rampIn)  // (the ramp ends at the cut, as it did; the new bars hold its tempo)
            timing.setChange(TempoChange{first + 1, before, perBarAt(*was, cut - 0.5), 0.0, false});
        if (through)  // (…and it goes on from where it was after them)
            timing.setChange(TempoChange{first + count + 1, was->bpmAtBeat(cut), perBarAt(*was, cut), 0.0, true});
        if (first == 0 && timing.ramp) {  // (bar 1's ramp starts after the new bars)
            timing.setChange(TempoChange{count + 1, timing.bpm, timing.beatsPerBar, 0.0, true});
            timing.ramp = false;
        }
        return std::string();
    }
    // (what plays right after the deleted bars, at its tempo: a ramp cut there goes on from its tempo there)
    double end = was->barBeat(first + count);
    double afterBpm = was->bpmAtBeat(end);
    int afterPerBar = perBarAt(*was, end);
    bool afterRamps = rampsOn(*was, end);
    std::vector<TempoChange> kept;
    for (auto c : timing.changes) {
        if (first <= c.bar - 1 && c.bar - 1 < first + count) continue;
        if (c.bar - 1 >= first + count) c.bar -= count;
        kept.push_back(c);
    }
    timing.changes.clear();
    for (auto& c : kept) {
        if (c.valid()) {
            timing.changes.push_back(c);
        } else {  // (moved to bar 1's start: it is bar 1's tempo now; Python keeps it as a change at bar 1, which a
                  // project file can't hold)
            timing.bpm = c.bpm;
            timing.beatsPerBar = c.beatsPerBar;
            timing.ramp = c.ramp;
        }
    }
    TempoMapPtr now = timing.map();
    if (first == 0) {
        timing.bpm = afterBpm;
        timing.beatsPerBar = afterPerBar;
        timing.ramp = afterRamps;
    } else if (!timing.changeAtBar(first + 1)) {
        double in = 0;
        bool rampIn = false;
        arriving(*now, cut, in, rampIn);
        if (rampIn || afterRamps || now->bpmAtBeat(cut) != afterBpm || perBarAt(*now, cut) != afterPerBar)
            timing.setChange(TempoChange{first + 1, afterBpm, afterPerBar, 0.0, afterRamps});
    }
    return std::string();
}

bool insertTime(Project& project, int64_t at, int64_t length, int sr, std::string& note, std::string& error) {
    note.clear();
    if (length <= 0) return true;
    error = checkFrozen(project, {at});
    if (!error.empty()) return false;
    Timing& timing = project.timing;
    double atS = (double)at / sr, lengthS = (double)length / sr;
    int first = 0, count = 0;
    bool bars = (!timing.changes.empty() || timing.ramp) && wholeBars(timing, atS, atS + lengthS, first, count);
    note = moveTempo(timing, atS, lengthS, true);  // (first: bars are counted at the old tempo map)
    if (bars) {  // (the new bars play at the tempo before them: as long as that makes them, so what comes after
                 // lands on its bars again)
        length = std::max<int64_t>(
            1, (int64_t)std::nearbyint((timing.barSeconds(first + count) - timing.barSeconds(first)) * sr));
        lengthS = (double)length / sr;
    }
    edits::splitClips(project, project.clipsIn(*project.tracks, at, at + 1), {at});
    for (auto& clip : project.clips)
        if (clip->start >= at) clip->start += length;
    for (auto& track : project.tracks) {
        if (track->frozen && track->frozen->start >= at) track->frozen->start += length;
        std::vector<ClipPtr> takes;
        for (auto& t : track->takes) {
            if (t->start >= at) {
                ClipPtr moved = copyClip(t);
                moved->start += length;
                takes.push_back(moved);
            } else {
                takes.push_back(t);
            }
        }
        if (!takes.empty()) track->takes = takes;
    }
    moveAutomation(project, atS, lengthS);
    moveMarkers(project, atS, lengthS);
    moveLoop(timing, atS, lengthS);
    return true;
}

bool deleteTime(Project& project, int64_t start, int64_t end, int sr, std::string& note, std::string& error) {
    note.clear();
    if (end <= start) return true;
    error = checkFrozen(project, {start, end}, start, end);
    if (!error.empty()) return false;
    Timing& timing = project.timing;
    double startS = (double)start / sr, endS = (double)end / sr;
    note = moveTempo(timing, startS, endS - startS, false);
    edits::deleteRange(project, *project.tracks, start, end);
    int64_t length = end - start;
    for (auto& clip : project.clips)
        if (clip->start >= end) clip->start -= length;
    for (auto& track : project.tracks) {
        if (track->frozen && track->frozen->start >= end) track->frozen->start -= length;
        std::vector<ClipPtr> takes;
        for (auto& t : track->takes) {
            if (t->start >= end) {
                ClipPtr moved = copyClip(t);
                moved->start -= length;
                takes.push_back(moved);
            } else {
                takes.push_back(t);
            }
        }
        if (!takes.empty()) track->takes = takes;
    }
    moveAutomation(project, startS, -(endS - startS), endS);
    moveMarkers(project, startS, -(endS - startS), endS);
    moveLoop(timing, startS, -(endS - startS), endS);
    return true;
}

}  // namespace song_time
