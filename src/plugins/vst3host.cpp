// Tiny DAW's VST3 plugin host, on its own declaration of the VST3 interfaces (plugins/vst3_api.h):
// the port of native/vst3host.cpp, with the helper objects Steinberg's SDK used to provide written
// here (loading modules, the host application, memory streams, messages, parameter changes, event
// lists, the window's frame and run loop).
//
// Threads (the VST3 rules): loading, state, parameters' names and texts, and the plugin's window
// belong to the main thread; process() and render() are called from the audio thread. Values flow
// between them through small queues: setParam() (either thread) reaches the plugin's processor with
// the next buffer and its window with the next idle(); changes made in the window reach the
// processor with the next buffer, and are listed by edited(). Call idle() now and then from the main
// thread. While the plugin is being reconfigured (prepare(), reset(), setState()) the audio thread
// gets silence (or, for an effect, its input unchanged) rather than waiting.
//
// A plugin's two halves (its processor and its controller, which runs its window) are connected
// directly, as JUCE-based hosts do, rather than through a proxy that only passes messages sent on
// the main thread: many plugins send their meters and analyzers (Pro-Q 4's spectrum, Pro-L 2's
// levels…) from the audio thread, and their windows would stand still.
//
// Buses: the main input and output are stereo (when the plugin accepts it); a plugin's first
// auxiliary audio input, if it has one, is its sidechain: process() feeds it, silence by default.
//
// Modules (the plugin files) are loaded once (ModuleEntry) and kept loaded, like the original's
// `loaded` map: a module stays loaded while any of its plugins lives, and unloadUnusedModules()
// (ModuleExit, then unloading) is only for those nothing uses any more.
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/param_changes.h"
#include "plugins/plugin.h"
#include "plugins/vst3_api.h"
#include "plugins/winlib.h"
#ifndef _WIN32
#include "plugins/library.h"
#include "plugins/wine.h"
#endif

namespace plug {

std::unique_ptr<Plugin> loadVst3(const std::string& path, const std::string& name, std::string& error);

namespace {

using namespace vst3;
using namespace vst3::Vst;

constexpr int32 MAX_EVENTS = 2048;   // notes per buffer
constexpr int32 MAX_CHANNELS = 32;   // per bus (surround plugins get silence on the channels past stereo)
constexpr int32 MAX_POINTS = 64;     // per parameter per buffer (the plugin's own changes)

#if defined(_WIN32)
constexpr const char* PLATFORM_TYPE = kPlatformTypeHWND;
#elif defined(__APPLE__)
constexpr const char* PLATFORM_TYPE = kPlatformTypeNSView;
#else
constexpr const char* PLATFORM_TYPE = kPlatformTypeX11EmbedWindowID;
#endif

// --- text: the interface's UTF-16 and our UTF-8 ---

std::string utf8(const char16* s, size_t max) {
    std::string out;
    for (size_t i = 0; i < max && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void utf16(const std::string& s, char16* out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < s.size() && n + 1 < max;) {
        unsigned char b = (unsigned char)s[i];
        uint32_t c;
        int len;
        if (b < 0x80) c = b, len = 1;
        else if ((b >> 5) == 6) c = b & 0x1F, len = 2;
        else if ((b >> 4) == 14) c = b & 0x0F, len = 3;
        else c = b & 0x07, len = 4;
        for (int k = 1; k < len && i + k < s.size(); k++)
            c = (c << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (c >= 0x10000) {
            if (n + 2 >= max)
                break;
            c -= 0x10000;
            out[n++] = (char16)(0xD800 + (c >> 10));
            out[n++] = (char16)(0xDC00 + (c & 0x3FF));
        } else {
            out[n++] = (char16)c;
        }
    }
    out[n] = 0;
}

std::string str(const char* s, size_t max) { return std::string(s, strnlen(s, max)); }

// --- counted objects of the host's ---

// queryInterface's answer for one interface I of `self`
template <class Self, class I>
bool offer(Self* self, const TUID iid, void** obj) {
    if (iidEqual(iid, I::iid)) {
        static_cast<I*>(self)->addRef();
        *obj = static_cast<I*>(self);
        return true;
    }
    return false;
}

#define TD_REFCOUNT                                                            \
    std::atomic<uint32> refs{1};                                               \
    uint32 PLUGIN_API addRef() override { return ++refs; }                     \
    uint32 PLUGIN_API release() override {                                     \
        uint32 n = --refs;                                                     \
        if (n == 0)                                                            \
            delete this;                                                       \
        return n;                                                              \
    }

// (members of a plugin, used only during a call: never deleted by their count)
#define TD_NOT_COUNTED                                       \
    uint32 PLUGIN_API addRef() override { return 1; }        \
    uint32 PLUGIN_API release() override { return 1; }

class AttributeList final : public IAttributeList {
public:
    TD_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<AttributeList, IAttributeList>(this, iid, obj) || offer<AttributeList, FUnknown>(this, iid, obj))
            return kResultOk;
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        if (!id) return kInvalidArgument;
        values[id] = Value{Value::Int, value, 0, {}};
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        auto* v = find(id, Value::Int);
        if (!v) return kResultFalse;
        value = v->i;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override {
        if (!id) return kInvalidArgument;
        values[id] = Value{Value::Float, 0, value, {}};
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        auto* v = find(id, Value::Float);
        if (!v) return kResultFalse;
        value = v->f;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar* string) override {
        if (!id || !string) return kInvalidArgument;
        size_t n = 0;
        while (string[n]) n++;
        values[id] = Value{Value::String, 0, 0, std::string((const char*)string, (n + 1) * sizeof(TChar))};
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) override {
        auto* v = find(id, Value::String);
        if (!v || !string || sizeInBytes < sizeof(TChar)) return kResultFalse;
        size_t n = std::min<size_t>(v->bytes.size(), sizeInBytes / sizeof(TChar) * sizeof(TChar));
        std::memcpy(string, v->bytes.data(), n);
        string[n / sizeof(TChar) - 1] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) override {
        if (!id) return kInvalidArgument;
        values[id] = Value{Value::Binary, 0, 0, std::string((const char*)data, data ? sizeInBytes : 0)};
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) override {
        auto* v = find(id, Value::Binary);
        if (!v) return kResultFalse;
        data = v->bytes.data();
        sizeInBytes = (uint32)v->bytes.size();
        return kResultOk;
    }

private:
    struct Value {
        enum Type { Int, Float, String, Binary } type;
        int64 i;
        double f;
        std::string bytes;
    };
    std::map<std::string, Value> values;
    Value* find(AttrID id, Value::Type type) {
        if (!id) return nullptr;
        auto it = values.find(id);
        return it != values.end() && it->second.type == type ? &it->second : nullptr;
    }
    ~AttributeList() = default;
};

class Message final : public IMessage {
public:
    TD_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<Message, IMessage>(this, iid, obj) || offer<Message, FUnknown>(this, iid, obj))
            return kResultOk;
        *obj = nullptr;
        return kNoInterface;
    }
    FIDString PLUGIN_API getMessageID() override { return hasId ? id.c_str() : nullptr; }
    void PLUGIN_API setMessageID(FIDString newId) override {
        hasId = newId != nullptr;
        id = newId ? newId : "";
    }
    IAttributeList* PLUGIN_API getAttributes() override {
        if (!attributes)
            attributes = owned<IAttributeList>(new AttributeList());
        return attributes.get();
    }

private:
    std::string id;
    bool hasId = false;
    Ptr<IAttributeList> attributes;
    ~Message() = default;
};

// the host, as plugins see it (one for the whole program, never deleted: plugins keep it)
class HostApplication final : public IHostApplication, public IPlugInterfaceSupport {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<HostApplication, IHostApplication>(this, iid, obj) ||
            offer<HostApplication, IPlugInterfaceSupport>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<FUnknown*>(static_cast<IHostApplication*>(this));
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getName(String128 name) override {
        utf16("Tiny DAW", name, 128);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (!obj)
            return kInvalidArgument;
        *obj = nullptr;
        // (the class asked for and the interface wanted: both are the interface's id)
        bool message = (cid && iidEqual(cid, IMessage::iid)) || (iid && iidEqual(iid, IMessage::iid));
        bool attributes = (cid && iidEqual(cid, IAttributeList::iid)) || (iid && iidEqual(iid, IAttributeList::iid));
        if (message) {
            *obj = static_cast<IMessage*>(new Message());
            return kResultOk;
        }
        if (attributes) {
            *obj = static_cast<IAttributeList*>(new AttributeList());
            return kResultOk;
        }
        return kResultFalse;
    }
    tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) override {
        for (const int8* known : {IComponent::iid, IAudioProcessor::iid, IEditController::iid, IConnectionPoint::iid,
                                  IPlugViewContentScaleSupport::iid})
            if (iidEqual(iid, known))
                return kResultTrue;
        return kResultFalse;
    }
};

