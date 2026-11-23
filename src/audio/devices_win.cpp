// Windows' sound devices: WASAPI (the "WASAPI" driver type; on Windows this file stands in for devices.cpp and the
// Linux drivers). Render and capture endpoints by their endpoint ids (stable across sessions) and "default" (the
// system's default device, followed when it changes). Streams are event-driven:
//   - shared mode, lowest delay first: IAudioClient3::InitializeSharedAudioStream at the period nearest the buffer
//     size asked for, down to the smallest the driver allows (buffer size 0: the driver's default period), when the
//     engine's own format is asked for;
//   - else shared mode through IAudioClient::Initialize with AUTOCONVERTPCM (any rate / channel count, Windows
//     converts);
//   - exclusive mode (StreamConfig::exclusive) at the period nearest the asked (down to the device's smallest), in the
//     first sample format the device takes (float32, int32, int24 in 32, int24, int16); if it can't be had, shared.
// Buffer sizes beyond the shared engine's largest period are had the plain way (blocks of that size).
// A stream gives either outputs or inputs (the engine opens a separate input stream, as with PulseAudio).
// The device lost (unplugged, disabled, the default changed under a "default" stream): `lost` is set, the callback
// goes on at the stream's pace with silence (the transport keeps moving), and the device is opened again every
// REOPEN_S, or at once on the new default; then `lost` is cleared. The audio thread runs under MMCSS "Pro Audio".
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "audio/devices.h"
#include "core/util.h"

namespace audio {
namespace detail {

// the calling thread as an MMCSS "Pro Audio" one; time-critical priority where MMCSS isn't running
void raiseThreadPriority() {
    DWORD task = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (h) {
        AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        util::debugf("audio: MMCSS not available (%lu): time-critical priority instead\n", GetLastError());
    }
}

}  // namespace detail

namespace {

const char* kDriver = "WASAPI";
const char* kDefaultId = "default";
constexpr double REOPEN_S = 0.5;     // a lost device is looked for this often
constexpr double CACHE_S = 1.0;      // the device lists are asked for again after this long
constexpr HRESULT kPeriodLocked = (HRESULT)0x88890028;  // AUDCLNT_E_ENGINE_PERIODICITY_LOCKED (newer SDKs only)

// COM's multithreaded apartment kept for the whole program: any thread may use the audio objects, wherever made
void comReady() {
    static bool once = [] {
        CO_MTA_USAGE_COOKIE cookie;
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));
    }();
    (void)once;
}

// (defined here: no libuuid / libksuser needed)
const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

template <class T>
struct Ptr {  // (a COM pointer, released when it goes)
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
    T** operator&() {
        reset();
        return &p;
    }
    explicit operator bool() const { return p != nullptr; }
};

struct Com {  // COM on this thread (any apartment it already has will do)
    HRESULT hr;
    Com() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Com() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::string utf8(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string hr(HRESULT h) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lx", (unsigned long)h);
    return b;
}

bool enumerator(Ptr<IMMDeviceEnumerator>& e) {
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      (void**)&e));
}

bool getDevice(IMMDeviceEnumerator* e, const std::string& id, bool input, Ptr<IMMDevice>& d) {
    if (id == kDefaultId) return SUCCEEDED(e->GetDefaultAudioEndpoint(input ? eCapture : eRender, eConsole, &d));
    return SUCCEEDED(e->GetDevice(wide(id).c_str(), &d));
}

// --- the device lists ---

struct Endpoint {
    std::string id, name;
    int channels = 2, rate = 48000;
};
struct Lists {
    std::vector<Endpoint> devs[2];  // [0] outputs, [1] inputs
    std::string def[2];             // the default's endpoint id
};

std::mutex g_listLock;
Lists g_lists;
std::chrono::steady_clock::time_point g_listedAt;
bool g_listed = false;

