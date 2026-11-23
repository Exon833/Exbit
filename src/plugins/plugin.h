// Tiny DAW's plugin host: VST3 (plugins/vst3host.cpp, on Tiny DAW's own declaration of the VST3
// interfaces in plugins/vst3_api.h, no Steinberg SDK) and VST2 (plugins/vst2host.cpp, on vst2.h).
// Both kinds have the same methods, so the rest of the program treats them alike. This is the C++
// form of the Python module `vst3host` (native/vst3host.cpp, native/vst2plugin.cpp).
//
// Threads (the VST3 rules): loading, state, parameters' names and texts, and the plugin's window
// belong to the main thread; process() and render() are called from the audio thread. Values flow
// between them through small queues: setParam() (either thread) reaches the plugin's processor with
// the next buffer and its window with the next idle(); changes made in the window reach the
// processor with the next buffer, and are listed by edited(). Call idle() now and then (every
// frame) from the main thread. While the plugin is being reconfigured (prepare(), reset(),
// setState()) the audio thread gets silence (or, for an effect, its input unchanged) rather than
// waiting.
//
// Audio is planar float: two channel pointers. Main input and output are stereo; a plugin's first
// auxiliary audio input (VST3), or its 3rd and 4th inputs (VST2), is its sidechain.
#pragma once
#include <cstdint>

// built into a plugin host (src/winehost: the Wine hosts, or tinydaw-plugin-host.exe on Windows), not the
// program: the plugin is loaded here, never passed on to another host
#if defined(TINYDAW_WINE_HOST) || defined(TINYDAW_WINE_PE) || defined(TINYDAW_PLUGIN_HOST)
#define TINYDAW_IN_HOST 1
#endif
#include <memory>
#include <string>
#include <vector>

namespace plug {

// A plugin in a VST3 file (a file can hold several), or the one in a VST2 file
struct ClassInfo {
    std::string name;
    std::string vendor;
    std::string subcategories;  // VST3: "Fx|Dynamics", "Instrument|Synth"…; VST2: "Instrument" / "Fx"
    std::string version;
    bool instrument = false;
};

// the plugins a .vst3 bundle (or file) holds, read from its factory (the module is loaded, nothing is created)
std::vector<ClassInfo> vst3Classes(const std::string& path, std::string& error);
// a VST2 plugin (created for a moment and asked), as a one-element list
std::vector<ClassInfo> vst2Classes(const std::string& path, std::string& error);
// is this file a VST2 plugin? (a shared library exporting VSTPluginMain or main, read from the file, never run)
bool isVst2File(const std::string& path);
// the .so (Windows: .vst3 dll) inside a .vst3 bundle for this machine, or the path itself if it's a file
std::string vst3Binary(const std::string& bundle);

struct ParamInfo {
    uint32_t id = 0;
    std::string name;
    std::string units;
    double defaultValue = 0;  // normalized 0..1
    int stepCount = 0;        // 0: continuous; n: n + 1 values (VST2: always 0)
    bool automatable = true;
    bool readOnly = false;
    bool hidden = false;
    bool isBypass = false;
    bool isProgramChange = false;
};

// A message for an instrument: a note on or off (kind Note: what every message was before pitch bend),
// or a pitch bend or control change. VST2 plugins get them as MIDI (0x90/0x80, 0xE0, 0xB0); VST3 ones
// get notes as note events and bends and controls as changes of the parameters the plugin maps them to
// (IMidiMapping; a plugin that maps none ignores them).
struct NoteMessage {
    enum Kind : int { Note = 0, PitchBend = 1, Control = 2 };
    int frame;      // within the buffer
    bool on;        // (Note)
    int pitch;      // (Note); Control: the control number 0..127
    int velocity;   // 0..127 (Note)
    int channel;    // 0..15
    Kind kind = Note;
    int value = 0;  // PitchBend: -8192..8191 (0: centre); Control: 0..127
};

class Plugin {
public:
    // loads a VST3 (".vst3") or VST2 (anything else) plugin; `name` picks one of a file's several
    // plugins ("" = the first audio one). nullptr and `error` if it can't.
    static std::unique_ptr<Plugin> load(const std::string& path, const std::string& name, std::string& error);
    virtual ~Plugin() = default;

