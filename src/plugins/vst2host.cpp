// VST2 plugins in Exbit's plugin host (the port of native/vst2plugin.cpp), with the same methods
// as the VST3 ones (plugin.h), so the program treats both alike.
//
// A VST2 plugin is a shared library (Linux: a .so; Windows: a DLL) whose "VSTPluginMain" (or older
// "main") hands over the plugin's structure (vst2.h); host and plugin talk through one function each
// way. Threads as with VST3: loading, state, names and the window on the main thread, process() and
// render() on the audio thread. VST2 lets parameters be set from either thread. While the plugin is
// being reconfigured, the audio thread gets its input (or silence) rather than waiting.
//
// A plugin with four or more inputs takes its third and fourth as its sidechain (the VST2 way).
// Its settings are its "chunk" if it keeps one, else the values of all its parameters.
// Shell plugins (several plugins in one file, e.g. Waves) aren't supported.
//
// Its window on Linux: effEditOpen gets the parent X11 Window as its pointer and the Display* as
// its value (as Linux hosts pass them); effEditIdle with every idle() lets it draw.
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
#include "plugins/plugin.h"
#include "plugins/vst2.h"
#include "plugins/winlib.h"
#ifndef EXBIT_IN_HOST
#include "plugins/wine.h"
#endif

namespace plug {

std::unique_ptr<Plugin> loadVst2(const std::string& path, std::string& error);

namespace {

constexpr int MAX_EVENTS = 2048;  // notes per buffer
constexpr int MAX_CHANNELS = 32;
const char CHUNK_MAGIC[] = "VST2CHNK", PARAMS_MAGIC[] = "VST2PARM";  // (state: 8 bytes, then the data)

#ifdef _WIN32
using Library = HMODULE;
Library openLibrary(const std::string& path, std::string& error) { return loadWindowsLibrary(path, error); }
void* symbol(Library lib, const char* name) { return (void*)GetProcAddress(lib, name); }
#else
using Library = void*;
Library openLibrary(const std::string& path, std::string& error) {
    dlerror();
    void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        const char* why = dlerror();
        error = "Cannot load " + path + (why ? std::string(": ") + why : "");
    }
    return h;
}
void* symbol(Library lib, const char* name) { return dlsym(lib, name); }
#endif

std::mutex libraries_lock;
std::map<std::string, Library>& libraries() {
    static auto* loaded = new std::map<std::string, Library>();  // never unloaded: plugins live in them
    return *loaded;
}

thread_local bool on_audio_thread = false;  // (for the plugins asking which thread calls them)

class Vst2Plugin;
thread_local Vst2Plugin* being_made = nullptr;  // (a plugin calls the host before it knows it)

vst2::intptr VST2_CALL host_callback(vst2::Effect* effect, int32_t opcode, int32_t index, vst2::intptr value, void* ptr, float opt);

class Vst2Plugin final : public Plugin {
public:
    explicit Vst2Plugin(const std::string& path) : path(path) {
        Library library;
        {
            std::lock_guard<std::mutex> hold(libraries_lock);
            auto& loaded = libraries();
            auto found = loaded.find(path);
            if (found != loaded.end()) {
                library = found->second;
            } else {
                std::string error;
                library = openLibrary(path, error);
                if (!library)
                    throw std::runtime_error(error);
                loaded[path] = library;
            }
        }
        auto entry = (vst2::EntryPoint)symbol(library, "VSTPluginMain");
        if (!entry)
            entry = (vst2::EntryPoint)symbol(library, "main");
        if (!entry)
            throw std::runtime_error(path + " is not a VST2 plugin");
        directory = util::dirName(path);
        being_made = this;
        effect = entry(host_callback);
        being_made = nullptr;
        if (!effect || effect->magic != vst2::MAGIC)
            throw std::runtime_error(path + " is not a VST2 plugin it can start");
        effect->host_reserved[0] = (vst2::intptr)this;
        dispatch(vst2::effect::OPEN);
        category = (int32_t)dispatch(vst2::effect::GET_PLUG_CATEGORY);
        if (category == vst2::CATEGORY_SHELL) {
            dispatch(vst2::effect::CLOSE);
            effect = nullptr;
            throw std::runtime_error(path + " holds several plugins (a shell, like Waves): not supported yet");
        }
        plugin_name = text_of(vst2::effect::GET_EFFECT_NAME);
        if (plugin_name.empty())
            plugin_name = text_of(vst2::effect::GET_PRODUCT_STRING);
        if (plugin_name.empty())
            plugin_name = util::stem(path);
        vendor_name = text_of(vst2::effect::GET_VENDOR_STRING);
        in_pointers.assign((size_t)std::max(2, std::min(effect->inputs, MAX_CHANNELS)), nullptr);
        out_pointers.assign((size_t)std::max(2, std::min(effect->outputs, MAX_CHANNELS)), nullptr);
        midi.resize(MAX_EVENTS);
        event_list.resize(sizeof(vst2::Events) + MAX_EVENTS * sizeof(void*));
        pending_notes.reserve(MAX_EVENTS);
        std::memset(&time, 0, sizeof(time));
        set_transport_now(120.0, 4, 4, false, 0);
        prepare(48000.0, 4096);
    }

