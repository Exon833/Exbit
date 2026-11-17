#include "ui/recording.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "audio/engine.h"
#include "core/util.h"
#include "model/edits.h"
#include "ui/record_offset_dialog.h"
#include "ui/toolkit/layout.h"

using namespace ui;

namespace recording {

const std::vector<int>& countInBars() {
    static const std::vector<int> v{0, 1, 2, 4};
    return v;
}

std::vector<std::pair<std::string, std::string>> inputChoices(int count) {
    std::vector<std::pair<std::string, std::string>> out;
    for (int i = 0; i < count; i++) out.emplace_back("In " + std::to_string(i + 1) + " (mono)", std::to_string(i + 1));
    for (int i = 0; i + 1 < count; i += 2) {
        std::string pair = std::to_string(i + 1) + "/" + std::to_string(i + 2);
        out.emplace_back("In " + pair + " (stereo)", pair);
    }
    return out;
}

std::vector<int> inputChannels(const std::string& setting) {
    std::vector<int> out;
    for (auto& part : util::split(setting, '/')) out.push_back(atoi(part.c_str()) - 1);
    return out;
}

int64_t countInAudio(const metronome::MetronomeSettings& settings, const Timing& timing, int bars, int sr,
                     AudioPtr& audio, double at) {
    // the clicks are on the song's beats (its bar lines accented), carried on back from where
    // recording starts: starting off a beat, the next beat after the count-in comes as counted
    audio = nullptr;
    if (bars <= 0) return 0;
    int perBar = std::max(1, timing.beatsPerBarAt(at));
    int beats = bars * perBar;
    double beatS = timing.beatSAt(at);
    double beatFrames = beatS * sr;
    int64_t frames = (int64_t)std::llround(beats * beatFrames);
    std::vector<float> mono((size_t)std::max<int64_t>(0, frames), 0.0f);
    std::vector<float> barClick = metronome::renderClick(settings.bar, sr);
    std::vector<float> beatClick = metronome::renderClick(settings.beat, sr);
    double now = (at - timing.segmentAt(at).seconds) / beatS;  // (beats since the segment's first bar line)
    double begin = now - beats;  // (where the count-in starts, in those beats)
    for (int64_t beat = (int64_t)std::ceil(begin - 1e-6); beat < (int64_t)std::ceil(now - 1e-6); beat++) {
        int64_t inBar = ((beat % perBar) + perBar) % perBar;  // (Python's %: never negative)
        const std::vector<float>& click = inBar == 0 ? barClick : beatClick;
        int64_t where = std::max<int64_t>(0, (int64_t)std::llround((beat - begin) * beatFrames));
        int64_t n = std::min<int64_t>((int64_t)click.size(), frames - where);
        for (int64_t i = 0; i < n; i++) mono[(size_t)(where + i)] += click[(size_t)i];
    }
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.resize(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); i++) buf->samples[i * 2] = buf->samples[i * 2 + 1] = mono[i];
    audio = buf;
    return frames;
}

bool takeAudio(int64_t& start, const std::vector<float>& audio, int channels, std::vector<float>& stereo) {
    stereo.clear();
    if (channels <= 0) return false;
    int64_t frames = (int64_t)(audio.size() / channels);
    int64_t skip = 0;
    if (start < 0) {  // it began right at 0 s: what came in before then is left out
        skip = std::min(frames, -start);
        start = 0;
    }
    if (frames - skip <= 0) return false;
    stereo.resize((size_t)(frames - skip) * 2);
    for (int64_t f = skip; f < frames; f++) {
        const float* in = audio.data() + f * channels;
        float* out = stereo.data() + (f - skip) * 2;
        out[0] = in[0];
        out[1] = channels > 1 ? in[1] : in[0];  // a mono input plays on both sides
    }
    return true;
}

std::vector<LoopPass> loopPasses(int64_t start, int64_t frames, int64_t loopStart, int64_t loopEnd, int64_t shortest) {
    std::vector<LoopPass> out;
    int64_t len = loopEnd - loopStart;
    if (frames <= 0) return out;
    if (len <= 0 || start >= loopEnd || start + frames <= loopEnd) {  // (it never went round)
        out.push_back({0, frames, start});
        return out;
    }
    int64_t first = loopEnd - start;
    out.push_back({0, first, start});
    for (int64_t from = first; from < frames; from += len) {
        int64_t to = std::min(frames, from + len);
        if (to - from < shortest) break;  // (stopped just after going round: nothing worth keeping)
        out.push_back({from, to, loopStart});
    }
    return out;
}

