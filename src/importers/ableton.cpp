// Converts an Ableton Live set (.als) into an Exbit project (importers/ableton.py; see ableton.h).
#include "importers/ableton.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

#include "audio/file_io.h"
#include "audio/midi_effects.h"
#include "audio/render.h"
#include "audio/sampler.h"
#include "audio/sound_file.h"
#include "core/json.h"
#include "core/util.h"
#include "core/xml.h"
#include "effects/effects.h"
#include "importers/gzip.h"
#include "model/project_io.h"
#include "model/routing.h"
#include "model/timebase.h"
#include "plugins/plugin.h"

namespace ableton {

namespace {

using xml::Element;
using Error = std::runtime_error;

// Ableton's 70 clip/track colours, by index
const char* const PALETTE[] = {
    "FF94A6", "FFA529", "CC9927", "F7F47C", "BFFB00", "1AFF2F", "25FFA8", "5CFFE8", "8BC5FF", "5480E4", "92A7FF", "D86CE4",
    "E553A0", "FFFFFF", "FF3636", "F66C03", "99724B", "FFF034", "87FF67", "3DC300", "00BFAF", "19E9FF", "10A4EE", "007DC0",
    "886CE4", "B677C6", "FF39D4", "D0D0D0", "E2675A", "FFA374", "D3AD71", "EDFFAE", "D2E498", "BAD074", "9BC48D", "D4FDE1",
    "CDF1F8", "B9C1E3", "CDBBE4", "AE98E5", "E5DCE1", "A9A9A9", "C6928B", "B78256", "99836A", "BFBA69", "A6BE00", "7DB04D",
    "88C2BA", "9BB3C4", "85A5C2", "8393CC", "A595B5", "BF9FBE", "BC7196", "7B7B7B", "AF3333", "A95131", "724F41", "DBC300",
    "85961F", "539F31", "0A9C8E", "236384", "1A2F96", "2F52A2", "624BAD", "A34BAD", "CC2E6E", "3C3C3C"};
constexpr int PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

// --- reading values (ableton.py's val / manual) ---

std::optional<std::string> raw(const Element* e, const std::string& path) {
    if (!e) return std::nullopt;
    const Element* found = e->find(path);
    if (!found) return std::nullopt;
    const std::string_view* v = found->attr("Value");
    if (!v) return std::nullopt;
    return std::string(*v);
}

double toDouble(const std::string& s, const std::string& what) {
    const char* b = s.c_str();
    char* end = nullptr;
    double v = std::strtod(b, &end);
    if (end == b) throw Error("could not convert string to float: '" + s + "' (" + what + ")");
    return v;
}

int toInt(const std::string& s, const std::string& what) {
    const char* b = s.c_str();
    char* end = nullptr;
    long v = std::strtol(b, &end, 10);
    if (end == b) throw Error("invalid literal for int(): '" + s + "' (" + what + ")");
    if (*end == '.' || *end == 'e' || *end == 'E') return (int)toDouble(s, what);
    return (int)v;
}

double valD(const Element* e, const std::string& path, double def) {
    auto v = raw(e, path);
    return v ? toDouble(*v, path) : def;
}
int valI(const Element* e, const std::string& path, int def) {
    auto v = raw(e, path);
    return v ? toInt(*v, path) : def;
}
bool valB(const Element* e, const std::string& path, bool def) {
    auto v = raw(e, path);
    return v ? *v == "true" : def;
}
std::string valS(const Element* e, const std::string& path, const std::string& def = "") {
    auto v = raw(e, path);
    return v ? *v : def;
}
std::optional<int> valOptI(const Element* e, const std::string& path) {
    auto v = raw(e, path);
    if (!v) return std::nullopt;
    return toInt(*v, path);
}
// a value the set must have
double needD(const Element* e, const std::string& path) {
    auto v = raw(e, path);
    if (!v) throw Error("the set has no " + path + (e ? " in a " + std::string(e->tag) : std::string()));
    return toDouble(*v, path);
}

double manualD(const Element* e, const std::string& path, double def) { return valD(e, path + "/Manual", def); }
int manualI(const Element* e, const std::string& path, int def) { return valI(e, path + "/Manual", def); }
bool manualB(const Element* e, const std::string& path, bool def) { return valB(e, path + "/Manual", def); }

double db(double gain) { return 20 * std::log10(std::max(gain, 1e-9)); }
double clamp(double x, double low, double high) { return std::min(high, std::max(low, x)); }
double round1(double x) { return std::nearbyint(x * 10) / 10; }   // (Python's round(x, 1))
double round2(double x) { return std::nearbyint(x * 100) / 100; }
int64_t pyRound(double x) { return (int64_t)std::nearbyint(x); }  // (Python's round(): halves to even)

std::string colorOf(const Element* e) {
    auto v = valOptI(e, "Color");
    return v ? color(*v) : std::string();
}

bool isOn(const Element* d) { return manualB(d, "On", true); }

std::string fmt0(double x) { return util::format("%.0f", x); }
std::string fmtSigned1(double x) { return util::format("%+.1f", x); }
std::string fmtG(double x) { return util::format("%g", x); }

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? sep : "") + parts[i];
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool startsWith(std::string_view s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// notes as ableton.py's tuples (start s, end s, pitch, velocity), sorted as tuples are
bool noteLess(const Note& a, const Note& b) {
    return std::tie(a.start, a.end, a.pitch, a.velocity) < std::tie(b.start, b.end, b.pitch, b.velocity);
}
void sortNotes(std::vector<Note>& notes) { std::sort(notes.begin(), notes.end(), noteLess); }

// --- stereo audio (frames × 2 interleaved, at SR) ---

struct Stereo {
    std::vector<float> s;
    int64_t frames() const { return (int64_t)(s.size() / 2); }
    explicit Stereo(int64_t n = 0) : s((size_t)std::max<int64_t>(n, 0) * 2, 0.0f) {}
};

// out[at:at + n] += sound (at rounded; what falls outside is left out)
void place(Stereo& out, const float* sound, int64_t frames, double atF) {
    int64_t at = pyRound(atF);
    int64_t len = out.frames();
    if (at >= len || at + frames <= 0) return;
    if (at < 0) {
        sound += -at * 2;
        frames += at;
        at = 0;
    }
    int64_t n = std::min(frames, len - at);
    float* o = out.s.data() + at * 2;
    for (int64_t i = 0; i < n * 2; i++) o[i] += sound[i];
}

// a second-order section (b, a normalised) over each channel of `audio` (scipy's lfilter, from rest)
void biquad(std::vector<float>& audio, const double b[3], const double a[3]) {
    double b0 = b[0] / a[0], b1 = b[1] / a[0], b2 = b[2] / a[0], a1 = a[1] / a[0], a2 = a[2] / a[0];
    for (int ch = 0; ch < 2; ch++) {
        double z0 = 0, z1 = 0;
        for (size_t i = ch; i < audio.size(); i += 2) {
            double x = audio[i];
            double y = b0 * x + z0;
            z0 = b1 * x - a1 * y + z1;
            z1 = b2 * x - a2 * y;
            audio[i] = (float)y;
        }
    }
}

// A 12 dB/octave low-pass (RBJ biquad) over the whole of `audio`
void lowpass(Stereo& audio, double freq, double resonance) {
    if (freq >= SR * 0.45) return;
    double w = 2 * M_PI * freq / SR;
    double q = 0.707 + resonance * 5;
    double alpha = std::sin(w) / (2 * q);
    double cw = std::cos(w);
    double b[3] = {(1 - cw) / 2, 1 - cw, (1 - cw) / 2};
    double a[3] = {1 + alpha, -2 * cw, 1 - alpha};
    biquad(audio.s, b, a);
}

// RMS over the parts that sound (50 ms blocks above -50 dBFS); `audio` interleaved stereo
double activeRms(const float* audio, int64_t frames) {
    int64_t block = (int64_t)(0.05 * SR);
    int64_t n = frames / block;
    if (n == 0) return 0.0;
    double floor = std::pow(10.0, -50.0 / 20);
    double sum = 0;
    int64_t loud = 0;
    for (int64_t k = 0; k < n; k++) {
        double sq = 0;
        for (int64_t i = k * block; i < (k + 1) * block; i++) {
            double m = ((double)audio[i * 2] + audio[i * 2 + 1]) / 2;
            sq += m * m;
        }
        double rms = std::sqrt(sq / block);
        if (rms > floor) {
            sum += rms * rms;
            loud++;
        }
    }
    return loud ? std::sqrt(sum / loud) : 0.0;
}

// Live's time signature value, (numerator - 1) + 99 · log2(denominator); one out of range (or not a number): 4/4
TimeSig liveSig(const std::string& value) {
    char* end = nullptr;
    long v = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || v < 0 || v / 99 > 5) {
        util::debugf("ableton: a time signature value '%s' out of range: read as 4/4\n", value.c_str());
        return TimeSig{4, 4};
    }
    return TimeSig{timing::clampedBeats((int)(v % 99) + 1), 1 << (int)(v / 99)};
}

// --- reading the set ---

class SetReader {
public:
    std::string folder;
    xml::Document doc;
    const Element* root = nullptr;
    const Element* live = nullptr;
    double bpm = 120;
    int beatsPerBar = 4, denominator = 4;
    double spb = 0.5;  // seconds per beat
    std::map<std::string, std::vector<std::string>> samples;  // file name -> paths, for every audio file in the set's folder
    std::vector<std::pair<double, std::string>> locators;
    std::vector<std::pair<double, double>> tempoEvents;  // the tempo's automation: (beat, BPM), by time
    std::vector<std::pair<double, TimeSig>> sigChanges;  // the time signature's automation after bar 1: (beat, it), by time