FUnknown* hostContext() {
    static HostApplication* host = new HostApplication();
    return static_cast<IHostApplication*>(host);
}

// a stream in memory (states)
class MemoryStream final : public IBStream, public ISizeableStream {
public:
    MemoryStream() = default;
    explicit MemoryStream(const std::string& bytes) : data(bytes) {}
    TD_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<MemoryStream, IBStream>(this, iid, obj) || offer<MemoryStream, ISizeableStream>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            addRef();
            *obj = static_cast<FUnknown*>(static_cast<IBStream*>(this));
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        int64 n = std::max<int64>(0, std::min<int64>(numBytes, (int64)data.size() - pos));
        if (n > 0 && buffer)
            std::memcpy(buffer, data.data() + pos, (size_t)n);
        else
            n = 0;
        pos += n;
        if (numBytesRead)
            *numBytesRead = (int32)n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (numBytes < 0 || (!buffer && numBytes > 0)) {
            if (numBytesWritten) *numBytesWritten = 0;
            return kInvalidArgument;
        }
        if ((int64)data.size() < pos + numBytes)
            data.resize((size_t)(pos + numBytes));
        if (numBytes)
            std::memcpy(&data[(size_t)pos], buffer, (size_t)numBytes);
        pos += numBytes;
        if (numBytesWritten)
            *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 mode, int64* result) override {
        int64 to;
        switch (mode) {
            case kIBSeekSet: to = offset; break;
            case kIBSeekCur: to = pos + offset; break;
            case kIBSeekEnd: to = (int64)data.size() + offset; break;
            default: return kInvalidArgument;
        }
        pos = std::max<int64>(0, to);
        if (result)
            *result = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* p) override {
        if (!p) return kInvalidArgument;
        *p = pos;
        return kResultOk;
    }
    tresult PLUGIN_API getStreamSize(int64& size) override {
        size = (int64)data.size();
        return kResultOk;
    }
    tresult PLUGIN_API setStreamSize(int64 size) override {
        if (size < 0) return kInvalidArgument;
        data.resize((size_t)size);
        return kResultOk;
    }
    std::string data;
    int64 pos = 0;

private:
    ~MemoryStream() = default;
};

// one parameter's changes in one buffer (points sorted by frame)
class ParamValueQueue final : public IParamValueQueue {
public:
    TD_NOT_COUNTED
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<ParamValueQueue, IParamValueQueue>(this, iid, obj) || offer<ParamValueQueue, FUnknown>(this, iid, obj))
            return kResultOk;
        *obj = nullptr;
        return kNoInterface;
    }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override {
        if (index < 0 || index >= count) return kResultFalse;
        offset = points[index].offset;
        value = points[index].value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override {
        int32 at = 0;
        while (at < count && points[at].offset < offset) at++;
        if (at < count && points[at].offset == offset) {
            points[at].value = value;
            index = at;
            return kResultOk;
        }
        if (count >= MAX_POINTS) return kResultFalse;
        for (int32 i = count; i > at; i--) points[i] = points[i - 1];
        points[at] = {offset, value};
        count++;
        index = at;
        return kResultOk;
    }
    ParamID id = 0;
    int32 count = 0;

private:
    struct Point {
        int32 offset;
        ParamValue value;
    };
    Point points[MAX_POINTS];
};

class ParameterChanges final : public IParameterChanges {
public:
    TD_NOT_COUNTED
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<ParameterChanges, IParameterChanges>(this, iid, obj) || offer<ParameterChanges, FUnknown>(this, iid, obj))
            return kResultOk;
        *obj = nullptr;
        return kNoInterface;
    }
    void setMaxParameters(int32 n) {
        queues.resize((size_t)std::max(1, n));
        index.reserve(queues.size());
    }
    void clear() {
        used = 0;
        index.clear();
    }
    int32 PLUGIN_API getParameterCount() override { return used; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < used ? &queues[(size_t)index] : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& at) override {
        int32 i = index.find(id);  // (thousands of parameters a buffer: not searched one by one)
        if (i != IdIndex::NONE) {
            at = i;
            return &queues[(size_t)i];
        }
        if (used >= (int32)queues.size()) return nullptr;
        auto& q = queues[(size_t)used];
        q.id = id;
        q.count = 0;
        index.insert(id, used);
        at = used++;
        return &q;
    }

