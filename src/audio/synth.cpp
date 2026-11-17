#include "audio/synth.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <tuple>

// --- what a note's own properties do (note_props) ---

namespace note_props {

namespace {
uint64_t mix64(uint64_t x) {  // (splitmix64's finalizer: every bit of x stirs every bit of the result)
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
}  // namespace

bool sounds(const Note& n, int64_t pass) {
    if (n.chance >= 100) return true;
    if (!(n.chance > 0)) return false;  // (0 %, or not a number)
    // the note's start to the microsecond and its pitch to the cent: what a saved and opened project gives back
    uint64_t key = mix64((uint64_t)(int64_t)std::llround(n.start * 1e6));
    key = mix64(key ^ (uint64_t)(int64_t)std::llround(n.pitch * 100));
    key = mix64(key ^ ((uint64_t)pass * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL));
    double u = (double)(key >> 11) * (1.0 / 9007199254740992.0);  // [0, 1)
    return u * 100 < n.chance;
}

void perform(const Note& n, int64_t pass, std::vector<Note>& out) {
    if (!sounds(n, pass)) return;
    int hits = std::min(MAX_REPEATS, std::max(1, n.repeats));
    if (hits == 1) {
        out.push_back(n);
        return;
    }
    double each = (n.end - n.start) / hits;
    for (int k = 0; k < hits; k++) {
        Note hit = n;
        hit.start = n.start + k * each;
        hit.end = k == hits - 1 ? n.end : n.start + (k + 1) * each;
        hit.repeats = 1;
        hit.slide = n.slide && k == 0;  // (the later hits strike the note again)
        out.push_back(hit);
    }
}

std::vector<Slide> slides(const std::vector<Note>& notes) {
    std::vector<Slide> out(notes.size());
    bool any = false;
    for (const Note& n : notes) any = any || n.slide;
    if (!any) return out;
    std::vector<int> order(notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        return notes[a].pitch < notes[b].pitch;
    });
    std::vector<char> taken(notes.size(), 0);
    for (size_t k = 0; k < order.size();) {
        size_t j = k;  // [k, j): the notes starting together
        while (j < order.size() && notes[order[j]].start == notes[order[k]].start) j++;
        int from = k > 0 ? order[k - 1] : -1;  // (the last to start before them; of several, the highest)
        for (size_t m = k; m < j && from >= 0; m++) {
            int i = order[m];
            if (!notes[i].slide) continue;
            out[i].from = from;
            if (!taken[from] && notes[from].end >= notes[i].start - LEGATO_S) {
                out[i].legato = true;
                taken[from] = 1;
            }
        }
        k = j;
    }
    return out;
}

void panGains(double pan, double& left, double& right) {
    const double PI = 3.14159265358979323846;
    pan = std::min(1.0, std::max(-1.0, pan));
    left = pan > 0 ? std::cos(pan * PI / 2) : 1.0;
    right = pan < 0 ? std::cos(pan * PI / 2) : 1.0;
}

}  // namespace note_props

namespace synth {

static const double HARMONICS[][2] = {{1, 1.0}, {2, 0.35}, {3, 0.15}};

double noteFrequency(double pitch) { return 440.0 * std::pow(2.0, (pitch - 69) / 12.0); }

namespace {
// each pitch's wave (its harmonics added up, before the envelope), least recently used first
struct Waves {
    std::mutex lock;
    std::list<std::pair<std::pair<double, int>, std::shared_ptr<const std::vector<double>>>> kept;
    size_t bytes = 0;
};
Waves& waves() {
    static Waves w;
    return w;
}

std::shared_ptr<const std::vector<double>> makeWave(double pitch, int64_t n, int sr) {
    auto wave = std::make_shared<std::vector<double>>((size_t)n);
    double f = noteFrequency(pitch);
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / sr, v = 0;
        for (auto& h : HARMONICS)
            if (f * h[0] < sr / 2.0) v += h[1] * std::sin(2 * M_PI * f * h[0] * t);
        (*wave)[(size_t)i] = v;
    }
    return wave;
}
}  // namespace

