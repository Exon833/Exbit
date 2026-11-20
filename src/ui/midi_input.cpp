// Playing MIDI keyboards into Tiny DAW (midi_input.py's GUI half).
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

namespace midi_input {

std::vector<std::pair<int, int>> throughEffects(const Track& track, int pitch, int velocity) {
    return midi_effects::live(track, pitch, velocity);
}

std::vector<int> playLive(audio::Engine& engine, const TrackPtr& track, int pitch, int velocity, double lengthS) {
    std::vector<int> pitches;
    if (!track) return pitches;
    auto live = engine.liveNotes(track.get());
    bool builtin = !track->playsPlugins();
    live::NoteRender render = subsynth::noteRenderer(*track);  // (its synth patch or sampler; empty: the classic synth)
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

// =================================================================================================

MidiInput::MidiInput(audio::Engine& engine_, Project& project_, std::function<TrackPtr()> currentTrack_,
                     std::function<void(const std::string&, int)> status_)
    : engine(engine_), project(project_), currentTrack(std::move(currentTrack_)), status(std::move(status_)) {}

MidiInput::~MidiInput() { close(); }

double MidiInput::now() const { return clock ? clock() : plat::now(); }

bool MidiInput::open(const std::string& name) {
    name_ = name;
    if (name.empty()) return true;
    hub.setOn(name, true, true);
    const midi_ports::Port* p = hub.find(name, true);
    bool ok = p && p->open;
    util::debugf("midi: input \"%s\" %s\n", name.c_str(), ok ? "open" : "isn't connected now");
    return ok;
}

void MidiInput::close() {
    releaseAll();
    hub.stop();
}

std::string MidiInput::portName(int slot) const {
    const midi_ports::Port* p = hub.bySlot(slot);
    return p ? p->key : std::string();
}

static bool playable(const TrackPtr& track, const Project& project) {
    return track && track->kind == Kind::Midi && !track->needsInstrument && !std::atomic_load(&track->frozen) &&
           project.tracks.contains(track);  // (removed meanwhile)
}

TrackPtr MidiInput::target() const {
    TrackPtr track = currentTrack ? currentTrack() : nullptr;
    return playable(track, project) ? track : nullptr;
}

std::vector<TrackPtr> MidiInput::targets(int slot, int channel) const {
    std::vector<TrackPtr> out;
    TrackPtr selected = target();
    if (slot < 0) {  // (the computer keyboard: the selected track)
        if (selected) out.push_back(selected);
        return out;
    }
    if (selected && selected->midiIn.empty()) out.push_back(selected);
    std::string port = portName(slot);
    for (auto& t : project.tracks)
        if (t != selected && !t->midiIn.empty() && t->midiIn == port &&
            (t->midiInChannel == 0 || t->midiInChannel == channel + 1) && playable(t, project))
            out.push_back(t);
    if (selected && !selected->midiIn.empty() && selected->midiIn == port &&
        (selected->midiInChannel == 0 || selected->midiInChannel == channel + 1))
        out.insert(out.begin(), selected);
    return out;
}

midi_in::Sustain& MidiInput::sustainOf(int slot) { return slot < 0 ? sustain : sustains_[slot]; }

void MidiInput::handle(const midi_in::Event& e) {
    if (e.type >= midi_in::Event::Clock) {  // (MIDI clock and the like: the transport's, midi_sync.h)
        const midi_ports::Port* p = hub.bySlot(e.port);
        if (realtime && p && p->clock) realtime(e);
        return;
    }
    // every controller's latest value, for the MIDI Control modulator and links to a controller (midi_cc.h)
    if (e.type == midi_in::Event::Control) midi_cc::control(e.data1, e.data2);
    if (e.type == midi_in::Event::PitchBend) midi_cc::pitchBend(e.bend);
    if (e.type == midi_in::Event::Control && controlHook && controlHook(e)) return;  // (MIDI learn's: midi_learn.h)
    out_.clear();
    sustainOf(e.port).handle(e, out_);
    std::vector<midi_in::Event> played = out_;  // (play() may be re-entered by the recorder)
    for (midi_in::Event p : played) {
        p.port = e.port;  // (what the pedal let go of: from the same port)
        play(p);
    }
}

void MidiInput::poll() {
    hub.tick(now());
    events_.clear();
    lost_.clear();
    hub.poll(events_, &lost_);
    for (const midi_in::Event& e : events_) handle(e);
    for (int slot : lost_) {
        releasePort(slot);
        if (status)
            status("MIDI input \xe2\x80\x9c" + portName(slot) +
                       "\xe2\x80\x9d was disconnected: it is listened to again when it comes back (Options \xe2\x86\x92 MIDI ports\xe2\x80\xa6)",
                   5000);
    }
}

void MidiInput::sendExternal(const TrackPtr& track, uint8_t status_, int d1, int d2) {
    const midi_ports::Port* p = hub.find(track->ext.port, false);
    if (!p || !p->open) return;
    uint8_t m[3] = {(uint8_t)(status_ | ((track->ext.channel - 1) & 15)), (uint8_t)(d1 & 127), (uint8_t)(d2 & 127)};
    hub.sendNow(p->slot, m, 3);
}

void MidiInput::play(const midi_in::Event& e) {
    if (e.isNote()) {
        if (recorder) recorder(e);
        noteDown(e);
    }
    int key = heldKey(e);
    if (e.type == midi_in::Event::NoteOn && e.data2 > 0) {
        std::vector<TrackPtr> tracks = targets(e.port, e.channel);
        if (tracks.empty()) {
            if (status) status("To play notes, select a MIDI track that has an instrument (click its lane or row)", 3000);
            return;
        }
        if (held_.count(key)) letGo(key);
        auto& held = held_[key];
        for (const TrackPtr& track : tracks) {
            if (track->ext.on) {  // (an External synth: the note to it, now)
                sendExternal(track, 0x90, e.data1, e.data2);
                held.push_back({track, {e.data1}});
            } else {
                held.push_back({track, playLive(engine, track, e.data1, e.data2)});  // (through its MIDI effects)
            }
        }
    } else if (e.isNote()) {
        letGo(key);
    } else if (e.type == midi_in::Event::Control &&
               (e.data1 == midi_in::ALL_SOUND_OFF || e.data1 == midi_in::ALL_NOTES_OFF)) {
        releasePort(e.port);
    } else if (e.type == midi_in::Event::PitchBend || e.type == midi_in::Event::Control) {
        controllers(e);
    }
}

// --- the bend wheel and the mod wheel (live) ---

void MidiInput::controllers(const midi_in::Event& e) {
    TrackPtr track = target();
    for (const TrackPtr& t : targets(e.port, e.channel))  // (External synths: every control and bend, as played)
        if (t->ext.on) {
            if (e.type == midi_in::Event::PitchBend) sendExternal(t, 0xE0, (e.bend + 8192) & 127, (e.bend + 8192) >> 7);
            else sendExternal(t, 0xB0, e.data1, e.data2);
        }
    if (track && track->ext.on) return;
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

void MidiInput::letGo(int key) {
    auto it = held_.find(key);
    if (it == held_.end()) return;
    auto held = it->second;
    held_.erase(it);
    for (auto& [track, pitches] : held) {
        if (track->ext.on) {
            for (int p : pitches) sendExternal(track, 0x80, p, 0);
            continue;
        }
        auto live = engine.liveNotes(track.get());
        for (int p : pitches) live->noteOff(p);
    }
}

void MidiInput::releasePort(int slot) {
    out_.clear();
    sustainOf(slot).releaseAll(out_, now());  // (the pedal's notes: their note-offs, for recording and Capture)
    std::vector<midi_in::Event> played = out_;
    for (midi_in::Event p : played) {
        p.port = (int16_t)slot;
        play(p);
    }
    std::vector<int> keys;  // (anything left from that port: let go without a message)
    for (auto& kv : held_)
        if (kv.first / 128 - 1 == slot) keys.push_back(kv.first);
    for (int k : keys) letGo(k);
}

void MidiInput::releaseAll() {
    unbend();  // (closing, unplugged: the wheel can't bring it back)
    std::vector<int> slots = {-1};
    for (auto& kv : sustains_) slots.push_back(kv.first);
    for (int slot : slots) releasePort(slot);
    std::vector<int> keys;  // (anything left: let go without a message)
    for (auto& kv : held_) keys.push_back(kv.first);
    for (int k : keys) letGo(k);
}

}  // namespace midi_input