private:
    std::vector<ParamValueQueue> queues;
    IdIndex index;  // (room for all of queues: setMaxParameters)
    int32 used = 0;
};

class EventList final : public IEventList {
public:
    TD_NOT_COUNTED
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<EventList, IEventList>(this, iid, obj) || offer<EventList, FUnknown>(this, iid, obj))
            return kResultOk;
        *obj = nullptr;
        return kNoInterface;
    }
    EventList() : events((size_t)MAX_EVENTS) {}
    void clear() { count = 0; }
    int32 PLUGIN_API getEventCount() override { return count; }
    tresult PLUGIN_API getEvent(int32 index, Event& e) override {
        if (index < 0 || index >= count) return kInvalidArgument;
        e = events[(size_t)index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) override {
        if (count >= (int32)events.size()) return kResultFalse;
        events[(size_t)count++] = e;
        return kResultOk;
    }

private:
    std::vector<Event> events;
    int32 count = 0;
};

// --- modules ---

struct ClassEntry {
    char cid[16];
    std::string category, name, vendor, version, subcategories;
};

struct Module {
    std::string path;
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    Ptr<IPluginFactory> factory;
    std::vector<ClassEntry> classes;

    void* symbol(const char* name) {
#ifdef _WIN32
        return (void*)GetProcAddress(handle, name);
#else
        return dlsym(handle, name);
#endif
    }

    ~Module() {
        factory.reset();
        if (!handle)
            return;
#ifdef _WIN32
        using ExitProc = bool(PLUGIN_API*)();
        if (auto exit = (ExitProc)symbol("ExitDll"))
            exit();
        FreeLibrary(handle);
#else
        using ExitProc = bool (*)();
        if (auto exit = (ExitProc)symbol("ModuleExit"))
            exit();
        dlclose(handle);
#endif
    }
};

std::mutex modules_lock;
std::map<std::string, std::shared_ptr<Module>>& modules() {
    static auto* loaded = new std::map<std::string, std::shared_ptr<Module>>();  // (kept to the end: plugins live in them)
    return *loaded;
}

void readClasses(Module& m) {
    PFactoryInfo factoryInfo{};
    if (m.factory->getFactoryInfo(&factoryInfo) != kResultOk)
        std::memset(&factoryInfo, 0, sizeof(factoryInfo));
    std::string factoryVendor = str(factoryInfo.vendor, sizeof(factoryInfo.vendor));
    auto f2 = queryPtr<IPluginFactory2>(m.factory.get());
    auto f3 = queryPtr<IPluginFactory3>(m.factory.get());
    int32 count = m.factory->countClasses();
    for (int32 i = 0; i < count; i++) {
        ClassEntry e;
        bool got = false;
        if (f3) {
            PClassInfoW w{};
            if (f3->getClassInfoUnicode(i, &w) == kResultOk) {
                std::memcpy(e.cid, w.cid, 16);
                e.category = str(w.category, sizeof(w.category));
                e.name = utf8(w.name, PClassInfo::kNameSize);
                e.vendor = utf8(w.vendor, PClassInfo2::kVendorSize);
                e.version = utf8(w.version, PClassInfo2::kVersionSize);
                e.subcategories = str(w.subCategories, sizeof(w.subCategories));
                got = true;
            }
        }
        if (!got && f2) {
            PClassInfo2 c{};
            if (f2->getClassInfo2(i, &c) == kResultOk) {
                std::memcpy(e.cid, c.cid, 16);
                e.category = str(c.category, sizeof(c.category));
                e.name = str(c.name, sizeof(c.name));
                e.vendor = str(c.vendor, sizeof(c.vendor));
                e.version = str(c.version, sizeof(c.version));
                e.subcategories = str(c.subCategories, sizeof(c.subCategories));
                got = true;
            }
        }
        if (!got) {
            PClassInfo c{};
            if (m.factory->getClassInfo(i, &c) != kResultOk)
                continue;
            std::memcpy(e.cid, c.cid, 16);
            e.category = str(c.category, sizeof(c.category));
            e.name = str(c.name, sizeof(c.name));
        }
        if (e.vendor.empty())
            e.vendor = factoryVendor;
        m.classes.push_back(e);
    }
}


std::shared_ptr<Module> loadModule(const std::string& given, std::string& error) {
    std::string path = vst3Binary(given);
    std::lock_guard<std::mutex> hold(modules_lock);
    auto& loaded = modules();
    auto found = loaded.find(path);
    if (found != loaded.end())
        return found->second;
    auto m = std::make_shared<Module>();
    m->path = path;
#ifdef _WIN32
    m->handle = loadWindowsLibrary(path, error);
    if (!m->handle)
        return nullptr;
    using InitProc = bool(PLUGIN_API*)();
    if (auto init = (InitProc)m->symbol("InitDll"))
        if (!init()) {
            error = "Cannot start " + path + " (InitDll failed)";
            FreeLibrary(m->handle);
            m->handle = nullptr;
            return nullptr;
        }
#else
    if (!util::fileExists(path)) {
        error = "Cannot load " + given + ": no " + util::baseName(path) + " for this computer in it";
        return nullptr;
    }
    dlerror();
    m->handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!m->handle) {
        const char* why = dlerror();
        error = "Cannot load " + path + (why ? std::string(": ") + why : "");
        return nullptr;
    }
    using EntryProc = bool (*)(void*);
    if (auto entry = (EntryProc)m->symbol("ModuleEntry")) {
        if (!entry(m->handle)) {
            error = "Cannot start " + path + " (ModuleEntry failed)";
            dlclose(m->handle);
            m->handle = nullptr;
            return nullptr;
        }
    }
#endif
    auto getFactory = (GetFactoryProc)m->symbol("GetPluginFactory");
    if (!getFactory) {
        error = path + " is not a VST3 plugin (it has no GetPluginFactory)";
        return nullptr;  // (~Module: ModuleExit, unloaded)
    }
    m->factory = owned(getFactory());
    if (!m->factory) {
        error = path + " has no plugin factory";
        return nullptr;
    }
    if (auto f3 = queryPtr<IPluginFactory3>(m->factory.get()))
        f3->setHostContext(hostContext());
    readClasses(*m);
    loaded[path] = m;
    return m;
}

