// Tiny Test Fx: a VST3 plugin file for Tiny DAW's tests (make plugins →
// build/test-plugins/TinyTestFx.vst3, with Contents/Resources/moduleinfo.json), written on
// plugins/vst3_api.h the way a plugin is written on the SDK: a module exporting GetPluginFactory,
// ModuleEntry and ModuleExit, a factory (IPluginFactory3) with three classes:
//
//   "Tiny Test Fx" (Fx|Dynamics): a processor with a separate controller.
//      Buses: stereo main in, stereo aux in ("Sidechain"), stereo main out.
//      out = in, 64 frames late (it reports latency 64), × 2·Gain, × 0.25 while Duck is on and the
//      sidechain is louder than 0.01 RMS in the buffer.
//      Parameters: 100 Gain (continuous, default 0.5 = ×1, "1.00 x"), 101 Duck (a switch: stepCount
//      1, default on, "Off"/"On"), 102 Meter (read only: the input's peak, sent by the processor from
//      the audio thread as an IMessage through the IConnectionPoints), 103 Peak (read only: the
//      same, reported as an output parameter change).
//      States: processor "TFX1" + gain + duck; controller "TFXC" + its window's size.
//      Its window (Xlib, an X11 child window, on the host's IRunLoop): click to switch Duck
//      (beginEdit/performEdit/endEdit), right-click to ask for a bigger window (resizeView).
//   "Tiny Test Fx Controller" (Component Controller Class).
//   "Tiny Test Single" (Tools): a single-component plugin (the component is its controller),
//      out = in × Level (parameter 1, default 1). Its sub-categories say neither instrument nor
//      effect, so the probe has to create it to ask. Its controller maps the mod wheel (CC 1) and the
//      bend wheel to Level (IMidiMapping): where and what Level's last change was is told to the tests
//      (TinyTestFx_counts 5, 6, 7).
//   On Windows (make win-plugins, for the Wine tests) its window is a Win32 child window (HWND),
//   redrawn on a timer of its own, and the module exports InitDll/ExitDll as Windows plugins do.
#include "plugins/vst3_api.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "win_dnd.h"  // (its window is an OLE drop target: files dropped on it are marked)
#else
#include <X11/Xlib.h>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vst3;
using namespace vst3::Vst;

namespace {

const TUID PROCESSOR_CID = VST3_UID(0x7D1A3B01, 0x11224A3B, 0x8C4D5E6F, 0x10203040);
const TUID CONTROLLER_CID = VST3_UID(0x7D1A3B02, 0x11224A3B, 0x8C4D5E6F, 0x10203040);
const TUID SINGLE_CID = VST3_UID(0x7D1A3B03, 0x11224A3B, 0x8C4D5E6F, 0x10203040);

constexpr int LATENCY = 64;
enum : ParamID { GAIN = 100, DUCK = 101, METER = 102, PEAK = 103 };

std::atomic<int> moduleEntries{0}, moduleExits{0}, instances{0};
// (Tiny Test Single) Level's last change in a buffer: its sample offset, value × 10000, and changes seen
std::atomic<int> levelOffset{-1}, levelValue{-1}, levelChanges{0};

void copy16(const char* s, char16* out, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++)
        out[i] = (char16)(unsigned char)s[i];
    out[i] = 0;
}

void copy8(const char* s, char* out, size_t max) {
    std::strncpy(out, s, max - 1);
    out[max - 1] = 0;
}

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// counted base for the plugin's objects
#define REFCOUNTED                                                       \
    std::atomic<uint32> refs{1};                                         \
    uint32 PLUGIN_API addRef() override { return ++refs; }               \
    uint32 PLUGIN_API release() override {                               \
        uint32 n = --refs;                                               \
        if (!n)                                                          \
            delete this;                                                 \
        return n;                                                        \
    }

template <class I, class T>
bool give(T* self, const TUID iid, void** obj) {
    if (!iidEqual(iid, I::iid))
        return false;
    *obj = static_cast<I*>(self);
    self->addRef();
    return true;
}