    ~Vst2Plugin() override {
        if (!effect)
            return;
        closeEditor();
        {
            std::lock_guard<std::mutex> hold(process_lock);
            suspend();
        }
        dispatch(vst2::effect::CLOSE);
        effect = nullptr;
    }

    vst2::intptr dispatch(int32_t opcode, int32_t index = 0, vst2::intptr value = 0, void* ptr = nullptr, float opt = 0) {
        return effect->dispatcher(effect, opcode, index, value, ptr, opt);
    }

    std::string text_of(int32_t opcode, int32_t index = 0) {
        char text[512] = {0};  // (plugins often write past the interface's short limits)
        dispatch(opcode, index, 0, text);
        text[sizeof(text) - 1] = 0;
        return text;
    }

    // --- facts ---

    bool isVst2() const override { return true; }
    std::string name() const override { return plugin_name; }
    std::string vendor() const override { return vendor_name; }
    std::string subcategories() const override { return isInstrument() ? "Instrument" : "Fx"; }
    bool isInstrument() const override { return (effect->flags & vst2::flag::IS_SYNTH) || category == vst2::CATEGORY_SYNTH; }
    bool hasSidechain() const override { return effect->inputs >= 4 && !isInstrument(); }
    int latency() const override { return std::max(0, (int)effect->initial_delay); }
    double sampleRate() const override { return rate; }
    int version() const { return effect->version; }

    // --- setting up ---

    void suspend() {
        if (active) {
            dispatch(vst2::effect::STOP_PROCESS);
            dispatch(vst2::effect::MAINS_CHANGED, 0, 0);
            active = false;
        }
    }

    // (under process_lock) notes still held get their note-offs with the next buffer: a plugin may keep
    // its idea of the keys held across a suspend and ignore those keys until let go of (as VST3: activate)
    void resume() {
        dispatch(vst2::effect::MAINS_CHANGED, 0, 1);
        dispatch(vst2::effect::START_PROCESS);
        active = true;
        if (held.any())
            notes_off_wanted = true;
    }

    // (main thread) the sample rate to process at; the plugin starts afresh
    void prepare(double sample_rate, int maxBlock) override {
        std::lock_guard<std::mutex> hold(process_lock);
        suspend();
        rate = sample_rate;
        block_max = std::max(16, maxBlock);
        zeros.assign((size_t)block_max, 0.0f);
        scratch.assign((size_t)MAX_CHANNELS, std::vector<float>((size_t)block_max, 0.0f));
        time.sample_rate = sample_rate;
        dispatch(vst2::effect::SET_SAMPLE_RATE, 0, 0, nullptr, (float)sample_rate);
        dispatch(vst2::effect::SET_BLOCK_SIZE, 0, block_max);
        resume();
    }

    // forget what it was holding or playing (its notes' note-offs come with the next buffer)
    void reset() override {
        std::lock_guard<std::mutex> hold(process_lock);
        suspend();
        resume();
    }