// --- the plugin's helpers that call back into it ---

class Vst3Plugin;

class ComponentHandler final : public IComponentHandler, public IComponentHandler2 {
public:
    explicit ComponentHandler(Vst3Plugin* p) : plugin(p) {}
    TD_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<ComponentHandler, IComponentHandler>(this, iid, obj) ||
            offer<ComponentHandler, IComponentHandler2>(this, iid, obj))
            return kResultOk;
        if (iidEqual(iid, FUnknown::iid)) {
            addRef();
            *obj = static_cast<FUnknown*>(static_cast<IComponentHandler*>(this));
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;
    tresult PLUGIN_API setDirty(TBool) override { return kResultOk; }
    tresult PLUGIN_API requestOpenEditor(FIDString) override { return kResultFalse; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }
    std::atomic<Vst3Plugin*> plugin;

private:
    ~ComponentHandler() = default;
};

// The frame around the plugin's view (it asks it to resize), and on Linux the run loop its window
// runs on (file descriptors and timers on the program's main loop).
class PlugFrame final : public IPlugFrame
#if !defined(_WIN32) && !defined(__APPLE__)
    , public Linux::IRunLoop
#endif
{
public:
    explicit PlugFrame(Vst3Plugin* p) : plugin(p) {}
    TD_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (offer<PlugFrame, IPlugFrame>(this, iid, obj))
            return kResultOk;
#if !defined(_WIN32) && !defined(__APPLE__)
        if (offer<PlugFrame, Linux::IRunLoop>(this, iid, obj))
            return kResultOk;
#endif
        if (iidEqual(iid, FUnknown::iid)) {
            addRef();
            *obj = static_cast<FUnknown*>(static_cast<IPlugFrame*>(this));
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override;
    Vst3Plugin* plugin;

#if !defined(_WIN32) && !defined(__APPLE__)
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        if (!handler)
            return kInvalidArgument;
        handler->addRef();
        onMain([this, handler, fd] {
            Linux::IEventHandler* h = handler;
            int id = plat::watchFd(fd, [h, fd] {
                h->addRef();  // (it may unregister itself meanwhile)
                h->onFDIsSet(fd);
                h->release();
            });
            fds.push_back({h, id});
        });
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        if (!handler)
            return kInvalidArgument;
        onMain([this, handler] {
            for (size_t i = 0; i < fds.size();)
                if (fds[i].handler == handler) {
                    plat::unwatchFd(fds[i].id);
                    fds[i].handler->release();
                    fds.erase(fds.begin() + (long)i);
                } else {
                    i++;
                }
        });
        return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override {
        if (!handler)
            return kInvalidArgument;
        handler->addRef();
        int interval = (int)std::max<Linux::TimerInterval>(1, std::min<Linux::TimerInterval>(ms, 1000000));
        onMain([this, handler, interval] {
            Linux::ITimerHandler* h = handler;
            int id = plat::addTimer(interval, [h] {
                h->addRef();
                h->onTimer();
                h->release();
            });
            timers.push_back({h, id});
        });
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        if (!handler)
            return kInvalidArgument;
        onMain([this, handler] {
            for (size_t i = 0; i < timers.size();)
                if (timers[i].handler == handler) {
                    plat::removeTimer(timers[i].id);
                    timers[i].handler->release();
                    timers.erase(timers.begin() + (long)i);
                } else {
                    i++;
                }
        });
        return kResultOk;
    }
#endif

    // (main thread, when the plugin goes) whatever it left registered
    void dropRegistrations() {
#if !defined(_WIN32) && !defined(__APPLE__)
        for (auto& f : fds) {
            plat::unwatchFd(f.id);
            f.handler->release();
        }
        fds.clear();
        for (auto& t : timers) {
            plat::removeTimer(t.id);
            t.handler->release();
        }
        timers.clear();
#endif
    }

private:
    template <class H>
    struct Registration {
        H* handler;
        int id;
    };
#if !defined(_WIN32) && !defined(__APPLE__)
    std::vector<Registration<Linux::IEventHandler>> fds;
    std::vector<Registration<Linux::ITimerHandler>> timers;
#endif
    // the main loop is the main thread's: a plugin registering from another thread is served there
    template <class F>
    void onMain(F fn) {
        if (plat::isMainThread()) {
            fn();
            return;
        }
        addRef();
        plat::postToMain([this, fn] {
            fn();
            release();
        });
    }
    ~PlugFrame() = default;
};

using Change = ParamValueChange;  // (plugins/param_changes.h: ChangeQueue)

// ===================================================================================== the plugin

class Vst3Plugin final : public Plugin {
public:
    Vst3Plugin(const std::string& path, const std::string& wanted) {
        std::string error;
        module = loadModule(path, error);
        if (!module)
            throw std::runtime_error(error);
        const ClassEntry* chosen = nullptr;
        for (const auto& c : module->classes) {
            if (c.category != kVstAudioEffectClass)
                continue;
            if (wanted.empty() || c.name == wanted) {
                chosen = &c;
                break;
            }
        }
        if (!chosen)
            throw std::runtime_error(wanted.empty() ? "No VST3 plugin in " + path : wanted + " is not in " + path);
        info = *chosen;
        void* obj = nullptr;
        if (module->factory->createInstance(info.cid, IComponent::iid, &obj) != kResultOk || !obj)
            throw std::runtime_error("Cannot start " + info.name);
        component = owned(static_cast<IComponent*>(obj));
        if (component->initialize(hostContext()) != kResultOk) {
            component.reset();
            throw std::runtime_error("Cannot start " + info.name);
        }
        initialized = true;
        try {
            setUp();
        } catch (...) {
            if (controller)
                controller->setComponentHandler(nullptr);
            if (handler)
                handler->plugin = nullptr;
            shutdown();
            if (frame) {
                frame->plugin = nullptr;
                frame->dropRegistrations();
            }
            throw;
        }
    }

    void setUp() {
        void* obj = nullptr;
        // its controller: the component itself (a single-component plugin), or a class of its own
        controller = queryPtr<IEditController>(component.get());
        single = (bool)controller;
        if (!controller) {
            TUID cid{};
            if (component->getControllerClassId(cid) == kResultOk) {
                obj = nullptr;
                if (module->factory->createInstance(cid, IEditController::iid, &obj) == kResultOk && obj) {
                    controller = owned(static_cast<IEditController*>(obj));
                    if (controller->initialize(hostContext()) != kResultOk)
                        controller.reset();  // (no window, no parameters: it still plays)
                }
            }
        }
        processor = queryPtr<IAudioProcessor>(component.get());
        if (!processor) {
            throw std::runtime_error(info.name + " has no audio processor");
        }
        connectDirectly();
        handler = owned(new ComponentHandler(this));
        frame = owned(new PlugFrame(this));
        if (controller)
            controller->setComponentHandler(handler.get());
        syncControllerToComponent();
        setupBuses();
        int32 params = controller ? controller->getParameterCount() : 0;
        inputChanges.setMaxParameters(params + 8);
        outputChanges.setMaxParameters(params + 8);
        pendingNotes.reserve(MAX_EVENTS);
        // room for a change of every parameter at once (a preset, automation of them all): the audio
        // thread adds to the queues and copies them into changesNow without allocating
        size_t room = std::max(ChangeQueue::ROOM, (size_t)std::max(0, params) + 8);
        toProcessor.reserve(room);
        toController.reserve(room);
        fromProcessor.reserve(room);
        changesNow.reserve(room);
        idleChanges.reserve(room);
        std::memset(&context, 0, sizeof(context));
        setTransport(120.0, 4, 4, false, 0);
        prepare(48000.0, 4096);
    }

    ~Vst3Plugin() override {
        closeEditor();
        {
            std::lock_guard<std::mutex> hold(processLock);
            deactivate();
        }
        if (controller)
            controller->setComponentHandler(nullptr);
        handler->plugin = nullptr;
        shutdown();
        frame->plugin = nullptr;
        frame->dropRegistrations();
    }

    // (the SDK's PlugProvider's order) disconnected, controller then component terminated, released
    void shutdown() {
        disconnectDirectly();
        processor.reset();
        if (controller && !single)
            controller->terminate();
        controller.reset();
        if (component && initialized)
            component->terminate();
        component.reset();
    }

    void connectDirectly() {
        auto processorEnd = queryPtr<IConnectionPoint>(component.get());
        auto controllerEnd = queryPtr<IConnectionPoint>(controller.get());
        if (!processorEnd || !controllerEnd || processorEnd.get() == controllerEnd.get())
            return;  // (a single-component plugin talks to itself)
        processorEnd->connect(controllerEnd.get());
        controllerEnd->connect(processorEnd.get());
        direct[0] = processorEnd;
        direct[1] = controllerEnd;
    }

    void disconnectDirectly() {
        if (direct[0]) {
            direct[0]->disconnect(direct[1].get());
            direct[1]->disconnect(direct[0].get());
            direct[0].reset();
            direct[1].reset();
        }
    }

    // --- facts ---

    bool isVst2() const override { return false; }
    std::string name() const override { return info.name; }
    std::string vendor() const override { return info.vendor; }
    std::string subcategories() const override { return info.subcategories; }
    bool isInstrument() const override {
        return info.subcategories.find("Instrument") != std::string::npos || (mainIn < 0 && eventInputs > 0);
    }
    bool hasSidechain() const override { return auxIn >= 0; }
    int latency() const override { return latencyFrames.load(); }
    double sampleRate() const override { return rate; }

    // --- setting up ---

    void setupBuses() {
        int32 inputs = component->getBusCount(kAudio, kInput), outputs = component->getBusCount(kAudio, kOutput);
        eventInputs = component->getBusCount(kEvent, kInput);
        std::vector<SpeakerArrangement> inArr((size_t)std::max(inputs, 0), 0), outArr((size_t)std::max(outputs, 0), 0);
        for (int32 i = 0; i < inputs; i++) {
            BusInfo bus{};
            component->getBusInfo(kAudio, kInput, i, bus);
            processor->getBusArrangement(kInput, i, inArr[(size_t)i]);
            if (bus.busType == kMain && mainIn < 0) {
                mainIn = i;
                inArr[(size_t)i] = SpeakerArr::kStereo;
            } else if (bus.busType == kAux && auxIn < 0) {
                auxIn = i;
                inArr[(size_t)i] = SpeakerArr::kStereo;
            }
        }
        for (int32 i = 0; i < outputs; i++) {
            BusInfo bus{};
            component->getBusInfo(kAudio, kOutput, i, bus);
            processor->getBusArrangement(kOutput, i, outArr[(size_t)i]);
            if (bus.busType == kMain && mainOut < 0) {
                mainOut = i;
                outArr[(size_t)i] = SpeakerArr::kStereo;
            }
        }
        if (mainOut < 0 && outputs > 0)
            mainOut = 0;
        // ask for stereo; if the plugin refuses, it keeps what it had
        processor->setBusArrangements(inArr.empty() ? nullptr : inArr.data(), inputs,
                                      outArr.empty() ? nullptr : outArr.data(), outputs);
        for (int32 i = 0; i < inputs; i++)
            component->activateBus(kAudio, kInput, i, i == mainIn || i == auxIn);
        for (int32 i = 0; i < outputs; i++)
            component->activateBus(kAudio, kOutput, i, i == mainOut);
        if (eventInputs > 0)
            component->activateBus(kEvent, kInput, 0, true);
        inBuses.assign((size_t)std::max(inputs, 0), AudioBusBuffers());
        outBuses.assign((size_t)std::max(outputs, 0), AudioBusBuffers());
        inPointers.assign(inBuses.size(), std::vector<float*>());
        outPointers.assign(outBuses.size(), std::vector<float*>());
        for (int32 i = 0; i < inputs; i++) {
            processor->getBusArrangement(kInput, i, inArr[(size_t)i]);
            inBuses[(size_t)i].numChannels = std::min(SpeakerArr::getChannelCount(inArr[(size_t)i]), MAX_CHANNELS);
            inPointers[(size_t)i].assign((size_t)inBuses[(size_t)i].numChannels, nullptr);
            inBuses[(size_t)i].channelBuffers32 = inPointers[(size_t)i].data();
        }
        for (int32 i = 0; i < outputs; i++) {
            processor->getBusArrangement(kOutput, i, outArr[(size_t)i]);
            outBuses[(size_t)i].numChannels = std::min(SpeakerArr::getChannelCount(outArr[(size_t)i]), MAX_CHANNELS);
            outPointers[(size_t)i].assign((size_t)outBuses[(size_t)i].numChannels, nullptr);
            outBuses[(size_t)i].channelBuffers32 = outPointers[(size_t)i].data();
        }
    }

    void syncControllerToComponent() {
        // a separate controller starts from the processor's settings (the VST3 way)
        if (!controller || single)
            return;
        auto stream = owned(new MemoryStream());
        if (component->getState(stream.get()) == kResultOk) {
            stream->seek(0, IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(stream.get());
        }
    }

    void deactivate() {
        if (active) {
            processor->setProcessing(false);
            component->setActive(false);
            active = false;
        }
    }

    // (under processLock) notes still held get their note-offs with the next buffer: a plugin may keep
    // its idea of the keys held across setActive(false/true) and ignore those keys until let go of
    // (FabFilter One stayed silent for them after a sample-rate change)
    void activate() {
        component->setActive(true);
        processor->setProcessing(true);
        active = true;
        latencyFrames = (int)processor->getLatencySamples();
        if (held.any())
            notesOffWanted = true;
    }

    // (main thread) the sample rate (and longest buffer) to process at; the plugin starts afresh
    void prepare(double sampleRate, int maxBlock) override {
        std::lock_guard<std::mutex> hold(processLock);
        deactivate();
        rate = sampleRate;
        blockMax = std::max(16, maxBlock);
        zeros.assign((size_t)blockMax, 0.0f);
        scratch.assign((size_t)MAX_CHANNELS, std::vector<float>((size_t)blockMax, 0.0f));
        ProcessSetup setup{kRealtime, kSample32, blockMax, sampleRate};
        if (processor->setupProcessing(setup) != kResultOk)
            throw std::runtime_error(info.name + " can't process at " + std::to_string((int)sampleRate) + " Hz");
        activate();
    }

    // forget what it was holding or playing (tails, delays, notes: their note-offs come with the next buffer)
    void reset() override {
        std::lock_guard<std::mutex> hold(processLock);
        deactivate();
        activate();
    }

    // --- processing (audio thread) ---

    void setTransport(double tempo, int num, int den, bool playing, int64_t position) override {
        context.tempo = tempo;
        context.timeSigNumerator = num;
        context.timeSigDenominator = den;
        context.projectTimeSamples = position;
        context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                        ProcessContext::kProjectTimeMusicValid | (playing ? ProcessContext::kPlaying : 0);
        context.projectTimeMusic = position / std::max(1.0, rate) * tempo / 60.0;
    }

    void process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) override {
        run(in, sidechain, out, frames, false);
    }

    void render(const std::vector<NoteMessage>& notes, float* const out[2], int frames) override {
        for (const auto& n : notes)  // (after any still waiting)
            if (pendingNotes.size() < pendingNotes.capacity())
                pendingNotes.push_back(n);
        run(nullptr, nullptr, out, frames, true);
    }

    // notes it was sent that it hasn't been sent the note-off of
    bool holding() const override { return held.any(); }

    // with the next buffer: note-offs for every note still sounding
    void notesOff() override { notesOffWanted = true; }

    void run(const float* const* inRows, const float* const* sideRows, float* const* outRows, int frames, bool instrument) {
        std::unique_lock<std::mutex> hold(processLock, std::try_to_lock);
        if (!hold.owns_lock() || !active) {  // being set up: let the sound pass (or be silent) meanwhile
            for (int c = 0; c < 2; c++)
                if (inRows && !instrument)
                    std::memmove(outRows[c], inRows[c], (size_t)frames * sizeof(float));
                else
                    std::memset(outRows[c], 0, (size_t)frames * sizeof(float));
            // (its notes wait for the next buffer, at its start: a note-off lost here would leave a note on)
            for (auto& note : pendingNotes)
                note.frame = 0;
            return;
        }
        for (int done = 0; done < frames; done += blockMax) {
            int32 n = std::min(blockMax, frames - done);
            processBlock(inRows, sideRows, outRows, done, n, instrument);
        }
        pendingNotes.clear();
    }

    void processBlock(const float* const* inRows, const float* const* sideRows, float* const* outRows, int at,
                      int32 n, bool instrument) {
        int scratchUsed = 0;
        auto spare = [&]() { return scratch[(size_t)std::min(scratchUsed++, MAX_CHANNELS - 1)].data(); };
        for (size_t b = 0; b < inBuses.size(); b++) {
            for (int32 c = 0; c < inBuses[b].numChannels; c++) {
                const float* source = nullptr;
                if ((int)b == mainIn && inRows && c < 2)
                    source = inRows[c] + at;
                else if ((int)b == auxIn && sideRows && c < 2)
                    source = sideRows[c] + at;
                inPointers[b][(size_t)c] = const_cast<float*>(source ? source : zeros.data());
            }
            inBuses[b].silenceFlags = 0;
        }
        for (size_t b = 0; b < outBuses.size(); b++) {
            for (int32 c = 0; c < outBuses[b].numChannels; c++)
                outPointers[b][(size_t)c] = ((int)b == mainOut && c < 2) ? outRows[c] + at : spare();
            outBuses[b].silenceFlags = 0;
        }
        // (a mono output: copied to the right channel afterwards)
        bool monoOut = mainOut >= 0 && outBuses[(size_t)mainOut].numChannels == 1;
        inputChanges.clear();
        outputChanges.clear();
        if (toProcessor.take(changesNow, false))
            for (auto& c : changesNow) {
                int32 index;
                if (auto* queue = inputChanges.addParameterData(c.id, index)) {
                    int32 point;
                    queue->addPoint(0, c.value, point);
                }
            }
        events.clear();
        if (instrument || eventInputs > 0) {
            if (notesOffWanted.exchange(false))
                for (int ch = 0; ch < 16; ch++)
                    for (int p = 0; p < 128; p++)
                        if (held.test((size_t)(ch * 128 + p)))
                            addNote(0, false, p, 0, ch);
            for (auto& note : pendingNotes) {
                int frame = note.frame - at;
                if (frame >= 0 && frame < n)
                    addNote(frame, note.on, note.pitch, note.velocity, note.channel);
            }
        }
        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = n;
        data.numInputs = (int32)inBuses.size();
        data.numOutputs = (int32)outBuses.size();
        data.inputs = inBuses.empty() ? nullptr : inBuses.data();
        data.outputs = outBuses.empty() ? nullptr : outBuses.data();
        data.inputParameterChanges = &inputChanges;
        data.outputParameterChanges = &outputChanges;
        data.inputEvents = &events;
        data.processContext = &context;
        context.sampleRate = rate;
        if (mainOut < 0 || processor->process(data) != kResultOk) {
            for (int c = 0; c < 2; c++)
                std::memset(outRows[c] + at, 0, (size_t)n * sizeof(float));
        } else if (monoOut) {
            std::memcpy(outRows[1] + at, outRows[0] + at, (size_t)n * sizeof(float));
        }
        if (context.state & ProcessContext::kPlaying) {
            context.projectTimeSamples += n;
            context.projectTimeMusic += n / std::max(1.0, rate) * context.tempo / 60.0;
        }
        for (int32 i = 0; i < outputChanges.getParameterCount(); i++) {  // the plugin's own changes, to its window
            auto* queue = outputChanges.getParameterData(i);
            int32 offset;
            ParamValue value;
            if (queue && queue->getPointCount() > 0 &&
                queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk)
                fromProcessor.add(queue->getParameterId(), value);
        }
    }

    void addNote(int32 frame, bool on, int pitch, int velocity, int channel) {
        Event e;
        std::memset(&e, 0, sizeof(e));
        e.busIndex = 0;
        e.sampleOffset = frame;
        channel &= 15;
        pitch &= 127;
        if (on) {
            e.type = Event::kNoteOnEvent;
            e.noteOn = {(int16)channel, (int16)pitch, 0.0f, std::max(0, std::min(127, velocity)) / 127.0f, 0, -1};
            held.set((size_t)(channel * 128 + pitch));
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff = {(int16)channel, (int16)pitch, std::max(0, std::min(127, velocity)) / 127.0f, -1, 0.0f};
            held.reset((size_t)(channel * 128 + pitch));
        }
        events.addEvent(e);
    }

    // --- parameters ---

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> result;
        if (!controller)
            return result;
        int32 count = controller->getParameterCount();
        for (int32 i = 0; i < count; i++) {
            ParameterInfo p;
            std::memset(&p, 0, sizeof(p));
            if (controller->getParameterInfo(i, p) != kResultOk)
                continue;
            ParamInfo d;
            d.id = p.id;
            d.name = utf8(p.title, 128);
            d.units = utf8(p.units, 128);
            d.stepCount = p.stepCount;
            d.defaultValue = p.defaultNormalizedValue;
            d.automatable = (p.flags & ParameterInfo::kCanAutomate) != 0;
            d.readOnly = (p.flags & ParameterInfo::kIsReadOnly) != 0;
            d.hidden = (p.flags & ParameterInfo::kIsHidden) != 0;
            d.isBypass = (p.flags & ParameterInfo::kIsBypass) != 0;
            d.isProgramChange = (p.flags & ParameterInfo::kIsProgramChange) != 0;
            result.push_back(d);
        }
        return result;
    }

    // (main thread) its value: the latest set, even if it hasn't reached the window yet
    double getParam(uint32_t id) override {
        double value;
        if (toController.find(id, value))
            return value;
        return controller ? controller->getParamNormalized(id) : 0.0;
    }

    // either thread: to the processor with the next buffer, to the window with the next idle()
    void setParam(uint32_t id, double value) override {
        value = std::max(0.0, std::min(1.0, value));
        toProcessor.add(id, value);
        toController.add(id, value);
    }

    std::string paramText(uint32_t id, double value) override {
        if (!controller)
            return "";
        String128 text{};
        if (controller->getParamStringByValue(id, value, text) != kResultOk)
            return "";
        return utf8(text, 128);
    }

    // parameters changed in the plugin's window since the last call, in order
    std::vector<uint32_t> edited() override {
        std::lock_guard<std::mutex> hold(editsLock);
        std::vector<uint32_t> result;
        result.swap(edits);
        return result;
    }

    void performed(ParamID id, ParamValue value) {  // (from the window)
        toProcessor.add(id, value);
        std::lock_guard<std::mutex> hold(editsLock);
        edits.push_back(id);
    }

    // a change as if made in the plugin's window (for tests)
    void windowEdit(uint32_t id, double value) override {
        if (controller)
            controller->setParamNormalized(id, value);
        performed(id, value);
    }

    // (main thread) values on their way to the window; a changed latency; a reload the plugin asked for
    void idle() override {
        std::vector<Change>& now = idleChanges;
        if (controller) {
            fromProcessor.take(now, true);
            for (auto& c : now)
                controller->setParamNormalized(c.id, c.value);
            toController.take(now, true);
            for (auto& c : now)
                controller->setParamNormalized(c.id, c.value);
        }
        int flags = restartFlags.exchange(0);
        if (flags & (kReloadComponent | kIoChanged | kLatencyChanged)) {
            std::lock_guard<std::mutex> hold(processLock);  // (latency is read while inactive, to be safe)
            deactivate();
            activate();
        }
    }

    // --- state ---

    std::string componentState() override { return stateOf(component.get(), nullptr); }
    std::string controllerState() override { return controller && !single ? stateOf(nullptr, controller.get()) : ""; }

    std::string stateOf(IComponent* comp, IEditController* ctrl) {
        auto stream = owned(new MemoryStream());
        tresult result = kResultFalse;
        if (comp)
            result = comp->getState(stream.get());
        else if (ctrl)
            result = ctrl->getState(stream.get());
        return result == kResultOk ? stream->data : std::string();
    }

    // (main thread) the processor's settings, then the window's (empty: leave it as it is)
    void setState(const std::string& comp, const std::string& ctrl) override {
        {
            std::lock_guard<std::mutex> hold(processLock);
            if (!comp.empty()) {
                auto stream = owned(new MemoryStream(comp));
                component->setState(stream.get());
            }
        }
        if (controller && !comp.empty()) {
            auto stream = owned(new MemoryStream(comp));
            controller->setComponentState(stream.get());
        }
        if (controller && !ctrl.empty() && !single) {
            auto stream = owned(new MemoryStream(ctrl));
            controller->setState(stream.get());
        }
        latencyFrames = (int)processor->getLatencySamples();
    }

    // --- its window (main thread) ---

    bool openEditor(uintptr_t parent, void* /*display*/, double scale, int& w, int& h) override {
        closeEditor();
        if (!controller)
            return false;
        view = owned(controller->createView(ViewType::kEditor));
        if (!view || view->isPlatformTypeSupported(PLATFORM_TYPE) != kResultTrue) {
            view.reset();
            return false;
        }
        view->setFrame(frame.get());
        if (auto scaling = queryPtr<IPlugViewContentScaleSupport>(view.get()))
            scaling->setContentScaleFactor((float)scale);
        if (view->attached((void*)parent, PLATFORM_TYPE) != kResultOk) {
            view->setFrame(nullptr);
            view.reset();
            return false;
        }
        ViewRect r;
        view->getSize(&r);
        wantedW = r.getWidth();
        wantedH = r.getHeight();
        w = wantedW;
        h = wantedH;
        editorKnown = 1;
        return true;
    }

    void closeEditor() override {
        if (view) {
            view->removed();
            view->setFrame(nullptr);
            view.reset();
        }
    }

    bool editorOpen() const override { return (bool)view; }

    bool hasEditor() override {
        if (view)
            return true;
        if (editorKnown < 0) {  // (asked once: a view made and let go without being shown)
            editorKnown = 0;
            if (controller) {
                auto probe = owned(controller->createView(ViewType::kEditor));
                editorKnown = probe && probe->isPlatformTypeSupported(PLATFORM_TYPE) == kResultTrue ? 1 : 0;
            }
        }
        return editorKnown == 1;
    }

    bool editorResizable() override { return view && view->canResize() == kResultTrue; }

    // the size the plugin asked for (it changed its own window), once
    bool requestedSize(int& w, int& h) override {
        if (!resizeWanted.exchange(false))
            return false;
        w = wantedW;
        h = wantedH;
        return true;
    }

    // the window's inside is now w x h (physical pixels); the size the plugin takes
    void editorResized(int& w, int& h) override {
        if (!view)
            return;
        ViewRect r(0, 0, w, h);
        if (view->canResize() != kResultTrue) {  // (a view of one size keeps it: told nothing)
            if (view->getSize(&r) == kResultOk && r.getWidth() > 0 && r.getHeight() > 0)
                w = r.getWidth(), h = r.getHeight();
            return;
        }
        view->checkSizeConstraint(&r);
        view->onSize(&r);
        w = r.getWidth();
        h = r.getHeight();
    }

    void pluginResized(ViewRect* size) {
        wantedW = size->getWidth();
        wantedH = size->getHeight();
        resizeWanted = true;
        if (view)
            view->onSize(size);  // (the window follows with the next requestedSize())
    }

    void restart(int32 flags) { restartFlags |= flags; }

private:
    std::shared_ptr<Module> module;
    ClassEntry info;
    Ptr<IComponent> component;
    Ptr<IAudioProcessor> processor;
    Ptr<IEditController> controller;
    bool single = false, initialized = false;
    Ptr<IConnectionPoint> direct[2];
    Ptr<ComponentHandler> handler;
    Ptr<PlugFrame> frame;
    Ptr<IPlugView> view;
    int editorKnown = -1;
    int mainIn = -1, auxIn = -1, mainOut = -1;
    int32 eventInputs = 0;
    std::vector<AudioBusBuffers> inBuses, outBuses;
    std::vector<std::vector<float*>> inPointers, outPointers;
    std::vector<float> zeros;
    std::vector<std::vector<float>> scratch;
    int blockMax = 4096;
    ParameterChanges inputChanges, outputChanges;
    EventList events;
    ProcessContext context;
    std::vector<NoteMessage> pendingNotes;
    std::bitset<16 * 128> held;
    std::atomic<bool> notesOffWanted{false};
    std::mutex processLock;
    bool active = false;
    double rate = 48000.0;
    std::atomic<int> latencyFrames{0};
    std::atomic<int> restartFlags{0};
    ChangeQueue toProcessor, toController, fromProcessor;
    std::vector<Change> changesNow;
    std::vector<Change> idleChanges;  // (main thread: idle()'s, keeping its room)
    std::mutex editsLock;
    std::vector<uint32_t> edits;
    int wantedW = 0, wantedH = 0;
    std::atomic<bool> resizeWanted{false};
};

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value) {
    if (Vst3Plugin* p = plugin.load())
        p->performed(id, value);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags) {
    if (Vst3Plugin* p = plugin.load())
        p->restart(flags);
    return kResultOk;
}

