#include "audio/sampler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <limits>
#include <tuple>

#include "audio/padkit.h"
#include "audio/synth.h"
#include "audio/warp.h"
#include "core/util.h"
#include "effects/svf.h"
#include "model/pad_tracks.h"

namespace sampler {

const char* const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int pitch) {
    int octave = (pitch >= 0 ? pitch / 12 : (pitch - 11) / 12) - 1;
    return std::string(NOTE_NAMES[((pitch % 12) + 12) % 12]) + std::to_string(octave);
}

int rootFromName(const std::string& name) {
    // the last match of (?<![A-Za-z])([A-G])(#|b)?(-?\d)(?!\d)
    auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    bool found = false;
    int pitch = 60;
    size_t n = name.size();
    for (size_t i = 0; i < n; i++) {
        char letter = name[i];
        if (letter < 'A' || letter > 'G' || (i > 0 && alpha(name[i - 1]))) continue;
        static const int index[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
        auto tryAt = [&](size_t j, int accidental) -> bool {
            int sign = 1;
            if (j < n && name[j] == '-' && j + 1 < n && digit(name[j + 1])) sign = -1, j++;
            if (j >= n || !digit(name[j])) return false;
            if (j + 1 < n && digit(name[j + 1])) return false;
            int octave = sign * (name[j] - '0');
            pitch = std::min(127, std::max(0, index[letter - 'A'] + accidental + (octave + 1) * 12));
            found = true;
            return true;
        };
        size_t j = i + 1;
        if (j < n && (name[j] == '#' || name[j] == 'b') && tryAt(j + 1, name[j] == '#' ? 1 : -1)) continue;
        tryAt(j, 0);
    }
    return found ? pitch : 60;
}

namespace {
std::mutex cacheLock;
std::map<std::pair<const AudioBuffer*, int>, std::pair<AudioPtr, AudioPtr>> cache;  // (source, rate) -> (source, at rate)
// the sound as played (reversed, normalised, warped): (source, rate, reverse, normalise, warp, its bpm, the song's,
// semitones, warp mode) -> (source, sound)
using SoundKey = std::tuple<const AudioBuffer*, int, bool, bool, bool, double, double, double, int>;
std::map<SoundKey, std::pair<AudioPtr, AudioPtr>> sounds;
// where the slices start: (sound, grid, sensitivity, bpm, first, last) -> (sound, starts)
using SliceKey = std::tuple<const AudioBuffer*, int, double, double, int64_t, int64_t>;
std::map<SliceKey, std::pair<AudioPtr, std::shared_ptr<const std::vector<int64_t>>>> sliceCache;
constexpr size_t MOST_KEPT = 64;  // (sounds and slicings kept; more: the cache starts over)
}  // namespace

AudioPtr sampleAt(const SamplerSettings& s, int sr) {
    if (!s.sample || !s.sample->source) return nullptr;
    const ClipPtr& clip = s.sample;
    std::lock_guard<std::mutex> hold(cacheLock);
    auto key = std::make_pair(clip->source.get(), sr);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == clip->source) return it->second.second;
    if (cache.size() > 32) cache.clear();
    AudioPtr data = dsp::resample(clip->source, clip->sourceSr > 0 ? clip->sourceSr : sr, sr);
    cache[key] = {clip->source, data};
    return data;
}

double songBpm() {
    TempoMapPtr map = clip_hooks::currentTempoMap();
    return map && map->bpm > 0 ? map->bpm : warp::DEFAULT_BPM;
}