    // --- processing (audio thread) ---

    void setTransport(double tempo, int num, int den, bool playing, int64_t position, double beats = -1) override {
        set_transport_now(tempo, num, den, playing, position, beats);
    }

    void set_transport_now(double tempo, int num, int den, bool playing, int64_t position, double beats = -1) {
        time.tempo = tempo;
        time.time_sig_numerator = num;
        time.time_sig_denominator = den;
        time.sample_pos = (double)position;
        // (the song's own beats: through its tempo changes and ramps)
        time.ppq_pos = beats >= 0 ? beats : position / std::max(1.0, rate) * tempo / 60.0;
        time.flags = vst2::time_flag::TEMPO_VALID | vst2::time_flag::TIME_SIG_VALID | vst2::time_flag::PPQ_VALID |
                     (playing ? vst2::time_flag::PLAYING : 0);
    }

    void process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) override {
        run(in, sidechain, out, frames, false);
    }

    void render(const std::vector<NoteMessage>& notes, float* const out[2], int frames) override {
        for (const auto& n : notes)  // (after any still waiting)
            if (pending_notes.size() < pending_notes.capacity())
                pending_notes.push_back(n);
        run(nullptr, nullptr, out, frames, true);
    }

    bool holding() const override { return held.any(); }

    void notesOff() override { notes_off_wanted = true; }

    void run(const float* const* in_rows, const float* const* side_rows, float* const* out_rows, int frames,
             bool instrument) {
        std::unique_lock<std::mutex> hold(process_lock, std::try_to_lock);
        if (!hold.owns_lock() || !active) {
            for (int c = 0; c < 2; c++)
                if (in_rows && !instrument)
                    std::memmove(out_rows[c], in_rows[c], (size_t)frames * sizeof(float));
                else
                    std::memset(out_rows[c], 0, (size_t)frames * sizeof(float));
            // (its notes wait for the next buffer, at its start: a note-off lost here would leave a note on)
            for (auto& note : pending_notes)
                note.frame = 0;
            return;
        }
        on_audio_thread = true;
        for (int done = 0; done < frames; done += block_max) {
            int32_t n = std::min(block_max, frames - done);
            process_block(in_rows, side_rows, out_rows, done, n, instrument);
        }
        on_audio_thread = false;
        pending_notes.clear();
    }

    void process_block(const float* const* in_rows, const float* const* side_rows, float* const* out_rows, int at,
                       int32_t n, bool instrument) {
        for (size_t c = 0; c < in_pointers.size(); c++) {
            const float* source = nullptr;
            if (c < 2 && in_rows)
                source = in_rows[c] + at;
            else if (c >= 2 && c < 4 && side_rows && hasSidechain())
                source = side_rows[c - 2] + at;
            in_pointers[c] = const_cast<float*>(source ? source : zeros.data());
        }
        for (size_t c = 0; c < out_pointers.size(); c++)
            out_pointers[c] = c < 2 ? out_rows[c] + at : scratch[std::min<size_t>(c, MAX_CHANNELS - 1)].data();
        send_notes(at, n, instrument);
        if (effect->flags & vst2::flag::CAN_REPLACE) {
            effect->process_replacing(effect, in_pointers.data(), out_pointers.data(), n);
        } else {  // (old plugins add to what is there)
            for (int c = 0; c < 2; c++)
                std::memset(out_rows[c] + at, 0, (size_t)n * sizeof(float));
            effect->process_accumulating(effect, in_pointers.data(), out_pointers.data(), n);
        }
        if (effect->outputs == 1)
            std::memcpy(out_rows[1] + at, out_rows[0] + at, (size_t)n * sizeof(float));
        if (time.flags & vst2::time_flag::PLAYING) {
            time.sample_pos += n;
            time.ppq_pos += n / std::max(1.0, rate) * time.tempo / 60.0;
        }
    }

