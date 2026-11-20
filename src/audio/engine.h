// The audio output stream and the transport (play/pause, position): engine.py.
//
// The stream can use any driver type devices.h offers (ALSA, PulseAudio). callback() runs on the
// driver's thread; everything else on the GUI thread. The stream uses every output channel the
// device has; each track plays on its own pair. The callback mixes, runs instruments and effects
// (mixer.h, plugin_audio.h), and for the meters hands copies of what played to `metering`, which
// measures on the GUI thread (metering.h). While looping (`loop`), playback jumps from the loop's
// end back to its start, exactly at that frame (a buffer that crosses it is mixed in two parts).
//
// Recording (startRecording / finishRecording): with ALSA the stream also carries the device's
// inputs (a duplex stream, so inputs and outputs come together; the Python program's ASIO case),
// and the callback keeps the input channels being recorded (a Recorder) for every buffer that
// plays. PulseAudio records from its default input, opened for the take only. A count-in first
// plays clicks while the transport waits, then playback and recording start on the same frame.
// While recording, the loop is ignored (a take is one stretch of time). JACK records through a second
// client on its default input, opened for the take (as PulseAudio). A take is placed earlier by the
// device's input and output delay (and plugin delay compensation), and by the recording offset of the
// device in use (recordOffsetMs: Options → Recording offset, record_offset.h), which corrects what the
// driver reports wrong: Recorder::audio() gives the timeline frame with all of them taken off.
//
// RECORDING SEVERAL TRACKS (ui/recording.h): startRecording can take a take for each of several tracks at once
// (RecordInput: its track and its own input channels), a Recorder each, all begun on the same frame; addRecorder /
// removeRecorders punch them in or out while recording. The set is replaced as a whole (the audio thread never
// waits for it); each Recorder keeps its take in blocks made on the GUI thread (idle() collects them all).
// With loopRecording, playback goes round the loop while recording, audio or MIDI.
// MONITORING (setMonitoring, mixer.h): the inputs heard through the tracks that monitor them: a duplex stream's
// own; with other drivers the default input, opened for it, its buffers handed over through a ring.
//
// MIDI recording (startMidiRecording / finishMidiRecording): playback from pos without looping; the GUI
// keeps the notes in a MidiTake (every note where it was heard as it was played: heardNow()), and
// finish() ends the notes still held where recording stopped. Every take, audio or MIDI, is kept with
// its track too (keepTake: Track::takes, for comping).
// It also plays the Explorer's sample preview (preview()), on outputs 1/2, playing or not, and the
// metronome while playing (setMetronome(), metronome.h), after the master meters so it never counts
// towards the master's level or LUFS.
//
// MIDI out (audio/midi_out.h): with setMidiOut(), every buffer also hands its parts to the MIDI scheduler (clock,
// Start/Stop, External synth notes, timed from this buffer) and, while monitoring, the inputs heard (Measure). An
// External synth's returning sound is heard through MONITORING's input path, whatever the driver (mixer.h).
//
// What the GUI must do:
//   - call idle() now and then (every frame): frees what the audio thread let go of, drains a
//     take being recorded into its Recorder, and publishes the tempo map (project.timing.publish());
//   - call patch::ensureStates(project) after adding tracks or changing cables, effects or racks;
//   - replace PluginSlot::instance with std::atomic_store; process plugins on the GUI thread
//     (renders, syncState) only inside an Engine::PluginsPaused.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio/cpu.h"
#include "audio/devices.h"
#include "audio/live.h"
#include "audio/metering.h"
#include "audio/metronome.h"
#include "audio/midi_out.h"
#include "audio/mixer.h"
#include "model/model.h"