tresult PLUGIN_API PlugFrame::resizeView(IPlugView*, ViewRect* size) {
    if (!size)
        return kInvalidArgument;
    if (plugin)
        plugin->pluginResized(size);
    return kResultTrue;
}

}  // namespace

// ===================================================================================== the module's functions

std::string vst3Binary(const std::string& bundle) {
    if (!util::isDir(bundle))
        return bundle;
    std::string stem = util::stem(bundle);
#if defined(_WIN32) && defined(__i386__)
    return util::joinPath(util::joinPath(bundle, "Contents/x86-win"), stem + ".vst3");  // (the 32-bit host)
#elif defined(_WIN32)
    return util::joinPath(util::joinPath(bundle, "Contents/x86_64-win"), stem + ".vst3");
#elif defined(__APPLE__)
    return util::joinPath(util::joinPath(bundle, "Contents/MacOS"), stem);
#elif defined(__aarch64__)
    return util::joinPath(util::joinPath(bundle, "Contents/aarch64-linux"), stem + ".so");
#elif defined(__i386__)
    return util::joinPath(util::joinPath(bundle, "Contents/i386-linux"), stem + ".so");
#else
    return util::joinPath(util::joinPath(bundle, "Contents/x86_64-linux"), stem + ".so");
#endif
}

