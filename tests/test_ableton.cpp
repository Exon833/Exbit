// The Ableton Live set importer (importers/ableton.h). There are no .als files here, so the test writes
// its own: small Live sets as gzip-compressed XML with the element paths ableton.py reads, and sample files
// in the set's folder; then converts them and checks the project (as returned, and as saved and loaded).
//
// Covered: tempo, time signature (its real denominator: 7/8), locators, tracks (name, colour,
// fader, pan, mute, solo), return tracks and sends, groups (buses with their own fader, pan, effects and sends;
// mute, fold, solo, nested groups), audio clips (place, offset, gain, fades, warping →
// stretch, unwarped transpose → tape-like, loops, deactivated), sample lookup (relative path, by name with
// the best-matching folders, same-named files copied once each), MIDI clips (notes, switched-off notes,
// loops, deactivated), MIDI effects (Arpeggiator, Note Length, Scale; others reported), the built-in effects
// with their values (EQ Eight, Compressor, Glue Compressor, Reverb, Delay, Echo, Saturator, Chorus, Limiter,
// Multiband Dynamics, Gate, Redux), racks (first chain, reported), unknown and switched-off devices, frozen
// instruments (Simpler, Drum Rack, Drift, Operator, Meld, unknown), the one-sample Simpler → built-in sampler,
// the master volume, and errors (missing sample, a used folder, not gzip, not a set).
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <string>
#include <vector>

#include "audio/sound_file.h"
#include "check.h"
#include "core/json.h"
#include "core/util.h"
#include "importers/ableton.h"
#include "model/routing.h"
#include "importers/gzip.h"
#include "model/project_io.h"
#include "plugins/plugin.h"
#include "ui/ableton_import.h"

static const std::string kDir = "build/test-tmp/ableton";
static const int SR = 48000;

static void clearDir(const std::string& dir) {
    if (!util::isDir(dir)) return;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) {
            clearDir(p);
            rmdir(p.c_str());
        } else {
            util::removeFile(p);
        }
    }
}

// --- writing XML as Live does ---

static std::string num(double x) { return util::format("%.10g", x); }
static std::string v(const std::string& tag, const std::string& value) { return "<" + tag + " Value=\"" + value + "\" />"; }
static std::string v(const std::string& tag, double value) { return v(tag, num(value)); }
static std::string vb(const std::string& tag, bool value) { return v(tag, value ? "true" : "false"); }
static std::string m(const std::string& tag, double value) { return "<" + tag + "><Manual Value=\"" + num(value) + "\" /></" + tag + ">"; }
static std::string mb(const std::string& tag, bool value) {
    return "<" + tag + "><Manual Value=\"" + (value ? "true" : "false") + "\" /></" + tag + ">";
}
static std::string w(const std::string& tag, const std::string& inner) { return "<" + tag + ">" + inner + "</" + tag + ">"; }
static std::string wid(const std::string& tag, int id, const std::string& inner) {
    return "<" + tag + " Id=\"" + std::to_string(id) + "\">" + inner + "</" + tag + ">";
}

static std::string sampleRef(const std::string& relative, const std::string& path) {
    return w("SampleRef", w("FileRef", v("RelativePath", relative) + v("Path", path)));
}

static std::string mixer(double volume = 1.0, double pan = 0.0, bool speaker = true, bool solo = false,
                         std::vector<double> sends = {}) {
    std::string s;
    for (size_t i = 0; i < sends.size(); i++) s += wid("TrackSendHolder", (int)i, m("Send", sends[i]) + vb("EnabledByUser", true));
    return w("Mixer", w("Sends", s) + m("Pan", pan) + m("Volume", volume) + mb("Speaker", speaker) + vb("SoloSink", solo));
}

struct AClip {
    std::string name = "Clip";
    double start = 0, end = 4;  // beats on the timeline
    double loopStart = 0, loopEnd = 4, startRelative = 0;
    bool loopOn = false;
    std::string relative, path;  // its sample
    bool warped = false;
    std::vector<std::pair<double, double>> markers;  // (beat, seconds)
    double volume = 1.0, fadeIn = 0, fadeOut = 0, pitchCoarse = 0, pitchFine = 0;
    bool disabled = false;
    int color = 2;
};
static std::string audioClip(const AClip& c) {
    std::string markers;
    for (size_t i = 0; i < c.markers.size(); i++)
        markers += "<WarpMarker Id=\"" + std::to_string(i) + "\" SecTime=\"" + num(c.markers[i].second) + "\" BeatTime=\"" +
                   num(c.markers[i].first) + "\" />";
    return "<AudioClip Id=\"0\" Time=\"" + num(c.start) + "\">" + v("CurrentStart", c.start) + v("CurrentEnd", c.end) +
           w("Loop", v("LoopStart", c.loopStart) + v("LoopEnd", c.loopEnd) + v("StartRelative", c.startRelative) +
                         vb("LoopOn", c.loopOn)) +
           v("Name", c.name) + v("Color", c.color) + vb("Disabled", c.disabled) + sampleRef(c.relative, c.path) +
           vb("IsWarped", c.warped) + w("WarpMarkers", markers) + v("SampleVolume", c.volume) +
           w("Fades", v("FadeInLength", c.fadeIn) + v("FadeOutLength", c.fadeOut)) + v("PitchCoarse", c.pitchCoarse) +
           v("PitchFine", c.pitchFine) + "</AudioClip>";
}

struct N {
    double time, duration;
    int pitch;
    double velocity;
    bool enabled = true;
};
struct MClip {
    std::string name = "Notes";
    double start = 0, end = 8, loopStart = 0, loopEnd = 8, startRelative = 0;
    bool loopOn = false, disabled = false;
    std::vector<N> notes;
};
static std::string midiClip(const MClip& c) {
    std::map<int, std::string> keys;
    for (auto& n : c.notes)
        keys[n.pitch] += "<MidiNoteEvent Time=\"" + num(n.time) + "\" Duration=\"" + num(n.duration) + "\" Velocity=\"" +
                         num(n.velocity) + "\" VelocityDeviation=\"0\" OffVelocity=\"64\" Probability=\"1\" IsEnabled=\"" +
                         (n.enabled ? "true" : "false") + "\" NoteId=\"1\" />";
    std::string tracks;
    int id = 0;
    for (auto& k : keys) tracks += wid("KeyTrack", id++, w("Notes", k.second) + v("MidiKey", k.first));
    return "<MidiClip Id=\"0\" Time=\"" + num(c.start) + "\">" + v("CurrentStart", c.start) + v("CurrentEnd", c.end) +
           w("Loop", v("LoopStart", c.loopStart) + v("LoopEnd", c.loopEnd) + v("StartRelative", c.startRelative) +
                         vb("LoopOn", c.loopOn)) +
           v("Name", c.name) + v("Color", 7) + vb("Disabled", c.disabled) + w("Notes", w("KeyTracks", tracks)) + "</MidiClip>";
}

static std::string track(const std::string& tag, int id, const std::string& name, int color, int group,
                         const std::string& mixerXml, const std::string& clips, const std::string& devices) {
    std::string seq = tag == "AudioTrack" ? w("Sample", w("ArrangerAutomation", w("Events", clips)))
                                          : w("ClipTimeable", w("ArrangerAutomation", w("Events", clips)));
    return "<" + tag + " Id=\"" + std::to_string(id) + "\">" + v("TrackGroupId", group) + v("TrackUnfolded", "false") +
           w("Name", v("EffectiveName", name) + v("UserName", name)) + v("Color", color) +
           w("DeviceChain", mixerXml + w("MainSequencer", seq) + w("DeviceChain", w("Devices", devices))) + "</" + tag + ">";
}

