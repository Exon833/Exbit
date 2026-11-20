// An audio file's tempo, for warping it (audio/warp.h): what the file says first, then what its sound says.
//   1. a WAV file's `acid` chunk (Acidized loops: its tempo field);
//   2. a BPM in its file name: "120bpm", "120 BPM", "bpm120", "loop_120_" (a number of 60-200 between
//      underscores, spaces or dashes);
//   3. analysis: the onset strength of its first minute (dsp::Onsets), its autocorrelation summed over half a
//      beat, a beat and its multiples (2, 3, 4 beats), for every tempo from 60 to 180 BPM in steps of 0.02, those
//      from 80 to 160 preferred (so a 128 BPM loop is taken for neither 64 nor 85); the best one refined from
//      where each multiple's peak really is (to a fraction of the analysis' hop). Metadata comes first because
//      analysis can drift on a trimmed sample (FL Studio's complaint).
#pragma once
#include <string>

#include "audio/dsp.h"

namespace tempo_detect {

constexpr double MIN_BPM = 60, MAX_BPM = 180;   // what analysis looks between…
constexpr double LIKELY_LO = 80, LIKELY_HI = 160;  // …preferring these
constexpr double ANALYSIS_S = 60;               // seconds of the start analysed

struct Found {
    double bpm = 0;    // 0: none
    std::string from;  // "acid chunk", "file name", "analysis"
};

double acidTempo(const std::string& path);   // a WAV file's acid chunk tempo (0: none)
double nameTempo(const std::string& name);   // a BPM in a file name (0: none)
double analyse(const AudioBuffer& audio, int sr);  // the tempo its sound has (0: none found)
// the first of the three that knows (path "" or a missing file: no acid chunk; its name: `name`, or the path's)
Found detect(const std::string& path, const AudioPtr& audio, int sr, const std::string& name = "");

}  // namespace tempo_detect
