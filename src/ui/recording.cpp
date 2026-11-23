#include "ui/recording.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "audio/engine.h"
#include "audio/mixer.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "model/edits.h"
#include "ui/record_offset_dialog.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

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

std::vector<std::pair<std::string, std::string>> trackInputChoices(int count) {
    std::vector<std::pair<std::string, std::string>> out{{"None", "none"}};
    for (auto& c : inputChoices(count)) out.push_back(c);
    return out;
}

std::string inputSetting(const Track& track) {
    if (track.inputCh < 0) return "none";
    return std::to_string(track.inputCh + 1) + (track.inputStereo ? "/" + std::to_string(track.inputCh + 2) : "");
}

std::string inputName(const Track& track) { return track.inputCh < 0 ? "None" : "In " + inputSetting(track); }

void setInput(Track& track, const std::string& setting) {
    std::vector<int> ch = setting == "none" ? std::vector<int>{} : inputChannels(setting);
    track.inputCh = ch.empty() || ch[0] < 0 ? -1 : ch[0];
    track.inputStereo = track.inputCh >= 0 && ch.size() > 1;
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
    TempoMapPtr map = timing.map();
    const Meter& meter = map->meterAtBeat(map->beatAt(at));
    int perBar = std::max(1, meter.beatsPerBar);
    int beats = bars * perBar;  // (counted beats: the signature's unit, 8ths in 6/8)
    double unit = 4.0 / meter.beatUnit;
    double beatS = timing.beatSAt(at) * unit;
    double beatFrames = beatS * sr;
    int64_t frames = (int64_t)std::llround(beats * beatFrames);
    std::vector<float> mono((size_t)std::max<int64_t>(0, frames), 0.0f);
    std::vector<float> barClick = metronome::renderClick(settings.bar, sr);
    std::vector<float> beatClick = metronome::renderClick(settings.beat, sr);
    double now = (timing.beatAt(at) - meter.beat) / unit;  // (counted beats since the time signature's first bar line)
    double begin = now - beats;  // (where the count-in starts, in those beats)
    int group = meter.sig().compound() ? 3 : 0;
    for (int64_t beat = (int64_t)std::ceil(begin - 1e-6); beat < (int64_t)std::ceil(now - 1e-6); beat++) {
        int64_t inBar = ((beat % perBar) + perBar) % perBar;  // (Python's %: never negative)
        bool groupStart = group && inBar != 0 && inBar % group == 0;  // (a compound signature's dotted groups)
        const std::vector<float>& click = inBar == 0 || groupStart ? barClick : beatClick;
        float g = groupStart ? (float)metronome::GROUP_GAIN : 1.0f;
        int64_t where = std::max<int64_t>(0, (int64_t)std::llround((beat - begin) * beatFrames));
        int64_t n = std::min<int64_t>((int64_t)click.size(), frames - where);
        for (int64_t i = 0; i < n; i++) mono[(size_t)(where + i)] += click[(size_t)i] * g;
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
    setStyleType("RecordSettingsDialog");
    FormLayout* f = styleRoom(form(this, 11, 6), computedStyle());
    setMinimumSize(std::max(0, style::px(computedStyle(), "min-width", 380)), 0);
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
    loopTakes = add<CheckBox>("Record round the loop");
    loopTakes->setChecked(prefs.loopTakes);
    loopTakes->setToolTip("With the loop on (and recording audio from before its end), playback goes round the loop while "
                          "recording, and each pass becomes a take of its own: the last one plays, the others are in "
                          "the track's takes to comp from (MIDI: merged or takes, below). Off: recording plays "
                          "straight on past the loop.");
    f->addRow(loopTakes);
    midiNotes = add<ComboBox>();
    midiNotes->addItem("The played notes", 0);
    midiNotes->addItem("The effects' notes", 1);
    midiNotes->setCurrentIndexSilently(prefs.recordEffectsNotes ? 1 : 0);
    midiNotes->setToolTip("Recording MIDI keeps the keys played (the track's MIDI effects then play on them), or the "
                          "notes the track's MIDI effects made of them (an arpeggio, chords, a groove) as they played");
    f->addRow("Record (MIDI)", midiNotes);
    midiLoop = add<ComboBox>();
    midiLoop->addItem("Merge: every pass into one clip", 0);
    midiLoop->addItem("Takes: each pass a take", 1);
    midiLoop->setCurrentIndexSilently(prefs.midiLoopTakes ? 1 : 0);
    midiLoop->setToolTip("Recording MIDI round the loop (the loop on, and the box above ticked): Merge adds each "
                         "pass's notes to the same clip; Takes makes each pass a take of its own, to comp from");
    f->addRow("MIDI round the loop", midiLoop);
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

TrackPtr Recording::track() const {
    auto all = tracks();
    return all.empty() ? nullptr : all.front();
}

std::vector<TrackPtr> Recording::tracks() const {
    std::vector<TrackPtr> out;
    for (auto& a : audio_) out.push_back(a.track);
    for (auto& m : midi_)
        if (std::find(out.begin(), out.end(), m.track) == out.end()) out.push_back(m.track);
    return out;
}

void Recording::showSettings() {
    if (!host.prefs) return;
    Preferences& prefs = *host.prefs;
    RecordSettingsDialog dialog(host.window, prefs, engine_.isOpen() ? engine_.inputCount() : 0);
    if (dialog.exec() == Dialog::Accepted) {
        if (!dialog.input->currentData().empty()) prefs.recordInput = dialog.input->currentData();
        prefs.countInBars = dialog.countIn->currentDataInt(0);
        prefs.loopTakes = dialog.loopTakes->isChecked();
        prefs.recordEffectsNotes = dialog.midiNotes->currentDataInt(0) == 1;
        prefs.midiLoopTakes = dialog.midiLoop->currentDataInt(0) == 1;
        prefs.save();
    }
}

void Recording::toggle() {
    if (active()) finish();
    else start();
    if (host.setRecording) host.setRecording(active());
}

std::vector<TrackPtr> Recording::armedTracks() const {
    std::vector<TrackPtr> out;
    for (auto& t : engine_.project.tracks)
        if (t->armed && t->kind != Kind::Return) out.push_back(t);
    return out;
}

std::optional<std::vector<int>> Recording::inputOf(const Track& track, std::string& why) const {
    if (track.inputCh < 0) {
        why = track.name + " has no input (None)";
        return std::nullopt;
    }
    std::vector<int> channels{track.inputCh};
    if (track.inputStereo) channels.push_back(track.inputCh + 1);
    int available = engine_.inputCount();
    if (channels.back() >= available) {
        why = available ? track.name + "'s " + recording::inputName(track) + ": the device has " +
                              std::to_string(available) + " input" + (available != 1 ? "s" : "")
                        : std::string("this audio device has no inputs: choose one that has (e.g. your audio "
                                      "interface's ALSA device) in Options → Audio properties");
        return std::nullopt;
    }
    return channels;
}

bool Recording::midiReady(const TrackPtr& track, bool tell) {
    std::string why;
    if (track->needsInstrument) why = track->name + " has no instrument yet: choose one first.";
    else if (!host.midiInputReady || !host.midiInputReady())
        why = "To record MIDI, choose your MIDI keyboard first (Options → MIDI input…), or play the computer keyboard "
              "(Ctrl+K)";
    if (why.empty()) return true;
    if (tell) msg::information(host.window, "Record", why);
    return false;
}

bool Recording::start() {
    if (active_) return false;
    Project& project = engine_.project;
    Preferences fallback;
    Preferences& prefs = host.prefs ? *host.prefs : fallback;
    bool open = engine_.isOpen() || engine_.offline;
    std::vector<audio::RecordInput> inputs;
    std::vector<TrackPtr> audioTracks, midiTracks;
    std::vector<std::string> left;  // (armed tracks that can't be recorded, and why)
    std::vector<TrackPtr> armed = armedTracks();
    if (armed.empty()) {  // as before: the selected track, from the Rec settings' input
        TrackPtr track = host.currentTrack ? host.currentTrack() : nullptr;
        bool known = track && project.tracks.contains(track);
        if (track && track->kind == Kind::Midi && !std::atomic_load(&track->frozen) && known) {
            if (!midiReady(track, true)) return false;
            midiTracks.push_back(track);
        } else if (!track || track->kind != Kind::Audio || std::atomic_load(&track->frozen) || !known) {
            msg::information(host.window, "Record",
                             !track || track->kind != Kind::Audio || !known
                                 ? "Select the audio or MIDI track to record onto first (click its lane or its row in "
                                   "the panel), or arm the tracks to record with their ●."
                                 : track->name + " is frozen: unfreeze it to record onto it.");
            return false;
        } else {
            if (!open) {
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
                                           : std::string("This audio device has no inputs: choose one that has (e.g. "
                                                         "your audio interface's ALSA device) in Options → Audio "
                                                         "properties."));
                return false;
            }
            inputs.push_back({track.get(), channels});
            audioTracks.push_back(track);
        }
    } else {  // RECORDING SEVERAL TRACKS: every armed one, from its own input
        for (auto& t : armed) {
            std::string why;
            if (std::atomic_load(&t->frozen)) {
                left.push_back(t->name + " is frozen");
            } else if (t->kind == Kind::Midi) {
                if (midiReady(t, false)) midiTracks.push_back(t);
                else left.push_back(t->needsInstrument ? t->name + " has no instrument" : t->name + ": no MIDI input");
            } else if (!open) {
                left.push_back(t->name + ": no audio device is open");
            } else if (auto channels = inputOf(*t, why)) {
                inputs.push_back({t.get(), *channels});
                audioTracks.push_back(t);
            } else {
                left.push_back(why);
            }
        }
        if (audioTracks.empty() && midiTracks.empty()) {
            std::string text = "None of the armed tracks can be recorded:";
            for (auto& why : left) text += "\n  " + why;
            if (!open) text += "\nNo audio device is open (Options → Audio properties).";
            msg::information(host.window, "Record", text);
            util::debugf("recording: nothing armed can be recorded (%zu armed)\n", armed.size());
            return false;
        }
    }
    if (!open) {
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
    engine_.recordOffsetMs = takesOffsetMs();  // (the Recorder takes it off: record_offset_dialog.h)
    loop_.reset();  // (loop recording: the loop on, and recording from before its end)
    if (auto loop = engine_.loop(); loop && prefs.loopTakes && loop->end > loop->start && start < loop->end) loop_ = *loop;
    engine_.loopRecording = loop_.has_value();
    bool started = !inputs.empty() ? engine_.startRecording(inputs, frames, clicks, error)
                                   : engine_.startMidiRecording(frames, clicks, error);
    if (!started) {
        engine_.loopRecording = false;
        loop_.reset();
        msg::warning(host.window, "Cannot record", error.empty() ? std::string("The audio device isn't ready") : error);
        return false;
    }
    if (!midiTracks.empty()) engine_.recordingMidi = true;
    active_ = true;
    start_ = start;
    pass_ = 0;
    lastHeard_ = (double)start;
    audio_.clear();
    midi_.clear();
    if (auto set = engine_.recorders())
        for (size_t i = 0; i < audioTracks.size() && i < set->recorders.size(); i++)
            audio_.push_back({audioTracks[i], set->recorders[i], start});
    for (auto& t : midiTracks) addMidiTake(t, start);
    recordEffectsNotes(prefs.recordEffectsNotes);
    lowLatency(prefs.lowLatencyMonitoring);
    if (host.resetMeters) host.resetMeters();
    showTakes();
    if (host.setPlaying) host.setPlaying(true);
    size_t count = audioTracks.size() + midiTracks.size();
    std::string text;
    if (count > 1) {
        text = "Recording " + std::to_string(count) + " tracks:";
        for (auto& t : audioTracks) text += " " + t->name + " (" + recording::inputName(*t) + ")";
        for (auto& t : midiTracks) text += " " + t->name + " (MIDI)";
    } else if (!audioTracks.empty()) {
        text = "Recording " + (armed.empty() ? "In " + prefs.recordInput : recording::inputName(*audioTracks[0])) +
               " onto " + audioTracks[0]->name;
    } else {
        text = "Recording MIDI onto " + midiTracks[0]->name;
    }
    if (frames) text += " after the count-in";
    text += "…";
    if (!left.empty()) {
        text += " (left out:";
        for (auto& why : left) text += " " + why + ";";
        text.back() = ')';
    }
    if (!bypassed_.empty())
        text += " Low-latency monitoring: " + std::to_string(bypassed_.size()) + " effect" +
                (bypassed_.size() != 1 ? "s" : "") + " with look-ahead left out while recording.";
    message(text, 0);
    util::debugf("recording: %zu audio and %zu MIDI take(s) from frame %lld%s%s\n", audioTracks.size(),
                 midiTracks.size(), (long long)start, loop_ ? " round the loop" : "",
                 prefs.lowLatencyMonitoring ? ", low-latency monitoring" : "");
    return true;
}

void Recording::showTakes() {
    if (!host.showRecording) return;
    std::vector<std::pair<TrackPtr, int64_t>> shown;
    for (auto& a : audio_) shown.emplace_back(a.track, a.from);
    for (auto& m : midi_) shown.emplace_back(m.track, m.from);
    host.showRecording(shown);
}

void Recording::ended() {
    if (host.showRecording) host.showRecording({});
    if (host.setPlaying) host.setPlaying(false);
    if (host.setRecording) host.setRecording(false);
}

void Recording::lowLatency(bool on) {
    for (auto& s : bypassed_) s->lowLatencyBypass = false;
    bypassed_.clear();
    if (!on || !active_) return;
    for (auto& t : tracks())
        for (auto& s : *t->effects)
            if (plugin_audio::usable(s) && !s->bypass && plugin_audio::latency(*s) > 0) {
                s->lowLatencyBypass = true;
                bypassed_.push_back(s);
                util::debugf("recording: low-latency monitoring leaves out \"%s\" on \"%s\" (%d frames of look-ahead)\n",
                             s->name.c_str(), t->name.c_str(), plugin_audio::latency(*s));
            }
}

void Recording::setArmed(const std::vector<TrackPtr>& tracks, bool on) {
    std::vector<TrackPtr> changed;
    for (auto& t : tracks)
        if (t && t->kind != Kind::Return && t->armed != on) {
            t->armed = on;
            changed.push_back(t);
            util::debugf("recording: \"%s\" %s\n", t->name.c_str(), on ? "armed" : "disarmed");
        }
    if (!active_ || changed.empty()) return;
    double heard = -1;
    bool playing = engine_.heardNow(heard);
    int64_t here = playing ? (int64_t)heard : start_;
    std::vector<std::string> names;
    if (!on) {  // punching out, every audio take at once (the audio thread let go of them together)
        std::vector<const Track*> out;
        std::vector<AudioTake> ending;
        for (auto& t : changed)
            for (auto it = audio_.begin(); it != audio_.end(); ++it)
                if (it->track == t) {
                    out.push_back(t.get());
                    ending.push_back(*it);
                    audio_.erase(it);
                    break;
                }
        auto gone = engine_.removeRecorders(out);
        for (auto& take : ending) {
            for (auto& rec : gone)
                if (rec == take.recorder) placeAudio(take);
            names.push_back(take.track->name);
        }
    }
    for (auto& t : changed) {
        auto inAudio = std::find_if(audio_.begin(), audio_.end(), [&](const AudioTake& a) { return a.track == t; });
        auto inMidi = std::find_if(midi_.begin(), midi_.end(), [&](const MidiRec& m) { return m.track == t; });
        if (on) {  // punching in from here
            if (inAudio != audio_.end() || inMidi != midi_.end()) continue;
            std::string why;
            if (t->kind == Kind::Midi) {
                if (!midiReady(t, false)) {
                    message("Can't punch in " + t->name + (t->needsInstrument ? ": it has no instrument yet"
                                                                               : ": no MIDI keyboard is chosen"), 5000);
                    continue;
                }
                addMidiTake(t, playing ? (int64_t)unroll(heard) : start_);
                midi_.back().from = here;
                engine_.recordingMidi = true;
            } else if (auto channels = inputOf(*t, why)) {
                std::string error;
                auto rec = engine_.addRecorder({t.get(), *channels}, error);
                if (!rec) {
                    message("Can't punch in " + t->name + ": " + error, 5000);
                    continue;
                }
                audio_.push_back({t, rec, here});
            } else {
                message("Can't punch in: " + why, 5000);
                continue;
            }
            names.push_back(t->name);
        } else {  // punching out: its take so far placed now
            if (inMidi != midi_.end()) {
                MidiRec rec = std::move(*inMidi);
                midi_.erase(inMidi);
                if (rec.processed) engine_.liveNotes(t.get())->report(false);
                placeMidi(rec, playing ? unroll(heard) : -1);
                names.push_back(t->name);
            }
        }
    }
    if (midi_.empty()) engine_.recordingMidi = false;
    lowLatency(host.prefs && host.prefs->lowLatencyMonitoring);
    showTakes();
    if (!names.empty()) {
        std::string list;
        for (auto& n : names) list += (list.empty() ? "" : ", ") + n;
        message((on ? "Punched in: recording " : "Punched out: kept the take of ") + list, 5000);
    }
}

ClipPtr Recording::finish() {
    double heard = -1;
    if (!engine_.heardNow(heard)) heard = -1;
    return finishMidiAt(heard);
}

ClipPtr Recording::finishMidiAt(std::optional<double> heard) {
    if (!active_) return nullptr;
    poll();  // (the effects' notes played until now)
    double end = heard && *heard >= 0 ? unroll(*heard) : -1;
    for (auto& m : midi_)
        if (m.processed) engine_.liveNotes(m.track.get())->report(false);
    if (engine_.recordingAudio()) engine_.finishRecordings();  // (the Recorders, all collected: audio_ holds them)
    engine_.finishMidiRecording();
    engine_.loopRecording = false;
    lowLatency(false);
    std::vector<AudioTake> audio = std::move(audio_);
    std::vector<MidiRec> midi = std::move(midi_);
    audio_.clear();
    midi_.clear();
    active_ = false;
    processed_ = false;
    ended();
    ClipPtr last;
    for (auto& a : audio)
        if (ClipPtr clip = placeAudio(a)) last = clip;
    for (auto& m : midi)
        if (ClipPtr clip = placeMidi(m, end)) last = clip;
    loop_.reset();
    return last;
}

ClipPtr Recording::placeAudio(const AudioTake& take) {
    Project& project = engine_.project;
    const audio::Recorder& rec = *take.recorder;
    if (!take.track || !project.tracks.contains(take.track)) return nullptr;
    int64_t start = rec.timelineStart(), frames = rec.frames();
    auto piece = [&](int64_t from, int64_t to, int64_t at) {
        if (at < 0) {  // (it began right at 0 s: what came in before then is left out)
            from += -at;
            at = 0;
        }
        std::vector<float> stereo;
        rec.stereo(from, to, stereo);
        return placeStereo(take.track, at, std::move(stereo), rec.offsetMs);
    };
    if (!loop_ || rec.channels.empty()) return piece(0, frames, start);
    // round the loop: each pass a take of its own, placed in turn (the last one heard where they overlap)
    auto passes = recording::loopPasses(start, frames, loop_->start, loop_->end, engine_.sr / 10);
    ClipPtr last;
    for (auto& pass : passes)
        if (ClipPtr clip = piece(pass.from, pass.to, pass.at)) last = clip;
    if (passes.size() > 1)
        message("Recorded " + std::to_string(passes.size()) + " takes round the loop onto " + take.track->name +
                    ": the last plays; all are in its takes, under it, to comp from",
                8000);
    return last;
}

double Recording::takesOffsetMs() const {
    return host.offsetMs ? host.offsetMs() : host.prefs ? record_offset_dialog::offsetMs(*host.prefs) : 0.0;
}

ClipPtr Recording::placeAudioTake(const TrackPtr& track, int64_t start, const std::vector<float>& raw, int channels,
                                  double usedMs) {
    std::vector<float> stereo;
    if (!recording::takeAudio(start, raw, channels, stereo)) {
        message("Nothing was recorded", 5000);
        return nullptr;
    }
    return placeStereo(track, start, std::move(stereo), usedMs);
}

ClipPtr Recording::placeStereo(const TrackPtr& track, int64_t start, std::vector<float> stereo, double usedMs) {
    Project& project = engine_.project;
    if (stereo.empty()) {
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

void Recording::addMidiTake(const TrackPtr& track, int64_t start) {
    midi_.push_back(MidiRec{track, audio::MidiTake(start, engine_.sr), false, start});
    if (processed_) recordEffectsNotes(true);
}

void Recording::beginMidiTake(const TrackPtr& track, int64_t start, std::optional<audio::LoopRange> loop) {
    active_ = true;
    start_ = start;
    loop_ = loop;
    pass_ = 0;
    lastHeard_ = (double)start;
    audio_.clear();
    midi_.clear();
    addMidiTake(track, start);
}

void Recording::follow(double heard) {
    if (!loop_ || heard < 0) return;
    double len = (double)(loop_->end - loop_->start);
    if (lastHeard_ >= 0 && heard < lastHeard_ - len / 2) {  // (gone back round the loop)
        pass_++;
        util::debugf("recording: round the loop, pass %lld\n", (long long)pass_ + 1);
    }
    lastHeard_ = heard;
}

double Recording::unroll(double heard) {
    if (heard < 0) return heard;
    follow(heard);
    if (!loop_ || pass_ == 0) return heard;
    return (double)loop_->end + (double)(pass_ - 1) * (double)(loop_->end - loop_->start) + (heard - (double)loop_->start);
}

void Recording::recordEffectsNotes(bool on) {
    processed_ = on;
    for (auto& m : midi_) {
        if (!on || m.processed) {
            if (!on) m.processed = false;
            continue;
        }
        m.processed = true;
        auto live = engine_.liveNotes(m.track.get());
        std::vector<live::Played> old;
        live->played(old);  // (from before: not this take's)
        live->report(true);
        util::debugf("recording: the MIDI effects' notes of \"%s\" are recorded (not the keys)\n", m.track->name.c_str());
    }
}

bool Recording::recordingEffectsNotes() const {
    for (auto& m : midi_)
        if (m.processed) return true;
    return false;
}

void Recording::poll() {
    for (auto& m : midi_) {
        if (!m.processed) continue;
        std::vector<live::Played> notes;
        engine_.liveNotes(m.track.get())->played(notes);
        for (auto& n : notes) {
            int velocity = n.on ? std::max(1, n.velocity) : 0;
            m.take.event(velocity > 0, n.pitch, velocity, unroll((double)n.frame));
        }
    }
}

void Recording::tick() {
    double heard = -1;
    if (active_ && loop_ && engine_.heardNow(heard)) follow(heard);
    poll();
    // MONITORING: the input opened while some track hears it (said once when it can't be)
    std::string device = engine_.driverType + "|" + engine_.deviceId + (engine_.isOpen() ? "+" : "-");
    if (device != monitorDevice_) monitorTold_ = false;  // (another device: try it)
    monitorDevice_ = device;
    bool want = false;
    for (auto& t : engine_.project.tracks)
        if (mixer::usesInput(*t)) want = true;  // (an External synth's sound comes back on it too: midi_out.h)
    if (engine_.inputWanted) want = true;  // (an External synth being measured)
    if (!want) monitorTold_ = false;
    if (want == engine_.monitoring() || (want && monitorTold_)) return;
    std::string error;
    if (!engine_.setMonitoring(want, error)) {
        monitorTold_ = true;
        message("Monitoring: the input can't be heard: " + error, 8000);
        util::debugf("recording: monitoring can't be had: %s\n", error.c_str());
    }
}

void Recording::midiNote(int pitch, int velocity, const std::function<bool(const Track&)>& to) {
    if (midi_.empty()) return;
    double heard = -1;
    if (!engine_.heardNow(heard)) heard = -1;  // (counting in: where the recording starts)
    double at = unroll(heard);
    for (auto& m : midi_)
        if (!m.processed && (!to || (m.track && to(*m.track))))  // (recording the effects' notes: poll())
            m.take.event(velocity > 0, pitch, velocity, at);
}

void Recording::midiNoteAt(int pitch, int velocity, double heardFrame) {
    // a note from the MIDI keyboard while recording: during the count-in it starts where the recording does
    double at = unroll(heardFrame);
    for (auto& m : midi_) m.take.event(velocity > 0, pitch, velocity, at);
}

ClipPtr Recording::placeMidi(MidiRec& rec, double end) {
    Project& project = engine_.project;
    int sr = engine_.sr;
    double lengthS = 0;  // (notes still held end where recording stopped)
    std::vector<Note> notes = rec.take.finish(end, lengthS);
    const TrackPtr& track = rec.track;
    if (notes.empty() || !track || !project.tracks.contains(track)) {
        message("No notes were played", 5000);
        return nullptr;
    }
    size_t count = notes.size();
    auto place = [&](std::vector<Note> ns, double seconds, int64_t at) {
        takes++;
        ClipPtr clip = makeMidiClip("Recording " + std::to_string(takes), std::move(ns), seconds, sr);
        if (host.placeClip) host.placeClip(clip, at, track);
        else recording::placeClip(project, clip, at, track, sr);
        keepTake(track, clip);
        return clip;
    };
    // the passes laid end to end (unroll): each note's pass, and where that is on the timeline
    int64_t first = rec.take.start;
    int64_t last = first + (int64_t)std::llround(lengthS * sr);
    auto passOf = [&](int64_t u) -> int64_t {
        if (!loop_ || u < loop_->end) return 0;
        return 1 + (u - loop_->end) / (loop_->end - loop_->start);
    };
    if (!loop_ || passOf(first) == passOf(std::max(first, last - 1))) {  // (never went round)
        int64_t at = passOf(first) ? loop_->start + (first - loop_->end) % (loop_->end - loop_->start) : first;
        ClipPtr clip = place(std::move(notes), lengthS, at);
        message("Recorded " + std::to_string(count) + " notes onto " + track->name, 5000);
        return clip;
    }
    const int64_t len = loop_->end - loop_->start;
    auto passEnd = [&](int64_t p) { return loop_->end + p * len; };       // (unrolled)
    auto timelineOf = [&](int64_t u) { return u < loop_->end ? u : loop_->start + (u - loop_->end) % len; };
    bool merge = !(host.prefs && host.prefs->midiLoopTakes);
    if (merge) {  // every pass's notes in one clip over the loop
        int64_t from = std::min(timelineOf(first), loop_->start);
        std::vector<Note> merged;
        for (auto& n : notes) {
            int64_t u = first + (int64_t)std::llround(n.start * sr);
            double cut = (double)(passEnd(passOf(u)) - first) / sr;  // (a note crossing the loop's end ends there)
            double at = (double)(timelineOf(u) - from) / sr;
            merged.push_back({at, at + std::min(n.end, cut) - n.start, n.pitch, n.velocity});
        }
        std::sort(merged.begin(), merged.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
        ClipPtr clip = place(std::move(merged), (double)(loop_->end - from) / sr, from);
        message("Recorded " + std::to_string(count) + " notes round the loop onto " + track->name +
                    " (merged: every pass in one clip)",
                8000);
        return clip;
    }
    // Takes: each pass a take of its own (passes without notes left out), placed in turn
    std::map<int64_t, std::vector<Note>> passes;
    for (auto& n : notes) {
        int64_t u = first + (int64_t)std::llround(n.start * sr);
        int64_t p = passOf(u);
        int64_t from = std::max(first, p ? passEnd(p - 1) : first);
        double cut = (double)(passEnd(p) - first) / sr;
        double at = (double)(u - from) / sr;
        passes[p].push_back({at, at + std::min(n.end, cut) - n.start, n.pitch, n.velocity});
    }
    ClipPtr clip;
    for (auto& [p, ns] : passes) {
        int64_t from = std::max(first, p ? passEnd(p - 1) : first);
        int64_t to = std::min(std::max(last, from + 1), passEnd(p));
        clip = place(std::move(ns), (double)(to - from) / sr, timelineOf(from));
    }
    message("Recorded " + std::to_string(passes.size()) + " takes round the loop onto " + track->name +
                ": the last plays; all are in its takes, under it, to comp from",
            8000);
    return clip;
}