bool writeAll(IBStream* s, const void* data, int32 n) {
    int32 done = 0;
    return s && s->write(const_cast<void*>(data), n, &done) == kResultOk && done == n;
}

bool readAll(IBStream* s, void* data, int32 n) {
    int32 done = 0;
    return s && s->read(data, n, &done) == kResultOk && done == n;
}

// ===================================================================================== the processor

class Processor final : public IComponent, public IAudioProcessor, public Vst::IConnectionPoint {
public:
    REFCOUNTED
    Processor() { instances++; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (give<IComponent>(this, iid, obj) || give<IPluginBase>(this, iid, obj) ||
            give<IAudioProcessor>(this, iid, obj) || give<Vst::IConnectionPoint>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<FUnknown*>(static_cast<IComponent*>(this));
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (host)
            return kResultFalse;
        host = query<IHostApplication>(context);
        return host ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API terminate() override {
        if (host)
            host->release();
        host = nullptr;
        return kResultOk;
    }

    tresult PLUGIN_API getControllerClassId(TUID classId) override {
        std::memcpy(classId, CONTROLLER_CID, 16);
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
        if (type != kAudio)
            return 0;
        return dir == kInput ? 2 : 1;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index < 0 || index >= getBusCount(type, dir))
            return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 2;
        bool aux = dir == kInput && index == 1;
        copy16(dir == kOutput ? "Output" : aux ? "Sidechain" : "Input", bus.name, 128);
        bus.busType = aux ? kAux : kMain;
        bus.flags = aux ? 0 : BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override {
        if (type == kAudio && dir == kInput && index == 1)
            sidechainActive = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            for (auto& line : delay)
                line.assign(LATENCY, 0.0f);
            delayAt = 0;
        }
        active = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override {
        char magic[4];
        double g, d;
        if (!readAll(state, magic, 4) || std::memcmp(magic, "TFX1", 4) != 0 || !readAll(state, &g, 8) ||
            !readAll(state, &d, 8))
            return kResultFalse;
        gain = clamp01(g);
        duck = clamp01(d);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override {
        double g = gain, d = duck;
        return writeAll(state, "TFX1", 4) && writeAll(state, &g, 8) && writeAll(state, &d, 8) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs,
                                          int32 numOuts) override {
        if (numIns != 2 || numOuts != 1)
            return kResultFalse;
        for (int32 i = 0; i < numIns; i++)
            if (inputs[i] != SpeakerArr::kStereo)
                return kResultFalse;
        return outputs[0] == SpeakerArr::kStereo ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
        if (index < 0 || index >= getBusCount(kAudio, dir))
            return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return LATENCY; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if (setup.symbolicSampleSize != kSample32 || setup.sampleRate <= 0)
            return kResultFalse;
        rate = setup.sampleRate;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return LATENCY; }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (auto* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); i++) {
                IParamValueQueue* q = changes->getParameterData(i);
                int32 offset;
                ParamValue v;
                if (!q || q->getPointCount() <= 0 || q->getPoint(q->getPointCount() - 1, offset, v) != kResultOk)
                    continue;
                if (q->getParameterId() == GAIN)
                    gain = clamp01(v);
                else if (q->getParameterId() == DUCK)
                    duck = clamp01(v);
            }
        }
        if (data.numSamples <= 0 || data.numOutputs < 1 || data.numInputs < 1)
            return kResultOk;  // (a flush of parameters only)
        AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];
        float level = 0;
        if (sidechainActive && data.numInputs > 1 && data.inputs[1].numChannels > 0) {
            double sum = 0;
            for (int32 c = 0; c < data.inputs[1].numChannels; c++)
                for (int32 i = 0; i < data.numSamples; i++) {
                    float x = data.inputs[1].channelBuffers32[c][i];
                    sum += x * x;
                }
            level = (float)std::sqrt(sum / (data.numSamples * data.inputs[1].numChannels));
        }
        float factor = (float)(2.0 * gain) * (duck >= 0.5 && level > 0.01f ? 0.25f : 1.0f);
        float peak = 0;
        for (int32 c = 0; c < out.numChannels && c < 2; c++) {
            const float* x = in.channelBuffers32[c < in.numChannels ? c : 0];
            float* y = out.channelBuffers32[c];
            std::vector<float>& line = delay[(size_t)c];
            int at = delayAt;
            for (int32 i = 0; i < data.numSamples; i++) {
                float now = x[i];  // (in place: read before writing)
                peak = std::max(peak, std::fabs(now));
                float late = line[(size_t)at];
                line[(size_t)at] = now;
                at = (at + 1) % LATENCY;
                y[i] = late * factor;
            }
        }
        delayAt = (delayAt + data.numSamples) % LATENCY;
        // the peak: to the controller as a message (from this, the audio thread), and as an output change
        if (++blocks % 2 == 0 && peer && host) {
            IMessage* message = nullptr;
            TUID iid;
            std::memcpy(iid, IMessage::iid, 16);
            if (host->createInstance(iid, iid, (void**)&message) == kResultOk && message) {
                message->setMessageID("meter");
                message->getAttributes()->setFloat("level", peak);
                peer->notify(message);
                message->release();
            }
        }
        if (data.outputParameterChanges) {
            int32 index, point;
            ParamID id = PEAK;
            if (IParamValueQueue* q = data.outputParameterChanges->addParameterData(id, index))
                q->addPoint(0, clamp01(peak), point);
        }
        return kResultOk;
    }

    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override {
        if (!other || peer)
            return kResultFalse;
        peer = other;
        peer->addRef();
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* other) override {
        if (!peer || other != peer)
            return kResultFalse;
        peer->release();
        peer = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

private:
    ~Processor() {
        if (peer)
            peer->release();
        if (host)
            host->release();
        instances--;
    }
    IHostApplication* host = nullptr;
    Vst::IConnectionPoint* peer = nullptr;
    double rate = 44100, gain = 0.5, duck = 1.0;
    bool active = false, sidechainActive = false;
    std::vector<float> delay[2] = {std::vector<float>(LATENCY, 0.0f), std::vector<float>(LATENCY, 0.0f)};
    int delayAt = 0;
    unsigned blocks = 0;
};