    explicit SetReader(const std::string& alsPath) {
        folder = util::dirName(util::absolutePath(alsPath));
        std::string text, error;
        if (!gzip::readFile(alsPath, text, error)) throw Error(error);
        if (!doc.parse(std::move(text), error)) throw Error("the set's XML is damaged: " + error);
        root = doc.root();
        live = root->find("LiveSet");
        if (!live) throw Error("not an Ableton Live set (it has no LiveSet)");
        const Element* main = live->find("MainTrack");
        if (!main) main = live->find("MasterTrack");  // (before Live 12)
        if (!main) throw Error("the set has no main track");
        bpm = needD(main, "DeviceChain/Mixer/Tempo/Manual");
        if (!(bpm > 0)) throw Error("the set's tempo isn't valid");
        // the tempo's automation: an envelope on the main track's Tempo parameter (its AutomationTarget's Id), its
        // FloatEvents (Time in beats, Value in BPM; Live goes linearly between them, and two at one time jump)
        const Element* target = main->find("DeviceChain/Mixer/Tempo/AutomationTarget");
        if (target && target->has("Id")) {
            std::string id(target->get("Id"));
            for (const Element* env : main->findAll("AutomationEnvelopes/Envelopes/AutomationEnvelope")) {
                if (valS(env, "EnvelopeTarget/PointeeId") != id) continue;
                for (const Element* ev : env->findAll("Automation/Events/FloatEvent"))
                    if (ev->has("Time") && ev->has("Value"))
                        tempoEvents.push_back({toDouble(std::string(ev->get("Time")), "FloatEvent/Time"),
                                               toDouble(std::string(ev->get("Value")), "FloatEvent/Value")});
            }
            std::stable_sort(tempoEvents.begin(), tempoEvents.end(),
                             [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
        }
        // the time signature's automation: an envelope on the main track's TimeSignature parameter, its EnumEvents
        // (Time in beats; before Live 10 they sit in the parameter's own ArrangerAutomation); else any EnumEvent
        std::vector<const Element*> sigEvents;
        const Element* sigTarget = main->find("DeviceChain/Mixer/TimeSignature/AutomationTarget");
        if (sigTarget && sigTarget->has("Id")) {
            std::string id(sigTarget->get("Id"));
            for (const Element* env : main->findAll("AutomationEnvelopes/Envelopes/AutomationEnvelope"))
                if (valS(env, "EnvelopeTarget/PointeeId") == id)
                    for (const Element* ev : env->findAll("Automation/Events/EnumEvent")) sigEvents.push_back(ev);
        }
        if (sigEvents.empty()) sigEvents = main->findAll("DeviceChain/Mixer/TimeSignature/ArrangerAutomation/Events/EnumEvent");
        if (sigEvents.empty())
            main->walk([&](const Element& e) {
                if (e.tag != "EnumEvent") return true;
                sigEvents.push_back(&e);
                return false;
            });
        TimeSig first;  // (4/4 without any)
        for (const Element* ev : sigEvents) {
            double at = ev->has("Time") ? std::strtod(std::string(ev->get("Time")).c_str(), nullptr) : 0;
            TimeSig sig = liveSig(std::string(ev->get("Value")));
            if (!std::isfinite(at) || at <= 0) first = sig;  // (Live's first event lies far before the song: bar 1's)
            else sigChanges.push_back({at, sig});
        }
        std::stable_sort(sigChanges.begin(), sigChanges.end(),
                         [](const std::pair<double, TimeSig>& a, const std::pair<double, TimeSig>& b) { return a.first < b.first; });
        if (!sigEvents.empty() && sigChanges.size() == sigEvents.size()) first = sigChanges[0].second;  // (none before: the first's)
        beatsPerBar = first.num;
        denominator = first.den;
        spb = 60.0 / bpm;
        util::walkFiles(folder, [&](const std::string& path) {
            std::string ext = util::extension(path);
            if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".mp3" || ext == ".ogg")
                samples[util::baseName(path)].push_back(path);
            return true;
        });
        for (const Element* l : live->findAll("Locators/Locators/Locator")) {
            const Element* time = l->find("Time");
            const Element* name = l->find("Name");
            if (!time || !time->attr("Value")) throw Error("a locator without its time");
            locators.push_back({toDouble(std::string(time->get("Value")), "Locator/Time"),
                                name ? std::string(name->get("Value")) : std::string()});
        }
    }

    // Where a SampleRef's file is in the set's folder (Ableton keeps the path it had on its computer): its
    // path relative to the set if that is there, else the file of its name whose folders match its path's
    // the furthest up (two samples may share a name: Samples/Imported/Kick.wav and Samples/Recorded/Kick.wav).
    std::string samplePath(const Element* sampleRef) const {
        if (!sampleRef) throw Error("a sample without its SampleRef");
        std::vector<std::string> parts = util::split(util::replaceAll(valS(sampleRef, "FileRef/Path"), "\\", "/"), '/');
        if (parts.empty()) parts.push_back("");
        std::string relative = valS(sampleRef, "FileRef/RelativePath");
        std::string candidate = util::joinPath(folder, relative);
        if (!relative.empty() && util::fileExists(candidate) && !util::isDir(candidate)) return candidate;
        auto found = samples.find(parts.back());
        if (found == samples.end() || found->second.empty())
            throw Error("sample not found in the set's folder: " + parts.back());
        // how many of its last folders (and its name) are the same as in the set's path
        auto matching = [&](const std::string& path) {
            std::string rel = path.substr(std::min(path.size(), folder.size() + (folder == "/" ? 0 : 1)));
            std::vector<std::string> mine = util::split(rel, '/');
            size_t n = std::min(mine.size(), parts.size());
            for (size_t k = 0; k < n; k++)
                if (mine[mine.size() - 1 - k] != parts[parts.size() - 1 - k]) return k;
            return n;
        };
        const std::string* best = nullptr;
        size_t bestScore = 0;
        for (auto& p : found->second) {
            size_t score = matching(p);
            if (!best || score > bestScore) {
                best = &p;
                bestScore = score;
            }
        }
        return *best;
    }

    std::vector<const Element*> ofKind(std::initializer_list<const char*> tags) const {
        std::vector<const Element*> out;
        const Element* tracks = live->find("Tracks");
        if (!tracks) return out;
        for (const Element* t : tracks->children)
            for (const char* tag : tags)
                if (t->tag == tag) out.push_back(t);
        return out;
    }
    std::vector<const Element*> tracks() const { return ofKind({"MidiTrack", "AudioTrack"}); }
    std::vector<const Element*> returns() const { return ofKind({"ReturnTrack"}); }
    // [(its Id, GroupTrack)] of the set's group tracks
    std::vector<std::pair<std::string, const Element*>> groups() const {
        std::vector<std::pair<std::string, const Element*>> out;
        for (const Element* t : ofKind({"GroupTrack"})) {
            std::string id(t->get("Id"));
            bool seen = false;
            for (auto& g : out)
                if (g.first == id) {
                    g.second = t;  // (a dict: the last of an Id wins, in the first's place)
                    seen = true;
                }
            if (!seen) out.push_back({id, t});
        }
        return out;
    }

    struct MidiClipNotes {
        const Element* clip;
        std::vector<Note> notes, offNotes;  // (start s, end s, pitch, velocity) on the timeline
    };
    // A MIDI track's arrangement clips: the notes that play in each clip's window, on the timeline, cut at the
    // clip's end; the switched-off ones the same way.
    std::vector<MidiClipNotes> midiNotes(const Element* track) const {
        std::vector<MidiClipNotes> result;
        for (const Element* clip : track->findAll(".//MainSequencer/ClipTimeable/ArrangerAutomation/Events/MidiClip")) {
            double start = needD(clip, "CurrentStart"), end = needD(clip, "CurrentEnd");
            double loopStart = needD(clip, "Loop/LoopStart"), loopEnd = valD(clip, "Loop/LoopEnd", loopStart);
            double first = loopStart + valD(clip, "Loop/StartRelative", 0.0);
            bool looping = valB(clip, "Loop/LoopOn", false);
            struct Content {
                double time, duration;
                int pitch;
                double velocity;
                bool off;
            };
            std::vector<Content> content;
            for (const Element* keyTrack : clip->findAll("Notes/KeyTracks/KeyTrack")) {
                auto key = raw(keyTrack, "MidiKey");
                if (!key) throw Error("a KeyTrack without its MidiKey");
                int pitch = toInt(*key, "MidiKey");
                for (const Element* n : keyTrack->findAll("Notes/MidiNoteEvent")) {
                    auto attr = [&](const char* name) {
                        const std::string_view* v = n->attr(name);
                        if (!v) throw Error(std::string("a note without its ") + name);
                        return toDouble(std::string(*v), name);
                    };
                    content.push_back({attr("Time"), attr("Duration"), pitch, attr("Velocity"), n->get("IsEnabled") == "false"});
                }
            }
            MidiClipNotes out{clip, {}, {}};
            // the clip plays its content from `first`; a looping clip repeats [loopStart, loopEnd)
            std::vector<std::pair<double, double>> passes = {{first, start}};
            if (looping) {
                double length = loopEnd - loopStart;
                double t = first, at = start;
                passes.clear();
                while (at < end) {
                    double windowEnd = t < loopEnd ? loopEnd : t + length;
                    if (windowEnd - t <= 0) break;  // (a loop of no length)
                    passes.push_back({t, at});
                    at += windowEnd - t;
                    t = loopStart;
                }
            }
            for (auto& pass : passes) {
                double from = pass.first, timelineAt = pass.second;
                double window = (looping ? loopEnd : from + (end - start)) - from;
                for (auto& c : content) {
                    if (from - 1e-6 <= c.time && c.time < from + window - 1e-6) {
                        double on = timelineAt + c.time - from;
                        if (on >= end - 1e-6) continue;
                        double off = std::min(on + c.duration, end);
                        (c.off ? out.offNotes : out.notes).push_back({on * spb, off * spb, (double)c.pitch, c.velocity});
                    }
                }
            }
            sortNotes(out.notes);
            sortNotes(out.offNotes);
            result.push_back(std::move(out));
        }
        return result;
    }

    struct AudioClipPlace {
        const Element* clip;
        std::string file;
        double start, offset, length, stretch;  // s on the timeline; s into the file (as recorded); s as it plays
        bool hasLoop = false;
        double loopStart = 0, loopLength = 0;  // in the file's seconds as recorded
        double pitch = 0;                      // semitones
        double fadeIn = 0, fadeOut = 0;        // s as it plays
        bool disabled = false;
    };
    // An audio track's arrangement clips; stretch: how many times as long as recorded it plays (warped to the
    // set's tempo)
    std::vector<AudioClipPlace> audioClips(const Element* track) const {
        std::vector<AudioClipPlace> result;
        for (const Element* clip : track->findAll(".//MainSequencer/Sample/ArrangerAutomation/Events/AudioClip")) {
            double start = needD(clip, "CurrentStart"), end = needD(clip, "CurrentEnd");
            double lengthBeats = end - start;
            if (lengthBeats * spb < 0.001) continue;  // (a leftover of no length)
            double first = needD(clip, "Loop/LoopStart") + valD(clip, "Loop/StartRelative", 0.0);
            double loopStart = valD(clip, "Loop/LoopStart", 0.0), loopEnd = valD(clip, "Loop/LoopEnd", 0.0);
            double stretch = 1.0;
            bool warped = valB(clip, "IsWarped", false);
            std::vector<std::pair<double, double>> markers;
            if (warped) {
                for (const Element* m : clip->findAll("WarpMarkers/WarpMarker"))
                    markers.push_back({toDouble(std::string(m->get("BeatTime")), "BeatTime"),
                                       toDouble(std::string(m->get("SecTime")), "SecTime")});
                std::sort(markers.begin(), markers.end());
                if (markers.size() < 2 || markers.back().first == markers.front().first)
                    throw Error("a warped clip without two warp markers");
                double rate = (markers.back().second - markers.front().second) /
                              (markers.back().first - markers.front().first);  // seconds of the file per beat
                if (rate > 0 && std::fabs(rate - spb) > spb * 0.002)
                    stretch = spb / rate;  // (warped to the set's tempo: stretched to it, at its pitch)
            }
            // (unwarped: the loop's and window's times are seconds)
            auto fileAt = [&](double v) { return warped ? warpSeconds(markers, v) : v; };
            AudioClipPlace c;
            c.clip = clip;
            double offset = fileAt(first);
            if (valB(clip, "Loop/LoopOn", false) && loopEnd > loopStart) {
                double a = std::max(0.0, fileAt(loopStart)), b = fileAt(loopEnd);
                if (b - a > 0.001) {
                    c.hasLoop = true;
                    c.loopStart = a;
                    c.loopLength = b - a;
                }
            }
            // Live's clip transpose: warped, the pitch alone; unwarped, like a tape (faster and higher:
            // stretched shorter too). Fades are in beats in a warped clip, else in seconds.
            double pitch = valD(clip, "PitchCoarse", 0.0) + valD(clip, "PitchFine", 0.0) / 100;
            if (pitch != 0 && !warped) stretch = std::pow(2.0, -pitch / 12);
            double unit = warped ? spb : 1.0;
            c.fadeIn = valD(clip, "Fades/FadeInLength", 0.0) * unit;
            c.fadeOut = valD(clip, "Fades/FadeOutLength", 0.0) * unit;
            double length = lengthBeats * spb;
            start *= spb;
            if (offset < 0) {  // (it starts before the file does, silent until then: start it where the sound does)
                start -= offset * stretch;
                length += offset * stretch;
                offset = 0.0;
            }
            if (length <= 0.001) continue;
            c.pitch = pitch;
            c.disabled = valB(clip, "Disabled", false);
            c.file = samplePath(clip->find("SampleRef"));
            c.start = start;
            c.offset = offset;
            c.length = length;
            c.stretch = stretch;
            result.push_back(c);
        }
        return result;
    }
};

// --- the conversion's state ---

struct Loaded {
    AudioPtr audio;  // stereo at its own rate
    int rate = 0;
};

struct Context {
    SetReader* reader = nullptr;
    double bpm = 120;  // (ableton.py's BPM)
    std::map<std::string, Loaded> files;  // (ableton.py's _files: each sample file read once)
    const Loaded& load(const std::string& path) {
        auto it = files.find(path);
        if (it != files.end()) return it->second;
        Loaded l;
        std::string error;
        if (!file_io::loadAudio(path, l.audio, l.rate, error)) throw Error(error);
        return files[path] = l;
    }
};

// --- Simpler / Sampler / Drum Rack ---

struct Instrument {
    virtual ~Instrument() = default;
    // notes (start s, end s, pitch, velocity) played into new audio of `frames`
    virtual Stereo render(const std::vector<Note>& notes, int64_t frames) = 0;
};
using InstrumentPtr = std::unique_ptr<Instrument>;

struct Zone {
    std::string path;
    int keyLo, keyHi, velLo, velHi, root;
    double detune, gain;  // cents; a factor
    int64_t start, end;   // frames of the file (end 0: to its end)
    int loopMode;         // 0 off, 1 forward, 2 back and forth
    int64_t loopA, loopB;
    bool active;
    Zone(const Element* part, const SetReader& reader) {
        path = reader.samplePath(part->find("SampleRef"));
        keyLo = valI(part, "KeyRange/Min", 0);
        keyHi = valI(part, "KeyRange/Max", 127);
        velLo = valI(part, "VelocityRange/Min", 1);
        velHi = valI(part, "VelocityRange/Max", 127);
        root = valI(part, "RootKey", 60);
        detune = valD(part, "Detune", 0);
        gain = valD(part, "Volume", 1.0);
        start = valI(part, "SampleStart", 0);
        end = valOptI(part, "SampleEnd").value_or(0);
        loopMode = valI(part, "SustainLoop/Mode", 0);
        loopA = valI(part, "SustainLoop/Start", 0);
        loopB = valI(part, "SustainLoop/End", 0);
        active = valB(part, "IsActive", true);
    }
};

// Simpler or Sampler: its zones (round-robin when they overlap and it is on), volume envelope, velocity
// sensitivity, transpose, filter and voice count.
struct Sampler : Instrument {
    Context& ctx;
    std::vector<Zone> zones;
    bool roundRobin;
    double gainDb, velScale;
    double attack, decay, sustain, release;
    double transpose;
    bool hasFilter = false;
    double filterFreq = 22000, filterRes = 0;
    int voices;
    midi_effects::NumpyRandom rng;
    int last = -1;
    std::map<std::tuple<int, double, int64_t, int64_t>, std::shared_ptr<const std::vector<float>>> cache;