    void send_notes(int at, int32_t n, bool instrument) {
        int count = 0;
        auto* list = reinterpret_cast<vst2::Events*>(event_list.data());
        auto add = [&](int32_t frame, bool on, int pitch, int velocity, int channel) {
            if (count >= MAX_EVENTS)
                return;
            vst2::MidiEvent& e = midi[(size_t)count];
            e = vst2::MidiEvent();
            e.delta_frames = frame;
            e.midi[0] = (char)((on ? 0x90 : 0x80) | (channel & 15));
            e.midi[1] = (char)(pitch & 127);
            e.midi[2] = (char)(on ? std::max(1, velocity & 127) : 0);
            list->events[count++] = &e;
            if (on)
                held.set((size_t)((channel & 15) * 128 + (pitch & 127)));
            else
                held.reset((size_t)((channel & 15) * 128 + (pitch & 127)));
        };
        if (notes_off_wanted.exchange(false))
            for (int ch = 0; ch < 16; ch++)
                for (int p = 0; p < 128; p++)
                    if (held.test((size_t)(ch * 128 + p)))
                        add(0, false, p, 0, ch);
        // the bend wheel (0xE0, 14 bits from 0 = all the way down) and control changes (0xB0)
        auto addMidi = [&](int32_t frame, int status, int data1, int data2) {
            if (count >= MAX_EVENTS)
                return;
            vst2::MidiEvent& e = midi[(size_t)count];
            e = vst2::MidiEvent();
            e.delta_frames = frame;
            e.midi[0] = (char)status;
            e.midi[1] = (char)(data1 & 127);
            e.midi[2] = (char)(data2 & 127);
            list->events[count++] = &e;
        };
        if (instrument)
            for (auto& note : pending_notes) {
                int frame = note.frame - at;
                if (frame < 0 || frame >= n)
                    continue;
                if (note.kind == NoteMessage::PitchBend) {
                    int v = std::max(0, std::min(16383, note.value + 8192));
                    addMidi(frame, 0xE0 | (note.channel & 15), v & 127, v >> 7);
                } else if (note.kind == NoteMessage::Control) {
                    addMidi(frame, 0xB0 | (note.channel & 15), note.pitch, std::max(0, std::min(127, note.value)));
                } else {
                    add(frame, note.on, note.pitch, note.velocity, note.channel);
                }
            }
        if (count == 0)
            return;
        list->count = count;
        list->reserved = 0;
        dispatch(vst2::effect::PROCESS_EVENTS, 0, 0, list);
    }

    // --- parameters ---

    std::vector<ParamInfo> parameters() override {
        std::vector<ParamInfo> result;
        for (int32_t i = 0; i < effect->parameters; i++) {
            ParamInfo d;
            d.id = (uint32_t)i;
            d.name = text_of(vst2::effect::GET_PARAM_NAME, i);
            d.units = text_of(vst2::effect::GET_PARAM_LABEL, i);
            d.stepCount = 0;
            d.defaultValue = effect->get_parameter(effect, i);
            d.automatable = true;  // (VST2 plugins rarely say; any parameter can be set)
            result.push_back(d);
        }
        return result;
    }

    double getParam(uint32_t id) override { return in_range(id) ? effect->get_parameter(effect, (int32_t)id) : 0.0; }

    void setParam(uint32_t id, double value) override {  // (either thread: VST2 allows it)
        if (in_range(id))
            effect->set_parameter(effect, (int32_t)id, (float)std::max(0.0, std::min(1.0, value)));
    }

    // the plugin's text for a value: VST2 plugins only describe their current value, so another
    // value is given as a number
    std::string paramText(uint32_t id, double value) override {
        if (!in_range(id))
            return "";
        if (std::abs(effect->get_parameter(effect, (int32_t)id) - value) > 1e-5) {
            char number[32];
            std::snprintf(number, sizeof(number), "%.2f", value);
            return number;
        }
        std::string display = util::trim(text_of(vst2::effect::GET_PARAM_DISPLAY, (int32_t)id));
        std::string label = util::trim(text_of(vst2::effect::GET_PARAM_LABEL, (int32_t)id));
        return label.empty() ? display : display + " " + label;
    }