void placeClip(Project& project, const ClipPtr& clip, int64_t frame, const TrackPtr& track, int sr) {
    clip->track = track;
    clip->start = std::max<int64_t>(0, frame);
    clip->prepare(sr);
    project.addClip(clip);
    edits::clearUnder(project, {clip});  // the new clip replaces what it covers
}

}  // namespace recording

// ---------------------------------------------------------------------------------------------

RecordSettingsDialog::RecordSettingsDialog(Window* parent, const Preferences& prefs, int inputs)
    : Dialog(parent, "Recording", 380, 200) {
    FormLayout* f = form(this, 11, 6);
    setMinimumSize(380, 0);
    input = add<ComboBox>();
    for (auto& c : recording::inputChoices(inputs)) input->addItem(c.first, c.second);
    input->setCurrentIndexSilently(std::max(0, input->findData(prefs.recordInput)));
    input->setEnabled(inputs > 0);
    f->addRow("Record from", input);
    countIn = add<ComboBox>();
    for (int bars : recording::countInBars())
        countIn->addItem(!bars ? std::string("Off") : std::to_string(bars) + " bar" + (bars > 1 ? "s" : ""), bars);
    countIn->setCurrentIndexSilently(std::max(0, countIn->findData(prefs.countInBars)));
    f->addRow("Count-in", countIn);
    loopTakes = add<CheckBox>("A take each time round the loop");
    loopTakes->setChecked(prefs.loopTakes);
    loopTakes->setToolTip("With the loop on (and recording audio from before its end), playback goes round the loop while "
                          "recording, and each pass becomes a take of its own: the last one plays, the others are in "
                          "the track's takes to comp from. Off: recording plays straight on past the loop.");
    f->addRow(loopTakes);
    Label* hint = add<Label>(inputs ? "The count-in plays the metronome's clicks (its sounds: right-click Metronome) "
                                      "before recording starts at the selection cursor."
                                    : "This audio device has no inputs: choose one that has (e.g. your audio "
                                      "interface's ALSA device) in Options → Audio properties.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");
    f->addRow(hint);
    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });
    f->addRow(bb);
    adjustSize();
}

// ---------------------------------------------------------------------------------------------

Recording::Recording(RecordingHost h, audio::Engine& engine) : host(std::move(h)), engine_(engine) {}

void Recording::message(const std::string& text, int ms) {
    if (host.showMessage) host.showMessage(text, ms);
}

void Recording::showSettings() {
    if (!host.prefs) return;
    Preferences& prefs = *host.prefs;
    RecordSettingsDialog dialog(host.window, prefs, engine_.isOpen() ? engine_.inputCount() : 0);
    if (dialog.exec() == Dialog::Accepted) {
        if (!dialog.input->currentData().empty()) prefs.recordInput = dialog.input->currentData();
        prefs.countInBars = dialog.countIn->currentDataInt(0);
        prefs.loopTakes = dialog.loopTakes->isChecked();
        prefs.save();
    }
}

void Recording::toggle() {
    if (active()) finish();
    else start();
    if (host.setRecording) host.setRecording(active());
}

