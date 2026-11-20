#include "audio/live.h"

#include <algorithm>
#include <cmath>

#include "audio/synth.h"
#include "audio/workers.h"
#include "core/util.h"

namespace live {

double bendSemitones(int value, int range) {
    value = std::max(-8192, std::min(8191, value));
    return (value >= 0 ? value / 8191.0 : value / 8192.0) * std::max(0, range);
}

std::vector<float> loopable(const std::vector<float>& audio, int channels, int64_t hold, int sr, double period,
                            int64_t& loopStart) {
    loopStart = -1;
    channels = std::max(1, channels);
    int64_t len = (int64_t)audio.size() / channels;
    hold = std::min(hold, len);
    int64_t length = std::min<int64_t>((int64_t)(LOOP_S * sr), hold / 2);
    if (period > 1) {
        bool found = false;
        double bestError = 0;
        int64_t best = 0;
        int64_t from = std::max<int64_t>(1, (int64_t)(0.5 * LOOP_S * sr / period));
        int64_t to = (int64_t)((double)length / period) + 1;
        for (int64_t m = from; m < to; m++) {
            double error = std::fabs(m * period - std::nearbyint(m * period));
            if (!found || error < bestError - 1e-9) {
                found = true;
                bestError = error;
                best = (int64_t)std::nearbyint(m * period);
            }
        }
        if (found) length = best;
    }
    if (length < 2) return audio;
    int64_t start = hold - length;
    int64_t fade = std::min({(int64_t)(LOOP_FADE_S * sr), length / 2, start});
    std::vector<float> out(audio.begin(), audio.begin() + hold * channels);
    for (int64_t k = 0; k < fade; k++) {
        float ramp = (float)(fade > 1 ? (double)k / (double)(fade - 1) : 0.0);  // np.linspace(0, 1, fade)
        for (int c = 0; c < channels; c++) {
            float& v = out[(size_t)((hold - fade + k) * channels + c)];
            v = v * (1.0f - ramp) + audio[(size_t)((start - fade + k) * channels + c)] * ramp;
        }
    }
    loopStart = start;
    return out;
}

std::vector<float> builtInNote(const NoteRender& r, int pitch, int velocity, double lengthS, int sr,
                               int64_t& loopStart) {
    loopStart = -1;
    auto render = [&](double seconds) {
        return r.render ? r.render(pitch, velocity, seconds, sr) : synth::renderNote(pitch, velocity, seconds, sr);
    };
    if (lengthS >= 0 || r.oneShot) return render(lengthS > 0 ? lengthS : HOLD_S);
    if (!r.sustains) return render(WHOLE_S);  // (a sample that doesn't loop: played to its end)
    double period = r.period ? r.period(pitch, sr) : sr / synth::noteFrequency(pitch);
    return loopable(render(HOLD_S), r.render ? r.channels : 1, (int64_t)(HOLD_S * sr), sr, period, loopStart);
}

bool LiveVoice::addTo(float* const signal[2], int signalChannels, int n, int fade, const float* rates) {
    if (delay > 0) {  // (starting later in this buffer: from its frame on)
        int d = std::min(delay, n);
        delay -= d;
        if (d >= n) return true;
        float* const later[2] = {signal[0] + d, signalChannels > 1 ? signal[1] + d : nullptr};
        return addTo(later, signalChannels, n - d, fade, rates ? rates + d : nullptr);
    }
    int64_t len = length();
    bool loops = loopStart >= 0;
    if (this->fade > 0) fade = this->fade;  // (cut off by its choke group: quickly)
    int64_t span = len - loopStart;
    int ch = std::max(1, channels);
    const float* d = data ? data->data() : nullptr;
    bool faded = false;
    if (!rates && frac == 0) {  // never bent (or back in step): frame by frame, as it was rendered
        int64_t count = loops ? n : std::max<int64_t>(0, std::min<int64_t>(n, len - pos));
        for (int64_t i = 0; i < count; i++) {
            double gain = 1.0;
            if (releasedAt >= 0) {
                gain = std::min(1.0, std::max(0.0, 1.0 - (double)(clock + i - releasedAt) / fade));
                if (i == count - 1 && gain <= 0) faded = true;  // (faded out: finished)
            }
            int64_t at = pos + i;
            if (loops && at >= len) at = loopStart + (at - loopStart) % span;
            for (int c = 0; c < signalChannels; c++) {
                int source = ch == 1 ? 0 : std::min(c, ch - 1);
                signal[c][i] += (float)(d[at * ch + source] * gain * level);
            }
        }
        pos += n;
        clock += n;
        return !faded && (loops || pos < len);
    }
    // bent: read at its rate, between frames (linear interpolation)
    if (loops && pos >= len) pos = loopStart + (pos - loopStart) % span;
    bool ended = false;
    for (int i = 0; i < n; i++) {
        double gain = 1.0;
        if (releasedAt >= 0) {
            gain = std::min(1.0, std::max(0.0, 1.0 - (double)(clock + i - releasedAt) / fade));
            if (gain <= 0) {  // (faded out: finished)
                faded = true;
                break;
            }
        }
        if (pos >= len) {
            ended = true;
            break;
        }
        int64_t next = pos + 1 < len ? pos + 1 : loops ? loopStart : -1;
        for (int c = 0; c < signalChannels; c++) {
            int source = ch == 1 ? 0 : std::min(c, ch - 1);
            double a = d[pos * ch + source], b = next >= 0 ? d[next * ch + source] : 0.0;
            signal[c][i] += (float)((a + (b - a) * frac) * gain * level);
        }
        frac += rates ? (double)rates[i] : 1.0;
        int64_t whole = (int64_t)frac;
        pos += whole;
        frac -= (double)whole;
        if (loops && pos >= len) pos = loopStart + (pos - loopStart) % span;
    }
    clock += n;
    return !faded && !ended;
}

LiveNotes::LiveNotes(size_t capacity)
    : inbox_(std::max<size_t>(capacity, 2)), done_(std::max<size_t>(capacity, 2) + MAX_VOICES), playedRing_(4096) {
    voices_.reserve(MAX_VOICES);
    timedOffs_.reserve(256);
    finished_.reserve(MAX_VOICES);
}

Bank::Bank() {
    for (int p = 0; p < 128; p++) loopStart[p] = -1, channels[p] = 1, floor[p] = 0, oneShot[p] = false;
}

Clock clockAt(const TempoMap& map, bool playing, int64_t start, int sr) {
    Clock c;
    double seconds = playing && sr > 0 ? (double)start / sr : 0.0;
    c.playing = playing;
    c.start = start;
    c.beat = playing ? map.beatAt(seconds) : 0.0;
    c.beatsPerFrame = sr > 0 ? 1.0 / (map.beatSAt(seconds) * sr) : 0.0;
    return c;
}

bool LiveNotes::push(const Item& item) {
    collect();
    uint64_t head = inHead_.load(std::memory_order_relaxed);
    if (head - inTail_.load(std::memory_order_acquire) >= inbox_.size()) return false;
    inbox_[head % inbox_.size()] = item;
    inHead_.store(head + 1, std::memory_order_release);
    return true;
}

bool LiveNotes::pitchBend(int value, double semitones) {
    Item it;
    it.kind = Item::BendItem;
    it.value = std::max(-8192, std::min(8191, value));
    it.semitones = std::isfinite(semitones) ? std::max(-48.0, std::min(48.0, semitones)) : 0.0;
    return push(it);
}

bool LiveNotes::control(int number, int value) {
    if (number < 0 || number > 127) return false;
    Item it;
    it.kind = Item::ControlItem;
    it.pitch = number;
    it.value = std::max(0, std::min(127, value));
    return push(it);
}

bool LiveNotes::noteOn(int pitch, int velocity, int sr, bool builtin, double lengthS, const NoteRender* render) {
    collect();
    uint64_t head = inHead_.load(std::memory_order_relaxed);
    if (head - inTail_.load(std::memory_order_acquire) >= inbox_.size()) return false;
    const NoteRender& given = render ? *render : renderer_;
    NoteRender keyed;  // (a Pad Kit: the key's own pad)
    if (given.perKey) given.perKey(pitch, keyed);
    const NoteRender& r = given.perKey ? keyed : given;
    Item& it = inbox_[head % inbox_.size()];
    it.kind = Item::NoteItem;
    it.on = true;
    it.pitch = pitch, it.velocity = velocity;
    it.audio = nullptr;
    it.channels = r.render ? std::max(1, r.channels) : 1;
    it.loopStart = -1;
    it.choke = builtin ? r.choke : 0;
    if (builtin) {
        int64_t loopStart;
        auto audio = std::make_shared<const std::vector<float>>(builtInNote(r, pitch, velocity, lengthS, sr, loopStart));
        int64_t frames = (int64_t)audio->size() / it.channels;
        it.loopStart = loopStart >= 0 && loopStart < frames - 1 ? loopStart : -1;
        it.audio = std::move(audio);
    }
    if (it.audio && lengthS < 0 && r.oneShot) {  // (a one-shot note stops when its audio does; its note-off is ignored)
        oneShots_[pitch]++;
        it.length = (int64_t)it.audio->size() / it.channels;
    } else {
        it.length = lengthS < 0 ? -1 : (int64_t)(lengthS * sr);
    }
    inHead_.store(head + 1, std::memory_order_release);
    return true;
}

bool LiveNotes::noteOff(int pitch) {
    collect();
    if (pitch >= 0 && pitch < 128 && timedKeys_[pitch] > 0) {  // (a key into the chain: let go there)
        Item it;
        it.kind = Item::KeyItem;
        it.on = false;
        it.pitch = pitch;
        if (!push(it)) return false;
        timedKeys_[pitch]--;
        return true;
    }
    auto one = oneShots_.find(pitch);
    if (one != oneShots_.end()) {  // (the key let go played a one-shot note: it goes on to its end)
        if (--one->second <= 0) oneShots_.erase(one);
        return true;
    }
    uint64_t head = inHead_.load(std::memory_order_relaxed);
    if (head - inTail_.load(std::memory_order_acquire) >= inbox_.size()) return false;
    Item& it = inbox_[head % inbox_.size()];
    it.kind = Item::NoteItem;
    it.on = false;
    it.pitch = pitch, it.velocity = 0;
    it.audio = nullptr;
    it.length = -1;
    inHead_.store(head + 1, std::memory_order_release);
    return true;
}

void LiveNotes::collect() {
    uint64_t head = doneHead_.load(std::memory_order_acquire);
    uint64_t tail = doneTail_.load(std::memory_order_relaxed);
    for (; tail < head; tail++) done_[tail % done_.size()].reset();
    doneTail_.store(tail, std::memory_order_release);
}

void LiveNotes::retire(NoteAudio&& audio) {
    if (!audio) return;
    uint64_t head = doneHead_.load(std::memory_order_relaxed);
    if (head - doneTail_.load(std::memory_order_acquire) >= done_.size()) {
        audio.reset();  // (the GUI thread hasn't collected for long: free it here)
        return;
    }
    done_[head % done_.size()] = std::move(audio);
    doneHead_.store(head + 1, std::memory_order_release);
}

bool LiveNotes::active() const {
    return inHead_.load(std::memory_order_acquire) != inTail_.load(std::memory_order_relaxed) || !voices_.empty() ||
           !timedOffs_.empty() || resendBend_ != NO_BEND || runner_.busy();
}

bool LiveNotes::holding() const {
    return inHead_.load(std::memory_order_acquire) != inTail_.load(std::memory_order_relaxed) || !timedOffs_.empty() ||
           heldTotal_ > 0 || runner_.busy();
}

void LiveNotes::clear() {
    uint64_t head = inHead_.load(std::memory_order_acquire);
    uint64_t tail = inTail_.load(std::memory_order_relaxed);
    for (; tail < head; tail++) {
        Item& item = inbox_[tail % inbox_.size()];
        if (item.kind == Item::BendItem) {  // (where the wheel is still counts: notes played later are bent so)
            bendTarget_ = bendNow_ = item.semitones;
            resendBend_ = item.value;
        }
        retire(std::move(item.audio));
    }
    inTail_.store(tail, std::memory_order_release);
    for (auto& v : voices_) retire(std::move(v.data));
    voices_.clear();
    timedOffs_.clear();
    std::fill(std::begin(held_), std::end(held_), 0);
    heldTotal_ = 0;
    runner_.reset();  // (the chain's notes are gone with the voices; its keys are forgotten)
}

plugin_audio::MidiMsg LiveNotes::release(int pitch, int64_t atFrame) {
    for (auto& v : voices_)
        if (v.pitch == pitch && v.releasedAt < 0 && v.id == 0) v.releasedAt = v.clock + atFrame;
    plugin_audio::MidiMsg m;
    m.type = plugin_audio::MidiMsg::NoteOff;
    m.frame = atFrame, m.pitch = pitch, m.velocity = 0;
    return m;
}

void LiveNotes::take(int frames, int sr, plugin_audio::Messages& out, const Clock* clock) {
    // the timed path's chain and notes, as the GUI thread last gave them (it keeps the ones replaced alive)
    programNow_ = std::atomic_load(&program_);
    bankNow_ = std::atomic_load(&bank_);
    out_ = &out;
    frames_ = frames;
    clock_ = clock ? *clock : Clock();
    if (programNow_) runner_.use(*programNow_, now_, &LiveNotes::emitted, this);
    if (resendBend_ != NO_BEND) {  // (a bend cleared away before an instrument got it)
        plugin_audio::MidiMsg m;
        m.type = plugin_audio::MidiMsg::PitchBend;
        m.value = resendBend_;
        out.push_back(m);
        resendBend_ = NO_BEND;
    }
    uint64_t head = inHead_.load(std::memory_order_acquire);
    uint64_t tail = inTail_.load(std::memory_order_relaxed);
    size_t sounding = voices_.size();  // (the voices a choke cuts off: those sounding before this buffer's notes)
    for (; tail < head; tail++) {
        Item& item = inbox_[tail % inbox_.size()];
        int p = std::min(127, std::max(0, item.pitch));
        if (item.kind == Item::KeyItem) {  // into the chain (live_fx): played by run() below
            if (programNow_)
                runner_.key(item.on, item.pitch, item.velocity, now_, item.length >= 0 ? now_ + item.length : -1);
            continue;
        }
        if (item.kind != Item::NoteItem) {  // the bend wheel, a control: to a VST instrument as MIDI
            plugin_audio::MidiMsg m;
            m.type = item.kind == Item::BendItem ? plugin_audio::MidiMsg::PitchBend : plugin_audio::MidiMsg::Control;
            m.frame = 0, m.pitch = p, m.velocity = 0, m.value = item.value;
            out.push_back(m);
            if (item.kind == Item::BendItem) {
                bendTarget_ = item.semitones;
                if (voices_.empty()) bendNow_ = bendTarget_;  // (nothing sounding to glide: notes start bent)
            }
            continue;
        }
        if (item.on) {
            if (item.audio) {
                if (item.choke > 0)  // (its choke group's notes stop, fading out over CHOKE_S)
                    for (size_t k = 0; k < sounding; k++) {
                        LiveVoice& other = voices_[k];
                        if (other.choke != item.choke || other.releasedAt >= 0) continue;
                        other.releasedAt = other.pos;
                        other.fade = std::max(1, (int)(CHOKE_S * sr));
                    }
                LiveVoice v;
                v.pitch = item.pitch;
                v.channels = item.channels;
                v.loopStart = item.loopStart;
                v.choke = item.choke;
                v.data = std::move(item.audio);
                voices_.push_back(std::move(v));
            } else {
                plugin_audio::MidiMsg m;
                m.type = plugin_audio::MidiMsg::NoteOn;
                m.frame = 0, m.pitch = item.pitch, m.velocity = std::max(1, item.velocity);
                out.push_back(m);
                if (item.length < 0) held_[p]++, heldTotal_++;
            }
            if (item.length >= 0) timedOffs_.push_back({item.length, item.pitch});
            handBack(true, item.pitch, item.velocity, 0);
        } else {
            out.push_back(release(item.pitch, 0));
            if (held_[p] > 0) held_[p]--, heldTotal_--;
            handBack(false, item.pitch, 0, 0);
        }
    }
    inTail_.store(tail, std::memory_order_release);
    if (programNow_) {  // the chain's notes of this buffer, at their frames (emitted())
        double bpf = clock_.beatsPerFrame > 0 ? clock_.beatsPerFrame : 2.0 / std::max(1, sr);
        double beat = clock_.playing ? clock_.beat : freeBeat_;
        runner_.run(now_, frames, beat, bpf, sr, &LiveNotes::emitted, this);
        freeBeat_ = beat + frames * bpf;  // (stopped: it goes on from where it was)
    }
    now_ += frames;
    out_ = nullptr;
    size_t keep = 0;
    for (size_t i = 0; i < timedOffs_.size(); i++) {
        auto& e = timedOffs_[i];
        e.first -= frames;
        if (e.first <= 0) {
            out.push_back(release(e.second, std::max<int64_t>(0, frames + e.first)));
            handBack(false, e.second, 0, (int)std::max<int64_t>(0, frames + e.first));
        } else
            timedOffs_[keep++] = e;
    }
    timedOffs_.resize(keep);
}

void LiveNotes::addVoices(float* const signal[2], int channels, int frames, int sr) {
    int fade = std::max(1, (int)(synth::RELEASE_S * sr));
    if (voices_.empty()) {
        bendNow_ = bendTarget_;
        return;
    }
    if (bendNow_ != 0 || bendTarget_ != 0) {  // bent (or coming back): CHUNK frames at a time, each at its rate
        double follow = 1.0 - std::exp(-1.0 / (BEND_SMOOTH_S * std::max(1, sr)));
        finished_.assign(voices_.size(), 0);  // (reserved: no allocation for up to 128 voices)
        for (int done = 0; done < frames; done += CHUNK) {
            int n = std::min(CHUNK, frames - done);
            for (int i = 0; i < n; i++) {
                bendNow_ += (bendTarget_ - bendNow_) * follow;
                if (std::fabs(bendTarget_ - bendNow_) < 1e-6) bendNow_ = bendTarget_;
                rates_[i] = (float)std::exp2(bendNow_ / 12.0);
            }
            float* part[2] = {signal[0] + done, channels > 1 ? signal[1] + done : nullptr};
            for (size_t v = 0; v < voices_.size(); v++)
                if (!finished_[v] && !voices_[v].addTo(part, channels, n, fade, rates_)) finished_[v] = 1;
        }
        size_t keep = 0;
        for (size_t i = 0; i < voices_.size(); i++) {
            if (!finished_[i]) {
                if (keep != i) voices_[keep] = std::move(voices_[i]);
                keep++;
            } else {
                retire(std::move(voices_[i].data));
            }
        }
        voices_.resize(keep);
        return;
    }
    size_t keep = 0;
    for (size_t i = 0; i < voices_.size(); i++) {
        if (voices_[i].addTo(signal, channels, frames, fade)) {
            if (keep != i) voices_[keep] = std::move(voices_[i]);
            keep++;
        } else {
            retire(std::move(voices_[i].data));
        }
    }
    voices_.resize(keep);
}

// --- the timed path (live_fx) ---

void LiveNotes::retireOld() {
    // the audio thread only reaches a program or bank through program_ / bank_: one held by nothing else but
    // this list can't be taken any more, so it is freed here, never on the audio thread
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(),
                               [](const std::shared_ptr<const void>& p) { return p.use_count() == 1; }),
                kept_.end());
}