    Sampler(const Element* device, Context& c, uint64_t seed = 1) : ctx(c), rng(seed) {
        for (const Element* p : device->findAll("Player/MultiSampleMap/SampleParts/MultiSamplePart")) {
            Zone z(p, *ctx.reader);
            if (z.active) zones.push_back(z);
        }
        roundRobin = valB(device, "Player/MultiSampleMap/RoundRobin", false);
        gainDb = manualD(device, "VolumeAndPan/Volume", 0.0);
        velScale = manualD(device, "VolumeAndPan/VolumeVelScale", 0.0);
        std::string env = "VolumeAndPan/Envelope/";
        attack = manualD(device, env + "AttackTime", 0.1) / 1000;
        decay = manualD(device, env + "DecayTime", 600) / 1000;
        sustain = manualD(device, env + "SustainLevel", 1.0);
        release = manualD(device, env + "ReleaseTime", 50) / 1000;
        transpose = manualD(device, "Pitch/TransposeKey", 0.0) + manualD(device, "Pitch/TransposeFine", 0.0) / 100;
        if (manualB(device, "Filter/IsOn", false)) {
            std::string f = "Filter/Slot/Value/SimplerFilter/";
            if (manualI(device, f + "Type", 0) == 0) {  // (low-pass; the others are left out)
                hasFilter = true;
                filterFreq = manualD(device, f + "Freq", 22000.0);
                filterRes = manualD(device, f + "Res", 0.0);
            }
        }
        bool simpler = device->tag == "OriginalSimpler";
        voices = std::max(1, simpler ? valI(device, "Globals/NumVoices", 5) : 32);
        if (simpler) {  // (Simpler's voice count is an index into 1, 2, 3, 4, 5, 6, 8, 12...)
            static const std::map<int, int> counts = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6},
                                                      {6, 8}, {7, 12}, {8, 16}, {9, 24}, {10, 32}};
            auto it = counts.find(voices);
            voices = it == counts.end() ? 5 : it->second;
        }
    }

    std::vector<int> choose(double pitch, double velocity) {
        std::vector<int> fits;
        for (size_t i = 0; i < zones.size(); i++) {
            const Zone& z = zones[i];
            if (z.keyLo <= pitch && pitch <= z.keyHi && z.velLo <= velocity && velocity <= z.velHi) fits.push_back((int)i);
        }
        if (fits.empty()) return {};
        if (roundRobin && fits.size() > 1) {
            std::vector<int> options;
            for (int z : fits)
                if (z != last) options.push_back(z);
            if (options.empty()) options = fits;
            last = options[(size_t)rng.integers(0, (int64_t)options.size())];
            return {last};
        }
        return fits;
    }

    std::shared_ptr<const std::vector<float>> voice(int zoneIndex, double pitch, double velocity, double gateS) {
        auto key = std::make_tuple(zoneIndex, pitch, pyRound(velocity), pyRound(gateS * 1000));
        auto hit = cache.find(key);
        if (hit != cache.end()) return hit->second;
        const Zone& zone = zones[zoneIndex];
        const Loaded& file = ctx.load(zone.path);
        const AudioBuffer& data = *file.audio;
        int64_t len = data.frames();
        int64_t end = zone.end ? zone.end : len;
        double ratio = std::pow(2.0, (pitch - zone.root + transpose + zone.detune / 100) / 12) * file.rate / SR;
        bool looping = zone.loopMode && zone.loopB > zone.loopA;
        double longest = gateS + release + 0.01;
        int64_t n = looping ? (int64_t)(longest * SR)
                            : std::min((int64_t)(longest * SR), (int64_t)((double)(end - zone.start) / ratio));
        n = std::max<int64_t>(n, 0);
        std::vector<double> pos;
        pos.reserve((size_t)n);
        double a = (double)zone.loopA, b = (double)zone.loopB;
        for (int64_t i = 0; i < n; i++) {
            double p = zone.start + i * ratio;
            if (looping && p >= b) {
                if (zone.loopMode == 2) {  // back and forth
                    double span = b - a;
                    double phase = std::fmod(p - b, 2 * span);
                    p = phase < span ? b - phase : a + (phase - span);
                } else {
                    p = a + std::fmod(p - a, b - a);
                }
            }
            if (p < len - 1) pos.push_back(p);
        }
        n = (int64_t)pos.size();
        std::vector<float> env = envelope(n, (int64_t)(gateS * SR), attack, decay, sustain, release);
        double gain = std::pow(10.0, gainDb / 20) * zone.gain * (1 - velScale + velScale * velocity / 127);
        int64_t lastSounding = -1;
        for (int64_t i = 0; i < n; i++)
            if (env[i] != 0) lastSounding = i;
        auto sound = std::make_shared<std::vector<float>>((size_t)(lastSounding + 1) * 2);
        for (int64_t i = 0; i <= lastSounding; i++) {
            int64_t k = (int64_t)pos[i];
            float frac = (float)(pos[i] - k);
            float amp = (float)(env[i] * gain);
            const float* s0 = data.frame(k);
            const float* s1 = data.frame(k + 1);
            for (int ch = 0; ch < 2; ch++) (*sound)[i * 2 + ch] = (s0[ch] * (1 - frac) + s1[ch] * frac) * amp;
        }
        cache[key] = sound;
        return sound;
    }

    // A voice is cut (with a short fade) when a new note needs it and all of them are busy, as in Ableton.
    Stereo render(const std::vector<Note>& notesIn, int64_t frames) override {
        Stereo out(frames);
        struct Playing {
            int64_t start, end;
            size_t slot;
        };
        struct Voice {
            int64_t at;
            std::shared_ptr<const std::vector<float>> sound;
            int64_t cut;  // -1: not cut
        };
        std::vector<Playing> playing;
        std::vector<Voice> voiced;
        std::vector<Note> notes = notesIn;
        sortNotes(notes);
        for (auto& note : notes) {
            int64_t at = (int64_t)(note.start * SR);
            std::vector<Playing> still;
            for (auto& v : playing)
                if (v.end > at) still.push_back(v);
            playing.swap(still);
            for (int zone : choose(note.pitch, note.velocity)) {
                if ((int)playing.size() >= voices) {
                    size_t oldest = 0;
                    for (size_t i = 1; i < playing.size(); i++)
                        if (playing[i].start < playing[oldest].start) oldest = i;
                    voiced[playing[oldest].slot].cut = at;  // cut there
                    playing.erase(playing.begin() + oldest);
                }
                auto sound = voice(zone, note.pitch, note.velocity, std::max(note.end - note.start, 0.001));
                voiced.push_back({at, sound, -1});
                playing.push_back({at, at + (int64_t)(sound->size() / 2), voiced.size() - 1});
            }
        }
        int64_t fade = (int64_t)(0.002 * SR);
        for (auto& v : voiced) {
            int64_t frames = (int64_t)(v.sound->size() / 2);
            if (v.cut >= 0 && v.cut - v.at < frames) {
                int64_t keep = std::min(frames, std::max<int64_t>(v.cut - v.at, 0) + fade);
                std::vector<float> s(v.sound->begin(), v.sound->begin() + keep * 2);
                int64_t tail = keep >= fade ? fade : keep;
                for (int64_t i = 0; i < tail; i++) {
                    float f = tail > 1 ? 1.0f - (float)i / (float)(tail - 1) : 0.0f;  // (np.linspace(1, 0, tail))
                    if (tail == 1) f = 1.0f;
                    s[(keep - tail + i) * 2] *= f;
                    s[(keep - tail + i) * 2 + 1] *= f;
                }
                place(out, s.data(), keep, (double)v.at);
            } else {
                place(out, v.sound->data(), frames, (double)v.at);
            }
        }
        if (hasFilter) lowpass(out, filterFreq, filterRes);
        return out;
    }
};

// A Drum Rack: each pad's note (128 - its ReceivingNote, as Live stores it) plays its own Simpler.
struct DrumRack : Instrument {
    std::map<int, std::vector<std::unique_ptr<Sampler>>> pads;
    std::vector<int> padOrder;  // (the order pads were first seen)
    DrumRack(const Element* device, Context& ctx) {
        auto branches = device->findAll("Branches/DrumBranch");
        for (size_t i = 0; i < branches.size(); i++) {
            const Element* branch = branches[i];
            if (raw(branch, "DeviceChain/Mixer/Speaker/Manual") && !valB(branch, "DeviceChain/Mixer/Speaker/Manual", true))
                continue;
            int note = 128 - valI(branch, "BranchInfo/ReceivingNote", 0);
            for (const char* tag : {"OriginalSimpler", "MultiSampler"})
                for (const Element* s : branch->iter(tag)) {
                    if (!pads.count(note)) padOrder.push_back(note);
                    pads[note].push_back(std::make_unique<Sampler>(s, ctx, i + 1));
                }
        }
    }
    Stereo render(const std::vector<Note>& notes, int64_t frames) override {
        Stereo out(frames);
        std::vector<double> order;
        std::map<double, std::vector<Note>> byPad;
        for (auto& n : notes) {
            if (!byPad.count(n.pitch)) order.push_back(n.pitch);
            byPad[n.pitch].push_back({n.start, n.end, 60, n.velocity});  // (a pad plays its sample as is)
        }
        for (double pitch : order) {
            auto it = pads.find((int)pitch);
            if (it == pads.end() || (double)(int)pitch != pitch) continue;
            for (auto& sampler : it->second) {
                Stereo one = sampler->render(byPad[pitch], frames);
                for (size_t i = 0; i < out.s.size(); i++) out.s[i] += one.s[i];
            }
        }
        return out;
    }
};