namespace audio {

constexpr double LATE_FACTOR = 1.5;  // falling this many buffers behind real time (at least) counts as a likely dropout
constexpr double CLOCK_REANCHOR_S = 10.0;

// A take being recorded: the input `channels` (0-based) of the device, as they come (audio thread,
// through a lock-free ring; collect() on the GUI thread moves them into the take). `start` is the
// timeline frame it began at; `late` how many frames the input arrives after what was heard (the
// device's input and output delay plus plugin delay compensation), so the take is moved that much
// earlier. The take is kept in blocks of BLOCK samples, each made on the GUI thread as it fills: it grows
// without ever copying what it holds (an hour stays an hour's worth of memory), and the audio thread only
// ever writes into the ring, made with the Recorder.
class Recorder {
public:
    static constexpr int64_t NONE = INT64_MIN;
    static constexpr size_t BLOCK = (size_t)1 << 16;  // samples a block of the take
    // track: the track it records onto (RECORDING SEVERAL TRACKS: Engine::startRecording; null: the one take)
    Recorder(std::vector<int> channels, double ioLatencyS, int sr, double ringSeconds = 20.0, double offsetMs = 0.0,
             const Track* track = nullptr);
    const std::vector<int> channels;
    const Track* const track;
    std::atomic<int64_t> start{NONE};
    const int64_t ioFrames;
    const double offsetMs;       // the recording offset it was placed with (ms; positive: earlier)
    const int64_t offsetFrames;
    std::atomic<int64_t> late;
    std::atomic<int64_t> dropped{0};  // frames lost because the GUI didn't collect() for that long

    void begin(int64_t start, int latency);                    // playback reached the take's first frame (audio thread)
    void take(const float* in, int inChannels, int frames);    // a buffer of the device's inputs (audio thread)
    void collect();                                            // GUI thread
    // (timeline frame, audio interleaved with channels.size() per frame) of the take so far (GUI thread): its
    // start less `late` and the recording offset (may be negative: then what came in before 0 s is to be dropped)
    int64_t audio(std::vector<float>& out);
    int64_t timelineStart() const;  // that frame alone
    int64_t frames() const { return channels.empty() ? 0 : (int64_t)(size_ / channels.size()); }  // collected so far
    // frames [from, to) of the take collected so far, as stereo (a mono input on both sides), into out
    void stereo(int64_t from, int64_t to, std::vector<float>& out) const;
    size_t memoryBytes() const;  // the ring and the blocks (tests: what an hour costs)
    size_t ringSamples() const { return ring_.size(); }

private:
    std::vector<float> ring_;
    std::atomic<uint64_t> head_{0}, tail_{0};
    std::vector<std::unique_ptr<float[]>> blocks_;
    size_t size_ = 0;  // samples in the blocks
    float at(size_t i) const { return blocks_[i / BLOCK][i % BLOCK]; }
};

// RECORDING SEVERAL TRACKS: one track's take: its track and the input channels it records (0-based)
struct RecordInput {
    const Track* track = nullptr;
    std::vector<int> channels;
};

// A sample being previewed: stereo audio at the engine rate and how far it has played; with
// `loop` (which the GUI may switch while it plays) it starts over at the end.
struct PreviewVoice {
    AudioPtr data;
    float gain = 1.0f;
    std::atomic<int64_t> pos{0};
    std::atomic<bool> loop{false};
};

struct LoopRange {
    int64_t start = 0, end = 0;  // frames
};

// The notes of a MIDI take as they are played (GUI thread; ui/recording.py's midi dict): each where it
// was heard (Engine::heardNow), never before the take's start; notes played during the count-in start
// where the recording does, and notes still held at the end end there (finish()).
class MidiTake {
public:
    explicit MidiTake(int64_t startFrame = 0, int sr = 48000) : start(startFrame), sr(sr) {}
    int64_t start;  // timeline frame
    int sr;
    // a note-on (velocity > 0) or note-off heard at timeline frame `heard` (< 0 / unknown: the start)
    void event(bool on, int pitch, int velocity, double heard);
    // the take's notes (seconds from its start, sorted) and its length (s): from the start to `endHeard`
    // (the frame heard when it stopped) or the last note's end. Empty if nothing was played.
    std::vector<Note> finish(double endHeard, double& lengthS);
    size_t count() const { return notes_.size() + held_.size(); }

private:
    std::vector<Note> notes_;                          // absolute seconds
    std::map<int, std::pair<double, int>> held_;       // pitch -> (on s, velocity)
};

// keep a copy of the take as recorded with the track's takes (for comping); from the second one on they
// show under the track (true: showTakes was just switched on, so the lanes changed)
bool keepTake(Track& track, const ClipPtr& clip);

class Engine {
public:
    explicit Engine(Project& project);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Project& project;
    int sr = 48000;                // the device's sample rate (FALLBACK_SAMPLE_RATE until one is open)
    int channels = 2;              // output channels of the open device
    int blocksize = 0;             // frames per buffer asked of the driver; 0 = the driver's own setting
    std::string driverType;        // "ALSA", "PulseAudio", once open
    std::string deviceId;
    bool exclusive = false;
    int cores = 0;                 // processor cores the tracks are worked out on: 0 = all (automatic), 1 = the
                                   // audio thread alone (mixer.h, SEVERAL CORES); takes effect when a device opens
    std::atomic<int64_t> pos{0};   // playhead, in frames (seek() to move it)
    std::atomic<bool> playing{false};
    int inputs = 0;                // input channels on the stream (duplex); 0: none (record from the default input)
    std::atomic<int64_t> countIn{0};  // frames the transport still waits (count-in) after play(); then it starts
    std::atomic<double> masterGain{1.0};
    std::atomic<bool> masterMute{false};
    std::shared_ptr<Metering> metering;  // replaced when a device opens
    CpuMeter cpu;
    TrackCpu trackCpu;             // each track's share of a core (cpu.h)
    std::atomic<int> latency{0};   // frames the sound lags behind pos (plugin delay compensation), set by the callback
    std::atomic<bool> pluginsPaused{false};
    std::atomic<bool> recordingMidi{false};  // recording MIDI (the GUI keeps the notes): playback doesn't loop
    std::atomic<bool> loopRecording{false};  // recording audio goes on looping (a take each pass: ui/recording.h)
    double recordOffsetMs = 0;     // the device's recording offset (record_offset::offsetMs), for new audio takes
    bool offline = false;          // tests: play() works without a device (callback() is called by hand)

