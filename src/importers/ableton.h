// Converts an Ableton Live set (.als) into a Tiny DAW project, as closely as Tiny DAW allows
// (importers/ableton.py): File → Import Ableton Live set… (ui/ableton_import.h).
//
// What carries over: the tempo and time signature, markers (locators), every track (name, colour, fader,
// pan, mute, solo) and return track with the sends to it, every arrangement clip where it is (MIDI notes,
// switched-off notes too; audio clips with their place in the file, their gain, and warped ones stretched to
// the set's tempo), clip colours. Ableton's Arpeggiator, Note Length and Scale become Tiny DAW's MIDI effects;
// its EQ Eight, Compressor, Glue Compressor, Reverb, Delay, Echo, Saturator, Chorus, Limiter, Multiband
// Dynamics, Gate and Redux become Tiny DAW's built-in effects (on audio and return tracks), set as near as
// their controls allow. What can't be carried over, or only approximately, is listed in the report.
//
// Group tracks become Tiny DAW groups with a bus each (model.h GROUP BUSES): the group's own effects, fader, pan,
// mute and sends, nested as in Live (a track goes in its innermost group). Clips
// deactivated in Live come in disabled. A clip's transpose: warped, the pitch alone; unwarped, like a tape.
// Its fades and loop come along.
//
// Ableton's own instruments can't run in Tiny DAW (it hosts VST plugins, a built-in synth and sampler), so
// each MIDI track keeps its notes and is *frozen* with a rendering of its instrument made here:
//   - Simpler, Sampler and Drum Rack are played from the set's own samples: their key and velocity
//     zones, root keys, transpose, sample start/end, sustain loops, volume envelopes, velocity
//     sensitivity, round-robin, voice count (voice stealing) and filter; MIDI effects in front of them
//     (Arpeggiator, Note Length) are applied to the notes first. A Simpler or Sampler with one sample also
//     becomes the track's built-in sampler, which plays once the track is unfrozen.
//   - Drift, Meld and Operator are approximated from their main settings (oscillator shapes and
//     octaves, filter, envelopes): the same notes, a similar kind of sound, not the same sound.
//   - The audio effects after an instrument are left out of its rendering.
// Because those effects change levels in ways that can't be known here, each rendered instrument is brought
// to INSTRUMENT_DB below the loudest audio clip before its fader; the master volume is then set so the mix as
// Tiny DAW plays it (effects included) peaks no higher than PEAK_DB. Unfreeze a track in Tiny DAW to edit
// its notes or give it a VST instrument instead.
//
// The set's samples must be in its folder (or below it): Ableton keeps the paths they had on its computer,
// so each is looked for there by its path relative to the set, else by its name. The project is saved as a
// new one (its audio folder must be empty or not there yet): the samples are copied into it.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "model/model.h"

namespace ableton {

constexpr int SR = 48000;
constexpr double INSTRUMENT_DB = -6.0;  // each rendered instrument's loudness (before its fader), against the loudest audio clip
constexpr double PEAK_DB = -1.0;        // the master volume keeps the mix's peak at or under this
constexpr double FLOOR_DB = -70.0;      // Ableton's envelopes end at -70 dB
constexpr double SEND_FLOOR_DB = -60.0; // sends quieter than this are left off (unused ones are stored as -70 dB)

// progress(text, 0…1): how far the conversion is ("Reading the set", "Converting Bass"…)
using Progress = std::function<void(const std::string& text, double share)>;

struct Result {
    std::vector<std::string> report;  // [0]: the summary line; then what was approximated or left out
    double masterDb = 0;              // the master volume it was saved with
};

// Converts the set `als` into the Tiny DAW project `output` (a .json in a new place), saved there (project_io)
// at SR. `project` is filled with it as saved (the UI opens the saved file instead). False and `error` (why:
// "sample not found in the set's folder: Kick.wav", "<folder> isn't empty: …", a damaged file…) if it couldn't;
// files written before the failure stay. GUI thread (it prepares clips and renders the mix); MIDI effects
// follow the project's timing while it runs, the current one's again afterwards.
bool convert(const std::string& als, const std::string& output, Project& project, Result& result,
             std::string& error, const Progress& progress = nullptr);

// --- parts (public for tests) ---
// Ableton's 70 clip/track colours by index: "#rrggbb", or "" for an index outside them
std::string color(int index);
// A new file in `folder` for a track's rendering: "<name> (frozen)<ext>", its name made safe for a file (Live
// allows "/" in track names) and never one already there (Live allows two tracks of the same name).
std::string frozenFile(const std::string& folder, const std::string& trackName, const std::string& ext);
// The file's time (s) at `beat` of a warped clip, from its warp markers (beat, seconds), sorted (straight
// lines between them, extended past the ends); at least two markers
double warpSeconds(const std::vector<std::pair<double, double>>& markers, double beat);
// An ADSR over n frames at SR: linear attack, then decay and release straight lines in dB down to FLOOR_DB
// (Ableton's), the note released after `gate` frames
std::vector<float> envelope(int64_t n, int64_t gate, double attackS, double decayS, double sustain, double releaseS);

}  // namespace ableton