// --- MIDI effects ---

const double ARP_RATES[] = {4, 2, 1, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 1.0 / 16, 1.0 / 24, 1.0 / 32};  // (beats, roughly Live's list)
constexpr int ARP_RATE_COUNT = 12;

// Live's Arpeggiator (styles Up and Down; other styles are played as Up): while notes are held, one of them
// at a time, every `rate`, each `gate` % of a step long.
std::vector<Note> arpeggiate(const std::vector<Note>& notes, const Element* device, double bpm) {
    int style = manualI(device, "Mode", 0);
    double step;
    if (manualB(device, "SyncState", false))
        step = ARP_RATES[std::max(0, std::min(manualI(device, "SyncedRate", 8), ARP_RATE_COUNT - 1))] * 60 / bpm;
    else
        step = manualD(device, "FreeRate", 100.0) / 1000;
    step = std::max(step, 1e-4);  // (never stuck)
    double gate = manualD(device, "Gate", 100.0) / 100 * step;
    int repeats = manualI(device, "RepeatCount", 0);
    struct Ev {
        double time;
        int on;
        double pitch, velocity;
        bool operator<(const Ev& o) const { return std::tie(time, on, pitch, velocity) < std::tie(o.time, o.on, o.pitch, o.velocity); }
    };
    std::vector<Ev> events;
    for (auto& n : notes) events.push_back({n.start, 1, n.pitch, n.velocity});
    for (auto& n : notes) events.push_back({n.end, 0, n.pitch, n.velocity});
    std::sort(events.begin(), events.end());
    std::vector<Note> out;
    std::map<double, double> held;  // pitch -> velocity
    size_t i = 0;
    std::optional<double> t;
    int64_t index = 0, count = 0;
    double endTime = 0;
    for (auto& n : notes) endTime = std::max(endTime, n.end);
    while (i < events.size() || (!held.empty() && t && *t < endTime)) {
        if (!t || (i < events.size() && events[i].time <= *t)) {
            const Ev& e = events[i++];
            if (e.on) {
                if (held.empty()) {
                    t = e.time;
                    index = count = 0;
                }
                held[e.pitch] = e.velocity;
            } else {
                held.erase(e.pitch);
                if (held.empty()) t.reset();
            }
            continue;
        }
        std::vector<double> order;
        for (auto& h : held) order.push_back(h.first);
        if (style == 1) std::reverse(order.begin(), order.end());
        if (!order.empty() && (repeats == 0 || count < (int64_t)repeats * (int64_t)order.size())) {
            double pitch = order[(size_t)(index % (int64_t)order.size())];
            out.push_back({*t, *t + gate, pitch, held[pitch]});
            index++;
            count++;
        }
        *t += step;
    }
    return out;
}

// Live's Note Length (time mode): every note becomes `length` long.
std::vector<Note> noteLength(const std::vector<Note>& notes, const Element* device, double bpm) {
    double length = manualB(device, "Mode", false) ? manualI(device, "SyncedLength", 4) * 0.25 * 60 / bpm
                                                   : manualD(device, "TimeLength", 100.0) / 1000;
    length *= manualD(device, "Gate", 100.0) / 100;
    std::vector<Note> out;
    for (auto& n : notes) out.push_back({n.start, n.start + length, n.pitch, n.velocity});
    return out;
}

// --- synthesizer stand-ins ---

enum class Shape { Sine, Triangle, Square, Saturated, Saw };

void oscillator(Shape shape, double freq, int64_t n, double phase, double gain, std::vector<double>& sig) {
    double t = phase, inc = freq / SR;
    const double tanh3 = std::tanh(3.0);
    for (int64_t i = 0; i < n; i++) {
        t += inc;
        double x = t - std::floor(t);
        double v;
        switch (shape) {
            case Shape::Sine: v = std::sin(2 * M_PI * x); break;
            case Shape::Triangle: v = 4 * std::fabs(x - 0.5) - 1; break;
            case Shape::Square: v = x < 0.5 ? 1.0 : -1.0; break;
            case Shape::Saturated: v = std::tanh(3 * std::sin(2 * M_PI * x)) / tanh3; break;
            default: v = 2 * x - 1;
        }
        sig[i] += gain * v;
    }
}

// A 12 dB low-pass whose cutoff follows `cutoffs` (per frame), changed every `block` frames
void movingLowpass(std::vector<double>& signal, const std::vector<double>& cutoffs, double resonance, int block = 256) {
    double z0 = 0, z1 = 0;
    for (size_t s = 0; s < signal.size(); s += block) {
        double f = clamp(cutoffs[s], 20, SR * 0.45);
        double w = 2 * M_PI * f / SR;
        double alpha = std::sin(w) / (2 * (0.707 + resonance * 4));
        double cw = std::cos(w);
        double b0 = (1 - cw) / 2 / (1 + alpha), b1 = (1 - cw) / (1 + alpha), b2 = b0;
        double a1 = -2 * cw / (1 + alpha), a2 = (1 - alpha) / (1 + alpha);
        size_t stop = std::min(signal.size(), s + block);
        for (size_t i = s; i < stop; i++) {
            double x = signal[i];
            double y = b0 * x + z0;
            z0 = b1 * x - a1 * y + z1;
            z1 = b2 * x - a2 * y;
            signal[i] = y;
        }
    }
}

struct Osc {
    Shape shape;
    double octave, semis, gain;
};
struct Adsr {
    double a, d, s, r;
};

// A simple polyphonic synth: up to two oscillators and noise, a low-pass whose cutoff an envelope can open,
// an amp ADSR, velocity, a little stereo detune. Settings come from the Ableton synth it stands in for.
struct Synth : Instrument {
    std::vector<Osc> oscs;
    double noise = 0.0, cutoff = 20000.0, resonance = 0.0, filterEnv = 0.0;
    Adsr filterAdsr{0.001, 0.2, 0, 0.2}, ampAdsr{0.002, 0.3, 0.8, 0.3};
    double velocity = 0.5, transpose = 0, spread = 6;
    bool mono = false;
    double drive = 0.0;
    midi_effects::NumpyRandom rng{7};

    Stereo note(double pitch, double vel, double gateS) {
        int64_t n = (int64_t)((gateS + ampAdsr.r) * SR) + 1;
        int64_t gate = (int64_t)(gateS * SR);
        std::vector<float> amp = envelope(n, gate, ampAdsr.a, ampAdsr.d, ampAdsr.s, ampAdsr.r);
        std::vector<float> fenv = envelope(n, gate, filterAdsr.a, filterAdsr.d, std::max(filterAdsr.s, 1e-4), filterAdsr.r);
        double freq = 440 * std::pow(2.0, (pitch + transpose - 69) / 12);
        std::vector<double> cutoffs((size_t)n);
        for (int64_t i = 0; i < n; i++) cutoffs[i] = cutoff * std::pow(2.0, filterEnv * 7 * fenv[i]);  // (up to 7 octaves wide open)
        double level = 1 - velocity + velocity * vel / 127;
        Stereo out(n);
        int side = 0;
        for (double cents : {-spread, spread}) {
            std::vector<double> sig((size_t)n, 0.0);
            for (auto& o : oscs) {
                double f = freq * std::pow(2.0, o.octave + o.semis / 12 + cents / 1200);
                oscillator(o.shape, f, n, rng.random(), o.gain, sig);
            }
            if (noise != 0)
                for (int64_t i = 0; i < n; i++) sig[i] += noise * (-1 + 2 * rng.random());
            if (drive != 0)
                for (int64_t i = 0; i < n; i++) sig[i] = std::tanh(sig[i] * (1 + 6 * drive));
            movingLowpass(sig, cutoffs, resonance);
            for (int64_t i = 0; i < n; i++) out.s[i * 2 + side] = (float)(sig[i] * (amp[i] * level));
            side++;
        }
        return out;
    }

    Stereo render(const std::vector<Note>& notesIn, int64_t frames) override {
        Stereo out(frames);
        std::vector<Note> notes = notesIn;
        sortNotes(notes);
        if (mono)  // one note at a time: each ends where the next starts
            for (size_t i = 0; i + 1 < notes.size(); i++) notes[i].end = std::min(notes[i].end, notes[i + 1].start);
        for (auto& n : notes) {
            Stereo one = note(n.pitch, n.velocity, std::max(n.end - n.start, 0.01));
            place(out, one.s.data(), one.frames(), n.start * SR);
        }
        return out;
    }
};

Shape driftShape(int type) {
    switch (type) {
        case 0: return Shape::Sine;
        case 1: return Shape::Triangle;
        case 3: return Shape::Saturated;
        case 5:
        case 6: return Shape::Square;
        default: return Shape::Saw;
    }
}

// Drift: oscillator 1 (shape, octave) and 2 (sine, detuned), noise, the low-pass and its envelope 2 amount,
// envelope 1 as the amp, its global transpose.
InstrumentPtr drift(const Element* device) {
    auto g = [&](const std::string& name, double def = 0.0) { return manualD(device, name, def); };
    auto on = [&](const std::string& name) { return manualB(device, name, true); };
    auto s = std::make_unique<Synth>();
    if (on("Mixer_OscillatorOn1"))
        s->oscs.push_back({driftShape((int)g("Oscillator1_Type")), (double)(int)g("Oscillator1_Transpose"), 0,
                           g("Mixer_OscillatorGain1", 0.7)});
    if (on("Mixer_OscillatorOn2"))
        s->oscs.push_back({(int)g("Oscillator2_Type") == 0 ? Shape::Sine : Shape::Saw, (double)(int)g("Oscillator2_Transpose"),
                           g("Oscillator2_Detune") * 12, g("Mixer_OscillatorGain2", 0.7)});
    s->noise = on("Mixer_NoiseOn") ? g("Mixer_NoiseLevel") * 0.3 : 0.0;
    s->cutoff = g("Filter_Frequency", 20000.0);
    s->resonance = g("Filter_Resonance");
    s->filterEnv = g("Filter_ModAmount1");
    s->filterAdsr = {g("Envelope2_Attack"), g("Envelope2_Decay"), g("Envelope2_Sustain"), g("Envelope2_Release")};
    s->ampAdsr = {g("Envelope1_Attack"), g("Envelope1_Decay"), g("Envelope1_Sustain", 1.0), g("Envelope1_Release")};
    s->velocity = g("Global_VolVelMod", 0.5);
    s->transpose = (int)g("Global_Transpose");
    s->spread = 4 + 10 * g("Global_StereoVoiceDepth");
    s->mono = on("Global_Legato");
    return s;
}

// Meld: its two engines as a sine (engine A) and a saw (engine B) at their octaves and volumes, with engine
// A's amp envelope and engine B's filter, drive and mono legato.
InstrumentPtr meld(const Element* device) {
    auto g = [&](const std::string& name, double def = 0.0) { return manualD(device, "MeldVoice_" + name, def); };
    auto s = std::make_unique<Synth>();
    for (auto& engine : {std::make_pair("EngineA", Shape::Sine), std::make_pair("EngineB", Shape::Saw)}) {
        std::string e = engine.first;
        if (manualB(device, "MeldVoice_" + e + "_On", true))
            s->oscs.push_back({engine.second, (double)(int)g(e + "_Oscillator_Pitch_TransposeOctaves"),
                               g(e + "_Oscillator_Pitch_Transpose"), std::pow(10.0, g(e + "_Volume") / 20)});
    }
    double cutoff = g("EngineB_Filter_Frequency", 2000.0);
    s->cutoff = cutoff > 100 ? cutoff : 2000.0;
    s->resonance = 0.2;
    s->filterEnv = 0.3;
    s->filterAdsr = {g("EngineB_FilterEnvelope_Times_Attack"), g("EngineB_FilterEnvelope_Times_Decay", 0.5), 0.0,
                     g("EngineB_FilterEnvelope_Times_Release", 0.3)};
    s->ampAdsr = {g("EngineA_AmpEnvelope_Times_Attack"), g("EngineA_AmpEnvelope_Times_Decay", 0.6),
                  g("EngineA_AmpEnvelope_Sustain", 1.0), g("EngineA_AmpEnvelope_Times_Release", 0.3)};
    s->drive = g("Drive");
    s->mono = manualB(device, "MonoLegato", false);
    s->spread = 10;
    return s;
}