AudioPtr soundAt(const SamplerSettings& s, int sr, double semitones) {
    if (!s.reverse && !s.normalise && !s.warp) return sampleAt(s, sr);  // (as it always was)
    if (!s.sample || !s.sample->source) return nullptr;
    const AudioPtr& source = s.sample->source;
    double song = s.warp ? songBpm() : 0, bpm = s.warp ? std::max(1.0, s.bpm) : 0;
    SoundKey key{source.get(), sr, s.reverse, s.normalise, s.warp, bpm, song, s.warp ? semitones : 0, s.warp ? s.warpMode : 0};
    {
        std::lock_guard<std::mutex> hold(cacheLock);
        auto it = sounds.find(key);
        if (it != sounds.end() && it->second.first == source) return it->second.second;
    }
    AudioPtr made;
    if (s.warp) {  // (the song's tempo, steady, from the sample's start: audio/warp.h's renderer, on every core)
        warp::Job j;
        j.source = source;
        j.sourceSr = s.sample->sourceSr > 0 ? s.sample->sourceSr : sr;
        j.bpm = bpm;
        j.mode = (warp::Mode)std::min(2, std::max(0, s.warpMode));
        j.semitones = semitones;
        j.reverse = s.reverse;
        j.map = std::make_shared<const TempoMap>(song);
        j.sr = sr;
        made = warp::render(j);
    } else {
        made = sampleAt(s, sr);
        if (made && s.reverse) {
            auto back = std::make_shared<AudioBuffer>();
            int64_t n = made->frames();
            back->samples.resize((size_t)n * 2);
            for (int64_t i = 0; i < n; i++) {
                back->samples[(size_t)i * 2] = made->samples[(size_t)(n - 1 - i) * 2];
                back->samples[(size_t)i * 2 + 1] = made->samples[(size_t)(n - 1 - i) * 2 + 1];
            }
            made = back;
        }
    }
    if (made && s.normalise) {
        float top = 0;
        for (float v : made->samples) top = std::max(top, std::fabs(v));
        if (top > 1e-6f) {
            auto louder = std::make_shared<AudioBuffer>(*made);
            for (float& v : louder->samples) v /= top;
            made = louder;
        }
    }
    util::debugf("sampler: %s as played (%s%s%s%.2f semitones) at %d Hz, %lld frames", s.sample->name.c_str(),
                 s.reverse ? "reversed, " : "", s.normalise ? "normalised, " : "", s.warp ? "warped, " : "", semitones,
                 sr, made ? (long long)made->frames() : 0LL);
    std::lock_guard<std::mutex> hold(cacheLock);
    if (sounds.size() > MOST_KEPT) sounds.clear();
    sounds[key] = {source, made};
    return made;
}

std::vector<float> adsr(int64_t n, int64_t held, double attack, double decay, double sustain, double release,
                        bool oneShot, int sr) {
    std::vector<double> level((size_t)std::max<int64_t>(0, n));
    for (int64_t k = 0; k < n; k++) {
        double t = (double)k / sr;
        double v = attack > 0 ? std::min(1.0, t / attack) : 1.0;
        double after = t - attack;
        if (decay > 0) {
            if (after > 0) v = v * (1 - (1 - sustain) * std::min(1.0, std::max(0.0, after / decay)));
        } else if (after > 0) {
            v = sustain;
        }
        level[(size_t)k] = v;
    }
    if (!oneShot && held < n) {  // let go: down to nothing over the release
        double at = level[(size_t)std::max<int64_t>(0, held - 1)];
        double r = std::max(release, 1e-4);
        for (int64_t k = held; k < n; k++)
            level[(size_t)k] = at * std::min(1.0, std::max(0.0, 1 - ((double)(k - held) / sr) / r));
    }
    return std::vector<float>(level.begin(), level.end());
}

std::vector<float> envelope(int64_t n, int64_t held, const SamplerSettings& s, int sr) {
    return adsr(n, held, s.attack, s.decay, s.sustain, s.release, s.oneShot, sr);
}

double lfoHz(const SamplerSettings& s) {
    if (!s.lfoSync) return std::max(0.0, s.lfoRate);
    int i = std::min(effects::SYNC_COUNT - 1, std::max(0, s.lfoBeats));
    return songBpm() / 60.0 / effects::SYNC_BEATS[i];
}