Lists scan() {
    comReady();
    Lists l;
    Ptr<IMMDeviceEnumerator> e;
    if (!enumerator(e)) {
        util::debugf("audio: WASAPI: no device enumerator (the Windows Audio service isn't running?)\n");
        return l;
    }
    for (int input = 0; input < 2; input++) {
        EDataFlow flow = input ? eCapture : eRender;
        Ptr<IMMDevice> def;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id))) l.def[input] = utf8(id), CoTaskMemFree(id);
        }
        Ptr<IMMDeviceCollection> all;
        UINT n = 0;
        if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all)) || FAILED(all->GetCount(&n))) continue;
        for (UINT i = 0; i < n; i++) {
            Ptr<IMMDevice> d;
            if (FAILED(all->Item(i, &d))) continue;
            Endpoint ep;
            LPWSTR id = nullptr;
            if (FAILED(d->GetId(&id))) continue;
            ep.id = utf8(id);
            CoTaskMemFree(id);
            Ptr<IPropertyStore> props;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) ep.name = utf8(v.pwszVal);
                PropVariantClear(&v);
            }
            if (ep.name.empty()) ep.name = ep.id;
            Ptr<IAudioClient> client;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) &&
                SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                ep.channels = mix->nChannels;
                ep.rate = (int)mix->nSamplesPerSec;
                CoTaskMemFree(mix);
            }
            l.devs[input].push_back(ep);
        }
    }
    return l;
}

Lists lists() {
    std::lock_guard<std::mutex> g(g_listLock);
    auto now = std::chrono::steady_clock::now();
    if (!g_listed || now - g_listedAt > std::chrono::duration<double>(CACHE_S)) {
        g_lists = scan();
        g_listedAt = now;
        if (!g_listed)
            util::debugf("audio: WASAPI: %d outputs, %d inputs\n", (int)g_lists.devs[0].size(), (int)g_lists.devs[1].size());
        g_listed = true;
    }
    return g_lists;
}

const Endpoint* find(const Lists& l, const std::string& id, bool input) {
    const std::string& want = id == kDefaultId ? l.def[input] : id;
    for (const Endpoint& e : l.devs[input])
        if (e.id == want) return &e;
    return nullptr;
}

DeviceInfo toInfo(const Endpoint& e, bool input, const std::string& id, const std::string& shown) {
    DeviceInfo i;
    i.id = id;
    i.name = shown;
    i.maxOutputs = input ? 0 : e.channels;
    i.maxInputs = input ? e.channels : 0;
    i.defaultSampleRate = e.rate > 0 ? e.rate : 48000;
    return i;
}

std::vector<DeviceInfo> devices(bool input) {
    Lists l = lists();
    std::vector<DeviceInfo> out;
    if (l.devs[input].empty()) return out;
    const Endpoint* def = find(l, kDefaultId, input);
    const Endpoint& d0 = def ? *def : l.devs[input][0];
    out.push_back(toInfo(d0, input, kDefaultId, "Default (" + d0.name + ")"));
    for (const Endpoint& e : l.devs[input]) out.push_back(toInfo(e, input, e.id, e.name));
    return out;
}

// --- sample formats ---

enum class Sample { F32, I32, I24, I16 };  // (I32 also carries 24 valid bits in 32)

bool isFloat32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat == kSubFloat;
}