    bool in_range(uint32_t id) const { return id < (uint32_t)std::max(0, effect->parameters); }

    std::vector<uint32_t> edited() override {
        std::lock_guard<std::mutex> hold(edits_lock);
        std::vector<uint32_t> result;
        result.swap(edits);
        return result;
    }

    void automated(int32_t id) {  // (the plugin: a parameter was changed in its window)
        if (id < 0)
            return;
        std::lock_guard<std::mutex> hold(edits_lock);
        edits.push_back((uint32_t)id);
    }

    void windowEdit(uint32_t id, double value) override {
        setParam(id, value);
        automated((int32_t)id);
    }

    // (main thread) time for the plugin's window; a changed latency
    void idle() override {
        if (editor)
            dispatch(vst2::effect::EDIT_IDLE);
        if (io_changed.exchange(false)) {  // (initial_delay is read by latency() as it is)
            std::lock_guard<std::mutex> hold(process_lock);
            suspend();
            resume();
        }
    }

    // --- state ---

    std::string componentState() override {
        std::string data;
        if (effect->flags & vst2::flag::PROGRAM_CHUNKS) {
            void* chunk = nullptr;
            auto size = dispatch(vst2::effect::GET_CHUNK, 0, 0, &chunk);  // (0: the whole bank)
            data.assign(CHUNK_MAGIC, 8);
            if (chunk && size > 0)
                data.append((const char*)chunk, (size_t)size);
        } else {
            data.assign(PARAMS_MAGIC, 8);
            for (int32_t i = 0; i < effect->parameters; i++) {
                float v = effect->get_parameter(effect, i);
                data.append((const char*)&v, sizeof(v));
            }
        }
        return data;
    }

    std::string controllerState() override { return ""; }

    void setState(const std::string& data, const std::string& /*controller*/) override {
        if (data.size() < 8)
            return;
        std::string kind = data.substr(0, 8), body = data.substr(8);
        if (kind == CHUNK_MAGIC) {
            std::lock_guard<std::mutex> hold(process_lock);
            dispatch(vst2::effect::SET_CHUNK, 0, (vst2::intptr)body.size(), &body[0]);
        } else if (kind == PARAMS_MAGIC) {
            for (int32_t i = 0; i < effect->parameters && (size_t)(i + 1) * sizeof(float) <= body.size(); i++) {
                float v;
                std::memcpy(&v, body.data() + i * sizeof(float), sizeof(v));
                effect->set_parameter(effect, i, v);
            }
        }
    }

    // --- its window (main thread) ---

    bool openEditor(uintptr_t parent, void* display, double /*scale*/, int& w, int& h) override {
        closeEditor();
        if (!(effect->flags & vst2::flag::HAS_EDITOR))
            return false;
        vst2::Rect* rect = nullptr;
        dispatch(vst2::effect::EDIT_GET_RECT, 0, 0, &rect);  // (some plugins only know their size after opening)
#ifdef _WIN32
        (void)display;
        dispatch(vst2::effect::EDIT_OPEN, 0, 0, (void*)parent);
#else
        dispatch(vst2::effect::EDIT_OPEN, 0, (vst2::intptr)display, (void*)parent);
#endif
        rect = nullptr;
        dispatch(vst2::effect::EDIT_GET_RECT, 0, 0, &rect);
        w = h = 0;
        if (rect) {
            w = rect->right - rect->left;
            h = rect->bottom - rect->top;
        }
        editor = true;
        if (w <= 0 || h <= 0)
            w = 640, h = 400;
        wanted_w = w;
        wanted_h = h;
        return true;
    }

    void closeEditor() override {
        if (editor) {
            dispatch(vst2::effect::EDIT_CLOSE);
            editor = false;
        }
    }

    bool editorOpen() const override { return editor; }
    bool hasEditor() override { return (effect->flags & vst2::flag::HAS_EDITOR) != 0; }
    bool editorResizable() override { return false; }  // (VST2 plugins resize themselves: requestedSize)

    bool requestedSize(int& w, int& h) override {
        if (!resize_wanted.exchange(false))
            return false;
        w = wanted_w;
        h = wanted_h;
        return true;
    }