// ===================================================================================== the controller

class Controller;

class View final : public IPlugView, public IPlugViewContentScaleSupport, public Linux::IEventHandler,
                   public Linux::ITimerHandler {
public:
    REFCOUNTED
    explicit View(Controller* c);
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (give<IPlugView>(this, iid, obj) || give<IPlugViewContentScaleSupport>(this, iid, obj) ||
            give<Linux::IEventHandler>(this, iid, obj) || give<Linux::ITimerHandler>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<FUnknown*>(static_cast<IPlugView*>(this));
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
#ifdef _WIN32
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
#else
        return type && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue : kResultFalse;
#endif
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* size) override;
    tresult PLUGIN_API onSize(ViewRect* newSize) override;
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* f) override {
        frame = f;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override {
        if (!rect)
            return kInvalidArgument;
        int w = std::max(200, std::min(1600, rect->getWidth())), h = std::max(120, std::min(1200, rect->getHeight()));
        rect->right = rect->left + w;
        rect->bottom = rect->top + h;
        return kResultOk;
    }
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override {
        scale = factor > 0 ? factor : 1;
        return kResultOk;
    }
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override { pump(); }
    void PLUGIN_API onTimer() override {
        pump();
        draw();
    }

#ifdef _WIN32
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
#endif

private:
    ~View();
    void pump();
    void draw();
    Controller* controller;
    IPlugFrame* frame = nullptr;
    Linux::IRunLoop* loop = nullptr;
#ifdef _WIN32
    HWND window = nullptr;
    win_dnd::Target* dropTarget = nullptr;
#else
    Display* display = nullptr;
    ::Window window = 0;
#endif
    float scale = 1;
};

