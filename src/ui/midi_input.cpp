// Playing a MIDI keyboard into Tiny DAW (midi_input.py's GUI half).
#include "ui/midi_input.h"

#include <algorithm>
#include <cmath>

#include "audio/midi_cc.h"
#include "audio/live_fx.h"
#include "audio/midi_effects.h"
#include "audio/sampler.h"
#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"
#include "ui/capture.h"
#include "ui/toolkit/dialogs.h"

namespace midi_input {

std::vector<std::pair<int, int>> throughEffects(const Track& track, int pitch, int velocity) {
    return midi_effects::live(track, pitch, velocity);
}

std::vector<int> playLive(audio::Engine& engine, const TrackPtr& track, int pitch, int velocity, double lengthS) {
    std::vector<int> pitches;
    if (!track) return pitches;
    auto live = engine.liveNotes(track.get());
    bool builtin = !track->playsPlugins();
    live::NoteRender render = sampler::noteRenderer(*track);  // (its sampler's; empty: the built-in synth)
    const live::NoteRender* how = builtin && render.render ? &render : nullptr;
    if (live_fx::timed(*track)) {  // (an arpeggiator, a groove…: the audio thread runs the whole chain, in time)
        live->useEffects(*track, builtin, engine.sr, pitch, how);
        if (live->keyOn(pitch, velocity, engine.sr, lengthS)) pitches.push_back(pitch);
        return pitches;
    }
    for (auto& pv : throughEffects(*track, pitch, velocity)) {
        live->noteOn(pv.first, pv.second, engine.sr, builtin, lengthS, how);
        pitches.push_back(pv.first);
    }
    return pitches;
}

std::string savedName() { return settings::get("options/midi_input").toString(""); }
void saveName(const std::string& name) { settings::set("options/midi_input", Json(name)); }

// =================================================================================================

MidiInput::MidiInput(audio::Engine& engine_, Project& project_, std::function<TrackPtr()> currentTrack_,
                     std::function<void(const std::string&, int)> status_)
    : engine(engine_), project(project_), currentTrack(std::move(currentTrack_)), status(std::move(status_)) {}

MidiInput::~MidiInput() { close(); }

double MidiInput::now() const { return clock ? clock() : plat::now(); }

bool MidiInput::open(const std::string& name) {
    close();
    name_ = name;
    lostReported_ = false;
    if (name.empty()) return true;
    bool ok = input.open(name);
    util::debugf("midi: input \"%s\" %s\n", name.c_str(), ok ? "open" : "can't be opened");
    return ok;
}

void MidiInput::close() {
    releaseAll();
    input.close();
}

TrackPtr MidiInput::target() const {
    TrackPtr track = currentTrack ? currentTrack() : nullptr;
    if (!track || track->kind != Kind::Midi || track->needsInstrument || std::atomic_load(&track->frozen) ||
        !project.tracks.contains(track))  // (removed meanwhile)
        return nullptr;
    return track;
}

void MidiInput::handle(const midi_in::Event& e) {
    // every controller's latest value, for the MIDI Control modulator and links to a controller (midi_cc.h)
    if (e.type == midi_in::Event::Control) midi_cc::control(e.data1, e.data2);
    if (e.type == midi_in::Event::PitchBend) midi_cc::pitchBend(e.bend);
    if (e.type == midi_in::Event::Control && controlHook && controlHook(e)) return;  // (MIDI learn's: midi_learn.h)
    out_.clear();
    sustain.handle(e, out_);
    std::vector<midi_in::Event> played = out_;  // (play() may be re-entered by the recorder)
    for (const midi_in::Event& p : played) play(p);
}

void MidiInput::poll() {
    events_.clear();
    input.poll(events_);
    for (const midi_in::Event& e : events_) handle(e);
    if (input.lost() && !lostReported_) {
        lostReported_ = true;
        releaseAll();
        if (status) status("MIDI input \xe2\x80\x9c" + name_ + "\xe2\x80\x9d was disconnected (Options \xe2\x86\x92 MIDI input\xe2\x80\xa6)", 5000);
    }
}

void MidiInput::play(const midi_in::Event& e) {
    if (e.isNote()) {
        if (recorder) recorder(e);
        noteDown(e);
    }
    if (e.type == midi_in::Event::NoteOn && e.data2 > 0) {
        TrackPtr track = target();
        if (!track) {
            if (status) status("To play notes, select a MIDI track that has an instrument (click its lane or row)", 3000);
            return;
        }
        if (held_.count(e.data1)) letGo(e.data1);
        held_[e.data1] = {track, playLive(engine, track, e.data1, e.data2)};  // (through its MIDI effects)
    } else if (e.isNote()) {
        letGo(e.data1);
    } else if (e.type == midi_in::Event::Control &&
               (e.data1 == midi_in::ALL_SOUND_OFF || e.data1 == midi_in::ALL_NOTES_OFF)) {
        std::vector<int> keys;
        for (auto& kv : held_) keys.push_back(kv.first);
        for (int k : keys) letGo(k);
    } else if (e.type == midi_in::Event::PitchBend || e.type == midi_in::Event::Control) {
        controllers(e);
    }
}

// --- the bend wheel and the mod wheel (live) ---

void MidiInput::controllers(const midi_in::Event& e) {
    TrackPtr track = target();
    if (e.type == midi_in::Event::PitchBend) {
        if (bent_ && bent_ != track) unbend();  // (another track selected: the one bent before goes straight)
        if (!track) return;
        int range = std::max(1, std::min(24, track->bendRange));
        engine.liveNotes(track.get())->pitchBend(e.bend, live::bendSemitones(e.bend, range));
        bent_ = e.bend ? track : nullptr;
    } else if (e.data1 == midi_cc::MOD_WHEEL && track && track->playsPlugins()) {
        engine.liveNotes(track.get())->control(e.data1, e.data2);  // (its instrument plugins use it themselves)
    }
}

void MidiInput::unbend() {
    TrackPtr track = bent_;
    bent_ = nullptr;
    if (track && project.tracks.contains(track)) engine.liveNotes(track.get())->pitchBend(0, 0.0);
}

void MidiInput::noteDown(const midi_in::Event& e) {
    double t = now();
    while (!log.empty() && t - log.front().clock > capture::KEPT_S) log.pop_front();
    while (log.size() >= LOG_NOTES) log.pop_front();
    LogEntry entry;
    entry.clock = t;
    if (engine.playing) entry.heard = engine.heardPos();
    entry.on = e.type == midi_in::Event::NoteOn && e.data2 > 0;
    entry.pitch = e.data1;
    entry.velocity = e.data2;
    log.push_back(entry);
}

void MidiInput::letGo(int note) {
    auto it = held_.find(note);
    if (it == held_.end()) return;
    auto track = it->second.first;
    std::vector<int> pitches = it->second.second;
    held_.erase(it);
    auto live = engine.liveNotes(track.get());
    for (int p : pitches) live->noteOff(p);
}

void MidiInput::releaseAll() {
    unbend();  // (closing, unplugged: the wheel can't bring it back)
    out_.clear();
    sustain.releaseAll(out_, now());
    std::vector<midi_in::Event> played = out_;
    for (const midi_in::Event& p : played) play(p);
    std::vector<int> keys;  // (anything left: let go without a message)
    for (auto& kv : held_) keys.push_back(kv.first);
    for (int k : keys) letGo(k);
}

void chooseInput(ui::Window* parent, MidiInput& input) {
    std::vector<std::string> names;
    for (const midi_in::PortInfo& p : midi_in::ports()) names.push_back(p.stable);
    std::vector<std::string> choices = {"None"};
    choices.insert(choices.end(), names.begin(), names.end());
    int index = 0;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == input.name()) index = (int)i + 1;
    if (!ui::getItem(parent, "MIDI input",
                     names.empty() ? "No MIDI inputs found (is the keyboard plugged in?)"
                                   : "MIDI keyboard to play into the selected MIDI track:",
                     choices, index))
        return;
    std::string name = index <= 0 || index >= (int)choices.size() ? "" : choices[index];
    if (input.open(name)) {
        saveName(name);
        if (!name.empty() && input.status) input.status("MIDI input: " + name + " plays into the selected MIDI track", 5000);
    } else {
        ui::msg::warning(parent, "MIDI input", name + " could not be opened.");
    }
}

}  // namespace midi_input