// Operator: a sine at its global transpose, with operator A's envelope (Operator's sub basses are sines).
InstrumentPtr operatorSynth(const Element* device) {
    std::string env = "Operator.0/Envelope/";
    auto g = [&](const std::string& name, double def = 0.0) { return manualD(device, name, def); };
    auto s = std::make_unique<Synth>();
    s->oscs = {{Shape::Sine, 0, 0, 1.0}, {Shape::Sine, 1, 0, 0.15}};
    s->cutoff = 4000.0;
    s->ampAdsr = {g(env + "AttackTime", 0.1) / 1000, std::max(g("Operator.1/Envelope/DecayTime", 600), g(env + "DecayTime", 600)) / 1000,
                  std::max(g(env + "SustainLevel", 1.0), 0.3), g(env + "ReleaseTime", 300) / 1000};
    s->transpose = (int)g("Globals/Transpose");
    s->velocity = 0.3;
    s->spread = 0;
    return s;
}

// --- one Ableton MIDI track ---

using Transform = std::function<std::vector<Note>(const std::vector<Note>&)>;

const Element* firstOf(const Element* track, const char* tag) {
    const Element* found = nullptr;
    track->walk([&](const Element& e) {
        if (e.tag == tag) {
            found = &e;
            return false;
        }
        return true;
    });
    return found;
}
std::set<std::string_view> kindsOf(const Element* track) {
    std::set<std::string_view> kinds;
    track->walk([&](const Element& e) {
        kinds.insert(e.tag);
        return true;
    });
    return kinds;
}
std::vector<const Element*> samplersOf(const Element* track) {
    std::vector<const Element*> out = track->iter("MultiSampler");
    for (const Element* e : track->iter("OriginalSimpler")) out.push_back(e);
    return out;
}

// (notes transforms, instrument, description) for a MIDI track's device chain
InstrumentPtr instrumentOf(const Element* track, Context& ctx, std::vector<Transform>& transforms, std::string& about) {
    double bpm = ctx.bpm;
    for (const Element* d : track->iter()) {
        if (d->tag == "MidiArpeggiator" && manualB(d, "On", true))
            transforms.push_back([d, bpm](const std::vector<Note>& notes) { return arpeggiate(notes, d, bpm); });
        else if (d->tag == "MidiNoteLength" && manualB(d, "On", true))
            transforms.push_back([d, bpm](const std::vector<Note>& notes) { return noteLength(notes, d, bpm); });
    }
    auto kinds = kindsOf(track);
    if (kinds.count("DrumGroupDevice")) {
        about = "Drum Rack, from its samples";
        return std::make_unique<DrumRack>(firstOf(track, "DrumGroupDevice"), ctx);
    }
    auto samplers = samplersOf(track);
    if (!samplers.empty()) {
        std::string tag(samplers[0]->tag);
        about = util::replaceAll(util::replaceAll(tag, "Original", ""), "Multi", "") + ", from its samples";
        return std::make_unique<Sampler>(samplers[0], ctx);
    }
    if (kinds.count("InstrumentMeld")) {
        about = "Meld, approximated";
        return meld(firstOf(track, "InstrumentMeld"));
    }
    if (kinds.count("Operator")) {
        about = "Operator, approximated";
        return operatorSynth(firstOf(track, "Operator"));
    }
    if (kinds.count("Drift")) {
        about = "Drift, approximated";
        return drift(firstOf(track, "Drift"));
    }
    about = "unknown instrument: a plain synth";
    auto s = std::make_unique<Synth>();
    s->oscs = {{Shape::Saw, 0, 0, 0.5}};
    s->cutoff = 3000;
    return s;
}

// --- devices carried over as Exbit's own ---

// The devices of a device chain in the order they play, racks opened up: a rack's first chain (the others
// are reported: Exbit's chains here are one after the other).
std::vector<const Element*> devicesOf(const Element* chain, std::vector<std::string>& report, const std::string& where) {
    std::vector<const Element*> out;
    if (!chain) return out;
    for (const Element* d : chain->children) {
        if (endsWith(d->tag, "GroupDevice")) {
            auto branches = d->findAll("Branches/*");
            if (branches.size() > 1)
                report.push_back(where + ": a rack with " + std::to_string(branches.size()) +
                                 " parallel chains: only its first is carried over");
            if (!branches.empty()) {
                const Element* inner = branches[0]->find("DeviceChain/*/Devices");
                auto more = devicesOf(inner, report, where);
                out.insert(out.end(), more.begin(), more.end());
            }
        } else {
            out.push_back(d);
        }
    }
    return out;
}

const double GLUE_RATIOS[] = {2.0, 4.0, 10.0};
const double GLUE_ATTACKS_MS[] = {0.01, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0};
const double GLUE_RELEASES_MS[] = {100.0, 200.0, 400.0, 600.0, 800.0, 1200.0, 400.0};  // (the last: Auto)

using Values = std::vector<std::pair<std::string, double>>;  // (in order, as the Python dicts)
void setValue(Values& v, const std::string& key, double x) {
    for (auto& kv : v)
        if (kv.first == key) {
            kv.second = x;
            return;
        }
    v.push_back({key, x});
}
const double* getValue(const Values& v, const std::string& key) {
    for (auto& kv : v)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

// Eq8's bands onto the EQ: the highest low cut, a low shelf, the strongest bell, a high shelf, the lowest high
// cut (the EQ has one of each; further bells are reported, as are frequencies outside the EQ's range, which
// are moved to its nearest).
Values eq8(const Element* d, std::vector<std::string>& report, const std::string& where) {
    Values v;
    struct Bell {
        double absGain, freq, gain, q;
        bool operator<(const Bell& o) const { return std::tie(absGain, freq, gain, q) < std::tie(o.absGain, o.freq, o.gain, o.q); }
    };
    std::vector<Bell> bells;
    std::vector<std::string> moved;
    auto fit = [&](double freq, double low, double high, const std::string& what) {
        double inside = clamp(freq, low, high);
        if (std::fabs(inside - freq) > 0.5) moved.push_back(what + " " + fmt0(freq) + " Hz \xE2\x86\x92 " + fmt0(inside) + " Hz");
        return inside;
    };
    for (int i = 0; i < 8; i++) {
        const Element* band = d->find("Bands." + std::to_string(i) + "/ParameterA");
        if (!band || !manualB(band, "IsOn", false)) continue;
        int mode = manualI(band, "Mode", 3);
        double freq = manualD(band, "Freq", 1000.0), gain = manualD(band, "Gain", 0.0), q = manualD(band, "Q", 0.7);
        if (mode == 0 || mode == 1) {
            const double* lc = getValue(v, "low_cut");
            setValue(v, "low_cut", std::max(freq, lc ? *lc : 0.0));
        } else if (mode == 2) {
            setValue(v, "low_freq", fit(freq, 30, 500, "low shelf"));
            setValue(v, "low_gain", clamp(gain, -18, 18));
        } else if (mode == 3 && std::fabs(gain) > 0.05) {
            bells.push_back({std::fabs(gain), freq, gain, q});
        } else if (mode == 5) {
            setValue(v, "high_freq", fit(freq, 1000, 16000, "high shelf"));
            setValue(v, "high_gain", clamp(gain, -18, 18));
        } else if (mode == 6 || mode == 7) {
            const double* hc = getValue(v, "high_cut");
            setValue(v, "high_cut", std::min(freq, hc ? *hc : 1e9));
        }
    }
    if (const double* lc = getValue(v, "low_cut")) setValue(v, "low_cut", fit(*lc, 10, 1000, "low cut"));
    if (const double* hc = getValue(v, "high_cut")) setValue(v, "high_cut", fit(*hc, 1000, 22000, "high cut"));
    if (!bells.empty()) {  // (the strongest that fits the mid band's range: one moved far off would boost the wrong place)
        std::vector<Bell> fitting;
        for (auto& b : bells)
            if (100 <= b.freq && b.freq <= 10000) fitting.push_back(b);
        const std::vector<Bell>& pool = fitting.empty() ? bells : fitting;
        Bell best = *std::max_element(pool.begin(), pool.end());
        setValue(v, "mid_freq", fit(best.freq, 100, 10000, "bell"));
        setValue(v, "mid_gain", clamp(best.gain, -18, 18));
        setValue(v, "mid_q", clamp(best.q, 0.3, 8));
        std::vector<Bell> sorted = bells;
        std::sort(sorted.rbegin(), sorted.rend());
        std::vector<std::string> left;
        for (auto& b : sorted)
            if (b.freq != best.freq) left.push_back(fmtSigned1(b.gain) + " dB at " + fmt0(b.freq) + " Hz");
        if (!left.empty())
            report.push_back(where + ": EQ Eight has " + std::to_string(bells.size()) + " bell bands; the EQ has one, and keeps " +
                             fmtSigned1(best.gain) + " dB at " + fmt0(best.freq) + " Hz (left out: " + join(left, ", ") + ")");
    }
    if (!moved.empty()) report.push_back(where + ": EQ Eight's " + join(moved, ", ") + " (outside the EQ's range)");
    setValue(v, "output", clamp(manualD(d, "GlobalGain", 0.0), -24, 24));
    return v;
}

constexpr double FREEVERB_COMB_S = 1356.0 / 44100;  // the reverb's (effects/reverb.py) average comb delay

// Live's Reverb: its decay time is matched by the size whose combs fall 60 dB in that time (the reverb's
// feedback is 0.7 + 0.28 × size); its Dry/Wet is stored as MixDirect.
Values reverb(const Element* d) {
    double decayS = std::max(0.1, manualD(d, "DecayTime", 2000.0) / 1000);
    double feedback = std::pow(10.0, -3 * FREEVERB_COMB_S / decayS);  // (60 dB down after decayS)
    return {{"size", clamp((feedback - 0.7) / 0.28 * 100, 0, 100)},
            {"predelay", clamp(manualD(d, "PreDelay", 10.0), 0, 100)},
            {"damping", manualB(d, "ShelfHighOn", false) ? 60.0 : 30.0},
            {"width", clamp(manualD(d, "StereoSeparation", 100.0), 0, 100)},
            {"mix", 100 * clamp(manualD(d, "MixDirect", 1.0), 0, 1)}};
}

Values delay(const Element* d, const std::string& prefix, const std::string& timeL, const std::string& timeR,
             const std::string& syncL) {
    Values v = {{"left", clamp(manualD(d, prefix + timeL, 0.25) * 1000, 1, 2000)},
                {"right", clamp(manualD(d, prefix + timeR, 0.375) * 1000, 1, 2000)},
                {"feedback", clamp(manualD(d, "Feedback", 0.4) * 100, 0, 95)},
                {"mix", 100 * clamp(manualD(d, "DryWet", 0.25), 0, 1)}};
    if (manualB(d, prefix + syncL, false)) {
        double beats = manualI(d, prefix + "SyncedSixteenthL", 4) / 4.0;  // (sixteenths)
        const double options[] = {0.25, 1.0 / 3, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0};  // (effects/delay.py's BEATS)
        int best = 0;
        for (int i = 1; i < 8; i++)
            if (std::fabs(options[i] - beats) < std::fabs(options[best] - beats)) best = i;
        setValue(v, "sync", 1.0);
        setValue(v, "left_beats", best);
        setValue(v, "right_beats", best);
    }
    if (manualB(d, prefix + "PingPong", false)) setValue(v, "pingpong", 1.0);
    return v;
}

// (built-in effect id, its values) standing in for an Ableton audio effect; false if none
bool audioEffect(const Element* d, std::vector<std::string>& report, const std::string& where, std::string& id, Values& v) {
    auto g = [&](const std::string& name, double def = 0.0) { return manualD(d, name, def); };
    std::string_view tag = d->tag;
    if (tag == "Compressor2") {
        id = "compressor";
        v = {{"threshold", clamp(db(g("Threshold", 1.0)), -60, 0)}, {"ratio", clamp(g("Ratio", 4.0), 1, 20)},
             {"attack", clamp(g("Attack", 10.0), 0.1, 100)}, {"release", clamp(g("Release", 120.0), 10, 1000)},
             {"knee", clamp(g("Knee", 6.0), 0, 18)}, {"makeup", clamp(g("Gain"), 0, 24)},
             {"mix", 100 * clamp(g("DryWet", 1.0), 0, 1)}};
    } else if (tag == "GlueCompressor") {
        auto pick = [&](const double* options, int count, const std::string& name, double def) {
            return options[(int)clamp((int)g(name, def), 0, count - 1)];
        };
        id = "compressor";
        v = {{"threshold", clamp(g("Threshold", -10.0), -60, 0)}, {"ratio", pick(GLUE_RATIOS, 3, "Ratio", 0)},
             {"attack", clamp(pick(GLUE_ATTACKS_MS, 7, "Attack", 3), 0.1, 100)}, {"release", pick(GLUE_RELEASES_MS, 7, "Release", 6)},
             {"knee", 6.0}, {"makeup", clamp(g("Makeup"), 0, 24)}, {"mix", 100 * clamp(g("DryWet", 1.0), 0, 1)}};
    } else if (tag == "Eq8") {
        id = "eq";
        v = eq8(d, report, where);
    } else if (tag == "Reverb") {
        id = "reverb";
        v = reverb(d);
    } else if (tag == "Saturator") {
        id = "saturation";
        v = {{"drive", clamp(g("BaseDrive", 12.0), 0, 36)}, {"mix", 100 * clamp(g("DryWet", 1.0), 0, 1)},
             {"output", clamp(g("PostDrive"), -24, 12)}};
    } else if (tag == "Chorus2") {
        id = "chorus";
        v = {{"rate", clamp(g("Rate", 0.8), 0.05, 5)}, {"depth", clamp(g("Amount", 0.4) * 8, 0, 8)},
             {"mix", 100 * clamp(g("DryWet", 0.5), 0, 1)}, {"width", 100 * clamp(g("Width", 1.0), 0, 1)}};
    } else if (tag == "Delay") {
        id = "delay";
        v = delay(d, "DelayLine_", "TimeL", "TimeR", "SyncL");
    } else if (tag == "Echo") {
        id = "delay";
        v = delay(d, "Delay_", "TimeL", "TimeR", "SyncL");
        setValue(v, "mix", 100 * clamp(g("DryWet", 0.5), 0, 1));
    } else if (tag == "Limiter") {
        id = "limiter";
        v = {{"gain", clamp(g("Gain"), 0, 24)}, {"ceiling", clamp(g("Ceiling", -0.3), -12, 0)},
             {"release", clamp(g("Release", 60.0), 1, 1000)}};
    } else if (tag == "MultibandDynamics") {
        id = "multiband_compressor";
        v = {{"low_cross", clamp(g("SplitLowMid", 200.0), 40, 1000)}, {"high_cross", clamp(g("SplitMidHigh", 3000.0), 1000, 12000)},
             {"output", clamp(g("OutputGain"), -24, 24)}};
    } else if (tag == "Gate") {
        id = "gate";
        v = {{"threshold", clamp(g("Threshold", -40.0), -80, 0)}, {"attack", clamp(g("Attack", 0.5), 0.05, 50)},
             {"hold", clamp(g("Hold", 30.0), 0, 500)}, {"release", clamp(g("Release", 100.0), 5, 2000)}};
    } else if (tag == "Redux2") {  // (its sample rate is in Hz: the bit-crusher holds each sample this many times instead)
        double rate = g("SampleRate", SR / 4.0);
        double hold = rate > 64 ? SR / rate : rate;  // (a small number: already a factor)
        id = "bitcrusher";
        v = {{"bits", clamp((double)pyRound(g("BitDepth", 8.0)), 1, 16)}, {"downsample", clamp((double)pyRound(hold), 1, 64)},
             {"mix", 100 * clamp(g("DryWet", 1.0), 0, 1)}};
    } else {
        return false;
    }
    return true;
}

// PluginSlots of Exbit's built-in effects standing in for a track's Ableton effects (in order)
std::vector<SlotPtr> audioEffects(const Element* track, std::vector<std::string>& report, const std::string& where) {
    std::vector<SlotPtr> slots;
    std::set<std::string> left;
    for (const Element* d : devicesOf(track->find("DeviceChain/DeviceChain/Devices"), report, where)) {
        if (!isOn(d)) continue;
        std::string id;
        Values values;
        if (!audioEffect(d, report, where, id, values) || !effects::find(id)) {
            left.insert(std::string(d->tag));
            continue;
        }
        std::string error;
        auto effect = effects::create(id, error);
        if (!effect) {
            left.insert(std::string(d->tag));
            continue;
        }
        Json state = Json::object();
        for (auto& kv : values) state.set(kv.first, Json((double)kv.second));
        effect->setState(state.dump(), "");
        auto slot = std::make_shared<PluginSlot>();
        slot->path = std::string(effects::PREFIX) + id;
        slot->name = effect->name();
        slot->kind = "effect";
        slot->state = plug::packState(effect->componentState(), effect->controllerState());
        slots.push_back(slot);
    }
    if (!left.empty())
        report.push_back(where + ": not carried over (no such effect in Exbit): " +
                         join(std::vector<std::string>(left.begin(), left.end()), ", "));
    return slots;
}

const std::map<int, int> ARP_STYLES = {{0, 0}, {1, 1}, {2, 2}, {4, 2}};  // Ableton's Up, Down, UpDown, Up & Down -> Exbit's
const std::map<int, int> SCALE_INDEX = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};  // Ableton's Major, Minor, Dorian, Mixolydian