std::vector<int64_t> sliceStarts(const SamplerSettings& s, int sr) {
    AudioPtr data = soundAt(s, sr, s.transpose);
    int64_t len = data ? data->frames() : 0;
    if (len < 3) return {};
    int64_t first = std::min((int64_t)(s.start * len), len - 2);
    int64_t last = std::min(std::max((int64_t)(s.end * len), first + 2), len - 1);
    double bpm = s.warp ? songBpm() : std::max(1.0, s.bpm);  // (warped: the sound is at the song's tempo)
    int grid = s.sliceGrid > 0 ? s.sliceGrid : 0;
    SliceKey key{data.get(), grid, grid ? 0.0 : s.sensitivity, grid ? bpm : 0.0, first, last};
    {
        std::lock_guard<std::mutex> hold(cacheLock);
        auto it = sliceCache.find(key);
        if (it != sliceCache.end() && it->second.first == data) return *it->second.second;
    }
    const float* region = data->samples.data() + first * 2;
    int64_t frames = last - first;
    std::vector<int64_t> hits;
    if (grid) {  // (padkit's grid at the sample's own tempo)
        Clip c;
        c.sourceBpm = bpm;
        hits = padkit::gridStarts(c, frames, Timing(), sr, grid);
    } else {
        hits = dsp::Onsets(region, frames, sr).find(std::min(1.0, std::max(0.0, s.sensitivity)));
    }
    std::vector<int64_t> starts = padkit::sliceStarts(region, frames, std::move(hits), MAX_SLICES);
    for (int64_t& f : starts) f += first;
    util::debugf("sampler: %zu slices (%s)", starts.size(), grid ? ("a 1/" + std::to_string(grid) + " grid").c_str() : "transients");
    std::lock_guard<std::mutex> hold(cacheLock);
    if (sliceCache.size() > MOST_KEPT) sliceCache.clear();
    sliceCache[key] = {data, std::make_shared<const std::vector<int64_t>>(starts)};
    return starts;
}

