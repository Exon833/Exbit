#include "audio/plugin_audio.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "audio/patch.h"

namespace plugin_audio {

bool usable(const PluginSlot* slot) {
    return slot && aload(slot->instance) && !slot->failed.load(std::memory_order_acquire);
}

bool active(const PluginSlot* slot) {
    return usable(slot) && !slot->bypass && !slot->lowLatencyBypass.load(std::memory_order_relaxed);
}

int latency(const PluginSlot& slot) {
    auto inst = aload(slot.instance);
    return inst ? std::max(0, inst->latency()) : 0;
}

void ensureRate(plug::Plugin& p, int sr) {
    if (p.sampleRate() != sr) p.prepare(sr);
}

void fail(PluginSlot& slot, const char* what) {
    if (slot.failed.load()) return;
    try {
        slot.error = what && *what ? what : "error";
    } catch (...) {
    }
    slot.failed.store(true, std::memory_order_release);
}

std::string whyStopped(const PluginSlot& slot) {
    const std::string& e = slot.error;
    size_t n = slot.name.size();
    if (n && e.size() > n + 1 && e.compare(0, n, slot.name) == 0 && e[n] == ' ') return e.substr(n + 1);
    return e;
}

void syncState(PluginSlot& slot, int sr, int frames) {
    if (!usable(&slot)) return;
    auto inst = aload(slot.instance);
    std::vector<float> buf((size_t)frames * 4, 0.0f);
    float* out[2] = {buf.data(), buf.data() + frames};
    const float* in[2] = {buf.data() + 2 * frames, buf.data() + 3 * frames};
    try {
        ensureRate(*inst, sr);
        if (slot.kind == INSTRUMENT)
            inst->render({}, out, frames);
        else
            inst->process(in, nullptr, out, frames);
    } catch (const std::exception& e) {  // (as when playing: the plugin is switched off)
        fail(slot, e.what());
    } catch (...) {
        fail(slot, "unknown error");
    }
}

bool processEffect(PluginSlot& slot, const float* const in[2], const float* const side[2], float* const out[2],
                   int frames, int sr, const Transport& t) {
    auto inst = aload(slot.instance);
    if (!inst) return false;
    try {
        ensureRate(*inst, sr);
        inst->setTransport(t.tempo, t.numerator, t.denominator, t.playing, t.position, t.beats);
        inst->process(in, side, out, frames);
        return true;
    } catch (const std::exception& e) {  // a failing plugin is switched off, never the whole audio stream
        fail(slot, e.what());
    } catch (...) {
        fail(slot, "unknown error");
    }
    return false;
}

bool inList(const Track* t, const TrackList& tracks) {
    for (auto& x : tracks)
        if (x.get() == t) return true;
    return false;
}

int trackLatency(const Track& track, const TrackList& tracks, const Seen* seen) {
    if (aload(track.frozen) || (seen && seen->contains(&track))) return 0;
    Seen me{&track, seen};
    auto cables = track.cables.get();
    int in = instrumentLatency(track);
    if (patch::isStraight(*cables)) return in;
    auto effects = track.effects.get();
    return patch::patchLatency(cables, *effects, in, tracks, &me, track.patchState);
}

int inputLatency(const Track& track, const PluginSlot* slot, const TrackList& tracks) {
    if (!slot || aload(track.frozen)) return 0;
    auto cables = track.cables.get();
    if (patch::isStraight(*cables)) return 0;  // (no effect heard)
    auto effects = track.effects.get();
    Seen me{&track, nullptr};
    return patch::planLatency(*patch::planFor(cables, track.patchState), *effects, instrumentLatency(track), tracks,
                              &me, slot);
}

bool playsPlugins(const Track& track) {
    return track.kind == Kind::Midi && !track.needsInstrument && !track.ext.on &&
           (track.usesRack || usable(aload(track.instrument)));
}

bool isFed(const PluginSlot* slot, const std::vector<Cable>& rc) {
    for (auto& c : rc)
        if (c.first == End::in() && c.second.type == End::Slot && c.second.slot == slot) return true;
    return false;
}

bool isHeard(const Track& track, const PluginSlot* slot, const std::vector<Cable>& rc) {
    if (track.kind != Kind::Midi || track.needsInstrument || !usable(slot)) return false;
    if (!track.usesRack) return aload(track.instrument).get() == slot;
    if (slot->bypass) return false;
    for (auto& c : rc)
        if (c.first.type == End::Slot && c.first.slot == slot && c.second == End::out()) return true;
    return false;
}

std::vector<SlotPtr> heardInstruments(const Track& track) {
    std::vector<SlotPtr> out;
    auto rc = track.rackCables.get();
    for (auto& s : track.instruments())
        if (isHeard(track, s.get(), *rc)) out.push_back(s);
    return out;
}

int instrumentLatency(const Track& track) {
    if (aload(track.frozen)) return 0;
    if (track.kind != Kind::Midi || track.needsInstrument) return 0;
    if (!track.usesRack) {
        auto inst = aload(track.instrument);
        return usable(inst) ? latency(*inst) : 0;
    }
    auto rack = track.rack.get();
    auto rc = track.rackCables.get();
    int late = 0;
    for (auto& s : *rack)
        if (isHeard(track, s.get(), *rc)) late = std::max(late, latency(*s));
    return late;
}

bool playInstrument(PluginSlot& slot, const MidiMsg* messages, size_t count, float* const out[2], int channels,
                    int frames, int sr, NoteScratch& scratch, const Transport& t) {
    auto inst = aload(slot.instance);
    if (!inst || frames <= 0) return false;
    try {
        scratch.notes.clear();
        for (size_t i = 0; i < count; i++) {
            const MidiMsg& m = messages[i];
            if (m.type == MidiMsg::AllOff) {
                inst->notesOff();
            } else {
                plug::NoteMessage n;
                n.frame = (int)std::min<int64_t>(frames - 1, std::max<int64_t>(0, m.frame));
                n.on = m.type == MidiMsg::NoteOn && m.velocity > 0;
                n.pitch = m.pitch, n.velocity = m.velocity, n.channel = m.channel;
                if (m.type == MidiMsg::PitchBend || m.type == MidiMsg::Control) {  // (the bend wheel, a control)
                    n.on = false;
                    n.velocity = 0;
                    n.kind = m.type == MidiMsg::PitchBend ? plug::NoteMessage::PitchBend : plug::NoteMessage::Control;
                    n.value = m.value;
                }
                // (sorted by frame: insertion keeps equal frames in their order and needs no memory)
                scratch.notes.push_back(n);
                for (size_t k = scratch.notes.size() - 1; k > 0 && scratch.notes[k - 1].frame > scratch.notes[k].frame; k--)
                    std::swap(scratch.notes[k - 1], scratch.notes[k]);
            }
        }
        if ((int)scratch.audio.size() < frames * 2) scratch.audio.assign((size_t)frames * 2, 0.0f);  // (rare)
        float* audio[2] = {scratch.audio.data(), scratch.audio.data() + frames};
        std::memset(audio[0], 0, sizeof(float) * frames * 2);
        ensureRate(*inst, sr);
        inst->setTransport(t.tempo, t.numerator, t.denominator, t.playing, t.position, t.beats);
        inst->render(scratch.notes, audio, frames);
        for (int c = 0; c < channels && c < 2; c++)
            for (int i = 0; i < frames; i++) out[c][i] += audio[c][i];
        return true;
    } catch (const std::exception& e) {  // a failing plugin is switched off, never the whole audio stream
        fail(slot, e.what());
    } catch (...) {
        fail(slot, "unknown error");
    }
    return false;
}

bool soundsOn(const Clip& clip, int pitch, int64_t at) {
    auto ev = aload(clip.events);
    if (!ev) return false;
    int64_t lo = clip.offset, hi = clip.offset + clip.frames();
    for (auto& e : *ev) {
        if (e.pitch != pitch || !(e.on >= lo && e.on < hi)) continue;
        int64_t tOn = clip.start + e.on - lo, tOff = clip.start + std::min(e.off, hi) - lo;
        if (tOn < at && tOff >= at) return true;
    }
    return false;
}

void noteMessages(const Clip* const* clips, size_t count, bool hasStart, int64_t start, int frames, bool restart,
                  Messages& out, std::vector<NoteEvent>& events, uint16_t* held) {
    out.clear();
    events.clear();
    if (restart) {
        MidiMsg m;
        m.type = MidiMsg::AllOff;
        out.push_back(m);
    }
    if (held && (restart || !hasStart)) std::fill(held, held + 128, (uint16_t)0);
    if (!hasStart) return;
    if (held)
        for (int p = 0; p < 128; p++) {
            if (!held[p]) continue;
            bool sounds = false;
            for (size_t k = 0; k < count && !sounds; k++) sounds = soundsOn(*clips[k], p, start);
            if (!sounds) {
                events.push_back({0, 0, p, 0});
                held[p] = 0;
            }
        }
    int64_t end = start + frames;
    for (size_t k = 0; k < count; k++) {
        const Clip& clip = *clips[k];
        auto ev = aload(clip.events);
        if (!ev) continue;
        int64_t lo = clip.offset, hi = clip.offset + clip.frames();
        for (auto& e : *ev) {
            if (!(e.on >= lo && e.on < hi)) continue;  // notes that start within the clip's window
            int64_t tOn = clip.start + e.on - lo;
            int64_t tOff = clip.start + std::min(e.off, hi) - lo;
            if (tOn >= start && tOn < end) events.push_back({tOn - start, 1, e.pitch, std::max(1, e.velocity)});
            if (tOff >= start && tOff < end) events.push_back({tOff - start, 0, e.pitch, 0});
        }
    }
    // by time; note-offs before note-ons at the same moment
    std::sort(events.begin(), events.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.t != b.t) return a.t < b.t;
        if (a.on != b.on) return a.on < b.on;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.velocity < b.velocity;
    });
    for (auto& e : events) {
        MidiMsg m;
        m.type = e.on ? MidiMsg::NoteOn : MidiMsg::NoteOff;
        m.frame = e.t, m.pitch = e.pitch, m.velocity = e.velocity;
        out.push_back(m);
        if (held && e.pitch >= 0 && e.pitch < 128) {
            if (e.on)
                held[e.pitch] = (uint16_t)std::min(65535, held[e.pitch] + 1);
            else if (held[e.pitch])
                held[e.pitch]--;
        }
    }
}

Transport Transport::at(const TempoMap& map, bool playing, int64_t start, int sr) {
    Transport t;
    double seconds = playing ? (double)start / sr : 0.0;
    t.tempo = map.bpmAt(seconds);
    TimeSig sig = map.sigAt(seconds);
    t.numerator = sig.num;
    t.denominator = sig.den;
    t.playing = playing;
    t.position = playing ? start : 0;
    t.beats = playing ? std::max(0.0, map.beatAt(seconds)) : 0.0;
    return t;
}

Messages noteMessages(const std::vector<ClipPtr>& clips, bool hasStart, int64_t start, int frames, bool restart) {
    std::vector<const Clip*> ptrs;
    for (auto& c : clips) ptrs.push_back(c.get());
    Messages out;
    std::vector<NoteEvent> events;
    noteMessages(ptrs.data(), ptrs.size(), hasStart, start, frames, restart, out, events);
    return out;
}

}  // namespace plugin_audio