// Exbit MIDI effects standing in for a MIDI track's Ableton ones (Arpeggiator, Note Length, Scale)
std::vector<MidiEffect> midiEffectsOf(const Element* track, const SetReader& reader, std::vector<std::string>& report,
                                      const std::string& where) {
    std::vector<MidiEffect> out;
    const std::vector<double>& rateBeats = midi_effects::RATE_BEATS;
    for (const Element* d : track->iter()) {
        if (!startsWith(d->tag, "Midi") || !d->find("On") || !isOn(d) || d->tag == "MidiEffectGroupDevice") continue;
        auto g = [&](const std::string& name, double def = 0.0) { return manualD(d, name, def); };
        MidiEffect e;
        if (d->tag == "MidiArpeggiator") {
            int mode = (int)g("Mode");
            if (!ARP_STYLES.count(mode)) report.push_back(where + ": the Arpeggiator's style isn't in Exbit's: it plays Up");
            double beats;
            if (manualB(d, "SyncState", false))
                beats = ARP_RATES[std::max(0, std::min((int)g("SyncedRate", 8), ARP_RATE_COUNT - 1))];
            else
                beats = g("FreeRate", 100.0) / 1000 / reader.spb;
            int rate = 0;
            double bestD = 1e300;
            for (size_t i = 0; i < rateBeats.size(); i++) {
                double dist = std::fabs(std::log(rateBeats[i] / std::max(beats, 1e-3)));
                if (dist < bestD) {
                    bestD = dist;
                    rate = (int)i;
                }
            }
            auto style = ARP_STYLES.find(mode);
            MidiEffect::make("Arpeggiator", {{"style", style == ARP_STYLES.end() ? 0 : style->second}, {"rate", rate},
                                             {"gate", clamp(g("Gate", 100.0), 10, 200)}}, e);
            out.push_back(e);
        } else if (d->tag == "MidiNoteLength") {
            double beats = manualB(d, "Mode", false) ? g("SyncedLength", 4) * 0.25 : g("TimeLength", 100.0) / 1000 / reader.spb;
            MidiEffect::make("Note Length", {{"mode", 0}, {"beats", clamp(beats * g("Gate", 100.0) / 100, 0.0625, 8)}}, e);
            out.push_back(e);
        } else if (d->tag == "MidiScale") {
            bool identity = true;
            for (int i = 0; i < 12; i++)
                if (manualI(d, "Mapping." + std::to_string(i), i) != i) identity = false;
            if (identity && !manualB(d, "UseCurrentScale", false)) continue;  // (it changes nothing)
            int scale = (int)g("InternalScale");
            if (!SCALE_INDEX.count(scale)) report.push_back(where + ": the Scale device's scale isn't in Exbit's: Major is used");
            auto idx = SCALE_INDEX.find(scale);
            int base = (int)g("Base");
            MidiEffect::make("Scale", {{"root", ((base % 12) + 12) % 12}, {"scale", idx == SCALE_INDEX.end() ? 0 : idx->second}}, e);
            out.push_back(e);
        } else {
            report.push_back(where + ": MIDI effect " + util::replaceAll(std::string(d->tag), "Midi", "") + " not carried over");
        }
    }
    return out;
}

// --- the conversion ---

struct Copied {
    AudioPtr source;
    int sourceSr = 0;
    std::string path;  // in the project's audio folder
    std::string name;  // the file's name without its extension
    ClipPtr holder;    // an audio clip holding it (the built-in sampler's), made when needed
};

bool copyFile(const std::string& from, const std::string& to) {
    std::string data;
    if (!util::readFile(from, data)) return false;
    return util::writeFile(to, data);
}

std::string splitextRoot(const std::string& path) {
    std::string base = util::baseName(path);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return path;
    return path.substr(0, path.size() - (base.size() - dot));
}
std::string splitextExt(const std::string& path) { return path.substr(splitextRoot(path).size()); }

std::shared_ptr<const SamplerSettings> samplerSettings(const Element* device, Context& ctx,
                                                        const std::function<Copied&(const std::string&)>& copy) {
    Sampler zones(device, ctx);
    if (zones.zones.size() != 1) return nullptr;
    const Zone& zone = zones.zones[0];
    Copied& sample = copy(zone.path);
    if (!sample.holder) {
        sample.holder = makeAudioClip(sample.name, sample.source, sample.sourceSr, SR);
        sample.holder->sourcePath = sample.path;
    }
    double frames = (double)(sample.source ? sample.source->frames() : 0);
    auto s = std::make_shared<SamplerSettings>();
    s->sample = sample.holder;
    s->root = zone.root;
    s->transpose = zones.transpose + zone.detune / 100;
    s->gainDb = zones.gainDb + db(zone.gain);
    s->velocity = clamp(zones.velScale, 0, 1);
    s->attack = zones.attack;
    s->decay = zones.decay;
    s->sustain = clamp(zones.sustain, 0, 1);
    s->release = zones.release;
    s->loop = zone.loopMode && zone.loopB > zone.loopA;
    s->start = frames ? clamp(zone.start / frames, 0, 0.999) : 0.0;
    s->end = frames ? clamp((zone.end ? zone.end : frames) / frames, s->start + 0.001, 1.0) : 1.0;
    return s;
}