namespace {
double heardPitch(const Note& n) { return (int)n.pitch + n.fine / 100; }  // (its fine tuning in cents)

// What a note plays: the sound, its part (first … last, looping back to loopFrom), how fast (ratio: frames of the
// sound per frame) and whether it plays to its end whatever the note's length (one-shot, a slice)
struct Span {
    AudioPtr data;
    int64_t first = 0, last = 0, loopFrom = 0;
    double ratio = 1;
    bool oneShot = false, loops = false;
};
bool spanFor(const SamplerSettings& s, int pitch, double cents, int sr, Span& sp) {
    bool warped = s.warp;
    double semis = s.slice ? s.transpose + cents / 100 : pitch - s.root + s.transpose + cents / 100;
    sp.data = soundAt(s, sr, semis);
    int64_t len = sp.data ? sp.data->frames() : 0;
    if (len < 3) return false;  // (nothing to play)
    // the part played: at least a frame long, whatever start and end say (a short sample, start at 99.9 %)
    sp.first = std::min((int64_t)(s.start * len), len - 2);
    sp.last = std::min(std::max((int64_t)(s.end * len), sp.first + 2), len - 1);
    sp.oneShot = s.oneShot || s.slice;
    sp.loops = s.loop && !sp.oneShot;
    sp.loopFrom = std::min(std::max(sp.first, (int64_t)(s.loopStart * len)), sp.last - 1);
    if (s.slice) {  // (slice i on key root + i)
        std::vector<int64_t> starts = sliceStarts(s, sr);
        int i = pitch - s.root;
        if (i < 0 || i >= (int)starts.size()) return false;
        if (i + 1 < (int)starts.size()) sp.last = std::max(starts[(size_t)i] + 2, starts[(size_t)i + 1]);
        sp.first = starts[(size_t)i];
        sp.loopFrom = sp.first;
        sp.ratio = warped ? 1.0 : std::pow(2.0, semis / 12.0);
        return true;
    }
    sp.ratio = warped ? 1.0 : std::pow(2.0, (pitch - s.root + s.transpose + cents / 100) / 12.0);
    return true;
}

bool lfoMovesPitch(const SamplerSettings& s) { return s.lfoDepth > 0 && s.lfoTarget == SamplerSettings::ToPitch; }

// the filter, the LFO's volume and pan on a voice's audio (stereo interleaved; held: frames the note is held;
// key: its pitch, for key tracking). Nothing when they are off.
void shape(std::vector<float>& out, int64_t held, double key, const SamplerSettings& s, int sr, bool oneShot) {
    bool lfoOn = s.lfoDepth > 0 && s.lfoTarget != SamplerSettings::ToPitch;
    bool filtered = s.filter > SamplerSettings::NoFilter && s.filter <= SamplerSettings::Notch;
    if (!filtered && !lfoOn) return;
    int64_t n = (int64_t)out.size() / 2;
    if (n <= 0) return;
    const double pi = 3.14159265358979323846;
    int type = s.filter - 1;
    bool two = s.steep && (type == effects::LOWPASS || type == effects::HIGHPASS);
    double k1 = 1, k2 = 1;
    effects::svfDampings(type, s.steep, s.resonance, k1, k2);
    std::vector<float> fenv;
    if (filtered && s.filterEnv != 0) fenv = adsr(n, held, s.fAttack, s.fDecay, s.fSustain, s.fRelease, oneShot, sr);
    double base = std::log2(std::min(22000.0, std::max(10.0, s.cutoff))) + s.keyTrack * (key - s.root) / 12;
    double lowest = std::log2(10.0), highest = std::log2(std::min(20000.0, sr * 0.45));
    double depth = std::min(1.0, std::max(0.0, s.lfoDepth)), step = lfoHz(s) / sr;
    int form = std::min((int)effects::LFO_SHAPES.size() - 1, std::max(0, s.lfoShape));
    effects::Lfo lfo;
    auto gAt = [&](int64_t k, const effects::Lfo& at) {  // (the cutoff at frame k: its envelope and the LFO)
        double oct = base + (fenv.empty() ? 0.0 : s.filterEnv * fenv[(size_t)k]);
        if (lfoOn && s.lfoTarget == SamplerSettings::ToFilter) oct += 4 * depth * at.at(form, 0);
        return std::tan(pi * std::exp2(std::min(highest, std::max(lowest, oct))) / sr);
    };
    effects::Svf a, b;
    constexpr int BLOCK = 16;  // (the cutoff worked out every 16 frames, gliding in between: no zipper, no click)
    double gFrom = filtered ? gAt(0, lfo) : 0;
    for (int64_t j = 0; j < n; j += BLOCK) {
        int64_t m = std::min<int64_t>(BLOCK, n - j);
        double gTo = 0;
        if (filtered) {
            effects::Lfo end = lfo;
            end.advance(step * (double)(m - 1));
            gTo = gAt(j + m - 1, end);
        }
        for (int64_t i = 0; i < m; i++) {
            float* f = &out[(size_t)(j + i) * 2];
            double l = f[0], r = f[1];
            if (filtered) {
                double g = gFrom + (gTo - gFrom) * (double)(i + 1) / (double)m;
                l = a.step(0, l, g, two ? k1 : k2, type);
                r = a.step(1, r, g, two ? k1 : k2, type);
                if (two) l = b.step(0, l, g, k2, type), r = b.step(1, r, g, k2, type);
            }
            if (lfoOn) {
                double w = lfo.at(form, 0);
                if (s.lfoTarget == SamplerSettings::ToVolume) {
                    double v = 1 - depth * (0.5 - 0.5 * w);
                    l *= v, r *= v;
                } else if (s.lfoTarget == SamplerSettings::ToPan) {
                    double pl, pr;
                    note_props::panGains(depth * w, pl, pr);
                    l *= pl, r *= pr;
                }
            }
            f[0] = (float)l, f[1] = (float)r;
            lfo.advance(step);
        }
        gFrom = gTo;
    }
    if (filtered) {  // (its ringing cut at the very end: faded, so nothing clicks)
        int64_t edge = std::min(n, (int64_t)(EDGE_S * sr));
        for (int64_t k = 0; k < edge; k++) {
            float g = (float)(edge > 1 ? 1.0 - (double)k / (double)(edge - 1) : 1.0);
            out[(size_t)(n - edge + k) * 2] *= g;
            out[(size_t)(n - edge + k) * 2 + 1] *= g;
        }
    }
}

// One voice of the sampler playing `voice` (indexes into notes, in time order: a note, then the sliding notes
// that took its voice over), starting at `fromPitch` (the pitch the first note slides from; its own: none):
// the sample plays on from where it is while its speed glides to each next note's pitch (synth's renderVoice,
// for the sampler), its level and pan gliding too, each over its note's glide (glides: seconds per note; null:
// note_props::GLIDE_S), the LFO moving the pitch too when it is set to. Stereo interleaved, at most `most`
// frames; tailS as in renderNote.
std::vector<float> renderVoice(const SamplerSettings& s, const std::vector<Note>& notes, const std::vector<int>& voice,
                               double fromPitch, int sr, double tailS, int64_t most,
                               const std::vector<double>* glides = nullptr) {
    const Note& head = notes[(size_t)voice.front()];
    Span sp;
    if (most <= 0 || !spanFor(s, (int)head.pitch, head.fine, sr, sp)) return {};
    AudioPtr data = sp.data;
    int64_t first = sp.first, last = sp.last;
    bool plain = !s.warp && !s.slice;  // (the speed from the pitch, as it always was)
    double headPitch = heardPitch(head);
    int64_t a0 = (int64_t)(head.start * sr);
    int64_t held = std::max<int64_t>(1, (int64_t)((notes[(size_t)voice.back()].end - head.start) * sr));
    int64_t limit = sp.oneShot ? most : held + (int64_t)(s.release * sr);
    if (tailS >= 0) limit = std::min(limit, held + (int64_t)(tailS * sr));
    limit = std::min(limit, most);
    struct Part { int64_t at; double pitch, gain, pan, glide; };
    std::vector<Part> parts;
    double level = std::pow(10.0, s.gainDb / 20);
    for (int v : voice) {
        const Note& note = notes[(size_t)v];
        double seconds = glides ? (*glides)[(size_t)v] : note_props::GLIDE_S;
        parts.push_back({std::max<int64_t>(0, (int64_t)(note.start * sr) - a0), heardPitch(note),
                         level * (1 - s.velocity + s.velocity * note.velocity / 127), note.pan,
                         std::max(1.0, seconds * sr)});
    }
    // where in the sample each frame plays, and its gain and pan, until the sample's end (or the limit)
    bool loops = sp.loops;
    double span = (double)std::max<int64_t>(1, last - sp.loopFrom), from = (double)sp.loopFrom;
    bool wobble = lfoMovesPitch(s);
    effects::Lfo lfo;
    double lfoStep = wobble ? lfoHz(s) / sr : 0, lfoSemis = 12 * std::min(1.0, s.lfoDepth);
    int form = std::min((int)effects::LFO_SHAPES.size() - 1, std::max(0, s.lfoShape));
    std::vector<double> at, gains, pans;
    size_t part = 0;
    double pitchFrom = fromPitch, gainFrom = parts[0].gain, panFrom = parts[0].pan;
    double pitch = fromPitch, gain = parts[0].gain, pan = parts[0].pan;
    double pos = (double)first, lastPitch = -1e9, ratio = 1;
    for (int64_t k = 0; k < limit; k++) {
        if (loops && pos >= (double)last) pos = from + std::fmod(pos - from, span);
        if (!loops && pos >= (double)last) break;  // (the sample's end)
        while (part + 1 < parts.size() && k >= parts[part + 1].at) {
            part++;
            pitchFrom = pitch, gainFrom = gain, panFrom = pan;
        }
        const Part& p = parts[part];
        double along = std::min(1.0, (double)(k - p.at) / p.glide);
        pitch = pitchFrom + (p.pitch - pitchFrom) * along;
        gain = gainFrom + (p.gain - gainFrom) * along;
        pan = panFrom + (p.pan - panFrom) * along;
        double heard = pitch;
        if (wobble) {
            heard += lfoSemis * lfo.at(form, 0);
            lfo.advance(lfoStep);
        }
        if (heard != lastPitch) {
            lastPitch = heard;
            ratio = plain ? std::pow(2.0, (heard - s.root + s.transpose) / 12.0)
                          : sp.ratio * std::pow(2.0, (heard - headPitch) / 12.0);
        }
        at.push_back(pos);
        gains.push_back(gain);
        pans.push_back(pan);
        pos += ratio;
    }
    int64_t n = (int64_t)at.size();
    if (n <= 0) return {};
    std::vector<float> env = adsr(n, held, s.attack, s.decay, s.sustain, s.release, sp.oneShot, sr);
    int64_t edge = std::min(n, (int64_t)(EDGE_S * sr));
    for (int64_t k = 0; k < edge; k++)
        env[(size_t)(n - edge + k)] *= (float)(edge > 1 ? 1.0 - (double)k / (double)(edge - 1) : 1.0);
    std::vector<float> out((size_t)n * 2);
    const float* d = data->samples.data();
    double l = 1, r = 1, lastPan = -1e9;
    for (int64_t k = 0; k < n; k++) {
        double q = std::min(at[(size_t)k], (double)(last - 1));
        int64_t i = (int64_t)q;
        float frac = (float)(q - (double)i);
        if (pans[(size_t)k] != lastPan) note_props::panGains(lastPan = pans[(size_t)k], l, r);
        float g = (float)(env[(size_t)k] * gains[(size_t)k]);
        for (int c = 0; c < 2; c++) {
            float sound = d[i * 2 + c] * (1.0f - frac) + d[(i + 1) * 2 + c] * frac;
            out[(size_t)k * 2 + c] = (float)(sound * g * (c == 0 ? l : r));
        }
    }
    shape(out, held, headPitch, s, sr, sp.oneShot);
    return out;
}
}  // namespace

