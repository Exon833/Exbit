// The VST 3 plugin binary interface, as far as Tiny DAW needs it to host plugins (and as far as its
// test plugin needs it to be one).
//
// Declared here from the interface's public behaviour, for interoperability: the COM-style
// interfaces plugins and hosts call each other through (their identifiers, the order of their
// methods, which fixes the layout of their tables of virtual functions), and the structures they
// pass (laid out as the plugins built with Steinberg's SDK expect them). It is not Steinberg's VST 3
// SDK and holds none of its code; only what is needed for hosting is here. The names are the
// interface's own, so code reads like any other VST3 host or plugin.
//
// ABI notes (what makes plugins built with the SDK work with this):
//   - every interface is a class with pure virtual methods only, no destructor, no data, in the
//     interface's exact order, deriving from its base interface (single inheritance), so its table
//     of virtual functions is laid out as the SDK's;
//   - structures use natural alignment on Linux and macOS (the SDK sets no packing there) and on
//     64-bit Windows (#pragma pack(16) changes nothing); the static_asserts below check the sizes;
//   - an interface identifier (TUID) is 16 bytes: on Windows in COM's GUID order (the first word and
//     the two halves of the second little-endian), elsewhere the four words big-endian in order;
//   - result codes are COM's on Windows and small numbers elsewhere (kNoInterface = -1);
//   - on 32-bit Windows calls are __stdcall (PLUGIN_API).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#define PLUGIN_API __stdcall
#else
#define PLUGIN_API
#endif

// 8-byte members (double, int64) of the structures plugins share: 32-bit Windows plugins (MSVC, the
// SDK's #pragma pack(8)) align them to 8, GCC's i386 ABI to 4; so on 32-bit Windows (the 32-bit Wine
// host) they are aligned by hand. Elsewhere their natural alignment is already 8: nothing changes.
#if defined(_WIN32) && defined(__i386__)
#define VST3_ALIGN8 alignas(8)
#else
#define VST3_ALIGN8
#endif