bool Recording::start() {
    Project& project = engine_.project;
    Preferences fallback;
    Preferences& prefs = host.prefs ? *host.prefs : fallback;
    TrackPtr track = host.currentTrack ? host.currentTrack() : nullptr;
    bool known = track && project.tracks.contains(track);
    if (track && track->kind == Kind::Midi && !std::atomic_load(&track->frozen) && known) return startMidi(track);
    if (!track || track->kind != Kind::Audio || std::atomic_load(&track->frozen) || !known) {
        msg::information(host.window, "Record",
                         !track || track->kind != Kind::Audio || !known
                             ? "Select the audio or MIDI track to record onto first: click its lane or its row in the "
                               "panel."
                             : track->name + " is frozen: unfreeze it to record onto it.");
        return false;
    }
    if (!engine_.isOpen()) {
        msg::information(host.window, "Record", "No audio device is open (Options → Audio properties).");
        return false;
    }
    std::vector<int> channels = recording::inputChannels(prefs.recordInput);
    int available = engine_.inputCount();
    int highest = channels.empty() ? 0 : *std::max_element(channels.begin(), channels.end());
    if (channels.empty() || highest >= available || *std::min_element(channels.begin(), channels.end()) < 0) {
        msg::information(host.window, "Record",
                         available ? "This audio device has " + std::to_string(available) + " input" +
                                         (available != 1 ? "s" : "") + ", so In " + prefs.recordInput +
                                         " can't be recorded. Right-click ● Rec to choose its input."
                                   : std::string("This audio device has no inputs: choose one that has (e.g. your "
                                                 "audio interface's ALSA device) in Options → Audio properties."));
        return false;
    }
    if (host.stop) host.stop();
    int64_t start = host.cursor ? host.cursor() : 0;
    if (host.seek) host.seek(start);
    else engine_.seek(start);
    metronome::MetronomeSettings defaults;
    AudioPtr clicks;
    int64_t frames = recording::countInAudio(host.metronome ? *host.metronome : defaults, project.timing,
                                             prefs.countInBars, engine_.sr, clicks, (double)start / engine_.sr);
    std::string error;
    engine_.recordOffsetMs = takesOffsetMs();  // (the Recorder takes it off: record_offset_dialog.h)
    loop_.reset();  // (loop recording: the loop on, and recording from before its end)
    if (auto loop = engine_.loop(); loop && prefs.loopTakes && loop->end > loop->start && start < loop->end) loop_ = *loop;
    engine_.loopRecording = loop_.has_value();
    if (!engine_.startRecording(channels, frames, clicks, error)) {
        engine_.loopRecording = false;
        loop_.reset();
        msg::warning(host.window, "Cannot record", error);
        return false;
    }
    if (host.resetMeters) host.resetMeters();
    track_ = track;
    start_ = start;
    if (host.showRecording) host.showRecording(track, start);
    if (host.setPlaying) host.setPlaying(true);
    message("Recording In " + prefs.recordInput + " onto " + track->name + (frames ? " after the count-in" : "") +
                "…",
            0);
    return true;
}

ClipPtr Recording::finish() {
    if (midi_) return finishMidi();
    std::shared_ptr<audio::Recorder> recorder = engine_.finishRecording();
    engine_.loopRecording = false;
    std::optional<audio::LoopRange> loop = loop_;
    loop_.reset();
    TrackPtr track = track_;
    track_ = nullptr;
    if (host.showRecording) host.showRecording(nullptr, 0);
    if (host.setPlaying) host.setPlaying(false);
    if (host.setRecording) host.setRecording(false);
    Project& project = engine_.project;
    if (!recorder || !track || !project.tracks.contains(track)) return nullptr;
    std::vector<float> raw;
    int64_t start = recorder->audio(raw);
    size_t w = recorder->channels.size();
    if (!loop || !w) return placeAudioTake(track, start, raw, (int)w, recorder->offsetMs);
    // round the loop: each pass a take of its own, placed in turn (the last one heard where they overlap)
    auto passes = recording::loopPasses(start, (int64_t)(raw.size() / w), loop->start, loop->end, engine_.sr / 10);
    ClipPtr last;
    for (auto& pass : passes) {
        std::vector<float> piece(raw.begin() + (ptrdiff_t)(pass.from * (int64_t)w), raw.begin() + (ptrdiff_t)(pass.to * (int64_t)w));
        if (ClipPtr clip = placeAudioTake(track, pass.at, piece, (int)w, recorder->offsetMs)) last = clip;
    }
    if (passes.size() > 1)
        message("Recorded " + std::to_string(passes.size()) + " takes round the loop onto " + track->name +
                    ": the last plays; all are in its takes, under it, to comp from",
                8000);
    return last;
}

double Recording::takesOffsetMs() const {
    return host.offsetMs ? host.offsetMs() : host.prefs ? record_offset_dialog::offsetMs(*host.prefs) : 0.0;
}

ClipPtr Recording::placeAudioTake(const TrackPtr& track, int64_t start, const std::vector<float>& raw, int channels,
                                  double usedMs) {
    Project& project = engine_.project;
    std::vector<float> stereo;
    if (!recording::takeAudio(start, raw, channels, stereo)) {
        message("Nothing was recorded", 5000);
        return nullptr;
    }
    takes++;
    int sr = engine_.sr;
    double seconds = (double)(stereo.size() / 2) / sr;
    auto source = std::make_shared<AudioBuffer>();
    source->samples = std::move(stereo);
    ClipPtr clip = std::make_shared<Clip>();
    clip->kind = Kind::Audio;
    clip->name = "Recording " + std::to_string(takes);
    clip->source = source;
    clip->sourceSr = sr;
    if (host.placeClip) host.placeClip(clip, start, track);
    else recording::placeClip(project, clip, start, track, sr);
    keepTake(track, clip);
    lastAudio = copyClip(clip);  // (a copy: moving the clip changes nothing)
    lastAudio->track = nullptr;
    lastOffsetMs = usedMs;
    message("Recorded " + util::fixed(seconds, 1) + " s onto " + track->name, 5000);
    return clip;
}