void LiveNotes::useEffects(const Track& track, bool builtin, int sr, int pitch, const NoteRender* render) {
    collect();
    retireOld();
    auto source = track.midiEffects.get();
    auto program = std::atomic_load(&program_);
    if (!program || source != fxSource_) {
        program = live_fx::compile(track);
        fxSource_ = source;
        kept_.push_back(program);
        std::atomic_store(&program_, program);
        util::debugf("live: \"%s\": %zu MIDI effects played live by the audio thread (they change timing)\n",
                     track.name.c_str(), program->stages.size());
    }
    if (!builtin) {  // (a VST instrument: the notes go to it as MIDI)
        if (std::atomic_load(&bank_)) std::atomic_store(&bank_, std::shared_ptr<const Bank>());
        return;
    }
    const void* key0 = std::atomic_load(&track.sampler).get();
    const void* key1 = std::atomic_load(&track.padKit).get();
    auto bank = std::atomic_load(&bank_);
    bool fresh = !bank || bank->key[0] != key0 || bank->key[1] != key1 || bank->sr != sr;
    bool want[128] = {};
    live_fx::reach(*program, pitch, want);
    std::vector<int> missing;
    for (int p = 0; p < 128; p++)
        if (want[p] && (fresh || !bank->audio[p])) missing.push_back(p);
    if (missing.empty() && !fresh) return;
    auto next = fresh ? std::make_shared<Bank>() : std::make_shared<Bank>(*bank);
    next->key[0] = key0, next->key[1] = key1, next->sr = sr;
    struct Work {
        const NoteRender* given;
        const std::vector<int>* missing;
        Bank* bank;
        int sr;
    } work{render ? render : &renderer_, &missing, next.get(), sr};
    auto job = [](void* ctx, int i) {  // (each pitch its own: on every core)
        Work& w = *static_cast<Work*>(ctx);
        int p = (*w.missing)[(size_t)i];
        NoteRender keyed;
        if (w.given->perKey) w.given->perKey(p, keyed);
        const NoteRender& r = w.given->perKey ? keyed : *w.given;
        int64_t loopStart = -1;
        auto audio = std::make_shared<const std::vector<float>>(builtInNote(r, p, 127, -1, w.sr, loopStart));
        int channels = r.render ? std::max(1, r.channels) : 1;
        int64_t frames = (int64_t)audio->size() / channels;
        w.bank->loopStart[p] = loopStart >= 0 && loopStart < frames - 1 ? loopStart : -1;
        w.bank->channels[p] = channels;
        w.bank->floor[p] = std::min(1.0, std::max(0.0, r.velocityFloor));
        w.bank->oneShot[p] = r.oneShot;
        w.bank->audio[p] = std::move(audio);
    };
    int threads = missing.size() >= 4 ? workers::workersFor(workers::chosenCores()) : 0;
    {
        workers::Pool pool(threads, false);
        pool.run((int)missing.size(), job, &work);
    }
    kept_.push_back(next);
    std::atomic_store(&bank_, std::shared_ptr<const Bank>(next));
    util::debugf("live: \"%s\": %zu notes rendered for the live MIDI effects (%d threads)\n", track.name.c_str(),
                 missing.size(), threads + 1);
}

