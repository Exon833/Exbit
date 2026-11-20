// MIDI effects on notes played live, on the audio thread (live_fx.h).
#include "audio/live_fx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>

namespace live_fx {

namespace {
std::atomic<uint64_t> gSerial{0};

const std::vector<double> GROOVE_BEATS = {0.5, 0.25, 0.125};  // (midi_effects.cpp's grids)

int pyRound(double x) { return (int)std::nearbyint(x); }
template <class T>
T clampTo(T v, T lo, T hi) { return std::min(hi, std::max(lo, v)); }
size_t choice(double v, size_t n) { return std::min(n - 1, (size_t)std::max(0.0, v)); }
}  // namespace

bool timed(const Track& track) {
    auto effects = track.midiEffects.get();
    for (auto& e : *effects)
        if (!e.bypass && !midi_effects::isLive(e.kind)) return true;
    return false;
}

std::shared_ptr<const Program> compile(const Track& track) {
    auto p = std::make_shared<Program>();
    p->serial = ++gSerial;
    if (midi_effects::swings(track)) {  // (as apply(): the swing first)
        Stage s;
        s.type = Stage::Groove;
        s.perPitch = true;
        s.grid = GROOVE_BEATS[(size_t)midi_effects::swingGrid()];
        for (int k = 0; k < 128; k++) s.pitchSwing[k] = 0.5 + midi_effects::swingOf(track, k) / 400;
        p->stages.push_back(std::move(s));
    }
    auto effects = track.midiEffects.get();
    for (auto& e : *effects) {
        if (e.bypass || (int)p->stages.size() >= MAX_STAGES) continue;
        Stage s;
        if (e.kind == "Pitch") {
            s.type = Stage::Pitch;
            s.semitones = (int)e.get("semitones");
        } else if (e.kind == "Chord") {
            s.type = Stage::Chord;
            const auto& added = midi_effects::CHORDS[choice(e.get("chord"), midi_effects::CHORDS.size())].second;
            for (int a : added)
                if (s.count < MAX_OUTS - 1) s.intervals[s.count++] = a;
            s.chordVelocity = e.get("velocity");
        } else if (e.kind == "Scale") {
            s.type = Stage::Scale;
            for (int k = 0; k < 128; k++) s.scaleMap[k] = (int16_t)midi_effects::liveOne(e, k, 100)[0].first;
        } else if (e.kind == "Velocity") {
            s.type = Stage::Velocity;
            s.velocityMap.resize(128 * 128);
            for (int k = 0; k < 128; k++)
                for (int v = 0; v < 128; v++)
                    s.velocityMap[(size_t)(k * 128 + v)] =
                        (uint8_t)clampTo((int)midi_effects::liveOne(e, k, std::max(1, v))[0].second, 1, 127);
        } else if (e.kind == "Random") {
            s.type = Stage::Random;
            s.chance = e.get("chance");
            s.range = e.get("range");
        } else if (e.kind == "Arpeggiator") {
            s.type = Stage::Arp;
            s.stepBeats = midi_effects::RATE_BEATS[choice(e.get("rate"), midi_effects::RATE_BEATS.size())];
            s.style = (int)choice(e.get("style"), midi_effects::STYLES.size());
            s.gate = e.get("gate");
            s.octaves = clampTo((int)e.get("octaves"), 1, 4);
        } else if (e.kind == "Note Length") {
            s.type = Stage::Length;
            s.share = e.get("mode") >= 0.5;
            s.beats = e.get("beats");
            s.sharePct = e.get("share");
        } else if (e.kind == "Groove") {
            s.type = Stage::Groove;
            s.grid = GROOVE_BEATS[choice(e.get("grid"), GROOVE_BEATS.size())];
            s.swing = clampTo(e.get("swing"), 50.0, 75.0) / 100;
            s.accent = clampTo(e.get("accent"), -50.0, 50.0) / 100;
            s.humanizeS = std::max(0.0, e.get("timing")) / 1000;
            s.humanizeVelocity = std::max(0.0, e.get("velocity"));
            for (int k = 0; k < 128; k++) s.rowOwn[k] = e.values.count("row:" + std::to_string(k)) > 0;
        } else {
            continue;
        }
        p->stages.push_back(std::move(s));
    }
    return p;
}

void reach(const Program& program, int pitch, bool out[128]) {
    bool set[128] = {}, next[128];
    if (pitch >= 0 && pitch < 128) set[pitch] = true;
    for (const Stage& s : program.stages) {
        std::fill(std::begin(next), std::end(next), false);
        for (int p = 0; p < 128; p++) {
            if (!set[p]) continue;
            auto add = [&](int q) {
                if (q >= 0 && q < 128) next[q] = true;
            };
            switch (s.type) {
            case Stage::Pitch: add(p + s.semitones); break;
            case Stage::Chord:
                add(p);
                for (int k = 0; k < s.count; k++) add(p + s.intervals[k]);
                break;
            case Stage::Scale: add(s.scaleMap[p]); break;
            case Stage::Random:
                for (int d = -(int)s.range; d <= (int)s.range; d++) add(p + d);
                break;
            case Stage::Arp:
                for (int o = 0; o < s.octaves; o++) add(p + 12 * o);
                break;
            default: add(p); break;
            }
        }
        std::copy(std::begin(next), std::end(next), set);
    }
    for (int p = 0; p < 128; p++) out[p] = out[p] || set[p];
}

// =================================================================================================

Runner::Runner()
    : rng_((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()), arpRng_(1), arpRngStart_(1) {
    slots_.resize(MAX_STAGES + 1);
    for (auto& s : slots_) {
        s.table.resize(TABLE);
        s.queue.resize(QUEUE);
    }
    soundingId_.resize(SOUNDING);
    soundingPitch_.resize(SOUNDING);
    pool_.resize((size_t)TABLE * 4 * 2);
    held_.resize(TABLE);
}

void Runner::reset() {
    for (auto& s : slots_) {
        s.tableCount = s.queueCount = 0;
        s.running = false;
        s.lastStep = INT64_MIN;
        s.index = 0;
    }
    std::fill(std::begin(keyId_), std::end(keyId_), 0u);
    soundingCount_ = 0;
    arpRng_ = arpRngStart_;
}

bool Runner::busy() const {
    if (soundingCount_) return true;
    for (auto& s : slots_)
        if (s.tableCount || s.queueCount || s.running) return true;
    for (uint32_t k : keyId_)
        if (k) return true;
    return false;
}

void Runner::flush(int64_t at, Emit emit, void* ctx) {
    for (int i = 0; i < soundingCount_; i++) {
        Ev e;
        e.at = at, e.id = soundingId_[(size_t)i], e.pitch = soundingPitch_[(size_t)i], e.on = false;
        if (emit) emit(ctx, e);
    }
    reset();
}

void Runner::use(const Program& program, int64_t now, Emit emit, void* ctx) {
    if (program.serial == serial_ && program_) {
        program_ = &program;
        return;
    }
    flush(now, emit, ctx);
    program_ = &program;
    serial_ = program.serial;
}

void Runner::key(bool on, int pitch, int velocity, int64_t at, int64_t offAt) {
    if (!program_ || pitch < 0 || pitch > 127) return;
    Ev e;
    e.at = at, e.pitch = (int16_t)pitch, e.velocity = (int16_t)clampTo(velocity, 1, 127);
    if (on) {
        if (keyId_[pitch]) key(false, pitch, 0, at);  // (struck again: the last one ends first)
        e.on = true;
        e.id = keyId_[pitch] = newId();
        schedule(0, e);
        if (offAt >= 0) {
            Ev off = e;
            off.on = false, off.at = std::max(at, offAt);
            schedule(0, off);
            keyId_[pitch] = 0;  // (it ends by itself)
        }
    } else {
        if (!keyId_[pitch]) return;
        e.on = false;
        e.id = keyId_[pitch];
        keyId_[pitch] = 0;
        schedule(0, e);
    }
}

void Runner::schedule(int stage, const Ev& e) {
    Slot& s = slots_[(size_t)stage];
    if (s.queueCount >= QUEUE) {  // full: a note-on is left out; a note-off goes now (none is ever lost)
        if (!e.on) deliver(stage, e);
        return;
    }
    s.queue[(size_t)s.queueCount++] = e;
}

int Runner::find(Slot& s, uint32_t id) const {
    for (int i = 0; i < s.tableCount; i++)
        if (s.table[(size_t)i].id == id) return i;
    return -1;
}

void Runner::removeAt(Slot& s, int i) {
    for (int k = i + 1; k < s.tableCount; k++) s.table[(size_t)(k - 1)] = s.table[(size_t)k];
    s.tableCount--;
}

bool Runner::addEntry(Slot& s, const Entry& e) {
    if (s.tableCount >= TABLE) return false;
    s.table[(size_t)s.tableCount++] = e;
    return true;
}

int64_t Runner::framesFor(double beats) const {
    return bpf_ > 0 ? std::max<int64_t>(0, (int64_t)std::llround(beats / bpf_)) : 0;
}

void Runner::out(const Ev& e) {
    if (e.on) {
        if (soundingCount_ >= SOUNDING) return;  // (no room to remember it: left out)
        soundingId_[(size_t)soundingCount_] = e.id;
        soundingPitch_[(size_t)soundingCount_] = e.pitch;
        soundingCount_++;
    } else {
        int i = 0;
        while (i < soundingCount_ && soundingId_[(size_t)i] != e.id) i++;
        if (i == soundingCount_) return;  // (never went out)
        for (int k = i + 1; k < soundingCount_; k++) {
            soundingId_[(size_t)(k - 1)] = soundingId_[(size_t)k];
            soundingPitch_[(size_t)(k - 1)] = soundingPitch_[(size_t)k];
        }
        soundingCount_--;
    }
    if (emit_) emit_(ctx_, e);
}

void Runner::deliver(int stage, const Ev& in) {
    int count = program_ ? std::min((int)program_->stages.size(), MAX_STAGES) : 0;
    if (stage >= count) {
        out(in);
        return;
    }
    const Stage& st = program_->stages[(size_t)stage];
    Slot& slot = slots_[(size_t)stage];
    Ev e = in;
    switch (st.type) {
    case Stage::Pitch:
    case Stage::Chord:
    case Stage::Scale:
    case Stage::Velocity:
    case Stage::Random: {
        if (!e.on) {  // the notes its note-on became end
            int i = find(slot, e.id);
            if (i < 0) return;
            Entry en = slot.table[(size_t)i];
            removeAt(slot, i);
            for (int k = 0; k < en.n; k++) {
                Ev off = e;
                off.id = en.outId[k], off.pitch = en.outPitch[k];
                deliver(stage + 1, off);
            }
            return;
        }
        int16_t pitch[MAX_OUTS], velocity[MAX_OUTS];
        int n = 0;
        auto add = [&](int p, int v) {
            if (p >= 0 && p <= 127 && n < MAX_OUTS) pitch[n] = (int16_t)p, velocity[n] = (int16_t)clampTo(v, 1, 127), n++;
        };
        if (st.type == Stage::Pitch) {
            add(e.pitch + st.semitones, e.velocity);
        } else if (st.type == Stage::Chord) {
            add(e.pitch, e.velocity);
            for (int k = 0; k < st.count; k++)
                add(e.pitch + st.intervals[k], std::max(1, pyRound(e.velocity * st.chordVelocity / 100)));
        } else if (st.type == Stage::Scale) {
            add(st.scaleMap[e.pitch], e.velocity);
        } else if (st.type == Stage::Velocity) {
            add(e.pitch, st.velocityMap.empty() ? e.velocity : st.velocityMap[(size_t)(e.pitch * 128 + e.velocity)]);
        } else {  // Random: some notes moved up or down (fresh each time, as midi_effects::live)
            int p = e.pitch;
            if (rng_.random() * 100 < st.chance) {
                int64_t r = (int64_t)st.range;
                p = clampTo((int)(p + rng_.integers(-r, r + 1)), 0, 127);
            }
            add(p, e.velocity);
        }
        Entry en;
        en.id = e.id;
        for (int k = 0; k < n; k++) en.outId[k] = k == 0 ? e.id : newId(), en.outPitch[k] = pitch[k];
        en.n = n;
        if (!addEntry(slot, en)) return;  // (no room: left out, and so is its note-off)
        for (int k = 0; k < n; k++) {
            Ev on = e;
            on.id = en.outId[k], on.pitch = pitch[k], on.velocity = velocity[k];
            deliver(stage + 1, on);
        }
        return;
    }
    case Stage::Length: {
        if (!st.share) {  // a length in beats: its own note-off when that has passed, the key's ignored
            if (!e.on) return;
            Ev off = e;
            off.on = false;
            off.at = e.at + framesFor(st.beats);
            deliver(stage + 1, e);
            if (off.at <= cursor_) deliver(stage + 1, off);
            else schedule(stage + 1, off);
            return;
        }
        if (e.on) {  // a share of its own length: worked out when it is let go
            Entry en;
            en.id = e.id, en.pitch = e.pitch, en.at = e.at;
            if (!addEntry(slot, en)) return;
            deliver(stage + 1, e);
            return;
        }
        int i = find(slot, e.id);
        if (i < 0) return;
        int64_t start = slot.table[(size_t)i].at;
        removeAt(slot, i);
        Ev off = e;
        off.at = std::max(e.at, start + (int64_t)std::llround((double)(e.at - start) * st.sharePct / 100));
        if (off.at <= cursor_) deliver(stage + 1, off);
        else schedule(stage + 1, off);
        return;
    }
    case Stage::Groove: {
        if (!e.on) {  // as late as its note-on was
            int i = find(slot, e.id);
            if (i < 0) return;
            e.at += slot.table[(size_t)i].delay;
            removeAt(slot, i);
            if (e.at <= cursor_) deliver(stage + 1, e);
            else schedule(stage + 1, e);
            return;
        }
        double beat = beatAt(e.at), pair = 2 * st.grid;
        double base = std::floor(beat / pair) * pair;
        double phase = clampTo(beat - base, 0.0, pair);  // 0 … 2 steps
        double later = 0;  // (frames)
        double swing = st.perPitch ? st.pitchSwing[e.pitch] : st.rowOwn[e.pitch] ? 0.5 : st.swing;
        if (swing != 0.5) {
            double moved = phase <= st.grid ? phase * 2 * swing : pair * swing + (phase - st.grid) * (2 - 2 * swing);
            later = bpf_ > 0 ? (moved - phase) / bpf_ : 0;
        }
        double v = e.velocity;
        bool changed = false;
        if (st.accent != 0) {
            v *= 1 + st.accent * std::max(0.0, 1 - std::fabs(phase - st.grid) / st.grid);
            changed = true;
        }
        if (st.humanizeS > 0 || st.humanizeVelocity > 0) {
            double early = rng_.random() * 2 - 1, louder = rng_.random() * 2 - 1;
            later += early * st.humanizeS * sr_;
            if (st.humanizeVelocity > 0) {
                v += louder * st.humanizeVelocity;
                changed = true;
            }
        }
        if (changed) e.velocity = (int16_t)clampTo(pyRound(v), 1, 127);
        Entry en;
        en.id = e.id;
        en.delay = std::max<int64_t>(0, (int64_t)std::llround(later));  // (never earlier than now)
        if (!addEntry(slot, en)) return;
        e.at += en.delay;
        if (e.at <= cursor_) deliver(stage + 1, e);
        else schedule(stage + 1, e);
        return;
    }
    case Stage::Arp: {
        if (e.on) {
            Entry en;
            en.id = e.id, en.pitch = e.pitch, en.velocity = e.velocity, en.at = e.at;
            if (!addEntry(slot, en)) return;
            slot.running = true;  // (its next step: the next line of its grid)
        } else {
            int i = find(slot, e.id);
            if (i >= 0) removeAt(slot, i);  // (an empty arpeggio stops at its next step)
        }
        return;
    }
    }
}

int64_t Runner::nextStep(int stage) const {
    const Slot& slot = slots_[(size_t)stage];
    const Stage& st = program_->stages[(size_t)stage];
    int64_t from = std::max(cursor_, slot.lastStep == INT64_MIN ? cursor_ : slot.lastStep + 1);
    double g = std::ceil(beatAt(from) / st.stepBeats - 1e-9) * st.stepBeats;
    int64_t f = now_ + (int64_t)std::llround((g - beat0_) / bpf_);
    return std::max(f, from);
}

void Runner::step(int stage, int64_t at) {
    Slot& slot = slots_[(size_t)stage];
    const Stage& st = program_->stages[(size_t)stage];
    slot.lastStep = at;
    if (slot.tableCount == 0) {  // nothing held: it stops (and starts again from the first note)
        slot.running = false;
        slot.index = 0;
        return;
    }
    // the notes held: lowest first ("As played": in the order played), each octave up, as the style says
    int n = slot.tableCount;
    std::copy(slot.table.begin(), slot.table.begin() + n, held_.begin());
    if (st.style != 3)  // (by pitch, stable: an insertion sort, which never allocates as std::stable_sort may)
        for (int i = 1; i < n; i++) {
            Entry e = held_[(size_t)i];
            int j = i - 1;
            for (; j >= 0 && held_[(size_t)j].pitch > e.pitch; j--) held_[(size_t)j + 1] = held_[(size_t)j];
            held_[(size_t)j + 1] = e;
        }
    size_t size = 0;
    for (int o = 0; o < st.octaves; o++)
        for (int k = 0; k < n; k++) pool_[size++] = {(int16_t)(held_[(size_t)k].pitch + 12 * o), held_[(size_t)k].velocity};
    if (st.style == 1) {
        std::reverse(pool_.begin(), pool_.begin() + (long)size);
    } else if (st.style == 2 && size > 2) {
        size_t m = size;
        for (size_t k = m - 2; k >= 1; k--) pool_[size++] = pool_[k];
    }
    std::pair<int16_t, int16_t> pick =
        st.style == 4 ? pool_[(size_t)arpRng_.integers(0, (int64_t)size)] : pool_[(size_t)(slot.index % (int64_t)size)];
    slot.index++;
    if (pick.first > 127) return;
    Ev on;
    on.at = at, on.id = newId(), on.pitch = pick.first, on.velocity = pick.second, on.on = true;
    Ev off = on;
    off.on = false;
    off.at = at + (int64_t)std::llround(st.stepBeats * st.gate / 100 / bpf_);
    deliver(stage + 1, on);
    if (off.at <= at) deliver(stage + 1, off);
    else schedule(stage + 1, off);
}

void Runner::run(int64_t now, int frames, double beat0, double beatsPerFrame, int sr, Emit emit, void* ctx) {
    if (!program_) return;
    emit_ = emit, ctx_ = ctx;
    now_ = cursor_ = now;
    beat0_ = beat0;
    sr_ = sr > 0 ? sr : 48000;
    bpf_ = beatsPerFrame > 0 ? beatsPerFrame : 2.0 / sr_;  // (none given: 120 BPM)
    int count = std::min((int)program_->stages.size(), MAX_STAGES);
    int64_t end = now + frames;
    for (int guard = 0; guard < 1 << 20; guard++) {
        int64_t best = end;
        int which = -1, at = -1;
        bool isStep = false;
        for (int k = 0; k <= count; k++) {  // the earliest note waiting (the first scheduled, of those as early)
            const Slot& s = slots_[(size_t)k];
            for (int i = 0; i < s.queueCount; i++)
                if (s.queue[(size_t)i].at < best) best = s.queue[(size_t)i].at, which = k, at = i;
        }
        for (int k = 0; k < count; k++) {  // an arpeggio's step (after the notes arriving at that very frame)
            if (program_->stages[(size_t)k].type != Stage::Arp || !slots_[(size_t)k].running) continue;
            int64_t f = nextStep(k);
            if (f < best) best = f, which = k, isStep = true;
        }
        if (which < 0) break;
        cursor_ = std::max(cursor_, best);
        if (isStep) {
            step(which, cursor_);
            continue;
        }
        Slot& s = slots_[(size_t)which];
        Ev e = s.queue[(size_t)at];
        for (int i = at + 1; i < s.queueCount; i++) s.queue[(size_t)(i - 1)] = s.queue[(size_t)i];
        s.queueCount--;
        e.at = cursor_;
        deliver(which, e);
    }
    emit_ = nullptr, ctx_ = nullptr;
}

}  // namespace live_fx
