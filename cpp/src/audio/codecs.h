// Compressed audio decoders (Tiny DAW's own): MP3 (MPEG-1/2/2.5 Layer III) and Ogg Vorbis.
// sound_file.cpp picks the decoder by the file's contents.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/sound_file.h"

// maxFrames < 0: everything; else stop after about that many frames (previews)
bool decodeMp3(const uint8_t* data, size_t size, SoundData& out, std::string& error, int64_t maxFrames = -1);
bool decodeVorbis(const uint8_t* data, size_t size, SoundData& out, std::string& error, int64_t maxFrames = -1);
bool looksLikeMp3(const uint8_t* data, size_t size);     // ID3 tag or an MPEG audio frame header
bool looksLikeVorbis(const uint8_t* data, size_t size);  // "OggS" with a Vorbis identification header