std::vector<float> renderNote(const SamplerSettings& s, int pitch, double velocity, double durationS, int sr,
                              double tailS, double cents, double fromPitch) {
    bool glides = fromPitch >= 0 && !s.slice;
    if (glides || lfoMovesPitch(s)) {  // (its pitch moving: played as a voice)
        Note note;
        note.pitch = pitch;
        note.velocity = velocity;
        note.start = 0;
        note.end = std::max(durationS, 1.0 / sr);
        note.fine = cents;
        std::vector<double> glide{glides ? s.glide : note_props::GLIDE_S};
        return renderVoice(s, {note}, {0}, glides ? fromPitch : heardPitch(note), sr, tailS,
                           std::numeric_limits<int64_t>::max() / 4, &glide);
    }
    Span sp;
    if (!spanFor(s, pitch, cents, sr, sp)) return {};
    AudioPtr data = sp.data;
    int64_t first = sp.first, last = sp.last;
    double ratio = sp.ratio;
    int64_t held = std::max<int64_t>(1, (int64_t)(durationS * sr));
    int64_t toEnd = (int64_t)((double)(last - first) / ratio);
    int64_t n;
    if (sp.oneShot)
        n = toEnd;
    else if (s.loop)
        n = held + (int64_t)(s.release * sr);
    else
        n = std::min(toEnd, held + (int64_t)(s.release * sr));
    if (tailS >= 0) n = std::min(n, held + (int64_t)(tailS * sr));
    if (n <= 0) return {};
    std::vector<float> level = adsr(n, held, s.attack, s.decay, s.sustain, s.release, sp.oneShot, sr);
    float gain = (float)(std::pow(10.0, s.gainDb / 20) * (1 - s.velocity + s.velocity * velocity / 127));
    for (auto& v : level) v *= gain;
    int64_t edge = std::min(n, (int64_t)(EDGE_S * sr));
    for (int64_t k = 0; k < edge; k++)  // np.linspace(1, 0, edge)
        level[(size_t)(n - edge + k)] *= (float)(edge > 1 ? 1.0 - (double)k / (double)(edge - 1) : 1.0);
    std::vector<float> out((size_t)n * 2);
    bool loops = sp.loops;
    double span = (double)std::max<int64_t>(1, last - sp.loopFrom), from = (double)sp.loopFrom;
    const float* d = data->samples.data();
    for (int64_t k = 0; k < n; k++) {
        double pos = (double)first + (double)k * ratio;
        if (loops && pos >= (double)last) pos = from + std::fmod(pos - from, span);
        pos = std::min(pos, (double)(last - 1));
        int64_t i = (int64_t)pos;
        float frac = (float)(pos - (double)i);
        for (int c = 0; c < 2; c++) {
            float a = d[i * 2 + c], b = d[(i + 1) * 2 + c];
            float sound = a * (1.0f - frac) + b * frac;
            out[(size_t)k * 2 + c] = sound * level[(size_t)k];
        }
    }
    shape(out, held, pitch + cents / 100, s, sr, sp.oneShot);
    return out;
}