bool LiveNotes::keyOn(int pitch, int velocity, int sr, double lengthS) {
    if (pitch < 0 || pitch > 127) return false;
    Item it;
    it.kind = Item::KeyItem;
    it.on = true;
    it.pitch = pitch;
    it.velocity = std::max(1, std::min(127, velocity));
    it.length = lengthS < 0 ? -1 : (int64_t)(lengthS * sr);
    if (!push(it)) return false;
    if (lengthS < 0) timedKeys_[pitch]++;
    return true;
}

void LiveNotes::report(bool on) { report_.store(on, std::memory_order_release); }

void LiveNotes::played(std::vector<Played>& out) {
    uint64_t head = playedHead_.load(std::memory_order_acquire);
    uint64_t tail = playedTail_.load(std::memory_order_relaxed);
    for (; tail < head; tail++) out.push_back(playedRing_[tail % playedRing_.size()]);
    playedTail_.store(tail, std::memory_order_release);
}

void LiveNotes::handBack(bool on, int pitch, int velocity, int at) {
    if (!report_.load(std::memory_order_acquire)) return;
    uint64_t head = playedHead_.load(std::memory_order_relaxed);
    if (head - playedTail_.load(std::memory_order_acquire) >= playedRing_.size()) return;  // (not read for long)
    Played& p = playedRing_[head % playedRing_.size()];
    p.on = on, p.pitch = pitch, p.velocity = velocity;
    p.frame = clock_.playing ? clock_.start + at : -1;
    playedHead_.store(head + 1, std::memory_order_release);
}

