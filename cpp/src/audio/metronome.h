// The metronome: its two click sounds and when they play (metronome.py). No GUI.
//
// One click marks the start of every bar, the other every other beat (quarter note) of the bar,
// following the project's tempo map: its tempo and time signature changes (timing.h, beatTimes;
// the audio thread reads timing.audioMap()). Each click is a short tone shaped
// by an attack-sustain-release envelope: it rises from silence to its level in `attackMs`, stays
// there for `sustainMs`, then falls back to silence in `releaseMs`.
//
// The clicks are rendered once (renderClicks, on the GUI thread, when the settings or the sample
// rate change); the audio thread only copies them in where beats fall (addClicks). Where they
// fall is worked out from the playhead each buffer, so seeking needs nothing special.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "model/timing.h"

namespace metronome {

extern const char* const WAVEFORMS[4];  // "sine", "triangle", "square", "noise"
constexpr double MAX_ATTACK_MS = 100, MAX_SUSTAIN_MS = 200, MAX_RELEASE_MS = 200;  // (the envelope editor shows 500 ms)
constexpr double MIN_DB = -40.0, MAX_DB = 0.0;
constexpr double MIN_PITCH = 100.0, MAX_PITCH = 8000.0;

struct Click {
    double pitch = 1000.0;   // Hz (not used by "noise")
    double levelDb = -8.0;   // sustain level
    double attackMs = 1.0;
    double sustainMs = 10.0;
    double releaseMs = 50.0;
    std::string waveform = "sine";  // one of WAVEFORMS
    bool operator==(const Click& o) const {
        return pitch == o.pitch && levelDb == o.levelDb && attackMs == o.attackMs && sustainMs == o.sustainMs &&
               releaseMs == o.releaseMs && waveform == o.waveform;
    }
};

struct MetronomeSettings {
    Click bar = barDefault();
    Click beat;  // the other beats of the bar
    static Click barDefault() {
        Click c;
        c.pitch = 1600.0, c.levelDb = -4.0, c.sustainMs = 15.0, c.releaseMs = 70.0;
        return c;
    }
    bool operator==(const MetronomeSettings& o) const { return bar == o.bar && beat == o.beat; }
};

// (bar click, beat click) as mono float audio, ready for addClicks
struct Clicks {
    std::vector<float> bar, beat;
};

std::vector<float> renderClick(const Click& click, int sr);  // the click as mono audio at rate sr
Clicks renderClicks(const MetronomeSettings& settings, int sr);

// Add the clicks of the beats that sound in out (interleaved, `channels` per frame; timeline frames
// [start, start + frames)) to its first two outputs, times gain; none for beats before frame
// `earliest`. Audio thread: no allocation (`beats` is scratch; it only grows).
void addClicks(float* out, int channels, int frames, int64_t start, const Clicks& clicks, const TempoMap& map,
               int sr, double gain, int64_t earliest, std::vector<BeatTime>& beats);
// the same with timing.map() (GUI thread: the audio thread passes timing.audioMap())
void addClicks(float* out, int channels, int frames, int64_t start, const Clicks& clicks, const Timing& timing,
               int sr, double gain = 1.0, int64_t earliest = 0);

// The count-in before recording: `bars` bars of clicks (stereo) at the tempo and time signature where
// recording starts (`at` seconds), on the song's beats (its bar lines accented), carried on back from
// there: starting off a beat, the next beat after the count-in comes as counted. Null (frames 0) for none.
AudioPtr countIn(const MetronomeSettings& settings, const Timing& timing, int bars, int sr, double at,
                 int64_t& frames);

}  // namespace metronome
