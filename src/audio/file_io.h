// Reading audio and MIDI files from disk (file_io.py). Nothing here throws: each function returns
// false and says why in `error`.
#pragma once
#include <string>
#include <vector>

#include "audio/dsp.h"
#include "audio/synth.h"

struct Timing;

namespace file_io {

extern const std::vector<std::string> AUDIO_EXTENSIONS;  // ".wav", ".flac", ".ogg", ".mp3", ".aif", ".aiff"
extern const std::vector<std::string> MIDI_EXTENSIONS;   // ".mid", ".midi"

bool isMidi(const std::string& path);
// an audio or MIDI file the program can load (by its extension)
bool isSample(const std::string& path);

// stereo float audio and its sample rate
bool loadAudio(const std::string& path, AudioPtr& data, int& sampleRate, std::string& error);
// the notes (start s, end s, pitch, velocity; sorted) and the length in seconds. All tracks and
// channels of the file are merged, with tempo changes applied.
bool loadMidi(const std::string& path, std::vector<Note>& notes, double& lengthSeconds, std::string& error);
// (notes: start beat, end beat, pitch, velocity; length in beats: the file's end, its last tick) of a MIDI
// file, in beats (quarter notes) rather than seconds: placed with the song's tempo, its notes land on the
// song's beats whatever tempo the file was written at. All tracks and channels merged.
bool loadMidiBeats(const std::string& path, std::vector<Note>& notes, double& lengthBeats, std::string& error);

constexpr int MIDI_TICKS = 480;  // per beat (quarter note) in MIDI files written
struct MidiPart {
    std::string name;
    std::vector<Note> notes;  // start s, end s, pitch, velocity: seconds from startS into the song
};
// Write a standard MIDI file (type 1): a track per part, times from `startS` seconds into the song. Its
// tempo and time signature (and their changes after startS) go first, in a track of their own ("Tempo"),
// so the notes land on the same beats elsewhere. A MIDI file can't hold a ramp: it is written as a tempo
// event every MIDI_RAMP_STEP beats (a 1/16), each the step's average tempo, so every step (and every note
// after the ramp) ends exactly where it does in the song.
constexpr double MIDI_RAMP_STEP = 0.25;
bool saveMidi(const std::string& path, const std::vector<MidiPart>& parts, const Timing& timing, double startS,
              std::string& error);

// stereo audio at rate sr for any sample file, to preview it: audio files resampled, MIDI files
// rendered with the built-in synth
bool loadPlayable(const std::string& path, int sr, AudioPtr& out, std::string& error);

}  // namespace file_io