namespace vst3 {

// --- basic types ---
using int8 = char;
using uint8 = unsigned char;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
using TSize = int64;
using tresult = int32;
using TBool = uint8;
using char8 = char;
using char16 = char16_t;
using TChar = char16;
using FIDString = const char8*;
using CString = const char8*;
using String128 = TChar[128];
using TUID = int8[16];

#ifdef _WIN32
enum : tresult {
    kNoInterface = (tresult)0x80004002L,
    kResultOk = 0,
    kResultTrue = kResultOk,
    kResultFalse = 1,
    kInvalidArgument = (tresult)0x80070057L,
    kNotImplemented = (tresult)0x80004001L,
    kInternalError = (tresult)0x80004005L,
    kNotInitialized = (tresult)0x8000FFFFL,
    kOutOfMemory = (tresult)0x8007000EL,
};
#else
enum : tresult {
    kNoInterface = -1,
    kResultOk = 0,
    kResultTrue = kResultOk,
    kResultFalse = 1,
    kInvalidArgument = 2,
    kNotImplemented = 3,
    kInternalError = 4,
    kNotInitialized = 5,
    kOutOfMemory = 6,
};
#endif

// an identifier from its four 32-bit words, as written in the interface's declarations
#ifdef _WIN32
#define VST3_UID(l1, l2, l3, l4)                                                                       \
    {                                                                                                  \
        (vst3::int8)((l1) & 0xFF), (vst3::int8)(((l1) >> 8) & 0xFF), (vst3::int8)(((l1) >> 16) & 0xFF), \
        (vst3::int8)(((l1) >> 24) & 0xFF), (vst3::int8)(((l2) >> 16) & 0xFF), (vst3::int8)(((l2) >> 24) & 0xFF), \
        (vst3::int8)((l2) & 0xFF), (vst3::int8)(((l2) >> 8) & 0xFF), (vst3::int8)(((l3) >> 24) & 0xFF), \
        (vst3::int8)(((l3) >> 16) & 0xFF), (vst3::int8)(((l3) >> 8) & 0xFF), (vst3::int8)((l3) & 0xFF),  \
        (vst3::int8)(((l4) >> 24) & 0xFF), (vst3::int8)(((l4) >> 16) & 0xFF), (vst3::int8)(((l4) >> 8) & 0xFF), \
        (vst3::int8)((l4) & 0xFF)                                                                      \
    }
#else
#define VST3_UID(l1, l2, l3, l4)                                                                       \
    {                                                                                                  \
        (vst3::int8)(((l1) >> 24) & 0xFF), (vst3::int8)(((l1) >> 16) & 0xFF), (vst3::int8)(((l1) >> 8) & 0xFF), \
        (vst3::int8)((l1) & 0xFF), (vst3::int8)(((l2) >> 24) & 0xFF), (vst3::int8)(((l2) >> 16) & 0xFF), \
        (vst3::int8)(((l2) >> 8) & 0xFF), (vst3::int8)((l2) & 0xFF), (vst3::int8)(((l3) >> 24) & 0xFF), \
        (vst3::int8)(((l3) >> 16) & 0xFF), (vst3::int8)(((l3) >> 8) & 0xFF), (vst3::int8)((l3) & 0xFF),  \
        (vst3::int8)(((l4) >> 24) & 0xFF), (vst3::int8)(((l4) >> 16) & 0xFF), (vst3::int8)(((l4) >> 8) & 0xFF), \
        (vst3::int8)((l4) & 0xFF)                                                                      \
    }
#endif

#define VST3_IID(l1, l2, l3, l4) static constexpr TUID iid = VST3_UID(l1, l2, l3, l4)

inline bool iidEqual(const void* a, const void* b) { return std::memcmp(a, b, 16) == 0; }

// ===================================================================================== base

class FUnknown {
public:
    virtual tresult PLUGIN_API queryInterface(const TUID iid, void** obj) = 0;
    virtual uint32 PLUGIN_API addRef() = 0;
    virtual uint32 PLUGIN_API release() = 0;
    VST3_IID(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
};

class IPluginBase : public FUnknown {
public:
    virtual tresult PLUGIN_API initialize(FUnknown* context) = 0;
    virtual tresult PLUGIN_API terminate() = 0;
    VST3_IID(0x22888DDB, 0x156E45AE, 0x8358B348, 0x08190625);
};

// --- the factory: GetPluginFactory() of the module ---
struct PFactoryInfo {
    enum FactoryFlags { kNoFlags = 0, kClassesDiscardable = 1 << 0, kLicenseCheck = 1 << 1,
                        kComponentNonDiscardable = 1 << 3, kUnicode = 1 << 4 };
    enum { kURLSize = 256, kEmailSize = 128, kNameSize = 64 };
    char8 vendor[kNameSize];
    char8 url[kURLSize];
    char8 email[kEmailSize];
    int32 flags;
};

struct PClassInfo {
    enum ClassCardinality { kManyInstances = 0x7FFFFFFF };
    enum { kCategorySize = 32, kNameSize = 64 };
    TUID cid;
    int32 cardinality;
    char8 category[kCategorySize];
    char8 name[kNameSize];
};

struct PClassInfo2 {
    enum { kVendorSize = 64, kVersionSize = 64, kSubCategoriesSize = 128 };
    TUID cid;
    int32 cardinality;
    char8 category[PClassInfo::kCategorySize];
    char8 name[PClassInfo::kNameSize];
    uint32 classFlags;
    char8 subCategories[kSubCategoriesSize];
    char8 vendor[kVendorSize];
    char8 version[kVersionSize];
    char8 sdkVersion[kVersionSize];
};

struct PClassInfoW {
    TUID cid;
    int32 cardinality;
    char8 category[PClassInfo::kCategorySize];
    char16 name[PClassInfo::kNameSize];
    uint32 classFlags;
    char8 subCategories[PClassInfo2::kSubCategoriesSize];
    char16 vendor[PClassInfo2::kVendorSize];
    char16 version[PClassInfo2::kVersionSize];
    char16 sdkVersion[PClassInfo2::kVersionSize];
};

class IPluginFactory : public FUnknown {
public:
    virtual tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32 PLUGIN_API countClasses() = 0;
    virtual tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) = 0;
    virtual tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) = 0;
    VST3_IID(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
};

class IPluginFactory2 : public IPluginFactory {
public:
    virtual tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) = 0;
    VST3_IID(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
};

class IPluginFactory3 : public IPluginFactory2 {
public:
    virtual tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) = 0;
    virtual tresult PLUGIN_API setHostContext(FUnknown* context) = 0;
    VST3_IID(0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
};

using GetFactoryProc = IPluginFactory*(PLUGIN_API*)();

// --- streams ---
class IBStream : public FUnknown {
public:
    enum IStreamSeekMode { kIBSeekSet = 0, kIBSeekCur, kIBSeekEnd };
    virtual tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead = nullptr) = 0;
    virtual tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten = nullptr) = 0;
    virtual tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result = nullptr) = 0;
    virtual tresult PLUGIN_API tell(int64* pos) = 0;
    VST3_IID(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
};

