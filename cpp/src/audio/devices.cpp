// Sound devices: the driver types (JACK, ALSA, PulseAudio), their device lists, and opening streams
// (daw/audio/devices.py's stand-in for sounddevice / PortAudio host APIs). The drivers
// themselves are in devices_jack.cpp, devices_alsa.cpp and devices_pulse.cpp.
#include "audio/devices.h"

#include <pthread.h>
#include <sched.h>

#include <cstring>
#include <map>
#include <utility>

namespace audio {

// --- the drivers (devices_jack.cpp, devices_alsa.cpp, devices_pulse.cpp) ---
namespace detail {
bool jackAvailable();
std::vector<DeviceInfo> jackDevices(bool input);
bool jackDeviceInfo(const std::string& id, DeviceInfo& out, bool input);
std::string jackDefault(bool input);
std::unique_ptr<Stream> jackOpen(const StreamConfig& cfg, Callback cb, std::string& error);

bool alsaShared(const std::string& id);
std::vector<DeviceInfo> alsaDevices(bool input);
bool alsaDeviceInfo(const std::string& id, DeviceInfo& out, bool input);
std::unique_ptr<Stream> alsaOpen(const StreamConfig& cfg, Callback cb, std::string& error);

bool pulseAvailable();
std::vector<DeviceInfo> pulseDevices(bool input);
bool pulseDeviceInfo(const std::string& id, DeviceInfo& out, bool input);
std::string pulseDefault(bool input);
std::unique_ptr<Stream> pulseOpen(const StreamConfig& cfg, Callback cb, std::string& error);

// the calling thread as a real-time (SCHED_FIFO) one if the system lets us; quietly not otherwise
void raiseThreadPriority() {
    sched_param sp{};
    int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
    sp.sched_priority = std::max(lo, std::min(hi, 70));
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        sp.sched_priority = std::max(lo, std::min(hi, 10));  // RLIMIT_RTPRIO may allow less
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
}
}  // namespace detail

namespace {
// Driver types in the order they are offered, with a short name and what they are good for.
struct DriverText {
    const char* key;
    const char* label;
    const char* description;
};
const DriverText kDrivers[] = {
    {"JACK", "JACK",
     "Low delay, shared with other programs: JACK, or PipeWire's JACK (pipewire-jack). The buffer size and sample "
     "rate are the server's."},
    {"ALSA", "ALSA",
     "Linux's own sound system: any device, lowest delay with a card's hw device (which takes the card for this "
     "program alone)."},
    {"PulseAudio", "PulseAudio", "The sound server (PulseAudio or PipeWire): shared with other programs."},
};
// ALSA's "default" PCM, else the sound server's PCMs, else a card's default (with conversions)
std::string alsaDefault(const std::vector<DeviceInfo>& devs) {
    for (const char* want : {"default", "pipewire", "pulse"})
        for (auto& d : devs)
            if (d.id == want) return d.id;
    for (const char* prefix : {"sysdefault", "plughw:"})
        for (auto& d : devs)
            if (d.id.compare(0, strlen(prefix), prefix) == 0) return d.id;
    return devs.empty() ? "" : devs[0].id;
}
}  // namespace

std::vector<DriverInfo> driverTypes() {
    std::vector<DriverInfo> out;
    for (const DriverText& d : kDrivers) {
        std::string key = d.key;
        bool present = key == "JACK"   ? detail::jackAvailable() && !detail::jackDevices(false).empty()
                       : key == "ALSA" ? !detail::alsaDevices(false).empty()
                                       : detail::pulseAvailable() && !detail::pulseDevices(false).empty();
        if (present) out.push_back({d.key, d.label, d.description});
    }
    return out;
}

std::string driverLabel(const std::string& key) {
    for (const DriverText& d : kDrivers)
        if (key == d.key) return d.label;
    return key;
}

std::vector<DeviceInfo> outputDevices(const std::string& driverType) {
    if (driverType == "JACK") return detail::jackDevices(false);
    if (driverType == "ALSA") return detail::alsaDevices(false);
    if (driverType == "PulseAudio") return detail::pulseDevices(false);
    return {};
}

std::vector<DeviceInfo> inputDevices(const std::string& driverType) {
    if (driverType == "JACK") return detail::jackDevices(true);
    if (driverType == "ALSA") return detail::alsaDevices(true);
    if (driverType == "PulseAudio") return detail::pulseDevices(true);
    return {};
}

std::string defaultOutput(const std::string& driverType) {
    if (driverType == "JACK") return detail::jackDefault(false);
    if (driverType == "ALSA") return alsaDefault(detail::alsaDevices(false));
    if (driverType == "PulseAudio") return detail::pulseDefault(false);
    return "";
}

std::string defaultInput(const std::string& driverType) {
    if (driverType == "JACK") return detail::jackDefault(true);
    if (driverType == "ALSA") return alsaDefault(detail::alsaDevices(true));
    if (driverType == "PulseAudio") return detail::pulseDefault(true);
    return "";
}

bool deviceInfo(const std::string& driverType, const std::string& id, DeviceInfo& out, bool input) {
    if (driverType == "JACK") return detail::jackDeviceInfo(id, out, input);
    if (driverType == "ALSA") return detail::alsaDeviceInfo(id, out, input);
    if (driverType == "PulseAudio") return detail::pulseDeviceInfo(id, out, input);
    return false;
}

double minLatency(const std::string& driverType) {
    (void)driverType;
    return 0;  // MIN_LATENCY only names Windows' DirectSound and MME
}

double minLatency(const std::string& driverType, const std::string& device) {
    if (driverType == "ALSA" && detail::alsaShared(device)) return SHARED_ALSA_MIN_LATENCY;
    return minLatency(driverType);
}

std::unique_ptr<Stream> Stream::open(const StreamConfig& cfg, Callback cb, std::string& error) {
    error.clear();
    if (!cb) {
        error = "No callback given";
        return nullptr;
    }
    if (cfg.outputDevice.empty() && cfg.inputDevice.empty()) {
        error = "No device chosen";
        return nullptr;
    }
    if (cfg.driverType == "JACK") return detail::jackOpen(cfg, std::move(cb), error);
    if (cfg.driverType == "ALSA") return detail::alsaOpen(cfg, std::move(cb), error);
    if (cfg.driverType == "PulseAudio") return detail::pulseOpen(cfg, std::move(cb), error);
    error = "Unknown driver type \"" + cfg.driverType + "\"";
    return nullptr;
}

}  // namespace audio
