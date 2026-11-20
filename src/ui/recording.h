// Recording audio or MIDI (● Rec next to Play, R): onto the selected audio track, from the selection
// cursor, from the input chosen in its settings (right-click ● Rec), with an optional count-in of
// metronome clicks first (recording.py). Pressing ● Rec again, Play/Pause or Space stops it; the
// take becomes an audio clip on the track, replacing what it covers, moved earlier by the device's
// input and output delay (and plugin delay compensation) so it lines up with what was heard while
// playing.
//
// The engine does the recording itself (audio/engine.h: startRecording / finishRecording; the
// Recorder's audio() gives the take's timeline frame, already moved earlier by the delays).
// Every take moves earlier still by the device's recording offset (Options → Recording offset…:
// record_offset_dialog.h), and a copy of it as recorded is kept with the track's takes (for comping;
// from the second one on they show under the track).
//
// On a MIDI track it records what is played on the MIDI keyboard (Options → MIDI input) or the
// computer keyboard (Ctrl+K): each note where it was heard as it was played (RecordingHost::heardNow:
// the device's output delay taken off), into a new MIDI clip from the selection cursor to where
// recording stopped, replacing what it covers. Notes played during the count-in start where the
// recording does; notes still held at the end end there. The main window sends the notes played
// to midiNote() while recordingMidi().
//
// RECORD: THE PLAYED NOTES / THE EFFECTS' NOTES (right-click ● Rec: Preferences::recordEffectsNotes). The played
// notes are the keys (the track's MIDI effects then play on them, as on any clip). The effects' notes are what
// the track's MIDI effects made of them as they played (an arpeggio, chords, a groove: live.h's report()), each
// where it was played on the timeline; the main window calls poll() every frame to take them in. The track's
// MIDI effects still change the take as it plays back: bypass them (the MIDI effects window) to hear it as
// recorded.
//
// The count-in's clicks are on the song's beats (its bar lines accented), at the tempo and time
// signature where recording starts: starting off a beat, the next beat after the count-in comes
// as counted.
// A track is selected by clicking its lane or its row in the panel (the row is outlined).
//
// RECORDING SEVERAL TRACKS. Each track's row has ● (arm: Track::armed; a click arms that track alone, or the
// selected tracks; Shift-click arms more), and an audio track's row its input (Track::inputCh / inputStereo: a
// device input or pair, or None) and its monitoring switch (Auto / In / Off: Track::monitor). ● Rec records every
// armed track at once, each from its own input (audio: Engine::startRecording with a take each; MIDI: the notes
// played, into every armed MIDI track); with no track armed it records the selected track from the Rec settings'
// input, as before. Arming a track while recording punches it in from there; disarming one punches it out (its
// take so far is placed then). An armed track with input None, or an input the device lacks, is left out (the
// status bar says so).
// MONITORING (audio/mixer.h): a track set to In, or Auto while armed, hears its input through its effects,
// fader and pan; tick() opens the input for it (Engine::setMonitoring) whenever some track wants it, and says
// once when the device has none. While recording, a track's clips are muted.
// LOW-LATENCY MONITORING (Options → Audio properties: Preferences::lowLatencyMonitoring): while recording, the
// effects with look-ahead on the tracks recorded are left out (PluginSlot::lowLatencyBypass, as if bypassed), so
// what is heard isn't held back by them; the status bar says how many. They come back when recording stops.
// MIDI ROUND THE LOOP (the Rec settings: Preferences::midiLoopTakes): with the loop on, recording MIDI goes round
// it (as audio does); Merge puts every pass's notes into one clip over the loop, Takes makes each pass a take of
// its own (the last one plays; all are in the track's takes, to comp from). The passes are told apart by where
// the notes are heard: the heard position jumping back round the loop starts the next pass (tick() follows it).
#pragma once
#include <map>
#include <optional>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "audio/dsp.h"
#include "audio/engine.h"
#include "audio/metronome.h"
#include "model/model.h"
#include "ui/preferences.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"