DWORD channelMask(int channels) {
    switch (channels) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

WAVEFORMATEXTENSIBLE makeFormat(Sample s, int rate, int channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE f{};
    int container = s == Sample::I16 ? 16 : s == Sample::I24 ? 24 : 32;
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = (DWORD)rate;
    f.Format.wBitsPerSample = (WORD)container;
    f.Format.nBlockAlign = (WORD)(channels * container / 8);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = (WORD)(s == Sample::I24 ? 24 : container);
    f.dwChannelMask = mask ? mask : channelMask(channels);
    f.SubFormat = s == Sample::F32 ? kSubFloat : kSubPcm;
    return f;
}

void toDevice(Sample s, const float* src, BYTE* dst, size_t n) {
    switch (s) {
        case Sample::F32: memcpy(dst, src, n * 4); return;
        case Sample::I16:
            for (size_t i = 0; i < n; i++)
                ((int16_t*)dst)[i] = (int16_t)std::lrint(std::clamp(src[i], -1.f, 1.f) * 32767.f);
            return;
        case Sample::I32:
            for (size_t i = 0; i < n; i++)
                ((int32_t*)dst)[i] = (int32_t)std::llrint(std::clamp((double)src[i], -1.0, 1.0) * 2147483647.0);
            return;
        case Sample::I24:
            for (size_t i = 0; i < n; i++) {
                int32_t v = (int32_t)std::lrint(std::clamp(src[i], -1.f, 1.f) * 8388607.f);
                dst[3 * i] = (BYTE)v, dst[3 * i + 1] = (BYTE)(v >> 8), dst[3 * i + 2] = (BYTE)(v >> 16);
            }
            return;
    }
}

void fromDevice(Sample s, const BYTE* src, float* dst, size_t n) {
    switch (s) {
        case Sample::F32: memcpy(dst, src, n * 4); return;
        case Sample::I16:
            for (size_t i = 0; i < n; i++) dst[i] = ((const int16_t*)src)[i] / 32768.f;
            return;
        case Sample::I32:
            for (size_t i = 0; i < n; i++) dst[i] = (float)(((const int32_t*)src)[i] / 2147483648.0);
            return;
        case Sample::I24:
            for (size_t i = 0; i < n; i++) {
                int32_t v = (int32_t)((uint32_t)src[3 * i] << 8 | (uint32_t)src[3 * i + 1] << 16 | (uint32_t)src[3 * i + 2] << 24);
                dst[i] = (float)(v / 2147483648.0);
            }
            return;
    }
}

// --- the default device followed ---

class DefaultWatch : public IMMNotificationClient {
public:
    std::atomic<bool> changed{false};
    EDataFlow flow;
    explicit DefaultWatch(EDataFlow f) : flow(f) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }  // (owned by its stream, unregistered before it goes)
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow f, ERole role, LPCWSTR) override {
        if (f == flow && role == eConsole) changed = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// --- streams ---

class WasapiStream : public Stream {
public:
    ~WasapiStream() override {
        stop();
        if (watch_) {
            if (notify_) notify_->UnregisterEndpointNotificationCallback(watch_.get());
            notify_.reset();
        }
        release();
        if (event_) CloseHandle(event_);
        if (quit_) CloseHandle(quit_);
    }
    bool open(const StreamConfig& cfg, Callback cb, std::string& error);
    bool start() override;
    void stop() override;
    int sampleRate() const override { return rate_; }
    int outChannels() const override { return input_ ? 0 : ch_; }
    int inChannels() const override { return input_ ? ch_ : 0; }
    int blocksize() const override { return block_; }
    double outputLatency() const override { return input_ ? 0 : latency_.load(); }
    double inputLatency() const override { return input_ ? latency_.load() : 0; }
    std::string deviceName() const override {
        std::lock_guard<std::mutex> g(nameLock_);
        return name_;
    }
    bool exclusiveMode() const { return exclusive_; }

private:
    StreamConfig cfg_;
    Callback cb_;
    bool input_ = false;
    std::string id_;
    int rate_ = 48000, ch_ = 2, block_ = 480;
    Sample sample_ = Sample::F32;
    bool exclusive_ = false;
    UINT32 bufferFrames_ = 0;
    std::atomic<double> latency_{0};
    mutable std::mutex nameLock_;
    std::string name_;
    HANDLE event_ = nullptr, quit_ = nullptr;
    Ptr<IAudioClient> client_;
    Ptr<IAudioRenderClient> render_;
    Ptr<IAudioCaptureClient> capture_;
    Ptr<IMMDeviceEnumerator> notify_;
    std::unique_ptr<DefaultWatch> watch_;
    std::thread thread_;
    std::vector<float> out_, in_;  // one block; the capture side's frames not given yet
    size_t inFrames_ = 0;
    bool fresh_ = true;  // (capture: no packet since it started)

    bool init(std::string& error, bool first);
    bool initExclusive(IMMDevice* dev, bool first, std::string& error);
    bool initShared(IMMDevice* dev, bool first, std::string& error);
    void release() {
        render_.reset();
        capture_.reset();
        client_.reset();
    }
    void loop();
    bool serve();        // one event's worth; false: the device is gone
    void silentBlock();  // (the device lost) one block of silence through the callback
};

bool WasapiStream::open(const StreamConfig& cfg, Callback cb, std::string& error) {
    cfg_ = cfg;
    cb_ = std::move(cb);
    if (!cfg.outputDevice.empty() && !cfg.inputDevice.empty()) {
        error = "WASAPI opens outputs and inputs as separate streams";
        return false;
    }
    input_ = cfg.outputDevice.empty();
    id_ = input_ ? cfg.inputDevice : cfg.outputDevice;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_ || !quit_) {
        error = "Can't make the stream's events";
        return false;
    }
    if (!init(error, true)) return false;
    if (id_ == kDefaultId && enumerator(notify_)) {  // (a "default" stream follows the default device)
        watch_ = std::make_unique<DefaultWatch>(input_ ? eCapture : eRender);
        if (FAILED(notify_->RegisterEndpointNotificationCallback(watch_.get()))) watch_.reset(), notify_.reset();
    }
    return true;
}