class ISizeableStream : public FUnknown {
public:
    virtual tresult PLUGIN_API getStreamSize(int64& size) = 0;
    virtual tresult PLUGIN_API setStreamSize(int64 size) = 0;
    VST3_IID(0x04F9549E, 0xE02F4E6E, 0x87E86A87, 0x47F4E17F);
};

// ===================================================================================== Vst

namespace Vst {

using ParamID = uint32;
using ParamValue = double;
using SampleRate = double;
using Sample32 = float;
using Sample64 = double;
using TSamples = int64;
using TQuarterNotes = double;
using SpeakerArrangement = uint64;
using Speaker = uint64;
using MediaType = int32;
using BusDirection = int32;
using BusType = int32;
using IoMode = int32;
using UnitID = int32;
using ProgramListID = int32;
using KnobMode = int32;
using NoteExpressionTypeID = uint32;

constexpr const char* kVstAudioEffectClass = "Audio Module Class";
constexpr const char* kVstComponentControllerClass = "Component Controller Class";

enum MediaTypes { kAudio = 0, kEvent, kNumMediaTypes };
enum BusDirections { kInput = 0, kOutput };
enum BusTypes { kMain = 0, kAux };
enum IoModes { kSimple = 0, kAdvanced, kOfflineProcessing };
enum ProcessModes { kRealtime = 0, kPrefetch, kOffline };
enum SymbolicSampleSizes { kSample32 = 0, kSample64 };
enum ComponentFlags { kDistributable = 1 << 0, kSimpleModeSupported = 1 << 1 };

enum RestartFlags {
    kReloadComponent = 1 << 0,
    kIoChanged = 1 << 1,
    kParamValuesChanged = 1 << 2,
    kLatencyChanged = 1 << 3,
    kParamTitlesChanged = 1 << 4,
    kMidiCCAssignmentChanged = 1 << 5,
    kNoteExpressionChanged = 1 << 6,
    kIoTitlesChanged = 1 << 7,
    kPrefetchableSupportChanged = 1 << 8,
    kRoutingInfoChanged = 1 << 9,
    kKeyswitchChanged = 1 << 10,
    kParamIDMappingChanged = 1 << 11,
};

constexpr uint32 kNoTail = 0;
constexpr uint32 kInfiniteTail = 0xFFFFFFFFu;

namespace SpeakerArr {
constexpr Speaker kSpeakerL = 1 << 0, kSpeakerR = 1 << 1, kSpeakerC = 1 << 2, kSpeakerLfe = 1 << 3;
constexpr Speaker kSpeakerM = 1 << 19;
constexpr SpeakerArrangement kEmpty = 0, kMono = kSpeakerM, kStereo = kSpeakerL | kSpeakerR;
inline int32 getChannelCount(SpeakerArrangement arr) {
    int32 n = 0;
    while (arr) {
        n += (int32)(arr & 1);
        arr >>= 1;
    }
    return n;
}
}  // namespace SpeakerArr

namespace ViewType {
constexpr const char* kEditor = "editor";
}

struct BusInfo {
    enum BusFlags { kDefaultActive = 1 << 0, kIsControlVoltage = 1 << 1 };
    MediaType mediaType;
    BusDirection direction;
    int32 channelCount;
    String128 name;
    BusType busType;
    uint32 flags;
};

struct RoutingInfo {
    MediaType mediaType;
    int32 busIndex;
    int32 channel;
};

class IComponent : public IPluginBase {
public:
    virtual tresult PLUGIN_API getControllerClassId(TUID classId) = 0;
    virtual tresult PLUGIN_API setIoMode(IoMode mode) = 0;
    virtual int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) = 0;
    virtual tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) = 0;
    virtual tresult PLUGIN_API getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) = 0;
    virtual tresult PLUGIN_API setActive(TBool state) = 0;
    virtual tresult PLUGIN_API setState(IBStream* state) = 0;
    virtual tresult PLUGIN_API getState(IBStream* state) = 0;
    VST3_IID(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
};

// --- processing ---
struct ProcessSetup {
    int32 processMode;
    int32 symbolicSampleSize;
    int32 maxSamplesPerBlock;
    VST3_ALIGN8 SampleRate sampleRate;
};

struct AudioBusBuffers {
    int32 numChannels = 0;
    VST3_ALIGN8 uint64 silenceFlags = 0;
    union {
        Sample32** channelBuffers32;
        Sample64** channelBuffers64;
    };
    AudioBusBuffers() : channelBuffers32(nullptr) {}
};

