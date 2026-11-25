// Exbit's built-in audio effects: the framework (daw/audio/effect.py, and the shared part of
// native/effects.cpp, its C++ twin, which is what plays).
//
// An effect is a class deriving from Effect, with a NAME and its PARAMS (Param: a key, a name, a
// range, a default, a unit; a scale for frequencies and times; or a list of choices), and:
//
//     allocate(sr, maxBlock)   (main thread) size what it holds for this sample rate (no allocation later)
//     start()                  clear what it holds (delays, envelopes); may run on the audio thread
//     changed()                parameters changed: work out coefficients (audio thread, before the next block)
//     block(x, side, y, n)     the sound: x (and side: the sidechain, or null) → y, 2 rows of n frames
//
// and, if it needs them, effectLatency() (frames it holds the sound back: delay compensation makes up
// for it), meters() ([(label, 0…1, text)] for its window: a gain reduction, a level) and curve()
// (points (0…1, -1…1) for its window: an EQ's response, a compressor's transfer curve), and an
// analyzer (a live spectrum behind the curve: effects/tap.h carries the sound to the window).
//
// Effect itself implements plug::Plugin, so the rest of the program treats built-in effects as it
// treats plugins: in slots and patch cables, automation, sidechains (hasSidechain()), delay
// compensation (latency()), freezing, saving (their state is their parameters' values as JSON,
// {"key": value, …} in their own units, the Python program's format, so projects interchange), and
// undo. They have no native window (hasEditor() false): the program shows ui/effect_window.h.
//
// Parameters are set 0…1 (setParam: the window, automation) from either thread; the effect works
// out its coefficients again before its next block. process() never allocates or locks.
//
// An effect whose input (and sidechain) and output have stayed silent for SLEEP_AFTER_S sleeps: it
// is restarted once (start(): what it held, its meters included, is cleared) and then not run at
// all, its output silent, until sound comes in again. A track's reverb costs nothing while quiet.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugins/plugin.h"

namespace effects {

constexpr double PI = 3.14159265358979323846;
constexpr double SILENT = 1e-6;  // -120 dB: what counts as silence for an effect going to sleep
constexpr double SLEEP_AFTER_S = 1.0;
constexpr const char* VENDOR = "Exbit";

// Python's round() and np.round: halves to even
inline double roundEven(double x) { return std::nearbyint(x); }
double dbToGain(double db);
double gainToDb(double gain);
// a float as Python's repr() writes it (json.dumps): "100.0", "0.1", "1e-05", "1e+16"
std::string pyFloat(double x);

// --- parameters (effect.py's Param) ---

enum class Scale { Linear, Log, Int };

struct Param {
    std::string key;   // how the effect's code names it
    std::string name;  // shown in its window and the automation chooser
    double low = 0, high = 1, def = 0;
    std::string unit;  // "dB", "Hz", "ms", "%", ":1"…
    Scale scale = Scale::Linear;
    std::vector<std::string> choices;  // names: the value is the index of the one chosen
    int decimals = -1;                 // shown after the point (-1: from the range)

