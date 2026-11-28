// Options → Audio properties…: which driver type and output device to play through, how big the
// buffers are, and what the open device is doing right now (audio_dialog.py).
#pragma once
#include <string>
#include <vector>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace audio { class Engine; }

namespace audio_dialog {
// the driver types that have an exclusive mode (taking the device for this program alone). None of
// Linux's: an ALSA hw device is always the program's alone, PulseAudio is always shared.
bool hasExclusive(const std::string& driverType);
// the choices of Processing cores: (cores, text) — 0 "Automatic (all 16)", 1 "1 (the audio thread alone)", 2, 4…
std::vector<std::pair<int, std::string>> coreChoices(int available);
// "Driver default" / "512 frames  (10.7 ms)"
std::string bufferText(int frames, int sr);
// what the open device is doing: "ALSA: 48000 Hz, 2 outputs, 2 inputs, 512 frames per buffer, 21.3 ms
// output delay"; "" when no device is open
std::string nowPlaying(const audio::Engine& engine);
}  // namespace audio_dialog

class AudioPropertiesDialog : public ui::Dialog {
public:
    // After exec() returned Accepted: driverType, device (its id), bufferSize (frames, 0 = driver
    // default) and exclusive hold the choice.
    AudioPropertiesDialog(ui::Window* parent, const std::string& driverType, const std::string& device, int bufferSize,
                          bool exclusive, const audio::Engine& engine);
    std::string driverType, device;
    int bufferSize;
    bool exclusive;
    int cores;  // Engine::cores
    bool lowLatency = false;  // Preferences::lowLatencyMonitoring (set lowLatencyBox before exec())

    ui::ComboBox* types;
    ui::Label* about;
    ui::ComboBox* devices;
    ui::CheckBox* exclusiveBox;
    ui::ComboBox* buffers;
    ui::ComboBox* coreChoice;
    ui::CheckBox* lowLatencyBox;  // Low-latency monitoring (ui/recording.h)
    ui::Label* realtimePriority;  // what the audio threads got (audio/realtime_priority.h: statusText)
    void showType();               // list the chosen driver type's devices (keeping the current one)
    void done(int result) override;
};