class Controller final : public IEditController, public Vst::IConnectionPoint {
public:
    REFCOUNTED
    Controller() { instances++; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (give<IEditController>(this, iid, obj) || give<IPluginBase>(this, iid, obj) ||
            give<Vst::IConnectionPoint>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<FUnknown*>(static_cast<IEditController*>(this));
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        IHostApplication* host = query<IHostApplication>(context);
        if (!host)
            return kResultFalse;
        String128 name;
        host->getName(name);
        hostName.clear();
        for (int i = 0; i < 128 && name[i]; i++)
            hostName += (char)name[i];
        host->release();
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        setComponentHandler(nullptr);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* state) override {
        char magic[4];
        double g, d;
        if (!readAll(state, magic, 4) || std::memcmp(magic, "TFX1", 4) != 0 || !readAll(state, &g, 8) ||
            !readAll(state, &d, 8))
            return kResultFalse;
        values[0] = clamp01(g);
        values[1] = clamp01(d);
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override {
        char magic[4];
        int32 w, h;
        if (!readAll(state, magic, 4) || std::memcmp(magic, "TFXC", 4) != 0 || !readAll(state, &w, 4) ||
            !readAll(state, &h, 4))
            return kResultFalse;
        viewW = w;
        viewH = h;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override {
        int32 w = viewW, h = viewH;
        return writeAll(state, "TFXC", 4) && writeAll(state, &w, 4) && writeAll(state, &h, 4) ? kResultOk : kResultFalse;
    }
    int32 PLUGIN_API getParameterCount() override { return 4; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index < 0 || index >= 4)
            return kInvalidArgument;
        static const char* titles[] = {"Gain", "Duck", "Meter", "Peak"};
        static const char* units[] = {"x", "", "", ""};
        std::memset(&info, 0, sizeof(info));
        info.id = (ParamID)(GAIN + index);
        copy16(titles[index], info.title, 128);
        copy16(titles[index], info.shortTitle, 128);
        copy16(units[index], info.units, 128);
        info.stepCount = index == 1 ? 1 : 0;
        info.defaultNormalizedValue = index == 0 ? 0.5 : index == 1 ? 1.0 : 0.0;
        info.unitId = 0;
        info.flags = index < 2 ? ParameterInfo::kCanAutomate : ParameterInfo::kIsReadOnly;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 string) override {
        char text[64];
        if (id == GAIN)
            std::snprintf(text, sizeof(text), "%.2f x", 2 * v);
        else if (id == DUCK)
            std::snprintf(text, sizeof(text), "%s", v >= 0.5 ? "On" : "Off");
        else if (id == METER || id == PEAK)
            std::snprintf(text, sizeof(text), "%.3f", v);
        else
            return kInvalidArgument;
        copy16(text, string, 128);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& v) override {
        char text[64];
        int i = 0;
        for (; string && string[i] && i < 63; i++)
            text[i] = (char)string[i];
        text[i] = 0;
        if (id == DUCK) {
            v = std::strcmp(text, "On") == 0 ? 1 : 0;
            return kResultOk;
        }
        double x = std::atof(text);
        v = clamp01(id == GAIN ? x / 2 : x);
        return kResultOk;
    }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override {
        return id == GAIN ? 2 * v : id == DUCK ? std::round(v) : v;
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) override {
        return clamp01(id == GAIN ? v / 2 : v);
    }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return id >= GAIN && id <= PEAK ? values[id - GAIN].load() : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id < GAIN || id > PEAK)
            return kInvalidArgument;
        values[id - GAIN] = id == DUCK ? (v >= 0.5 ? 1.0 : 0.0) : clamp01(v);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
        if (h)
            h->addRef();
        if (handler)
            handler->release();
        handler = h;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        if (!name || std::strcmp(name, ViewType::kEditor) != 0)
            return nullptr;
        return new View(this);
    }

    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override {
        if (!other || peer)
            return kResultFalse;
        peer = other;
        peer->addRef();
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* other) override {
        if (!peer || other != peer)
            return kResultFalse;
        peer->release();
        peer = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(IMessage* message) override {  // (any thread)
        if (!message || !message->getMessageID() || std::strcmp(message->getMessageID(), "meter") != 0)
            return kResultFalse;
        double level = 0;
        if (message->getAttributes()->getFloat("level", level) != kResultOk)
            return kResultFalse;
        values[2] = clamp01(level);
        return kResultOk;
    }

    // (the window) a click switches Duck, as a user's edit
    void userSwitchesDuck() {
        double v = values[1] >= 0.5 ? 0.0 : 1.0;
        values[1] = v;
        if (handler) {
            handler->beginEdit(DUCK);
            handler->performEdit(DUCK, v);
            handler->endEdit(DUCK);
        }
    }

    std::atomic<double> values[4] = {{0.5}, {1.0}, {0.0}, {0.0}};
    int32 viewW = 300, viewH = 200;
    std::string hostName;

private:
    ~Controller() {
        if (handler)
            handler->release();
        if (peer)
            peer->release();
        instances--;
    }
    IComponentHandler* handler = nullptr;
    Vst::IConnectionPoint* peer = nullptr;
};

View::View(Controller* c) : controller(c) { controller->addRef(); }

View::~View() {
    removed();
    controller->release();
}

#ifdef _WIN32

namespace {
COLORREF rgb(unsigned c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

void fill(HDC dc, int x, int y, int w, int h, unsigned color) {
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(rgb(color));
    FillRect(dc, &r, b);
    DeleteObject(b);
}
}  // namespace

LRESULT CALLBACK View::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View* v = (View*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!v)
        return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Controller* c = v->controller;
            int w = (int)(c->viewW * v->scale), h = (int)(c->viewH * v->scale);
            fill(dc, 0, 0, w, h, 0x283040);
            fill(dc, 10, 40, 60, 30, c->values[1] >= 0.5 ? 0xE0A030 : 0x606060);
            fill(dc, 10, h - 30, (int)((w - 20) * c->values[2]), 16, 0x40C0E0);
            static const unsigned swatches[] = {0xE04040, 0x4080E0, 0xE0E040, 0xA040E0, 0x40E0C0};
            for (int i = 0; i < 5; i++) fill(dc, 90 + i * 24, 46, 18, 18, swatches[i]);  // (colours to see)
            char text[160];
            std::snprintf(text, sizeof(text), "Tiny Test Fx (Windows) in %s: click = Duck %s, right-click = bigger",
                          c->hostName.c_str(), c->values[1] >= 0.5 ? "on" : "off");
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, rgb(0xFFFFFF));
            TextOutA(dc, 10, 8, text, (int)std::strlen(text));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);  // (the meter)
            return 0;
        case WM_LBUTTONDOWN:
            v->controller->userSwitchesDuck();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_RBUTTONDOWN:
            if (v->frame) {
                ViewRect r(0, 0, (int32)((v->controller->viewW + 40) * v->scale),
                           (int32)((v->controller->viewH + 20) * v->scale));
                v->frame->resizeView(v, &r);  // (the host calls onSize)
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

tresult PLUGIN_API View::attached(void* parent, FIDString type) {
    if (isPlatformTypeSupported(type) != kResultTrue || !parent)
        return kResultFalse;
    static ATOM cls = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&View::windowProc, &self);
    if (!cls) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = View::windowProc;
        wc.hInstance = self;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TinyTestFxView";
        cls = RegisterClassW(&wc);
    }
    int w = (int)(controller->viewW * scale), h = (int)(controller->viewH * scale);
    window = CreateWindowExW(0, L"TinyTestFxView", L"Tiny Test Fx", WS_CHILD | WS_VISIBLE, 0, 0, w, h, (HWND)parent,
                             nullptr, self, nullptr);
    if (!window)
        return kResultFalse;
    SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)this);
    SetTimer(window, 1, 50, nullptr);
    OleInitialize(nullptr);
    dropTarget = new win_dnd::Target(L"vst3");
    if (RegisterDragDrop(window, dropTarget) != S_OK) {
        dropTarget->Release();
        dropTarget = nullptr;
    }
    return kResultOk;
}