bool WasapiStream::init(std::string& error, bool first) {
    release();
    Ptr<IMMDeviceEnumerator> e;
    Ptr<IMMDevice> dev;
    if (!enumerator(e)) {
        error = "The Windows Audio service isn't running";
        return false;
    }
    if (!getDevice(e.p, id_, input_, dev)) {
        error = id_ == kDefaultId ? std::string(input_ ? "No input device" : "No output device")
                                  : "No such audio device: " + id_;
        return false;
    }
    Ptr<IPropertyStore> props;
    std::string name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = utf8(v.pwszVal);
        PropVariantClear(&v);
    }
    if (first) {
        Ptr<IAudioClient> probe;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&probe)) ||
            FAILED(probe->GetMixFormat(&mix)) || !mix) {
            error = "The audio device can't be used (it may be in use by another program)";
            return false;
        }
        rate_ = cfg_.sampleRate > 0 ? cfg_.sampleRate : (int)mix->nSamplesPerSec;
        int want = input_ ? cfg_.inChannels : cfg_.outChannels;
        ch_ = want > 0 ? want : mix->nChannels;
        ch_ = std::max(1, std::min(ch_, 32));
        CoTaskMemFree(mix);
    }
    bool ok = false;
    if (cfg_.exclusive) {
        ok = initExclusive(dev.p, first, error);
        if (!ok) util::debugf("audio: WASAPI exclusive mode can't be had (%s): shared mode\n", error.c_str());
    }
    if (!ok) ok = initShared(dev.p, first, error);
    if (!ok) {
        release();
        return false;
    }
    REFERENCE_TIME lat = 0;
    client_->GetStreamLatency(&lat);
    latency_ = (double)bufferFrames_ / rate_ + lat / 1e7;
    if (client_->SetEventHandle(event_) != S_OK) {
        error = "The audio device can't signal its buffers";
        release();
        return false;
    }
    HRESULT h = input_ ? client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_)
                       : client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
    if (FAILED(h)) {
        error = "The audio device's buffers can't be had (" + hr(h) + ")";
        release();
        return false;
    }
    if (!input_) {  // (silence in the whole buffer first: no glitch as it starts)
        BYTE* data = nullptr;
        UINT32 pad = 0;
        if (!exclusive_) client_->GetCurrentPadding(&pad);
        UINT32 n = bufferFrames_ - std::min(pad, bufferFrames_);
        if (n && SUCCEEDED(render_->GetBuffer(n, &data))) render_->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    {
        std::lock_guard<std::mutex> g(nameLock_);
        name_ = id_ == kDefaultId ? "Default (" + name + ")" : name;
    }
    int bits = sample_ == Sample::F32 ? 32 : sample_ == Sample::I16 ? 16 : sample_ == Sample::I24 ? 24 : 32;
    util::debugf("audio: WASAPI %s \"%s\" %s: %d Hz, %d channels, %s%d-bit, %d-frame period, %u-frame buffer, %.1f ms\n",
                 input_ ? "input" : "output", name.c_str(), exclusive_ ? "exclusive" : "shared", rate_, ch_,
                 sample_ == Sample::F32 ? "float " : "int ", bits, block_, bufferFrames_, latency_.load() * 1000);
    if (first) {
        out_.assign((size_t)block_ * (input_ ? 1 : ch_), 0.f);
        in_.assign((size_t)std::max<UINT32>(bufferFrames_ * 2, block_ * 4) * ch_, 0.f);
    }
    return true;
}