    void editorResized(int& /*w*/, int& /*h*/) override {}

    void plugin_resized(int w, int h) {
        wanted_w = w;
        wanted_h = h;
        resize_wanted = true;
    }

    // --- for the host callback ---
    vst2::TimeInfo time;
    std::atomic<bool> io_changed{false};
    std::string directory;
    double rate = 48000.0;
    int block_max = 4096;
    int32_t unique_id() const { return effect ? effect->unique_id : 0; }

private:
    std::string path, plugin_name, vendor_name;
    vst2::Effect* effect = nullptr;
    int32_t category = 0;
    bool active = false, editor = false;
    std::mutex process_lock, edits_lock;
    std::vector<float> zeros;
    std::vector<std::vector<float>> scratch;
    std::vector<float*> in_pointers, out_pointers;
    std::vector<vst2::MidiEvent> midi;
    std::vector<char> event_list;
    std::vector<NoteMessage> pending_notes;
    std::bitset<16 * 128> held;
    std::atomic<bool> notes_off_wanted{false};
    std::vector<uint32_t> edits;
    int wanted_w = 0, wanted_h = 0;
    std::atomic<bool> resize_wanted{false};
};

vst2::intptr VST2_CALL host_callback(vst2::Effect* effect, int32_t opcode, int32_t index, vst2::intptr value, void* ptr, float) {
    Vst2Plugin* plugin = effect && effect->host_reserved[0] ? (Vst2Plugin*)effect->host_reserved[0] : being_made;
    auto copy = [&](const char* text) {
        if (ptr)
            std::strcpy((char*)ptr, text);
        return (vst2::intptr)1;
    };
    switch (opcode) {
        case vst2::host::VERSION:
            return 2400;
        case vst2::host::AUTOMATE:
            if (plugin)
                plugin->automated(index);
            return 0;
        case vst2::host::CURRENT_ID:
            return plugin ? plugin->unique_id() : 0;
        case vst2::host::IDLE:
            return 1;
        case vst2::host::GET_TIME:
            return plugin ? (vst2::intptr)&plugin->time : 0;
        case vst2::host::IO_CHANGED:
            if (plugin)
                plugin->io_changed = true;
            return 1;
        case vst2::host::SIZE_WINDOW:
            if (plugin)
                plugin->plugin_resized(index, (int)value);
            return 1;
        case vst2::host::GET_SAMPLE_RATE:
            return plugin ? (vst2::intptr)plugin->rate : 48000;
        case vst2::host::GET_BLOCK_SIZE:
            return plugin ? plugin->block_max : 4096;
        case vst2::host::GET_INPUT_LATENCY:
        case vst2::host::GET_OUTPUT_LATENCY:
            return 0;
        case vst2::host::GET_CURRENT_PROCESS_LEVEL:
            return on_audio_thread ? vst2::PROCESS_LEVEL_REALTIME : vst2::PROCESS_LEVEL_USER;
        case vst2::host::GET_AUTOMATION_STATE:
            return 1;  // (off)
        case vst2::host::GET_VENDOR_STRING:
            return copy("Exbit");
        case vst2::host::GET_PRODUCT_STRING:
            return copy("Exbit");
        case vst2::host::GET_VENDOR_VERSION:
            return 1;
        case vst2::host::GET_LANGUAGE:
            return 1;  // (English)
        case vst2::host::GET_DIRECTORY:
            return plugin ? (vst2::intptr)plugin->directory.c_str() : 0;
        case vst2::host::CAN_DO: {
            static const char* yes[] = {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents",
                                        "receiveVstMidiEvent", "sizeWindow", "supplyIdle"};
            for (const char* thing : yes)
                if (ptr && std::strcmp((const char*)ptr, thing) == 0)
                    return 1;
            return 0;
        }
        case vst2::host::WANT_MIDI:
        case vst2::host::NEED_IDLE:
        case vst2::host::UPDATE_DISPLAY:
        case vst2::host::BEGIN_EDIT:
        case vst2::host::END_EDIT:
            return 1;
        default:
            return 0;
    }
}

