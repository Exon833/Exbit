// Sound devices: listing them by driver type and opening a stream (the stand-in for PortAudio /
// sounddevice). Linux driver types: "JACK" (offered first when a JACK server runs, PipeWire's JACK
// too: libjack.so.0 loaded at run time, devices_jack.cpp; the sample rate and buffer size are the
// server's), "ALSA" (every PCM ALSA knows: default, pipewire, pulse, the cards' hw/plughw devices…)
// and "PulseAudio" (the sound server's sinks, through libpulse-simple).
//
// A Stream calls its callback on its own high-priority thread with interleaved float32 buffers:
// `out` has outChannels per frame (to be filled; it arrives zeroed), `in` inChannels per frame
// (nullptr when the stream has no inputs).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace audio {

struct DriverInfo {
    std::string key;          // "JACK", "ALSA", "PulseAudio" (saved in the settings)
    std::string label;        // short name shown
    std::string description;  // what it is good for (Audio properties dialog)
};

struct DeviceInfo {
    std::string id;           // what open() takes (ALSA: the PCM name, e.g. "default", "hw:CARD=PCH,DEV=0";
                              // JACK: the client owning the physical ports, e.g. "system")
    std::string name;         // shown
    int maxOutputs = 2;
    int maxInputs = 0;
    int defaultSampleRate = 48000;
};

std::vector<DriverInfo> driverTypes();                         // those with at least one output device
std::string driverLabel(const std::string& key);
std::vector<DeviceInfo> outputDevices(const std::string& driverType);
std::vector<DeviceInfo> inputDevices(const std::string& driverType);
std::string defaultOutput(const std::string& driverType);      // device id, or "" if none
std::string defaultInput(const std::string& driverType);       // device id, or "" if none
bool deviceInfo(const std::string& driverType, const std::string& id, DeviceInfo& out, bool input = false);
// least total output delay (s) a driver type keeps up with (MIN_LATENCY); 0 = none
double minLatency(const std::string& driverType);
// (addition) the same for one device: ALSA's shared devices (default, pipewire, pulse, sysdefault…:
// a sound server mixes them) are opened with at least SHARED_ALSA_MIN_LATENCY whatever blocksize is
// asked for; a hw:/plughw: device, and JACK, keep the delay asked for
constexpr double SHARED_ALSA_MIN_LATENCY = 0.08;
double minLatency(const std::string& driverType, const std::string& device);
// (addition) a latency a driver reports beyond this is a wrong one (PipeWire's JACK: 2^32 frames)
constexpr double MAX_PLAUSIBLE_LATENCY_S = 10.0;
inline double plausibleLatency(double reported, double fallback) {
    return reported >= 0.0 && reported <= MAX_PLAUSIBLE_LATENCY_S ? reported : fallback;
}

using Callback = std::function<void(const float* in, float* out, int frames)>;

struct StreamConfig {
    std::string driverType;
    std::string outputDevice;      // "" = no outputs
    std::string inputDevice;       // "" = no inputs (a duplex stream when both are set and the driver allows it)
    int sampleRate = 0;            // 0 = the device's own
    int outChannels = 0;           // 0 = every output the device has
    int inChannels = 0;
    int blocksize = 0;             // frames per buffer; 0 = the driver's own setting
    bool exclusive = false;        // take the device for this program alone, where the driver can
};

class Stream {
public:
    // opens (not started); nullptr and `error` set if it can't
    static std::unique_ptr<Stream> open(const StreamConfig& cfg, Callback cb, std::string& error);
    virtual ~Stream() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;     // waits for the callback to finish
    virtual int sampleRate() const = 0;
    virtual int outChannels() const = 0;
    virtual int inChannels() const = 0;
    virtual int blocksize() const = 0;        // frames per callback (as it turned out)
    virtual double outputLatency() const = 0; // seconds
    virtual double inputLatency() const = 0;  // seconds
    virtual std::string deviceName() const = 0;
    // buffers the device ran dry since the stream started (xruns / underflows)
    std::atomic<int> underflows{0};
    // (addition) the device went away (unplugged, the sound server quit): the callback no longer runs
    std::atomic<bool> lost{false};
};

}  // namespace audio