void Recording::keepTake(const TrackPtr& track, const ClipPtr& clip) {
    // a copy of the take as recorded, with the track's takes (for comping); from the second one on,
    // they show under the track
    // (the panel's take rows built again whenever they show: not only when they first do, at the second take)
    bool shownNow = audio::keepTake(*track, clip);
    if ((shownNow || track->showTakes) && host.lanesChanged) host.lanesChanged();
}

// --- MIDI ---

bool Recording::startMidi(const TrackPtr& track) {
    Project& project = engine_.project;
    Preferences fallback;
    Preferences& prefs = host.prefs ? *host.prefs : fallback;
    if (track->needsInstrument) {
        msg::information(host.window, "Record", track->name + " has no instrument yet: choose one first.");
        return false;
    }
    if (!host.midiInputReady || !host.midiInputReady()) {
        msg::information(host.window, "Record",
                         "To record MIDI, choose your MIDI keyboard first (Options → MIDI input…), or play the "
                         "computer keyboard (Ctrl+K)");
        return false;
    }
    if (!engine_.isOpen()) {
        msg::information(host.window, "Record", "No audio device is open (Options → Audio properties).");
        return false;
    }
    if (host.stop) host.stop();
    int64_t start = host.cursor ? host.cursor() : 0;
    if (host.seek) host.seek(start);
    else engine_.seek(start);
    metronome::MetronomeSettings defaults;
    AudioPtr clicks;
    int64_t frames = recording::countInAudio(host.metronome ? *host.metronome : defaults, project.timing,
                                             prefs.countInBars, engine_.sr, clicks, (double)start / engine_.sr);
    std::string error;
    if (!engine_.startMidiRecording(frames, clicks, error)) {
        msg::warning(host.window, "Cannot record", error.empty() ? std::string("The audio device isn't ready") : error);
        return false;
    }
    midi_.emplace(start, engine_.sr);
    if (host.resetMeters) host.resetMeters();
    track_ = track;
    start_ = start;
    if (host.showRecording) host.showRecording(track, start);
    if (host.setPlaying) host.setPlaying(true);
    message("Recording MIDI onto " + track->name + (frames ? " after the count-in" : "") + "…", 0);
    return true;
}

void Recording::beginMidiTake(const TrackPtr& track, int64_t start) {
    midi_.emplace(start, engine_.sr);
    track_ = track;
    start_ = start;
}

void Recording::midiNote(int pitch, int velocity) {
    if (!midi_) return;
    double heard = -1;
    if (!engine_.heardNow(heard)) heard = -1;  // (counting in: where the recording starts)
    midiNoteAt(pitch, velocity, heard);
}

void Recording::midiNoteAt(int pitch, int velocity, double heardFrame) {
    // a note from the MIDI keyboard while recording: during the count-in it starts where the recording does
    if (midi_) midi_->event(velocity > 0, pitch, velocity, heardFrame);
}

ClipPtr Recording::finishMidi() {
    double heard = -1;
    if (!engine_.heardNow(heard)) heard = -1;
    return finishMidiAt(heard);
}

ClipPtr Recording::finishMidiAt(std::optional<double> heard) {
    engine_.finishMidiRecording();
    audio::MidiTake take = std::move(*midi_);
    midi_.reset();
    TrackPtr track = track_;
    track_ = nullptr;
    if (host.showRecording) host.showRecording(nullptr, 0);
    if (host.setPlaying) host.setPlaying(false);
    if (host.setRecording) host.setRecording(false);
    int sr = engine_.sr;
    double lengthS = 0;  // (notes still held end where recording stopped)
    std::vector<Note> notes = take.finish(heard ? *heard : -1.0, lengthS);
    Project& project = engine_.project;
    if (notes.empty() || !track || !project.tracks.contains(track)) {
        message("No notes were played", 5000);
        return nullptr;
    }
    takes++;
    size_t count = notes.size();
    ClipPtr clip = makeMidiClip("Recording " + std::to_string(takes), std::move(notes), lengthS, sr);
    if (host.placeClip) host.placeClip(clip, take.start, track);
    else recording::placeClip(project, clip, take.start, track, sr);
    keepTake(track, clip);
    message("Recorded " + std::to_string(count) + " notes onto " + track->name, 5000);
    return clip;
}
