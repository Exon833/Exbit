#include "audio/live.h"

#include <algorithm>
#include <cmath>

#include "audio/synth.h"

namespace live {

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

bool LiveVoice::addTo(float* const signal[2], int signalChannels, int n, int fade) {
    int64_t len = length();
    bool loops = loopStart >= 0;
    int64_t count = loops ? n : std::max<int64_t>(0, std::min<int64_t>(n, len - pos));
    int64_t span = len - loopStart;
    int ch = std::max(1, channels);
    const float* d = data ? data->data() : nullptr;
    bool faded = false;
    for (int64_t i = 0; i < count; i++) {
        double gain = 1.0;
        if (releasedAt >= 0) {
            gain = std::min(1.0, std::max(0.0, 1.0 - (double)(pos + i - releasedAt) / fade));
            if (i == count - 1 && gain <= 0) faded = true;  // (faded out: finished)
        }
        int64_t at = pos + i;
        if (loops && at >= len) at = loopStart + (at - loopStart) % span;
        for (int c = 0; c < signalChannels; c++) {
            int source = ch == 1 ? 0 : std::min(c, ch - 1);
            signal[c][i] += (float)(d[at * ch + source] * gain);
        }
    }
    pos += n;
    return !faded && (loops || pos < len);
}

LiveNotes::LiveNotes(size_t capacity) : inbox_(std::max<size_t>(capacity, 2)), done_(std::max<size_t>(capacity, 2)) {
    voices_.reserve(128);
    timedOffs_.reserve(256);
}

bool LiveNotes::noteOn(int pitch, int velocity, int sr, bool builtin, double lengthS, const NoteRender* render) {
    collect();
    uint64_t head = inHead_.load(std::memory_order_relaxed);
    if (head - inTail_.load(std::memory_order_acquire) >= inbox_.size()) return false;
    const NoteRender& r = render ? *render : renderer_;
    Item& it = inbox_[head % inbox_.size()];
    it.on = true;
    it.pitch = pitch, it.velocity = velocity;
    it.audio = nullptr;
    it.channels = r.render ? std::max(1, r.channels) : 1;
    it.loopStart = -1;
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
    auto one = oneShots_.find(pitch);
    if (one != oneShots_.end()) {  // (the key let go played a one-shot note: it goes on to its end)
        if (--one->second <= 0) oneShots_.erase(one);
        return true;
    }
    uint64_t head = inHead_.load(std::memory_order_relaxed);
    if (head - inTail_.load(std::memory_order_acquire) >= inbox_.size()) return false;
    Item& it = inbox_[head % inbox_.size()];
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
           !timedOffs_.empty();
}

bool LiveNotes::holding() const {
    return inHead_.load(std::memory_order_acquire) != inTail_.load(std::memory_order_relaxed) || !timedOffs_.empty() ||
           heldTotal_ > 0;
}

void LiveNotes::clear() {
    uint64_t head = inHead_.load(std::memory_order_acquire);
    uint64_t tail = inTail_.load(std::memory_order_relaxed);
    for (; tail < head; tail++) retire(std::move(inbox_[tail % inbox_.size()].audio));
    inTail_.store(tail, std::memory_order_release);
    for (auto& v : voices_) retire(std::move(v.data));
    voices_.clear();
    timedOffs_.clear();
    std::fill(std::begin(held_), std::end(held_), 0);
    heldTotal_ = 0;
}

plugin_audio::MidiMsg LiveNotes::release(int pitch, int64_t atFrame) {
    for (auto& v : voices_)
        if (v.pitch == pitch && v.releasedAt < 0) v.releasedAt = v.pos + atFrame;
    plugin_audio::MidiMsg m;
    m.type = plugin_audio::MidiMsg::NoteOff;
    m.frame = atFrame, m.pitch = pitch, m.velocity = 0;
    return m;
}

void LiveNotes::take(int frames, int sr, plugin_audio::Messages& out) {
    uint64_t head = inHead_.load(std::memory_order_acquire);
    uint64_t tail = inTail_.load(std::memory_order_relaxed);
    for (; tail < head; tail++) {
        Item& item = inbox_[tail % inbox_.size()];
        int p = std::min(127, std::max(0, item.pitch));
        if (item.on) {
            if (item.audio) {
                LiveVoice v;
                v.pitch = item.pitch;
                v.channels = item.channels;
                v.loopStart = item.loopStart;
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
        } else {
            out.push_back(release(item.pitch, 0));
            if (held_[p] > 0) held_[p]--, heldTotal_--;
        }
    }
    inTail_.store(tail, std::memory_order_release);
    size_t keep = 0;
    for (size_t i = 0; i < timedOffs_.size(); i++) {
        auto& e = timedOffs_[i];
        e.first -= frames;
        if (e.first <= 0)
            out.push_back(release(e.second, std::max<int64_t>(0, frames + e.first)));
        else
            timedOffs_[keep++] = e;
    }
    timedOffs_.resize(keep);
}

void LiveNotes::addVoices(float* const signal[2], int channels, int frames, int sr) {
    int fade = std::max(1, (int)(synth::RELEASE_S * sr));
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

LiveNotes* find(const LiveMap* map, const Track* track) {
    if (!map) return nullptr;
    for (auto& kv : *map)
        if (kv.first == track) return kv.second.get();
    return nullptr;
}

}  // namespace live