std::shared_ptr<const std::vector<double>> waveOf(double pitch, int64_t n, int sr) {
    Waves& w = waves();
    std::lock_guard<std::mutex> hold(w.lock);
    auto key = std::make_pair(pitch, sr);
    std::shared_ptr<const std::vector<double>> wave;
    for (auto it = w.kept.begin(); it != w.kept.end(); ++it)
        if (it->first == key) {
            wave = it->second;
            w.kept.erase(it);
            break;
        }
    if (!wave || (int64_t)wave->size() < n) {
        if (n > KEPT_LONGEST_S * sr) {  // (longer notes: their wave is worked out each time)
            if (wave) w.kept.emplace_back(key, wave);
            return makeWave(pitch, n, sr);
        }
        if (wave) w.bytes -= wave->size() * sizeof(double);
        wave = makeWave(pitch, std::max<int64_t>(n, sr), sr);
        w.bytes += wave->size() * sizeof(double);
        while (!w.kept.empty() && w.bytes > KEPT_BYTES) {
            w.bytes -= w.kept.front().second->size() * sizeof(double);
            w.kept.pop_front();
        }
    }
    w.kept.emplace_back(key, wave);  // (now the newest)
    return wave;
}

std::vector<float> renderNote(double pitch, double velocity, double durationS, int sr) {
    int64_t hold = std::max<int64_t>(1, (int64_t)(durationS * sr));
    int64_t n = hold + (int64_t)(RELEASE_S * sr);
    std::vector<float> out((size_t)n);
    auto wave = waveOf(pitch, n, sr);
    double gain = NOTE_GAIN * velocity / 127.0;
    int64_t tail = n - hold;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / sr;
        double env = std::min(1.0, t / ATTACK_S);
        if (i >= hold) {  // np.linspace(1, 0, n - hold)
            int64_t k = i - hold;
            env *= tail > 1 ? 1.0 - (double)k / (double)(tail - 1) : 1.0;
        }
        out[(size_t)i] = (float)(gain * (*wave)[(size_t)i] * env);
    }
    return out;
}

namespace {
double heardPitch(const Note& n) { return n.pitch + n.fine / 100; }  // (its fine tuning in cents)

// One voice playing `voice` (indexes into notes, in time order: the first note, then the sliding notes that
// took its voice over), starting at `fromPitch` (the pitch the first note slides from; its own: none), as
// stereo frames into left / right (resized). Its harmonics' phases run on unbroken as the pitch glides; the
// attack is the first note's, the release the last one's; level and pan glide with the pitch.
void renderVoice(const std::vector<Note>& notes, const std::vector<int>& voice, double fromPitch, int sr,
                 std::vector<float>& left, std::vector<float>& right) {
    const Note& first = notes[(size_t)voice.front()];
    int64_t a0 = (int64_t)(first.start * sr);
    int64_t hold = std::max<int64_t>(1, (int64_t)((notes[(size_t)voice.back()].end - first.start) * sr));
    int64_t n = hold + (int64_t)(RELEASE_S * sr), tail = n - hold;
    struct Part { int64_t at; double pitch, gain, pan; };
    std::vector<Part> parts;
    for (int v : voice) {
        const Note& note = notes[(size_t)v];
        parts.push_back({std::max<int64_t>(0, (int64_t)(note.start * sr) - a0), heardPitch(note),
                         NOTE_GAIN * note.velocity / 127.0, note.pan});
    }
    left.assign((size_t)n, 0.0f);
    right.assign((size_t)n, 0.0f);
    double glide = std::max(1.0, note_props::GLIDE_S * sr);
    double phase[3] = {0, 0, 0};
    size_t part = 0;
    double pitchFrom = fromPitch, gainFrom = parts[0].gain, panFrom = parts[0].pan;
    double pitch = fromPitch, gain = parts[0].gain, pan = parts[0].pan;
    double lastPitch = -1e9, f = 0, l = 1, r = 1, lastPan = -1e9;
    for (int64_t i = 0; i < n; i++) {
        while (part + 1 < parts.size() && i >= parts[part + 1].at) {  // the next note takes the voice over
            part++;
            pitchFrom = pitch, gainFrom = gain, panFrom = pan;
        }
        const Part& p = parts[part];
        double along = std::min(1.0, (double)(i - p.at) / glide);
        pitch = pitchFrom + (p.pitch - pitchFrom) * along;
        gain = gainFrom + (p.gain - gainFrom) * along;
        pan = panFrom + (p.pan - panFrom) * along;
        if (pitch != lastPitch) f = noteFrequency(lastPitch = pitch);
        if (pan != lastPan) note_props::panGains(lastPan = pan, l, r);
        double v = 0;
        for (int h = 0; h < 3; h++) {
            if (f * HARMONICS[h][0] < sr / 2.0) v += HARMONICS[h][1] * std::sin(phase[h]);
            phase[h] = std::fmod(phase[h] + 2 * M_PI * f * HARMONICS[h][0] / sr, 2 * M_PI);
        }
        double env = std::min(1.0, ((double)i / sr) / ATTACK_S);
        if (i >= hold) env *= tail > 1 ? 1.0 - (double)(i - hold) / (double)(tail - 1) : 1.0;
        double s = gain * v * env;
        left[(size_t)i] = (float)(s * l);
        right[(size_t)i] = (float)(s * r);
    }
}
}  // namespace

