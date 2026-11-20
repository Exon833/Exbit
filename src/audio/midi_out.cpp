// MIDI out from the audio thread: clock, Start/Stop/Song Position, External synth notes (midi_out.h).
#include "audio/midi_out.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "model/timing.h"

namespace midi_out {

Out::Out() : ring_(16384) {
    wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    clips_.reserve(256);
    messages_.reserve(4096);
    events_.reserve(4096);
    std::atomic_store(&config_, std::shared_ptr<const Config>(std::make_shared<Config>()));
}

Out::~Out() {
    if (wake_ >= 0) ::close(wake_);
}

void Out::publish() {
    auto c = std::make_shared<Config>();
    c->clocks = clocks_;
    c->tracks = tracksSet_;
    std::shared_ptr<const Config> old = std::atomic_load(&config_);
    kept_.push_back(old);  // (the audio thread may hold it this very moment: it never frees one)
    if (kept_.size() > 64) kept_.erase(kept_.begin(), kept_.begin() + 32);  // (long since let go)
    std::atomic_store(&config_, std::shared_ptr<const Config>(c));
}

void Out::setClocks(std::vector<ClockPort> clocks) {
    clocks_ = std::move(clocks);
    publish();
}

void Out::setTracks(std::vector<ExternalTrack> tracks) {
    auto same = [](const ExternalTrack& a, const ExternalTrack& b) {
        return a.track == b.track && a.slot == b.slot && a.channel == b.channel;
    };
    if (tracks.size() == tracksSet_.size() && std::equal(tracks.begin(), tracks.end(), tracksSet_.begin(), same)) return;
    tracksSet_ = std::move(tracks);
    publish();
}

void Out::measure(int slot, int channel, int input) {
    state_ = Measuring::Idle;
    mSlot_ = slot, mChannel_ = channel & 15, mInput_ = input;
    request_++;  // (the audio thread starts afresh: it resets its own counts)
    state_ = Measuring::Listening;
}

void Out::push(double when, int slot, uint8_t a, int n, uint8_t b, uint8_t c) {
    Event e;
    e.when = when;
    e.slot = (int16_t)slot;
    e.n = (uint8_t)n;
    e.b[0] = a, e.b[1] = b, e.b[2] = c;
    ring_.push(e);
    pushed_ = true;
}

void Out::process(const Buffer& b, const Part* parts, int count) {
    std::shared_ptr<const Config> c = std::atomic_load(&config_);
    measureBuffer(b);
    for (int i = 0; i < count; i++) part(b, parts[i], *c);
    stream_ += b.frames;
    if (pushed_ && wake_ >= 0) {  // (one system call a buffer, never blocking)
        uint64_t one = 1;
        ssize_t w = ::write(wake_, &one, sizeof one);
        (void)w;
    }
    pushed_ = false;
}

void Out::measureBuffer(const Buffer& b) {
    Measuring s = state_.load();
    if (s != Measuring::Listening && s != Measuring::Waiting) return;
    if (int r = request_.load(); r != seen_) {  // (a new Measure)
        seen_ = r;
        mFrames_ = 0;
        mNoise_ = 0;
        if (s != Measuring::Listening) return;
    }
    bool can = b.in && mInput_ >= 0 && mInput_ < b.inChannels;
    if (s == Measuring::Listening) {
        if (!can) return;  // (the input not heard yet: monitoring starting)
        for (int i = 0; i < b.frames; i++) mNoise_ = std::max(mNoise_, std::fabs(b.in[(size_t)i * b.inChannels + mInput_]));
        mFrames_ += b.frames;
        if (mFrames_ >= (int64_t)(MEASURE_NOISE_S * b.sr)) {
            push(b.now, mSlot_, (uint8_t)(NOTE_ON | mChannel_), 3, MEASURE_NOTE, 127);  // (at this buffer's start)
            mSent_ = stream_;
            state_ = Measuring::Waiting;
        }
        return;
    }
    float threshold = std::max(0.02f, mNoise_ * 4);
    if (can)
        for (int i = 0; i < b.frames; i++)
            if (std::fabs(b.in[(size_t)i * b.inChannels + mInput_]) > threshold) {
                measured_ = stream_ + i - mSent_;
                push(b.now, mSlot_, (uint8_t)(NOTE_OFF | mChannel_), 3, MEASURE_NOTE, 0);
                state_ = Measuring::Done;
                return;
            }
    if (stream_ + b.frames - mSent_ > (int64_t)(MEASURE_TIMEOUT_S * b.sr)) {
        push(b.now, mSlot_, (uint8_t)(NOTE_OFF | mChannel_), 3, MEASURE_NOTE, 0);
        state_ = Measuring::Failed;
    }
}

void Out::notesOff(TrackState& st, double when) {
    uint8_t ch = (uint8_t)(st.channel & 15);
    for (int p = 0; p < 128; p++)
        if (st.held[p]) {
            push(when, st.slot, (uint8_t)(NOTE_OFF | ch), 3, (uint8_t)p, 0);
            st.held[p] = false;
        }
    push(when, st.slot, (uint8_t)(CONTROL | ch), 3, 123, 0);  // All Notes Off (whatever else sounds)
}

void Out::part(const Buffer& b, const Part& p, const Config& c) {
    const int sr = b.sr;
    double t0 = b.now + (double)p.offset / sr;                       // this part as made
    double heard0 = t0 + b.outLatency + (double)b.latency / sr;      // …and as heard
    bool restart = p.moving != wasMoving_ || (p.moving && p.start != nextPos_);
    bool loopJump = restart && p.moving && wasMoving_ && b.loops && p.start == b.loopStart && nextPos_ == b.loopEnd;
    // --- clock ---
    if (restart && !c.clocks.empty()) {
        if (wasMoving_)
            for (auto& k : c.clocks) push(heard0 + k.offsetS, k.slot, STOP);
        if (p.moving && b.map) {
            double beat = b.map->beatAt((double)p.start / sr);
            int64_t sixteenth = std::max<int64_t>(0, (int64_t)std::ceil(beat * 4 - 1e-6));  // the next 16th
            pulse_ = sixteenth * (PPQN / 4);
            for (auto& k : c.clocks) {
                if (sixteenth > 0 || k.alwaysStart || wasMoving_) {
                    int v = (int)std::min<int64_t>(sixteenth, 16383);
                    push(heard0 + k.offsetS, k.slot, SONG_POSITION, 3, (uint8_t)(v & 127), (uint8_t)(v >> 7));
                }
                push(heard0 + k.offsetS, k.slot, sixteenth == 0 || k.alwaysStart ? START : CONTINUE);
            }
        }
    }
    if (p.moving && b.map && !c.clocks.empty()) {
        double from = (double)p.start / sr, to = (double)(p.start + p.frames) / sr;
        // (a clock output turned on while playing: the pulses from where it is, never the whole song at once)
        int64_t here = (int64_t)std::ceil(b.map->beatAt(from) * PPQN - 1e-6);
        if (pulse_ < here || pulse_ > here + PPQN) pulse_ = here;
        for (;; pulse_++) {
            double s = b.map->secondsAt((double)pulse_ / PPQN);
            if (s >= to) break;
            if (s < from - 1e-9) continue;  // (a pulse before where it started: skipped)
            for (auto& k : c.clocks) push(heard0 + (s - from) + k.offsetS, k.slot, CLOCK);
        }
    }
    // --- External synths ---
    for (auto& s : states_) {  // (one no longer an External synth: its notes let go)
        if (!s.used) continue;
        bool listed = false;
        for (auto& ext : c.tracks) listed = listed || ext.track == s.track;
        if (!listed) {
            notesOff(s, t0);
            s.used = false;
        }
    }
    for (auto& ext : c.tracks) {
        bool inProject = false;
        if (b.tracks)
            for (auto& t : *b.tracks)
                if (t.get() == ext.track) inProject = true;
        TrackState* st = nullptr;
        for (auto& s : states_)
            if (s.used && s.track == ext.track) st = &s;
        if (!inProject) {
            if (st) {  // (gone from the project: its notes let go)
                notesOff(*st, t0);
                st->used = false;
            }
            continue;
        }
        bool fresh = !st;
        if (fresh)
            for (auto& s : states_)
                if (!s.used) {
                    st = &s;
                    *st = TrackState();
                    st->used = true;
                    st->track = ext.track;
                    break;
                }
        if (!st) continue;  // (more than MAX_EXTERNAL)
        if (st->slot != ext.slot || st->channel != ext.channel) {  // (another port or channel: the old one let go)
            if (st->slot >= 0) notesOff(*st, t0);
            st->slot = ext.slot, st->channel = ext.channel;
        }
        external(b, p, *st, restart || fresh, loopJump && !fresh);
    }
    wasMoving_ = p.moving;
    nextPos_ = p.moving ? p.start + p.frames : 0;
}

void Out::external(const Buffer& b, const Part& p, TrackState& st, bool restart, bool loopJump) {
    const int sr = b.sr;
    const Track& track = *st.track;
    double t0 = b.now + (double)p.offset / sr;
    // its sound comes back on the inputs: sent as made; else heard from the synth itself: sent as heard
    double base = track.ext.input >= 0 ? t0 : t0 + b.outLatency + (double)b.latency / sr;
    int64_t delay = std::max<int64_t>(0, (int64_t)std::llround(track.ext.delayMs * sr / 1000.0));
    if (loopJump) {
        st.next -= b.loopEnd - b.loopStart;  // (its window went round already: where it is in the loop)
    } else if (restart) {
        notesOff(st, base);
        st.next = p.start;
    }
    if (!p.moving) return;
    int64_t end = p.start + p.frames + delay;
    if (end <= st.next) return;
    uint8_t ch = (uint8_t)(st.channel & 15);
    // the notes of [a, e) (timeline frames), sent as virtual frames a + shift
    auto send = [&](int64_t a, int64_t e, int64_t shift) {
        clips_.clear();
        if (b.clips)
            for (auto& clip : *b.clips) {
                if (clip->disabled || clip->kind != Kind::Midi) continue;
                if (std::atomic_load(&clip->track).get() != st.track) continue;
                if (clip->start < e && clip->end() >= a) clips_.push_back(clip.get());
            }
        if (clips_.empty()) return;
        plugin_audio::noteMessages(clips_.data(), clips_.size(), true, a, (int)(e - a), false, messages_, events_,
                                   nullptr);
        for (const plugin_audio::MidiMsg& m : messages_) {
            int64_t v = a + m.frame + shift;
            double when = std::max(t0, base + (double)(v - delay - p.start) / sr);
            int pitch = m.pitch & 127;
            if (m.type == plugin_audio::MidiMsg::NoteOn) {
                push(when, st.slot, (uint8_t)(NOTE_ON | ch), 3, (uint8_t)pitch, (uint8_t)std::clamp(m.velocity, 1, 127));
                st.held[pitch] = true;
            } else if (m.type == plugin_audio::MidiMsg::NoteOff && st.held[pitch]) {
                push(when, st.slot, (uint8_t)(NOTE_OFF | ch), 3, (uint8_t)pitch, 0);
                st.held[pitch] = false;
            }
        }
    };
    int64_t a = st.next;
    if (b.loops && p.start < b.loopEnd && end > b.loopEnd && b.loopEnd > b.loopStart) {
        // the window goes past the loop's end: what follows it is the loop's start
        if (a < b.loopEnd) {
            send(a, b.loopEnd, 0);
            notesOff(st, std::max(t0, base + (double)(b.loopEnd - delay - p.start) / sr));
            a = b.loopEnd;
        }
        int64_t len = b.loopEnd - b.loopStart;
        int64_t from = b.loopStart + (a - b.loopEnd), to = b.loopStart + std::min(end - b.loopEnd, len);
        if (to > from) send(from, to, b.loopEnd - b.loopStart);
    } else {
        send(a, end, 0);
    }
    st.next = end;
}

}  // namespace midi_out