// --- reading a file's exported symbols without running it ---

struct FileReader {
    std::ifstream f;
    explicit FileReader(const std::string& path) : f(path, std::ios::binary) {}
    bool at(uint64_t offset, void* out, size_t size) {
        f.clear();
        f.seekg((std::streamoff)offset);
        f.read((char*)out, (std::streamsize)size);
        return (size_t)f.gcount() == size;
    }
    template <class T>
    T get(uint64_t offset, bool& ok) {
        T v{};
        if (!at(offset, &v, sizeof(T)))
            ok = false;
        return v;
    }
};

bool isEntryName(const char* name) { return std::strcmp(name, "VSTPluginMain") == 0 || std::strcmp(name, "main") == 0; }

// a 64-bit ELF shared library (of this machine) whose dynamic symbols define VSTPluginMain or main
bool elfExportsVst2(const std::string& path) {
    FileReader r(path);
    if (!r.f)
        return false;
    unsigned char ident[16];
    if (!r.at(0, ident, 16) || std::memcmp(ident, "\x7f" "ELF", 4) != 0 || ident[4] != 2 || ident[5] != 1)
        return false;  // (64-bit, little-endian)
    bool ok = true;
    uint16_t type = r.get<uint16_t>(16, ok), machine = r.get<uint16_t>(18, ok);
#if defined(__aarch64__)
    const uint16_t wantedMachine = 183;
#else
    const uint16_t wantedMachine = 62;  // x86-64
#endif
    if (!ok || type != 3 || machine != wantedMachine)  // (ET_DYN)
        return false;
    uint64_t shoff = r.get<uint64_t>(0x28, ok);
    uint16_t shentsize = r.get<uint16_t>(0x3A, ok), shnum = r.get<uint16_t>(0x3C, ok);
    if (!ok || !shoff || shentsize < 64 || shnum == 0)
        return false;
    for (uint16_t i = 0; i < shnum; i++) {
        uint64_t sh = shoff + (uint64_t)i * shentsize;
        if (r.get<uint32_t>(sh + 4, ok) != 11 || !ok)  // SHT_DYNSYM
            continue;
        uint64_t symOff = r.get<uint64_t>(sh + 24, ok), symSize = r.get<uint64_t>(sh + 32, ok);
        uint32_t link = r.get<uint32_t>(sh + 40, ok);
        uint64_t entsize = r.get<uint64_t>(sh + 56, ok);
        if (!ok || link >= shnum)
            return false;
        uint64_t strSh = shoff + (uint64_t)link * shentsize;
        uint64_t strOff = r.get<uint64_t>(strSh + 24, ok), strSize = r.get<uint64_t>(strSh + 32, ok);
        if (!ok || entsize < 24)
            return false;
        uint64_t count = std::min<uint64_t>(symSize / entsize, 1000000);
        for (uint64_t k = 0; k < count; k++) {
            uint64_t sym = symOff + k * entsize;
            uint32_t nameOff = r.get<uint32_t>(sym, ok);
            unsigned char info = r.get<unsigned char>(sym + 4, ok);
            uint16_t shndx = r.get<uint16_t>(sym + 6, ok);
            if (!ok)
                return false;
            int bind = info >> 4;
            if (shndx == 0 || (bind != 1 && bind != 2) || nameOff >= strSize)  // (defined, global or weak)
                continue;
            char name[16] = {0};
            r.at(strOff + nameOff, name, std::min<uint64_t>(sizeof(name) - 1, strSize - nameOff));
            if (isEntryName(name))
                return true;
        }
    }
    return false;
}