struct Chord {
    uint8 keyNote;
    uint8 rootNote;
    int16 chordMask;
};

struct FrameRate {
    uint32 framesPerSecond;
    uint32 flags;
};

struct ProcessContext {
    enum StatesAndFlags {
        kPlaying = 1 << 1,
        kCycleActive = 1 << 2,
        kRecording = 1 << 3,
        kSystemTimeValid = 1 << 8,
        kContTimeValid = 1 << 17,
        kProjectTimeMusicValid = 1 << 9,
        kBarPositionValid = 1 << 11,
        kCycleValid = 1 << 12,
        kTempoValid = 1 << 10,
        kTimeSigValid = 1 << 13,
        kChordValid = 1 << 18,
        kSmpteValid = 1 << 14,
        kClockValid = 1 << 15,
    };
    uint32 state;
    VST3_ALIGN8 double sampleRate;
    VST3_ALIGN8 TSamples projectTimeSamples;
    VST3_ALIGN8 int64 systemTime;
    VST3_ALIGN8 TSamples continousTimeSamples;
    VST3_ALIGN8 TQuarterNotes projectTimeMusic;
    VST3_ALIGN8 TQuarterNotes barPositionMusic;
    VST3_ALIGN8 TQuarterNotes cycleStartMusic;
    VST3_ALIGN8 TQuarterNotes cycleEndMusic;
    VST3_ALIGN8 double tempo;
    int32 timeSigNumerator;
    int32 timeSigDenominator;
    Chord chord;
    int32 smpteOffsetSubframes;
    FrameRate frameRate;
    int32 samplesToNextClock;
};

// --- events (notes) ---
struct NoteOnEvent {
    int16 channel;
    int16 pitch;
    float tuning;
    float velocity;
    int32 length;
    int32 noteId;
};

struct NoteOffEvent {
    int16 channel;
    int16 pitch;
    float velocity;
    int32 noteId;
    float tuning;
};

struct DataEvent {
    enum DataTypes { kMidiSysEx = 0 };
    uint32 size;
    uint32 type;
    const uint8* bytes;
};

struct PolyPressureEvent {
    int16 channel;
    int16 pitch;
    float pressure;
    int32 noteId;
};

struct NoteExpressionValueEvent {
    NoteExpressionTypeID typeId;
    int32 noteId;
    VST3_ALIGN8 double value;
};

struct NoteExpressionTextEvent {
    NoteExpressionTypeID typeId;
    int32 noteId;
    uint32 textLen;
    const TChar* text;
};

struct ChordEvent {
    int16 root;
    int16 bassNote;
    int16 mask;
    uint16 textLen;
    const TChar* text;
};

struct ScaleEvent {
    int16 root;
    int16 mask;
    uint16 textLen;
    const TChar* text;
};

struct LegacyMIDICCOutEvent {
    uint8 controlNumber;
    int8 channel;
    int8 value;
    int8 value2;
};

struct Event {
    enum EventFlags { kIsLive = 1 << 0, kUserReserved1 = 1 << 14, kUserReserved2 = 1 << 15 };
    enum EventTypes {
        kNoteOnEvent = 0,
        kNoteOffEvent = 1,
        kDataEvent = 2,
        kPolyPressureEvent = 3,
        kNoteExpressionValueEvent = 4,
        kNoteExpressionTextEvent = 5,
        kChordEvent = 6,
        kScaleEvent = 7,
        kLegacyMIDICCOutEvent = 65535,
    };
    int32 busIndex;
    int32 sampleOffset;
    VST3_ALIGN8 TQuarterNotes ppqPosition;
    uint16 flags;
    uint16 type;
    union {
        NoteOnEvent noteOn;
        NoteOffEvent noteOff;
        DataEvent data;
        PolyPressureEvent polyPressure;
        NoteExpressionValueEvent noteExpressionValue;
        NoteExpressionTextEvent noteExpressionText;
        ChordEvent chord;
        ScaleEvent scale;
        LegacyMIDICCOutEvent midiCCOut;
    };
};

class IEventList : public FUnknown {
public:
    virtual int32 PLUGIN_API getEventCount() = 0;
    virtual tresult PLUGIN_API getEvent(int32 index, Event& e) = 0;
    virtual tresult PLUGIN_API addEvent(Event& e) = 0;
    VST3_IID(0x3A2C4214, 0x346349FE, 0xB2C4F397, 0xB9695A44);
};

