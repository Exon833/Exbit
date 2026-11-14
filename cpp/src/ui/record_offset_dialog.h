// Options → Recording offset…: making recordings line up on any audio device (record_offset.py).
//
// A take is moved earlier by the delay the device reports (its input and output latency:
// recording.h). Some drivers report it wrong (PipeWire's JACK on Linux, some USB interfaces), so
// takes land a few milliseconds late or early and flam against the beat. The offset corrects what is
// left: a positive offset moves every audio take that much earlier still. It is remembered for each
// driver type and device (the settings' "options/record_offsets", the successor's Preferences key:
// JSON {"driver|device": ms}, read and written here directly).
//
// Measure from the last take: connect an output to an input with a cable (or hold a microphone to a
// speaker), switch the metronome on, and record a few seconds on an audio track; Measure finds each
// click in that take and compares it with the beat it was played on (the engine's measurement:
// audio/record_offset.h). The median difference is how late the takes are; the spread says how
// steady the device is.
//
// THE MAIN WINDOW: Options → Recording offset… opens RecordOffsetDialog(this, prefs, recording,
// project, engine.sr) with exec(); OK keeps the offset (Recording gives it to the engine,
// Engine::recordOffsetMs, whenever an audio take starts).
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "audio/record_offset.h"
#include "model/model.h"
#include "ui/preferences.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class Recording;

namespace record_offset_dialog {
constexpr const char* KEY = "options/record_offsets";
constexpr double MAX_MS = 200.0;  // the offset's range: -200 … 200 ms
// "driver|device"
std::string deviceKey(const std::string& driverType, const std::string& device);
// every device's offset (ms), from the settings ({} when unset or broken)
std::map<std::string, double> offsets();
// the correction for the device in use (ms; positive: takes move earlier)
double offsetMs(const std::string& driverType, const std::string& device);
inline double offsetMs(const Preferences& prefs) { return offsetMs(prefs.driverType, prefs.deviceName); }
// remember it (rounded to 0.01 ms)
void setOffsetMs(const std::string& driverType, const std::string& device, double ms);
inline void setOffsetMs(const Preferences& prefs, double ms) { setOffsetMs(prefs.driverType, prefs.deviceName, ms); }

// what a measurement found: how late the take's clicks are against the beats (s), their spread (s), how many
using Measured = record_offset::Measurement;
// the metronome take's clicks against the beats (audio/record_offset.h); none when too few line up
std::optional<Measured> measure(const Clip& take, const Timing& timing, int sr);
}  // namespace record_offset_dialog

// The offset for the device in use, and Measure from the last take.
class RecordOffsetDialog : public ui::Dialog {
public:
    // lastTake: the last audio take of this session as it was placed (Recording::lastAudio), and the
    // offset it was placed with
    RecordOffsetDialog(ui::Window* parent, const Preferences& prefs, ClipPtr lastTake, double lastOffsetMs,
                       const Timing& timing, int sr);
    RecordOffsetDialog(ui::Window* parent, const Preferences& prefs, const Recording& recording, const Project& project,
                       int sr);
    ui::SpinBox* offset;
    ui::Button* measureButton;
    ui::Label* result;
    // Measure: sets the offset from the last take; returns how late it was (s), or none (and says why)
    std::optional<double> measureLastTake();
    void done(int result) override;  // OK keeps the offset
    // (tests) the measurement used instead of record_offset_dialog::measure
    std::function<std::optional<record_offset_dialog::Measured>(const Clip&, const Timing&, int)> measurer;
private:
    std::string driverType_, device_;
    ClipPtr take_;
    double takeOffsetMs_;
    Timing timing_;
    int sr_;
};