    // how many steps between its lowest and highest value (0: any value), as plugins report it
    int steps() const;
    bool isToggle() const { return choices.size() == 2 && choices[0] == "Off" && choices[1] == "On"; }
    double toNormal(double value) const;  // its value as automation and the window move it: 0…1
    double fromNormal(double normal) const;
    std::string text(double value) const;  // "+3.0 dB" style: "3.0 dB", "1.50 kHz", "4.0:1", "Peak"
};

Param param(const std::string& key, const std::string& name, double low, double high, double def,
            const std::string& unit = "", Scale scale = Scale::Linear, int decimals = -1);
Param choice(const std::string& key, const std::string& name, std::vector<std::string> choices, double def = 0);
Param toggle(const std::string& key, const std::string& name, bool def = false);  // Off / On

struct Meter {
    std::string label;
    double fraction;  // 0…1
    std::string text;
};
struct GridLine {
    double pos;  // x 0…1 or y -1…1
    std::string label;
};
struct CurvePoint {
    double x, y;  // x 0…1 left to right, y -1…1 bottom to top
};

// Where the song is when a modulator (modulators.cpp) is asked for its value (audio/modulation.h):
// at the start of a piece of `frames` frames.
struct ModClock {
    bool playing = false;
    double seconds = 0;      // timeline seconds (playing), else 0
    double beats = 0;        // beats since 0 s on the tempo map (playing), else 0
    double tempo = 120;      // bpm there
    double beatsPerBar = 4;  // a bar's quarter notes there
    int frames = 0;          // the piece's length
    double sr = 48000;
};

// --- building blocks (effect.py; in double precision as scipy) ---

using Sos = std::array<double, 6>;  // b0 b1 b2 1 a1 a2
enum class Kind { Flat, Lowpass, Highpass, Peak, Lowshelf, Highshelf };
// one second-order section (RBJ's cookbook)
Sos biquad(Kind kind, double freq, double sr, double q = 0.707, double gainDb = 0.0);
// a 4th-order Linkwitz-Riley low- or high-pass (two 2nd-order Butterworths): out[0], out[1]
void linkwitzRiley(Kind kind, double freq, double sr, Sos out[2]);
// the oversampler's elliptic low-pass, scipy's ellip(8, 0.05, 90, 0.45, output="sos")
extern const Sos OVERSAMPLING_SOS[4];
// |H| of a chain of sections at `freq` (Hz)
double magnitude(const Sos* sections, int count, double freq, double sr);

// A chain of up to 8 second-order sections over stereo audio, its state kept between blocks
// (scipy's sosfilt: transposed direct form II); set() with the same number of sections keeps it.
struct Filters {
    static constexpr int MAX = 8;
    Sos sos[MAX];
    double zi[MAX][4] = {};  // per section: z0, z1 of the left, then of the right
    int count = 0;
    void set(const Sos* sections, int n);
    void set(std::initializer_list<Sos> sections);
    void reset();
    // in → out (n frames, 2 rows; in place is fine), stored as float32
    void run(const float* const* in, float* const* out, int n);
};

// the one-pole coefficient that moves 63 % of the way in `ms` milliseconds, stepping every `block` frames
double timeCoeff(double ms, double sr, double block = 1);

constexpr int ENVELOPE_BLOCK = 16;
// the most blocks a buffer of n frames has
inline int blocksFor(int n) { return n / ENVELOPE_BLOCK + 1; }

// A level follower for dynamics: the level of each 16-frame block (peak or RMS over both channels),
// its rises followed at the attack's speed and its falls at the release's; dB.
struct Envelope {
    double level = -120.0;
    // out: room for blocksFor(n) values; returns the block size; *count the number of blocks
    double follow(const float* const* x, int n, double attackMs, double releaseMs, double sr, bool rms,
                  double* out, int* count);
};

// block values (one every `size` frames) as one value per frame: straight lines from `last` (np.interp)
void perFrame(const double* values, int count, double size, int n, double last, float* out);

// a compressor's gain change (dB, 0 or less) for a level (dB), easing in over a soft knee
double compression(double levelDb, double threshold, double ratio, double knee);

// a stereo delay of up to `longest` frames, read at any (fractional) delay per frame and channel
struct FractionalDelay {
    std::vector<std::array<float, 2>> history;  // the last h frames, oldest at `head`
    size_t head = 0;
    void allocate(double longest);  // (main thread)
    void clear();
    float read(int j, int c, double delay, const float* const* x, int frames) const;
    void write(const float* const* x, int frames);

private:
    float at(size_t index, int c, const float* const* x) const;
};

// stereo working buffers, allocated in allocate()
struct Rows {
    std::vector<float> data[2];
    float* row[2] = {nullptr, nullptr};
    void allocate(int n);
    float* const* rows() { return row; }
};

// --- the effect ---

class Effect : public plug::Plugin {
public:
    Effect(std::string id, std::string name, std::vector<Param> params, bool sidechain = false);

    const std::string& id() const { return id_; }  // "eq"…: projects save it as "builtin:<id>"
    std::string path() const;                       // "builtin:<id>"
    const std::vector<Param>& params() const { return params_; }
    double value(int index) const { return values_[index].load(std::memory_order_relaxed); }  // in its own units
    double value(const std::string& key) const;
    int key(const char* name) const;  // a parameter's index by key (throws if none)

    // its window's extras
    virtual std::vector<Meter> meters() const { return {}; }
    virtual bool hasCurve() const { return false; }
    virtual std::vector<CurvePoint> curve() const { return {}; }
    std::vector<GridLine> curveXGrid, curveYGrid;
    // a live spectrum behind its curve (the EQ's analyzer): the ways it can listen ({"Off", "Pre",
    // "Post"}; none: no analyzer), the one chosen (kept by the instance, not saved: it changes no
    // sound), and the spectrum now as `count` points (x 0…1 as the curve's, y -1…1), worked out on
    // the caller's thread (the window's, as it repaints) from what the audio thread copied for it;
    // false: nothing to show. `dt`: seconds since the last call (the levels fall back slowly).
    virtual std::vector<std::string> analyzerChoices() const { return {}; }
    virtual int analyzerChoice() const { return 0; }
    virtual void setAnalyzerChoice(int) {}
    virtual bool analyzer(int count, double dt, std::vector<CurvePoint>& out) { return false; }