bool WasapiStream::initExclusive(IMMDevice* dev, bool first, std::string& error) {
    exclusive_ = false;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_))) {
        error = "The audio device can't be used";
        return false;
    }
    // the first sample format the device takes as it is (exclusive mode converts nothing)
    struct Try {
        Sample s;
        int valid;
    };
    const Try order[] = {{Sample::F32, 32}, {Sample::I32, 32}, {Sample::I32, 24}, {Sample::I24, 24}, {Sample::I16, 16}};
    WAVEFORMATEXTENSIBLE fmt{};
    bool found = false;
    for (const Try& t : order) {
        fmt = makeFormat(t.s, rate_, ch_, 0);
        fmt.Samples.wValidBitsPerSample = (WORD)t.valid;
        if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
            sample_ = t.s;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "the device takes none of float32 / int32 / int24 / int16 at " + std::to_string(rate_) + " Hz, " +
                std::to_string(ch_) + " channels";
        client_.reset();
        return false;
    }
    REFERENCE_TIME def = 0, min = 0;
    client_->GetDevicePeriod(&def, &min);
    int want = first ? cfg_.blocksize : block_;  // (0: the device's default period; else as near as it allows)
    REFERENCE_TIME period = want > 0 ? std::max(min > 0 ? min : def, (REFERENCE_TIME)(want * 1e7 / rate_ + 0.5)) : def;
    HRESULT h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                    &fmt.Format, nullptr);
    if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {  // (the period the device can do: from its buffer, a client anew)
        UINT32 n = 0;
        client_->GetBufferSize(&n);
        period = (REFERENCE_TIME)(1e7 * n / rate_ + 0.5);
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_))) return false;
        h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                &fmt.Format, nullptr);
    }
    if (SUCCEEDED(h)) client_->GetBufferSize(&bufferFrames_);
    if (SUCCEEDED(h) && !first && (int)bufferFrames_ != block_) h = E_FAIL;  // (reopened: the blocks must stay)
    if (FAILED(h)) {
        error = h == AUDCLNT_E_DEVICE_IN_USE ? "the device is in use by another program" : "Initialize failed (" + hr(h) + ")";
        client_.reset();
        return false;
    }
    block_ = (int)bufferFrames_;
    exclusive_ = true;
    return true;
}

bool WasapiStream::initShared(IMMDevice* dev, bool first, std::string& error) {
    exclusive_ = false;
    sample_ = Sample::F32;
    // the lowest delay: IAudioClient3 at the engine's own format and its smallest period (or the nearest to the
    // asked); (reopened after a loss: the plain way below, which keeps the blocks as they were)
    Ptr<IAudioClient3> c3;
    if (first && SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&c3))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(c3->GetMixFormat(&mix)) && mix) {
            if (isFloat32(mix) && (int)mix->nSamplesPerSec == rate_ && mix->nChannels == ch_) {
                UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                // (a buffer bigger than the engine's largest period: the plain way below, which can have it)
                if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx)) && fund > 0 && mn > 0 &&
                    cfg_.blocksize <= (int)std::max(mn, mx)) {
                    UINT32 want = cfg_.blocksize > 0 ? (UINT32)cfg_.blocksize : def;  // (0: the driver's own)
                    UINT32 period = std::clamp((want + fund - 1) / fund * fund, mn, std::max(mn, mx));
                    HRESULT h = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, mix, nullptr);
                    if (h == kPeriodLocked) {  // (another program set the engine's period: that one)
                        WAVEFORMATEX* cur = nullptr;
                        UINT32 curPeriod = 0;
                        if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cur, &curPeriod)) && curPeriod) {
                            period = curPeriod;
                            h = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, mix, nullptr);
                        }
                        if (cur) CoTaskMemFree(cur);
                    }
                    if (SUCCEEDED(h)) {
                        util::debugf("audio: WASAPI IAudioClient3: periods %u..%u (step %u, default %u): %u frames\n", mn,
                                     mx, fund, def, period);
                        client_.reset();
                        client_.p = c3.p, c3.p = nullptr;  // (an IAudioClient3 is an IAudioClient)
                        client_->GetBufferSize(&bufferFrames_);
                        block_ = (int)period;
                        CoTaskMemFree(mix);
                        return true;
                    }
                    util::debugf("audio: WASAPI InitializeSharedAudioStream failed (%s)\n", hr(h).c_str());
                }
            }
            CoTaskMemFree(mix);
        }
    }
    c3.reset();
    // any rate and channel count (Windows converts), at the device's default period
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_))) {
        error = "The audio device can't be used";
        return false;
    }
    WAVEFORMATEXTENSIBLE fmt = makeFormat(Sample::F32, rate_, ch_, 0);
    REFERENCE_TIME def = 0, min = 0;
    client_->GetDevicePeriod(&def, &min);
    if (def <= 0) def = 100000;
    int frames = first ? cfg_.blocksize : block_;
    REFERENCE_TIME buffer = frames > 0 ? (REFERENCE_TIME)(2e7 * frames / rate_) : 0;  // (two blocks)
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT h = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &fmt.Format, nullptr);
    if (SUCCEEDED(h)) client_->GetBufferSize(&bufferFrames_);
    if (SUCCEEDED(h) && !first && (int)bufferFrames_ < block_) h = E_FAIL;
    if (FAILED(h)) {
        error = "The audio device can't be opened (" + hr(h) + ")";
        client_.reset();
        return false;
    }
    if (first) block_ = std::max(16, std::min((int)bufferFrames_ / (frames > 0 ? 2 : 1),
                                             frames > 0 ? frames : (int)(def * rate_ / 10000000)));
    return true;
}

