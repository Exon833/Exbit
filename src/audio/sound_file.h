// Reading and writing sound files, and reading MIDI files: Tiny DAW's own decoders and encoders
// (the stand-in for soundfile and mido).
//
//   read:  WAV (PCM 8/16/24/32-bit, float 32/64, WAVE_FORMAT_EXTENSIBLE, RF64), AIFF / AIFF-C
//          (PCM, fl32/fl64, sowt), FLAC, MP3 (MPEG-1/2/2.5 Layer III, with ID3 tags skipped),
//          Ogg Vorbis
//   write: WAV (16-bit, 24-bit, 32-bit float), FLAC (16-bit, 24-bit)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SoundData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved, frames * channels
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// false (and `error`) if the file can't be read
bool readSoundFile(const std::string& path, SoundData& out, std::string& error);
// only the first `maxSeconds` (for quick previews); the rest isn't decoded where the format allows
bool readSoundFileStart(const std::string& path, double maxSeconds, SoundData& out, std::string& error);

enum class SampleFormat { Pcm16, Pcm24, Float32 };
// interleaved samples; WAV: Pcm16 / Pcm24 / Float32; FLAC: Pcm16 / Pcm24
bool writeWav(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
              SampleFormat format, std::string& error);
bool writeFlac(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
               SampleFormat format, std::string& error);

// --- MIDI files (format 0 and 1; all tracks and channels merged, tempo changes applied) ---
struct MidiFileNote {
    double start = 0, end = 0;  // seconds from the file's start
    int pitch = 60, velocity = 100, channel = 0;
};
bool readMidiFile(const std::string& path, std::vector<MidiFileNote>& notes, double& lengthSeconds,
                  std::string& error);
// the file's time signatures (time_signature meta events, FF 58), in order: where (quarter notes from its
// start) and num / den; a denominator past 1/32 or a numerator of 0 reads as 4/4 (with a debug message)
struct MidiFileSig {
    double beat = 0;
    int num = 4, den = 4;
};
// the same in beats: start and end in quarter notes (ticks / the file's ticks per beat, 480 for SMPTE
// files), length: the file's last tick in beats; and its time signatures, if `sigs`
bool readMidiFileBeats(const std::string& path, std::vector<MidiFileNote>& notes, double& lengthBeats,
                       std::string& error, std::vector<MidiFileSig>* sigs = nullptr);
bool readMidiFileSignatures(const std::string& path, std::vector<MidiFileSig>& sigs, std::string& error);

// --- writing MIDI files (type 1) ---
struct MidiOutEvent {
    int64_t tick = 0;
    int order = 0;               // at one tick: lower first (note-offs before note-ons)
    std::vector<uint8_t> bytes;  // a channel message (status, data…) or a meta event (0xFF, type, data…)
};
struct MidiOutTrack {
    std::string name;                 // its track_name meta event
    std::vector<MidiOutEvent> events;  // any order: sorted by (tick, order), stable
};
// a type 1 Standard MIDI File, each track starting with its name and ending with end-of-track
bool writeMidiFile(const std::string& path, int ticksPerBeat, const std::vector<MidiOutTrack>& tracks,
                   std::string& error);
namespace midi_out {
MidiOutEvent noteOn(int64_t tick, int pitch, int velocity, int channel = 0);  // order 1
MidiOutEvent noteOff(int64_t tick, int pitch, int channel = 0);                // order 0 (note_off, velocity 0)
MidiOutEvent tempo(int64_t tick, double bpm);                                   // set_tempo, order 0
MidiOutEvent timeSignature(int64_t tick, int numerator, int denominator);       // order 1
}  // namespace midi_out