// --- parameter changes, with each buffer ---
class IParamValueQueue : public FUnknown {
public:
    virtual ParamID PLUGIN_API getParameterId() = 0;
    virtual int32 PLUGIN_API getPointCount() = 0;
    virtual tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) = 0;
    virtual tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) = 0;
    VST3_IID(0x01263A18, 0xED074F6F, 0x98C9D356, 0x4686F9BA);
};

class IParameterChanges : public FUnknown {
public:
    virtual int32 PLUGIN_API getParameterCount() = 0;
    virtual IParamValueQueue* PLUGIN_API getParameterData(int32 index) = 0;
    virtual IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) = 0;
    VST3_IID(0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D);
};

struct ProcessData {
    int32 processMode = kRealtime;
    int32 symbolicSampleSize = kSample32;
    int32 numSamples = 0;
    int32 numInputs = 0;
    int32 numOutputs = 0;
    AudioBusBuffers* inputs = nullptr;
    AudioBusBuffers* outputs = nullptr;
    IParameterChanges* inputParameterChanges = nullptr;
    IParameterChanges* outputParameterChanges = nullptr;
    IEventList* inputEvents = nullptr;
    IEventList* outputEvents = nullptr;
    ProcessContext* processContext = nullptr;
};

class IAudioProcessor : public FUnknown {
public:
    virtual tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                  SpeakerArrangement* outputs, int32 numOuts) = 0;
    virtual tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) = 0;
    virtual tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) = 0;
    virtual uint32 PLUGIN_API getLatencySamples() = 0;
    virtual tresult PLUGIN_API setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult PLUGIN_API setProcessing(TBool state) = 0;
    virtual tresult PLUGIN_API process(ProcessData& data) = 0;
    virtual uint32 PLUGIN_API getTailSamples() = 0;
    VST3_IID(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
};

class IProcessContextRequirements : public FUnknown {
public:
    enum Flags {
        kNeedSystemTime = 1 << 0, kNeedContinousTimeSamples = 1 << 1, kNeedProjectTimeMusic = 1 << 2,
        kNeedBarPositionMusic = 1 << 3, kNeedCycleMusic = 1 << 4, kNeedSamplesToNextClock = 1 << 5,
        kNeedTempo = 1 << 6, kNeedTimeSignature = 1 << 7, kNeedChord = 1 << 8, kNeedFrameRate = 1 << 9,
        kNeedTransportState = 1 << 10,
    };
    virtual uint32 PLUGIN_API getProcessContextRequirements() = 0;
    VST3_IID(0x2A654303, 0xEF764E3D, 0x95B5FE83, 0x730EF6D0);
};

// --- messages between a plugin's processor and its controller ---
class IAttributeList : public FUnknown {
public:
    using AttrID = const char*;
    virtual tresult PLUGIN_API setInt(AttrID id, int64 value) = 0;
    virtual tresult PLUGIN_API getInt(AttrID id, int64& value) = 0;
    virtual tresult PLUGIN_API setFloat(AttrID id, double value) = 0;
    virtual tresult PLUGIN_API getFloat(AttrID id, double& value) = 0;
    virtual tresult PLUGIN_API setString(AttrID id, const TChar* string) = 0;
    virtual tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) = 0;
    virtual tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) = 0;
    virtual tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) = 0;
    VST3_IID(0x1E5F0AEB, 0xCC7F4533, 0xA2544011, 0x38AE5BF6);
};

class IStreamAttributes : public FUnknown {
public:
    virtual tresult PLUGIN_API getFileName(String128 name) = 0;
    virtual IAttributeList* PLUGIN_API getAttributes() = 0;
    VST3_IID(0xD6CE2FFC, 0xEFAF4B8C, 0x9E74F1BB, 0x12DA44B4);
};

class IMessage : public FUnknown {
public:
    virtual FIDString PLUGIN_API getMessageID() = 0;
    virtual void PLUGIN_API setMessageID(FIDString id) = 0;
    virtual IAttributeList* PLUGIN_API getAttributes() = 0;
    VST3_IID(0x936F033B, 0xC6C047DB, 0xBB0882F8, 0x13C1E613);
};

class IConnectionPoint : public FUnknown {
public:
    virtual tresult PLUGIN_API connect(IConnectionPoint* other) = 0;
    virtual tresult PLUGIN_API disconnect(IConnectionPoint* other) = 0;
    virtual tresult PLUGIN_API notify(IMessage* message) = 0;
    VST3_IID(0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1);
};