namespace recording {
const std::vector<int>& countInBars();  // COUNT_IN_BARS: 0, 1, 2, 4
// [(label, setting)] for a device with `count` input channels: each one alone (mono), then pairs
std::vector<std::pair<std::string, std::string>> inputChoices(int count);
// a track's input chooser: ("None", "none"), then inputChoices
std::vector<std::pair<std::string, std::string>> trackInputChoices(int count);
std::string inputSetting(const Track& track);                // "1", "3/4", "none"
std::string inputName(const Track& track);                   // "In 1", "In 3/4", "None"
void setInput(Track& track, const std::string& setting);     // (Track::inputCh, inputStereo)
// "1" -> {0}; "3/4" -> {2, 3} (0-based channels)
std::vector<int> inputChannels(const std::string& setting);
// `bars` bars of metronome clicks (the count-in) as stereo audio, at the tempo and time signature where
// recording starts (`at` seconds), on the song's beats carried on back from there; returns its frames
// (0 and null for none)
int64_t countInAudio(const metronome::MetronomeSettings& settings, const Timing& timing, int bars, int sr,
                     AudioPtr& audio, double at = 0.0);
// The take as it is placed: `audio` interleaved with `channels` per frame from timeline frame
// `start` (which may be < 0: a take that began right at 0 s leaves out what came in before then)
// becomes stereo (a mono input plays on both sides) in `stereo`, and start >= 0. False if nothing is left.
bool takeAudio(int64_t& start, const std::vector<float>& audio, int channels, std::vector<float>& stereo);
// timeline.place_clip without the selection and the redraw: the clip goes onto `track` at `frame`
// (at least 0), prepared for `sr`, replacing what it covers
void placeClip(Project& project, const ClipPtr& clip, int64_t frame, const TrackPtr& track, int sr);
// Loop recording: a take of `frames` frames that began at timeline frame `start` while playback went round the
// loop [loopStart, loopEnd) cut into its passes: (first frame, end frame) in the take, and the timeline frame
// each goes to. The first pass is from `start` to the loop's end; each next one is the loop's length, put at
// the loop's start (the last may be shorter). Pieces shorter than `shortest` frames after the first are left
// out (stopping just after going round). One piece (the take whole) when it never reached the loop's end.
struct LoopPass {
    int64_t from, to, at;
};
std::vector<LoopPass> loopPasses(int64_t start, int64_t frames, int64_t loopStart, int64_t loopEnd, int64_t shortest);
}  // namespace recording

// Right-click ● Rec: the input to record and the count-in.
class RecordSettingsDialog : public ui::Dialog {
public:
    RecordSettingsDialog(ui::Window* parent, const Preferences& prefs, int inputs);
    ui::ComboBox* input;
    ui::ComboBox* countIn;
    ui::CheckBox* loopTakes;
    ui::ComboBox* midiNotes;  // Record: the played notes (0) / the effects' notes (1)
    ui::ComboBox* midiLoop;   // MIDI round the loop: Merge (0) / Takes (1)
};

// What Recording needs from the main window and the timeline.
struct RecordingHost {
    ui::Window* window = nullptr;                              // (messages)
    Preferences* prefs = nullptr;                              // recordInput, countInBars (saved when changed)
    const metronome::MetronomeSettings* metronome = nullptr;   // the count-in's sounds
    std::function<TrackPtr()> currentTrack;                    // timeline.current_track (null: none)
    std::function<int64_t()> cursor;                           // timeline.cursor (frames)
    std::function<void(int64_t)> seek;                         // timeline.seek
    std::function<void()> stop;                                // MainWindow::stop
    std::function<void(bool)> setPlaying;                      // transport.set_playing
    std::function<void(bool)> setRecording;                    // transport.set_recording
    // timeline.recording = [(track, start)] while recording (each take grows on its lane); empty when it ends (and
    // the timeline is redrawn)
    std::function<void(const std::vector<std::pair<TrackPtr, int64_t>>&)> showRecording;
    // timeline.place_clip (selects the clip, relayouts); when unset, recording::placeClip
    std::function<void(const ClipPtr&, int64_t, const TrackPtr&)> placeClip;
    std::function<void(const std::string&, int)> showMessage;  // statusBar().showMessage(text, ms; 0 = until replaced)
    std::function<void()> resetMeters;                         // MainWindow::reset_meters (held peaks, clip lights)
    std::function<void()> lanesChanged;                        // timeline.lanes_changed (take lanes shown)
    // the recording offset for the device in use (ms; positive: takes move earlier); unset: the
    // settings' (record_offset_dialog::offsetMs(*prefs)). Given to the engine (Engine::recordOffsetMs)
    // when a take starts: its Recorder takes it off with the device's delays.
    std::function<double()> offsetMs;
    // --- MIDI recording ---
    // a MIDI keyboard is chosen (Options → MIDI input) or the computer keyboard plays (Ctrl+K)
    std::function<bool()> midiInputReady;
};