static std::string groupTrack(int id, const std::string& name, int group, bool unfolded, const std::string& mixerXml,
                              const std::string& devices) {
    return "<GroupTrack Id=\"" + std::to_string(id) + "\">" + v("TrackGroupId", group) + vb("TrackUnfolded", unfolded) +
           w("Name", v("EffectiveName", name)) + v("Color", 20) +
           w("DeviceChain", mixerXml + w("DeviceChain", w("Devices", devices))) + "</GroupTrack>";
}

static std::string liveSet(const std::string& tracks, double bpm, int signature = 201,
                           const std::vector<std::pair<double, std::string>>& locators = {}) {
    std::string locs;
    for (size_t i = 0; i < locators.size(); i++)
        locs += wid("Locator", (int)i, v("Time", locators[i].first) + v("Name", locators[i].second));
    std::string main = w("MainTrack", w("DeviceChain", w("Mixer", m("Volume", 1.0) + m("Tempo", bpm) +
                                                                       w("TimeSignature", w("ArrangerAutomation", w("Events",
                                                                           "<EnumEvent Id=\"0\" Time=\"-63072000\" Value=\"" +
                                                                           std::to_string(signature) + "\" />"))))));
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Ableton MajorVersion=\"5\" MinorVersion=\"12.0_12049\" "
           "Creator=\"Ableton Live 12.0\">\n<LiveSet>" +
           w("Tracks", tracks) + main + w("Locators", w("Locators", locs)) + "</LiveSet>\n</Ableton>\n";
}

static void writeSet(const std::string& path, const std::string& xml) {
    auto gz = gzip::compress(xml);
    util::makeDirs(util::dirName(path));
    CHECK(util::writeFile(path, std::string(gz.begin(), gz.end())));
}

// a mono sample: a decaying sine
static void writeSample(const std::string& path, double seconds, double freq, double amp, int rate = 44100) {
    util::makeDirs(util::dirName(path));
    size_t n = (size_t)(seconds * rate);
    std::vector<float> s(n);
    for (size_t i = 0; i < n; i++) s[i] = (float)(amp * std::sin(2 * M_PI * freq * i / rate) * std::exp(-1.5 * i / (double)rate));
    std::string error;
    CHECK(writeWav(path, s.data(), n, 1, rate, SampleFormat::Pcm16, error));
}

// --- devices ---

static std::string simpler(const std::string& relative, const std::string& path, int root = 60, bool filter = false) {
    std::string part = wid("MultiSamplePart", 0,
                           w("KeyRange", v("Min", 0) + v("Max", 127)) + w("VelocityRange", v("Min", 1) + v("Max", 127)) +
                               v("RootKey", root) + v("Detune", 10) + v("Volume", 0.5) + v("SampleStart", 4410) +
                               v("SampleEnd", 39690) + w("SustainLoop", v("Mode", 1) + v("Start", 8820) + v("End", 17640)) +
                               vb("IsActive", true) + sampleRef(relative, path));
    std::string f = filter ? w("Filter", mb("IsOn", true) + w("Slot", w("Value", w("SimplerFilter", m("Type", 0) + m("Freq", 2000) + m("Res", 0.2)))))
                           : "";
    return wid("OriginalSimpler", 1,
               mb("On", true) + w("Player", w("MultiSampleMap", w("SampleParts", part) + vb("RoundRobin", false))) +
                   w("VolumeAndPan", m("Volume", -6) + m("VolumeVelScale", 0.25) +
                                         w("Envelope", m("AttackTime", 2) + m("DecayTime", 300) + m("SustainLevel", 0.5) +
                                                           m("ReleaseTime", 100))) +
                   w("Pitch", m("TransposeKey", 2) + m("TransposeFine", 50)) + f + w("Globals", v("NumVoices", 3)));
}

static std::string effectsChain() {
    auto band = [](int i, bool on, int mode, double freq, double gain, double q) {
        return w("Bands." + std::to_string(i), w("ParameterA", mb("IsOn", on) + m("Mode", mode) + m("Freq", freq) + m("Gain", gain) + m("Q", q)));
    };
    std::string eq = wid("Eq8", 1, mb("On", true) + m("GlobalGain", 2) + band(0, true, 1, 80, 0, 0.7) + band(1, true, 2, 20, 4, 0.7) +
                                       band(2, true, 3, 1000, 3, 1) + band(3, true, 3, 5000, -6, 2) + band(4, true, 5, 20000, -2, 0.7) +
                                       band(5, true, 7, 18000, 0, 0.7) + band(6, false, 3, 300, 12, 1));
    std::string comp = wid("Compressor2", 2, mb("On", true) + m("Threshold", 0.25) + m("Ratio", 4) + m("Attack", 5) + m("Release", 100) +
                                                 m("Knee", 6) + m("Gain", 3) + m("DryWet", 0.8));
    std::string glue = wid("GlueCompressor", 3, mb("On", true) + m("Threshold", -20) + m("Ratio", 1) + m("Attack", 2) + m("Release", 3) +
                                                    m("Makeup", 2) + m("DryWet", 1));
    std::string delayFx = wid("Delay", 4, mb("On", true) + m("DelayLine_TimeL", 0.3) + m("DelayLine_TimeR", 0.45) + m("Feedback", 0.5) +
                                              m("DryWet", 0.3) + mb("DelayLine_SyncL", true) + m("DelayLine_SyncedSixteenthL", 3) +
                                              mb("DelayLine_PingPong", true));
    std::string echo = wid("Echo", 5, mb("On", true) + m("Delay_TimeL", 0.2) + m("Delay_TimeR", 0.2) + m("Feedback", 0.3) + m("DryWet", 0.4));
    std::string sat = wid("Saturator", 6, mb("On", true) + m("BaseDrive", 9) + m("DryWet", 0.5) + m("PostDrive", -3));
    std::string rack = wid("AudioEffectGroupDevice", 7,
                           mb("On", true) +
                               w("Branches", wid("AudioEffectBranch", 0, w("DeviceChain", w("AudioToAudioDeviceChain", w("Devices", sat)))) +
                                                 wid("AudioEffectBranch", 1, w("DeviceChain", w("AudioToAudioDeviceChain",
                                                                                                w("Devices", wid("Limiter", 8, mb("On", true))))))));
    std::string chorus = wid("Chorus2", 9, mb("On", true) + m("Rate", 1.5) + m("Amount", 0.5) + m("DryWet", 0.4) + m("Width", 0.8));
    std::string limiter = wid("Limiter", 10, mb("On", true) + m("Gain", 2) + m("Ceiling", -1) + m("Release", 100));
    std::string multi = wid("MultibandDynamics", 11, mb("On", true) + m("SplitLowMid", 150) + m("SplitMidHigh", 4000) + m("OutputGain", 1));
    std::string gate = wid("Gate", 12, mb("On", true) + m("Threshold", -50) + m("Attack", 1) + m("Hold", 20) + m("Release", 200));
    std::string redux = wid("Redux2", 13, mb("On", true) + m("SampleRate", 12000) + m("BitDepth", 6.4) + m("DryWet", 0.7));
    std::string unknown = wid("AutoFilter", 14, mb("On", true));
    std::string off = wid("Reverb", 15, mb("On", false));
    return eq + comp + glue + delayFx + echo + rack + chorus + limiter + multi + gate + redux + unknown + off;
}

static Json stateOf(const SlotPtr& slot) {
    std::string component, controller;
    CHECK(plug::unpackState(slot->state, component, controller));
    return Json::parse(component);
}