// --- the host ---
class IHostApplication : public FUnknown {
public:
    virtual tresult PLUGIN_API getName(String128 name) = 0;
    virtual tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) = 0;
    VST3_IID(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
};

class IPlugInterfaceSupport : public FUnknown {
public:
    virtual tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) = 0;
    VST3_IID(0x4FB58B9E, 0x9EAA4E0F, 0xAB361C1C, 0xCCB56FEA);
};

// --- the controller (parameters, the window) ---
struct ParameterInfo {
    enum ParameterFlags {
        kNoFlags = 0,
        kCanAutomate = 1 << 0,
        kIsReadOnly = 1 << 1,
        kIsWrapAround = 1 << 2,
        kIsList = 1 << 3,
        kIsHidden = 1 << 4,
        kIsProgramChange = 1 << 15,
        kIsBypass = 1 << 16,
    };
    ParamID id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32 stepCount;
    VST3_ALIGN8 ParamValue defaultNormalizedValue;
    UnitID unitId;
    int32 flags;
};

class IComponentHandler : public FUnknown {
public:
    virtual tresult PLUGIN_API beginEdit(ParamID id) = 0;
    virtual tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult PLUGIN_API endEdit(ParamID id) = 0;
    virtual tresult PLUGIN_API restartComponent(int32 flags) = 0;
    VST3_IID(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
};

class IComponentHandler2 : public FUnknown {
public:
    virtual tresult PLUGIN_API setDirty(TBool state) = 0;
    virtual tresult PLUGIN_API requestOpenEditor(FIDString name = ViewType::kEditor) = 0;
    virtual tresult PLUGIN_API startGroupEdit() = 0;
    virtual tresult PLUGIN_API finishGroupEdit() = 0;
    VST3_IID(0xF040B4B3, 0xA36045EC, 0xABCDC045, 0xB4D5A2CD);
};

}  // namespace Vst

class IPlugView;

namespace Vst {

class IEditController : public IPluginBase {
public:
    virtual tresult PLUGIN_API setComponentState(IBStream* state) = 0;
    virtual tresult PLUGIN_API setState(IBStream* state) = 0;
    virtual tresult PLUGIN_API getState(IBStream* state) = 0;
    virtual int32 PLUGIN_API getParameterCount() = 0;
    virtual tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo& info) = 0;
    virtual tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) = 0;
    virtual tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) = 0;
    virtual ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) = 0;
    virtual ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plainValue) = 0;
    virtual ParamValue PLUGIN_API getParamNormalized(ParamID id) = 0;
    virtual tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* PLUGIN_API createView(FIDString name) = 0;
    VST3_IID(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
};

class IEditController2 : public FUnknown {
public:
    enum KnobModes { kCircularMode = 0, kRelativCircularMode, kLinearMode };
    virtual tresult PLUGIN_API setKnobMode(KnobMode mode) = 0;
    virtual tresult PLUGIN_API openHelp(TBool onlyCheck) = 0;
    virtual tresult PLUGIN_API openAboutBox(TBool onlyCheck) = 0;
    VST3_IID(0x7F4EFE59, 0xF3204967, 0xAC27A3AE, 0xAFB63038);
};

// MIDI controllers a plugin takes as parameters (VST3 has no control-change or pitch-bend events):
// the controller asked which parameter a controller of a channel moves
using CtrlNumber = int16;
enum ControllerNumbers : CtrlNumber {
    kCtrlModWheel = 1,
    kAfterTouch = 128,  // (beyond MIDI's 0..127 control numbers)
    kPitchBend = 129,
    kCountCtrlNumber
};

class IMidiMapping : public FUnknown {
public:
    virtual tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel, CtrlNumber midiControllerNumber,
                                                           ParamID& id) = 0;
    VST3_IID(0xDF0FF9F7, 0x49B74669, 0xB63AB732, 0x7ADBF5E5);
};

struct UnitInfo {
    UnitID id;
    UnitID parentUnitId;
    String128 name;
    ProgramListID programListId;
};

struct ProgramListInfo {
    ProgramListID id;
    String128 name;
    int32 programCount;
};

