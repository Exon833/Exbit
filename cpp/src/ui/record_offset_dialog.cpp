// Options → Recording offset… (record_offset.py).
#include "ui/record_offset_dialog.h"

#include <algorithm>
#include <cmath>

#include "audio/record_offset.h"
#include "core/json.h"
#include "core/util.h"
#include "ui/recording.h"
#include "ui/toolkit/layout.h"

using namespace ui;

namespace record_offset_dialog {

std::string deviceKey(const std::string& driverType, const std::string& device) {
    return record_offset::deviceKey(driverType, device);
}

std::map<std::string, double> offsets() {
    std::map<std::string, double> out;
    std::string text = settings::get(KEY).toString();
    if (text.empty()) return out;
    std::string error;
    Json j = Json::parse(text, &error);
    if (!j.isObject()) return out;
    for (auto& [key, value] : j.members())
        if (value.isNumber()) out[key] = value.toDouble();
    return out;
}

double offsetMs(const std::string& driverType, const std::string& device) {
    return record_offset::offsetMs(settings::get(KEY).toString(), deviceKey(driverType, device));
}

void setOffsetMs(const std::string& driverType, const std::string& device, double ms) {
    settings::set(KEY, record_offset::setOffsetMs(settings::get(KEY).toString(), deviceKey(driverType, device), ms));
}

std::optional<Measured> measure(const Clip& take, const Timing& timing, int sr) {
    return record_offset::measure(take, timing, sr);
}

}  // namespace record_offset_dialog

using namespace record_offset_dialog;

RecordOffsetDialog::RecordOffsetDialog(Window* parent, const Preferences& prefs, ClipPtr lastTake, double lastOffsetMs,
                                       const Timing& timing, int sr)
    : Dialog(parent, "Recording offset", 460, 260),
      driverType_(prefs.driverType),
      device_(prefs.deviceName),
      take_(std::move(lastTake)),
      takeOffsetMs_(lastOffsetMs),
      timing_(timing),
      sr_(sr) {
    FormLayout* f = form(this, 11, 6);
    setMinimumSize(460, 0);
    Label* about = add<Label>("For " + (device_.empty() ? std::string("the default device") : device_) + " (" +
                              driverType_ + "). If your takes land a little late (or early) against the beat, this "
                              "moves every new audio take earlier (or, negative, later) by that much.");
    about->setWordWrap(true);
    f->addRow(about);
    offset = add<SpinBox>();
    offset->setRange(-MAX_MS, MAX_MS);
    offset->setDecimals(1);
    offset->setSuffix(" ms");
    offset->setValueSilently(offsetMs(driverType_, device_));
    f->addRow("Recording offset", offset);
    Label* how = add<Label>("To measure it: connect an output to an input with a cable (or hold a microphone to a "
                            "speaker), switch the metronome on, record a few seconds on an audio track, then press "
                            "Measure.");
    how->setWordWrap(true);
    how->setProperty("hint", "true");
    f->addRow(how);
    measureButton = add<Button>("Measure from the last take");
    measureButton->clicked.connect([this] { measureLastTake(); });
    f->addRow(measureButton);
    result = add<Label>("");
    result->setWordWrap(true);
    f->addRow(result);
    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });
    f->addRow(bb);
    adjustSize();
}

RecordOffsetDialog::RecordOffsetDialog(Window* parent, const Preferences& prefs, const Recording& recording,
                                       const Project& project, int sr)
    : RecordOffsetDialog(parent, prefs, recording.lastAudio, recording.lastOffsetMs, project.timing, sr) {}

std::optional<double> RecordOffsetDialog::measureLastTake() {
    if (!take_) {
        result->setText("There is no audio take yet in this session: record the metronome first (see above).");
        adjustSize();
        return std::nullopt;
    }
    auto found = measurer ? measurer(*take_, timing_, sr_) : measure(*take_, timing_, sr_);
    if (!found) {
        result->setText("No clicks in time with the beat were found in the last take: was the metronome on, and "
                        "loud enough at the input?");
        adjustSize();
        return std::nullopt;
    }
    // (the take was placed with the offset then in use: corrected from that one)
    offset->setValue(takeOffsetMs_ + found->late * 1000);
    std::string steady = found->spread < 0.003
                             ? ""
                             : " The clicks varied by " + util::fixed(found->spread * 1000, 1) +
                                   " ms: the device's delay isn't steady (a bigger buffer, or JACK or a hw: device, "
                                   "may help).";
    result->setText("From " + std::to_string(found->count) + " clicks: the take was " +
                    util::fixed(std::fabs(found->late) * 1000, 1) + " ms " + (found->late > 0 ? "late" : "early") +
                    ". The offset above is corrected; OK keeps it." + steady);
    adjustSize();  // (room for the result's lines)
    return found->late;
}

void RecordOffsetDialog::done(int r) {
    if (r == Accepted) setOffsetMs(driverType_, device_, offset->value());
    Dialog::done(r);
}