tresult PLUGIN_API View::removed() {
    if (window) {
        if (dropTarget) {
            RevokeDragDrop(window);
            dropTarget->Release();
            dropTarget = nullptr;
        }
        KillTimer(window, 1);
        DestroyWindow(window);
        window = nullptr;
    }
    return kResultOk;
}

tresult PLUGIN_API View::getSize(ViewRect* size) {
    if (!size)
        return kInvalidArgument;
    *size = ViewRect(0, 0, (int32)(controller->viewW * scale), (int32)(controller->viewH * scale));
    return kResultOk;
}

tresult PLUGIN_API View::onSize(ViewRect* r) {
    if (!r)
        return kInvalidArgument;
    controller->viewW = (int32)(r->getWidth() / scale);
    controller->viewH = (int32)(r->getHeight() / scale);
    if (window)
        SetWindowPos(window, nullptr, 0, 0, std::max(1, r->getWidth()), std::max(1, r->getHeight()),
                     SWP_NOMOVE | SWP_NOZORDER);
    return kResultOk;
}

void View::draw() {
    if (window)
        InvalidateRect(window, nullptr, FALSE);
}

void View::pump() {}  // (Windows sends the window its messages itself)

#else

tresult PLUGIN_API View::attached(void* parent, FIDString type) {
    if (isPlatformTypeSupported(type) != kResultTrue || !parent)
        return kResultFalse;
    display = XOpenDisplay(nullptr);
    if (!display)
        return kResultFalse;
    int w = (int)(controller->viewW * scale), h = (int)(controller->viewH * scale);
    window = XCreateSimpleWindow(display, (::Window)(uintptr_t)parent, 0, 0, (unsigned)w, (unsigned)h, 0, 0, 0x283040);
    XSelectInput(display, window, ExposureMask | ButtonPressMask);
    XMapWindow(display, window);
    XFlush(display);
    loop = query<Linux::IRunLoop>(frame);  // (the host's run loop is asked of the frame)
    if (loop) {
        loop->registerEventHandler(this, ConnectionNumber(display));
        loop->registerTimer(this, 50);
    }
    return kResultOk;
}

