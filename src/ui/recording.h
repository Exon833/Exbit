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
// The count-in's clicks are on the song's beats (its bar lines accented), at the tempo and time
// signature where recording starts: starting off a beat, the next beat after the count-in comes
// as counted.
// A track is selected by clicking its lane or its row in the panel (the row is outlined).
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
}  // namespace recording

// Right-click ● Rec: the input to record and the count-in.
class RecordSettingsDialog : public ui::Dialog {
public:
    RecordSettingsDialog(ui::Window* parent, const Preferences& prefs, int inputs);
    ui::ComboBox* input;
    ui::ComboBox* countIn;
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
    // timeline.recording = (track, start) while recording (the take grows on its lane); (null, 0)
    // when it ends (and the timeline is redrawn)
    std::function<void(const TrackPtr&, int64_t)> showRecording;
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
    bool active() const { return (bool)track_; }
    const TrackPtr& track() const { return track_; }  // the track being recorded onto, while recording
    int64_t startFrame() const { return start_; }
    int takes = 0;                 // numbering for the takes' names
    void showSettings();           // right-click ● Rec
    void toggle();                 // ● Rec: start recording, or stop and keep the take
    bool start();
    // stop, and place the take on the track as an audio clip, or the notes played as a MIDI clip (null: none)
    ClipPtr finish();
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
    // --- MIDI ---
    bool recordingMidi() const { return midi_.has_value(); }
    // a note from the MIDI keyboard (or the computer keyboard) while recording: velocity 0 = off
    void midiNote(int pitch, int velocity);
    // the same with the time it was heard given (timeline frame; tests)
    void midiNoteAt(int pitch, int velocity, double heardFrame);
    // (tests) a MIDI take from timeline frame `start` onto `track`, as startMidi() begins one
    void beginMidiTake(const TrackPtr& track, int64_t start);
    RecordingHost host;
private:
    audio::Engine& engine_;
    TrackPtr track_;
    int64_t start_ = 0;
    std::optional<audio::MidiTake> midi_;
    bool startMidi(const TrackPtr& track);
    ClipPtr finishMidi();
    ClipPtr finishMidiAt(std::optional<double> heard);
    void keepTake(const TrackPtr& track, const ClipPtr& clip);
    void message(const std::string& text, int ms);
public:
    // (tests) finish a MIDI recording as if `heard` were the frame heard at the end
    ClipPtr finishMidiAtFrame(double heard) { return finishMidiAt(heard); }
};