AudioPtr render(const SamplerSettings& s, const std::vector<Note>& notes, int sr, double lengthS) {
    // (one-shot: every note plays to the sample's end, however short it is: only the clip's tail limits it)
    bool oneShot = s.oneShot || s.slice;
    double tail = oneShot ? MAX_TAIL_S : std::min(MAX_TAIL_S, std::max(synth::RELEASE_S, s.release));
    int64_t total = (int64_t)((lengthS + tail) * sr) + 1;
    auto out = std::make_shared<AudioBuffer>();
    out->samples.assign((size_t)std::max<int64_t>(0, total) * 2, 0.0f);
    // the voices sliding notes carry on (note_props::slides): a note taken over is played by its voice
    std::vector<note_props::Slide> slid = note_props::slides(notes);
    std::vector<double> glideOf, *glides = nullptr;
    if (!s.slice && (s.glide > 0 || s.mono)) {  // (glide: every note slides from the one before; mono: takes it over)
        std::vector<Note> all = notes;
        for (Note& n : all) n.slide = true;
        std::vector<note_props::Slide> every = note_props::slides(all);
        glideOf.assign(notes.size(), 0.0);
        for (size_t i = 0; i < notes.size(); i++) {
            bool own = notes[i].slide;
            note_props::Slide made;
            bool legato = every[i].from >= 0 && (s.mono ? every[i].legato : own && slid[i].legato);
            if (every[i].from >= 0 && (s.glide > 0 || own || legato)) made.from = every[i].from;
            made.legato = legato;
            slid[i] = made;
            glideOf[i] = s.glide > 0 ? s.glide : own ? note_props::GLIDE_S : 0.0;  // (mono, no glide: a jump)
        }
        glides = &glideOf;
        util::debugf("sampler: %zu notes, %s, gliding %.3f s", notes.size(), s.mono ? "mono" : "poly", s.glide);
    }
    std::vector<int> takenBy(notes.size(), -1);
    for (size_t i = 0; i < notes.size(); i++)
        if (slid[i].legato) takenBy[(size_t)slid[i].from] = (int)i;
    // (a note played again is rendered once)
    std::map<std::tuple<int, double, int64_t, double>, std::vector<float>> rendered;
    for (size_t j = 0; j < notes.size(); j++) {
        const Note& note = notes[j];
        if (slid[j].legato) continue;  // (played by the voice it took over)
        int64_t a = (int64_t)(note.start * sr);
        if (a >= total) continue;
        if (!s.slice && (takenBy[j] >= 0 || slid[j].from >= 0)) {  // a voice gliding from note to note
            if (a < 0) continue;
            std::vector<int> voice{(int)j};
            while (takenBy[(size_t)voice.back()] >= 0 && voice.size() <= notes.size())
                voice.push_back(takenBy[(size_t)voice.back()]);
            double from = slid[j].from >= 0 ? heardPitch(notes[(size_t)slid[j].from]) : heardPitch(note);
            std::vector<float> sound = renderVoice(s, notes, voice, from, sr, oneShot ? -1 : tail, total - a, glides);
            float* o = out->samples.data() + a * 2;
            for (size_t k = 0; k < sound.size(); k++) o[k] += sound[k];
            continue;
        }
        auto key = std::make_tuple((int)note.pitch, note.velocity,
                                   std::max<int64_t>(1, (int64_t)((note.end - note.start) * sr)), note.fine);
        auto it = rendered.find(key);
        if (it == rendered.end())
            it = rendered
                     .emplace(key, renderNote(s, (int)note.pitch, note.velocity, note.end - note.start, sr,
                                              oneShot ? -1 : tail, note.fine))
                     .first;
        const std::vector<float>& sound = it->second;
        int64_t frames = std::min<int64_t>((int64_t)sound.size() / 2, total - a);
        if (a < 0) continue;  // (a note before the clip's start: never made by the model)
        float* o = out->samples.data() + a * 2;
        if (note.pan == 0) {
            for (int64_t k = 0; k < frames * 2; k++) o[k] += sound[(size_t)k];
        } else {  // (its balance: the sample's left and right, each turned down as the track's pan does)
            double l, r;
            note_props::panGains(note.pan, l, r);
            for (int64_t k = 0; k < frames; k++) {
                o[2 * k] += (float)(sound[(size_t)(2 * k)] * l);
                o[2 * k + 1] += (float)(sound[(size_t)(2 * k + 1)] * r);
            }
        }
    }
    return out;
}