tresult PLUGIN_API View::removed() {
    if (loop) {
        loop->unregisterEventHandler(this);
        loop->unregisterTimer(this);
        loop->release();
        loop = nullptr;
    }
    if (display) {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        display = nullptr;
        window = 0;
    }
    return kResultOk;
}

tresult PLUGIN_API View::getSize(ViewRect* size) {
    if (!size)
        return kInvalidArgument;
    *size = ViewRect(0, 0, (int32)(controller->viewW * scale), (int32)(controller->viewH * scale));
    return kResultOk;
}

tresult PLUGIN_API View::onSize(ViewRect* r) {
    if (!r)
        return kInvalidArgument;
    controller->viewW = (int32)(r->getWidth() / scale);
    controller->viewH = (int32)(r->getHeight() / scale);
    if (display) {
        XResizeWindow(display, window, (unsigned)std::max(1, r->getWidth()), (unsigned)std::max(1, r->getHeight()));
        XFlush(display);
    }
    return kResultOk;
}

void View::draw() {
    if (!display)
        return;
    int w = (int)(controller->viewW * scale), h = (int)(controller->viewH * scale);
    GC gc = DefaultGC(display, DefaultScreen(display));
    XSetForeground(display, gc, 0x283040);
    XFillRectangle(display, window, gc, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(display, gc, controller->values[1] >= 0.5 ? 0xE0A030 : 0x606060);
    XFillRectangle(display, window, gc, 10, 40, 60, 30);
    XSetForeground(display, gc, 0x40C0E0);
    XFillRectangle(display, window, gc, 10, h - 30, (unsigned)((w - 20) * controller->values[2]), 16);
    XSetForeground(display, gc, 0xFFFFFF);
    char text[160];
    std::snprintf(text, sizeof(text), "Tiny Test Fx in %s: click = Duck %s, right-click = bigger",
                  controller->hostName.c_str(), controller->values[1] >= 0.5 ? "on" : "off");
    XDrawString(display, window, gc, 10, 20, text, (int)std::strlen(text));
    XFlush(display);
}

void View::pump() {
    if (!display)
        return;
    while (display && XPending(display)) {
        XEvent ev;
        XNextEvent(display, &ev);
        if (ev.type == Expose) {
            draw();
        } else if (ev.type == ButtonPress && ev.xbutton.button == 1) {
            controller->userSwitchesDuck();
            draw();
        } else if (ev.type == ButtonPress && ev.xbutton.button == 3 && frame) {
            ViewRect r(0, 0, (int32)((controller->viewW + 40) * scale), (int32)((controller->viewH + 20) * scale));
            frame->resizeView(this, &r);  // (the host calls onSize)
        }
    }
}

#endif

// ===================================================================================== the single-component plugin

class Single final : public IComponent, public IAudioProcessor, public IEditController, public IMidiMapping {
public:
    REFCOUNTED
    Single() { instances++; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (give<IComponent>(this, iid, obj) || give<IAudioProcessor>(this, iid, obj) ||
            give<IEditController>(this, iid, obj) || give<IMidiMapping>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, IPluginBase::iid) || iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IPluginBase*>(static_cast<IComponent*>(this));
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override {
        terminated++;
        return kResultOk;
    }
    // component
    tresult PLUGIN_API getControllerClassId(TUID) override { return kResultFalse; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0)
            return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 2;
        copy16(dir == kInput ? "In" : "Out", bus.name, 128);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream* state) override {
        char magic[4];
        double v;
        if (!readAll(state, magic, 4) || std::memcmp(magic, "TSG1", 4) != 0 || !readAll(state, &v, 8))
            return kResultFalse;
        level = clamp01(v);
        levelShown = level.load();  // (one object: its controller shows what it now plays)
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override {
        double v = level;
        return writeAll(state, "TSG1", 4) && writeAll(state, &v, 8) ? kResultOk : kResultFalse;
    }
    // processor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32, SpeakerArrangement*, int32) override {
        return kResultOk;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0)
            return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (auto* changes = data.inputParameterChanges)
            for (int32 i = 0; i < changes->getParameterCount(); i++) {
                IParamValueQueue* q = changes->getParameterData(i);
                int32 offset;
                ParamValue v;
                if (q && q->getParameterId() == 1 && q->getPointCount() > 0 &&
                    q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk) {
                    level = clamp01(v);
                    levelOffset = offset;
                    levelValue = (int)std::lround(v * 10000);
                    levelChanges++;
                }
            }
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1)
            return kResultOk;
        for (int32 c = 0; c < data.outputs[0].numChannels; c++)
            for (int32 i = 0; i < data.numSamples; i++)
                data.outputs[0].channelBuffers32[c][i] =
                    data.inputs[0].channelBuffers32[c < data.inputs[0].numChannels ? c : 0][i] * (float)level;
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    // controller
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0)
            return kInvalidArgument;
        std::memset(&info, 0, sizeof(info));
        info.id = 1;
        copy16("Level", info.title, 128);
        copy16("Level", info.shortTitle, 128);
        info.defaultNormalizedValue = 1.0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 string) override {
        if (id != 1)
            return kInvalidArgument;
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f %%", v * 100);
        copy16(text, string, 128);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id == 1 ? (double)levelShown : 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id != 1)
            return kInvalidArgument;
        levelShown = clamp01(v);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }
    // the mod wheel and the bend wheel (any channel) move Level
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16, CtrlNumber number, ParamID& id) override {
        if (bus != 0 || (number != kCtrlModWheel && number != kPitchBend))
            return kResultFalse;
        id = 1;
        return kResultTrue;
    }

    static std::atomic<int> terminated;

