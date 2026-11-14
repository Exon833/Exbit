// Ogg Vorbis encoder (Tiny DAW's own) — what File → Render writes as "OGG Vorbis".
//
// Vorbis I as the specification lays it out, with the choices libvorbis makes in spirit: blocks of
// 2048 samples, and of 256 around transients (no pre-echo); a psychoacoustic model (tonal and noise
// maskers spread in Bark, the threshold in quiet) gives each line its allowed noise; a floor 1 curve
// is fitted under it, and the spectrum divided by the floor is rounded to integers (the residue),
// coupled square-polar between the channels (lossless) and coded as residue type 2 with
// vector-quantization codebooks in classes and cascades. The codebooks' Huffman codes are made for
// each file from the file's own statistics (they're stored in the setup header), so encoding runs
// the analysis twice. Ogg pages carry granule positions so decoders return exactly the input's length.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Writes `frames` frames of interleaved float audio (1 channel: mono; 2 or more: stereo from the
// first two). `quality` is on oggenc's -q scale (-1 … 10; 6 ≈ 190 kbps for stereo music at 44.1 kHz).
// Any sample rate. `progress` (0..1) is called now and then; returning false cancels (no file is left).
bool encodeVorbis(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
                  double quality, std::string& error, std::function<bool(double)> progress = nullptr);
// the same into memory
bool encodeVorbisToMemory(std::vector<uint8_t>& out, const float* interleaved, size_t frames, int channels,
                          int sampleRate, double quality, std::string& error,
                          std::function<bool(double)> progress = nullptr);