static bool reportHas(const ableton::Result& r, const std::string& text) {
    for (auto& line : r.report)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

static ClipPtr clipNamed(const Project& p, const std::string& name) {
    for (auto& c : *p.clips)
        if (c->name == name) return c;
    return nullptr;
}
static TrackPtr trackNamed(const Project& p, const std::string& name) {
    for (auto& t : *p.tracks)
        if (t->name == name) return t;
    return nullptr;
}

// --- the big set ---

static void testFullSet() {
    std::string dir = kDir + "/Song Project";
    writeSample(dir + "/Samples/Imported/Kick.wav", 1.0, 60, 0.8);
    writeSample(dir + "/Samples/Imported/Loop.wav", 2.2, 220, 0.4, 48000);
    writeSample(dir + "/Samples/Imported/Piano C4.wav", 1.0, 261.6, 0.5);
    writeSample(dir + "/Samples/Recorded/Snare.wav", 0.5, 200, 0.6);
    writeSample(dir + "/Other/Snare.wav", 0.25, 300, 0.6);

    std::string tracks;
    // the group "Band" (muted, folded, an effect of its own) with "Inner" inside it
    tracks += groupTrack(50, "Band", -1, false, mixer(0.5, 0.25, false, true), wid("Saturator", 1, mb("On", true)));
    tracks += groupTrack(51, "Inner", 50, true, mixer(1.0, 0.0, true, false), "");
    // audio: plain, warped, transposed, looping, deactivated clips; effects
    AClip kick;
    kick.name = "Kick";
    kick.start = 4;
    kick.end = 5;
    kick.loopStart = 0.25;
    kick.loopEnd = 0.75;
    kick.relative = "Samples/Imported/Kick.wav";
    kick.path = "C:/Users/me/Music/Song Project/Samples/Imported/Kick.wav";
    kick.volume = 0.5;
    kick.fadeIn = 0.01;
    kick.fadeOut = 0.02;
    AClip warped;
    warped.name = "Warped";
    warped.start = 8;
    warped.end = 12;
    warped.loopStart = 0;
    warped.loopEnd = 4;
    warped.relative = "Samples/Imported/Loop.wav";
    warped.path = "/old/Loop.wav";
    warped.warped = true;
    warped.markers = {{0, 0}, {4, 2.2}};
    warped.fadeIn = 0.5;  // (beats: warped)
    AClip tape;
    tape.name = "Tape";
    tape.start = 12;
    tape.end = 13;
    tape.loopStart = 0;
    tape.loopEnd = 1;
    tape.relative = "Samples/Imported/Kick.wav";
    tape.path = "Kick.wav";
    tape.pitchCoarse = 12;
    AClip looping;
    looping.name = "Looping";
    looping.start = 16;
    looping.end = 20;  // (2 s of a 1 s sample)
    looping.loopStart = 0;
    looping.loopEnd = 0.5;
    looping.loopOn = true;
    looping.relative = "Samples/Imported/Kick.wav";
    looping.path = "Kick.wav";
    AClip off = kick;
    off.name = "Off";
    off.start = 20;
    off.end = 21;
    off.disabled = true;
    AClip early = kick;  // starts before its file: starts where the sound does
    early.name = "Early";
    early.start = 24;
    early.end = 26;
    early.loopStart = -0.5;
    early.loopEnd = 1;
    early.volume = 1;
    early.fadeIn = early.fadeOut = 0;
    AClip leftover = kick;
    leftover.name = "Leftover";
    leftover.end = leftover.start;  // (no length: left out)
    tracks += track("AudioTrack", 10, "Drums", 3, 51, mixer(0.5, -0.5, true, false, {0.5, 0.0003}),
                    audioClip(kick) + audioClip(warped) + audioClip(tape) + audioClip(looping) + audioClip(off) + audioClip(early) +
                        audioClip(leftover),
                    effectsChain());
    // samples found by name: the one whose folders match the set's path
    AClip snare;
    snare.name = "Snare";
    snare.start = 0;
    snare.end = 1;
    snare.relative = "../Elsewhere/Snare.wav";
    snare.path = "C:\\Users\\me\\Music\\Song Project\\Samples\\Recorded\\Snare.wav";
    AClip other = snare;
    other.name = "Other snare";
    other.start = 2;
    other.end = 3;
    other.path = "D:/Stuff/Other/Snare.wav";
    tracks += track("AudioTrack", 11, "Snares", 200, 50, mixer(1.0, 0.0, false, true), audioClip(snare) + audioClip(other), "");
    // MIDI: a one-sample Simpler with MIDI effects in front
    MClip notes;
    notes.name = "Chords";
    notes.notes = {{0, 1, 60, 100}, {1, 1, 64, 90.6}, {2, 1, 67, 80, false}, {6, 4, 72, 70}};
    MClip loop;
    loop.name = "Loop";
    loop.start = 8;
    loop.end = 14;
    loop.loopStart = 0;
    loop.loopEnd = 2;
    loop.loopOn = true;
    loop.notes = {{0, 1, 48, 100}, {1.5, 1, 50, 100}};
    MClip dead;
    dead.name = "Dead";
    dead.start = 16;
    dead.end = 18;
    dead.loopEnd = 2;
    dead.disabled = true;
    dead.notes = {{0, 1, 40, 100}};
    std::string midiFx = wid("MidiArpeggiator", 1, mb("On", true) + m("Mode", 1) + mb("SyncState", true) + m("SyncedRate", 5) + m("Gate", 50)) +
                         wid("MidiNoteLength", 2, mb("On", true) + mb("Mode", false) + m("TimeLength", 250) + m("Gate", 100)) +
                         wid("MidiScale", 3, mb("On", true) + m("Base", 14) + m("InternalScale", 1) + mb("UseCurrentScale", true)) +
                         wid("MidiScale", 4, mb("On", true) + m("Base", 0) + m("InternalScale", 0)) +  // (identity: left out)
                         wid("MidiChord", 5, mb("On", true)) + wid("MidiRandom", 6, mb("On", false));
    tracks += track("MidiTrack", 20, "Keys", 12, -1, mixer(1.0, 0.3, true, false, {0.25}), midiClip(notes) + midiClip(loop) + midiClip(dead),
                    midiFx + simpler("Samples/Imported/Piano C4.wav", "Piano C4.wav", 60, true));
    // a Drum Rack: pad C1 (36) plays Kick; a switched-off pad is left out
    std::string pad = wid("DrumBranch", 0, w("BranchInfo", v("ReceivingNote", 92)) +
                                               w("DeviceChain", mixer() + w("MidiToAudioDeviceChain", w("Devices",
                                                   simpler("Samples/Imported/Kick.wav", "Kick.wav")))));
    std::string mutedPad = wid("DrumBranch", 1, w("BranchInfo", v("ReceivingNote", 91)) +
                                                    w("DeviceChain", w("Mixer", m("Speaker", 0) + mb("Speaker", false)) +
                                                                         w("MidiToAudioDeviceChain", w("Devices",
                                                                             simpler("Samples/Imported/Loop.wav", "Loop.wav")))));
    MClip beat;
    beat.name = "Beat";
    beat.end = 4;
    beat.loopEnd = 4;
    beat.notes = {{0, 0.25, 36, 127}, {1, 0.25, 36, 100}, {2, 0.25, 37, 100}};
    tracks += track("MidiTrack", 21, "Drum Rack", 5, -1, mixer(), midiClip(beat),
                    wid("DrumGroupDevice", 1, mb("On", true) + w("Branches", pad + mutedPad)));
    // synths: Drift, Operator, Meld, and none at all
    MClip tune;
    tune.name = "Tune";
    tune.end = 2;
    tune.loopEnd = 2;
    tune.notes = {{0, 0.5, 57, 100}, {1, 0.5, 59, 100}};
    tracks += track("MidiTrack", 22, "Lead/Pad", 30, -1, mixer(), midiClip(tune),
                    wid("Drift", 1, mb("On", true) + m("Oscillator1_Type", 2) + m("Oscillator1_Transpose", 0) + m("Oscillator2_Type", 0) +
                                        m("Oscillator2_Transpose", -1) + m("Oscillator2_Detune", 0.1) + m("Filter_Frequency", 3000) +
                                        m("Filter_ModAmount1", 0.3) + m("Envelope1_Attack", 0.01) + m("Envelope1_Decay", 0.3) +
                                        m("Envelope1_Sustain", 0.7) + m("Envelope1_Release", 0.2) + mb("Mixer_NoiseOn", true) +
                                        m("Mixer_NoiseLevel", 0.1) + mb("Global_Legato", false)));
    tracks += track("MidiTrack", 23, "Lead/Pad", 31, -1, mixer(), midiClip(tune),
                    wid("Operator", 1, mb("On", true) + w("Globals", m("Transpose", -12)) + w("Operator.0", w("Envelope", m("AttackTime", 5) + m("DecayTime", 400) + m("SustainLevel", 0.2) + m("ReleaseTime", 200)))));
    tracks += track("MidiTrack", 24, "Meld", 32, -1, mixer(), midiClip(tune),
                    wid("InstrumentMeld", 1, mb("On", true) + m("MeldVoice_Drive", 0.2) + m("MeldVoice_EngineB_Filter_Frequency", 50)));
    tracks += track("MidiTrack", 25, "Nothing", 33, -1, mixer(), midiClip(tune), "");
    MClip empty;
    empty.name = "Empty";
    tracks += track("MidiTrack", 26, "Silent", 34, -1, mixer(), midiClip(empty), "");
    // returns
    tracks += track("ReturnTrack", 30, "A Reverb", 40, -1, mixer(0.7, 0.1),  "",
                    wid("Reverb", 1, mb("On", true) + m("DecayTime", 3000) + m("PreDelay", 20) + mb("ShelfHighOn", true) +
                                         m("StereoSeparation", 80) + m("MixDirect", 1)));
    tracks += track("ReturnTrack", 31, "B Delay", 41, -1, mixer(), "", "");

    std::string als = dir + "/Song.als";
    writeSet(als, liveSet(tracks, 120, 201, {{8, "Chorus"}, {16, ""}}));

    std::string output = kDir + "/out/Song (Tiny DAW)/Song.json";
    Project project;
    ableton::Result result;
    std::string error;
    std::vector<std::pair<std::string, double>> steps;
    bool ok = ableton::convert(als, output, project, result, error,
                               [&](const std::string& text, double share) { steps.push_back({text, share}); });
    if (!ok) fprintf(stderr, "convert failed: %s\n", error.c_str());
    CHECK(ok);
    if (!ok) return;
    for (auto& line : result.report) printf("  %s\n", line.c_str());
    CHECK(!steps.empty() && steps.front().first == "Reading the set" && steps.back().first == "Done" && steps.back().second == 1.0);
    bool rendering = false;
    for (auto& s : steps) rendering = rendering || s.first.find("Rendering Keys (Simpler, from its samples)") == 0;
    CHECK(rendering);

    // the summary
    CHECK(!result.report.empty());
    CHECK(result.report[0].find("Song.als: 120 BPM, 4/4; 9 tracks, 2 returns, ") == 0);
    CHECK(result.report[0].find("4 frozen") == std::string::npos);
    CHECK(result.report[0].find(" frozen, 2 markers; master volume ") != std::string::npos);
    CHECK(result.report[0].find("dB (so the mix peaks under -1 dBFS)") != std::string::npos);
    CHECK(result.masterDb <= 0);

    // timing, markers
    CHECK(project.timing.bpm == 120);
    CHECK(project.timing.beatsPerBar == 4);
    CHECK(project.markers.size() == 2);
    CHECK(project.markers.size() == 2 && project.markers[0].seconds == 4.0 && project.markers[0].name == "Chorus");
    CHECK(project.markers.size() == 2 && project.markers[1].seconds == 8.0 && project.markers[1].name == "Locator");

    // tracks: 9 tracks then the 2 returns
    CHECK(project.tracks.size() == 11);
    auto returns = project.returns();
    CHECK(returns.size() == 2 && returns[0]->name == "A Reverb" && returns[0]->kind == Kind::Return);
    CHECK(project.tracks[9]->kind == Kind::Return && project.tracks[10]->kind == Kind::Return);
    TrackPtr drums = trackNamed(project, "Drums");
    CHECK(drums && drums->kind == Kind::Audio);
    if (!drums) return;
    CHECK(drums->color == "#f7f47c");  // (palette 3)
    CHECK_NEAR(drums->volumeDb, -6.0, 1e-9);  // its own fader (its groups' are their buses')
    CHECK_NEAR(drums->pan, -0.5, 1e-9);
    CHECK(!drums->muted);  // (its outer group is muted: that mutes it, as a group)
    CHECK(Project::mutedIn(*drums));
    CHECK(drums->sends.size() == 1);            // (the second send is below -60 dB)
    CHECK(drums->sends.size() == 1 && drums->sends[0].track == returns[0] && drums->sends[0].db == -6.0);
    // groups: Drums is in "Inner", which is inside "Band" (muted, soloed, folded, a Saturator on its bus)
    CHECK(drums->group && drums->group->name == "Inner");
    CHECK(drums->group && drums->group->folded == false && !drums->group->muted && !drums->group->soloed);
    CHECK(drums->group && drums->group->bus->volumeDb == 0 && drums->group->bus->pan == 0 && !drums->group->foldOnly);
    CHECK(reportHas(result, "Band: a group bus (its effects, fader, pan, mute and sends)"));
    CHECK(reportHas(result, "Inner: a group bus (its effects, fader, pan, mute and sends)"));
    TrackPtr snares = trackNamed(project, "Snares");
    CHECK(snares && snares->muted && snares->soloed && snares->color.empty());
    CHECK(snares && snares->group && snares->group->name == "Band" && snares->group->muted && snares->group->folded &&
          snares->group->soloed && snares->group->color == "#00bfaf");
    CHECK(snares && snares->volumeDb == 0 && snares->pan == 0);
    CHECK(snares && drums->group != snares->group);
    CHECK(snares && drums->group && drums->group->parent == snares->group);  // (nested as in Live)
    if (snares && snares->group) {
        const Track& band = *snares->group->bus;
        CHECK(band.volumeDb == -6.0 && band.pan == 0.25 && !band.muted && band.name == "Band");
        CHECK(band.effects.size() == 1 && band.effects[0]->name == "Saturation");
        CHECK(routing::target(*drums) == drums->group->bus.get());
        CHECK(routing::target(*drums->group->bus) == &band);
        CHECK(routing::target(band) == nullptr);
    }
    TrackPtr keys = trackNamed(project, "Keys");
    CHECK(keys && keys->kind == Kind::Midi && keys->color == "#e553a0");
    CHECK(keys && keys->pan == 0.3 && keys->volumeDb == 0);
    CHECK(keys && keys->sends.size() == 1 && keys->sends[0].db == -12.0);
    CHECK(returns[0]->volumeDb == -3.1 && returns[0]->pan == 0.1);

    // effects of the audio track: in order, racks opened up (their first chain)
    std::vector<std::string> paths;
    for (auto& s : *drums->effects) paths.push_back(s->path);
    std::vector<std::string> expected = {"builtin:eq", "builtin:compressor", "builtin:compressor", "builtin:delay", "builtin:delay",
                                         "builtin:saturation", "builtin:chorus", "builtin:limiter", "builtin:multiband_compressor",
                                         "builtin:gate", "builtin:bitcrusher"};
    CHECK(paths == expected);
    CHECK(drums->cables.size() == expected.size() + 1);  // (one after the other)
    if (paths == expected) {
        auto fx = *drums->effects;
        CHECK(fx[0]->kind == "effect" && fx[0]->name == "EQ");
        Json eq = stateOf(fx[0]);
        CHECK_NEAR(eq["low_cut"].toDouble(), 80, 1e-6);
        CHECK_NEAR(eq["low_freq"].toDouble(), 30, 1e-6);
        CHECK_NEAR(eq["low_gain"].toDouble(), 4, 1e-6);
        CHECK_NEAR(eq["mid_freq"].toDouble(), 5000, 1e-6);
        CHECK_NEAR(eq["mid_gain"].toDouble(), -6, 1e-6);
        CHECK_NEAR(eq["mid_q"].toDouble(), 2, 1e-6);
        CHECK_NEAR(eq["high_freq"].toDouble(), 16000, 1e-6);
        CHECK_NEAR(eq["high_gain"].toDouble(), -2, 1e-6);
        CHECK_NEAR(eq["high_cut"].toDouble(), 18000, 1e-6);
        CHECK_NEAR(eq["output"].toDouble(), 2, 1e-6);
        CHECK(reportHas(result, "Drums: EQ Eight has 2 bell bands; the EQ has one, and keeps -6.0 dB at 5000 Hz (left out: +3.0 dB at 1000 Hz)"));
        CHECK(reportHas(result, "Drums: EQ Eight's low shelf 20 Hz \xE2\x86\x92 30 Hz, high shelf 20000 Hz \xE2\x86\x92 16000 Hz (outside the EQ's range)"));
        Json comp = stateOf(fx[1]);
        CHECK_NEAR(comp["threshold"].toDouble(), 20 * std::log10(0.25), 1e-6);
        CHECK_NEAR(comp["ratio"].toDouble(), 4, 1e-6);
        CHECK_NEAR(comp["attack"].toDouble(), 5, 1e-6);
        CHECK_NEAR(comp["release"].toDouble(), 100, 1e-6);
        CHECK_NEAR(comp["makeup"].toDouble(), 3, 1e-6);
        CHECK_NEAR(comp["mix"].toDouble(), 80, 1e-6);
        Json glue = stateOf(fx[2]);
        CHECK_NEAR(glue["threshold"].toDouble(), -20, 1e-6);
        CHECK_NEAR(glue["ratio"].toDouble(), 4, 1e-6);    // (index 1 of 2, 4, 10)
        CHECK_NEAR(glue["attack"].toDouble(), 0.3, 1e-6); // (index 2)
        CHECK_NEAR(glue["release"].toDouble(), 600, 1e-6);
        CHECK_NEAR(glue["makeup"].toDouble(), 2, 1e-6);
        Json del = stateOf(fx[3]);
        CHECK_NEAR(del["left"].toDouble(), 300, 1e-6);
        CHECK_NEAR(del["right"].toDouble(), 450, 1e-6);
        CHECK_NEAR(del["feedback"].toDouble(), 50, 1e-6);
        CHECK_NEAR(del["mix"].toDouble(), 30, 1e-6);
        CHECK_NEAR(del["sync"].toDouble(), 1, 1e-6);
        CHECK_NEAR(del["left_beats"].toDouble(), 3, 1e-6);  // (3 sixteenths = 0.75 beats)
        CHECK_NEAR(del["pingpong"].toDouble(), 1, 1e-6);
        Json echo = stateOf(fx[4]);
        CHECK_NEAR(echo["left"].toDouble(), 200, 1e-6);
        CHECK_NEAR(echo["mix"].toDouble(), 40, 1e-6);
        CHECK_NEAR(echo["sync"].toDouble(), 0, 1e-6);
        Json sat = stateOf(fx[5]);
        CHECK_NEAR(sat["drive"].toDouble(), 9, 1e-6);
        CHECK_NEAR(sat["mix"].toDouble(), 50, 1e-6);
        CHECK_NEAR(sat["output"].toDouble(), -3, 1e-6);
        Json chorus = stateOf(fx[6]);
        CHECK_NEAR(chorus["rate"].toDouble(), 1.5, 1e-6);
        CHECK_NEAR(chorus["depth"].toDouble(), 4, 1e-6);
        CHECK_NEAR(chorus["width"].toDouble(), 80, 1e-6);
        Json lim = stateOf(fx[7]);
        CHECK_NEAR(lim["gain"].toDouble(), 2, 1e-6);
        CHECK_NEAR(lim["ceiling"].toDouble(), -1, 1e-6);
        Json mb = stateOf(fx[8]);
        CHECK_NEAR(mb["low_cross"].toDouble(), 150, 1e-6);
        CHECK_NEAR(mb["high_cross"].toDouble(), 4000, 1e-6);
        CHECK_NEAR(mb["output"].toDouble(), 1, 1e-6);
        Json gate = stateOf(fx[9]);
        CHECK_NEAR(gate["threshold"].toDouble(), -50, 1e-6);
        CHECK_NEAR(gate["hold"].toDouble(), 20, 1e-6);
        CHECK_NEAR(gate["release"].toDouble(), 200, 1e-6);
        Json crush = stateOf(fx[10]);
        CHECK_NEAR(crush["bits"].toDouble(), 6, 1e-6);
        CHECK_NEAR(crush["downsample"].toDouble(), 4, 1e-6);  // (48000 / 12000)
        CHECK_NEAR(crush["mix"].toDouble(), 70, 1e-6);
    }
    CHECK(reportHas(result, "Drums: a rack with 2 parallel chains: only its first is carried over"));
    CHECK(reportHas(result, "Drums: not carried over (no such effect in Tiny DAW): AutoFilter"));
    // the return's reverb
    CHECK(returns[0]->effects.size() == 1 && returns[0]->effects[0]->path == "builtin:reverb");
    if (returns[0]->effects.size() == 1) {
        Json rev = stateOf(returns[0]->effects[0]);
        double feedback = std::pow(10.0, -3 * (1356.0 / 44100) / 3.0);
        CHECK_NEAR(rev["size"].toDouble(), (feedback - 0.7) / 0.28 * 100, 1e-6);
        CHECK_NEAR(rev["predelay"].toDouble(), 20, 1e-6);
        CHECK_NEAR(rev["damping"].toDouble(), 60, 1e-6);
        CHECK_NEAR(rev["width"].toDouble(), 80, 1e-6);
        CHECK_NEAR(rev["mix"].toDouble(), 100, 1e-6);
    }
    CHECK(returns[1]->effects.empty());

    // audio clips
    ClipPtr k = clipNamed(project, "Kick");
    CHECK(k && k->kind == Kind::Audio && k->track == drums);
    if (k) {
        CHECK(k->start == 2 * SR);           // beat 4 at 120 BPM
        CHECK(k->offset == (int64_t)(0.25 * SR));
        CHECK(k->length == SR / 2);          // one beat
        CHECK(k->gainDb == -6.02);
        CHECK(k->fadeIn == (int64_t)(0.01 * SR) && k->fadeOut == (int64_t)(0.02 * SR));
        CHECK(k->color == "#cc9927");
        CHECK(k->stretch == 1.0 && k->pitch == 0 && !k->loop && !k->disabled && k->sourceBpm == 0);
        CHECK(k->sourceSr == 44100);
        CHECK(util::dirName(k->sourcePath) == project_io::audioFolder(output));
    }
    ClipPtr wp = clipNamed(project, "Warped");
    CHECK(wp != nullptr);
    if (wp) {
        CHECK_NEAR(wp->stretch, 0.5 / 0.55, 1e-9);
        CHECK_NEAR(wp->sourceBpm, 120 * 0.5 / 0.55, 1e-6);
        CHECK(wp->start == 4 * SR);
        CHECK(wp->offset == 0);
        CHECK(wp->fadeIn == SR / 4);  // half a beat
        CHECK(wp->pitch == 0);
        CHECK(wp->length > 0 && wp->length <= 2 * SR);
    }
    ClipPtr tp = clipNamed(project, "Tape");
    CHECK(tp && tp->stretch == 0.5 && tp->pitch == 12);
    ClipPtr lp = clipNamed(project, "Looping");
    CHECK(lp && lp->loop && lp->loopStart == 0 && lp->loopLength == 0.5);
    if (lp) {
        CHECK(lp->start == 8 * SR);
        CHECK(lp->length == 2 * SR);  // (longer than its sample: it repeats)
        CHECK(lp->data && lp->data->frames() >= 2 * SR);
    }
    ClipPtr offClip = clipNamed(project, "Off");
    CHECK(offClip && offClip->disabled);
    ClipPtr earlyClip = clipNamed(project, "Early");
    CHECK(earlyClip && earlyClip->start == (int64_t)(12.5 * SR) && earlyClip->offset == 0 && earlyClip->length == SR / 2);
    CHECK(clipNamed(project, "Leftover") == nullptr);
    // samples by name: the one in the folders matching the set's path
    ClipPtr sn = clipNamed(project, "Snare"), os = clipNamed(project, "Other snare");
    CHECK(sn && os);
    if (sn && os) {
        CHECK(sn->source->frames() == 22050);  // Samples/Recorded/Snare.wav (0.5 s)
        CHECK(os->source->frames() == 11025);  // Other/Snare.wav (0.25 s)
        CHECK(sn->sourcePath != os->sourcePath);
        CHECK(sn->length == 11025 * SR / 44100 + 0 || sn->length > 0);
        CHECK(os->length <= (int64_t)(0.25 * SR) + 1);  // (its window kept inside its audio)
    }
    // the one sample copied once for all its clips
    CHECK(k && tp && lp && k->sourcePath == tp->sourcePath && k->source == lp->source);

    // MIDI clips
    ClipPtr ch = clipNamed(project, "Chords");
    CHECK(ch && ch->kind == Kind::Midi && ch->track == keys);
    if (ch) {
        CHECK(ch->start == 0 && ch->length == 4 * SR && ch->lengthS == 4.0);
        CHECK(ch->notes && ch->notes->size() == 3);
        if (ch->notes && ch->notes->size() == 3) {
            auto& n = *ch->notes;
            CHECK(n[0].start == 0 && n[0].end == 0.5 && n[0].pitch == 60 && n[0].velocity == 100);
            CHECK(n[1].start == 0.5 && n[1].velocity == 91);
            CHECK(n[2].start == 3.0 && n[2].end == 4.0 && n[2].pitch == 72);  // (cut at the clip's end)
        }
        CHECK(ch->mutedNotes && ch->mutedNotes->size() == 1 && (*ch->mutedNotes)[0].pitch == 67);
        CHECK(ch->color == "#5cffe8");
    }
    ClipPtr lc = clipNamed(project, "Loop");
    CHECK(lc != nullptr);
    if (lc && lc->notes) {
        CHECK(lc->start == 4 * SR && lc->lengthS == 3.0);
        std::vector<double> starts, ends;
        for (auto& n : *lc->notes) {
            starts.push_back(n.start);
            ends.push_back(n.end);
        }
        CHECK((starts == std::vector<double>{0, 0.75, 1, 1.75, 2, 2.75}));
        CHECK(ends.size() == 6 && ends[1] == 1.25 && ends[5] == 3.0);  // (the last cut at the clip's end)
    }
    ClipPtr dc = clipNamed(project, "Dead");
    CHECK(dc && dc->disabled);
    // MIDI effects: Arpeggiator, Note Length, Scale (the identity one left out); Chord reported
    CHECK(keys && keys->midiEffects.size() == 3);
    if (keys && keys->midiEffects.size() == 3) {
        auto& fx = *keys->midiEffects;
        CHECK(fx[0].kind == "Arpeggiator" && fx[0].get("style") == 1 && fx[0].get("rate") == 3 && fx[0].get("gate") == 50);
        CHECK(fx[1].kind == "Note Length" && fx[1].get("mode") == 0 && fx[1].get("beats") == 0.5);
        CHECK(fx[2].kind == "Scale" && fx[2].get("root") == 2 && fx[2].get("scale") == 1);
    }
    CHECK(reportHas(result, "Keys: MIDI effect Chord not carried over"));
    CHECK(!reportHas(result, "MIDI effect Random"));
    // the one-sample Simpler: the built-in sampler
    CHECK(keys && keys->sampler);
    if (keys && keys->sampler) {
        const SamplerSettings& s = *keys->sampler;
        CHECK(s.sample && s.sample->source && s.sample->source->frames() == 44100);
        CHECK(s.root == 60);
        CHECK_NEAR(s.transpose, 2.5 + 0.1, 1e-9);
        CHECK_NEAR(s.gainDb, -6 + 20 * std::log10(0.5), 1e-9);
        CHECK(s.velocity == 0.25);
        CHECK_NEAR(s.attack, 0.002, 1e-12);
        CHECK_NEAR(s.decay, 0.3, 1e-12);
        CHECK(s.sustain == 0.5);
        CHECK_NEAR(s.release, 0.1, 1e-12);
        CHECK(s.loop);
        CHECK_NEAR(s.start, 0.1, 1e-9);
        CHECK_NEAR(s.end, 0.9, 1e-9);
        CHECK(util::dirName(s.sample->sourcePath) == project_io::audioFolder(output));
    }
    // frozen renderings
    CHECK(keys && keys->frozen && keys->frozen->source && keys->frozen->source->frames() > 0);
    CHECK(reportHas(result, "Keys: Simpler, from its samples; frozen (as rendered here, without its effects); unfrozen it plays the built-in sampler with its sample"));
    TrackPtr drumRack = trackNamed(project, "Drum Rack");
    CHECK(drumRack && drumRack->frozen && !drumRack->sampler);
    CHECK(reportHas(result, "Drum Rack: Drum Rack, from its samples; frozen (as rendered here, without its effects); unfrozen it plays the built-in synth (choose an instrument for it)"));
    if (drumRack && drumRack->frozen) {
        // the pad plays the kick at beats 0 and 1; pad 37 (switched off) is silent
        const AudioBuffer& a = *drumRack->frozen->data;
        CHECK(drumRack->frozen->start == 0);
        double before = 0, at = 0, after = 0;
        for (int64_t i = 0; i < a.frames(); i++) {
            double x = std::fabs(a.frame(i)[0]);
            if (i < SR / 2) before = std::max(before, x);
            else if (i < SR / 2 + SR / 10) at = std::max(at, x);
            else if (i >= SR && i < SR + 2 * SR / 5) after = std::max(after, x);
        }
        CHECK(before > 0.01 && at > 0.01);
        CHECK(after < 1e-4);
    }
    int leads = 0;
    for (auto& t : *project.tracks)
        if (t->name == "Lead/Pad") {
            leads++;
            CHECK(t->frozen && t->frozen->sourcePath.find("Lead_Pad (frozen") != std::string::npos);
        }
    CHECK(leads == 2);
    auto lp1 = std::vector<TrackPtr>();
    for (auto& t : *project.tracks)
        if (t->name == "Lead/Pad") lp1.push_back(t);
    CHECK(lp1.size() == 2 && lp1[0]->frozen && lp1[1]->frozen && lp1[0]->frozen->sourcePath != lp1[1]->frozen->sourcePath);
    CHECK(reportHas(result, "Lead/Pad: Drift, approximated; frozen"));
    CHECK(reportHas(result, "Lead/Pad: Operator, approximated; frozen"));
    CHECK(reportHas(result, "Meld: Meld, approximated; frozen"));
    CHECK(reportHas(result, "Nothing: unknown instrument: a plain synth; frozen"));
    TrackPtr silent = trackNamed(project, "Silent");
    CHECK(silent && !silent->frozen);  // (no notes: nothing rendered, nothing reported)
    CHECK(!reportHas(result, "Silent:"));
    // the renderings are brought to the loudest audio clip's level less 6 dB: none is far louder than full scale
    for (auto& t : *project.tracks)
        if (t->frozen) {
            float peak = 0;
            for (float x : t->frozen->source->samples) peak = std::max(peak, std::fabs(x));
            CHECK(peak > 0.001f);
            CHECK(peak < 4.0f);
        }

    // saved: it opens as it was
    CHECK(util::fileExists(output));
    Project loaded;
    project_io::MasterSettings master;
    CHECK(project_io::loadProject(output, loaded, SR, master, error));
    CHECK(master.volumeDb == result.masterDb && !master.mute);
    CHECK(loaded.tracks.size() == 11);
    CHECK(loaded.clips.size() == project.clips.size());
    CHECK(loaded.timing.bpm == 120);
    CHECK(loaded.markers.size() == 2);
    TrackPtr lkeys = trackNamed(loaded, "Keys");
    CHECK(lkeys && lkeys->frozen && lkeys->sampler && lkeys->midiEffects.size() == 3);
    TrackPtr ldrums = trackNamed(loaded, "Drums");
    CHECK(ldrums && ldrums->effects.size() == 11 && ldrums->sends.size() == 1 && ldrums->group && ldrums->group->name == "Inner");
    CHECK(ldrums && ldrums->group && ldrums->group->parent && ldrums->group->parent->name == "Band" &&
          ldrums->group->parent->bus->effects.size() == 1 && ldrums->group->parent->bus->volumeDb == -6.0 &&
          ldrums->group->parent->muted);  // (the buses saved and loaded)
    ClipPtr lch = clipNamed(loaded, "Chords");
    CHECK(lch && lch->mutedNotes && lch->mutedNotes->size() == 1);
    ClipPtr llp = clipNamed(loaded, "Looping");
    CHECK(llp && llp->loop && llp->loopLength == 0.5);
    ClipPtr lwp = clipNamed(loaded, "Warped");
    CHECK(lwp && std::fabs(lwp->stretch - 0.5 / 0.55) < 1e-9);

    // importing again into the same place: refused (its audio folder holds files)
    Project again;
    ableton::Result r2;
    error.clear();
    CHECK(!ableton::convert(als, output, again, r2, error));
    CHECK(error.find("isn't empty: import into a new place (or empty it first)") != std::string::npos);
}

static void testSmallSets() {
    std::string dir = kDir + "/Small";
    writeSample(dir + "/Hit.wav", 0.5, 100, 0.5);
    // 7/8 at 100 BPM; an audio clip (seconds at 0.6 s per beat), MIDI with no instrument
    AClip hit;
    hit.name = "Hit";
    hit.start = 2;
    hit.end = 3;
    hit.loopEnd = 1;
    hit.relative = "Hit.wav";
    hit.path = "Hit.wav";
    std::string set = liveSet(track("AudioTrack", 1, "A", 0, -1, mixer(), audioClip(hit), ""), 100, 6 + 99 * 3);
    writeSet(dir + "/Seven.als", set);
    Project p;
    ableton::Result r;
    std::string error;
    CHECK(ableton::convert(dir + "/Seven.als", kDir + "/out/Seven/Seven.json", p, r, error));
    CHECK(p.timing.bpm == 100 && p.timing.beatsPerBar == 7 && p.timing.beatUnit == 8);  // (the real denominator)
    CHECK(p.timing.barBeat(1) == 3.5);  // (a bar of 7/8: three and a half quarter notes)
    CHECK(r.report.size() >= 1 && r.report[0].find("Seven.als: 100 BPM, 7/8; 1 tracks, 0 returns, 1 clips, 0 frozen, 0 markers") == 0);
    ClipPtr c = clipNamed(p, "Hit");
    CHECK(c && c->start == (int64_t)(1.2 * SR) && c->length == (int64_t)(0.5 * SR));
    CHECK(r.masterDb <= 0);
    // the time signature's automation (an envelope on the main track's TimeSignature): every change on the lane; a
    // value out of range is 4/4, one inside a bar goes to the nearest bar line. With a tempo ramp running through them
    {
        auto replace = [](std::string& s, const std::string& from, const std::string& to) {
            size_t at = s.find(from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) s.replace(at, from.size(), to);
        };
        auto envelope = [](int id, const std::string& events) {
            return wid("AutomationEnvelope", id, w("EnvelopeTarget", v("PointeeId", id)) + w("Automation", w("Events", events)));
        };
        auto e = [](const std::string& tag, double time, double value) {
            return "<" + tag + " Time=\"" + num(time) + "\" Value=\"" + num(value) + "\" />";
        };
        std::string sigSet = liveSet("", 100);
        replace(sigSet, "<TimeSignature>", "<TimeSignature><AutomationTarget Id=\"8\" />");
        replace(sigSet, "<Tempo><Manual Value=\"100\" /></Tempo>", "<Tempo><Manual Value=\"100\" /><AutomationTarget Id=\"9\" /></Tempo>");
        replace(sigSet, "</MainTrack>",
                w("AutomationEnvelopes", w("Envelopes",
                    envelope(8, e("EnumEvent", -63072000, 2 + 99 * 2) + e("EnumEvent", 12, 6 + 99 * 3) + e("EnumEvent", 15.5, 9999) +
                                    e("EnumEvent", 23.5, 2 + 99 * 2) + e("EnumEvent", 25, 4 + 99 * 2)) +
                    envelope(9, e("FloatEvent", -63072000, 100) + e("FloatEvent", 6, 100) + e("FloatEvent", 18, 160)))) +
                    "</MainTrack>");
        writeSet(dir + "/Sigs.als", sigSet);
        Project sp;
        ableton::Result sr;
        CHECK(ableton::convert(dir + "/Sigs.als", kDir + "/out/Sigs/Sigs.json", sp, sr, error));
        const Timing& t = sp.timing;
        CHECK(t.sigAtBar(1) == TimeSig({3, 4}) && t.sigAtBar(4) == TimeSig({3, 4}) && t.sigAtBar(5) == TimeSig({7, 8}));
        CHECK(t.sigAtBar(6) == TimeSig({4, 4}) && t.sigAtBar(7) == TimeSig({4, 4}) && t.sigAtBar(8) == TimeSig({3, 4}));
        CHECK(t.sigAtBar(9) == TimeSig({5, 4}));  // (beat 25: halfway through bar 8, so bar 9)
        TempoMapPtr tm = t.map();
        CHECK(tm->barBeat(4) == 12 && tm->barBeat(5) == 15.5 && tm->barBeat(8) == 26.5);
        CHECK_NEAR(tm->bpmAtBeat(3), 100, 1e-9);
        CHECK_NEAR(tm->bpmAtBeat(9), 115, 1e-6);    // (the ramp, 6 → 18: 100 → 160, through bars 5 and 6)
        CHECK_NEAR(tm->bpmAtBeat(12), 130, 1e-6);
        CHECK_NEAR(tm->bpmAtBeat(15.5), 147.5, 1e-6);
        CHECK_NEAR(tm->bpmAtBeat(17), 155, 1e-6);
        CHECK_NEAR(tm->bpmAtBeat(30), 160, 1e-9);
        CHECK(t.changeAt(3, 0) && t.changeAt(3, 0)->ramp && t.changeAt(6, 2.5) && !t.changeAt(6, 2.5)->ramp);
        CHECK(reportHas(sr, "as 2 tempo changes"));
    }

    // a loud mix: the master volume brings its peak under -1 dBFS (whole dB)
    writeSample(dir + "/Loud.wav", 0.5, 100, 1.0);
    AClip loud = hit;
    loud.name = "Loud";
    loud.relative = "Loud.wav";
    loud.path = "Loud.wav";
    writeSet(dir + "/Loud.als", liveSet(track("AudioTrack", 1, "A", 0, -1, mixer(2.0), audioClip(loud), ""), 120));
    Project l;
    CHECK(ableton::convert(dir + "/Loud.als", kDir + "/out/Loud/Loud.json", l, r, error));
    double peak = 0;
    ClipPtr loudClip = clipNamed(l, "Loud");
    CHECK(loudClip != nullptr);
    if (loudClip)
        for (float x : loudClip->source->samples) peak = std::max(peak, (double)std::fabs(x));
    double mixPeakDb = 20 * std::log10(peak) + 20 * std::log10(2.0);
    printf("loud mix: %.2f dBFS before the master, master %g dB\n", mixPeakDb, r.masterDb);
    CHECK(r.masterDb == std::floor(-1.0 - mixPeakDb));  // (the sample's peak, through the +6 dB fader)
    CHECK(r.masterDb < -4);

    // an empty set: nothing to measure, master at 0 dB
    writeSet(dir + "/Empty.als", liveSet("", 128));
    Project e;
    CHECK(ableton::convert(dir + "/Empty.als", kDir + "/out/Empty/Empty.json", e, r, error));
    CHECK(e.tracks.empty() && r.masterDb == 0);
    CHECK(r.report.size() == 1 && r.report[0] == "Empty.als: 128 BPM, 4/4; 0 tracks, 0 returns, 0 clips, 0 frozen, 0 markers; master volume 0 dB (so the mix peaks under -1 dBFS)");

    // a sample not in the set's folder
    AClip missing = hit;
    missing.relative = "Samples/Missing.wav";
    missing.path = "C:/Somewhere/Missing.wav";
    writeSet(dir + "/Missing.als", liveSet(track("AudioTrack", 1, "A", 0, -1, mixer(), audioClip(missing), ""), 120));
    error.clear();
    CHECK(!ableton::convert(dir + "/Missing.als", kDir + "/out/Missing/Missing.json", e, r, error));
    CHECK(error == "sample not found in the set's folder: Missing.wav");

    // not gzip; not a Live set; damaged XML
    util::writeFile(dir + "/Plain.als", set);
    error.clear();
    CHECK(!ableton::convert(dir + "/Plain.als", kDir + "/out/Plain/Plain.json", e, r, error));
    CHECK(error == "not a gzip file");
    writeSet(dir + "/Other.als", "<?xml version=\"1.0\"?><Something/>");
    error.clear();
    CHECK(!ableton::convert(dir + "/Other.als", kDir + "/out/Other/Other.json", e, r, error));
    CHECK(error.find("not an Ableton Live set") == 0);
    writeSet(dir + "/Broken.als", "<Ableton><LiveSet></Ableton>");
    error.clear();
    CHECK(!ableton::convert(dir + "/Broken.als", kDir + "/out/Broken/Broken.json", e, r, error));
    CHECK(error.find("the set's XML is damaged") == 0);
    error.clear();
    CHECK(!ableton::convert(dir + "/Nowhere.als", kDir + "/out/Nowhere/Nowhere.json", e, r, error));
    CHECK(!error.empty());

    // Session view clips: left out (no clip launcher), and the report says how many; the arrangement still comes
    MClip arranged;
    arranged.name = "Arranged";
    arranged.notes = {{0, 1, 60, 100}};
    MClip jam = arranged;
    jam.name = "Jam";
    std::string slots = w("ClipSlotList", wid("ClipSlot", 0, w("ClipSlot", w("Value", midiClip(jam)))) +
                                              wid("ClipSlot", 1, w("ClipSlot", w("Value", midiClip(jam)))) +
                                              wid("ClipSlot", 2, w("ClipSlot", "<Value />")));  // (an empty slot)
    std::string withSlots = track("MidiTrack", 1, "M", 0, -1, mixer(), midiClip(arranged), "");
    withSlots = util::replaceAll(withSlots, "<MainSequencer>", "<MainSequencer>" + slots);
    writeSet(dir + "/Session.als", liveSet(withSlots, 120));
    Project s;
    error.clear();
    CHECK(ableton::convert(dir + "/Session.als", kDir + "/out/Session/Session.json", s, r, error));
    CHECK(reportHas(r, "2 Session view clips left out: only the arrangement is imported"));
    CHECK(clipNamed(s, "Arranged") && !clipNamed(s, "Jam"));
    // none: nothing said
    writeSet(dir + "/NoSession.als", liveSet(track("MidiTrack", 1, "M", 0, -1, mixer(), midiClip(arranged), ""), 120));
    Project n;
    CHECK(ableton::convert(dir + "/NoSession.als", kDir + "/out/NoSession/NoSession.json", n, r, error));
    CHECK(!reportHas(r, "Session view clip"));
}

static void testParts() {
    CHECK(ableton::color(0) == "#ff94a6");
    CHECK(ableton::color(69) == "#3c3c3c");
    CHECK(ableton::color(70).empty() && ableton::color(-1).empty());
    std::string dir = kDir + "/parts";
    util::makeDirs(dir);
    CHECK(ableton::frozenFile(dir, "Bass/Sub: \"1\"", ".flac") == dir + "/Bass_Sub_ _1_ (frozen).flac");
    CHECK(ableton::frozenFile(dir, " .. ", ".wav") == dir + "/Track (frozen).wav");
    util::writeFile(dir + "/Pad (frozen).wav", "x");
    util::writeFile(dir + "/Pad (frozen 2).wav", "x");
    CHECK(ableton::frozenFile(dir, "Pad", ".wav") == dir + "/Pad (frozen 3).wav");
    std::vector<std::pair<double, double>> markers = {{0, 0}, {4, 2}, {8, 5}};
    CHECK_NEAR(ableton::warpSeconds(markers, 2), 1, 1e-12);
    CHECK_NEAR(ableton::warpSeconds(markers, 6), 3.5, 1e-12);
    CHECK_NEAR(ableton::warpSeconds(markers, 10), 6.5, 1e-12);  // (past the last: the last line on)
    CHECK_NEAR(ableton::warpSeconds(markers, -2), -1, 1e-12);
    // the envelope: attack, decay to the sustain, release down to -70 dB, then silence
    auto env = ableton::envelope(SR, SR / 2, 0.01, 0.1, 0.5, 0.1);
    CHECK(env.size() == (size_t)SR);
    CHECK(env[0] == 0);
    CHECK_NEAR(env[SR / 100], 1.0, 0.01);
    CHECK_NEAR(env[SR / 4], 0.5, 0.01);
    CHECK(env[SR / 2 + SR / 20] < 0.5 && env[SR / 2 + SR / 20] > 0.001);
    CHECK(env[SR / 2 + SR / 5] == 0);
    // the project proposed for a set: "<stem> (Tiny DAW)/<stem>.json" next to the set's folder when that is there
    CHECK(ableton_import::proposedOutput("/nowhere/Sets/Song.als") == "/nowhere/Song (Tiny DAW).json");
    util::makeDirs(kDir + "/prop/Song (Tiny DAW)");
    CHECK(ableton_import::proposedOutput(kDir + "/prop/Set/Song.als") ==
          util::absolutePath(kDir + "/prop/Song (Tiny DAW)/Song.json"));
}

int main() {
    clearDir(kDir);
    util::makeDirs(kDir);
    testParts();
    testSmallSets();
    testFullSet();
    TEST_MAIN_END();
}