private:
    ~Single() { instances--; }
    std::atomic<double> level{1.0};
    std::atomic<double> levelShown{1.0};
};
std::atomic<int> Single::terminated{0};

// ===================================================================================== the factory

struct ClassDef {
    const int8* cid;
    const char* category;
    const char* name;
    const char* subcategories;
};

const ClassDef CLASSES[] = {
    {PROCESSOR_CID, kVstAudioEffectClass, "Tiny Test Fx", "Fx|Dynamics"},
    {CONTROLLER_CID, kVstComponentControllerClass, "Tiny Test Fx Controller", ""},
    {SINGLE_CID, kVstAudioEffectClass, "Tiny Test Single", "Tools"},
};

class Factory final : public IPluginFactory3 {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iidEqual(iid, IPluginFactory3::iid) || iidEqual(iid, IPluginFactory2::iid) ||
            iidEqual(iid, IPluginFactory::iid) || iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }  // (static: never deleted)
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info)
            return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        copy8("Tiny DAW Tests", info->vendor, sizeof(info->vendor));
        copy8("https://example.invalid/tinydaw", info->url, sizeof(info->url));
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 3; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index < 0 || index >= 3 || !info)
            return kInvalidArgument;
        std::memcpy(info->cid, CLASSES[index].cid, 16);
        info->cardinality = PClassInfo::kManyInstances;
        copy8(CLASSES[index].category, info->category, sizeof(info->category));
        copy8(CLASSES[index].name, info->name, sizeof(info->name));
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (index < 0 || index >= 3 || !info)
            return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, CLASSES[index].cid, 16);
        info->cardinality = PClassInfo::kManyInstances;
        copy8(CLASSES[index].category, info->category, sizeof(info->category));
        copy8(CLASSES[index].name, info->name, sizeof(info->name));
        copy8(CLASSES[index].subcategories, info->subCategories, sizeof(info->subCategories));
        copy8("", info->vendor, sizeof(info->vendor));  // (empty: the factory's vendor)
        copy8("1.0.0", info->version, sizeof(info->version));
        copy8("VST 3.7.9", info->sdkVersion, sizeof(info->sdkVersion));
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) override {
        if (index < 0 || index >= 3 || !info)
            return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, CLASSES[index].cid, 16);
        info->cardinality = PClassInfo::kManyInstances;
        copy8(CLASSES[index].category, info->category, sizeof(info->category));
        copy16(CLASSES[index].name, info->name, PClassInfo::kNameSize);
        copy8(CLASSES[index].subcategories, info->subCategories, sizeof(info->subCategories));
        copy16("", info->vendor, PClassInfo2::kVendorSize);
        copy16("1.0.0", info->version, PClassInfo2::kVersionSize);
        copy16("VST 3.7.9", info->sdkVersion, PClassInfo2::kVersionSize);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!obj)
            return kInvalidArgument;
        *obj = nullptr;
        FUnknown* made = nullptr;
        if (iidEqual(cid, PROCESSOR_CID))
            made = static_cast<IComponent*>(new Processor());
        else if (iidEqual(cid, CONTROLLER_CID))
            made = static_cast<IEditController*>(new Controller());
        else if (iidEqual(cid, SINGLE_CID))
            made = static_cast<IComponent*>(new Single());
        if (!made)
            return kNoInterface;
        tresult result = made->queryInterface(iid, obj);
        made->release();
        return result;
    }
    tresult PLUGIN_API setHostContext(FUnknown* context) override {
        hostContextSet = context != nullptr;
        return kResultOk;
    }
    std::atomic<uint32> refs{0};
    bool hostContextSet = false;
};

Factory factory;

}  // namespace

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXPORT IPluginFactory* PLUGIN_API GetPluginFactory() {
    factory.addRef();
    return &factory;
}

#ifdef _WIN32
// (Windows: InitDll/ExitDll, called once the DLL is loaded and before it is let go)
EXPORT bool InitDll() {
    moduleEntries++;
    return true;
}

EXPORT bool ExitDll() {
    moduleExits++;
    return true;
}
#else
EXPORT bool ModuleEntry(void*) {
    moduleEntries++;
    return true;
}

EXPORT bool ModuleExit() {
    moduleExits++;
    return true;
}
#endif

// (for the tests: what the host did with the module)
EXPORT int TinyTestFx_counts(int which) {
    switch (which) {
        case 0: return moduleEntries.load();
        case 1: return moduleExits.load();
        case 2: return instances.load();
        case 3: return factory.hostContextSet ? 1 : 0;
        case 4: return Single::terminated.load();
        case 5: return levelOffset.load();
        case 6: return levelValue.load();
        case 7: return levelChanges.load();
        default: return -1;
    }
}