class IUnitInfo : public FUnknown {
public:
    virtual int32 PLUGIN_API getUnitCount() = 0;
    virtual tresult PLUGIN_API getUnitInfo(int32 unitIndex, UnitInfo& info) = 0;
    virtual int32 PLUGIN_API getProgramListCount() = 0;
    virtual tresult PLUGIN_API getProgramListInfo(int32 listIndex, ProgramListInfo& info) = 0;
    virtual tresult PLUGIN_API getProgramName(ProgramListID listId, int32 programIndex, String128 name) = 0;
    virtual tresult PLUGIN_API getProgramInfo(ProgramListID listId, int32 programIndex, CString attributeId,
                                              String128 attributeValue) = 0;
    virtual tresult PLUGIN_API hasProgramPitchNames(ProgramListID listId, int32 programIndex) = 0;
    virtual tresult PLUGIN_API getProgramPitchName(ProgramListID listId, int32 programIndex, int16 midiPitch,
                                                   String128 name) = 0;
    virtual UnitID PLUGIN_API getSelectedUnit() = 0;
    virtual tresult PLUGIN_API selectUnit(UnitID unitId) = 0;
    virtual tresult PLUGIN_API getUnitByBus(MediaType type, BusDirection dir, int32 busIndex, int32 channel,
                                            UnitID& unitId) = 0;
    virtual tresult PLUGIN_API setUnitProgramData(int32 listOrUnitId, int32 programIndex, IBStream* data) = 0;
    VST3_IID(0x3D4BD6B5, 0x913A4FD2, 0xA886E768, 0xA5EB92C1);
};

}  // namespace Vst

// ===================================================================================== the window

constexpr const char* kPlatformTypeHWND = "HWND";
constexpr const char* kPlatformTypeHIView = "HIView";
constexpr const char* kPlatformTypeNSView = "NSView";
constexpr const char* kPlatformTypeUIView = "UIView";
constexpr const char* kPlatformTypeX11EmbedWindowID = "X11EmbedWindowID";

struct ViewRect {
    int32 left = 0, top = 0, right = 0, bottom = 0;
    ViewRect() = default;
    ViewRect(int32 l, int32 t, int32 r, int32 b) : left(l), top(t), right(r), bottom(b) {}
    int32 getWidth() const { return right - left; }
    int32 getHeight() const { return bottom - top; }
};

class IPlugFrame;

class IPlugView : public FUnknown {
public:
    virtual tresult PLUGIN_API isPlatformTypeSupported(FIDString type) = 0;
    virtual tresult PLUGIN_API attached(void* parent, FIDString type) = 0;
    virtual tresult PLUGIN_API removed() = 0;
    virtual tresult PLUGIN_API onWheel(float distance) = 0;
    virtual tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult PLUGIN_API getSize(ViewRect* size) = 0;
    virtual tresult PLUGIN_API onSize(ViewRect* newSize) = 0;
    virtual tresult PLUGIN_API onFocus(TBool state) = 0;
    virtual tresult PLUGIN_API setFrame(IPlugFrame* frame) = 0;
    virtual tresult PLUGIN_API canResize() = 0;
    virtual tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) = 0;
    VST3_IID(0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
};

class IPlugFrame : public FUnknown {
public:
    virtual tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) = 0;
    VST3_IID(0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
};

class IPlugViewContentScaleSupport : public FUnknown {
public:
    using ScaleFactor = float;
    virtual tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) = 0;
    VST3_IID(0x65ED9690, 0x8AC44525, 0x8AADEF7A, 0x72EA703F);
};

// Linux: the host's event loop, which a plugin's window runs on (asked of the IPlugFrame)
namespace Linux {

using FileDescriptor = int;
using TimerInterval = uint64;

class IEventHandler : public FUnknown {
public:
    virtual void PLUGIN_API onFDIsSet(FileDescriptor fd) = 0;
    VST3_IID(0x561E65C9, 0x13A0496F, 0x813A2C35, 0x654D7983);
};

class ITimerHandler : public FUnknown {
public:
    virtual void PLUGIN_API onTimer() = 0;
    VST3_IID(0x10BDD94F, 0x41424774, 0x821FAD8F, 0xECA72CA9);
};

class IRunLoop : public FUnknown {
public:
    virtual tresult PLUGIN_API registerEventHandler(IEventHandler* handler, FileDescriptor fd) = 0;
    virtual tresult PLUGIN_API unregisterEventHandler(IEventHandler* handler) = 0;
    virtual tresult PLUGIN_API registerTimer(ITimerHandler* handler, TimerInterval milliseconds) = 0;
    virtual tresult PLUGIN_API unregisterTimer(ITimerHandler* handler) = 0;
    VST3_IID(0x18C35366, 0x97764F1A, 0x9C5B8385, 0x7A871389);
};

}  // namespace Linux

// ===================================================================================== helpers (not part of the interface)