// a Windows DLL exporting VSTPluginMain or main (read from its headers and export table): 64-bit
// (PE32+), or 32-bit (PE32, for the 32-bit Wine host)
bool peExportsVst2(const std::string& path) {
    FileReader r(path);
    if (!r.f)
        return false;
    bool ok = true;
    char mz[2];
    if (!r.at(0, mz, 2) || mz[0] != 'M' || mz[1] != 'Z')
        return false;
    uint32_t pe = r.get<uint32_t>(0x3C, ok);
    char sig[4];
    if (!ok || !r.at(pe, sig, 4) || std::memcmp(sig, "PE\0\0", 4) != 0)
        return false;
    uint16_t machine = r.get<uint16_t>(pe + 4, ok), count = r.get<uint16_t>(pe + 6, ok);
    if (!ok || (machine != 0x8664 && machine != 0x14c))
        return false;
    uint16_t optionalSize = r.get<uint16_t>(pe + 20, ok);
    uint32_t optional = pe + 24;
    uint16_t magic = r.get<uint16_t>(optional, ok);  // (0x20B: PE32+, 64-bit; 0x10B: PE32, 32-bit)
    if (!ok || (magic != 0x20B && magic != 0x10B) || (magic == 0x20B) != (machine == 0x8664))
        return false;
    uint32_t exportRva = r.get<uint32_t>(optional + (magic == 0x20B ? 112 : 96), ok);  // (the data directories' first)
    if (!ok || !exportRva)
        return false;
    struct Section { uint32_t vsize, vaddr, rawSize, rawPtr; };
    std::vector<Section> sections;
    for (int i = 0; i < count; i++) {
        uint64_t s = optional + optionalSize + 40ull * i;
        sections.push_back({r.get<uint32_t>(s + 8, ok), r.get<uint32_t>(s + 12, ok), r.get<uint32_t>(s + 16, ok),
                            r.get<uint32_t>(s + 20, ok)});
    }
    if (!ok)
        return false;
    auto offsetOf = [&](uint32_t rva, uint64_t& out) {
        for (auto& s : sections)
            if (s.vaddr <= rva && rva < s.vaddr + std::max(s.vsize, s.rawSize)) {
                out = (uint64_t)rva - s.vaddr + s.rawPtr;
                return true;
            }
        return false;
    };
    uint64_t directory, table;
    if (!offsetOf(exportRva, directory))
        return false;
    uint32_t names = r.get<uint32_t>(directory + 24, ok), namesRva = r.get<uint32_t>(directory + 32, ok);
    if (!ok || !offsetOf(namesRva, table))
        return false;
    for (uint32_t i = 0; i < std::min<uint32_t>(names, 4096); i++) {
        uint64_t at;
        uint32_t rva = r.get<uint32_t>(table + 4ull * i, ok);
        if (!ok || !offsetOf(rva, at))
            return false;
        char name[15] = {0};
        r.at(at, name, 14);
        if (isEntryName(name))
            return true;
    }
    return false;
}

}  // namespace

bool isVst2File(const std::string& path) {
#ifdef _WIN32
    (void)elfExportsVst2;
    return peExportsVst2(path);
#else
    int machine = 0;
    if (wine::isWindowsBinary(path, &machine))  // (a Windows plugin, run through Wine: 64- or 32-bit)
        return (machine == 0x8664 || machine == 0x14c) && peExportsVst2(path);
    return elfExportsVst2(path);
#endif
}

std::unique_ptr<Plugin> loadVst2(const std::string& path, std::string& error) {
    try {
        return std::unique_ptr<Plugin>(new Vst2Plugin(path));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::vector<ClassInfo> vst2Classes(const std::string& path, std::string& error) {
#ifndef EXBIT_IN_HOST
    if (wine::isWindowsPlugin(path))  // (a Windows plugin: asked through Wine; on Windows, of its own host)
        return bridgedClasses(path, error);
#endif
    std::vector<ClassInfo> result;
    Vst2Plugin* plugin = nullptr;
    try {
        plugin = new Vst2Plugin(path);
    } catch (const std::exception& e) {
        error = e.what();
        return result;
    }
    ClassInfo info;
    info.name = plugin->name();
    info.vendor = plugin->vendor();
    info.subcategories = plugin->subcategories();
    info.instrument = plugin->isInstrument();
    int v = plugin->version();
    info.version = v > 0 ? std::to_string(v) : "";
    result.push_back(info);
    delete plugin;
    return result;
}

}  // namespace plug