AudioPtr render(const std::vector<Note>& notes, int sr, double lengthS) {
    int64_t len = (int64_t)((lengthS + RELEASE_S) * sr) + 1;
    std::vector<float> mono((size_t)len, 0.0f);
    std::vector<float> sideL, sideR;  // (panned notes: made only when there are some)
    auto addPanned = [&](const float* l, const float* r, int64_t frames, int64_t a) {
        if (sideL.empty()) sideL.assign((size_t)len, 0.0f), sideR.assign((size_t)len, 0.0f);
        int64_t m = std::min(frames, len - a);
        for (int64_t i = 0; i < m; i++) sideL[(size_t)(a + i)] += l[i], sideR[(size_t)(a + i)] += r[i];
    };
    // the voices sliding notes carry on (note_props::slides): a note taken over is played by its voice
    std::vector<note_props::Slide> slid = note_props::slides(notes);
    std::vector<int> takenBy(notes.size(), -1);
    for (size_t i = 0; i < notes.size(); i++)
        if (slid[i].legato) takenBy[(size_t)slid[i].from] = (int)i;
    // a note played again (same pitch, velocity and length: a part's repeats, a drum pattern) is rendered
    // once and added again
    std::map<std::tuple<double, double, int64_t>, std::vector<float>> rendered;
    std::vector<float> voiceL, voiceR;
    for (size_t k = 0; k < notes.size(); k++) {
        const Note& note = notes[k];
        if (slid[k].legato) continue;  // (played by the voice it took over)
        int64_t a = (int64_t)(note.start * sr);
        if (a >= len || a < 0) continue;
        if (takenBy[k] < 0 && slid[k].from < 0) {  // an ordinary note
            double pitch = heardPitch(note);
            auto key = std::make_tuple(pitch, note.velocity, std::max<int64_t>(1, (int64_t)((note.end - note.start) * sr)));
            auto it = rendered.find(key);
            if (it == rendered.end())
                it = rendered.emplace(key, renderNote(pitch, note.velocity, note.end - note.start, sr)).first;
            const std::vector<float>& wave = it->second;
            int64_t m = std::min<int64_t>((int64_t)wave.size(), len - a);
            if (note.pan == 0) {
                for (int64_t i = 0; i < m; i++) mono[(size_t)(a + i)] += wave[(size_t)i];
            } else {
                double l, r;
                note_props::panGains(note.pan, l, r);
                voiceL.resize((size_t)m), voiceR.resize((size_t)m);
                for (int64_t i = 0; i < m; i++)
                    voiceL[(size_t)i] = (float)(wave[(size_t)i] * l), voiceR[(size_t)i] = (float)(wave[(size_t)i] * r);
                addPanned(voiceL.data(), voiceR.data(), m, a);
            }
            continue;
        }
        std::vector<int> voice{(int)k};  // a voice gliding from note to note
        bool panned = note.pan != 0;
        while (takenBy[(size_t)voice.back()] >= 0 && voice.size() <= notes.size()) {
            voice.push_back(takenBy[(size_t)voice.back()]);
            panned = panned || notes[(size_t)voice.back()].pan != 0;
        }
        double from = slid[k].from >= 0 ? heardPitch(notes[(size_t)slid[k].from]) : heardPitch(note);
        renderVoice(notes, voice, from, sr, voiceL, voiceR);
        int64_t m = std::min<int64_t>((int64_t)voiceL.size(), len - a);
        if (!panned) {
            for (int64_t i = 0; i < m; i++) mono[(size_t)(a + i)] += voiceL[(size_t)i];
        } else {
            addPanned(voiceL.data(), voiceR.data(), m, a);
        }
    }
    auto out = std::make_shared<AudioBuffer>();
    out->samples.resize((size_t)len * 2);
    if (sideL.empty()) {
        for (int64_t i = 0; i < len; i++) out->samples[2 * i] = out->samples[2 * i + 1] = mono[(size_t)i];
    } else {
        for (int64_t i = 0; i < len; i++) {
            out->samples[2 * i] = mono[(size_t)i] + sideL[(size_t)i];
            out->samples[2 * i + 1] = mono[(size_t)i] + sideR[(size_t)i];
        }
    }
    return out;
}

}  // namespace synth
