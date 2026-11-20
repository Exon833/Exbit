// Capture: what was just played, as a clip (capture.py).
#include "ui/capture.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace capture {

std::vector<PlayedNote> notesOf(const std::vector<midi_input::LogEntry>& log, double now) {
    struct Held { double start; std::optional<int64_t> frame; int velocity; };
    std::map<int, Held> held;
    std::vector<PlayedNote> notes;
    for (const midi_input::LogEntry& e : log) {
        auto it = held.find(e.pitch);
        if (it != held.end()) {
            notes.push_back({it->second.start, std::max(e.clock, it->second.start + MIN_NOTE_S), e.pitch,
                             it->second.velocity, it->second.frame});
            held.erase(it);
        }
        if (e.on) held[e.pitch] = {e.clock, e.heard, e.velocity};
    }
    for (auto& kv : held)
        notes.push_back({kv.second.start, std::max(now, kv.second.start + MIN_NOTE_S), kv.first, kv.second.velocity,
                         kv.second.frame});
    std::sort(notes.begin(), notes.end(), [](const PlayedNote& a, const PlayedNote& b) {
        // (Python sorts the tuples: on, off, pitch, velocity, frame)
        return std::tie(a.on, a.off, a.pitch, a.velocity) < std::tie(b.on, b.off, b.pitch, b.velocity);
    });
    return notes;
}

std::vector<PlayedNote> phrase(const std::vector<midi_input::LogEntry>& log, double now, double gapS) {
    std::vector<PlayedNote> notes = notesOf(log, now);
    size_t first = 0;
    double reach = -1e300;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].on - reach > gapS) first = i;  // a pause before this note: a new phrase starts here
        reach = std::max(reach, notes[i].off);
    }
    return std::vector<PlayedNote>(notes.begin() + (long)first, notes.end());
}

Placement placement(const std::vector<PlayedNote>& notes, int sr, const Timing& timing, double cursorS) {
    Placement out;
    if (notes.empty()) return out;
    std::vector<double> offsets;
    for (const PlayedNote& n : notes)
        if (n.heard) offsets.push_back((double)*n.heard / sr - n.on);
    bool along = offsets.size() == notes.size() &&
                 *std::max_element(offsets.begin(), offsets.end()) - *std::min_element(offsets.begin(), offsets.end()) <=
                     STEADY_S;
    std::vector<std::pair<double, double>> at;
    if (along) {  // played along with playback: where it was heard
        std::vector<double> sorted = offsets;
        std::sort(sorted.begin(), sorted.end());
        size_t m = sorted.size();
        double shift = m % 2 ? sorted[m / 2] : (sorted[m / 2 - 1] + sorted[m / 2]) / 2;  // (np.median)
        out.start = timing.barSeconds(timing.barAt(std::max(0.0, notes[0].on + shift)));
        for (const PlayedNote& n : notes) at.push_back({n.on + shift - out.start, n.off + shift - out.start});
    } else {  // at the selection cursor, from the first note
        out.start = cursorS;
        double first = notes[0].on;
        for (const PlayedNote& n : notes) at.push_back({n.on - first, n.off - first});
    }
    double last = 0;
    for (size_t i = 0; i < notes.size(); ++i) {
        double on = std::max(0.0, at[i].first);  // (nothing before the clip starts)
        double off = std::max(at[i].second, on + MIN_NOTE_S);
        Note n;
        n.start = on;
        n.end = off;
        n.pitch = notes[i].pitch;
        n.velocity = notes[i].velocity;
        out.notes.push_back(n);
        last = std::max(last, off);
    }
    double end = out.start + last;
    double length;
    if (along) {  // to the end of the bar the last note ends in
        int bar = timing.barAt(end - 1e-6);
        length = timing.barSeconds(bar + 1) - out.start;
    } else {  // whole bars from the cursor
        double b0 = timing.beatAt(out.start), perBar = timing.beatsPerBarAt(out.start);  // (in beats: ramps too)
        double bars = std::max(1.0, std::ceil((timing.beatAt(end) - b0) / perBar - 1e-6));
        length = timing.secondsAt(b0 + bars * perBar) - out.start;
    }
    out.lengthS = std::max(length, last);
    return out;
}

ClipPtr capture(const Host& host) {
    midi_input::MidiInput& input = *host.input;
    auto status = [&](const std::string& m) { if (host.status) host.status(m, 5000); };
    TrackPtr track = input.target();
    if (!track) {
        status("Capture: select the MIDI track to put what you played on (click its lane or row)");
        return nullptr;
    }
    double now = input.now();
    std::vector<midi_input::LogEntry> log;
    for (const midi_input::LogEntry& e : input.log)
        if (e.clock > input.capturedAt) log.push_back(e);
    std::vector<PlayedNote> notes = phrase(log, now);
    if (notes.empty()) {
        status("Capture: nothing new was played on the MIDI keyboard or the computer keyboard (Ctrl+K)");
        return nullptr;
    }
    int sr = input.engine.sr;
    int64_t cursor = host.cursor ? host.cursor() : 0;
    Placement place = placement(notes, sr, input.project.timing, (double)cursor / sr);
    input.capturedAt = now;
    int take = host.nextTake ? host.nextTake() : 1;
    ClipPtr clip = makeMidiClip("Capture " + std::to_string(take), place.notes, place.lengthS, sr);
    clip->length = (int64_t)std::nearbyint(place.lengthS * sr);
    clip->prepare(sr);
    if (host.placeClip) host.placeClip(clip, (int64_t)std::nearbyint(place.start * sr), track);
    size_t n = place.notes.size();
    status("Captured " + std::to_string(n) + " note" + (n != 1 ? "s" : "") + " onto " + track->name);
    return clip;
}

}  // namespace capture
