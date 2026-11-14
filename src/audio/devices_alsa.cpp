// ALSA: the PCM list (snd_device_name_hint) and streams (snd_pcm_writei of interleaved samples on
// a real-time thread; duplex with a second capture PCM read in the same thread).
#include <alsa/asoundlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "audio/devices.h"

namespace audio {
namespace detail {
void raiseThreadPriority();
bool alsaDeviceInfo(const std::string& id, DeviceInfo& out, bool input);

// a PCM that isn't the hardware itself (default, pipewire, pulse, sysdefault, dmix…): a sound server
// or ALSA's mixer works in chunks of its own and runs dry with less than SHARED_ALSA_MIN_LATENCY
// (PipeWire: 1024 frames; see devices.py's measurements)
bool alsaShared(const std::string& id) {
    return id.compare(0, 3, "hw:") != 0 && id.compare(0, 7, "plughw:") != 0;
}

namespace {

// ALSA prints its errors to stderr (e.g. "unable to open slave" for every busy dmix): quiet
// while we try devices
void quietAlsa(const char*, int, const char*, int, const char*, ...) {}
struct QuietAlsa {
    QuietAlsa() { snd_lib_error_set_handler(quietAlsa); }
    ~QuietAlsa() { snd_lib_error_set_handler(nullptr); }
};

bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

bool wanted(const std::string& name) {
    return name == "default" || name == "pipewire" || name == "pulse" || name == "sysdefault" ||
           startsWith(name, "sysdefault:") || startsWith(name, "hw:") || startsWith(name, "plughw:");
}

struct Probe {
    bool ok = false;
    int maxChannels = 0;
    int defaultRate = 48000;
};

// what a PCM offers in one direction: opened non-blocking for a moment (never held open)
Probe probePcm(const std::string& name, snd_pcm_stream_t dir, int* err = nullptr) {
    Probe p;
    snd_pcm_t* pcm = nullptr;
    int r = snd_pcm_open(&pcm, name.c_str(), dir, SND_PCM_NONBLOCK);
    if (err) *err = r;
    if (r < 0) return p;
    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    if (snd_pcm_hw_params_any(pcm, hp) >= 0) {
        unsigned cmax = 0;
        snd_pcm_hw_params_get_channels_max(hp, &cmax);
        // plug-type devices say 10000 channels: they take any count; offer stereo
        p.maxChannels = cmax > 64 ? 2 : (int)cmax;
        p.defaultRate = 0;
        for (unsigned rate : {48000u, 44100u, 96000u, 88200u})
            if (snd_pcm_hw_params_test_rate(pcm, hp, rate, 0) == 0) {
                p.defaultRate = (int)rate;
                break;
            }
        if (!p.defaultRate) {
            unsigned rmin = 0;
            snd_pcm_hw_params_get_rate_min(hp, &rmin, nullptr);
            p.defaultRate = rmin ? (int)rmin : 48000;
        }
        p.ok = p.maxChannels > 0;
    }
    snd_pcm_close(pcm);
    return p;
}

struct Hint {
    std::string name, desc;
    bool out = true, in = true;
};

std::mutex g_mutex;
std::map<std::pair<std::string, int>, Probe> g_probes;  // (PCM, direction) → what it offers (busy ones not kept)

Probe cachedProbe(const std::string& name, snd_pcm_stream_t dir, bool* busy = nullptr) {
    auto key = std::make_pair(name, (int)dir);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_probes.find(key);
        if (it != g_probes.end()) return it->second;
    }
    int err = 0;
    Probe p = probePcm(name, dir, &err);
    bool isBusy = err == -EBUSY || err == -EAGAIN;
    if (busy) *busy = isBusy;
    if (isBusy) {
        p.ok = true;  // in use by another program now: list it all the same (stereo, 48 kHz)
        p.maxChannels = 2;
        p.defaultRate = 48000;
        return p;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_probes[key] = p;
    return p;
}

std::vector<Hint> hints() {
    std::vector<Hint> out;
    void** list = nullptr;
    if (snd_device_name_hint(-1, "pcm", &list) < 0 || !list) return out;
    for (void** h = list; *h; h++) {
        char* n = snd_device_name_get_hint(*h, "NAME");
        char* d = snd_device_name_get_hint(*h, "DESC");
        char* io = snd_device_name_get_hint(*h, "IOID");
        if (n && wanted(n)) {
            Hint hint;
            hint.name = n;
            hint.desc = d ? d : "";
            if (io) {
                hint.out = strcmp(io, "Output") == 0;
                hint.in = strcmp(io, "Input") == 0;
            }
            bool dup = false;
            for (auto& e : out) dup |= e.name == hint.name;
            if (!dup) out.push_back(hint);
        }
        free(n);
        free(d);
        free(io);
    }
    snd_device_name_free_hint(list);
    // "default" may be missing from the hints (no plugin defines it); ALSA always has a default PCM
    bool hasDefault = false;
    for (auto& e : out) hasDefault |= e.name == "default";
    if (!hasDefault) out.insert(out.begin(), Hint{"default", "Default ALSA device", true, true});
    // default, pipewire, pulse first; then the cards' devices in ALSA's order
    std::stable_sort(out.begin(), out.end(), [](const Hint& a, const Hint& b) {
        auto rank = [](const std::string& n) { return n == "default" ? 0 : n == "pipewire" ? 1 : n == "pulse" ? 2 : 3; };
        return rank(a.name) < rank(b.name);
    });
    return out;
}

std::string shownName(const Hint& h) {
    std::string first = h.desc.substr(0, h.desc.find('\n'));
    if (first.empty()) first = h.name;
    if (h.name == "default" || h.name == "pipewire" || h.name == "pulse") return first;
    std::string kind = h.name.substr(0, h.name.find(':'));
    return first + " [" + kind + "]";
}

// --- sample formats ---

enum class Fmt { F32, S32, S24, S24_3, S16 };

snd_pcm_format_t alsaFormat(Fmt f) {
    switch (f) {
        case Fmt::F32: return SND_PCM_FORMAT_FLOAT_LE;
        case Fmt::S32: return SND_PCM_FORMAT_S32_LE;
        case Fmt::S24: return SND_PCM_FORMAT_S24_LE;
        case Fmt::S24_3: return SND_PCM_FORMAT_S24_3LE;
        default: return SND_PCM_FORMAT_S16_LE;
    }
}

int bytesOf(Fmt f) {
    switch (f) {
        case Fmt::F32: case Fmt::S32: case Fmt::S24: return 4;
        case Fmt::S24_3: return 3;
        default: return 2;
    }
}

inline float clampf(float x) { return x > 1.f ? 1.f : x < -1.f ? -1.f : x; }

void toDevice(const float* src, uint8_t* dst, size_t n, Fmt f) {
    switch (f) {
        case Fmt::F32: memcpy(dst, src, n * 4); break;
        case Fmt::S32: {
            int32_t* d = (int32_t*)dst;
            for (size_t i = 0; i < n; i++) d[i] = (int32_t)std::lrint((double)clampf(src[i]) * 2147483647.0);
            break;
        }
        case Fmt::S24: {
            int32_t* d = (int32_t*)dst;
            for (size_t i = 0; i < n; i++) d[i] = (int32_t)std::lrint(clampf(src[i]) * 8388607.0f);
            break;
        }
        case Fmt::S24_3:
            for (size_t i = 0; i < n; i++) {
                int32_t v = (int32_t)std::lrint(clampf(src[i]) * 8388607.0f);
                dst[i * 3] = (uint8_t)(v & 255);
                dst[i * 3 + 1] = (uint8_t)((v >> 8) & 255);
                dst[i * 3 + 2] = (uint8_t)((v >> 16) & 255);
            }
            break;
        case Fmt::S16: {
            int16_t* d = (int16_t*)dst;
            for (size_t i = 0; i < n; i++) d[i] = (int16_t)std::lrint(clampf(src[i]) * 32767.0f);
            break;
        }
    }
}

void fromDevice(const uint8_t* src, float* dst, size_t n, Fmt f) {
    switch (f) {
        case Fmt::F32: memcpy(dst, src, n * 4); break;
        case Fmt::S32: {
            const int32_t* s = (const int32_t*)src;
            for (size_t i = 0; i < n; i++) dst[i] = (float)(s[i] / 2147483648.0);
            break;
        }
        case Fmt::S24: {
            const int32_t* s = (const int32_t*)src;
            for (size_t i = 0; i < n; i++) {
                int32_t v = (int32_t)((uint32_t)s[i] << 8) >> 8;  // sign-extend the low 24 bits
                dst[i] = v / 8388608.0f;
            }
            break;
        }
        case Fmt::S24_3:
            for (size_t i = 0; i < n; i++) {
                int32_t v = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                if (v & 0x800000) v -= 0x1000000;
                dst[i] = v / 8388608.0f;
            }
            break;
        case Fmt::S16: {
            const int16_t* s = (const int16_t*)src;
            for (size_t i = 0; i < n; i++) dst[i] = s[i] / 32768.0f;
            break;
        }
    }
}

std::string alsaError(const std::string& what, const std::string& dev, int err) {
    std::string msg = what + " \"" + dev + "\": " + snd_strerror(err);
    if (err == -EBUSY) msg += " (another program is using it)";
    return msg;
}

// sets up one PCM: interleaved, the best sample format it takes, channels (0: all), rate (0: its
// own), `period` frames per period, `periods` of them (more if that holds less than minBufferS
// seconds). Reports what it turned out to be.
bool setupPcm(snd_pcm_t* pcm, const std::string& dev, int& channels, int& rate, int& period, int periods, Fmt& fmt,
              int& bufferFrames, std::string& error, double minBufferS = 0) {
    snd_pcm_hw_params_t* hp;
    snd_pcm_hw_params_alloca(&hp);
    int r = snd_pcm_hw_params_any(pcm, hp);
    if (r < 0) {
        error = alsaError("Can't configure", dev, r);
        return false;
    }
    if ((r = snd_pcm_hw_params_set_access(pcm, hp, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        error = alsaError("Interleaved access not supported by", dev, r);
        return false;
    }
    bool found = false;
    for (Fmt f : {Fmt::F32, Fmt::S32, Fmt::S24, Fmt::S24_3, Fmt::S16}) {
        if (snd_pcm_hw_params_test_format(pcm, hp, alsaFormat(f)) == 0) {
            snd_pcm_hw_params_set_format(pcm, hp, alsaFormat(f));
            fmt = f;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "\"" + dev + "\" takes no sample format Tiny DAW can write (float, 32, 24 or 16 bit)";
        return false;
    }
    unsigned ch = (unsigned)channels;
    if (ch == 0) {
        unsigned cmax = 2;
        snd_pcm_hw_params_get_channels_max(hp, &cmax);
        ch = cmax > 64 ? 2 : cmax;
    }
    if ((r = snd_pcm_hw_params_set_channels_near(pcm, hp, &ch)) < 0) {
        error = alsaError("Can't set the channels of", dev, r);
        return false;
    }
    unsigned rt = (unsigned)rate;
    if (rt == 0) {
        rt = 48000;
        for (unsigned c : {48000u, 44100u, 96000u})
            if (snd_pcm_hw_params_test_rate(pcm, hp, c, 0) == 0) {
                rt = c;
                break;
            }
    }
    if ((r = snd_pcm_hw_params_set_rate_near(pcm, hp, &rt, nullptr)) < 0) {
        error = alsaError("Can't set the sample rate of", dev, r);
        return false;
    }
    snd_pcm_uframes_t per = (snd_pcm_uframes_t)period;
    int dirArg = 0;
    snd_pcm_hw_params_set_period_size_near(pcm, hp, &per, &dirArg);
    if (minBufferS > 0 && per > 0) periods = std::max(periods, (int)std::ceil(minBufferS * rt / (double)per));
    snd_pcm_uframes_t buf = per * (snd_pcm_uframes_t)periods;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hp, &buf);
    if ((r = snd_pcm_hw_params(pcm, hp)) < 0) {
        error = alsaError("Can't set up", dev, r);
        return false;
    }
    snd_pcm_hw_params_get_period_size(hp, &per, &dirArg);
    snd_pcm_hw_params_get_buffer_size(hp, &buf);
    channels = (int)ch;
    rate = (int)rt;
    period = (int)per;
    bufferFrames = (int)buf;
    return true;
}

class AlsaStream : public Stream {
public:
    ~AlsaStream() override {
        stop();
        if (pcm_) snd_pcm_close(pcm_);
        if (cap_) snd_pcm_close(cap_);
    }
    bool open(const StreamConfig& cfg, Callback cb, std::string& error);
    bool start() override;
    void stop() override;
    int sampleRate() const override { return rate_; }
    int outChannels() const override { return outCh_; }
    int inChannels() const override { return inCh_; }
    int blocksize() const override { return period_; }
    double outputLatency() const override { return pcm_ ? (double)buffer_ / rate_ : 0; }
    double inputLatency() const override { return cap_ ? (double)period_ * 2 / rate_ : 0; }
    std::string deviceName() const override { return name_; }

private:
    snd_pcm_t* pcm_ = nullptr;
    snd_pcm_t* cap_ = nullptr;
    Fmt fmt_ = Fmt::F32, capFmt_ = Fmt::F32;
    int rate_ = 48000, outCh_ = 0, inCh_ = 0, period_ = 256, buffer_ = 512, capBuffer_ = 0;
    std::string name_;
    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<float> out_, in_;
    std::vector<uint8_t> raw_, capRaw_;

    void loop();
    bool recover(snd_pcm_t* p, int err, bool playback);
    void prefill();
    bool readInput();
    void callback();
};

bool AlsaStream::open(const StreamConfig& cfg, Callback cb, std::string& error) {
    QuietAlsa quiet;
    cb_ = std::move(cb);
    int period = cfg.blocksize > 0 ? cfg.blocksize : 256;
    int rate = cfg.sampleRate;
    if (!cfg.outputDevice.empty()) {
        int r = snd_pcm_open(&pcm_, cfg.outputDevice.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (r < 0) {
            pcm_ = nullptr;
            error = alsaError("Can't open the output", cfg.outputDevice, r);
            return false;
        }
        int ch = cfg.outChannels;
        // three periods: with two, HD-Audio cards (whose position lags the period interrupt) run dry.
        // A shared device (a sound server behind it) holds at least SHARED_ALSA_MIN_LATENCY.
        double least = alsaShared(cfg.outputDevice) ? SHARED_ALSA_MIN_LATENCY : 0;
        if (!setupPcm(pcm_, cfg.outputDevice, ch, rate, period, 3, fmt_, buffer_, error, least)) return false;
        outCh_ = ch;
        snd_pcm_sw_params_t* sp;
        snd_pcm_sw_params_alloca(&sp);
        snd_pcm_sw_params_current(pcm_, sp);
        snd_pcm_sw_params_set_start_threshold(pcm_, sp, (snd_pcm_uframes_t)buffer_);
        snd_pcm_sw_params_set_avail_min(pcm_, sp, (snd_pcm_uframes_t)period);
        snd_pcm_sw_params(pcm_, sp);
        name_ = cfg.outputDevice;
    }
    if (!cfg.inputDevice.empty()) {
        int r = snd_pcm_open(&cap_, cfg.inputDevice.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (r < 0) {
            cap_ = nullptr;
            error = alsaError("Can't open the input", cfg.inputDevice, r);
            return false;
        }
        int ch = cfg.inChannels;
        int crate = rate, cper = period, cbuf = 0;
        if (!setupPcm(cap_, cfg.inputDevice, ch, crate, cper, 4, capFmt_, cbuf, error)) return false;
        if (pcm_ && crate != rate) {
            error = "The input \"" + cfg.inputDevice + "\" can't run at " + std::to_string(rate) + " Hz like the output";
            return false;
        }
        if (!pcm_) {
            rate = crate;
            period = cper;
        }
        inCh_ = ch;
        capBuffer_ = cbuf;
        if (name_.empty()) name_ = cfg.inputDevice;
        if (pcm_) snd_pcm_link(pcm_, cap_);  // the same card: they start together (fails quietly otherwise)
    }
    rate_ = rate;
    period_ = period;
    out_.assign((size_t)period_ * std::max(1, outCh_), 0.f);
    in_.assign((size_t)period_ * std::max(1, inCh_), 0.f);
    raw_.assign((size_t)period_ * std::max(1, outCh_) * bytesOf(fmt_), 0);
    capRaw_.assign((size_t)period_ * std::max(1, inCh_) * bytesOf(capFmt_), 0);
    DeviceInfo info;
    if (alsaDeviceInfo(name_, info, !pcm_)) name_ = info.name;
    return true;
}

bool AlsaStream::start() {
    if (running_) return true;
    if (pcm_) {
        snd_pcm_drop(pcm_);
        snd_pcm_prepare(pcm_);
    }
    if (cap_) {
        snd_pcm_drop(cap_);
        snd_pcm_prepare(cap_);
    }
    underflows = 0;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void AlsaStream::stop() {
    if (!running_ && !thread_.joinable()) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (pcm_) snd_pcm_drop(pcm_);
    if (cap_) snd_pcm_drop(cap_);
}

// the output buffer full of silence: the stream starts (start threshold = the buffer)
void AlsaStream::prefill() {
    std::vector<uint8_t> zeros((size_t)buffer_ * outCh_ * bytesOf(fmt_), 0);
    snd_pcm_sframes_t left = buffer_ - period_;  // the first callback's block makes it full
    const uint8_t* p = zeros.data();
    while (left > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm_, p, (snd_pcm_uframes_t)left);
        if (w < 0) break;
        left -= w;
        p += (size_t)w * outCh_ * bytesOf(fmt_);
    }
}

bool AlsaStream::recover(snd_pcm_t* p, int err, bool playback) {
    if (err == -EPIPE) {
        if (playback) underflows++;
        if (snd_pcm_prepare(p) < 0) return false;
        if (playback) prefill();
        else snd_pcm_start(p);
        return true;
    }
    if (err == -ESTRPIPE) {  // suspended (the computer slept)
        int r;
        for (int i = 0; i < 100 && (r = snd_pcm_resume(p)) == -EAGAIN; i++) usleep(10000);
        if (snd_pcm_prepare(p) < 0) return false;
        if (playback) prefill();
        else snd_pcm_start(p);
        return true;
    }
    if (err == -EAGAIN || err == -EINTR) return true;
    return false;  // -ENODEV (unplugged), -EBADFD…
}

// a block from the input when one is there, else silence (never waits for it)
bool AlsaStream::readInput() {
    snd_pcm_sframes_t avail = snd_pcm_avail_update(cap_);
    if (avail < 0) {
        std::fill(in_.begin(), in_.end(), 0.f);
        return recover(cap_, (int)avail, false);
    }
    if (avail > 3 * period_) {
        // fell behind (e.g. the capture started early): keep the delay low
        snd_pcm_forward(cap_, (snd_pcm_uframes_t)(avail - period_));
        avail = period_;
    }
    if (avail < period_) {
        std::fill(in_.begin(), in_.end(), 0.f);
        return true;
    }
    snd_pcm_sframes_t r = snd_pcm_readi(cap_, capRaw_.data(), (snd_pcm_uframes_t)period_);
    if (r < 0) {
        std::fill(in_.begin(), in_.end(), 0.f);
        return recover(cap_, (int)r, false);
    }
    fromDevice(capRaw_.data(), in_.data(), (size_t)r * inCh_, capFmt_);
    if (r < period_) std::fill(in_.begin() + (size_t)r * inCh_, in_.end(), 0.f);
    return true;
}

void AlsaStream::callback() {
    std::fill(out_.begin(), out_.end(), 0.f);
    try {
        cb_(cap_ ? in_.data() : nullptr, out_.data(), period_);
    } catch (...) {
        std::fill(out_.begin(), out_.end(), 0.f);
    }
}

void AlsaStream::loop() {
    raiseThreadPriority();
    if (cap_ && !pcm_) {
        // input only: paced by the capture
        snd_pcm_start(cap_);
        while (running_) {
            int w = snd_pcm_wait(cap_, 100);
            if (w < 0 && !recover(cap_, w, false)) {
                lost = true;
                break;
            }
            snd_pcm_sframes_t avail = snd_pcm_avail_update(cap_);
            if (avail < 0) {
                if (!recover(cap_, (int)avail, false)) {
                    lost = true;
                    break;
                }
                continue;
            }
            if (avail < period_) continue;
            if (!readInput()) {
                lost = true;
                break;
            }
            callback();
        }
        return;
    }
    prefill();
    if (cap_) snd_pcm_start(cap_);
    bool first = true;
    while (running_) {
        snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
        if (avail < 0) {
            if (!recover(pcm_, (int)avail, true)) {
                lost = true;
                break;
            }
            continue;
        }
        if (avail < period_ && !first) {
            int w = snd_pcm_wait(pcm_, 100);
            if (w < 0 && !recover(pcm_, w, true)) {
                lost = true;
                break;
            }
            continue;
        }
        first = false;
        if (cap_ && !readInput()) {
            lost = true;
            break;
        }
        callback();
        toDevice(out_.data(), raw_.data(), (size_t)period_ * outCh_, fmt_);
        const uint8_t* p = raw_.data();
        snd_pcm_sframes_t left = period_;
        while (left > 0 && running_) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm_, p, (snd_pcm_uframes_t)left);
            if (w == -EAGAIN) {
                snd_pcm_wait(pcm_, 100);
                continue;
            }
            if (w < 0) {
                if (!recover(pcm_, (int)w, true)) {
                    lost = true;
                    running_ = false;
                }
                break;  // after an underrun the block is dropped: the buffer was refilled with silence
            }
            left -= w;
            p += (size_t)w * outCh_ * bytesOf(fmt_);
        }
    }
}

}  // namespace

std::vector<DeviceInfo> alsaDevices(bool input) {
    QuietAlsa quiet;
    std::vector<DeviceInfo> out;
    for (const Hint& h : hints()) {
        if (input ? !h.in : !h.out) continue;
        Probe p = cachedProbe(h.name, input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK);
        if (!p.ok) continue;
        DeviceInfo d;
        d.id = h.name;
        d.name = shownName(h);
        (input ? d.maxInputs : d.maxOutputs) = p.maxChannels;
        if (input) d.maxOutputs = 0;
        d.defaultSampleRate = p.defaultRate;
        out.push_back(d);
    }
    return out;
}

bool alsaDeviceInfo(const std::string& id, DeviceInfo& out, bool input) {
    QuietAlsa quiet;
    for (const Hint& h : hints()) {
        if (h.name != id) continue;
        Probe p = cachedProbe(h.name, input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK);
        out = DeviceInfo();
        out.id = id;
        out.name = shownName(h);
        out.maxOutputs = input ? 0 : (p.ok ? p.maxChannels : 0);
        out.maxInputs = input ? (p.ok ? p.maxChannels : 0) : 0;
        out.defaultSampleRate = p.defaultRate;
        return p.ok;
    }
    // a PCM the hints don't list (e.g. typed in the settings): probe it as it is
    Probe p = cachedProbe(id, input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK);
    if (!p.ok) return false;
    out = DeviceInfo();
    out.id = out.name = id;
    out.maxOutputs = input ? 0 : p.maxChannels;
    out.maxInputs = input ? p.maxChannels : 0;
    out.defaultSampleRate = p.defaultRate;
    return true;
}

std::unique_ptr<Stream> alsaOpen(const StreamConfig& cfg, Callback cb, std::string& error) {
    std::unique_ptr<AlsaStream> s(new AlsaStream());
    if (!s->open(cfg, std::move(cb), error)) return nullptr;
    return s;
}

}  // namespace detail
}  // namespace audio
