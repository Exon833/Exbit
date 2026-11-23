// Following MIDI clock and JACK's transport (midi_sync.h).
#include "audio/midi_sync.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/util.h"

namespace midi_sync {

constexpr size_t PULSES_KEPT = 48;  // 2 beats

const char* modeName(Mode m) {
    switch (m) {
        case Mode::MidiClock: return "MIDI clock";
        case Mode::Jack: return "JACK";
        default: return "Internal";
    }
}

Mode modeFrom(const std::string& name) {
    if (name == "MIDI clock") return Mode::MidiClock;
    if (name == "JACK") return Mode::Jack;
    return Mode::Internal;
}

// --- MIDI clock ---

void ClockFollower::reset() {
    pulses_.clear();
    running_ = false;
    songPos_ = 0;
    located_ = false;
}

void ClockFollower::event(const midi_in::Event& e, std::vector<Action>& out) {
    using midi_in::Event;
    switch (e.type) {
        case Event::Clock:
            if (!pulses_.empty() && e.time - pulses_.back() > 1.0) pulses_.clear();  // (a pause: the tempo afresh)
            pulses_.push_back(e.time);
            while (pulses_.size() > PULSES_KEPT) pulses_.pop_front();
            break;
        case Event::Start:
            running_ = true;
            songPos_ = 0;
            located_ = false;
            out.push_back({Action::Locate, 0});
            out.push_back({Action::Play, 0});
            break;
        case Event::Continue:  // (from the Song Position received since it stopped; else on from where it is)
            running_ = true;
            if (located_) out.push_back({Action::Locate, songPos_});
            located_ = false;
            out.push_back({Action::Play, 0});
            break;
        case Event::Stop:
            running_ = false;
            out.push_back({Action::Stop, 0});
            break;
        case Event::SongPos:
            songPos_ = e.bend / 4.0;  // (16ths)
            located_ = !running_;
            if (running_) out.push_back({Action::Locate, songPos_});
            break;
        default: break;
    }
}

double ClockFollower::bpm() const {
    size_t n = pulses_.size();
    if (n < 12) return 0;  // (half a beat at least)
    // least squares: time = a + b·i
    double si = 0, st = 0, sii = 0, sit = 0, t0 = pulses_.front();
    for (size_t i = 0; i < n; i++) {
        double t = pulses_[i] - t0;
        si += (double)i, st += t, sii += (double)i * i, sit += (double)i * t;
    }
    double d = n * sii - si * si;
    if (d <= 0) return 0;
    double perPulse = (n * sit - si * st) / d;
    if (!(perPulse > 0)) return 0;
    return 60.0 / (perPulse * 24.0);
}

// --- JACK ---

void JackFollower::tick(JackTransport& t, const Host& h, double now) {
    JackState s;
    if (!t.query(s) || s.rate <= 0) return;
    int64_t jf = (int64_t)std::llround((double)s.frame * h.sr / s.rate);  // (JACK's frame at our rate)
    bool playing = h.playing();
    int64_t pos = h.pos();
    const int64_t tol = (int64_t)(JACK_TOLERANCE_S * h.sr);
    if (known_) {
        int64_t moved = (int64_t)((now - at_) * h.sr);
        bool jackMoved = s.rolling != rolling_ || std::llabs(jf - (jack_ + (rolling_ ? moved : 0))) > tol;
        bool weMoved = playing != playing_ || std::llabs(pos - (pos_ + (playing_ ? moved : 0))) > tol;
        if (weMoved && !jackMoved) {  // Tiny DAW drives JACK
            if (std::llabs(pos - jf) > tol) {
                util::debugf("sync: JACK located to %.3f s\n", (double)pos / h.sr);
                t.locate((int64_t)std::llround((double)pos * s.rate / h.sr));
            }
            if (playing != s.rolling) {
                util::debugf("sync: JACK %s\n", playing ? "started" : "stopped");
                playing ? t.start() : t.stop();
            }
        } else if (jackMoved) {  // JACK drives Tiny DAW
            if (std::llabs(jf - pos) > tol) h.seek(jf);
            if (s.rolling != playing) {
                util::debugf("sync: JACK %s, so does Tiny DAW\n", s.rolling ? "rolls" : "stopped");
                s.rolling ? h.play() : h.stop();
            }
        } else if (s.rolling && playing && std::llabs(jf - pos) > tol) {
            h.seek(jf);  // (the two drifted apart)
        }
    } else if (s.rolling != playing || std::llabs(jf - pos) > tol) {  // (the first look: JACK's as it is)
        if (std::llabs(jf - pos) > tol) h.seek(jf);
        if (s.rolling != playing) s.rolling ? h.play() : h.stop();
    }
    if (s.bbt && !t.master() && s.bpm > 0 && h.tempo) h.tempo(s.bpm);
    known_ = true;
    rolling_ = s.rolling;
    jack_ = jf;
    playing_ = h.playing();
    pos_ = h.pos();
    at_ = now;
    // (what we asked JACK for shows at its next cycle: until then it looks as before, and isn't followed back)
    if (playing_ != rolling_ || std::llabs(pos_ - jack_) > tol) rolling_ = playing_, jack_ = pos_;
}

void fillBBT(const TempoMap& map, uint32_t frame, uint32_t rate, jack::Position& pos) {
    double seconds = rate ? (double)frame / rate : 0.0;
    double beat = map.beatAt(seconds);
    int bar = map.barAtBeat(beat);
    // (JACK's beats are the signature's unit, `beat_type`: 8ths in 6/8, so its tempo is in them too)
    TimeSig sig = map.sigAt(seconds);
    double unit = 4.0 / sig.den;
    double barBeat = map.barBeat(bar), into = std::max(0.0, beat - barBeat) / unit;
    double whole = std::floor(into);
    pos.valid |= jack::PositionBBT;
    pos.bar = bar + 1;
    pos.beat = (int32_t)whole + 1;
    pos.tick = (int32_t)std::min(TICKS_PER_BEAT - 1, std::floor((into - whole) * TICKS_PER_BEAT));
    pos.bar_start_tick = barBeat / unit * TICKS_PER_BEAT;
    pos.beats_per_bar = (float)sig.num;
    pos.beat_type = (float)sig.den;
    pos.ticks_per_beat = TICKS_PER_BEAT;
    pos.beats_per_minute = map.bpmAt(seconds) / unit;
}

}  // namespace midi_sync