void convertSet(const std::string& als, const std::string& output, Project& project, Result& result, const Progress& progressIn) {
    auto progress = [&](const std::string& text, double share) {
        if (progressIn) progressIn(text, share);
    };
    std::vector<std::string>& report = result.report;
    report.clear();
    progress("Reading the set", 0.0);
    SetReader reader(als);
    Context ctx;
    ctx.reader = &reader;
    ctx.bpm = reader.bpm;
    // the Session view's clips: Exbit has no clip launcher, so only the arrangement comes over; say so
    size_t sessionClips = reader.live->findAll(".//ClipSlotList//MidiClip").size() +
                          reader.live->findAll(".//ClipSlotList//AudioClip").size();
    if (sessionClips)
        report.push_back(util::format("%zu Session view clip%s left out: only the arrangement is imported (Exbit "
                                      "has no clip launcher; record or drag them into Live's arrangement first)",
                                      sessionClips, sessionClips == 1 ? "" : "s"));
    std::string folder = project_io::audioFolder(output);
    if (util::isDir(folder) && !util::listDir(folder).empty())
        throw Error(folder + " isn't empty: import into a new place (or empty it first)");
    if (!util::isDir(folder) && !util::makeDirs(folder)) throw Error("Cannot make the folder " + folder);

    project.clear();
    project.timing.bpm = reader.bpm;
    project.timing.beatsPerBar = reader.beatsPerBar;
    project.timing.beatUnit = reader.denominator;
    for (auto& c : reader.sigChanges) project.timing.setSignatureNear(c.first, c.second);  // (Live's beats: quarter notes)
    if (!reader.sigChanges.empty())
        util::debugf("ableton: time signature automation: %zu events -> %zu time signatures\n", reader.sigChanges.size(),
                     project.timing.map()->meters().size());
    project.timing.publish();
    // clips are prepared as Exbit plays them: through their track's MIDI effects and built-in sampler
    // (the program sets these hooks at start-up; a command-line conversion or a test may not have)
    if (!clip_hooks::midiEffects) midi_effects::install(midi_effects::timing());
    if (!clip_hooks::renderSampler) sampler::install();
    midi_effects::TimingScope scope(&project.timing);

    std::vector<TrackPtr> tracks;
    std::vector<ClipPtr> clips;
    struct Render {
        TrackPtr track;
        double first;
        Stereo audio;
    };
    std::vector<Render> renders;
    std::map<std::string, Copied> copied;
    std::vector<std::string> copiedOrder;

    // the file in the project's audio folder (one copy per file)
    std::function<Copied&(const std::string&)> copy = [&](const std::string& path) -> Copied& {
        auto it = copied.find(path);
        if (it != copied.end()) return it->second;
        std::string target = util::joinPath(folder, util::baseName(path));
        if (util::fileExists(target))  // (two files of the same name in different folders)
            target = splitextRoot(target) + " (" + std::to_string(copied.size()) + ")" + splitextExt(target);
        if (!copyFile(path, target)) throw Error("Cannot copy " + path + " to " + target);
        Copied c;
        std::string error;
        if (!file_io::loadAudio(target, c.source, c.sourceSr, error)) throw Error(error);
        c.path = target;
        c.name = util::stem(path);
        copiedOrder.push_back(path);
        return copied[path] = c;
    };

    auto mixerOf = [&](const Element* t, Track& track) {
        const Element* mixer = t->find("DeviceChain/Mixer");
        if (!mixer) throw Error("a track without its mixer: " + track.name);
        track.volumeDb = round1(db(manualD(mixer, "Volume", 1.0)));
        track.muted = !manualB(mixer, "Speaker", true);
        track.soloed = valB(mixer, "SoloSink", false);
        track.pan = clamp(manualD(mixer, "Pan", 0.0), -1.0, 1.0);
        std::vector<double> sends;
        for (const Element* send : mixer->findAll("Sends/TrackSendHolder/Send")) sends.push_back(manualD(send, ".", 0.0));
        return sends;
    };

    std::vector<TrackPtr> returns;
    for (const Element* t : reader.returns()) {
        std::string name = valS(t, "Name/EffectiveName");
        auto track = std::make_shared<Track>(name, Kind::Return);
        track->color = colorOf(t);
        mixerOf(t, *track);
        auto fx = audioEffects(t, report, name);
        track->effects = fx;
        track->cables = chain(fx);
        returns.push_back(track);
    }

    // group tracks: an Exbit group each, a bus (model.h GROUP BUSES) with the group's own effects, fader, pan,
    // mute and sends, nested as in Live
    auto groupTracks = reader.groups();
    std::map<std::string, GroupPtr> madeGroups;
    for (auto& [gid, g] : groupTracks) {
        std::string gname = valS(g, "Name/EffectiveName", "Group");
        auto group = std::make_shared<Group>();
        group->name = gname;
        group->color = colorOf(g);
        group->folded = !valB(g, "TrackUnfolded", true);
        Track& bus = *group->bus;
        bus.name = gname;
        if (g->find("DeviceChain/Mixer")) {
            std::vector<double> sends = mixerOf(g, bus);
            group->muted = bus.muted;  // (mute and solo are the group's: they reach its tracks)
            group->soloed = bus.soloed;
            bus.muted = bus.soloed = false;
            std::vector<Send> busSends;
            for (size_t i = 0; i < std::min(returns.size(), sends.size()); i++)
                if (db(sends[i]) > SEND_FLOOR_DB) busSends.push_back({returns[i], round1(db(sends[i]))});
            bus.sends = busSends;
        }
        auto fx = audioEffects(g, report, gname);
        bus.effects = fx;
        bus.cables = chain(fx);
        madeGroups[gid] = group;
        report.push_back(gname + ": a group bus (its effects, fader, pan, mute and sends)");
    }
    for (auto& [gid, g] : groupTracks) {  // (inside another group, as in Live)
        auto parent = madeGroups.find(valS(g, "TrackGroupId", "-1"));
        if (parent != madeGroups.end() && parent->second != madeGroups[gid] &&
            routing::canNest(madeGroups[gid], parent->second))
            madeGroups[gid]->parent = parent->second;
    }

    auto sources = reader.tracks();
    for (size_t number = 0; number < sources.size(); number++) {
        const Element* t = sources[number];
        std::string name = valS(t, "Name/EffectiveName");
        double denom = (double)std::max<size_t>(1, sources.size());
        progress("Converting " + name, 0.05 + 0.75 * number / denom);
        auto track = std::make_shared<Track>(name, t->tag == "MidiTrack" ? Kind::Midi : Kind::Audio);
        track->color = colorOf(t);
        std::vector<double> sends = mixerOf(t, *track);
        std::vector<Send> trackSends;
        for (size_t i = 0; i < std::min(returns.size(), sends.size()); i++)
            if (db(sends[i]) > SEND_FLOOR_DB) trackSends.push_back({returns[i], round1(db(sends[i]))});
        track->sends = trackSends;
        auto inGroup = madeGroups.find(valS(t, "TrackGroupId", "-1"));  // (its innermost group)
        if (inGroup != madeGroups.end()) track->group = inGroup->second;
        tracks.push_back(track);
        if (track->kind == Kind::Audio) {
            auto fx = audioEffects(t, report, name);
            track->effects = fx;
            track->cables = chain(fx);
            for (auto& place : reader.audioClips(t)) {
                Copied& held = copy(place.file);
                int64_t frames = pyRound(place.length * SR);
                auto clip = std::make_shared<Clip>();
                clip->kind = Kind::Audio;
                clip->name = valS(place.clip, "Name");
                clip->track = track;
                clip->start = pyRound(place.start * SR);
                clip->offset = pyRound(place.offset * place.stretch * SR);
                clip->length = frames;
                clip->color = colorOf(place.clip);
                clip->gainDb = round2(db(valD(place.clip, "SampleVolume", 1.0)));
                clip->stretch = place.stretch;
                clip->pitch = place.pitch;
                clip->fadeIn = std::max<int64_t>(0, std::min(pyRound(place.fadeIn * SR), frames / 2));
                clip->fadeOut = std::max<int64_t>(0, std::min(pyRound(place.fadeOut * SR), frames / 2));
                clip->disabled = place.disabled;
                clip->loop = place.hasLoop;
                clip->loopStart = place.hasLoop ? place.loopStart * place.stretch : 0.0;
                clip->loopLength = place.hasLoop ? place.loopLength * place.stretch : 0.0;
                clip->sourceBpm = place.stretch != 1.0 ? reader.bpm * place.stretch : 0;
                clip->source = held.source;
                clip->sourceSr = held.sourceSr;
                clip->sourcePath = held.path;
                clips.push_back(clip);
            }
            continue;
        }
        std::vector<Note> allNotes;
        for (auto& m : reader.midiNotes(t)) {
            double start = needD(m.clip, "CurrentStart") * reader.spb, end = needD(m.clip, "CurrentEnd") * reader.spb;
            auto relative = [&](const std::vector<Note>& ns) {
                std::vector<Note> out;
                for (auto& n : ns) out.push_back({n.start - start, n.end - start, n.pitch, (double)pyRound(n.velocity)});
                return out;
            };
            bool disabled = valB(m.clip, "Disabled", false);  // (switched off in Live: kept, silent, not in the rendering)
            auto clip = std::make_shared<Clip>();
            clip->kind = Kind::Midi;
            clip->name = valS(m.clip, "Name");
            clip->track = track;
            clip->start = pyRound(start * SR);
            clip->length = pyRound((end - start) * SR);
            clip->color = colorOf(m.clip);
            clip->notes = std::make_shared<const std::vector<Note>>(relative(m.notes));
            if (!m.offNotes.empty()) clip->mutedNotes = std::make_shared<const std::vector<Note>>(relative(m.offNotes));
            clip->lengthS = end - start;
            clip->disabled = disabled;
            clips.push_back(clip);
            if (!disabled) allNotes.insert(allNotes.end(), m.notes.begin(), m.notes.end());
        }
        track->midiEffects = midiEffectsOf(t, reader, report, name);
        auto samplers = samplersOf(t);
        if (!samplers.empty() && !kindsOf(t).count("DrumGroupDevice")) track->sampler = samplerSettings(samplers[0], ctx, copy);
        std::vector<Transform> transforms;
        std::string about;
        InstrumentPtr instrument = instrumentOf(t, ctx, transforms, about);
        std::vector<Note> played = allNotes;
        for (auto& transform : transforms) played = transform(played);
        if (played.empty()) continue;
        double first = played[0].start, last = played[0].end;
        for (auto& n : played) {
            first = std::min(first, n.start);
            last = std::max(last, n.end);
        }
        last += 4.0;  // (room for release tails)
        int64_t frames = (int64_t)((last - first) * SR);
        progress("Rendering " + name + " (" + about + ")", 0.05 + 0.75 * (number + 0.5) / denom);
        std::vector<Note> shifted;
        for (auto& n : played) shifted.push_back({n.start - first, n.end - first, n.pitch, n.velocity});
        Stereo audio = instrument->render(shifted, frames);
        int64_t lastLoud = -1;
        for (int64_t i = audio.frames() - 1; i >= 0; i--)
            if (std::fabs(audio.s[i * 2]) > 1e-5f || std::fabs(audio.s[i * 2 + 1]) > 1e-5f) {
                lastLoud = i;
                break;
            }
        if (lastLoud >= 0) audio.s.resize((size_t)std::min(audio.frames(), lastLoud + (int64_t)(0.05 * SR)) * 2);
        renders.push_back({track, first, std::move(audio)});
        std::string unfreeze = track->sampler ? "the built-in sampler with its sample" : "the built-in synth (choose an instrument for it)";
        report.push_back(name + ": " + about + "; frozen (as rendered here, without its effects); unfrozen it plays " + unfreeze);
    }

    progress("Preparing the audio", 0.82);
    for (auto& clip : clips) {
        clip->prepare(SR);
        if (clip->kind == Kind::Audio) {  // (a window reaching past the file's end: to its end)
            int64_t len = clip->data ? clip->data->frames() : 0;
            clip->length = std::max<int64_t>(1, std::min(clip->length, len - clip->offset));
            clip->fadeIn = std::min(clip->fadeIn, clip->length / 2);
            clip->fadeOut = std::min(clip->fadeOut, clip->length / 2);
        }
    }
    {
        std::vector<ClipPtr> kept;
        for (auto& c : clips)
            if (c->kind != Kind::Audio || (c->data && c->offset < c->data->frames())) kept.push_back(c);
        clips.swap(kept);
    }
    // the loudest audio clip's loudness, which every rendering is brought to (before its fader)
    double loudest = -1;
    for (auto& clip : clips)
        if (clip->kind == Kind::Audio && !clip->disabled) {
            int64_t n = std::min(clip->frames(), clip->data->frames() - clip->offset);
            loudest = std::max(loudest, activeRms(clip->data->frame(clip->offset), n));
        }
    double target = (loudest >= 0 ? loudest : 0.1) * std::pow(10.0, INSTRUMENT_DB / 20);
    for (auto& r : renders) {
        double level = activeRms(r.audio.s.data(), r.audio.frames());
        if (level > 0)
            for (auto& x : r.audio.s) x = (float)(x * (target / level));
        float peak = 0;
        for (float x : r.audio.s) peak = std::max(peak, std::fabs(x));
        std::string file, error;
        bool ok;
        if (peak <= 1) {  // 24-bit FLAC: a fraction of the size of Exbit's 32-bit WAVs
            file = frozenFile(folder, r.track->name, ".flac");
            ok = writeFlac(file, r.audio.s.data(), (size_t)r.audio.frames(), 2, SR, SampleFormat::Pcm24, error);
        } else {  // louder than full scale before its fader: kept as 32-bit float, so nothing is clipped
            file = frozenFile(folder, r.track->name, ".wav");
            ok = writeWav(file, r.audio.s.data(), (size_t)r.audio.frames(), 2, SR, SampleFormat::Float32, error);
        }
        if (!ok) throw Error(error);
        AudioPtr source;
        int sourceSr = 0;
        if (!file_io::loadAudio(file, source, sourceSr, error)) throw Error(error);
        auto frozen = std::make_shared<Clip>();
        frozen->kind = Kind::Audio;
        frozen->name = r.track->name + " (frozen)";
        frozen->start = pyRound(r.first * SR);
        frozen->source = source;
        frozen->sourceSr = sourceSr;
        frozen->sourcePath = file;
        frozen->prepare(SR);
        std::atomic_store(&r.track->frozen, frozen);
    }

    std::vector<TrackPtr> all = tracks;
    all.insert(all.end(), returns.begin(), returns.end());
    project.tracks = all;
    project.clips = clips;
    std::vector<Marker> markers;
    for (auto& l : reader.locators) markers.push_back({std::max(0.0, l.first * reader.spb), l.second.empty() ? "Locator" : l.second});
    project.setMarkers(markers);
    if (!reader.tempoEvents.empty()) {  // (everything was placed at the set's tempo: it moves to its beats on the new map)
        timebase::Before before = timebase::remember(project);
        int made = tempoFromEnvelope(project.timing, reader.tempoEvents, reader.beatsPerBar, reader.denominator);
        size_t moved = timebase::follow(project, before, SR).size();
        util::debugf("ableton: tempo automation: %zu events → %d tempo changes; %zu clips moved to their beats\n",
                     reader.tempoEvents.size(), made, moved);
        if (made || project.timing.ramp)
            report.push_back(util::format("the tempo automation came over as %d tempo change%s (ramps where Live ramps); "
                                          "audio clips keep their own speed through it",
                                          made, made == 1 ? "" : "s"));
    }

    // the mix as Exbit plays it (its effects too, returns, faders, pan): its peak sets the master volume
    progress("Measuring the mix", 0.86);
    int masterDb = 0;
    int64_t end = project.endFrame();
    if (end) {
        std::vector<SlotPtr> slots;
        for (auto& t : project.allTracks())
            for (auto& s : *t->effects) slots.push_back(s);
        for (auto& slot : slots) {
            std::string error;
            auto effect = effects::create(slot->path, error);
            if (!effect) throw Error(error);
            effect->prepare(SR);
            std::string component, controller;
            if (plug::unpackState(slot->state, component, controller)) effect->setState(component, controller);
            slot->instance = std::shared_ptr<plug::Plugin>(effect.release());
        }
        auto mix = render::renderMix(project, 0, end, SR, 0.0, true);
        for (auto& slot : slots) slot->instance = nullptr;  // (Exbit loads them itself when the project opens)
        render::clearPatchStates(project);
        float peak = 0;
        if (mix)
            for (float x : *mix) peak = std::max(peak, std::fabs(x));
        double peakDb = db(peak);
        masterDb = (int)std::min(0.0, std::floor(PEAK_DB - peakDb));  // (Exbit's master volume is in whole dB)
    }
    progress("Saving", 0.95);
    project_io::MasterSettings master;
    master.volumeDb = masterDb;
    master.mute = false;
    std::string error;
    if (!project_io::saveProject(output, project, SR, master, error)) throw Error(error);
    result.masterDb = masterDb;
    report.insert(report.begin(),
                  util::baseName(als) + ": " + fmtG(reader.bpm) + " BPM, " + std::to_string(reader.beatsPerBar) + "/" +
                      std::to_string(reader.denominator) + "; " + std::to_string(tracks.size()) + " tracks, " +
                      std::to_string(returns.size()) + " returns, " + std::to_string(clips.size()) + " clips, " +
                      std::to_string(renders.size()) + " frozen, " + std::to_string(reader.locators.size()) +
                      " markers; master volume " + std::to_string(masterDb) + " dB (so the mix peaks under " + fmtG(PEAK_DB) +
                      " dBFS)");
    progress("Done", 1.0);
}

}  // namespace

