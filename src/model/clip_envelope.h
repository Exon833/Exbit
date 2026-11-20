// An audio clip's gain envelope (Clip::gainEnvelope): a volume curve inside the clip, applied with its gain and
// fades before its track's effects (mixer::addClip), so levels can be evened out before a compressor (as FL Studio's
// audio clip envelopes and Pro Tools' clip gain). Automation stays for mixing.
//
// Its points are automation's (AutoPoint: a value 0…1 and a bend per segment; the curve is automation::valueAt's),
// placed on the clip's own audio as it plays: t is a frame of its audio after stretching and reversing but before
// looping (Clip::data before its loop repeats; frames at the engine's rate, saved in seconds). So the envelope stays
// on the same sound when the clip is moved, trimmed, split, copied or duplicated (its pieces share it), and every
// pass of a looping clip has it again; Reverse mirrors it, Stretch scales it (edits.h), and Join bakes it into the
// joined audio. v is the gain: 0 silence, then MIN_DB … MAX_DB (+24 dB) in a straight line of dB.
//
// The timeline (right-click a clip → Gain envelope) draws it as a line over the waveform and edits it like an
// automation lane: click to add a point, drag one, Ctrl+drag a segment to bend it, right-click (or double-click) a
// point to remove it. "Gain envelope points…" lists the points (time, dB) to add, change and remove (the terminal
// UI's way). The clip's title shows "env" while it has one.
//
// The mixer works the gain out every STEP frames of the clip's audio, walking the points forwards (no search per
// frame, no allocation: the audio thread), in a straight line between; Clip::level and Join use the same values,
// so what is drawn, played, rendered and joined agree.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model/model.h"

namespace clip_envelope {

constexpr double MAX_DB = 24;  // the top of the range (v = 1); v = 0 is silence
constexpr int STEP = 32;       // frames between the gains the mixer works out (straight lines between)

using Points = std::shared_ptr<const std::vector<AutoPoint>>;

double dbOf(double v);              // config::MIN_DB (silence at 0) … MAX_DB
double valueOf(double db);          // the value of `db` (at or below MIN_DB: 0), within 0…1
double unity();                     // the value of 0 dB
float gainOf(double v);             // as a factor (0 at v = 0)
std::string dbText(double v);       // "-inf dB", "+3.0 dB"

// points as an envelope: in time order, t ≥ 0, v within 0…1, bends within -1…1; none: null
Points make(std::vector<AutoPoint> points);
// a frame of the clip's data (offset + frames into its window) as a frame of its audio before looping
int64_t audioFrame(const Clip& clip, int64_t frame);
// the frames of the clip's data within [lo, hi) where frame t of its audio is heard (a looping clip: every pass)
std::vector<int64_t> framesOf(const Clip& clip, double t, int64_t lo, int64_t hi);
// the envelope's value (0…1) at a frame of the clip's data (unity() without one)
double valueAt(const Clip& clip, int64_t frame);

// The gain at frames of a clip's data, walked forwards (the audio thread; going back is allowed, it searches once).
struct Walk {
    const Clip& clip;
    const std::vector<AutoPoint>& points;
    size_t cursor = 0;
    int64_t cell = -2;  // the STEP-frame cell whose two ends' gains are g0, g1 (-2: none yet)
    float g0 = 1, g1 = 1;
    float gain(int64_t frame);
    float gainAtStep(int64_t frame);  // (the exact gain at a cell's end)
};

// --- what edits do to it (edits.h) ---
std::vector<AutoPoint> scaled(const std::vector<AutoPoint>& points, double k);         // stretched k times as long
std::vector<AutoPoint> mirrored(const std::vector<AutoPoint>& points, int64_t frames);  // its audio (frames) reversed

}  // namespace clip_envelope