    // the loop, (start, end) frames while looping; replaced as a whole by the GUI thread
    void setLoop(int64_t start, int64_t end);
    void clearLoop();
    std::optional<LoopRange> loop() const;

    // Open a device at its own sample rate with all its outputs. blocksize: frames per buffer
    // (bigger = less CPU, more delay); < 0 keeps the current choice. exclusive: take the device for
    // this program alone where the driver can. False and `error` if it can't.
    bool open(const std::string& driverType, const std::string& device, int blocksize, bool exclusive,
              std::string& error);
    void close();
    bool isOpen() const { return (bool)stream_; }
    std::string deviceName() const;
    double outputLatency() const;  // seconds
    // How many input channels can be recorded: the stream's (duplex), else the driver type's default
    // input device's (0 if there is none).
    int inputCount() const;

    // Play from pos and record the input `channels` (0-based) from there, after `countIn` frames during
    // which `clicks` (the count-in, stereo) play. False and `error` if the input can't be opened.
    bool startRecording(const std::vector<int>& channels, int64_t countIn, AudioPtr clicks, std::string& error);
    // RECORDING SEVERAL TRACKS: the same with a take for each of `takes` at once, each from its own inputs
    bool startRecording(const std::vector<RecordInput>& takes, int64_t countIn, AudioPtr clicks, std::string& error);
    // Stop playback and recording; returns the Recorder (null if not recording), all collected.
    std::shared_ptr<Recorder> finishRecording();
    std::vector<std::shared_ptr<Recorder>> finishRecordings();  // (every take's, in the order they began)
    // While recording: a take for one more track from here (punching in; null if not recording or the input
    // can't be had), or these tracks' takes ended here (punching out, all at once: their Recorders, all collected)
    std::shared_ptr<Recorder> addRecorder(const RecordInput& take, std::string& error);
    std::vector<std::shared_ptr<Recorder>> removeRecorders(const std::vector<const Track*>& tracks);
    bool recordingAudio() const { return (bool)std::atomic_load(&recorders_); }
    // MONITORING (mixer.h): some track hears its input (the GUI says so whenever that changes). Drivers without
    // a duplex stream open their default input for it (as for recording) and keep it open while it is wanted.
    // False and `error` if it can't be heard (no device, or no input).
    bool setMonitoring(bool on, std::string& error);
    bool monitoring() const { return monitoring_.load(); }
    // (GUI thread) something besides the tracks wants the input heard (an External synth being measured:
    // ui/midi_ports_dialog.h); ui/recording keeps monitoring on meanwhile
    bool inputWanted = false;
    // Play from pos for recording MIDI (the GUI keeps the notes: MidiTake), after `countIn` frames of
    // `clicks`. It doesn't loop meanwhile. False and `error` if no device is open (or it plays already).
    bool startMidiRecording(int64_t countIn, AudioPtr clicks, std::string& error);
    void finishMidiRecording();
    // (GUI thread) the timeline frame being heard at this moment while playing (false while stopped or
    // counting in): the last buffer's start, plus the time since it was made, less the device's output
    // delay and the plugins' delay (so a note played along with what is heard lands where it was heard)
    bool heardNow(double& frame) const;
    std::shared_ptr<Recorder> recorder() const;  // the first take being recorded (null: none)
    // every take being recorded (null: none), and the tracks they record onto (in the same order)
    struct RecordSet {
        std::vector<std::shared_ptr<Recorder>> recorders;
        std::vector<const Track*> tracks;
    };
    std::shared_ptr<const RecordSet> recorders() const { return std::atomic_load(&recorders_); }

