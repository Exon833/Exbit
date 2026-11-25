// MP3 encoder (Exbit's own): MPEG-1 Layer III CBR at 32 / 44.1 / 48 kHz, and MPEG-2 / 2.5 Layer III
// (the "LSF" rates 16 / 22.05 / 24 and 8 / 11.025 / 12 kHz) — what File → Render writes as "MP3, 320 kbps".
//
// The usual design (ISO 11172-3 annex C, as encoders like LAME do it): the analysis polyphase
// filterbank, a hybrid MDCT with window switching (long blocks, and three short ones around
// transients so their quantization noise doesn't spread before them: no pre-echo), a psychoacoustic
// model from FFTs (tonality by unpredictability, spreading in Bark, absolute threshold) that gives
// each scalefactor band its allowed noise, mid/side stereo when it needs fewer bits, the quantization
// loops (inner: the global gain for the bits; outer: scalefactors amplify the bands whose noise is
// above what's allowed), Huffman table and region selection, and the bit reservoir. The first frame
// is a Xing "Info" frame with a LAME-style tag carrying the encoder delay and padding, so decoders
// that read it (ours, mpg123, ffmpeg …) give back exactly the samples that went in.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Writes `frames` frames of interleaved float audio (1 channel: a mono MP3; 2 or more: stereo from the
// first two) at `kbps` (MPEG-1: 32 … 320, MPEG-2/2.5: 8 … 160; the nearest allowed one is used).
// A sample rate MP3 has no frame for (e.g. 96 kHz) is resampled to the nearest MPEG rate (see
// mp3EncodedRate). `progress` (0..1) is called now and then; returning false cancels (no file is left).
bool encodeMp3(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
               int kbps, std::string& error, std::function<bool(double)> progress = nullptr);
// the same into memory
bool encodeMp3ToMemory(std::vector<uint8_t>& out, const float* interleaved, size_t frames, int channels,
                       int sampleRate, int kbps, std::string& error, std::function<bool(double)> progress = nullptr);

// the sample rate an MP3 of audio at `sampleRate` is written at (itself when MP3 has it)
int mp3EncodedRate(int sampleRate);
// the bitrate used for `kbps` at that rate (the nearest one the MPEG version allows)
int mp3EncodedBitrate(int sampleRate, int kbps);