double loopPeriod(const SamplerSettings& s, int pitch, int sr) {
    if (s.lfoDepth > 0) {  // (the LFO is what is heard repeating)
        double hz = lfoHz(s);
        return hz > 0 ? sr / hz : -1;
    }
    Span sp;
    if (!spanFor(s, pitch, 0, sr, sp)) return -1;
    return (double)std::max<int64_t>(1, sp.last - sp.loopFrom) / sp.ratio;
}

AudioPtr renderNotes(const Track& track, const std::vector<Note>& notes, int sr, double lengthS) {
    PadKitPtr kit = std::atomic_load(&track.padKit);
    if (kit && track.playsPadKit()) return padkit::render(*kit, notes, sr, lengthS);
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (s && track.playsSampler()) return render(*s, notes, sr, lengthS);
    return synth::render(notes, sr, lengthS);
}

live::NoteRender noteRenderer(const Track& track) {
    live::NoteRender r;
    PadKitPtr kit = std::atomic_load(&track.padKit);
    if (kit && track.playsPadKit()) return padkit::noteRenderer(kit);  // (each key its pad)
    SamplerPtr s = std::atomic_load(&track.sampler);
    if (!s || !track.playsSampler()) return r;  // (the built-in synth)
    r.render = [s](int pitch, int velocity, double durationS, int sr) {
        return renderNote(*s, pitch, velocity, durationS, sr);
    };
    if (s->glide > 0 && !s->slice)  // (a live note gliding from the one played before it)
        r.glideFrom = [s](int pitch, int velocity, double durationS, int sr, double from) {
            return renderNote(*s, pitch, velocity, durationS, sr, -1, 0, from);
        };
    r.channels = 2;
    r.oneShot = s->oneShot || s->slice;  // (such a note plays to its end, letting go of the key or not)
    r.sustains = s->loop;    // (a held note loops; else it plays the sample to its end)
    r.mono = s->mono && !s->slice;  // (a new key lets go of the others)
    r.velocityFloor = 1 - s->velocity;  // (renderNote's level: 1 - velocity + velocity * v / 127)
    r.period = [s](int pitch, int sr) { return loopPeriod(*s, pitch, sr); };
    return r;
}

bool followsTempo(const SamplerSettings& s) { return s.warp || (s.lfoSync && s.lfoDepth > 0) || (s.slice && s.sliceGrid > 0); }

void install() {
    clip_hooks::renderSampler = [](const Track& track, const std::vector<Note>& notes, int sr, double lengthS) {
        PadKitPtr kit = std::atomic_load(&track.padKit);
        // (its pads playing on tracks of their own kept apart: model/pad_tracks.h)
        if (kit && track.playsPadKit()) return padkit::render(*kit, notes, sr, lengthS, pad_tracks::routedMask(track));
        SamplerPtr s = std::atomic_load(&track.sampler);
        return s ? render(*s, notes, sr, lengthS) : synth::render(notes, sr, lengthS);
    };
}

}  // namespace sampler