// asks `unknown` for interface I (a new reference, or null)
template <class I>
I* query(FUnknown* unknown) {
    void* obj = nullptr;
    if (unknown && unknown->queryInterface(I::iid, &obj) == kResultOk && obj)
        return static_cast<I*>(obj);
    return nullptr;
}

// a counted reference, as the SDK's IPtr
template <class T>
class Ptr {
public:
    Ptr() = default;
    Ptr(T* p, bool addRef) : p_(p) {
        if (p_ && addRef)
            p_->addRef();
    }
    Ptr(const Ptr& o) : p_(o.p_) {
        if (p_)
            p_->addRef();
    }
    Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ptr() { reset(); }
    Ptr& operator=(const Ptr& o) {
        if (o.p_)
            o.p_->addRef();
        reset();
        p_ = o.p_;
        return *this;
    }
    Ptr& operator=(Ptr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p_) {
            T* p = p_;
            p_ = nullptr;
            p->release();
        }
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const Ptr& o) const { return p_ == o.p_; }

private:
    T* p_ = nullptr;
};

template <class T>
Ptr<T> owned(T* p) { return Ptr<T>(p, false); }   // takes over a reference already counted
template <class T>
Ptr<T> shared(T* p) { return Ptr<T>(p, true); }   // adds one
template <class I>
Ptr<I> queryPtr(FUnknown* unknown) { return owned(query<I>(unknown)); }

// --- checks of the layouts plugins expect (64-bit) ---
static_assert(sizeof(TUID) == 16, "TUID");
static_assert(sizeof(PFactoryInfo) == 452, "PFactoryInfo");
static_assert(sizeof(PClassInfo) == 116, "PClassInfo");
static_assert(sizeof(PClassInfo2) == 440, "PClassInfo2");
static_assert(sizeof(PClassInfoW) == 696, "PClassInfoW");
static_assert(sizeof(Vst::BusInfo) == 276, "BusInfo");
static_assert(sizeof(Vst::RoutingInfo) == 12, "RoutingInfo");
static_assert(sizeof(ViewRect) == 16, "ViewRect");
#if UINTPTR_MAX > 0xFFFFFFFFu
static_assert(sizeof(Vst::ParameterInfo) == 792, "ParameterInfo");
static_assert(offsetof(Vst::ParameterInfo, defaultNormalizedValue) == 776, "ParameterInfo");
static_assert(sizeof(Vst::ProcessSetup) == 24, "ProcessSetup");
static_assert(sizeof(Vst::AudioBusBuffers) == 24, "AudioBusBuffers");
static_assert(sizeof(Vst::ProcessData) == 80, "ProcessData");
static_assert(sizeof(Vst::ProcessContext) == 112, "ProcessContext");
static_assert(offsetof(Vst::ProcessContext, tempo) == 72, "ProcessContext");
static_assert(sizeof(Vst::Event) == 48, "Event");
static_assert(offsetof(Vst::Event, type) == 18 && offsetof(Vst::Event, noteOn) == 24, "Event");
static_assert(sizeof(Vst::NoteOnEvent) == 20 && sizeof(Vst::NoteOffEvent) == 16, "note events");
#elif defined(_WIN32) && defined(__i386__)
// (32-bit Windows, as plugins built with MSVC lay them out: pointers of 4 bytes, 8-byte members at 8)
static_assert(sizeof(Vst::ParameterInfo) == 792, "ParameterInfo");
static_assert(offsetof(Vst::ParameterInfo, defaultNormalizedValue) == 776, "ParameterInfo");
static_assert(sizeof(Vst::ProcessSetup) == 24 && offsetof(Vst::ProcessSetup, sampleRate) == 16, "ProcessSetup");
static_assert(sizeof(Vst::AudioBusBuffers) == 24 && offsetof(Vst::AudioBusBuffers, silenceFlags) == 8, "AudioBusBuffers");
static_assert(sizeof(Vst::ProcessData) == 48, "ProcessData");
static_assert(sizeof(Vst::ProcessContext) == 112, "ProcessContext");
static_assert(offsetof(Vst::ProcessContext, tempo) == 72, "ProcessContext");
static_assert(sizeof(Vst::Event) == 48, "Event");
static_assert(offsetof(Vst::Event, type) == 18 && offsetof(Vst::Event, noteOn) == 24, "Event");
static_assert(sizeof(Vst::NoteOnEvent) == 20 && sizeof(Vst::NoteOffEvent) == 16, "note events");
#endif

}  // namespace vst3