void LiveNotes::emitted(void* ctx, const live_fx::Ev& e) {
    LiveNotes& self = *static_cast<LiveNotes*>(ctx);
    int at = (int)std::max<int64_t>(0, std::min<int64_t>(self.frames_ - 1, e.at - self.now_));
    self.handBack(e.on, e.pitch, e.velocity, at);
    const Bank* bank = self.bankNow_.get();
    if (!bank) {  // a VST instrument: MIDI at the very frame
        if (!self.out_) return;
        plugin_audio::MidiMsg m;
        m.type = e.on ? plugin_audio::MidiMsg::NoteOn : plugin_audio::MidiMsg::NoteOff;
        m.frame = at, m.pitch = e.pitch, m.velocity = e.on ? std::max<int>(1, e.velocity) : 0;
        self.out_->push_back(m);
        return;
    }
    if (!e.on) {
        for (auto& v : self.voices_)
            if (v.id == e.id && v.releasedAt < 0 && !v.oneShot) v.releasedAt = v.clock + std::max(0, at - v.delay);
        return;
    }
    int p = e.pitch;
    if (!bank->audio[p] || self.voices_.size() >= (size_t)MAX_VOICES) return;  // (not rendered, or no room)
    LiveVoice v;
    v.pitch = p;
    v.id = e.id;
    v.data = bank->audio[p];
    v.channels = bank->channels[p];
    v.loopStart = bank->loopStart[p];
    v.oneShot = bank->oneShot[p];
    v.delay = at;
    v.level = (float)(bank->floor[p] + (1 - bank->floor[p]) * e.velocity / 127.0);
    self.voices_.push_back(std::move(v));
}

LiveNotes* find(const LiveMap* map, const Track* track) {
    if (!map) return nullptr;
    for (auto& kv : *map)
        if (kv.first == track) return kv.second.get();
    return nullptr;
}

}  // namespace live
