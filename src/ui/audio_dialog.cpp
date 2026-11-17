#include "ui/audio_dialog.h"

#include "audio/devices.h"
#include "audio/engine.h"
#include "audio/workers.h"
#include "core/util.h"
#include "ui/preferences.h"
#include "ui/toolkit/layout.h"

#include <thread>

using namespace ui;

namespace audio_dialog {

bool hasExclusive(const std::string& driverType) { return false; }

std::vector<std::pair<int, std::string>> coreChoices(int available) {
    available = std::max(1, available);
    std::vector<std::pair<int, std::string>> out;
    out.emplace_back(0, "Automatic (all " + std::to_string(available) + ")");
    out.emplace_back(1, "1 (the audio thread alone)");
    for (int n = 2; n < available; n *= 2) out.emplace_back(n, std::to_string(n));
    if (available > 1) out.emplace_back(available, std::to_string(available));
    return out;
}

std::string bufferText(int frames, int sr) {
    if (frames == 0) return "Driver default";
    return std::to_string(frames) + " frames  (" + util::fixed(frames / (double)sr * 1000.0, 1) + " ms)";
}

std::string nowPlaying(const audio::Engine& engine) {
    if (!engine.isOpen()) return "";
    std::string frames = engine.blocksize ? std::to_string(engine.blocksize) : std::string("driver's");
    std::string mode = engine.exclusive ? " exclusive" : "";
    std::string text = audio::driverLabel(engine.driverType) + mode + ": " + std::to_string(engine.sr) + " Hz, " +
                       std::to_string(engine.channels) + " outputs";
    if (engine.inputs) text += ", " + std::to_string(engine.inputs) + " inputs";
    text += ", " + frames + " frames per buffer, " + util::fixed(engine.outputLatency() * 1000.0, 1) +
            " ms output delay";
    return text;
}

}  // namespace audio_dialog

AudioPropertiesDialog::AudioPropertiesDialog(Window* parent, const std::string& driverType_, const std::string& device_,
                                             int bufferSize_, bool exclusive_, const audio::Engine& engine)
    : Dialog(parent, "Audio properties", 480, 300),
      driverType(driverType_),
      device(device_),
      bufferSize(bufferSize_),
      exclusive(exclusive_),
      cores(engine.cores) {
    FormLayout* form = ui::form(this, 11, 6);
    setMinimumSize(480, 0);

    types = add<ComboBox>();
    for (auto& t : audio::driverTypes()) types->addItem(t.label, t.key);
    form->addRow("Driver type", types);
    about = add<Label>();
    about->setWordWrap(true);
    about->setProperty("hint", "true");  // the skin's hint colour
    form->addRow(about);

    devices = add<ComboBox>();
    form->addRow("Output device", devices);
    exclusiveBox = add<CheckBox>("Exclusive mode (lower delay; other programs go silent)");
    exclusiveBox->setChecked(exclusive);
    form->addRow(exclusiveBox);

    buffers = add<ComboBox>();
    for (int frames : prefs::bufferSizes()) buffers->addItem(audio_dialog::bufferText(frames, engine.sr), frames);
    buffers->setCurrentIndexSilently(std::max(0, buffers->findData(bufferSize)));
    buffers->setToolTip(
        "Smaller: less delay, but more CPU and a higher risk of clicks.\n"
        "Bigger: fewer audio callbacks, less CPU, more delay.");
    form->addRow("Buffer size", buffers);
    Label* hint = add<Label>(
        "Smaller buffers: less delay, more CPU and risk of clicks.\nBigger buffers: less CPU, more "
        "delay. The sample rate is set in the device's own settings.");
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    form->addRow(hint);

    coreChoice = add<ComboBox>();
    for (auto& c : audio_dialog::coreChoices(workers::availableCores()))
        coreChoice->addItem(c.second, std::to_string(c.first));
    coreChoice->setCurrentIndexSilently(std::max(0, coreChoice->findData(std::to_string(cores))));
    coreChoice->setToolTip(
        "Tracks are worked out side by side on this many processor cores, so a song with many\n"
        "tracks of plugins doesn't have to fit on one. The sound is the same either way.\n"
        "1: everything on the audio thread, as before (try it if a plugin misbehaves).");
    form->addRow("Processing cores", coreChoice);

    if (engine.isOpen()) {
        Label* now = add<Label>(audio_dialog::nowPlaying(engine));
        now->setWordWrap(true);
        form->addRow("Now playing", now);
    } else {
        form->addRow("Now", add<Label>("No audio device open"));
    }

    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });
    form->addRow(bb);

    types->currentIndexChanged.connect([this](int) { showType(); });
    types->setCurrentIndexSilently(std::max(0, types->findData(driverType)));
    showType();
    adjustSize();
}

void AudioPropertiesDialog::showType() {
    std::string type = types->currentData();
    std::string text;
    for (auto& t : audio::driverTypes())
        if (t.key == type) text = t.description;
    about->setText(text);
    about->setVisible(!text.empty());
    devices->clear();
    for (auto& d : audio::outputDevices(type)) devices->addItem(d.name, d.id);
    int keep = devices->findData(device);
    devices->setCurrentIndexSilently(keep >= 0 ? keep : std::max(0, devices->findData(audio::defaultOutput(type))));
    bool excl = audio_dialog::hasExclusive(type);
    exclusiveBox->setEnabled(excl);
    exclusiveBox->setVisible(excl);  // (none of Linux's driver types has an exclusive mode)
}

void AudioPropertiesDialog::done(int result) {
    if (result == Accepted) {
        driverType = types->currentData();
        device = devices->currentData();
        bufferSize = buffers->currentDataInt(0);
        exclusive = exclusiveBox->isChecked() && audio_dialog::hasExclusive(driverType);
        cores = std::max(0, coreChoice->currentDataInt(0));
    }
    Dialog::done(result);
}