bool WasapiStream::start() {
    if (thread_.joinable()) return true;
    underflows = 0;
    fresh_ = true;
    if (client_ && FAILED(client_->Start())) return false;
    ResetEvent(quit_);
    thread_ = std::thread([this] { loop(); });
    return true;
}

void WasapiStream::stop() {
    if (!thread_.joinable()) return;
    SetEvent(quit_);
    thread_.join();
    if (client_) {
        client_->Stop();
        client_->Reset();
    }
}

bool WasapiStream::serve() {
    if (input_) {
        UINT32 packet = 0;
        if (FAILED(capture_->GetNextPacketSize(&packet))) return false;
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &n, &flags, nullptr, nullptr))) return false;
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !fresh_) underflows++;  // (the first: starting)
            fresh_ = false;
            size_t room = in_.size() / ch_ - inFrames_;
            size_t take = std::min<size_t>(n, room);
            float* dst = in_.data() + inFrames_ * ch_;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                std::fill(dst, dst + take * ch_, 0.f);
            else
                fromDevice(sample_, data, dst, take * ch_);
            inFrames_ += take;
            if (FAILED(capture_->ReleaseBuffer(n))) return false;
            size_t at = 0;
            for (; inFrames_ - at >= (size_t)block_; at += block_) {
                try {
                    cb_(in_.data() + at * ch_, out_.data(), block_);
                } catch (...) {
                }
            }
            if (at) memmove(in_.data(), in_.data() + at * ch_, (inFrames_ - at) * ch_ * sizeof(float)), inFrames_ -= at;
            if (FAILED(capture_->GetNextPacketSize(&packet))) return false;
        }
        return true;
    }
    UINT32 pad = 0;
    if (!exclusive_ && FAILED(client_->GetCurrentPadding(&pad))) return false;
    if (!exclusive_ && pad == 0) underflows++;  // (the device ran dry before this event)
    UINT32 avail = exclusive_ ? bufferFrames_ : bufferFrames_ - std::min(pad, bufferFrames_);
    for (; avail >= (UINT32)block_; avail -= block_) {
        BYTE* data = nullptr;
        HRESULT h = render_->GetBuffer((UINT32)block_, &data);
        if (FAILED(h)) return false;
        std::fill(out_.begin(), out_.end(), 0.f);
        try {
            cb_(nullptr, out_.data(), block_);
        } catch (...) {
            std::fill(out_.begin(), out_.end(), 0.f);
        }
        toDevice(sample_, out_.data(), data, out_.size());
        if (FAILED(render_->ReleaseBuffer((UINT32)block_, 0))) return false;
    }
    return true;
}

void WasapiStream::silentBlock() {
    std::fill(out_.begin(), out_.end(), 0.f);
    std::fill(in_.begin(), in_.begin() + (size_t)block_ * ch_, 0.f);
    try {
        cb_(input_ ? in_.data() : nullptr, out_.data(), block_);
    } catch (...) {
    }
}