std::vector<ClassInfo> vst3Classes(const std::string& path, std::string& error) {
#ifndef _WIN32
    if (wine::isWindowsPlugin(loadableFile(path)))  // (a Windows plugin: asked through Wine)
        return bridgedClasses(loadableFile(path), error);
#endif
    std::vector<ClassInfo> result;
    auto module = loadModule(path, error);
    if (!module)
        return result;
    for (const auto& c : module->classes) {
        if (c.category != kVstAudioEffectClass)
            continue;
        ClassInfo info;
        info.name = c.name;
        info.vendor = c.vendor;
        info.subcategories = c.subcategories;
        info.instrument = c.subcategories.find("Instrument") != std::string::npos;
        info.version = c.version;
        result.push_back(info);
    }
    return result;
}

std::unique_ptr<Plugin> loadVst3(const std::string& path, const std::string& name, std::string& error) {
    try {
        return std::unique_ptr<Plugin>(new Vst3Plugin(path, name));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

int unloadUnusedModules() {
    std::vector<std::shared_ptr<Module>> going;  // (unloaded outside the lock)
    {
        std::lock_guard<std::mutex> hold(modules_lock);
        auto& loaded = modules();
        for (auto it = loaded.begin(); it != loaded.end();)
            if (it->second.use_count() == 1) {
                going.push_back(it->second);
                it = loaded.erase(it);
            } else {
                ++it;
            }
    }
    return (int)going.size();
}

}  // namespace plug

