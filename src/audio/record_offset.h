// The recording offset: making recordings line up on any audio device (ui/record_offset.py, its
// arithmetic; the dialog is ui/record_offset_dialog).
//
// A take is moved earlier by the delay the device reports (its input and output latency: engine.h's
// Recorder). Some drivers report it wrong (PipeWire's JACK, some USB interfaces), so takes land a few
// milliseconds late or early and flam against the beat. The offset corrects what is left: a positive
// offset moves every new audio take that much earlier still (Engine::recordOffsetMs). It is remembered
// for each driver type and device: a JSON object {"driver|device": ms} in the preferences
// (record_offsets).
//
// Measure from the last take: connect an output to an input with a cable (or hold a microphone to a
// speaker), switch the metronome on, and record a few seconds on an audio track; measure() finds each
// click in that take (where the sound first rises above a third of its loudest after QUIET_S under it)
// and compares it with the beat it was played on (the tempo map's beats). The median difference is how
// late the takes are; the spread (90th - 10th percentile) says how steady the device is.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"

namespace record_offset {

constexpr int MIN_CLICKS = 3;         // clicks a measurement needs
constexpr double MAX_ERROR_S = 0.2;   // a click further than this from a beat isn't one of the metronome's
constexpr double QUIET_S = 0.05;      // a click's onset: the sound rises after at least this long under the threshold

// the preferences' key of a device: "driver|device"
std::string deviceKey(const std::string& driverType, const std::string& deviceName);
// the correction (ms; positive: takes move earlier) for `key` in the preferences' JSON text (0 if none)
double offsetMs(const std::string& offsetsJson, const std::string& key);
// the JSON text with `key` set to ms (rounded to 0.01 ms)
std::string setOffsetMs(const std::string& offsetsJson, const std::string& key, double ms);

// frames where clicks start in `audio` (interleaved, `channels` per frame)
std::vector<int64_t> onsets(const float* audio, int64_t frames, int channels, int sr);

struct Measurement {
    double late = 0;    // s the take's clicks are after the beats (negative: before)
    double spread = 0;  // s
    int count = 0;      // clicks that lined up with beats
};
// For an audio take of the metronome (as placed: its start, offset and frames on the timeline); nothing
// when fewer than MIN_CLICKS clicks line up with beats.
std::optional<Measurement> measure(const Clip& take, const Timing& timing, int sr);
std::optional<Measurement> measure(const float* audio, int64_t frames, int channels, int64_t startFrame,
                                   const TempoMap& map, int sr);

}  // namespace record_offset