void WasapiStream::loop() {
    Com com;
    detail::raiseThreadPriority();
    using clock = std::chrono::steady_clock;
    const auto blockTime = std::chrono::duration<double>((double)block_ / rate_);
    bool gone = !client_;
    auto paced = clock::now(), nextTry = clock::now();
    HANDLE waits[2] = {quit_, event_};
    while (WaitForSingleObject(quit_, 0) != WAIT_OBJECT_0) {
        bool follow = watch_ && watch_->changed.exchange(false);
        if (follow && !gone) {  // (the default device changed: onto the new one)
            util::debugf("audio: WASAPI: the default %s changed\n", input_ ? "input" : "output");
            client_->Stop();
            release();
            gone = true;
            nextTry = clock::now();
        }
        if (gone) {
            if (follow) nextTry = clock::now();
            if (clock::now() >= nextTry) {
                std::string err;
                if (init(err, false) && SUCCEEDED(client_->Start())) {
                    gone = false;
                    lost = false;
                    inFrames_ = 0;
                    fresh_ = true;
                    util::debugf("audio: WASAPI: the %s is back\n", input_ ? "input" : "output");
                    continue;
                }
                release();
                lost = true;
                nextTry = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(REOPEN_S));
            }
            // silence at the stream's pace meanwhile (the transport goes on)
            auto now = clock::now();
            if (now - paced > std::chrono::seconds(1)) paced = now;  // (no burst after a long stall)
            while (paced + blockTime <= now) {
                silentBlock();
                paced += std::chrono::duration_cast<clock::duration>(blockTime);
            }
            WaitForSingleObject(quit_, std::max<DWORD>(1, (DWORD)(blockTime.count() * 1000)));
            continue;
        }
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        bool ok = true;
        if (w == WAIT_TIMEOUT) {  // (no event for 2 s: the device stopped; is it still there?)
            UINT32 pad;
            ok = SUCCEEDED(client_->GetCurrentPadding(&pad));
        } else {
            ok = serve();
        }
        if (!ok) {
            util::debugf("audio: WASAPI: the %s went away: silent until it comes back\n", input_ ? "input" : "output");
            release();
            gone = true;
            lost = true;
            paced = clock::now();
            nextTry = paced + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(REOPEN_S));
        }
    }
}

}  // namespace

// --- the public interface (devices.h) ---

std::vector<DriverInfo> driverTypes() {
    if (lists().devs[0].empty()) return {};
    return {{kDriver, "WASAPI",
             "Windows' own sound system: shared with other programs at the lowest delay the driver allows, or "
             "exclusive (lower still; other programs go silent)."}};
}

std::string driverLabel(const std::string& key) { return key; }

std::vector<DeviceInfo> outputDevices(const std::string& driverType) {
    return driverType == kDriver ? devices(false) : std::vector<DeviceInfo>();
}

std::vector<DeviceInfo> inputDevices(const std::string& driverType) {
    return driverType == kDriver ? devices(true) : std::vector<DeviceInfo>();
}

std::string defaultOutput(const std::string& driverType) {
    return driverType == kDriver && !lists().devs[0].empty() ? kDefaultId : "";
}

std::string defaultInput(const std::string& driverType) {
    return driverType == kDriver && !lists().devs[1].empty() ? kDefaultId : "";
}

bool deviceInfo(const std::string& driverType, const std::string& id, DeviceInfo& out, bool input) {
    if (driverType != kDriver) return false;
    Lists l = lists();
    const Endpoint* e = find(l, id, input);
    if (!e) return false;
    out = toInfo(*e, input, id, id == kDefaultId ? "Default (" + e->name + ")" : e->name);
    return true;
}

double minLatency(const std::string&) { return 0; }
double minLatency(const std::string& driverType, const std::string&) { return minLatency(driverType); }

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
    if (cfg.driverType != kDriver) {
        error = "Unknown driver type \"" + cfg.driverType + "\"";
        return nullptr;
    }
    comReady();
    std::unique_ptr<WasapiStream> s(new WasapiStream());
    if (!s->open(cfg, std::move(cb), error)) return nullptr;
    s->exclusive = s->exclusiveMode();
    return s;
}

}  // namespace audio