class Recording {
public:
    Recording(RecordingHost host, audio::Engine& engine);
    bool active() const { return active_; }
    // the first track being recorded onto, while recording (null: none)
    TrackPtr track() const;
    std::vector<TrackPtr> tracks() const;  // every track being recorded onto
    int64_t startFrame() const { return start_; }
    int takes = 0;                 // numbering for the takes' names
    void showSettings();           // right-click ● Rec
    void toggle();                 // ● Rec: start recording, or stop and keep the take
    bool start();
    // stop, and place the takes on their tracks as audio clips, or the notes played as MIDI clips; the last one
    // placed (null: none)
    ClipPtr finish();
    // RECORDING SEVERAL TRACKS: arm (or disarm) the tracks; while recording, each punches in or out (see the top)
    void setArmed(const std::vector<TrackPtr>& tracks, bool on);
    // the audio take (`raw`: `channels` a frame from timeline frame `start`, already moved earlier by the
    // device's delays and the recording offset `offsetMs`: Recorder::audio) placed on the track, kept
    // with the track's takes and as lastAudio (null if nothing is left)
    ClipPtr placeAudioTake(const TrackPtr& track, int64_t start, const std::vector<float>& raw, int channels,
                           double offsetMs);
    // the recording offset new audio takes get (ms): host.offsetMs, else the settings' for the device
    double takesOffsetMs() const;
    // the last audio take of this session, as it was placed (Recording offset: Measure), and the
    // recording offset it was placed with
    ClipPtr lastAudio;
    double lastOffsetMs = 0;
    // (every frame) the MIDI effects' notes, following the loop's passes, and monitoring (see the top)
    void tick();
    // --- MIDI ---
    bool recordingMidi() const { return !midi_.empty(); }
    // a note from the MIDI keyboard (or the computer keyboard) while recording: velocity 0 = off; `to` (if given)
    // says which tracks it goes to (a track's MIDI input: ui/midi_input.h), the others don't record it
    void midiNote(int pitch, int velocity, const std::function<bool(const Track&)>& to = nullptr);
    // the same with the time it was heard given (timeline frame; tests)
    void midiNoteAt(int pitch, int velocity, double heardFrame);
    // record the notes the track's MIDI effects make instead of the keys played (see the top); set when a
    // MIDI recording starts, from the preferences
    void recordEffectsNotes(bool on);
    bool recordingEffectsNotes() const;
    // (every frame) the effects' notes played since the last call, into the take
    void poll();
    // (tests) a MIDI take from timeline frame `start` onto `track`, as startMidi() begins one; with `loop`, round it
    void beginMidiTake(const TrackPtr& track, int64_t start, std::optional<audio::LoopRange> loop = std::nullopt);
    // (tests; tick() with the frame heard now) the frame heard at this moment: going back round the loop starts
    // the next pass
    void follow(double heardFrame);
    RecordingHost host;
    // (tests) the plugins left out for low-latency monitoring now
    const std::vector<SlotPtr>& lowLatencyBypassed() const { return bypassed_; }

private:
    struct AudioTake {
        TrackPtr track;
        std::shared_ptr<audio::Recorder> recorder;
        int64_t from = 0;  // (the timeline frame its take is drawn from)
    };
    struct MidiRec {
        TrackPtr track;
        audio::MidiTake take;
        bool processed = false;  // its MIDI effects' notes (poll), not the keys
        int64_t from = 0;
    };
    audio::Engine& engine_;
    bool active_ = false;
    int64_t start_ = 0;
    std::optional<audio::LoopRange> loop_;  // recording round this loop: a take each pass (MIDI: merged, or takes)
    std::vector<AudioTake> audio_;
    std::vector<MidiRec> midi_;
    bool processed_ = false;  // (the setting, for MIDI takes punched in)
    int64_t pass_ = 0;        // the loop's passes gone round (MIDI)
    double lastHeard_ = -1;
    std::vector<SlotPtr> bypassed_;  // low-latency monitoring
    bool monitorTold_ = false;       // (monitoring couldn't be had: said once)
    std::string monitorDevice_;      // (the device it was tried on)
    std::vector<TrackPtr> armedTracks() const;  // the project's armed tracks
    bool midiReady(const TrackPtr& track, bool tell);
    std::optional<std::vector<int>> inputOf(const Track& track, std::string& why) const;
    void addMidiTake(const TrackPtr& track, int64_t start);
    ClipPtr finishMidiAt(std::optional<double> heard);
    ClipPtr placeMidi(MidiRec& rec, double endUnrolled);
    ClipPtr placeAudio(const AudioTake& take);
    ClipPtr placeStereo(const TrackPtr& track, int64_t start, std::vector<float> stereo, double offsetMs);
    double unroll(double heard);  // a heard frame with the loop's passes laid end to end after each other
    void lowLatency(bool on);
    void ended();                 // (the transport and the timeline told: recording is over)
    void showTakes();             // the timeline's growing takes as they are now
    void keepTake(const TrackPtr& track, const ClipPtr& clip);
    void message(const std::string& text, int ms);

public:
    // (tests) finish a MIDI recording as if `heard` were the frame heard at the end
    ClipPtr finishMidiAtFrame(double heard) { return finishMidiAt(heard); }
};