    void seek(int64_t frame);
    // Where what is heard now is on the timeline: plugins with look-ahead hold the sound back
    // (`latency`), so while playing it lags behind pos. The playback cursor shows this.
    int64_t heardPos() const;
    void play();
    void stop();  // the playback cursor stays where the sound stopped

    // Play `data` (stereo, engine rate) on outputs 1/2 from frame `start`, over whatever else plays:
    // once, or over and over (loop). gain: PREVIEW_GAIN for the Explorer.
    void preview(AudioPtr data, int64_t start = 0, bool loop = false, double gain = 0.7);
    void stopPreview();
    std::shared_ptr<PreviewVoice> previewVoice() const { return std::atomic_load(&preview_); }

    // the LiveNotes that play into `track` (made the first time)
    std::shared_ptr<live::LiveNotes> liveNotes(const Track* track);

    // Switch the metronome on with these sounds, or off (nullopt).
    void setMetronome(const std::optional<metronome::MetronomeSettings>& settings);
    const std::optional<metronome::MetronomeSettings>& metronomeSettings() const { return metronome_; }

    // MIDI out (audio/midi_out.h): the scheduler every buffer is handed to (null: none); GUI thread
    void setMidiOut(std::shared_ptr<midi_out::Out> out);

    // the latest error inside the audio callback, if any (it then played silence); "" = none
    std::string lastError() const;

    void idle();  // GUI thread, now and then (see the top)

    // Use around processing plugins on the main thread (renders, freezing, bouncing): a plugin must
    // never be processed by two threads at once, so the audio thread leaves plugins alone for that
    // time; audio tracks keep playing. Afterwards instruments get "all notes off", as their notes
    // were missed.
    class PluginsPaused {
    public:
        explicit PluginsPaused(Engine& e);
        ~PluginsPaused();
        PluginsPaused(const PluginsPaused&) = delete;
        PluginsPaused& operator=(const PluginsPaused&) = delete;

    private:
        Engine& e_;
    };