// --- public ---

std::string color(int index) {
    if (index < 0 || index >= PALETTE_SIZE) return "";
    return "#" + util::lower(PALETTE[index]);
}

std::string frozenFile(const std::string& folder, const std::string& trackName, const std::string& ext) {
    // re.sub(r'[\\/:*?"<>|\x00-\x1f]+', "_", name).strip(" .") or "Track"
    std::string stem;
    bool inRun = false;
    for (unsigned char c : trackName) {
        bool bad = c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        if (bad) {
            if (!inRun) stem += '_';
            inRun = true;
        } else {
            stem += (char)c;
            inRun = false;
        }
    }
    size_t a = stem.find_first_not_of(" ."), b = stem.find_last_not_of(" .");
    stem = a == std::string::npos ? std::string() : stem.substr(a, b - a + 1);
    if (stem.empty()) stem = "Track";
    std::string file = util::joinPath(folder, stem + " (frozen)" + ext);
    for (int n = 2; util::fileExists(file); n++) file = util::joinPath(folder, stem + " (frozen " + std::to_string(n) + ")" + ext);
    return file;
}

int tempoFromEnvelope(Timing& timing, const std::vector<std::pair<double, double>>& events, int beatsPerBar,
                      int beatUnit) {
    if (events.empty()) return 0;
    const double JUMP = 1e-3;  // beats: a ramp running into a jump ends this much before it
    auto bpmOf = [](double v) { return std::min(config::MAX_BPM, std::max(config::MIN_BPM, v)); };
    struct P {
        double beat, bpm;
        bool hold;  // (the end of a ramp right before a jump: it doesn't ramp on)
    };
    std::vector<P> ps;
    for (size_t i = 0; i < events.size();) {
        double at = std::max(0.0, events[i].first);  // (Live's first event lies far before the song)
        size_t j = i;
        while (j + 1 < events.size() && std::max(0.0, events[j + 1].first) == at) j++;
        double first = bpmOf(events[i].second), last = bpmOf(events[j].second);
        if (ps.empty() && at > 0) ps.push_back({0.0, first, false});  // (before the first event: its tempo)
        if (first != last && !ps.empty() && at - JUMP > ps.back().beat + JUMP) ps.push_back({at - JUMP, first, true});
        if (!ps.empty() && ps.back().beat >= at) ps.back() = {ps.back().beat, last, false};
        else ps.push_back({at, last, false});
        i = j + 1;
    }
    auto ramps = [&](size_t k) { return !ps[k].hold && k + 1 < ps.size() && ps[k + 1].bpm != ps[k].bpm; };
    timing.bpm = ps[0].bpm;
    timing.beatsPerBar = std::max(1, beatsPerBar);
    timing.beatUnit = timing::checkedUnit(beatUnit, "tempoFromEnvelope");
    timing.ramp = ramps(0);
    // the time signatures already set from bar 2 on stay: the bars (in quarter notes, as Live counts its beats) are theirs
    std::vector<TempoChange> sigs;
    for (const TempoChange& c : timing.changes)
        if (c.atBar()) sigs.push_back(c);
    const TempoMap bars(120, timing.beatsPerBar, sigs, false, timing.beatUnit);
    timing.changes.clear();
    double lastBpm = ps[0].bpm;
    bool lastRamps = timing.ramp;
    for (size_t k = 1; k < ps.size(); k++) {
        bool ramp = ramps(k);
        if (ps[k].bpm == lastBpm && !lastRamps && !ramp) continue;  // (no change)
        int bar = bars.barAtBeat(ps[k].beat);
        double in = ps[k].beat - bars.barBeat(bar);
        if (in < 1e-9) in = 0;
        TimeSig sig = bars.meterAtBeat(ps[k].beat).sig();
        timing.changes.push_back(TempoChange{bar + 1, ps[k].bpm, sig.num, in, ramp, sig.den});
        lastBpm = ps[k].bpm;
        lastRamps = ramp;
    }
    int made = (int)timing.changes.size();
    // a time signature where no tempo change is: the tempo there as the envelope has it (a ramp through it goes on)
    for (const Meter& m : bars.meters()) {
        if (m.bar == 0 || timing.changeAt(m.bar + 1, 0)) continue;
        size_t k = 0;
        while (k + 1 < ps.size() && ps[k + 1].beat <= m.beat) k++;
        bool ramp = ramps(k);
        double bpm = ramp ? ps[k].bpm + (ps[k + 1].bpm - ps[k].bpm) * (m.beat - ps[k].beat) / (ps[k + 1].beat - ps[k].beat)
                          : ps[k].bpm;
        timing.changes.push_back(TempoChange{m.bar + 1, bpm, m.beatsPerBar, 0.0, ramp, m.beatUnit});
    }
    std::sort(timing.changes.begin(), timing.changes.end());
    timing.publish();
    return made;
}

double warpSeconds(const std::vector<std::pair<double, double>>& markers, double beat) {
    if (markers.empty()) return beat;
    if (markers.size() == 1) return markers[0].second + (beat - markers[0].first);
    std::pair<double, double> m0 = markers[markers.size() - 2], m1 = markers.back();
    for (size_t i = 0; i + 1 < markers.size(); i++)
        if (beat <= markers[i + 1].first) {
            m0 = markers[i];
            m1 = markers[i + 1];
            break;
        }
    if (m1.first == m0.first) return m0.second;
    return m0.second + (beat - m0.first) * (m1.second - m0.second) / (m1.first - m0.first);
}

std::vector<float> envelope(int64_t n, int64_t gate, double attackS, double decayS, double sustain, double releaseS) {
    std::vector<float> amp((size_t)std::max<int64_t>(n, 0));
    if (n <= 0) return amp;
    double sustainDb = std::max(FLOOR_DB, db(sustain));
    decayS = std::max(decayS, 1e-4);
    double gateS = (double)gate / SR;
    auto held = [&](double t) { return sustainDb * clamp((t - attackS) / decayS, 0, 1); };
    // np.interp(gate_s, t, level_db): the held level at the gate (the last frame's past the end)
    double gateT = std::min(gateS, (double)(n - 1) / SR);
    double levelAtGate;
    {
        double pos = gateT * SR;
        int64_t i = (int64_t)pos;
        double frac = pos - i;
        double a = held((double)i / SR), b = held((double)std::min(i + 1, n - 1) / SR);
        levelAtGate = a + (b - a) * frac;
    }
    double rel = std::max(releaseS, 1e-4);
    for (int64_t k = 0; k < n; k++) {
        double t = (double)k / SR;
        double levelDb = t >= gateS ? levelAtGate + (FLOOR_DB - levelAtGate) * clamp((t - gateS) / rel, 0, 1) : held(t);
        double a = std::pow(10.0, levelDb / 20);
        if (attackS > 0) a *= clamp(t / attackS, 0, 1);
        if (levelDb <= FLOOR_DB + 0.01) a = 0;
        amp[k] = (float)a;
    }
    return amp;
}

bool convert(const std::string& als, const std::string& output, Project& project, Result& result, std::string& error,
             const Progress& progress) {
    try {
        convertSet(als, output, project, result, progress);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace ableton