    virtual bool isVst2() const = 0;
    virtual std::string name() const = 0;
    virtual std::string vendor() const = 0;
    virtual std::string subcategories() const = 0;
    virtual bool isInstrument() const = 0;
    virtual bool hasSidechain() const = 0;
    virtual int latency() const = 0;          // frames its output is behind its input
    virtual double sampleRate() const = 0;

    // (main thread, never while the audio thread processes it)
    virtual void prepare(double sampleRate, int maxBlock = 4096) = 0;
    virtual void reset() = 0;

    // (audio thread) before process()/render()
    // beats: where positionFrames is in the song, in quarter notes (the tempo map's); < 0: worked out from the tempo
    virtual void setTransport(double tempo, int numerator, int denominator, bool playing, int64_t positionFrames,
                              double beats = -1) = 0;
    // an effect: input/sidechain (sidechain may be null: silence) → output; 2 channels each; frames ≤ maxBlock
    virtual void process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) = 0;
    // an instrument: these notes (sorted by frame) into output
    virtual void render(const std::vector<NoteMessage>& notes, float* const out[2], int frames) = 0;
    virtual void notesOff() = 0;              // note-offs for every note still held, with the next buffer
    virtual bool holding() const = 0;         // some note is still held

    // --- parameters (main thread, except setParam) ---
    virtual std::vector<ParamInfo> parameters() = 0;
    virtual double getParam(uint32_t id) = 0;                  // normalized
    virtual void setParam(uint32_t id, double value) = 0;      // either thread
    virtual std::string paramText(uint32_t id, double value) = 0;  // as the plugin shows it ("+3.0 dB")
    virtual std::vector<uint32_t> edited() = 0;               // changed in its window since last asked, in order
    virtual void windowEdit(uint32_t id, double value) = 0;   // as if changed in its window (tests)
    virtual void idle() = 0;                                  // main thread, every frame

    // --- state (main thread) ---
    virtual std::string componentState() = 0;   // VST2: the chunk (or all parameters)
    virtual std::string controllerState() = 0;  // VST2: ""
    virtual void setState(const std::string& component, const std::string& controller) = 0;

    // --- its window (main thread) ---
    // embeds the plugin's view in the native window `parent` (X11 Window id / HWND); on success
    // sets w, h to the size it wants (physical pixels). `display`: the X11 Display* (or null).
    virtual bool openEditor(uintptr_t parent, void* display, double scale, int& w, int& h) = 0;
    virtual void closeEditor() = 0;
    virtual bool editorOpen() const = 0;
    virtual bool hasEditor() = 0;
    virtual bool editorResizable() = 0;
    // the size the plugin asked for (it resized its own view), once; false if none pending
    virtual bool requestedSize(int& w, int& h) = 0;
    // the window's inside is now w x h; returns (in w, h) the size the plugin takes
    virtual void editorResized(int& w, int& h) = 0;
};

// The state blob of a slot (what projects save): "TDV3" + uint32 LE length of the component state
// + component state + controller state (the Python program's format, plugins.py state_of/set_state)
std::string packState(const std::string& component, const std::string& controller);
bool unpackState(const std::string& blob, std::string& component, std::string& controller);

// Tiny DAW's built-in effects (src/effects/effects.h), whose path is "builtin:<id>": Plugin::load()
// makes them through this hook, which src/effects/effects.cpp sets when it is linked in (null: such
// a path can't be loaded).
using BuiltinFactory = std::unique_ptr<Plugin> (*)(const std::string& path, std::string& error);
extern BuiltinFactory createBuiltin;
inline bool isBuiltinPath(const std::string& path) { return path.compare(0, 8, "builtin:") == 0; }

}  // namespace plug