    // --- audio thread ---
    // one buffer: in (inputs per frame, or null), out (channels per frame) to fill
    void callback(const float* in, float* out, int frames);
    // a part of a buffer: (offset in the buffer, frames, timeline frame it starts at; hasStart false:
    // the transport waits, counting in)
    struct Part {
        int offset, frames;
        bool hasStart;
        int64_t start;
    };
    // Move the playhead on by a buffer (audio thread, under the position lock). Its parts: one, or more
    // where the loop jumps back, or a count-in ends. Playback that starts after the loop's end just
    // carries on; while recording, it doesn't loop. (Public for tests.)
    void advance(int frames, std::vector<Part>& parts);

private:
    class Tap;
    void inputCallback(const float* in, int frames);
    void playPreview(float* out, int frames, const std::shared_ptr<PreviewVoice>& voice);
    bool hasPlugins() const;
    void checkDropout(double now, int frames, double budget, bool underflow);
    void setError(const char* what);
    void keep(std::shared_ptr<const void> p);  // GUI thread: hold on to it a while, so the audio thread never frees it
    void prepareScratch(int frames);

    std::unique_ptr<Stream> stream_, inputStream_;
    mutable std::mutex posLock_;  // guards pos / playFrom_ between the GUI and audio threads (held for microseconds)
    std::atomic<int64_t> playFrom_{0};  // where playback last started or jumped to
    std::shared_ptr<const LoopRange> loop_;
    std::shared_ptr<const RecordSet> recorders_;
    void setRecorders(std::shared_ptr<const RecordSet> set);  // GUI thread (the old one kept a while)
    bool openInput(int channels, std::string& error);  // the default input for drivers without a duplex stream
    bool prepareInput(int channels, double& io, std::string& error);  // (that, or the duplex stream's; and its delay)
    void closeInput();
    // the separate input heard while monitoring: a ring from its thread to the audio thread
    struct InputRing {
        std::vector<float> data;
        int channels = 0;
        std::atomic<uint64_t> head{0}, tail{0};
        std::vector<float> scratch;  // (audio thread) a buffer of it, pulled from the ring
    };
    std::shared_ptr<InputRing> inputRing_;
    double recordIo_ = 0;              // (the takes' input and output delay, for one punched in)
    std::atomic<bool> monitoring_{false};
    std::shared_ptr<PreviewVoice> preview_;
    std::shared_ptr<const metronome::Clicks> clicks_;
    std::optional<metronome::MetronomeSettings> metronome_;
    live::LiveMapPtr live_;
    std::atomic<bool> restart_{true};   // next buffer starts playback afresh (instruments: all notes off)
    std::atomic<bool> clockReset_{true};
    mutable std::mutex errorLock_;
    std::string lastError_;
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::shared_ptr<const void>>> kept_;
    std::atomic<bool> haveClock_{false};  // (the last buffer's clock, for heardNow)
    std::atomic<double> clockMade_{0};
    std::atomic<int64_t> clockFrame_{0};
    double askedLatency_ = 0;              // the delay asked for when opening: stands in for an implausible one
    // audio thread only
    mixer::MixState mix_;
    std::unique_ptr<workers::Pool> pool_;  // (made when a device opens, gone when it closes)
    std::unique_ptr<Tap> tap_;
    std::vector<Part> parts_;
    std::vector<BeatTime> beats_;  // (metronome scratch)
    int64_t nextPos_ = 0;
    bool wasPlaying_ = false;
    double clockStart_ = 0, clockFrames_ = 0;
    bool clockValid_ = false;
    int lastUnderflows_ = 0;
    int toldLate_ = 0;      // (GUI thread: idle) dropouts already in the diagnostics
    double toldLateAt_ = 0;
    const float* input_ = nullptr;
    int inputChannels_ = 0;
    int inputStreamChannels_ = 0;
    std::shared_ptr<PreviewVoice> guiPreview_;  // the voice last started (GUI thread): never freed by the audio thread
    std::shared_ptr<midi_out::Out> midiOut_;    // (atomic_load; one replaced is kept a while: keep())
    std::vector<midi_out::Part> midiParts_;     // (audio thread scratch)
    std::atomic<double> outLatency_{0};         // outputLatency(), as idle() last found it
};

}  // namespace audio