    // A modulator (LFO, Level Follower, Random, Macro: modulators.cpp) moves other effects' and
    // instruments' parameters through modulation cables (audio/modulation.h) instead of changing the
    // sound: it lets nothing through (silence out) and gives a value for each piece of a buffer.
    // modulation(): -1…1 for a two-way one (LFO, Random), 0…1 for a one-way one (Level Follower,
    // Macro, and an LFO set to one way). Audio thread (one per track's processing at a time).
    virtual bool isModulator() const { return false; }
    virtual double modulation(const ModClock& clock) { return 0; }
    // its value as modulation() last gave it (any thread: its window, the patch panel), -1…1
    double lastModulation() const { return lastModulation_.load(std::memory_order_relaxed); }

    // --- plug::Plugin ---
    bool isVst2() const override { return false; }
    std::string name() const override { return name_; }
    std::string vendor() const override { return VENDOR; }
    std::string subcategories() const override { return "Fx"; }
    bool isInstrument() const override { return false; }
    bool hasSidechain() const override { return sidechain_; }
    int latency() const override { return effectLatency(); }
    double sampleRate() const override { return sr; }
    void prepare(double sampleRate, int maxBlock = 4096) override;
    void reset() override;
    void setTransport(double tempo, int numerator, int denominator, bool playing, int64_t positionFrames,
                      double beats = -1) override;
    void process(const float* const in[2], const float* const sidechain[2], float* const out[2], int frames) override;
    void render(const std::vector<plug::NoteMessage>& notes, float* const out[2], int frames) override;
    void notesOff() override {}
    bool holding() const override { return false; }
    std::vector<plug::ParamInfo> parameters() override;
    double getParam(uint32_t id) override;
    void setParam(uint32_t id, double value) override;
    std::string paramText(uint32_t id, double value) override;
    std::vector<uint32_t> edited() override;
    void windowEdit(uint32_t id, double value) override;
    void idle() override {}
    std::string componentState() override;
    std::string controllerState() override { return ""; }
    void setState(const std::string& component, const std::string& controller) override;
    bool openEditor(uintptr_t parent, void* display, double scale, int& w, int& h) override { return false; }
    void closeEditor() override {}
    bool editorOpen() const override { return false; }
    bool hasEditor() override { return false; }
    bool editorResizable() override { return false; }
    bool requestedSize(int& w, int& h) override { return false; }
    void editorResized(int& w, int& h) override {}

    bool asleep() const { return quiet_ >= sleepAfter(); }
    int maxBlock() const { return maxBlock_; }

protected:
    std::atomic<double> lastModulation_{0};
    // --- for the effects to fill in ---
    virtual void allocate(double rate, int maxBlock) {}
    virtual void start() = 0;
    virtual void changed() {}
    virtual void block(const float* const* x, const float* const* side, float* const* y, int n) = 0;
    virtual int effectLatency() const { return 0; }
    // frames of silence in and out after which it sleeps: SLEEP_AFTER_S, and longer for an effect
    // that still holds sound it hasn't let out (a delay's echo due later than that)
    virtual double sleepAfter() const { return SLEEP_AFTER_S * sr; }

    double v(int index) const { return value(index); }
    double tempo() const { return tempo_.load(std::memory_order_relaxed); }
    // the transport, as setTransport() last told it: playing or stopped, a bar's length in beats
    // (quarter notes: 4 in 4/4, 3 in 6/8), and the timeline frame where this block starts (it moves
    // on by itself, block after block, while playing and no new position is told)
    bool playing() const { return playing_.load(std::memory_order_relaxed); }
    double beatsPerBar() const;
    int64_t position() const { return position_.load(std::memory_order_relaxed); }
    double sr = 48000.0;

private:
    void chunk(const float* const in[2], const float* const side[2], float* const out[2], int n);
    static bool silent(const float* const* x, int n);

    std::string id_, name_;
    std::vector<Param> params_;
    bool sidechain_;
    std::vector<std::atomic<double>> normals_, values_;
    std::atomic<bool> changed_{true};
    std::atomic<double> tempo_{120.0};
    std::atomic<int> numerator_{4}, denominator_{4};
    std::atomic<bool> playing_{false};
    std::atomic<int64_t> position_{0};
    int64_t quiet_ = 0;  // frames its input and output have been silent for
    int maxBlock_ = 0;
    Rows in_, side_;  // (the input copied: process() may be given the same buffers in and out)
    std::vector<uint32_t> edits_;  // changed in its window since edited() was last asked (main thread)
};

}  // namespace effects
